#include <unity.h>
#include <cstring>

#include "DrinkDetectorCore.h"

namespace {

struct RecordingSink final : DrinkDetectorEventSink {
    int drinkEvents = 0;
    int refillEvents = 0;
    float lastDrinkMl = 0.0f;
    float lastRefillMl = 0.0f;

    void onDrinkConfirmed(float amountMl) override {
        drinkEvents++;
        lastDrinkMl = amountMl;
    }

    void onRefillDetected(float amountMl) override {
        refillEvents++;
        lastRefillMl = amountMl;
    }
};

struct RecordingPersistence final : DrinkCounterPersistence {
    DrinkCounterLoadStatus loadStatus = DrinkCounterLoadStatus::EMPTY;
    DrinkCounterSnapshot stored;
    int loadCalls = 0;
    int saveCalls = 0;
    char lastSavePeriod[16] = {};
    DrinkCounterSnapshot lastSaved;

    DrinkCounterLoadStatus load(const char*, DrinkCounterSnapshot& snapshot) override {
        loadCalls++;
        snapshot = stored;
        return loadStatus;
    }

    void save(const char* period, const DrinkCounterSnapshot& snapshot) override {
        saveCalls++;
        std::strncpy(lastSavePeriod, period, sizeof(lastSavePeriod) - 1);
        lastSavePeriod[sizeof(lastSavePeriod) - 1] = '\0';
        lastSaved = snapshot;
    }
};

struct RecordingEffects final : DrinkDetectorEffects {
    int reminderResets = 0;
    int drinkBeeps = 0;
    int drinkNotifications = 0;
    int drinkLogs = 0;
    int statusPublishes = 0;
    float lastPublishedTotal = 0.0f;
    uint32_t lastNotifiedCount = 0;
    char lastPublishedEvent[16] = {};
    char lastTimestamp[32] = {};

    void resetReminder() override { reminderResets++; }
    void playDrinkBeep() override { drinkBeeps++; }
    void notifyDrink(float, float, uint32_t drinkCount) override {
        drinkNotifications++;
        lastNotifiedCount = drinkCount;
    }
    void logDrink(const char* timestamp, float, float) override {
        drinkLogs++;
        std::strncpy(lastTimestamp, timestamp, sizeof(lastTimestamp) - 1);
        lastTimestamp[sizeof(lastTimestamp) - 1] = '\0';
    }
    void publishStatus(float totalMl, const char* event) override {
        statusPublishes++;
        lastPublishedTotal = totalMl;
        std::strncpy(lastPublishedEvent, event, sizeof(lastPublishedEvent) - 1);
        lastPublishedEvent[sizeof(lastPublishedEvent) - 1] = '\0';
    }
};

DrinkDetectorObservation sample(float weight, float stableWeight, bool stable) {
    DrinkDetectorObservation observation;
    observation.weightGrams = weight;
    observation.stableWeightGrams = stableWeight;
    observation.scaleStable = stable;
    return observation;
}

void initializeAtStableCup(DrinkDetectorCore& detector, uint32_t& nowMs,
                           float weight = 100.0f) {
    detector.update(sample(0.0f, 0.0f, false), nowMs++);
    detector.update(sample(weight, weight, false), nowMs++);
    detector.update(sample(weight, weight, true), nowMs++);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::CUP_STABLE),
                          static_cast<int>(detector.getState()));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_initial_cup_path_reaches_stable() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    detector.init(DrinkDetectorConfig{}, &sink);
    uint32_t nowMs = 0;

    initializeAtStableCup(detector, nowMs);

    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(0, sink.refillEvents);
}

void test_weight_change_without_lift_does_not_create_event() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    detector.init(DrinkDetectorConfig{}, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 250.0f);

    detector.update(sample(220.0f, 220.0f, false), nowMs++);
    detector.update(sample(220.0f, 220.0f, true), nowMs++);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::CUP_STABLE),
                          static_cast<int>(detector.getState()));
    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(0, sink.refillEvents);
}

void test_lift_and_valid_delta_confirms_drink() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    detector.init(DrinkDetectorConfig{}, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 250.0f);

    detector.update(sample(0.0f, 0.0f, false), nowMs++);
    detector.update(sample(200.0f, 200.0f, true), nowMs++);
    detector.update(sample(200.0f, 200.0f, true), nowMs++);

    TEST_ASSERT_EQUAL_INT(1, sink.drinkEvents);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, sink.lastDrinkMl);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::DRINK_CONFIRMED),
                          static_cast<int>(detector.getState()));

    detector.update(sample(200.0f, 200.0f, true), nowMs++);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::CUP_STABLE),
                          static_cast<int>(detector.getState()));
}

void test_delta_above_maximum_is_ignored() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    DrinkDetectorConfig config;
    config.minDrinkDeltaMl = 20.0f;
    config.maxDrinkDeltaMl = 100.0f;
    detector.init(config, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 250.0f);

    detector.update(sample(0.0f, 0.0f, false), nowMs++);
    detector.update(sample(120.0f, 120.0f, true), nowMs++);
    detector.update(sample(120.0f, 120.0f, true), nowMs++);

    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(0, sink.refillEvents);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::CUP_STABLE),
                          static_cast<int>(detector.getState()));
}

void test_delta_below_minimum_is_ignored() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    detector.init(DrinkDetectorConfig{}, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 250.0f);

    detector.update(sample(0.0f, 0.0f, false), nowMs++);
    detector.update(sample(235.0f, 235.0f, true), nowMs++);
    detector.update(sample(235.0f, 235.0f, true), nowMs++);

    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(0, sink.refillEvents);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::CUP_STABLE),
                          static_cast<int>(detector.getState()));
}

void test_lift_and_increased_weight_reports_refill() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    detector.init(DrinkDetectorConfig{}, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 150.0f);

    detector.update(sample(0.0f, 0.0f, false), nowMs++);
    detector.update(sample(200.0f, 200.0f, true), nowMs++);
    detector.update(sample(200.0f, 200.0f, true), nowMs++);

    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(1, sink.refillEvents);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, sink.lastRefillMl);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::REFILL_DETECTED),
                          static_cast<int>(detector.getState()));
}

void test_lift_timeout_clears_pending_lift() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    DrinkDetectorConfig config;
    config.liftTimeoutMs = 100;
    detector.init(config, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 250.0f);

    detector.update(sample(0.0f, 0.0f, false), 10);
    detector.update(sample(200.0f, 200.0f, true), 111);
    detector.update(sample(200.0f, 200.0f, true), 112);

    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(0, sink.refillEvents);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkDetectorState::CUP_STABLE),
                          static_cast<int>(detector.getState()));
}

void test_lift_timeout_handles_uint32_wraparound() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    DrinkDetectorConfig config;
    config.liftTimeoutMs = 100;
    detector.init(config, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 250.0f);

    detector.update(sample(0.0f, 0.0f, false), UINT32_MAX - 15);
    detector.update(sample(200.0f, 200.0f, true), 64);
    detector.update(sample(200.0f, 200.0f, true), 65);

    TEST_ASSERT_EQUAL_INT(1, sink.drinkEvents);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, sink.lastDrinkMl);
}

void test_lift_timeout_is_strictly_greater_than_boundary() {
    RecordingSink boundarySink;
    DrinkDetectorCore boundaryDetector;
    DrinkDetectorConfig config;
    config.liftTimeoutMs = 100;
    boundaryDetector.init(config, &boundarySink);
    uint32_t nowMs = 0;
    initializeAtStableCup(boundaryDetector, nowMs, 250.0f);

    boundaryDetector.update(sample(0.0f, 0.0f, false), 100);
    boundaryDetector.update(sample(200.0f, 200.0f, true), 200);
    boundaryDetector.update(sample(200.0f, 200.0f, true), 201);
    TEST_ASSERT_EQUAL_INT(1, boundarySink.drinkEvents);

    RecordingSink expiredSink;
    DrinkDetectorCore expiredDetector;
    expiredDetector.init(config, &expiredSink);
    nowMs = 0;
    initializeAtStableCup(expiredDetector, nowMs, 250.0f);
    expiredDetector.update(sample(0.0f, 0.0f, false), 100);
    expiredDetector.update(sample(200.0f, 200.0f, true), 201);
    expiredDetector.update(sample(200.0f, 200.0f, true), 202);
    TEST_ASSERT_EQUAL_INT(0, expiredSink.drinkEvents);
}

void test_event_handler_restores_and_dispatches_drink_effects() {
    RecordingPersistence persistence;
    persistence.loadStatus = DrinkCounterLoadStatus::CURRENT_PERIOD;
    std::strcpy(persistence.stored.period, "2026-07-13");
    persistence.stored.totalMl = 300.0f;
    persistence.stored.lastDrinkMl = 25.0f;
    persistence.stored.drinkCount = 3;
    RecordingEffects effects;
    DrinkDetectorEventHandler handler;
    handler.init(&persistence, &effects);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkCounterLoadStatus::CURRENT_PERIOD),
                          static_cast<int>(handler.restore("2026-07-13")));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, handler.getTodayTotalMl());
    TEST_ASSERT_EQUAL_UINT32(3, handler.getDrinkCountToday());

    handler.onDrinkConfirmed(50.0f, "2026-07-13", "2026-07-13T10:00:00");

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 350.0f, handler.getTodayTotalMl());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, handler.getLastDrinkMl());
    TEST_ASSERT_EQUAL_UINT32(4, handler.getDrinkCountToday());
    TEST_ASSERT_EQUAL_INT(1, effects.reminderResets);
    TEST_ASSERT_EQUAL_INT(1, effects.drinkBeeps);
    TEST_ASSERT_EQUAL_INT(1, effects.drinkNotifications);
    TEST_ASSERT_EQUAL_INT(1, effects.drinkLogs);
    TEST_ASSERT_EQUAL_INT(1, effects.statusPublishes);
    TEST_ASSERT_EQUAL_UINT32(4, effects.lastNotifiedCount);
    TEST_ASSERT_EQUAL_STRING("drink", effects.lastPublishedEvent);
    TEST_ASSERT_EQUAL_STRING("2026-07-13T10:00:00", effects.lastTimestamp);
    TEST_ASSERT_EQUAL_INT(1, persistence.saveCalls);
    TEST_ASSERT_EQUAL_STRING("2026-07-13", persistence.lastSavePeriod);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 350.0f, persistence.lastSaved.totalMl);
}

void test_event_handler_previous_restore_and_reset_save() {
    RecordingPersistence persistence;
    persistence.loadStatus = DrinkCounterLoadStatus::PREVIOUS_PERIOD;
    std::strcpy(persistence.stored.period, "2026-07-12");
    persistence.stored.totalMl = 600.0f;
    persistence.stored.drinkCount = 5;
    RecordingEffects effects;
    DrinkDetectorEventHandler handler;
    handler.init(&persistence, &effects);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkCounterLoadStatus::PREVIOUS_PERIOD),
                          static_cast<int>(handler.restore("2026-07-13")));
    TEST_ASSERT_EQUAL_STRING("2026-07-12", handler.getRestoredPeriod());
    TEST_ASSERT_EQUAL_INT(0, persistence.saveCalls);

    handler.resetDailyCounters("2026-07-13");

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, handler.getTodayTotalMl());
    TEST_ASSERT_EQUAL_UINT32(0, handler.getDrinkCountToday());
    TEST_ASSERT_EQUAL_INT(1, persistence.saveCalls);
    TEST_ASSERT_EQUAL_STRING("2026-07-13", persistence.lastSavePeriod);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, persistence.lastSaved.totalMl);
}

void test_event_handler_empty_restore_initializes_persistence() {
    RecordingPersistence persistence;
    RecordingEffects effects;
    DrinkDetectorEventHandler handler;
    handler.init(&persistence, &effects);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkCounterLoadStatus::EMPTY),
                          static_cast<int>(handler.restore("2026-07-13")));
    TEST_ASSERT_EQUAL_INT(1, persistence.saveCalls);
    TEST_ASSERT_EQUAL_STRING("2026-07-13", persistence.lastSavePeriod);
    TEST_ASSERT_EQUAL_UINT32(0, persistence.lastSaved.drinkCount);
}

void test_event_handler_load_failure_preserves_ram_counters() {
    RecordingPersistence persistence;
    RecordingEffects effects;
    DrinkDetectorEventHandler handler;
    handler.init(&persistence, &effects);
    handler.onDrinkConfirmed(40.0f, "", "boot+100ms");
    const int savesBefore = persistence.saveCalls;
    DrinkCounterSnapshot unavailable;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(DrinkCounterLoadStatus::LOAD_FAILED),
                          static_cast<int>(handler.applyRestore(
                              DrinkCounterLoadStatus::LOAD_FAILED, unavailable,
                              "2026-07-13")));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, handler.getTodayTotalMl());
    TEST_ASSERT_EQUAL_UINT32(1, handler.getDrinkCountToday());
    TEST_ASSERT_EQUAL_INT(savesBefore, persistence.saveCalls);
}

void test_event_handler_merges_pre_restore_snapshot() {
    RecordingPersistence persistence;
    RecordingEffects effects;
    DrinkDetectorEventHandler handler;
    handler.init(&persistence, &effects);
    DrinkCounterSnapshot persisted;
    std::strcpy(persisted.period, "2026-07-13");
    persisted.totalMl = 300.0f;
    persisted.lastDrinkMl = 100.0f;
    persisted.drinkCount = 3;
    handler.applyRestore(DrinkCounterLoadStatus::CURRENT_PERIOD, persisted, "2026-07-13");

    DrinkCounterSnapshot preSync;
    preSync.totalMl = 50.0f;
    preSync.lastDrinkMl = 50.0f;
    preSync.drinkCount = 1;
    handler.mergeSnapshot(preSync, "2026-07-13");

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 350.0f, handler.getTodayTotalMl());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, handler.getLastDrinkMl());
    TEST_ASSERT_EQUAL_UINT32(4, handler.getDrinkCountToday());
    TEST_ASSERT_EQUAL_INT(1, persistence.saveCalls);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 350.0f, persistence.lastSaved.totalMl);
}

void test_event_handler_refill_only_publishes_status() {
    RecordingPersistence persistence;
    RecordingEffects effects;
    DrinkDetectorEventHandler handler;
    handler.init(&persistence, &effects);
    handler.onRefillDetected(100.0f);

    TEST_ASSERT_EQUAL_INT(1, effects.statusPublishes);
    TEST_ASSERT_EQUAL_STRING("refill", effects.lastPublishedEvent);
    TEST_ASSERT_EQUAL_INT(0, persistence.saveCalls);
    TEST_ASSERT_EQUAL_UINT32(0, handler.getDrinkCountToday());
}

void test_refill_above_drink_maximum_keeps_existing_refill_rule() {
    RecordingSink sink;
    DrinkDetectorCore detector;
    DrinkDetectorConfig config;
    config.maxDrinkDeltaMl = 100.0f;
    detector.init(config, &sink);
    uint32_t nowMs = 0;
    initializeAtStableCup(detector, nowMs, 150.0f);

    detector.update(sample(0.0f, 0.0f, false), nowMs++);
    detector.update(sample(400.0f, 400.0f, true), nowMs++);
    detector.update(sample(400.0f, 400.0f, true), nowMs++);

    TEST_ASSERT_EQUAL_INT(0, sink.drinkEvents);
    TEST_ASSERT_EQUAL_INT(1, sink.refillEvents);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, sink.lastRefillMl);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_initial_cup_path_reaches_stable);
    RUN_TEST(test_weight_change_without_lift_does_not_create_event);
    RUN_TEST(test_lift_and_valid_delta_confirms_drink);
    RUN_TEST(test_delta_above_maximum_is_ignored);
    RUN_TEST(test_delta_below_minimum_is_ignored);
    RUN_TEST(test_lift_and_increased_weight_reports_refill);
    RUN_TEST(test_lift_timeout_clears_pending_lift);
    RUN_TEST(test_lift_timeout_handles_uint32_wraparound);
    RUN_TEST(test_lift_timeout_is_strictly_greater_than_boundary);
    RUN_TEST(test_event_handler_restores_and_dispatches_drink_effects);
    RUN_TEST(test_event_handler_previous_restore_and_reset_save);
    RUN_TEST(test_event_handler_empty_restore_initializes_persistence);
    RUN_TEST(test_event_handler_load_failure_preserves_ram_counters);
    RUN_TEST(test_event_handler_merges_pre_restore_snapshot);
    RUN_TEST(test_event_handler_refill_only_publishes_status);
    RUN_TEST(test_refill_above_drink_maximum_keeps_existing_refill_rule);
    return UNITY_END();
}
