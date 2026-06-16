#pragma once
#include <Arduino.h>
#include <HX711.h>

class ConfigManager;

class ScaleManager {
public:
    void init(float calibFactor, long tareOffset,
              float stableToleranceG, uint32_t stableDurationMs);
    void update();

    bool  isReady()               const { return _ready; }
    bool  isSamplesReady()        const { return _ringFilled >= SAMPLE_COUNT; }
    bool  isStable()              const { return _stable; }
    float getWeightGrams()        const { return _weightGrams; }
    float getStableWeightGrams()  const { return _stableWeight; }
    long  getRawAverage()         const { return _rawAvg; }

    void  tare();
    void  setCalibrationFactor(float factor);
    float calibrateWithKnownWeight(float knownGrams);

    long  getTareOffset()        const { return _tareOffset; }
    float getCalibrationFactor() const { return _calibFactor; }

    void  setConfigManager(ConfigManager* mgr) { _cfgMgr = mgr; }

private:
    static const uint8_t SAMPLE_COUNT = 10;

    HX711    _scale;
    bool     _ready       = false;
    float    _calibFactor  = 1.0f;
    long     _tareOffset   = 0;
    float    _weightGrams  = 0.0f;

    // Moving average ring buffer
    long     _ringBuf[SAMPLE_COUNT] = {};
    uint8_t  _ringIdx    = 0;
    uint8_t  _ringFilled = 0;
    long     _rawAvg     = 0;

    // Stability tracking
    bool     _stable           = false;
    float    _stableWeight     = 0.0f;
    float    _stableRefGrams   = 0.0f;
    uint32_t _stableStartMs    = 0;
    float    _stableToleranceG = 3.0f;
    uint32_t _stableDurationMs = 1500;

    uint32_t _lastReadMs = 0;

    ConfigManager* _cfgMgr = nullptr;

    void _updateAverage(long raw);
    void _recalcWeight();
    void _updateStability();
};
