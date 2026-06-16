#include "ConfigPortal.h"
#include <ArduinoJson.h>

const char* ConfigPortal::_setupPageHtml = R"rawhtml(
<!DOCTYPE html><html lang="zh-TW"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Water Tracker Setup</title>
<style>
  body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px;background:#f5f5f5}
  h2{color:#1a73e8;margin-bottom:4px}
  p.hint{font-size:.8em;color:#888;margin:0 0 16px}
  label{display:block;font-size:.85em;color:#555;margin-bottom:3px}
  input,select{width:100%;padding:8px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;font-size:1em;background:#fff}
  .field{margin-bottom:14px}
  .scan-row{display:flex;gap:8px;align-items:stretch;margin-bottom:14px}
  .scan-row select{flex:1;min-width:0}
  .scan-row button{padding:8px 14px;background:#1565c0;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:.9em;white-space:nowrap}
  .scan-row button:disabled{opacity:.5;cursor:default}
  .save-btn{width:100%;padding:11px;background:#1a73e8;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:1em;margin-top:4px}
  #msg{margin-top:12px;font-weight:bold}
  .ok{color:#388e3c}.err{color:#c62828}
</style>
</head><body>
<h2>水杯追蹤器 WiFi 設定</h2>
<p class="hint">掃描附近 WiFi 後從下拉選單選擇，再輸入密碼儲存。</p>

<label>附近 WiFi 網路</label>
<div class="scan-row">
  <select id="nets" onchange="pickNet(this)">
    <option value="">-- 按右側按鈕掃描 --</option>
  </select>
  <button id="scanBtn" onclick="scanWifi()">掃描</button>
</div>

<div class="field">
  <label>WiFi SSID（已選擇或手動輸入）</label>
  <input id="ssid" type="text" placeholder="WiFi 名稱">
</div>
<div class="field">
  <label>WiFi 密碼</label>
  <input id="pass" type="password" placeholder="WiFi 密碼">
</div>
<button class="save-btn" onclick="save()">儲存並重新啟動</button>
<div id="msg"></div>

<script>
function rssiBar(r){return r>=-60?'████':r>=-70?'███░':r>=-80?'██░░':'█░░░';}
function scanWifi(){
  var btn=document.getElementById('scanBtn');
  var sel=document.getElementById('nets');
  btn.disabled=true;btn.textContent='掃描中…';
  fetch('/api/wifi/scan').then(function(r){return r.json();}).then(function(j){
    sel.innerHTML='';
    if(!j.ok){
      sel.innerHTML='<option value="">掃描失敗：'+( j.error||'未知錯誤')+'，請稍後重試</option>';
    } else if(j.networks&&j.networks.length>0){
      j.networks.sort(function(a,b){return b.rssi-a.rssi;});
      var ph=document.createElement('option');
      ph.value='';ph.textContent='-- 選擇 WiFi（共 '+j.networks.length+' 個）--';
      sel.appendChild(ph);
      j.networks.forEach(function(n){
        var opt=document.createElement('option');
        opt.value=n.ssid;
        opt.textContent=rssiBar(n.rssi)+' '+n.ssid+(n.secure?' 🔒':'');
        sel.appendChild(opt);
      });
    } else {
      sel.innerHTML='<option value="">未找到任何 WiFi，請重試</option>';
    }
  }).catch(function(){
    sel.innerHTML='<option value="">連線失敗，請重試</option>';
  }).then(function(){
    btn.disabled=false;btn.textContent='重新掃描';
  });
}
function pickNet(sel){
  if(sel.value) document.getElementById('ssid').value=sel.value;
}
function save(){
  var ssid=document.getElementById('ssid').value.trim();
  var pass=document.getElementById('pass').value;
  var msg=document.getElementById('msg');
  if(!ssid){msg.className='err';msg.textContent='請輸入或選擇 WiFi SSID';return;}
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({wifi_ssid:ssid,wifi_password:pass})})
    .then(function(r){return r.json();}).then(function(j){
      msg.className=j.ok?'ok':'err';
      msg.textContent=j.ok?'儲存成功，裝置重新啟動中…':'錯誤：'+j.error;
    }).catch(function(){msg.className='err';msg.textContent='連線失敗';});
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

    _server->on("/",              HTTP_GET,  [this]{ _handleRoot();     });
    _server->on("/api/status",   HTTP_GET,  [this]{ _handleStatus();   });
    _server->on("/api/config",   HTTP_POST, [this]{ _handleConfig();   });
    _server->on("/api/wifi/scan",HTTP_GET,  [this]{ _handleWifiScan(); });
    _server->on("/api/reboot",   HTTP_POST, [this]{ _handleReboot();   });

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

void ConfigPortal::_handleWifiScan() {
    if (millis() - _lastScanMs < SCAN_COOLDOWN_MS) {
        _server->send(429, "application/json", "{\"ok\":false,\"error\":\"scan_busy\"}");
        return;
    }
    _lastScanMs = millis();

    // 100ms per channel; ~1.3s total across 13 channels
    const int n = WiFi.scanNetworks(false, false, false, 100);
    if (n < 0) {
        const char* err = (n == WIFI_SCAN_RUNNING) ? "scan_busy" : "scan_failed";
        _server->sendHeader("Cache-Control", "no-store");
        _server->send(503, "application/json",
                      String("{\"ok\":false,\"error\":\"") + err + "\"}");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        const String ssid   = WiFi.SSID(i);
        const int    rssi   = WiFi.RSSI(i);
        const bool   secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        if (ssid.isEmpty()) continue;
        bool found = false;
        for (JsonObject existing : arr) {
            if (existing["ssid"].as<String>() == ssid) {
                if (rssi > existing["rssi"].as<int>()) {
                    existing["rssi"]   = rssi;
                    existing["secure"] = secure;  // sync together with stronger signal
                }
                found = true;
                break;
            }
        }
        if (!found) {
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"]   = ssid;
            obj["rssi"]   = rssi;
            obj["secure"] = secure;
        }
    }
    WiFi.scanDelete();
    String json; serializeJson(doc, json);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", json);
}

void ConfigPortal::_handleReboot() {
    _server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}
