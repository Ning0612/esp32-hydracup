#include "ReminderManager.h"

#include "hal_log.h"
#include "hal_time.h"

void ReminderManager::init(uint32_t intervalMin, bool enabled) {
    const uint64_t milliseconds = static_cast<uint64_t>(intervalMin) * 60000ULL;
    _intervalMs = static_cast<uint32_t>(std::min<uint64_t>(milliseconds, 0xFFFFFFFFULL));
    _enabled = enabled;
    _core.init(_intervalMs, enabled, hal_millis());
}

void ReminderManager::setEnabled(bool en) {
    _enabled = en;
    _core.setEnabled(en, hal_millis());
    _soundActive = false;
    _beepCycleEndMs = 0;
    if (_buzzer) _buzzer->stop();
}

void ReminderManager::setIntervalMin(uint32_t min) {
    const uint64_t milliseconds = static_cast<uint64_t>(min) * 60000ULL;
    _intervalMs = static_cast<uint32_t>(std::min<uint64_t>(milliseconds, 0xFFFFFFFFULL));
    _core.setIntervalMs(_intervalMs, hal_millis(), _cupIsStable());
}

void ReminderManager::setAlertTimeoutSec(uint32_t sec) {
    _alertTimeoutMs = static_cast<uint32_t>(std::min<uint64_t>(
        static_cast<uint64_t>(sec) * 1000ULL, 0xFFFFFFFFULL));
}

void ReminderManager::update() {
    const uint32_t now = hal_millis();
    const bool cupStable = _cupIsStable();
    _core.update(now, cupStable);
    if (_core.consumeAlertStarted()) {
        _soundActive = true;
        _alertStartMs = now;
        _beepCycleEndMs = 0;
        LOG_INFO("Reminder", "time to drink water");
        if (_buzzer) _buzzer->play(BeepPattern::REMINDER);
    }

    if (_soundActive) {
        const bool timedOut = now - _alertStartMs >= _alertTimeoutMs;
        if (!cupStable || timedOut) {
            _soundActive = false;
            _beepCycleEndMs = 0;
            if (_buzzer) _buzzer->stop();
            LOG_INFO("Reminder", "sound stopped (%s)", !cupStable ? "cup away" : "timeout");
            return;
        }
        if (_buzzer && !_buzzer->isPlaying()) {
            if (_beepCycleEndMs == 0) _beepCycleEndMs = now;
            else if (now - _beepCycleEndMs >= BEEP_CYCLE_GAP_MS) {
                _beepCycleEndMs = 0;
                _buzzer->play(BeepPattern::REMINDER);
            }
        }
    }
}

void ReminderManager::onDrinkConfirmed() {
    if (_buzzer) _buzzer->stop();
    _core.onDrinkConfirmed(hal_millis(), _cupIsStable());
    _soundActive = false;
    _beepCycleEndMs = 0;
}

void ReminderManager::snooze(uint32_t minutes) {
    const uint64_t milliseconds = static_cast<uint64_t>(minutes) * 60000ULL;
    _core.snooze(static_cast<uint32_t>(std::min<uint64_t>(milliseconds, 0xFFFFFFFFULL)),
                 hal_millis(), _cupIsStable());
    _soundActive = false;
    _beepCycleEndMs = 0;
    if (_buzzer) _buzzer->stop();
}

void ReminderManager::setPausedToday(bool paused) {
    _core.setPausedToday(paused, hal_millis(), _cupIsStable());
    _soundActive = false;
    _beepCycleEndMs = 0;
    if (_buzzer) _buzzer->stop();
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
    const uint32_t remaining = _core.remainingMs(hal_millis());
    return remaining == 0 ? 0 : (remaining + 999) / 1000;
}
