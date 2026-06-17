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
        const bool timedOut  = (millis() - _alertStartMs >= ALERT_TIMEOUT_MS);

        if (cupLifted || timedOut) {
            _alerting    = false;
            _lastEventMs = millis();
            if (_buzzer) _buzzer->stop();
            Serial.printf("[Reminder] Alert stopped (%s)\n", cupLifted ? "cup lifted" : "timeout");
            return;
        }

        if (_buzzer && !_buzzer->isPlaying()) {
            _buzzer->play(BeepPattern::REMINDER);
        }
        return;
    }

    if (millis() - _lastEventMs >= _intervalMs) {
        _alerting     = true;
        _alertStartMs = millis();
        Serial.println("[Reminder] Time to drink water!");
        if (_buzzer) _buzzer->play(BeepPattern::REMINDER);
    }
}

void ReminderManager::resetTimer() {
    _lastEventMs = millis();
    _alerting    = false;
}

void ReminderManager::setBuzzer(BuzzerController* buz) {
    _buzzer = buz;
}

uint32_t ReminderManager::getNextReminderSec() const {
    if (!_enabled || _intervalMs == 0) return 0;
    if (_alerting) return 0;
    const uint32_t elapsed = millis() - _lastEventMs;
    if (elapsed >= _intervalMs) return 0;
    return (_intervalMs - elapsed) / 1000;
}
