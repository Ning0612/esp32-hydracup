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
    void _handleStyleCss();
    void _handleCalibrationRedirect();

    // API handlers
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
    bool _runCommand(ControlCommandType type, uint32_t uintValue = 0,
                     float floatValue = 0.0f, bool boolValue = false,
                     ControlResult* result = nullptr,
                     TickType_t timeoutTicks = pdMS_TO_TICKS(500));
};
