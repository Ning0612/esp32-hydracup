#include "ReminderCore.h"

void ReminderCore::init(uint32_t intervalMs, bool enabled, uint32_t nowMs) {
    _intervalMs = intervalMs;
    _enabled = enabled;
    _pausedToday = false;
    _remainingMs = intervalMs;
    _lastUpdateMs = nowMs;
    _snoozing = false;
    _alertStarted = false;
    _state = enabled && intervalMs > 0
        ? ReminderState::WAITING_FOR_CUP
        : ReminderState::DISABLED;
}

void ReminderCore::update(uint32_t nowMs, bool cupStable) {
    if (!_enabled || _intervalMs == 0 || _state == ReminderState::DISABLED ||
        _state == ReminderState::PAUSED_TODAY || _state == ReminderState::ALERTED) {
        return;
    }

    if (_state == ReminderState::WAITING_FOR_CUP) {
        if (cupStable) _startCountdown(_intervalMs, nowMs, true, false);
        return;
    }

    _advanceCountdown(nowMs, cupStable);
}

void ReminderCore::onDrinkConfirmed(uint32_t nowMs, bool cupStable) {
    if (!_enabled || _intervalMs == 0 || _pausedToday) return;
    _startCountdown(_intervalMs, nowMs, cupStable, false);
}

void ReminderCore::setEnabled(bool enabled, uint32_t nowMs) {
    _enabled = enabled;
    _lastUpdateMs = nowMs;
    _alertStarted = false;
    _snoozing = false;
    _remainingMs = _intervalMs;
    if (!enabled || _intervalMs == 0) _state = ReminderState::DISABLED;
    else _state = _pausedToday ? ReminderState::PAUSED_TODAY
                               : ReminderState::WAITING_FOR_CUP;
}

void ReminderCore::setIntervalMs(uint32_t intervalMs, uint32_t nowMs,
                                 bool cupStable) {
    _intervalMs = intervalMs;
    if (!_enabled || intervalMs == 0) {
        _state = ReminderState::DISABLED;
        _remainingMs = 0;
        _alertStarted = false;
        return;
    }
    if (_pausedToday) {
        _state = ReminderState::PAUSED_TODAY;
        _remainingMs = intervalMs;
        _lastUpdateMs = nowMs;
        return;
    }
    _startCountdown(intervalMs, nowMs, cupStable, false);
}

void ReminderCore::snooze(uint32_t durationMs, uint32_t nowMs, bool cupStable) {
    if (!_enabled || _intervalMs == 0 || durationMs == 0 || _pausedToday) return;
    _startCountdown(durationMs, nowMs, cupStable, true);
}

void ReminderCore::setPausedToday(bool paused, uint32_t nowMs, bool cupStable) {
    _pausedToday = paused;
    if (paused) {
        _state = _enabled && _intervalMs > 0 ? ReminderState::PAUSED_TODAY
                                             : ReminderState::DISABLED;
        _alertStarted = false;
        _lastUpdateMs = nowMs;
        return;
    }
    if (_enabled && _intervalMs > 0) _startCountdown(_intervalMs, nowMs, cupStable, false);
}

uint32_t ReminderCore::remainingMs(uint32_t nowMs) const {
    if (_state != ReminderState::COUNTING && _state != ReminderState::SNOOZED) {
        return _state == ReminderState::PAUSED_CUP_AWAY ? _remainingMs : 0;
    }
    const uint32_t elapsed = nowMs - _lastUpdateMs;
    return elapsed >= _remainingMs ? 0 : _remainingMs - elapsed;
}

bool ReminderCore::consumeAlertStarted() {
    const bool result = _alertStarted;
    _alertStarted = false;
    return result;
}

const char* ReminderCore::stateName(ReminderState state) {
    switch (state) {
        case ReminderState::WAITING_FOR_CUP: return "waiting_for_cup";
        case ReminderState::COUNTING: return "counting";
        case ReminderState::PAUSED_CUP_AWAY: return "paused_cup_away";
        case ReminderState::ALERTED: return "alerted";
        case ReminderState::SNOOZED: return "snoozed";
        case ReminderState::PAUSED_TODAY: return "paused_today";
        case ReminderState::DISABLED: return "disabled";
        default: return "unknown";
    }
}

void ReminderCore::_startCountdown(uint32_t durationMs, uint32_t nowMs,
                                   bool cupStable, bool snoozing) {
    _remainingMs = durationMs;
    _lastUpdateMs = nowMs;
    _snoozing = snoozing;
    _alertStarted = false;
    _state = cupStable
        ? (snoozing ? ReminderState::SNOOZED : ReminderState::COUNTING)
        : ReminderState::PAUSED_CUP_AWAY;
}

void ReminderCore::_advanceCountdown(uint32_t nowMs, bool cupStable) {
    if (_state == ReminderState::PAUSED_CUP_AWAY) {
        if (cupStable) {
            _lastUpdateMs = nowMs;
            _state = _snoozing ? ReminderState::SNOOZED : ReminderState::COUNTING;
        }
        return;
    }

    const uint32_t elapsed = nowMs - _lastUpdateMs;
    _lastUpdateMs = nowMs;
    if (!cupStable) {
        _remainingMs = elapsed >= _remainingMs ? 1 : _remainingMs - elapsed;
        _state = ReminderState::PAUSED_CUP_AWAY;
        return;
    }
    if (elapsed >= _remainingMs) {
        _remainingMs = 0;
        _enterAlerted();
        return;
    }
    _remainingMs -= elapsed;
}

void ReminderCore::_enterAlerted() {
    _state = ReminderState::ALERTED;
    _snoozing = false;
    _alertStarted = true;
}
