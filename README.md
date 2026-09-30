# NTP_2

## Purpose

`NTP_2` is the ESP32 time source for the companion Teensy matrix clock.
It gets local CET/CEST time from NTP, synchronizes once per minute, and sends
the time to the Teensy. If an NTP attempt fails, the ESP32 keeps using its
running system clock and marks that minute as not freshly synchronized.

Time path:

```text
NTP -> ESP32 local CET/CEST time -> Teensy
```

The timezone rule is:

```text
CET-1CEST,M3.5.0/2,M10.5.0/3
```

## Hardware

- ESP32
- Wi-Fi connection
- SPI-like link to the Teensy

| Signal | ESP32 pin |
| --- | ---: |
| CS | GPIO 5 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| CLK | GPIO 18 |

## Time frame

The software-driven link uses a fixed 32-byte frame:

| Byte | Content |
| --- | --- |
| 0..18 | `YYYY-MM-DD HH:MM:SS` |
| 19 | `0x00` terminator |
| 20 | NTP status |
| 21 | Sequence ID |
| 22 | Timezone status |
| 23..31 | `0x00` |

Byte 20:

- `0x00`: NTP failed; ESP32 system time was used
- `0x01`: fresh NTP synchronization succeeded

Byte 22:

- `0x01`: CET / UTC+1
- `0x02`: CEST / UTC+2

The Teensy returns an ACK status and the related Sequence ID. An update is
accepted only when the ACK is successful and its Sequence ID matches the sent
frame.

## Configuration and build

- PlatformIO environment: `esp32dev`
- Main source: `src/ntp_2.cpp`
- Current upload and monitor port: `COM16`

Create `include/credential.h` from `include/credential.example.h` and add the
local Wi-Fi credentials. The real credential file is ignored by Git.

```bash
pio run -e esp32dev
pio run -e esp32dev -t upload
```

## Related project

The receiving Teensy firmware is in `../myMatrixClock2`.
