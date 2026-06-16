#include "ConfigPortal.h"
#include <ArduinoJson.h>

// Minimal WiFi setup page — full UI comes in Phase 5
const char* ConfigPortal::_setupPageHtml = R"rawhtml(
<!DOCTYPE html><html lang="zh-TW"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water Tracker Setup</title>
<style>
  body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px;background:#f5f5f5}
  h2{color:#1a73e8}
  input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px}
  button{width:100%;padding:10px;background:#1a73e8;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:1em}
  #msg{margin-top:12px;color:#388e3c;font-weight:bold}
</style>
</head><body>
<h2>水杯追蹤器 WiFi 設定</h2>
<label>WiFi SSID</label>
<input id="ssid" type="text" placeholder="WiFi 名稱">
<label>WiFi 密碼</label>
<input id="pass" type="password" placeholder="WiFi 密碼">
<button onclick="save()">儲存並重新啟動</button>
<div id="msg"></div>
<script>
function save(){
  var d={wifi_ssid:document.getElementById('ssid').value,
         wifi_password:document.getElementById('pass').value};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(d)}).then(r=>r.json()).then(j=>{
    document.getElementById('msg').textContent=j.ok?'儲存成功，裝置重新啟動中...':('錯誤：'+j.error);
  }).catch(()=>{document.getElementById('msg').textContent='連線失敗';});
}
</script>
</body></html>
)rawhtml";

void ConfigPortal::begin(ConfigManager& cfgMgr, AppState& state) {
    if (_server != nullptr) {
        Serial.println("[ConfigPortal] Already initialized");
        return;
    }
    _cfgMgr = &cfgMgr;
    _state  = &state;
    _server = new WebServer(80);

    _server->on("/",           HTTP_GET,  [this]{ _handleRoot();   });
    _server->on("/api/status", HTTP_GET,  [this]{ _handleStatus(); });
    _server->on("/api/config", HTTP_POST, [this]{ _handleConfig(); });
    _server->on("/api/reboot", HTTP_POST, [this]{ _handleReboot(); });

    _server->begin();
    Serial.println("[ConfigPortal] HTTP server started on port 80");
}

void ConfigPortal::loop() {
    if (_server) _server->handleClient();
}

void ConfigPortal::_handleRoot() {
    _server->send(200, "text/html", _setupPageHtml);
}

void ConfigPortal::_handleStatus() {
    JsonDocument doc;
    doc["ok"]      = true;
    doc["mode"]    = "ap";
    doc["ap_ssid"] = WiFi.softAPSSID();
    doc["ap_ip"]   = WiFi.softAPIP().toString();
    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void ConfigPortal::_handleConfig() {
    if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    const String ssid = doc["wifi_ssid"] | "";
    const String pass = doc["wifi_password"] | "";
    if (ssid.isEmpty()) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"SSID required\"}");
        return;
    }
    _cfgMgr->saveWifi(ssid, pass);
    _server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}

void ConfigPortal::_handleReboot() {
    _server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}
