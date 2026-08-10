#pragma once

#include <cstdint>

enum class ReminderState : uint8_t {
    WAITING_FOR_CUP,
    COUNTING,
    PAUSED_CUP_AWAY,
    ALERTED,
    SNOOZED,
    PAUSED_TODAY,
    DISABLED
};

class ReminderCore {
public:
    void init(uint32_t intervalMs, bool enabled, uint32_t nowMs = 0);
    void update(uint32_t nowMs, bool cupStable);
    void onDrinkConfirmed(uint32_t nowMs, bool cupStable);
    void setEnabled(bool enabled, uint32_t nowMs);
    void setIntervalMs(uint32_t intervalMs, uint32_t nowMs, bool cupStable);
    void snooze(uint32_t durationMs, uint32_t nowMs, bool cupStable);
    void setPausedToday(bool paused, uint32_t nowMs, bool cupStable);

    ReminderState state() const { return _state; }
    uint32_t remainingMs(uint32_t nowMs) const;
    bool consumeAlertStarted();
    static const char* stateName(ReminderState state);

private:
    void _startCountdown(uint32_t durationMs, uint32_t nowMs,
                         bool cupStable, bool snoozing);
    void _advanceCountdown(uint32_t nowMs, bool cupStable);
    void _enterAlerted();

    ReminderState _state = ReminderState::DISABLED;
    uint32_t _intervalMs = 0;
    uint32_t _remainingMs = 0;
    uint32_t _lastUpdateMs = 0;
    bool _enabled = false;
    bool _pausedToday = false;
    bool _snoozing = false;
    bool _alertStarted = false;
};
