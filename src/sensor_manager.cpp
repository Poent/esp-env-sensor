// Sensor-specific initialization, sampling, and recovery behavior.
//
// The runtime asks this module for validated readings; this module handles the
// details of BME680 configuration, plausibility checks, and staged I2C/BME
// recovery when measurements look wrong.

#include "sensor_manager.h"

#include <Adafruit_BME680.h>
#include <Wire.h>
#include <core_logic.h>

#include "hardware.h"
#include "telemetry.h"

namespace {

// Single shared BME680 instance for the firmware.
Adafruit_BME680 gBme;

// Applies a fixed calibration offset after the BME680 has produced a reading.
float applyTemperatureCompensation(float rawTemperatureC) {
  return rawTemperatureC + static_cast<float>(BME_TEMPERATURE_OFFSET_C);
}

// Configures the BME680 for this project's low-power temperature/humidity/
// pressure use case and disables unused gas measurements.
void bmeConfigure() {
  Wire.setClock(100000);
  Wire.setTimeOut(25);
  gBme.setTemperatureOversampling(BME680_OS_8X);
  gBme.setHumidityOversampling(BME680_OS_2X);
  gBme.setPressureOversampling(BME680_OS_4X);
  gBme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  gBme.setGasHeater(0, 0);
}

// Writes the BME680 soft-reset command to either supported address.
bool bmeSoftReset() {
  uint8_t addrs[2] = {0x76, 0x77};
  bool wrote = false;
  for (uint8_t address : addrs) {
    Wire.beginTransmission(address);
    Wire.write(0xE0);
    Wire.write(0xB6);
    if (Wire.endTransmission() == 0) {
      wrote = true;
    }
  }
  delay(5);
  return wrote;
}

// Attempts to release a stuck I2C bus by manually pulsing SCL and issuing a
// stop condition before the next sensor re-init.
bool i2cClearBus() {
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  gApp.lastI2cClearRequired =
      (digitalRead(I2C_SDA_PIN) == LOW) || (digitalRead(I2C_SCL_PIN) == LOW);

  pinMode(I2C_SDA_PIN, OUTPUT_OPEN_DRAIN);
  pinMode(I2C_SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA_PIN, HIGH);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(5);

  if (!gApp.lastI2cClearRequired && digitalRead(I2C_SDA_PIN) == HIGH &&
      digitalRead(I2C_SCL_PIN) == HIGH) {
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    pinMode(I2C_SCL_PIN, INPUT_PULLUP);
    return true;
  }

  for (int i = 0; i < 9; ++i) {
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
  }

  digitalWrite(I2C_SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(I2C_SDA_PIN, HIGH);
  delayMicroseconds(5);

  bool clear = digitalRead(I2C_SDA_PIN) == HIGH;
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  return clear;
}

// Checks whether a specific I2C address acknowledges on the current bus.
bool isI2cAddressResponsive(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// Reads one 8-bit register from an I2C device for chip-ID probing.
bool readI2cRegister8(uint8_t address, uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(address), 1) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

// Prints every responsive I2C address to help debug missing or miswired sensors.
void logI2cProbeResults() {
  bool foundAny = false;
  for (uint8_t address = 0x08; address <= 0x77; ++address) {
    if (!isI2cAddressResponsive(address)) {
      continue;
    }
    foundAny = true;
    Serial.printf("I2C probe: found device at 0x%02X\n", address);
  }
  if (!foundAny) {
    Serial.println("I2C probe: no devices responded on the bus.");
  }
}

// Probes the expected BME addresses and prints human-readable hints based on
// the returned chip ID.
void logBmeDetectionHints() {
  uint8_t addrs[2] = {0x76, 0x77};
  bool foundCandidate = false;
  for (uint8_t address : addrs) {
    if (!isI2cAddressResponsive(address)) {
      continue;
    }
    foundCandidate = true;
    uint8_t chipId = 0;
    if (!readI2cRegister8(address, 0xD0, chipId)) {
      Serial.printf("BME probe: device acknowledged at 0x%02X but chip ID read failed.\n",
                    address);
      continue;
    }

    Serial.printf("BME probe: address 0x%02X reports chip ID 0x%02X\n", address,
                  chipId);
    if (chipId == 0x61) {
      Serial.println(
          "BME probe: this looks like a BME680, so init failure is likely power, timing, or bus integrity.");
    } else if (chipId == 0x60) {
      Serial.println(
          "BME probe: this looks like a BME280. This firmware expects a BME680 library/device.");
    } else if (chipId == 0x58) {
      Serial.println(
          "BME probe: this looks like a BMP280. It will not provide humidity and will not init as a BME680.");
    } else {
      Serial.println("BME probe: unexpected chip ID. Verify the sensor module and wiring.");
    }
  }

  if (!foundCandidate) {
    Serial.println("BME probe: no response at 0x76 or 0x77.");
  }
}

// Rebuilds the I2C/BME state after a fault by resetting the bus and then
// trying both supported BME680 addresses again.
bool bmeReinit() {
  #if !defined(ARDUINO_ARCH_AVR)
  Wire.end();
  #endif
  delay(2);
  if (!i2cClearBus()) {
    Serial.println("I2C bus clear failed");
    return false;
  }
  if (gApp.lastI2cClearRequired) {
    postEvent("i2c_bus_clear", "warning", "cleared I2C bus before reinit");
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(25);

  bool ok = false;
  if (gBme.begin(0x76)) {
    gApp.bmeAddress = 0x76;
    ok = true;
  } else if (gBme.begin(0x77)) {
    gApp.bmeAddress = 0x77;
    ok = true;
  }
  gApp.bmeInitialized = ok;
  if (ok) {
    bmeConfigure();
  }
  return ok;
}

// Requests one forced BME680 reading and copies the result into `out`.
bool takeReading(SensorReadings& out) {
  if (!gBme.performReading()) {
    return false;
  }

  out.temperature = applyTemperatureCompensation(gBme.temperature);
  out.humidity = gBme.humidity;
  out.pressure = gBme.pressure / 100.0f;
  return !(isnan(out.temperature) || isnan(out.humidity) || isnan(out.pressure));
}

// Wraps the pure plausibility helper so sensor readings can be compared against
// an optional last-known-good sample.
bool plausible(const SensorReadings& readings, const SensorReadings* lastReadings) {
  envnode::core::LogicReadings current{readings.temperature, readings.humidity,
                                       readings.pressure};
  envnode::core::LogicReadings last;
  if (lastReadings) {
    last.temperature = lastReadings->temperature;
    last.humidity = lastReadings->humidity;
    last.pressure = lastReadings->pressure;
  }
  return envnode::core::PlausibleReadings(current, lastReadings ? &last : nullptr);
}

// Attempts up to three sensor readings before giving up on the current cycle.
// `lastKnownGood` is optional and is only used for jump detection.
bool tryTakePlausibleReading(SensorReadings& reading,
                             const SensorReadings* lastKnownGood) {
  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (takeReading(reading) && plausible(reading, lastKnownGood)) {
      return true;
    }
    if (attempt == 1) {
      postEvent("implausible_reading", "warning", "plausibility failed", &reading,
                nullptr, attempt, false);
    }
    delay(10);
  }
  return false;
}

// Runs the staged recovery flow after a bad reading: soft reset, BME reinit,
// and finally a full I2C restart before the sample is dropped.
bool attemptRecoverySequence(SensorReadings& reading,
                             const SensorReadings* lastKnownGood) {
  Serial.println("Reading implausible -> recovery sequence...");

  if (!gApp.inErrorState) {
    gApp.inErrorState = true;
    sendWebhook("sensor_error",
                "Device entering error state - attempting recovery",
                "error",
                &reading);
  }

  postEvent("soft_reset", "warning", "attempting BME soft reset");
  bool softOk = bmeSoftReset();
  postEvent("soft_reset_result",
            softOk ? "info" : "error",
            softOk ? "soft reset write OK" : "soft reset write FAILED",
            nullptr,
            "soft_reset",
            1,
            softOk);

  bool reinitOk = false;
  if (softOk) {
    postEvent("reinit", "warning", "reinit after soft reset", nullptr, "reinit", 1,
              false);
    reinitOk = bmeReinit();
    postEvent("reinit_result",
              reinitOk ? "info" : "error",
              reinitOk ? "bme reinit ok" : "bme reinit failed",
              nullptr,
              "reinit",
              1,
              reinitOk);
  }

  if (!reinitOk) {
    postEvent("i2c_restart", "warning", "restarting I2C + reinit", nullptr,
              "i2c_restart", 1, false);
    reinitOk = bmeReinit();
    postEvent("i2c_restart_result",
              reinitOk ? "info" : "error",
              reinitOk ? "I2C restart ok" : "I2C restart failed",
              nullptr,
              "i2c_restart",
              1,
              reinitOk);
  }

  if (reinitOk && takeReading(reading) && plausible(reading, lastKnownGood)) {
    postEvent("recovery_ok", "info", "reading ok after recovery", &reading, nullptr,
              0, true);
    if (gApp.inErrorState) {
      gApp.inErrorState = false;
      sendWebhook("sensor_recovered",
                  "Device successfully recovered from error state",
                  "info",
                  &reading);
    }
    return true;
  }

  postEvent("recovery_failed", "error", "dropping bad reading after recovery",
            nullptr, nullptr, 0, false);
  sendWebhook("recovery_failed",
              "Device failed to recover - dropping reading",
              "error");
  return false;
}

}  // namespace

// Clears sensor-ready state after power transitions or hard failures.
void resetSensorState() {
  gApp.bmeInitialized = false;
  gApp.bmeAddress = 0;
}

// Initializes the BME680 and emits detailed probe hints if the sensor is not
// found at either supported address.
bool initBME() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(25);

  bool ok = false;
  if (gBme.begin(0x76)) {
    gApp.bmeAddress = 0x76;
    ok = true;
  } else if (gBme.begin(0x77)) {
    gApp.bmeAddress = 0x77;
    ok = true;
  }

  gApp.bmeInitialized = ok;
  if (ok) {
    bmeConfigure();
    Serial.printf("BME680 ready at I2C address 0x%02X\n", gApp.bmeAddress);
  } else {
    Serial.println("BME680 not found (0x76/0x77). Check SDA on pin 9 and SCL on pin 10.");
    logBmeDetectionHints();
    logI2cProbeResults();
  }
  return ok;
}

// Captures one accepted reading, escalating into the recovery flow if the first
// attempts fail plausibility checks.
bool captureValidatedReading(SensorReadings& reading,
                             const SensorReadings* lastKnownGood) {
  bool ok = tryTakePlausibleReading(reading, lastKnownGood);
  if (!ok) {
    ok = attemptRecoverySequence(reading, lastKnownGood);
  }
  return ok;
}
