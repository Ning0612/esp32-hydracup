#pragma once
#include <Arduino.h>
#include <atomic>
#include "AppState.h"
#include "app_types.h"

class DiscordNotifier {
public:
    void init(AppState& state, const AppConfig& cfg);
    void notifyOnline(const String& ipAddress);
    void notifyDrink(float amountMl, float totalMl, uint32_t drinkCount);
    bool notifyDailySummary(float totalMl, uint32_t drinkCount, const String& dateStr);
    void update();

private:
    struct TaskParam {
        char                webhookUrl[512];
        char                body[768];
        std::atomic<bool>*  lastOkPtr;
        std::atomic<bool>*  taskRunningPtr;
    };

    static void _sendTask(void* param);

    AppState*              _state             = nullptr;
    const AppConfig*       _cfg               = nullptr;
    std::atomic<bool>      _taskRunning{false};
    std::atomic<bool>      _onlineTaskRunning{false};
    std::atomic<bool>      _summaryTaskRunning{false};
};
