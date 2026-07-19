#pragma once

#include <cstdint>
#include <string>

#include "esp_event.h"

struct WifiNetwork {
    char ssid[33] = {};
    int rssi = 0;
    bool secure = false;
};

class WiFiManager {
public:
    bool begin();
    bool connectSTA(const std::string& ssid, const std::string& password,
                   uint32_t timeoutMs = 10000);
    bool startAP(const std::string& ssid, const std::string& password);
    void loop();
    bool isConnected() const { return _connected; }
    std::string getIP() const;
    std::string getAPIP() const;
    std::string getAPSSID() const { return _apSsid; }
    int scan(WifiNetwork* networks, int capacity);

private:
    static void _eventHandler(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData);
    void _onEvent(esp_event_base_t eventBase, int32_t eventId, void* eventData);
    std::string _ipFrom( bool ap) const;

    bool _initialized = false;
    bool _connected = false;
    bool _apMode = false;
    std::string _apSsid;
    void* _staNetif = nullptr;
    void* _apNetif = nullptr;
};
