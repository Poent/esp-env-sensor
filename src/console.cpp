// Serial command parser and dispatcher.
//
// The console is intentionally limited to startup/service-mode diagnostics and
// small configuration changes so the main runtime path stays deterministic.

#include "console.h"

#include "app_context.h"
#include "hardware.h"
#include "runtime.h"
#include "wifi_manager.h"

namespace {

// Prints the supported serial commands shown during startup and service mode.
void printSerialConfigHelp() {
  Serial.println("Serial config commands:");
  Serial.println("  help               Show available commands");
  Serial.println("  interval           Print the active sample interval");
  Serial.println("  interval <seconds> Persist a new sample interval");
  Serial.println("  interval default   Clear the stored override");
  Serial.println("  mode               Print runtime mode / USB / sensor state");
  Serial.println("  status             Print WiFi/IP/tx power details");
  Serial.println("  scan               Scan nearby WiFi networks");
  Serial.println("  ping               Ping gateway, 1.1.1.1, and google.com");
  Serial.println("  resolve <host>     Resolve a hostname");
  Serial.println("  txpower            Print configured WiFi TX power");
  Serial.println("  reconnect          Restart STA and reconnect WiFi");
  Serial.println("  sample             Take one local sensor reading (USB service mode)");
  Serial.println("  sample upload      Take one reading and upload it once (USB service mode)");
  Serial.println("  voltage            Read and display battery voltage + charge %");
}

// Parses one complete serial command line and dispatches it to the appropriate
// subsystem. `rawCommand` may contain whitespace and trailing newlines.
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

  if (command.equalsIgnoreCase("mode")) {
    printRuntimeModeStatus();
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

  if (command.equalsIgnoreCase("sample upload")) {
    performManualSample(true);
    return;
  }

  if (command.equalsIgnoreCase("sample")) {
    performManualSample(false);
    return;
  }

  if (command.equalsIgnoreCase("voltage")) {
    enableSensePower();
    delay(10);
    float voltage = readBatteryVoltage();
    float percent = batteryVoltageToPercent(voltage);
    disableSensePower();
    Serial.printf("Battery: %.3fV (%.1f%%)\n", voltage, percent);
    return;
  }

  if (command.startsWith("resolve ")) {
    String host = command.substring(strlen("resolve "));
    host.trim();
    if (!host.length()) {
      Serial.println("Usage: resolve <hostname>");
      return;
    }

    if (!gApp.networkAvailable) {
      Serial.println("Resolve skipped: WiFi unavailable");
      return;
    }

    IPAddress resolved;
    if (WiFi.hostByName(host.c_str(), resolved)) {
      Serial.printf("Resolved %s -> %s\n", host.c_str(),
                    resolved.toString().c_str());
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
      Serial.println(cleared ? "Sample interval override cleared."
                             : "Failed to clear sample interval override.");
      printSampleIntervalConfig();
      return;
    }

    char* endPtr = nullptr;
    unsigned long parsed = strtoul(arg.c_str(), &endPtr, 10);
    bool parseFailed =
        arg.length() == 0 || endPtr == nullptr || (*endPtr != '\0');
    if (parseFailed || parsed == 0) {
      Serial.println(
          "Invalid interval. Use an integer number of seconds or 'interval default'.");
      return;
    }

    uint32_t sanitized =
        sanitizeSampleIntervalSeconds(static_cast<uint32_t>(parsed));
    bool saved = saveSampleIntervalSeconds(sanitized);
    if (!saved) {
      Serial.println("Failed to save sample interval.");
      return;
    }

    if (sanitized != parsed) {
      Serial.printf("Sample interval clamped to %lu seconds before saving.\n",
                    static_cast<unsigned long>(sanitized));
    }
    printSampleIntervalConfig();
    return;
  }

  Serial.println("Unknown command. Type 'help' for supported commands.");
}

}  // namespace

// Consumes bytes from the serial port until complete newline-delimited commands
// can be dispatched.
void pollSerialCommands() {
  while (Serial.available()) {
    char ch = static_cast<char>(Serial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      handleSerialCommand(gApp.serialInputBuffer);
      gApp.serialInputBuffer = "";
      continue;
    }
    gApp.serialInputBuffer += ch;
  }
}

// Keeps the device awake for a short startup window so manual commands or
// firmware uploads can begin before automation starts.
void handleSerialConfigWindow() {
  if (gApp.bootMode == BootMode::TimerWake || SERIAL_CONFIG_WINDOW_MS == 0) {
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

  if (gApp.serialInputBuffer.length()) {
    handleSerialCommand(gApp.serialInputBuffer);
    gApp.serialInputBuffer = "";
  }
}
