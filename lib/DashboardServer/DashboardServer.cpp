#include "DashboardServer.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <cstdlib>
#include <cstring>
#include "DiscordNotifier.h"
#include "EventLogger.h"

namespace {

constexpr size_t AUTH_TOKEN_BYTES = 16;
constexpr size_t PASSWORD_SALT_BYTES = 16;
constexpr size_t PASSWORD_HASH_BYTES = 32;
constexpr uint32_t PASSWORD_ITERATIONS = 12000;

String randomHexToken(size_t byteCount) {
    uint8_t bytes[AUTH_TOKEN_BYTES];
    if (byteCount > sizeof(bytes)) byteCount = sizeof(bytes);
    esp_fill_random(bytes, byteCount);
    static const char* hex = "0123456789abcdef";
    String token;
    token.reserve(byteCount * 2);
    for (size_t i = 0; i < byteCount; ++i) {
        token += hex[bytes[i] >> 4];
        token += hex[bytes[i] & 0x0f];
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    return token;
}

bool constantTimeEqual(const String& left, const String& right) {
    const size_t maxLength = left.length() > right.length() ? left.length() : right.length();
    size_t difference = left.length() ^ right.length();
    for (size_t i = 0; i < maxLength; ++i) {
        const uint8_t a = i < left.length() ? (uint8_t)left[i] : 0;
        const uint8_t b = i < right.length() ? (uint8_t)right[i] : 0;
        difference |= (uint8_t)(a ^ b);
    }
    return difference == 0;
}

bool constantTimeEqualBytes(const uint8_t* left, const uint8_t* right, size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0;
}

void hexEncode(const uint8_t* input, size_t length, String& output) {
    static const char* hex = "0123456789abcdef";
    output.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        output += hex[input[i] >> 4];
        output += hex[input[i] & 0x0f];
    }
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool hexDecode(const String& input, uint8_t* output, size_t outputLength) {
    if (input.length() != outputLength * 2) return false;
    for (size_t i = 0; i < outputLength; ++i) {
        const int high = hexValue(input[i * 2]);
        const int low = hexValue(input[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool hmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* input, size_t inputLength,
                uint8_t output[PASSWORD_HASH_BYTES]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != nullptr &&
           mbedtls_md_hmac(info, key, keyLength, input, inputLength, output) == 0;
}

bool pbkdf2Sha256(const String& password, const uint8_t* salt, size_t saltLength,
                  uint32_t iterations, uint8_t output[PASSWORD_HASH_BYTES]) {
    if (password.isEmpty() || saltLength > 60 || iterations == 0) return false;

    uint8_t block[64] = {};
    uint8_t u[PASSWORD_HASH_BYTES] = {};
    uint8_t nextU[PASSWORD_HASH_BYTES] = {};
    uint8_t t[PASSWORD_HASH_BYTES] = {};
    memcpy(block, salt, saltLength);
    block[saltLength]     = 0;
    block[saltLength + 1] = 0;
    block[saltLength + 2] = 0;
    block[saltLength + 3] = 1;

    const uint8_t* passwordBytes = reinterpret_cast<const uint8_t*>(password.c_str());
    const size_t passwordLength = password.length();
    if (!hmacSha256(passwordBytes, passwordLength, block, saltLength + 4, u)) return false;
    memcpy(t, u, sizeof(t));

    for (uint32_t round = 1; round < iterations; ++round) {
        if (!hmacSha256(passwordBytes, passwordLength, u, sizeof(u), nextU)) return false;
        for (size_t i = 0; i < sizeof(t); ++i) t[i] ^= nextU[i];
        memcpy(u, nextU, sizeof(u));
    }

    memcpy(output, t, sizeof(t));
    mbedtls_platform_zeroize(block, sizeof(block));
    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(nextU, sizeof(nextU));
    mbedtls_platform_zeroize(t, sizeof(t));
    return true;
}

String createPasswordHash(const String& password) {
    uint8_t salt[PASSWORD_SALT_BYTES] = {};
    uint8_t derived[PASSWORD_HASH_BYTES] = {};
    esp_fill_random(salt, sizeof(salt));
    if (!pbkdf2Sha256(password, salt, sizeof(salt), PASSWORD_ITERATIONS, derived)) return String();

    String saltHex;
    String derivedHex;
    hexEncode(salt, sizeof(salt), saltHex);
    hexEncode(derived, sizeof(derived), derivedHex);
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(derived, sizeof(derived));
    return String("pbkdf2-sha256$") + String(PASSWORD_ITERATIONS) + "$" + saltHex + "$" + derivedHex;
}

bool verifyPasswordHash(const String& password, const String& stored) {
    const int first = stored.indexOf('$');
    const int second = first < 0 ? -1 : stored.indexOf('$', first + 1);
    const int third = second < 0 ? -1 : stored.indexOf('$', second + 1);
    if (first <= 0 || second <= first || third <= second) return false;

    const String algorithm = stored.substring(0, first);
    if (!constantTimeEqual(algorithm, "pbkdf2-sha256")) return false;
    const String iterationText = stored.substring(first + 1, second);
    char* end = nullptr;
    const unsigned long parsedIterations = strtoul(iterationText.c_str(), &end, 10);
    if (end == iterationText.c_str() || *end != '\0' ||
        parsedIterations == 0 || parsedIterations > 1000000UL) return false;

    uint8_t salt[PASSWORD_SALT_BYTES] = {};
    uint8_t expected[PASSWORD_HASH_BYTES] = {};
    uint8_t actual[PASSWORD_HASH_BYTES] = {};
    const bool decoded = hexDecode(stored.substring(second + 1, third), salt, sizeof(salt)) &&
                         hexDecode(stored.substring(third + 1), expected, sizeof(expected));
    const bool derived = decoded && pbkdf2Sha256(password, salt, sizeof(salt),
                                                 (uint32_t)parsedIterations, actual);
    const bool matches = derived && constantTimeEqualBytes(actual, expected, sizeof(actual));
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(actual, sizeof(actual));
    return matches;
}

String cookieValue(const String& header, const char* name) {
    const String prefix = String(name) + "=";
    int start = 0;
    while (start < (int)header.length()) {
        while (start < (int)header.length() && (header[start] == ' ' || header[start] == ';')) ++start;
        const int separator = header.indexOf(';', start);
        const int end = separator < 0 ? header.length() : separator;
        if (header.substring(start, end).startsWith(prefix))
            return header.substring(start + prefix.length(), end);
        if (separator < 0) break;
        start = separator + 1;
    }
    return String();
}

}  // namespace

// ── Helpers ────────────────────────────────────────────────────────────────

const char* DashboardServer::_cupStateStr(CupState s) {
    switch (s) {
        case CupState::NO_CUP:          return "no_cup";
        case CupState::CUP_UNSTABLE:    return "unstable";
        case CupState::CUP_STABLE:      return "stable";
        case CupState::POSSIBLE_DRINK:  return "possible_drink";
        case CupState::DRINK_CONFIRMED: return "drink_confirmed";
        case CupState::REFILL_DETECTED: return "refill_detected";
        default: return "unknown";
    }
}

String DashboardServer::_maskWebhookUrl(const String& url) {
    if (url.isEmpty()) return "";
    int last = url.lastIndexOf('/');
    if (last > 0) return url.substring(0, last + 1) + "****";
    return "****";
}

void DashboardServer::_serveFile(const char* path, const char* contentType) {
    if (!_state->fsOk) {
        _server->send(503, "text/plain", "Web assets unavailable. Run: pio run -t uploadfs");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        _server->send(404, "text/plain",
                      String("Not found: ") + path + "\nRun: pio run -t uploadfs");
        return;
    }
    _server->streamFile(f, contentType);
    f.close();
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void DashboardServer::begin(ScaleManager& scale, ConfigManager& cfgMgr,
                            AppState& state, AppConfig& cfg,
                            BuzzerController& buzzer, ReminderManager& reminder,
                            fs::LittleFSFS& logFs, RuntimeCoordinator& runtime,
                            EventLogger& eventLogger, DiscordNotifier& discord) {
    if (_server != nullptr) {
        Serial.println("[Dashboard] Already initialized");
        return;
    }
    _scale    = &scale;
    _cfgMgr   = &cfgMgr;
    _state    = &state;
    _cfg      = &cfg;
    _buzzer   = &buzzer;
    _reminder = &reminder;
    _logFs    = &logFs;
    _runtime = &runtime;
    _eventLogger = &eventLogger;
    _discord = &discord;
    _preAuthCsrfToken = randomHexToken(AUTH_TOKEN_BYTES);
    _server   = new WebServer(80);

    const char* headerKeys[] = {"Cookie", "X-CSRF-Token"};
    _server->collectHeaders(headerKeys, 2);

    _server->on("/",              HTTP_GET,  [this]{ _handleRoot();                });
    _server->on("/settings",      HTTP_GET,  [this]{ _handleSettings();            });
    _server->on("/history",       HTTP_GET,  [this]{ _handleHistory();             });
    _server->on("/login",         HTTP_GET,  [this]{ _handleLogin();                });
    _server->on("/style.css",     HTTP_GET,  [this]{ _handleStyleCss();            });
    _server->on("/ui.js",         HTTP_GET,  [this]{ _handleUiJs();                 });
    _server->on("/favicon.svg",   HTTP_GET,  [this]{ _handleFavicon();              });
    _server->on("/calibration",   HTTP_GET,  [this]{ _handleCalibrationRedirect(); });
    _server->on("/api/auth/csrf", HTTP_GET,  [this]{ _handleAuthCsrf();             });
    _server->on("/api/auth/login",HTTP_POST, [this]{ _handleAuthLogin();            });
    _server->on("/api/auth/logout",HTTP_POST,[this]{ _handleAuthLogout();           });
    _server->on("/api/weight",    HTTP_GET,  [this]{ _handleWeight();              });
    _server->on("/api/status",    HTTP_GET,  [this]{ _handleStatus();              });
    _server->on("/api/config",    HTTP_GET,  [this]{ _handleGetConfig();           });
    _server->on("/api/config",    HTTP_POST, [this]{ _handlePostConfig();          });
    _server->on("/api/tare",      HTTP_POST, [this]{ _handleTare();                });
    _server->on("/api/calibrate", HTTP_POST, [this]{ _handleCalibrate();           });
    _server->on("/api/wifi/scan", HTTP_GET,  [this]{ _handleWifiScan();            });
    _server->on("/api/reboot",    HTTP_POST, [this]{ _handleReboot();              });
    _server->on("/api/logs",      HTTP_GET,  [this]{ _handleLogs();                });

    _server->begin();
    Serial.println("[Dashboard] HTTP server started on port 80");
}

bool DashboardServer::_runCommand(ControlCommandType type, uint32_t uintValue,
                                  float floatValue, bool boolValue,
                                  ControlResult* result, TickType_t timeoutTicks) {
    if (!_runtime || !_runtime->isControlRunning()) return false;
    ControlCommand command;
    command.type = type;
    command.uintValue = uintValue;
    command.floatValue = floatValue;
    command.boolValue = boolValue;
    ControlResult localResult;
    const bool ok = _runtime->request(command, localResult, timeoutTicks);
    if (result) *result = localResult;
    return ok;
}

void DashboardServer::loop() {
    if (_server) _server->handleClient();
}

// ── Page handlers ──────────────────────────────────────────────────────────

bool DashboardServer::_isSessionValid() {
    if (_sessionToken.isEmpty() || !_server->hasHeader("Cookie")) return false;
    const String cookie = cookieValue(_server->header("Cookie"), "session");
    if (!constantTimeEqual(cookie, _sessionToken)) return false;

    const uint32_t now = millis();
    if (now - _sessionStartMs > SESSION_ABSOLUTE_TIMEOUT_MS ||
        now - _lastActivityMs > SESSION_IDLE_TIMEOUT_MS) {
        _clearSession();
        return false;
    }
    _lastActivityMs = now;
    return true;
}

bool DashboardServer::_hasValidCsrf() const {
    if (!_server->hasHeader("X-CSRF-Token")) return false;
    return constantTimeEqual(_server->header("X-CSRF-Token"), _sessionCsrfToken);
}

void DashboardServer::_clearSession() {
    _sessionToken = String();
    _sessionCsrfToken = String();
    _sessionStartMs = 0;
    _lastActivityMs = 0;
}

void DashboardServer::_establishSession() {
    _sessionToken = randomHexToken(AUTH_TOKEN_BYTES);
    _sessionCsrfToken = randomHexToken(AUTH_TOKEN_BYTES);
    _sessionStartMs = millis();
    _lastActivityMs = _sessionStartMs;
}

bool DashboardServer::_requirePageAuth(const char* nextPath) {
    if (_isSessionValid()) {
        _server->sendHeader("Cache-Control", "no-store");
        return true;
    }
    _server->sendHeader("Cache-Control", "no-store");
    _server->sendHeader("Location", String("/login?next=") + nextPath, true);
    _server->send(302, "text/plain", "Redirecting to login");
    return false;
}

bool DashboardServer::_requireApiAuth(bool requireCsrf) {
    if (!_isSessionValid()) {
        _server->sendHeader("Cache-Control", "no-store");
        _server->send(401, "application/json", "{\"ok\":false,\"error\":\"authentication_required\"}");
        return false;
    }
    _server->sendHeader("Cache-Control", "no-store");
    if (requireCsrf && !_hasValidCsrf()) {
        _server->send(403, "application/json", "{\"ok\":false,\"error\":\"csrf_failed\"}");
        return false;
    }
    return true;
}

bool DashboardServer::_isRateLimited(const String& ip) {
    const uint32_t now = millis();
    for (AuthFailureBucket& bucket : _authFailures) {
        if (bucket.ip != ip) continue;
        uint8_t kept = 0;
        for (uint8_t i = 0; i < bucket.count; ++i) {
            if (now - bucket.timestamps[i] <= AUTH_FAILURE_WINDOW_MS)
                bucket.timestamps[kept++] = bucket.timestamps[i];
        }
        bucket.count = kept;
        bucket.lastSeen = now;
        return bucket.count >= AUTH_FAILURE_LIMIT;
    }
    return false;
}

void DashboardServer::_recordAuthFailure(const String& ip) {
    const uint32_t now = millis();
    AuthFailureBucket* bucket = nullptr;
    for (AuthFailureBucket& candidate : _authFailures) {
        if (candidate.ip == ip) { bucket = &candidate; break; }
    }
    if (!bucket) {
        for (AuthFailureBucket& candidate : _authFailures) {
            if (candidate.ip.isEmpty()) { bucket = &candidate; break; }
        }
    }
    if (!bucket) {
        bucket = &_authFailures[0];
        for (AuthFailureBucket& candidate : _authFailures)
            if (candidate.lastSeen < bucket->lastSeen) bucket = &candidate;
        bucket->ip = String();
        bucket->count = 0;
    }
    bucket->ip = ip;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < bucket->count; ++i) {
        if (now - bucket->timestamps[i] <= AUTH_FAILURE_WINDOW_MS)
            bucket->timestamps[kept++] = bucket->timestamps[i];
    }
    bucket->count = kept;
    if (bucket->count < AUTH_FAILURE_LIMIT)
        bucket->timestamps[bucket->count++] = now;
    bucket->lastSeen = now;
}

void DashboardServer::_sendAuthFailure(int statusCode, const char* error) {
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(statusCode, "application/json",
                  String("{\"ok\":false,\"error\":\"") + error + "\"}");
}

void DashboardServer::_handleRoot() {
    if (_requirePageAuth("/")) _serveFile("/index.html", "text/html");
}

void DashboardServer::_handleSettings() {
    if (_requirePageAuth("/settings")) _serveFile("/settings.html", "text/html");
}

void DashboardServer::_handleHistory() {
    if (_requirePageAuth("/history")) _serveFile("/history.html", "text/html");
}

void DashboardServer::_handleLogin() {
    if (_isSessionValid()) {
        _server->sendHeader("Cache-Control", "no-store");
        _server->sendHeader("Location", "/", true);
        _server->send(302, "text/plain", "Redirecting to dashboard");
        return;
    }
    _server->sendHeader("Cache-Control", "no-store");
    _serveFile("/login.html", "text/html");
}

void DashboardServer::_handleStyleCss() { _serveFile("/style.css", "text/css"); }
void DashboardServer::_handleUiJs()     { _serveFile("/ui.js", "application/javascript"); }
void DashboardServer::_handleFavicon()  { _serveFile("/favicon.svg", "image/svg+xml"); }

void DashboardServer::_handleCalibrationRedirect() {
    if (!_requirePageAuth("/calibration")) return;
    _server->sendHeader("Location", "/settings#calibration", true);
    _server->send(301, "text/plain", "Moved Permanently");
}

// ── API handlers ───────────────────────────────────────────────────────────

void DashboardServer::_handleAuthCsrf() {
    const bool authenticated = _isSessionValid();
    JsonDocument doc;
    doc["ok"] = true;
    doc["configured"] = !_cfg->adminPasswordHash.isEmpty();
    doc["authenticated"] = authenticated;
    doc["csrf"] = authenticated ? _sessionCsrfToken : _preAuthCsrfToken;
    String json;
    serializeJson(doc, json);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleAuthLogin() {
    const String ip = _server->client().remoteIP().toString();
    if (_isRateLimited(ip)) {
        _sendAuthFailure(429, "rate_limited");
        return;
    }
    if (!_server->hasHeader("X-CSRF-Token") ||
        !constantTimeEqual(_server->header("X-CSRF-Token"), _preAuthCsrfToken)) {
        _sendAuthFailure(403, "csrf_failed");
        return;
    }
    if (!_server->hasArg("plain")) {
        _recordAuthFailure(ip);
        _sendAuthFailure(400, "invalid_request");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _recordAuthFailure(ip);
        _sendAuthFailure(400, "invalid_request");
        return;
    }

    const String password = doc["password"] | "";
    const String confirmation = doc["confirm"] | "";
    if (password.isEmpty() || password.length() > 128) {
        _recordAuthFailure(ip);
        _sendAuthFailure(401, "invalid_credentials");
        return;
    }

    const bool configured = !_cfg->adminPasswordHash.isEmpty();
    if (!configured) {
        if (password.length() < 8 || !constantTimeEqual(password, confirmation)) {
            _recordAuthFailure(ip);
            _sendAuthFailure(401, "invalid_credentials");
            return;
        }
        const String passwordHash = createPasswordHash(password);
        if (passwordHash.isEmpty()) {
            _sendAuthFailure(500, "password_setup_failed");
            return;
        }
        _cfg->adminPasswordHash = passwordHash;
        if (!_cfgMgr->save(*_cfg)) {
            _cfg->adminPasswordHash = String();
            _sendAuthFailure(500, "password_persist_failed");
            return;
        }
    } else if (!verifyPasswordHash(password, _cfg->adminPasswordHash)) {
        _recordAuthFailure(ip);
        _sendAuthFailure(401, "invalid_credentials");
        return;
    }

    _establishSession();
    _server->sendHeader("Set-Cookie",
                        String("session=") + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict",
                        true);
    JsonDocument response;
    response["ok"] = true;
    response["configured"] = true;
    String json;
    serializeJson(response, json);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleAuthLogout() {
    if (!_requireApiAuth(true)) return;
    _clearSession();
    _server->sendHeader("Set-Cookie", "session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict", false);
    _server->send(200, "application/json", "{\"ok\":true}");
}

void DashboardServer::_handleWeight() {
    if (!_requireApiAuth(false)) return;
    const RuntimeSnapshot runtime = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
    JsonDocument doc;
    doc["ok"]        = true;
    doc["weight_g"]  = _runtime && _runtime->isControlRunning()
        ? runtime.weightGrams : _scale->getWeightGrams();
    doc["cup_state"] = (int)(_runtime && _runtime->isControlRunning()
        ? runtime.cupState : _state->cupState);
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleStatus() {
    if (!_requireApiAuth(false)) return;
    const RuntimeSnapshot runtime = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
    const bool rtos = _runtime && _runtime->isControlRunning();
    const CupState cupState = rtos ? runtime.cupState : _state->cupState;
    JsonDocument doc;
    doc["ok"]                = true;
    doc["mode"]              = "normal";
    doc["wifi_connected"]    = rtos ? runtime.wifiConnected : _state->wifiConnected;
    doc["ip"]                = rtos ? runtime.ipAddress : _state->ipAddress.c_str();
    doc["ntp_synced"]        = rtos ? runtime.ntpSynced : _state->ntpSynced;
    doc["weight_g"]          = rtos ? runtime.weightGrams : _scale->getWeightGrams();
    doc["cup_state"]         = (int)cupState;
    doc["cup_state_name"]    = _cupStateStr(cupState);
    doc["today_total_ml"]    = rtos ? runtime.todayTotalMl : _state->todayTotalMl;
    doc["daily_goal_ml"]     = _cfg->dailyGoalMl;
    doc["drink_count_today"] = rtos ? runtime.drinkCountToday : _state->drinkCountToday;
    doc["last_drink_ml"]     = rtos ? runtime.lastDrinkMl : _state->lastDrinkMl;
    doc["next_reminder_sec"] = rtos ? runtime.nextReminderSec : _state->nextReminderSec;
    doc["webhook_configured"] = _state->webhookConfigured;
    doc["webhook_last_ok"]   = _state->webhookLastOk.load();
    doc["discord_worker_ready"] = _discord && _discord->isWorkerReady();
    doc["discord_queue_drops"] = _discord ? _discord->getDroppedCount() : 0;
    doc["mqtt_configured"]   = _state->mqttConfigured;
    doc["mqtt_connected"]    = _state->mqttConnected.load();
    doc["hw_hx711"]          = _state->hx711Ok;
    doc["hw_oled"]           = _state->oledOk;
    doc["hw_fs"]             = _state->fsOk;
    doc["hw_logfs"]          = _state->logFsOk;
    doc["rtos"]              = rtos;
    doc["rtos_healthy"]      = _runtime && _runtime->isControlHealthy();
    doc["rtos_sequence"]     = runtime.sequence;
    doc["rtos_command_drops"] = runtime.commandDrops;
    doc["rtos_result_drops"]  = runtime.resultDrops;
    doc["log_queue_drops"]    = _eventLogger ? _eventLogger->getDroppedCount() : 0;
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleGetConfig() {
    if (!_requireApiAuth(false)) return;
    JsonDocument doc;
    doc["ok"]                      = true;
    doc["wifiSsid"]                = _cfg->wifiSsid;
    doc["wifiPassword"]            = "****";
    doc["wifiPasswordSet"]         = !_cfg->wifiPassword.isEmpty();
    doc["discordWebhookUrl"]       = _maskWebhookUrl(_cfg->discordWebhookUrl);
    doc["reminderEnabled"]         = _cfg->reminderEnabled;
    doc["reminderIntervalMin"]     = _cfg->reminderIntervalMin;
    doc["reminderAlertTimeoutSec"] = _cfg->reminderAlertTimeoutSec;
    doc["dailyGoalMl"]             = _cfg->dailyGoalMl;
    doc["buzzerEnabled"]           = _cfg->buzzerEnabled;
    doc["buzzerFrequencyHz"]       = _cfg->buzzerFrequencyHz;
    doc["buzzerDurationMs"]        = _cfg->buzzerDurationMs;
    doc["buzzerVolumePercent"]     = _cfg->buzzerVolumePercent;
    doc["ntpEnabled"]              = _cfg->ntpEnabled;
    doc["ntpServer1"]              = _cfg->ntpServer1;
    doc["ntpServer2"]              = _cfg->ntpServer2;
    doc["timezoneOffsetSec"]       = _cfg->timezoneOffsetSec;
    doc["mqttEnabled"]             = _cfg->mqttEnabled;
    doc["mqttBrokerHost"]          = _cfg->mqttBrokerHost;
    doc["mqttBrokerPort"]          = _cfg->mqttBrokerPort;
    doc["mqttUsername"]            = _cfg->mqttUsername;
    doc["mqttPassword"]            = "****";
    doc["mqttPasswordSet"]         = !_cfg->mqttPassword.isEmpty();
    doc["mqttClientId"]            = _cfg->mqttClientId;
    doc["mqttHeartbeatSec"]        = _cfg->mqttHeartbeatSec;
    const RuntimeSnapshot runtime = _runtime ? _runtime->snapshot() : RuntimeSnapshot{};
    doc["calibrationFactor"]       = _runtime && _runtime->isControlRunning()
        ? runtime.calibrationFactor : _scale->getCalibrationFactor();
    doc["cupPresentThresholdGram"] = _cfg->cupPresentThresholdGram;
    doc["stableToleranceGram"]     = _cfg->stableToleranceGram;
    doc["stableDurationMs"]        = _cfg->stableDurationMs;
    doc["minDrinkDeltaMl"]         = _cfg->minDrinkDeltaMl;
    doc["maxDrinkDeltaMl"]         = _cfg->maxDrinkDeltaMl;
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handlePostConfig() {
    if (!_requireApiAuth(true)) return;
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }

    bool rebootRequired = false;
    bool controlCommandsOk = true;

    // Hydration — clamp to [100, 9999]
    if (!doc["dailyGoalMl"].isNull()) {
        _cfg->dailyGoalMl = (uint32_t)constrain((long)doc["dailyGoalMl"], 100L, 9999L);
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_DAILY_GOAL_ML,
                                             _cfg->dailyGoalMl);
    }

    // Reminder — apply immediately
    if (!doc["reminderEnabled"].isNull()) {
        _cfg->reminderEnabled = (bool)doc["reminderEnabled"];
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_REMINDER_ENABLED,
                                             0, 0.0f, _cfg->reminderEnabled);
        else
            _reminder->setEnabled(_cfg->reminderEnabled);
    }
    if (!doc["reminderIntervalMin"].isNull()) {
        _cfg->reminderIntervalMin =
            (uint32_t)constrain((long)doc["reminderIntervalMin"], 1L, 1440L);
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_REMINDER_INTERVAL_MIN,
                                             _cfg->reminderIntervalMin);
        else
            _reminder->setIntervalMin(_cfg->reminderIntervalMin);
    }
    if (!doc["reminderAlertTimeoutSec"].isNull()) {
        _cfg->reminderAlertTimeoutSec =
            (uint32_t)constrain((long)doc["reminderAlertTimeoutSec"], 5L, 3600L);
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_REMINDER_ALERT_TIMEOUT_SEC,
                                             _cfg->reminderAlertTimeoutSec);
        else
            _reminder->setAlertTimeoutSec(_cfg->reminderAlertTimeoutSec);
    }

    // Buzzer — apply immediately
    if (!doc["buzzerEnabled"].isNull()) {
        _cfg->buzzerEnabled = (bool)doc["buzzerEnabled"];
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_BUZZER_ENABLED,
                                             0, 0.0f, _cfg->buzzerEnabled);
        else
            _buzzer->setEnabled(_cfg->buzzerEnabled);
    }
    if (!doc["buzzerFrequencyHz"].isNull()) {
        _cfg->buzzerFrequencyHz =
            (uint32_t)constrain((long)doc["buzzerFrequencyHz"], 500L, 5000L);
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_BUZZER_FREQUENCY_HZ,
                                             _cfg->buzzerFrequencyHz);
        else
            _buzzer->setFrequency(_cfg->buzzerFrequencyHz);
    }
    if (!doc["buzzerVolumePercent"].isNull()) {
        _cfg->buzzerVolumePercent =
            (uint8_t)constrain((long)doc["buzzerVolumePercent"], 0L, 100L);
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_BUZZER_VOLUME_PERCENT,
                                             _cfg->buzzerVolumePercent);
        else
            _buzzer->setVolume(_cfg->buzzerVolumePercent);
    }
    if (!doc["buzzerDurationMs"].isNull()) {
        _cfg->buzzerDurationMs =
            (uint32_t)constrain((long)doc["buzzerDurationMs"], 50L, 2000L);
        if (_runtime && _runtime->isControlRunning())
            controlCommandsOk &= _runCommand(ControlCommandType::SET_BUZZER_DURATION_MS,
                                             _cfg->buzzerDurationMs);
        else
            _buzzer->setDuration(_cfg->buzzerDurationMs);
    }

    // WiFi — needs reboot
    if (!doc["wifiSsid"].isNull()) {
        _cfg->wifiSsid = doc["wifiSsid"].as<String>();
        rebootRequired = true;
    }
    if (!doc["wifiPassword"].isNull()) {
        const String p = doc["wifiPassword"].as<String>();
        if (!p.isEmpty() && p != "****") {
            _cfg->wifiPassword = p;
            rebootRequired = true;
        }
    }

    // Discord Webhook — allow empty string to clear
    if (!doc["discordWebhookUrl"].isNull()) {
        const String u = doc["discordWebhookUrl"].as<String>();
        if (u.indexOf("****") < 0)
            _cfg->discordWebhookUrl = u;
    }

    // NTP — needs reboot
    if (!doc["ntpEnabled"].isNull()) {
        _cfg->ntpEnabled = (bool)doc["ntpEnabled"];
        rebootRequired = true;
    }
    if (!doc["ntpServer1"].isNull()) {
        _cfg->ntpServer1 = doc["ntpServer1"].as<String>();
        rebootRequired = true;
    }
    if (!doc["ntpServer2"].isNull()) {
        _cfg->ntpServer2 = doc["ntpServer2"].as<String>();
        rebootRequired = true;
    }
    {
        JsonVariant tzV = doc["timezoneOffsetSec"];
        if (!tzV.isNull() && (tzV.is<int>() || tzV.is<float>())) {
            _cfg->timezoneOffsetSec =
                (int)constrain((long)tzV.as<int>(), -43200L, 50400L);
            rebootRequired = true;
        }
    }

    // Advanced sensor settings — ScaleManager reads at init, needs reboot.
    // Type-check before apply: wrong type (e.g. string) would produce 0.0 and constrain to min.
    auto applyFloat = [&](const char* key, float& target, float lo, float hi) {
        JsonVariant v = doc[key];
        if (!v.isNull() && (v.is<float>() || v.is<int>())) {
            target = constrain(v.as<float>(), lo, hi);
            rebootRequired = true;
        }
    };
    auto applyUlong = [&](const char* key, uint32_t& target, long lo, long hi) {
        JsonVariant v = doc[key];
        if (!v.isNull() && (v.is<int>() || v.is<float>())) {
            target = (uint32_t)constrain((long)v.as<float>(), lo, hi);
            rebootRequired = true;
        }
    };
    auto applyUint16 = [&](const char* key, uint16_t& target, long lo, long hi) {
        JsonVariant v = doc[key];
        if (!v.isNull() && (v.is<int>() || v.is<float>())) {
            target = (uint16_t)constrain((long)v.as<float>(), lo, hi);
            rebootRequired = true;
        }
    };
    applyFloat("cupPresentThresholdGram", _cfg->cupPresentThresholdGram, 10.0f,  500.0f);
    applyFloat("stableToleranceGram",     _cfg->stableToleranceGram,      0.5f,   20.0f);
    applyUlong("stableDurationMs",        _cfg->stableDurationMs,          500L, 10000L);
    applyFloat("minDrinkDeltaMl",         _cfg->minDrinkDeltaMl,           5.0f,  100.0f);
    applyFloat("maxDrinkDeltaMl",         _cfg->maxDrinkDeltaMl,          50.0f, 1000.0f);

    // MQTT — needs reboot
    if (!doc["mqttEnabled"].isNull())    { _cfg->mqttEnabled    = (bool)doc["mqttEnabled"]; rebootRequired = true; }
    if (!doc["mqttBrokerHost"].isNull()) { _cfg->mqttBrokerHost = doc["mqttBrokerHost"].as<String>(); rebootRequired = true; }
    if (!doc["mqttUsername"].isNull())   { _cfg->mqttUsername   = doc["mqttUsername"].as<String>();   rebootRequired = true; }
    if (!doc["mqttClientId"].isNull()) {
        const String cid = doc["mqttClientId"].as<String>();
        _cfg->mqttClientId = cid.isEmpty() ? String(DEFAULT_MQTT_CLIENT_ID) : cid;
        rebootRequired = true;
    }
    if (!doc["mqttPassword"].isNull()) {
        const String p = doc["mqttPassword"].as<String>();
        if (!p.isEmpty() && p != "****") { _cfg->mqttPassword = p; rebootRequired = true; }
    }
    applyUint16("mqttBrokerPort",   _cfg->mqttBrokerPort,   1L, 65535L);
    applyUint16("mqttHeartbeatSec", _cfg->mqttHeartbeatSec, 5L, 3600L);

    _cfgMgr->save(*_cfg);
    if (_discord) _discord->configure(*_cfg);
    if (!controlCommandsOk) rebootRequired = true;

    JsonDocument resp;
    resp["ok"]              = true;
    resp["reboot_required"] = rebootRequired;
    resp["control_applied"] = controlCommandsOk;
    String json; serializeJson(resp, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleTare() {
    if (!_requireApiAuth(true)) return;
    if (_runtime && _runtime->isControlRunning()) {
        ControlResult result;
        if (!_runCommand(ControlCommandType::TARE, 0, 0.0f, false,
                         &result, pdMS_TO_TICKS(4500))) {
            const int code = result.status == ControlResultStatus::BUSY ? 409 :
                             result.status == ControlResultStatus::NOT_READY ? 503 : 504;
            _server->send(code, "application/json",
                          "{\"ok\":false,\"error\":\"tare_failed_or_timed_out\"}");
            return;
        }
        _cfg->tareOffset = result.tareOffset;
        _cfg->calibrationFactor = result.calibrationFactor;
        if (!_cfgMgr->saveCalibration(result.calibrationFactor, result.tareOffset)) {
            _server->send(500, "application/json",
                          "{\"ok\":false,\"error\":\"tare_applied_but_persist_failed\"}");
            return;
        }
        _server->send(200, "application/json", "{\"ok\":true}");
        return;
    }

    _server->send(503, "application/json",
                  "{\"ok\":false,\"error\":\"rtos_control_unavailable\"}");
}

void DashboardServer::_handleCalibrate() {
    if (!_requireApiAuth(true)) return;
    if (!_runtime || !_runtime->isControlRunning()) {
        _server->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"rtos_control_unavailable\"}");
        return;
    }
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    if (!doc["known_weight_g"].is<float>()) {
        _server->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"known_weight_g required\"}");
        return;
    }
    const float knownG = doc["known_weight_g"].as<float>();
    if (knownG <= 0.0f) {
        _server->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"known_weight_g must be > 0\"}");
        return;
    }
    float factor;
    float currentWeight;
    ControlResult result;
    if (!_runCommand(ControlCommandType::CALIBRATE, 0, knownG, false,
                     &result, pdMS_TO_TICKS(750))) {
        const int code = result.status == ControlResultStatus::BUSY ? 409 :
                         result.status == ControlResultStatus::NOT_READY ? 503 : 500;
        _server->send(code, "application/json",
                      "{\"ok\":false,\"error\":\"calibration_failed\"}");
        return;
    }
    factor = result.calibrationFactor;
    currentWeight = result.weightGrams;
    _cfg->calibrationFactor = factor;
    _cfg->tareOffset = result.tareOffset;
    if (!_cfgMgr->saveCalibration(factor, result.tareOffset)) {
        _server->send(500, "application/json",
                      "{\"ok\":false,\"error\":\"calibration_applied_but_persist_failed\"}");
        return;
    }
    if (factor == 0.0f) {
        _server->send(500, "application/json",
                      "{\"ok\":false,\"error\":\"Calibration failed: net raw is zero\"}");
        return;
    }
    JsonDocument resp;
    resp["ok"]                 = true;
    resp["calibration_factor"] = factor;
    resp["current_weight_g"]   = currentWeight;
    String json; serializeJson(resp, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleWifiScan() {
    if (!_requireApiAuth(false)) return;
    const int n = WiFi.scanNetworks(false, false, false, 100);

    if (n < 0) {
        const char* err = (n == WIFI_SCAN_RUNNING) ? "scan_busy" : "scan_failed";
        _server->send(503, "application/json",
                      String("{\"ok\":false,\"error\":\"") + err + "\"}");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        bool found = false;
        for (JsonObject existing : arr) {
            if (existing["ssid"].as<String>() == ssid) {
                if (WiFi.RSSI(i) > existing["rssi"].as<int>())
                    existing["rssi"] = WiFi.RSSI(i);
                found = true;
                break;
            }
        }
        if (!found) {
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"]   = ssid;
            obj["rssi"]   = WiFi.RSSI(i);
            obj["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
    }
    WiFi.scanDelete();

    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleReboot() {
    if (!_requireApiAuth(true)) return;
    _server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}

void DashboardServer::_handleLogs() {
    if (!_requireApiAuth(false)) return;
    if (!_server->hasArg("month")) {
        _server->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"month parameter required (YYYY-MM or unsynced)\"}");
        return;
    }
    const String month = _server->arg("month");

    // Accept "unsynced" (NTP not synced at log time) or YYYY-MM with valid range
    const bool isUnsynced = (month == "unsynced");
    if (!isUnsynced) {
        bool valid = (month.length() == 7 && month[4] == '-');
        if (valid) {
            for (int i = 0; i < 4 && valid; i++) valid = isDigit(month[i]);
            for (int i = 5; i < 7 && valid; i++) valid = isDigit(month[i]);
        }
        if (valid) {
            const int mm = month.substring(5, 7).toInt();
            valid = (mm >= 1 && mm <= 12);
        }
        if (!valid) {
            _server->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"invalid month, expected YYYY-MM (01-12) or unsynced\"}");
            return;
        }
    }

    if (!_state->logFsOk || _logFs == nullptr) {
        _server->send(503, "application/json", "{\"ok\":false,\"error\":\"logfs unavailable\"}");
        return;
    }

    const String path = "/logs/drink-" + month + ".jsonl";
    if (_eventLogger && !_eventLogger->lockFilesystem(pdMS_TO_TICKS(2000))) {
        _server->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"logfs_busy\"}");
        return;
    }
    File f = _logFs->open(path, "r");
    if (!f) {
        if (_eventLogger) _eventLogger->unlockFilesystem();
        _server->send(200, "application/json",
                      "{\"ok\":true,\"month\":\"" + month + "\",\"entries\":[],\"skipped\":0}");
        return;
    }

    _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server->send(200, "application/json", "");
    _server->sendContent("{\"ok\":true,\"month\":\"" + month + "\",\"entries\":[");

    bool first = true;
    uint32_t skipped = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        JsonDocument entry;
        if (deserializeJson(entry, line)) { skipped++; continue; }

        String entryJson;
        serializeJson(entry, entryJson);
        if (!first) _server->sendContent(",");
        _server->sendContent(entryJson);
        first = false;
    }

    _server->sendContent("],\"skipped\":" + String(skipped) + "}");
    f.close();
    if (_eventLogger) _eventLogger->unlockFilesystem();
}
