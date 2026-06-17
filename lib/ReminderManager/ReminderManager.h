#pragma once
#include <Arduino.h>
#include "AppState.h"
#include "BuzzerController.h"

class ReminderManager {
public:
    void init(uint32_t intervalMin, bool enabled);
    void update();
    void resetTimer();
    void setBuzzer(BuzzerController* buz);
    void setAppState(AppState* state)  { _appState = state; }
    void setEnabled(bool en) {
        _enabled = en;
        if (!en) {
            _alerting         = false;
            _overdueWhileAway = false;
            if (_buzzer) _buzzer->stop();
        }
    }
    void setIntervalMin(uint32_t min) {
        const uint64_t ms = (uint64_t)min * 60000ULL;
        _intervalMs = (ms > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)ms;
    }

    uint32_t getNextReminderSec() const;

private:
    static constexpr uint32_t ALERT_TIMEOUT_MS = 30000;

    BuzzerController* _buzzer       = nullptr;
    AppState*         _appState     = nullptr;
    uint32_t          _intervalMs   = 60UL * 60000UL;
    bool              _enabled      = true;
    uint32_t          _lastEventMs  = 0;
    bool              _alerting         = false;
    uint32_t          _alertStartMs     = 0;
    bool              _overdueWhileAway = false;

    bool _cupIsStable() const;
};
