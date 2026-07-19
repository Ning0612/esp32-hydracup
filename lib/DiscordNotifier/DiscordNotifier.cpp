#include "DiscordNotifier.h"

#include <cstdio>
#include <cstring>

#include "esp_http_client.h"
#include "hal_log.h"
#include "hal_time.h"

// Discord webhook TLS trust anchor. Refresh when the Discord certificate chain changes.
static const char DISCORD_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";

void DiscordNotifier::init(AppState& state, const AppConfig& cfg) {
    _state = &state; _configMutex = xSemaphoreCreateMutex(); _highQueue = xQueueCreate(6, sizeof(TaskParam*)); _lowQueue = xQueueCreate(1, sizeof(TaskParam*)); configure(cfg); _state->webhookLastOk.store(false);
    if (!_configMutex || !_highQueue || !_lowQueue || xTaskCreate(_workerTask, "discord_worker", 8192, this, 1, &_workerHandle) != pdPASS) { _state->webhookConfigured = false; LOG_ERROR("Discord", "worker creation failed"); return; }
    _workerReady.store(true); _state->webhookConfigured = _webhookUrl[0] != '\0'; LOG_INFO("Discord", "configured=%s", _state->webhookConfigured ? "yes" : "no");
}

void DiscordNotifier::configure(const AppConfig& cfg) {
    if (!_configMutex || xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    std::strncpy(_webhookUrl, cfg.discordWebhookUrl.c_str(), sizeof(_webhookUrl) - 1); _webhookUrl[sizeof(_webhookUrl) - 1] = '\0'; _dailyGoalMl = cfg.dailyGoalMl ? cfg.dailyGoalMl : 2000;
    if (_state) _state->webhookConfigured = _webhookUrl[0] != '\0' && _workerReady.load();
    xSemaphoreGive(_configMutex);
}

bool DiscordNotifier::_copyConfig(char* webhookUrl, size_t size, uint32_t& dailyGoalMl) {
    if (!_configMutex || xSemaphoreTake(_configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    std::strncpy(webhookUrl, _webhookUrl, size - 1); webhookUrl[size - 1] = '\0'; dailyGoalMl = _dailyGoalMl;
    xSemaphoreGive(_configMutex); return webhookUrl[0] != '\0';
}

void DiscordNotifier::notifyOnline(const std::string& ipAddress) {
    char url[512]; uint32_t goal = 0; if (!_copyConfig(url, sizeof(url), goal) || !_state || !_state->wifiConnected || ipAddress.empty() || ipAddress == "0.0.0.0") return; auto* message = new TaskParam{}; std::strncpy(message->webhookUrl, url, sizeof(message->webhookUrl) - 1); std::snprintf(message->body, sizeof(message->body), "{\"content\":\"HydraCup online\\nWebUI: http://%s\"}", ipAddress.c_str()); _enqueue(message, false);
}

void DiscordNotifier::notifyDrink(float amountMl, float totalMl, uint32_t drinkCount) {
    (void)drinkCount; char url[512]; uint32_t goal = 0; if (!_state || !_state->ntpSynced || !_state->wifiConnected || !_copyConfig(url, sizeof(url), goal)) return; auto* message = new TaskParam{}; std::strncpy(message->webhookUrl, url, sizeof(message->webhookUrl) - 1); std::snprintf(message->body, sizeof(message->body), "{\"content\":\"HydraCup drink +%.0f ml (today %.0f/%u ml)\"}", amountMl, totalMl, static_cast<unsigned>(goal)); _enqueue(message, true);
}

bool DiscordNotifier::notifyDailySummary(float totalMl, uint32_t drinkCount, const std::string& dateStr) {
    char url[512]; uint32_t goal = 0; if (!_state || !_state->ntpSynced || !_state->wifiConnected || !_copyConfig(url, sizeof(url), goal)) return false; auto* message = new TaskParam{}; std::strncpy(message->webhookUrl, url, sizeof(message->webhookUrl) - 1);
    const float pct = goal ? totalMl * 100.0f / goal : 0; const float average = drinkCount ? totalMl / drinkCount : 0; const int written = std::snprintf(message->body, sizeof(message->body), "{\"embeds\":[{\"title\":\"HydraCup daily summary - %s\",\"fields\":[{\"name\":\"intake\",\"value\":\"%.0f ml\",\"inline\":true},{\"name\":\"goal\",\"value\":\"%u ml\",\"inline\":true},{\"name\":\"completion\",\"value\":\"%.0f%%\",\"inline\":true},{\"name\":\"drinks\",\"value\":\"%u\",\"inline\":true},{\"name\":\"average\",\"value\":\"%.0f ml\",\"inline\":true}],\"color\":3066993}]}", dateStr.c_str(), totalMl, static_cast<unsigned>(goal), pct, static_cast<unsigned>(drinkCount), average);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(message->body)) { delete message; return false; } return _enqueue(message, true);
}

void DiscordNotifier::_send(TaskParam* message) {
    bool ok = false; esp_http_client_config_t config = {}; config.url = message->webhookUrl; config.method = HTTP_METHOD_POST; config.cert_pem = DISCORD_ROOT_CA; config.timeout_ms = 10000; esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) { esp_http_client_set_header(client, "Content-Type", "application/json"); esp_http_client_set_post_field(client, message->body, std::strlen(message->body)); const esp_err_t error = esp_http_client_perform(client); const int status = esp_http_client_get_status_code(client); ok = error == ESP_OK && status >= 200 && status < 300; if (!ok) LOG_WARN("Discord", "POST failed error=%s status=%d", esp_err_to_name(error), status); esp_http_client_cleanup(client); } else { LOG_WARN("Discord", "client init failed"); }
    if (_state) _state->webhookLastOk.store(ok);
    LOG_INFO("Discord", "POST %s", ok ? "OK" : "FAILED"); delete message;
}

bool DiscordNotifier::_enqueue(TaskParam* message, bool highPriority) {
    if (!message) return false;
    QueueHandle_t queue = highPriority ? _highQueue : _lowQueue;
    if (!queue || !_workerReady.load()) { _droppedCount++; delete message; return false; }
    if (!highPriority) { TaskParam* old = nullptr; if (xQueueReceive(queue, &old, 0) == pdTRUE) delete old; }
    if (xQueueSend(queue, &message, 0) == pdTRUE) return true;
    _droppedCount++; delete message; return false;
}
void DiscordNotifier::_workerTask(void* param) { static_cast<DiscordNotifier*>(param)->_workerLoop(); }
void DiscordNotifier::_workerLoop() { TaskParam* message = nullptr; for (;;) { if (xQueueReceive(_highQueue, &message, pdMS_TO_TICKS(50)) == pdTRUE || xQueueReceive(_lowQueue, &message, 0) == pdTRUE) { _send(message); message = nullptr; } } }
