#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <ESP32Ping.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
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
#define FW_VERSION "envnode-1.2.0"

#ifndef SAMPLE_INTERVAL_SECONDS
  #define SAMPLE_INTERVAL_SECONDS 600UL
#endif

#ifndef DEBUG_SAMPLE_INTERVAL_SECONDS
  #define DEBUG_SAMPLE_INTERVAL_SECONDS 60UL
#endif

#ifndef MIN_SAMPLE_INTERVAL_SECONDS
  #define MIN_SAMPLE_INTERVAL_SECONDS 60UL
#endif

#ifndef MAX_SAMPLE_INTERVAL_SECONDS
  #define MAX_SAMPLE_INTERVAL_SECONDS 86400UL
#endif

#ifndef WIFI_CONNECT_TIMEOUT_MS
  #define WIFI_CONNECT_TIMEOUT_MS 15000UL
#endif

#ifndef WIFI_RECONNECT_INTERVAL_MS
  #define WIFI_RECONNECT_INTERVAL_MS 10000UL
#endif

#ifndef SERIAL_CONFIG_WINDOW_MS
  #define SERIAL_CONFIG_WINDOW_MS 10000UL
#endif

#ifndef DEBUG_AWAKE_WINDOW_MS
  #define DEBUG_AWAKE_WINDOW_MS 0UL
#endif

#ifndef DISABLE_DEEP_SLEEP
  #define DISABLE_DEEP_SLEEP 0
#endif

#ifndef DEVICE_DEBUG_MODE
  #define DEVICE_DEBUG_MODE 0
#endif

#ifndef WIFI_USE_STATIC_IP
  #define WIFI_USE_STATIC_IP 0
#endif

#ifndef WIFI_STATIC_IP
  #define WIFI_STATIC_IP 10,0,0,50
#endif

#ifndef WIFI_GATEWAY
  #define WIFI_GATEWAY 10,0,0,1
#endif

#ifndef WIFI_SUBNET
  #define WIFI_SUBNET 255,255,255,0
#endif

#ifndef WIFI_DNS1
  #define WIFI_DNS1 1,1,1,1
#endif

#ifndef WIFI_DNS2
  #define WIFI_DNS2 8,8,8,8
#endif

#ifndef WIFI_OVERRIDE_DNS
  #define WIFI_OVERRIDE_DNS 0
#endif

#ifndef WIFI_TX_POWER_DBM
  #define WIFI_TX_POWER_DBM 15
#endif

#ifndef SENSOR_POWER_SETTLE_MS
  #define SENSOR_POWER_SETTLE_MS 500UL
#endif

#ifndef BME_TEMPERATURE_OFFSET_C
  #define BME_TEMPERATURE_OFFSET_C 0.0f
#endif

#ifndef DEBUG_DISCORD_WEBHOOK_URL
  #define DEBUG_DISCORD_WEBHOOK_URL ""
#endif

#ifndef BOOT_LED_BLINK_COUNT
  #define BOOT_LED_BLINK_COUNT 3
#endif

#ifndef BOOT_LED_BLINK_ON_MS
  #define BOOT_LED_BLINK_ON_MS 120UL
#endif

#ifndef BOOT_LED_BLINK_OFF_MS
  #define BOOT_LED_BLINK_OFF_MS 120UL
#endif

constexpr bool DEBUG_MODE_ENABLED = DEVICE_DEBUG_MODE != 0;
constexpr bool DEEP_SLEEP_ENABLED = DISABLE_DEEP_SLEEP == 0;
constexpr uint32_t DEBUG_SAMPLE_INTERVAL = DEBUG_SAMPLE_INTERVAL_SECONDS;
constexpr uint32_t PRODUCTION_SAMPLE_INTERVAL = SAMPLE_INTERVAL_SECONDS;
constexpr uint32_t DEFAULT_SAMPLE_INTERVAL_SECONDS = DEBUG_MODE_ENABLED ? DEBUG_SAMPLE_INTERVAL : PRODUCTION_SAMPLE_INTERVAL;
constexpr uint32_t MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS = MIN_SAMPLE_INTERVAL_SECONDS;
constexpr uint32_t MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS = MAX_SAMPLE_INTERVAL_SECONDS;
constexpr const char* CONFIG_NAMESPACE = "envnode";
constexpr const char* SAMPLE_INTERVAL_KEY = "interval_s";

// Data model
struct SensorReadings {
  float temperature;
  float humidity;
  float pressure; // hPa
};

enum class BootMode {
  ColdBoot,
  TimerWake,
  OtherReset,
};

// ========= Sensor & timing =========
Adafruit_BME680 bme;                   // I2C
RTC_DATA_ATTR SensorReadings gLastGood = {NAN, NAN, NAN};
uint32_t gSampleIntervalSeconds = DEFAULT_SAMPLE_INTERVAL_SECONDS;
BootMode gBootMode = BootMode::OtherReset;
uint8_t gBmeAddress = 0;

#if defined(LED_BUILTIN)
constexpr int STATUS_LED_PIN = LED_BUILTIN;
constexpr bool STATUS_LED_AVAILABLE = true;
#else
constexpr int STATUS_LED_PIN = -1;
constexpr bool STATUS_LED_AVAILABLE = false;
#endif

constexpr uint8_t STATUS_LED_ON_LEVEL = HIGH;
constexpr uint8_t STATUS_LED_OFF_LEVEL = LOW;

// ========= XIAO ESP32-S3 I2C pins =========
// Seeed XIAO ESP32-S3: pin 9 = SDA (GPIO8), pin 10 = SCL (GPIO9)
// We prefer the board pin macros (D9/D10) when available.
#if defined(D9) && defined(D10)
constexpr int I2C_SDA_PIN = D9;
constexpr int I2C_SCL_PIN = D10;
#else
constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;
#endif

bool gLastI2cClearRequired = false;

// Error state tracking for webhook alerts
bool gInErrorState = false;
unsigned long gLastWebhookSent = 0;
constexpr unsigned long WEBHOOK_COOLDOWN_MS = 1000; // 1 second minimum between webhooks
constexpr uint16_t WEBHOOK_TIMEOUT_MS = 10000; // Reasonable timeout for TLS connect + webhook response.
bool gNetworkAvailable = false;
wl_status_t gLastReportedWiFiStatus = WL_IDLE_STATUS;
bool gWiFiHasConfiguredSta = false;
wifi_event_id_t gWiFiEventLoggerHandle = 0;
uint8_t gTargetBssid[6] = {0};
bool gHasTargetBssid = false;
int32_t gTargetChannel = 0;
uint16_t gLastWiFiDisconnectReason = 0;
uint32_t gWiFiConnectFailures = 0;
unsigned long gLastSampleRunMs = 0;
String gSerialInputBuffer;
bool gStayAwakeOnStartupError = false;

// ========= Debug mode =========
// Set to true to send test webhooks at startup, false for normal operation
constexpr bool DEBUG_WEBHOOKS = false;

// Session correlation for logs
String gSessionId;

constexpr uint8_t WIFI_STATIC_IP_BYTES[4] = {WIFI_STATIC_IP};
constexpr uint8_t WIFI_GATEWAY_BYTES[4] = {WIFI_GATEWAY};
constexpr uint8_t WIFI_SUBNET_BYTES[4] = {WIFI_SUBNET};
constexpr uint8_t WIFI_DNS1_BYTES[4] = {WIFI_DNS1};
constexpr uint8_t WIFI_DNS2_BYTES[4] = {WIFI_DNS2};

bool connectWiFi(unsigned long timeoutMs = WIFI_CONNECT_TIMEOUT_MS);
void logWiFiScanResults();
void runConnectivityChecks();
void printWiFiDiagnostics();
void printTxPowerSummary();
bool checkSupabaseTablesOnce();

float applyTemperatureCompensation(float rawTemperatureC) {
  return rawTemperatureC + static_cast<float>(BME_TEMPERATURE_OFFSET_C);
}

BootMode detectBootMode() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    return BootMode::TimerWake;
  }

  return esp_reset_reason() == ESP_RST_POWERON ? BootMode::ColdBoot : BootMode::OtherReset;
}

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

bool hasLastGoodReading() {
  return !(isnan(gLastGood.temperature) || isnan(gLastGood.humidity) || isnan(gLastGood.pressure));
}

uint32_t sanitizeSampleIntervalSeconds(uint32_t intervalSeconds) {
  if (intervalSeconds < MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS) {
    return MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS;
  }
  if (intervalSeconds > MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS) {
    return MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS;
  }
  return intervalSeconds;
}

uint32_t loadSampleIntervalSeconds() {
  if (DEBUG_MODE_ENABLED) {
    return sanitizeSampleIntervalSeconds(DEBUG_SAMPLE_INTERVAL);
  }

  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    return DEFAULT_SAMPLE_INTERVAL_SECONDS;
  }

  uint32_t intervalSeconds = prefs.getULong(SAMPLE_INTERVAL_KEY, DEFAULT_SAMPLE_INTERVAL_SECONDS);
  prefs.end();
  return sanitizeSampleIntervalSeconds(intervalSeconds);
}

bool saveSampleIntervalSeconds(uint32_t intervalSeconds) {
  if (DEBUG_MODE_ENABLED) {
    gSampleIntervalSeconds = sanitizeSampleIntervalSeconds(DEBUG_SAMPLE_INTERVAL);
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
    gSampleIntervalSeconds = sanitized;
  }
  return ok;
}

bool clearSampleIntervalOverride() {
  if (DEBUG_MODE_ENABLED) {
    gSampleIntervalSeconds = sanitizeSampleIntervalSeconds(DEBUG_SAMPLE_INTERVAL);
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }

  bool ok = prefs.remove(SAMPLE_INTERVAL_KEY);
  prefs.end();
  gSampleIntervalSeconds = DEFAULT_SAMPLE_INTERVAL_SECONDS;
  return ok;
}

void printSampleIntervalConfig() {
  Serial.printf("Sample interval: %lu seconds (mode=%s, default %lu, allowed %lu-%lu)\n",
                static_cast<unsigned long>(gSampleIntervalSeconds),
                DEBUG_MODE_ENABLED ? "debug" : "production",
                static_cast<unsigned long>(DEFAULT_SAMPLE_INTERVAL_SECONDS),
                static_cast<unsigned long>(MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS),
                static_cast<unsigned long>(MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS));
}

void printSerialConfigHelp() {
  Serial.println("Serial config commands:");
  Serial.println("  help               Show available commands");
  Serial.println("  interval           Print the active sample interval");
  Serial.println("  interval <seconds> Persist a new sample interval");
  Serial.println("  interval default   Clear the stored override");
  Serial.println("  status             Print WiFi/IP/tx power details");
  Serial.println("  scan               Scan nearby WiFi networks");
  Serial.println("  ping               Ping gateway, 1.1.1.1, and google.com");
  Serial.println("  resolve <host>     Resolve a hostname");
  Serial.println("  txpower            Print configured WiFi TX power");
  Serial.println("  reconnect          Restart STA and reconnect WiFi");
}

void handleSerialCommand(const String& rawCommand) {
  String command = rawCommand;
  command.trim();
  if (!command.length()) {
    return;
  }

  if (command.equalsIgnoreCase("help")) {
    printSerialConfigHelp();
    return;
  }

  if (command.equalsIgnoreCase("interval")) {
    printSampleIntervalConfig();
    return;
  }

  if (command.equalsIgnoreCase("status")) {
    printWiFiDiagnostics();
    return;
  }

  if (command.equalsIgnoreCase("scan")) {
    logWiFiScanResults();
    return;
  }

  if (command.equalsIgnoreCase("ping")) {
    runConnectivityChecks();
    return;
  }

  if (command.equalsIgnoreCase("txpower")) {
    printTxPowerSummary();
    return;
  }

  if (command.equalsIgnoreCase("reconnect")) {
    connectWiFi();
    return;
  }

  if (command.startsWith("resolve ")) {
    String host = command.substring(strlen("resolve "));
    host.trim();
    if (!host.length()) {
      Serial.println("Usage: resolve <hostname>");
      return;
    }

    if (!gNetworkAvailable) {
      Serial.println("Resolve skipped: WiFi unavailable");
      return;
    }

    IPAddress resolved;
    if (WiFi.hostByName(host.c_str(), resolved)) {
      Serial.printf("Resolved %s -> %s\n", host.c_str(), resolved.toString().c_str());
    } else {
      Serial.printf("Failed to resolve %s\n", host.c_str());
    }
    return;
  }

  if (command.startsWith("interval ")) {
    String arg = command.substring(strlen("interval "));
    arg.trim();

    if (arg.equalsIgnoreCase("default")) {
      bool cleared = clearSampleIntervalOverride();
      Serial.println(cleared ? "Sample interval override cleared." : "Failed to clear sample interval override.");
      printSampleIntervalConfig();
      return;
    }

    char* endPtr = nullptr;
    unsigned long parsed = strtoul(arg.c_str(), &endPtr, 10);
    bool parseFailed = (arg.length() == 0) || (endPtr == nullptr) || (*endPtr != '\0');
    if (parseFailed || parsed == 0) {
      Serial.println("Invalid interval. Use an integer number of seconds or 'interval default'.");
      return;
    }

    uint32_t sanitized = sanitizeSampleIntervalSeconds(static_cast<uint32_t>(parsed));
    bool saved = saveSampleIntervalSeconds(sanitized);
    if (!saved) {
      Serial.println("Failed to save sample interval.");
      return;
    }

    if (sanitized != parsed) {
      Serial.printf("Sample interval clamped to %lu seconds before saving.\n", static_cast<unsigned long>(sanitized));
    }
    printSampleIntervalConfig();
    return;
  }

  Serial.println("Unknown command. Type 'help' for supported commands.");
}

void pollSerialCommands() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      handleSerialCommand(gSerialInputBuffer);
      gSerialInputBuffer = "";
      continue;
    }
    gSerialInputBuffer += ch;
  }
}

bool isStartupBoot() {
  return gBootMode != BootMode::TimerWake;
}

void recordStartupError(const char* message) {
  if (!isStartupBoot()) {
    return;
  }

  gStayAwakeOnStartupError = true;
  Serial.printf("Startup error: %s\n", message);
}

void handleSerialConfigWindow() {
  if (gBootMode == BootMode::TimerWake || SERIAL_CONFIG_WINDOW_MS == 0) {
    return;
  }

  Serial.printf("Startup hold open for %lu ms. Type 'help' for commands or start a firmware upload.\n",
                static_cast<unsigned long>(SERIAL_CONFIG_WINDOW_MS));

  unsigned long start = millis();
  while (millis() - start < SERIAL_CONFIG_WINDOW_MS) {
    if (Serial) {
      pollSerialCommands();
    }
    delay(10);
  }

  if (gSerialInputBuffer.length()) {
    handleSerialCommand(gSerialInputBuffer);
    gSerialInputBuffer = "";
  }
}

void initStatusLed() {
  if (!STATUS_LED_AVAILABLE) {
    return;
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, STATUS_LED_OFF_LEVEL);
}

void setAwakeLed(bool on) {
  if (!STATUS_LED_AVAILABLE) {
    return;
  }

  digitalWrite(STATUS_LED_PIN, on ? STATUS_LED_ON_LEVEL : STATUS_LED_OFF_LEVEL);
}

void blinkColdBootSuccessLed() {
  if (!STATUS_LED_AVAILABLE || gBootMode != BootMode::ColdBoot) {
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

bool isWiFiStaModeEnabled() {
  wifi_mode_t mode = WiFi.getMode();
  return mode == WIFI_STA || mode == WIFI_AP_STA;
}

void shutdownWiFi() {
  gNetworkAvailable = false;
  gWiFiHasConfiguredSta = false;
  if (isWiFiStaModeEnabled() && WiFi.isConnected()) {
    WiFi.disconnect(false, false);
    delay(50);
  }
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.mode(WIFI_OFF);
  }
}

void enterDeepSleep() {
  #if DISABLE_DEEP_SLEEP
  setAwakeLed(true);
  Serial.printf("Deep sleep disabled; WiFi status=%d. Staying awake.\n",
                static_cast<int>(WiFi.status()));
  Serial.flush();
  return;
  #endif

  if (gStayAwakeOnStartupError) {
    setAwakeLed(true);
    Serial.println("Deep sleep blocked because startup errors were detected. Staying awake for diagnostics.");
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
  shutdownWiFi();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(gSampleIntervalSeconds) * 1000000ULL);
  Serial.printf("Sleeping for %lu seconds...\n", static_cast<unsigned long>(gSampleIntervalSeconds));
  Serial.flush();
  esp_deep_sleep_start();
}

// ================= Network helpers =================
const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "no_ssid";
    case WL_SCAN_COMPLETED:
      return "scan_completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

void logWiFiStatus(const char* prefix, wl_status_t status) {
  Serial.printf("%s%s (%d)\n", prefix, wifiStatusName(status), static_cast<int>(status));
}

void printWiFiNetworkSummary() {
  Serial.printf("WiFi config: IP=%s Gateway=%s Subnet=%s DNS1=%s DNS2=%s MAC=%s RSSI=%d dBm BSSID=%s CH=%d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str(),
                WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str(),
                WiFi.macAddress().c_str(),
                static_cast<int>(WiFi.RSSI()),
                WiFi.BSSIDstr().c_str(),
                static_cast<int>(WiFi.channel()));
}

wifi_power_t configuredTxPower() {
  #if WIFI_TX_POWER_DBM >= 19
  return WIFI_POWER_19_5dBm;
  #elif WIFI_TX_POWER_DBM >= 17
  return WIFI_POWER_17dBm;
  #else
  return WIFI_POWER_15dBm;
  #endif
}

const char* txPowerName(wifi_power_t power) {
  switch (power) {
    case WIFI_POWER_15dBm:
      return "15 dBm";
    case WIFI_POWER_17dBm:
      return "17 dBm";
    case WIFI_POWER_19_5dBm:
      return "19.5 dBm";
    default:
      return "other";
  }
}

void printTxPowerSummary() {
  wifi_power_t currentPower = WiFi.getTxPower();
  Serial.printf("WiFi TX power: %s (%d)\n", txPowerName(currentPower), static_cast<int>(currentPower));
}

void applyWiFiTxPower() {
  wifi_power_t power = configuredTxPower();
  WiFi.setTxPower(power);
  Serial.printf("WiFi: applied TX power %s\n", txPowerName(power));
}

void logPingResult(const char* label, const IPAddress& address) {
  Serial.printf("Ping test: %s (%s) ... ", label, address.toString().c_str());
  bool ok = Ping.ping(address, 1);
  if (ok) {
    Serial.printf("ok, avg=%u ms\n", Ping.averageTime());
  } else {
    Serial.printf("failed, avg=%u ms\n", Ping.averageTime());
  }
}

void runConnectivityChecks() {
  Serial.println("Connectivity checks: waiting 5 seconds before ping tests...");
  delay(5000);

  IPAddress gateway = WiFi.gatewayIP();
  if (gateway != INADDR_NONE) {
    logPingResult("gateway", gateway);
  } else {
    Serial.println("Ping test: gateway unavailable");
  }

  IPAddress publicIp(1, 1, 1, 1);
  logPingResult("1.1.1.1", publicIp);

  IPAddress googleIp;
  if (WiFi.hostByName("google.com", googleIp)) {
    Serial.printf("DNS test: google.com -> %s\n", googleIp.toString().c_str());
    logPingResult("google.com", googleIp);
  } else {
    Serial.println("DNS test: google.com resolution failed");
  }
}

void printWiFiDiagnostics() {
  Serial.printf("WiFi status: %s (%d)\n", wifiStatusName(WiFi.status()), static_cast<int>(WiFi.status()));
  printWiFiNetworkSummary();
  Serial.printf("Last disconnect reason=%u\n", static_cast<unsigned>(gLastWiFiDisconnectReason));
  printTxPowerSummary();
}

String formatMacAddress(const uint8_t* mac) {
  char buffer[18];
  snprintf(buffer,
           sizeof(buffer),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
  return String(buffer);
}

IPAddress makeIpAddress(const uint8_t bytes[4]) {
  return IPAddress(bytes[0], bytes[1], bytes[2], bytes[3]);
}

bool isHttpsUrl(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

bool beginHttpRequest(HTTPClient& http,
                      WiFiClient& plainClient,
                      WiFiClientSecure& secureClient,
                      const char* url) {
  if (isHttpsUrl(url)) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }

  return http.begin(plainClient, url);
}

void configureWiFiNetworkStack() {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  if (WIFI_USE_STATIC_IP) {
    IPAddress localIp = makeIpAddress(WIFI_STATIC_IP_BYTES);
    IPAddress gateway = makeIpAddress(WIFI_GATEWAY_BYTES);
    IPAddress subnet = makeIpAddress(WIFI_SUBNET_BYTES);
    IPAddress dns1 = makeIpAddress(WIFI_DNS1_BYTES);
    IPAddress dns2 = makeIpAddress(WIFI_DNS2_BYTES);
    if (!WiFi.config(localIp, gateway, subnet, dns1, dns2)) {
      Serial.println("WiFi: static IP config failed; falling back to DHCP.");
    }
  } else if (WIFI_OVERRIDE_DNS) {
    IPAddress dns1 = makeIpAddress(WIFI_DNS1_BYTES);
    IPAddress dns2 = makeIpAddress(WIFI_DNS2_BYTES);
    if (!WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, dns1, dns2)) {
      Serial.println("WiFi: DHCP+DNS override config failed; falling back to DHCP defaults.");
    }
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }
}

bool isAuthOrAssocDisconnectReason(uint16_t reason) {
  switch (reason) {
    case 2:   // AUTH_EXPIRE
    case 4:   // ASSOC_EXPIRE
    case 8:   // ASSOC_LEAVE
    case 15:  // 4WAY_HANDSHAKE_TIMEOUT
    case 203: // ASSOC_FAIL
      return true;
    default:
      return false;
  }
}

void clearLockedBssid() {
  memset(gTargetBssid, 0, sizeof(gTargetBssid));
  gHasTargetBssid = false;
  gTargetChannel = 0;
}

void registerWiFiEventLogger() {
  if (gWiFiEventLoggerHandle != 0) {
    return;
  }

  gWiFiEventLoggerHandle = WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        Serial.println("WiFi event: STA start");
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        gLastWiFiDisconnectReason = 0;
        Serial.printf("WiFi event: STA connected on channel %u, authmode=%u\n",
                      static_cast<unsigned>(info.wifi_sta_connected.channel),
                      static_cast<unsigned>(info.wifi_sta_connected.authmode));
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        gLastWiFiDisconnectReason = static_cast<uint16_t>(info.wifi_sta_disconnected.reason);
        Serial.printf("WiFi event: STA disconnected, reason=%u (%s)\n",
                      static_cast<unsigned>(info.wifi_sta_disconnected.reason),
                      WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason)));
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("WiFi event: got IP %s\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        Serial.println("WiFi event: lost IP");
        break;
      case ARDUINO_EVENT_WIFI_STA_STOP:
        Serial.println("WiFi event: STA stop");
        break;
      default:
        break;
    }
  });
}

void ensureWiFiStaReady(bool forceRestart) {
  if (forceRestart && WiFi.getMode() != WIFI_OFF) {
    gWiFiHasConfiguredSta = false;
    WiFi.disconnect(false, false);
    delay(150);
    WiFi.mode(WIFI_OFF);
    delay(300);
  }

  if (!isWiFiStaModeEnabled()) {
    WiFi.mode(WIFI_STA);
    delay(300);
  }
}

void logWiFiScanResults() {
  ensureWiFiStaReady(true);
  Serial.println("WiFi scan: starting...");
  int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount < 0) {
    Serial.printf("WiFi scan: failed with code %d\n", networkCount);
    return;
  }

  Serial.printf("WiFi scan: found %d network(s)\n", networkCount);
  bool foundTarget = false;
  int32_t bestTargetRssi = INT32_MIN;
  for (int index = 0; index < networkCount; ++index) {
    String ssid = WiFi.SSID(index);
    int32_t rssi = WiFi.RSSI(index);
    uint8_t encryption = WiFi.encryptionType(index);
    int32_t channel = WiFi.channel(index);
    uint8_t bssid[6] = {0};
    WiFi.BSSID(index, bssid);
    bool isTarget = ssid == WIFI_SSID;
    if (isTarget) {
      foundTarget = true;
      if (rssi > bestTargetRssi) {
        memcpy(gTargetBssid, bssid, sizeof(gTargetBssid));
        gTargetChannel = channel;
        gHasTargetBssid = true;
        bestTargetRssi = rssi;
      }
    }

    Serial.printf("  [%d] SSID='%s' RSSI=%ld dBm CH=%ld ENC=%u BSSID=%s%s\n",
                  index,
                  ssid.c_str(),
                  static_cast<long>(rssi),
                  static_cast<long>(channel),
                  static_cast<unsigned>(encryption),
                  formatMacAddress(bssid).c_str(),
                  isTarget ? " <target>" : "");
  }

  if (!foundTarget) {
    gHasTargetBssid = false;
    gTargetChannel = 0;
    Serial.printf("WiFi scan: target SSID '%s' not visible to the ESP32 radio.\n", WIFI_SSID);
  } else {
    Serial.printf("WiFi scan: locking target BSSID=%s CH=%ld for next association attempt.\n",
                  formatMacAddress(gTargetBssid).c_str(),
                  static_cast<long>(gTargetChannel));
  }

  WiFi.scanDelete();
}

bool connectWiFi(unsigned long timeoutMs) {
  configureWiFiNetworkStack();

  wl_status_t preStatus = WiFi.status();
  bool authOrAssocIssue = isAuthOrAssocDisconnectReason(gLastWiFiDisconnectReason);
  bool forceRestart = preStatus == WL_CONNECT_FAILED ||
                      preStatus == WL_CONNECTION_LOST ||
                      preStatus == WL_DISCONNECTED ||
                      authOrAssocIssue;
  ensureWiFiStaReady(forceRestart);

  if (authOrAssocIssue && gHasTargetBssid) {
    Serial.printf("WiFi: clearing locked BSSID after disconnect reason %u.\n",
                  static_cast<unsigned>(gLastWiFiDisconnectReason));
    clearLockedBssid();
  }

  bool useLockedBssid = gHasTargetBssid && gTargetChannel > 0;

  if (!gWiFiHasConfiguredSta) {
    if (useLockedBssid) {
      Serial.printf("WiFi: associating to locked BSSID=%s CH=%ld\n",
                    formatMacAddress(gTargetBssid).c_str(),
                    static_cast<long>(gTargetChannel));
      WiFi.begin(WIFI_SSID, WIFI_PASS, gTargetChannel, gTargetBssid, true);
    } else {
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    applyWiFiTxPower();
    gWiFiHasConfiguredSta = true;
  } else if (!WiFi.reconnect()) {
    Serial.println("WiFi: reconnect request failed; restarting STA and reapplying credentials.");
    ensureWiFiStaReady(true);
    if (useLockedBssid) {
      Serial.printf("WiFi: reassociating to locked BSSID=%s CH=%ld\n",
                    formatMacAddress(gTargetBssid).c_str(),
                    static_cast<long>(gTargetChannel));
      WiFi.begin(WIFI_SSID, WIFI_PASS, gTargetChannel, gTargetBssid, true);
    } else {
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    applyWiFiTxPower();
    gWiFiHasConfiguredSta = true;
  }

  Serial.print("WiFi: connecting");
  unsigned long connectStartedAt = millis();
  unsigned long start = millis();
  wl_status_t lastStatus = WiFi.status();
  gLastReportedWiFiStatus = lastStatus;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
    wl_status_t currentStatus = WiFi.status();
    if (currentStatus != lastStatus) {
      Serial.println();
      logWiFiStatus("WiFi: status -> ", currentStatus);
      lastStatus = currentStatus;
      gLastReportedWiFiStatus = currentStatus;
    }
  }

  wl_status_t finalStatus = WiFi.status();
  gLastReportedWiFiStatus = finalStatus;
  gNetworkAvailable = finalStatus == WL_CONNECTED;
  if (!gNetworkAvailable) {
    ++gWiFiConnectFailures;
    Serial.println();
    Serial.printf("WiFi: connection timed out; status=%s (%d).\n",
                  wifiStatusName(finalStatus),
                  static_cast<int>(finalStatus));
    if (authOrAssocIssue) {
      Serial.printf("WiFi: last disconnect reason=%u suggests AP/auth negotiation trouble rather than DHCP.\n",
                    static_cast<unsigned>(gLastWiFiDisconnectReason));
    }
    if (finalStatus == WL_NO_SSID_AVAIL ||
        finalStatus == WL_CONNECT_FAILED ||
        finalStatus == WL_DISCONNECTED ||
        gWiFiConnectFailures >= 2) {
      logWiFiScanResults();
    }
    #if !DISABLE_DEEP_SLEEP
    shutdownWiFi();
    #endif
    return false;
  }

  gWiFiConnectFailures = 0;
  gLastWiFiDisconnectReason = 0;
  Serial.printf("\nWiFi: connected, IP=%s in %lu ms\n",
                WiFi.localIP().toString().c_str(),
                static_cast<unsigned long>(millis() - connectStartedAt));
  printWiFiNetworkSummary();
  printTxPowerSummary();
  runConnectivityChecks();
  return true;
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
  if (!gNetworkAvailable) {
    Serial.printf("Skipping Supabase insert for %s: WiFi unavailable\n", table);
    return false;
  }

  String endpoint = String(SUPABASE_URL) + "/rest/v1/" + table;

  WiFiClient plainClient;
  WiFiClientSecure client;

  HTTPClient https;
  if (!beginHttpRequest(https, plainClient, client, endpoint.c_str())) {
    Serial.println("HTTP begin failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  String authHeader = String("Bearer ") + SUPABASE_API_KEY;
  https.addHeader("apikey", SUPABASE_API_KEY);
  https.addHeader("Authorization", authHeader);

  unsigned long startedAt = millis();
  int code = https.POST(payloadJson);
  Serial.printf("POST %s -> %d (%lu ms)\n",
                endpoint.c_str(),
                code,
                static_cast<unsigned long>(millis() - startedAt));
  if (code < 0) Serial.printf("HTTP error: %s\n", https.errorToString(code).c_str());
  https.end();
  return code >= 200 && code < 300;
}

bool supabaseTableExists(const char* table) {
  if (!gNetworkAvailable) {
    Serial.printf("Skipping Supabase table check for %s: WiFi unavailable\n", table);
    return false;
  }

  String endpoint = String(SUPABASE_URL) + "/rest/v1/" + table + "?select=*&limit=1";

  WiFiClient plainClient;
  WiFiClientSecure client;

  HTTPClient https;
  if (!beginHttpRequest(https, plainClient, client, endpoint.c_str())) {
    Serial.printf("Supabase table check: begin failed for %s\n", table);
    return false;
  }
  https.addHeader("Accept", "application/json");
  https.addHeader("Range-Unit", "items");
  https.addHeader("Range", "0-0");

  String authHeader = String("Bearer ") + SUPABASE_API_KEY;
  https.addHeader("apikey", SUPABASE_API_KEY);
  https.addHeader("Authorization", authHeader);

  unsigned long startedAt = millis();
  int code = https.GET();
  if (code < 0) {
    Serial.printf("Supabase table check: HTTP error for %s -> %s\n",
                  table, https.errorToString(code).c_str());
  } else {
    Serial.printf("Supabase table check %s -> %d (%lu ms)\n",
                  table,
                  code,
                  static_cast<unsigned long>(millis() - startedAt));
  }
  https.end();
  return code >= 200 && code < 300;
}

bool checkSupabaseTablesOnce() {
  if (!gNetworkAvailable) {
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
  if (!gNetworkAvailable) {
    Serial.printf("Skipping webhook %s: WiFi unavailable\n", alert_type);
    return false;
  }

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

  WiFiClient plainClient;
  WiFiClientSecure client;

  HTTPClient http;
  http.setConnectTimeout(WEBHOOK_TIMEOUT_MS);
  http.setTimeout(WEBHOOK_TIMEOUT_MS);

  // The webhook path is confirmed good via curl. If this fails, focus on device HTTPS transport,
  // payload contents, or downstream workflow handling rather than the server route itself.
  Serial.printf("Webhook: starting send [%s/%s]\n", alert_type, severity);
  Serial.printf("Webhook: target URL %s\n", N8N_WEBHOOK_URL);
  
  if (!beginHttpRequest(http, plainClient, client, N8N_WEBHOOK_URL)) {
    Serial.printf("Webhook: begin failed for %s\n", N8N_WEBHOOK_URL);
    return false;
  }
  
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String responseBody;
  if (code > 0) {
    responseBody = http.getString();
  }
  
  Serial.printf("Webhook POST [%s/%s] -> %d\n", alert_type, severity, code);
  if (code < 0) {
    Serial.printf("Webhook error: %s\n", http.errorToString(code).c_str());
  }
  Serial.printf("Webhook response body: %s\n", responseBody.length() ? responseBody.c_str() : "<empty>");
  
  http.end();
  
  bool ok = (code >= 200 && code < 300);
  if (ok) {
    gLastWebhookSent = now;
  }
  return ok;
}

bool sendDebugDiscordMessage(const SensorReadings* readings,
                             bool uploadOk,
                             unsigned long cycleStartedAtMs) {
  if (!DEBUG_MODE_ENABLED) {
    return false;
  }

  if (!gNetworkAvailable) {
    Serial.println("Skipping debug Discord message: WiFi unavailable");
    return false;
  }

  const char* debugWebhookUrl = strlen(DEBUG_DISCORD_WEBHOOK_URL) ? DEBUG_DISCORD_WEBHOOK_URL : N8N_WEBHOOK_URL;
  if (!debugWebhookUrl || !debugWebhookUrl[0]) {
    return false;
  }

  bool useStructuredWebhookPayload = strcmp(debugWebhookUrl, N8N_WEBHOOK_URL) == 0;

  String content = String("ESP debug heartbeat `") + DEVICE_ID + "` ";
  content += uploadOk ? "upload ok" : "upload failed";
  content += " | interval=" + String(gSampleIntervalSeconds) + "s";
  content += " | cycle_ms=" + String(millis() - cycleStartedAtMs);
  if (readings) {
    content += " | T=" + String(readings->temperature, 2) + "C";
    content += " RH=" + String(readings->humidity, 1) + "%";
    content += " P=" + String(readings->pressure, 1) + "hPa";
  }
  if (gNetworkAvailable) {
    content += " | IP=" + WiFi.localIP().toString();
    content += " RSSI=" + String(WiFi.RSSI());
  }

  if (useStructuredWebhookPayload) {
    String extra = String("{\"mode\":\"debug\",\"interval_s\":") + String(gSampleIntervalSeconds) +
                   ",\"cycle_ms\":" + String(millis() - cycleStartedAtMs) +
                   ",\"upload_ok\":" + String(uploadOk ? "true" : "false") + "}";
    return sendWebhook("debug_heartbeat", content, uploadOk ? "info" : "warning", readings, extra.c_str());
  }

  String payload = String("{\"content\":\"") + jsonEscape(content) + "\"}";

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setConnectTimeout(WEBHOOK_TIMEOUT_MS);
  http.setTimeout(WEBHOOK_TIMEOUT_MS);

  if (!beginHttpRequest(http, plainClient, secureClient, debugWebhookUrl)) {
    Serial.printf("Discord debug webhook: begin failed for %s\n", debugWebhookUrl);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String responseBody;
  if (code > 0) {
    responseBody = http.getString();
  }
  http.end();

  Serial.printf("Discord debug webhook -> %d\n", code);
  if (code < 0) {
    Serial.printf("Discord debug webhook error: %s\n", http.errorToString(code).c_str());
  } else if (responseBody.length()) {
    Serial.printf("Discord debug response: %s\n", responseBody.c_str());
  }
  return code >= 200 && code < 300;
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

  // Gas heater is disabled because this project currently uploads only
  // temperature, humidity, and pressure. Disabling gas measurements reduces
  // active time and power draw on each wake cycle.
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(0, 0);
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
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  delayMicroseconds(5);
  gLastI2cClearRequired = (digitalRead(I2C_SDA_PIN) == LOW) || (digitalRead(I2C_SCL_PIN) == LOW);

  pinMode(I2C_SDA_PIN, OUTPUT_OPEN_DRAIN);
  pinMode(I2C_SCL_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA_PIN, HIGH);
  digitalWrite(I2C_SCL_PIN, HIGH);
  delayMicroseconds(5);

  if (!gLastI2cClearRequired && digitalRead(I2C_SDA_PIN) == HIGH && digitalRead(I2C_SCL_PIN) == HIGH) {
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

  bool clear = (digitalRead(I2C_SDA_PIN) == HIGH);

  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);

  return clear;
}

bool isI2cAddressResponsive(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

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
      Serial.printf("BME probe: device acknowledged at 0x%02X but chip ID read failed.\n", address);
      continue;
    }

    Serial.printf("BME probe: address 0x%02X reports chip ID 0x%02X\n", address, chipId);
    if (chipId == 0x61) {
      Serial.println("BME probe: this looks like a BME680, so init failure is likely power, timing, or bus integrity.");
    } else if (chipId == 0x60) {
      Serial.println("BME probe: this looks like a BME280. This firmware expects a BME680 library/device.");
    } else if (chipId == 0x58) {
      Serial.println("BME probe: this looks like a BMP280. It will not provide humidity and will not init as a BME680.");
    } else {
      Serial.println("BME probe: unexpected chip ID. Verify the sensor module and wiring.");
    }
  }

  if (!foundCandidate) {
    Serial.println("BME probe: no response at 0x76 or 0x77.");
  }
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
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(25);
  bool ok = false;
  if (bme.begin(0x76)) {
    gBmeAddress = 0x76;
    ok = true;
  } else if (bme.begin(0x77)) {
    gBmeAddress = 0x77;
    ok = true;
  }
  if (ok) bmeConfigure();
  return ok;
}

bool takeReading(SensorReadings& out) {
  if (!bme.performReading()) {
    return false;
  }

  out.temperature = applyTemperatureCompensation(bme.temperature);
  out.humidity    = bme.humidity;
  out.pressure    = bme.pressure / 100.0f; // Pa -> hPa
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

// Attempts up to three forced BME680 readings and validates each against an optional
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
bool postReadings(const SensorReadings& readings) {
  if (!isnan(readings.temperature) && !isnan(readings.humidity) && !isnan(readings.pressure)) {
    bool ok = postReadingRow(readings.temperature, readings.humidity, readings.pressure);
    Serial.println(ok ? "Upload ok" : "Upload failed");
    return ok;
  } else {
    Serial.println("Sensor returned NaN (check the BME680 wiring and power).");
    return false;
  }
}

bool initBME() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(25);
  bool ok = false;
  if (bme.begin(0x76)) {
    gBmeAddress = 0x76;
    ok = true;
  } else if (bme.begin(0x77)) {
    gBmeAddress = 0x77;
    ok = true;
  }

  if (ok) {
    bmeConfigure();
    Serial.printf("BME680 ready at I2C address 0x%02X\n", gBmeAddress);
  } else {
    Serial.println("BME680 not found (0x76/0x77). Check SDA on pin 9 and SCL on pin 10.");
    logBmeDetectionHints();
    logI2cProbeResults();
  }
  return ok;
}

String buildBootMetaJson(bool firstReadingFailed) {
  String meta = String("{\"fw\":\"") + FW_VERSION +
                "\",\"boot_mode\":\"" + bootModeName(gBootMode) +
                "\",\"interval_s\":" + String(gSampleIntervalSeconds);

  if (gNetworkAvailable) {
    meta += ",\"ip\":\"" + WiFi.localIP().toString() +
            "\",\"mac_address\":\"" + WiFi.macAddress() +
            "\",\"rssi_dbm\":" + String(WiFi.RSSI());
  }

  if (gSessionId.length()) {
    meta += ",\"session_id\":\"" + gSessionId + "\"";
  }

  if (firstReadingFailed) {
    meta += ",\"first_reading_failed\":true";
  }

  meta += "}";
  return meta;
}

void ensureSessionId() {
  if (gSessionId.length()) {
    return;
  }

  uint64_t mac = ESP.getEfuseMac();
  gSessionId = String((uint32_t)(mac >> 32), HEX) + String((uint32_t)mac, HEX) + "-" + String((uint32_t)esp_random(), HEX);
}

void waitForSensorPowerRail() {
  if (SENSOR_POWER_SETTLE_MS == 0) {
    return;
  }

  Serial.printf("Sensor power settle: waiting %lu ms before BME init.\n",
                static_cast<unsigned long>(SENSOR_POWER_SETTLE_MS));
  delay(SENSOR_POWER_SETTLE_MS);
}

bool isAwakeMode() {
  return !DEEP_SLEEP_ENABLED;
}

bool shouldRunStartupHooks() {
  return isStartupBoot() && gLastSampleRunMs == 0;
}

void maybeRunStartupHooks(const SensorReadings* readings, bool readingOk) {
  if (!shouldRunStartupHooks()) {
    return;
  }

  String startupMeta = buildBootMetaJson(!readingOk);
  bool startupEventOk = postEvent("startup",
                                  readingOk ? "info" : "warning",
                                  readingOk ? "device boot" : "device boot with invalid first reading",
                                  readingOk ? readings : nullptr,
                                  nullptr,
                                  0,
                                  readingOk,
                                  startupMeta.c_str());
  if (!startupEventOk) {
    recordStartupError("startup event post failed");
  }

  if (readingOk) {
    if (!sendWebhook("device_startup", "Device booted successfully", "info", readings, startupMeta.c_str())) {
      recordStartupError("startup webhook failed");
    }
    blinkColdBootSuccessLed();
  } else {
    if (!sendWebhook("device_startup", "Device booted but first reading failed", "warning", nullptr, startupMeta.c_str())) {
      recordStartupError("startup warning webhook failed");
    }
  }
}

void runSamplingCycle() {
  unsigned long cycleStartedAt = millis();
  setAwakeLed(true);
  ensureSessionId();

  SensorReadings r;
  const SensorReadings* lastKnownGood = hasLastGoodReading() ? &gLastGood : nullptr;
  bool ok = tryTakePlausibleReading(r, lastKnownGood);
  if (!ok) {
    ok = attemptRecoverySequence(r, lastKnownGood);
  }

  if (!gNetworkAvailable && WiFi.status() != WL_CONNECTED) {
    bool wifiOk = connectWiFi();
    if (shouldRunStartupHooks() && !wifiOk) {
      recordStartupError("initial WiFi connect failed");
    }
  }

  if (shouldRunStartupHooks() && gNetworkAvailable) {
    bool tablesOk = checkSupabaseTablesOnce();
    if (!tablesOk) {
      recordStartupError("Supabase connectivity check failed");
    }
  }

  maybeRunStartupHooks(ok ? &r : nullptr, ok);

  if (ok) {
    gLastGood = r;
    Serial.printf("GOOD: T=%.2f°C RH=%.1f%% P=%.1f hPa\n", r.temperature, r.humidity, r.pressure);

    bool uploadOk = false;
    if (gNetworkAvailable) {
      uploadOk = postReadings(r);
      if (shouldRunStartupHooks() && !uploadOk) {
        recordStartupError("initial reading upload failed");
      }
    } else {
      Serial.println("Skipping upload: WiFi unavailable for this cycle.");
      if (shouldRunStartupHooks()) {
        recordStartupError("WiFi unavailable during initial upload");
      }
    }

    if (gInErrorState) {
      gInErrorState = false;
      sendWebhook("sensor_recovered", "Device recovered - normal operation resumed", "info", &r);
    }

    sendDebugDiscordMessage(&r, uploadOk, cycleStartedAt);
  } else {
    Serial.println("Dropping bad reading after recovery attempts.");
    sendDebugDiscordMessage(nullptr, false, cycleStartedAt);
  }

  gLastSampleRunMs = millis();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  registerWiFiEventLogger();
  gBootMode = detectBootMode();
  gSampleIntervalSeconds = loadSampleIntervalSeconds();
  initStatusLed();
  setAwakeLed(true);

  Serial.printf("\nBooting (%s)...\n", bootModeName(gBootMode));
  printSampleIntervalConfig();
  Serial.printf("Runtime mode: %s, deep sleep: %s\n",
                DEBUG_MODE_ENABLED ? "debug" : "production",
                DEEP_SLEEP_ENABLED ? "enabled" : "disabled");
  handleSerialConfigWindow();
  ensureSessionId();
  waitForSensorPowerRail();

  if (!initBME()) {
    if (!gNetworkAvailable && WiFi.status() != WL_CONNECTED) {
      bool wifiOk = connectWiFi();
      if (shouldRunStartupHooks() && !wifiOk) {
        recordStartupError("initial WiFi connect failed during BME fault report");
      }
    }

    String meta = buildBootMetaJson(false);
    if (!postEvent("startup", "error", "BME init failed", nullptr, nullptr, 0, false, meta.c_str())) {
      recordStartupError("startup BME error event failed");
    }
    if (shouldRunStartupHooks()) {
      if (!sendWebhook("device_startup", "Device booted but BME init failed", "error", nullptr, meta.c_str())) {
        recordStartupError("startup BME failure webhook failed");
      }
    }
    Serial.println(DEEP_SLEEP_ENABLED ? "BME680 init failed; sleeping until the next interval."
                                      : "BME680 init failed; staying awake for debug/monitoring.");
    recordStartupError("BME init failed");
    enterDeepSleep();
    return;
  }

  runSamplingCycle();

  if (DEBUG_WEBHOOKS && shouldRunStartupHooks()) {
    testWebhooks();
  }

  if (DEEP_SLEEP_ENABLED) {
    enterDeepSleep();
  }
}

void loop() {
  #if DISABLE_DEEP_SLEEP
  const bool awakeLoopActive = true;
  #else
  const bool awakeLoopActive = gStayAwakeOnStartupError;
  #endif

  if (awakeLoopActive) {
  static unsigned long lastStatusLogMs = 0;
  static unsigned long lastReconnectAttemptMs = 0;

  pollSerialCommands();

  unsigned long now = millis();
  wl_status_t status = WiFi.status();
  gNetworkAvailable = status == WL_CONNECTED;

  if (status != gLastReportedWiFiStatus || now - lastStatusLogMs >= 5000) {
    logWiFiStatus("WiFi: current status ", status);
    gLastReportedWiFiStatus = status;
    lastStatusLogMs = now;
  }

  if (!gNetworkAvailable && now - lastReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    Serial.println("WiFi: retrying connection from debug loop...");
    lastReconnectAttemptMs = now;
    connectWiFi();
  }

  if (gLastSampleRunMs == 0 || now - gLastSampleRunMs >= static_cast<unsigned long>(gSampleIntervalSeconds) * 1000UL) {
    Serial.println("Sample interval elapsed; running awake-mode cycle.");
    runSamplingCycle();
  }

  delay(250);
  return;
  }

  delay(1000);
}
