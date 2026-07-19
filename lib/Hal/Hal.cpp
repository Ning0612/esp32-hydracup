#include "hal_time.h"
#include "hal_delay.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint64_t hal_micros() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint32_t hal_millis() {
    return static_cast<uint32_t>(hal_micros() / 1000ULL);
}

void hal_delay_ms(uint32_t milliseconds) {
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}
