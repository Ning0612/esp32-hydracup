#include "DrinkDetector.h"
#include "DiscordNotifier.h"
#include "EventLogger.h"
#include "TimeManager.h"
#include "MqttPublisher.h"
#include <cstring>

static constexpr uint32_t DRINK_LIFT_TIMEOUT_MS = 120000;

class PreferencesCounterStore final : public DrinkCounterPersistence {
public:
    DrinkCounterLoadStatus load(const char* currentPeriod,
                                 DrinkCounterSnapshot& snapshot) override {
        Preferences prefs;
        prefs.begin("drink_ctr", true);
        const String period = prefs.getString("period", "");
        snapshot.totalMl = prefs.getFloat("total_ml", 0.0f);
        snapshot.drinkCount = prefs.getUInt("count", 0);
        snapshot.lastDrinkMl = prefs.getFloat("last_ml", 0.0f);
        prefs.end();

        std::strncpy(snapshot.period, period.c_str(), sizeof(snapshot.period) - 1);
        snapshot.period[sizeof(snapshot.period) - 1] = '\0';
        if (period.isEmpty()) return DrinkCounterLoadStatus::EMPTY;
        if (period == currentPeriod) return DrinkCounterLoadStatus::CURRENT_PERIOD;
        return DrinkCounterLoadStatus::PREVIOUS_PERIOD;
    }

    void save(const char* currentPeriod,
              const DrinkCounterSnapshot& snapshot) override {
        Preferences prefs;
        prefs.begin("drink_ctr", false);
        prefs.putString("period", currentPeriod);
        prefs.putFloat("total_ml", snapshot.totalMl);
        prefs.putUInt("count", snapshot.drinkCount);
        prefs.putFloat("last_ml", snapshot.lastDrinkMl);
        prefs.end();
    }
};

static CupState _toCupState(DrinkDetectorState state) {
    switch (state) {
        case DrinkDetectorState::NO_CUP:          return CupState::NO_CUP;
        case DrinkDetectorState::CUP_UNSTABLE:    return CupState::CUP_UNSTABLE;
        case DrinkDetectorState::CUP_STABLE:      return CupState::CUP_STABLE;
        case DrinkDetectorState::POSSIBLE_DRINK:  return CupState::POSSIBLE_DRINK;
        case DrinkDetectorState::DRINK_CONFIRMED: return CupState::DRINK_CONFIRMED;
        case DrinkDetectorState::REFILL_DETECTED: return CupState::REFILL_DETECTED;
        default:                                  return CupState::NO_CUP;
    }
}

void DrinkDetector::init(ScaleManager& scale, AppState& state, const AppConfig& cfg,
                         ReminderManager& reminder, BuzzerController& buzzer) {
    _scale    = &scale;
    _state    = &state;
    _cfg      = &cfg;
    _reminder = &reminder;
    _buzzer   = &buzzer;
    _state->cupState = CupState::NO_CUP;

    static PreferencesCounterStore defaultStore;
    if (!_counterStore) _counterStore = &defaultStore;
    _events.init(_counterStore, this);

    DrinkDetectorConfig coreConfig;
    coreConfig.cupPresentThresholdGram = cfg.cupPresentThresholdGram;
    coreConfig.minDrinkDeltaMl = cfg.minDrinkDeltaMl;
    coreConfig.maxDrinkDeltaMl = cfg.maxDrinkDeltaMl;
    coreConfig.liftTimeoutMs = DRINK_LIFT_TIMEOUT_MS;
    _core.init(coreConfig, this);
}

void DrinkDetector::update() {
    if (!_scale || !_state || !_cfg || !_reminder || !_buzzer) return;

    if (!_nvsDone && _time && _time->isSynced()) {
        _nvsRestore();
        _nvsDone = true;
    }

    if (!_scale->isReady() || !_scale->isSamplesReady()) return;

    DrinkDetectorObservation observation;
    observation.weightGrams = _scale->getWeightGrams();
    observation.stableWeightGrams = _scale->getStableWeightGrams();
    observation.scaleStable = _scale->isStable();
    _core.update(observation, millis());
    _syncCupState();

    // Sync to AppState
    _state->todayTotalMl    = _events.getTodayTotalMl();
    _state->lastDrinkMl     = _events.getLastDrinkMl();
    _state->drinkCountToday = _events.getDrinkCountToday();
}

void DrinkDetector::resetDailyCounters() {
    const String today = (_time && _time->isSynced()) ? _time->getDateString() : String("");
    _events.resetDailyCounters(today.isEmpty() ? nullptr : today.c_str());
    if (_state) {
        _state->todayTotalMl    = 0.0f;
        _state->lastDrinkMl     = 0.0f;
        _state->drinkCountToday = 0;
    }
    Serial.println("[Drink] Daily counters reset");
}

void DrinkDetector::_syncCupState() {
    const CupState next = _toCupState(_core.getState());
    if (_state->cupState != next) {
        Serial.printf("[CupState] %s → %s\n",
                      _cupStateName(_state->cupState), _cupStateName(next));
        _state->cupState = next;
    }
}

void DrinkDetector::onDrinkConfirmed(float amountMl) {
    const String ts = _time ? _time->getISOTimestamp() : String("");
    const String period = (_time && _time->isSynced()) ? _time->getDateString() : String("");
    Serial.printf("[DRINK] %.0f ml  total=%.0f ml  count=%lu\n",
                  amountMl, _events.getTodayTotalMl() + amountMl,
                  (unsigned long)(_events.getDrinkCountToday() + 1));
    _events.onDrinkConfirmed(amountMl, period.c_str(), ts.c_str());
}

void DrinkDetector::_nvsRestore() {
    const String today = _time->getDateString();

    const DrinkCounterLoadStatus status = _events.restore(today.c_str());

    if (status == DrinkCounterLoadStatus::CURRENT_PERIOD) {
        // Same day: restore counters
        Serial.printf("[Drink] NVS restore: %s total=%.0f count=%u\n",
                      today.c_str(), _events.getTodayTotalMl(),
                      _events.getDrinkCountToday());
    } else if (status == DrinkCounterLoadStatus::PREVIOUS_PERIOD) {
        // Previous period: restore into RAM so DailySummaryManager can settle
        // Do NOT overwrite NVS — resetDailyCounters() will write today after settlement
        Serial.printf("[Drink] NVS prev period %s total=%.0f — pending settlement\n",
                      _events.getRestoredPeriod(), _events.getTodayTotalMl());
    }
    if (status == DrinkCounterLoadStatus::EMPTY) Serial.println("[Drink] NVS no data, fresh start");
}

void DrinkDetector::onRefillDetected(float amountMl) {
    Serial.printf("[REFILL] +%.0f ml\n", amountMl);
    _events.onRefillDetected(amountMl);
}

void DrinkDetector::resetReminder() {
    if (_reminder) _reminder->resetTimer();
}

void DrinkDetector::playDrinkBeep() {
    if (_buzzer) _buzzer->play(BeepPattern::DRINK);
}

void DrinkDetector::notifyDrink(float amountMl, float totalMl, uint32_t drinkCount) {
    if (_discord) _discord->notifyDrink(amountMl, totalMl, drinkCount);
}

void DrinkDetector::logDrink(const char* timestamp, float amountMl, float totalMl) {
    if (_eventLog) _eventLog->logDrink(String(timestamp), amountMl, totalMl, _time);
}

void DrinkDetector::publishStatus(float totalMl, const char* event) {
    if (_mqtt) _mqtt->publishStatus(totalMl, event);
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
