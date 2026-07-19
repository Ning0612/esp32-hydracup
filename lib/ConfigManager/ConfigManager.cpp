#include "ConfigManager.h"

#include <cstring>

#include "config.h"
#include "hal_log.h"
#include "nvs.h"
#include "StorageLock.h"

namespace {

constexpr const char* TAG = "Config";

bool openConfig(nvs_handle_t& handle, nvs_open_mode_t mode) {
    return nvs_open(NVS_NAMESPACE, mode, &handle) == ESP_OK;
}

std::string readString(nvs_handle_t handle, const char* key,
                       const std::string& fallback) {
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
        return fallback;
    }
    std::string result(length, '\0');
    if (nvs_get_str(handle, key, result.data(), &length) != ESP_OK) return fallback;
    result.resize(length > 0 ? length - 1 : 0);
    return result;
}

template <typename T>
T readValue(nvs_handle_t handle, const char* key, T fallback,
            esp_err_t (*reader)(nvs_handle_t, const char*, T*)) {
    T value = fallback;
    if (reader(handle, key, &value) != ESP_OK) return fallback;
    return value;
}

float readFloat(nvs_handle_t handle, const char* key, float fallback) {
    float result = fallback;
    size_t length = sizeof(result);
    if (nvs_get_blob(handle, key, &result, &length) != ESP_OK ||
        length != sizeof(result)) return fallback;
    return result;
}

bool writeString(nvs_handle_t handle, const char* key, const std::string& value) {
    return nvs_set_str(handle, key, value.c_str()) == ESP_OK;
}

bool writeFloat(nvs_handle_t handle, const char* key, float value) {
    return nvs_set_blob(handle, key, &value, sizeof(value)) == ESP_OK;
}

bool writeConfig(nvs_handle_t handle, const AppConfig& cfg) {
    bool ok = true;
#define WRITE_STRING(key, field) ok = writeString(handle, key, cfg.field) && ok
#define WRITE_BOOL(key, field) ok = (nvs_set_u8(handle, key, cfg.field ? 1 : 0) == ESP_OK) && ok
#define WRITE_UINT(key, field) ok = (nvs_set_u32(handle, key, cfg.field) == ESP_OK) && ok
#define WRITE_INT(key, field) ok = (nvs_set_i32(handle, key, cfg.field) == ESP_OK) && ok
#define WRITE_FLOAT(key, field) ok = writeFloat(handle, key, cfg.field) && ok

    WRITE_STRING("wifi_ssid", wifiSsid);
    WRITE_STRING("wifi_pass", wifiPassword);
    WRITE_STRING("webhook_url", discordWebhookUrl);
    WRITE_BOOL("rem_en", reminderEnabled);
    WRITE_UINT("rem_interval", reminderIntervalMin);
    WRITE_UINT("rem_alert_to", reminderAlertTimeoutSec);
    WRITE_UINT("daily_goal", dailyGoalMl);
    WRITE_BOOL("buz_en", buzzerEnabled);
    WRITE_UINT("buz_freq", buzzerFrequencyHz);
    WRITE_UINT("buz_dur", buzzerDurationMs);
    WRITE_UINT("buz_vol", buzzerVolumePercent);
    WRITE_BOOL("ntp_en", ntpEnabled);
    WRITE_STRING("ntp_srv1", ntpServer1);
    WRITE_STRING("ntp_srv2", ntpServer2);
    WRITE_STRING("tz", timezone);
    WRITE_INT("tz_off", timezoneOffsetSec);
    WRITE_INT("dst_off", daylightOffsetSec);
    WRITE_BOOL("mqtt_en", mqttEnabled);
    WRITE_STRING("mqtt_host", mqttBrokerHost);
    WRITE_UINT("mqtt_port", mqttBrokerPort);
    WRITE_STRING("mqtt_user", mqttUsername);
    WRITE_STRING("mqtt_pass", mqttPassword);
    WRITE_STRING("mqtt_cid", mqttClientId);
    WRITE_UINT("mqtt_hb", mqttHeartbeatSec);
    WRITE_FLOAT("cal_factor", calibrationFactor);
    ok = (nvs_set_i32(handle, "tare_offset", static_cast<int32_t>(cfg.tareOffset)) == ESP_OK) && ok;
    WRITE_FLOAT("cup_thresh", cupPresentThresholdGram);
    WRITE_FLOAT("stable_tol", stableToleranceGram);
    WRITE_UINT("stable_dur", stableDurationMs);
    WRITE_FLOAT("min_drink", minDrinkDeltaMl);
    WRITE_FLOAT("max_drink", maxDrinkDeltaMl);
    WRITE_STRING("ap_ssid", apSsid);
    WRITE_STRING("ap_pass", apPassword);
    WRITE_STRING("admin_hash", adminPasswordHash);

#undef WRITE_STRING
#undef WRITE_BOOL
#undef WRITE_UINT
#undef WRITE_INT
#undef WRITE_FLOAT
    return ok;
}

}  // namespace

void ConfigManager::load(AppConfig& cfg) {
    _applyDefaults(cfg);
    if (!lockNvs()) return;

    nvs_handle_t handle = 0;
    if (!openConfig(handle, NVS_READONLY)) {
        unlockNvs();
        return;
    }

    cfg.wifiSsid = readString(handle, "wifi_ssid", cfg.wifiSsid);
    cfg.wifiPassword = readString(handle, "wifi_pass", cfg.wifiPassword);
    cfg.discordWebhookUrl = readString(handle, "webhook_url", cfg.discordWebhookUrl);
    cfg.reminderEnabled = readValue<uint8_t>(handle, "rem_en", cfg.reminderEnabled ? 1 : 0, nvs_get_u8) != 0;
    cfg.reminderIntervalMin = readValue<uint32_t>(handle, "rem_interval", cfg.reminderIntervalMin, nvs_get_u32);
    cfg.reminderAlertTimeoutSec = readValue<uint32_t>(handle, "rem_alert_to", cfg.reminderAlertTimeoutSec, nvs_get_u32);
    cfg.dailyGoalMl = readValue<uint32_t>(handle, "daily_goal", cfg.dailyGoalMl, nvs_get_u32);
    cfg.buzzerEnabled = readValue<uint8_t>(handle, "buz_en", cfg.buzzerEnabled ? 1 : 0, nvs_get_u8) != 0;
    cfg.buzzerFrequencyHz = readValue<uint32_t>(handle, "buz_freq", cfg.buzzerFrequencyHz, nvs_get_u32);
    cfg.buzzerDurationMs = readValue<uint32_t>(handle, "buz_dur", cfg.buzzerDurationMs, nvs_get_u32);
    cfg.buzzerVolumePercent = static_cast<uint8_t>(readValue<uint32_t>(handle, "buz_vol", cfg.buzzerVolumePercent, nvs_get_u32));
    cfg.ntpEnabled = readValue<uint8_t>(handle, "ntp_en", cfg.ntpEnabled ? 1 : 0, nvs_get_u8) != 0;
    cfg.ntpServer1 = readString(handle, "ntp_srv1", cfg.ntpServer1);
    cfg.ntpServer2 = readString(handle, "ntp_srv2", cfg.ntpServer2);
    cfg.timezone = readString(handle, "tz", cfg.timezone);
    cfg.timezoneOffsetSec = readValue<int32_t>(handle, "tz_off", cfg.timezoneOffsetSec, nvs_get_i32);
    cfg.daylightOffsetSec = readValue<int32_t>(handle, "dst_off", cfg.daylightOffsetSec, nvs_get_i32);
    cfg.mqttEnabled = readValue<uint8_t>(handle, "mqtt_en", cfg.mqttEnabled ? 1 : 0, nvs_get_u8) != 0;
    cfg.mqttBrokerHost = readString(handle, "mqtt_host", cfg.mqttBrokerHost);
    cfg.mqttBrokerPort = static_cast<uint16_t>(readValue<uint32_t>(handle, "mqtt_port", cfg.mqttBrokerPort, nvs_get_u32));
    cfg.mqttUsername = readString(handle, "mqtt_user", cfg.mqttUsername);
    cfg.mqttPassword = readString(handle, "mqtt_pass", cfg.mqttPassword);
    cfg.mqttClientId = readString(handle, "mqtt_cid", cfg.mqttClientId);
    cfg.mqttHeartbeatSec = static_cast<uint16_t>(readValue<uint32_t>(handle, "mqtt_hb", cfg.mqttHeartbeatSec, nvs_get_u32));
    cfg.calibrationFactor = readFloat(handle, "cal_factor", cfg.calibrationFactor);
    cfg.tareOffset = readValue<int32_t>(handle, "tare_offset", static_cast<int32_t>(cfg.tareOffset), nvs_get_i32);
    cfg.cupPresentThresholdGram = readFloat(handle, "cup_thresh", cfg.cupPresentThresholdGram);
    cfg.stableToleranceGram = readFloat(handle, "stable_tol", cfg.stableToleranceGram);
    cfg.stableDurationMs = readValue<uint32_t>(handle, "stable_dur", cfg.stableDurationMs, nvs_get_u32);
    cfg.minDrinkDeltaMl = readFloat(handle, "min_drink", cfg.minDrinkDeltaMl);
    cfg.maxDrinkDeltaMl = readFloat(handle, "max_drink", cfg.maxDrinkDeltaMl);
    cfg.apSsid = readString(handle, "ap_ssid", cfg.apSsid);
    cfg.apPassword = readString(handle, "ap_pass", cfg.apPassword);
    cfg.adminPasswordHash = readString(handle, "admin_hash", cfg.adminPasswordHash);

    nvs_close(handle);
    unlockNvs();
    LOG_INFO(TAG, "configuration loaded");
}

bool ConfigManager::save(const AppConfig& cfg) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    if (!openConfig(handle, NVS_READWRITE)) {
        unlockNvs();
        return false;
    }
    const bool ok = writeConfig(handle, cfg) && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    unlockNvs();
    if (!ok) LOG_ERROR(TAG, "configuration save failed");
    return ok;
}

bool ConfigManager::saveCalibration(float factor, long offset) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    if (!openConfig(handle, NVS_READWRITE)) {
        unlockNvs();
        return false;
    }
    const bool ok = writeFloat(handle, "cal_factor", factor) &&
                    nvs_set_i32(handle, "tare_offset", static_cast<int32_t>(offset)) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    unlockNvs();
    return ok;
}

bool ConfigManager::saveWifi(const std::string& ssid, const std::string& password) {
    if (!lockNvs()) return false;
    nvs_handle_t handle = 0;
    if (!openConfig(handle, NVS_READWRITE)) {
        unlockNvs();
        return false;
    }
    const bool ok = writeString(handle, "wifi_ssid", ssid) &&
                    writeString(handle, "wifi_pass", password) &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    unlockNvs();
    return ok;
}

void ConfigManager::clear() {
    if (!lockNvs()) return;
    nvs_handle_t handle = 0;
    if (openConfig(handle, NVS_READWRITE)) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    unlockNvs();
}

void ConfigManager::_applyDefaults(AppConfig& cfg) {
    cfg = AppConfig{};
    cfg.reminderIntervalMin = DEFAULT_REMINDER_INTERVAL_MIN;
    cfg.reminderAlertTimeoutSec = DEFAULT_REMINDER_ALERT_TIMEOUT_SEC;
    cfg.dailyGoalMl = DEFAULT_DAILY_GOAL_ML;
    cfg.buzzerFrequencyHz = BUZZER_DEFAULT_FREQ_HZ;
    cfg.buzzerDurationMs = BUZZER_DEFAULT_DURATION_MS;
    cfg.buzzerVolumePercent = BUZZER_DEFAULT_VOLUME_PCT;
    cfg.ntpServer1 = DEFAULT_NTP_SERVER1;
    cfg.ntpServer2 = DEFAULT_NTP_SERVER2;
    cfg.timezone = DEFAULT_TIMEZONE;
    cfg.timezoneOffsetSec = DEFAULT_TZ_OFFSET_SEC;
    cfg.daylightOffsetSec = DEFAULT_DST_OFFSET_SEC;
    cfg.mqttBrokerPort = DEFAULT_MQTT_BROKER_PORT;
    cfg.mqttClientId = DEFAULT_MQTT_CLIENT_ID;
    cfg.mqttHeartbeatSec = DEFAULT_MQTT_HEARTBEAT_SEC;
    cfg.cupPresentThresholdGram = DEFAULT_CUP_THRESHOLD_G;
    cfg.stableToleranceGram = DEFAULT_STABLE_TOLERANCE_G;
    cfg.stableDurationMs = DEFAULT_STABLE_DURATION_MS;
    cfg.minDrinkDeltaMl = DEFAULT_MIN_DRINK_ML;
    cfg.maxDrinkDeltaMl = DEFAULT_MAX_DRINK_ML;
    cfg.apSsid = AP_DEFAULT_SSID;
    cfg.apPassword = AP_DEFAULT_PASSWORD;
}
