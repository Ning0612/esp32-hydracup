#pragma once

#include <string>

#include "app_types.h"

class ConfigManager {
public:
    void load(AppConfig& cfg);
    bool save(const AppConfig& cfg);
    bool saveCalibration(float factor, long offset);
    bool saveWifi(const std::string& ssid, const std::string& password);
    void clear();

private:
    void _applyDefaults(AppConfig& cfg);
};
