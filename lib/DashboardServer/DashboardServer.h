#pragma once

#include <cstdint>
#include <string>

#include "RuntimeCoordinator.h"
#include "app_types.h"
#include "esp_http_server.h"

class AppState;
class BuzzerController;
class ConfigManager;
class DiscordNotifier;
class EventLogger;
class ReminderManager;
class ScaleManager;
class WiFiManager;

class DashboardServer {
public:
    void begin(ScaleManager& scale, ConfigManager& cfgMgr, AppState& state, AppConfig& cfg,
               BuzzerController& buzzer, ReminderManager& reminder, bool logFsOk,
               RuntimeCoordinator& runtime, EventLogger& eventLogger,
               DiscordNotifier& discord, WiFiManager& wifi);
    void loop() {}

private:
    static esp_err_t _getHandler(httpd_req_t* request);
    static esp_err_t _postHandler(httpd_req_t* request);
    esp_err_t _handleGet(httpd_req_t* request);
    esp_err_t _handlePost(httpd_req_t* request);
    void _handleStatic(httpd_req_t* request, const char* path, const char* contentType);
    void _sendJson(httpd_req_t* request, const std::string& json, int status = 200);
    bool _requirePageAuth(httpd_req_t* request, const char* nextPath);
    bool _requireApiAuth(httpd_req_t* request, bool requireCsrf);
    bool _isSessionValid(httpd_req_t* request);
    bool _hasValidCsrf(httpd_req_t* request) const;
    void _clearSession();
    void _establishSession();
    bool _isRateLimited(const std::string& ip);
    void _recordAuthFailure(const std::string& ip);
    void _sendAuthFailure(httpd_req_t* request, int status, const char* error);
    bool _runCommand(ControlCommandType type, uint32_t uintValue = 0,
                     float floatValue = 0.0f, bool boolValue = false,
                     ControlResult* result = nullptr,
                     TickType_t timeoutTicks = pdMS_TO_TICKS(500));
    static const char* _cupStateStr(CupState state);
    static std::string _maskWebhookUrl(const std::string& url);

    struct AuthFailureBucket {
        std::string ip;
        uint32_t timestamps[10] = {};
        uint8_t count = 0;
        uint32_t lastSeen = 0;
    };

    httpd_handle_t _server = nullptr;
    ScaleManager* _scale = nullptr;
    ConfigManager* _cfgMgr = nullptr;
    AppState* _state = nullptr;
    AppConfig* _cfg = nullptr;
    BuzzerController* _buzzer = nullptr;
    ReminderManager* _reminder = nullptr;
    RuntimeCoordinator* _runtime = nullptr;
    EventLogger* _eventLogger = nullptr;
    DiscordNotifier* _discord = nullptr;
    WiFiManager* _wifi = nullptr;
    bool _logFsOk = false;
    std::string _sessionToken;
    std::string _sessionCsrfToken;
    std::string _preAuthCsrfToken;
    uint32_t _sessionStartMs = 0;
    uint32_t _lastActivityMs = 0;
    AuthFailureBucket _authFailures[8];

    static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
    static constexpr uint32_t SESSION_ABSOLUTE_TIMEOUT_MS = 24UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t AUTH_FAILURE_WINDOW_MS = 5UL * 60UL * 1000UL;
    static constexpr uint8_t AUTH_FAILURE_LIMIT = 10;
};
