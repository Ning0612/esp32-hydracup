#include "OtaUpdater.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "AppState.h"
#include "RuntimeCoordinator.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "hal_log.h"
#include "hal_time.h"
#include "version.h"

namespace {

constexpr const char* TAG = "Ota";

// ESP-IDF sizes task stacks in bytes, unlike vanilla FreeRTOS. 10 KB matches the
// cloud_sync task, which already runs a full mbedtls session plus file IO; the OTA
// path is shallower because esp_ota_write() sits directly on spi_flash.
constexpr uint32_t OTA_TASK_STACK_BYTES = 10240;
constexpr UBaseType_t OTA_TASK_PRIORITY = 3;  // below httpd (5) so /api/ota/status stays responsive
constexpr BaseType_t OTA_TASK_CORE = 0;       // WiFi/lwip core; hydracup_control owns core 1

// Long enough for the 2 s status poll to observe READY_PENDING_REBOOT before the link drops.
constexpr uint32_t OTA_REBOOT_DELAY_MS = 2500;

StaticTask_t s_taskBuffer;
StackType_t s_taskStack[OTA_TASK_STACK_BYTES];

struct ManifestCapture {
    char* buffer;
    size_t capacity;  // includes the terminator; must be >= 1
    size_t length;
    bool truncated;
};

esp_err_t captureManifest(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->user_data) return ESP_OK;
    auto* capture = static_cast<ManifestCapture*>(event->user_data);
    const size_t room = capture->capacity - 1 - capture->length;
    const size_t incoming = event->data_len > 0 ? static_cast<size_t>(event->data_len) : 0;
    const size_t copied = std::min(room, incoming);
    if (copied > 0) {
        std::memcpy(capture->buffer + capture->length, event->data, copied);
        capture->length += copied;
    }
    // Silently truncating would let a wrong or oversized body parse as a valid prefix.
    if (copied < incoming) capture->truncated = true;
    capture->buffer[capture->length] = '\0';
    return ESP_OK;
}

// Accepts exactly "X.Y.Z" with at most one leading 'v'. Trailing content is rejected rather
// than ignored: "1.2.3.4" or a manifest truncated mid-number must not read as a valid version.
bool parseVersion(const char* text, uint32_t parts[3]) {
    if (!text) return false;
    while (*text == ' ') ++text;
    if (*text == 'v' || *text == 'V') ++text;
    for (int index = 0; index < 3; ++index) {
        if (*text < '0' || *text > '9') return false;
        uint32_t value = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + static_cast<uint32_t>(*text - '0');
            if (value > 65535) return false;
            ++text;
        }
        parts[index] = value;
        if (index < 2) {
            if (*text != '.') return false;
            ++text;
        }
    }
    return *text == '\0';
}

bool sameVersion(const char* left, const char* right) {
    uint32_t a[3] = {};
    uint32_t b[3] = {};
    if (!parseVersion(left, a) || !parseVersion(right, b)) return false;
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

// Strictly greater, so a release that is older than the running build never triggers an update.
bool isNewer(const char* candidate, const char* running) {
    uint32_t left[3] = {};
    uint32_t right[3] = {};
    if (!parseVersion(candidate, left) || !parseVersion(running, right)) return false;
    for (int index = 0; index < 3; ++index) {
        if (left[index] != right[index]) return left[index] > right[index];
    }
    return false;
}

void trimVersionText(char* text) {
    size_t length = std::strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
}

std::atomic<bool> g_pendingVerify{false};
std::atomic<bool> g_verifyResolved{false};

}  // namespace

void ota_boot_check() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
    const bool pending = running && esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
                         imageState == ESP_OTA_IMG_PENDING_VERIFY;
    g_pendingVerify.store(pending);
    if (pending) LOG_INFO(TAG, "running a freshly written image, pending verification");
}

bool ota_pending_verify() { return g_pendingVerify.load(); }

void ota_mark_app_valid_if_due(bool controlHealthy) {
    static uint32_t nextAttemptMs = OTA_MARK_VALID_DELAY_MS;
    if (g_verifyResolved.load()) return;
    if (!g_pendingVerify.load()) { g_verifyResolved.store(true); return; }
    if (hal_millis() < nextAttemptMs || !controlHealthy) return;
    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result != ESP_OK) {
        // Writing otadata failed, so this image really is still pending: keep retrying and
        // keep reporting pending_verify. Claiming success here would tell the user the update
        // is confirmed while the bootloader is still set to roll it back.
        nextAttemptMs = hal_millis() + OTA_MARK_VALID_RETRY_MS;
        LOG_WARN(TAG, "marking app valid failed (%s), still pending", esp_err_to_name(result));
        return;
    }
    g_verifyResolved.store(true);
    g_pendingVerify.store(false);
    LOG_INFO(TAG, "app marked valid, rollback cancelled");
}

const char* OtaUpdater::checkStateName(OtaCheckState state) {
    switch (state) {
        case OtaCheckState::CHECKING: return "checking";
        case OtaCheckState::UP_TO_DATE: return "up_to_date";
        case OtaCheckState::UPDATE_AVAILABLE: return "update_available";
        case OtaCheckState::CHECK_FAILED: return "check_failed";
        default: return "unknown";
    }
}

const char* OtaUpdater::updateStateName(OtaUpdateState state) {
    switch (state) {
        case OtaUpdateState::DOWNLOADING: return "downloading";
        case OtaUpdateState::WRITING: return "writing";
        case OtaUpdateState::READY_PENDING_REBOOT: return "ready_pending_reboot";
        case OtaUpdateState::FAILED: return "failed";
        default: return "idle";
    }
}

bool OtaUpdater::init(AppState& state, RuntimeCoordinator& runtime) {
    _state = &state;
    _runtime = &runtime;
    _statusMutex = xSemaphoreCreateMutex();
    _wake = xSemaphoreCreateBinary();
    if (!_statusMutex || !_wake) {
        LOG_ERROR(TAG, "sync primitive creation failed");
        _releasePrimitives();
        return false;
    }
    std::snprintf(_status.runningVersion, sizeof(_status.runningVersion), "%s", APP_VERSION);
    _publishRunningPartition();
    _task = xTaskCreateStaticPinnedToCore(_taskFunc, "ota_worker", OTA_TASK_STACK_BYTES, this,
                                          OTA_TASK_PRIORITY, s_taskStack, &s_taskBuffer,
                                          OTA_TASK_CORE);
    if (!_task) {
        LOG_ERROR(TAG, "worker creation failed");
        _releasePrimitives();
        return false;
    }
    LOG_INFO(TAG, "ready version=%s partition=%s", _status.runningVersion, _status.runningPartition);
    return true;
}

void OtaUpdater::_releasePrimitives() {
    if (_wake) { vSemaphoreDelete(_wake); _wake = nullptr; }
    if (_statusMutex) { vSemaphoreDelete(_statusMutex); _statusMutex = nullptr; }
}

void OtaUpdater::_publishRunningPartition() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;
    std::snprintf(_status.runningPartition, sizeof(_status.runningPartition), "%s", running->label);
}

OtaStatus OtaUpdater::snapshot() const {
    OtaStatus copy;
    if (!_statusMutex) return copy;
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    copy = _status;
    xSemaphoreGive(_statusMutex);
    copy.pendingVerify = ota_pending_verify();
    return copy;
}

void OtaUpdater::_setMessage(const char* format, ...) {
    if (!_statusMutex) return;
    char text[sizeof(_status.message)];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    std::snprintf(_status.message, sizeof(_status.message), "%s", text);
    xSemaphoreGive(_statusMutex);
}

OtaRequestResult OtaUpdater::requestCheck() {
    if (!_wake) return OtaRequestResult::BUSY;
    if (_state && !_state->wifiConnected.load()) return OtaRequestResult::OFFLINE;
    bool expected = false;
    if (!_busy.compare_exchange_strong(expected, true)) return OtaRequestResult::BUSY;
    _pending.store(Command::CHECK);
    xSemaphoreGive(_wake);
    return OtaRequestResult::ACCEPTED;
}

OtaRequestResult OtaUpdater::requestUpdate() {
    if (!_wake) return OtaRequestResult::BUSY;
    if (_state && !_state->wifiConnected.load()) return OtaRequestResult::OFFLINE;
    const OtaStatus current = snapshot();
    if (current.checkState != OtaCheckState::UPDATE_AVAILABLE) {
        return OtaRequestResult::NO_UPDATE_AVAILABLE;
    }
    // Refusing up front beats running out of memory mid-write and leaving a half-written slot.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < OTA_MIN_FREE_HEAP_BYTES) {
        return OtaRequestResult::INSUFFICIENT_MEMORY;
    }
    bool idle = false;
    if (!_busy.compare_exchange_strong(idle, true)) return OtaRequestResult::BUSY;
    // Raised here rather than in _runUpdate() so the other workers start standing down while
    // the request is still being answered, instead of after the download has already begun.
    if (_state) _state->otaInProgress.store(true);
    _pending.store(Command::UPDATE);
    xSemaphoreGive(_wake);
    return OtaRequestResult::ACCEPTED;
}

void OtaUpdater::_taskFunc(void* param) {
    static_cast<OtaUpdater*>(param)->_taskLoop();
}

void OtaUpdater::_taskLoop() {
    for (;;) {
        xSemaphoreTake(_wake, portMAX_DELAY);
        const Command command = _pending.exchange(Command::NONE);
        if (command == Command::CHECK) _runCheck();
        else if (command == Command::UPDATE) _runUpdate();
        _busy.store(false);
    }
}

bool OtaUpdater::_fetchLatestVersion(char* out, size_t outLength, int& statusCode) {
    ManifestCapture capture{out, outLength, 0, false};
    out[0] = '\0';
    esp_http_client_config_t config = {};
    config.url = OTA_MANIFEST_URL;
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = OTA_HTTP_TIMEOUT_MS;
    config.event_handler = captureManifest;
    config.user_data = &capture;
    // GitHub redirects release assets to objects.githubusercontent.com with a signed URL
    // measured at 883 bytes; the 512-byte default cannot hold the redirected request line.
    config.buffer_size = OTA_HTTP_BUFFER_BYTES;
    config.buffer_size_tx = OTA_HTTP_BUFFER_BYTES;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    const esp_err_t result = esp_http_client_perform(client);
    statusCode = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        LOG_WARN(TAG, "manifest fetch failed error=%s", esp_err_to_name(result));
        return false;
    }
    if (statusCode < 200 || statusCode >= 300) {
        LOG_WARN(TAG, "manifest fetch status=%d", statusCode);
        return false;
    }
    if (capture.truncated) {
        LOG_WARN(TAG, "manifest larger than %u bytes, refusing to parse a prefix",
                 static_cast<unsigned>(outLength));
        out[0] = '\0';
        return false;
    }
    trimVersionText(out);
    return out[0] != '\0';
}

void OtaUpdater::_runCheck() {
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.checkState = OtaCheckState::CHECKING;
    _status.message[0] = '\0';
    _status.latestVersion[0] = '\0';
    xSemaphoreGive(_statusMutex);

    char latest[16] = {};
    int statusCode = 0;
    const bool fetched = _fetchLatestVersion(latest, sizeof(latest), statusCode);
    uint32_t parsed[3] = {};
    const bool parseable = fetched && parseVersion(latest, parsed);

    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.lastHttpStatus = statusCode;
    if (!fetched) {
        _status.checkState = OtaCheckState::CHECK_FAILED;
        std::snprintf(_status.message, sizeof(_status.message),
                      "無法取得版本資訊（HTTP %d）", statusCode);
    } else if (!parseable) {
        _status.checkState = OtaCheckState::CHECK_FAILED;
        std::snprintf(_status.message, sizeof(_status.message), "版本格式無法解析：%s", latest);
    } else {
        std::snprintf(_status.latestVersion, sizeof(_status.latestVersion), "%s", latest);
        const bool newer = isNewer(latest, APP_VERSION);
        _status.checkState = newer ? OtaCheckState::UPDATE_AVAILABLE : OtaCheckState::UP_TO_DATE;
        std::snprintf(_status.message, sizeof(_status.message),
                      newer ? "有新版本 %s 可更新" : "目前已是最新版本", latest);
    }
    const OtaCheckState resultState = _status.checkState;
    xSemaphoreGive(_statusMutex);
    LOG_INFO(TAG, "check result=%s latest=%s", checkStateName(resultState), latest);
}

bool OtaUpdater::_downloadAndWrite() {
    // Started before begin() so TLS handshake and redirects count against the deadline too.
    const uint32_t startedAt = hal_millis();

    // otaInProgress was raised in requestUpdate(); give in-flight cloud sync and Discord
    // requests a moment to finish before competing with the download for heap and sockets.
    vTaskDelay(pdMS_TO_TICKS(OTA_SETTLE_DELAY_MS));
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < OTA_MIN_FREE_HEAP_BYTES) {
        _setMessage("可用記憶體不足，已取消更新");
        return false;
    }

    char expected[16] = {};
    {
        const OtaStatus current = snapshot();
        std::snprintf(expected, sizeof(expected), "%s", current.latestVersion);
    }

    esp_http_client_config_t httpConfig = {};
    httpConfig.url = OTA_FIRMWARE_URL;
    httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
    httpConfig.timeout_ms = OTA_HTTP_TIMEOUT_MS;
    httpConfig.keep_alive_enable = true;
    httpConfig.buffer_size = OTA_HTTP_BUFFER_BYTES;
    httpConfig.buffer_size_tx = OTA_HTTP_BUFFER_BYTES;

    esp_https_ota_config_t otaConfig = {};
    otaConfig.http_config = &httpConfig;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t result = esp_https_ota_begin(&otaConfig, &handle);
    if (result != ESP_OK || !handle) {
        _setMessage("無法開始更新：%s", esp_err_to_name(result));
        return false;
    }

    // The manifest and the firmware asset are two separate requests against a mutable
    // "latest" pointer, so trust the image itself: refuse anything that is not the version
    // the check announced, and not newer than what is running. TLS and the image header
    // prove integrity, not identity.
    esp_app_desc_t incoming = {};
    if (esp_https_ota_get_img_desc(handle, &incoming) != ESP_OK) {
        esp_https_ota_abort(handle);
        _setMessage("無法讀取韌體版本資訊，已中止");
        return false;
    }
    incoming.version[sizeof(incoming.version) - 1] = '\0';
    if (!sameVersion(incoming.version, expected) || !isNewer(incoming.version, APP_VERSION)) {
        esp_https_ota_abort(handle);
        _setMessage("韌體版本不符（預期 %s，實際 %s），已中止", expected, incoming.version);
        return false;
    }

    const int imageSize = esp_https_ota_get_image_size(handle);
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.imageSize = imageSize > 0 ? static_cast<uint32_t>(imageSize) : 0;
    xSemaphoreGive(_statusMutex);

    while ((result = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (hal_millis() - startedAt > OTA_OVERALL_DEADLINE_MS) {
            esp_https_ota_abort(handle);
            _setMessage("更新逾時，已中止（未寫入完成）");
            return false;
        }
        const int readLength = esp_https_ota_get_image_len_read(handle);
        xSemaphoreTake(_statusMutex, portMAX_DELAY);
        _status.bytesRead = readLength > 0 ? static_cast<uint32_t>(readLength) : 0;
        if (_status.imageSize > 0) {
            _status.progressPercent = static_cast<uint8_t>(
                std::min<uint32_t>(100, _status.bytesRead * 100 / _status.imageSize));
        }
        if (_status.bytesRead > 0) _status.updateState = OtaUpdateState::WRITING;
        xSemaphoreGive(_statusMutex);
    }

    if (result != ESP_OK) {
        esp_https_ota_abort(handle);
        _setMessage("下載失敗：%s", esp_err_to_name(result));
        return false;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        _setMessage("韌體資料不完整，已中止");
        return false;
    }
    result = esp_https_ota_finish(handle);
    if (result != ESP_OK) {
        _setMessage("寫入驗證失敗：%s", esp_err_to_name(result));
        return false;
    }
    return true;
}

void OtaUpdater::_runUpdate() {
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.updateState = OtaUpdateState::DOWNLOADING;
    _status.progressPercent = 0;
    _status.imageSize = 0;
    _status.bytesRead = 0;
    std::snprintf(_status.message, sizeof(_status.message), "開始下載韌體");
    xSemaphoreGive(_statusMutex);

    const bool ok = _downloadAndWrite();

    if (!ok) {
        if (_state) _state->otaInProgress.store(false);
        xSemaphoreTake(_statusMutex, portMAX_DELAY);
        _status.updateState = OtaUpdateState::FAILED;
        const OtaStatus failure = _status;
        xSemaphoreGive(_statusMutex);
        LOG_WARN(TAG, "update failed: %s", failure.message);
        return;
    }

    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.updateState = OtaUpdateState::READY_PENDING_REBOOT;
    _status.progressPercent = 100;
    std::snprintf(_status.message, sizeof(_status.message), "更新完成，即將重新開機");
    xSemaphoreGive(_statusMutex);
    LOG_INFO(TAG, "update complete, restarting");
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}
