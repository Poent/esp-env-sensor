// Wi-Fi bring-up, reconnect, scan, and diagnostics helpers.
//
// This module hides the ESP32 station-state details from the runtime layer so
// sampling code can simply ask for connectivity or print diagnostics.

#pragma once

#include "app_context.h"

// Connects to the configured SSID and waits up to `timeoutMs` for success.
bool connectWiFi(unsigned long timeoutMs = WIFI_CONNECT_TIMEOUT_MS);

// Registers the one-time Wi-Fi event logger used for serial diagnostics.
void registerWiFiEventLogger();

// Shuts down station mode so the radio is off before deep sleep.
void shutdownWiFi();

// Returns a printable name for the current Arduino Wi-Fi status code.
const char* wifiStatusName(wl_status_t status);

// Prints a Wi-Fi status line with a caller-provided prefix.
void logWiFiStatus(const char* prefix, wl_status_t status);

// Scans nearby networks and captures the best BSSID/channel for the target SSID.
void logWiFiScanResults();

// Runs manual ping/DNS diagnostics from the serial console.
void runConnectivityChecks();

// Prints the current Wi-Fi/IP/TX-power summary for diagnostics.
void printWiFiDiagnostics();

// Prints the current configured Wi-Fi transmit power.
void printTxPowerSummary();
