#include "DrinkDetectorCore.h"
#include <cstring>

void DrinkDetectorEventHandler::init(DrinkCounterPersistence* persistence,
                                     DrinkDetectorEffects* effects) {
    _persistence = persistence;
    _effects = effects;
    _todayTotalMl = 0.0f;
    _lastDrinkMl = 0.0f;
    _drinkCount = 0;
    _restoredPeriod[0] = '\0';
}

DrinkCounterLoadStatus DrinkDetectorEventHandler::restore(const char* currentPeriod) {
    DrinkCounterSnapshot snapshot;
    const DrinkCounterLoadStatus status =
        _persistence ? _persistence->load(currentPeriod, snapshot)
                     : DrinkCounterLoadStatus::EMPTY;

    return applyRestore(status, snapshot, currentPeriod);
}

DrinkCounterLoadStatus DrinkDetectorEventHandler::applyRestore(
        DrinkCounterLoadStatus status, const DrinkCounterSnapshot& snapshot,
        const char* currentPeriod) {

    if (status == DrinkCounterLoadStatus::LOAD_FAILED) return status;

    std::strncpy(_restoredPeriod, snapshot.period, sizeof(_restoredPeriod) - 1);
    _restoredPeriod[sizeof(_restoredPeriod) - 1] = '\0';
    if (status == DrinkCounterLoadStatus::CURRENT_PERIOD ||
        status == DrinkCounterLoadStatus::PREVIOUS_PERIOD) {
        _todayTotalMl = snapshot.totalMl;
        _lastDrinkMl = snapshot.lastDrinkMl;
        _drinkCount = snapshot.drinkCount;
    } else {
        _todayTotalMl = 0.0f;
        _lastDrinkMl = 0.0f;
        _drinkCount = 0;
        _save(currentPeriod);
    }
    return status;
}

void DrinkDetectorEventHandler::save(const char* currentPeriod) {
    _save(currentPeriod);
}

void DrinkDetectorEventHandler::onDrinkConfirmed(float amountMl,
                                                 const char* period,
                                                 const char* timestamp) {
    _todayTotalMl += amountMl;
    _lastDrinkMl = amountMl;
    _drinkCount++;
    if (_effects) {
        _effects->resetReminder();
        _effects->playDrinkBeep();
        _effects->notifyDrink(amountMl, _todayTotalMl, _drinkCount);
        _effects->logDrink(timestamp, amountMl, _todayTotalMl);
        _effects->publishStatus(_todayTotalMl, "drink");
    }
    _save(period);
}

void DrinkDetectorEventHandler::onRefillDetected(float amountMl) {
    if (_effects) _effects->publishStatus(_todayTotalMl, "refill");
    (void)amountMl;
}

void DrinkDetectorEventHandler::resetDailyCounters(const char* period) {
    _todayTotalMl = 0.0f;
    _lastDrinkMl = 0.0f;
    _drinkCount = 0;
    if (period) _save(period);
}

DrinkCounterSnapshot DrinkDetectorEventHandler::snapshot(const char* period) const {
    DrinkCounterSnapshot result;
    if (period) {
        std::strncpy(result.period, period, sizeof(result.period) - 1);
        result.period[sizeof(result.period) - 1] = '\0';
    }
    result.totalMl = _todayTotalMl;
    result.lastDrinkMl = _lastDrinkMl;
    result.drinkCount = _drinkCount;
    return result;
}

void DrinkDetectorEventHandler::mergeSnapshot(const DrinkCounterSnapshot& incoming,
                                              const char* period) {
    if (incoming.drinkCount == 0 && incoming.totalMl == 0.0f) return;
    _todayTotalMl += incoming.totalMl;
    _drinkCount += incoming.drinkCount;
    if (incoming.lastDrinkMl > 0.0f) _lastDrinkMl = incoming.lastDrinkMl;
    _save(period);
}

void DrinkDetectorEventHandler::_save(const char* period) {
    if (!_persistence || !period || period[0] == '\0') return;
    DrinkCounterSnapshot snapshot;
    snapshot.totalMl = _todayTotalMl;
    snapshot.lastDrinkMl = _lastDrinkMl;
    snapshot.drinkCount = _drinkCount;
    _persistence->save(period, snapshot);
}

void DrinkDetectorCore::init(const DrinkDetectorConfig& config,
                             DrinkDetectorEventSink* sink) {
    _config = config;
    _sink = sink;
    _state = DrinkDetectorState::NO_CUP;
    _prevStableWeight = 0.0f;
    _cupLifted = false;
    _cupLiftedAtMs = 0;
}

void DrinkDetectorCore::update(const DrinkDetectorObservation& observation,
                               uint32_t nowMs) {
    const float weight = observation.weightGrams;
    const float threshold = _config.cupPresentThresholdGram;

    switch (_state) {
        case DrinkDetectorState::NO_CUP:
            if (_cupLifted && (nowMs - _cupLiftedAtMs > _config.liftTimeoutMs)) {
                _cupLifted = false;
            }
            if (weight >= threshold) {
                _transitionTo(DrinkDetectorState::CUP_UNSTABLE);
            }
            break;

        case DrinkDetectorState::CUP_UNSTABLE:
            if (weight < threshold) {
                _transitionTo(DrinkDetectorState::NO_CUP);
            } else if (observation.scaleStable) {
                const float newStable = observation.stableWeightGrams;
                if (_cupLifted) {
                    _cupLifted = false;
                    const float prevRef = _prevStableWeight;
                    const float delta = prevRef - newStable;
                    _prevStableWeight = newStable;

                    if (delta >= _config.minDrinkDeltaMl &&
                        delta <= _config.maxDrinkDeltaMl) {
                        if (_sink) _sink->onDrinkConfirmed(delta);
                        _transitionTo(DrinkDetectorState::DRINK_CONFIRMED);
                    } else if ((newStable - prevRef) >= _config.minDrinkDeltaMl) {
                        if (_sink) _sink->onRefillDetected(newStable - prevRef);
                        _transitionTo(DrinkDetectorState::REFILL_DETECTED);
                    } else {
                        _transitionTo(DrinkDetectorState::CUP_STABLE);
                    }
                } else {
                    _prevStableWeight = newStable;
                    _transitionTo(DrinkDetectorState::CUP_STABLE);
                }
            }
            break;

        case DrinkDetectorState::CUP_STABLE:
            if (weight < threshold) {
                _cupLifted = true;
                _cupLiftedAtMs = nowMs;
                _transitionTo(DrinkDetectorState::NO_CUP);
            } else if (!observation.scaleStable) {
                _transitionTo(DrinkDetectorState::POSSIBLE_DRINK);
            }
            break;

        case DrinkDetectorState::POSSIBLE_DRINK:
            if (weight < threshold) {
                _cupLifted = true;
                _cupLiftedAtMs = nowMs;
                _transitionTo(DrinkDetectorState::NO_CUP);
            } else if (observation.scaleStable) {
                _prevStableWeight = observation.stableWeightGrams;
                _transitionTo(DrinkDetectorState::CUP_STABLE);
            }
            break;

        case DrinkDetectorState::DRINK_CONFIRMED:
        case DrinkDetectorState::REFILL_DETECTED:
            _prevStableWeight = observation.stableWeightGrams;
            _transitionTo(DrinkDetectorState::CUP_STABLE);
            break;
    }
}

void DrinkDetectorCore::_transitionTo(DrinkDetectorState next) {
    _state = next;
}
