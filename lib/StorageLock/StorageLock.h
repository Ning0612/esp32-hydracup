#pragma once

#include <freertos/FreeRTOS.h>

bool lockNvs(TickType_t timeoutTicks = pdMS_TO_TICKS(2000));
void unlockNvs();
