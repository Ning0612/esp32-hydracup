#include "DrinkDetector.h"
#include "DiscordNotifier.h"
#include "EventLogger.h"
#include "TimeManager.h"
#include "MqttPublisher.h"

static constexpr uint32_t DRINK_LIFT_TIMEOUT_MS = 120000;

void DrinkDetector::init(ScaleManager& scale, AppState& state, const AppConfig& cfg,
                         ReminderManager& reminder, BuzzerController& buzzer) {
    _scale    = &scale;
    _state    = &state;
    _cfg      = &cfg;
    _reminder = &reminder;
    _buzzer   = &buzzer;
    _state->cupState = CupState::NO_CUP;
}

void DrinkDetector::update() {
    if (!_scale || !_state || !_cfg || !_reminder || !_buzzer) return;

    if (!_nvsDone && _time && _time->isSynced()) {
        _nvsRestore();
        _nvsDone = true;
    }

    if (!_scale->isReady() || !_scale->isSamplesReady()) return;

    const float  weight    = _scale->getWeightGrams();
    const float  threshold = _cfg->cupPresentThresholdGram;
    const CupState current = _state->cupState;

    switch (current) {

        case CupState::NO_CUP:
            if (_cupLifted && (millis() - _cupLiftedAtMs > DRINK_LIFT_TIMEOUT_MS)) {
                _cupLifted = false;
            }
            if (weight >= threshold) {
                _transitionTo(CupState::CUP_UNSTABLE);
            }
            break;

        case CupState::CUP_UNSTABLE:
            if (weight < threshold) {
                _transitionTo(CupState::NO_CUP);
            } else if (_scale->isStable()) {
                const float newStable = _scale->getStableWeightGrams();
                if (_cupLifted) {
                    _cupLifted = false;
                    const float prevRef = _prevStableWeight;
                    const float delta   = prevRef - newStable;
                    _prevStableWeight   = newStable;
                    if (delta >= _cfg->minDrinkDeltaMl && delta <= _cfg->maxDrinkDeltaMl) {
                        _onDrinkConfirmed(delta);
                    } else if ((newStable - prevRef) >= _cfg->minDrinkDeltaMl) {
                        _onRefillDetected(newStable - prevRef);
                    } else {
                        _transitionTo(CupState::CUP_STABLE);
                    }
                } else {
                    _prevStableWeight = newStable;
                    _transitionTo(CupState::CUP_STABLE);
                }
            }
            break;

        case CupState::CUP_STABLE:
            if (weight < threshold) {
                _cupLifted     = true;
                _cupLiftedAtMs = millis();
                _transitionTo(CupState::NO_CUP);
            } else if (!_scale->isStable()) {
                _transitionTo(CupState::POSSIBLE_DRINK);
            }
            break;

        case CupState::POSSIBLE_DRINK:
            if (weight < threshold) {
                _cupLifted     = true;
                _cupLiftedAtMs = millis();
                _transitionTo(CupState::NO_CUP);
            } else if (_scale->isStable()) {
                _prevStableWeight = _scale->getStableWeightGrams();
                _transitionTo(CupState::CUP_STABLE);
            }
            break;

        // Transient states — visible for one update() cycle; update reference on exit
        case CupState::DRINK_CONFIRMED:
        case CupState::REFILL_DETECTED:
            _prevStableWeight = _scale->getStableWeightGrams();
            _transitionTo(CupState::CUP_STABLE);
            break;
    }

    // Sync to AppState
    _state->todayTotalMl    = _todayTotalMl;
    _state->lastDrinkMl     = _lastDrinkMl;
    _state->drinkCountToday = _drinkCount;
}

void DrinkDetector::resetDailyCounters() {
    _todayTotalMl = 0.0f;
    _lastDrinkMl  = 0.0f;
    _drinkCount   = 0;
    if (_state) {
        _state->todayTotalMl    = 0.0f;
        _state->lastDrinkMl     = 0.0f;
        _state->drinkCountToday = 0;
    }
    _nvsSave();
    Serial.println("[Drink] Daily counters reset");
}

void DrinkDetector::_transitionTo(CupState next) {
    if (_state->cupState != next) {
        Serial.printf("[CupState] %s → %s\n",
                      _cupStateName(_state->cupState), _cupStateName(next));
        _state->cupState = next;
    }
}

void DrinkDetector::_onDrinkConfirmed(float amountMl) {
    _todayTotalMl += amountMl;
    _lastDrinkMl   = amountMl;
    _drinkCount++;
    _reminder->resetTimer();
    _buzzer->play(BeepPattern::DRINK);
    Serial.printf("[DRINK] %.0f ml  total=%.0f ml  count=%lu\n",
                  amountMl, _todayTotalMl, (unsigned long)_drinkCount);
    _transitionTo(CupState::DRINK_CONFIRMED);

    const String ts = _time ? _time->getISOTimestamp() : String("");
    if (_discord)  _discord->notifyDrink(amountMl, _todayTotalMl, _drinkCount);
    if (_eventLog) _eventLog->logDrink(ts, amountMl, _todayTotalMl, _time);
    if (_mqtt)     _mqtt->publishStatus(_todayTotalMl, "drink");
    _nvsSave();
}

void DrinkDetector::_nvsRestore() {
    const String today = _time->getDateString();

    Preferences prefs;
    prefs.begin("drink_ctr", true);
    const String   period = prefs.getString("period", "");
    const float    total  = prefs.getFloat("total_ml", 0.0f);
    const uint32_t count  = prefs.getUInt("count", 0);
    const float    lastMl = prefs.getFloat("last_ml", 0.0f);
    prefs.end();

    if (period == today) {
        // Same day: restore counters
        _todayTotalMl = total;
        _drinkCount   = count;
        _lastDrinkMl  = lastMl;
        Serial.printf("[Drink] NVS restore: %s total=%.0f count=%u\n",
                      today.c_str(), total, count);
    } else if (!period.isEmpty()) {
        // Previous period: restore into RAM so DailySummaryManager can settle
        // Do NOT overwrite NVS — resetDailyCounters() will write today after settlement
        _todayTotalMl = total;
        _drinkCount   = count;
        _lastDrinkMl  = lastMl;
        Serial.printf("[Drink] NVS prev period %s total=%.0f — pending settlement\n",
                      period.c_str(), total);
    } else {
        // No saved data: fresh start
        Serial.println("[Drink] NVS no data, fresh start");
        _nvsSave();
    }
}

void DrinkDetector::_nvsSave() {
    if (!_time || !_time->isSynced()) return;
    Preferences prefs;
    prefs.begin("drink_ctr", false);
    prefs.putString("period",   _time->getDateString());
    prefs.putFloat("total_ml",  _todayTotalMl);
    prefs.putUInt("count",      _drinkCount);
    prefs.putFloat("last_ml",   _lastDrinkMl);
    prefs.end();
}

void DrinkDetector::_onRefillDetected(float amountMl) {
    Serial.printf("[REFILL] +%.0f ml\n", amountMl);
    _transitionTo(CupState::REFILL_DETECTED);
    if (_mqtt) _mqtt->publishStatus(_todayTotalMl, "refill");
}

const char* DrinkDetector::_cupStateName(CupState s) {
    switch (s) {
        case CupState::NO_CUP:          return "NO_CUP";
        case CupState::CUP_UNSTABLE:    return "CUP_UNSTABLE";
        case CupState::CUP_STABLE:      return "CUP_STABLE";
        case CupState::POSSIBLE_DRINK:  return "POSSIBLE_DRINK";
        case CupState::DRINK_CONFIRMED: return "DRINK_CONFIRMED";
        case CupState::REFILL_DETECTED: return "REFILL_DETECTED";
        default: return "UNKNOWN";
    }
}
