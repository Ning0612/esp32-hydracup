#pragma once
#include <Arduino.h>
#include <atomic>
#include "AppState.h"
#include "app_types.h"

class DiscordNotifier {
public:
    void init(AppState& state, const AppConfig& cfg);
    void notifyOnline(const String& ipAddress);
    void notifyDrink(float amountMl, float totalMl, const String& timestamp);
    void update();

private:
    struct TaskParam {
        char                webhookUrl[512];
        char                body[512];
        bool*               lastOkPtr;
        std::atomic<bool>*  taskRunningPtr;
    };

    static void _sendTask(void* param);

    AppState*              _state       = nullptr;
    const AppConfig*       _cfg         = nullptr;
    std::atomic<bool>      _taskRunning{false};
};
