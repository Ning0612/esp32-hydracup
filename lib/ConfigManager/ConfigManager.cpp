#include "ConfigManager.h"
#include "config.h"
#include "StorageLock.h"

void ConfigManager::load(AppConfig& cfg) {
    _applyDefaults(cfg);
    if (!lockNvs()) return;
    _prefs.begin(NVS_NAMESPACE, true);  // read-only

    cfg.wifiSsid             = _prefs.getString("wifi_ssid",     "");
    cfg.wifiPassword         = _prefs.getString("wifi_pass",     "");
    cfg.discordWebhookUrl    = _prefs.getString("webhook_url",   "");

    cfg.reminderEnabled          = _prefs.getBool("rem_en",        cfg.reminderEnabled);
    cfg.reminderIntervalMin      = _prefs.getUInt("rem_interval",  cfg.reminderIntervalMin);
    cfg.reminderAlertTimeoutSec  = _prefs.getUInt("rem_alert_to",  cfg.reminderAlertTimeoutSec);
    cfg.dailyGoalMl              = _prefs.getUInt("daily_goal",    cfg.dailyGoalMl);

    cfg.buzzerEnabled        = _prefs.getBool  ("buz_en",        cfg.buzzerEnabled);
    cfg.buzzerFrequencyHz    = _prefs.getUInt  ("buz_freq",      cfg.buzzerFrequencyHz);
    cfg.buzzerDurationMs     = _prefs.getUInt  ("buz_dur",       cfg.buzzerDurationMs);
    cfg.buzzerVolumePercent  = (uint8_t)_prefs.getUInt("buz_vol",cfg.buzzerVolumePercent);

    cfg.ntpEnabled           = _prefs.getBool  ("ntp_en",        cfg.ntpEnabled);
    cfg.ntpServer1           = _prefs.getString("ntp_srv1",      cfg.ntpServer1);
    cfg.ntpServer2           = _prefs.getString("ntp_srv2",      cfg.ntpServer2);
    cfg.timezone             = _prefs.getString("tz",            cfg.timezone);
    cfg.timezoneOffsetSec    = _prefs.getInt   ("tz_off",        cfg.timezoneOffsetSec);
    cfg.daylightOffsetSec    = _prefs.getInt   ("dst_off",       cfg.daylightOffsetSec);

    cfg.mqttEnabled          = _prefs.getBool  ("mqtt_en",       cfg.mqttEnabled);
    cfg.mqttBrokerHost       = _prefs.getString("mqtt_host",     cfg.mqttBrokerHost);
    cfg.mqttBrokerPort       = (uint16_t)_prefs.getUInt("mqtt_port", cfg.mqttBrokerPort);
    cfg.mqttUsername         = _prefs.getString("mqtt_user",     cfg.mqttUsername);
    cfg.mqttPassword         = _prefs.getString("mqtt_pass",     cfg.mqttPassword);
    cfg.mqttClientId         = _prefs.getString("mqtt_cid",      cfg.mqttClientId);
    cfg.mqttHeartbeatSec     = (uint16_t)_prefs.getUInt("mqtt_hb", cfg.mqttHeartbeatSec);

    cfg.calibrationFactor    = _prefs.getFloat ("cal_factor",    cfg.calibrationFactor);
    cfg.tareOffset           = _prefs.getLong  ("tare_offset",   cfg.tareOffset);
    cfg.cupPresentThresholdGram = _prefs.getFloat("cup_thresh",  cfg.cupPresentThresholdGram);
    cfg.stableToleranceGram  = _prefs.getFloat ("stable_tol",    cfg.stableToleranceGram);
    cfg.stableDurationMs     = _prefs.getUInt  ("stable_dur",    cfg.stableDurationMs);
    cfg.minDrinkDeltaMl      = _prefs.getFloat ("min_drink",     cfg.minDrinkDeltaMl);
    cfg.maxDrinkDeltaMl      = _prefs.getFloat ("max_drink",     cfg.maxDrinkDeltaMl);

    cfg.apSsid               = _prefs.getString("ap_ssid",       cfg.apSsid);
    cfg.apPassword           = _prefs.getString("ap_pass",       cfg.apPassword);
    cfg.adminPasswordHash    = _prefs.getString("admin_hash",    "");

    _prefs.end();
    unlockNvs();
}

bool ConfigManager::save(const AppConfig& cfg) {
    if (!lockNvs()) return false;
    if (!_prefs.begin(NVS_NAMESPACE, false)) {  // read-write
        unlockNvs();
        return false;
    }

    _prefs.putString("wifi_ssid",    cfg.wifiSsid);
    _prefs.putString("wifi_pass",    cfg.wifiPassword);
    _prefs.putString("webhook_url",  cfg.discordWebhookUrl);

    _prefs.putBool("rem_en",       cfg.reminderEnabled);
    _prefs.putUInt("rem_interval", cfg.reminderIntervalMin);
    _prefs.putUInt("rem_alert_to", cfg.reminderAlertTimeoutSec);
    _prefs.putUInt("daily_goal",   cfg.dailyGoalMl);

    _prefs.putBool  ("buz_en",       cfg.buzzerEnabled);
    _prefs.putUInt  ("buz_freq",     cfg.buzzerFrequencyHz);
    _prefs.putUInt  ("buz_dur",      cfg.buzzerDurationMs);
    _prefs.putUInt  ("buz_vol",      cfg.buzzerVolumePercent);

    _prefs.putBool  ("ntp_en",       cfg.ntpEnabled);
    _prefs.putString("ntp_srv1",     cfg.ntpServer1);
    _prefs.putString("ntp_srv2",     cfg.ntpServer2);
    _prefs.putString("tz",           cfg.timezone);
    _prefs.putInt   ("tz_off",       cfg.timezoneOffsetSec);
    _prefs.putInt   ("dst_off",      cfg.daylightOffsetSec);

    _prefs.putBool  ("mqtt_en",      cfg.mqttEnabled);
    _prefs.putString("mqtt_host",    cfg.mqttBrokerHost);
    _prefs.putUInt  ("mqtt_port",    cfg.mqttBrokerPort);
    _prefs.putString("mqtt_user",    cfg.mqttUsername);
    _prefs.putString("mqtt_pass",    cfg.mqttPassword);
    _prefs.putString("mqtt_cid",     cfg.mqttClientId);
    _prefs.putUInt  ("mqtt_hb",      cfg.mqttHeartbeatSec);

    _prefs.putFloat ("cal_factor",   cfg.calibrationFactor);
    _prefs.putLong  ("tare_offset",  cfg.tareOffset);
    _prefs.putFloat ("cup_thresh",   cfg.cupPresentThresholdGram);
    _prefs.putFloat ("stable_tol",   cfg.stableToleranceGram);
    _prefs.putUInt  ("stable_dur",   cfg.stableDurationMs);
    _prefs.putFloat ("min_drink",    cfg.minDrinkDeltaMl);
    _prefs.putFloat ("max_drink",    cfg.maxDrinkDeltaMl);

    _prefs.putString("ap_ssid",      cfg.apSsid);
    _prefs.putString("ap_pass",      cfg.apPassword);
    const size_t adminHashBytes = _prefs.putString("admin_hash", cfg.adminPasswordHash);

    _prefs.end();
    unlockNvs();
    return adminHashBytes == cfg.adminPasswordHash.length();
}

bool ConfigManager::saveCalibration(float factor, long offset) {
    if (!lockNvs()) return false;
    if (!_prefs.begin(NVS_NAMESPACE, false)) {
        unlockNvs();
        return false;
    }
    const bool ok = _prefs.putFloat("cal_factor", factor) == sizeof(float) &&
                    _prefs.putLong("tare_offset", offset) == sizeof(int32_t);
    _prefs.end();
    unlockNvs();
    return ok;
}

void ConfigManager::saveWifi(const String& ssid, const String& password) {
    if (!lockNvs()) return;
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.putString("wifi_ssid", ssid);
    _prefs.putString("wifi_pass", password);
    _prefs.end();
    unlockNvs();
}

void ConfigManager::clear() {
    if (!lockNvs()) return;
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.clear();
    _prefs.end();
    unlockNvs();
}

void ConfigManager::_applyDefaults(AppConfig& cfg) {
    cfg.reminderEnabled         = true;
    cfg.reminderIntervalMin     = DEFAULT_REMINDER_INTERVAL_MIN;
    cfg.reminderAlertTimeoutSec = DEFAULT_REMINDER_ALERT_TIMEOUT_SEC;
    cfg.dailyGoalMl             = DEFAULT_DAILY_GOAL_ML;
    cfg.buzzerEnabled        = true;
    cfg.buzzerFrequencyHz    = BUZZER_DEFAULT_FREQ_HZ;
    cfg.buzzerDurationMs     = BUZZER_DEFAULT_DURATION_MS;
    cfg.buzzerVolumePercent  = BUZZER_DEFAULT_VOLUME_PCT;
    cfg.ntpEnabled           = true;
    cfg.ntpServer1           = DEFAULT_NTP_SERVER1;
    cfg.ntpServer2           = DEFAULT_NTP_SERVER2;
    cfg.timezone             = DEFAULT_TIMEZONE;
    cfg.timezoneOffsetSec    = DEFAULT_TZ_OFFSET_SEC;
    cfg.daylightOffsetSec    = DEFAULT_DST_OFFSET_SEC;
    cfg.mqttEnabled          = false;
    cfg.mqttBrokerHost       = "";
    cfg.mqttBrokerPort       = DEFAULT_MQTT_BROKER_PORT;
    cfg.mqttUsername         = "";
    cfg.mqttPassword         = "";
    cfg.mqttClientId         = DEFAULT_MQTT_CLIENT_ID;
    cfg.mqttHeartbeatSec     = DEFAULT_MQTT_HEARTBEAT_SEC;
    cfg.calibrationFactor    = 1.0f;
    cfg.tareOffset           = 0;
    cfg.cupPresentThresholdGram = DEFAULT_CUP_THRESHOLD_G;
    cfg.stableToleranceGram  = DEFAULT_STABLE_TOLERANCE_G;
    cfg.stableDurationMs     = DEFAULT_STABLE_DURATION_MS;
    cfg.minDrinkDeltaMl      = DEFAULT_MIN_DRINK_ML;
    cfg.maxDrinkDeltaMl      = DEFAULT_MAX_DRINK_ML;
    cfg.apSsid               = AP_DEFAULT_SSID;
    cfg.apPassword           = AP_DEFAULT_PASSWORD;
}
