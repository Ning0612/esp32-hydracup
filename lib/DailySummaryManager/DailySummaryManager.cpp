#include "DailySummaryManager.h"
#include "DiscordNotifier.h"
#include "DrinkDetector.h"
#include "TimeManager.h"
#include "app_types.h"

void DailySummaryManager::init(DiscordNotifier& discord, DrinkDetector& detector,
                                TimeManager& time, const AppConfig& cfg) {
    _discord  = &discord;
    _detector = &detector;
    _time     = &time;
    _cfg      = &cfg;

    Preferences prefs;
    prefs.begin("daily_sum", true);
    _lastSettledKey = prefs.getString("settled", "");
    prefs.end();
}

void DailySummaryManager::update() {
    if (!_discord || !_detector || !_time || !_cfg) return;
    if (!_time->isSynced()) return;

    struct tm now;
    if (!_time->getLocalTm(now)) return;

    if (now.tm_hour != 0) return;

    const String today = _time->getDateString();
    if (_lastSettledKey == today) return;

    _fire(now);
}

void DailySummaryManager::_fire(const struct tm& now) {
    // At 00:00 of day D we're settling day D-1
    struct tm nowCopy = now;
    time_t t = mktime(&nowCopy);
    t -= 86400;
    struct tm yesterday;
    localtime_r(&t, &yesterday);
    char dateStr[12];
    strftime(dateStr, sizeof(dateStr), "%Y/%m/%d", &yesterday);

    const float    totalMl    = _detector->getTodayTotalMl();
    const uint32_t drinkCount = _detector->getDrinkCountToday();

    Serial.printf("[DailySummary] 00:00 settlement  total=%.0f ml  count=%u\n",
                  totalMl, (unsigned)drinkCount);

    const bool notified = _discord->notifyDailySummary(totalMl, drinkCount, String(dateStr));
    if (!notified) {
        Serial.println("[DailySummary] Notification skipped (WiFi/webhook); counters reset anyway");
    }

    _detector->resetDailyCounters();

    _lastSettledKey = _time->getDateString();
    Preferences prefs;
    prefs.begin("daily_sum", false);
    prefs.putString("settled", _lastSettledKey);
    prefs.end();
}
