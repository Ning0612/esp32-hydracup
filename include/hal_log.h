#pragma once

#include "esp_log.h"

#define LOG_INFO(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
