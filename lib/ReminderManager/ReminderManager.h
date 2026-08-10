#pragma once

#include <algorithm>
#include <cstdint>

#include "AppState.h"
#include "BuzzerController.h"
#include "ReminderCore.h"

class ReminderManager {
public:
    void init(uint32_t intervalMin, bool enabled);
    void update();
    void onDrinkConfirmed();
    void setBuzzer(BuzzerController* buz);
    void setAppState(AppState* state) { _appState = state; }
    void setEnabled(bool en);
    void setIntervalMin(uint32_t min);
    void setAlertTimeoutSec(uint32_t sec);
    void snooze(uint32_t minutes);
    void setPausedToday(bool paused);
    uint32_t getNextReminderSec() const;
    ReminderState getState() const { return _core.state(); }
    const char* getStateName() const { return ReminderCore::stateName(_core.state()); }
    uint32_t getAlertEpisodeUptimeMs() const { return _alertStartMs; }
    static constexpr uint32_t BEEP_CYCLE_GAP_MS = 800;

private:
    bool _cupIsStable() const;
    BuzzerController* _buzzer = nullptr;
    AppState* _appState = nullptr;
    ReminderCore _core;
    uint32_t _intervalMs = 60UL * 60000UL;
    uint32_t _alertTimeoutMs = 60000UL;
    bool _enabled = true;
    bool _soundActive = false;
    uint32_t _alertStartMs = 0;
    uint32_t _beepCycleEndMs = 0;
};
