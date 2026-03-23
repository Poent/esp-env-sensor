// Wi-Fi connection and diagnostics implementation.
//
// This module owns ESP32 station setup, reconnect heuristics, scan/BSSID
// locking, and the serial-friendly diagnostics used in service mode.

#include "wifi_manager.h"

#include <ESP32Ping.h>

namespace {

// Returns true when the radio is in any station-capable mode.
bool isWiFiStaModeEnabled() {
  wifi_mode_t mode = WiFi.getMode();
  return mode == WIFI_STA || mode == WIFI_AP_STA;
}

// Converts a 4-byte IP macro into an Arduino `IPAddress`.
IPAddress makeIpAddress(const uint8_t bytes[4]) {
  return IPAddress(bytes[0], bytes[1], bytes[2], bytes[3]);
}

// Formats a MAC address for serial logs without pulling in extra helpers.
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

// Maps the configured TX power macro to the closest ESP32 enum value.
wifi_power_t configuredTxPower() {
  #if WIFI_TX_POWER_DBM >= 19
  return WIFI_POWER_19_5dBm;
  #elif WIFI_TX_POWER_DBM >= 17
  return WIFI_POWER_17dBm;
  #else
  return WIFI_POWER_15dBm;
  #endif
}

// Returns a printable label for the current TX power enum.
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

// Applies the configured TX power once station mode has been started.
void applyWiFiTxPower() {
  wifi_power_t power = configuredTxPower();
  WiFi.setTxPower(power);
  Serial.printf("WiFi: applied TX power %s\n", txPowerName(power));
}

// Prints the current IP-layer connection details for diagnostics.
void printWiFiNetworkSummary() {
  Serial.printf(
      "WiFi config: IP=%s Gateway=%s Subnet=%s DNS1=%s DNS2=%s MAC=%s RSSI=%d dBm BSSID=%s CH=%d\n",
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

// Applies the project's preferred station-mode and IP/DNS configuration before
// a connection attempt.
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

// Flags disconnect reasons that usually point to association/auth problems
// rather than IP-layer issues.
bool isAuthOrAssocDisconnectReason(uint16_t reason) {
  switch (reason) {
    case 2:
    case 4:
    case 8:
    case 15:
    case 203:
      return true;
    default:
      return false;
  }
}

// Clears any remembered BSSID/channel lock so the next connect can roam freely.
void clearLockedBssid() {
  memset(gApp.targetBssid, 0, sizeof(gApp.targetBssid));
  gApp.hasTargetBssid = false;
  gApp.targetChannel = 0;
}

// Ensures the STA interface is running, optionally forcing a clean restart.
void ensureWiFiStaReady(bool forceRestart) {
  if (forceRestart && WiFi.getMode() != WIFI_OFF) {
    gApp.wifiHasConfiguredSta = false;
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

// Pings a single address and prints the result to serial.
void logPingResult(const char* label, const IPAddress& address) {
  Serial.printf("Ping test: %s (%s) ... ", label, address.toString().c_str());
  bool ok = Ping.ping(address, 1);
  if (ok) {
    Serial.printf("ok, avg=%u ms\n", Ping.averageTime());
  } else {
    Serial.printf("failed, avg=%u ms\n", Ping.averageTime());
  }
}

}  // namespace

// Converts an Arduino Wi-Fi status enum into a stable log/telemetry string.
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

// Prints a status message plus the human-readable Wi-Fi state name.
void logWiFiStatus(const char* prefix, wl_status_t status) {
  Serial.printf("%s%s (%d)\n", prefix, wifiStatusName(status), static_cast<int>(status));
}

// Prints the active Wi-Fi transmit power for diagnostics.
void printTxPowerSummary() {
  wifi_power_t currentPower = WiFi.getTxPower();
  Serial.printf("WiFi TX power: %s (%d)\n", txPowerName(currentPower),
                static_cast<int>(currentPower));
}

// Runs a manual set of gateway/public/DNS connectivity checks.
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

// Prints the current Wi-Fi status, network details, and TX power.
void printWiFiDiagnostics() {
  Serial.printf("WiFi status: %s (%d)\n", wifiStatusName(WiFi.status()),
                static_cast<int>(WiFi.status()));
  printWiFiNetworkSummary();
  Serial.printf("Last disconnect reason=%u\n",
                static_cast<unsigned>(gApp.lastWiFiDisconnectReason));
  printTxPowerSummary();
}

// Turns off the Wi-Fi radio and clears connection-tracking state.
void shutdownWiFi() {
  gApp.networkAvailable = false;
  gApp.wifiHasConfiguredSta = false;
  if (isWiFiStaModeEnabled() && WiFi.isConnected()) {
    WiFi.disconnect(false, false);
    delay(50);
  }
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.mode(WIFI_OFF);
  }
}

// Hooks the ESP32 Wi-Fi event stream once so disconnect reasons and state
// changes are visible over serial.
void registerWiFiEventLogger() {
  if (gApp.wifiEventLoggerHandle != 0) {
    return;
  }

  gApp.wifiEventLoggerHandle = WiFi.onEvent([](arduino_event_id_t event,
                                               arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        Serial.println("WiFi event: STA start");
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        gApp.lastWiFiDisconnectReason = 0;
        Serial.printf("WiFi event: STA connected on channel %u, authmode=%u\n",
                      static_cast<unsigned>(info.wifi_sta_connected.channel),
                      static_cast<unsigned>(info.wifi_sta_connected.authmode));
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        gApp.lastWiFiDisconnectReason =
            static_cast<uint16_t>(info.wifi_sta_disconnected.reason);
        Serial.printf("WiFi event: STA disconnected, reason=%u (%s)\n",
                      static_cast<unsigned>(info.wifi_sta_disconnected.reason),
                      WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(
                          info.wifi_sta_disconnected.reason)));
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("WiFi event: got IP %s\n",
                      IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
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

// Scans nearby networks, prints the results, and remembers the best matching
// BSSID/channel for faster future associations.
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
    uint8_t* bssidPtr = WiFi.BSSID(index);
    if (bssidPtr) {
      memcpy(bssid, bssidPtr, sizeof(bssid));
    }

    bool isTarget = ssid == WIFI_SSID;
    if (isTarget) {
      foundTarget = true;
      if (rssi > bestTargetRssi) {
        memcpy(gApp.targetBssid, bssid, sizeof(gApp.targetBssid));
        gApp.targetChannel = channel;
        gApp.hasTargetBssid = true;
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
    gApp.hasTargetBssid = false;
    gApp.targetChannel = 0;
    Serial.printf("WiFi scan: target SSID '%s' not visible to the ESP32 radio.\n",
                  WIFI_SSID);
  } else {
    Serial.printf("WiFi scan: locking target BSSID=%s CH=%ld for next association attempt.\n",
                  formatMacAddress(gApp.targetBssid).c_str(),
                  static_cast<long>(gApp.targetChannel));
  }

  WiFi.scanDelete();
}

// Connects or reconnects station mode, applying the project's heuristics for
// BSSID locking, restart-on-failure, and scan-after-repeat-failure.
bool connectWiFi(unsigned long timeoutMs) {
  configureWiFiNetworkStack();

  wl_status_t preStatus = WiFi.status();
  bool authOrAssocIssue =
      isAuthOrAssocDisconnectReason(gApp.lastWiFiDisconnectReason);
  bool forceRestart = preStatus == WL_CONNECT_FAILED ||
                      preStatus == WL_CONNECTION_LOST ||
                      preStatus == WL_DISCONNECTED || authOrAssocIssue;
  ensureWiFiStaReady(forceRestart);

  if (authOrAssocIssue && gApp.hasTargetBssid) {
    Serial.printf("WiFi: clearing locked BSSID after disconnect reason %u.\n",
                  static_cast<unsigned>(gApp.lastWiFiDisconnectReason));
    clearLockedBssid();
  }

  bool useLockedBssid = gApp.hasTargetBssid && gApp.targetChannel > 0;
  if (!gApp.wifiHasConfiguredSta) {
    if (useLockedBssid) {
      Serial.printf("WiFi: associating to locked BSSID=%s CH=%ld\n",
                    formatMacAddress(gApp.targetBssid).c_str(),
                    static_cast<long>(gApp.targetChannel));
      WiFi.begin(WIFI_SSID, WIFI_PASS, gApp.targetChannel, gApp.targetBssid, true);
    } else {
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    applyWiFiTxPower();
    gApp.wifiHasConfiguredSta = true;
  } else if (!WiFi.reconnect()) {
    Serial.println("WiFi: reconnect request failed; restarting STA and reapplying credentials.");
    ensureWiFiStaReady(true);
    if (useLockedBssid) {
      Serial.printf("WiFi: reassociating to locked BSSID=%s CH=%ld\n",
                    formatMacAddress(gApp.targetBssid).c_str(),
                    static_cast<long>(gApp.targetChannel));
      WiFi.begin(WIFI_SSID, WIFI_PASS, gApp.targetChannel, gApp.targetBssid, true);
    } else {
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    applyWiFiTxPower();
    gApp.wifiHasConfiguredSta = true;
  }

  Serial.print("WiFi: connecting");
  unsigned long connectStartedAt = millis();
  unsigned long start = millis();
  wl_status_t lastStatus = WiFi.status();
  gApp.lastReportedWiFiStatus = lastStatus;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
    wl_status_t currentStatus = WiFi.status();
    if (currentStatus != lastStatus) {
      Serial.println();
      logWiFiStatus("WiFi: status -> ", currentStatus);
      lastStatus = currentStatus;
      gApp.lastReportedWiFiStatus = currentStatus;
    }
  }

  wl_status_t finalStatus = WiFi.status();
  gApp.lastReportedWiFiStatus = finalStatus;
  gApp.networkAvailable = finalStatus == WL_CONNECTED;
  if (!gApp.networkAvailable) {
    ++gApp.wifiConnectFailures;
    Serial.println();
    Serial.printf("WiFi: connection timed out; status=%s (%d).\n",
                  wifiStatusName(finalStatus),
                  static_cast<int>(finalStatus));
    if (authOrAssocIssue) {
      Serial.printf("WiFi: last disconnect reason=%u suggests AP/auth negotiation trouble rather than DHCP.\n",
                    static_cast<unsigned>(gApp.lastWiFiDisconnectReason));
    }

    if (finalStatus == WL_NO_SSID_AVAIL || finalStatus == WL_CONNECT_FAILED ||
        finalStatus == WL_DISCONNECTED || gApp.wifiConnectFailures >= 2) {
      logWiFiScanResults();
    }

    #if !DISABLE_DEEP_SLEEP
    shutdownWiFi();
    #endif
    return false;
  }

  gApp.wifiConnectFailures = 0;
  gApp.lastWiFiDisconnectReason = 0;
  Serial.printf("\nWiFi: connected, IP=%s in %lu ms\n",
                WiFi.localIP().toString().c_str(),
                static_cast<unsigned long>(millis() - connectStartedAt));
  printWiFiNetworkSummary();
  printTxPowerSummary();
  return true;
}
