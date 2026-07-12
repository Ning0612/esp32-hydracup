#pragma once
#include <Arduino.h>
#include <HX711.h>

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
    bool  startTare();
    bool  takeTareResult(long& offset);
    bool  takeTareFailure();
    bool  isTareRunning() const { return _tareRunning || _tareWarming; }
    void  setCalibrationFactor(float factor);
    float calibrateWithKnownWeight(float knownGrams);

    long  getTareOffset()        const { return _tareOffset; }
    float getCalibrationFactor() const { return _calibFactor; }

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

    bool     _tareRunning   = false;
    bool     _tareWarming   = false;
    bool     _tareCompleted = false;
    bool     _tareFailed    = false;
    long     _tareSum       = 0;
    uint8_t  _tareCount     = 0;
    uint32_t _tareStartMs   = 0;

    void _updateAverage(long raw);
    void _recalcWeight();
    void _updateStability();
    void _finishTare();
};
