#include "DashboardServer.h"
#include <ArduinoJson.h>
#include <WiFi.h>

// ── Inline HTML pages ──────────────────────────────────────────────────────

static const char _INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html lang="zh-TW"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>水杯追蹤器</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px;background:#f5f5f5}
h2{color:#1a73e8;margin:0 0 12px}
.card{background:#fff;border-radius:8px;padding:14px 16px;margin:10px 0;box-shadow:0 1px 3px rgba(0,0,0,.15)}
.label{color:#888;font-size:.8em;margin-bottom:2px}
.value{font-size:1.6em;font-weight:bold;color:#212121}
.sub{color:#555;font-size:.9em;margin-top:2px}
.bar-bg{background:#e0e0e0;border-radius:4px;height:8px;margin:8px 0}
.bar{background:#1a73e8;border-radius:4px;height:8px;transition:width .4s}
nav{margin-bottom:12px}
nav a{margin-right:12px;color:#1a73e8;text-decoration:none;font-size:.95em}
</style></head><body>
<h2>&#128167; 水杯追蹤器</h2>
<nav><a href="/calibration">校正</a><a href="/settings">設定</a></nav>
<div class="card">
  <div class="label">目前重量</div>
  <div class="value" id="wt">--</div>
  <div class="sub" id="st"></div>
</div>
<div class="card">
  <div class="label">今日飲水</div>
  <div class="value"><span id="today">0</span> ml</div>
  <div class="bar-bg"><div class="bar" id="bar" style="width:0%"></div></div>
  <div class="sub" id="pct"></div>
</div>
<div class="card">
  <div class="label">上次喝水</div>
  <div class="value" id="last">--</div>
  <div class="label" style="margin-top:10px">下次提醒</div>
  <div style="font-size:1.1em;font-weight:bold;color:#388e3c" id="next">--</div>
</div>
<script>
function r(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('wt').textContent=(d.weight_g!=null?d.weight_g.toFixed(0)+' g':'--');
    document.getElementById('st').textContent=d.cup_state_name||'';
    var t=d.today_total_ml||0;
    document.getElementById('today').textContent=t.toFixed(0);
    var g=d.daily_goal_ml||2000,p=g>0?Math.min(100,Math.round(t*100/g)):0;
    document.getElementById('bar').style.width=p+'%';
    document.getElementById('pct').textContent=p+'% / 目標 '+g+' ml（'+( d.drink_count_today||0)+'次）';
    document.getElementById('last').textContent=d.last_drink_ml>0?(d.last_drink_ml.toFixed(0)+' ml'):'--';
    var s=d.next_reminder_sec||0;
    document.getElementById('next').textContent=s>0?(s>=60?Math.floor(s/60)+'分鐘後':s+'秒後'):'現在提醒！';
  }).catch(()=>{});
}
r();setInterval(r,2000);
</script></body></html>
)html";

static const char _SETTINGS_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html lang="zh-TW"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>設定</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px;background:#f5f5f5}
h2{color:#1a73e8}
h3{color:#555;margin:16px 0 4px;font-size:.85em;text-transform:uppercase;letter-spacing:.05em;border-bottom:1px solid #ddd;padding-bottom:4px}
label{font-size:.85em;color:#555;display:block;margin-top:6px}
input[type=text],input[type=number],input[type=password],input[type=url]{width:100%;padding:7px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;margin-top:2px}
.chk{display:flex;align-items:center;gap:6px;margin-top:6px}
button{padding:9px 18px;background:#1a73e8;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:.95em;margin-right:6px;margin-top:16px}
button.danger{background:#c62828}
#msg{margin-top:12px;padding:10px;border-radius:4px;display:none;font-size:.9em}
.ok{background:#e8f5e9;color:#1b5e20}.err{background:#ffebee;color:#b71c1c}
nav a{margin-right:12px;color:#1a73e8;text-decoration:none;font-size:.95em}
</style></head><body>
<h2>設定</h2>
<nav><a href="/">首頁</a><a href="/calibration">校正</a></nav>

<h3>飲水目標</h3>
<label>每日目標 (ml)</label><input id="dg" type="number" min="100" max="9999">

<h3>提醒</h3>
<label>間隔（分鐘）</label><input id="ri" type="number" min="1" max="1440">
<label>提醒警報持續時間（秒，拿起杯子可提前停止）</label><input id="rat" type="number" min="5" max="3600">
<div class="chk"><input id="re" type="checkbox"><label style="margin:0">啟用提醒</label></div>

<h3>蜂鳴器</h3>
<div class="chk"><input id="be" type="checkbox"><label style="margin:0">啟用蜂鳴器</label></div>
<label>頻率 (Hz)</label><input id="bf" type="number" min="500" max="5000">
<label>音量 (%)</label><input id="bv" type="number" min="0" max="100">
<label>持續 (ms)</label><input id="bd" type="number" min="50" max="2000">

<h3>WiFi（變更後需重新啟動）</h3>
<label>SSID</label><input id="ws" type="text">
<label>密碼（不修改請留空）</label><input id="wp" type="password" placeholder="留空 = 不修改">

<h3>Discord Webhook（選填）</h3>
<label>Webhook URL</label><input id="wh" type="url" placeholder="https://discord.com/api/webhooks/...">

<h3>NTP（變更後需重新啟動）</h3>
<div class="chk"><input id="ne" type="checkbox"><label style="margin:0">啟用 NTP 同步</label></div>
<label>伺服器 1</label><input id="n1" type="text">
<label>伺服器 2</label><input id="n2" type="text">
<label>時區字串</label><input id="tz" type="text" placeholder="Asia/Taipei">

<div>
  <button onclick="save()">儲存</button>
  <button class="danger" onclick="reboot()">重新啟動</button>
</div>
<div id="msg"></div>
<script>
function msg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';}
function load(){
  fetch('/api/config').then(r=>r.json()).then(d=>{
    document.getElementById('dg').value=d.dailyGoalMl||2000;
    document.getElementById('ri').value=d.reminderIntervalMin||60;
    document.getElementById('rat').value=d.reminderAlertTimeoutSec||60;
    document.getElementById('re').checked=!!d.reminderEnabled;
    document.getElementById('be').checked=!!d.buzzerEnabled;
    document.getElementById('bf').value=d.buzzerFrequencyHz||2000;
    document.getElementById('bv').value=d.buzzerVolumePercent||50;
    document.getElementById('bd').value=d.buzzerDurationMs||150;
    document.getElementById('ws').value=d.wifiSsid||'';
    document.getElementById('wh').value=d.discordWebhookUrl||'';
    document.getElementById('ne').checked=!!d.ntpEnabled;
    document.getElementById('n1').value=d.ntpServer1||'pool.ntp.org';
    document.getElementById('n2').value=d.ntpServer2||'time.google.com';
    document.getElementById('tz').value=d.timezone||'Asia/Taipei';
  });
}
function save(){
  var p=document.getElementById('wp').value;
  var wh=document.getElementById('wh').value;
  var d={
    dailyGoalMl:parseInt(document.getElementById('dg').value)||2000,
    reminderIntervalMin:parseInt(document.getElementById('ri').value)||60,
    reminderAlertTimeoutSec:parseInt(document.getElementById('rat').value)||60,
    reminderEnabled:document.getElementById('re').checked,
    buzzerEnabled:document.getElementById('be').checked,
    buzzerFrequencyHz:parseInt(document.getElementById('bf').value)||2000,
    buzzerVolumePercent:parseInt(document.getElementById('bv').value)||50,
    buzzerDurationMs:parseInt(document.getElementById('bd').value)||150,
    wifiSsid:document.getElementById('ws').value,
    ntpEnabled:document.getElementById('ne').checked,
    ntpServer1:document.getElementById('n1').value,
    ntpServer2:document.getElementById('n2').value,
    timezone:document.getElementById('tz').value
  };
  if(p) d.wifiPassword=p;
  if(wh.indexOf('****')<0) d.discordWebhookUrl=wh;
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})
    .then(r=>r.json()).then(j=>{
      if(j.ok) msg(j.reboot_required?'儲存成功。WiFi/NTP 變更需重新啟動。':'儲存成功！',true);
      else msg('錯誤：'+(j.error||'未知'),false);
    }).catch(()=>msg('連線失敗',false));
}
function reboot(){
  fetch('/api/reboot',{method:'POST'}).then(()=>msg('裝置重新啟動中...',true)).catch(()=>{});
}
load();
</script></body></html>
)html";

static const char _FALLBACK_CAL_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html lang="zh-TW"><head><meta charset="UTF-8"><title>校正</title></head><body>
<h2>校正頁面無法載入</h2>
<p>請執行 <code>pio run --target uploadfs</code> 後重新整理。</p>
<p>API 仍可用：<code>POST /api/tare</code>、<code>POST /api/calibrate</code>、<code>GET /api/weight</code></p>
</body></html>)html";

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
    // Show base URL up to the last '/' then ****
    int last = url.lastIndexOf('/');
    if (last > 0) return url.substring(0, last + 1) + "****";
    return "****";
}

// ── Lifecycle ──────────────────────────────────────────────────────────────

void DashboardServer::begin(ScaleManager& scale, ConfigManager& cfgMgr,
                            AppState& state, AppConfig& cfg,
                            BuzzerController& buzzer, ReminderManager& reminder) {
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
    _server   = new WebServer(80);

    _server->on("/",              HTTP_GET,  [this]{ _handleRoot();        });
    _server->on("/settings",      HTTP_GET,  [this]{ _handleSettings();    });
    _server->on("/calibration",   HTTP_GET,  [this]{ _handleCalibrationPage(); });
    _server->on("/api/weight",    HTTP_GET,  [this]{ _handleWeight();      });
    _server->on("/api/status",    HTTP_GET,  [this]{ _handleStatus();      });
    _server->on("/api/config",    HTTP_GET,  [this]{ _handleGetConfig();   });
    _server->on("/api/config",    HTTP_POST, [this]{ _handlePostConfig();  });
    _server->on("/api/tare",      HTTP_POST, [this]{ _handleTare();        });
    _server->on("/api/calibrate", HTTP_POST, [this]{ _handleCalibrate();   });
    _server->on("/api/wifi/scan", HTTP_GET,  [this]{ _handleWifiScan();    });
    _server->on("/api/reboot",    HTTP_POST, [this]{ _handleReboot();      });

    _server->begin();
    Serial.println("[Dashboard] HTTP server started on port 80");
}

void DashboardServer::loop() {
    if (_server) _server->handleClient();
}

// ── Page handlers ──────────────────────────────────────────────────────────

void DashboardServer::_handleRoot() {
    _server->send_P(200, "text/html", _INDEX_HTML);
}

void DashboardServer::_handleSettings() {
    _server->send_P(200, "text/html", _SETTINGS_HTML);
}

void DashboardServer::_handleCalibrationPage() {
    if (_state->fsOk) {
        File f = LittleFS.open("/calibration.html", "r");
        if (f) { _server->streamFile(f, "text/html"); f.close(); return; }
    }
    _server->send_P(200, "text/html", _FALLBACK_CAL_HTML);
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
    String json; serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void DashboardServer::_handleGetConfig() {
    JsonDocument doc;
    doc["ok"]                  = true;
    doc["wifiSsid"]            = _cfg->wifiSsid;
    doc["wifiPassword"]        = "****";
    doc["wifiPasswordSet"]     = !_cfg->wifiPassword.isEmpty();
    doc["discordWebhookUrl"]   = _maskWebhookUrl(_cfg->discordWebhookUrl);
    doc["reminderEnabled"]          = _cfg->reminderEnabled;
    doc["reminderIntervalMin"]      = _cfg->reminderIntervalMin;
    doc["reminderAlertTimeoutSec"]  = _cfg->reminderAlertTimeoutSec;
    doc["dailyGoalMl"]              = _cfg->dailyGoalMl;
    doc["buzzerEnabled"]       = _cfg->buzzerEnabled;
    doc["buzzerFrequencyHz"]   = _cfg->buzzerFrequencyHz;
    doc["buzzerDurationMs"]    = _cfg->buzzerDurationMs;
    doc["buzzerVolumePercent"] = _cfg->buzzerVolumePercent;
    doc["ntpEnabled"]          = _cfg->ntpEnabled;
    doc["ntpServer1"]          = _cfg->ntpServer1;
    doc["ntpServer2"]          = _cfg->ntpServer2;
    doc["timezone"]            = _cfg->timezone;
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
        const uint32_t v = (uint32_t)constrain((long)doc["dailyGoalMl"], 100L, 9999L);
        _cfg->dailyGoalMl = v;
    }

    // Reminder — apply immediately; clamp interval to [1, 1440 min]
    if (!doc["reminderEnabled"].isNull()) {
        _cfg->reminderEnabled = (bool)doc["reminderEnabled"];
        _reminder->setEnabled(_cfg->reminderEnabled);
    }
    if (!doc["reminderIntervalMin"].isNull()) {
        const uint32_t v = (uint32_t)constrain((long)doc["reminderIntervalMin"], 1L, 1440L);
        _cfg->reminderIntervalMin = v;
        _reminder->setIntervalMin(_cfg->reminderIntervalMin);
    }
    if (!doc["reminderAlertTimeoutSec"].isNull()) {
        const uint32_t v = (uint32_t)constrain((long)doc["reminderAlertTimeoutSec"], 5L, 3600L);
        _cfg->reminderAlertTimeoutSec = v;
        _reminder->setAlertTimeoutSec(_cfg->reminderAlertTimeoutSec);
    }

    // Buzzer — apply immediately with range validation
    if (!doc["buzzerEnabled"].isNull()) {
        _cfg->buzzerEnabled = (bool)doc["buzzerEnabled"];
        _buzzer->setEnabled(_cfg->buzzerEnabled);
    }
    if (!doc["buzzerFrequencyHz"].isNull()) {
        const uint32_t v = (uint32_t)constrain((long)doc["buzzerFrequencyHz"], 500L, 5000L);
        _cfg->buzzerFrequencyHz = v;
        _buzzer->setFrequency(_cfg->buzzerFrequencyHz);
    }
    if (!doc["buzzerVolumePercent"].isNull()) {
        const uint8_t v = (uint8_t)constrain((long)doc["buzzerVolumePercent"], 0L, 100L);
        _cfg->buzzerVolumePercent = v;
        _buzzer->setVolume(_cfg->buzzerVolumePercent);
    }
    if (!doc["buzzerDurationMs"].isNull()) {
        const uint32_t v = (uint32_t)constrain((long)doc["buzzerDurationMs"], 50L, 2000L);
        _cfg->buzzerDurationMs = v;
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
    if (!doc["timezone"].isNull()) {
        _cfg->timezone = doc["timezone"].as<String>();
        rebootRequired = true;
    }

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
        _server->send(503, "application/json", "{\"ok\":false,\"error\":\"Warming up, try again shortly\"}");
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
        _server->send(503, "application/json", "{\"ok\":false,\"error\":\"Warming up, try again shortly\"}");
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
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"known_weight_g required\"}");
        return;
    }
    const float knownG = doc["known_weight_g"].as<float>();
    if (knownG <= 0.0f) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"known_weight_g must be > 0\"}");
        return;
    }
    const float factor = _scale->calibrateWithKnownWeight(knownG);
    if (factor == 0.0f) {
        _server->send(500, "application/json", "{\"ok\":false,\"error\":\"Calibration failed: net raw is zero\"}");
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
    // 100ms per channel limits block time to ~1.3s across 13 channels
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

    // Deduplicate by SSID — keep strongest RSSI
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
