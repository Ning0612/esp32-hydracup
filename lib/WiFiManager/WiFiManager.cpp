#include "WiFiManager.h"

#include <algorithm>
#include <cstring>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "hal_delay.h"
#include "hal_log.h"
#include "hal_time.h"

namespace { constexpr const char* TAG = "WiFi"; }

bool WiFiManager::begin() {
    if (_initialized) return true;
    esp_netif_init();
    const esp_err_t loopResult = esp_event_loop_create_default();
    if (loopResult != ESP_OK && loopResult != ESP_ERR_INVALID_STATE) return false;
    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&initConfig) != ESP_OK) return false;
    _staNetif = esp_netif_create_default_wifi_sta();
    _apNetif = esp_netif_create_default_wifi_ap();
    if (!_staNetif || !_apNetif) return false;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_eventHandler, this) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_eventHandler, this) != ESP_OK ||
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &_eventHandler, this) != ESP_OK) {
        return false;
    }
    _initialized = true;
    return true;
}

bool WiFiManager::connectSTA(const std::string& ssid, const std::string& password,
                             uint32_t timeoutMs) {
    if (ssid.empty() || !begin()) return false;
    _apMode = false;
    _connected = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid.c_str(), sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password.c_str(), sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK ||
        esp_wifi_start() != ESP_OK || esp_wifi_connect() != ESP_OK) return false;

    const uint32_t start = hal_millis();
    while (!_connected && hal_millis() - start <= timeoutMs) hal_delay_ms(200);
    if (!_connected) {
        esp_wifi_disconnect();
        LOG_WARN(TAG, "STA connection timed out");
        return false;
    }
    LOG_INFO(TAG, "connected: %s", getIP().c_str());
    return true;
}

bool WiFiManager::startAP(const std::string& ssid, const std::string& password) {
    if (!begin()) return false;
    _apMode = true;
    _apSsid = ssid;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid.c_str(), sizeof(config.ap.ssid));
    std::strncpy(reinterpret_cast<char*>(config.ap.password), password.c_str(), sizeof(config.ap.password));
    config.ap.ssid_len = static_cast<uint8_t>(std::min<size_t>(ssid.size(), sizeof(config.ap.ssid)));
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = password.size() >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (config.ap.authmode == WIFI_AUTH_OPEN) config.ap.password[0] = '\0';
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK || esp_wifi_start() != ESP_OK) return false;
    LOG_INFO(TAG, "AP started: %s IP=%s", _apSsid.c_str(), getAPIP().c_str());
    return true;
}

void WiFiManager::loop() {
    if (!_apMode && !_connected) esp_wifi_connect();
}

std::string WiFiManager::_ipFrom(bool ap) const {
    esp_netif_ip_info_t info = {};
    esp_netif_t* netif = static_cast<esp_netif_t*>(ap ? _apNetif : _staNetif);
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return "0.0.0.0";
    char value[16];
    std::snprintf(value, sizeof(value), IPSTR, IP2STR(&info.ip));
    return value;
}

std::string WiFiManager::getIP() const { return _ipFrom(false); }
std::string WiFiManager::getAPIP() const { return _ipFrom(true); }

int WiFiManager::scan(WifiNetwork* networks, int capacity) {
    if (!networks || capacity <= 0 || !_initialized) return -1;
    wifi_scan_config_t config = {};
    config.show_hidden = false;
    const esp_err_t result = esp_wifi_scan_start(&config, true);
    if (result != ESP_OK) return -1;
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t records[32] = {};
    uint16_t fetch = std::min<uint16_t>(count, 32);
    esp_wifi_scan_get_ap_records(&fetch, records);
    int used = 0;
    for (uint16_t i = 0; i < fetch; ++i) {
        if (records[i].ssid[0] == '\0') continue;
        int existing = -1;
        for (int j = 0; j < used; ++j) if (std::strcmp(networks[j].ssid, reinterpret_cast<char*>(records[i].ssid)) == 0) existing = j;
        const bool secure = records[i].authmode != WIFI_AUTH_OPEN;
        if (existing >= 0) {
            if (records[i].rssi > networks[existing].rssi) {
                networks[existing].rssi = records[i].rssi;
                networks[existing].secure = secure;
            }
        } else if (used < capacity) {
            std::strncpy(networks[used].ssid, reinterpret_cast<char*>(records[i].ssid), 32);
            networks[used].ssid[32] = '\0';
            networks[used].rssi = records[i].rssi;
            networks[used].secure = secure;
            ++used;
        }
    }
    return used;
}

void WiFiManager::_eventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    static_cast<WiFiManager*>(arg)->_onEvent(eventBase, eventId, eventData);
}

void WiFiManager::_onEvent(esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    if (eventBase == WIFI_EVENT) {
        if (eventId == WIFI_EVENT_STA_DISCONNECTED) _connected = false;
    } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
        _connected = true;
    } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_LOST_IP) {
        _connected = false;
    }
    (void)eventData;
}
