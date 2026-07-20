#include "DisplayManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "driver/i2c_master.h"
#include "hal_time.h"
#include "pins.h"

namespace {
constexpr i2c_port_num_t PORT = I2C_NUM_0;
constexpr uint8_t CONTROL_COMMAND = 0x00;
constexpr uint8_t CONTROL_DATA = 0x40;
constexpr int I2C_TIMEOUT_MS = 100;

// Compact 5x7 glyphs for the ASCII characters used on the device screen.
// The lowercase g uses the optional eighth row for its descender.
const uint8_t DIGITS[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x62,0x51,0x49,0x49,0x46},{0x22,0x49,0x49,0x49,0x36},
    {0x18,0x14,0x12,0x7f,0x10},{0x2f,0x49,0x49,0x49,0x31},
    {0x3e,0x49,0x49,0x49,0x32},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x26,0x49,0x49,0x49,0x3e}
};
const uint8_t LETTERS[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
    {0x7f,0x20,0x18,0x20,0x7f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};
const uint8_t LOWER_LETTERS[26][5] = {
    {0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},
    {0x18,0xa4,0xa4,0xa4,0x78},{0x7f,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00},{0x00,0x41,0x7f,0x40,0x00},
    {0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20},{0x04,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},
    {0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44}
};

const uint8_t* glyph(char input) {
    static const uint8_t space[5] = {0,0,0,0,0};
    static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
    static const uint8_t colon[5] = {0,0x36,0x36,0,0};
    static const uint8_t dot[5] = {0,0x60,0x60,0,0};
    static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const uint8_t percent[5] = {0x63,0x13,0x08,0x64,0x63};
    static const uint8_t plus[5] = {0x08,0x08,0x3e,0x08,0x08};
    static const uint8_t question[5] = {0x02,0x01,0x51,0x09,0x06};
    if (input >= 'a' && input <= 'z') return LOWER_LETTERS[input - 'a'];
    if (input >= '0' && input <= '9') return DIGITS[input - '0'];
    if (input >= 'A' && input <= 'Z') return LETTERS[input - 'A'];
    switch (input) {
        case ' ': return space; case '-': return dash; case ':': return colon;
        case '.': return dot; case '/': return slash; case '%': return percent;
        case '+': return plus; default: return question;
    }
}
}

void DisplayManager::_deinitI2c() {
    if (_i2cDevice) {
        i2c_master_bus_rm_device(_i2cDevice);
        _i2cDevice = nullptr;
    }
    if (_i2cBus) {
        i2c_del_master_bus(_i2cBus);
        _i2cBus = nullptr;
    }
}

bool DisplayManager::init() {
    if (_available) return true;
    i2c_master_bus_config_t busConfig = {};
    busConfig.i2c_port = PORT;
    busConfig.sda_io_num = static_cast<gpio_num_t>(PIN_OLED_SDA);
    busConfig.scl_io_num = static_cast<gpio_num_t>(PIN_OLED_SCL);
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = 1;
    if (i2c_new_master_bus(&busConfig, &_i2cBus) != ESP_OK) return false;

    i2c_device_config_t deviceConfig = {};
    deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    deviceConfig.device_address = OLED_I2C_ADDRESS;
    deviceConfig.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(_i2cBus, &deviceConfig, &_i2cDevice) != ESP_OK) {
        _deinitI2c();
        return false;
    }

    static const uint8_t setup[] = {0xae,0xd5,0x80,0xa8,0x1f,0xd3,0x00,0x40,0x8d,0x14,
        0x20,0x00,0xa1,0xc8,0xda,0x02,0x81,0x8f,0xd9,0xf1,0xdb,0x40,0xa4,0xa6,0xaf};
    for (uint8_t command : setup) {
        if (!_command(command)) {
            _deinitI2c();
            return false;
        }
    }
    _available = true;
    _clear();
    _flush();
    return true;
}

bool DisplayManager::_command(uint8_t command) {
    if (!_i2cDevice) return false;
    const uint8_t packet[2] = {CONTROL_COMMAND, command};
    return i2c_master_transmit(_i2cDevice, packet, sizeof(packet), I2C_TIMEOUT_MS) == ESP_OK;
}

bool DisplayManager::_data(const uint8_t* bytes, size_t length) {
    if (!_i2cDevice) return false;
    uint8_t packet[33];
    while (length > 0) {
        const size_t chunk = std::min<size_t>(length, 32);
        packet[0] = CONTROL_DATA;
        std::memcpy(packet + 1, bytes, chunk);
        if (i2c_master_transmit(_i2cDevice, packet, chunk + 1, I2C_TIMEOUT_MS) != ESP_OK)
            return false;
        bytes += chunk; length -= chunk;
    }
    return true;
}

void DisplayManager::_clear() { std::memset(_buffer, 0, sizeof(_buffer)); }

void DisplayManager::_flush() {
    for (uint8_t page = 0; page < 4; ++page) {
        _command(static_cast<uint8_t>(0xb0 + page));
        _command(0x00); _command(0x10);
        _data(_buffer + page * 128, 128);
    }
}

void DisplayManager::_drawText(int x, int y, const char* text, uint8_t scale) {
    if (!text || scale == 0) return;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        const uint8_t* data = glyph(text[index]);
        const int origin = x + static_cast<int>(index) * 6 * scale;
        for (uint8_t column = 0; column < 5; ++column) {
            for (uint8_t row = 0; row < 8; ++row) {
                if ((data[column] & (1U << row)) == 0) continue;
                for (uint8_t dx = 0; dx < scale; ++dx) for (uint8_t dy = 0; dy < scale; ++dy) {
                    const int px = origin + column * scale + dx;
                    const int py = y + row * scale + dy;
                    if (px >= 0 && px < 128 && py >= 0 && py < 32)
                        _buffer[px + (py / 8) * 128] |= static_cast<uint8_t>(1U << (py % 8));
                }
            }
        }
    }
}

void DisplayManager::_centerText(const char* text, int y, uint8_t scale) {
    const int width = static_cast<int>(std::strlen(text)) * 6 * scale;
    _drawText(std::max(0, (128 - width) / 2), y, text, scale);
}

void DisplayManager::showBootScreen() {
    if (!_available) return;
    _screenOn = true; _wakeMs = hal_millis(); _clear();
    _centerText("WATER TRACKER", 4, 1); _centerText("BOOTING", 18, 1); _flush();
}

void DisplayManager::showError(const char* msg) {
    if (!_available) return;
    _screenOn = true; _wakeMs = hal_millis(); _clear();
    _drawText(0, 4, "ERROR", 1); char value[22] = {}; std::strncpy(value, msg, 21);
    _drawText(0, 18, value, 1); _flush();
}

void DisplayManager::showWifiConnecting(const std::string& ssid) {
    if (!_available) return;
    _screenOn = true; _wakeMs = hal_millis(); _clear();
    _drawText(0, 4, "CONNECTING WIFI", 1); std::string shown = ssid.substr(0, 21);
    _drawText(0, 18, shown.c_str(), 1); _flush();
}

void DisplayManager::showAPMode(const std::string& apSsid, const std::string& apPassword,
                                const std::string& ip) {
    if (!_available) return;
    _screenOn = true; _wakeMs = hal_millis(); _clear();
    _drawText(0, 0, apSsid.substr(0, 21).c_str(), 1);
    _drawText(0, 11, ("PW:" + apPassword.substr(0, 17)).c_str(), 1);
    _drawText(0, 22, ("OPEN: " + ip).c_str(), 1); _flush();
}

void DisplayManager::showNormalMode(float weightG, bool stable, float todayMl, uint32_t goalMl,
                                    uint32_t drinkCount, float lastDrinkMl, uint32_t nextRemSec,
                                    bool wifiOk, const std::string& ip, bool ntpSynced) {
    (void)stable; (void)wifiOk; (void)ntpSynced;
    if (!_available || hal_millis() - _lastUpdateMs < OLED_UPDATE_INTERVAL_MS) return;
    _lastUpdateMs = hal_millis();
    if (_pageChangedMs == 0) _pageChangedMs = hal_millis();
    else if (hal_millis() - _pageChangedMs >= PAGE_INTERVAL_MS) {
        _page = (_page + 1) % PAGE_COUNT; _pageChangedMs = hal_millis();
    }
    if (!_screenOn) return;
    _clear();
    if (_page == 0) _drawPage0Weight(weightG, ip);
    else _drawPage1Hydration(todayMl, goalMl, drinkCount, lastDrinkMl, nextRemSec);
    _flush();
}

void DisplayManager::wake() {
    if (!_available) return;
    _screenOn = true; _wakeMs = hal_millis(); _page = 0; _pageChangedMs = 0; _lastUpdateMs = 0;
    _command(0xaf);
}

void DisplayManager::sleep() { if (_available) { _command(0xae); _screenOn = false; } }

void DisplayManager::update() {
    if (!_available) return;
    const uint32_t now = hal_millis();
    if (_screenOn && now - _wakeMs >= SCREEN_ON_DURATION_MS) sleep();
}

void DisplayManager::_drawPage0Weight(float weightG, const std::string& ip) {
    if (!std::isfinite(weightG)) weightG = 0.0f;
    _centerText(ip.empty() ? "--" : ip.c_str(), 0, 1);
    char value[16]; std::snprintf(value, sizeof(value), "%.0f g", weightG);
    _centerText(value, 7, 3);
}

void DisplayManager::_drawPage1Hydration(float todayMl, uint32_t goalMl, uint32_t drinkCount,
                                         float lastDrinkMl, uint32_t nextRemSec) {
    if (!std::isfinite(todayMl) || todayMl < 0) todayMl = 0;
    char value[40]; std::snprintf(value, sizeof(value), "%.0f mL", todayMl); _centerText(value, 0, 2);
    const uint32_t pct = goalMl ? static_cast<uint32_t>(todayMl * 100.0f / goalMl) : 0;
    std::snprintf(value, sizeof(value), "%lu%% G:%lu mL D:%lu",
                  static_cast<unsigned long>(std::min<uint32_t>(pct, 100U)),
                  static_cast<unsigned long>(goalMl), static_cast<unsigned long>(drinkCount));
    _centerText(value, 16, 1);
    if (nextRemSec == 0) _centerText("DRINK NOW", 24, 1);
    else {
        std::snprintf(value, sizeof(value), "L:%.0f mL N:%lu s", lastDrinkMl,
                      static_cast<unsigned long>(nextRemSec));
        _centerText(value, 24, 1);
    }
}
