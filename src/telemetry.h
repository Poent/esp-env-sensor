// Outbound telemetry helpers for Supabase rows, device events, and webhooks.
//
// This module owns HTTP/TLS behavior so the runtime can focus on when to report
// information rather than how requests are built or authenticated.

#pragma once

#include "app_context.h"

// Performs a lightweight readiness check against the configured Supabase tables.
bool checkSupabaseTablesOnce();

// Posts one accepted reading to the configured readings table.
bool postReadings(const SensorReadings& readings);

// Posts an operational event to the events table. Optional fields allow the
// caller to attach a reading snapshot, action name, attempt count, and JSON
// metadata when those details are available.
bool postEvent(const char* eventType,
               const char* severity,
               const String& message,
               const SensorReadings* snapshot = nullptr,
               const char* action = nullptr,
               int attempt = 0,
               bool actionSuccess = false,
               const char* metaJson = nullptr);

// Sends a webhook notification for startup, errors, recovery, battery alerts,
// or debug heartbeats.
bool sendWebhook(const char* alertType,
                 const String& message,
                 const char* severity = "info",
                 const SensorReadings* readings = nullptr,
                 const char* extraData = nullptr);

// Sends the debug heartbeat to Discord or the structured webhook target.
bool sendDebugDiscordMessage(const SensorReadings* readings,
                             bool uploadOk,
                             unsigned long cycleStartedAtMs);

// Builds JSON metadata describing the current boot and first-reading outcome.
String buildBootMetaJson(bool firstReadingFailed);

// Builds JSON metadata describing USB service mode and its paused state.
String buildServiceModeMetaJson();

// Sends a small set of synthetic webhook examples for manual endpoint testing.
void testWebhooks();
