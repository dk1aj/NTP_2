# NTP_2

`NTP_2` is a small PlatformIO project for an ESP32 that synchronizes time over Wi-Fi using NTP and forwards the current timestamp to a Teensy over UART.

The project is intended as a dedicated network time provider:

- connect to a configured Wi-Fi network
- try a list of NTP servers until a valid time is received
- send the timestamp to the Teensy in `YYYY-MM-DD HH:MM:SS` format
- print debug information on USB serial
- retry synchronization if NTP is not available
- shut Wi-Fi down after a successful sync to reduce unnecessary activity

## Hardware

- ESP32 development board (`board = esp32dev`)
- USB connection for flashing and serial monitoring
- UART connection from ESP32 `TX=17` to Teensy RX
- UART connection from ESP32 `RX=16` to Teensy TX if needed
- common GND between ESP32 and Teensy

## Software Stack

- PlatformIO
- Arduino framework for ESP32
- Library dependency:
  - `arduino-libraries/NTPClient`

Note: the current implementation uses the ESP32 time API from `time.h` for the actual synchronization flow.

## Project Structure

- `src/ntp_2.cpp`
  Main application logic for Wi-Fi connection, NTP synchronization, UART transfer, and USB debug output.
- `include/credential.h`
  Local Wi-Fi credentials. This file is ignored by git and must not contain production secrets in a public repository.
- `platformio.ini`
  PlatformIO environment configuration for the ESP32 target.

## Configuration

The project expects a local file:

```cpp
// include/credential.h
const char* ssid = "your-wifi-name";
const char* password = "your-wifi-password";
```

This file is already excluded in `.gitignore`.

## Runtime Behavior

At startup the firmware:

1. starts USB serial at `115200`
2. starts UART2 at `115200` on pins `RX=16` and `TX=17`
3. connects to Wi-Fi
4. tries the configured NTP servers one by one
5. validates the received time
6. sends the timestamp to the Teensy over UART
7. prints detailed debug output over USB serial
8. disables Wi-Fi after a successful sync

After a successful sync, the firmware continues to print and transmit the current local time once per second.

If synchronization fails, it retries every 30 seconds while Wi-Fi is connected.

## NTP Servers

The current firmware tries these servers in order:

- `fritz.box`
- `0.europe.pool.ntp.org`
- `1.europe.pool.ntp.org`
- `0.pool.ntp.org`
- `1.pool.ntp.org`

## Time Handling

Current configuration in the source:

- GMT offset: `0`
- daylight saving offset: `3600`
- minimum valid year: `2024`

The code formats the UART transfer string as:

```text
YYYY-MM-DD HH:MM:SS
```

Each transmitted line is terminated with `CRLF`.

## Build and Upload

From the project directory:

```bash
pio run
pio run --target upload
pio device monitor
```

Current `platformio.ini` defaults:

- board: `esp32dev`
- upload port: `COM8`
- monitor port: `COM8`
- monitor speed: `115200`

Adjust the COM port if your ESP32 appears on a different device.

## Serial Debug Output

USB serial output includes messages such as:

- Wi-Fi connection progress
- assigned IP address
- NTP server attempts
- successful NTP source
- transmitted time string
- parsed local time fields
- retry notices if synchronization fails

## UART Output to Teensy

The ESP32 sends the current timestamp through `Serial2` at `115200` baud.

Current pin mapping:

- ESP32 `TX = 17`
- ESP32 `RX = 16`

Expected line format:

```text
2026-03-29 11:23:45
```

## Notes

- `include/credential.h` should stay private.
- For GitHub publication, consider replacing the real credential file with a sample such as `include/credential.example.h`.
- The source currently includes the original Rui Santos license header comment at the top of `src/ntp_2.cpp`.

## License

The repository does not currently contain a dedicated top-level license file.
Please add one before publishing if you want the GitHub project to have an explicit license.
