#include "DailySummaryManager.h"

#include <cstring>

#include "DiscordNotifier.h"
#include "DrinkDetector.h"
#include "StorageLock.h"
#include "TimeManager.h"
#include "app_types.h"
#include "hal_log.h"
#include "hal_time.h"
#include "nvs.h"

void DailySummaryManager::init(DiscordNotifier& discord, DrinkDetector& detector,
                               TimeManager& time, const AppConfig& cfg) {
    _discord = &discord; _detector = &detector; _time = &time; _cfg = &cfg;
    if (lockNvs()) {
        nvs_handle_t handle = 0;
        if (nvs_open("daily_sum", NVS_READONLY, &handle) == ESP_OK) {
            size_t length = 0;
            if (nvs_get_str(handle, "settled", nullptr, &length) == ESP_OK && length > 0) {
                _lastSettledKey.assign(length, '\0');
                if (nvs_get_str(handle, "settled", _lastSettledKey.data(), &length) == ESP_OK)
                    _lastSettledKey.resize(length - 1);
                else _lastSettledKey.clear();
            }
            nvs_close(handle);
        }
        unlockNvs();
    }
    _markerQueue = xQueueCreate(1, 16);
    if (!_markerQueue || xTaskCreate(_markerTaskFunc, "storage_daily", 2304,
                                     this, 1, &_markerTask) != pdPASS) {
        LOG_WARN("DailySummary", "marker worker unavailable");
        _markerQueue = nullptr;
    }
}

void DailySummaryManager::update() {
    bool expected = false;
    if (!_updateBusy.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                              std::memory_order_relaxed)) return;
    struct Release { std::atomic<bool>& busy; ~Release() { busy.store(false); } } release{_updateBusy};
    if (!_discord || !_detector || !_time || !_cfg || !_time->isSynced() ||
        !_detector->isPersistenceReady()) return;
    if (_stage != SettlementStage::IDLE) { _progressSettlement(); return; }
    struct tm now = {};
    if (!_time->getLocalTm(now)) return;
    const std::string today = _time->getDateString();
    if (_detector->hasPreviousPeriodCounters()) {
        _beginSettlement(_detector->getRestoredPeriod(), today); return;
    }
    if (now.tm_hour != 0 || _lastSettledKey == today) return;
    _fire(now);
}

void DailySummaryManager::_fire(const struct tm& now) {
    struct tm copy = now;
    time_t value = mktime(&copy) - 86400;
    struct tm yesterday = {};
    localtime_r(&value, &yesterday);
    char date[12]; strftime(date, sizeof(date), "%Y-%m-%d", &yesterday);
    _beginSettlement(date, _time->getDateString());
}

void DailySummaryManager::_beginSettlement(const std::string& summaryDate,
                                            const std::string& settledKey) {
    if (!_markerQueue) return;
    _pendingTotalMl = _detector->getTodayTotalMl();
    _pendingDrinkCount = _detector->getDrinkCountToday();
    _pendingSummaryDate = summaryDate;
    _pendingSettledKey = settledKey;
    LOG_INFO("DailySummary", "settlement %s total=%.0f count=%u",
             summaryDate.c_str(), _pendingTotalMl, static_cast<unsigned>(_pendingDrinkCount));
    _detector->resetDailyCounters();
    _stage = SettlementStage::WAIT_COUNTER;
}

void DailySummaryManager::_progressSettlement() {
    if (_stage == SettlementStage::WAIT_COUNTER) {
        if (!_detector->isPersistenceIdle() || !_markerQueue) return;
        char settled[16] = {};
        std::strncpy(settled, _pendingSettledKey.c_str(), sizeof(settled) - 1);
        _markerDone.store(false); _markerOk.store(false);
        if (xQueueOverwrite(_markerQueue, settled) == pdTRUE) _stage = SettlementStage::WAIT_MARKER;
        return;
    }
    if (_stage != SettlementStage::WAIT_MARKER || !_markerDone.load()) return;
    if (!_markerOk.load()) { _stage = SettlementStage::WAIT_COUNTER; return; }
    _lastSettledKey = _pendingSettledKey;
    _discord->notifyDailySummary(_pendingTotalMl, _pendingDrinkCount, _pendingSummaryDate);
    _stage = SettlementStage::IDLE;
}

void DailySummaryManager::_markerTaskFunc(void* param) {
    static_cast<DailySummaryManager*>(param)->_markerTaskLoop();
}

void DailySummaryManager::_markerTaskLoop() {
    char settled[16] = {};
    for (;;) {
        if (xQueueReceive(_markerQueue, settled, portMAX_DELAY) != pdTRUE) continue;
        bool ok = false;
        if (lockNvs()) {
            nvs_handle_t handle = 0;
            if (nvs_open("daily_sum", NVS_READWRITE, &handle) == ESP_OK) {
                ok = nvs_set_str(handle, "settled", settled) == ESP_OK && nvs_commit(handle) == ESP_OK;
                nvs_close(handle);
            }
            unlockNvs();
        }
        _markerOk.store(ok); _markerDone.store(true);
    }
}
