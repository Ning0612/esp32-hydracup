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
    std::atomic<bool> wifiConnected{false};
    bool     ntpSynced     = false;

    float    weightGrams   = 0.0f;
    CupState cupState      = CupState::NO_CUP;

    float    todayTotalMl    = 0.0f;
    float    lastDrinkMl     = 0.0f;
    uint32_t drinkCountToday = 0;
    uint32_t nextReminderSec = 0;
    char     lastDrinkAt[32] = {};

    bool               webhookConfigured = false;
    std::atomic<bool>  webhookLastOk{false};

    bool               mqttConfigured = false;
    std::atomic<bool>  mqttConnected{false};

    // Cross-module shed signal: writing the app partition disables the flash cache in
    // bursts, so timing-sensitive and memory-hungry work stands down while it runs.
    std::atomic<bool> otaInProgress{false};

    std::string ipAddress;
};
