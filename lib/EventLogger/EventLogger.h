#pragma once
#include <Arduino.h>
#include <LittleFS.h>

class TimeManager;

class EventLogger {
public:
    void init(bool fsOk, fs::LittleFSFS& fs);
    void logDrink(const String& timestamp, float amountMl, float totalMl, TimeManager* tm);
    void update() {}

private:
    bool        _fsOk = false;
    fs::LittleFSFS* _fs   = nullptr;
};
