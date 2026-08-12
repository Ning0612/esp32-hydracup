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

// OTA firmware update
// Both assets are published by .github/workflows/release.yml under fixed names; the URLs
// are baked in, so renaming a release asset breaks updates on every deployed device.
// /releases/latest/download/ resolves only to non-draft, non-prerelease releases, which
// makes prereleases safe for internal testing.
#define OTA_MANIFEST_URL "https://github.com/Ning0612/esp32-hydracup/releases/latest/download/version.txt"
#define OTA_FIRMWARE_URL "https://github.com/Ning0612/esp32-hydracup/releases/latest/download/hydracup-esp32dev-firmware.bin"
// The web assets live in a data partition esp_https_ota cannot target, so they are streamed
// straight onto it. SHA256SUMS is published alongside them and is the only way to tell whether
// the image actually arrived intact - there is no second webfs partition to fall back to.
#define OTA_WEBFS_URL      "https://github.com/Ning0612/esp32-hydracup/releases/latest/download/hydracup-esp32dev-littlefs.bin"
#define OTA_CHECKSUMS_URL  "https://github.com/Ning0612/esp32-hydracup/releases/latest/download/SHA256SUMS"
#define OTA_WEBFS_ASSET    "hydracup-esp32dev-littlefs.bin"
#define OTA_WEBFS_LABEL    "webfs"
#define OTA_WEBFS_CHUNK    4096
#define OTA_WEBFS_DEADLINE_MS    (3 * 60 * 1000)
#define OTA_OVERALL_DEADLINE_MS  (5 * 60 * 1000)
#define OTA_HTTP_TIMEOUT_MS      15000
#define OTA_HTTP_BUFFER_BYTES    4096
#define OTA_MIN_FREE_HEAP_BYTES  (60 * 1024)
// Uptime a freshly flashed image must reach, with the control task still beating, before
// the bootloader's rollback is cancelled. Never add connectivity to this condition.
#define OTA_MARK_VALID_DELAY_MS  30000
#define OTA_MARK_VALID_RETRY_MS  5000
// Wait for in-flight cloud sync / Discord requests to release their TLS heap after the shed
// flag goes up. The timeout exceeds their 10 s HTTP timeout so a request that is already
// blocked still gets to finish rather than making the update fail.
#define OTA_SETTLE_MIN_MS        1000
#define OTA_SETTLE_POLL_MS       250
#define OTA_SETTLE_TIMEOUT_MS    12000

// MQTT defaults
#define DEFAULT_MQTT_BROKER_PORT    1883
#define DEFAULT_MQTT_CLIENT_ID      "hydracup-device"
#define DEFAULT_MQTT_HEARTBEAT_SEC  60
