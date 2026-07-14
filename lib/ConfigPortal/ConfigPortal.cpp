#include "ConfigPortal.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t AUTH_TOKEN_BYTES = 16;
constexpr size_t PASSWORD_SALT_BYTES = 16;
constexpr size_t PASSWORD_HASH_BYTES = 32;

String randomToken() {
    uint8_t bytes[AUTH_TOKEN_BYTES];
    esp_fill_random(bytes, sizeof(bytes));
    static const char* hex = "0123456789abcdef";
    String token;
    token.reserve(sizeof(bytes) * 2);
    for (uint8_t byte : bytes) {
        token += hex[byte >> 4];
        token += hex[byte & 0x0f];
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    return token;
}

bool constantTimeEqual(const String& left, const String& right) {
    const size_t maxLength = left.length() > right.length() ? left.length() : right.length();
    size_t difference = left.length() ^ right.length();
    for (size_t i = 0; i < maxLength; ++i) {
        const uint8_t a = i < left.length() ? (uint8_t)left[i] : 0;
        const uint8_t b = i < right.length() ? (uint8_t)right[i] : 0;
        difference |= (size_t)(a ^ b);
    }
    return difference == 0;
}

bool constantTimeEqualBytes(const uint8_t* left, const uint8_t* right, size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= (uint8_t)(left[i] ^ right[i]);
    return difference == 0;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool hexDecode(const String& input, uint8_t* output, size_t outputLength) {
    if (input.length() != outputLength * 2) return false;
    for (size_t i = 0; i < outputLength; ++i) {
        const int high = hexValue(input[i * 2]);
        const int low = hexValue(input[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool hmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* input, size_t inputLength,
                uint8_t output[PASSWORD_HASH_BYTES]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != nullptr &&
           mbedtls_md_hmac(info, key, keyLength, input, inputLength, output) == 0;
}

bool pbkdf2Sha256(const String& password, const uint8_t* salt, size_t saltLength,
                  uint32_t iterations, uint8_t output[PASSWORD_HASH_BYTES]) {
    if (password.isEmpty() || saltLength > 60 || iterations == 0) return false;

    uint8_t block[64] = {};
    uint8_t u[PASSWORD_HASH_BYTES] = {};
    uint8_t nextU[PASSWORD_HASH_BYTES] = {};
    uint8_t t[PASSWORD_HASH_BYTES] = {};
    memcpy(block, salt, saltLength);
    block[saltLength]     = 0;
    block[saltLength + 1] = 0;
    block[saltLength + 2] = 0;
    block[saltLength + 3] = 1;

    const uint8_t* passwordBytes = reinterpret_cast<const uint8_t*>(password.c_str());
    const size_t passwordLength = password.length();
    if (!hmacSha256(passwordBytes, passwordLength, block, saltLength + 4, u)) return false;
    memcpy(t, u, sizeof(t));

    for (uint32_t round = 1; round < iterations; ++round) {
        if (!hmacSha256(passwordBytes, passwordLength, u, sizeof(u), nextU)) return false;
        for (size_t i = 0; i < sizeof(t); ++i) t[i] ^= nextU[i];
        memcpy(u, nextU, sizeof(u));
    }

    memcpy(output, t, sizeof(t));
    mbedtls_platform_zeroize(block, sizeof(block));
    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(nextU, sizeof(nextU));
    mbedtls_platform_zeroize(t, sizeof(t));
    return true;
}

bool verifyPasswordHash(const String& password, const String& stored) {
    const int first = stored.indexOf('$');
    const int second = first < 0 ? -1 : stored.indexOf('$', first + 1);
    const int third = second < 0 ? -1 : stored.indexOf('$', second + 1);
    if (first <= 0 || second <= first || third <= second) return false;

    if (!constantTimeEqual(stored.substring(0, first), "pbkdf2-sha256")) return false;
    const String iterationText = stored.substring(first + 1, second);
    char* end = nullptr;
    const unsigned long parsedIterations = strtoul(iterationText.c_str(), &end, 10);
    if (end == iterationText.c_str() || *end != '\0' ||
        parsedIterations == 0 || parsedIterations > 1000000UL) return false;

    uint8_t salt[PASSWORD_SALT_BYTES] = {};
    uint8_t expected[PASSWORD_HASH_BYTES] = {};
    uint8_t actual[PASSWORD_HASH_BYTES] = {};
    const bool decoded = hexDecode(stored.substring(second + 1, third), salt, sizeof(salt)) &&
                         hexDecode(stored.substring(third + 1), expected, sizeof(expected));
    const bool derived = decoded && pbkdf2Sha256(password, salt, sizeof(salt),
                                                 (uint32_t)parsedIterations, actual);
    const bool matches = derived && constantTimeEqualBytes(actual, expected, sizeof(actual));
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(expected, sizeof(expected));
    mbedtls_platform_zeroize(actual, sizeof(actual));
    return matches;
}

String cookieValue(const String& header, const char* name) {
    const String prefix = String(name) + "=";
    int start = 0;
    while (start < (int)header.length()) {
        while (start < (int)header.length() && (header[start] == ' ' || header[start] == ';')) ++start;
        const int separator = header.indexOf(';', start);
        const int end = separator < 0 ? header.length() : separator;
        if (header.substring(start, end).startsWith(prefix))
            return header.substring(start + prefix.length(), end);
        if (separator < 0) break;
        start = separator + 1;
    }
    return String();
}

}  // namespace

const char* ConfigPortal::_setupPageHtml = R"rawhtml(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HydraCup / 網路設定</title>
<script>
(function(){
  var key='iot-ui-theme',stored=null;
  try{stored=localStorage.getItem(key)}catch(e){}
  var theme=(stored==='light'||stored==='dark')?stored:(matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light');
  document.documentElement.dataset.theme=theme;
})();
</script>
<style>
:root{color-scheme:light;--paper:#f1ede2;--surface:#fbf8ef;--surface-2:#e7e3d6;--ink:#17231f;--ink-soft:#263a34;--line:#b9b8aa;--muted:#68736c;--teal:#0b716a;--teal-dark:#07534f;--coral:#cf5a47;--coral-dark:#a94436;--amber:#bd812d;--inset:#fffdf7;--input-border:#7e9188;--grid-dot:rgba(23,35,31,.045);--mint:#a8d2c4;--on-dark:#fbf8ef;--on-light:#17231f;--dark-block-muted:#aab8ae;--nav-border:#71847a;--on-ring:rgba(168,210,196,.18)}
:root[data-theme="dark"]{color-scheme:dark;--paper:#141b18;--surface:#1c2622;--surface-2:#24312c;--ink:#eef1ea;--ink-soft:#0a100d;--line:#3a4a43;--muted:#93a69c;--teal:#1fae9c;--teal-dark:#167d70;--coral:#e2705a;--coral-dark:#b8543f;--amber:#d99a3f;--inset:#101512;--input-border:#48584f;--grid-dot:rgba(255,255,255,.035)}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background-color:var(--paper);background-image:linear-gradient(var(--grid-dot) 1px,transparent 1px),linear-gradient(90deg,var(--grid-dot) 1px,transparent 1px);background-size:20px 20px;color:var(--ink);font-family:Georgia,'Noto Serif TC',serif}.topbar{padding:1rem;background:var(--ink-soft);border-bottom:5px solid var(--coral);color:var(--on-dark)}.topbar-in,main,footer>div{max-width:760px;margin:auto}.topbar-in{display:flex;align-items:center;justify-content:space-between;gap:1rem}.brand{display:flex;align-items:center;gap:.7rem;color:inherit;text-decoration:none}.brand-mark{display:grid;place-items:center;width:2.25rem;height:2.25rem;border:2px solid var(--mint);color:var(--mint);font:700 1rem Consolas,monospace}.brand-copy{font:700 .82rem Consolas,monospace;letter-spacing:.08em}.brand-sub{display:block;margin-top:.22rem;color:var(--dark-block-muted);font:400 .68rem Consolas,monospace}.theme-toggle{display:flex;overflow:hidden;border:1px solid var(--nav-border)}.theme-toggle button{padding:.46rem .6rem;border:0;background:transparent;color:var(--on-dark);font:700 .68rem Consolas,monospace;cursor:pointer}.theme-toggle button+button{border-left:1px solid var(--nav-border)}.theme-toggle button.active{background:var(--mint);color:var(--on-light)}.theme-toggle button:hover:not(.active){background:var(--on-ring)}.wrap{padding:1.4rem 1rem 3rem}.kicker,.label{color:var(--teal);font:700 .72rem Consolas,monospace;letter-spacing:.14em;text-transform:uppercase}.intro{display:flex;align-items:flex-end;justify-content:space-between;gap:1.5rem;margin:.4rem 0 1.25rem}.intro h1{margin:.45rem 0 0;font-size:clamp(2rem,6vw,3.4rem);line-height:.92;letter-spacing:-.06em}.intro p{max-width:30rem;margin:0;color:var(--muted);line-height:1.55}.card{position:relative;margin-bottom:1rem;padding:1.05rem;background:var(--surface);border:1px solid var(--ink);box-shadow:5px 5px 0 var(--line)}.card:before{content:'';position:absolute;top:0;left:0;width:42px;height:4px;background:var(--teal)}.card h2{margin:0 0 1rem;padding-bottom:.65rem;border-bottom:1px solid var(--line);font-size:1.05rem}.card h2 span{margin-right:.55rem;color:var(--coral);font:700 .7rem Consolas,monospace}.field{margin-bottom:.85rem}.field:last-child{margin-bottom:0}label{display:block;margin-bottom:.3rem;color:var(--muted);font:700 .72rem Consolas,monospace;letter-spacing:.03em}input,select,button{font:700 .88rem Consolas,monospace}input,select{width:100%;padding:.64rem .68rem;border:1px solid var(--input-border);border-radius:0;background:var(--inset);color:var(--ink)}input:focus,select:focus{outline:2px solid var(--mint);outline-offset:1px;border-color:var(--teal)}input::placeholder{color:var(--muted);font-weight:400}.field-with-action{display:flex;gap:.5rem}.field-with-action select{flex:1;min-width:0}.field-with-action button{flex:none;white-space:nowrap}button{padding:.7rem .85rem;border:1px solid var(--teal-dark);border-radius:0;background:var(--teal);color:var(--on-dark);cursor:pointer}button:hover{background:var(--teal-dark)}button.ghost{background:transparent;border-color:var(--teal);color:var(--teal)}button.ghost:hover{background:var(--teal);color:var(--on-dark)}button:disabled{opacity:.55;cursor:wait}.message{display:none;margin:0 0 1rem;padding:.8rem 1rem;border:1px solid var(--teal);border-left:5px solid var(--teal);background:var(--surface);font:700 .8rem Consolas,monospace}.message.error{display:block;border-color:var(--coral);color:var(--coral)}.message.success{display:block;color:var(--teal-dark)}.info{margin:.85rem 0 0;padding:.65rem .75rem;border-left:3px solid var(--teal);background:var(--surface-2);color:var(--muted);font:400 .76rem Consolas,monospace;line-height:1.5}.status-banner{margin:1.4rem 0 1rem;padding:.8rem 1rem;background:var(--ink-soft);color:var(--on-dark);font:700 .76rem Consolas,monospace}.site-footer{margin-top:2rem;padding:1.2rem 1rem;background:var(--ink-soft);color:var(--dark-block-muted);font:400 .7rem Consolas,monospace}.site-footer>div{display:flex;justify-content:space-between;gap:1rem;flex-wrap:wrap}.site-footer .label{color:var(--mint)}.login-card{max-width:22rem;margin:2rem auto}.login-card h1{margin:0 0 1rem;font-size:1.8rem;line-height:1}.login-tools{display:flex;justify-content:center;margin-top:1rem}.login-tools .theme-toggle{border-color:var(--line)}.login-tools button{color:var(--muted)}.login-tools button.active{color:var(--on-light)}button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid var(--mint);outline-offset:2px}@media(max-width:600px){.topbar-in,.intro{align-items:flex-start;flex-direction:column}.field-with-action{flex-direction:column}.field-with-action button{width:100%}}
</style>
</head>
<body>
<header class="topbar"><div class="topbar-in"><div class="brand"><span class="brand-mark">HC</span><span class="brand-copy">HYDRACUP<span class="brand-sub">LOCAL DEVICE / NETWORK SETUP</span></span></div><div class="theme-toggle" role="group" aria-label="色彩主題"><button type="button" data-theme-choice="light" aria-pressed="false">紙</button><button type="button" data-theme-choice="dark" aria-pressed="false">夜</button></div></div></header>
<main class="wrap">
  <div class="intro"><div><div class="kicker">00 / provisioning</div><h1>網路設定</h1></div><p>HydraCup 目前以設定 AP 提供本頁。首次設定可直接輸入 WiFi；已設定管理密碼的裝置需要先登入。</p></div>
  <section class="card login-card" id="login-card" style="display:none"><div class="kicker">01 / access</div><h1>登入</h1><p class="info">這是已設定裝置的 recovery AP，請輸入既有管理密碼。</p><div class="message" id="login-msg" role="alert" aria-live="polite"></div><form id="login-form"><div class="field"><label for="login-password">管理密碼</label><input id="login-password" type="password" autocomplete="current-password" required></div><button type="submit" id="login-btn">登入</button></form></section>
  <div id="portal" style="display:none">
    <div class="status-banner">AP MODE · <span id="ap-ip">192.168.4.1</span><button id="logout-btn" type="button" style="display:none;float:right;padding:.35rem .55rem;font-size:.72rem">登出</button></div>
    <div class="message" id="msg" role="status" aria-live="polite"></div>
    <section class="card"><h2><span>01</span>選擇 WiFi</h2><p class="info">掃描清單只顯示附近可見網路；也可以直接輸入隱藏 SSID。</p><div class="field"><label for="nets">附近 WiFi</label><div class="field-with-action"><select id="nets"><option value="">尚未掃描</option></select><button class="ghost" id="scan-btn" type="button">掃描</button></div></div><div class="field"><label for="ssid">WiFi SSID</label><input id="ssid" type="text" autocomplete="off" placeholder="請輸入或選擇 WiFi SSID"></div></section>
    <section class="card"><h2><span>02</span>儲存設定</h2><div class="field"><label for="pass">WiFi 密碼</label><input id="pass" type="password" autocomplete="current-password" placeholder="WiFi 密碼"></div><button id="save-btn" type="button">儲存並重新啟動</button><p class="info">儲存後裝置會重新啟動並嘗試連線；成功後請改用 OLED 顯示的 IP 開啟 WebUI。</p></section>
  </div>
</main>
<footer class="site-footer"><div><span><span class="label">SOURCE</span> HydraCup</span><span>AP setup · no external assets</span></div></footer>
<script>
var csrf='',configured=false,authenticated=false,scanning=false;
function syncTheme(){var current=document.documentElement.dataset.theme;document.querySelectorAll('[data-theme-choice]').forEach(function(b){var active=b.getAttribute('data-theme-choice')===current;b.classList.toggle('active',active);b.setAttribute('aria-pressed',active?'true':'false');});}
function setTheme(theme){if(theme!=='light'&&theme!=='dark')return;document.documentElement.dataset.theme=theme;try{localStorage.setItem('iot-ui-theme',theme)}catch(e){}syncTheme();}
document.querySelectorAll('[data-theme-choice]').forEach(function(b){b.addEventListener('click',function(){setTheme(b.getAttribute('data-theme-choice'));});});syncTheme();
function setMessage(id,text,ok){var el=document.getElementById(id);el.textContent=text;el.className='message '+(ok?'success':'error');el.style.display='block';}
function setView(){var allowed=!configured||authenticated;document.getElementById('login-card').style.display=allowed?'none':'';document.getElementById('portal').style.display=allowed?'':'none';document.getElementById('logout-btn').style.display=configured&&authenticated?'':'none';}
function refreshAuth(){return fetch('/api/auth/csrf',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('auth');return r.json();}).then(function(d){csrf=d.csrf||'';configured=!!d.configured;authenticated=!!d.authenticated;setView();if(!configured||authenticated){loadStatus();return scanWifi(false);}document.getElementById('login-password').focus();});}
function apiFetch(url,options){options=options||{};var headers=new Headers(options.headers||{}),method=(options.method||'GET').toUpperCase();if(method!=='GET'&&method!=='HEAD'&&csrf)headers.set('X-CSRF-Token',csrf);options.headers=headers;return fetch(url,options).then(function(r){if(r.status===401){authenticated=false;setView();}return r;});}
function loadStatus(){apiFetch('/api/status',{cache:'no-store'}).then(function(r){return r.ok?r.json():null;}).then(function(d){if(d&&d.ap_ip)document.getElementById('ap-ip').textContent=d.ap_ip;}).catch(function(){});}
function scanWifi(showError){if(scanning)return Promise.resolve();var btn=document.getElementById('scan-btn'),sel=document.getElementById('nets'),current=document.getElementById('ssid').value;scanning=true;btn.disabled=true;btn.textContent='掃描中…';return apiFetch('/api/wifi/scan',{cache:'no-store'}).then(function(r){return r.json().then(function(j){return {ok:r.ok&&j.ok,data:j};});}).then(function(result){if(!result.ok){if(showError)setMessage('msg','掃描失敗：'+(result.data.error||'無法取得附近網路。'),false);return;}var nets=(result.data.networks||[]).slice().sort(function(a,b){return b.rssi-a.rssi;});sel.textContent='';var first=document.createElement('option');first.value='';first.textContent=nets.length?'附近網路（'+nets.length+' 個）':'找不到可見網路，請手動輸入';sel.appendChild(first);nets.forEach(function(n){var o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+(n.secure?' · 加密':' · 開放')+' · '+n.rssi+' dBm';sel.appendChild(o);});if(current)sel.value=current;}).catch(function(){if(showError)setMessage('msg','掃描失敗：暫時無法取得附近網路。',false);}).finally(function(){scanning=false;btn.disabled=false;btn.textContent='掃描';});}
document.getElementById('nets').addEventListener('change',function(){if(this.value)document.getElementById('ssid').value=this.value;});document.getElementById('scan-btn').addEventListener('click',function(){scanWifi(true);});
document.getElementById('login-form').addEventListener('submit',function(e){e.preventDefault();var btn=document.getElementById('login-btn'),password=document.getElementById('login-password').value;if(!password||!csrf){setMessage('login-msg',!password?'請輸入管理密碼。':'安全驗證尚未準備好，請稍後再試。',false);return;}btn.disabled=true;btn.textContent='驗證中…';fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':csrf},body:JSON.stringify({password:password})}).then(function(r){return r.json().then(function(j){return {ok:r.ok&&j.ok,data:j};});}).then(function(result){if(!result.ok){setMessage('login-msg',result.data.error==='rate_limited'?'嘗試次數過多，請稍後再試。':'登入失敗，請確認管理密碼。',false);btn.disabled=false;btn.textContent='登入';return;}document.getElementById('login-password').value='';refreshAuth();}).catch(function(){setMessage('login-msg','暫時無法連線，請稍後再試。',false);btn.disabled=false;btn.textContent='登入';});});
document.getElementById('logout-btn').addEventListener('click',function(){apiFetch('/api/auth/logout',{method:'POST'}).then(function(){return refreshAuth();}).catch(function(){refreshAuth();});});
document.getElementById('save-btn').addEventListener('click',function(){var ssid=document.getElementById('ssid').value.trim(),pass=document.getElementById('pass').value,btn=document.getElementById('save-btn');if(!ssid){setMessage('msg','請輸入或選擇 WiFi SSID。',false);return;}if(!csrf){setMessage('msg','安全驗證尚未準備好，請稍後再試。',false);return;}btn.disabled=true;apiFetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({wifi_ssid:ssid,wifi_password:pass})}).then(function(r){return r.json().then(function(j){return {ok:r.ok&&j.ok,data:j};});}).then(function(result){if(result.ok){setMessage('msg','儲存成功，裝置重新啟動中…',true);}else{setMessage('msg','錯誤：'+(result.data.error||'無法儲存'),false);btn.disabled=false;}}).catch(function(){setMessage('msg','暫時無法連線，請稍後再試。',false);btn.disabled=false;});});
refreshAuth().catch(function(){setMessage('login-msg','暫時無法連線，請重新整理頁面。',false);});
</script>
</body>
</html>
)rawhtml";

void ConfigPortal::begin(ConfigManager& cfgMgr, AppState& state, AppConfig& cfg) {
    if (_server != nullptr) {
        Serial.println("[ConfigPortal] Already initialized");
        return;
    }
    _cfgMgr = &cfgMgr;
    _state  = &state;
    _cfg    = &cfg;
    _csrfToken = randomToken();
    _server = new WebServer(80);
    const char* headerKeys[] = {"Cookie", "X-CSRF-Token"};
    _server->collectHeaders(headerKeys, 2);

    _server->on("/",                HTTP_GET,  [this]{ _handleRoot();       });
    _server->on("/api/auth/csrf",  HTTP_GET,  [this]{ _handleAuthCsrf();    });
    _server->on("/api/auth/login", HTTP_POST, [this]{ _handleAuthLogin();   });
    _server->on("/api/auth/logout",HTTP_POST, [this]{ _handleAuthLogout();  });
    _server->on("/api/status",     HTTP_GET,  [this]{ _handleStatus();      });
    _server->on("/api/config",     HTTP_POST, [this]{ _handleConfig();      });
    _server->on("/api/wifi/scan",  HTTP_GET,  [this]{ _handleWifiScan();    });
    _server->on("/api/reboot",    HTTP_POST, [this]{ _handleReboot();       });

    _server->begin();
    Serial.println("[ConfigPortal] HTTP server started on port 80");
}

void ConfigPortal::loop() {
    if (_server) _server->handleClient();
}

bool ConfigPortal::_isSessionValid() {
    if (_sessionToken.isEmpty() || !_server->hasHeader("Cookie")) return false;
    const String cookie = cookieValue(_server->header("Cookie"), "session");
    if (!constantTimeEqual(cookie, _sessionToken)) return false;

    const uint32_t now = millis();
    if (now - _sessionStartMs > SESSION_ABSOLUTE_TIMEOUT_MS ||
        now - _lastActivityMs > SESSION_IDLE_TIMEOUT_MS) {
        _clearSession();
        return false;
    }
    _lastActivityMs = now;
    return true;
}

bool ConfigPortal::_hasValidCsrf() const {
    if (!_server->hasHeader("X-CSRF-Token")) return false;
    const String& expected = _sessionToken.isEmpty() ? _csrfToken : _sessionCsrfToken;
    return constantTimeEqual(_server->header("X-CSRF-Token"), expected);
}

void ConfigPortal::_clearSession() {
    _sessionToken = String();
    _sessionCsrfToken = String();
    _sessionStartMs = 0;
    _lastActivityMs = 0;
}

void ConfigPortal::_establishSession() {
    _sessionToken = randomToken();
    _sessionCsrfToken = randomToken();
    _sessionStartMs = millis();
    _lastActivityMs = _sessionStartMs;
}

bool ConfigPortal::_requireApiAuth(bool requireCsrf) {
    const bool configured = _cfg && !_cfg->adminPasswordHash.isEmpty();
    if (configured && !_isSessionValid()) {
        _server->sendHeader("Cache-Control", "no-store");
        _server->send(401, "application/json", "{\"ok\":false,\"error\":\"authentication_required\"}");
        return false;
    }
    if (requireCsrf && !_hasValidCsrf()) {
        _server->sendHeader("Cache-Control", "no-store");
        _server->send(403, "application/json", "{\"ok\":false,\"error\":\"csrf_failed\"}");
        return false;
    }
    _server->sendHeader("Cache-Control", "no-store");
    return true;
}

bool ConfigPortal::_isRateLimited(const String& ip) {
    const uint32_t now = millis();
    for (AuthFailureBucket& bucket : _authFailures) {
        if (bucket.ip != ip) continue;
        uint8_t kept = 0;
        for (uint8_t i = 0; i < bucket.count; ++i) {
            if (now - bucket.timestamps[i] <= AUTH_FAILURE_WINDOW_MS)
                bucket.timestamps[kept++] = bucket.timestamps[i];
        }
        bucket.count = kept;
        bucket.lastSeen = now;
        return bucket.count >= AUTH_FAILURE_LIMIT;
    }
    return false;
}

void ConfigPortal::_recordAuthFailure(const String& ip) {
    const uint32_t now = millis();
    AuthFailureBucket* bucket = nullptr;
    for (AuthFailureBucket& candidate : _authFailures) {
        if (candidate.ip == ip) { bucket = &candidate; break; }
    }
    if (!bucket) {
        for (AuthFailureBucket& candidate : _authFailures) {
            if (candidate.ip.isEmpty()) { bucket = &candidate; break; }
        }
    }
    if (!bucket) {
        bucket = &_authFailures[0];
        for (AuthFailureBucket& candidate : _authFailures)
            if (candidate.lastSeen < bucket->lastSeen) bucket = &candidate;
        bucket->ip = String();
        bucket->count = 0;
    }
    bucket->ip = ip;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < bucket->count; ++i) {
        if (now - bucket->timestamps[i] <= AUTH_FAILURE_WINDOW_MS)
            bucket->timestamps[kept++] = bucket->timestamps[i];
    }
    bucket->count = kept;
    if (bucket->count < AUTH_FAILURE_LIMIT)
        bucket->timestamps[bucket->count++] = now;
    bucket->lastSeen = now;
}

void ConfigPortal::_sendAuthFailure(int statusCode, const char* error) {
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(statusCode, "application/json",
                  String("{\"ok\":false,\"error\":\"") + error + "\"}");
}

void ConfigPortal::_handleRoot() {
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "text/html", _setupPageHtml);
}

void ConfigPortal::_handleAuthCsrf() {
    const bool authenticated = _isSessionValid();
    JsonDocument doc;
    doc["ok"] = true;
    doc["configured"] = _cfg && !_cfg->adminPasswordHash.isEmpty();
    doc["authenticated"] = authenticated;
    doc["csrf"] = authenticated ? _sessionCsrfToken : _csrfToken;
    String json;
    serializeJson(doc, json);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", json);
}

void ConfigPortal::_handleAuthLogin() {
    const String ip = _server->client().remoteIP().toString();
    if (_isRateLimited(ip)) {
        _sendAuthFailure(429, "rate_limited");
        return;
    }
    if (!_server->hasHeader("X-CSRF-Token") ||
        !constantTimeEqual(_server->header("X-CSRF-Token"), _csrfToken)) {
        _sendAuthFailure(403, "csrf_failed");
        return;
    }
    if (!_cfg || _cfg->adminPasswordHash.isEmpty()) {
        _sendAuthFailure(409, "setup_required");
        return;
    }
    if (!_server->hasArg("plain")) {
        _recordAuthFailure(ip);
        _sendAuthFailure(400, "invalid_request");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, _server->arg("plain"))) {
        _recordAuthFailure(ip);
        _sendAuthFailure(400, "invalid_request");
        return;
    }
    const String password = doc["password"] | "";
    if (password.isEmpty() || password.length() > 128 ||
        !verifyPasswordHash(password, _cfg->adminPasswordHash)) {
        _recordAuthFailure(ip);
        _sendAuthFailure(401, "invalid_credentials");
        return;
    }

    _establishSession();
    _server->sendHeader("Set-Cookie",
                        String("session=") + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict",
                        true);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", "{\"ok\":true,\"authenticated\":true}");
}

void ConfigPortal::_handleAuthLogout() {
    const bool configured = _cfg && !_cfg->adminPasswordHash.isEmpty();
    if (configured && !_requireApiAuth(true)) return;
    _clearSession();
    _server->sendHeader("Set-Cookie", "session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict", false);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(200, "application/json", "{\"ok\":true}");
}

void ConfigPortal::_handleStatus() {
    if (!_requireApiAuth(false)) return;
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
    if (!_requireApiAuth(true)) return;
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
    if (!_requireApiAuth(false)) return;
    if (millis() - _lastScanMs < SCAN_COOLDOWN_MS) {
        _server->send(429, "application/json", "{\"ok\":false,\"error\":\"scan_busy\"}");
        return;
    }
    _lastScanMs = millis();

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
        const int rssi = WiFi.RSSI(i);
        const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        if (ssid.isEmpty()) continue;
        bool found = false;
        for (JsonObject existing : arr) {
            if (existing["ssid"].as<String>() == ssid) {
                if (rssi > existing["rssi"].as<int>()) {
                    existing["rssi"] = rssi;
                    existing["secure"] = secure;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            JsonObject obj = arr.add<JsonObject>();
            obj["ssid"] = ssid;
            obj["rssi"] = rssi;
            obj["secure"] = secure;
        }
    }
    WiFi.scanDelete();
    String json;
    serializeJson(doc, json);
    _server->send(200, "application/json", json);
}

void ConfigPortal::_handleReboot() {
    if (!_requireApiAuth(true)) return;
    _server->send(200, "application/json", "{\"ok\":true}");
    delay(500);
    ESP.restart();
}
