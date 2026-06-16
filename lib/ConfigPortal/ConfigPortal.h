#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "ConfigManager.h"
#include "AppState.h"

class ConfigPortal {
public:
    void begin(ConfigManager& cfgMgr, AppState& state);
    void loop();

private:
    WebServer*     _server  = nullptr;
    ConfigManager* _cfgMgr  = nullptr;
    AppState*      _state   = nullptr;

    void _handleRoot();
    void _handleStatus();
    void _handleConfig();
    void _handleWifiScan();
    void _handleReboot();

    static constexpr uint32_t SCAN_COOLDOWN_MS = 5000;
    uint32_t _lastScanMs = 0;

    static const char* _setupPageHtml;
};
