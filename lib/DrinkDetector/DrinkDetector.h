#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "app_types.h"
#include "ScaleManager.h"
#include "AppState.h"
#include "ReminderManager.h"
#include "BuzzerController.h"
#include "DrinkDetectorCore.h"

class DiscordNotifier;
class EventLogger;
class TimeManager;
class MqttPublisher;

class DrinkDetector : private DrinkDetectorEventSink,
                      private DrinkDetectorEffects {
public:
    void init(ScaleManager& scale, AppState& state, const AppConfig& cfg,
              ReminderManager& reminder, BuzzerController& buzzer);
    void update();

    void setDiscordNotifier(DiscordNotifier* dn) { _discord  = dn; }
    void setEventLogger(EventLogger* el)         { _eventLog = el; }
    void setTimeManager(TimeManager* tm)         { _time     = tm; }
    void setMqttPublisher(MqttPublisher* mp)     { _mqtt     = mp; }
    void setCounterPersistence(DrinkCounterPersistence* store) {
        _counterStore = store;
        _events.setPersistence(store);
    }

    CupState getCupState()        const { return _state->cupState; }
    float    getTodayTotalMl()    const { return _events.getTodayTotalMl(); }
    float    getLastDrinkMl()     const { return _events.getLastDrinkMl(); }
    uint32_t getDrinkCountToday() const { return _events.getDrinkCountToday(); }
    void     resetDailyCounters();

private:
    void onDrinkConfirmed(float amountMl) override;
    void onRefillDetected(float amountMl) override;
    void resetReminder() override;
    void playDrinkBeep() override;
    void notifyDrink(float amountMl, float totalMl, uint32_t drinkCount) override;
    void logDrink(const char* timestamp, float amountMl, float totalMl) override;
    void publishStatus(float totalMl, const char* event) override;

    DrinkDetectorCore _core;
    DrinkDetectorEventHandler _events;
    ScaleManager*     _scale      = nullptr;
    AppState*         _state      = nullptr;
    const AppConfig*  _cfg        = nullptr;
    ReminderManager*  _reminder   = nullptr;
    BuzzerController* _buzzer     = nullptr;
    DiscordNotifier*  _discord    = nullptr;
    EventLogger*      _eventLog   = nullptr;
    TimeManager*      _time       = nullptr;
    MqttPublisher*    _mqtt       = nullptr;
    DrinkCounterPersistence* _counterStore = nullptr;

    bool     _nvsDone               = false;

    void _syncCupState();
    void _nvsRestore();

    static const char* _cupStateName(CupState s);
};
