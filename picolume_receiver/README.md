# PicoLume Receiver

Wireless LED prop receiver for synchronized light shows. Receives timecode from the PicoLume Remote and plays back scheduled LED effects.

## Hardware Requirements

- Raspberry Pi Pico (RP2040)
- RFM69HCW radio module (915 MHz)
- SSD1306 OLED display (128x32, I2C)
- WS2812B NeoPixel LED strip
- Momentary push button

## Pin Configuration

| Function      | Pin |
| ------------- | --- |
| LED Data      | 14  |
| Config Button | 22  |
| OLED SDA      | 10  |
| OLED SCL      | 11  |
| RF69 CS       | 17  |
| RF69 INT      | 21  |
| RF69 RST      | 20  |

## Features

- Receives encrypted RF timecode at 915 MHz
- Supports up to 224 unique prop IDs
- Per-prop LED count configuration (V2 show format)
- USB mass storage mode for show file upload
- 17 built-in LED effects
- OLED status display with RSSI monitoring
- Automatic strip timeout after 30 minutes of inactivity

## Configuration

### Customization (in code)

```cpp
#define RF_BITRATE 19                    // Options: 2, 19, 57, 125, 250
#define ENCRYPT_KEY "GoMarchingBand!!"   // Must match remote (16 chars)
```

### Setting Prop ID

1. Power on the device
2. Press and hold the config button for 3+ seconds to enter Setup Mode
3. Short press to increment the ID (1-224)
4. Long press (3 sec) to save and reboot

## Usage

### Normal Operation

1. Power on - device loads show.bin and waits for RF signal
2. Short press button to toggle Test Mode (rainbow chase animation)
3. Display shows: Prop ID, event count, RSSI, timecode, and play state

### USB Mass Storage Mode

1. Hold config button while powering on
2. Device appears as USB drive on computer
3. Copy `show.bin` to the drive
4. Eject the drive (device reboots automatically)

## Show File Format

### Header (16 bytes)

| Offset | Size | Field                         |
| ------ | ---- | ----------------------------- |
| 0      | 4    | Magic (`0x5049434F` = "PICO") |
| 4      | 2    | Version (1 or 2)              |
| 6      | 2    | Event count                   |
| 8      | 2    | LED count (global default)    |
| 10     | 1    | Brightness (0-255)            |
| 11     | 5    | Reserved                      |

### Version 2 Addition

After header: 1 byte padding + 448 bytes LUT (224 x 2-byte LED counts per prop)

### Event Structure (48 bytes each)

| Offset | Size | Field                  |
| ------ | ---- | ---------------------- |
| 0      | 4    | Start time (ms)        |
| 4      | 4    | Duration (ms)          |
| 8      | 1    | Effect type            |
| 9      | 3    | Padding                |
| 12     | 4    | Color (0xRRGGBB)       |
| 16     | 28   | Target mask (224 bits) |

## LED Effects

| ID  | Effect        | Description                    |
| --- | ------------- | ------------------------------ |
| 0   | Off           | All LEDs off                   |
| 1   | Solid Color   | Static color fill              |
| 2   | Camera Flash  | Brief white flash every 500ms  |
| 3   | Strobe        | Fast on/off at ~30Hz           |
| 4   | Rainbow Chase | Moving rainbow pattern         |
| 5   | Rainbow Hold  | Static rainbow gradient        |
| 6   | Chase         | Moving color band              |
| 9   | Wipe          | Progressive fill               |
| 10  | Scanner       | Larson scanner (Knight Rider)  |
| 11  | Meteor        | Falling streak with tail       |
| 12  | Fire          | Flickering fire simulation     |
| 13  | Heartbeat     | Pulsing heartbeat pattern      |
| 14  | Glitch        | Random flicker/dropout         |
| 15  | Energy        | Dual sine wave pattern         |
| 16  | Sparkle       | Random white sparkles on color |
| 17  | Breathe       | Smooth fade in/out             |
| 18  | Alternate     | Even/odd pixel pattern         |

## Dependencies

- [RadioHead](http://www.airspayce.com/mikem/arduino/RadioHead/) - RH_RF69
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)
- [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library)
- Arduino-Pico core (for FatFS, FatFSUSB, EEPROM)

## Version

v0.1.0 - Binary Format Version 2 support with per-prop LED counts
