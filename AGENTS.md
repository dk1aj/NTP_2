## Project

This project is the ESP32 time source for the Teensy matrix clock.

Target:

- ESP32
- Arduino framework
- PlatformIO
- environment: `esp32dev`

Main source:

```text
src/ntp_2.cpp
```

Purpose:

- connect to Wi-Fi
- obtain network time via NTP
- apply CET/CEST timezone handling
- maintain ESP32 system time
- send local time to the Teensy project `myMatrixClock2`

The Teensy and this ESP32 form one combined system.

---

# Working rules

1. Preserve working code whenever possible.
2. Make the smallest change necessary.
3. Do not rewrite working code for style reasons.
4. Do not add unrelated features.
5. Do not change GPIO assignments without explicit approval.
6. Do not change the communication protocol silently.
7. Do not update libraries unless required.
8. Avoid unnecessary dynamic memory allocation.
9. Avoid blocking behaviour where practical.
10. Explain changes in plain language.
11. Compile after code changes.

---

# Cross-project rule

Any modification affecting communication with the Teensy must be checked against:

```text
../myMatrixClock2
```

This includes:

- SPI
- frame structure
- timestamp format
- acknowledgement handling
- status codes
- timing
- retries
- timeouts
- sequencing
- timezone handling
- RTC synchronization

Do not change the ESP32 side of the protocol without verifying compatibility with the Teensy implementation.

---

# Current ESP32 communication pins

Treat the following as fixed hardware wiring:

```text
CS    GPIO 5
MOSI  GPIO 23
MISO  GPIO 19
CLK   GPIO 18
```

Do not change these pins without explicit approval.

---

# Time source

The ESP32 obtains time using NTP.

Current timezone configuration:

```text
CET-1CEST,M3.5.0/2,M10.5.0/3
```

Always distinguish between:

- UTC
- ESP32 system time
- CET/CEST local time
- time transmitted to the Teensy
- time stored by the Teensy RTC

Do not add another timezone correction without analysing the complete time path.

---

# Current protocol

The ESP32 acts as master.

Current properties:

```text
Master: ESP32
Slave: Teensy
Bit order: MSB first
Clock idle: LOW
Sampling: rising edge
Equivalent SPI mode: 0
Frame size: 32 bytes
```

Timestamp format:

```text
YYYY-MM-DD HH:MM:SS
```

Current time frame layout:

```text
Byte 0..18  timestamp
Byte 19     null terminator
Byte 20     current-minute NTP status
Byte 21     sequence ID
Byte 22..31 zero-filled
```

The `STATUS?` response returns the status code in byte 0 and the related
sequence ID in byte 1. A successful acknowledgement is valid only when both
the status is `0x01` and the returned sequence matches the transmitted frame.

Current status codes returned by the Teensy:

```text
0x00 idle
0x01 accepted
0x02 parse/format error
0x03 RTC write error
```

Do not modify these values or the frame format unless explicitly requested.

Any protocol change requires inspection and build verification of both projects.

---

# Known issues

These issues are known but must not be changed automatically.

## DST

The ESP32 sends local CET/CEST time.

The Teensy RTC stores local civil time.

The autumn DST transition contains a duplicated local `02:xx` hour.

Do not attempt a local one-line correction without analysing both projects.

## Software SPI

The current link is software-driven and deliberately slow.

Do not replace it with hardware SPI unless explicitly requested.

---

# Removed components

The ESP32-S3/LVGL demonstration code that previously existed inside `myMatrixClock2` is unrelated to this project.

Do not recreate LVGL or ESP32-S3 demo dependencies.

---

# Build

After code changes run the normal PlatformIO build for:

```text
env:esp32dev
```

For changes affecting communication, timestamps or synchronization, also build:

```text
../myMatrixClock2
env:teensy31
```

Report:

- build result
- warnings introduced by the change
- RAM use
- Flash use

Do not claim hardware verification when only compilation was performed.

---

# Git

Before changes:

```bash
git status
```

After changes:

```bash
git status
```

Do not automatically:

- stage
- commit
- push
- tag
- archive

unless explicitly requested.

---

# Scope

If one issue is requested, solve only that issue.

Example:

If the task is:

```text
Fix ACK handling
```

do not also modify:

- NTP synchronization
- timezone handling
- SPI speed
- GPIO assignments
- Teensy display code
- RTC architecture

Report additional findings instead of silently changing them.

---

# Final rule

Analyse first.

Preserve existing working behaviour.

Make the smallest technically correct change.

Verify compatibility with the Teensy when communication is affected.

Build after code changes.

Never invent hardware details.
