// Pure helper logic implementation shared by firmware and host-side tests.

#include "core_logic.h"

#include <algorithm>
#include <cstdio>

namespace envnode::core {

// Clamps one requested interval into the allowed bounds.
uint32_t SanitizeSampleInterval(uint32_t intervalSeconds,
                                uint32_t minIntervalSeconds,
                                uint32_t maxIntervalSeconds) {
  return std::clamp(intervalSeconds, minIntervalSeconds, maxIntervalSeconds);
}

// Converts a battery voltage into a coarse percentage using fixed endpoints.
float BatteryVoltageToPercent(float voltage, float minVoltage, float maxVoltage) {
  if (std::isnan(voltage)) {
    return NAN;
  }

  const float normalized =
      (voltage - minVoltage) / (maxVoltage - minVoltage) * 100.0f;
  return std::clamp(normalized, 0.0f, 100.0f);
}

// Advances the battery-alert state machine for one new voltage sample.
BatteryAlertResult EvaluateBatteryAlert(float voltage,
                                        bool alertActive,
                                        bool alertPending,
                                        float lowThresholdVoltage,
                                        float clearThresholdVoltage,
                                        bool networkAvailable) {
  BatteryAlertResult result{alertActive, alertPending, BatteryAlertAction::None};
  if (std::isnan(voltage)) {
    return result;
  }

  if (!result.active && voltage <= lowThresholdVoltage) {
    result.active = true;
    result.pending = true;
  } else if (result.active && voltage >= clearThresholdVoltage) {
    result.active = false;
    result.pending = false;
    result.action = BatteryAlertAction::SendClear;
    return result;
  }

  if (!result.pending) {
    return result;
  }

  result.action = networkAvailable ? BatteryAlertAction::SendLow
                                   : BatteryAlertAction::WaitingForNetwork;
  return result;
}

// Validates a sample against absolute limits and optional jump thresholds.
bool PlausibleReadings(const LogicReadings& readings,
                       const LogicReadings* lastReadings) {
  if (!(readings.temperature > -40.0f && readings.temperature < 85.0f)) {
    return false;
  }
  if (!(readings.humidity >= 0.0f && readings.humidity <= 100.0f)) {
    return false;
  }
  if (!(readings.pressure > 300.0f && readings.pressure < 1100.0f)) {
    return false;
  }

  if (!lastReadings) {
    return true;
  }

  if (std::fabs(readings.temperature - lastReadings->temperature) > 5.0f) {
    return false;
  }
  if (std::fabs(readings.humidity - lastReadings->humidity) > 15.0f) {
    return false;
  }
  if (std::fabs(readings.pressure - lastReadings->pressure) > 10.0f) {
    return false;
  }

  return true;
}

// Escapes a string so it can be embedded safely inside JSON output.
std::string JsonEscape(std::string_view input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '\"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          output += buffer;
        } else {
          output += c;
        }
        break;
    }
  }
  return output;
}

}  // namespace envnode::core
