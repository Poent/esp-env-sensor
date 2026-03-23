// Pure helper logic shared by firmware modules and host-side unit tests.
//
// Nothing in this header depends on Arduino, which lets the project test core
// state transitions and validation rules on the host.

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace envnode::core {

// Minimal reading shape used by plausibility checks outside Arduino code.
struct LogicReadings {
  float temperature = NAN;
  float humidity = NAN;
  float pressure = NAN;
};

// Actions the runtime may need to take after evaluating battery-alert state.
enum class BatteryAlertAction {
  None,
  SendLow,
  SendClear,
  WaitingForNetwork,
};

// Result of one battery-alert state transition evaluation.
struct BatteryAlertResult {
  bool active = false;
  bool pending = false;
  BatteryAlertAction action = BatteryAlertAction::None;
};

// Clamps a requested interval into the allowed runtime bounds.
uint32_t SanitizeSampleInterval(uint32_t intervalSeconds,
                                uint32_t minIntervalSeconds,
                                uint32_t maxIntervalSeconds);

// Converts battery voltage into a coarse 0-100% estimate.
float BatteryVoltageToPercent(float voltage, float minVoltage, float maxVoltage);

// Evaluates retained battery-alert state for one new voltage sample.
BatteryAlertResult EvaluateBatteryAlert(float voltage,
                                        bool alertActive,
                                        bool alertPending,
                                        float lowThresholdVoltage,
                                        float clearThresholdVoltage,
                                        bool networkAvailable);

// Validates a reading against absolute ranges and, optionally, against the last
// accepted sample to catch sudden jumps.
bool PlausibleReadings(const LogicReadings& readings,
                       const LogicReadings* lastReadings = nullptr);

// Escapes arbitrary text for JSON string-safe output.
std::string JsonEscape(std::string_view input);

}  // namespace envnode::core
