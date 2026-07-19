#include "DrinkDetector.h"

#include <cstring>

#include "AppState.h"
#include "BuzzerController.h"
#include "DiscordNotifier.h"
#include "EventLogger.h"
#include "MqttPublisher.h"
#include "ReminderManager.h"
#include "ScaleManager.h"
#include "StorageLock.h"
#include "TimeManager.h"
#include "app_types.h"
#include "hal_log.h"
#include "hal_time.h"
#include "nvs.h"

namespace {
constexpr uint32_t DRINK_LIFT_TIMEOUT_MS = 120000;
constexpr const char* TAG = "Drink";

class NvsCounterStore final : public DrinkCounterPersistence {
public:
    void begin() {
        if (_queue) return;
        _queue = xQueueCreate(1, sizeof(SaveMessage));
        _operationMutex = xSemaphoreCreateMutex();
        if (!_queue || !_operationMutex ||
            xTaskCreate(_taskFunc, "storage_count", 2560, this, 1, &_task) != pdPASS) {
            LOG_WARN(TAG, "counter worker unavailable; synchronous fallback enabled");
            _asyncReady = false;
            return;
        }
        _asyncReady = true;
    }

    DrinkCounterLoadStatus load(const char* currentPeriod,
                                DrinkCounterSnapshot& snapshot) override {
        if (!lockNvs()) return DrinkCounterLoadStatus::LOAD_FAILED;
        nvs_handle_t handle = 0;
        const esp_err_t opened = nvs_open("drink_ctr", NVS_READONLY, &handle);
        if (opened == ESP_ERR_NVS_NOT_FOUND) { unlockNvs(); return DrinkCounterLoadStatus::EMPTY; }
        if (opened != ESP_OK) { unlockNvs(); return DrinkCounterLoadStatus::LOAD_FAILED; }
        char period[sizeof(snapshot.period)] = {};
        size_t length = sizeof(period);
        const bool periodOk = nvs_get_str(handle, "period", period, &length) == ESP_OK;
        float total = 0.0f, last = 0.0f;
        size_t floatLength = sizeof(float);
        const bool totalOk = nvs_get_blob(handle, "total_ml", &total, &floatLength) == ESP_OK && floatLength == sizeof(float);
        floatLength = sizeof(float);
        const bool lastOk = nvs_get_blob(handle, "last_ml", &last, &floatLength) == ESP_OK && floatLength == sizeof(float);
        uint32_t count = 0;
        const bool countOk = nvs_get_u32(handle, "count", &count) == ESP_OK;
        nvs_close(handle);
        unlockNvs();
        if (!periodOk || !totalOk || !lastOk || !countOk) return DrinkCounterLoadStatus::LOAD_FAILED;
        std::strncpy(snapshot.period, period, sizeof(snapshot.period) - 1);
        snapshot.totalMl = total; snapshot.lastDrinkMl = last; snapshot.drinkCount = count;
        if (period[0] == '\0') return DrinkCounterLoadStatus::EMPTY;
        return std::strcmp(period, currentPeriod) == 0 ? DrinkCounterLoadStatus::CURRENT_PERIOD
                                                        : DrinkCounterLoadStatus::PREVIOUS_PERIOD;
    }

    void save(const char* currentPeriod, const DrinkCounterSnapshot& snapshot) override {
        SaveMessage message = {};
        std::strncpy(message.period, currentPeriod ? currentPeriod : "", sizeof(message.period) - 1);
        message.snapshot = snapshot;
        message.version = _pendingVersion.fetch_add(1) + 1;
        if (_asyncReady && xQueueOverwrite(_queue, &message) == pdTRUE) return;
        if (_saveNow(message)) _completedVersion.store(message.version);
    }

    bool isIdle() const override { return _completedVersion.load() == _pendingVersion.load(); }

private:
    struct SaveMessage { char period[16]; DrinkCounterSnapshot snapshot; uint32_t version; };
    static void _taskFunc(void* param) { static_cast<NvsCounterStore*>(param)->_taskLoop(); }
    void _taskLoop() {
        SaveMessage message = {};
        for (;;) {
            if (xQueueReceive(_queue, &message, portMAX_DELAY) != pdTRUE) continue;
            if (xSemaphoreTake(_operationMutex, portMAX_DELAY) != pdTRUE) continue;
            const bool saved = _saveNow(message);
            xSemaphoreGive(_operationMutex);
            if (saved) _completedVersion.store(message.version);
            else if (_pendingVersion.load() == message.version) {
                vTaskDelay(pdMS_TO_TICKS(500));
                if (_pendingVersion.load() == message.version) xQueueOverwrite(_queue, &message);
            }
        }
    }
    bool _saveNow(const SaveMessage& message) {
        if (!lockNvs()) return false;
        nvs_handle_t handle = 0;
        if (nvs_open("drink_ctr", NVS_READWRITE, &handle) != ESP_OK) { unlockNvs(); return false; }
        const bool ok = nvs_set_str(handle, "period", message.period) == ESP_OK &&
            nvs_set_blob(handle, "total_ml", &message.snapshot.totalMl, sizeof(float)) == ESP_OK &&
            nvs_set_u32(handle, "count", message.snapshot.drinkCount) == ESP_OK &&
            nvs_set_blob(handle, "last_ml", &message.snapshot.lastDrinkMl, sizeof(float)) == ESP_OK &&
            nvs_commit(handle) == ESP_OK;
        nvs_close(handle); unlockNvs();
        if (!ok) LOG_WARN(TAG, "counter save failed");
        return ok;
    }
    QueueHandle_t _queue = nullptr;
    SemaphoreHandle_t _operationMutex = nullptr;
    TaskHandle_t _task = nullptr;
    bool _asyncReady = false;
    std::atomic<uint32_t> _pendingVersion{0};
    std::atomic<uint32_t> _completedVersion{0};
};

CupState toCupState(DrinkDetectorState state) {
    switch (state) {
        case DrinkDetectorState::NO_CUP: return CupState::NO_CUP;
        case DrinkDetectorState::CUP_UNSTABLE: return CupState::CUP_UNSTABLE;
        case DrinkDetectorState::CUP_STABLE: return CupState::CUP_STABLE;
        case DrinkDetectorState::POSSIBLE_DRINK: return CupState::POSSIBLE_DRINK;
        case DrinkDetectorState::DRINK_CONFIRMED: return CupState::DRINK_CONFIRMED;
        case DrinkDetectorState::REFILL_DETECTED: return CupState::REFILL_DETECTED;
        default: return CupState::NO_CUP;
    }
}
NvsCounterStore counterStore;
}

void DrinkDetector::init(ScaleManager& scale, AppState& state, const AppConfig& cfg,
                         ReminderManager& reminder, BuzzerController& buzzer) {
    _scale = &scale; _state = &state; _cfg = &cfg; _reminder = &reminder; _buzzer = &buzzer;
    _state->cupState = CupState::NO_CUP;
    if (!_counterStore) { counterStore.begin(); _counterStore = &counterStore; }
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
    LOG_INFO(TAG, "scale baseline reset after tare");
}

void DrinkDetector::update() {
    if (!_scale || !_state || !_cfg || !_reminder || !_buzzer) return;
    if (!_nvsDone && _time && _time->isSynced()) {
        const RestoreState state = _restoreState.load();
        if (state == RestoreState::IDLE) _startNvsRestore();
        if (_restoreState.load() == RestoreState::LOADING) return;
        if (_restoreState.load() == RestoreState::READY) { _finishNvsRestore(); return; }
        if (_restoreState.load() == RestoreState::FAILED) {
            if (static_cast<int32_t>(hal_millis() - _restoreRetryAtMs) >= 0) _restoreState.store(RestoreState::IDLE);
            return;
        }
    }
    if (!_scale->isReady() || !_scale->isSamplesReady()) return;
    DrinkDetectorObservation observation;
    observation.weightGrams = _scale->getWeightGrams();
    observation.stableWeightGrams = _scale->getStableWeightGrams();
    observation.scaleStable = _scale->isStable();
    _core.update(observation, hal_millis());
    _syncCupState();
    _state->todayTotalMl = _events.getTodayTotalMl();
    _state->lastDrinkMl = _events.getLastDrinkMl();
    _state->drinkCountToday = _events.getDrinkCountToday();
}

void DrinkDetector::resetDailyCounters() {
    const std::string today = _time && _time->isSynced() ? _time->getDateString() : "";
    _events.resetDailyCounters(today.empty() ? nullptr : today.c_str());
    if (_deferredCurrentSnapshot.drinkCount > 0 || _deferredCurrentSnapshot.totalMl > 0.0f) {
        _events.mergeSnapshot(_deferredCurrentSnapshot, today.c_str());
        _deferredCurrentSnapshot = DrinkCounterSnapshot{};
    }
    if (_state) {
        _state->todayTotalMl = _events.getTodayTotalMl(); _state->lastDrinkMl = _events.getLastDrinkMl();
        _state->drinkCountToday = _events.getDrinkCountToday();
    }
    _restoreStatus = DrinkCounterLoadStatus::CURRENT_PERIOD;
    LOG_INFO(TAG, "daily counters reset");
}

bool DrinkDetector::isPersistenceIdle() const { return !_counterStore || _counterStore->isIdle(); }

void DrinkDetector::_syncCupState() {
    const CupState next = toCupState(_core.getState());
    if (_state->cupState != next) {
        LOG_INFO("CupState", "%s -> %s", _cupStateName(_state->cupState), _cupStateName(next));
        _state->cupState = next;
    }
}

void DrinkDetector::onDrinkConfirmed(float amountMl) {
    const std::string timestamp = _time ? _time->getISOTimestamp() : "";
    const std::string period = _time && _time->isSynced() ? _time->getDateString() : "";
    LOG_INFO(TAG, "drink %.0f ml total=%.0f count=%lu", amountMl,
             _events.getTodayTotalMl() + amountMl,
             static_cast<unsigned long>(_events.getDrinkCountToday() + 1));
    _events.onDrinkConfirmed(amountMl, period.c_str(), timestamp.c_str());
}

void DrinkDetector::_startNvsRestore() {
    const std::string today = _time->getDateString();
    std::strncpy(_restorePeriod, today.c_str(), sizeof(_restorePeriod) - 1);
    _preRestoreSnapshot = _events.snapshot();
    _restoreState.store(RestoreState::LOADING);
    if (xTaskCreate(_restoreTaskFunc, "storage_restore", 3072, this, 1, &_restoreTask) != pdPASS)
        _restoreState.store(RestoreState::FAILED);
}

void DrinkDetector::_restoreTaskFunc(void* param) {
    auto* detector = static_cast<DrinkDetector*>(param);
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        detector->_restoreStatus = detector->_counterStore
            ? detector->_counterStore->load(detector->_restorePeriod, detector->_restoreSnapshot)
            : DrinkCounterLoadStatus::EMPTY;
        if (detector->_restoreStatus != DrinkCounterLoadStatus::LOAD_FAILED) {
            detector->_restoreState.store(RestoreState::READY); detector->_restoreTask = nullptr; vTaskDelete(nullptr); return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    detector->_restoreRetryAtMs = hal_millis() + 5000;
    detector->_restoreState.store(RestoreState::FAILED); detector->_restoreTask = nullptr; vTaskDelete(nullptr);
}

void DrinkDetector::_finishNvsRestore() {
    const auto status = _events.applyRestore(_restoreStatus, _restoreSnapshot, _restorePeriod);
    if (status == DrinkCounterLoadStatus::CURRENT_PERIOD || status == DrinkCounterLoadStatus::EMPTY)
        _events.mergeSnapshot(_preRestoreSnapshot, _restorePeriod);
    else if (status == DrinkCounterLoadStatus::PREVIOUS_PERIOD) _deferredCurrentSnapshot = _preRestoreSnapshot;
    _preRestoreSnapshot = DrinkCounterSnapshot{};
    LOG_INFO(TAG, "counter restore status=%u total=%.0f count=%u", static_cast<unsigned>(status),
             _events.getTodayTotalMl(), static_cast<unsigned>(_events.getDrinkCountToday()));
    _nvsDone = true; _restoreState.store(RestoreState::IDLE);
}

void DrinkDetector::onRefillDetected(float amountMl) { LOG_INFO(TAG, "refill +%.0f ml", amountMl); _events.onRefillDetected(amountMl); }
void DrinkDetector::resetReminder() { if (_reminder) _reminder->resetTimer(); }
void DrinkDetector::playDrinkBeep() { if (_buzzer) _buzzer->play(BeepPattern::DRINK); }
void DrinkDetector::notifyDrink(float amountMl, float totalMl, uint32_t count) { if (_discord) _discord->notifyDrink(amountMl, totalMl, count); }
void DrinkDetector::logDrink(const char* timestamp, float amountMl, float totalMl) {
    if (_eventLog) _eventLog->logDrink(timestamp ? timestamp : "", amountMl, totalMl, _time);
}
void DrinkDetector::publishStatus(float totalMl, const char* event) { if (_mqtt) _mqtt->publishStatus(totalMl, event); }

const char* DrinkDetector::_cupStateName(CupState state) {
    switch (state) {
        case CupState::NO_CUP: return "NO_CUP"; case CupState::CUP_UNSTABLE: return "CUP_UNSTABLE";
        case CupState::CUP_STABLE: return "CUP_STABLE"; case CupState::POSSIBLE_DRINK: return "POSSIBLE_DRINK";
        case CupState::DRINK_CONFIRMED: return "DRINK_CONFIRMED"; case CupState::REFILL_DETECTED: return "REFILL_DETECTED";
        default: return "UNKNOWN";
    }
}
