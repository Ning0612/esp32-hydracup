#pragma once
#include <Arduino.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "AppState.h"
#include "app_types.h"

class TimeManager;

class MqttPublisher {
public:
    void init(AppState& state, const AppConfig& cfg);
    void setTimeManager(TimeManager* tm) { _time = tm; }

    void loop();                                              // main thread; heartbeat timing + enqueue
    void publishStatus(float currentMl, const char* event);   // main thread; build payload + enqueue

private:
    struct PublishMsg {
        char payload[256];
        bool retained;
    };

    static void _taskFunc(void* param);
    void _taskLoop();   // background task only; owns _wifiClient/_mqttClient exclusively
    bool _connect();    // background task only

    AppState*         _state = nullptr;
    const AppConfig*  _cfg   = nullptr;   // main thread only; read for dailyGoalMl/mqttHeartbeatSec
    TimeManager*       _time = nullptr;   // main thread only; read for device_time

    WiFiClient    _wifiClient;   // background task only
    PubSubClient  _mqttClient;   // background task only

    QueueHandle_t _publishQueue = nullptr;
    TaskHandle_t  _taskHandle   = nullptr;

    bool     _enabled       = false;
    uint32_t _lastPublishMs = 0;   // main thread only; heartbeat timing

    // Copied out of AppConfig in init() before the background task starts;
    // the task only ever reads these local copies, never AppConfig/String directly.
    char     _brokerHost[64] = {0};
    uint16_t _brokerPort     = 1883;
    char     _username[32]   = {0};
    char     _password[64]   = {0};
    char     _clientId[32]   = {0};

    static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;
};
