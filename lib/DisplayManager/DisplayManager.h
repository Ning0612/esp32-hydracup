#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

class DisplayManager {
public:
    bool init();

    // Boot / error screens
    void showBootScreen();
    void showError(const char* msg);

    // Mode screens
    void showWifiConnecting(const String& ssid);
    void showAPMode(const String& apSsid, const String& apPassword, const String& ip);

    void showNormalMode(float weightG, bool stable,
                        float todayMl, uint32_t goalMl, uint32_t drinkCount,
                        float lastDrinkMl, uint32_t nextRemSec,
                        bool wifiOk, const String& ip, bool ntpSynced);

    void update();

    bool isAvailable() const { return _available; }

private:
    static constexpr uint8_t  PAGE_COUNT       = 2;
    static constexpr uint32_t PAGE_INTERVAL_MS = 4000;

    Adafruit_SSD1306 _display{OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET_PIN};
    bool     _available    = false;
    uint32_t _lastUpdateMs = 0;
    uint8_t  _page         = 0;
    uint32_t _pageChangedMs = 0;

    void _centerPrint(const char* text, int16_t y);
    void _drawPage0Weight(float weightG, const String& ip);
    void _drawPage1Hydration(float todayMl, uint32_t goalMl, uint32_t drinkCount,
                             float lastDrinkMl, uint32_t nextRemSec);
};
