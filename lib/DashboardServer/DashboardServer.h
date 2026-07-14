#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "app_types.h"
#include "AppState.h"
#include "ConfigManager.h"
#include "ScaleManager.h"
#include "BuzzerController.h"
#include "ReminderManager.h"
#include "RuntimeCoordinator.h"

class DiscordNotifier;
class EventLogger;
class RuntimeCoordinator;

class DashboardServer {
public:
    void begin(ScaleManager& scale, ConfigManager& cfgMgr,
               AppState& state, AppConfig& cfg,
               BuzzerController& buzzer, ReminderManager& reminder,
               fs::LittleFSFS& logFs, RuntimeCoordinator& runtime,
               EventLogger& eventLogger, DiscordNotifier& discord);
    void loop();

private:
    WebServer*        _server   = nullptr;
    ScaleManager*     _scale    = nullptr;
    ConfigManager*    _cfgMgr   = nullptr;
    AppState*         _state    = nullptr;
    AppConfig*        _cfg      = nullptr;
    BuzzerController* _buzzer   = nullptr;
    ReminderManager*  _reminder = nullptr;
    fs::LittleFSFS*   _logFs    = nullptr;
    RuntimeCoordinator* _runtime = nullptr;
    EventLogger* _eventLogger = nullptr;
    DiscordNotifier* _discord = nullptr;

    // Shared LittleFS file server
    void _serveFile(const char* path, const char* contentType);

    // Page handlers
    void _handleRoot();
    void _handleSettings();
    void _handleHistory();
    void _handleLogin();
    void _handleStyleCss();
    void _handleUiJs();
    void _handleFavicon();
    void _handleCalibrationRedirect();

    // API handlers
    void _handleAuthCsrf();
    void _handleAuthLogin();
    void _handleAuthLogout();
    void _handleWeight();
    void _handleStatus();
    void _handleGetConfig();
    void _handlePostConfig();
    void _handleTare();
    void _handleCalibrate();
    void _handleWifiScan();
    void _handleReboot();
    void _handleLogs();

    static const char* _cupStateStr(CupState s);
    static String _maskWebhookUrl(const String& url);
    bool _requirePageAuth(const char* nextPath);
    bool _requireApiAuth(bool requireCsrf);
    bool _isSessionValid();
    bool _hasValidCsrf() const;
    void _clearSession();
    void _establishSession();
    bool _isRateLimited(const String& ip);
    void _recordAuthFailure(const String& ip);
    void _sendAuthFailure(int statusCode, const char* error);
    bool _runCommand(ControlCommandType type, uint32_t uintValue = 0,
                     float floatValue = 0.0f, bool boolValue = false,
                     ControlResult* result = nullptr,
                     TickType_t timeoutTicks = pdMS_TO_TICKS(500));

    struct AuthFailureBucket {
        String ip;
        uint32_t timestamps[10] = {};
        uint8_t count = 0;
        uint32_t lastSeen = 0;
    };

    static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
    static constexpr uint32_t SESSION_ABSOLUTE_TIMEOUT_MS = 24UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t AUTH_FAILURE_WINDOW_MS = 5UL * 60UL * 1000UL;
    static constexpr uint8_t AUTH_FAILURE_LIMIT = 10;
    static constexpr size_t AUTH_BUCKET_COUNT = 8;

    String _sessionToken;
    String _sessionCsrfToken;
    String _preAuthCsrfToken;
    uint32_t _sessionStartMs = 0;
    uint32_t _lastActivityMs = 0;
    AuthFailureBucket _authFailures[AUTH_BUCKET_COUNT];
};
