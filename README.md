# PicoLume Firmware

Firmware for the PicoLume wireless LED prop system. This system enables synchronized LED light shows across multiple props using RF communication.

## System Overview

PicoLume consists of two main components:

- **Remote** - Master clock transmitter that broadcasts timecode at 10Hz
- **Receiver** - LED prop controllers that receive timecode and execute scheduled effects

The remote sends continuous timing information, and receivers use this to play back pre-loaded show files with precise synchronization across all props.

## Repository Structure

```
firmware/
├── picolume_receiver/    # Current receiver firmware (v0.1.0)
├── picolume_remote/      # Current remote firmware (v0.1.0)
└── Archived/             # Legacy prototype firmware
```

## Current Firmware

### [PicoLume Receiver](picolume_receiver/)

Wireless LED prop receiver for synchronized light shows.

- Raspberry Pi Pico (RP2040) based
- Receives encrypted RF timecode at 915 MHz
- Supports up to 224 unique prop IDs
- USB mass storage mode for show file upload
- 17 built-in LED effects
- OLED status display

### [PicoLume Remote](picolume_remote/)

Master clock transmitter for synchronized LED light shows.

- Raspberry Pi Pico (RP2040) based
- Broadcasts encrypted timecode at 10Hz
- Play/Pause and Reset controls
- 16x2 LCD display
- Configurable RF bitrate options

## Quick Start

1. Flash the remote firmware to one Pico
2. Flash the receiver firmware to each prop Pico
3. Set unique prop IDs on each receiver (1-224)
4. Ensure all devices use matching `RF_BITRATE` and `ENCRYPT_KEY` settings
5. Upload a `show.bin` file to each receiver via USB mass storage mode (or use PicoLume Studio's one-click upload)
6. Power on all devices and use the remote to control playback

**Tip:** When using PicoLume Studio on Windows, the app automatically ejects the USB drive after upload for maximum reliability.

## RF Configuration

Both remote and receiver must use matching settings:

```cpp
#define RF_BITRATE 19                    // Options: 2, 19, 57, 125, 250
#define ENCRYPT_KEY "GoMarchingBand!!"   // Exactly 16 characters
```

| Bitrate | Mode              | Use Case                 |
| ------- | ----------------- | ------------------------ |
| 2       | FSK_Rb2Fd5        | Maximum range            |
| 19      | GFSK_Rb19_2Fd38_4 | Default (recommended)    |
| 57      | GFSK_Rb57_6Fd120  | Balanced                 |
| 125     | GFSK_Rb125Fd125   | Faster updates           |
| 250     | GFSK_Rb250Fd250   | Minimum latency          |

## Archived Firmware

The `Archived/` directory contains legacy firmware from the prototyping phase (dated October 2025). These used a different approach with hardcoded animation sequences rather than binary show files.

| Firmware | Description |
| -------- | ----------- |
| `pico_receiver_20251029` | Legacy receiver with 15 hardcoded sequences |
| `PicoLume_Remote_20251029` | Legacy RP2040 remote with sequence-based control |
| `Arduino_Uno_Remote_20251029` | Arduino Uno version of the legacy remote |

The legacy system transmitted sequence numbers (0-14) and receivers executed predefined animations. The current system is more flexible, using binary show files with per-prop LED count support and timecode-based synchronization.

## Hardware Platform

All current firmware targets the Raspberry Pi Pico (RP2040) with:

- RFM69HCW radio module (915 MHz ISM band)
- I2C display (OLED for receiver, LCD for remote)
- WS2812B NeoPixel LED strips (receiver only)

## Dependencies

- [Arduino-Pico](https://github.com/earlephilhower/arduino-pico) - RP2040 Arduino core
- [RadioHead](http://www.airspayce.com/mikem/arduino/RadioHead/) - RF69 radio library
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) - LED strip control
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) - OLED display (receiver)
- [LiquidCrystal_I2C](https://github.com/johnrickman/LiquidCrystal_I2C) - LCD display (remote)

## Version

v0.1.0 - Binary show file format with per-prop LED count support
