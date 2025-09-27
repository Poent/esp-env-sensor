# ESP32 Environmental Sensor

This project runs on an ESP32 and periodically samples a Bosch BME280 environmental sensor to log temperature, relative humidity, and barometric pressure to Supabase. It is built with the Arduino framework via PlatformIO and includes robust recovery routines that keep the sensor publishing even if I²C communication glitches occur.

Grafana pointed at supabase database:

<img width="1268" height="1123" alt="2025-09-27_12h36_09" src="https://github.com/user-attachments/assets/883bda60-dfb4-4abc-b0ba-9136d26e5e1b" />

## Setup Overview

Getting started requires configuring the development environment, backend storage, and visualization stack. The following
resources provide the canonical setup guides for each component:

- **PlatformIO:** Follow the [official PlatformIO IDE installation guide](https://docs.platformio.org/en/stable/integration/ide/vscode.html)
  or the [CLI quick start](https://docs.platformio.org/en/stable/core/quickstart.html) to prepare your build and upload tools.
- **Supabase:** Create a project and REST-enabled Postgres database using the [Supabase quickstart](https://supabase.com/docs/guides/getting-started/quickstarts).
- **Grafana:** Point Grafana at your Supabase database (or any compatible data source) by referencing the
  [Grafana data source documentation](https://grafana.com/docs/grafana/latest/datasources/) and relevant Postgres connection guide.

## Hardware

- **MCU:** ESP32 development board (tested with the `esp32dev` PlatformIO target).
- **Sensor:** Bosch BME280 connected over I²C (SDA on GPIO 21, SCL on GPIO 22 by default).
- **Connectivity:** 2.4 GHz Wi-Fi network with internet access for Supabase REST API calls.

## Firmware Architecture

- `src/main.cpp` drives the application loop. On boot it connects to Wi-Fi, initializes the BME280, and logs a startup event to Supabase.
- Sensor readings are captured once per minute (`SEND_EVERY_MS = 60000`) and posted to the Supabase `readings` table. Each payload carries `temperature_c`, `humidity_rh`, and `pressure_hpa` fields together with the `device_id`.
- Event logging helpers stream operational telemetry (startup, implausible readings, recovery attempts) to the Supabase `device_events` table. This allows for observability when recovering from I²C or sensor errors.
- Recovery flows perform plausibility checks, attempt soft resets, and reinitialize the sensor if measurements fall outside acceptable ranges.

## Configuration and Secrets

Copy the example secrets header into place and fill in your credentials before building:

```bash
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` with your Wi-Fi SSID/password, Supabase project URL, anon key, preferred readings table, and device identifier. You can optionally override the default events table by defining `SUPABASE_EVENTS_TABLE`.

> ⚠️ `include/secrets.h` is ignored by Git; keep your credentials private.

## Building and Uploading with PlatformIO

The project uses a single PlatformIO environment defined in `platformio.ini` (`esp32-wroom-32e`). I personally use the [PlatformIO extension for VSCode](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide), but you can also build and upload from the command line with the `pio` CLI. 


If PlatformIO cannot auto-detect your serial port, pass `--upload-port /dev/ttyUSB0` (or the correct device on your system) to the upload command.

## Runtime Behavior

Successful Boot Serial Output Example:
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
WiFi: connecting...
WiFi: connected, IP=10.0.0.2
BME sensor ID: 0x60
POST https://<database-path>.supabase.co/rest/v1/device_events -> 201
EVENT[startup/info]: logged
GOOD: T=24.48°C RH=39.1% P=828.8 hPa
```

- **Cadence:** The loop publishes fresh readings once per minute. Implausible data triggers up to three retries before the firmware attempts sensor recovery.
- **Supabase endpoints:** Readings are POSTed to `https://<your-project>.supabase.co/rest/v1/<table>` using the anon key for authentication. Events follow the same pattern, defaulting to the `device_events` table unless overridden.
- **Session correlation:** Each boot generates a unique session ID combining the ESP32 MAC address and a random value to correlate events in Supabase.

## Testing and Troubleshooting

- Use `pio device monitor` to inspect serial output. Successful uploads print `GOOD` lines with sensor values and HTTP status codes for Supabase requests.
- Seeing repeated "BME280 not found" or "implausible reading" messages usually indicates wiring or sensor issues. Verify power, SDA/SCL connections, and that you are using a BME280 (not BMP280).
- If Wi-Fi fails to connect, double-check your SSID/password in `include/secrets.h` and ensure the network allows the ESP32 MAC address.
- Supabase errors (HTTP codes outside 200–299) often mean the anon key or table names are incorrect. Confirm your REST endpoint and permissions.

## Contributing

Contributions are welcome! Please open an issue to discuss substantial changes before submitting a pull request. Follow typical PlatformIO/Arduino coding conventions and avoid committing personal secrets.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.



