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
    if (!_onlineTaskRunning.compare_exchange_strong(expected, true)) {
        Serial.println("[Discord] Drop online: previous send in progress");
        return;
    }

    TaskParam* p = new TaskParam();
    if (!p) {
        Serial.println("[Discord] notifyOnline failed: heap alloc failed");
        _onlineTaskRunning.store(false);
        return;
    }

    const size_t urlLen = _cfg->discordWebhookUrl.length();
    if (urlLen >= sizeof(p->webhookUrl)) {
        Serial.printf("[Discord] notifyOnline: webhook URL too long (%u bytes), truncated\n", urlLen);
    }
    strncpy(p->webhookUrl, _cfg->discordWebhookUrl.c_str(), sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

    const int onlineWritten = snprintf(p->body, sizeof(p->body),
             "{\"content\":\"[HydraCup] Online - http://%s\"}",
             ipAddress.c_str());
    if (onlineWritten >= (int)sizeof(p->body)) {
        Serial.println("[Discord] notifyOnline: body truncated");
    }

    p->lastOkPtr      = &_state->webhookLastOk;
    p->taskRunningPtr = &_onlineTaskRunning;

    if (xTaskCreate(_sendTask, "discord_online", 8192, p, 1, nullptr) != pdPASS) {
        Serial.println("[Discord] xTaskCreate failed (online)");
        delete p;
        _onlineTaskRunning.store(false);
    }
}

void DiscordNotifier::notifyDrink(float amountMl, float totalMl, uint32_t drinkCount) {
    if (!_cfg || _cfg->discordWebhookUrl.isEmpty()) {
        Serial.println("[Discord] notifyDrink skipped: webhook URL not configured");
        return;
    }
    if (!WiFi.isConnected()) {
        Serial.println("[Discord] notifyDrink skipped: WiFi not connected");
        return;
    }

    // Atomic check-and-set: prevents double-task on preemption
    bool expected = false;
    if (!_taskRunning.compare_exchange_strong(expected, true)) {
        Serial.println("[Discord] Drop: previous send in progress");
        return;
    }

    TaskParam* p = new TaskParam();
    if (!p) {
        Serial.println("[Discord] notifyDrink failed: heap alloc failed");
        _taskRunning.store(false);
        return;
    }

    const size_t urlLen = _cfg->discordWebhookUrl.length();
    if (urlLen >= sizeof(p->webhookUrl)) {
        Serial.printf("[Discord] notifyDrink: webhook URL too long (%u bytes), truncated\n", urlLen);
    }
    strncpy(p->webhookUrl, _cfg->discordWebhookUrl.c_str(), sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

    const uint32_t goalMl = _cfg->dailyGoalMl > 0 ? _cfg->dailyGoalMl : 2000;
    const float    pct    = (totalMl / (float)goalMl) * 100.0f;
    const float    remain = totalMl >= (float)goalMl ? 0.0f : ((float)goalMl - totalMl);

    uint32_t    color;
    const char* filledEmoji;
    if (pct < 50.0f) {
        color = 15158332; filledEmoji = "🟥";
    } else if (pct < 80.0f) {
        color = 15132194; filledEmoji = "🟨";
    } else if (pct < 100.0f) {
        color = 3447003;  filledEmoji = "🟦";
    } else {
        color = 3066993;  filledEmoji = "🟩";
    }

    // Clamp to [0,10]: guards against negative totalMl or NaN (NaN comparisons are false)
    const int filled = (pct != pct || pct <= 0.0f) ? 0 : (pct >= 100.0f ? 10 : (int)(pct / 10.0f));
    char bar[60];
    bar[0] = '\0';
    for (int i = 0;      i < filled; i++) strcat(bar, filledEmoji);
    for (int i = filled; i < 10;     i++) strcat(bar, "⬜");

    char lastLine[48];
    if (remain <= 0.0f) {
        snprintf(lastLine, sizeof(lastLine), "第 %u 杯，已達標", (unsigned)drinkCount);
    } else {
        snprintf(lastLine, sizeof(lastLine), "第 %u 杯，還差 %.0f ml 達標", (unsigned)drinkCount, remain);
    }

    const int written = snprintf(p->body, sizeof(p->body),
        "{\"embeds\":[{\"title\":\"本次 +%.0f ml\","
        "\"description\":\"今日累計\\n%.0f ml / %u ml\\n\\n%s  %.0f%%\\n%s\","
        "\"color\":%u}]}",
        amountMl, totalMl, (unsigned)goalMl,
        bar, pct, lastLine,
        (unsigned)color);
    if (written >= (int)sizeof(p->body)) {
        Serial.println("[Discord] notifyDrink: body truncated, aborting POST");
        delete p;
        _taskRunning.store(false);
        return;
    }

    p->lastOkPtr      = &_state->webhookLastOk;
    p->taskRunningPtr = &_taskRunning;

    if (xTaskCreate(_sendTask, "discord_send", 8192, p, 1, nullptr) != pdPASS) {
        Serial.println("[Discord] xTaskCreate failed");
        delete p;
        _taskRunning.store(false);
    }
}

bool DiscordNotifier::notifyDailySummary(float totalMl, uint32_t drinkCount, const String& dateStr) {
    if (!_cfg || _cfg->discordWebhookUrl.isEmpty()) {
        Serial.println("[Discord] notifyDailySummary skipped: webhook not configured");
        return false;
    }
    if (!WiFi.isConnected()) {
        Serial.println("[Discord] notifyDailySummary skipped: WiFi not connected");
        return false;
    }

    bool expected = false;
    if (!_summaryTaskRunning.compare_exchange_strong(expected, true)) {
        Serial.println("[Discord] Drop summary: previous send in progress");
        return false;
    }

    TaskParam* p = new TaskParam();
    if (!p) {
        Serial.println("[Discord] notifyDailySummary failed: heap alloc failed");
        _summaryTaskRunning.store(false);
        return false;
    }

    strncpy(p->webhookUrl, _cfg->discordWebhookUrl.c_str(), sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

    const uint32_t goalMl   = _cfg->dailyGoalMl > 0 ? _cfg->dailyGoalMl : 2000;
    const float    pct      = (totalMl > 0.0f) ? (totalMl / (float)goalMl) * 100.0f : 0.0f;
    const bool     achieved = (totalMl >= (float)goalMl);
    const float    remain   = achieved ? 0.0f : ((float)goalMl - totalMl);
    const float    over     = achieved ? (totalMl - (float)goalMl) : 0.0f;
    const float    avgMl    = (drinkCount > 0) ? (totalMl / (float)drinkCount) : 0.0f;

    uint32_t    color;
    const char* filledEmoji;
    if (pct < 50.0f) {
        color = 15158332; filledEmoji = "🟥";
    } else if (pct < 80.0f) {
        color = 15132194; filledEmoji = "🟨";
    } else if (pct < 100.0f) {
        color = 3447003;  filledEmoji = "🟦";
    } else {
        color = 3066993;  filledEmoji = "🟩";
    }

    // Clamp to [0,10]: guards against NaN (NaN comparisons are false)
    const int filled = (pct != pct || pct <= 0.0f) ? 0 : (pct >= 100.0f ? 10 : (int)(pct / 10.0f));
    char bar[60];
    bar[0] = '\0';
    for (int i = 0;      i < filled; i++) strcat(bar, filledEmoji);
    for (int i = filled; i < 10;     i++) strcat(bar, "⬜");

    // 完成度 field: achieved → "✅ 已達標", else actual percentage
    char completionVal[28];
    if (achieved) {
        strncpy(completionVal, "✅ 已達標", sizeof(completionVal));
    } else {
        snprintf(completionVal, sizeof(completionVal), "%.0f%%", pct);
    }

    // Row 2 Col 3: 還差達標 (未達標) / 超標量 (已達標)
    const char* thirdName;
    char        thirdVal[28];
    if (!achieved) {
        thirdName = "還差達標";
        snprintf(thirdVal, sizeof(thirdVal), "%.0f ml", remain);
    } else if (over < 1.0f) {
        thirdName = "超標量";
        strncpy(thirdVal, "✅ 剛好達標", sizeof(thirdVal));
    } else {
        thirdName = "超標量";
        snprintf(thirdVal, sizeof(thirdVal), "+%.0f ml", over);
    }

    const int written = snprintf(p->body, sizeof(p->body),
        "{\"embeds\":[{"
        "\"title\":\"📊 飲水日報 · %s\","
        "\"description\":\"%s  %.0f%%\","
        "\"fields\":["
          "{\"name\":\"今日攝取\",\"value\":\"%.0f ml\",\"inline\":true},"
          "{\"name\":\"今日目標\",\"value\":\"%u ml\",\"inline\":true},"
          "{\"name\":\"完成度\",\"value\":\"%s\",\"inline\":true},"
          "{\"name\":\"飲水次數\",\"value\":\"%u 次\",\"inline\":true},"
          "{\"name\":\"平均每次\",\"value\":\"%.0f ml\",\"inline\":true},"
          "{\"name\":\"%s\",\"value\":\"%s\",\"inline\":true}"
        "],"
        "\"color\":%u}]}",
        dateStr.c_str(), bar, pct,
        totalMl, (unsigned)goalMl, completionVal,
        (unsigned)drinkCount, avgMl,
        thirdName, thirdVal,
        (unsigned)color);

    if (written >= (int)sizeof(p->body)) {
        Serial.println("[Discord] notifyDailySummary: body truncated, aborting POST");
        delete p;
        _summaryTaskRunning.store(false);
        return false;
    }

    p->lastOkPtr      = &_state->webhookLastOk;
    p->taskRunningPtr = &_summaryTaskRunning;

    if (xTaskCreate(_sendTask, "discord_summary", 8192, p, 1, nullptr) != pdPASS) {
        Serial.println("[Discord] xTaskCreate failed (summary)");
        delete p;
        _summaryTaskRunning.store(false);
        return false;
    }
    return true;
}

void DiscordNotifier::update() {
    if (_state && _cfg) {
        _state->webhookConfigured = !_cfg->discordWebhookUrl.isEmpty();
    }
}

void DiscordNotifier::_sendTask(void* param) {
    TaskParam* p = static_cast<TaskParam*>(param);

    {   // Scope ensures WiFiClientSecure/HTTPClient destructors run before vTaskDelete
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(10000);

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
    }

    p->taskRunningPtr->store(false, std::memory_order_release);
    delete p;
    vTaskDelete(nullptr);
}
