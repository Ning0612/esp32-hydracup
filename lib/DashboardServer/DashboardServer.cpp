#include "DashboardServer.h"
#include <ArduinoJson.h>
#include <WiFi.h>

// ── Helpers ────────────────────────────────────────────────────────────────

const char* DashboardServer::_cupStateStr(CupState s) {
    switch (s) {
        case CupState::NO_CUP:          return "no_cup";
        case CupState::CUP_UNSTABLE:    return "unstable";
        case CupState::CUP_STABLE:      return "stable";
        case CupState::POSSIBLE_DRINK:  return "possible_drink";
        case CupState::DRINK_CONFIRMED: return "drink_confirmed";
        case CupState::REFILL_DETECTED: return "refill_detected";
        default: return "unknown";
    }
}

String DashboardServer::_maskWebhookUrl(const String& url) {
    if (url.isEmpty()) return "";
    int last = url.lastIndexOf('/');
    if (last > 0) return url.substring(0, last + 1) + "****";
    return "****";
}

void DashboardServer::_serveFile(const char* path, const char* contentType) {
    if (!_state->fsOk) {
        _server->send(503, "text/plain", "Web assets unavailable. Run: pio run -t uploadfs");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        _server->send(404, "text/plain",
                      String("Not found: ") + path + "\nRun: pio run -t uploadfs");
        return;
    }
    _server->streamFile(f, contentType);
    f.close();
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void DashboardServer::begin(ScaleManager& scale, ConfigManager& cfgMgr,
                            AppState& state, AppConfig& cfg,
                            BuzzerController& buzzer, ReminderManager& reminder,
                            fs::LittleFSFS& logFs) {
    if (_server != nullptr) {
        Serial.println("[Dashboard] Already initialized");
        return;
    }
    _scale    = &scale;
    _cfgMgr   = &cfgMgr;
    _state    = &state;
    _cfg      = &cfg;
    _buzzer   = &buzzer;
    _reminder = &reminder;
    _logFs    = &logFs;
    _server   = new WebServer(80);

    _server->on("/",              HTTP_GET,  [this]{ _handleRoot();                });
    _server->on("/settings",      HTTP_GET,  [this]{ _handleSettings();            });
    _server->on("/history",       HTTP_GET,  [this]{ _handleHistory();             });
    _server->on("/style.css",     HTTP_GET,  [this]{ _handleStyleCss();            });
    _server->on("/calibration",   HTTP_GET,  [this]{ _handleCalibrationRedirect(); });
    _server->on("/api/weight",    HTTP_GET,  [this]{ _handleWeight();              });
    _server->on("/api/status",    HTTP_GET,  [this]{ _handleStatus();              });
    _server->on("/api/config",    HTTP_GET,  [this]{ _handleGetConfig();           });
    _server->on("/api/config",    HTTP_POST, [this]{ _handlePostConfig();          });
    _server->on("/api/tare",      HTTP_POST, [this]{ _handleTare();                });
    _server->on("/api/calibrate", HTTP_POST, [this]{ _handleCalibrate();           });
    _server->on("/api/wifi/scan", HTTP_GET,  [this]{ _handleWifiScan();            });
    _server->on("/api/reboot",    HTTP_POST, [this]{ _handleReboot();              });
    _server->on("/api/logs",      HTTP_GET,  [this]{ _handleLogs();                });

    _server->begin();
    Serial.println("[Dashboard] HTTP server started on port 80");
}

void DashboardServer::loop() {
    if (_server) _server->handleClient();
}

// ── Page handlers ──────────────────────────────────────────────────────────

void DashboardServer::_handleRoot()     { _serveFile("/index.html",    "text/html"); }
void DashboardServer::_handleSettings() { _serveFile("/settings.html", "text/html"); }
void DashboardServer::_handleHistory()  { _serveFile("/history.html",  "text/html"); }
void DashboardServer::_handleStyleCss() { _serveFile("/style.css",     "text/css");  }

void DashboardServer::_handleCalibrationRedirect() {
    _server->sendHeader("Location", "/settings#calibration", true);
    _server->send(301, "text/plain", "Moved Permanently");
}

// ── API handlers ───────────────────────────────────────────────────────────

void DashboardServer::_handleWeight() {
    JsonDocument doc;
    doc["ok"]        = true;
    doc["weight_g"]  = _scale->getWeightGrams();
    doc["cup_state"] = (int)_state->cupState;
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleStatus() {
    JsonDocument doc;
    doc["ok"]                = true;
    doc["mode"]              = "normal";
    doc["wifi_connected"]    = _state->wifiConnected;
    doc["ip"]                = _state->ipAddress;
    doc["ntp_synced"]        = _state->ntpSynced;
    doc["weight_g"]          = _scale->getWeightGrams();
    doc["cup_state"]         = (int)_state->cupState;
    doc["cup_state_name"]    = _cupStateStr(_state->cupState);
    doc["today_total_ml"]    = _state->todayTotalMl;
    doc["daily_goal_ml"]     = _cfg->dailyGoalMl;
    doc["drink_count_today"] = _state->drinkCountToday;
    doc["last_drink_ml"]     = _state->lastDrinkMl;
    doc["next_reminder_sec"] = _state->nextReminderSec;
    doc["webhook_configured"] = _state->webhookConfigured;
    doc["webhook_last_ok"]   = _state->webhookLastOk.load();
    doc["mqtt_configured"]   = _state->mqttConfigured;
    doc["mqtt_connected"]    = _state->mqttConnected.load();
    doc["hw_hx711"]          = _state->hx711Ok;
    doc["hw_oled"]           = _state->oledOk;
    doc["hw_fs"]             = _state->fsOk;
    doc["hw_logfs"]          = _state->logFsOk;
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleGetConfig() {
    JsonDocument doc;
    doc["ok"]                      = true;
    doc["wifiSsid"]                = _cfg->wifiSsid;
    doc["wifiPassword"]            = "****";
    doc["wifiPasswordSet"]         = !_cfg->wifiPassword.isEmpty();
    doc["discordWebhookUrl"]       = _maskWebhookUrl(_cfg->discordWebhookUrl);
    doc["reminderEnabled"]         = _cfg->reminderEnabled;
    doc["reminderIntervalMin"]     = _cfg->reminderIntervalMin;
    doc["reminderAlertTimeoutSec"] = _cfg->reminderAlertTimeoutSec;
    doc["dailyGoalMl"]             = _cfg->dailyGoalMl;
    doc["buzzerEnabled"]           = _cfg->buzzerEnabled;
    doc["buzzerFrequencyHz"]       = _cfg->buzzerFrequencyHz;
    doc["buzzerDurationMs"]        = _cfg->buzzerDurationMs;
    doc["buzzerVolumePercent"]     = _cfg->buzzerVolumePercent;
    doc["ntpEnabled"]              = _cfg->ntpEnabled;
    doc["ntpServer1"]              = _cfg->ntpServer1;
    doc["ntpServer2"]              = _cfg->ntpServer2;
    doc["timezoneOffsetSec"]       = _cfg->timezoneOffsetSec;
    doc["mqttEnabled"]             = _cfg->mqttEnabled;
    doc["mqttBrokerHost"]          = _cfg->mqttBrokerHost;
    doc["mqttBrokerPort"]          = _cfg->mqttBrokerPort;
    doc["mqttUsername"]            = _cfg->mqttUsername;
    doc["mqttPassword"]            = "****";
    doc["mqttPasswordSet"]         = !_cfg->mqttPassword.isEmpty();
    doc["mqttClientId"]            = _cfg->mqttClientId;
    doc["mqttHeartbeatSec"]        = _cfg->mqttHeartbeatSec;
    doc["calibrationFactor"]       = _scale->getCalibrationFactor();
    doc["cupPresentThresholdGram"] = _cfg->cupPresentThresholdGram;
    doc["stableToleranceGram"]     = _cfg->stableToleranceGram;
    doc["stableDurationMs"]        = _cfg->stableDurationMs;
    doc["minDrinkDeltaMl"]         = _cfg->minDrinkDeltaMl;
    doc["maxDrinkDeltaMl"]         = _cfg->maxDrinkDeltaMl;
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handlePostConfig() {
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }

    bool rebootRequired = false;

    // Hydration — clamp to [100, 9999]
    if (!doc["dailyGoalMl"].isNull()) {
        _cfg->dailyGoalMl = (uint32_t)constrain((long)doc["dailyGoalMl"], 100L, 9999L);
    }

    // Reminder — apply immediately
    if (!doc["reminderEnabled"].isNull()) {
        _cfg->reminderEnabled = (bool)doc["reminderEnabled"];
        _reminder->setEnabled(_cfg->reminderEnabled);
    }
    if (!doc["reminderIntervalMin"].isNull()) {
        _cfg->reminderIntervalMin =
            (uint32_t)constrain((long)doc["reminderIntervalMin"], 1L, 1440L);
        _reminder->setIntervalMin(_cfg->reminderIntervalMin);
    }
    if (!doc["reminderAlertTimeoutSec"].isNull()) {
        _cfg->reminderAlertTimeoutSec =
            (uint32_t)constrain((long)doc["reminderAlertTimeoutSec"], 5L, 3600L);
        _reminder->setAlertTimeoutSec(_cfg->reminderAlertTimeoutSec);
    }

    // Buzzer — apply immediately
    if (!doc["buzzerEnabled"].isNull()) {
        _cfg->buzzerEnabled = (bool)doc["buzzerEnabled"];
        _buzzer->setEnabled(_cfg->buzzerEnabled);
    }
    if (!doc["buzzerFrequencyHz"].isNull()) {
        _cfg->buzzerFrequencyHz =
            (uint32_t)constrain((long)doc["buzzerFrequencyHz"], 500L, 5000L);
        _buzzer->setFrequency(_cfg->buzzerFrequencyHz);
    }
    if (!doc["buzzerVolumePercent"].isNull()) {
        _cfg->buzzerVolumePercent =
            (uint8_t)constrain((long)doc["buzzerVolumePercent"], 0L, 100L);
        _buzzer->setVolume(_cfg->buzzerVolumePercent);
    }
    if (!doc["buzzerDurationMs"].isNull()) {
        _cfg->buzzerDurationMs =
            (uint32_t)constrain((long)doc["buzzerDurationMs"], 50L, 2000L);
        _buzzer->setDuration(_cfg->buzzerDurationMs);
    }

    // WiFi — needs reboot
    if (!doc["wifiSsid"].isNull()) {
        _cfg->wifiSsid = doc["wifiSsid"].as<String>();
        rebootRequired = true;
    }
    if (!doc["wifiPassword"].isNull()) {
        const String p = doc["wifiPassword"].as<String>();
        if (!p.isEmpty() && p != "****") {
            _cfg->wifiPassword = p;
            rebootRequired = true;
        }
    }

    // Discord Webhook — allow empty string to clear
    if (!doc["discordWebhookUrl"].isNull()) {
        const String u = doc["discordWebhookUrl"].as<String>();
        if (u.indexOf("****") < 0)
            _cfg->discordWebhookUrl = u;
    }

    // NTP — needs reboot
    if (!doc["ntpEnabled"].isNull()) {
        _cfg->ntpEnabled = (bool)doc["ntpEnabled"];
        rebootRequired = true;
    }
    if (!doc["ntpServer1"].isNull()) {
        _cfg->ntpServer1 = doc["ntpServer1"].as<String>();
        rebootRequired = true;
    }
    if (!doc["ntpServer2"].isNull()) {
        _cfg->ntpServer2 = doc["ntpServer2"].as<String>();
        rebootRequired = true;
    }
    {
        JsonVariant tzV = doc["timezoneOffsetSec"];
        if (!tzV.isNull() && (tzV.is<int>() || tzV.is<float>())) {
            _cfg->timezoneOffsetSec =
                (int)constrain((long)tzV.as<int>(), -43200L, 50400L);
            rebootRequired = true;
        }
    }

    // Advanced sensor settings — ScaleManager reads at init, needs reboot.
    // Type-check before apply: wrong type (e.g. string) would produce 0.0 and constrain to min.
    auto applyFloat = [&](const char* key, float& target, float lo, float hi) {
        JsonVariant v = doc[key];
        if (!v.isNull() && (v.is<float>() || v.is<int>())) {
            target = constrain(v.as<float>(), lo, hi);
            rebootRequired = true;
        }
    };
    auto applyUlong = [&](const char* key, uint32_t& target, long lo, long hi) {
        JsonVariant v = doc[key];
        if (!v.isNull() && (v.is<int>() || v.is<float>())) {
            target = (uint32_t)constrain((long)v.as<float>(), lo, hi);
            rebootRequired = true;
        }
    };
    auto applyUint16 = [&](const char* key, uint16_t& target, long lo, long hi) {
        JsonVariant v = doc[key];
        if (!v.isNull() && (v.is<int>() || v.is<float>())) {
            target = (uint16_t)constrain((long)v.as<float>(), lo, hi);
            rebootRequired = true;
        }
    };
    applyFloat("cupPresentThresholdGram", _cfg->cupPresentThresholdGram, 10.0f,  500.0f);
    applyFloat("stableToleranceGram",     _cfg->stableToleranceGram,      0.5f,   20.0f);
    applyUlong("stableDurationMs",        _cfg->stableDurationMs,          500L, 10000L);
    applyFloat("minDrinkDeltaMl",         _cfg->minDrinkDeltaMl,           5.0f,  100.0f);
    applyFloat("maxDrinkDeltaMl",         _cfg->maxDrinkDeltaMl,          50.0f, 1000.0f);

    // MQTT — needs reboot
    if (!doc["mqttEnabled"].isNull())    { _cfg->mqttEnabled    = (bool)doc["mqttEnabled"]; rebootRequired = true; }
    if (!doc["mqttBrokerHost"].isNull()) { _cfg->mqttBrokerHost = doc["mqttBrokerHost"].as<String>(); rebootRequired = true; }
    if (!doc["mqttUsername"].isNull())   { _cfg->mqttUsername   = doc["mqttUsername"].as<String>();   rebootRequired = true; }
    if (!doc["mqttClientId"].isNull()) {
        const String cid = doc["mqttClientId"].as<String>();
        _cfg->mqttClientId = cid.isEmpty() ? String(DEFAULT_MQTT_CLIENT_ID) : cid;
        rebootRequired = true;
    }
    if (!doc["mqttPassword"].isNull()) {
        const String p = doc["mqttPassword"].as<String>();
        if (!p.isEmpty() && p != "****") { _cfg->mqttPassword = p; rebootRequired = true; }
    }
    applyUint16("mqttBrokerPort",   _cfg->mqttBrokerPort,   1L, 65535L);
    applyUint16("mqttHeartbeatSec", _cfg->mqttHeartbeatSec, 5L, 3600L);

    _cfgMgr->save(*_cfg);

    JsonDocument resp;
    resp["ok"]              = true;
    resp["reboot_required"] = rebootRequired;
    String json; serializeJson(resp, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleTare() {
    if (!_scale->isReady()) {
        _server->send(503, "application/json", "{\"ok\":false,\"error\":\"HX711 not ready\"}");
        return;
    }
    if (!_scale->isSamplesReady()) {
        _server->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"Warming up, try again shortly\"}");
        return;
    }
    _scale->tare();
    _server->send(200, "application/json", "{\"ok\":true}");
}

void DashboardServer::_handleCalibrate() {
    if (!_scale->isReady()) {
        _server->send(503, "application/json", "{\"ok\":false,\"error\":\"HX711 not ready\"}");
        return;
    }
    if (!_scale->isSamplesReady()) {
        _server->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"Warming up, try again shortly\"}");
        return;
    }
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    if (!doc["known_weight_g"].is<float>()) {
        _server->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"known_weight_g required\"}");
        return;
    }
    const float knownG = doc["known_weight_g"].as<float>();
    if (knownG <= 0.0f) {
        _server->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"known_weight_g must be > 0\"}");
        return;
    }
    const float factor = _scale->calibrateWithKnownWeight(knownG);
    if (factor == 0.0f) {
        _server->send(500, "application/json",
                      "{\"ok\":false,\"error\":\"Calibration failed: net raw is zero\"}");
        return;
    }
    JsonDocument resp;
    resp["ok"]                 = true;
    resp["calibration_factor"] = factor;
    resp["current_weight_g"]   = _scale->getWeightGrams();
    String json; serializeJson(resp, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleWifiScan() {
    const int n = WiFi.scanNetworks(false, false, false, 100);

    if (n < 0) {
        const char* err = (n == WIFI_SCAN_RUNNING) ? "scan_busy" : "scan_failed";
        _server->send(503, "application/json",
                      String("{\"ok\":false,\"error\":\"") + err + "\"}");
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;
        bool found = false;
        for (JsonObject existing : arr) {
            if (existing["ssid"].as<String>() == ssid) {
                if (WiFi.RSSI(i) > existing["rssi"].as<int>())
                    existing["rssi"] = WiFi.RSSI(i);
                found = true;
                break;
            }
        }
        if (!found) {
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"]   = ssid;
            obj["rssi"]   = WiFi.RSSI(i);
            obj["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
    }
    WiFi.scanDelete();

    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleReboot() {
    _server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}

void DashboardServer::_handleLogs() {
    if (!_server->hasArg("month")) {
        _server->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"month parameter required (YYYY-MM or unsynced)\"}");
        return;
    }
    const String month = _server->arg("month");

    // Accept "unsynced" (NTP not synced at log time) or YYYY-MM with valid range
    const bool isUnsynced = (month == "unsynced");
    if (!isUnsynced) {
        bool valid = (month.length() == 7 && month[4] == '-');
        if (valid) {
            for (int i = 0; i < 4 && valid; i++) valid = isDigit(month[i]);
            for (int i = 5; i < 7 && valid; i++) valid = isDigit(month[i]);
        }
        if (valid) {
            const int mm = month.substring(5, 7).toInt();
            valid = (mm >= 1 && mm <= 12);
        }
        if (!valid) {
            _server->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"invalid month, expected YYYY-MM (01-12) or unsynced\"}");
            return;
        }
    }

    if (!_state->logFsOk || _logFs == nullptr) {
        _server->send(503, "application/json", "{\"ok\":false,\"error\":\"logfs unavailable\"}");
        return;
    }

    const String path = "/logs/drink-" + month + ".jsonl";
    File f = _logFs->open(path, "r");
    if (!f) {
        _server->send(200, "application/json",
                      "{\"ok\":true,\"month\":\"" + month + "\",\"entries\":[],\"skipped\":0}");
        return;
    }

    _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    _server->send(200, "application/json", "");
    _server->sendContent("{\"ok\":true,\"month\":\"" + month + "\",\"entries\":[");

    bool first = true;
    uint32_t skipped = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        JsonDocument entry;
        if (deserializeJson(entry, line)) { skipped++; continue; }

        String entryJson;
        serializeJson(entry, entryJson);
        if (!first) _server->sendContent(",");
        _server->sendContent(entryJson);
        first = false;
    }

    _server->sendContent("],\"skipped\":" + String(skipped) + "}");
    f.close();
}
