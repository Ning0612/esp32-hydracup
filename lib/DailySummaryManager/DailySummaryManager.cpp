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
}

void DailySummaryManager::update() {
    if (!_discord || !_detector || !_time || !_cfg) return;
    if (!_time->isSynced()) return;

    struct tm now;
    if (!_time->getLocalTm(now)) return;

    if (now.tm_hour != 12 || now.tm_min != 0) return;

    const int dayMark = now.tm_year * 1000 + now.tm_yday;
    if (_lastFiredDay == dayMark) return;
    _lastFiredDay = dayMark;

    _fire(now);
}

void DailySummaryManager::_fire(const struct tm& now) {
    char dateStr[12];
    strftime(dateStr, sizeof(dateStr), "%Y/%m/%d", &now);

    const float    totalMl    = _detector->getTodayTotalMl();
    const uint32_t drinkCount = _detector->getDrinkCountToday();

    Serial.printf("[DailySummary] 12:00 settlement  total=%.0f ml  count=%u\n",
                  totalMl, (unsigned)drinkCount);

    const bool notified = _discord->notifyDailySummary(totalMl, drinkCount, String(dateStr));
    if (!notified) {
        Serial.println("[DailySummary] Notification not sent (WiFi/webhook unavailable); counters reset anyway");
    }

    // Always reset: period boundary is authoritative regardless of notification outcome
    _detector->resetDailyCounters();
}
