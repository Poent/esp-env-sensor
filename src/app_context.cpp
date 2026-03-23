// Shared application state implementation.
//
// This file owns the actual global state instances plus the small helpers that
// convert between retained state, user-facing names, and stored interval
// configuration in NVS.

#include "app_context.h"

#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <core_logic.h>

AppContext gApp;
RTC_DATA_ATTR PersistentState gPersistentState = {};

// Determines whether the current wake was caused by the timer, a cold boot, or
// some other reset source.
BootMode detectBootMode() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    return BootMode::TimerWake;
  }

  return esp_reset_reason() == ESP_RST_POWERON ? BootMode::ColdBoot
                                               : BootMode::OtherReset;
}

// Converts the boot-mode enum into a stable string for logs and telemetry.
const char* bootModeName(BootMode mode) {
  switch (mode) {
    case BootMode::ColdBoot:
      return "cold_boot";
    case BootMode::TimerWake:
      return "timer_wake";
    case BootMode::OtherReset:
    default:
      return "other_reset";
  }
}

// Converts the runtime-mode enum into a stable string for logs and telemetry.
const char* runtimeModeName(RuntimeMode mode) {
  switch (mode) {
    case RuntimeMode::UsbService:
      return "usb_service";
    case RuntimeMode::Normal:
    default:
      return "normal";
  }
}

// Reports whether the retained last-good reading has been initialized yet.
bool hasLastGoodReading() {
  return gPersistentState.hasLastGood;
}

// Returns the retained last-good reading so callers can do plausibility checks.
const SensorReadings* getLastGoodReading() {
  return gPersistentState.hasLastGood ? &gPersistentState.lastGood : nullptr;
}

// Updates the retained last-good reading after a sample has been accepted.
void setLastGoodReading(const SensorReadings& readings) {
  gPersistentState.lastGood = readings;
  gPersistentState.hasLastGood = true;
}

// Clamps an interval request using the pure helper library shared with tests.
uint32_t sanitizeSampleIntervalSeconds(uint32_t intervalSeconds) {
  return envnode::core::SanitizeSampleInterval(intervalSeconds,
                                               MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS,
                                               MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS);
}

// Reads the persisted interval from NVS unless debug mode forces its own fixed
// cadence.
uint32_t loadSampleIntervalSeconds() {
  if (DEBUG_MODE_ENABLED) {
    return sanitizeSampleIntervalSeconds(DEBUG_SAMPLE_INTERVAL);
  }

  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    return DEFAULT_SAMPLE_INTERVAL_SECONDS;
  }

  uint32_t intervalSeconds =
      prefs.getULong(SAMPLE_INTERVAL_KEY, DEFAULT_SAMPLE_INTERVAL_SECONDS);
  prefs.end();
  return sanitizeSampleIntervalSeconds(intervalSeconds);
}

// Saves a new interval override into NVS and mirrors it into the runtime state.
bool saveSampleIntervalSeconds(uint32_t intervalSeconds) {
  if (DEBUG_MODE_ENABLED) {
    gApp.sampleIntervalSeconds = sanitizeSampleIntervalSeconds(DEBUG_SAMPLE_INTERVAL);
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }

  uint32_t sanitized = sanitizeSampleIntervalSeconds(intervalSeconds);
  bool ok = prefs.putULong(SAMPLE_INTERVAL_KEY, sanitized) == sizeof(uint32_t);
  prefs.end();
  if (ok) {
    gApp.sampleIntervalSeconds = sanitized;
  }
  return ok;
}

// Removes any saved interval override and restores the compiled default.
bool clearSampleIntervalOverride() {
  if (DEBUG_MODE_ENABLED) {
    gApp.sampleIntervalSeconds = sanitizeSampleIntervalSeconds(DEBUG_SAMPLE_INTERVAL);
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }

  bool ok = prefs.remove(SAMPLE_INTERVAL_KEY);
  prefs.end();
  gApp.sampleIntervalSeconds = DEFAULT_SAMPLE_INTERVAL_SECONDS;
  return ok;
}

// Prints the active interval plus the allowed/default bounds for serial use.
void printSampleIntervalConfig() {
  Serial.printf("Sample interval: %lu seconds (mode=%s, default %lu, allowed %lu-%lu)\n",
                static_cast<unsigned long>(gApp.sampleIntervalSeconds),
                DEBUG_MODE_ENABLED ? "debug" : "production",
                static_cast<unsigned long>(DEFAULT_SAMPLE_INTERVAL_SECONDS),
                static_cast<unsigned long>(MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS),
                static_cast<unsigned long>(MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS));
}

// Builds a per-boot session ID from the device MAC and a random suffix.
void ensureSessionId() {
  if (gApp.sessionId.length()) {
    return;
  }

  uint64_t mac = ESP.getEfuseMac();
  gApp.sessionId = String(static_cast<uint32_t>(mac >> 32), HEX) +
                   String(static_cast<uint32_t>(mac), HEX) + "-" +
                   String(static_cast<uint32_t>(esp_random()), HEX);
}
