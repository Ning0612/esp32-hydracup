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
<title>HydraCup / 網路設定</title>
<script>
(function () {
  var key = 'iot-ui-theme', stored = null;
  try { stored = localStorage.getItem(key); } catch (e) {}
  var theme = (stored === 'light' || stored === 'dark') ? stored :
    (matchMedia('(prefers-color-scheme:dark)').matches ? 'dark' : 'light');
  document.documentElement.dataset.theme = theme;
})();
</script>
<style>
:root{color-scheme:light;--paper:#f1ede2;--surface:#fbf8ef;--surface-2:#e7e3d6;--ink:#17231f;--ink-soft:#263a34;--line:#b9b8aa;--muted:#68736c;--teal:#0b716a;--teal-dark:#07534f;--coral:#cf5a47;--coral-dark:#a94436;--amber:#bd812d;--shadow:rgba(23,35,31,.15);--inset:#fffdf7;--input-border:#7e9188;--grid-dot:rgba(23,35,31,.045);--danger-surface:#fff6ed;--mint:#a8d2c4;--on-dark:#fbf8ef;--on-light:#17231f;--dark-block-muted:#aab8ae;--nav-border:#71847a;--on-ring:rgba(168,210,196,.18)}
:root[data-theme="dark"]{color-scheme:dark;--paper:#141b18;--surface:#1c2622;--surface-2:#24312c;--ink:#eef1ea;--ink-soft:#0a100d;--line:#3a4a43;--muted:#93a69c;--teal:#1fae9c;--teal-dark:#167d70;--coral:#e2705a;--coral-dark:#b8543f;--amber:#d99a3f;--shadow:rgba(0,0,0,.4);--inset:#101512;--input-border:#48584f;--grid-dot:rgba(255,255,255,.035);--danger-surface:#241a16}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;min-height:100vh;overflow-x:hidden;background-color:var(--paper);background-image:linear-gradient(var(--grid-dot) 1px,transparent 1px),linear-gradient(90deg,var(--grid-dot) 1px,transparent 1px);background-size:20px 20px;color:var(--ink);font-family:Georgia,'Noto Serif TC',serif}button,input,select{font:700 .82rem Consolas,monospace}button,a,input,select{-webkit-tap-highlight-color:transparent}
.topbar{padding:1rem;background:var(--ink-soft);border-bottom:5px solid var(--coral);color:var(--on-dark)}.topbar-in{max-width:1040px;margin:auto;display:flex;align-items:center;justify-content:space-between;gap:1rem}.brand{display:flex;align-items:center;gap:.7rem;color:inherit;text-decoration:none}.brand-mark{display:grid;place-items:center;width:2.25rem;height:2.25rem;border:2px solid var(--mint);color:var(--mint);font:700 1rem Consolas,monospace}.brand-copy{color:inherit;font:700 .82rem Consolas,monospace;letter-spacing:.08em}.brand-sub{display:block;margin-top:.22rem;color:var(--dark-block-muted);font:400 .68rem Consolas,monospace;letter-spacing:.02em}.topbar-actions{display:flex;align-items:center;justify-content:flex-end;gap:.6rem;flex-wrap:wrap}.theme-toggle{display:flex;overflow:hidden;border:1px solid var(--nav-border)}.theme-toggle button{min-width:0;padding:.46rem .6rem;border:0;background:transparent;color:var(--on-dark);font-size:.68rem;letter-spacing:.04em}.theme-toggle button+button{border-left:1px solid var(--nav-border)}.theme-toggle button.active{background:var(--mint);color:var(--on-light)}.theme-toggle button:hover:not(.active){background:var(--on-ring)}button:focus-visible{outline:2px solid var(--mint);outline-offset:2px}
.wrap{max-width:1040px;margin:auto;padding:1.4rem 1rem 3rem}.intro{display:flex;align-items:flex-end;justify-content:space-between;gap:1.5rem;margin:.4rem 0 1.25rem}.kicker,.label{color:var(--teal);font:700 .72rem Consolas,monospace;letter-spacing:.14em;text-transform:uppercase}.intro h1{margin:.45rem 0 0;font-size:clamp(2rem,6vw,4rem);line-height:.92;letter-spacing:-.06em}.intro p{max-width:30rem;margin:0;color:var(--muted);line-height:1.55}.card{position:relative;margin-bottom:1rem;padding:1.05rem;background:var(--surface);border:1px solid var(--ink);box-shadow:5px 5px 0 var(--line)}.card::before{content:'';position:absolute;top:0;left:0;width:42px;height:4px;background:var(--teal)}.card h2{display:flex;align-items:baseline;gap:.55rem;margin:0 0 1rem;padding-bottom:.65rem;border-bottom:1px solid var(--line);font-size:1.05rem}.card h2 span{color:var(--coral);font:700 .7rem Consolas,monospace}.section-note{margin:-.45rem 0 .9rem;color:var(--muted);font-size:.82rem;line-height:1.45}.info{margin:.85rem 0 0;padding:.65rem .75rem;border-left:3px solid var(--teal);background:var(--surface-2);color:var(--muted);font-size:.82rem;line-height:1.5}.hint{margin:.35rem 0 0;color:var(--muted);font:400 .76rem Consolas,monospace}.status-banner{display:flex;align-items:center;justify-content:space-between;gap:1rem;margin-bottom:1rem;padding:1rem 1.1rem;background:var(--ink-soft);color:var(--on-dark);box-shadow:5px 5px 0 var(--line)}.status-label{color:var(--mint);font:700 .7rem Consolas,monospace;letter-spacing:.12em}.status-value{margin-top:.25rem;font-size:1.5rem}.updated{color:var(--dark-block-muted);font:400 .7rem Consolas,monospace;text-align:right}.updated strong{color:var(--on-dark)}
form{display:grid;grid-template-columns:1fr;gap:1rem}.login-card{max-width:30rem;margin:2rem auto 1rem}.field{margin-bottom:.85rem}.field:last-child{margin-bottom:0}.field label{display:block;margin-bottom:.3rem;color:var(--muted);font:700 .72rem Consolas,monospace;letter-spacing:.03em}input,select{width:100%;padding:.64rem .68rem;border:1px solid var(--input-border);border-radius:0;background:var(--inset);color:var(--ink)}input:focus,select:focus{outline:2px solid var(--mint);outline-offset:1px;border-color:var(--teal)}input::placeholder{color:var(--muted);font-weight:400}option{background:var(--inset);color:var(--ink)}.field-with-action{display:flex;align-items:stretch;gap:.5rem}.field-with-action select{flex:1;min-width:0}.field-with-action button{flex:none;white-space:nowrap}button,.btn{min-width:150px;padding:.7rem .85rem;border:1px solid var(--teal-dark);border-radius:0;cursor:pointer;background:var(--teal);color:var(--on-dark)}button:hover,.btn:hover{background:var(--teal-dark)}button:disabled{opacity:.55;cursor:wait}.btn-primary{background:var(--teal);color:var(--on-dark)}button.ghost{background:transparent;border-color:var(--teal);color:var(--teal)}button.ghost:hover{background:var(--teal);color:var(--on-dark)}.actions{display:flex;flex-wrap:wrap;gap:.6rem}.actions button{flex:1;min-width:170px}.message{display:none;margin:0 0 1rem;padding:.8rem 1rem;border:1px solid var(--teal);border-left:5px solid var(--teal);background:var(--surface);color:var(--teal-dark);font:700 .8rem Consolas,monospace}.message.error{display:block;border-color:var(--coral);color:var(--coral)}.message.ok{display:block}.warn-banner{margin-bottom:1rem;padding:.75rem .9rem;background:var(--surface-2);border:1px solid var(--amber);border-left:3px solid var(--amber);color:var(--ink);font:700 .78rem Consolas,monospace}[hidden]{display:none!important}
.site-footer{margin-top:2rem;padding:1.2rem 1rem;background:var(--ink-soft);color:var(--dark-block-muted)}.footer-in{max-width:1040px;margin:auto;display:flex;align-items:center;justify-content:space-between;gap:1rem;flex-wrap:wrap}.footer-meta{color:var(--dark-block-muted);font:400 .7rem Consolas,monospace}.footer-repo{display:flex;align-items:baseline;gap:.5rem;font:700 .74rem Consolas,monospace}.footer-repo .label{color:var(--mint);text-transform:uppercase;letter-spacing:.12em}
@media(max-width:720px){.topbar-in,.intro{align-items:flex-start;flex-direction:column}.topbar-actions{justify-content:flex-start;width:100%}.status-banner{align-items:flex-start;flex-direction:column}.updated{text-align:left}.wrap{padding-top:1rem}}@media(max-width:480px){.field-with-action{flex-direction:column}.field-with-action button{width:100%}.actions button{width:100%}.footer-in{flex-direction:column;align-items:flex-start}}
</style>
</head>
<body>
<header class="topbar"><div class="topbar-in">
  <div class="brand"><span class="brand-mark">HC</span><span class="brand-copy">HYDRACUP<span class="brand-sub">LOCAL DEVICE / WATER TRACKER</span></span></div>
  <div class="topbar-actions">
    <div class="theme-toggle" role="group" aria-label="色彩主題"><button type="button" data-theme-choice="light" aria-pressed="false">LIGHT</button><button type="button" data-theme-choice="dark" aria-pressed="false">DARK</button></div>
  </div>
</div></header>

<main class="wrap">
  <div class="intro"><div><div class="kicker">00 / provisioning</div><h1>網路設定</h1></div><p>HydraCup 目前以設定 AP 提供本頁。首次設定可直接輸入 WiFi；已設定管理密碼的裝置需要先登入。</p></div>

  <section class="card login-card" id="login-panel" hidden>
    <div class="kicker">01 / access</div>
    <h2>登入 recovery AP</h2>
    <p class="section-note">此裝置已設定管理密碼，請先登入才能掃描或修改 WiFi。</p>
    <div id="login-msg" class="message" role="alert" aria-live="polite"></div>
    <form id="login-form" class="login-form">
      <div class="field"><label for="login-password">管理密碼</label><input id="login-password" type="password" autocomplete="current-password" required></div>
      <button id="login" type="submit">登入</button>
    </form>
  </section>

  <section id="portal" hidden>
    <div id="ap-status" class="status-banner"><div><div class="status-label">AP MODE / SETUP PORTAL</div><div class="status-value">設定 WiFi</div></div><div class="updated">ADDRESS<br><strong id="ap-ip">192.168.4.1</strong></div></div>
    <div id="msg" class="message" role="status" aria-live="polite"></div>
    <section class="card"><h2><span>01</span>選擇 WiFi</h2><p class="info">掃描清單只顯示附近可見網路；也可以直接輸入隱藏 SSID。</p>
      <div class="field"><label for="nets">附近 WiFi 網路</label><div class="field-with-action"><select id="nets"><option value="">尚未掃描</option></select><button class="ghost" id="scan" type="button">掃描</button></div><p class="hint" id="scan-hint" aria-live="polite">掃描結果會保留目前選取的 SSID。</p></div>
      <div class="field"><label for="ssid">WiFi SSID（可手動輸入）</label><input id="ssid" type="text" autocomplete="off" placeholder="請輸入或選擇 WiFi SSID"></div>
    </section>
    <section class="card"><h2><span>02</span>儲存設定</h2>
      <div class="field"><label for="pass">WiFi 密碼</label><input id="pass" type="password" autocomplete="current-password" placeholder="WiFi 密碼"></div>
      <div class="actions"><button class="btn btn-primary" id="save" type="button">儲存並重新啟動</button></div>
      <p class="info">儲存後裝置會重新啟動並嘗試連線；成功後請改用 OLED 顯示的 IP 開啟 WebUI。</p>
    </section>
  </section>
</main>

<footer class="site-footer"><div class="footer-in"><div class="footer-repo"><span class="label">LOCAL AP</span><span>HydraCup provisioning portal</span></div><div class="footer-meta">ESP32 local device · HTTP setup</div></div></footer>

<script>
let csrf = '';
let configured = false;
let authenticated = false;
const $ = id => document.getElementById(id);

function syncThemeButtons() {
  const current = document.documentElement.dataset.theme;
  document.querySelectorAll('[data-theme-choice]').forEach(button => {
    const active = button.getAttribute('data-theme-choice') === current;
    button.classList.toggle('active', active);
    button.setAttribute('aria-pressed', active ? 'true' : 'false');
  });
}

function setTheme(theme) {
  if (theme !== 'light' && theme !== 'dark') return;
  document.documentElement.dataset.theme = theme;
  try { localStorage.setItem('iot-ui-theme', theme); } catch (e) {}
  syncThemeButtons();
}

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

function loadStatus() {
  return apiFetch('/api/status', {cache: 'no-store'}).then(response => response.ok ? response.json() : null)
    .then(data => { if (data && data.ap_ip) $('ap-ip').textContent = data.ap_ip; })
    .catch(() => {});
}

function scanWifi(showError) {
  const button = $('scan');
  const select = $('nets');
  const current = $('ssid').value;
  button.disabled = true;
  button.textContent = '掃描中…';
  $('scan-hint').textContent = '正在搜尋附近網路…';
  setMessage('msg', '', false);
  return apiFetch('/api/wifi/scan', {cache: 'no-store'})
    .then(response => response.json().then(data => ({ok: response.ok && data.ok, data: data})))
    .then(result => {
      if (!result.ok) {
        $('scan-hint').textContent = '掃描未完成，請稍後重試或手動輸入。';
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
      $('scan-hint').textContent = networks.length ? '已依訊號強度排序；也可以手動輸入隱藏 SSID。' : '沒有找到可見網路，請手動輸入 SSID。';
      if (current) select.value = current;
    })
    .catch(() => { $('scan-hint').textContent = '掃描未完成，請稍後重試或手動輸入。'; if (showError) setMessage('msg', '掃描失敗：暫時無法取得附近網路。', false); })
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
      if (!configured || authenticated) { loadStatus(); return scanWifi(false); }
      $('login-password').focus();
    });
}

document.querySelectorAll('[data-theme-choice]').forEach(button => button.addEventListener('click', () => setTheme(button.getAttribute('data-theme-choice'))));
syncThemeButtons();
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

refreshAuth().catch(() => { configured = false; authenticated = false; setView(); setMessage('msg', '安全狀態暫時無法取得，請重新整理頁面。', false); });
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
