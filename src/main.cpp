#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AppState.h"
#include "BuzzerController.h"
#include "ConfigManager.h"
#include "ConfigPortal.h"
#include "DashboardServer.h"
#include "DailySummaryManager.h"
#include "DiscordNotifier.h"
#include "DisplayManager.h"
#include "DrinkDetector.h"
#include "EventLogger.h"
#include "MqttPublisher.h"
#include "ReminderManager.h"
#include "RuntimeCoordinator.h"
#include "ScaleManager.h"
#include "TimeManager.h"
#include "WiFiManager.h"
#include "config.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_log.h"
#include "hal_time.h"
#include "nvs_flash.h"
#include "version.h"

namespace {
constexpr const char* TAG = "HydraCup";
AppState appState;
AppConfig appConfig;
ConfigManager configManager;
BuzzerController buzzerController;
DisplayManager displayManager;
ScaleManager scaleManager;
WiFiManager wifiManager;
ConfigPortal configPortal;
DashboardServer dashboardServer;
DrinkDetector drinkDetector;
ReminderManager reminderManager;
TimeManager timeManager;
DiscordNotifier discordNotifier;
EventLogger eventLogger;
DailySummaryManager dailySummaryManager;
MqttPublisher mqttPublisher;
RuntimeCoordinator runtimeCoordinator;
TaskHandle_t controlTaskHandle = nullptr;
uint32_t controlHeartbeat = 0;
uint32_t dailyGoalMl = DEFAULT_DAILY_GOAL_ML;
uint32_t pendingTareRequestId = 0;
bool onlineNotified = false;

void mountFilesystems() {
    esp_vfs_littlefs_conf_t web = {};
    web.base_path = "/webfs"; web.partition_label = "webfs"; web.format_if_mount_failed = false; web.dont_mount = false;
    esp_err_t result = esp_vfs_littlefs_register(&web);
    if (result == ESP_OK) appState.fsOk = true;
    else LOG_WARN(TAG, "webfs mount failed: %s", esp_err_to_name(result));
    esp_vfs_littlefs_conf_t log = {};
    log.base_path = "/logfs"; log.partition_label = "logfs"; log.format_if_mount_failed = false; log.dont_mount = false;
    result = esp_vfs_littlefs_register(&log);
    if (result == ESP_OK) appState.logFsOk = true;
    else LOG_WARN(TAG, "logfs mount failed: %s", esp_err_to_name(result));
}

void replyControl(const ControlCommand& command, ControlResultStatus status) {
    ControlResult result; result.requestId = command.requestId; result.status = status; result.calibrationFactor = scaleManager.getCalibrationFactor(); result.tareOffset = scaleManager.getTareOffset(); result.weightGrams = scaleManager.getWeightGrams(); runtimeCoordinator.reply(result);
}

void processControlCommands() {
    ControlCommand command;
    for (uint8_t i = 0; i < 4 && runtimeCoordinator.receive(command); ++i) {
        switch (command.type) {
            case ControlCommandType::TARE:
                if (pendingTareRequestId || scaleManager.isTareRunning()) replyControl(command, ControlResultStatus::BUSY);
                else if (!scaleManager.isReady() || !scaleManager.isSamplesReady()) replyControl(command, ControlResultStatus::NOT_READY);
                else if (scaleManager.startTare()) { pendingTareRequestId = command.requestId; reminderManager.resetTimer(); buzzerController.stop(); }
                else replyControl(command, ControlResultStatus::FAILED);
                break;
            case ControlCommandType::CALIBRATE:
                if (scaleManager.isTareRunning()) replyControl(command, ControlResultStatus::BUSY);
                else if (!scaleManager.isReady() || !scaleManager.isSamplesReady() || command.floatValue <= 0.0f || scaleManager.getRawAverage() == scaleManager.getTareOffset()) replyControl(command, ControlResultStatus::FAILED);
                else { scaleManager.calibrateWithKnownWeight(command.floatValue); replyControl(command, ControlResultStatus::OK); }
                break;
            case ControlCommandType::SET_DAILY_GOAL_ML: dailyGoalMl = command.uintValue; mqttPublisher.setDailyGoal(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_REMINDER_ENABLED: reminderManager.setEnabled(command.boolValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_REMINDER_INTERVAL_MIN: reminderManager.setIntervalMin(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_REMINDER_ALERT_TIMEOUT_SEC: reminderManager.setAlertTimeoutSec(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_ENABLED: buzzerController.setEnabled(command.boolValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_FREQUENCY_HZ: buzzerController.setFrequency(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_DURATION_MS: buzzerController.setDuration(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_VOLUME_PERCENT: buzzerController.setVolume(static_cast<uint8_t>(command.uintValue)); replyControl(command, ControlResultStatus::OK); break;
        }
    }
}

void runControlIteration() {
    processControlCommands(); buzzerController.update(); scaleManager.update(); timeManager.update();
    long tareOffset = 0;
    if (scaleManager.takeTareResult(tareOffset) && pendingTareRequestId) { drinkDetector.resetScaleBaseline(); ControlResult result; result.requestId = pendingTareRequestId; result.status = ControlResultStatus::OK; result.calibrationFactor = scaleManager.getCalibrationFactor(); result.tareOffset = tareOffset; result.weightGrams = scaleManager.getWeightGrams(); runtimeCoordinator.reply(result); pendingTareRequestId = 0; }
    else if (scaleManager.takeTareFailure() && pendingTareRequestId) { ControlResult result; result.requestId = pendingTareRequestId; result.status = ControlResultStatus::FAILED; result.calibrationFactor = scaleManager.getCalibrationFactor(); result.tareOffset = scaleManager.getTareOffset(); result.weightGrams = scaleManager.getWeightGrams(); runtimeCoordinator.reply(result); pendingTareRequestId = 0; }
    const bool persistenceReadyBefore = drinkDetector.isPersistenceInitialized(); if (persistenceReadyBefore) dailySummaryManager.update(); if (!scaleManager.isTareRunning()) drinkDetector.update(); if (!persistenceReadyBefore && drinkDetector.isPersistenceInitialized()) dailySummaryManager.update(); reminderManager.update();
    appState.weightGrams = scaleManager.getWeightGrams(); appState.nextReminderSec = reminderManager.getNextReminderSec(); appState.ntpSynced = timeManager.isSynced();
    if (appState.mode == AppMode::NORMAL && !onlineNotified && appState.ntpSynced) { discordNotifier.notifyOnline(appState.ipAddress); onlineNotified = true; }
    static CupState previousCup = CupState::NO_CUP; static uint32_t previousReminder = 1;
    if (appState.mode == AppMode::NORMAL) { const CupState current = drinkDetector.getCupState(); if (current != previousCup) { if (current == CupState::NO_CUP || previousCup == CupState::NO_CUP) displayManager.wake(); previousCup = current; } if (appState.nextReminderSec == 0 && previousReminder > 0) displayManager.wake(); previousReminder = appState.nextReminderSec; const RuntimeSnapshot state = runtimeCoordinator.snapshot(); displayManager.showNormalMode(appState.weightGrams, scaleManager.isStable(), appState.todayTotalMl, dailyGoalMl, appState.drinkCountToday, appState.lastDrinkMl, appState.nextReminderSec, state.wifiConnected, state.ipAddress, appState.ntpSynced); }
    displayManager.update();
    RuntimeSnapshot snapshot; snapshot.controlHeartbeat = ++controlHeartbeat; snapshot.mode = appState.mode; snapshot.fsOk = appState.fsOk; snapshot.logFsOk = appState.logFsOk; snapshot.oledOk = appState.oledOk; snapshot.hx711Ok = appState.hx711Ok; snapshot.buzzerOk = appState.buzzerOk; snapshot.ntpSynced = appState.ntpSynced; snapshot.scaleStable = scaleManager.isStable(); snapshot.scaleSamplesReady = scaleManager.isSamplesReady(); snapshot.tareRunning = scaleManager.isTareRunning(); snapshot.weightGrams = appState.weightGrams; snapshot.todayTotalMl = appState.todayTotalMl; snapshot.lastDrinkMl = appState.lastDrinkMl; snapshot.calibrationFactor = scaleManager.getCalibrationFactor(); snapshot.tareOffset = scaleManager.getTareOffset(); snapshot.cupState = appState.cupState; snapshot.drinkCountToday = appState.drinkCountToday; snapshot.nextReminderSec = appState.nextReminderSec; runtimeCoordinator.publishControl(snapshot);
}

void controlTask(void*) { TickType_t last = xTaskGetTickCount(); for (;;) { runControlIteration(); vTaskDelayUntil(&last, pdMS_TO_TICKS(10)); } }

void serviceTask(void*) {
    uint32_t lastHealth = 0; bool lastWifi = appState.wifiConnected;
    for (;;) {
        if (appState.mode == AppMode::NORMAL) { wifiManager.loop(); const bool connected = wifiManager.isConnected(); appState.wifiConnected = connected; appState.ipAddress = connected ? wifiManager.getIP() : "0.0.0.0"; if (connected != lastWifi) { lastWifi = connected; runtimeCoordinator.publishConnectivity(connected, appState.ipAddress); } mqttPublisher.loop(appState.todayTotalMl); }
        const uint32_t now = hal_millis(); if (now - lastHealth >= 30000) { lastHealth = now; const RuntimeSnapshot snapshot = runtimeCoordinator.snapshot(); const UBaseType_t stack = controlTaskHandle ? uxTaskGetStackHighWaterMark(controlTaskHandle) : 0; LOG_INFO("RTOS", "heartbeat=%lu stack_free=%u heap=%u min_heap=%u cmd_drop=%lu result_drop=%lu", static_cast<unsigned long>(snapshot.controlHeartbeat), static_cast<unsigned>(stack), static_cast<unsigned>(esp_get_free_heap_size()), static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)), static_cast<unsigned long>(snapshot.commandDrops), static_cast<unsigned long>(snapshot.resultDrops)); if (!runtimeCoordinator.isControlHealthy()) LOG_WARN("RTOS", "control task heartbeat stalled"); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}

extern "C" void app_main(void) {
    esp_log_level_set("HydraCup", ESP_LOG_INFO);
    LOG_INFO(TAG, "HydraCup v%s booting (ESP-IDF)", APP_VERSION);
    const esp_err_t nvsResult = nvs_flash_init();
    if (nvsResult != ESP_OK) { LOG_ERROR(TAG, "NVS init failed: %s", esp_err_to_name(nvsResult)); return; }
    mountFilesystems(); configManager.load(appConfig); dailyGoalMl = appConfig.dailyGoalMl; runtimeCoordinator.begin();
    appState.oledOk = displayManager.init(); if (appState.oledOk) displayManager.showBootScreen();
    scaleManager.init(appConfig.calibrationFactor, appConfig.tareOffset, appConfig.stableToleranceGram, appConfig.stableDurationMs); appState.hx711Ok = scaleManager.isReady();
    appState.buzzerOk = buzzerController.init(appConfig.buzzerFrequencyHz, appConfig.buzzerVolumePercent); buzzerController.setDuration(appConfig.buzzerDurationMs); buzzerController.setEnabled(appConfig.buzzerEnabled);
    reminderManager.init(appConfig.reminderIntervalMin, appConfig.reminderEnabled); reminderManager.setAlertTimeoutSec(appConfig.reminderAlertTimeoutSec); reminderManager.setBuzzer(&buzzerController); reminderManager.setAppState(&appState); drinkDetector.init(scaleManager, appState, appConfig, reminderManager, buzzerController);
    const bool connected = !appConfig.wifiSsid.empty() && wifiManager.connectSTA(appConfig.wifiSsid, appConfig.wifiPassword, 10000);
    if (connected) {
        appState.mode = AppMode::NORMAL; appState.wifiConnected = true; appState.ipAddress = wifiManager.getIP();
        if (appConfig.ntpEnabled) timeManager.init(appConfig);
        discordNotifier.init(appState, appConfig);
        if (appConfig.mqttEnabled) mqttPublisher.init(appState, appConfig);
        eventLogger.init(appState.logFsOk);
        drinkDetector.setTimeManager(&timeManager); drinkDetector.setDiscordNotifier(&discordNotifier); drinkDetector.setEventLogger(&eventLogger); drinkDetector.setMqttPublisher(&mqttPublisher); mqttPublisher.setTimeManager(&timeManager);
        dailySummaryManager.init(discordNotifier, drinkDetector, timeManager, appConfig); dashboardServer.begin(scaleManager, configManager, appState, appConfig, buzzerController, reminderManager, appState.logFsOk, runtimeCoordinator, eventLogger, discordNotifier, wifiManager); runtimeCoordinator.publishConnectivity(true, appState.ipAddress); displayManager.sleep(); LOG_INFO(TAG, "normal mode IP=%s", appState.ipAddress.c_str());
    } else {
        const bool ap = wifiManager.startAP(appConfig.apSsid, appConfig.apPassword); appState.mode = AppMode::AP_MODE; appState.ipAddress = wifiManager.getAPIP(); runtimeCoordinator.publishConnectivity(false, appState.ipAddress); if (ap) { configPortal.begin(configManager, appState, appConfig, wifiManager); displayManager.showAPMode(appConfig.apSsid, appConfig.apPassword, appState.ipAddress); LOG_INFO(TAG, "AP mode SSID=%s IP=%s", appConfig.apSsid.c_str(), appState.ipAddress.c_str()); } else if (appState.oledOk) displayManager.showError("AP FAILED");
    }
    if (!appState.hx711Ok) buzzerController.play(BeepPattern::ERROR_BEEP); else if (connected) buzzerController.play(BeepPattern::WIFI_CONNECTED); else buzzerController.play(BeepPattern::AP_MODE);
    runtimeCoordinator.setControlRunning(true); if (xTaskCreatePinnedToCore(controlTask, "hydracup_control", 8192, nullptr, 3, &controlTaskHandle, 1) != pdPASS) { runtimeCoordinator.setControlRunning(false); LOG_ERROR(TAG, "control task creation failed"); }
    xTaskCreate(serviceTask, "hydracup_service", 4096, nullptr, 1, nullptr); LOG_INFO(TAG, "boot complete");
}
