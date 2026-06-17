#include "DrinkDetector.h"
#include "DiscordNotifier.h"
#include "EventLogger.h"
#include "TimeManager.h"

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
                const float currentStable = _scale->getStableWeightGrams();
                const float delta         = _prevStableWeight - currentStable;

                if (delta >= _cfg->minDrinkDeltaMl && delta <= _cfg->maxDrinkDeltaMl) {
                    _onDrinkConfirmed(delta);
                    // stay in DRINK_CONFIRMED this cycle so OLED/API/EventLogger can observe it
                } else if ((currentStable - _prevStableWeight) >= _cfg->minDrinkDeltaMl) {
                    _onRefillDetected(currentStable - _prevStableWeight);
                    // stay in REFILL_DETECTED this cycle
                } else {
                    // noise or out-of-range: update reference and return to stable immediately
                    _prevStableWeight = currentStable;
                    _transitionTo(CupState::CUP_STABLE);
                }
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
}

void DrinkDetector::_onRefillDetected(float amountMl) {
    Serial.printf("[REFILL] +%.0f ml\n", amountMl);
    _transitionTo(CupState::REFILL_DETECTED);
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
