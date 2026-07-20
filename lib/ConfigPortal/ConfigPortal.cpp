#include "ConfigPortal.h"

#include <algorithm>
#include <cstdlib>

#include "AppState.h"
#include "ConfigManager.h"
#include "WiFiManager.h"
#include "cJSON.h"
#include "hal_time.h"
#include "hydracup_auth.h"
#include "http_server_support.h"

namespace {
const char* SETUP_HTML = R"HTML(
<!doctype html>
<html lang="zh-TW">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HydraCup 網路設定</title>
<style>
body{font:16px sans-serif;max-width:40rem;margin:2rem auto;padding:1rem}
label{display:block;margin-top:1rem}
input,select,button{box-sizing:border-box;padding:.6rem;width:100%;margin-top:.3rem}
button{cursor:pointer}
.message{margin:1rem 0;padding:.6rem;background:#eef6f5}
.error{background:#fff0f0;color:#a00}
.ok{background:#edf8ed;color:#175b17}
[hidden]{display:none!important}
</style>
</head>
<body>
<h1>HydraCup 網路設定</h1>
<p>連線至此設定 AP 後，輸入裝置要使用的 WiFi。</p>

<section id="login-panel" hidden>
  <h2>需要登入</h2>
  <p>此裝置已設定管理密碼，請先登入才能掃描或修改 WiFi。</p>
  <form id="login-form">
    <label>管理密碼<input id="login-password" type="password" autocomplete="current-password" required></label>
    <button id="login" type="submit">登入</button>
  </form>
  <div id="login-msg" class="message" role="alert"></div>
</section>

<section id="portal" hidden>
  <div id="msg" class="message" role="status"></div>
  <label>附近 WiFi
    <select id="nets"><option value="">按「掃描」取得附近網路</option></select>
  </label>
  <button id="scan" type="button">掃描</button>
  <label>WiFi SSID<input id="ssid" autocomplete="off"></label>
  <label>WiFi 密碼<input id="pass" type="password" autocomplete="new-password"></label>
  <button id="save" type="button">儲存並重新啟動</button>
</section>

<script>
let csrf = '';
let configured = false;
let authenticated = false;
const $ = id => document.getElementById(id);

function setMessage(id, value, ok) {
  const element = $(id);
  element.textContent = value || '';
  element.className = 'message ' + (ok ? 'ok' : 'error');
  element.style.display = value ? 'block' : 'none';
}

function setView() {
  const needsLogin = configured && !authenticated;
  $('login-panel').hidden = !needsLogin;
  $('portal').hidden = needsLogin;
}

function apiFetch(url, options) {
  options = options || {};
  const headers = new Headers(options.headers || {});
  const method = (options.method || 'GET').toUpperCase();
  if (method !== 'GET' && method !== 'HEAD' && csrf) headers.set('X-CSRF-Token', csrf);
  options.headers = headers;
  return fetch(url, options).then(response => {
    if (response.status === 401) {
      authenticated = false;
      setView();
    }
    return response;
  });
}

function scanWifi(showError) {
  const button = $('scan');
  const select = $('nets');
  const current = $('ssid').value;
  button.disabled = true;
  button.textContent = '掃描中…';
  setMessage('msg', '', false);
  return apiFetch('/api/wifi/scan', {cache: 'no-store'})
    .then(response => response.json().then(data => ({ok: response.ok && data.ok, data: data})))
    .then(result => {
      if (!result.ok) {
        if (showError) setMessage('msg', '掃描失敗：' + (result.data.error || '無法取得附近網路。'), false);
        return;
      }
      const networks = (result.data.networks || []).slice().sort((a, b) => b.rssi - a.rssi);
      select.textContent = '';
      const first = document.createElement('option');
      first.value = '';
      first.textContent = networks.length ? '附近網路（' + networks.length + ' 個）' : '找不到可見網路，請手動輸入';
      select.appendChild(first);
      networks.forEach(network => {
        const option = document.createElement('option');
        option.value = network.ssid;
        option.textContent = network.ssid + (network.secure ? ' · 加密' : ' · 開放') + ' · ' + network.rssi + ' dBm';
        select.appendChild(option);
      });
      if (current) select.value = current;
    })
    .catch(() => { if (showError) setMessage('msg', '掃描失敗：暫時無法取得附近網路。', false); })
    .finally(() => { button.disabled = false; button.textContent = '掃描'; });
}

function refreshAuth() {
  return fetch('/api/auth/csrf', {cache: 'no-store'})
    .then(response => response.json().then(data => ({ok: response.ok, data: data})))
    .then(result => {
      if (!result.ok) throw new Error('auth');
      csrf = result.data.csrf || '';
      configured = !!result.data.configured;
      authenticated = !!result.data.authenticated;
      setView();
      if (!configured || authenticated) return scanWifi(false);
    });
}

$('nets').addEventListener('change', () => { if ($('nets').value) $('ssid').value = $('nets').value; });
$('scan').addEventListener('click', () => scanWifi(true));

$('login-form').addEventListener('submit', event => {
  event.preventDefault();
  const password = $('login-password').value;
  if (!password || !csrf) { setMessage('login-msg', '請輸入管理密碼，並稍候安全驗證完成。', false); return; }
  const button = $('login');
  button.disabled = true;
  button.textContent = '驗證中…';
  fetch('/api/auth/login', {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-CSRF-Token': csrf},
    body: JSON.stringify({password: password})
  }).then(response => response.json().then(data => ({ok: response.ok && data.ok, data: data})))
    .then(result => {
      if (!result.ok) { setMessage('login-msg', result.data.error || '登入失敗。', false); return; }
      $('login-password').value = '';
      return refreshAuth();
    })
    .catch(() => setMessage('login-msg', '暫時無法連線，請稍後再試。', false))
    .finally(() => { button.disabled = false; button.textContent = '登入'; });
});

$('save').addEventListener('click', () => {
  const ssid = $('ssid').value.trim();
  if (!ssid) { setMessage('msg', '請輸入或選擇 WiFi SSID。', false); return; }
  apiFetch('/api/config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({wifi_ssid: ssid, wifi_password: $('pass').value})
  }).then(response => response.json().then(data => ({ok: response.ok && data.ok, data: data})))
    .then(result => setMessage('msg', result.ok ? '已儲存，裝置重新啟動中…' : (result.data.error || '儲存失敗。'), result.ok))
    .catch(() => setMessage('msg', '暫時無法連線，請稍後再試。', false));
});

refreshAuth().catch(() => setMessage('login-msg', '暫時無法連線，請重新整理頁面。', false));
</script>
</body>
</html>
)HTML";
std::string jsonString(cJSON* object) { char* text = cJSON_PrintUnformatted(object); if (!text) return "{}"; std::string result(text); std::free(text); return result; }
std::string stringValue(cJSON* object, const char* key) { cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key); return value && cJSON_IsString(value) && value->valuestring ? value->valuestring : ""; }
}

void ConfigPortal::begin(ConfigManager& cfgMgr, AppState& state, AppConfig& cfg, WiFiManager& wifi) {
    _cfgMgr = &cfgMgr; _state = &state; _cfg = &cfg; _wifi = &wifi; _csrfToken = http_random_token();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); config.server_port = 80; config.max_uri_handlers = 12; config.stack_size = 8192; config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&_server, &config) != ESP_OK) return;
    httpd_uri_t get = {}; get.uri = "/*"; get.method = HTTP_GET; get.handler = &_getHandler; get.user_ctx = this;
    httpd_uri_t post = {}; post.uri = "/*"; post.method = HTTP_POST; post.handler = &_postHandler; post.user_ctx = this;
    httpd_register_uri_handler(_server, &get); httpd_register_uri_handler(_server, &post);
}

esp_err_t ConfigPortal::_getHandler(httpd_req_t* request) { return static_cast<ConfigPortal*>(request->user_ctx)->_handleGet(request); }
esp_err_t ConfigPortal::_postHandler(httpd_req_t* request) { return static_cast<ConfigPortal*>(request->user_ctx)->_handlePost(request); }
void ConfigPortal::_sendJson(httpd_req_t* request, const std::string& json, int status) { http_send(request, "application/json", json, status); }

esp_err_t ConfigPortal::_handleGet(httpd_req_t* request) {
    std::string uri(request->uri); const size_t queryStart = uri.find('?'); if (queryStart != std::string::npos) uri.resize(queryStart);
    if (uri == "/") { http_send(request, "text/html", SETUP_HTML); return ESP_OK; }
    if (uri == "/api/auth/csrf") {
        const bool authenticated = _isSessionValid(request); cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true); cJSON_AddBoolToObject(doc, "configured", !_cfg->adminPasswordHash.empty()); cJSON_AddBoolToObject(doc, "authenticated", authenticated); cJSON_AddStringToObject(doc, "csrf", (authenticated ? _sessionCsrfToken : _csrfToken).c_str()); _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/status") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true); cJSON_AddStringToObject(doc, "mode", "ap"); cJSON_AddStringToObject(doc, "ap_ssid", _wifi->getAPSSID().c_str()); cJSON_AddStringToObject(doc, "ap_ip", _wifi->getAPIP().c_str()); _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    if (uri == "/api/wifi/scan") {
        if (!_requireApiAuth(request, false)) return ESP_OK;
        const uint32_t now = hal_millis(); if (now - _lastScanMs < SCAN_COOLDOWN_MS) { _sendJson(request, "{\"ok\":false,\"error\":\"scan_busy\"}", 429); return ESP_OK; } _lastScanMs = now;
        WifiNetwork networks[20]; const int count = _wifi->scan(networks, 20); if (count < 0) { _sendJson(request, "{\"ok\":false,\"error\":\"scan_failed\"}", 503); return ESP_OK; }
        cJSON* doc = cJSON_CreateObject(); cJSON_AddBoolToObject(doc, "ok", true); cJSON* list = cJSON_AddArrayToObject(doc, "networks"); for (int i = 0; i < count; ++i) { cJSON* item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "ssid", networks[i].ssid); cJSON_AddNumberToObject(item, "rssi", networks[i].rssi); cJSON_AddBoolToObject(item, "secure", networks[i].secure); cJSON_AddItemToArray(list, item); } _sendJson(request, jsonString(doc)); cJSON_Delete(doc); return ESP_OK;
    }
    http_send(request, "text/plain", "Not found", 404); return ESP_OK;
}

esp_err_t ConfigPortal::_handlePost(httpd_req_t* request) {
    std::string uri(request->uri); const size_t queryStart = uri.find('?'); if (queryStart != std::string::npos) uri.resize(queryStart);
    if (uri == "/api/auth/login") {
        const std::string ip = http_client_ip(request); if (_isRateLimited(ip)) { _sendAuthFailure(request, 429, "rate_limited"); return ESP_OK; } if (!http_constant_time_equal(http_request_header(request, "X-CSRF-Token"), _csrfToken)) { _sendAuthFailure(request, 403, "csrf_failed"); return ESP_OK; } if (_cfg->adminPasswordHash.empty()) { _sendAuthFailure(request, 409, "setup_required"); return ESP_OK; }
        std::string body; if (!http_read_body(request, body)) { _recordAuthFailure(ip); _sendAuthFailure(request, 400, "invalid_request"); return ESP_OK; } cJSON* doc = cJSON_ParseWithLength(body.c_str(), body.size()); if (!doc) { _recordAuthFailure(ip); _sendAuthFailure(request, 400, "invalid_request"); return ESP_OK; } const std::string password = stringValue(doc, "password"); cJSON_Delete(doc); if (!http_verify_password_hash(password, _cfg->adminPasswordHash)) { _recordAuthFailure(ip); _sendAuthFailure(request, 401, "invalid_credentials"); return ESP_OK; }
        _establishSession(); const std::string cookie = "session=" + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict"; httpd_resp_set_hdr(request, "Set-Cookie", cookie.c_str()); _sendJson(request, "{\"ok\":true,\"authenticated\":true}"); return ESP_OK;
    }
    if (uri == "/api/auth/logout") { if (!_requireApiAuth(request, true)) return ESP_OK; _clearSession(); httpd_resp_set_hdr(request, "Set-Cookie", "session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict"); _sendJson(request, "{\"ok\":true}"); return ESP_OK; }
    if (uri == "/api/config") {
        if (!_requireApiAuth(request, true)) return ESP_OK;
        std::string body; if (!http_read_body(request, body)) { _sendJson(request, "{\"ok\":false,\"error\":\"No body\"}", 400); return ESP_OK; } cJSON* doc = cJSON_ParseWithLength(body.c_str(), body.size()); if (!doc) { _sendJson(request, "{\"ok\":false,\"error\":\"Invalid JSON\"}", 400); return ESP_OK; } const std::string ssid = stringValue(doc, "wifi_ssid"); const std::string pass = stringValue(doc, "wifi_password"); cJSON_Delete(doc); if (ssid.empty()) { _sendJson(request, "{\"ok\":false,\"error\":\"SSID required\"}", 400); return ESP_OK; } if (!_cfgMgr->saveWifi(ssid, pass)) { _sendJson(request, "{\"ok\":false,\"error\":\"save_failed\"}", 500); return ESP_OK; } _sendJson(request, "{\"ok\":true}"); http_restart_after_response(); return ESP_OK;
    }
    if (uri == "/api/reboot") { if (!_requireApiAuth(request, true)) return ESP_OK; _sendJson(request, "{\"ok\":true}"); http_restart_after_response(); return ESP_OK; }
    http_send(request, "text/plain", "Not found", 404); return ESP_OK;
}

bool ConfigPortal::_isSessionValid(httpd_req_t* request) { if (_sessionToken.empty() || !http_constant_time_equal(http_cookie_value(http_request_header(request, "Cookie"), "session"), _sessionToken)) return false; const uint32_t now = hal_millis(); if (now - _sessionStartMs > SESSION_ABSOLUTE_TIMEOUT_MS || now - _lastActivityMs > SESSION_IDLE_TIMEOUT_MS) { _clearSession(); return false; } _lastActivityMs = now; return true; }
bool ConfigPortal::_hasValidCsrf(httpd_req_t* request) const { const std::string expected = _sessionToken.empty() ? _csrfToken : _sessionCsrfToken; return http_constant_time_equal(http_request_header(request, "X-CSRF-Token"), expected); }
bool ConfigPortal::_requireApiAuth(httpd_req_t* request, bool requireCsrf) { if (!_cfg->adminPasswordHash.empty() && !_isSessionValid(request)) { _sendJson(request, "{\"ok\":false,\"error\":\"authentication_required\"}", 401); return false; } if (requireCsrf && !_hasValidCsrf(request)) { _sendJson(request, "{\"ok\":false,\"error\":\"csrf_failed\"}", 403); return false; } return true; }
void ConfigPortal::_clearSession() { _sessionToken.clear(); _sessionCsrfToken.clear(); _sessionStartMs = 0; _lastActivityMs = 0; }
void ConfigPortal::_establishSession() { _sessionToken = http_random_token(); _sessionCsrfToken = http_random_token(); _sessionStartMs = hal_millis(); _lastActivityMs = _sessionStartMs; }
bool ConfigPortal::_isRateLimited(const std::string& ip) { const uint32_t now = hal_millis(); for (auto& bucket : _authFailures) if (bucket.ip == ip) { uint8_t kept = 0; for (uint8_t i = 0; i < bucket.count; ++i) if (now - bucket.timestamps[i] <= AUTH_FAILURE_WINDOW_MS) bucket.timestamps[kept++] = bucket.timestamps[i]; bucket.count = kept; bucket.lastSeen = now; return bucket.count >= AUTH_FAILURE_LIMIT; } return false; }
void ConfigPortal::_recordAuthFailure(const std::string& ip) { const uint32_t now = hal_millis(); AuthFailureBucket* bucket = nullptr; for (auto& value : _authFailures) if (value.ip == ip) { bucket = &value; break; } if (!bucket) for (auto& value : _authFailures) if (value.ip.empty()) { bucket = &value; break; } if (!bucket) bucket = &_authFailures[0]; bucket->ip = ip; uint8_t kept = 0; for (uint8_t i = 0; i < bucket->count; ++i) if (now - bucket->timestamps[i] <= AUTH_FAILURE_WINDOW_MS) bucket->timestamps[kept++] = bucket->timestamps[i]; bucket->count = kept; if (bucket->count < AUTH_FAILURE_LIMIT) bucket->timestamps[bucket->count++] = now; bucket->lastSeen = now; }
void ConfigPortal::_sendAuthFailure(httpd_req_t* request, int status, const char* error) { _sendJson(request, std::string("{\"ok\":false,\"error\":\"") + error + "\"}", status); }
