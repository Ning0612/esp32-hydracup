#include "ScaleManager.h"

#include <cmath>

#include "config.h"
#include "hal_delay.h"
#include "hal_log.h"
#include "hal_time.h"
#include "pins.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr const char* TAG = "Scale";
portMUX_TYPE hx711Mux = portMUX_INITIALIZER_UNLOCKED;
}

void ScaleManager::init(float calibFactor, long tareOffset,
                        float stableToleranceG, uint32_t stableDurationMs) {
    _calibFactor = calibFactor;
    _tareOffset = tareOffset;
    _stableToleranceG = stableToleranceG;
    _stableDurationMs = stableDurationMs;
    gpio_config_t dout = {};
    dout.pin_bit_mask = 1ULL << PIN_HX711_DOUT;
    dout.mode = GPIO_MODE_INPUT;
    dout.pull_up_en = GPIO_PULLUP_DISABLE;
    dout.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dout.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&dout);
    gpio_config_t sck = {};
    sck.pin_bit_mask = 1ULL << PIN_HX711_SCK;
    sck.mode = GPIO_MODE_OUTPUT;
    sck.pull_up_en = GPIO_PULLUP_DISABLE;
    sck.pull_down_en = GPIO_PULLDOWN_DISABLE;
    sck.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&sck);
    gpio_set_level(static_cast<gpio_num_t>(PIN_HX711_SCK), 0);

    const uint32_t start = hal_millis();
    while (!_readReady()) {
        if (hal_millis() - start > HX711_INIT_TIMEOUT_MS) return;
        hal_delay_ms(10);
    }
    _ready = true;
    _stableStartMs = hal_millis();
    LOG_INFO(TAG, "HX711 ready");
}

bool ScaleManager::_readReady() const {
    return gpio_get_level(static_cast<gpio_num_t>(PIN_HX711_DOUT)) == 0;
}

long ScaleManager::_readRaw() const {
    uint32_t value = 0;
    portENTER_CRITICAL(&hx711Mux);
    for (uint8_t bit = 0; bit < 24; ++bit) {
        gpio_set_level(static_cast<gpio_num_t>(PIN_HX711_SCK), 1);
        esp_rom_delay_us(1);
        value = (value << 1) | static_cast<uint32_t>(gpio_get_level(static_cast<gpio_num_t>(PIN_HX711_DOUT)));
        gpio_set_level(static_cast<gpio_num_t>(PIN_HX711_SCK), 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_HX711_SCK), 1);
    esp_rom_delay_us(1);
    gpio_set_level(static_cast<gpio_num_t>(PIN_HX711_SCK), 0);
    portEXIT_CRITICAL(&hx711Mux);
    if (value & 0x800000U) value |= 0xFF000000U;
    return static_cast<long>(static_cast<int32_t>(value));
}

void ScaleManager::update() {
    if (!_ready) return;
    const uint32_t now = hal_millis();
    if (isTareRunning() && now - _tareStartMs >= 2000) {
        _tareRunning = false;
        _tareWarming = false;
        _tareFailed = true;
        LOG_WARN(TAG, "tare timed out");
        return;
    }
    if (now - _lastReadMs < HX711_READ_INTERVAL_MS || !_readReady()) return;
    _lastReadMs = now;
    const long raw = _readRaw();
    if (_tareRunning) {
        _tareSum += raw;
        _tareCount++;
        if (_tareCount >= SAMPLE_COUNT) _finishTare();
        return;
    }
    _updateAverage(raw);
    _recalcWeight();
    _updateStability();
    if (_tareWarming && _ringFilled >= SAMPLE_COUNT) {
        _tareWarming = false;
        _tareCompleted = true;
        LOG_INFO(TAG, "tare warm-up complete");
    }
}

void ScaleManager::tare() { startTare(); }

bool ScaleManager::startTare() {
    if (!_ready || isTareRunning()) return false;
    _tareRunning = true;
    _tareWarming = false;
    _tareCompleted = false;
    _tareFailed = false;
    _tareSum = 0;
    _tareCount = 0;
    _tareStartMs = hal_millis();
    _lastReadMs = 0;
    return true;
}

bool ScaleManager::takeTareFailure() {
    if (!_tareFailed) return false;
    _tareFailed = false;
    return true;
}

bool ScaleManager::takeTareResult(long& offset) {
    if (!_tareCompleted) return false;
    offset = _tareOffset;
    _tareCompleted = false;
    return true;
}

void ScaleManager::_finishTare() {
    if (_tareCount == 0) { _tareRunning = false; return; }
    _tareOffset = static_cast<long>(_tareSum / _tareCount);
    _ringFilled = 0;
    _ringIdx = 0;
    _rawAvg = 0;
    _weightGrams = 0.0f;
    _stable = false;
    _stableStartMs = hal_millis();
    _tareRunning = false;
    _tareWarming = true;
    _tareStartMs = hal_millis();
    LOG_INFO(TAG, "tare offset=%ld", _tareOffset);
}

void ScaleManager::setCalibrationFactor(float factor) {
    _calibFactor = factor;
    _recalcWeight();
}

float ScaleManager::calibrateWithKnownWeight(float knownGrams) {
    if (knownGrams <= 0.0f || _rawAvg == 0) return _calibFactor;
    const long netRaw = _rawAvg - _tareOffset;
    if (netRaw == 0) return _calibFactor;
    _calibFactor = static_cast<float>(netRaw) / knownGrams;
    _recalcWeight();
    LOG_INFO(TAG, "calibration factor=%.4f weight=%.1f", _calibFactor, _weightGrams);
    return _calibFactor;
}

void ScaleManager::_updateAverage(long raw) {
    _ringBuf[_ringIdx] = raw;
    _ringIdx = (_ringIdx + 1) % SAMPLE_COUNT;
    if (_ringFilled < SAMPLE_COUNT) _ringFilled++;
    int64_t sum = 0;
    for (uint8_t i = 0; i < _ringFilled; ++i) sum += _ringBuf[i];
    _rawAvg = static_cast<long>(sum / _ringFilled);
}

void ScaleManager::_recalcWeight() {
    _weightGrams = _calibFactor == 0.0f ? 0.0f :
        static_cast<float>(_rawAvg - _tareOffset) / _calibFactor;
}

void ScaleManager::_updateStability() {
    const uint32_t now = hal_millis();
    if (std::fabs(_weightGrams - _stableRefGrams) < _stableToleranceG) {
        if (!_stable && now - _stableStartMs >= _stableDurationMs) {
            _stable = true;
            _stableWeight = _weightGrams;
        }
    } else {
        _stableRefGrams = _weightGrams;
        _stableStartMs = now;
        _stable = false;
    }
}
