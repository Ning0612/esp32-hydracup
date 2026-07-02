#include "MqttPublisher.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include "TimeManager.h"

namespace {
constexpr const char* TOPIC_STATUS       = "hydracup/status";
constexpr const char* TOPIC_AVAILABILITY = "hydracup/availability";
constexpr const char* LWT_OFFLINE_PAYLOAD = "{\"online\":false}";

// sensitive=true suppresses the length in the log line (avoids leaking a password-length side channel)
void copyTruncated(char* dst, size_t dstSize, const String& src, const char* fieldName, bool sensitive = false) {
    if (src.length() >= dstSize) {
        if (sensitive) {
            Serial.printf("[MQTT] init: %s too long, truncated\n", fieldName);
        } else {
            Serial.printf("[MQTT] init: %s too long (%u bytes), truncated\n",
                          fieldName, (unsigned)src.length());
        }
    }
    strncpy(dst, src.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}
}

void MqttPublisher::init(AppState& state, const AppConfig& cfg) {
    _state = &state;
    _cfg   = &cfg;

    _enabled = cfg.mqttEnabled && !cfg.mqttBrokerHost.isEmpty();
    _state->mqttConfigured = _enabled;
    _state->mqttConnected.store(false, std::memory_order_relaxed);

    if (!_enabled) {
        Serial.println("[MQTT] init: disabled or broker host empty");
        return;
    }

    copyTruncated(_brokerHost, sizeof(_brokerHost), cfg.mqttBrokerHost, "mqttBrokerHost");
    _brokerPort = cfg.mqttBrokerPort;
    copyTruncated(_username, sizeof(_username), cfg.mqttUsername, "mqttUsername");
    copyTruncated(_password, sizeof(_password), cfg.mqttPassword, "mqttPassword", /*sensitive=*/true);
    copyTruncated(_clientId, sizeof(_clientId), cfg.mqttClientId, "mqttClientId");

    Serial.printf("[MQTT] init  host=%s port=%u clientId=%s\n",
                  _brokerHost, _brokerPort, _clientId);

    _publishQueue = xQueueCreate(8, sizeof(PublishMsg));
    if (!_publishQueue) {
        Serial.println("[MQTT] init failed: queue creation failed");
        _enabled = false;
        _state->mqttConfigured = false;
        return;
    }

    if (xTaskCreate(_taskFunc, "mqtt_task", 4096, this, 1, &_taskHandle) != pdPASS) {
        Serial.println("[MQTT] init failed: xTaskCreate failed");
        vQueueDelete(_publishQueue);
        _publishQueue = nullptr;
        _enabled = false;
        _state->mqttConfigured = false;
    }
}

void MqttPublisher::publishStatus(float currentMl, const char* event) {
    if (!_enabled || !_publishQueue) return;

    const uint32_t goalMl = _cfg->dailyGoalMl;

    JsonDocument doc;
    doc["current_ml"] = (int)constrain(currentMl, 0.0f, 9999.0f);
    doc["goal_ml"]     = (int)constrain((float)goalMl, 0.0f, 9999.0f);
    if (goalMl > 0) doc["pct"] = currentMl / (float)goalMl;
    doc["event"] = event;
    if (_time && _time->isSynced()) doc["device_time"] = _time->getISOTimestamp();

    PublishMsg msg;
    msg.retained = true;
    const size_t n = serializeJson(doc, msg.payload, sizeof(msg.payload));
    if (n == 0 || n >= sizeof(msg.payload)) {
        Serial.println("[MQTT] publishStatus: payload too large, dropped");
        return;
    }

    if (xQueueSend(_publishQueue, &msg, 0) != pdTRUE) {
        Serial.println("[MQTT] publishStatus: queue full, dropped");
    }
}

void MqttPublisher::loop() {
    if (!_enabled) return;

    const uint32_t now = millis();
    const uint32_t heartbeatMs = (uint32_t)_cfg->mqttHeartbeatSec * 1000UL;
    if (heartbeatMs == 0 || (now - _lastPublishMs) < heartbeatMs) return;

    // Skip while disconnected: nothing would deliver it anyway, and it would just
    // occupy a queue slot that a real drink/refill event might need. _lastPublishMs
    // is intentionally left unchanged so the next loop() call retries immediately
    // once mqttConnected flips true (giving a prompt fresh status right after reconnect).
    if (!_state->mqttConnected.load(std::memory_order_relaxed)) return;

    _lastPublishMs = now;
    publishStatus(_state->todayTotalMl, "heartbeat");
}

void MqttPublisher::_taskFunc(void* param) {
    static_cast<MqttPublisher*>(param)->_taskLoop();
}

void MqttPublisher::_taskLoop() {
    _mqttClient.setClient(_wifiClient);
    _mqttClient.setServer(_brokerHost, _brokerPort);
    _mqttClient.setSocketTimeout(2);

    uint32_t lastConnectAttempt = 0;

    for (;;) {
        if (_mqttClient.connected()) {
            _mqttClient.loop();
            _state->mqttConnected.store(true, std::memory_order_relaxed);

            PublishMsg msg;
            if (xQueueReceive(_publishQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (!_mqttClient.publish(TOPIC_STATUS, msg.payload, msg.retained)) {
                    Serial.println("[MQTT] publish failed");
                }
            }
        } else {
            _state->mqttConnected.store(false, std::memory_order_relaxed);
            const uint32_t now = millis();
            if (now - lastConnectAttempt >= RECONNECT_INTERVAL_MS && WiFi.isConnected()) {
                lastConnectAttempt = now;
                _connect();
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

bool MqttPublisher::_connect() {
    const bool ok = _username[0]
        ? _mqttClient.connect(_clientId, _username, _password,
                               TOPIC_AVAILABILITY, 1, true, LWT_OFFLINE_PAYLOAD)
        : _mqttClient.connect(_clientId,
                               TOPIC_AVAILABILITY, 1, true, LWT_OFFLINE_PAYLOAD);

    if (ok) {
        Serial.println("[MQTT] Connected");
        _mqttClient.publish(TOPIC_AVAILABILITY, "{\"online\":true}", true);
    } else {
        Serial.printf("[MQTT] Connect failed, state=%d\n", _mqttClient.state());
    }
    return ok;
}
