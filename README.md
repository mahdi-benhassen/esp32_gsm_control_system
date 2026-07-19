# ESP32 GSM Agriculture Control System

ESP-IDF firmware for a GSM/SMS-controlled smart irrigation controller, designed
for **Kincony KC868-A2** style hardware (ESP32 + SIM800L GSM + 2 relays +
analog/digital inputs). Written with native Espressif tools only — no Arduino,
no external components. Built in CI with the official `espressif/idf` image.

## Features

- **AUTO irrigation** — pump + valve start when soil moisture drops below a
  configurable threshold; stop at threshold + hysteresis
- **Safety limits** — pump max runtime cut-off, minimum cycle interval,
  tank-full float switch override
- **SMS remote control** — commands for pump, valve, mode, threshold, status
- **Authorization** — a registered master phone number; other numbers ignored
- **Persistent settings** — mode / threshold / master number stored in NVS
- **Sensors** — capacitive soil moisture (ADC), DHT22 temperature/humidity,
  float switch, spare analog input
- **Self-contained SIM800 AT driver** — UART + FreeRTOS, thread-safe
- **GitHub Actions CI/CD** — every push builds firmware artifacts; pushing a
  `v*` tag publishes a GitHub release with the firmware

## Hardware

Pin map follows the **official Kincony KC868-A2 I/O definition** (KC868-A2
V2.x schematic / Kincony forum):

| Function            | ESP32 GPIO | Notes                                        |
|---------------------|-----------:|----------------------------------------------|
| SIM800 TX → ESP RX  | GPIO 34    | UART2, 115200 baud (input-only pin = OK)      |
| SIM800 RX ← ESP TX  | GPIO 13    |                                               |
| Relay 1 — pump      | GPIO 15    | active high                                   |
| Relay 2 — valve     | GPIO 2     | active high                                   |
| Soil moisture (an.) | GPIO 32    | ADC1_CH4, 0–3.1 V · shared with RS485 TX      |
| Aux analog input    | GPIO 35    | ADC1_CH7 · shared with RS485 RX               |
| DHT22 data          | GPIO 33    | 1-wire terminal 1, 4.7 kΩ pull-up recommended |
| Float switch        | GPIO 36    | digital input DI1 (input-only pin)            |
| Spare digital in    | GPIO 39    | digital input DI2 (input-only pin)            |
| Status LED          | GPIO 14    | 1-wire terminal 2 · 1 Hz OK · 5 Hz = no GSM   |

> ⚠️ On the stock KC868-A2, GPIO32/35 are wired to the RS485 transceiver:
> use them as analog inputs only when RS485 is not in use. Otherwise attach
> an I2C ADC (e.g. ADS1115) on the I2C header (SDA=GPIO4, SCL=GPIO16).
> Verify the pin map against your exact board revision's schematic and
> adjust `main/board_config.h` if needed. The SIM800L needs a solid
> 4 V / 2 A supply — power it from the KC868 board, not from the ESP32
> 3.3 V rail.

### Soil moisture calibration

Edit `SOIL_MV_DRY` / `SOIL_MV_WET` in `main/board_config.h`: read the log or
send `STATUS` while the probe is in open air (dry value) and in water
(wet value). Default firmware logs raw percent; for raw millivolts enable
debug logs in the `sensors` tag.

## SMS commands

Send from any phone to the SIM card number (text mode):

| Command           | Effect                                              |
|-------------------|-----------------------------------------------------|
| `MASTER 1234`     | Register sender as authorized number (default PIN)  |
| `HELP`            | Reply with command list                             |
| `STATUS`          | Reply: mode, relays, moisture, T/H, signal, uptime  |
| `PUMP ON` / `OFF` | Manual pump control (switches to MANUAL mode)       |
| `VALVE ON` / `OFF`| Manual valve control (switches to MANUAL mode)      |
| `AUTO`            | Automatic irrigation mode                           |
| `MANUAL`          | Manual mode                                         |
| `THRESH 35`       | Set moisture threshold to 35 % (persisted)          |
| `UNMASTER`        | Clear master number (from master only)              |

Change the default PIN via `idf.py menuconfig` → *Agriculture Controller
Configuration* → *PIN for MASTER registration*.

## Build & flash (local)

```bash
# ESP-IDF v5.3 environment active (export.sh / export.bat)
idf.py set-target esp32        # only needed the first time
idf.py menuconfig              # optional: PIN, thresholds, intervals
idf.py build
idf.py -p COM5 flash monitor   # Linux: /dev/ttyUSB0
```

## GitHub Actions CI/CD

`.github/workflows/build.yml`:

- **push / PR to `main` (or `master`)** → builds inside the
  `espressif/idf:v5.3.1` container, merges a single flash image
  (`firmware_merged.bin`), and uploads all binaries + ELF + flasher args as
  workflow artifacts (Actions → run → *Artifacts*).
- **push a tag like `v1.0.0`** → additionally creates a GitHub release with
  the firmware zip attached.

```bash
git tag v1.0.0
git push origin v1.0.0     # triggers build + release
```

Flash the merged image at offset `0x0`:

```bash
esptool.py --chip esp32 -p COM5 write_flash 0x0 firmware_merged.bin
```

## Project structure

```
├── .github/workflows/build.yml   CI/CD pipeline
├── CMakeLists.txt                project root
├── sdkconfig.defaults            target esp32, 4 MB flash, custom partitions
├── partitions.csv
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild         menuconfig: PIN, thresholds, intervals
    ├── app_main.c                boot sequence
    ├── board_config.h            pin map + calibration (EDIT THIS)
    ├── gsm_modem.c/.h            SIM800 AT driver (UART, SMS send/receive)
    ├── sensors.c/.h              DHT22 bit-bang, ADC moisture, float switch
    ├── control.c/.h              irrigation state machine + SMS commands
    └── config_store.c/.h         NVS persistence
```

## Notes / tuning

- ESP-IDF v5.3 APIs are used (`esp_adc/adc_oneshot.h`, `esp_rom_delay_us`, …).
- The modem driver polls the SMS inbox (`AT+CMGL`) instead of parsing URCs —
  robust against timing races. Interval is configurable in menuconfig.
- To extend to MQTT/HTTP over GPRS, add `AT+CIP*`/`AT+HTTP*` sequences in
  `gsm_modem.c` (or migrate to the `esp_modem` component with PPP).

MIT-style, use freely in your own products.
