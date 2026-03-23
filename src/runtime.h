// Top-level firmware orchestrator.
//
// This module glues the subsystems together into the device's two main paths:
// normal low-power sampling and USB service-mode diagnostics.

#pragma once

#include "app_context.h"

// Performs one-time startup initialization and runs the first boot path.
void setupApp();

// Advances the currently active runtime path from the Arduino main loop.
void loopApp();

// Prints a one-line summary of boot/runtime/USB/sensor/Wi-Fi state.
void printRuntimeModeStatus();

// Runs one manual sample in USB service mode. When `uploadRequested` is true,
// the reading is also posted using the normal telemetry path.
bool performManualSample(bool uploadRequested);
