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
#include "ReminderManager.h"
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

    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

    appState.oledOk = displayManager.init();
    if (appState.oledOk) {
        Serial.println("[INFO] OLED OK");
        displayManager.showBootScreen();
    } else {
        Serial.println("[WARN] OLED init failed");
    }

    scaleManager.setConfigManager(&configManager);
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
        dashboardServer.begin(scaleManager, configManager, appState, appConfig,
                              buzzerController, reminderManager);
        if (appConfig.ntpEnabled) timeManager.init(appConfig);
        discordNotifier.init(appState, appConfig);
        discordNotifier.notifyOnline(appState.ipAddress);
        eventLogger.init(appState.logFsOk, LogFS);
        drinkDetector.setTimeManager(&timeManager);
        drinkDetector.setDiscordNotifier(&discordNotifier);
        drinkDetector.setEventLogger(&eventLogger);
        dailySummaryManager.init(discordNotifier, drinkDetector, timeManager, appConfig);
        Serial.printf("[INFO] Normal Mode  IP: %s\n", appState.ipAddress.c_str());
        displayManager.sleep();
        s_prevCupState    = drinkDetector.getCupState();
        s_prevReminderSec = reminderManager.getNextReminderSec();
    } else {
        const bool apOk = wifiManager.startAP(appConfig.apSsid, appConfig.apPassword);
        appState.mode      = AppMode::AP_MODE;
        appState.ipAddress = wifiManager.getAPIP();
        if (apOk) {
            configPortal.begin(configManager, appState);
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

    Serial.println("[INFO] Boot complete");
}

void loop() {
    buzzerController.update();
    scaleManager.update();
    drinkDetector.update();
    reminderManager.update();

    appState.weightGrams     = scaleManager.getWeightGrams();
    appState.nextReminderSec = reminderManager.getNextReminderSec();

    if (appState.mode == AppMode::NORMAL) {
        // Wake display on cup pick-up/put-down or reminder fire
        {
            const CupState curCupState = drinkDetector.getCupState();
            if (curCupState != s_prevCupState) {
                if (curCupState == CupState::NO_CUP || s_prevCupState == CupState::NO_CUP)
                    displayManager.wake();
                s_prevCupState = curCupState;
            }
            if (appState.nextReminderSec == 0 && s_prevReminderSec > 0)
                displayManager.wake();
            s_prevReminderSec = appState.nextReminderSec;
        }

        wifiManager.loop();
        dashboardServer.loop();
        appState.wifiConnected = wifiManager.isConnected();
        timeManager.update();
        appState.ntpSynced = timeManager.isSynced();
        discordNotifier.update();
        dailySummaryManager.update();

        displayManager.showNormalMode(
            appState.weightGrams,    scaleManager.isStable(),
            appState.todayTotalMl,   appConfig.dailyGoalMl,   appState.drinkCountToday,
            appState.lastDrinkMl,    appState.nextReminderSec,
            appState.wifiConnected,  appState.ipAddress,       appState.ntpSynced);
    } else {
        configPortal.loop();
    }

    displayManager.update();
}
