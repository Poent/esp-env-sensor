#pragma once
// Copy this file to secrets.h and fill in your credentials before building.

#define WIFI_SSID            "your-ssid"
#define WIFI_PASS            "your-wifi-password"
#define SUPABASE_URL         "https://your-project-id.supabase.co"
#define SUPABASE_API_KEY     "your-project-api-key"   // Project Settings → API → Project API keys
#define SUPABASE_TABLE       "readings"
#define DEVICE_ID            "esp32-env-node-01"
#define N8N_WEBHOOK_URL      "http://your-n8n-server/webhook-url"  // N8N webhook for error alerts

// Optional: override the default events table declared in src/main.cpp
// #define SUPABASE_EVENTS_TABLE "device_events"
