#pragma once

#include <cstdint>

enum class DrinkDetectorState : uint8_t {
    NO_CUP,
    CUP_UNSTABLE,
    CUP_STABLE,
    POSSIBLE_DRINK,
    DRINK_CONFIRMED,
    REFILL_DETECTED
};

struct DrinkDetectorConfig {
    float cupPresentThresholdGram = 80.0f;
    float minDrinkDeltaMl = 20.0f;
    float maxDrinkDeltaMl = 500.0f;
    uint32_t liftTimeoutMs = 120000;
};

struct DrinkDetectorObservation {
    float weightGrams = 0.0f;
    float stableWeightGrams = 0.0f;
    bool scaleStable = false;
};

struct DrinkCounterSnapshot {
    char period[16] = {};
    float totalMl = 0.0f;
    float lastDrinkMl = 0.0f;
    uint32_t drinkCount = 0;
};

enum class DrinkCounterLoadStatus : uint8_t {
    LOAD_FAILED,
    EMPTY,
    CURRENT_PERIOD,
    PREVIOUS_PERIOD
};

class DrinkCounterPersistence {
public:
    virtual ~DrinkCounterPersistence() = default;
    virtual DrinkCounterLoadStatus load(const char* currentPeriod,
                                        DrinkCounterSnapshot& snapshot) = 0;
    virtual void save(const char* currentPeriod,
                      const DrinkCounterSnapshot& snapshot) = 0;
    virtual bool isIdle() const { return true; }
};

class DrinkDetectorEffects {
public:
    virtual ~DrinkDetectorEffects() = default;
    virtual void resetReminder() = 0;
    virtual void playDrinkBeep() = 0;
    virtual void notifyDrink(float amountMl, float totalMl,
                             uint32_t drinkCount) = 0;
    virtual void logDrink(const char* timestamp, float amountMl,
                          float totalMl) = 0;
    virtual void publishStatus(float totalMl, const char* event) = 0;
};

class DrinkDetectorEventHandler {
public:
    void init(DrinkCounterPersistence* persistence, DrinkDetectorEffects* effects);
    void setPersistence(DrinkCounterPersistence* persistence) { _persistence = persistence; }
    DrinkCounterLoadStatus restore(const char* currentPeriod);
    DrinkCounterLoadStatus applyRestore(DrinkCounterLoadStatus status,
                                        const DrinkCounterSnapshot& snapshot,
                                        const char* currentPeriod);
    void save(const char* currentPeriod);
    void onDrinkConfirmed(float amountMl, const char* period, const char* timestamp);
    void onRefillDetected(float amountMl);
    void resetDailyCounters(const char* period);
    DrinkCounterSnapshot snapshot(const char* period = nullptr) const;
    void mergeSnapshot(const DrinkCounterSnapshot& snapshot, const char* period);

    float getTodayTotalMl() const { return _todayTotalMl; }
    float getLastDrinkMl() const { return _lastDrinkMl; }
    uint32_t getDrinkCountToday() const { return _drinkCount; }
    const char* getRestoredPeriod() const { return _restoredPeriod; }

private:
    DrinkCounterPersistence* _persistence = nullptr;
    DrinkDetectorEffects* _effects = nullptr;
    float _todayTotalMl = 0.0f;
    float _lastDrinkMl = 0.0f;
    uint32_t _drinkCount = 0;
    char _restoredPeriod[16] = {};

    void _save(const char* period);
};

class DrinkDetectorEventSink {
public:
    virtual ~DrinkDetectorEventSink() = default;
    virtual void onDrinkConfirmed(float amountMl) = 0;
    virtual void onRefillDetected(float amountMl) = 0;
};

class DrinkDetectorCore {
public:
    void init(const DrinkDetectorConfig& config, DrinkDetectorEventSink* sink);
    void update(const DrinkDetectorObservation& observation, uint32_t nowMs);

    DrinkDetectorState getState() const { return _state; }

private:
    DrinkDetectorConfig _config;
    DrinkDetectorEventSink* _sink = nullptr;
    DrinkDetectorState _state = DrinkDetectorState::NO_CUP;
    float _prevStableWeight = 0.0f;
    bool _cupLifted = false;
    uint32_t _cupLiftedAtMs = 0;

    void _transitionTo(DrinkDetectorState next);
};
