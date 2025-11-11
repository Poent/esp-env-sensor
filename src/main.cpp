#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include "secrets.h"

#ifdef SUPABASE_ANON_KEY
  #error "SUPABASE_ANON_KEY has been replaced by SUPABASE_API_KEY. Update include/secrets.h to use SUPABASE_API_KEY."
#endif

#ifndef SUPABASE_API_KEY
  #error "SUPABASE_API_KEY must be defined in include/secrets.h"
#endif

// ========= Config =========
#ifndef SUPABASE_EVENTS_TABLE
  #define SUPABASE_EVENTS_TABLE "device_events"
#endif
#define FW_VERSION "envnode-1.1.0"

// ========= Sensor & timing =========
Adafruit_BME280 bme;                   // I2C
constexpr unsigned long SEND_EVERY_MS = 60000; // 1 minute
unsigned long lastSend = 0;

bool gLastI2cClearRequired = false;


// Data model
struct SensorReadings {
  float temperature;
  float humidity;
  float pressure; // hPa
};
SensorReadings lastGood = {NAN, NAN, NAN};

// Error state tracking for webhook alerts
bool gInErrorState = false;
unsigned long gLastWebhookSent = 0;
constexpr unsigned long WEBHOOK_COOLDOWN_MS = 1000; // 5 minutes minimum between webhooks

// ========= Debug mode =========
// Set to true to send test webhooks at startup, false for normal operation
constexpr bool DEBUG_WEBHOOKS = true;

// Session correlation for logs
String gSessionId;

// ================= Network helpers =================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);   // note: "WiFi_SSID" / "WiFi_PASS" or "WIFI_SSID" / "WIFI_PASS" —
                                      // mirror your secrets.h symbols (adjust if needed)
  Serial.print("WiFi: connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
}

String jsonEscape(const String& s) {
  String out; out.reserve(s.length()+8);
  for (size_t i=0;i<s.length();++i) {
    char c = s[i];
    switch(c){
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) { char buf[7]; snprintf(buf,sizeof(buf),"\\u%04x",c); out += buf; }
        else out += c;
    }
  }
  return out;
}

bool supabaseInsert(const char* table, const String& payloadJson) {
  String endpoint = String(SUPABASE_URL) + "/rest/v1/" + table;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, endpoint)) {
    Serial.println("HTTP begin failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  String authHeader = String("Bearer ") + SUPABASE_API_KEY;
  https.addHeader("apikey", SUPABASE_API_KEY);
  https.addHeader("Authorization", authHeader);

  int code = https.POST(payloadJson);
  Serial.printf("POST %s -> %d\n", endpoint.c_str(), code);
  if (code < 0) Serial.printf("HTTP error: %s\n", https.errorToString(code).c_str());
  https.end();
  return code >= 200 && code < 300;
}

bool supabaseTableExists(const char* table) {
  String endpoint = String(SUPABASE_URL) + "/rest/v1/" + table + "?select=*&limit=1";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  if (!https.begin(client, endpoint)) {
    Serial.printf("Supabase table check: begin failed for %s\n", table);
    return false;
  }
  https.addHeader("Accept", "application/json");
  https.addHeader("Range-Unit", "items");
  https.addHeader("Range", "0-0");

  String authHeader = String("Bearer ") + SUPABASE_API_KEY;
  https.addHeader("apikey", SUPABASE_API_KEY);
  https.addHeader("Authorization", authHeader);

  int code = https.GET();
  if (code < 0) {
    Serial.printf("Supabase table check: HTTP error for %s -> %s\n",
                  table, https.errorToString(code).c_str());
  } else {
    Serial.printf("Supabase table check %s -> %d\n", table, code);
  }
  https.end();
  return code >= 200 && code < 300;
}

void waitForSupabaseTable(const char* table) {
  constexpr unsigned long RETRY_DELAY_MS = 15000;
  while (!supabaseTableExists(table)) {
    Serial.printf("Supabase table '%s' not ready. Waiting %lu ms before retry...\n",
                  table, RETRY_DELAY_MS);
    delay(RETRY_DELAY_MS);
  }
}

bool postReadingRow(float tC, float h, float p_hPa) {
  String payload = String("{\"device_id\":\"") + DEVICE_ID +
                   "\",\"temperature_c\":" + String(tC, 2) +
                   ",\"humidity_rh\":"   + String(h, 2) +
                   ",\"pressure_hpa\":"  + String(p_hPa, 2) + "}";
  return supabaseInsert(SUPABASE_TABLE, payload);
}

// ================= Event logging =================
bool postEvent(const char* event_type,
               const char* severity,
               const String& message,
               const SensorReadings* snap = nullptr,
               const char* action = nullptr,
               int attempt = 0,
               bool action_success = false,
               const char* meta_json = nullptr)
{
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
  if (gSessionId.length()) payload += ",\"session_id\":\"" + gSessionId + "\"";
  payload += ",\"event_type\":\"" + String(event_type) + "\"";
  payload += ",\"severity\":\"" + String(severity) + "\"";
  if (message.length()) payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  if (snap) {
    payload += ",\"reading_temp_c\":"     + String(snap->temperature, 2);
    payload += ",\"reading_humidity_rh\":"+ String(snap->humidity, 2);
    payload += ",\"reading_pressure_hpa\":"+ String(snap->pressure, 2);
  }
  if (action)  payload += ",\"action\":\"" + String(action) + "\"";
  if (attempt) payload += ",\"attempt\":" + String(attempt);
  payload += ",\"action_success\":" + String(action_success ? "true" : "false");
  if (meta_json && meta_json[0]) payload += ",\"meta\":" + String(meta_json);
  payload += "}";

  bool ok = supabaseInsert(SUPABASE_EVENTS_TABLE, payload);
  Serial.printf("EVENT[%s/%s]: %s\n", event_type, severity, ok ? "logged" : "log failed");
  return ok;
}

bool sendWebhook(const char* alert_type, 
                 const String& message, 
                 const char* severity = "info",
                 const SensorReadings* readings = nullptr,
                 const char* extra_data = nullptr) {
  // Cooldown check to prevent spam (except for info/success messages)
  unsigned long now = millis();
  bool isError = (strcmp(severity, "error") == 0 || strcmp(severity, "warning") == 0);
  if (isError && (now - gLastWebhookSent < WEBHOOK_COOLDOWN_MS)) {
    Serial.printf("Webhook: skipping (cooldown active, %lu ms remaining)\n", 
                  WEBHOOK_COOLDOWN_MS - (now - gLastWebhookSent));
    return false;
  }

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
  payload += ",\"alert_type\":\"" + String(alert_type) + "\"";
  payload += ",\"severity\":\"" + String(severity) + "\"";
  payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  payload += ",\"timestamp\":" + String(millis());
  payload += ",\"fw_version\":\"" + String(FW_VERSION) + "\"";
  
  // Add sensor readings if provided
  if (readings && !isnan(readings->temperature)) {
    payload += ",\"readings\":{";
    payload += "\"temperature_c\":" + String(readings->temperature, 2);
    payload += ",\"humidity_rh\":" + String(readings->humidity, 2);
    payload += ",\"pressure_hpa\":" + String(readings->pressure, 2);
    payload += "}";
  }
  
  // Add extra data if provided
  if (extra_data && extra_data[0]) {
    payload += ",\"extra\":" + String(extra_data);
  }
  
  payload += "}";

  WiFiClient client;
  HTTPClient http;
  
  if (!http.begin(client, N8N_WEBHOOK_URL)) {
    Serial.println("Webhook: HTTP begin failed");
    return false;
  }
  
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  
  Serial.printf("Webhook POST [%s/%s] -> %d\n", alert_type, severity, code);
  if (code < 0) {
    Serial.printf("Webhook error: %s\n", http.errorToString(code).c_str());
  }
  
  http.end();
  
  bool ok = (code >= 200 && code < 300);
  if (ok) {
    gLastWebhookSent = now;
  }
  return ok;
}

void testWebhooks() {
  Serial.println("\n=== WEBHOOK DEBUG MODE ACTIVE ===");
  Serial.println("Sending test webhooks (one of each type)...");
  
  // Create some sample readings for testing
  SensorReadings testReadings = {23.5, 45.0, 1013.25};
  String testExtra = "{\"test_mode\":true,\"ip_address\":\"" + WiFi.localIP().toString() + "\"}";
  
  // Test 1: Info - Device Startup
  Serial.println("\n[1/4] Sending INFO webhook (device_startup)...");
  sendWebhook("device_startup", "TEST: Device startup message", "info", &testReadings, testExtra.c_str());
  delay(10000);
  
  // Test 2: Warning - Sensor Error
  Serial.println("\n[2/4] Sending WARNING webhook (sensor_error)...");
  sendWebhook("sensor_error", "TEST: Sensor error warning", "warning", &testReadings);
  delay(10000);
  
  // Test 3: Error - Recovery Failed
  Serial.println("\n[3/4] Sending ERROR webhook (recovery_failed)...");
  sendWebhook("recovery_failed", "TEST: Recovery failed error", "error");
  delay(10000);
  
  // Test 4: Info - Recovery Success
  Serial.println("\n[4/4] Sending INFO webhook (sensor_recovered)...");
  sendWebhook("sensor_recovered", "TEST: Sensor recovered successfully", "info", &testReadings);
  
  Serial.println("\n=== WEBHOOK DEBUG MODE COMPLETE ===");
  Serial.println("Set DEBUG_WEBHOOKS = false to disable test mode\n");
}

// ================= BME robustness =================
void bmeConfigure() {
  Wire.setClock(100000);     // 100 kHz for robustness
  Wire.setTimeOut(25);       // ms
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X2,   // temp
                  Adafruit_BME280::SAMPLING_X4,   // pressure
                  Adafruit_BME280::SAMPLING_X2,   // humidity
                  Adafruit_BME280::FILTER_X16,
                  Adafruit_BME280::STANDBY_MS_10);
}

bool bmeSoftReset() {
  uint8_t addrs[2] = {0x76, 0x77};
  bool wrote = false;
  for (uint8_t a : addrs) {
    Wire.beginTransmission(a);
    Wire.write(0xE0); Wire.write(0xB6);
    if (Wire.endTransmission() == 0) wrote = true;
  }
  delay(5);
  return wrote;
}

bool i2cClearBus() {
  constexpr int SDA_PIN = 21;
  constexpr int SCL_PIN = 22;

  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  gLastI2cClearRequired = (digitalRead(SDA_PIN) == LOW) || (digitalRead(SCL_PIN) == LOW);

  pinMode(SDA_PIN, OUTPUT_OPEN_DRAIN);
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(SDA_PIN, HIGH);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(5);

  if (!gLastI2cClearRequired && digitalRead(SDA_PIN) == HIGH && digitalRead(SCL_PIN) == HIGH) {
    pinMode(SDA_PIN, INPUT_PULLUP);
    pinMode(SCL_PIN, INPUT_PULLUP);
    return true;
  }

  for (int i = 0; i < 9; ++i) {
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(5);
  }

  digitalWrite(SDA_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(SDA_PIN, HIGH);
  delayMicroseconds(5);

  bool clear = (digitalRead(SDA_PIN) == HIGH);

  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);

  return clear;
}

bool bmeReinit() {
  #if !defined(ARDUINO_ARCH_AVR)
  Wire.end();
  #endif
  delay(2);
  if (!i2cClearBus()) {
    Serial.println("I2C bus clear failed");
    return false;
  }
  if (gLastI2cClearRequired) {
    postEvent("i2c_bus_clear", "warning", "cleared I2C bus before reinit");
  }
  Wire.begin(21, 22);
  Wire.setClock(100000);
  Wire.setTimeOut(25);
  bool ok = bme.begin(0x76) || bme.begin(0x77);
  if (ok) bmeConfigure();
  return ok;
}

bool takeReading(SensorReadings& out) {
  (void)bme.takeForcedMeasurement(); // some cores return void
  out.temperature = bme.readTemperature();
  out.humidity    = bme.readHumidity();
  out.pressure    = bme.readPressure() / 100.0f; // Pa -> hPa
  return !(isnan(out.temperature) || isnan(out.humidity) || isnan(out.pressure));
}

bool plausible(const SensorReadings& r, const SensorReadings* last) {
  if (!(r.temperature > -40 && r.temperature < 85)) return false;
  if (!(r.humidity    >= 0   && r.humidity    <= 100)) return false;
  if (!(r.pressure    >  300 && r.pressure    < 1100)) return false;
  if (last && !isnan(last->temperature)) {
    if (fabs(r.temperature - last->temperature) > 5.0)  return false;
    if (fabs(r.humidity    - last->humidity)    > 15.0) return false;
    if (fabs(r.pressure    - last->pressure)    > 10.0) return false;
  }
  return true;
}

// Attempts up to three forced BME280 readings and validates each against an optional
// last-known-good snapshot. Inputs: `reading` is populated in-place; `last` can be
// null to disable delta checks. Side effects: delays between attempts and posts an
// implausible_reading warning event on the first failure.
bool tryTakePlausibleReading(SensorReadings& reading, const SensorReadings* last) {
  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (takeReading(reading) && plausible(reading, last)) {
      return true;
    }
    if (attempt == 1) {
      postEvent("implausible_reading", "warning", "plausibility failed", &reading, nullptr, attempt, false);
    }
    delay(10);
  }
  return false;
}

// Executes the staged recovery flow (soft reset, reinit, I2C restart) when
// plausibility fails. Inputs: `reading` holds the latest attempt and is updated
// with any post-recovery measurement; `last` allows plausibility deltas post-reset.
// Side effects: serial logging, multiple recovery events, sensor reset attempts.
bool attemptRecoverySequence(SensorReadings& reading, const SensorReadings* last) {
  Serial.println("Reading implausible -> recovery sequence…");

  // Send webhook alert when entering error state (only once)
  if (!gInErrorState) {
    gInErrorState = true;
    sendWebhook("sensor_error", "Device entering error state - attempting recovery", "error", &reading);
  }

  postEvent("soft_reset", "warning", "attempting BME soft reset");
  bool softOk = bmeSoftReset();
  postEvent("soft_reset_result", softOk ? "info" : "error",
            softOk ? "soft reset write OK" : "soft reset write FAILED",
            nullptr, "soft_reset", 1, softOk);

  bool reinitOk = false;
  if (softOk) {
    postEvent("reinit", "warning", "reinit after soft reset", nullptr, "reinit", 1, false);
    reinitOk = bmeReinit();
    postEvent("reinit_result", reinitOk ? "info" : "error",
              reinitOk ? "bme reinit ok" : "bme reinit failed",
              nullptr, "reinit", 1, reinitOk);
  }

  if (!reinitOk) {
    postEvent("i2c_restart", "warning", "restarting I2C + reinit", nullptr, "i2c_restart", 1, false);
    reinitOk = bmeReinit();
    postEvent("i2c_restart_result", reinitOk ? "info" : "error",
              reinitOk ? "I2C restart ok" : "I2C restart failed",
              nullptr, "i2c_restart", 1, reinitOk);
  }

  if (reinitOk && takeReading(reading) && plausible(reading, last)) {
    postEvent("recovery_ok", "info", "reading ok after recovery", &reading, nullptr, 0, true);
    // Send recovery success webhook
    if (gInErrorState) {
      gInErrorState = false;
      sendWebhook("sensor_recovered", "Device successfully recovered from error state", "info", &reading);
    }
    return true;
  }

  postEvent("recovery_failed", "error", "dropping bad reading after recovery", nullptr, nullptr, 0, false);
  // Send recovery failure webhook (respects cooldown)
  sendWebhook("recovery_failed", "Device failed to recover - dropping reading", "error");
  return false;
}

// ================= App logic =================
void postReadings(const SensorReadings& readings) {
  if (!isnan(readings.temperature) && !isnan(readings.humidity) && !isnan(readings.pressure)) {
    bool ok = postReadingRow(readings.temperature, readings.humidity, readings.pressure);
    Serial.println(ok ? "Upload ok" : "Upload failed");
  } else {
    Serial.println("Sensor returned NaN (is your board BME280, not BMP280?).");
  }
}

bool initBME() {
  Wire.begin(21, 22);
  Wire.setClock(100000);
  Wire.setTimeOut(25);
  bool ok = (bme.begin(0x76) || bme.begin(0x77));
  if (ok) {
    bmeConfigure();
    uint8_t id = bme.sensorID();
    Serial.printf("BME sensor ID: 0x%02X\n", id);
  } else {
    Serial.println("BME280 not found (0x76/0x77). Is it wired? Is it a BMP280?");
  }
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBooting...");

  connectWiFi();

  Serial.println("Checking Supabase tables...");
  waitForSupabaseTable(SUPABASE_TABLE);
  if (String(SUPABASE_EVENTS_TABLE) != String(SUPABASE_TABLE)) {
    waitForSupabaseTable(SUPABASE_EVENTS_TABLE);
  }
  Serial.println("Supabase tables ready.");

  // Build a session ID (mac + random) for correlating events
  uint64_t mac = ESP.getEfuseMac();
  gSessionId = String((uint32_t)(mac >> 32), HEX) + String((uint32_t)mac, HEX) + "-" + String((uint32_t)esp_random(), HEX);

  if (!initBME()) {
    postEvent("startup", "error", "BME init failed", nullptr, nullptr, 0, false,
              "{\"fw\":\"" FW_VERSION "\"}");
    Serial.println("Halting: BME280 init failed.");
    while (true) delay(1000);
  }

  // Log startup
  String meta = String("{\"fw\":\"") + FW_VERSION + "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  postEvent("startup", "info", "device boot", nullptr, nullptr, 0, true, meta.c_str());

  // First read
  SensorReadings r;
  if (takeReading(r) && plausible(r, nullptr)) {
    lastGood = r;
    Serial.printf("GOOD: T=%.2f°C RH=%.1f%% P=%.1f hPa\n", r.temperature, r.humidity, r.pressure);
    postReadings(r);
    
    // Send startup webhook with boot info and first reading
    String bootInfo = String("{\"ip_address\":\"") + WiFi.localIP().toString() + 
                      "\",\"mac_address\":\"" + WiFi.macAddress() + 
                      "\",\"session_id\":\"" + gSessionId + 
                      "\",\"rssi_dbm\":" + String(WiFi.RSSI()) + "}";
    sendWebhook("device_startup", "Device booted successfully", "info", &r, bootInfo.c_str());
  } else {
    Serial.println("Initial reading implausible, will try again in loop.");
    postEvent("implausible_reading", "warning", "initial reading failed plausibility", &r);
    
    // Send startup webhook even if first reading failed
    String bootInfo = String("{\"ip_address\":\"") + WiFi.localIP().toString() + 
                      "\",\"mac_address\":\"" + WiFi.macAddress() + 
                      "\",\"session_id\":\"" + gSessionId + 
                      "\",\"rssi_dbm\":" + String(WiFi.RSSI()) + 
                      "\",\"first_reading_failed\":true}";
    sendWebhook("device_startup", "Device booted but first reading failed", "warning", nullptr, bootInfo.c_str());
  }
  
  // Debug mode: test all webhook types
  if (DEBUG_WEBHOOKS) {
    testWebhooks();
  }
}

/**
 * Main periodic loop: wakes each minute to capture sensor data, validates the
 * reading, and uploads to Supabase. Failed plausibility checks trigger a
 * staged recovery routine (soft reset, reinit, full I2C restart) before the
 * sample is ultimately accepted or dropped.
 */
void loop() {
  unsigned long now = millis();
  if (now - lastSend >= SEND_EVERY_MS) {
    lastSend = now;

    SensorReadings r;
    bool ok = false;

    const SensorReadings* lastKnownGood = isnan(lastGood.temperature) ? nullptr : &lastGood;
    ok = tryTakePlausibleReading(r, lastKnownGood);

    if (!ok) {
      ok = attemptRecoverySequence(r, lastKnownGood);
    }

    if (ok) {
      lastGood = r;
      Serial.printf("GOOD: T=%.2f°C RH=%.1f%% P=%.1f hPa\n", r.temperature, r.humidity, r.pressure);
      postReadings(r);
      // Clear error state if we got a good reading without recovery
      if (gInErrorState) {
        gInErrorState = false;
        sendWebhook("sensor_recovered", "Device recovered - normal operation resumed", "info", &r);
      }
    } else {
      Serial.println("Dropping bad reading after recovery attempts.");
    }
  }
  delay(50);
}
