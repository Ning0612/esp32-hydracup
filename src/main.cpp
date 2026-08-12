#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "AppState.h"
#include "BuzzerController.h"
#include "CloudSyncClient.h"
#include "ConfigManager.h"
#include "ConfigPortal.h"
#include "DashboardServer.h"
#include "DailySummaryManager.h"
#include "DiscordNotifier.h"
#include "DisplayManager.h"
#include "DrinkDetector.h"
#include "EventLogger.h"
#include "MqttPublisher.h"
#include "OtaUpdater.h"
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
CloudSyncClient cloudSyncClient;
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
OtaUpdater otaUpdater;
RuntimeCoordinator runtimeCoordinator;
TaskHandle_t controlTaskHandle = nullptr;
uint32_t controlHeartbeat = 0;
uint32_t dailyGoalMl = DEFAULT_DAILY_GOAL_ML;
uint32_t reminderIntervalMinSetting = DEFAULT_REMINDER_INTERVAL_MIN;
bool reminderEnabledSetting = true;
std::string reminderPausedUntilDate;
uint32_t pendingTareRequestId = 0;
bool onlineNotified = false;

bool validDateString(const char* value) {
    if (!value || std::strlen(value) != 10 || value[4] != '-' || value[7] != '-') return false;
    for (size_t i = 0; i < 10; ++i) {
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    }
    const int month = (value[5] - '0') * 10 + value[6] - '0';
    const int day = (value[8] - '0') * 10 + value[9] - '0';
    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
                     (value[2] - '0') * 10 + value[3] - '0';
    if (month < 1 || month > 12 || day < 1) return false;
    static constexpr uint8_t daysPerMonth[] = {31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};
    int maxDay = daysPerMonth[month - 1];
    if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))) ++maxDay;
    return day <= maxDay;
}

CloudAppliedSettings currentCloudSettings() {
    CloudAppliedSettings settings;
    settings.reminderEnabled = reminderEnabledSetting;
    settings.reminderIntervalMin = reminderIntervalMinSetting;
    settings.dailyGoalMl = dailyGoalMl;
    std::snprintf(settings.pausedUntilDate, sizeof(settings.pausedUntilDate), "%s",
                  reminderPausedUntilDate.c_str());
    return settings;
}

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
                else if (scaleManager.startTare()) { pendingTareRequestId = command.requestId; buzzerController.stop(); }
                else replyControl(command, ControlResultStatus::FAILED);
                break;
            case ControlCommandType::CALIBRATE:
                if (scaleManager.isTareRunning()) replyControl(command, ControlResultStatus::BUSY);
                else if (!scaleManager.isReady() || !scaleManager.isSamplesReady() || command.floatValue <= 0.0f || scaleManager.getRawAverage() == scaleManager.getTareOffset()) replyControl(command, ControlResultStatus::FAILED);
                else { scaleManager.calibrateWithKnownWeight(command.floatValue); replyControl(command, ControlResultStatus::OK); }
                break;
            case ControlCommandType::SET_DAILY_GOAL_ML: dailyGoalMl = command.uintValue; mqttPublisher.setDailyGoal(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_REMINDER_ENABLED: reminderEnabledSetting = command.boolValue; reminderManager.setEnabled(command.boolValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_REMINDER_INTERVAL_MIN: reminderIntervalMinSetting = command.uintValue; reminderManager.setIntervalMin(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_REMINDER_ALERT_TIMEOUT_SEC: reminderManager.setAlertTimeoutSec(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_ENABLED: buzzerController.setEnabled(command.boolValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_FREQUENCY_HZ: buzzerController.setFrequency(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_DURATION_MS: buzzerController.setDuration(command.uintValue); replyControl(command, ControlResultStatus::OK); break;
            case ControlCommandType::SET_BUZZER_VOLUME_PERCENT: buzzerController.setVolume(static_cast<uint8_t>(command.uintValue)); replyControl(command, ControlResultStatus::OK); break;
        }
    }
}

void processCloudCommands() {
    CloudCommand command;
    for (uint8_t i = 0; i < 4 && cloudSyncClient.receiveCommand(command); ++i) {
        bool ok = true;
        switch (command.type) {
            case CloudCommandType::SET_SETTINGS:
                if (command.reminderIntervalMin < 1 || command.reminderIntervalMin > 1440 ||
                    command.dailyGoalMl < 100 || command.dailyGoalMl > 9999) {
                    ok = false;
                } else {
                    reminderEnabledSetting = command.reminderEnabled;
                    reminderIntervalMinSetting = command.reminderIntervalMin;
                    reminderManager.setEnabled(command.reminderEnabled);
                    reminderManager.setIntervalMin(command.reminderIntervalMin);
                    dailyGoalMl = command.dailyGoalMl;
                    mqttPublisher.setDailyGoal(command.dailyGoalMl);
                }
                break;
            case CloudCommandType::SET_REMINDER_ENABLED:
                reminderEnabledSetting = command.boolValue;
                reminderManager.setEnabled(command.boolValue);
                break;
            case CloudCommandType::SET_REMINDER_INTERVAL_MIN:
                if (command.uintValue < 1 || command.uintValue > 1440) ok = false;
                else {
                    reminderIntervalMinSetting = command.uintValue;
                    reminderManager.setIntervalMin(command.uintValue);
                }
                break;
            case CloudCommandType::SET_DAILY_GOAL_ML:
                if (command.uintValue < 100 || command.uintValue > 9999) ok = false;
                else {
                    dailyGoalMl = command.uintValue;
                    mqttPublisher.setDailyGoal(command.uintValue);
                }
                break;
            case CloudCommandType::SNOOZE_MINUTES:
                if (command.uintValue != 20 && command.uintValue != 40 &&
                    command.uintValue != 60) ok = false;
                else reminderManager.snooze(command.uintValue);
                break;
            case CloudCommandType::PAUSE_TODAY:
                if (!validDateString(command.stringValue) ||
                    (timeManager.isSynced() && timeManager.getDateString() != command.stringValue)) {
                    ok = false;
                } else {
                    reminderPausedUntilDate = command.stringValue;
                    reminderManager.setPausedToday(true);
                }
                break;
            default:
                ok = false;
                break;
        }
        cloudSyncClient.acknowledgeCommand(command, ok, currentCloudSettings());
    }
}

void runControlIteration() {
    processControlCommands(); processCloudCommands(); buzzerController.update(); scaleManager.update(); timeManager.update();
    long tareOffset = 0;
    if (scaleManager.takeTareResult(tareOffset) && pendingTareRequestId) { drinkDetector.resetScaleBaseline(); ControlResult result; result.requestId = pendingTareRequestId; result.status = ControlResultStatus::OK; result.calibrationFactor = scaleManager.getCalibrationFactor(); result.tareOffset = tareOffset; result.weightGrams = scaleManager.getWeightGrams(); runtimeCoordinator.reply(result); pendingTareRequestId = 0; }
    else if (scaleManager.takeTareFailure() && pendingTareRequestId) { ControlResult result; result.requestId = pendingTareRequestId; result.status = ControlResultStatus::FAILED; result.calibrationFactor = scaleManager.getCalibrationFactor(); result.tareOffset = scaleManager.getTareOffset(); result.weightGrams = scaleManager.getWeightGrams(); runtimeCoordinator.reply(result); pendingTareRequestId = 0; }
    const bool persistenceReadyBefore = drinkDetector.isPersistenceInitialized(); if (persistenceReadyBefore) dailySummaryManager.update(); if (!scaleManager.isTareRunning() && !appState.otaInProgress.load()) drinkDetector.update(); if (!persistenceReadyBefore && drinkDetector.isPersistenceInitialized()) dailySummaryManager.update(); reminderManager.update();
    appState.weightGrams = scaleManager.getWeightGrams(); appState.nextReminderSec = reminderManager.getNextReminderSec(); appState.ntpSynced = timeManager.isSynced();
    static bool pauseRestored = false;
    if (!pauseRestored && appState.ntpSynced) {
        pauseRestored = true;
        const std::string today = timeManager.getDateString();
        if (!reminderPausedUntilDate.empty() && today <= reminderPausedUntilDate) {
            reminderManager.setPausedToday(true);
        } else if (!reminderPausedUntilDate.empty()) {
            reminderPausedUntilDate.clear();
            cloudSyncClient.persistSettings(currentCloudSettings());
        }
    }
    if (appState.ntpSynced && reminderManager.getState() == ReminderState::PAUSED_TODAY &&
        !reminderPausedUntilDate.empty() &&
        timeManager.getDateString() > reminderPausedUntilDate) {
        reminderPausedUntilDate.clear();
        reminderManager.setPausedToday(false);
        cloudSyncClient.persistSettings(currentCloudSettings());
    }
    CloudDeviceStatus cloudStatus;
    cloudStatus.todayTotalMl = appState.todayTotalMl; cloudStatus.lastDrinkMl = appState.lastDrinkMl;
    cloudStatus.drinkCount = appState.drinkCountToday; cloudStatus.reminderRemainingSec = appState.nextReminderSec;
    cloudStatus.alertEpisodeUptimeMs = reminderManager.getAlertEpisodeUptimeMs();
    cloudStatus.dailyGoalMl = dailyGoalMl; std::strncpy(cloudStatus.cupState, DrinkDetector::cupStateName(appState.cupState), sizeof(cloudStatus.cupState) - 1);
    std::strncpy(cloudStatus.reminderState, reminderManager.getStateName(), sizeof(cloudStatus.reminderState) - 1);
    std::strncpy(cloudStatus.lastDrinkAt, appState.lastDrinkAt, sizeof(cloudStatus.lastDrinkAt) - 1);
    cloudSyncClient.updateStatus(cloudStatus);
    if (appState.mode == AppMode::NORMAL && !onlineNotified && appState.ntpSynced) { const RuntimeSnapshot connectivity = runtimeCoordinator.snapshot(); discordNotifier.notifyOnline(connectivity.ipAddress); onlineNotified = true; }
    static CupState previousCup = CupState::NO_CUP; static uint32_t previousReminder = 1;
    if (appState.mode == AppMode::NORMAL) { const CupState current = drinkDetector.getCupState(); if (current != previousCup) { if (current == CupState::NO_CUP || previousCup == CupState::NO_CUP) displayManager.wake(); previousCup = current; } if (appState.nextReminderSec == 0 && previousReminder > 0) displayManager.wake(); previousReminder = appState.nextReminderSec; const RuntimeSnapshot state = runtimeCoordinator.snapshot(); displayManager.showNormalMode(appState.weightGrams, scaleManager.isStable(), appState.todayTotalMl, dailyGoalMl, appState.drinkCountToday, appState.lastDrinkMl, appState.nextReminderSec, state.wifiConnected, state.ipAddress, appState.ntpSynced); }
    displayManager.update();
    RuntimeSnapshot snapshot; snapshot.controlHeartbeat = ++controlHeartbeat; snapshot.mode = appState.mode; snapshot.fsOk = appState.fsOk; snapshot.logFsOk = appState.logFsOk; snapshot.oledOk = appState.oledOk; snapshot.hx711Ok = appState.hx711Ok; snapshot.buzzerOk = appState.buzzerOk; snapshot.ntpSynced = appState.ntpSynced; snapshot.scaleStable = scaleManager.isStable(); snapshot.scaleSamplesReady = scaleManager.isSamplesReady(); snapshot.tareRunning = scaleManager.isTareRunning(); snapshot.weightGrams = appState.weightGrams; snapshot.todayTotalMl = appState.todayTotalMl; snapshot.lastDrinkMl = appState.lastDrinkMl; snapshot.calibrationFactor = scaleManager.getCalibrationFactor(); snapshot.tareOffset = scaleManager.getTareOffset(); snapshot.cupState = appState.cupState; snapshot.drinkCountToday = appState.drinkCountToday; snapshot.dailyGoalMl = dailyGoalMl; snapshot.reminderIntervalMin = reminderIntervalMinSetting; snapshot.reminderEnabled = reminderEnabledSetting; snapshot.nextReminderSec = appState.nextReminderSec; std::strncpy(snapshot.reminderState, reminderManager.getStateName(), sizeof(snapshot.reminderState) - 1); std::strncpy(snapshot.reminderPausedUntilDate, reminderPausedUntilDate.c_str(), sizeof(snapshot.reminderPausedUntilDate) - 1); std::strncpy(snapshot.lastDrinkAt, appState.lastDrinkAt, sizeof(snapshot.lastDrinkAt) - 1); runtimeCoordinator.publishControl(snapshot);
}

void controlTask(void*) { TickType_t last = xTaskGetTickCount(); for (;;) { runControlIteration(); vTaskDelayUntil(&last, pdMS_TO_TICKS(10)); } }

void serviceTask(void*) {
    uint32_t lastHealth = 0; bool lastWifi = runtimeCoordinator.snapshot().wifiConnected;
    for (;;) {
        if (appState.mode == AppMode::NORMAL) { wifiManager.loop(); const bool connected = wifiManager.isConnected(); const std::string ipAddress = connected ? wifiManager.getIP() : "0.0.0.0"; appState.wifiConnected.store(connected); cloudSyncClient.setConnectivity(connected); if (connected != lastWifi) { lastWifi = connected; runtimeCoordinator.publishConnectivity(connected, ipAddress); } mqttPublisher.loop(runtimeCoordinator.snapshot().todayTotalMl); }
        ota_mark_app_valid_if_due(runtimeCoordinator.isControlHealthy());
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
    ota_boot_check();
    mountFilesystems(); configManager.load(appConfig); dailyGoalMl = appConfig.dailyGoalMl;
    reminderEnabledSetting = appConfig.reminderEnabled;
    reminderIntervalMinSetting = appConfig.reminderIntervalMin;
    reminderPausedUntilDate = appConfig.reminderPausedUntilDate;
    runtimeCoordinator.begin();
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
        if (!cloudSyncClient.init(appState, appConfig, configManager, eventLogger, appState.logFsOk)) {
            LOG_ERROR(TAG, "cloud sync initialization failed");
        }
        drinkDetector.setTimeManager(&timeManager); drinkDetector.setDiscordNotifier(&discordNotifier); drinkDetector.setEventLogger(&eventLogger); drinkDetector.setMqttPublisher(&mqttPublisher); drinkDetector.setCloudSyncClient(&cloudSyncClient); mqttPublisher.setTimeManager(&timeManager);
        dailySummaryManager.init(discordNotifier, drinkDetector, timeManager, appConfig); dashboardServer.begin(scaleManager, configManager, appState, appConfig, buzzerController, reminderManager, appState.logFsOk, runtimeCoordinator, eventLogger, discordNotifier, wifiManager, cloudSyncClient);
        if (otaUpdater.init(appState, runtimeCoordinator)) dashboardServer.setOtaUpdater(otaUpdater);
        runtimeCoordinator.publishConnectivity(true, appState.ipAddress); displayManager.sleep(); LOG_INFO(TAG, "normal mode IP=%s", appState.ipAddress.c_str());
    } else {
        const bool ap = wifiManager.startAP(appConfig.apSsid, appConfig.apPassword); appState.mode = AppMode::AP_MODE; appState.ipAddress = wifiManager.getAPIP(); runtimeCoordinator.publishConnectivity(false, appState.ipAddress); if (ap) { configPortal.begin(configManager, appState, appConfig, wifiManager); displayManager.showAPMode(appConfig.apSsid, appConfig.apPassword, appState.ipAddress); LOG_INFO(TAG, "AP mode SSID=%s IP=%s", appConfig.apSsid.c_str(), appState.ipAddress.c_str()); } else if (appState.oledOk) displayManager.showError("AP FAILED");
    }
    if (!appState.hx711Ok) buzzerController.play(BeepPattern::ERROR_BEEP); else if (connected) buzzerController.play(BeepPattern::WIFI_CONNECTED); else buzzerController.play(BeepPattern::AP_MODE);
    runtimeCoordinator.setControlRunning(true); if (xTaskCreatePinnedToCore(controlTask, "hydracup_control", 8192, nullptr, 3, &controlTaskHandle, 1) != pdPASS) { runtimeCoordinator.setControlRunning(false); LOG_ERROR(TAG, "control task creation failed"); }
    xTaskCreate(serviceTask, "hydracup_service", 4096, nullptr, 1, nullptr); LOG_INFO(TAG, "boot complete");
}
