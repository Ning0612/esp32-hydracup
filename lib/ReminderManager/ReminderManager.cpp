#include "ReminderManager.h"

#include "hal_log.h"
#include "hal_time.h"

void ReminderManager::init(uint32_t intervalMin, bool enabled) {
    setIntervalMin(intervalMin);
    _enabled = enabled;
    _lastEventMs = hal_millis();
}

void ReminderManager::setEnabled(bool en) {
    _enabled = en;
    if (!en) {
        _alerting = false;
        _beepCycleEndMs = 0;
        _overdueWhileAway = false;
        if (_buzzer) _buzzer->stop();
    }
}

void ReminderManager::setIntervalMin(uint32_t min) {
    const uint64_t milliseconds = static_cast<uint64_t>(min) * 60000ULL;
    _intervalMs = static_cast<uint32_t>(std::min<uint64_t>(milliseconds, 0xFFFFFFFFULL));
}

void ReminderManager::setAlertTimeoutSec(uint32_t sec) {
    _alertTimeoutMs = static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(sec) * 1000ULL, 0xFFFFFFFFULL));
}

void ReminderManager::update() {
    if (!_enabled || _intervalMs == 0) return;
    const uint32_t now = hal_millis();
    if (_alerting) {
        const bool cupLifted = _appState && _appState->cupState == CupState::NO_CUP;
        const bool timedOut = now - _alertStartMs >= _alertTimeoutMs;
        if (cupLifted || timedOut) {
            _alerting = false;
            _beepCycleEndMs = 0;
            _lastEventMs = now;
            if (_buzzer) _buzzer->stop();
            LOG_INFO("Reminder", "alert stopped (%s)", cupLifted ? "cup lifted" : "timeout");
            return;
        }
        if (_buzzer && !_buzzer->isPlaying()) {
            if (_beepCycleEndMs == 0) _beepCycleEndMs = now;
            else if (now - _beepCycleEndMs >= BEEP_CYCLE_GAP_MS) {
                _beepCycleEndMs = 0;
                _buzzer->play(BeepPattern::REMINDER);
            }
        }
        return;
    }
    if (_overdueWhileAway) {
        if (_cupIsStable()) {
            _overdueWhileAway = false;
            _alerting = true;
            _alertStartMs = now;
            LOG_INFO("Reminder", "time to drink water (overdue)");
            if (_buzzer) _buzzer->play(BeepPattern::REMINDER);
        }
        return;
    }
    if (now - _lastEventMs >= _intervalMs) {
        if (!_cupIsStable()) {
            _overdueWhileAway = true;
            return;
        }
        _alerting = true;
        _alertStartMs = now;
        LOG_INFO("Reminder", "time to drink water");
        if (_buzzer) _buzzer->play(BeepPattern::REMINDER);
    }
}

void ReminderManager::resetTimer() {
    if (_alerting && _buzzer) _buzzer->stop();
    _lastEventMs = hal_millis();
    _alerting = false;
    _beepCycleEndMs = 0;
    _overdueWhileAway = false;
}

void ReminderManager::setBuzzer(BuzzerController* buz) { _buzzer = buz; }

bool ReminderManager::_cupIsStable() const {
    if (!_appState) {
        LOG_WARN("Reminder", "app state not set");
        return true;
    }
    return _appState->cupState == CupState::CUP_STABLE;
}

uint32_t ReminderManager::getNextReminderSec() const {
    if (!_enabled || _intervalMs == 0 || _alerting || _overdueWhileAway) return 0;
    const uint32_t elapsed = hal_millis() - _lastEventMs;
    if (elapsed >= _intervalMs) return 0;
    return (_intervalMs - elapsed) / 1000;
}
