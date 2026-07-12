#include "DrinkDetector.h"
#include "DiscordNotifier.h"
#include "EventLogger.h"
#include "TimeManager.h"
#include "MqttPublisher.h"
#include "StorageLock.h"
#include <cstring>

static constexpr uint32_t DRINK_LIFT_TIMEOUT_MS = 120000;

class PreferencesCounterStore final : public DrinkCounterPersistence {
public:
    void begin() {
        if (_queue) return;
        _queue = xQueueCreate(1, sizeof(SaveMessage));
        _operationMutex = xSemaphoreCreateMutex();
        if (!_queue || !_operationMutex ||
            xTaskCreate(_taskFunc, "storage_count", 2560, this, 1, &_task) != pdPASS) {
            Serial.println("[Drink] counter worker unavailable; synchronous fallback enabled");
            _asyncReady = false;
            return;
        }
        _asyncReady = true;
    }

    DrinkCounterLoadStatus load(const char* currentPeriod,
                                 DrinkCounterSnapshot& snapshot) override {
        Preferences prefs;
        if (!lockNvs()) return DrinkCounterLoadStatus::LOAD_FAILED;
        if (!prefs.begin("drink_ctr", true)) {
            unlockNvs();
            return DrinkCounterLoadStatus::LOAD_FAILED;
        }
        const String period = prefs.getString("period", "");
        snapshot.totalMl = prefs.getFloat("total_ml", 0.0f);
        snapshot.drinkCount = prefs.getUInt("count", 0);
        snapshot.lastDrinkMl = prefs.getFloat("last_ml", 0.0f);
        prefs.end();
        unlockNvs();

        std::strncpy(snapshot.period, period.c_str(), sizeof(snapshot.period) - 1);
        snapshot.period[sizeof(snapshot.period) - 1] = '\0';
        if (period.isEmpty()) return DrinkCounterLoadStatus::EMPTY;
        if (period == currentPeriod) return DrinkCounterLoadStatus::CURRENT_PERIOD;
        return DrinkCounterLoadStatus::PREVIOUS_PERIOD;
    }

    void save(const char* currentPeriod,
              const DrinkCounterSnapshot& snapshot) override {
        SaveMessage message{};
        std::strncpy(message.period, currentPeriod ? currentPeriod : "", sizeof(message.period) - 1);
        message.snapshot = snapshot;
        message.version = _pendingVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (_asyncReady && xQueueOverwrite(_queue, &message) == pdTRUE) {
            return;
        }
        if (_saveNow(message))
            _completedVersion.store(message.version, std::memory_order_release);
    }

    bool isIdle() const override {
        return _completedVersion.load(std::memory_order_acquire) ==
               _pendingVersion.load(std::memory_order_acquire);
    }

private:
    struct SaveMessage {
        char period[16];
        DrinkCounterSnapshot snapshot;
        uint32_t version;
    };

    static void _taskFunc(void* param) {
        static_cast<PreferencesCounterStore*>(param)->_taskLoop();
    }

    void _taskLoop() {
        SaveMessage message;
        for (;;) {
            if (xQueueReceive(_queue, &message, portMAX_DELAY) != pdTRUE) continue;
            if (xSemaphoreTake(_operationMutex, portMAX_DELAY) != pdTRUE) continue;
            const bool saved = _saveNow(message);
            xSemaphoreGive(_operationMutex);
            if (saved) {
                _completedVersion.store(message.version, std::memory_order_release);
            } else if (_pendingVersion.load(std::memory_order_acquire) == message.version) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (_pendingVersion.load(std::memory_order_acquire) == message.version)
                    xQueueOverwrite(_queue, &message);
            }
        }
    }

    bool _saveNow(const SaveMessage& message) {
        Preferences prefs;
        if (!lockNvs()) {
            Serial.println("[Drink] NVS busy, counter save dropped");
            return false;
        }
        if (!prefs.begin("drink_ctr", false)) {
            unlockNvs();
            return false;
        }
        const bool ok =
            prefs.putString("period", message.period) == strlen(message.period) &&
            prefs.putFloat("total_ml", message.snapshot.totalMl) == sizeof(float) &&
            prefs.putUInt("count", message.snapshot.drinkCount) == sizeof(uint32_t) &&
            prefs.putFloat("last_ml", message.snapshot.lastDrinkMl) == sizeof(float);
        prefs.end();
        unlockNvs();
        if (!ok) Serial.println("[Drink] counter save failed");
        return ok;
    }

    QueueHandle_t _queue = nullptr;
    SemaphoreHandle_t _operationMutex = nullptr;
    TaskHandle_t _task = nullptr;
    bool _asyncReady = false;
    std::atomic<uint32_t> _pendingVersion{0};
    std::atomic<uint32_t> _completedVersion{0};
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
    if (!_counterStore) {
        defaultStore.begin();
        _counterStore = &defaultStore;
    }
    _events.init(_counterStore, this);

    _coreConfig.cupPresentThresholdGram = cfg.cupPresentThresholdGram;
    _coreConfig.minDrinkDeltaMl = cfg.minDrinkDeltaMl;
    _coreConfig.maxDrinkDeltaMl = cfg.maxDrinkDeltaMl;
    _coreConfig.liftTimeoutMs = DRINK_LIFT_TIMEOUT_MS;
    _core.init(_coreConfig, this);
}

void DrinkDetector::resetScaleBaseline() {
    _core.init(_coreConfig, this);
    if (_state) _state->cupState = CupState::NO_CUP;
    Serial.println("[Drink] Scale baseline reset after tare");
}

void DrinkDetector::update() {
    if (!_scale || !_state || !_cfg || !_reminder || !_buzzer) return;

    if (!_nvsDone && _time && _time->isSynced()) {
        const RestoreState restoreState = _restoreState.load(std::memory_order_acquire);
        if (restoreState == RestoreState::IDLE) _startNvsRestore();
        if (_restoreState.load(std::memory_order_acquire) == RestoreState::LOADING) return;
        if (_restoreState.load(std::memory_order_acquire) == RestoreState::READY) {
            _finishNvsRestore();
            return;
        }
        if (_restoreState.load(std::memory_order_acquire) == RestoreState::FAILED) {
            if ((int32_t)(millis() - _restoreRetryAtMs) >= 0) {
                Serial.println("[Drink] NVS restore failed; retrying");
                _restoreState.store(RestoreState::IDLE, std::memory_order_release);
            }
            return;
        }
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
    if (_deferredCurrentSnapshot.drinkCount > 0 || _deferredCurrentSnapshot.totalMl > 0.0f) {
        _events.mergeSnapshot(_deferredCurrentSnapshot, today.c_str());
        _deferredCurrentSnapshot = DrinkCounterSnapshot{};
    }
    if (_state) {
        _state->todayTotalMl    = _events.getTodayTotalMl();
        _state->lastDrinkMl     = _events.getLastDrinkMl();
        _state->drinkCountToday = _events.getDrinkCountToday();
    }
    _restoreStatus = DrinkCounterLoadStatus::CURRENT_PERIOD;
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

bool DrinkDetector::isPersistenceIdle() const {
    return !_counterStore || _counterStore->isIdle();
}

void DrinkDetector::_startNvsRestore() {
    const String today = _time->getDateString();
    std::strncpy(_restorePeriod, today.c_str(), sizeof(_restorePeriod) - 1);
    _restorePeriod[sizeof(_restorePeriod) - 1] = '\0';
    _preRestoreSnapshot = _events.snapshot();
    _restoreState.store(RestoreState::LOADING, std::memory_order_release);
    if (xTaskCreate(_restoreTaskFunc, "storage_restore", 3072, this, 1, &_restoreTask) != pdPASS) {
        _restoreState.store(RestoreState::FAILED, std::memory_order_release);
    }
}

void DrinkDetector::_restoreTaskFunc(void* param) {
    DrinkDetector* detector = static_cast<DrinkDetector*>(param);
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        detector->_restoreStatus = detector->_counterStore
            ? detector->_counterStore->load(detector->_restorePeriod, detector->_restoreSnapshot)
            : DrinkCounterLoadStatus::EMPTY;
        if (detector->_restoreStatus != DrinkCounterLoadStatus::LOAD_FAILED) {
            detector->_restoreState.store(RestoreState::READY, std::memory_order_release);
            detector->_restoreTask = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    detector->_restoreRetryAtMs = millis() + 5000;
    detector->_restoreState.store(RestoreState::FAILED, std::memory_order_release);
    detector->_restoreTask = nullptr;
    vTaskDelete(nullptr);
}

void DrinkDetector::_finishNvsRestore() {
    const DrinkCounterLoadStatus status = _events.applyRestore(
        _restoreStatus, _restoreSnapshot, _restorePeriod);
    if (status == DrinkCounterLoadStatus::CURRENT_PERIOD ||
        status == DrinkCounterLoadStatus::EMPTY) {
        _events.mergeSnapshot(_preRestoreSnapshot, _restorePeriod);
    } else if (status == DrinkCounterLoadStatus::PREVIOUS_PERIOD) {
        _deferredCurrentSnapshot = _preRestoreSnapshot;
    }
    _preRestoreSnapshot = DrinkCounterSnapshot{};
    if (status == DrinkCounterLoadStatus::CURRENT_PERIOD) {
        // Same day: restore counters
        Serial.printf("[Drink] NVS restore: %s total=%.0f count=%u\n",
                      _restorePeriod, _events.getTodayTotalMl(),
                      _events.getDrinkCountToday());
    } else if (status == DrinkCounterLoadStatus::PREVIOUS_PERIOD) {
        // Previous period: restore into RAM so DailySummaryManager can settle
        // Do NOT overwrite NVS — resetDailyCounters() will write today after settlement
        Serial.printf("[Drink] NVS prev period %s total=%.0f — pending settlement\n",
                      _events.getRestoredPeriod(), _events.getTodayTotalMl());
    }
    if (status == DrinkCounterLoadStatus::EMPTY) Serial.println("[Drink] NVS no data, fresh start");
    _nvsDone = true;
    _restoreState.store(RestoreState::IDLE, std::memory_order_release);
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
