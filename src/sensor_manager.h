// BME680 setup, sampling, plausibility checks, and recovery helpers.
//
// This module owns sensor-specific behavior so the runtime only has to request
// "initialize" and "capture a validated reading" rather than orchestrating I2C
// recovery details itself.

#pragma once

#include "app_context.h"

// Clears the cached sensor-ready state after power transitions or failures.
void resetSensorState();

// Initializes the BME680 on the configured I2C bus and applies sampling config.
bool initBME();

// Captures one reading and only returns success after plausibility and recovery
// checks have passed. `lastKnownGood` may be `nullptr` to skip delta checks.
bool captureValidatedReading(SensorReadings& reading,
                             const SensorReadings* lastKnownGood);
