# PicoLume Receiver

Wireless LED prop receiver for synchronized light shows. Receives timecode from the PicoLume Remote and plays back scheduled LED effects.

## Hardware Requirements

- Raspberry Pi Pico (RP2040)
- RFM69HCW radio module (915 MHz)
- SSD1306 OLED display (128x32, I2C)
- Addressable LED strip (WS2812B, SK6812, WS2811, WS2813, WS2815, or SK6812 RGBW)
- Momentary push button

## Pin Configuration

| Function      | Pin |
| ------------- | --- |
| LED Data      | 22  |
| Config Button | 28  |
| OLED SDA      | 10  |
| OLED SCL      | 11  |
| RF69 CS       | 17  |
| RF69 INT      | 21  |
| RF69 RST      | 20  |

## Features

- Receives encrypted RF timecode at 915 MHz
- Supports up to 224 unique prop IDs
- Per-prop configuration: LED count, type, color order, brightness cap (V3 format)
- Supports 6 LED chipsets and 6 color orders
- USB mass storage mode for show file upload
- 17 built-in LED effects
- OLED status display with RSSI monitoring
- Automatic strip timeout after 30 minutes of inactivity

## Configuration

### Debug Mode

Enable verbose serial output for troubleshooting:

```cpp
#define DEBUG_MODE 1  // Set to 0 for production (faster boot)
```

When enabled:
- Waits up to 3 seconds for serial monitor connection
- Prints detailed show.bin parsing info
- Displays strip configuration details

### RF Configuration

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

**Note:** When using PicoLume Studio to upload, the app automatically ejects the USB drive on Windows for maximum reliability. The receiver performs proper filesystem cleanup before rebooting to ensure the new show file is read correctly.

## Show File Format (V3)

### Header (16 bytes)

| Offset | Size | Field                         |
| ------ | ---- | ----------------------------- |
| 0      | 4    | Magic (`0x5049434F` = "PICO") |
| 4      | 2    | Version (3)                   |
| 6      | 2    | Event count                   |
| 8      | 8    | Reserved                      |

### PropConfig LUT (1792 bytes)

224 entries x 8 bytes each, immediately after header:

| Offset | Size | Field         | Values                                    |
| ------ | ---- | ------------- | ----------------------------------------- |
| 0      | 2    | LED count     | 1-1000                                    |
| 2      | 1    | LED type      | 0=WS2812B, 1=SK6812, 2=SK6812_RGBW, 3=WS2811, 4=WS2813, 5=WS2815 |
| 3      | 1    | Color order   | 0=GRB, 1=RGB, 2=BRG, 3=RBG, 4=GBR, 5=BGR  |
| 4      | 1    | Brightness    | 0-255                                     |
| 5      | 3    | Reserved      |                                           |

### Event Structure (48 bytes each)

| Offset | Size | Field                  |
| ------ | ---- | ---------------------- |
| 0      | 4    | Start time (ms)        |
| 4      | 4    | Duration (ms)          |
| 8      | 1    | Effect type            |
| 9      | 3    | Padding                |
| 12     | 4    | Color (0xRRGGBB)       |
| 16     | 4    | Color2 (0xRRGGBB)      |
| 20     | 28   | Target mask (224 bits) |

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

## Supported LED Types

| Type | Chipset      | Timing  | Notes                    |
| ---- | ------------ | ------- | ------------------------ |
| 0    | WS2812B      | 800 KHz | Most common, default     |
| 1    | SK6812       | 800 KHz | WS2812B compatible       |
| 2    | SK6812 RGBW  | 800 KHz | 4-channel with white LED |
| 3    | WS2811       | 400 KHz | Often 12V, 3 LEDs per IC |
| 4    | WS2813       | 800 KHz | Backup data line         |
| 5    | WS2815       | 800 KHz | 12V version of WS2813    |

## Upload Reliability

The receiver includes several features to ensure reliable show file uploads:

- **Filesystem cleanup on eject/reset:** When USB is ejected or a serial reset is received, the firmware properly unmounts FatFS before rebooting to ensure all writes are complete.
- **Retry logic:** On boot, the receiver attempts to load `show.bin` up to 3 times with delays between attempts, handling cases where the filesystem hasn't fully settled.
- **Visual feedback:** The display shows "SYNCING..." during filesystem cleanup and "EJECTED!" or "REBOOTING" before restart.

## Dependencies

- [RadioHead](http://www.airspayce.com/mikem/arduino/RadioHead/) - RH_RF69
- [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)
- [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library)
- Arduino-Pico core (for FatFS, FatFSUSB, EEPROM)

## Version

v0.2.0 - Binary Format V3 with per-prop LED type, color order, and brightness
