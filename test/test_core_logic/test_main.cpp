// Host-side unit tests for the pure helper logic in `lib/envnode_core`.
//
// These tests exercise validation and state-transition rules without requiring
// an ESP32 build or physical hardware.

#include <unity.h>

#include <cmath>
#include <core_logic.h>

using envnode::core::BatteryAlertAction;
using envnode::core::EvaluateBatteryAlert;
using envnode::core::JsonEscape;
using envnode::core::LogicReadings;
using envnode::core::PlausibleReadings;
using envnode::core::SanitizeSampleInterval;

// Unity fixture hook required by the test runner.
void setUp() {}

// Unity fixture hook required by the test runner.
void tearDown() {}

// Verifies that requested sample intervals are clamped to configured bounds.
void test_sanitize_interval_clamps_to_bounds() {
  TEST_ASSERT_EQUAL_UINT32(60, SanitizeSampleInterval(10, 60, 86400));
  TEST_ASSERT_EQUAL_UINT32(600, SanitizeSampleInterval(600, 60, 86400));
  TEST_ASSERT_EQUAL_UINT32(86400, SanitizeSampleInterval(90000, 60, 86400));
}

// Confirms that a normal environmental sample passes plausibility checks.
void test_plausible_readings_accepts_reasonable_sample() {
  LogicReadings sample{24.5f, 41.0f, 840.0f};
  LogicReadings last{24.8f, 40.0f, 839.5f};
  TEST_ASSERT_TRUE(PlausibleReadings(sample, &last));
}

// Confirms that a large jump from the previous sample is rejected.
void test_plausible_readings_rejects_large_temperature_jump() {
  LogicReadings sample{35.0f, 41.0f, 840.0f};
  LogicReadings last{24.8f, 40.0f, 839.5f};
  TEST_ASSERT_FALSE(PlausibleReadings(sample, &last));
}

// Verifies the low-battery and clear transitions of the battery-alert state
// machine.
void test_battery_alert_transitions_low_and_clear() {
  auto lowResult = EvaluateBatteryAlert(3.40f, false, false, 3.50f, 3.65f, true);
  TEST_ASSERT_TRUE(lowResult.active);
  TEST_ASSERT_TRUE(lowResult.pending);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryAlertAction::SendLow),
                    static_cast<int>(lowResult.action));

  auto clearResult = EvaluateBatteryAlert(3.70f, true, false, 3.50f, 3.65f, true);
  TEST_ASSERT_FALSE(clearResult.active);
  TEST_ASSERT_FALSE(clearResult.pending);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryAlertAction::SendClear),
                    static_cast<int>(clearResult.action));
}

// Verifies that the alert stays pending when the battery is low but the device
// cannot currently report it.
void test_battery_alert_waits_for_network() {
  auto result = EvaluateBatteryAlert(3.45f, false, false, 3.50f, 3.65f, false);
  TEST_ASSERT_TRUE(result.active);
  TEST_ASSERT_TRUE(result.pending);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryAlertAction::WaitingForNetwork),
                    static_cast<int>(result.action));
}

// Confirms that JSON escaping handles common control characters correctly.
void test_json_escape_escapes_control_characters() {
  const auto escaped = JsonEscape("line1\n\"quoted\"\tvalue");
  TEST_ASSERT_EQUAL_STRING("line1\\n\\\"quoted\\\"\\tvalue", escaped.c_str());
}

// Native test entry point for the Unity runner.
int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_sanitize_interval_clamps_to_bounds);
  RUN_TEST(test_plausible_readings_accepts_reasonable_sample);
  RUN_TEST(test_plausible_readings_rejects_large_temperature_jump);
  RUN_TEST(test_battery_alert_transitions_low_and_clear);
  RUN_TEST(test_battery_alert_waits_for_network);
  RUN_TEST(test_json_escape_escapes_control_characters);
  return UNITY_END();
}
