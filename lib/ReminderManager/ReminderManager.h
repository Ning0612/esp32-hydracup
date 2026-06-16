#pragma once
#include <Arduino.h>
#include "BuzzerController.h"

class ReminderManager {
public:
    void init(uint32_t intervalMin, bool enabled);
    void update();
    void resetTimer();
    void setBuzzer(BuzzerController* buz);
    void setEnabled(bool en)          { _enabled = en; }
    void setIntervalMin(uint32_t min) {
        const uint64_t ms = (uint64_t)min * 60000ULL;
        _intervalMs = (ms > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)ms;
    }

    uint32_t getNextReminderSec() const;

private:
    BuzzerController* _buzzer      = nullptr;
    uint32_t          _intervalMs  = 60UL * 60000UL;
    bool              _enabled     = true;
    uint32_t          _lastEventMs = 0;
};
