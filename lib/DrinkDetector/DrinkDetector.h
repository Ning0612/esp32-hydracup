#pragma once

#include <atomic>
#include <cstdint>

#include "DrinkDetectorCore.h"
#include "AppState.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class AppState;
struct AppConfig;
class BuzzerController;
class DiscordNotifier;
class EventLogger;
class MqttPublisher;
class ReminderManager;
class ScaleManager;
class TimeManager;

class DrinkDetector : private DrinkDetectorEventSink, private DrinkDetectorEffects {
public:
    void init(ScaleManager& scale, AppState& state, const AppConfig& cfg,
              ReminderManager& reminder, BuzzerController& buzzer);
    void update();
    void resetScaleBaseline();
    bool isPersistenceIdle() const;
    bool isPersistenceReady() const { return _restoreState.load() != RestoreState::LOADING; }
    bool isPersistenceInitialized() const { return _nvsDone; }
    bool hasPreviousPeriodCounters() const {
        return _nvsDone && _restoreStatus == DrinkCounterLoadStatus::PREVIOUS_PERIOD;
    }
    const char* getRestoredPeriod() const { return _events.getRestoredPeriod(); }
    void setDiscordNotifier(DiscordNotifier* value) { _discord = value; }
    void setEventLogger(EventLogger* value) { _eventLog = value; }
    void setTimeManager(TimeManager* value) { _time = value; }
    void setMqttPublisher(MqttPublisher* value) { _mqtt = value; }
    void setCounterPersistence(DrinkCounterPersistence* store) {
        _counterStore = store; _events.setPersistence(store);
    }
    CupState getCupState() const { return _state ? _state->cupState : CupState::NO_CUP; }
    float getTodayTotalMl() const { return _events.getTodayTotalMl(); }
    float getLastDrinkMl() const { return _events.getLastDrinkMl(); }
    uint32_t getDrinkCountToday() const { return _events.getDrinkCountToday(); }
    void resetDailyCounters();

private:
    void onDrinkConfirmed(float amountMl) override;
    void onRefillDetected(float amountMl) override;
    void resetReminder() override;
    void playDrinkBeep() override;
    void notifyDrink(float amountMl, float totalMl, uint32_t drinkCount) override;
    void logDrink(const char* timestamp, float amountMl, float totalMl) override;
    void publishStatus(float totalMl, const char* event) override;
    void _syncCupState();
    void _startNvsRestore();
    void _finishNvsRestore();
    static void _restoreTaskFunc(void* param);
    static const char* _cupStateName(CupState state);

    DrinkDetectorCore _core;
    DrinkDetectorConfig _coreConfig;
    DrinkDetectorEventHandler _events;
    ScaleManager* _scale = nullptr;
    AppState* _state = nullptr;
    const AppConfig* _cfg = nullptr;
    ReminderManager* _reminder = nullptr;
    BuzzerController* _buzzer = nullptr;
    DiscordNotifier* _discord = nullptr;
    EventLogger* _eventLog = nullptr;
    TimeManager* _time = nullptr;
    MqttPublisher* _mqtt = nullptr;
    DrinkCounterPersistence* _counterStore = nullptr;
    bool _nvsDone = false;
    enum class RestoreState : uint8_t { IDLE, LOADING, READY, FAILED };
    std::atomic<RestoreState> _restoreState{RestoreState::IDLE};
    DrinkCounterLoadStatus _restoreStatus = DrinkCounterLoadStatus::EMPTY;
    DrinkCounterSnapshot _restoreSnapshot;
    DrinkCounterSnapshot _preRestoreSnapshot;
    DrinkCounterSnapshot _deferredCurrentSnapshot;
    char _restorePeriod[16] = {};
    TaskHandle_t _restoreTask = nullptr;
    uint32_t _restoreRetryAtMs = 0;
};
