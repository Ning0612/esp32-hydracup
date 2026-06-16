#include "TimeManager.h"

void TimeManager::init(const AppConfig& cfg) {
    _tzOffsetSec  = cfg.timezoneOffsetSec;
    _dstOffsetSec = cfg.daylightOffsetSec;
    _ntpServer1   = cfg.ntpServer1;
    _ntpServer2   = cfg.ntpServer2;
    configTime(_tzOffsetSec, _dstOffsetSec,
               _ntpServer1.c_str(), _ntpServer2.c_str());
    _initialized = true;
    _lastSyncMs  = millis();
    Serial.printf("[NTP] configTime tz=%d dst=%d srv=%s/%s\n",
                  _tzOffsetSec, _dstOffsetSec,
                  _ntpServer1.c_str(), _ntpServer2.c_str());
}

void TimeManager::update() {
    if (!_initialized) return;
    if (millis() - _lastSyncMs >= RESYNC_INTERVAL_MS) {
        configTime(_tzOffsetSec, _dstOffsetSec,
                   _ntpServer1.c_str(), _ntpServer2.c_str());
        _lastSyncMs = millis();
        Serial.println("[NTP] Periodic re-sync triggered");
    }
}

bool TimeManager::isSynced() const {
    if (!_initialized) return false;
    struct tm tm;
    if (!getLocalTime(&tm, 0)) return false;
    return tm.tm_year > (2020 - 1900);
}

String TimeManager::getISOTimestamp() const {
    if (!isSynced()) return String("boot+") + String(millis()) + "ms";
    struct tm tm;
    if (!getLocalTime(&tm, 0)) return String("boot+") + String(millis()) + "ms";

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);

    const int absOff = abs(_tzOffsetSec);
    const int h      = absOff / 3600;
    const int m      = (absOff % 3600) / 60;
    char tzBuf[8];
    snprintf(tzBuf, sizeof(tzBuf), "%c%02d:%02d",
             _tzOffsetSec >= 0 ? '+' : '-', h, m);

    return String(buf) + tzBuf;
}

String TimeManager::getYearMonth() const {
    if (!isSynced()) return "unsynced";
    struct tm tm;
    if (!getLocalTime(&tm, 0)) return "unsynced";
    char buf[8];
    strftime(buf, sizeof(buf), "%Y-%m", &tm);
    return String(buf);
}
