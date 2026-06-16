#include "WiFiManager.h"

bool WiFiManager::connectSTA(const String& ssid, const String& password, uint32_t timeoutMs) {
    if (ssid.isEmpty()) return false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            WiFi.disconnect(true);
            return false;
        }
        delay(200);
    }
    _apMode = false;
    Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

bool WiFiManager::startAP(const String& ssid, const String& password) {
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(ssid.c_str(), password.c_str());
    if (ok) {
        _apMode = true;
        Serial.printf("[WiFi] AP started: %s  IP: %s\n",
                      ssid.c_str(), WiFi.softAPIP().toString().c_str());
    }
    return ok;
}

void WiFiManager::loop() {
    if (_apMode) return;
    // STA reconnect handled by ESP32 SDK automatically;
    // future phases can add explicit reconnect logic here
}

bool WiFiManager::isConnected() const {
    return !_apMode && (WiFi.status() == WL_CONNECTED);
}

String WiFiManager::getIP() const {
    return WiFi.localIP().toString();
}

String WiFiManager::getAPIP() const {
    return WiFi.softAPIP().toString();
}
