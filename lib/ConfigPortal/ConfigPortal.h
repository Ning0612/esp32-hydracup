#pragma once

#include <cstdint>
#include <string>

#include "esp_http_server.h"

class AppState;
class ConfigManager;
struct AppConfig;
class WiFiManager;

class ConfigPortal {
public:
    void begin(ConfigManager& cfgMgr, AppState& state, AppConfig& cfg, WiFiManager& wifi);
    void loop() {}

private:
    static esp_err_t _getHandler(httpd_req_t* request);
    static esp_err_t _postHandler(httpd_req_t* request);
    esp_err_t _handleGet(httpd_req_t* request);
    esp_err_t _handlePost(httpd_req_t* request);
    bool _requireApiAuth(httpd_req_t* request, bool requireCsrf);
    bool _isSessionValid(httpd_req_t* request);
    bool _hasValidCsrf(httpd_req_t* request) const;
    void _clearSession();
    void _establishSession();
    bool _isRateLimited(const std::string& ip);
    void _recordAuthFailure(const std::string& ip);
    void _sendAuthFailure(httpd_req_t* request, int status, const char* error);
    void _sendJson(httpd_req_t* request, const std::string& json, int status = 200);

    struct AuthFailureBucket { std::string ip; uint32_t timestamps[10] = {}; uint8_t count = 0; uint32_t lastSeen = 0; };
    httpd_handle_t _server = nullptr;
    ConfigManager* _cfgMgr = nullptr;
    AppState* _state = nullptr;
    AppConfig* _cfg = nullptr;
    WiFiManager* _wifi = nullptr;
    uint32_t _lastScanMs = 0;
    std::string _csrfToken;
    std::string _sessionToken;
    std::string _sessionCsrfToken;
    uint32_t _sessionStartMs = 0;
    uint32_t _lastActivityMs = 0;
    AuthFailureBucket _authFailures[8];
    static constexpr uint32_t SCAN_COOLDOWN_MS = 5000;
    static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30UL * 60UL * 1000UL;
    static constexpr uint32_t SESSION_ABSOLUTE_TIMEOUT_MS = 24UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t AUTH_FAILURE_WINDOW_MS = 5UL * 60UL * 1000UL;
    static constexpr uint8_t AUTH_FAILURE_LIMIT = 10;
};
