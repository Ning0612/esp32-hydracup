#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "app_types.h"

class ConfigManager {
public:
    void load(AppConfig& cfg);
    void save(const AppConfig& cfg);
    bool saveCalibration(float factor, long offset);
    void saveWifi(const String& ssid, const String& password);
    void clear();

private:
    Preferences _prefs;

    void _applyDefaults(AppConfig& cfg);
};
