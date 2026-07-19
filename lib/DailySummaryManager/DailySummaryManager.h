#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

class DiscordNotifier;
class DrinkDetector;
class TimeManager;
struct AppConfig;

class DailySummaryManager {
public:
    void init(DiscordNotifier& discord, DrinkDetector& detector,
              TimeManager& time, const AppConfig& cfg);
    void update();

private:
    enum class SettlementStage : uint8_t { IDLE, WAIT_COUNTER, WAIT_MARKER };
    static void _markerTaskFunc(void* param);
    void _markerTaskLoop();
    void _fire(const struct tm& now);
    void _beginSettlement(const std::string& summaryDate, const std::string& settledKey);
    void _progressSettlement();

    DiscordNotifier* _discord = nullptr;
    DrinkDetector* _detector = nullptr;
    TimeManager* _time = nullptr;
    const AppConfig* _cfg = nullptr;
    std::string _lastSettledKey;
    SettlementStage _stage = SettlementStage::IDLE;
    float _pendingTotalMl = 0.0f;
    uint32_t _pendingDrinkCount = 0;
    std::string _pendingSummaryDate;
    std::string _pendingSettledKey;
    QueueHandle_t _markerQueue = nullptr;
    TaskHandle_t _markerTask = nullptr;
    std::atomic<bool> _updateBusy{false};
    std::atomic<bool> _markerDone{false};
    std::atomic<bool> _markerOk{false};
};
