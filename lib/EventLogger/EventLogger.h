#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class TimeManager;

class EventLogger {
public:
    void init(bool fsOk);
    void logDrink(const std::string& timestamp, float amountMl, float totalMl,
                  TimeManager* tm);
    bool lockFilesystem(TickType_t timeoutTicks);
    void unlockFilesystem();
    uint32_t getDroppedCount() const { return _droppedCount.load(); }

private:
    struct LogMessage {
        char timestamp[32];
        char yearMonth[12];
        float amountMl;
        float totalMl;
    };
    static void _taskFunc(void* param);
    void _taskLoop();
    bool _fsOk = false;
    QueueHandle_t _queue = nullptr;
    SemaphoreHandle_t _fsMutex = nullptr;
    TaskHandle_t _taskHandle = nullptr;
    std::atomic<uint32_t> _droppedCount{0};
};
