#pragma once
#include <Arduino.h>
#include <Preferences.h>

class DiscordNotifier;
class DrinkDetector;
class TimeManager;
struct AppConfig;

class DailySummaryManager {
public:
    void init(DiscordNotifier& discord, DrinkDetector& detector,
              TimeManager& time, const AppConfig& cfg);
    void update();

private:
    DiscordNotifier* _discord        = nullptr;
    DrinkDetector*   _detector       = nullptr;
    TimeManager*     _time           = nullptr;
    const AppConfig* _cfg            = nullptr;
    String           _lastSettledKey;

    void _fire(const struct tm& now);
};
