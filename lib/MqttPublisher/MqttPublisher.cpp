#include "MqttPublisher.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "TimeManager.h"
#include "cJSON.h"
#include "hal_log.h"
#include "hal_time.h"

namespace { constexpr const char* TAG = "MQTT"; constexpr const char* STATUS = "hydracup/status"; }

void MqttPublisher::init(AppState& state, const AppConfig& cfg) {
    _state = &state; _dailyGoalMl.store(cfg.dailyGoalMl); _heartbeatSec = cfg.mqttHeartbeatSec;
    _enabled = cfg.mqttEnabled && !cfg.mqttBrokerHost.empty(); _state->mqttConfigured = _enabled; _state->mqttConnected.store(false);
    if (!_enabled) return;
    std::snprintf(_uri, sizeof(_uri), "mqtt://%s:%u", cfg.mqttBrokerHost.c_str(), cfg.mqttBrokerPort);
    std::strncpy(_username, cfg.mqttUsername.c_str(), sizeof(_username) - 1); std::strncpy(_password, cfg.mqttPassword.c_str(), sizeof(_password) - 1); std::strncpy(_clientId, cfg.mqttClientId.c_str(), sizeof(_clientId) - 1);
    _publishQueue = xQueueCreate(8, sizeof(PublishMsg));
    if (!_publishQueue) { _enabled = false; _state->mqttConfigured = false; return; }
    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = _uri;
    config.credentials.client_id = _clientId;
    if (_username[0]) { config.credentials.username = _username; config.credentials.authentication.password = _password; }
    config.session.last_will.topic = "hydracup/availability";
    config.session.last_will.msg = "{\"online\":false}";
    config.session.last_will.msg_len = 16;
    config.session.last_will.qos = 1;
    config.session.last_will.retain = true;
    config.network.reconnect_timeout_ms = 5000;
    _client = esp_mqtt_client_init(&config);
    if (!_client || esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY, &_eventHandler, this) != ESP_OK || esp_mqtt_client_start(_client) != ESP_OK || xTaskCreate(_taskFunc, "mqtt_task", 4096, this, 1, &_taskHandle) != pdPASS) {
        if (_client) esp_mqtt_client_destroy(_client);
        _client = nullptr; _enabled = false; _state->mqttConfigured = false;
        LOG_ERROR(TAG, "initialization failed"); return;
    }
    LOG_INFO(TAG, "configured host=%s port=%u", cfg.mqttBrokerHost.c_str(), cfg.mqttBrokerPort);
}

void MqttPublisher::publishStatus(float currentMl, const char* event) {
    if (!_enabled || !_publishQueue) return;
    cJSON* doc = cJSON_CreateObject(); const uint32_t goal = _dailyGoalMl.load(); const float bounded = std::clamp(currentMl, 0.0f, 9999.0f);
    cJSON_AddNumberToObject(doc, "current_ml", static_cast<int>(bounded)); cJSON_AddNumberToObject(doc, "goal_ml", static_cast<int>(std::min<uint32_t>(goal, 9999))); if (goal) cJSON_AddNumberToObject(doc, "pct", currentMl / static_cast<float>(goal)); cJSON_AddStringToObject(doc, "event", event ? event : "status"); if (_time && _time->isSynced()) cJSON_AddStringToObject(doc, "device_time", _time->getISOTimestamp().c_str());
    char* json = cJSON_PrintUnformatted(doc); cJSON_Delete(doc); if (!json) return; PublishMsg message = {}; std::strncpy(message.payload, json, sizeof(message.payload) - 1); std::free(json); message.retained = true; if (xQueueSend(_publishQueue, &message, 0) != pdTRUE) LOG_WARN(TAG, "publish queue full");
}

void MqttPublisher::loop(float todayTotalMl) {
    if (!_enabled || !_state) return;
    const uint32_t interval = static_cast<uint32_t>(_heartbeatSec) * 1000U;
    const uint32_t now = hal_millis();
    if (interval == 0 || now - _lastPublishMs < interval || !_state->mqttConnected.load()) return;
    _lastPublishMs = now; publishStatus(todayTotalMl, "heartbeat");
}

void MqttPublisher::_taskFunc(void* param) { static_cast<MqttPublisher*>(param)->_taskLoop(); }
void MqttPublisher::_taskLoop() { PublishMsg message = {}; for (;;) { if (xQueueReceive(_publishQueue, &message, portMAX_DELAY) == pdTRUE && _state->mqttConnected.load()) esp_mqtt_client_publish(_client, STATUS, message.payload, 0, 1, message.retained ? 1 : 0); } }
void MqttPublisher::_eventHandler(void* handlerArg, esp_event_base_t, int32_t eventId, void*) { static_cast<MqttPublisher*>(handlerArg)->_onEvent(eventId); }
void MqttPublisher::_onEvent(int32_t eventId) { if (!_state) return; if (eventId == MQTT_EVENT_CONNECTED) { _state->mqttConnected.store(true); esp_mqtt_client_publish(_client, "hydracup/availability", "{\"online\":true}", 0, 1, 1); LOG_INFO(TAG, "connected"); } else if (eventId == MQTT_EVENT_DISCONNECTED) { _state->mqttConnected.store(false); } }
