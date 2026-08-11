#include <unity.h>

#include "ReminderCore.h"

void setUp() {}
void tearDown() {}

namespace {

void test_waits_for_stable_cup_before_starting() {
    ReminderCore core;
    core.init(60000, true, 1000);

    core.update(61000, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::WAITING_FOR_CUP),
                          static_cast<int>(core.state()));
    TEST_ASSERT_EQUAL_UINT32(0, core.remainingMs(61000));

    core.update(62000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::COUNTING),
                          static_cast<int>(core.state()));
    TEST_ASSERT_EQUAL_UINT32(60000, core.remainingMs(62000));
}

void test_cup_away_pauses_and_return_resumes_remaining_time() {
    ReminderCore core;
    core.init(60000, true, 0);
    core.update(1000, true);
    core.update(21000, true);
    core.update(22000, false);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::PAUSED_CUP_AWAY),
                          static_cast<int>(core.state()));
    TEST_ASSERT_EQUAL_UINT32(39000, core.remainingMs(122000));

    core.update(222000, true);
    core.update(260999, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::COUNTING),
                          static_cast<int>(core.state()));
    core.update(261000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));
    TEST_ASSERT_TRUE(core.consumeAlertStarted());
    TEST_ASSERT_FALSE(core.consumeAlertStarted());
}

void test_alert_remains_until_valid_drink() {
    ReminderCore core;
    core.init(10000, true, 0);
    core.update(100, true);
    core.update(10100, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));

    core.update(600000, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));

    core.onDrinkConfirmed(601000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::COUNTING),
                          static_cast<int>(core.state()));
    TEST_ASSERT_EQUAL_UINT32(10000, core.remainingMs(601000));
}

void test_refill_or_invalid_delta_cannot_reset_core() {
    ReminderCore core;
    core.init(10000, true, 0);
    core.update(100, true);
    core.update(5100, true);

    // ReminderCore deliberately exposes no refill/weight-change reset API.
    core.update(10100, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));
}

void test_snooze_counts_only_stable_cup_time() {
    ReminderCore core;
    core.init(60000, true, 0);
    core.update(100, true);
    core.update(60100, true);
    core.snooze(20000, 61000, true);

    core.update(71000, true);
    core.update(72000, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::PAUSED_CUP_AWAY),
                          static_cast<int>(core.state()));
    TEST_ASSERT_EQUAL_UINT32(9000, core.remainingMs(172000));

    core.update(180000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::SNOOZED),
                          static_cast<int>(core.state()));
    core.update(189000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));
}

void test_pause_today_requires_explicit_resume() {
    ReminderCore core;
    core.init(60000, true, 0);
    core.update(100, true);
    core.setPausedToday(true, 1000, true);
    core.update(1000000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::PAUSED_TODAY),
                          static_cast<int>(core.state()));

    core.setPausedToday(false, 1000001, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::COUNTING),
                          static_cast<int>(core.state()));
    TEST_ASSERT_EQUAL_UINT32(60000, core.remainingMs(1000001));
}

void test_pause_today_survives_interval_and_enable_updates() {
    ReminderCore core;
    core.init(60000, true, 0);
    core.update(100, true);
    core.setPausedToday(true, 1000, true);
    core.setIntervalMs(120000, 2000, true);
    core.setEnabled(true, 3000);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::PAUSED_TODAY),
                          static_cast<int>(core.state()));
}

void test_cup_away_at_expiry_waits_for_stable_return() {
    ReminderCore core;
    core.init(10000, true, 0);
    core.update(100, true);
    core.update(10100, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::PAUSED_CUP_AWAY),
                          static_cast<int>(core.state()));
    core.update(20000, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::COUNTING),
                          static_cast<int>(core.state()));
    core.update(20001, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));
}

void test_pause_today_survives_disabled_state_and_drink() {
    ReminderCore core;
    core.init(60000, false, 0);
    core.setPausedToday(true, 1, true);
    TEST_ASSERT_EQUAL(ReminderState::DISABLED, core.state());
    core.setEnabled(true, 2);
    TEST_ASSERT_EQUAL(ReminderState::PAUSED_TODAY, core.state());
    core.onDrinkConfirmed(3, true);
    TEST_ASSERT_EQUAL(ReminderState::PAUSED_TODAY, core.state());
    core.setEnabled(false, 4);
    core.setEnabled(true, 5);
    TEST_ASSERT_EQUAL(ReminderState::PAUSED_TODAY, core.state());
    core.setPausedToday(false, 6, true);
    TEST_ASSERT_EQUAL(ReminderState::COUNTING, core.state());
}

void test_wraparound_elapsed_is_supported() {
    ReminderCore core;
    core.init(1000, true, 0xFFFFFF00u);
    core.update(0xFFFFFF00u, true);
    core.update(0x000002E8u, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ReminderState::ALERTED),
                          static_cast<int>(core.state()));
}

}  // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_waits_for_stable_cup_before_starting);
    RUN_TEST(test_cup_away_pauses_and_return_resumes_remaining_time);
    RUN_TEST(test_alert_remains_until_valid_drink);
    RUN_TEST(test_refill_or_invalid_delta_cannot_reset_core);
    RUN_TEST(test_snooze_counts_only_stable_cup_time);
    RUN_TEST(test_pause_today_requires_explicit_resume);
    RUN_TEST(test_pause_today_survives_interval_and_enable_updates);
    RUN_TEST(test_cup_away_at_expiry_waits_for_stable_return);
    RUN_TEST(test_pause_today_survives_disabled_state_and_drink);
    RUN_TEST(test_wraparound_elapsed_is_supported);
    return UNITY_END();
}
