#pragma once

#include <algorithm>
#include <cstdint>

#include "AppState.h"
#include "BuzzerController.h"

class ReminderManager {
public:
    void init(uint32_t intervalMin, bool enabled);
    void update();
    void resetTimer();
    void setBuzzer(BuzzerController* buz);
    void setAppState(AppState* state) { _appState = state; }
    void setEnabled(bool en);
    void setIntervalMin(uint32_t min);
    void setAlertTimeoutSec(uint32_t sec);
    uint32_t getNextReminderSec() const;
    static constexpr uint32_t BEEP_CYCLE_GAP_MS = 800;

private:
    bool _cupIsStable() const;
    BuzzerController* _buzzer = nullptr;
    AppState* _appState = nullptr;
    uint32_t _intervalMs = 60UL * 60000UL;
    uint32_t _alertTimeoutMs = 60000UL;
    bool _enabled = true;
    uint32_t _lastEventMs = 0;
    bool _alerting = false;
    uint32_t _alertStartMs = 0;
    uint32_t _beepCycleEndMs = 0;
    bool _overdueWhileAway = false;
};
