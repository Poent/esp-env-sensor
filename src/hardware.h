// Hardware-facing helpers for LEDs, power gating, battery ADC, and deep sleep.
//
// Keeping these small board-level operations together makes it easier to change
// wiring or low-power behavior without touching the higher-level runtime logic.

#pragma once

#include "app_context.h"

// Initializes the sensor power-control pin into a known-off state.
void initSensePower();

// Enables the switched sensor power rail if it is currently off.
void enableSensePower();

// Disables the switched sensor power rail and clears cached sensor state.
void disableSensePower();

// Reads the battery divider through the ADC and returns pack voltage in volts.
float readBatteryVoltage();

// Converts battery voltage into a coarse 0-100% estimate.
float batteryVoltageToPercent(float voltage);

// Configures the board status LED, if one is available on the target board.
void initStatusLed();

// Sets the awake-status LED to the requested logical state.
void setAwakeLed(bool on);

// Blinks the status LED after a successful cold boot.
void blinkColdBootSuccessLed();

// Detects whether a USB host is currently attached to the native USB port.
bool isUsbHostAttached();

// Waits for the powered sensor rail to settle before I2C access begins.
void waitForSensorPowerRail();

// Transitions the device into deep sleep unless diagnostics are intentionally
// keeping it awake.
void enterDeepSleep();
