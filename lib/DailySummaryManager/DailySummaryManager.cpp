#include "DailySummaryManager.h"
#include "DiscordNotifier.h"
#include "DrinkDetector.h"
#include "TimeManager.h"
#include "app_types.h"
#include "StorageLock.h"

void DailySummaryManager::init(DiscordNotifier& discord, DrinkDetector& detector,
                                TimeManager& time, const AppConfig& cfg) {
    _discord  = &discord;
    _detector = &detector;
    _time     = &time;
    _cfg      = &cfg;

    Preferences prefs;
    if (lockNvs()) {
        prefs.begin("daily_sum", false);
        _lastSettledKey = prefs.getString("settled", "");
        prefs.end();
        unlockNvs();
    }

    _markerQueue = xQueueCreate(1, 16);
    if (!_markerQueue || xTaskCreate(_markerTaskFunc, "storage_daily", 2304,
                                     this, 1, &_markerTask) != pdPASS) {
        Serial.println("[DailySummary] marker worker unavailable");
        _markerQueue = nullptr;
    }

}

void DailySummaryManager::update() {
    bool expected = false;
    if (!_updateBusy.compare_exchange_strong(expected, true,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
        return;
    }
    struct UpdateRelease {
        std::atomic<bool>& busy;
        ~UpdateRelease() { busy.store(false, std::memory_order_release); }
    } release{_updateBusy};

    if (!_discord || !_detector || !_time || !_cfg) return;
    if (!_time->isSynced()) return;
    if (!_detector->isPersistenceReady()) return;

    if (_stage != SettlementStage::IDLE) {
        _progressSettlement();
        return;
    }

    struct tm now;
    if (!_time->getLocalTm(now)) return;

    const String today = _time->getDateString();
    if (_detector->hasPreviousPeriodCounters()) {
        String summaryDate = _detector->getRestoredPeriod();
        _beginSettlement(summaryDate, today);
        return;
    }

    if (now.tm_hour != 0) return;
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
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &yesterday);

    _beginSettlement(String(dateStr), _time->getDateString());
}

void DailySummaryManager::_beginSettlement(const String& summaryDate,
                                           const String& settledKey) {
    if (!_markerQueue) {
        static bool warned = false;
        if (!warned) {
            Serial.println("[DailySummary] settlement deferred: marker worker unavailable");
            warned = true;
        }
        return;
    }
    _pendingTotalMl = _detector->getTodayTotalMl();
    _pendingDrinkCount = _detector->getDrinkCountToday();
    _pendingSummaryDate = summaryDate;
    _pendingSettledKey = settledKey;
    Serial.printf("[DailySummary] settlement %s  total=%.0f ml  count=%u\n",
                  summaryDate.c_str(), _pendingTotalMl, (unsigned)_pendingDrinkCount);

    _detector->resetDailyCounters();
    _stage = SettlementStage::WAIT_COUNTER;
}

void DailySummaryManager::_progressSettlement() {
    if (_stage == SettlementStage::WAIT_COUNTER) {
        if (!_detector->isPersistenceIdle() || !_markerQueue) return;
        char settled[16] = {};
        strncpy(settled, _pendingSettledKey.c_str(), sizeof(settled) - 1);
        _markerDone.store(false, std::memory_order_release);
        _markerOk.store(false, std::memory_order_release);
        if (xQueueOverwrite(_markerQueue, settled) == pdTRUE)
            _stage = SettlementStage::WAIT_MARKER;
        return;
    }

    if (_stage != SettlementStage::WAIT_MARKER ||
        !_markerDone.load(std::memory_order_acquire)) return;
    if (!_markerOk.load(std::memory_order_acquire)) {
        Serial.println("[DailySummary] settled marker write failed; retrying");
        _stage = SettlementStage::WAIT_COUNTER;
        return;
    }

    _lastSettledKey = _pendingSettledKey;
    const bool notified = _discord->notifyDailySummary(
        _pendingTotalMl, _pendingDrinkCount, _pendingSummaryDate);
    if (!notified)
        Serial.println("[DailySummary] Notification skipped (WiFi/webhook); counters reset anyway");
    _stage = SettlementStage::IDLE;
}

void DailySummaryManager::_markerTaskFunc(void* param) {
    static_cast<DailySummaryManager*>(param)->_markerTaskLoop();
}

void DailySummaryManager::_markerTaskLoop() {
    char settled[16];
    for (;;) {
        if (xQueueReceive(_markerQueue, settled, portMAX_DELAY) != pdTRUE) continue;
        bool ok = false;
        if (lockNvs()) {
            Preferences prefs;
            if (prefs.begin("daily_sum", false)) {
                ok = prefs.putString("settled", settled) == strlen(settled);
                prefs.end();
            }
            unlockNvs();
        }
        _markerOk.store(ok, std::memory_order_release);
        _markerDone.store(true, std::memory_order_release);
    }
}
