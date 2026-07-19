#include "EventLogger.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "TimeManager.h"
#include "hal_log.h"

void EventLogger::init(bool fsOk) {
    _fsOk = fsOk;
    _fsMutex = xSemaphoreCreateMutex();
    _queue = xQueueCreate(8, sizeof(LogMessage));
    if (!_fsOk || !_fsMutex || !_queue ||
        xTaskCreate(_taskFunc, "storage_log", 3072, this, 1, &_taskHandle) != pdPASS) {
        _fsOk = false;
        LOG_WARN("EventLog", "worker unavailable; logging disabled");
        return;
    }
    mkdir("/logfs/logs", 0777);
}

void EventLogger::logDrink(const std::string& timestamp, float amountMl, float totalMl,
                           TimeManager* tm) {
    if (!_fsOk || !_queue) return;
    LogMessage message = {};
    const std::string yearMonth = tm && tm->isSynced() ? tm->getYearMonth() : "unsynced";
    std::strncpy(message.timestamp, timestamp.c_str(), sizeof(message.timestamp) - 1);
    std::strncpy(message.yearMonth, yearMonth.c_str(), sizeof(message.yearMonth) - 1);
    message.amountMl = amountMl;
    message.totalMl = totalMl;
    if (xQueueSend(_queue, &message, 0) != pdTRUE) {
        _droppedCount++;
        LOG_WARN("EventLog", "queue full, drink log dropped");
    }
}

bool EventLogger::lockFilesystem(TickType_t timeoutTicks) {
    return _fsMutex && xSemaphoreTake(_fsMutex, timeoutTicks) == pdTRUE;
}

void EventLogger::unlockFilesystem() { if (_fsMutex) xSemaphoreGive(_fsMutex); }

void EventLogger::_taskFunc(void* param) { static_cast<EventLogger*>(param)->_taskLoop(); }

void EventLogger::_taskLoop() {
    LogMessage message = {};
    for (;;) {
        if (xQueueReceive(_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (!lockFilesystem(pdMS_TO_TICKS(2000))) { _droppedCount++; continue; }
        char path[64];
        std::snprintf(path, sizeof(path), "/logfs/logs/drink-%s.jsonl", message.yearMonth);
        FILE* file = std::fopen(path, "a");
        if (file) {
            std::fprintf(file, "{\"ts\":\"%s\",\"ml\":%.0f,\"total\":%.0f}\n",
                         message.timestamp, message.amountMl, message.totalMl);
            std::fclose(file);
            LOG_INFO("EventLog", "%s", path);
        } else {
            _droppedCount++;
            LOG_WARN("EventLog", "failed to open %s", path);
        }
        unlockFilesystem();
    }
}
