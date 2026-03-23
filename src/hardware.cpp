// Board-level helpers for power, LEDs, ADC, USB-host detection, and sleep.
//
// These functions are intentionally small and direct because they sit at the
// boundary between the firmware state machine and the physical board behavior.

#include "hardware.h"

#include <core_logic.h>

#include "wifi_manager.h"

// Drives the sensor power-control transistor low during boot.
void initSensePower() {
  pinMode(SENSE_EN_PIN, OUTPUT);
  digitalWrite(SENSE_EN_PIN, LOW);
  gApp.sensePowerEnabled = false;
}

// Enables the switched sensor rail only when a sampling path needs it.
void enableSensePower() {
  if (gApp.sensePowerEnabled) {
    return;
  }

  digitalWrite(SENSE_EN_PIN, HIGH);
  gApp.sensePowerEnabled = true;
}

// Cuts power to the sensor rail and invalidates any cached sensor state that
// depended on that rail remaining powered.
void disableSensePower() {
  if (!gApp.sensePowerEnabled) {
    return;
  }

  digitalWrite(SENSE_EN_PIN, LOW);
  gApp.sensePowerEnabled = false;
  gApp.bmeInitialized = false;
  gApp.bmeAddress = 0;
}

// Oversamples the ADC so battery readings are less noisy before they are used
// for alerts and telemetry.
float readBatteryVoltage() {
  long sum = 0;
  for (int i = 0; i < VBAT_OVERSAMPLE; ++i) {
    sum += analogRead(VBAT_ADC_PIN);
  }

  float pinVoltage = (static_cast<float>(sum) / VBAT_OVERSAMPLE) *
                     (VBAT_ADC_REF_V / ((1 << VBAT_ADC_BITS) - 1));
  return pinVoltage * VBAT_DIVIDER_SCALE;
}

// Converts the measured battery voltage into a coarse charge percentage.
float batteryVoltageToPercent(float voltage) {
  return envnode::core::BatteryVoltageToPercent(voltage, LIPO_MIN_V, LIPO_MAX_V);
}

// Prepares the status LED for runtime feedback if the board exposes one.
void initStatusLed() {
  if (!STATUS_LED_AVAILABLE) {
    return;
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, STATUS_LED_OFF_LEVEL);
}

// Mirrors the runtime awake/asleep state to the board status LED.
void setAwakeLed(bool on) {
  if (!STATUS_LED_AVAILABLE) {
    return;
  }

  digitalWrite(STATUS_LED_PIN, on ? STATUS_LED_ON_LEVEL : STATUS_LED_OFF_LEVEL);
}

// Provides a simple visual indication that a cold boot completed successfully.
void blinkColdBootSuccessLed() {
  if (!STATUS_LED_AVAILABLE || gApp.bootMode != BootMode::ColdBoot) {
    return;
  }

  for (int i = 0; i < BOOT_LED_BLINK_COUNT; ++i) {
    digitalWrite(STATUS_LED_PIN, STATUS_LED_ON_LEVEL);
    delay(BOOT_LED_BLINK_ON_MS);
    digitalWrite(STATUS_LED_PIN, STATUS_LED_OFF_LEVEL);
    if (i + 1 < BOOT_LED_BLINK_COUNT) {
      delay(BOOT_LED_BLINK_OFF_MS);
    }
  }

  setAwakeLed(true);
}

// Uses the native USB CDC/JTAG helper to detect a host connection on boards
// that support it.
bool isUsbHostAttached() {
  #if defined(ARDUINO_USB_MODE) && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  return HWCDC::isPlugged();
  #else
  return false;
  #endif
}

// Gives the powered sensor rail time to stabilize before the first I2C access.
void waitForSensorPowerRail() {
  if (SENSOR_POWER_SETTLE_MS == 0) {
    return;
  }

  Serial.printf("Sensor power settle: waiting %lu ms before BME init.\n",
                static_cast<unsigned long>(SENSOR_POWER_SETTLE_MS));
  delay(SENSOR_POWER_SETTLE_MS);
}

// Applies the firmware's sleep policy, including the rule that only
// startup sensor/bootstrap faults can block sleep for diagnostics.
void enterDeepSleep() {
  #if DISABLE_DEEP_SLEEP
  setAwakeLed(true);
  Serial.printf("Deep sleep disabled; WiFi status=%d. Staying awake.\n",
                static_cast<int>(WiFi.status()));
  Serial.flush();
  return;
  #endif

  if (gApp.holdAwakeForDiagnostics) {
    setAwakeLed(true);
    Serial.println("Deep sleep blocked because a startup sensor/bootstrap fault requires diagnostics.");
    Serial.flush();
    return;
  }

  if (DEBUG_AWAKE_WINDOW_MS > 0) {
    Serial.printf("Debug awake window: holding for %lu ms before sleep.\n",
                  static_cast<unsigned long>(DEBUG_AWAKE_WINDOW_MS));
    Serial.flush();
    delay(DEBUG_AWAKE_WINDOW_MS);
  }

  setAwakeLed(false);
  disableSensePower();
  shutdownWiFi();
  esp_sleep_enable_timer_wakeup(
      static_cast<uint64_t>(gApp.sampleIntervalSeconds) * 1000000ULL);
  Serial.printf("Sleeping for %lu seconds...\n",
                static_cast<unsigned long>(gApp.sampleIntervalSeconds));
  Serial.flush();
  esp_deep_sleep_start();
}
