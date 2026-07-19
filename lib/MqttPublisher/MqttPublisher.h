#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "AppState.h"
#include "app_types.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

class TimeManager;

class MqttPublisher {
public:
    void init(AppState& state, const AppConfig& cfg);
    void setTimeManager(TimeManager* tm) { _time = tm; }
    void setDailyGoal(uint32_t dailyGoalMl) { _dailyGoalMl.store(dailyGoalMl); }
    void loop(float todayTotalMl);
    void publishStatus(float currentMl, const char* event);

private:
    struct PublishMsg { char payload[256]; bool retained; };
    static void _taskFunc(void* param);
    static void _eventHandler(void* handlerArg, esp_event_base_t base, int32_t eventId, void* eventData);
    void _taskLoop();
    void _onEvent(int32_t eventId);
    AppState* _state = nullptr;
    TimeManager* _time = nullptr;
    esp_mqtt_client_handle_t _client = nullptr;
    QueueHandle_t _publishQueue = nullptr;
    TaskHandle_t _taskHandle = nullptr;
    bool _enabled = false;
    uint32_t _lastPublishMs = 0;
    std::atomic<uint32_t> _dailyGoalMl{2000};
    uint16_t _heartbeatSec = 60;
    char _uri[128] = {};
    char _username[32] = {};
    char _password[64] = {};
    char _clientId[32] = {};
};
