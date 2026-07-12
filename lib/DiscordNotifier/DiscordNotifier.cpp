#include "DiscordNotifier.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

void DiscordNotifier::init(AppState& state, const AppConfig& cfg) {
    _state = &state;
    _configMutex = xSemaphoreCreateMutex();
    _highQueue = xQueueCreate(6, sizeof(TaskParam*));
    _lowQueue = xQueueCreate(1, sizeof(TaskParam*));
    configure(cfg);
    _state->webhookLastOk.store(false, std::memory_order_relaxed);
    if (!_configMutex || !_highQueue || !_lowQueue ||
        xTaskCreate(_workerTask, "discord_worker", 8192, this, 1, &_workerHandle) != pdPASS) {
        Serial.println("[Discord] persistent worker creation failed");
        _state->webhookConfigured = false;
        return;
    }
    _workerReady.store(true, std::memory_order_release);
    _state->webhookConfigured = (_webhookUrl[0] != '\0');
    Serial.printf("[Discord] init  configured=%s\n",
                  _state->webhookConfigured ? "yes" : "no");
}

void DiscordNotifier::configure(const AppConfig& cfg) {
    if (!_configMutex || xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const size_t length = cfg.discordWebhookUrl.length();
    if (length >= sizeof(_webhookUrl)) {
        Serial.printf("[Discord] webhook URL too long (%u bytes), truncated\n", (unsigned)length);
    }
    strncpy(_webhookUrl, cfg.discordWebhookUrl.c_str(), sizeof(_webhookUrl) - 1);
    _webhookUrl[sizeof(_webhookUrl) - 1] = '\0';
    _dailyGoalMl = cfg.dailyGoalMl > 0 ? cfg.dailyGoalMl : 2000;
    if (_state) _state->webhookConfigured =
        (_webhookUrl[0] != '\0') && _workerReady.load(std::memory_order_acquire);
    xSemaphoreGive(_configMutex);
}

bool DiscordNotifier::_copyConfig(char* webhookUrl, size_t size, uint32_t& dailyGoalMl) {
    if (!_configMutex || xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    strncpy(webhookUrl, _webhookUrl, size - 1);
    webhookUrl[size - 1] = '\0';
    dailyGoalMl = _dailyGoalMl;
    xSemaphoreGive(_configMutex);
    return webhookUrl[0] != '\0';
}

void DiscordNotifier::notifyOnline(const String& ipAddress) {
    char webhookUrl[512];
    uint32_t goalMl;
    if (!_copyConfig(webhookUrl, sizeof(webhookUrl), goalMl)) return;
    (void)goalMl;
    if (!WiFi.isConnected()) return;
    if (ipAddress.isEmpty() || ipAddress == "0.0.0.0") return;

    TaskParam* p = new TaskParam();
    if (!p) {
        Serial.println("[Discord] notifyOnline failed: heap alloc failed");
        return;
    }

    strncpy(p->webhookUrl, webhookUrl, sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

    const int onlineWritten = snprintf(p->body, sizeof(p->body),
             "{\"content\":\"[HydraCup] Online - http://%s\"}",
             ipAddress.c_str());
    if (onlineWritten >= (int)sizeof(p->body)) {
        Serial.println("[Discord] notifyOnline: body truncated");
    }

    _enqueue(p, false);
}

void DiscordNotifier::notifyDrink(float amountMl, float totalMl, uint32_t drinkCount) {
    char webhookUrl[512];
    uint32_t goalMl;
    if (!_copyConfig(webhookUrl, sizeof(webhookUrl), goalMl)) {
        Serial.println("[Discord] notifyDrink skipped: webhook URL not configured");
        return;
    }
    if (!WiFi.isConnected()) {
        Serial.println("[Discord] notifyDrink skipped: WiFi not connected");
        return;
    }

    TaskParam* p = new TaskParam();
    if (!p) {
        Serial.println("[Discord] notifyDrink failed: heap alloc failed");
        return;
    }

    strncpy(p->webhookUrl, webhookUrl, sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

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
        return;
    }

    _enqueue(p, true);
}

bool DiscordNotifier::notifyDailySummary(float totalMl, uint32_t drinkCount, const String& dateStr) {
    char webhookUrl[512];
    uint32_t goalMl;
    if (!_copyConfig(webhookUrl, sizeof(webhookUrl), goalMl)) {
        Serial.println("[Discord] notifyDailySummary skipped: webhook not configured");
        return false;
    }
    if (!WiFi.isConnected()) {
        Serial.println("[Discord] notifyDailySummary skipped: WiFi not connected");
        return false;
    }

    TaskParam* p = new TaskParam();
    if (!p) {
        Serial.println("[Discord] notifyDailySummary failed: heap alloc failed");
        return false;
    }

    strncpy(p->webhookUrl, webhookUrl, sizeof(p->webhookUrl) - 1);
    p->webhookUrl[sizeof(p->webhookUrl) - 1] = '\0';

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
        return false;
    }

    return _enqueue(p, true);
}

void DiscordNotifier::update() {
}

void DiscordNotifier::_send(TaskParam* p) {

    // 3 total attempts; delays (ms) before attempt 1 and 2
    static constexpr int      MAX_ATTEMPTS = 3;
    static constexpr uint32_t RETRY_MS[]   = {2000, 8000};

    bool ok = false;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS[attempt - 1]));
            Serial.printf("[Discord] Retry %d/%d\n", attempt, MAX_ATTEMPTS - 1);
        }

        bool retryable = false;
        {   // Scope ensures destructors run before next iteration / vTaskDelete
            WiFiClientSecure client;
            client.setInsecure();
            HTTPClient http;
            http.setTimeout(10000);

            if (!http.begin(client, p->webhookUrl)) {
                Serial.println("[Discord] http.begin() failed");
                retryable = true;
            } else {
                http.addHeader("Content-Type", "application/json");
                const int code = http.POST(String(p->body));
                http.end();

                if (code >= 200 && code < 300) {
                    ok = true;
                } else if (code == 408 || code == 429 || code >= 500) {
                    // Transient HTTP response: request timeout, rate-limited, or server error
                    Serial.printf("[Discord] POST HTTP %d (retryable)\n", code);
                    retryable = true;
                } else if (code < 0) {
                    // Do not retry transport errors: the webhook may have received the body already.
                    Serial.printf("[Discord] POST transport error %d (%s); not retrying to avoid duplicate sends\n",
                                  code, http.errorToString(code).c_str());
                } else {
                    // Permanent: bad request, wrong URL, auth error, etc.
                    Serial.printf("[Discord] POST HTTP %d (permanent failure)\n", code);
                }
            }
        }

        if (ok || !retryable) break;
    }

    if (_state) _state->webhookLastOk.store(ok, std::memory_order_release);
    Serial.printf("[Discord] POST %s\n", ok ? "OK" : "FAILED (all attempts exhausted)");
    delete p;
}

bool DiscordNotifier::_enqueue(TaskParam* message, bool highPriority) {
    if (!message) return false;
    QueueHandle_t queue = highPriority ? _highQueue : _lowQueue;
    if (!queue || !_workerReady.load(std::memory_order_acquire)) {
        _droppedCount++;
        delete message;
        return false;
    }

    if (!highPriority) {
        TaskParam* replaced = nullptr;
        if (xQueueReceive(queue, &replaced, 0) == pdTRUE) delete replaced;
    }

    if (xQueueSend(queue, &message, 0) == pdTRUE) return true;
    _droppedCount++;
    Serial.println(highPriority
        ? "[Discord] priority queue full, event dropped"
        : "[Discord] online event dropped");
    delete message;
    return false;
}

void DiscordNotifier::_workerTask(void* param) {
    static_cast<DiscordNotifier*>(param)->_workerLoop();
}

void DiscordNotifier::_workerLoop() {
    TaskParam* message = nullptr;
    for (;;) {
        if (xQueueReceive(_highQueue, &message, pdMS_TO_TICKS(50)) == pdTRUE ||
            xQueueReceive(_lowQueue, &message, 0) == pdTRUE) {
            _send(message);
            message = nullptr;
        }
    }
}
