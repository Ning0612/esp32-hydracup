#pragma once
#include <Arduino.h>
#include <WiFi.h>

class WiFiManager {
public:
    bool connectSTA(const String& ssid, const String& password, uint32_t timeoutMs = 10000);
    bool startAP(const String& ssid, const String& password);
    void loop();

    bool    isConnected() const;
    String  getIP()       const;
    String  getAPIP()     const;

private:
    bool _apMode = false;
};
