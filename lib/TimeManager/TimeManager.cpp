#include "TimeManager.h"

#include <cstdio>
#include <cstdlib>

#include "esp_sntp.h"
#include "hal_log.h"
#include "hal_time.h"

namespace {
constexpr const char* TAG = "NTP";

void configureTimezone(int offsetSeconds, int daylightSeconds) {
    const int total = offsetSeconds + daylightSeconds;
    const char sign = total >= 0 ? '-' : '+';
    const int absolute = total >= 0 ? total : -total;
    char tz[32];
    std::snprintf(tz, sizeof(tz), "UTC%c%02d:%02d",
                  sign, absolute / 3600, (absolute % 3600) / 60);
    setenv("TZ", tz, 1);
    tzset();
}
}

void TimeManager::init(const AppConfig& cfg) {
    _tzOffsetSec = cfg.timezoneOffsetSec;
    _dstOffsetSec = cfg.daylightOffsetSec;
    _ntpServer1 = cfg.ntpServer1;
    _ntpServer2 = cfg.ntpServer2;
    configureTimezone(_tzOffsetSec, _dstOffsetSec);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, _ntpServer1.c_str());
    esp_sntp_setservername(1, _ntpServer2.c_str());
    esp_sntp_init();
    _initialized = true;
    _lastSyncMs = hal_millis();
    LOG_INFO(TAG, "SNTP configured offset=%d servers=%s/%s",
             _tzOffsetSec + _dstOffsetSec, _ntpServer1.c_str(), _ntpServer2.c_str());
}

void TimeManager::update() {
    if (!_initialized) return;
    if (hal_millis() - _lastSyncMs >= RESYNC_INTERVAL_MS) {
        esp_sntp_stop();
        esp_sntp_init();
        _lastSyncMs = hal_millis();
        LOG_INFO(TAG, "periodic resync triggered");
    }
}

bool TimeManager::getLocalTm(struct tm& t) const {
    if (!_initialized) return false;
    const time_t now = time(nullptr);
    if (now < 0) return false;
    localtime_r(&now, &t);
    return t.tm_year > (2020 - 1900);
}

bool TimeManager::isSynced() const {
    struct tm local = {};
    return getLocalTm(local);
}

std::string TimeManager::getISOTimestamp() const {
    if (!isSynced()) return "boot+" + std::to_string(hal_millis()) + "ms";
    struct tm local = {};
    if (!getLocalTm(local)) return "boot+" + std::to_string(hal_millis()) + "ms";
    char value[40];
    strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%S", &local);
    const int offset = _tzOffsetSec + _dstOffsetSec;
    const int absolute = offset >= 0 ? offset : -offset;
    char zone[16];
    std::snprintf(zone, sizeof(zone), "%c%02d:%02d", offset >= 0 ? '+' : '-',
                  absolute / 3600, (absolute % 3600) / 60);
    return std::string(value) + zone;
}

std::string TimeManager::getYearMonth() const {
    struct tm local = {};
    if (!getLocalTm(local)) return "unsynced";
    char value[8];
    strftime(value, sizeof(value), "%Y-%m", &local);
    return value;
}

std::string TimeManager::getDateString() const {
    struct tm local = {};
    if (!getLocalTm(local)) return {};
    char value[12];
    strftime(value, sizeof(value), "%Y-%m-%d", &local);
    return value;
}
