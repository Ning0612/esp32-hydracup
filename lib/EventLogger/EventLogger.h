#pragma once
#include <Arduino.h>

class TimeManager;

class EventLogger {
public:
    void init(bool fsOk);
    void logDrink(const String& timestamp, float amountMl, float totalMl, TimeManager* tm);
    void update() {}

private:
    bool _fsOk = false;
};
