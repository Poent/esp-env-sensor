// Shared state and retained data for the firmware.
//
// `AppContext` holds runtime-only state that changes while the board is awake,
// while `PersistentState` stores values we keep across deep-sleep wakes.

#pragma once

#include "app_config.h"

// One environmental sample plus optional battery information collected during
// the same cycle.
struct SensorReadings {
  float temperature = NAN;
  float humidity = NAN;
  float pressure = NAN;
  float batteryVoltage = NAN;
  float batteryPercent = NAN;
};

// High-level boot source used to decide whether startup-only hooks should run.
enum class BootMode {
  ColdBoot,
  TimerWake,
  OtherReset,
};

// Current top-level runtime path. Normal mode samples automatically; USB
// service mode pauses automation and exposes diagnostics over serial.
enum class RuntimeMode {
  Normal,
  UsbService,
};

// Retained values that should survive deep sleep without re-deriving them on
// every boot.
struct PersistentState {
  SensorReadings lastGood;
  bool hasLastGood = false;
  bool lowBatteryAlertActive = false;
  bool lowBatteryAlertPending = false;
};

// Runtime state shared by the firmware modules while the board is awake.
struct AppContext {
  uint32_t sampleIntervalSeconds = DEFAULT_SAMPLE_INTERVAL_SECONDS;
  BootMode bootMode = BootMode::OtherReset;
  RuntimeMode runtimeMode = RuntimeMode::Normal;
  uint8_t bmeAddress = 0;
  bool bmeInitialized = false;
  bool sensePowerEnabled = false;
  bool lastI2cClearRequired = false;
  bool inErrorState = false;
  unsigned long lastWebhookSent = 0;
  bool networkAvailable = false;
  wl_status_t lastReportedWiFiStatus = WL_IDLE_STATUS;
  bool wifiHasConfiguredSta = false;
  wifi_event_id_t wifiEventLoggerHandle = 0;
  uint8_t targetBssid[6] = {0};
  bool hasTargetBssid = false;
  int32_t targetChannel = 0;
  uint16_t lastWiFiDisconnectReason = 0;
  uint32_t wifiConnectFailures = 0;
  unsigned long lastSampleRunMs = 0;
  String serialInputBuffer;
  bool holdAwakeForDiagnostics = false;
  bool usbServiceEventSent = false;
  bool usbServiceWebhookSent = false;
  String sessionId;
};

extern AppContext gApp;
extern PersistentState gPersistentState;

// Detects why the current boot happened so startup logic can branch cleanly.
BootMode detectBootMode();

// Returns a stable printable name for the current boot source.
const char* bootModeName(BootMode mode);

// Returns a stable printable name for the current runtime mode.
const char* runtimeModeName(RuntimeMode mode);

// Indicates whether a last-known-good reading is currently available.
bool hasLastGoodReading();

// Returns the last-known-good reading when available, otherwise `nullptr`.
const SensorReadings* getLastGoodReading();

// Stores a newly accepted reading as the retained last-known-good snapshot.
void setLastGoodReading(const SensorReadings& readings);

// Clamps a requested interval to the firmware's allowed runtime bounds.
uint32_t sanitizeSampleIntervalSeconds(uint32_t intervalSeconds);

// Loads the persisted interval override from NVS, falling back to defaults.
uint32_t loadSampleIntervalSeconds();

// Persists a new interval override and updates the in-memory copy.
bool saveSampleIntervalSeconds(uint32_t intervalSeconds);

// Removes any persisted interval override and restores the default cadence.
bool clearSampleIntervalOverride();

// Prints the active/default interval configuration for diagnostics.
void printSampleIntervalConfig();

// Creates a per-boot session identifier for correlating telemetry events.
void ensureSessionId();
