#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string>
#include "app_types.h"

struct RuntimeSnapshot {
    uint32_t sequence = 0;
    uint32_t controlHeartbeat = 0;
    uint32_t commandDrops = 0;
    uint32_t resultDrops = 0;

    AppMode mode = AppMode::BOOT;
    bool fsOk = false;
    bool logFsOk = false;
    bool oledOk = false;
    bool hx711Ok = false;
    bool buzzerOk = false;
    bool wifiConnected = false;
    bool ntpSynced = false;
    bool scaleStable = false;
    bool scaleSamplesReady = false;
    bool tareRunning = false;

    float weightGrams = 0.0f;
    float todayTotalMl = 0.0f;
    float lastDrinkMl = 0.0f;
    float calibrationFactor = 1.0f;
    long tareOffset = 0;
    CupState cupState = CupState::NO_CUP;
    uint32_t drinkCountToday = 0;
    uint32_t nextReminderSec = 0;
    char ipAddress[16] = {};
};

enum class ControlCommandType : uint8_t {
    TARE,
    CALIBRATE,
    SET_DAILY_GOAL_ML,
    SET_REMINDER_ENABLED,
    SET_REMINDER_INTERVAL_MIN,
    SET_REMINDER_ALERT_TIMEOUT_SEC,
    SET_BUZZER_ENABLED,
    SET_BUZZER_FREQUENCY_HZ,
    SET_BUZZER_DURATION_MS,
    SET_BUZZER_VOLUME_PERCENT
};

struct ControlCommand {
    ControlCommandType type = ControlCommandType::TARE;
    uint32_t requestId = 0;
    uint32_t uintValue = 0;
    float floatValue = 0.0f;
    bool boolValue = false;
};

enum class ControlResultStatus : uint8_t {
    OK,
    BUSY,
    NOT_READY,
    FAILED,
    TIMEOUT,
    QUEUE_FULL
};

struct ControlResult {
    uint32_t requestId = 0;
    ControlResultStatus status = ControlResultStatus::FAILED;
    float calibrationFactor = 0.0f;
    float weightGrams = 0.0f;
    long tareOffset = 0;
};

class RuntimeCoordinator {
public:
    bool begin();

    void publishControl(const RuntimeSnapshot& snapshot);
    void publishConnectivity(bool wifiConnected, const std::string& ipAddress);
    RuntimeSnapshot snapshot() const;

    // Single requester only: DashboardServer owns the synchronous result queue.
    bool request(ControlCommand command, ControlResult& result,
                 TickType_t timeoutTicks = pdMS_TO_TICKS(2500));
    bool receive(ControlCommand& command, TickType_t timeoutTicks = 0);
    void reply(const ControlResult& result);

    void setControlRunning(bool running) { _controlRunning.store(running); }
    bool isControlRunning() const { return _controlRunning.load(); }
    bool isControlHealthy(TickType_t maxAgeTicks = pdMS_TO_TICKS(2000)) const;

private:
    mutable SemaphoreHandle_t _snapshotMutex = nullptr;
    QueueHandle_t _commandQueue = nullptr;
    QueueHandle_t _resultQueue = nullptr;
    RuntimeSnapshot _snapshot;
    uint32_t _nextRequestId = 1;
    std::atomic<bool> _controlRunning{false};
    std::atomic<TickType_t> _lastControlPublishTick{0};
};
