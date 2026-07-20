#pragma once

#include <cstdint>
#include <string>

#include "driver/i2c_types.h"

class DisplayManager {
public:
    bool init();
    void showBootScreen();
    void showError(const char* msg);
    void showWifiConnecting(const std::string& ssid);
    void showAPMode(const std::string& apSsid, const std::string& apPassword,
                    const std::string& ip);
    void showNormalMode(float weightG, bool stable, float todayMl, uint32_t goalMl,
                        uint32_t drinkCount, float lastDrinkMl, uint32_t nextRemSec,
                        bool wifiOk, const std::string& ip, bool ntpSynced);
    void wake();
    void sleep();
    void update();
    bool isAvailable() const { return _available; }

private:
    static constexpr uint8_t PAGE_COUNT = 2;
    static constexpr uint32_t PAGE_INTERVAL_MS = 4000;
    static constexpr uint32_t SCREEN_ON_DURATION_MS = 60000;

    bool _available = false;
    uint32_t _lastUpdateMs = 0;
    uint8_t _page = 0;
    uint32_t _pageChangedMs = 0;
    bool _screenOn = false;
    uint32_t _wakeMs = 0;
    uint8_t _buffer[128 * 32 / 8] = {};
    i2c_master_bus_handle_t _i2cBus = nullptr;
    i2c_master_dev_handle_t _i2cDevice = nullptr;

    void _deinitI2c();
    bool _command(uint8_t command);
    bool _data(const uint8_t* bytes, size_t length);
    void _clear();
    void _flush();
    void _drawText(int x, int y, const char* text, uint8_t scale);
    void _centerText(const char* text, int y, uint8_t scale);
    void _drawPage0Weight(float weightG, const std::string& ip);
    void _drawPage1Hydration(float todayMl, uint32_t goalMl, uint32_t drinkCount,
                             float lastDrinkMl, uint32_t nextRemSec);
};
