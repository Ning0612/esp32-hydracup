#pragma once
#include <Arduino.h>
#include "app_types.h"
#include "ScaleManager.h"
#include "AppState.h"
#include "ReminderManager.h"
#include "BuzzerController.h"

class DiscordNotifier;
class EventLogger;
class TimeManager;

class DrinkDetector {
public:
    void init(ScaleManager& scale, AppState& state, const AppConfig& cfg,
              ReminderManager& reminder, BuzzerController& buzzer);
    void update();

    void setDiscordNotifier(DiscordNotifier* dn) { _discord  = dn; }
    void setEventLogger(EventLogger* el)         { _eventLog = el; }
    void setTimeManager(TimeManager* tm)         { _time     = tm; }

    CupState getCupState()        const { return _state->cupState; }
    float    getTodayTotalMl()    const { return _todayTotalMl; }
    float    getLastDrinkMl()     const { return _lastDrinkMl; }
    uint32_t getDrinkCountToday() const { return _drinkCount; }

private:
    ScaleManager*     _scale      = nullptr;
    AppState*         _state      = nullptr;
    const AppConfig*  _cfg        = nullptr;
    ReminderManager*  _reminder   = nullptr;
    BuzzerController* _buzzer     = nullptr;
    DiscordNotifier*  _discord    = nullptr;
    EventLogger*      _eventLog   = nullptr;
    TimeManager*      _time       = nullptr;

    float    _prevStableWeight = 0.0f;
    float    _todayTotalMl     = 0.0f;
    float    _lastDrinkMl      = 0.0f;
    uint32_t _drinkCount       = 0;

    void _transitionTo(CupState next);
    void _onDrinkConfirmed(float amountMl);
    void _onRefillDetected(float amountMl);

    static const char* _cupStateName(CupState s);
};
