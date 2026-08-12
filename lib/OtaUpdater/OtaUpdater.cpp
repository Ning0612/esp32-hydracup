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
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "hal_log.h"
#include "hal_time.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "StorageLock.h"
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

// Created in ota_boot_check(), which runs before any task that could contend for it.
SemaphoreHandle_t g_webfsMutex = nullptr;
bool g_webfsAvailable = true;  // guarded by g_webfsMutex

bool remountWebfs() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/webfs";
    conf.partition_label = OTA_WEBFS_LABEL;
    conf.format_if_mount_failed = false;
    conf.dont_mount = false;
    return esp_vfs_littlefs_register(&conf) == ESP_OK;
}

bool isHex64(const char* text) {
    for (int i = 0; i < 64; ++i) {
        const char c = text[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return text[64] == '\0';
}

// Pulls one entry out of a `sha256sum` listing: "<64 hex><spaces><name>" per line.
bool extractSha(const char* text, const char* asset, char* out) {
    const size_t assetLength = std::strlen(asset);
    for (const char* line = text; line && *line;) {
        const char* newline = std::strchr(line, '\n');
        size_t length = newline ? static_cast<size_t>(newline - line) : std::strlen(line);
        while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == ' ')) --length;
        if (length > 64) {
            const char* name = line + 64;
            while (*name == ' ' || *name == '*') ++name;
            if (std::strncmp(name, "./", 2) == 0) name += 2;
            const size_t nameLength = (line + length) - name;
            if (nameLength == assetLength && std::strncmp(name, asset, assetLength) == 0) {
                std::memcpy(out, line, 64);
                out[64] = '\0';
                return isHex64(out);
            }
        }
        line = newline ? newline + 1 : nullptr;
    }
    return false;
}

void shaToHex(const uint8_t digest[32], char* out) {
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

// The hash of the image currently on the partition. Hashing the live partition instead is not
// an option: LittleFS rewrites metadata as it runs, so its bytes drift from the published
// image even when the content is identical.
bool loadInstalledWebfsSha(char* out) {
    out[0] = '\0';
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("ota", NVS_READONLY, &handle) == ESP_OK) {
        size_t length = 65;
        ok = nvs_get_str(handle, "webfs_sha", out, &length) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    if (!ok) out[0] = '\0';
    return ok;
}

void loadLastOutcome(char* out, size_t outLength) {
    out[0] = '\0';
    if (!lockNvs()) return;
    nvs_handle_t handle = 0;
    if (nvs_open("ota", NVS_READONLY, &handle) == ESP_OK) {
        size_t length = outLength;
        if (nvs_get_str(handle, "last_msg", out, &length) != ESP_OK) out[0] = '\0';
        nvs_close(handle);
    }
    unlockNvs();
}

void saveLastOutcome(const char* message) {
    if (!lockNvs()) return;
    nvs_handle_t handle = 0;
    if (nvs_open("ota", NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_str(handle, "last_msg", message) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
    unlockNvs();
}

bool saveInstalledWebfsSha(const char* sha) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("ota", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_str(handle, "webfs_sha", sha) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    return ok;
}

}  // namespace

bool webfs_read_begin() {
    if (!g_webfsMutex) return true;  // never initialised: behave as before this guard existed
    if (xSemaphoreTake(g_webfsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    if (!g_webfsAvailable) {
        xSemaphoreGive(g_webfsMutex);
        return false;
    }
    return true;
}

void webfs_read_end() {
    if (g_webfsMutex) xSemaphoreGive(g_webfsMutex);
}

void ota_boot_check() {
    if (!g_webfsMutex) g_webfsMutex = xSemaphoreCreateMutex();
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
    // Signed difference so the comparison survives the ~49.7 day millis() rollover; a plain
    // `now < nextAttemptMs` would wrap and retry every service tick instead of every 5 s.
    if (static_cast<int32_t>(hal_millis() - nextAttemptMs) < 0 || !controlHealthy) return;
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
    loadLastOutcome(_status.lastOutcome, sizeof(_status.lastOutcome));
    _publishRunningPartition();
    if (_status.lastOutcome[0]) LOG_INFO(TAG, "last update outcome: %s", _status.lastOutcome);
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
    if (!_fetchText(OTA_MANIFEST_URL, out, outLength, statusCode)) return false;
    trimVersionText(out);
    return out[0] != '\0';
}

bool OtaUpdater::_fetchText(const char* url, char* out, size_t outLength, int& statusCode) {
    ManifestCapture capture{out, outLength, 0, false};
    out[0] = '\0';
    esp_http_client_config_t config = {};
    config.url = url;
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
        LOG_WARN(TAG, "response larger than %u bytes, refusing to parse a prefix",
                 static_cast<unsigned>(outLength));
        out[0] = '\0';
        return false;
    }
    return true;
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

    // otaInProgress was raised in requestUpdate(), but the other workers only observe it at
    // the top of their loops - one already inside an HTTPS request keeps its TLS heap until
    // it finishes, and their HTTP timeout is 10 s. A fixed delay cannot cover that, so wait
    // on the thing actually at stake: free internal heap. This doubles as the pre-flight
    // check, since a worker still holding a session is exactly what keeps it low.
    size_t freeHeap = 0;
    for (uint32_t waited = 0;; waited += OTA_SETTLE_POLL_MS) {
        freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (freeHeap >= OTA_MIN_FREE_HEAP_BYTES && waited >= OTA_SETTLE_MIN_MS) break;
        if (waited >= OTA_SETTLE_TIMEOUT_MS) {
            _setMessage("可用記憶體不足（%u KB），已取消更新",
                        static_cast<unsigned>(freeHeap / 1024));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_SETTLE_POLL_MS));
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

void OtaUpdater::_setStage(const char* stage) {
    if (!_statusMutex) return;
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    std::snprintf(_status.stage, sizeof(_status.stage), "%s", stage);
    _status.progressPercent = 0;
    _status.imageSize = 0;
    _status.bytesRead = 0;
    xSemaphoreGive(_statusMutex);
}

// Streams the LittleFS image straight onto the webfs partition. There is no spare data
// partition to stage it in and the image does not fit in RAM, so this overwrites in place:
// an interrupted write leaves the partition unusable until a USB `uploadfs`. The firmware
// itself keeps running - only the static pages are served from here, not the API.
bool OtaUpdater::_updateWebfs(const char* expectedSha) {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, OTA_WEBFS_LABEL);
    if (!partition) {
        _setMessage("找不到 webfs 分割區，略過網頁資源更新");
        return false;
    }

    uint8_t* buffer = static_cast<uint8_t*>(heap_caps_malloc(OTA_WEBFS_CHUNK, MALLOC_CAP_INTERNAL));
    if (!buffer) {
        _setMessage("記憶體不足，略過網頁資源更新");
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = OTA_WEBFS_URL;
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = OTA_HTTP_TIMEOUT_MS;
    config.buffer_size = OTA_HTTP_BUFFER_BYTES;
    config.buffer_size_tx = OTA_HTTP_BUFFER_BYTES;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        heap_caps_free(buffer);
        _setMessage("無法建立連線，略過網頁資源更新");
        return false;
    }

    bool ok = false;
    bool unmounted = false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    do {
        // Redirects have to be followed by hand here. esp_http_client only resolves them
        // inside esp_http_client_perform(); the open/fetch_headers/read flow used for
        // streaming hands the 302 straight back. GitHub always redirects release assets to
        // objects.githubusercontent.com, so without this every webfs download fails at the
        // first response.
        int contentLength = 0;
        int status = 0;
        bool opened = false;
        for (int hop = 0; hop <= OTA_MAX_REDIRECTS; ++hop) {
            if (esp_http_client_open(client, 0) != ESP_OK) {
                _setMessage("網頁資源下載無法開始");
                break;
            }
            opened = true;
            contentLength = esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) break;
            if (hop == OTA_MAX_REDIRECTS) {
                _setMessage("網頁資源轉址次數過多（%d 次）", hop);
                status = -1;
                break;
            }
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            opened = false;
        }
        if (!opened || status < 200 || status >= 300) {
            if (opened) _setMessage("網頁資源下載失敗（HTTP %d）", status);
            break;
        }
        if (contentLength <= 0 || static_cast<size_t>(contentLength) > partition->size) {
            _setMessage("網頁資源大小不符（%d bytes）", contentLength);
            break;
        }
        xSemaphoreTake(_statusMutex, portMAX_DELAY);
        _status.imageSize = static_cast<uint32_t>(contentLength);
        xSemaphoreGive(_statusMutex);

        // Take the read lock so no static-file request is inside fread() when the mount goes
        // away, then mark webfs unavailable while still holding it: a request that passed the
        // check can therefore never reach the filesystem after this point. If httpd is stuck
        // streaming for this long, skip webfs rather than race it - the firmware is already in.
        if (g_webfsMutex && xSemaphoreTake(g_webfsMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
            _setMessage("網頁資源忙碌中，本次略過更新");
            break;
        }
        g_webfsAvailable = false;
        if (g_webfsMutex) xSemaphoreGive(g_webfsMutex);
        esp_vfs_littlefs_unregister(OTA_WEBFS_LABEL);
        unmounted = true;
        if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
            _setMessage("webfs 分割區抹除失敗，需以 USB 重新燒錄");
            break;
        }

        mbedtls_sha256_starts(&sha, 0);
        const uint32_t startedAt = hal_millis();
        size_t written = 0;
        size_t pending = 0;
        bool failed = false;
        for (;;) {
            if (hal_millis() - startedAt > OTA_WEBFS_DEADLINE_MS) {
                _setMessage("網頁資源下載逾時，需以 USB 重新燒錄");
                failed = true;
                break;
            }
            const int read = esp_http_client_read(client, reinterpret_cast<char*>(buffer) + pending,
                                                  OTA_WEBFS_CHUNK - pending);
            if (read < 0) {
                _setMessage("網頁資源下載中斷，需以 USB 重新燒錄");
                failed = true;
                break;
            }
            if (read == 0) break;
            pending += static_cast<size_t>(read);
            if (pending < OTA_WEBFS_CHUNK) continue;
            if (written + pending > partition->size ||
                esp_partition_write(partition, written, buffer, pending) != ESP_OK) {
                _setMessage("webfs 寫入失敗，需以 USB 重新燒錄");
                failed = true;
                break;
            }
            mbedtls_sha256_update(&sha, buffer, pending);
            written += pending;
            pending = 0;
            xSemaphoreTake(_statusMutex, portMAX_DELAY);
            _status.bytesRead = written;
            _status.progressPercent = static_cast<uint8_t>(
                std::min<uint32_t>(100, written * 100 / static_cast<uint32_t>(contentLength)));
            xSemaphoreGive(_statusMutex);
        }
        if (failed) break;

        // esp_partition_write needs a 4-byte aligned length; the padding is never hashed.
        if (pending > 0) {
            const size_t padded = (pending + 3u) & ~3u;
            std::memset(buffer + pending, 0xFF, padded - pending);
            if (written + padded > partition->size ||
                esp_partition_write(partition, written, buffer, padded) != ESP_OK) {
                _setMessage("webfs 寫入失敗，需以 USB 重新燒錄");
                break;
            }
            mbedtls_sha256_update(&sha, buffer, pending);
            written += pending;
        }
        if (!esp_http_client_is_complete_data_received(client) ||
            written != static_cast<size_t>(contentLength)) {
            _setMessage("網頁資源不完整，需以 USB 重新燒錄");
            break;
        }

        uint8_t digest[32] = {};
        char actual[65] = {};
        mbedtls_sha256_finish(&sha, digest);
        shaToHex(digest, actual);
        if (std::strcmp(actual, expectedSha) != 0) {
            _setMessage("網頁資源校驗不符，需以 USB 重新燒錄");
            break;
        }
        if (!saveInstalledWebfsSha(actual)) {
            // Harmless: the next update simply rewrites an identical image.
            LOG_WARN(TAG, "webfs written but its hash could not be recorded");
        }
        ok = true;
    } while (false);

    mbedtls_sha256_free(&sha);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(buffer);
    // A reboot follows either way, but do not leave the mount missing in the meantime - and a
    // checksum mismatch still leaves a complete image behind, which usually mounts fine.
    if (unmounted && remountWebfs()) {
        if (g_webfsMutex) xSemaphoreTake(g_webfsMutex, portMAX_DELAY);
        g_webfsAvailable = true;
        if (g_webfsMutex) xSemaphoreGive(g_webfsMutex);
    } else if (unmounted) {
        LOG_WARN(TAG, "webfs could not be remounted; USB uploadfs required");
    }
    return ok;
}

void OtaUpdater::_runUpdate() {
    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.updateState = OtaUpdateState::DOWNLOADING;
    std::snprintf(_status.message, sizeof(_status.message), "開始下載韌體");
    xSemaphoreGive(_statusMutex);
    _setStage("firmware");

    // Read the checksum listing before writing anything: without it the web assets cannot be
    // verified, and an unverifiable image must not be written to a partition with no spare.
    char checksums[768] = {};
    char expectedWebfsSha[65] = {};
    int checksumStatus = 0;
    const bool haveWebfsSha = _fetchText(OTA_CHECKSUMS_URL, checksums, sizeof(checksums),
                                         checksumStatus) &&
                              extractSha(checksums, OTA_WEBFS_ASSET, expectedWebfsSha);

    // Firmware first. If it fails nothing is committed and webfs is untouched; the reverse
    // order could leave a UI newer than the firmware serving it.
    if (!_downloadAndWrite()) {
        if (_state) _state->otaInProgress.store(false);
        xSemaphoreTake(_statusMutex, portMAX_DELAY);
        _status.updateState = OtaUpdateState::FAILED;
        const OtaStatus failure = _status;
        xSemaphoreGive(_statusMutex);
        LOG_WARN(TAG, "update failed: %s", failure.message);
        return;
    }

    // The app slot is now committed, so from here every path reboots: the new firmware is
    // better than the old one even if the web assets did not make it.
    char installedWebfsSha[65] = {};
    loadInstalledWebfsSha(installedWebfsSha);
    char webfsDetail[sizeof(OtaStatus::message)] = {};
    const char* webfsOutcome = "網頁資源無校驗資訊，維持原樣";
    if (haveWebfsSha && std::strcmp(installedWebfsSha, expectedWebfsSha) == 0) {
        webfsOutcome = "網頁資源已是最新";
        LOG_INFO(TAG, "webfs already matches the published image, skipping");
    } else if (haveWebfsSha) {
        _setStage("webfs");
        _setMessage("開始更新網頁資源");
        if (_updateWebfs(expectedWebfsSha)) {
            webfsOutcome = "網頁資源已更新";
        } else {
            // Carry the specific reason through. A generic "failed" hides which step gave up,
            // and wrongly implies a USB reflash even when the partition was never touched.
            const OtaStatus failure = snapshot();
            std::snprintf(webfsDetail, sizeof(webfsDetail), "%s", failure.message);
            webfsOutcome = webfsDetail;
        }
    } else {
        LOG_WARN(TAG, "SHA256SUMS unavailable (HTTP %d), leaving webfs untouched", checksumStatus);
    }

    xSemaphoreTake(_statusMutex, portMAX_DELAY);
    _status.updateState = OtaUpdateState::READY_PENDING_REBOOT;
    _status.progressPercent = 100;
    std::snprintf(_status.message, sizeof(_status.message), "韌體更新完成 · %s · 即將重新開機",
                  webfsOutcome);
    xSemaphoreGive(_statusMutex);
    // Recorded before the restart so the result is still readable afterwards.
    saveLastOutcome(snapshot().message);
    LOG_INFO(TAG, "update complete, restarting");
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}
