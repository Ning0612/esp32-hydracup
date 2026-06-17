#include "ScaleManager.h"
#include "ConfigManager.h"
#include "pins.h"
#include "config.h"

void ScaleManager::init(float calibFactor, long tareOffset,
                        float stableToleranceG, uint32_t stableDurationMs) {
    _calibFactor      = calibFactor;
    _tareOffset       = tareOffset;
    _stableToleranceG = stableToleranceG;
    _stableDurationMs = stableDurationMs;

    _scale.begin(PIN_HX711_DOUT, PIN_HX711_SCK);

    const uint32_t start = millis();
    while (!_scale.is_ready()) {
        if (millis() - start > HX711_INIT_TIMEOUT_MS) return;
        delay(10);
    }
    _ready = true;
    _scale.set_offset(_tareOffset);
    _stableStartMs = millis();
}

void ScaleManager::update() {
    if (!_ready) return;
    if (millis() - _lastReadMs < HX711_READ_INTERVAL_MS) return;
    _lastReadMs = millis();

    if (_scale.is_ready()) {
        _updateAverage(_scale.read());
        _recalcWeight();
        _updateStability();
    }
}

void ScaleManager::tare() {
    if (!_ready) return;

    long sum   = 0;
    uint8_t n  = 0;
    for (uint8_t i = 0; i < SAMPLE_COUNT; i++) {
        if (_scale.is_ready()) {
            sum += _scale.read();
            n++;
        }
        delay(HX711_READ_INTERVAL_MS);
    }
    if (n == 0) return;

    _tareOffset = sum / n;
    _scale.set_offset(_tareOffset);

    _ringFilled  = 0;
    _ringIdx     = 0;
    _rawAvg      = 0;
    _weightGrams = 0.0f;
    _stable      = false;
    _stableStartMs = millis();

    if (_cfgMgr) _cfgMgr->saveCalibration(_calibFactor, _tareOffset);
    Serial.printf("[Scale] Tare: %ld\n", _tareOffset);
}

void ScaleManager::setCalibrationFactor(float factor) {
    _calibFactor = factor;
    if (_cfgMgr) _cfgMgr->saveCalibration(_calibFactor, _tareOffset);
    _recalcWeight();
}

float ScaleManager::calibrateWithKnownWeight(float knownGrams) {
    if (knownGrams <= 0.0f || _rawAvg == 0) return _calibFactor;

    const long netRaw = _rawAvg - _tareOffset;
    if (netRaw == 0) return _calibFactor;

    _calibFactor = static_cast<float>(netRaw) / knownGrams;
    if (_cfgMgr) _cfgMgr->saveCalibration(_calibFactor, _tareOffset);
    _recalcWeight();
    Serial.printf("[Scale] Cal factor: %.4f  weight: %.1fg\n", _calibFactor, _weightGrams);
    return _calibFactor;
}

void ScaleManager::_updateAverage(long raw) {
    _ringBuf[_ringIdx] = raw;
    _ringIdx = (_ringIdx + 1) % SAMPLE_COUNT;
    if (_ringFilled < SAMPLE_COUNT) _ringFilled++;

    long sum = 0;
    for (uint8_t i = 0; i < _ringFilled; i++) sum += _ringBuf[i];
    _rawAvg = sum / _ringFilled;
}

void ScaleManager::_recalcWeight() {
    if (_calibFactor == 0.0f) { _weightGrams = 0.0f; return; }
    _weightGrams = static_cast<float>(_rawAvg - _tareOffset) / _calibFactor;
}

void ScaleManager::_updateStability() {
    if (fabsf(_weightGrams - _stableRefGrams) < _stableToleranceG) {
        if (!_stable && (millis() - _stableStartMs >= _stableDurationMs)) {
            _stable       = true;
            _stableWeight = _weightGrams;
        }
    } else {
        _stableRefGrams = _weightGrams;
        _stableStartMs  = millis();
        _stable         = false;
    }
}
