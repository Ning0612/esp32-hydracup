#include "EventLogger.h"
#include "TimeManager.h"
#include <cstring>

void EventLogger::init(bool fsOk, fs::LittleFSFS& fs) {
    _fsOk = fsOk;
    _fs   = &fs;
    _fsMutex = xSemaphoreCreateMutex();
    _queue = xQueueCreate(8, sizeof(LogMessage));
    if (!_fsMutex || !_queue ||
        xTaskCreate(_taskFunc, "storage_log", 3072, this, 1, &_taskHandle) != pdPASS) {
        Serial.println("[EventLog] async worker unavailable — logging disabled");
        _fsOk = false;
        return;
    }
    if (_fsOk && !_fs->exists("/logs")) {
        if (!_fs->mkdir("/logs")) {
            Serial.println("[EventLog] Failed to create /logs — logging disabled");
            _fsOk = false;
        }
    }
}

void EventLogger::logDrink(const String& timestamp, float amountMl, float totalMl, TimeManager* tm) {
    if (!_fsOk || !_queue) return;
    LogMessage message{};
    const String yearMonth = (tm && tm->isSynced()) ? tm->getYearMonth() : String("unsynced");
    std::strncpy(message.timestamp, timestamp.c_str(), sizeof(message.timestamp) - 1);
    std::strncpy(message.yearMonth, yearMonth.c_str(), sizeof(message.yearMonth) - 1);
    message.amountMl = amountMl;
    message.totalMl = totalMl;
    if (xQueueSend(_queue, &message, 0) != pdTRUE) {
        _droppedCount++;
        Serial.println("[EventLog] queue full, drink log dropped");
    }
}

bool EventLogger::lockFilesystem(TickType_t timeoutTicks) {
    return _fsMutex && xSemaphoreTake(_fsMutex, timeoutTicks) == pdTRUE;
}

void EventLogger::unlockFilesystem() {
    if (_fsMutex) xSemaphoreGive(_fsMutex);
}

void EventLogger::_taskFunc(void* param) {
    static_cast<EventLogger*>(param)->_taskLoop();
}

void EventLogger::_taskLoop() {
    LogMessage message;
    for (;;) {
        if (xQueueReceive(_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (!lockFilesystem(pdMS_TO_TICKS(2000))) {
            _droppedCount++;
            continue;
        }

        const String path = String("/logs/drink-") + message.yearMonth + ".jsonl";
        File f = _fs->open(path, "a");
        if (f) {
            char line[128];
            snprintf(line, sizeof(line),
                     "{\"ts\":\"%s\",\"ml\":%.0f,\"total\":%.0f}\n",
                     message.timestamp, message.amountMl, message.totalMl);
            f.print(line);
            f.close();
            Serial.printf("[EventLog] %s\n", path.c_str());
        } else {
            _droppedCount++;
            Serial.printf("[EventLog] Failed to open %s\n", path.c_str());
        }
        unlockFilesystem();
    }
}
