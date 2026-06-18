#include "ConfigManager.h"
#include "config.h"

void ConfigManager::load(AppConfig& cfg) {
    _applyDefaults(cfg);
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
}

void ConfigManager::save(const AppConfig& cfg) {
    _prefs.begin(NVS_NAMESPACE, false);  // read-write

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

    _prefs.putFloat ("cal_factor",   cfg.calibrationFactor);
    _prefs.putLong  ("tare_offset",  cfg.tareOffset);
    _prefs.putFloat ("cup_thresh",   cfg.cupPresentThresholdGram);
    _prefs.putFloat ("stable_tol",   cfg.stableToleranceGram);
    _prefs.putUInt  ("stable_dur",   cfg.stableDurationMs);
    _prefs.putFloat ("min_drink",    cfg.minDrinkDeltaMl);
    _prefs.putFloat ("max_drink",    cfg.maxDrinkDeltaMl);

    _prefs.putString("ap_ssid",      cfg.apSsid);
    _prefs.putString("ap_pass",      cfg.apPassword);
    _prefs.putString("admin_hash",   cfg.adminPasswordHash);

    _prefs.end();
}

void ConfigManager::saveCalibration(float factor, long offset) {
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.putFloat("cal_factor",  factor);
    _prefs.putLong ("tare_offset", offset);
    _prefs.end();
}

void ConfigManager::saveWifi(const String& ssid, const String& password) {
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.putString("wifi_ssid", ssid);
    _prefs.putString("wifi_pass", password);
    _prefs.end();
}

void ConfigManager::clear() {
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.clear();
    _prefs.end();
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
