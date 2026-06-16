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

class DashboardServer {
public:
    void begin(ScaleManager& scale, ConfigManager& cfgMgr,
               AppState& state, AppConfig& cfg,
               BuzzerController& buzzer, ReminderManager& reminder);
    void loop();

private:
    WebServer*        _server   = nullptr;
    ScaleManager*     _scale    = nullptr;
    ConfigManager*    _cfgMgr   = nullptr;
    AppState*         _state    = nullptr;
    AppConfig*        _cfg      = nullptr;
    BuzzerController* _buzzer   = nullptr;
    ReminderManager*  _reminder = nullptr;

    // Page handlers
    void _handleRoot();
    void _handleSettings();
    void _handleCalibrationPage();

    // API handlers
    void _handleWeight();
    void _handleStatus();
    void _handleGetConfig();
    void _handlePostConfig();
    void _handleTare();
    void _handleCalibrate();
    void _handleWifiScan();
    void _handleReboot();

    static const char* _cupStateStr(CupState s);
    static String _maskWebhookUrl(const String& url);
};
