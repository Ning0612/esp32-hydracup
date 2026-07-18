#include <Arduino.h>
#include <Wire.h>
#include <LittleFS.h>

static fs::LittleFSFS LogFS;

#include "AppState.h"
#include "app_types.h"
#include "BuzzerController.h"
#include "ConfigManager.h"
#include "ConfigPortal.h"
#include "DashboardServer.h"
#include "DisplayManager.h"
#include "DailySummaryManager.h"
#include "DiscordNotifier.h"
#include "DrinkDetector.h"
#include "EventLogger.h"
#include "MqttPublisher.h"
#include "ReminderManager.h"
#include "RuntimeCoordinator.h"
#include "ScaleManager.h"
#include "TimeManager.h"
#include "WiFiManager.h"
#include "config.h"
#include "pins.h"
#include "version.h"

static AppState         appState;
static AppConfig        appConfig;
static CupState         s_prevCupState    = CupState::NO_CUP;
static uint32_t         s_prevReminderSec = 1;
static ConfigManager    configManager;
static BuzzerController buzzerController;
static DisplayManager   displayManager;
static ScaleManager     scaleManager;
static WiFiManager      wifiManager;
static ConfigPortal     configPortal;
static DashboardServer  dashboardServer;
static DrinkDetector    drinkDetector;
static ReminderManager  reminderManager;
static TimeManager      timeManager;
static DiscordNotifier     discordNotifier;
static EventLogger         eventLogger;
static DailySummaryManager dailySummaryManager;
static MqttPublisher       mqttPublisher;
static RuntimeCoordinator  runtimeCoordinator;
static TaskHandle_t        s_controlTaskHandle = nullptr;
static uint32_t            s_controlHeartbeat = 0;
static uint32_t            s_dailyGoalMl = DEFAULT_DAILY_GOAL_ML;
static uint32_t            s_pendingTareRequestId = 0;
static bool                s_onlineNotified = false;

static void replyControl(const ControlCommand& command, ControlResultStatus status) {
    ControlResult result;
    result.requestId = command.requestId;
    result.status = status;
    result.calibrationFactor = scaleManager.getCalibrationFactor();
    result.tareOffset = scaleManager.getTareOffset();
    result.weightGrams = scaleManager.getWeightGrams();
    runtimeCoordinator.reply(result);
}

static void processControlCommands() {
    ControlCommand command;
    for (uint8_t i = 0; i < 4 && runtimeCoordinator.receive(command); i++) {
        switch (command.type) {
            case ControlCommandType::TARE:
                if (s_pendingTareRequestId || scaleManager.isTareRunning()) {
                    replyControl(command, ControlResultStatus::BUSY);
                } else if (!scaleManager.isReady() || !scaleManager.isSamplesReady()) {
                    replyControl(command, ControlResultStatus::NOT_READY);
                } else if (scaleManager.startTare()) {
                    s_pendingTareRequestId = command.requestId;
                    reminderManager.resetTimer();
                    buzzerController.stop();
                } else {
                    replyControl(command, ControlResultStatus::FAILED);
                }
                break;

            case ControlCommandType::CALIBRATE:
                if (scaleManager.isTareRunning()) {
                    replyControl(command, ControlResultStatus::BUSY);
                } else if (!scaleManager.isReady() || !scaleManager.isSamplesReady()) {
                    replyControl(command, ControlResultStatus::NOT_READY);
                } else if (command.floatValue <= 0.0f ||
                           scaleManager.getRawAverage() == scaleManager.getTareOffset()) {
                    replyControl(command, ControlResultStatus::FAILED);
                } else {
                    scaleManager.calibrateWithKnownWeight(command.floatValue);
                    replyControl(command, ControlResultStatus::OK);
                }
                break;

            case ControlCommandType::SET_DAILY_GOAL_ML:
                s_dailyGoalMl = command.uintValue;
                mqttPublisher.setDailyGoal(command.uintValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_REMINDER_ENABLED:
                reminderManager.setEnabled(command.boolValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_REMINDER_INTERVAL_MIN:
                reminderManager.setIntervalMin(command.uintValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_REMINDER_ALERT_TIMEOUT_SEC:
                reminderManager.setAlertTimeoutSec(command.uintValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_BUZZER_ENABLED:
                buzzerController.setEnabled(command.boolValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_BUZZER_FREQUENCY_HZ:
                buzzerController.setFrequency(command.uintValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_BUZZER_DURATION_MS:
                buzzerController.setDuration(command.uintValue);
                replyControl(command, ControlResultStatus::OK);
                break;
            case ControlCommandType::SET_BUZZER_VOLUME_PERCENT:
                buzzerController.setVolume((uint8_t)command.uintValue);
                replyControl(command, ControlResultStatus::OK);
                break;
        }
    }
}

static void runControlIteration() {
    processControlCommands();
    buzzerController.update();
    scaleManager.update();
    timeManager.update();

    long tareOffset;
    if (scaleManager.takeTareResult(tareOffset) && s_pendingTareRequestId) {
        drinkDetector.resetScaleBaseline();
        ControlResult result;
        result.requestId = s_pendingTareRequestId;
        result.status = ControlResultStatus::OK;
        result.calibrationFactor = scaleManager.getCalibrationFactor();
        result.tareOffset = tareOffset;
        result.weightGrams = scaleManager.getWeightGrams();
        runtimeCoordinator.reply(result);
        s_pendingTareRequestId = 0;
    } else if (scaleManager.takeTareFailure() && s_pendingTareRequestId) {
        ControlResult result;
        result.requestId = s_pendingTareRequestId;
        result.status = ControlResultStatus::FAILED;
        result.calibrationFactor = scaleManager.getCalibrationFactor();
        result.tareOffset = scaleManager.getTareOffset();
        result.weightGrams = scaleManager.getWeightGrams();
        runtimeCoordinator.reply(result);
        s_pendingTareRequestId = 0;
    }

    const bool persistenceWasInitialized = drinkDetector.isPersistenceInitialized();
    if (persistenceWasInitialized) dailySummaryManager.update();
    if (!scaleManager.isTareRunning()) drinkDetector.update();
    if (!persistenceWasInitialized && drinkDetector.isPersistenceInitialized())
        dailySummaryManager.update();
    reminderManager.update();

    appState.weightGrams = scaleManager.getWeightGrams();
    appState.nextReminderSec = reminderManager.getNextReminderSec();
    appState.ntpSynced = timeManager.isSynced();

    // Discord uses verified TLS; wait until the RTC has a trustworthy time so
    // certificate validity checks do not make the boot notification fail.
    if (appState.mode == AppMode::NORMAL && !s_onlineNotified && appState.ntpSynced) {
        discordNotifier.notifyOnline(appState.ipAddress);
        s_onlineNotified = true;
    }

    if (appState.mode == AppMode::NORMAL) {
        const CupState currentCupState = drinkDetector.getCupState();
        if (currentCupState != s_prevCupState) {
            if (currentCupState == CupState::NO_CUP || s_prevCupState == CupState::NO_CUP)
                displayManager.wake();
            s_prevCupState = currentCupState;
        }
        if (appState.nextReminderSec == 0 && s_prevReminderSec > 0) displayManager.wake();
        s_prevReminderSec = appState.nextReminderSec;

        const RuntimeSnapshot connectivity = runtimeCoordinator.snapshot();
        displayManager.showNormalMode(
            appState.weightGrams, scaleManager.isStable(),
            appState.todayTotalMl, s_dailyGoalMl, appState.drinkCountToday,
            appState.lastDrinkMl, appState.nextReminderSec,
            connectivity.wifiConnected, connectivity.ipAddress, appState.ntpSynced);
    }
    displayManager.update();

    RuntimeSnapshot snapshot;
    snapshot.controlHeartbeat = ++s_controlHeartbeat;
    snapshot.mode = appState.mode;
    snapshot.fsOk = appState.fsOk;
    snapshot.logFsOk = appState.logFsOk;
    snapshot.oledOk = appState.oledOk;
    snapshot.hx711Ok = appState.hx711Ok;
    snapshot.buzzerOk = appState.buzzerOk;
    snapshot.ntpSynced = appState.ntpSynced;
    snapshot.scaleStable = scaleManager.isStable();
    snapshot.scaleSamplesReady = scaleManager.isSamplesReady();
    snapshot.tareRunning = scaleManager.isTareRunning();
    snapshot.weightGrams = appState.weightGrams;
    snapshot.todayTotalMl = appState.todayTotalMl;
    snapshot.lastDrinkMl = appState.lastDrinkMl;
    snapshot.calibrationFactor = scaleManager.getCalibrationFactor();
    snapshot.tareOffset = scaleManager.getTareOffset();
    snapshot.cupState = appState.cupState;
    snapshot.drinkCountToday = appState.drinkCountToday;
    snapshot.nextReminderSec = appState.nextReminderSec;
    runtimeCoordinator.publishControl(snapshot);
}

static void controlTask(void*) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        runControlIteration();
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(10));
    }
}

static void initFilesystem() {
    if (LittleFS.begin(false, "/webfs", 5, "webfs")) {
        appState.fsOk = true;
        return;
    }
    Serial.println("[WARN] webfs mount failed, attempting format...");
    if (LittleFS.format() && LittleFS.begin(false, "/webfs", 5, "webfs")) {
        appState.fsOk = true;
        Serial.println("[WARN] webfs formatted (web assets cleared, run uploadfs)");
    } else {
        appState.fsOk = false;
        Serial.println("[ERROR] webfs unavailable");
    }
}

static void initLogFilesystem() {
    if (LogFS.begin(true, "/logfs", 5, "logfs")) {
        appState.logFsOk = true;
        Serial.println("[INFO] logfs OK");
    } else {
        appState.logFsOk = false;
        Serial.println("[ERROR] logfs unavailable — drink logging disabled");
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD_RATE);
    Serial.printf("\n[INFO] Water Tracker v%s booting\n", APP_VERSION);

    initFilesystem();
    initLogFilesystem();
    configManager.load(appConfig);
    s_dailyGoalMl = appConfig.dailyGoalMl;
    const bool runtimeReady = runtimeCoordinator.begin();

    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

    appState.oledOk = displayManager.init();
    if (appState.oledOk) {
        Serial.println("[INFO] OLED OK");
        displayManager.showBootScreen();
    } else {
        Serial.println("[WARN] OLED init failed");
    }

    scaleManager.init(appConfig.calibrationFactor, appConfig.tareOffset,
                      appConfig.stableToleranceGram, appConfig.stableDurationMs);
    appState.hx711Ok = scaleManager.isReady();
    if (appState.hx711Ok) {
        Serial.printf("[INFO] HX711 OK  factor=%.4f  tare=%ld\n",
                      appConfig.calibrationFactor, appConfig.tareOffset);
    } else {
        Serial.println("[ERROR] HX711 not found");
        if (appState.oledOk) displayManager.showError("HX711 not found");
    }

    appState.buzzerOk = buzzerController.init(appConfig.buzzerFrequencyHz,
                                              appConfig.buzzerVolumePercent);
    buzzerController.setDuration(appConfig.buzzerDurationMs);
    buzzerController.setEnabled(appConfig.buzzerEnabled);

    reminderManager.init(appConfig.reminderIntervalMin, appConfig.reminderEnabled);
    reminderManager.setAlertTimeoutSec(appConfig.reminderAlertTimeoutSec);
    reminderManager.setBuzzer(&buzzerController);
    reminderManager.setAppState(&appState);
    drinkDetector.init(scaleManager, appState, appConfig, reminderManager, buzzerController);

    // WiFi mode selection
    bool wifiConnected = false;
    if (!appConfig.wifiSsid.isEmpty()) {
        displayManager.showWifiConnecting(appConfig.wifiSsid);
        Serial.printf("[INFO] Connecting to WiFi: %s\n", appConfig.wifiSsid.c_str());
        wifiConnected = wifiManager.connectSTA(appConfig.wifiSsid, appConfig.wifiPassword, 10000);
    }

    if (wifiConnected) {
        appState.mode          = AppMode::NORMAL;
        appState.wifiConnected = true;
        appState.ipAddress     = wifiManager.getIP();
        if (appConfig.ntpEnabled) timeManager.init(appConfig);
        discordNotifier.init(appState, appConfig);
        if (appConfig.mqttEnabled) mqttPublisher.init(appState, appConfig);
        eventLogger.init(appState.logFsOk, LogFS);
        drinkDetector.setTimeManager(&timeManager);
        drinkDetector.setDiscordNotifier(&discordNotifier);
        drinkDetector.setEventLogger(&eventLogger);
        drinkDetector.setMqttPublisher(&mqttPublisher);
        mqttPublisher.setTimeManager(&timeManager);
        dailySummaryManager.init(discordNotifier, drinkDetector, timeManager, appConfig);
        dashboardServer.begin(scaleManager, configManager, appState, appConfig,
                              buzzerController, reminderManager, LogFS,
                              runtimeCoordinator, eventLogger, discordNotifier);
        runtimeCoordinator.publishConnectivity(true, appState.ipAddress);
        Serial.printf("[INFO] Normal Mode  IP: %s\n", appState.ipAddress.c_str());
        displayManager.sleep();
        s_prevCupState    = drinkDetector.getCupState();
        s_prevReminderSec = reminderManager.getNextReminderSec();
    } else {
        const bool apOk = wifiManager.startAP(appConfig.apSsid, appConfig.apPassword);
        appState.mode      = AppMode::AP_MODE;
        appState.ipAddress = wifiManager.getAPIP();
        runtimeCoordinator.publishConnectivity(false, appState.ipAddress);
        if (apOk) {
            configPortal.begin(configManager, appState, appConfig);
            displayManager.showAPMode(appConfig.apSsid, appConfig.apPassword, appState.ipAddress);
            Serial.printf("[INFO] AP Mode  SSID: %s  IP: %s\n",
                          appConfig.apSsid.c_str(), appState.ipAddress.c_str());
        } else {
            Serial.println("[ERROR] AP start failed");
            if (appState.oledOk) displayManager.showError("AP start failed");
        }
    }

    // Single boot completion sound
    if (!appState.hx711Ok) {
        buzzerController.play(BeepPattern::ERROR_BEEP);
    } else if (wifiConnected) {
        buzzerController.play(BeepPattern::WIFI_CONNECTED);
    } else {
        buzzerController.play(BeepPattern::AP_MODE);
    }

    if (runtimeReady) {
        // Claim the control domain before creating the task so loop() cannot
        // run a second control iteration during the task hand-off window.
        runtimeCoordinator.setControlRunning(true);
        if (xTaskCreatePinnedToCore(controlTask, "hydracup_control", 8192, nullptr, 3,
                                    &s_controlTaskHandle, ARDUINO_RUNNING_CORE) != pdPASS) {
            runtimeCoordinator.setControlRunning(false);
            Serial.println("[ERROR] RTOS control task creation failed; degraded loop control active (tare/calibrate unavailable)");
            s_controlTaskHandle = nullptr;
        }
    }

    Serial.println("[INFO] Boot complete");
}

void loop() {
    if (!runtimeCoordinator.isControlRunning()) runControlIteration();

    if (appState.mode == AppMode::NORMAL) {
        wifiManager.loop();
        dashboardServer.loop();
        static bool lastWifiConnected = true;
        static uint32_t lastConnectivityMs = 0;
        const bool wifiConnected = wifiManager.isConnected();
        if (wifiConnected != lastWifiConnected || millis() - lastConnectivityMs >= 1000) {
            lastWifiConnected = wifiConnected;
            lastConnectivityMs = millis();
            runtimeCoordinator.publishConnectivity(wifiConnected, appState.ipAddress);
        }
        discordNotifier.update();
        const RuntimeSnapshot snapshot = runtimeCoordinator.snapshot();
        mqttPublisher.loop(snapshot.todayTotalMl);
    } else {
        configPortal.loop();
    }

    static uint32_t lastHealthMs = 0;
    if (millis() - lastHealthMs >= 30000) {
        lastHealthMs = millis();
        const RuntimeSnapshot snapshot = runtimeCoordinator.snapshot();
        const UBaseType_t stackMargin = s_controlTaskHandle
            ? uxTaskGetStackHighWaterMark(s_controlTaskHandle) : 0;
        Serial.printf("[RTOS] heartbeat=%lu stack_free=%u heap=%u min_heap=%u cmd_drop=%lu result_drop=%lu\n",
                      (unsigned long)snapshot.controlHeartbeat,
                      (unsigned)stackMargin,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMinFreeHeap(),
                      (unsigned long)snapshot.commandDrops,
                      (unsigned long)snapshot.resultDrops);
        if (runtimeCoordinator.isControlRunning() && !runtimeCoordinator.isControlHealthy())
            Serial.println("[RTOS][ERROR] control task heartbeat stalled");
    }
    vTaskDelay(pdMS_TO_TICKS(1));
}
