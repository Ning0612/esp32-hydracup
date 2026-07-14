#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "ConfigManager.h"
#include "AppState.h"

class ConfigPortal {
public:
    void begin(ConfigManager& cfgMgr, AppState& state, AppConfig& cfg);
    void loop();

private:
    WebServer*     _server  = nullptr;
    ConfigManager* _cfgMgr  = nullptr;
    AppState*      _state   = nullptr;
    AppConfig*     _cfg     = nullptr;

    void _handleRoot();
    void _handleAuthCsrf();
    void _handleAuthLogin();
    void _handleAuthLogout();
    void _handleStatus();
    void _handleConfig();
    void _handleWifiScan();
    void _handleReboot();

    bool _requireApiAuth(bool requireCsrf);
    bool _isSessionValid();
    bool _hasValidCsrf() const;
    void _clearSession();
    void _establishSession();
    bool _isRateLimited(const String& ip);
    void _recordAuthFailure(const String& ip);
    void _sendAuthFailure(int statusCode, const char* error);

    static constexpr uint32_t SCAN_COOLDOWN_MS = 5000;
    static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
    static constexpr uint32_t SESSION_ABSOLUTE_TIMEOUT_MS = 24UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t AUTH_FAILURE_WINDOW_MS = 5UL * 60UL * 1000UL;
    static constexpr uint8_t AUTH_FAILURE_LIMIT = 10;
    static constexpr size_t AUTH_BUCKET_COUNT = 8;

    struct AuthFailureBucket {
        String ip;
        uint32_t timestamps[10] = {};
        uint8_t count = 0;
        uint32_t lastSeen = 0;
    };

    uint32_t _lastScanMs = 0;
    String _csrfToken;
    String _sessionToken;
    String _sessionCsrfToken;
    uint32_t _sessionStartMs = 0;
    uint32_t _lastActivityMs = 0;
    AuthFailureBucket _authFailures[AUTH_BUCKET_COUNT];

    static const char* _setupPageHtml;
};
