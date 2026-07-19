#pragma once
#include <atomic>
#include <string>
#include "app_types.h"

struct AppState {
    AppMode  mode          = AppMode::BOOT;
    bool     fsOk          = false;
    bool     logFsOk       = false;
    bool     oledOk        = false;
    bool     hx711Ok       = false;
    bool     buzzerOk      = false;
    bool     wifiConnected = false;
    bool     ntpSynced     = false;

    float    weightGrams   = 0.0f;
    CupState cupState      = CupState::NO_CUP;

    float    todayTotalMl    = 0.0f;
    float    lastDrinkMl     = 0.0f;
    uint32_t drinkCountToday = 0;
    uint32_t nextReminderSec = 0;

    bool               webhookConfigured = false;
    std::atomic<bool>  webhookLastOk{false};

    bool               mqttConfigured = false;
    std::atomic<bool>  mqttConnected{false};

    std::string ipAddress;
};
