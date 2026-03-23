// Central compile-time configuration for the firmware.
//
// This header normalizes defaults from `secrets.h`, exposes project-wide timing
// and hardware constants, and keeps the rest of the modules from repeating the
// same macro/constexpr setup in multiple places.

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

#ifdef SUPABASE_ANON_KEY
  #error "SUPABASE_ANON_KEY has been replaced by SUPABASE_API_KEY. Update include/secrets.h to use SUPABASE_API_KEY."
#endif

#ifndef SUPABASE_API_KEY
  #error "SUPABASE_API_KEY must be defined in include/secrets.h"
#endif

#ifndef SUPABASE_EVENTS_TABLE
  #define SUPABASE_EVENTS_TABLE "device_events"
#endif

#ifndef N8N_CF_ACCESS_CLIENT_ID
  #define N8N_CF_ACCESS_CLIENT_ID ""
#endif

#ifndef N8N_CF_ACCESS_CLIENT_SECRET
  #define N8N_CF_ACCESS_CLIENT_SECRET ""
#endif

#ifndef SUPABASE_ROOT_CA_PEM
  #define SUPABASE_ROOT_CA_PEM ""
#endif

#ifndef N8N_WEBHOOK_ROOT_CA_PEM
  #define N8N_WEBHOOK_ROOT_CA_PEM ""
#endif

#ifndef DEBUG_WEBHOOK_ROOT_CA_PEM
  #define DEBUG_WEBHOOK_ROOT_CA_PEM ""
#endif

#ifndef ALLOW_INSECURE_HTTPS
  #define ALLOW_INSECURE_HTTPS 0
#endif

#ifndef VERBOSE_HTTP_LOGGING
  #define VERBOSE_HTTP_LOGGING 0
#endif

#define FW_VERSION "envnode-1.3.0"

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

#if defined(D3)
constexpr int SENSE_EN_PIN = D3;
#else
constexpr int SENSE_EN_PIN = 4;
#endif

#if defined(A0)
constexpr int VBAT_ADC_PIN = A0;
#else
constexpr int VBAT_ADC_PIN = 1;
#endif

constexpr float VBAT_DIVIDER_SCALE = 2.0f;
constexpr float VBAT_ADC_REF_V = 3.3f;
constexpr int VBAT_ADC_BITS = 12;
constexpr float LIPO_MIN_V = 3.0f;
constexpr float LIPO_MAX_V = 4.2f;
constexpr int VBAT_OVERSAMPLE = 8;

#ifndef LOW_BATTERY_ALERT_V
  #define LOW_BATTERY_ALERT_V 3.5f
#endif

#ifndef LOW_BATTERY_CLEAR_V
  #define LOW_BATTERY_CLEAR_V 3.65f
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

#ifndef USB_SERVICE_MODE_ENABLED
  #define USB_SERVICE_MODE_ENABLED 1
#endif

#ifndef USB_SERVICE_STATUS_INTERVAL_MS
  #define USB_SERVICE_STATUS_INTERVAL_MS 5000UL
#endif

constexpr bool DEBUG_MODE_ENABLED = DEVICE_DEBUG_MODE != 0;
constexpr bool DEEP_SLEEP_ENABLED = DISABLE_DEEP_SLEEP == 0;
constexpr bool ALLOW_INSECURE_HTTPS_REQUESTS =
    DEBUG_MODE_ENABLED || (ALLOW_INSECURE_HTTPS != 0);
constexpr uint32_t DEBUG_SAMPLE_INTERVAL = DEBUG_SAMPLE_INTERVAL_SECONDS;
constexpr uint32_t PRODUCTION_SAMPLE_INTERVAL = SAMPLE_INTERVAL_SECONDS;
constexpr uint32_t DEFAULT_SAMPLE_INTERVAL_SECONDS =
    DEBUG_MODE_ENABLED ? DEBUG_SAMPLE_INTERVAL : PRODUCTION_SAMPLE_INTERVAL;
constexpr uint32_t MIN_ALLOWED_SAMPLE_INTERVAL_SECONDS = MIN_SAMPLE_INTERVAL_SECONDS;
constexpr uint32_t MAX_ALLOWED_SAMPLE_INTERVAL_SECONDS = MAX_SAMPLE_INTERVAL_SECONDS;
constexpr const char* CONFIG_NAMESPACE = "envnode";
constexpr const char* SAMPLE_INTERVAL_KEY = "interval_s";
constexpr unsigned long WEBHOOK_COOLDOWN_MS = 1000UL;
constexpr uint16_t WEBHOOK_TIMEOUT_MS = 10000U;
constexpr bool DEBUG_WEBHOOKS = false;

#if defined(LED_BUILTIN)
constexpr int STATUS_LED_PIN = LED_BUILTIN;
constexpr bool STATUS_LED_AVAILABLE = true;
#else
constexpr int STATUS_LED_PIN = -1;
constexpr bool STATUS_LED_AVAILABLE = false;
#endif

constexpr uint8_t STATUS_LED_ON_LEVEL = HIGH;
constexpr uint8_t STATUS_LED_OFF_LEVEL = LOW;

#if defined(D9) && defined(D10)
constexpr int I2C_SDA_PIN = D9;
constexpr int I2C_SCL_PIN = D10;
#else
constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;
#endif

constexpr uint8_t WIFI_STATIC_IP_BYTES[4] = {WIFI_STATIC_IP};
constexpr uint8_t WIFI_GATEWAY_BYTES[4] = {WIFI_GATEWAY};
constexpr uint8_t WIFI_SUBNET_BYTES[4] = {WIFI_SUBNET};
constexpr uint8_t WIFI_DNS1_BYTES[4] = {WIFI_DNS1};
constexpr uint8_t WIFI_DNS2_BYTES[4] = {WIFI_DNS2};
