#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include "AppState.h"
#include "app_types.h"

class DiscordNotifier {
public:
    void init(AppState& state, const AppConfig& cfg);
    void configure(const AppConfig& cfg);
    void notifyOnline(const String& ipAddress);
    void notifyDrink(float amountMl, float totalMl, uint32_t drinkCount);
    bool notifyDailySummary(float totalMl, uint32_t drinkCount, const String& dateStr);
    void update();
    bool isWorkerReady() const { return _workerReady.load(); }
    uint32_t getDroppedCount() const { return _droppedCount.load(); }

private:
    struct TaskParam {
        char                webhookUrl[512];
        char                body[768];
    };

    static void _workerTask(void* param);
    void _workerLoop();
    void _send(TaskParam* message);
    bool _enqueue(TaskParam* message, bool highPriority);
    bool _copyConfig(char* webhookUrl, size_t size, uint32_t& dailyGoalMl);

    AppState* _state = nullptr;
    QueueHandle_t _highQueue = nullptr;
    QueueHandle_t _lowQueue = nullptr;
    SemaphoreHandle_t _configMutex = nullptr;
    TaskHandle_t _workerHandle = nullptr;
    char _webhookUrl[512] = {};
    uint32_t _dailyGoalMl = 2000;
    std::atomic<bool> _workerReady{false};
    std::atomic<uint32_t> _droppedCount{0};
};
