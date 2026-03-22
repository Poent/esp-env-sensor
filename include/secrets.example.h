#pragma once
// Copy this file to secrets.h and fill in your credentials before building.

#define WIFI_SSID            "your-ssid"
#define WIFI_PASS            "your-wifi-password"
#define SUPABASE_URL         "https://your-project-id.supabase.co"
#define SUPABASE_API_KEY     "your-project-api-key"   // Project Settings → API → Project API keys
#define SUPABASE_TABLE       "readings"
#define DEVICE_ID            "esp32-env-node-01"
#define N8N_WEBHOOK_URL      "https://your-n8n-domain/webhook-url"  // N8N webhook for alerts

// Optional Cloudflare Access service-token headers for N8N webhooks.
// Define both values together when the N8N endpoint is protected by Access.
// #define N8N_CF_ACCESS_CLIENT_ID     "your-cloudflare-access-client-id"
// #define N8N_CF_ACCESS_CLIENT_SECRET "your-cloudflare-access-client-secret"

// Optional low-power tuning overrides.
// Default production wake/upload interval is 600 seconds (10 minutes).
// #define SAMPLE_INTERVAL_SECONDS 600
// Debug mode uses its own default interval.
// #define DEBUG_SAMPLE_INTERVAL_SECONDS 60
// Low-battery alert defaults: warn at 3.50 V and clear at 3.65 V.
// #define LOW_BATTERY_ALERT_V 3.5f
// #define LOW_BATTERY_CLEAR_V 3.65f
// #define MIN_SAMPLE_INTERVAL_SECONDS 60
// #define MAX_SAMPLE_INTERVAL_SECONDS 86400

// Debug mode is selected by building the `xiao-esp32s3-debug` environment in
// platformio.ini. In debug mode the firmware posts a heartbeat to Discord on
// each cycle (if DEBUG_DISCORD_WEBHOOK_URL is defined) and uses
// DEBUG_SAMPLE_INTERVAL_SECONDS as the default cadence.
// #define DEBUG_DISCORD_WEBHOOK_URL "https://discord.com/api/webhooks/..."

// Optional static IP configuration. A DHCP reservation in UniFi keeps the same
// address, but enabling this removes the DHCP exchange on the ESP itself.
// #define WIFI_USE_STATIC_IP 1
// #define WIFI_STATIC_IP 10,0,0,50
// #define WIFI_GATEWAY 10,0,0,1
// #define WIFI_SUBNET 255,255,255,0
// #define WIFI_DNS1 1,1,1,1
// #define WIFI_DNS2 8,8,8,8
// #define WIFI_OVERRIDE_DNS 1
// #define WIFI_TX_POWER_DBM 15

// Optional serial config window on non-timer boots. Set to 0 to disable.
// Supported commands: `help`, `interval`, `interval <seconds>`, `interval default`,
// `mode`, `status`, `scan`, `ping`, `resolve <host>`, `txpower`, `reconnect`,
// `sample`, and `sample upload`
// #define SERIAL_CONFIG_WINDOW_MS 5000

// Enable USB host service mode on non-timer boots when the board is attached
// to a computer over the ESP32 USB CDC/JTAG interface. In this mode automatic
// readings are paused, the board stays awake, and serial commands remain active.
// #define USB_SERVICE_MODE_ENABLED 1
// #define USB_SERVICE_STATUS_INTERVAL_MS 5000

// Optional debug hold before entering deep sleep. Useful when the board only
// enumerates on COM7 while awake and you need time to attach a monitor.
// #define DEBUG_AWAKE_WINDOW_MS 30000

// Keep the board awake between cycles. In this mode the built-in LED stays on
// while the firmware is awake, and `loop()` schedules periodic uploads.
// #define DISABLE_DEEP_SLEEP 1

// Optional: override the default events table declared in src/main.cpp
// #define SUPABASE_EVENTS_TABLE "device_events"
