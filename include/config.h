#pragma once

// Serial
#define SERIAL_BAUD_RATE 115200

// OLED
#define OLED_I2C_ADDRESS    0x3C
#define OLED_SCREEN_WIDTH   128
#define OLED_SCREEN_HEIGHT   32
#define OLED_RESET_PIN       -1
#define OLED_UPDATE_INTERVAL_MS 500

// Buzzer LEDC
#define BUZZER_LEDC_CHANNEL    0
#define BUZZER_LEDC_RESOLUTION 8
#define BUZZER_DEFAULT_FREQ_HZ    2000
#define BUZZER_DEFAULT_DURATION_MS 150
#define BUZZER_DEFAULT_VOLUME_PCT   50

// HX711
#define HX711_INIT_TIMEOUT_MS    3000
#define HX711_SAMPLE_COUNT         10
#define HX711_READ_INTERVAL_MS    100
#define SCALE_SERIAL_INTERVAL_MS 1000

// NVS
#define NVS_NAMESPACE "water_config"

// AP Mode defaults
#define AP_DEFAULT_SSID     "WaterCupTracker-Setup"
#define AP_DEFAULT_PASSWORD "12345678"

// Drink detection defaults
#define DEFAULT_CUP_THRESHOLD_G    80.0f
#define DEFAULT_STABLE_TOLERANCE_G  3.0f
#define DEFAULT_STABLE_DURATION_MS 3000
#define DEFAULT_MIN_DRINK_ML        20.0f
#define DEFAULT_MAX_DRINK_ML       500.0f

// Reminder defaults
#define DEFAULT_REMINDER_INTERVAL_MIN    60
#define DEFAULT_REMINDER_ALERT_TIMEOUT_SEC 60
#define DEFAULT_DAILY_GOAL_ML          2000

// NTP defaults
#define DEFAULT_NTP_SERVER1    "pool.ntp.org"
#define DEFAULT_NTP_SERVER2    "time.google.com"
#define DEFAULT_TIMEZONE       "Asia/Taipei"
#define DEFAULT_TZ_OFFSET_SEC  (8 * 3600)
#define DEFAULT_DST_OFFSET_SEC 0
