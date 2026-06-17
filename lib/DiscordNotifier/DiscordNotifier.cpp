#include "DiscordNotifier.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

void DiscordNotifier::init(AppState& state, const AppConfig& cfg) {
    _state = &state;
    _cfg   = &cfg;
    _state->webhookConfigured = !_cfg->discordWebhookUrl.isEmpty();
    _state->webhookLastOk     = false;
    Serial.printf("[Discord] init  configured=%s\n",
                  _state->webhookConfigured ? "yes" : "no");
}

void DiscordNotifier::notifyOnline(const String& ipAddress) {
    if (!_cfg || _cfg->discordWebhookUrl.isEmpty()) return;
    if (!WiFi.isConnected()) return;
    if (ipAddress.isEmpty() || ipAddress == "0.0.0.0") return;

    bool expected = false;
    if (!_taskRunning.compare_exchange_strong(expected, true)) {
        Serial.println("[Discord] Drop online: previous send in progress");
        return;
    }

    TaskParam* p = new TaskParam();
    if (!p) { _taskRunning.store(false); return; }

    strncpy(p->webhookUrl, _cfg->discordWebhookUrl.c_str(), sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

    snprintf(p->body, sizeof(p->body),
             "{\"content\":\"[HydraCup] Online - http://%s\"}",
             ipAddress.c_str());

    p->lastOkPtr      = &_state->webhookLastOk;
    p->taskRunningPtr = &_taskRunning;

    if (xTaskCreate(_sendTask, "discord_online", 8192, p, 1, nullptr) != pdPASS) {
        Serial.println("[Discord] xTaskCreate failed (online)");
        delete p;
        _taskRunning.store(false);
    }
}

void DiscordNotifier::notifyDrink(float amountMl, float totalMl, const String& timestamp) {
    if (!_cfg || _cfg->discordWebhookUrl.isEmpty()) return;
    if (!WiFi.isConnected()) return;

    // Atomic check-and-set: prevents double-task on preemption
    bool expected = false;
    if (!_taskRunning.compare_exchange_strong(expected, true)) {
        Serial.println("[Discord] Drop: previous send in progress");
        return;
    }

    TaskParam* p = new TaskParam();
    if (!p) {
        _taskRunning.store(false);
        return;
    }

    strncpy(p->webhookUrl, _cfg->discordWebhookUrl.c_str(), sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

    snprintf(p->body, sizeof(p->body),
             "{\"content\":\"Drank %.0f ml | Today total %.0f ml\","
             "\"embeds\":[{\"color\":3066993,\"description\":\"Time: %s\"}]}",
             amountMl, totalMl, timestamp.c_str());

    p->lastOkPtr      = &_state->webhookLastOk;
    p->taskRunningPtr = &_taskRunning;

    if (xTaskCreate(_sendTask, "discord_send", 8192, p, 1, nullptr) != pdPASS) {
        Serial.println("[Discord] xTaskCreate failed");
        delete p;
        _taskRunning.store(false);
    }
}

void DiscordNotifier::update() {
    if (_state && _cfg) {
        _state->webhookConfigured = !_cfg->discordWebhookUrl.isEmpty();
    }
}

void DiscordNotifier::_sendTask(void* param) {
    TaskParam* p = static_cast<TaskParam*>(param);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (http.begin(client, p->webhookUrl)) {
        http.addHeader("Content-Type", "application/json");
        const int code = http.POST(String(p->body));
        *p->lastOkPtr = (code >= 200 && code < 300);
        Serial.printf("[Discord] POST %s  HTTP %d\n",
                      *p->lastOkPtr ? "OK" : "FAILED", code);
        http.end();
    } else {
        *p->lastOkPtr = false;
        Serial.println("[Discord] http.begin() failed");
    }

    p->taskRunningPtr->store(false, std::memory_order_release);
    delete p;
    vTaskDelete(nullptr);
}
