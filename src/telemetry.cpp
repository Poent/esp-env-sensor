// HTTP/TLS telemetry implementation.
//
// This module centralizes request construction, TLS verification, webhook
// cooldown rules, and the JSON payload formats used by Supabase and external
// webhooks.

#include "telemetry.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <core_logic.h>

#include "hardware.h"

namespace {

// Returns true when the supplied URL uses HTTPS and therefore needs TLS setup.
bool isHttpsUrl(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

// Checks whether `value` begins with `prefix` without allocating temporary
// strings.
bool urlStartsWith(const char* value, const char* prefix) {
  return value && prefix && prefix[0] &&
         strncmp(value, prefix, strlen(prefix)) == 0;
}

// Escapes arbitrary text for safe embedding inside JSON string values.
String jsonEscape(const String& input) {
  return String(envnode::core::JsonEscape(input.c_str()).c_str());
}

// Chooses the configured root CA for a target URL so HTTPS verification uses
// the right trust anchor.
const char* rootCaForUrl(const char* url) {
  if (urlStartsWith(url, SUPABASE_URL)) {
    return SUPABASE_ROOT_CA_PEM;
  }
  if (url && N8N_WEBHOOK_URL[0] && strcmp(url, N8N_WEBHOOK_URL) == 0) {
    return N8N_WEBHOOK_ROOT_CA_PEM;
  }
  if (url && DEBUG_DISCORD_WEBHOOK_URL[0] &&
      strcmp(url, DEBUG_DISCORD_WEBHOOK_URL) == 0) {
    return DEBUG_WEBHOOK_ROOT_CA_PEM;
  }
  return "";
}

// Starts an HTTP or HTTPS request. HTTPS requests require a configured CA
// unless insecure fallback is explicitly allowed.
bool beginHttpRequest(HTTPClient& http,
                      WiFiClient& plainClient,
                      WiFiClientSecure& secureClient,
                      const char* url) {
  if (!isHttpsUrl(url)) {
    return http.begin(plainClient, url);
  }

  const char* rootCa = rootCaForUrl(url);
  if (rootCa && rootCa[0]) {
    secureClient.setCACert(rootCa);
    return http.begin(secureClient, url);
  }

  if (!ALLOW_INSECURE_HTTPS_REQUESTS) {
    Serial.println("HTTPS request blocked: configure a root CA certificate or enable insecure HTTPS explicitly for debug use.");
    return false;
  }

  secureClient.setInsecure();
  return http.begin(secureClient, url);
}

// Adds Cloudflare Access headers only for the protected n8n webhook endpoint.
void addWebhookAccessHeaders(HTTPClient& http, const char* url) {
  if (!url || strcmp(url, N8N_WEBHOOK_URL) != 0) {
    return;
  }

  if (N8N_CF_ACCESS_CLIENT_ID[0] && N8N_CF_ACCESS_CLIENT_SECRET[0]) {
    http.addHeader("CF-Access-Client-Id", N8N_CF_ACCESS_CLIENT_ID);
    http.addHeader("CF-Access-Client-Secret", N8N_CF_ACCESS_CLIENT_SECRET);
    return;
  }

  if (N8N_CF_ACCESS_CLIENT_ID[0] || N8N_CF_ACCESS_CLIENT_SECRET[0]) {
    Serial.println("Webhook: Cloudflare Access service token is incomplete; skipping auth headers");
  }
}

// Sends one JSON payload to a Supabase REST table endpoint.
bool supabaseInsert(const char* table, const String& payloadJson) {
  if (!gApp.networkAvailable) {
    Serial.printf("Skipping Supabase insert for %s: WiFi unavailable\n", table);
    return false;
  }

  String endpoint = String(SUPABASE_URL) + "/rest/v1/" + table;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;

  if (!beginHttpRequest(http, plainClient, secureClient, endpoint.c_str())) {
    Serial.printf("Supabase insert begin failed for %s\n", table);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  String authHeader = String("Bearer ") + SUPABASE_API_KEY;
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", authHeader);

  unsigned long startedAt = millis();
  int code = http.POST(payloadJson);
  Serial.printf("POST %s -> %d (%lu ms)\n",
                table,
                code,
                static_cast<unsigned long>(millis() - startedAt));
  if (code < 0) {
    Serial.printf("HTTP error: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  return code >= 200 && code < 300;
}

// Issues a lightweight GET against a Supabase table so startup can confirm the
// endpoint is reachable and authorized.
bool supabaseTableExists(const char* table) {
  if (!gApp.networkAvailable) {
    Serial.printf("Skipping Supabase table check for %s: WiFi unavailable\n", table);
    return false;
  }

  String endpoint = String(SUPABASE_URL) + "/rest/v1/" + table + "?select=*&limit=1";
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;

  if (!beginHttpRequest(http, plainClient, secureClient, endpoint.c_str())) {
    Serial.printf("Supabase table check: begin failed for %s\n", table);
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.addHeader("Range-Unit", "items");
  http.addHeader("Range", "0-0");
  String authHeader = String("Bearer ") + SUPABASE_API_KEY;
  http.addHeader("apikey", SUPABASE_API_KEY);
  http.addHeader("Authorization", authHeader);

  unsigned long startedAt = millis();
  int code = http.GET();
  if (code < 0) {
    Serial.printf("Supabase table check: HTTP error for %s -> %s\n",
                  table,
                  http.errorToString(code).c_str());
  } else {
    Serial.printf("Supabase table check %s -> %d (%lu ms)\n",
                  table,
                  code,
                  static_cast<unsigned long>(millis() - startedAt));
  }
  http.end();
  return code >= 200 && code < 300;
}

// Builds and posts a readings-table row. Battery fields are only included when
// a battery reading was available in the current cycle.
bool postReadingRow(float temperatureC,
                    float humidityRh,
                    float pressureHpa,
                    float batteryVoltage,
                    float batteryPercent) {
  String payload = String("{\"device_id\":\"") + DEVICE_ID +
                   "\",\"temperature_c\":" + String(temperatureC, 2) +
                   ",\"humidity_rh\":" + String(humidityRh, 2) +
                   ",\"pressure_hpa\":" + String(pressureHpa, 2);
  if (!isnan(batteryVoltage)) {
    payload += ",\"battery_voltage_v\":" + String(batteryVoltage, 3);
    payload += ",\"battery_pct\":" + String(batteryPercent, 1);
  }
  payload += "}";
  return supabaseInsert(SUPABASE_TABLE, payload);
}

}  // namespace

// Performs a one-time startup check that both configured Supabase tables are
// reachable.
bool checkSupabaseTablesOnce() {
  if (!gApp.networkAvailable) {
    return false;
  }

  Serial.println("Checking Supabase tables...");
  bool readingsOk = supabaseTableExists(SUPABASE_TABLE);
  bool eventsOk = true;
  if (String(SUPABASE_EVENTS_TABLE) != String(SUPABASE_TABLE)) {
    eventsOk = supabaseTableExists(SUPABASE_EVENTS_TABLE);
  }

  if (readingsOk && eventsOk) {
    Serial.println("Supabase tables ready.");
    return true;
  }

  Serial.println("Supabase table check failed; continuing without blocking.");
  return false;
}

// Posts one validated reading to Supabase and prints a concise serial result.
bool postReadings(const SensorReadings& readings) {
  if (isnan(readings.temperature) || isnan(readings.humidity) ||
      isnan(readings.pressure)) {
    Serial.println("Sensor returned NaN (check the BME680 wiring and power).");
    return false;
  }

  bool ok = postReadingRow(readings.temperature,
                           readings.humidity,
                           readings.pressure,
                           readings.batteryVoltage,
                           readings.batteryPercent);
  Serial.println(ok ? "Upload ok" : "Upload failed");
  return ok;
}

// Builds an event payload and writes it to the configured events table.
bool postEvent(const char* eventType,
               const char* severity,
               const String& message,
               const SensorReadings* snapshot,
               const char* action,
               int attempt,
               bool actionSuccess,
               const char* metaJson) {
  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
  if (gApp.sessionId.length()) {
    payload += ",\"session_id\":\"" + gApp.sessionId + "\"";
  }
  payload += ",\"event_type\":\"" + String(eventType) + "\"";
  payload += ",\"severity\":\"" + String(severity) + "\"";
  if (message.length()) {
    payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  }
  if (snapshot) {
    payload += ",\"reading_temp_c\":" + String(snapshot->temperature, 2);
    payload += ",\"reading_humidity_rh\":" + String(snapshot->humidity, 2);
    payload += ",\"reading_pressure_hpa\":" + String(snapshot->pressure, 2);
  }
  if (action) {
    payload += ",\"action\":\"" + String(action) + "\"";
  }
  if (attempt) {
    payload += ",\"attempt\":" + String(attempt);
  }
  payload += ",\"action_success\":" + String(actionSuccess ? "true" : "false");
  if (metaJson && metaJson[0]) {
    payload += ",\"meta\":" + String(metaJson);
  }
  payload += "}";

  bool ok = supabaseInsert(SUPABASE_EVENTS_TABLE, payload);
  Serial.printf("EVENT[%s/%s]: %s\n", eventType, severity, ok ? "logged" : "log failed");
  return ok;
}

// Sends a webhook payload with optional reading data and extra JSON metadata.
bool sendWebhook(const char* alertType,
                 const String& message,
                 const char* severity,
                 const SensorReadings* readings,
                 const char* extraData) {
  if (!gApp.networkAvailable) {
    Serial.printf("Skipping webhook %s: WiFi unavailable\n", alertType);
    return false;
  }

  unsigned long now = millis();
  bool isError = strcmp(severity, "error") == 0 || strcmp(severity, "warning") == 0;
  if (isError && now - gApp.lastWebhookSent < WEBHOOK_COOLDOWN_MS) {
    Serial.printf("Webhook: skipping (cooldown active, %lu ms remaining)\n",
                  WEBHOOK_COOLDOWN_MS - (now - gApp.lastWebhookSent));
    return false;
  }

  String payload = "{";
  payload += "\"device_id\":\"" + String(DEVICE_ID) + "\"";
  payload += ",\"alert_type\":\"" + String(alertType) + "\"";
  payload += ",\"severity\":\"" + String(severity) + "\"";
  payload += ",\"message\":\"" + jsonEscape(message) + "\"";
  payload += ",\"timestamp\":" + String(millis());
  payload += ",\"fw_version\":\"" + String(FW_VERSION) + "\"";
  if (readings && !isnan(readings->temperature)) {
    payload += ",\"readings\":{";
    payload += "\"temperature_c\":" + String(readings->temperature, 2);
    payload += ",\"humidity_rh\":" + String(readings->humidity, 2);
    payload += ",\"pressure_hpa\":" + String(readings->pressure, 2);
    if (!isnan(readings->batteryVoltage)) {
      payload += ",\"battery_voltage_v\":" + String(readings->batteryVoltage, 3);
      payload += ",\"battery_pct\":" + String(readings->batteryPercent, 1);
    }
    payload += "}";
  }
  if (extraData && extraData[0]) {
    payload += ",\"extra\":" + String(extraData);
  }
  payload += "}";

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setConnectTimeout(WEBHOOK_TIMEOUT_MS);
  http.setTimeout(WEBHOOK_TIMEOUT_MS);

  if (!beginHttpRequest(http, plainClient, secureClient, N8N_WEBHOOK_URL)) {
    Serial.println("Webhook: begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  addWebhookAccessHeaders(http, N8N_WEBHOOK_URL);
  int code = http.POST(payload);
  String responseBody;
  if (VERBOSE_HTTP_LOGGING && code > 0) {
    responseBody = http.getString();
  }

  Serial.printf("Webhook POST [%s/%s] -> %d\n", alertType, severity, code);
  if (code < 0) {
    Serial.printf("Webhook error: %s\n", http.errorToString(code).c_str());
  } else if (VERBOSE_HTTP_LOGGING && responseBody.length()) {
    Serial.printf("Webhook response body: %s\n", responseBody.c_str());
  }

  http.end();
  bool ok = code >= 200 && code < 300;
  if (ok) {
    gApp.lastWebhookSent = now;
  }
  return ok;
}

// Sends the per-cycle debug heartbeat, either as a Discord message or the
// structured webhook payload used by the rest of the project.
bool sendDebugDiscordMessage(const SensorReadings* readings,
                             bool uploadOk,
                             unsigned long cycleStartedAtMs) {
  if (!DEBUG_MODE_ENABLED) {
    return false;
  }

  if (!gApp.networkAvailable) {
    Serial.println("Skipping debug Discord message: WiFi unavailable");
    return false;
  }

  const char* debugWebhookUrl = DEBUG_DISCORD_WEBHOOK_URL[0]
                                    ? DEBUG_DISCORD_WEBHOOK_URL
                                    : N8N_WEBHOOK_URL;
  if (!debugWebhookUrl || !debugWebhookUrl[0]) {
    return false;
  }

  bool useStructuredWebhookPayload = strcmp(debugWebhookUrl, N8N_WEBHOOK_URL) == 0;
  String content = String("ESP debug heartbeat `") + DEVICE_ID + "` ";
  content += uploadOk ? "upload ok" : "upload failed";
  content += " | interval=" + String(gApp.sampleIntervalSeconds) + "s";
  content += " | cycle_ms=" + String(millis() - cycleStartedAtMs);
  if (readings) {
    content += " | T=" + String(readings->temperature, 2) + "C";
    content += " RH=" + String(readings->humidity, 1) + "%";
    content += " P=" + String(readings->pressure, 1) + "hPa";
  }
  if (gApp.networkAvailable) {
    content += " | IP=" + WiFi.localIP().toString();
    content += " RSSI=" + String(WiFi.RSSI());
  }

  if (useStructuredWebhookPayload) {
    String extra = String("{\"mode\":\"debug\",\"interval_s\":") +
                   String(gApp.sampleIntervalSeconds) +
                   ",\"cycle_ms\":" + String(millis() - cycleStartedAtMs) +
                   ",\"upload_ok\":" + String(uploadOk ? "true" : "false") + "}";
    return sendWebhook("debug_heartbeat",
                       content,
                       uploadOk ? "info" : "warning",
                       readings,
                       extra.c_str());
  }

  String payload = String("{\"content\":\"") + jsonEscape(content) + "\"}";
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setConnectTimeout(WEBHOOK_TIMEOUT_MS);
  http.setTimeout(WEBHOOK_TIMEOUT_MS);

  if (!beginHttpRequest(http, plainClient, secureClient, debugWebhookUrl)) {
    Serial.println("Discord debug webhook: begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  addWebhookAccessHeaders(http, debugWebhookUrl);
  int code = http.POST(payload);
  String responseBody;
  if (VERBOSE_HTTP_LOGGING && code > 0) {
    responseBody = http.getString();
  }
  http.end();

  Serial.printf("Discord debug webhook -> %d\n", code);
  if (code < 0) {
    Serial.printf("Discord debug webhook error: %s\n", http.errorToString(code).c_str());
  } else if (VERBOSE_HTTP_LOGGING && responseBody.length()) {
    Serial.printf("Discord debug response: %s\n", responseBody.c_str());
  }
  return code >= 200 && code < 300;
}

// Builds a compact JSON object describing the current boot/session context.
String buildBootMetaJson(bool firstReadingFailed) {
  String meta = String("{\"fw\":\"") + FW_VERSION +
                "\",\"boot_mode\":\"" + bootModeName(gApp.bootMode) +
                "\",\"runtime_mode\":\"" + runtimeModeName(gApp.runtimeMode) +
                "\",\"interval_s\":" + String(gApp.sampleIntervalSeconds);
  if (gApp.networkAvailable) {
    meta += ",\"ip\":\"" + WiFi.localIP().toString() +
            "\",\"mac_address\":\"" + WiFi.macAddress() +
            "\",\"rssi_dbm\":" + String(WiFi.RSSI());
  }
  if (gApp.sessionId.length()) {
    meta += ",\"session_id\":\"" + gApp.sessionId + "\"";
  }
  if (firstReadingFailed) {
    meta += ",\"first_reading_failed\":true";
  }
  meta += "}";
  return meta;
}

// Builds a compact JSON object describing the current USB service-mode context.
String buildServiceModeMetaJson() {
  String meta = String("{\"fw\":\"") + FW_VERSION +
                "\",\"boot_mode\":\"" + bootModeName(gApp.bootMode) +
                "\",\"runtime_mode\":\"" + runtimeModeName(gApp.runtimeMode) +
                "\",\"usb_host_attached\":" +
                String(isUsbHostAttached() ? "true" : "false") +
                ",\"readings_paused\":true" +
                ",\"interval_s\":" + String(gApp.sampleIntervalSeconds);
  if (gApp.networkAvailable) {
    meta += ",\"ip\":\"" + WiFi.localIP().toString() +
            "\",\"mac_address\":\"" + WiFi.macAddress() +
            "\",\"rssi_dbm\":" + String(WiFi.RSSI());
  }
  if (gApp.sessionId.length()) {
    meta += ",\"session_id\":\"" + gApp.sessionId + "\"";
  }
  meta += "}";
  return meta;
}

// Emits a fixed set of test webhook payloads so endpoints can be validated
// without waiting for real startup/error conditions.
void testWebhooks() {
  Serial.println("Sending test webhooks (one of each type)...");

  SensorReadings sample;
  sample.temperature = 22.5f;
  sample.humidity = 45.0f;
  sample.pressure = 1012.3f;
  sample.batteryVoltage = 3.95f;
  sample.batteryPercent = 79.0f;

  String extra = String("{\"test_mode\":true,\"ip_address\":\"") +
                 WiFi.localIP().toString() + "\"}";

  sendWebhook("device_startup", "Device booted successfully", "info", &sample,
              extra.c_str());
  sendWebhook("sensor_error", "Device entering error state - attempting recovery",
              "warning", &sample, extra.c_str());
  sendWebhook("recovery_failed", "Device failed to recover - dropping reading",
              "error", &sample, extra.c_str());
  sendWebhook("sensor_recovered", "Device successfully recovered from error state",
              "info", &sample, extra.c_str());
}
