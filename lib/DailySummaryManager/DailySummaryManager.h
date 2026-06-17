#pragma once
#include <Arduino.h>

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
    DiscordNotifier* _discord  = nullptr;
    DrinkDetector*   _detector = nullptr;
    TimeManager*     _time     = nullptr;
    const AppConfig* _cfg      = nullptr;
    int              _lastFiredDay = -1;

    void _fire(const struct tm& now);
};
