#include "DashboardServer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <type_traits>

#include "AppState.h"
#include "BuzzerController.h"
#include "CloudSyncClient.h"
#include "ConfigManager.h"
#include "DiscordNotifier.h"
#include "EventLogger.h"
#include "HistoryMaintenance.h"
#include "OtaUpdater.h"
#include "ReminderManager.h"
#include "ScaleManager.h"
#include "WiFiManager.h"
#include "config.h"
#include "cJSON.h"
#include "esp_system.h"
#include "hal_log.h"
#include "hal_time.h"
#include "hydracup_auth.h"
#include "http_server_support.h"
#include "version.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "Dashboard";

std::string jsonString(cJSON* object) {
    char* text = cJSON_PrintUnformatted(object);
    if (!text) return "{}";
    std::string result(text); std::free(text); return result;
}

cJSON* parseBody(httpd_req_t* request, std::string& body) {
    if (!http_read_body(request, body)) return nullptr;
    return cJSON_ParseWithLength(body.c_str(), body.size());
}

bool has(cJSON* object, const char* key) { return cJSON_GetObjectItemCaseSensitive(object, key) != nullptr; }
bool number(cJSON* object, const char* key) { cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key); return value && cJSON_IsNumber(value); }
bool boolean(cJSON* object, const char* key) { cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key); return value && cJSON_IsBool(value); }
std::string stringValue(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return value && cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool normalizeCloudOrigin(const std::string& input, std::string& output) {
    const auto isSpace = [](unsigned char value) { return std::isspace(value) != 0; };
    auto first = std::find_if_not(input.begin(), input.end(), isSpace);
    auto last = std::find_if_not(input.rbegin(), input.rend(), isSpace).base();
    output = first < last ? std::string(first, last) : "";
    while (!output.empty() && output.back() == '/') output.pop_back();
    if (output.empty()) return true;
    if (output.size() >= 192) return false;
    constexpr size_t schemeLength = 8;
    if (output.rfind("https://", 0) != 0 || output.size() == schemeLength) return false;
    const std::string authority = output.substr(schemeLength);
    if (authority.find_first_of("/?#@") != std::string::npos ||
        std::any_of(authority.begin(), authority.end(), isSpace)) return false;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        if (authority.find(':') != colon || colon == 0 || colon + 1 == authority.size()) return false;
        const std::string portText = authority.substr(colon + 1);
        if (!std::all_of(portText.begin(), portText.end(), [](unsigned char value) { return std::isdigit(value) != 0; })) return false;
        const unsigned long port = std::strtoul(portText.c_str(), nullptr, 10);
        if (port == 0 || port > 65535) return false;
    }
    const std::string host = authority.substr(0, colon);
    if (host.empty() || host.front() == '.' || host.back() == '.') return false;
    bool labelStart = true;
    bool previousHyphen = false;
    for (unsigned char value : host) {
        if (value == '.') {
            if (labelStart || previousHyphen) return false;
            labelStart = true; previousHyphen = false; continue;
        }
        if (!(std::isalnum(value) || value == '-')) return false;
        if (labelStart && value == '-') return false;
        labelStart = false; previousHyphen = value == '-';
    }
    return !labelStart && !previousHyphen;
}
}

void DashboardServer::begin(ScaleManager& scale, ConfigManager& cfgMgr, AppState& state, AppConfig& cfg,
                            BuzzerController& buzzer, ReminderManager& reminder, bool logFsOk,
                            RuntimeCoordinator& runtime, EventLogger& eventLogger,
                            DiscordNotifier& discord, WiFiManager& wifi,
                            CloudSyncClient& cloudSync) {
    _scale = &scale; _cfgMgr = &cfgMgr; _state = &state; _cfg = &cfg; _buzzer = &buzzer;
    _reminder = &reminder; _logFsOk = logFsOk; _runtime = &runtime; _eventLogger = &eventLogger;
    _discord = &discord; _wifi = &wifi; _cloudSync = &cloudSync; _preAuthCsrfToken = http_random_token();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 32;
    config.stack_size = 8192;
    // Browsers keep up to 6 persistent connections open per host, so anything below that
    // strands requests in the TCP backlog until an existing connection is reclaimed - the
    // history page's 13-month heatmap fan-out then takes minutes instead of seconds.
    // CONFIG_LWIP_MAX_SOCKETS=16 leaves room for this alongside MQTT, cloud sync, Discord
    // and the 2-3 connections an OTA download needs while following GitHub's redirect
    // (those workers also stand down for the duration of an update).
    config.max_open_sockets = 8;
    // Close the least recently used idle connection instead of refusing the new one, so a
    // client that holds more connections than budgeted degrades in latency, not in reach.
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&_server, &config) != ESP_OK) {
        LOG_ERROR(TAG, "HTTP server start failed"); return;
    }
    httpd_uri_t get = {}; get.uri = "/*"; get.method = HTTP_GET; get.handler = &_getHandler; get.user_ctx = this;
    httpd_uri_t post = {}; post.uri = "/*"; post.method = HTTP_POST; post.handler = &_postHandler; post.user_ctx = this;
    httpd_register_uri_handler(_server, &get); httpd_register_uri_handler(_server, &post);
    LOG_INFO(TAG, "HTTP server started on port 80");
}

esp_err_t DashboardServer::_getHandler(httpd_req_t* request) {
    return static_cast<DashboardServer*>(request->user_ctx)->_handleGet(request);
}
esp_err_t DashboardServer::_postHandler(httpd_req_t* request) {
    return static_cast<DashboardServer*>(request->user_ctx)->_handlePost(request);
}

void DashboardServer::_sendJson(httpd_req_t* request, const std::string& json, int status) {
    http_send(request, "application/json", json, status);
}

void DashboardServer::_handleStatic(httpd_req_t* request, const char* path, const char* contentType) {
    // webfs_read_begin() holds off while an OTA replaces the partition, so a page request can
    // never be inside fread() when the filesystem is unmounted underneath it.
    if (!_state->fsOk || !webfs_read_begin()) {
        http_send(request, "text/plain", "Web assets unavailable. Run: pio run -t uploadfs", 503);
        return;
    }
    const bool sent = http_send_file(request, path, contentType);
    webfs_read_end();
    if (!sent)
        http_send(request, "text/plain", "Web assets unavailable. Run: pio run -t uploadfs", 503);
}

esp_err_t DashboardServer::_handleGet(httpd_req_t* request) {
    std::string uri(request->uri);
    const size_t queryStart = uri.find('?');
    if (queryStart != std::string::npos) uri.resize(queryStart);
    if (uri == "/api/auth/csrf") {
        const bool authenticated = _isSessionValid(request);
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true);
        cJSON_AddBoolToObject(doc, "configured", !_cfg->adminPasswordHash.empty());
        cJSON_AddBoolToObject(doc, "authenticated", authenticated);
        cJSON_AddStringToObject(doc, "csrf", (authenticated ? _sessionCsrfToken : _preAuthCsrfToken).c_str());
        _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/auth/logout") { _sendJson(request, "{\"ok\":false,\"error\":\"method_not_allowed\"}", 405); return ESP_OK; }
    if (uri == "/api/weight") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        const RuntimeSnapshot snapshot = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true);
        cJSON_AddNumberToObject(doc, "weight_g", _runtime && _runtime->isControlRunning() ? snapshot.weightGrams : _scale->getWeightGrams());
        cJSON_AddNumberToObject(doc, "cup_state", static_cast<int>(snapshot.cupState));
        _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/status") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        const RuntimeSnapshot snapshot = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
        const bool rtos = _runtime && _runtime->isControlRunning();
        const CupState cup = snapshot.cupState;
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true);
        cJSON_AddStringToObject(doc, "mode", "normal");
        cJSON_AddStringToObject(doc, "app_version", APP_VERSION);
        cJSON_AddBoolToObject(doc, "wifi_connected", snapshot.wifiConnected);
        cJSON_AddStringToObject(doc, "ip", snapshot.ipAddress);
        cJSON_AddBoolToObject(doc, "ntp_synced", snapshot.ntpSynced);
        cJSON_AddNumberToObject(doc, "weight_g", snapshot.weightGrams);
        cJSON_AddNumberToObject(doc, "cup_state", static_cast<int>(cup));
        cJSON_AddStringToObject(doc, "cup_state_name", _cupStateStr(cup));
        cJSON_AddNumberToObject(doc, "today_total_ml", snapshot.todayTotalMl);
        cJSON_AddNumberToObject(doc, "daily_goal_ml", snapshot.dailyGoalMl);
        cJSON_AddNumberToObject(doc, "drink_count_today", snapshot.drinkCountToday);
        cJSON_AddNumberToObject(doc, "last_drink_ml", snapshot.lastDrinkMl);
        cJSON_AddNumberToObject(doc, "next_reminder_sec", snapshot.nextReminderSec);
        cJSON_AddStringToObject(doc, "reminder_state", snapshot.reminderState);
        cJSON_AddStringToObject(doc, "last_drink_at", snapshot.lastDrinkAt);
        cJSON_AddBoolToObject(doc, "cloud_configured", _cloudSync && _cloudSync->isConfigured());
        cJSON_AddBoolToObject(doc, "cloud_last_ok", _cloudSync && _cloudSync->lastSyncOk());
        cJSON_AddNumberToObject(doc, "cloud_pending_events", _cloudSync ? _cloudSync->pendingEvents() : 0);
        cJSON_AddNumberToObject(doc, "cloud_last_sync_age_sec", _cloudSync ? _cloudSync->lastSyncAgeSec() : 0);
        cJSON_AddNumberToObject(doc, "cloud_last_http_status", _cloudSync ? _cloudSync->lastHttpStatus() : 0);
        cJSON_AddNumberToObject(doc, "cloud_queue_drops", _cloudSync ? _cloudSync->droppedEvents() : 0);
        cJSON_AddStringToObject(doc, "cloud_history_backfill_state",
                                _cloudSync ? _cloudSync->historyBackfillState() : "idle");
        cJSON_AddBoolToObject(doc, "cloud_history_backfill_running",
                              _cloudSync && _cloudSync->historyBackfillRunning());
        cJSON_AddNumberToObject(doc, "cloud_history_backfill_uploaded_days",
                                _cloudSync ? _cloudSync->historyBackfillUploadedDays() : 0);
        cJSON_AddNumberToObject(doc, "cloud_history_backfill_http_status",
                                _cloudSync ? _cloudSync->historyBackfillHttpStatus() : 0);
        const std::string pairingCode = _cloudSync ? _cloudSync->pairingCode() : "";
        cJSON_AddStringToObject(doc, "cloud_pairing_code", pairingCode.c_str());
        cJSON_AddBoolToObject(doc, "webhook_configured", _state->webhookConfigured);
        cJSON_AddBoolToObject(doc, "webhook_last_ok", _state->webhookLastOk.load());
        cJSON_AddBoolToObject(doc, "discord_worker_ready", _discord && _discord->isWorkerReady());
        cJSON_AddNumberToObject(doc, "discord_queue_drops", _discord ? _discord->getDroppedCount() : 0);
        cJSON_AddBoolToObject(doc, "mqtt_configured", _state->mqttConfigured);
        cJSON_AddBoolToObject(doc, "mqtt_connected", _state->mqttConnected.load());
        cJSON_AddBoolToObject(doc, "hw_hx711", _state->hx711Ok); cJSON_AddBoolToObject(doc, "hw_oled", _state->oledOk);
        cJSON_AddBoolToObject(doc, "hw_fs", _state->fsOk); cJSON_AddBoolToObject(doc, "hw_logfs", _state->logFsOk);
        cJSON_AddBoolToObject(doc, "rtos", rtos); cJSON_AddBoolToObject(doc, "rtos_healthy", _runtime && _runtime->isControlHealthy());
        cJSON_AddNumberToObject(doc, "rtos_sequence", snapshot.sequence); cJSON_AddNumberToObject(doc, "rtos_command_drops", snapshot.commandDrops);
        cJSON_AddNumberToObject(doc, "rtos_result_drops", snapshot.resultDrops); cJSON_AddNumberToObject(doc, "log_queue_drops", _eventLogger ? _eventLogger->getDroppedCount() : 0);
        _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/config") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        const RuntimeSnapshot snapshot = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true);
#define ADD_STR(key, value) cJSON_AddStringToObject(doc, key, value.c_str())
        ADD_STR("wifiSsid", _cfg->wifiSsid); cJSON_AddStringToObject(doc, "wifiPassword", "****"); cJSON_AddBoolToObject(doc, "wifiPasswordSet", !_cfg->wifiPassword.empty());
        ADD_STR("discordWebhookUrl", _maskWebhookUrl(_cfg->discordWebhookUrl)); cJSON_AddBoolToObject(doc, "reminderEnabled", snapshot.reminderEnabled);
        cJSON_AddBoolToObject(doc, "cloudEnabled", _cfg->cloudEnabled); ADD_STR("cloudBaseUrl", _cfg->cloudBaseUrl); ADD_STR("cloudDeviceId", _cfg->cloudDeviceId);
        const std::string cloudTokenHash = _cloudSync ? _cloudSync->tokenHash() : ""; ADD_STR("cloudDeviceTokenHash", cloudTokenHash);
        cJSON_AddNumberToObject(doc, "reminderIntervalMin", snapshot.reminderIntervalMin); cJSON_AddNumberToObject(doc, "reminderAlertTimeoutSec", _cfg->reminderAlertTimeoutSec);
        cJSON_AddNumberToObject(doc, "dailyGoalMl", snapshot.dailyGoalMl); cJSON_AddBoolToObject(doc, "buzzerEnabled", _cfg->buzzerEnabled);
        cJSON_AddNumberToObject(doc, "buzzerFrequencyHz", _cfg->buzzerFrequencyHz); cJSON_AddNumberToObject(doc, "buzzerDurationMs", _cfg->buzzerDurationMs); cJSON_AddNumberToObject(doc, "buzzerVolumePercent", _cfg->buzzerVolumePercent);
        cJSON_AddBoolToObject(doc, "ntpEnabled", _cfg->ntpEnabled); ADD_STR("ntpServer1", _cfg->ntpServer1); ADD_STR("ntpServer2", _cfg->ntpServer2); cJSON_AddNumberToObject(doc, "timezoneOffsetSec", _cfg->timezoneOffsetSec);
        cJSON_AddBoolToObject(doc, "mqttEnabled", _cfg->mqttEnabled); ADD_STR("mqttBrokerHost", _cfg->mqttBrokerHost); cJSON_AddNumberToObject(doc, "mqttBrokerPort", _cfg->mqttBrokerPort); ADD_STR("mqttUsername", _cfg->mqttUsername); cJSON_AddStringToObject(doc, "mqttPassword", "****"); cJSON_AddBoolToObject(doc, "mqttPasswordSet", !_cfg->mqttPassword.empty()); ADD_STR("mqttClientId", _cfg->mqttClientId); cJSON_AddNumberToObject(doc, "mqttHeartbeatSec", _cfg->mqttHeartbeatSec);
#undef ADD_STR
        cJSON_AddNumberToObject(doc, "calibrationFactor", _runtime && _runtime->isControlRunning() ? snapshot.calibrationFactor : _scale->getCalibrationFactor());
        cJSON_AddNumberToObject(doc, "cupPresentThresholdGram", _cfg->cupPresentThresholdGram); cJSON_AddNumberToObject(doc, "stableToleranceGram", _cfg->stableToleranceGram); cJSON_AddNumberToObject(doc, "stableDurationMs", _cfg->stableDurationMs); cJSON_AddNumberToObject(doc, "minDrinkDeltaMl", _cfg->minDrinkDeltaMl); cJSON_AddNumberToObject(doc, "maxDrinkDeltaMl", _cfg->maxDrinkDeltaMl);
        _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/wifi/scan") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        WifiNetwork networks[20]; const int count = _wifi ? _wifi->scan(networks, 20) : -1;
        if (count < 0) { _sendJson(request, "{\"ok\":false,\"error\":\"scan_failed\"}", 503); return ESP_OK; }
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true); cJSON* list = cJSON_AddArrayToObject(doc, "networks");
        for (int i = 0; i < count; ++i) { cJSON* item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "ssid", networks[i].ssid); cJSON_AddNumberToObject(item, "rssi", networks[i].rssi); cJSON_AddBoolToObject(item, "secure", networks[i].secure); cJSON_AddItemToArray(list, item); }
        _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/logs") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        char rawQuery[64] = {}, monthValue[16] = {}; if (httpd_req_get_url_query_str(request, rawQuery, sizeof(rawQuery)) != ESP_OK || httpd_query_key_value(rawQuery, "month", monthValue, sizeof(monthValue)) != ESP_OK) { _sendJson(request, "{\"ok\":false,\"error\":\"month parameter required\"}", 400); return ESP_OK; }
        const std::string month(monthValue); bool valid = month == "unsynced" || (month.size() == 7 && month[4] == '-' && month[5] >= '0' && month[5] <= '1' && month[6] >= '0' && month[6] <= '9');
        if (!valid || !_logFsOk) { _sendJson(request, valid ? "{\"ok\":false,\"error\":\"logfs unavailable\"}" : "{\"ok\":false,\"error\":\"invalid month\"}", valid ? 503 : 400); return ESP_OK; }
        if (_eventLogger && !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) { _sendJson(request, "{\"ok\":false,\"error\":\"logfs_busy\"}", 503); return ESP_OK; }
        char path[64]; std::snprintf(path, sizeof(path), "/logfs/logs/drink-%s.jsonl", month.c_str()); FILE* file = std::fopen(path, "r");
        std::string result = "{\"ok\":true,\"month\":\"" + month + "\",\"entries\":["; bool first = true; uint32_t skipped = 0;
        if (file) { char line[256]; while (std::fgets(line, sizeof(line), file)) { std::string value(line); while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back(); cJSON* entry = cJSON_Parse(value.c_str()); if (!entry) { ++skipped; continue; } char* encoded = cJSON_PrintUnformatted(entry); cJSON_Delete(entry); if (!encoded) { ++skipped; continue; } if (!first) result += ','; result += encoded; std::free(encoded); first = false; } std::fclose(file); }
        if (_eventLogger) _eventLogger->unlockFilesystem();
        result += "],\"skipped\":" + std::to_string(skipped) + "}";
        _sendJson(request, result); return ESP_OK;
    }
    if (uri == "/api/ota/status") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        if (!_ota) { _sendJson(request, "{\"ok\":false,\"error\":\"ota_unavailable\"}", 503); return ESP_OK; }
        const OtaStatus ota = _ota->snapshot();
        cJSON* doc = cJSON_CreateObject();
        cJSON_AddBoolToObject(doc, "ok", true);
        cJSON_AddStringToObject(doc, "check_state", OtaUpdater::checkStateName(ota.checkState));
        cJSON_AddStringToObject(doc, "update_state", OtaUpdater::updateStateName(ota.updateState));
        cJSON_AddStringToObject(doc, "running_version", ota.runningVersion);
        cJSON_AddStringToObject(doc, "latest_version", ota.latestVersion);
        cJSON_AddStringToObject(doc, "running_partition", ota.runningPartition);
        cJSON_AddStringToObject(doc, "stage", ota.stage);
        cJSON_AddNumberToObject(doc, "progress_percent", ota.progressPercent);
        cJSON_AddNumberToObject(doc, "image_size", ota.imageSize);
        cJSON_AddNumberToObject(doc, "bytes_read", ota.bytesRead);
        cJSON_AddNumberToObject(doc, "last_http_status", ota.lastHttpStatus);
        cJSON_AddStringToObject(doc, "message", ota.message);
        cJSON_AddBoolToObject(doc, "pending_verify", ota.pendingVerify);
        _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/") { if (_requirePageAuth(request, "/")) _handleStatic(request, "/webfs/index.html", "text/html"); return ESP_OK; }
    if (uri == "/settings") { if (_requirePageAuth(request, "/settings")) _handleStatic(request, "/webfs/settings.html", "text/html"); return ESP_OK; }
    if (uri == "/history") { if (_requirePageAuth(request, "/history")) _handleStatic(request, "/webfs/history.html", "text/html"); return ESP_OK; }
    if (uri == "/login") { if (_isSessionValid(request)) { httpd_resp_set_hdr(request, "Location", "/"); http_send(request, "text/plain", "Redirecting to dashboard", 302); } else _handleStatic(request, "/webfs/login.html", "text/html"); return ESP_OK; }
    if (uri == "/style.css") { _handleStatic(request, "/webfs/style.css", "text/css"); return ESP_OK; }
    if (uri == "/ui.js") { _handleStatic(request, "/webfs/ui.js", "application/javascript"); return ESP_OK; }
    if (uri == "/favicon.svg") { _handleStatic(request, "/webfs/favicon.svg", "image/svg+xml"); return ESP_OK; }
    if (uri == "/calibration") { if (_requirePageAuth(request, "/calibration")) { httpd_resp_set_hdr(request, "Location", "/settings#calibration"); http_send(request, "text/plain", "Moved Permanently", 301); } return ESP_OK; }
    http_send(request, "text/plain", "Not found", 404); return ESP_OK;
}

esp_err_t DashboardServer::_handlePost(httpd_req_t* request) {
    std::string uri(request->uri); const size_t queryStart = uri.find('?'); if (queryStart != std::string::npos) uri.resize(queryStart); std::string body; cJSON* doc = nullptr;
    if (uri == "/api/auth/login") {
        const std::string ip = http_client_ip(request);
        if (_isRateLimited(ip)) { _sendAuthFailure(request, 429, "rate_limited"); return ESP_OK; }
        if (!http_constant_time_equal(http_request_header(request, "X-CSRF-Token"), _preAuthCsrfToken)) { _sendAuthFailure(request, 403, "csrf_failed"); return ESP_OK; }
        doc = parseBody(request, body); if (!doc) { _recordAuthFailure(ip); _sendAuthFailure(request, 400, "invalid_request"); return ESP_OK; }
        const std::string password = stringValue(doc, "password"); const std::string confirm = stringValue(doc, "confirm"); cJSON_Delete(doc);
        if (password.empty() || password.size() > 128) { _recordAuthFailure(ip); _sendAuthFailure(request, 401, "invalid_credentials"); return ESP_OK; }
        if (_cfg->adminPasswordHash.empty()) { if (password.size() < 8 || !http_constant_time_equal(password, confirm)) { _recordAuthFailure(ip); _sendAuthFailure(request, 401, "invalid_credentials"); return ESP_OK; } _cfg->adminPasswordHash = http_create_password_hash(password); if (_cfg->adminPasswordHash.empty() || !_cfgMgr->save(*_cfg)) { _cfg->adminPasswordHash.clear(); _sendAuthFailure(request, 500, "password_persist_failed"); return ESP_OK; } }
        else if (!http_verify_password_hash(password, _cfg->adminPasswordHash)) { _recordAuthFailure(ip); _sendAuthFailure(request, 401, "invalid_credentials"); return ESP_OK; }
        _establishSession(); const std::string cookie = "session=" + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict"; httpd_resp_set_hdr(request, "Set-Cookie", cookie.c_str()); _sendJson(request, "{\"ok\":true,\"configured\":true}"); return ESP_OK;
    }
    if (uri == "/api/auth/logout") { if (!_requireApiAuth(request, true)) return ESP_OK; _clearSession(); httpd_resp_set_hdr(request, "Set-Cookie", "session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict"); _sendJson(request, "{\"ok\":true}"); return ESP_OK; }
    if (uri == "/api/reboot") { if (!_requireApiAuth(request, true)) return ESP_OK; _sendJson(request, "{\"ok\":true}"); http_restart_after_response(); return ESP_OK; }
    if (uri == "/api/cloud/history-backfill") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        if (!_logFsOk) { _sendJson(request, "{\"ok\":false,\"error\":\"logfs_unavailable\"}", 503); return ESP_OK; }
        if (!_cloudSync || !_cloudSync->isConfigured()) { _sendJson(request, "{\"ok\":false,\"error\":\"cloud_not_configured\"}", 409); return ESP_OK; }
        if (!_cloudSync->requestHistoryBackfill()) { _sendJson(request, "{\"ok\":false,\"error\":\"history_backfill_queue_failed\"}", 503); return ESP_OK; }
        _sendJson(request, "{\"ok\":true,\"state\":\"queued\"}", 202); return ESP_OK;
    }
    if (uri == "/api/logs/clear") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        // Destructive and irreversible, so the session alone is not enough: re-prove the
        // password, and count failures towards the same lockout the login endpoint uses so
        // this cannot become a brute-force oracle for an unattended tab.
        const std::string ip = http_client_ip(request);
        if (_isRateLimited(ip)) { _sendJson(request, "{\"ok\":false,\"error\":\"rate_limited\"}", 429); return ESP_OK; }
        doc = parseBody(request, body);
        const std::string password = doc ? stringValue(doc, "password") : std::string();
        if (doc) cJSON_Delete(doc);
        if (password.empty() || password.size() > 128 || _cfg->adminPasswordHash.empty() ||
            !http_verify_password_hash(password, _cfg->adminPasswordHash)) {
            _recordAuthFailure(ip);
            _sendJson(request, "{\"ok\":false,\"error\":\"invalid_credentials\"}", 401); return ESP_OK;
        }
        // Only records the request; the wipe itself runs on the next boot, where nothing is
        // writing to the data being erased. See lib/HistoryMaintenance.
        if (!history_request_clear()) {
            _sendJson(request, "{\"ok\":false,\"error\":\"request_not_stored\"}", 500); return ESP_OK;
        }
        LOG_INFO(TAG, "drink history clear requested by %s", ip.c_str());
        _sendJson(request, "{\"ok\":true,\"state\":\"restarting\"}");
        http_restart_after_response();
        return ESP_OK;
    }
    if (uri == "/api/ota/check" || uri == "/api/ota/update") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        if (!_ota) { _sendJson(request, "{\"ok\":false,\"error\":\"ota_unavailable\"}", 503); return ESP_OK; }
        const bool isUpdate = uri == "/api/ota/update";
        switch (isUpdate ? _ota->requestUpdate() : _ota->requestCheck()) {
            case OtaRequestResult::ACCEPTED:
                _sendJson(request, isUpdate ? "{\"ok\":true,\"state\":\"queued\"}" : "{\"ok\":true,\"state\":\"checking\"}", isUpdate ? 202 : 200);
                break;
            case OtaRequestResult::BUSY:
                _sendJson(request, "{\"ok\":false,\"error\":\"ota_busy\"}", 409); break;
            case OtaRequestResult::NO_UPDATE_AVAILABLE:
                _sendJson(request, "{\"ok\":false,\"error\":\"no_update_available\"}", 409); break;
            case OtaRequestResult::INSUFFICIENT_MEMORY:
                _sendJson(request, "{\"ok\":false,\"error\":\"insufficient_memory\"}", 503); break;
            case OtaRequestResult::OFFLINE:
                _sendJson(request, "{\"ok\":false,\"error\":\"offline\"}", 503); break;
        }
        return ESP_OK;
    }
    if (uri == "/api/tare") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        ControlResult result;
        if (!_runCommand(ControlCommandType::TARE, 0, 0, false, &result, pdMS_TO_TICKS(4500))) { _sendJson(request, "{\"ok\":false,\"error\":\"tare_failed_or_timed_out\"}", result.status == ControlResultStatus::BUSY ? 409 : 504); return ESP_OK; }
        _cfg->tareOffset = result.tareOffset; _cfg->calibrationFactor = result.calibrationFactor;
        if (!_cfgMgr->saveCalibration(result.calibrationFactor, result.tareOffset)) { _sendJson(request, "{\"ok\":false,\"error\":\"tare_applied_but_persist_failed\"}", 500); return ESP_OK; }
        _sendJson(request, "{\"ok\":true}"); return ESP_OK;
    }
    if (uri == "/api/calibrate") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        doc = parseBody(request, body); if (!doc || !number(doc, "known_weight_g")) { if (doc) cJSON_Delete(doc); _sendJson(request, "{\"ok\":false,\"error\":\"known_weight_g required\"}", 400); return ESP_OK; }
        const float known = static_cast<float>(cJSON_GetObjectItem(doc, "known_weight_g")->valuedouble); cJSON_Delete(doc); ControlResult result;
        if (known <= 0 || !_runCommand(ControlCommandType::CALIBRATE, 0, known, false, &result, pdMS_TO_TICKS(750))) { _sendJson(request, "{\"ok\":false,\"error\":\"calibration_failed\"}", 500); return ESP_OK; }
        _cfg->calibrationFactor = result.calibrationFactor; _cfg->tareOffset = result.tareOffset; if (!_cfgMgr->saveCalibration(result.calibrationFactor, result.tareOffset)) { _sendJson(request, "{\"ok\":false,\"error\":\"calibration_applied_but_persist_failed\"}", 500); return ESP_OK; }
        cJSON* response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "ok", true); cJSON_AddNumberToObject(response, "calibration_factor", result.calibrationFactor); cJSON_AddNumberToObject(response, "current_weight_g", result.weightGrams); _sendJson(request, jsonString(response)); cJSON_Delete(response); return ESP_OK;
    }
    if (uri == "/api/config") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        doc = parseBody(request, body); if (!doc) { _sendJson(request, "{\"ok\":false,\"error\":\"Invalid JSON\"}", 400); return ESP_OK; }
        std::string normalizedCloudOrigin;
        if (has(doc, "cloudBaseUrl") &&
            (!cJSON_IsString(cJSON_GetObjectItem(doc, "cloudBaseUrl")) ||
             !normalizeCloudOrigin(stringValue(doc, "cloudBaseUrl"), normalizedCloudOrigin))) {
            cJSON_Delete(doc); _sendJson(request, "{\"ok\":false,\"error\":\"cloudBaseUrl must be an HTTPS origin\"}", 400); return ESP_OK;
        }
        const bool cloudWillBeEnabled = boolean(doc, "cloudEnabled")
            ? cJSON_IsTrue(cJSON_GetObjectItem(doc, "cloudEnabled")) : _cfg->cloudEnabled;
        const std::string cloudOriginAfterUpdate = has(doc, "cloudBaseUrl")
            ? normalizedCloudOrigin : _cfg->cloudBaseUrl;
        if (cloudWillBeEnabled && cloudOriginAfterUpdate.empty()) {
            cJSON_Delete(doc); _sendJson(request, "{\"ok\":false,\"error\":\"cloudBaseUrl is required when cloud sync is enabled\"}", 400); return ESP_OK;
        }
        const int timezoneAfterUpdate = number(doc, "timezoneOffsetSec")
            ? static_cast<int>(std::clamp(cJSON_GetObjectItem(doc, "timezoneOffsetSec")->valuedouble,
                                          -43200.0, 50400.0))
            : _cfg->timezoneOffsetSec;
        if (cloudWillBeEnabled && timezoneAfterUpdate + _cfg->daylightOffsetSec != 8 * 3600) {
            cJSON_Delete(doc); _sendJson(request, "{\"ok\":false,\"error\":\"cloud sync requires UTC+8 timezone\"}", 400); return ESP_OK;
        }
        const bool cloudConnectionWillChange =
            (boolean(doc, "cloudEnabled") && cloudWillBeEnabled != _cfg->cloudEnabled) ||
            (has(doc, "cloudBaseUrl") && cloudOriginAfterUpdate != _cfg->cloudBaseUrl);
        if (cloudConnectionWillChange && _cloudSync && _cloudSync->historyBackfillRunning()) {
            cJSON_Delete(doc); _sendJson(request, "{\"ok\":false,\"error\":\"history_backfill_in_progress\"}", 409); return ESP_OK;
        }
        const RuntimeSnapshot live = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
        if (_runtime && _runtime->isControlRunning()) {
            _cfg->reminderEnabled = live.reminderEnabled;
            _cfg->reminderIntervalMin = live.reminderIntervalMin;
            _cfg->dailyGoalMl = live.dailyGoalMl;
            _cfg->reminderPausedUntilDate = live.reminderPausedUntilDate;
        }
        bool reboot = false; bool applied = true;
        auto applyNumber = [&](const char* key, auto& target, double min, double max, bool needsReboot) { if (number(doc, key)) { target = static_cast<std::decay_t<decltype(target)>>(std::clamp(cJSON_GetObjectItem(doc, key)->valuedouble, min, max)); reboot |= needsReboot; } };
        if (boolean(doc, "reminderEnabled")) { _cfg->reminderEnabled = cJSON_IsTrue(cJSON_GetObjectItem(doc, "reminderEnabled")); applied &= _runCommand(ControlCommandType::SET_REMINDER_ENABLED, 0, 0, _cfg->reminderEnabled); }
        applyNumber("dailyGoalMl", _cfg->dailyGoalMl, 100, 9999, false); if (number(doc, "dailyGoalMl")) applied &= _runCommand(ControlCommandType::SET_DAILY_GOAL_ML, _cfg->dailyGoalMl);
        if (number(doc, "reminderIntervalMin")) { _cfg->reminderIntervalMin = static_cast<uint32_t>(std::clamp(cJSON_GetObjectItem(doc, "reminderIntervalMin")->valuedouble, 1.0, 1440.0)); applied &= _runCommand(ControlCommandType::SET_REMINDER_INTERVAL_MIN, _cfg->reminderIntervalMin); }
        if (number(doc, "reminderAlertTimeoutSec")) { _cfg->reminderAlertTimeoutSec = static_cast<uint32_t>(std::clamp(cJSON_GetObjectItem(doc, "reminderAlertTimeoutSec")->valuedouble, 5.0, 3600.0)); applied &= _runCommand(ControlCommandType::SET_REMINDER_ALERT_TIMEOUT_SEC, _cfg->reminderAlertTimeoutSec); }
        if (boolean(doc, "buzzerEnabled")) { _cfg->buzzerEnabled = cJSON_IsTrue(cJSON_GetObjectItem(doc, "buzzerEnabled")); applied &= _runCommand(ControlCommandType::SET_BUZZER_ENABLED, 0, 0, _cfg->buzzerEnabled); }
        if (number(doc, "buzzerFrequencyHz")) { _cfg->buzzerFrequencyHz = static_cast<uint32_t>(std::clamp(cJSON_GetObjectItem(doc, "buzzerFrequencyHz")->valuedouble, 500.0, 5000.0)); applied &= _runCommand(ControlCommandType::SET_BUZZER_FREQUENCY_HZ, _cfg->buzzerFrequencyHz); }
        if (number(doc, "buzzerDurationMs")) { _cfg->buzzerDurationMs = static_cast<uint32_t>(std::clamp(cJSON_GetObjectItem(doc, "buzzerDurationMs")->valuedouble, 50.0, 2000.0)); applied &= _runCommand(ControlCommandType::SET_BUZZER_DURATION_MS, _cfg->buzzerDurationMs); }
        if (number(doc, "buzzerVolumePercent")) { _cfg->buzzerVolumePercent = static_cast<uint8_t>(std::clamp(cJSON_GetObjectItem(doc, "buzzerVolumePercent")->valuedouble, 0.0, 100.0)); applied &= _runCommand(ControlCommandType::SET_BUZZER_VOLUME_PERCENT, _cfg->buzzerVolumePercent); }
        if (has(doc, "wifiSsid") && cJSON_IsString(cJSON_GetObjectItem(doc, "wifiSsid"))) { _cfg->wifiSsid = stringValue(doc, "wifiSsid"); reboot = true; }
        if (has(doc, "wifiPassword") && cJSON_IsString(cJSON_GetObjectItem(doc, "wifiPassword")) && stringValue(doc, "wifiPassword") != "****") { _cfg->wifiPassword = stringValue(doc, "wifiPassword"); reboot = true; }
        if (has(doc, "discordWebhookUrl") && cJSON_IsString(cJSON_GetObjectItem(doc, "discordWebhookUrl")) && stringValue(doc, "discordWebhookUrl").find("****") == std::string::npos) _cfg->discordWebhookUrl = stringValue(doc, "discordWebhookUrl");
        if (boolean(doc, "cloudEnabled")) { _cfg->cloudEnabled = cJSON_IsTrue(cJSON_GetObjectItem(doc, "cloudEnabled")); reboot = true; }
        if (has(doc, "cloudBaseUrl")) { _cfg->cloudBaseUrl = normalizedCloudOrigin; reboot = true; }
        if (boolean(doc, "ntpEnabled")) { _cfg->ntpEnabled = cJSON_IsTrue(cJSON_GetObjectItem(doc, "ntpEnabled")); reboot = true; }
        if (has(doc, "ntpServer1")) { _cfg->ntpServer1 = stringValue(doc, "ntpServer1"); reboot = true; } if (has(doc, "ntpServer2")) { _cfg->ntpServer2 = stringValue(doc, "ntpServer2"); reboot = true; }
        applyNumber("timezoneOffsetSec", _cfg->timezoneOffsetSec, -43200, 50400, true); applyNumber("cupPresentThresholdGram", _cfg->cupPresentThresholdGram, 10, 500, true); applyNumber("stableToleranceGram", _cfg->stableToleranceGram, .5, 20, true); applyNumber("stableDurationMs", _cfg->stableDurationMs, 500, 10000, true); applyNumber("minDrinkDeltaMl", _cfg->minDrinkDeltaMl, 5, 100, true); applyNumber("maxDrinkDeltaMl", _cfg->maxDrinkDeltaMl, 50, 1000, true);
        if (boolean(doc, "mqttEnabled")) { _cfg->mqttEnabled = cJSON_IsTrue(cJSON_GetObjectItem(doc, "mqttEnabled")); reboot = true; } if (has(doc, "mqttBrokerHost")) { _cfg->mqttBrokerHost = stringValue(doc, "mqttBrokerHost"); reboot = true; } if (number(doc, "mqttBrokerPort")) { _cfg->mqttBrokerPort = static_cast<uint16_t>(std::clamp(cJSON_GetObjectItem(doc, "mqttBrokerPort")->valuedouble, 1.0, 65535.0)); reboot = true; } if (has(doc, "mqttUsername")) { _cfg->mqttUsername = stringValue(doc, "mqttUsername"); reboot = true; } if (has(doc, "mqttPassword") && stringValue(doc, "mqttPassword") != "****") { _cfg->mqttPassword = stringValue(doc, "mqttPassword"); reboot = true; } if (has(doc, "mqttClientId")) { _cfg->mqttClientId = stringValue(doc, "mqttClientId"); if (_cfg->mqttClientId.empty()) _cfg->mqttClientId = DEFAULT_MQTT_CLIENT_ID; reboot = true; } if (number(doc, "mqttHeartbeatSec")) { _cfg->mqttHeartbeatSec = static_cast<uint16_t>(std::clamp(cJSON_GetObjectItem(doc, "mqttHeartbeatSec")->valuedouble, 5.0, 3600.0)); reboot = true; }
        if (!_cfgMgr->save(*_cfg)) { cJSON_Delete(doc); _sendJson(request, "{\"ok\":false,\"error\":\"config_applied_but_persist_failed\"}", 500); return ESP_OK; }
        if (_discord) _discord->configure(*_cfg); if (_cloudSync) _cloudSync->configure(*_cfg); if (!applied) reboot = true;
        cJSON_Delete(doc); cJSON* response = cJSON_CreateObject(); cJSON_AddBoolToObject(response, "ok", true); cJSON_AddBoolToObject(response, "reboot_required", reboot); cJSON_AddBoolToObject(response, "control_applied", applied); _sendJson(request, jsonString(response)); cJSON_Delete(response); return ESP_OK;
    }
    http_send(request, "text/plain", "Not found", 404); return ESP_OK;
}

bool DashboardServer::_isSessionValid(httpd_req_t* request) {
    if (_sessionToken.empty() || !http_constant_time_equal(http_cookie_value(http_request_header(request, "Cookie"), "session"), _sessionToken)) return false;
    const uint32_t now = hal_millis(); if (now - _sessionStartMs > SESSION_ABSOLUTE_TIMEOUT_MS || now - _lastActivityMs > SESSION_IDLE_TIMEOUT_MS) { _clearSession(); return false; }
    _lastActivityMs = now; return true;
}
bool DashboardServer::_hasValidCsrf(httpd_req_t* request) const { return http_constant_time_equal(http_request_header(request, "X-CSRF-Token"), _sessionCsrfToken); }
void DashboardServer::_clearSession() { _sessionToken.clear(); _sessionCsrfToken.clear(); _sessionStartMs = 0; _lastActivityMs = 0; }
void DashboardServer::_establishSession() { _sessionToken = http_random_token(); _sessionCsrfToken = http_random_token(); _sessionStartMs = hal_millis(); _lastActivityMs = _sessionStartMs; }
bool DashboardServer::_requirePageAuth(httpd_req_t* request, const char* nextPath) { if (_isSessionValid(request)) return true; char location[64]; std::snprintf(location, sizeof(location), "/login?next=%s", nextPath); httpd_resp_set_hdr(request, "Location", location); http_send(request, "text/plain", "Redirecting to login", 302); return false; }
bool DashboardServer::_requireApiAuth(httpd_req_t* request, bool requireCsrf) { if (!_isSessionValid(request)) { _sendJson(request, "{\"ok\":false,\"error\":\"authentication_required\"}", 401); return false; } if (requireCsrf && !_hasValidCsrf(request)) { _sendJson(request, "{\"ok\":false,\"error\":\"csrf_failed\"}", 403); return false; } return true; }
bool DashboardServer::_isRateLimited(const std::string& ip) { const uint32_t now = hal_millis(); for (auto& bucket : _authFailures) if (bucket.ip == ip) { uint8_t kept = 0; for (uint8_t i = 0; i < bucket.count; ++i) if (now - bucket.timestamps[i] <= AUTH_FAILURE_WINDOW_MS) bucket.timestamps[kept++] = bucket.timestamps[i]; bucket.count = kept; bucket.lastSeen = now; return bucket.count >= AUTH_FAILURE_LIMIT; } return false; }
void DashboardServer::_recordAuthFailure(const std::string& ip) { const uint32_t now = hal_millis(); AuthFailureBucket* bucket = nullptr; for (auto& value : _authFailures) if (value.ip == ip) { bucket = &value; break; } if (!bucket) for (auto& value : _authFailures) if (value.ip.empty()) { bucket = &value; break; } if (!bucket) { bucket = &_authFailures[0]; for (auto& value : _authFailures) if (value.lastSeen < bucket->lastSeen) bucket = &value; bucket->ip.clear(); bucket->count = 0; } bucket->ip = ip; uint8_t kept = 0; for (uint8_t i = 0; i < bucket->count; ++i) if (now - bucket->timestamps[i] <= AUTH_FAILURE_WINDOW_MS) bucket->timestamps[kept++] = bucket->timestamps[i]; bucket->count = kept; if (bucket->count < AUTH_FAILURE_LIMIT) bucket->timestamps[bucket->count++] = now; bucket->lastSeen = now; }
void DashboardServer::_sendAuthFailure(httpd_req_t* request, int status, const char* error) { _sendJson(request, std::string("{\"ok\":false,\"error\":\"") + error + "\"}", status); }

bool DashboardServer::_runCommand(ControlCommandType type, uint32_t uintValue, float floatValue,
                                  bool boolValue, ControlResult* result, TickType_t timeoutTicks) {
    if (!_runtime || !_runtime->isControlRunning()) return false;
    ControlCommand command; command.type = type; command.uintValue = uintValue; command.floatValue = floatValue; command.boolValue = boolValue;
    ControlResult local; const bool ok = _runtime->request(command, local, timeoutTicks); if (result) *result = local; return ok;
}

const char* DashboardServer::_cupStateStr(CupState state) { switch (state) { case CupState::NO_CUP: return "no_cup"; case CupState::CUP_UNSTABLE: return "unstable"; case CupState::CUP_STABLE: return "stable"; case CupState::POSSIBLE_DRINK: return "possible_drink"; case CupState::DRINK_CONFIRMED: return "drink_confirmed"; case CupState::REFILL_DETECTED: return "refill_detected"; default: return "unknown"; } }
std::string DashboardServer::_maskWebhookUrl(const std::string& url) { if (url.empty()) return {}; const size_t last = url.rfind('/'); return last == std::string::npos ? "****" : url.substr(0, last + 1) + "****"; }
