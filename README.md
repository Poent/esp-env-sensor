# ESP32 Environmental Sensor

This project is a proof of concept for logging environmental data to Supabase and then graphing that historical data using Grafana. It runs on an ESP32 and periodically samples a Bosch BME680 environmental sensor to log temperature, relative humidity, and barometric pressure to Supabase. It is built with the Arduino framework via PlatformIO and now uses the ESP32-S3 deep-sleep timer to wake, sample, upload, and return to sleep for low-power operation. The firmware also includes robust recovery routines that keep the sensor publishing even if I²C communication glitches occur.



## Final Dashboard on Grafana

<img width="1268" height="1123" alt="2025-09-27_12h36_09" src="https://github.com/user-attachments/assets/883bda60-dfb4-4abc-b0ba-9136d26e5e1b" />

## Setup Overview

### Hardware Setup

The hardware is very simple and consists only of two parts: a Seeed Studio XIAO ESP32-S3 and a BME680. In this example the XIAO uses pin 9 for I²C SDA and pin 10 for I²C SCL to the BME680 (D9 = GPIO8, D10 = GPIO9).

<img width="885" height="516" alt="2025-09-27_18h00_38" src="https://github.com/user-attachments/assets/2b477d58-fbf3-41f0-8232-650ef7979f20" />

### Software setup

Getting started requires configuring the development environment, backend storage, and visualization stack. Start by cloning this repository to a local folder and then opening the folder in VSCode. Ensure that the platformIO extension is installed, and then proceed to setting up Supabase and Grafana free instances. Setting up Supabase and Grafana is outside the scope of this project, but the following resources provide the canonical setup guides for each component:

- **PlatformIO:** Follow the [official PlatformIO IDE installation guide](https://docs.platformio.org/en/stable/integration/ide/vscode.html) for setup with VSCode
  or the [CLI quick start](https://docs.platformio.org/en/stable/core/quickstart.html) to prepare your build and upload tools.
- **Supabase:** Create a project and REST-enabled Postgres database using the [Supabase quickstart](https://supabase.com/docs/guides/getting-started/quickstarts). After the project is provisioned, run the SQL in [Supabase Schema Setup](#supabase-schema-setup) to create the required tables and policies.
- **Grafana:** Point Grafana at your Supabase database (or any compatible data source) by referencing the
  [Grafana data source documentation](https://grafana.com/docs/grafana/latest/datasources/) and relevant Postgres connection guide.

### Supabase Schema Setup

The firmware for the ESP32 sends environmental samples to a `readings` table and operational telemetry to a `device_events` table. Once your Supabase account, project, and database are setup you can create both tables (and enable inserts using your project's public API key) by executing the SQL below inside the Supabase SQL editor. Re-run the block if you redeploy into a fresh Supabase project.

```sql
-- Enables gen_random_uuid() for the device_events primary key.
create extension if not exists "pgcrypto";

create table public.readings (
  id bigint generated always as identity not null,
  device_id text not null,
  recorded_at timestamp with time zone not null default now(),
  temperature_c double precision not null,
  humidity_rh double precision not null,
  pressure_hpa double precision not null,
  constraint readings_pkey primary key (id)
);

create table public.device_events (
  id uuid not null default gen_random_uuid (),
  created_at timestamp with time zone not null default now(),
  device_id text not null,
  session_id text null,
  event_type text not null,
  severity text not null,
  message text null,
  reading_temp_c numeric null,
  reading_humidity_rh numeric null,
  reading_pressure_hpa numeric null,
  action text null,
  attempt smallint null,
  action_success boolean null,
  meta jsonb null,
  constraint device_events_pkey primary key (id),
  constraint device_events_severity_check check (
    (
      severity = any (
        array['info'::text, 'warning'::text, 'error'::text]
      )
    )
  )
);

```
> When Row Level Security is enabled you can still query the data from Grafana or other backends by creating additional policies (e.g., `for select`) scoped to the roles you use for analytics connections.

### Grafana Setup

[TODO] -> grafana dashboard configuration is included under "grafana\ESP32 Sensors-dashboard.json". Setting up grafana, connecting it to Supabase, and loading the dashboard layout is not currently described in the project. 

## Hardware

- **MCU:** Seeed Studio XIAO ESP32-S3 (tested with the `seeed_xiao_esp32s3` PlatformIO target).
- **Sensor:** Bosch BME680 connected over I²C (SDA on D9 / GPIO8, SCL on D10 / GPIO9).
- **Connectivity:** 2.4 GHz Wi-Fi network with internet access for Supabase REST API calls.

## Firmware Architecture

- `src/main.cpp` supports two operating modes. Production mode defaults to a 10-minute sample/upload cadence, while debug mode defaults to 60 seconds and can send a Discord heartbeat on every cycle.
- The firmware can either deep-sleep between cycles or stay awake and run the cadence from `loop()`. When the board is awake, the built-in LED stays on.
- Event logging helpers stream operational telemetry (startup, implausible readings, recovery attempts) to the Supabase `device_events` table. This allows for observability when recovering from I²C or sensor errors.
- Recovery flows perform plausibility checks, attempt soft resets, and reinitialize the sensor if measurements fall outside acceptable ranges.
- Production mode stores the effective sample interval in NVS so it can be overridden at runtime and survive resets and deep-sleep cycles.
- On successful cold boot, the built-in LED blinks three times and then remains on while the board stays awake.

## Configuration and Secrets

Copy the example secrets header into place and fill in your credentials before building:

```bash
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` with your Wi-Fi SSID/password, Supabase project URL, Supabase API key, preferred readings table, and device identifier. You can optionally override the default events table by defining `SUPABASE_EVENTS_TABLE`.

Runtime and network behavior can also be overridden in `include/secrets.h`:

- `DEVICE_DEBUG_MODE` switches the firmware into debug cadence/notification behavior.
- `SAMPLE_INTERVAL_SECONDS` sets the default production interval in seconds. The shipped default is `600` (10 minutes).
- `DEBUG_SAMPLE_INTERVAL_SECONDS` sets the default debug interval in seconds. The shipped default is `60`.
- `MIN_SAMPLE_INTERVAL_SECONDS` and `MAX_SAMPLE_INTERVAL_SECONDS` define the allowed bounds for runtime overrides.
- `DISABLE_DEEP_SLEEP` keeps the board awake between cycles and runs the schedule from `loop()`.
- `BME_TEMPERATURE_OFFSET_C` applies a fixed calibration offset to the reported temperature in Celsius. Leave it at `0.0f` unless you have compared the node against a stable reference and want to trim a known warm or cool bias.
- `DEBUG_DISCORD_WEBHOOK_URL` lets debug mode send a Discord heartbeat on each cycle.
- `WIFI_USE_STATIC_IP` together with `WIFI_STATIC_IP`, `WIFI_GATEWAY`, `WIFI_SUBNET`, and DNS settings removes the DHCP exchange on the device. A UniFi DHCP reservation keeps the address stable, but it does not eliminate the DHCP round trip.
- `SERIAL_CONFIG_WINDOW_MS` controls how long the firmware holds on non-timer boots before sensor/network work begins. During that window you can issue serial config commands or start a firmware upload. Set it to `0` to disable the boot hold entirely.
- `USB_SERVICE_MODE_ENABLED` enables a special service mode on non-timer boots when the board detects a computer host on the ESP32 USB CDC/JTAG interface.
- `USB_SERVICE_STATUS_INTERVAL_MS` controls how often service mode prints its local status summary.

When a serial monitor is attached during a non-timer boot, the firmware accepts these commands:

- `help`
- `interval`
- `interval <seconds>`
- `interval default`
- `mode`
- `status`
- `scan`
- `ping`
- `resolve <host>`
- `txpower`
- `reconnect`
- `sample`
- `sample upload`

> Supabase exposes project API keys under **Project Settings → API**. Use the "Generate new API key" action to rotate credentials and copy the fresh client key into `SUPABASE_API_KEY` so that it matches the latest Supabase recommendations.

> `include/secrets.h` is ignored by Git; keep your credentials private.

## Building and Uploading with PlatformIO

The project uses a single PlatformIO environment defined in `platformio.ini` (`xiao-esp32s3`). I personally use the [PlatformIO extension for VSCode](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide), but you can also build and upload from the command line with the `pio` CLI. 


If PlatformIO cannot auto-detect your serial port, pass `--upload-port /dev/ttyUSB0` (or the correct device on your system) to the upload command.

## Runtime Behavior

Successful Cold-Boot Serial Output Example:
```
0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:4744
load:0x40078000,len:15712
load:0x40080400,len:3152
entry 0x4008059c


Booting...
Sample interval: 600 seconds (default 600, allowed 60-86400)
WiFi: connecting...
WiFi: connected, IP=10.0.0.2
BME680 ready at I2C address 0x76
POST https://<database-path>.supabase.co/rest/v1/device_events -> 201
EVENT[startup/info]: logged
GOOD: T=24.48°C RH=39.1% P=828.8 hPa
Sleeping for 600 seconds...
```

- **Cadence:** In debug mode the board defaults to a 60-second sample/upload cadence. In production mode it defaults to 10 minutes unless you override it.
- **Awake vs sleep:** With deep sleep enabled, the device wakes, samples, uploads, and sleeps. With deep sleep disabled, it stays awake, keeps Wi-Fi warm, and runs the same cycle from `loop()`.
- **USB service mode:** On non-timer boots with a computer host attached over the ESP32 USB CDC/JTAG port, the firmware enters `usb_service` mode instead of sampling automatically. In this mode it stays awake, keeps serial commands active, connects to Wi-Fi for diagnostics, sends one informational paused-readings notification, and suppresses automatic polling, automatic fault alarms, and deep sleep until the host disconnects.
- **Wi-Fi speed:** The firmware caches the target BSSID/channel after a scan failure and can optionally use a static IP to avoid DHCP delay on future connects.
- **Cold boot behavior:** Successful cold boots log a startup event, optionally send the startup webhook, blink the built-in LED three times, and then leave the LED on while awake.
- **Debug notifications:** When `DEVICE_DEBUG_MODE=1` and `DEBUG_DISCORD_WEBHOOK_URL` is configured, each cycle also posts a Discord heartbeat with reading and upload status.
- **Supabase endpoints:** Readings are POSTed to `https://<your-project>.supabase.co/rest/v1/<table>` using your Supabase project's API key for authentication. Events follow the same pattern, defaulting to the `device_events` table unless overridden.
- **Session correlation:** Each wake generates a unique session ID combining the ESP32 MAC address and a random value to correlate events in Supabase.

### USB Service Mode

When `usb_service` mode is active, the recommended serial commands are:

- `mode` to confirm the runtime mode, USB-host state, Wi-Fi status, and sensor readiness.
- `sample` to take one local BME680 reading without uploading it.
- `sample upload` to take one reading and POST it once using the normal readings endpoint.

If the USB host is unplugged while the board remains powered by battery, the firmware automatically exits `usb_service` mode, resumes the normal startup path, performs a normal sample/upload cycle, and then returns to its configured awake/sleep behavior.

## Testing and Troubleshooting

- Use `pio device monitor` to inspect serial output. Successful uploads print `GOOD` lines with sensor values and HTTP status codes for Supabase requests.
- To validate USB service mode, boot the board from a computer USB port with the sensor intentionally unpowered or disconnected. You should see `usb_service` status output, no automatic BME init attempts, no automatic deep sleep, and one informational paused-readings notification after Wi-Fi connects.
- To validate manual sampling in service mode, keep the board on computer USB, power the sensor path you want to test, then run `sample` or `sample upload` from the serial monitor.
- To validate automatic resume, keep the board battery-powered, start in USB service mode from a computer, then unplug the USB host. The firmware should announce that the host detached, resume the normal sensing path, and either sleep or stay awake based on your current deep-sleep configuration.
- Seeing repeated "BME680 not found" or "implausible reading" messages usually indicates wiring or sensor issues. Verify power, SDA on pin 9, SCL on pin 10, and that you are using a BME680.
- If temperature reads consistently warm, the usual cause is local self-heating from the ESP32, regulator, or stagnant air around the breakout rather than a missing Bosch library compensation step. Increase physical separation from the MCU if possible, avoid enclosing the sensor near warm components, and only then apply a small `BME_TEMPERATURE_OFFSET_C` trim if the warm bias is repeatable.
- If Wi-Fi fails to connect, double-check your SSID/password in `include/secrets.h` and ensure the network allows the ESP32 MAC address.
- Supabase errors (HTTP codes outside 200–299) often mean the API key or table names are incorrect. Confirm your REST endpoint and permissions.

## Contributing

Contributions are welcome! Please open an issue to discuss substantial changes before submitting a pull request. Follow typical PlatformIO/Arduino coding conventions and avoid committing personal secrets.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.



