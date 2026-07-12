#include "StorageLock.h"
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t nvsMutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}
}

bool lockNvs(TickType_t timeoutTicks) {
    SemaphoreHandle_t mutex = nvsMutex();
    return mutex && xSemaphoreTake(mutex, timeoutTicks) == pdTRUE;
}

void unlockNvs() {
    SemaphoreHandle_t mutex = nvsMutex();
    if (mutex) xSemaphoreGive(mutex);
}
