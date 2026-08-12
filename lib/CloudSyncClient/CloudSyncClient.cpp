#include "CloudSyncClient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ConfigManager.h"
#include "AppState.h"
#include "EventLogger.h"
#include "StorageLock.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "hal_log.h"
#include "hal_time.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "version.h"

namespace {
constexpr const char* TAG = "CloudSync";
constexpr const char* OUTBOX_PATH = "/logfs/cloud/outbox.jsonl";
constexpr const char* OUTBOX_TEMP_PATH = "/logfs/cloud/outbox.tmp";
constexpr const char* OUTBOX_BACKUP_PATH = "/logfs/cloud/outbox.bak";
constexpr uint32_t SYNC_INTERVAL_MS = 15000;
constexpr uint8_t MAX_BATCH_EVENTS = 12;
constexpr uint8_t MAX_BATCH_ACKS = 8;
constexpr uint8_t MAX_HISTORY_BATCH_DAYS = 31;
constexpr long MAX_OUTBOX_BYTES = 512 * 1024;
constexpr size_t HISTORY_LOG_FILENAME_LENGTH = sizeof("drink-YYYY-MM.jsonl") - 1;
constexpr size_t HISTORY_LOG_SUFFIX_OFFSET = sizeof("drink-YYYY-MM") - 1;
static_assert(HISTORY_LOG_FILENAME_LENGTH == 19, "history log filename contract changed");

const char CLOUD_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";

std::string encode(cJSON* value) {
    char* raw = cJSON_PrintUnformatted(value);
    if (!raw) return {};
    std::string result(raw);
    cJSON_free(raw);
    return result;
}

esp_err_t captureHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data_len > 0) {
        static_cast<std::string*>(event->user_data)->append(
            static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

bool validHistoryMonth(const char* value) {
    if (!value || std::strlen(value) != 7 || value[4] != '-') return false;
    for (size_t index = 0; index < 7; ++index) {
        if (index != 4 && (value[index] < '0' || value[index] > '9')) return false;
    }
    const int month = (value[5] - '0') * 10 + value[6] - '0';
    return month >= 1 && month <= 12;
}

bool validHistoryTimestamp(const char* value, const std::string& month, const char* today) {
    if (!value || std::strlen(value) != 25 || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        (value[19] != '+' && value[19] != '-') || value[22] != ':' ||
        std::strncmp(value, month.c_str(), 7) != 0 ||
        (today && std::strncmp(value, today, 10) >= 0)) return false;
    for (size_t index = 0; index < 25; ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 ||
            index == 19 || index == 22) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
                     (value[2] - '0') * 10 + value[3] - '0';
    const int monthNumber = (value[5] - '0') * 10 + value[6] - '0';
    const int day = (value[8] - '0') * 10 + value[9] - '0';
    static constexpr uint8_t DAYS_BY_MONTH[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (year < 2021 || monthNumber < 1 || monthNumber > 12) return false;
    int maxDay = DAYS_BY_MONTH[monthNumber - 1];
    if (monthNumber == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))) ++maxDay;
    const int hour = (value[11] - '0') * 10 + value[12] - '0';
    const int minute = (value[14] - '0') * 10 + value[15] - '0';
    const int second = (value[17] - '0') * 10 + value[18] - '0';
    const int offsetHour = (value[20] - '0') * 10 + value[21] - '0';
    const int offsetMinute = (value[23] - '0') * 10 + value[24] - '0';
    return day >= 1 && day <= maxDay && hour <= 23 && minute <= 59 && second <= 59 &&
           offsetHour <= 14 && offsetMinute <= 59;
}

bool currentLocalDate(char value[11]) {
    const time_t now = time(nullptr);
    struct tm local = {};
    if (now < 0 || !localtime_r(&now, &local) || local.tm_year <= 120) return false;
    return std::strftime(value, 11, "%Y-%m-%d", &local) == 10;
}

}  // namespace

bool CloudSyncClient::init(AppState& appState, AppConfig& config,
                           ConfigManager& configManager, EventLogger& eventLogger,
                           bool logFsOk) {
    _appState = &appState;
    _configManager = &configManager;
    _eventLogger = &eventLogger;
    _logFsOk = logFsOk;
    _commandQueue = xQueueCreate(8, sizeof(CloudCommand));
    _ackQueue = xQueueCreate(16, sizeof(CommandAck));
    _statusMutex = xSemaphoreCreateMutex();
    _configMutex = xSemaphoreCreateMutex();
    _commandMutex = xSemaphoreCreateMutex();
    _overflowMutex = xSemaphoreCreateMutex();
    _historyBackfillMutex = xSemaphoreCreateMutex();
    if (!_commandQueue || !_ackQueue || !_statusMutex || !_configMutex || !_commandMutex ||
        !_overflowMutex || !_historyBackfillMutex) {
        LOG_ERROR(TAG, "queue or mutex allocation failed");
        return false;
    }

    configure(config);
    setConnectivity(appState.wifiConnected);
    if (_logFsOk) {
        mkdir("/logfs/cloud", 0777);
        _recoverOutbox();
        _loadSequenceAndCount();
        _loadHistoryBackfillProgress();
    }
    if (xTaskCreate(_taskFunc, "cloud_sync", 10240, this, 1, &_task) != pdPASS) {
        LOG_ERROR(TAG, "worker creation failed");
        return false;
    }
    _workerReady.store(true);
    const ConnectionConfig connection = _connectionConfig();
    const size_t tokenLength = std::strlen(connection.token);
    LOG_INFO(TAG, "configured=%s device=%s token=****%s", isConfigured() ? "yes" : "no",
             connection.deviceId, tokenLength >= 4 ? connection.token + tokenLength - 4 : "");
    return true;
}

void CloudSyncClient::configure(const AppConfig& config) {
    if (!_configMutex || xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    _connection.enabled = config.cloudEnabled;
    _connection.timezoneOffsetSec = config.timezoneOffsetSec + config.daylightOffsetSec;
    std::snprintf(_connection.baseUrl, sizeof(_connection.baseUrl), "%s",
                  config.cloudBaseUrl.c_str());
    std::snprintf(_connection.deviceId, sizeof(_connection.deviceId), "%s",
                  config.cloudDeviceId.c_str());
    std::snprintf(_connection.token, sizeof(_connection.token), "%s",
                  config.cloudDeviceToken.c_str());
    xSemaphoreGive(_configMutex);
}

CloudSyncClient::ConnectionConfig CloudSyncClient::_connectionConfig() const {
    ConnectionConfig result = {};
    if (_configMutex && xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = _connection;
        xSemaphoreGive(_configMutex);
    }
    return result;
}

void CloudSyncClient::updateStatus(const CloudDeviceStatus& status) {
    if (_statusMutex && xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        _status = status;
        xSemaphoreGive(_statusMutex);
    }
}

void CloudSyncClient::enqueueDrink(const char* timestamp, uint32_t uptimeMs,
                                   float amountMl, float totalMl,
                                   uint32_t drinkCount) {
    _enqueue("drink", timestamp, uptimeMs, amountMl, totalMl, drinkCount);
}

void CloudSyncClient::enqueueRefill(const char* timestamp, uint32_t uptimeMs,
                                    float amountMl) {
    _enqueue("refill", timestamp, uptimeMs, amountMl, 0.0f, 0);
}

bool CloudSyncClient::receiveCommand(CloudCommand& command) {
    return _commandQueue && xQueueReceive(_commandQueue, &command, 0) == pdTRUE;
}

void CloudSyncClient::acknowledgeCommand(const CloudCommand& command, bool ok,
                                         const CloudAppliedSettings& currentSettings) {
    if (!_ackQueue) return;
    if (ok) {
        if (!_configMutex) return;
        xSemaphoreTake(_configMutex, portMAX_DELAY);
        _pendingSettings = currentSettings;
        _configPersistPending = true;
        ++_configGeneration;
        xSemaphoreGive(_configMutex);
    }
    CommandAck ack = {};
    std::strncpy(ack.id, command.id, sizeof(ack.id) - 1);
    ack.revision = command.revision;
    ack.ok = ok;
    if (xQueueSend(_ackQueue, &ack, 0) != pdTRUE) {
        if (!_storeDeferredAck(ack)) LOG_ERROR(TAG, "deferred command ack storage full id=%s", ack.id);
    } else {
        _clearCommandInFlight(command.id, command.revision);
        _syncRequested.store(true);
    }
}

void CloudSyncClient::persistSettings(const CloudAppliedSettings& settings) {
    if (!_configMutex) return;
    xSemaphoreTake(_configMutex, portMAX_DELAY);
    _pendingSettings = settings;
    _configPersistPending = true;
    ++_configGeneration;
    xSemaphoreGive(_configMutex);
}

bool CloudSyncClient::isConfigured() const {
    const ConnectionConfig config = _connectionConfig();
    return _workerReady.load() && _logFsOk && config.enabled &&
           config.timezoneOffsetSec == 8 * 3600 &&
           std::strncmp(config.baseUrl, "https://", 8) == 0 &&
           config.deviceId[0] && std::strlen(config.token) == 64;
}

uint32_t CloudSyncClient::lastSyncAgeSec() const {
    const uint32_t syncedAt = _lastSyncMs.load();
    return syncedAt == 0 ? 0 : (hal_millis() - syncedAt) / 1000;
}

std::string CloudSyncClient::pairingCode() const {
    if (!_statusMutex || xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(20)) != pdTRUE) return {};
    const std::string result(_pairingCode);
    xSemaphoreGive(_statusMutex);
    return result;
}

std::string CloudSyncClient::tokenHash() const {
    const ConnectionConfig config = _connectionConfig();
    if (std::strlen(config.token) != 64) return {};
    unsigned char digest[32] = {};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(config.token),
                       std::strlen(config.token), digest, 0) != 0) return {};
    char encoded[65] = {};
    for (size_t i = 0; i < sizeof(digest); ++i) {
        std::snprintf(encoded + i * 2, 3, "%02x", digest[i]);
    }
    return encoded;
}

bool CloudSyncClient::requestHistoryBackfill() {
    if (!isConfigured() || !_historyBackfillMutex) return false;
    xSemaphoreTake(_historyBackfillMutex, portMAX_DELAY);
    if (_historyBackfillActive.load()) {
        xSemaphoreGive(_historyBackfillMutex);
        return true;
    }
    const bool saved = _saveHistoryBackfillProgress(true, "");
    if (saved) {
        _historyBackfillCursor[0] = '\0';
        _historyBackfillUploadedDays.store(0);
        _historyBackfillHttpStatus.store(0);
        _historyBackfillActive.store(true);
        _historyBackfillState.store(CloudHistoryBackfillState::QUEUED);
    }
    xSemaphoreGive(_historyBackfillMutex);
    return saved;
}

const char* CloudSyncClient::historyBackfillState() const {
    switch (_historyBackfillState.load()) {
        case CloudHistoryBackfillState::QUEUED: return "queued";
        case CloudHistoryBackfillState::UPLOADING: return "uploading";
        case CloudHistoryBackfillState::RETRYING: return "retrying";
        case CloudHistoryBackfillState::COMPLETE: return "complete";
        default: return "idle";
    }
}

void CloudSyncClient::_taskFunc(void* param) {
    static_cast<CloudSyncClient*>(param)->_taskLoop();
}

void CloudSyncClient::_taskLoop() {
    uint32_t lastAttemptMs = 0;
    uint32_t lastHistoryAttemptMs = 0;
    for (;;) {
        // Stand down entirely during OTA: uploads open a second TLS session (~40 KB heap)
        // and touch logfs, both of which compete with writing the app partition. Queued
        // events are preserved and drain once the update finishes or fails.
        if (_appState && _appState->otaInProgress.load()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        _drainOverflowEvents();
        _drainDeferredAcks();
        _persistPendingConfig();
        const uint32_t now = hal_millis();
        const bool requested = _syncRequested.exchange(false);
        if (isConfigured() && _wifiConnected.load() &&
            (requested || now - lastAttemptMs >= SYNC_INTERVAL_MS)) {
            lastAttemptMs = now;
            _lastSyncOk.store(_syncOnce());
        }
        if (_historyBackfillActive.load() && isConfigured() && _wifiConnected.load() &&
            (lastHistoryAttemptMs == 0 || now - lastHistoryAttemptMs >= SYNC_INTERVAL_MS)) {
            lastHistoryAttemptMs = now;
            _historyBackfillOnce();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

bool CloudSyncClient::_appendEvent(const EventMessage& source, bool allowOverflow) {
    EventMessage event = source;
    const bool assignedSequence = event.sequence == 0;
    if (assignedSequence) event.sequence = _nextSequence++;
    bool ok = false;
    const bool filesystemLocked = _logFsOk && _eventLogger &&
        _eventLogger->lockFilesystem(pdMS_TO_TICKS(2000));
    cJSON* doc = filesystemLocked ? cJSON_CreateObject() : nullptr;
    if (doc) {
        cJSON_AddNumberToObject(doc, "seq", static_cast<double>(event.sequence));
        cJSON_AddStringToObject(doc, "type", event.type);
        if (event.timestamp[0]) cJSON_AddStringToObject(doc, "occurredAt", event.timestamp);
        else cJSON_AddNullToObject(doc, "occurredAt");
        cJSON_AddNumberToObject(doc, "uptimeMs", event.uptimeMs);
        cJSON_AddStringToObject(doc, "timeQuality", event.timestamp[0] ? "synced" : "uptime_only");
        cJSON_AddNumberToObject(doc, "amountMl", event.amountMl);
        if (std::strcmp(event.type, "drink") == 0) {
            cJSON_AddNumberToObject(doc, "totalMl", event.totalMl);
            cJSON_AddNumberToObject(doc, "count", event.drinkCount);
        }
        const std::string line = encode(doc);
        cJSON_Delete(doc);
        FILE* file = std::fopen(OUTBOX_PATH, "a+");
        if (file) {
            std::fseek(file, 0, SEEK_END);
            const long startOffset = std::ftell(file);
            const std::string record = line + '\n';
            const size_t written = startOffset >= 0 && startOffset < MAX_OUTBOX_BYTES
                                       ? std::fwrite(record.data(), 1, record.size(), file) : 0;
            if (written == record.size() && std::fflush(file) == 0 &&
                fsync(fileno(file)) == 0) {
                ok = true;
            } else if (startOffset >= 0) {
                std::fflush(file);
                if (ftruncate(fileno(file), startOffset) == 0) fsync(fileno(file));
            }
            if (std::fclose(file) != 0) ok = false;
        }
    }
    if (filesystemLocked) _eventLogger->unlockFilesystem();

    if (ok) {
        if (assignedSequence) _saveSequence(event.sequence);
        _pendingEvents++;
        _syncRequested.store(true);
        return true;
    }
    if (allowOverflow && _saveOverflowEvent(event)) {
        if (assignedSequence) _saveSequence(event.sequence);
        _pendingEvents++;
        _syncRequested.store(true);
        LOG_WARN(TAG, "event stored in NVS overflow seq=%llu",
                 static_cast<unsigned long long>(event.sequence));
        return true;
    }
    if (allowOverflow) {
        _droppedEvents++;
        LOG_ERROR(TAG, "outbox and NVS overflow unavailable seq=%llu",
                  static_cast<unsigned long long>(event.sequence));
    }
    return false;
}

bool CloudSyncClient::_loadOverflowStore(OverflowStore& store) {
    store = OverflowStore{};
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("cloud_sync", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(store);
        const esp_err_t result = nvs_get_blob(handle, "evt_overflow", &store, &size);
        ok = result == ESP_ERR_NVS_NOT_FOUND ||
             (result == ESP_OK && size == sizeof(store) && store.version == 1 && store.count <= 8);
        if (result == ESP_ERR_NVS_NOT_FOUND) store = OverflowStore{};
        nvs_close(handle);
    }
    unlockNvs();
    return ok;
}

bool CloudSyncClient::_saveOverflowStore(const OverflowStore& store) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("cloud_sync", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_blob(handle, "evt_overflow", &store, sizeof(store)) == ESP_OK &&
             nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    return ok;
}

bool CloudSyncClient::_saveOverflowEvent(const EventMessage& event) {
    if (!_overflowMutex) return false;
    xSemaphoreTake(_overflowMutex, portMAX_DELAY);
    OverflowStore store;
    const bool loaded = _loadOverflowStore(store);
    bool ok = false;
    if (loaded && store.count < 8) {
        store.events[store.count++] = event;
        ok = _saveOverflowStore(store);
    }
    xSemaphoreGive(_overflowMutex);
    return ok;
}

void CloudSyncClient::_drainOverflowEvents() {
    if (!_overflowMutex || xSemaphoreTake(_overflowMutex, 0) != pdTRUE) return;
    OverflowStore store;
    if (!_loadOverflowStore(store) || store.count == 0) {
        xSemaphoreGive(_overflowMutex);
        return;
    }
    if (_appendEvent(store.events[0], false)) {
        for (uint32_t i = 1; i < store.count; ++i) store.events[i - 1] = store.events[i];
        std::memset(&store.events[store.count - 1], 0, sizeof(EventMessage));
        --store.count;
        _saveOverflowStore(store);
        if (_pendingEvents.load() > 0) _pendingEvents--;
    }
    xSemaphoreGive(_overflowMutex);
}

bool CloudSyncClient::_syncOnce() {
    const ConnectionConfig connection = _connectionConfig();
    CommandAck acks[MAX_BATCH_ACKS] = {};
    uint8_t ackCount = 0;
    while (ackCount < MAX_BATCH_ACKS &&
           xQueueReceive(_ackQueue, &acks[ackCount], 0) == pdTRUE) {
        ++ackCount;
    }
    if (ackCount > 0 && !_persistPendingConfig()) {
        for (int i = ackCount - 1; i >= 0; --i) xQueueSendToFront(_ackQueue, &acks[i], 0);
        return false;
    }
    for (uint8_t i = 0; i < ackCount; ++i) {
        if (acks[i].ok && !_recordCommandApplied(acks[i].id, acks[i].revision)) {
            for (int j = ackCount - 1; j >= 0; --j) xQueueSendToFront(_ackQueue, &acks[j], 0);
            return false;
        }
    }

    if (!_eventLogger || !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) {
        for (int i = ackCount - 1; i >= 0; --i) xQueueSendToFront(_ackQueue, &acks[i], 0);
        return false;
    }
    cJSON* request = cJSON_CreateObject();
    if (!request) {
        _eventLogger->unlockFilesystem();
        for (int i = ackCount - 1; i >= 0; --i) xQueueSendToFront(_ackQueue, &acks[i], 0);
        return false;
    }
    cJSON_AddNumberToObject(request, "schemaVersion", 1);
    cJSON_AddStringToObject(request, "deviceId", connection.deviceId);
    cJSON_AddStringToObject(request, "firmwareVersion", APP_VERSION);
    cJSON_AddNumberToObject(request, "sentAtUptimeMs", hal_millis());
    cJSON* status = cJSON_AddObjectToObject(request, "status");
    CloudDeviceStatus current = {};
    if (_statusMutex && xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        current = _status;
        xSemaphoreGive(_statusMutex);
    }
    cJSON_AddNumberToObject(status, "todayTotalMl", current.todayTotalMl);
    cJSON_AddNumberToObject(status, "lastDrinkMl", current.lastDrinkMl);
    cJSON_AddNumberToObject(status, "drinkCount", current.drinkCount);
    cJSON_AddNumberToObject(status, "dailyGoalMl", current.dailyGoalMl);
    cJSON_AddStringToObject(status, "cupState", current.cupState);
    cJSON_AddStringToObject(status, "reminderState", current.reminderState);
    cJSON_AddNumberToObject(status, "reminderRemainingSec", current.reminderRemainingSec);
    cJSON_AddNumberToObject(status, "alertEpisodeUptimeMs", current.alertEpisodeUptimeMs);
    if (current.lastDrinkAt[0]) cJSON_AddStringToObject(status, "lastDrinkAt", current.lastDrinkAt);
    else cJSON_AddNullToObject(status, "lastDrinkAt");

    cJSON* events = cJSON_AddArrayToObject(request, "events");
    FILE* file = std::fopen(OUTBOX_PATH, "r");
    char line[512];
    uint8_t count = 0;
    uint64_t maxSentSequence = _ackedSequence;
    while (file && count < MAX_BATCH_EVENTS && std::fgets(line, sizeof(line), file)) {
        cJSON* event = cJSON_Parse(line);
        cJSON* seq = event ? cJSON_GetObjectItemCaseSensitive(event, "seq") : nullptr;
        if (event && cJSON_IsNumber(seq) && seq->valuedouble > _ackedSequence) {
            const uint64_t value = static_cast<uint64_t>(seq->valuedouble);
            maxSentSequence = std::max(maxSentSequence, value);
            cJSON_AddItemToArray(events, event);
            ++count;
        } else {
            cJSON_Delete(event);
        }
    }
    if (file) std::fclose(file);

    cJSON* commandAcks = cJSON_AddArrayToObject(request, "commandAcks");
    for (uint8_t i = 0; i < ackCount; ++i) {
        cJSON* ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "id", acks[i].id);
        cJSON_AddNumberToObject(ack, "revision", acks[i].revision);
        cJSON_AddStringToObject(ack, "status", acks[i].ok ? "applied" : "rejected");
        cJSON_AddItemToArray(commandAcks, ack);
    }
    const std::string body = encode(request);
    cJSON_Delete(request);
    _eventLogger->unlockFilesystem();

    std::string response;
    int statusCode = 0;
    const bool posted = _post(connection, body, response, statusCode, "/api/v1/device/sync");
    _lastHttpStatus.store(statusCode);
    if (!posted || !_applyResponse(response, maxSentSequence)) {
        for (int i = ackCount - 1; i >= 0; --i) xQueueSendToFront(_ackQueue, &acks[i], 0);
        return false;
    }
    _lastSyncMs.store(hal_millis());
    return true;
}

bool CloudSyncClient::_post(const ConnectionConfig& connection, const std::string& body,
                            std::string& response, int& statusCode, const char* path) {
    std::string url = connection.baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += path;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.cert_pem = CLOUD_ROOT_CA;
    config.timeout_ms = 10000;
    config.event_handler = captureHttpEvent;
    config.user_data = &response;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    std::string authorization = "Bearer ";
    authorization += connection.token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), body.size());
    const esp_err_t result = esp_http_client_perform(client);
    statusCode = esp_http_client_get_status_code(client);
    const bool ok = result == ESP_OK && statusCode >= 200 && statusCode < 300;
    if (!ok) LOG_WARN(TAG, "sync failed error=%s status=%d", esp_err_to_name(result), statusCode);
    esp_http_client_cleanup(client);
    return ok;
}

bool CloudSyncClient::_loadHistoryBackfillProgress() {
    _historyBackfillCursor[0] = '\0';
    _historyBackfillIdentityTag[0] = '\0';
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    uint8_t active = 0;
    bool ok = true;
    const esp_err_t opened = nvs_open("cloud_sync", NVS_READONLY, &handle);
    if (opened == ESP_OK) {
        const esp_err_t activeResult = nvs_get_u8(handle, "hist_active", &active);
        size_t cursorLength = sizeof(_historyBackfillCursor);
        const esp_err_t cursorResult = nvs_get_str(
            handle, "hist_cursor", _historyBackfillCursor, &cursorLength);
        size_t identityLength = sizeof(_historyBackfillIdentityTag);
        const esp_err_t identityResult = nvs_get_str(
            handle, "hist_identity", _historyBackfillIdentityTag, &identityLength);
        ok = (activeResult == ESP_OK || activeResult == ESP_ERR_NVS_NOT_FOUND) &&
             (cursorResult == ESP_OK || cursorResult == ESP_ERR_NVS_NOT_FOUND) &&
             (identityResult == ESP_OK || identityResult == ESP_ERR_NVS_NOT_FOUND);
        nvs_close(handle);
    } else if (opened != ESP_ERR_NVS_NOT_FOUND) {
        ok = false;
    }
    unlockNvs();
    if (!validHistoryMonth(_historyBackfillCursor)) _historyBackfillCursor[0] = '\0';
    _historyBackfillActive.store(ok && active != 0);
    _historyBackfillState.store(ok && active != 0
        ? CloudHistoryBackfillState::QUEUED : CloudHistoryBackfillState::IDLE);
    return ok;
}

bool CloudSyncClient::_saveHistoryBackfillProgress(bool active, const char* cursorMonth) {
    const std::string identity = _historyBackfillIdentity();
    if (identity.size() != 64) return false;
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("cloud_sync", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_u8(handle, "hist_active", active ? 1 : 0) == ESP_OK &&
             nvs_set_str(handle, "hist_cursor", cursorMonth ? cursorMonth : "") == ESP_OK &&
             nvs_set_str(handle, "hist_identity", identity.c_str()) == ESP_OK &&
             nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    if (ok) {
        std::strncpy(_historyBackfillIdentityTag, identity.c_str(),
                     sizeof(_historyBackfillIdentityTag) - 1);
        _historyBackfillIdentityTag[sizeof(_historyBackfillIdentityTag) - 1] = '\0';
    }
    return ok;
}

std::string CloudSyncClient::_historyBackfillIdentity() const {
    const ConnectionConfig connection = _connectionConfig();
    if (!connection.baseUrl[0] || !connection.deviceId[0] || !connection.token[0]) return {};
    std::string input(connection.baseUrl);
    input.push_back('\0');
    input += connection.deviceId;
    input.push_back('\0');
    input += connection.token;
    unsigned char digest[32] = {};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()),
                       input.size(), digest, 0) != 0) return {};
    char encoded[65] = {};
    for (size_t index = 0; index < sizeof(digest); ++index) {
        std::snprintf(encoded + index * 2, 3, "%02x", digest[index]);
    }
    return encoded;
}

bool CloudSyncClient::_nextHistoryMonth(const char* afterMonth, std::string& month) {
    month.clear();
    if (!_eventLogger || !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) return false;
    DIR* directory = opendir("/logfs/logs");
    if (!directory) {
        _eventLogger->unlockFilesystem();
        return false;
    }
    for (dirent* entry = readdir(directory); entry; entry = readdir(directory)) {
        const char* name = entry->d_name;
        if (std::strlen(name) != HISTORY_LOG_FILENAME_LENGTH ||
            std::strncmp(name, "drink-", 6) != 0 ||
            std::strcmp(name + HISTORY_LOG_SUFFIX_OFFSET, ".jsonl") != 0) continue;
        char candidate[8] = {};
        std::memcpy(candidate, name + 6, 7);
        if (!validHistoryMonth(candidate) ||
            (afterMonth && afterMonth[0] && std::strcmp(candidate, afterMonth) <= 0)) continue;
        if (month.empty() || month > candidate) month = candidate;
    }
    closedir(directory);
    _eventLogger->unlockFilesystem();
    return true;
}

bool CloudSyncClient::_buildHistoryBatch(const std::string& month, const char* today,
                                         HistoryDay* days, uint8_t& dayCount) {
    dayCount = 0;
    if (!days || !_eventLogger) return false;
    char path[64] = {};
    std::snprintf(path, sizeof(path), "/logfs/logs/drink-%s.jsonl", month.c_str());
    long offset = 0;
    bool finished = false;
    while (!finished) {
        char lines[8][256] = {};
        uint8_t lineCount = 0;
        if (!_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) return false;
        FILE* file = std::fopen(path, "r");
        bool ioOk = file && std::fseek(file, offset, SEEK_SET) == 0;
        while (ioOk && lineCount < 8 && std::fgets(lines[lineCount], sizeof(lines[lineCount]), file)) {
            ++lineCount;
        }
        const long nextOffset = file ? std::ftell(file) : -1;
        finished = file && std::feof(file);
        if (file && std::ferror(file)) ioOk = false;
        if (file && std::fclose(file) != 0) ioOk = false;
        _eventLogger->unlockFilesystem();
        if (!ioOk || nextOffset < offset || (lineCount == 0 && !finished)) return false;
        offset = nextOffset;

        for (uint8_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
            cJSON* entry = cJSON_Parse(lines[lineIndex]);
            cJSON* timestamp = entry ? cJSON_GetObjectItemCaseSensitive(entry, "ts") : nullptr;
            cJSON* total = entry ? cJSON_GetObjectItemCaseSensitive(entry, "total") : nullptr;
            const char* value = cJSON_IsString(timestamp) ? timestamp->valuestring : nullptr;
            const bool validDate = validHistoryTimestamp(value, month, today);
            if (!validDate || !cJSON_IsNumber(total) || !std::isfinite(total->valuedouble) ||
                total->valuedouble < 0 || total->valuedouble > 100000) {
                cJSON_Delete(entry);
                continue;
            }
            char localDate[11] = {};
            std::memcpy(localDate, value, 10);
            uint8_t index = 0;
            while (index < dayCount && std::strcmp(days[index].localDate, localDate) != 0) ++index;
            if (index == dayCount) {
                if (dayCount >= MAX_HISTORY_BATCH_DAYS) {
                    cJSON_Delete(entry);
                    continue;
                }
                std::strncpy(days[index].localDate, localDate, sizeof(days[index].localDate) - 1);
                ++dayCount;
            }
            days[index].totalMl = std::max(days[index].totalMl,
                                           static_cast<float>(total->valuedouble));
            if (days[index].drinkCount < 10000) ++days[index].drinkCount;
            if (!days[index].lastDrinkAt[0] ||
                std::strcmp(value, days[index].lastDrinkAt) > 0) {
                std::strncpy(days[index].lastDrinkAt, value,
                             sizeof(days[index].lastDrinkAt) - 1);
            }
            cJSON_Delete(entry);
        }
    }
    return true;
}

bool CloudSyncClient::_historyBackfillOnce() {
    if (!_historyBackfillMutex) return false;
    char cursor[8] = {};
    xSemaphoreTake(_historyBackfillMutex, portMAX_DELAY);
    const std::string currentIdentity = _historyBackfillIdentity();
    if (currentIdentity.empty() || currentIdentity != _historyBackfillIdentityTag) {
        if (currentIdentity.empty() || !_saveHistoryBackfillProgress(true, "")) {
            _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
            xSemaphoreGive(_historyBackfillMutex);
            return false;
        }
        _historyBackfillCursor[0] = '\0';
        _historyBackfillUploadedDays.store(0);
    }
    std::strncpy(cursor, _historyBackfillCursor, sizeof(cursor) - 1);
    _historyBackfillState.store(CloudHistoryBackfillState::UPLOADING);
    xSemaphoreGive(_historyBackfillMutex);

    char today[11] = {};
    if (!currentLocalDate(today)) {
        _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
        return false;
    }
    std::string month;
    if (!_nextHistoryMonth(cursor, month)) {
        _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
        return false;
    }
    if (month.empty()) {
        xSemaphoreTake(_historyBackfillMutex, portMAX_DELAY);
        const bool saved = _saveHistoryBackfillProgress(false, _historyBackfillCursor);
        if (saved) {
            _historyBackfillActive.store(false);
            _historyBackfillState.store(CloudHistoryBackfillState::COMPLETE);
        } else {
            _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
        }
        xSemaphoreGive(_historyBackfillMutex);
        return saved;
    }

    HistoryDay days[MAX_HISTORY_BATCH_DAYS] = {};
    uint8_t dayCount = 0;
    if (!_buildHistoryBatch(month, today, days, dayCount)) {
        _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
        return false;
    }
    if (dayCount > 0) {
        const ConnectionConfig connection = _connectionConfig();
        cJSON* request = cJSON_CreateObject();
        cJSON* values = request ? cJSON_AddArrayToObject(request, "days") : nullptr;
        if (!request || !values) {
            cJSON_Delete(request);
            _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
            return false;
        }
        cJSON_AddNumberToObject(request, "schemaVersion", 1);
        cJSON_AddStringToObject(request, "deviceId", connection.deviceId);
        cJSON_AddStringToObject(request, "firmwareVersion", APP_VERSION);
        for (uint8_t index = 0; index < dayCount; ++index) {
            cJSON* day = cJSON_CreateObject();
            cJSON_AddStringToObject(day, "localDate", days[index].localDate);
            cJSON_AddNumberToObject(day, "totalMl", days[index].totalMl);
            cJSON_AddNumberToObject(day, "drinkCount", days[index].drinkCount);
            cJSON_AddStringToObject(day, "lastDrinkAt", days[index].lastDrinkAt);
            cJSON_AddItemToArray(values, day);
        }
        const std::string body = encode(request);
        cJSON_Delete(request);
        std::string response;
        int statusCode = 0;
        const bool posted = _post(connection, body, response, statusCode,
                                  "/api/v1/device/history-backfill");
        _historyBackfillHttpStatus.store(statusCode);
        cJSON* result = posted ? cJSON_Parse(response.c_str()) : nullptr;
        cJSON* ok = result ? cJSON_GetObjectItemCaseSensitive(result, "ok") : nullptr;
        cJSON* accepted = result ? cJSON_GetObjectItemCaseSensitive(result, "acceptedDays") : nullptr;
        const bool acknowledged = cJSON_IsTrue(ok) && cJSON_IsNumber(accepted) &&
            accepted->valuedouble == dayCount;
        cJSON_Delete(result);
        if (!acknowledged) {
            _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
            return false;
        }
    }

    xSemaphoreTake(_historyBackfillMutex, portMAX_DELAY);
    const bool saved = _saveHistoryBackfillProgress(true, month.c_str());
    if (saved) {
        std::strncpy(_historyBackfillCursor, month.c_str(), sizeof(_historyBackfillCursor) - 1);
        _historyBackfillUploadedDays.fetch_add(dayCount);
        _historyBackfillState.store(CloudHistoryBackfillState::QUEUED);
    } else {
        _historyBackfillState.store(CloudHistoryBackfillState::RETRYING);
    }
    xSemaphoreGive(_historyBackfillMutex);
    return saved;
}

bool CloudSyncClient::_applyResponse(const std::string& response, uint64_t maxSentSequence) {
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return false;
    bool ok = true;
    cJSON* acked = cJSON_GetObjectItemCaseSensitive(root, "ackedThroughSeq");
    if (!cJSON_IsNumber(acked) || acked->valuedouble < _ackedSequence ||
        acked->valuedouble > maxSentSequence) {
        ok = false;
    } else {
        const uint64_t acknowledged = static_cast<uint64_t>(acked->valuedouble);
        if (acknowledged > _ackedSequence) {
            ok = _saveAckedSequence(acknowledged);
            if (ok) {
                _ackedSequence = acknowledged;
                if (!_rewriteOutbox(acknowledged)) {
                    LOG_WARN(TAG, "outbox compaction deferred after durable ack");
                }
            }
        }
    }
    if (!ok) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* pairing = cJSON_GetObjectItemCaseSensitive(root, "pairingCode");
    if (cJSON_IsString(pairing) && pairing->valuestring && _statusMutex &&
        xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        std::strncpy(_pairingCode, pairing->valuestring, sizeof(_pairingCode) - 1);
        _pairingCode[sizeof(_pairingCode) - 1] = '\0';
        xSemaphoreGive(_statusMutex);
    }
    cJSON* deviceBound = cJSON_GetObjectItemCaseSensitive(root, "deviceBound");
    if (cJSON_IsTrue(deviceBound) && _statusMutex &&
        xSemaphoreTake(_statusMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        _pairingCode[0] = '\0';
        xSemaphoreGive(_statusMutex);
    }

    cJSON* commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, commands) {
        CloudCommand command = {};
        cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON* type = cJSON_GetObjectItemCaseSensitive(item, "type");
        cJSON* revision = cJSON_GetObjectItemCaseSensitive(item, "revision");
        cJSON* value = cJSON_GetObjectItemCaseSensitive(item, "value");
        cJSON* untilDate = cJSON_GetObjectItemCaseSensitive(item, "untilDate");
        if (!cJSON_IsString(id) || !id->valuestring || std::strlen(id->valuestring) >= sizeof(command.id) ||
            !cJSON_IsString(type) || !type->valuestring) continue;
        std::strncpy(command.id, id->valuestring, sizeof(command.id) - 1);
        command.type = _commandType(type->valuestring);
        command.revision = cJSON_IsNumber(revision) && revision->valuedouble >= 0
                               ? static_cast<uint32_t>(revision->valuedouble) : 0;
        if (command.type == CloudCommandType::SET_SETTINGS && cJSON_IsObject(value)) {
            cJSON* enabled = cJSON_GetObjectItemCaseSensitive(value, "reminderEnabled");
            cJSON* interval = cJSON_GetObjectItemCaseSensitive(value, "reminderIntervalMin");
            cJSON* goal = cJSON_GetObjectItemCaseSensitive(value, "dailyGoalMl");
            if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(interval) || !cJSON_IsNumber(goal)) continue;
            command.reminderEnabled = cJSON_IsTrue(enabled);
            command.reminderIntervalMin = static_cast<uint32_t>(interval->valuedouble);
            command.dailyGoalMl = static_cast<uint32_t>(goal->valuedouble);
        } else {
            command.uintValue = cJSON_IsNumber(value) ? static_cast<uint32_t>(value->valuedouble) : 0;
            command.boolValue = cJSON_IsBool(value) && cJSON_IsTrue(value);
        }
        if (cJSON_IsString(untilDate) && untilDate->valuestring) {
            std::strncpy(command.stringValue, untilDate->valuestring, sizeof(command.stringValue) - 1);
        }
        if (command.type == CloudCommandType::UNKNOWN) continue;
        if (_isCommandApplied(command.id, command.revision)) {
            CommandAck duplicate = {};
            std::strncpy(duplicate.id, command.id, sizeof(duplicate.id) - 1);
            duplicate.revision = command.revision;
            duplicate.ok = true;
            xQueueSend(_ackQueue, &duplicate, 0);
        } else if (!_isCommandInFlight(command.id, command.revision)) {
            _recordCommandInFlight(command.id, command.revision);
            if (xQueueSend(_commandQueue, &command, 0) != pdTRUE) {
                _clearCommandInFlight(command.id, command.revision);
                LOG_WARN(TAG, "command queue full id=%s", command.id);
            }
        }
    }
    cJSON_Delete(root);
    return true;
}

bool CloudSyncClient::_rewriteOutbox(uint64_t ackedThroughSeq) {
    if (!_eventLogger || !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) return false;
    FILE* input = std::fopen(OUTBOX_PATH, "r");
    if (!input) {
        _pendingEvents.store(0);
        _eventLogger->unlockFilesystem();
        return true;
    }
    FILE* output = std::fopen(OUTBOX_TEMP_PATH, "w");
    if (!output) {
        std::fclose(input);
        _eventLogger->unlockFilesystem();
        return false;
    }
    char line[512];
    uint32_t kept = 0;
    bool writeOk = true;
    while (std::fgets(line, sizeof(line), input)) {
        cJSON* event = cJSON_Parse(line);
        cJSON* seq = event ? cJSON_GetObjectItemCaseSensitive(event, "seq") : nullptr;
        const bool keep = !cJSON_IsNumber(seq) || seq->valuedouble > ackedThroughSeq;
        if (keep) {
            if (std::fputs(line, output) == EOF) writeOk = false;
            ++kept;
        }
        cJSON_Delete(event);
    }
    std::fclose(input);
    writeOk = writeOk && std::fflush(output) == 0 && fsync(fileno(output)) == 0;
    if (std::fclose(output) != 0) writeOk = false;
    if (!writeOk) {
        std::remove(OUTBOX_TEMP_PATH);
        _eventLogger->unlockFilesystem();
        return false;
    }

    std::remove(OUTBOX_BACKUP_PATH);
    bool ok = std::rename(OUTBOX_PATH, OUTBOX_BACKUP_PATH) == 0;
    if (ok) {
        ok = std::rename(OUTBOX_TEMP_PATH, OUTBOX_PATH) == 0;
        if (ok) std::remove(OUTBOX_BACKUP_PATH);
        else std::rename(OUTBOX_BACKUP_PATH, OUTBOX_PATH);
    }
    if (ok) _pendingEvents.store(kept);
    _eventLogger->unlockFilesystem();
    return ok;
}

void CloudSyncClient::_recoverOutbox() {
    if (!_eventLogger || !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) return;
    struct stat info = {};
    const bool outboxExists = stat(OUTBOX_PATH, &info) == 0;
    if (outboxExists) {
        std::remove(OUTBOX_TEMP_PATH);
        std::remove(OUTBOX_BACKUP_PATH);
    } else if (stat(OUTBOX_TEMP_PATH, &info) == 0) {
        if (std::rename(OUTBOX_TEMP_PATH, OUTBOX_PATH) == 0) {
            std::remove(OUTBOX_BACKUP_PATH);
        } else if (stat(OUTBOX_BACKUP_PATH, &info) == 0) {
            std::rename(OUTBOX_BACKUP_PATH, OUTBOX_PATH);
        }
    } else if (stat(OUTBOX_BACKUP_PATH, &info) == 0) {
        std::rename(OUTBOX_BACKUP_PATH, OUTBOX_PATH);
    }
    _eventLogger->unlockFilesystem();
}

void CloudSyncClient::_loadSequenceAndCount() {
    uint64_t last = 0;
    if (lockNvs()) {
        nvs_handle_t handle = 0;
        if (nvs_open("cloud_sync", NVS_READONLY, &handle) == ESP_OK) {
            nvs_get_u64(handle, "last_seq", &last);
            nvs_get_u64(handle, "acked_seq", &_ackedSequence);
            size_t commandsSize = sizeof(_appliedCommands);
            nvs_get_blob(handle, "cmd_ledger", _appliedCommands, &commandsSize);
            uint8_t index = 0;
            nvs_get_u8(handle, "cmd_index", &index);
            _appliedCommandIndex = index % 16;
            nvs_close(handle);
        }
        unlockNvs();
    }
    OverflowStore overflow;
    uint32_t overflowCount = 0;
    uint64_t overflowMax = 0;
    if (_loadOverflowStore(overflow)) {
        overflowCount = overflow.count;
        for (uint32_t i = 0; i < overflow.count; ++i) {
            overflowMax = std::max(overflowMax, overflow.events[i].sequence);
        }
    }
    if (!_eventLogger || !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) {
        _pendingEvents.store(overflowCount);
        _nextSequence = std::max(std::max(last, overflowMax), _ackedSequence) + 1;
        return;
    }
    FILE* file = std::fopen(OUTBOX_PATH, "r");
    uint32_t count = overflowCount;
    uint64_t fileMax = overflowMax;
    char line[512];
    while (file && std::fgets(line, sizeof(line), file)) {
        cJSON* event = cJSON_Parse(line);
        cJSON* seq = event ? cJSON_GetObjectItemCaseSensitive(event, "seq") : nullptr;
        if (cJSON_IsNumber(seq) && seq->valuedouble >= 0) {
            const uint64_t value = static_cast<uint64_t>(seq->valuedouble);
            fileMax = std::max(fileMax, value);
            if (value > _ackedSequence) ++count;
        }
        cJSON_Delete(event);
    }
    if (file) std::fclose(file);
    _pendingEvents.store(count);
    _nextSequence = std::max(std::max(last, fileMax), _ackedSequence) + 1;
    _eventLogger->unlockFilesystem();
}

void CloudSyncClient::_saveSequence(uint64_t sequence) {
    if (!lockNvs()) {
        LOG_ERROR(TAG, "sequence persistence lock failed seq=%llu",
                  static_cast<unsigned long long>(sequence));
        return;
    }
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("cloud_sync", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_u64(handle, "last_seq", sequence) == ESP_OK &&
             nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    if (!ok) LOG_ERROR(TAG, "sequence persistence failed seq=%llu",
                       static_cast<unsigned long long>(sequence));
}

bool CloudSyncClient::_saveAckedSequence(uint64_t sequence) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("cloud_sync", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_u64(handle, "acked_seq", sequence) == ESP_OK &&
             nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    return ok;
}

bool CloudSyncClient::_persistPendingConfig() {
    CloudAppliedSettings pending;
    bool shouldPersist = false;
    uint32_t generation = 0;
    if (!_configMutex || xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    shouldPersist = _configPersistPending;
    if (shouldPersist) {
        pending = _pendingSettings;
        generation = _configGeneration;
    }
    xSemaphoreGive(_configMutex);
    if (!shouldPersist) return true;
    if (!_configManager || !_configManager->saveReminderSettings(
            pending.reminderEnabled, pending.reminderIntervalMin,
            pending.dailyGoalMl, pending.pausedUntilDate)) return false;
    if (xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (_configGeneration == generation) _configPersistPending = false;
    xSemaphoreGive(_configMutex);
    return true;
}

bool CloudSyncClient::_isCommandApplied(const char* id, uint32_t revision) const {
    if (!id || !id[0]) return false;
    for (const CommandRecord& value : _appliedCommands) {
        if (value.revision == revision && std::strcmp(value.id, id) == 0) return true;
    }
    return false;
}

bool CloudSyncClient::_isCommandInFlight(const char* id, uint32_t revision) const {
    if (!id || !id[0]) return false;
    if (!_commandMutex || xSemaphoreTake(_commandMutex, pdMS_TO_TICKS(20)) != pdTRUE) return true;
    bool found = false;
    for (const CommandRecord& value : _inflightCommands) {
        if (value.revision == revision && std::strcmp(value.id, id) == 0) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(_commandMutex);
    return found;
}

bool CloudSyncClient::_recordCommandApplied(const char* id, uint32_t revision) {
    if (!id || !id[0] || _isCommandApplied(id, revision)) return true;
    const uint8_t recordIndex = _appliedCommandIndex;
    const uint8_t nextIndex = (_appliedCommandIndex + 1) % 16;
    CommandRecord candidate[16];
    std::memcpy(candidate, _appliedCommands, sizeof(candidate));
    std::memset(&candidate[recordIndex], 0, sizeof(candidate[recordIndex]));
    std::strncpy(candidate[recordIndex].id, id, sizeof(candidate[recordIndex].id) - 1);
    candidate[recordIndex].revision = revision;
    if (!lockNvs()) {
        LOG_ERROR(TAG, "command ledger lock failed id=%s", id);
        return false;
    }
    nvs_handle_t handle = 0;
    bool ok = false;
    if (nvs_open("cloud_sync", NVS_READWRITE, &handle) == ESP_OK) {
        ok = nvs_set_blob(handle, "cmd_ledger", candidate, sizeof(candidate)) == ESP_OK &&
             nvs_set_u8(handle, "cmd_index", nextIndex) == ESP_OK &&
             nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    unlockNvs();
    if (ok) {
        std::memcpy(_appliedCommands, candidate, sizeof(candidate));
        _appliedCommandIndex = nextIndex;
    } else LOG_ERROR(TAG, "command ledger persistence failed id=%s", id);
    return ok;
}

void CloudSyncClient::_recordCommandInFlight(const char* id, uint32_t revision) {
    if (!_commandMutex || xSemaphoreTake(_commandMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    CommandRecord& record = _inflightCommands[_inflightCommandIndex];
    std::memset(&record, 0, sizeof(record));
    std::strncpy(record.id, id ? id : "", sizeof(record.id) - 1);
    record.revision = revision;
    _inflightCommandIndex = (_inflightCommandIndex + 1) % 8;
    xSemaphoreGive(_commandMutex);
}

void CloudSyncClient::_clearCommandInFlight(const char* id, uint32_t revision) {
    if (!_commandMutex || xSemaphoreTake(_commandMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    for (CommandRecord& value : _inflightCommands) {
        if (value.revision == revision && std::strcmp(value.id, id ? id : "") == 0) {
            std::memset(&value, 0, sizeof(value));
        }
    }
    xSemaphoreGive(_commandMutex);
}

bool CloudSyncClient::_storeDeferredAck(const CommandAck& ack) {
    if (!_commandMutex) return false;
    xSemaphoreTake(_commandMutex, portMAX_DELAY);
    for (CommandAck& value : _deferredAcks) {
        if (!value.id[0]) {
            value = ack;
            xSemaphoreGive(_commandMutex);
            return true;
        }
    }
    xSemaphoreGive(_commandMutex);
    return false;
}

void CloudSyncClient::_drainDeferredAcks() {
    if (!_commandMutex || xSemaphoreTake(_commandMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    for (CommandAck& value : _deferredAcks) {
        if (value.id[0] && xQueueSend(_ackQueue, &value, 0) == pdTRUE) {
            const CommandAck delivered = value;
            std::memset(&value, 0, sizeof(value));
            xSemaphoreGive(_commandMutex);
            _clearCommandInFlight(delivered.id, delivered.revision);
            _syncRequested.store(true);
            if (xSemaphoreTake(_commandMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
        }
    }
    xSemaphoreGive(_commandMutex);
}

void CloudSyncClient::_enqueue(const char* type, const char* timestamp,
                               uint32_t uptimeMs, float amountMl, float totalMl,
                               uint32_t drinkCount) {
    EventMessage event = {};
    std::strncpy(event.type, type ? type : "unknown", sizeof(event.type) - 1);
    std::strncpy(event.timestamp, timestamp ? timestamp : "", sizeof(event.timestamp) - 1);
    event.uptimeMs = uptimeMs;
    event.amountMl = amountMl;
    event.totalMl = totalMl;
    event.drinkCount = drinkCount;
    _appendEvent(event);
}

CloudCommandType CloudSyncClient::_commandType(const char* value) {
    if (!value) return CloudCommandType::UNKNOWN;
    if (std::strcmp(value, "set_settings") == 0) return CloudCommandType::SET_SETTINGS;
    if (std::strcmp(value, "set_reminder_enabled") == 0) return CloudCommandType::SET_REMINDER_ENABLED;
    if (std::strcmp(value, "set_reminder_interval_min") == 0) return CloudCommandType::SET_REMINDER_INTERVAL_MIN;
    if (std::strcmp(value, "set_daily_goal_ml") == 0) return CloudCommandType::SET_DAILY_GOAL_ML;
    if (std::strcmp(value, "snooze_minutes") == 0) return CloudCommandType::SNOOZE_MINUTES;
    if (std::strcmp(value, "pause_today") == 0) return CloudCommandType::PAUSE_TODAY;
    return CloudCommandType::UNKNOWN;
}
