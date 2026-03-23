// Top-level runtime orchestration.
//
// This file coordinates the low-power sampling path and the diagnostics/service
// path. It decides when to wake hardware, when to upload, and when to sleep.

#include "runtime.h"

#include <core_logic.h>

#include "console.h"
#include "hardware.h"
#include "sensor_manager.h"
#include "telemetry.h"
#include "wifi_manager.h"

namespace {

// Distinguishes automatic cycles from operator-triggered manual samples.
enum class SampleRunKind {
  Automatic,
  ManualLocal,
  ManualUpload,
};

// Describes how one sampling run should behave, including whether uploads,
// startup hooks, timestamps, and debug notifications should be applied.
struct SampleRunOptions {
  SampleRunKind kind = SampleRunKind::Automatic;
  bool uploadRequested = true;
  bool runStartupHooks = false;
  bool updateLastSampleTimestamp = false;
  bool sendDebugHeartbeat = false;
};

// Captures the outcome of one sampling run so callers can act on success/failure
// without re-reading global state.
struct SampleRunResult {
  bool sensorReady = false;
  bool readingOk = false;
  bool uploadAttempted = false;
  bool uploadOk = false;
  SensorReadings reading;
  unsigned long cycleStartedAtMs = 0;
};

// Returns true when the current boot is a cold/other boot rather than a timer wake.
bool isStartupBoot() {
  return gApp.bootMode != BootMode::TimerWake;
}

// Records a startup issue without changing the device's sleep policy.
void noteStartupIssue(const char* message) {
  if (!isStartupBoot()) {
    return;
  }
  Serial.printf("Startup note: %s\n", message);
}

// Marks the device to stay awake after boot because the sensor/bootstrap path
// failed and the node should remain available for diagnostics.
void requestStartupDiagnosticsHold(const char* message) {
  if (!isStartupBoot()) {
    return;
  }

  gApp.holdAwakeForDiagnostics = true;
  Serial.printf("Startup sensor/bootstrap fault: %s\n", message);
}

// Startup hooks only run once on a non-timer boot before the first sample cycle.
bool shouldRunStartupHooks() {
  return gApp.runtimeMode == RuntimeMode::Normal && isStartupBoot() &&
         gApp.lastSampleRunMs == 0;
}

// Emits the one-time startup event/webhook after the first sampling attempt.
// `readings` may be `nullptr` when the first reading failed.
void maybeRunStartupHooks(const SensorReadings* readings, bool readingOk) {
  if (!shouldRunStartupHooks()) {
    return;
  }

  String startupMeta = buildBootMetaJson(!readingOk);
  bool startupEventOk = postEvent("startup",
                                  readingOk ? "info" : "warning",
                                  readingOk ? "device boot"
                                            : "device boot with invalid first reading",
                                  readingOk ? readings : nullptr,
                                  nullptr,
                                  0,
                                  readingOk,
                                  startupMeta.c_str());
  if (!startupEventOk) {
    noteStartupIssue("startup event post failed");
  }

  if (readingOk) {
    if (!sendWebhook("device_startup",
                     "Device booted successfully",
                     "info",
                     readings,
                     startupMeta.c_str())) {
      noteStartupIssue("startup webhook failed");
    }
    blinkColdBootSuccessLed();
  } else if (!sendWebhook("device_startup",
                          "Device booted but first reading failed",
                          "warning",
                          nullptr,
                          startupMeta.c_str())) {
    noteStartupIssue("startup warning webhook failed");
  }
}

// Updates retained battery-alert state and sends low/clear notifications when
// thresholds are crossed and connectivity is available.
void maybeHandleBatteryAlerts(const SensorReadings& readings) {
  auto result = envnode::core::EvaluateBatteryAlert(
      readings.batteryVoltage,
      gPersistentState.lowBatteryAlertActive,
      gPersistentState.lowBatteryAlertPending,
      LOW_BATTERY_ALERT_V,
      LOW_BATTERY_CLEAR_V,
      gApp.networkAvailable);

  gPersistentState.lowBatteryAlertActive = result.active;
  gPersistentState.lowBatteryAlertPending = result.pending;

  if (result.action == envnode::core::BatteryAlertAction::None) {
    return;
  }

  if (result.action == envnode::core::BatteryAlertAction::WaitingForNetwork) {
    Serial.println("Battery low alert pending: WiFi unavailable");
    return;
  }

  String meta = String("{\"battery_voltage_v\":") + String(readings.batteryVoltage, 3) +
                ",\"battery_pct\":" + String(readings.batteryPercent, 1) +
                ",\"alert_threshold_v\":" + String(LOW_BATTERY_ALERT_V, 2) +
                ",\"clear_threshold_v\":" + String(LOW_BATTERY_CLEAR_V, 2) + "}";
  String message;

  if (result.action == envnode::core::BatteryAlertAction::SendClear) {
    message = String("Battery recovered to ") + String(readings.batteryVoltage, 2) +
              "V (" + String(readings.batteryPercent, 0) + "%)";
    postEvent("battery_ok", "info", message, &readings, nullptr, 0, true, meta.c_str());
    sendWebhook("battery_ok", message, "info", &readings, meta.c_str());
    return;
  }

  message = String("Battery low: ") + String(readings.batteryVoltage, 2) + "V (" +
            String(readings.batteryPercent, 0) + "%)";
  bool eventOk =
      postEvent("battery_low", "warning", message, &readings, nullptr, 0, false,
                meta.c_str());
  bool webhookOk = sendWebhook("battery_low", message, "warning", &readings,
                               meta.c_str());
  if (eventOk && webhookOk) {
    gPersistentState.lowBatteryAlertPending = false;
  }
}

// Runs one complete sample path according to `options`. This is the shared core
// used by automatic cycles and manual USB-triggered samples.
SampleRunResult executeSampleRun(const SampleRunOptions& options) {
  SampleRunResult result;
  result.cycleStartedAtMs = millis();

  if (options.kind != SampleRunKind::Automatic &&
      gApp.runtimeMode != RuntimeMode::UsbService) {
    Serial.println("Manual sampling is only available in USB service mode.");
    return result;
  }

  setAwakeLed(true);
  ensureSessionId();
  enableSensePower();

  if (!gApp.bmeInitialized) {
    waitForSensorPowerRail();
    if (!initBME()) {
      disableSensePower();
      resetSensorState();

      if (options.runStartupHooks) {
        if (!gApp.networkAvailable && WiFi.status() != WL_CONNECTED &&
            !connectWiFi()) {
          noteStartupIssue("initial WiFi connect failed during BME fault report");
        }

        String meta = buildBootMetaJson(false);
        if (!postEvent("startup", "error", "BME init failed", nullptr, nullptr, 0,
                       false, meta.c_str())) {
          noteStartupIssue("startup BME error event failed");
        }
        if (shouldRunStartupHooks() &&
            !sendWebhook("device_startup",
                         "Device booted but BME init failed",
                         "error",
                         nullptr,
                         meta.c_str())) {
          noteStartupIssue("startup BME failure webhook failed");
        }
        requestStartupDiagnosticsHold("BME init failed");
      } else if (options.kind == SampleRunKind::ManualLocal ||
                 options.kind == SampleRunKind::ManualUpload) {
        Serial.println("Manual sample aborted: BME680 is unavailable.");
      } else {
        Serial.println("Skipping sample cycle: BME680 unavailable.");
      }

      if (options.sendDebugHeartbeat) {
        sendDebugDiscordMessage(nullptr, false, result.cycleStartedAtMs);
      }
      if (options.updateLastSampleTimestamp) {
        gApp.lastSampleRunMs = millis();
      }
      return result;
    }
  }

  result.sensorReady = true;
  float rawBatteryVoltage = readBatteryVoltage();
  float rawBatteryPercent = batteryVoltageToPercent(rawBatteryVoltage);
  if (options.kind == SampleRunKind::Automatic) {
    Serial.printf("Battery: %.2fV (%.0f%%)\n", rawBatteryVoltage, rawBatteryPercent);
  }

  result.readingOk = captureValidatedReading(result.reading, getLastGoodReading());
  result.reading.batteryVoltage = rawBatteryVoltage;
  result.reading.batteryPercent = rawBatteryPercent;

  disableSensePower();
  resetSensorState();

  if (!gApp.networkAvailable && options.uploadRequested && WiFi.status() != WL_CONNECTED) {
    bool wifiOk = connectWiFi();
    if (options.runStartupHooks && !wifiOk) {
      noteStartupIssue("initial WiFi connect failed");
    }
  }

  if (options.runStartupHooks && gApp.networkAvailable) {
    bool tablesOk = checkSupabaseTablesOnce();
    if (!tablesOk) {
      noteStartupIssue("Supabase connectivity check failed");
    }
  }

  if (options.runStartupHooks) {
    maybeRunStartupHooks(result.readingOk ? &result.reading : nullptr, result.readingOk);
  }

  if (result.readingOk) {
    setLastGoodReading(result.reading);
    if (options.kind == SampleRunKind::Automatic) {
      Serial.printf("GOOD: T=%.2f°C RH=%.1f%% P=%.1f hPa  VBAT=%.2fV (%.0f%%)\n",
                    result.reading.temperature,
                    result.reading.humidity,
                    result.reading.pressure,
                    result.reading.batteryVoltage,
                    result.reading.batteryPercent);
    } else {
      Serial.printf("MANUAL: T=%.2fC RH=%.1f%% P=%.1fhPa  VBAT=%.2fV (%.0f%%)\n",
                    result.reading.temperature,
                    result.reading.humidity,
                    result.reading.pressure,
                    result.reading.batteryVoltage,
                    result.reading.batteryPercent);
    }

    if (options.uploadRequested) {
      result.uploadAttempted = true;
      if (gApp.networkAvailable) {
        result.uploadOk = postReadings(result.reading);
        if (options.runStartupHooks && !result.uploadOk) {
          noteStartupIssue("initial reading upload failed");
        }
      } else if (options.kind == SampleRunKind::ManualUpload) {
        Serial.println("Manual upload skipped: WiFi unavailable.");
      } else {
        Serial.println("Skipping upload: WiFi unavailable for this cycle.");
        if (options.runStartupHooks) {
          noteStartupIssue("WiFi unavailable during initial upload");
        }
      }
    }

    if (options.kind == SampleRunKind::Automatic) {
      maybeHandleBatteryAlerts(result.reading);
    }

    if (gApp.inErrorState) {
      gApp.inErrorState = false;
      sendWebhook("sensor_recovered",
                  "Device recovered - normal operation resumed",
                  "info",
                  &result.reading);
    }
  } else if (options.kind == SampleRunKind::Automatic) {
    Serial.println("Dropping bad reading after recovery attempts.");
  } else {
    Serial.println("Manual sample failed: sensor did not return a stable reading.");
  }

  if (options.sendDebugHeartbeat) {
    bool heartbeatOk = result.uploadAttempted ? result.uploadOk : result.readingOk;
    sendDebugDiscordMessage(result.readingOk ? &result.reading : nullptr,
                            heartbeatOk,
                            result.cycleStartedAtMs);
  }

  if (options.updateLastSampleTimestamp) {
    gApp.lastSampleRunMs = millis();
  }

  return result;
}

// Sends the one-time "service mode active" event/webhook after Wi-Fi comes up.
void maybeAnnounceUsbServiceMode() {
  if (gApp.runtimeMode != RuntimeMode::UsbService || !gApp.networkAvailable) {
    return;
  }

  String meta = buildServiceModeMetaJson();
  if (!gApp.usbServiceEventSent) {
    gApp.usbServiceEventSent = postEvent(
        "service_mode",
        "info",
        "USB host detected; readings paused in diagnostic mode",
        nullptr,
        nullptr,
        0,
        true,
        meta.c_str());
  }

  if (!gApp.usbServiceWebhookSent) {
    gApp.usbServiceWebhookSent = sendWebhook(
        "device_service_mode",
        "Device is in USB diagnostic/charging mode; readings are paused",
        "info",
        nullptr,
        meta.c_str());
  }
}

// Runs the normal automatic wake/sample/upload cycle.
void runSamplingCycle() {
  executeSampleRun(
      {SampleRunKind::Automatic, true, shouldRunStartupHooks(), true, true});
}

// Switches the runtime into USB service mode and announces it if network access
// is available.
void enterUsbServiceMode() {
  gApp.runtimeMode = RuntimeMode::UsbService;
  resetSensorState();
  gApp.usbServiceEventSent = false;
  gApp.usbServiceWebhookSent = false;
  setAwakeLed(true);

  Serial.println("USB service mode active: automatic readings paused, deep sleep suppressed.");
  Serial.println("Use 'help' to list serial commands.");

  if (!gApp.networkAvailable && WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  maybeAnnounceUsbServiceMode();
  printRuntimeModeStatus();
}

// Runs the normal post-boot path: first sample cycle, optional debug hooks, and
// then deep sleep if enabled.
void runNormalModeStartupSequence() {
  gApp.runtimeMode = RuntimeMode::Normal;
  setAwakeLed(true);

  runSamplingCycle();

  if (DEBUG_WEBHOOKS && shouldRunStartupHooks() && gApp.networkAvailable) {
    testWebhooks();
  }

  if (DEEP_SLEEP_ENABLED) {
    enterDeepSleep();
  }
}

// Advances one iteration of USB service mode, including reconnect attempts,
// periodic status output, and host-detach detection.
void runUsbServiceLoopIteration() {
  static unsigned long lastStatusLogMs = 0;
  static unsigned long lastReconnectAttemptMs = 0;

  pollSerialCommands();

  unsigned long now = millis();
  wl_status_t status = WiFi.status();
  gApp.networkAvailable = status == WL_CONNECTED;

  if (status != gApp.lastReportedWiFiStatus) {
    logWiFiStatus("WiFi: current status ", status);
    gApp.lastReportedWiFiStatus = status;
  }

  if (!gApp.networkAvailable &&
      now - lastReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("USB service mode: retrying WiFi connection...");
    lastReconnectAttemptMs = now;
    connectWiFi();
  }

  maybeAnnounceUsbServiceMode();

  if (now - lastStatusLogMs >= USB_SERVICE_STATUS_INTERVAL_MS) {
    printRuntimeModeStatus();
    lastStatusLogMs = now;
  }

  if (!isUsbHostAttached()) {
    Serial.println("USB host detached; resuming normal sensing mode.");
    runNormalModeStartupSequence();
    return;
  }

  delay(250);
}

}  // namespace

// Prints a summary line describing the current runtime, boot, sensor, and
// network state.
void printRuntimeModeStatus() {
  wl_status_t wifiStatus = WiFi.status();
  gApp.networkAvailable = wifiStatus == WL_CONNECTED;
  Serial.printf("Mode status: runtime=%s boot=%s usb_host=%s sensor=%s deep_sleep=%s wifi=%s (%d)\n",
                runtimeModeName(gApp.runtimeMode),
                bootModeName(gApp.bootMode),
                isUsbHostAttached() ? "attached" : "detached",
                gApp.bmeInitialized ? "ready" : "not_ready",
                DEEP_SLEEP_ENABLED ? "enabled" : "disabled",
                wifiStatusName(wifiStatus),
                static_cast<int>(wifiStatus));
  if (gApp.networkAvailable) {
    Serial.printf("Mode status: IP=%s RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  static_cast<int>(WiFi.RSSI()));
  }
}

// Executes one manual sample in USB service mode, optionally uploading it.
bool performManualSample(bool uploadRequested) {
  SampleRunOptions options;
  options.kind = uploadRequested ? SampleRunKind::ManualUpload
                                 : SampleRunKind::ManualLocal;
  options.uploadRequested = uploadRequested;
  SampleRunResult result = executeSampleRun(options);
  return uploadRequested ? result.uploadOk : result.readingOk;
}

// Performs startup initialization, chooses the initial runtime path, and runs
// the first startup sequence.
void setupApp() {
  Serial.begin(115200);
  delay(1000);

  registerWiFiEventLogger();
  gApp.bootMode = detectBootMode();
  gApp.runtimeMode = RuntimeMode::Normal;
  gApp.sampleIntervalSeconds = loadSampleIntervalSeconds();
  initStatusLed();
  initSensePower();
  setAwakeLed(true);

  Serial.printf("\nBooting (%s)...\n", bootModeName(gApp.bootMode));
  printSampleIntervalConfig();
  Serial.printf("Runtime profile: %s, deep sleep: %s\n",
                DEBUG_MODE_ENABLED ? "debug" : "production",
                DEEP_SLEEP_ENABLED ? "enabled" : "disabled");
  ensureSessionId();

  if (gApp.bootMode != BootMode::TimerWake && USB_SERVICE_MODE_ENABLED &&
      isUsbHostAttached()) {
    enterUsbServiceMode();
    return;
  }

  handleSerialConfigWindow();
  runNormalModeStartupSequence();
}

// Advances the currently active runtime path from Arduino `loop()`.
void loopApp() {
  if (gApp.runtimeMode == RuntimeMode::UsbService) {
    runUsbServiceLoopIteration();
    return;
  }

  #if DISABLE_DEEP_SLEEP
  const bool awakeLoopActive = true;
  #else
  const bool awakeLoopActive = gApp.holdAwakeForDiagnostics;
  #endif

  if (awakeLoopActive) {
    static unsigned long lastStatusLogMs = 0;
    static unsigned long lastReconnectAttemptMs = 0;

    pollSerialCommands();

    unsigned long now = millis();
    wl_status_t status = WiFi.status();
    gApp.networkAvailable = status == WL_CONNECTED;

    if (status != gApp.lastReportedWiFiStatus || now - lastStatusLogMs >= 5000) {
      logWiFiStatus("WiFi: current status ", status);
      gApp.lastReportedWiFiStatus = status;
      lastStatusLogMs = now;
    }

    if (!gApp.networkAvailable &&
        now - lastReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
      Serial.println("WiFi: retrying connection from awake diagnostics loop...");
      lastReconnectAttemptMs = now;
      connectWiFi();
    }

    if (gApp.lastSampleRunMs == 0 ||
        now - gApp.lastSampleRunMs >=
            static_cast<unsigned long>(gApp.sampleIntervalSeconds) * 1000UL) {
      Serial.println("Sample interval elapsed; running awake-mode cycle.");
      runSamplingCycle();
    }

    delay(250);
    return;
  }

  delay(1000);
}
