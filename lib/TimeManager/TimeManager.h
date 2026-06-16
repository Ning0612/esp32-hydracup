#pragma once
#include <Arduino.h>
#include "app_types.h"

class TimeManager {
public:
    void   init(const AppConfig& cfg);
    void   update();
    bool   isSynced() const;
    String getISOTimestamp() const;
    String getYearMonth() const;

private:
    static constexpr uint32_t RESYNC_INTERVAL_MS = 12UL * 60UL * 60UL * 1000UL;

    int      _tzOffsetSec  = 8 * 3600;
    int      _dstOffsetSec = 0;
    String   _ntpServer1;
    String   _ntpServer2;
    bool     _initialized  = false;
    uint32_t _lastSyncMs   = 0;
};
