#include "ReminderManager.h"

void ReminderManager::init(uint32_t intervalMin, bool enabled) {
    const uint64_t ms = (uint64_t)intervalMin * 60000ULL;
    _intervalMs  = (ms > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)ms;
    _enabled     = enabled;
    _lastEventMs = millis();
}

void ReminderManager::update() {
    if (!_enabled || _intervalMs == 0) return;

    if (_alerting) {
        const bool cupLifted = _appState && (_appState->cupState == CupState::NO_CUP);
        const bool timedOut  = (millis() - _alertStartMs >= _alertTimeoutMs);

        if (cupLifted || timedOut) {
            _alerting    = false;
            _lastEventMs = millis();
            if (_buzzer) _buzzer->stop();
            Serial.printf("[Reminder] Alert stopped (%s)\n",
                          cupLifted ? "cup lifted" : "timeout");
            return;
        }

        // Keep looping the beep pattern until alert ends
        if (_buzzer && !_buzzer->isPlaying()) {
            _buzzer->play(BeepPattern::REMINDER);
        }
        return;
    }

    // Overdue: interval fired while cup was away; alert as soon as cup is stable
    if (_overdueWhileAway) {
        if (_cupIsStable()) {
            _overdueWhileAway = false;
            _alerting         = true;
            _alertStartMs     = millis();
            Serial.println("[Reminder] Time to drink water! (overdue)");
            if (_buzzer) _buzzer->play(BeepPattern::REMINDER);
        }
        return;
    }

    if (millis() - _lastEventMs >= _intervalMs) {
        if (!_cupIsStable()) {
            _overdueWhileAway = true;
            return;
        }
        _alerting     = true;
        _alertStartMs = millis();
        Serial.println("[Reminder] Time to drink water!");
        if (_buzzer) _buzzer->play(BeepPattern::REMINDER);
    }
}

void ReminderManager::resetTimer() {
    if (_alerting && _buzzer) _buzzer->stop();
    _lastEventMs      = millis();
    _alerting         = false;
    _overdueWhileAway = false;
}

void ReminderManager::setBuzzer(BuzzerController* buz) {
    _buzzer = buz;
}

bool ReminderManager::_cupIsStable() const {
    if (!_appState) {
        Serial.println("[Reminder][WARN] _appState not set");
        return true;  // fail-open: assume cup present
    }
    return _appState->cupState == CupState::CUP_STABLE;
}

uint32_t ReminderManager::getNextReminderSec() const {
    if (!_enabled || _intervalMs == 0) return 0;
    if (_alerting || _overdueWhileAway) return 0;
    const uint32_t elapsed = millis() - _lastEventMs;
    if (elapsed >= _intervalMs) return 0;
    return (_intervalMs - elapsed) / 1000;
}
