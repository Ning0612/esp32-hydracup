#pragma once

#include <string>

#include "app_types.h"

class ConfigManager {
public:
    void load(AppConfig& cfg);
    bool save(const AppConfig& cfg);
    bool saveCalibration(float factor, long offset);
    bool saveWifi(const std::string& ssid, const std::string& password);
    bool saveReminderSettings(bool enabled, uint32_t intervalMin, uint32_t dailyGoalMl,
                              const std::string& pausedUntilDate);
    void clear();

private:
    void _applyDefaults(AppConfig& cfg);
    void _ensureCloudIdentity(AppConfig& cfg);
};
