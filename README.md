# NTP_2

`NTP_2` is the ESP32-side companion firmware for `myMatrixClock2`.
It synchronizes local Berlin time over Wi-Fi via NTP and forwards validated
timestamps to the Teensy over a custom bit-banged SPI link.

## Features

- Wi-Fi based NTP synchronization on ESP32
- Berlin timezone handling with automatic `CET` / `CEST`
- custom 32-byte SPI frame transport to the Teensy
- one immediate transfer after successful sync
- further automatic transfers only when the minute changes
- USB serial monitor commands for manual sends and regression tests
- reply/status polling from the Teensy (`0x01`, `0x02`, `0x03`)

## Repository Layout

- `src/ntp_2.cpp`
  Main firmware for Wi-Fi, NTP, SPI transfer, and serial command handling.
- `include/credential.h`
  Local Wi-Fi credentials, intentionally ignored by git.
- `include/credential.example.h`
  Safe template for creating `credential.h`.
- `platformio.ini`
  PlatformIO environment for `esp32dev`.

## Hardware

- ESP32 development board (`board = esp32dev`)
- Wi-Fi access to one of the configured NTP servers
- USB connection for flashing and serial monitoring
- SPI wiring to the Teensy clock firmware

### SPI Wiring

Current ESP32 pin assignment:

- ESP32 `GPIO5`  -> Teensy `CS`
- ESP32 `GPIO23` -> Teensy `SIN`
- ESP32 `GPIO19` <- Teensy `SOUT`
- ESP32 `GPIO18` -> Teensy `CLK`
- common `GND`

### Time Transfer Format

The ESP32 sends timestamps as ASCII:

```text
YYYY-MM-DD HH:MM:SS
```

These bytes are packed into a fixed 32-byte SPI frame:

- bytes `0..18`: timestamp text
- byte `19`: null terminator
- remaining bytes: zero padding

## Configuration

Create a local credentials file from the template:

```cpp
// include/credential.h
const char* ssid = "your-wifi-name";
const char* password = "your-wifi-password";
```

`include/credential.h` stays ignored by git.

## Runtime Behavior

At startup the firmware:

1. starts USB serial at `115200`
2. configures the custom SPI GPIO lines
3. connects to Wi-Fi
4. tries the configured NTP servers until one returns valid local time
5. sends the current local timestamp to the Teensy
6. keeps servicing time locally and sends again only on minute change

If sync fails, the firmware retries after `30` seconds.

## Serial Monitor Commands

The ESP32 monitor supports:

- `help`
- `now`
- `test`
- `test <name>`
- `invalid`
- `send <YYYY-MM-DD HH:MM:SS>`

The canned tests currently include:

- `winter`
- `summer`
- `dst-start-before`
- `dst-start-at`
- `dst-end-before`
- `dst-end-at`
- `invalid-date`
- `invalid-format`
- `invalid-terminator`

## NTP Servers

The firmware tries these servers in order:

- `fritz.box`
- `0.europe.pool.ntp.org`
- `1.europe.pool.ntp.org`
- `0.pool.ntp.org`
- `1.pool.ntp.org`

## Build

From the repository root:

```bash
pio run -e esp32dev
```

The current `platformio.ini` contains local `upload_port` and `monitor_port`
settings (`COM8`). Adjust them to match your system before flashing.

## Notes For GitHub

- Wi-Fi credentials are excluded from version control
- generated PlatformIO build artifacts are ignored
- the project no longer depends on `NTPClient`; synchronization uses the ESP32
  `time.h` API directly

If you want the repository to show an explicit license on GitHub, add a
top-level `LICENSE` file before publishing.
