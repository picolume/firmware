# PicoLume Remote

Master clock transmitter for synchronized LED light shows. Broadcasts timecode to all PicoLume receivers at 10Hz.

## Hardware Requirements

- Raspberry Pi Pico (RP2040)
- RFM69HCW radio module (915 MHz)
- 16x2 I2C LCD display
- 2 momentary push buttons (Play/Pause, Reset)

## Pin Configuration

| Function     | Pin |
| ------------ | --- |
| Cycle Button | 6   |
| Off Button   | 7   |
| I2C SDA      | 4   |
| I2C SCL      | 5   |
| RF69 CS      | 17  |
| RF69 INT     | 20  |
| RF69 RST     | 21  |

## Features

- Broadcasts encrypted timecode at 10Hz (100ms intervals)
- Play/Pause and Reset controls
- 16x2 LCD shows current state and elapsed time
- Configurable RF bitrate (2, 19, 57, 125, 250 kbps)
- AES encryption for secure communication
- Mesh networking ready (hop count and source ID fields)

## Configuration

Edit these values to match your receivers:

```cpp
#define RF_BITRATE 19                    // Options: 2, 19, 57, 125, 250
#define ENCRYPT_KEY "GoMarchingBand!!"   // Exactly 16 characters
```

### RF Bitrate Options

| Value | Mode              | Notes                    |
| ----- | ----------------- | ------------------------ |
| 2     | FSK_Rb2Fd5        | Longest range, slowest   |
| 19    | GFSK_Rb19_2Fd38_4 | Default "sweet spot"     |
| 57    | GFSK_Rb57_6Fd120  | Balanced                 |
| 125   | GFSK_Rb125Fd125   | Faster                   |
| 250   | GFSK_Rb250Fd250   | Shortest range, fastest  |

## Usage

### Controls

| Button       | Action                        |
| ------------ | ----------------------------- |
| Cycle Button | Toggle Play/Pause             |
| Off Button   | Stop playback and reset to 0  |

### LCD Display

```
Line 1: PLAYING  or  STOPPED
Line 2: 00.0 s
```

## Radio Packet Structure

| Field         | Size | Description                        |
| ------------- | ---- | ---------------------------------- |
| packetCounter | 4    | Incrementing packet number         |
| masterTime    | 4    | Current timecode (milliseconds)    |
| state         | 1    | 0 = STOPPED, 1 = PLAYING           |
| hopCount      | 1    | Mesh hop count (0 = from remote)   |
| sourceID      | 1    | Source identifier (0 = remote)     |

Total: 11 bytes per packet

## Dependencies

- [RadioHead](http://www.airspayce.com/mikem/arduino/RadioHead/) - RH_RF69
- [LiquidCrystal_I2C](https://github.com/johnrickman/LiquidCrystal_I2C)
- Arduino-Pico core

## Version

v0.1.0 - Initial release
