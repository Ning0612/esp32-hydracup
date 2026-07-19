#pragma once
#include <cstdint>
#include <string>
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
    std::string wifiSsid;
    std::string wifiPassword;

    std::string discordWebhookUrl;

    bool     reminderEnabled          = true;
    uint32_t reminderIntervalMin      = 60;
    uint32_t reminderAlertTimeoutSec  = 60;
    uint32_t dailyGoalMl              = 2000;

    bool     buzzerEnabled       = true;
    uint32_t buzzerFrequencyHz   = 2000;
    uint32_t buzzerDurationMs    = 150;
    uint8_t  buzzerVolumePercent = 50;

    bool     ntpEnabled          = true;
    std::string ntpServer1       = "pool.ntp.org";
    std::string ntpServer2       = "time.google.com";
    std::string timezone         = "Asia/Taipei";
    int      timezoneOffsetSec   = 8 * 3600;
    int      daylightOffsetSec   = 0;

    bool     mqttEnabled         = false;
    std::string mqttBrokerHost;
    uint16_t mqttBrokerPort      = DEFAULT_MQTT_BROKER_PORT;
    std::string mqttUsername;
    std::string mqttPassword;
    std::string mqttClientId     = DEFAULT_MQTT_CLIENT_ID;
    uint16_t mqttHeartbeatSec    = DEFAULT_MQTT_HEARTBEAT_SEC;

    float    calibrationFactor        = 1.0f;
    long     tareOffset               = 0;
    float    cupPresentThresholdGram  = 80.0f;
    float    stableToleranceGram      = 3.0f;
    uint32_t stableDurationMs         = DEFAULT_STABLE_DURATION_MS;
    float    minDrinkDeltaMl          = 20.0f;
    float    maxDrinkDeltaMl          = 500.0f;

    std::string apSsid           = "WaterCupTracker-Setup";
    std::string apPassword       = "12345678";

    std::string adminPasswordHash;
};
