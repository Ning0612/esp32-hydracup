#pragma once
#include <Arduino.h>
#include "config.h"

enum class AppMode : uint8_t {
    BOOT,
    AP_MODE,
    NORMAL
};

enum class CupState : uint8_t {
    NO_CUP,
    CUP_UNSTABLE,
    CUP_STABLE,
    POSSIBLE_DRINK,
    DRINK_CONFIRMED,
    REFILL_DETECTED
};

struct AppConfig {
    String   wifiSsid;
    String   wifiPassword;

    String   discordWebhookUrl;

    bool     reminderEnabled          = true;
    uint32_t reminderIntervalMin      = 60;
    uint32_t reminderAlertTimeoutSec  = 60;
    uint32_t dailyGoalMl              = 2000;

    bool     buzzerEnabled       = true;
    uint32_t buzzerFrequencyHz   = 2000;
    uint32_t buzzerDurationMs    = 150;
    uint8_t  buzzerVolumePercent = 50;

    bool     ntpEnabled          = true;
    String   ntpServer1          = "pool.ntp.org";
    String   ntpServer2          = "time.google.com";
    String   timezone            = "Asia/Taipei";
    int      timezoneOffsetSec   = 8 * 3600;
    int      daylightOffsetSec   = 0;

    float    calibrationFactor        = 1.0f;
    long     tareOffset               = 0;
    float    cupPresentThresholdGram  = 80.0f;
    float    stableToleranceGram      = 3.0f;
    uint32_t stableDurationMs         = DEFAULT_STABLE_DURATION_MS;
    float    minDrinkDeltaMl          = 20.0f;
    float    maxDrinkDeltaMl          = 500.0f;

    String   apSsid              = "WaterCupTracker-Setup";
    String   apPassword          = "12345678";

    String   adminPasswordHash;
};
