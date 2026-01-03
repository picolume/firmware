# PicoLume Remote

Master clock transmitter for synchronized LED light shows. Broadcasts timecode to all PicoLume receivers at 10Hz.

## Hardware Requirements

- Raspberry Pi Pico (RP2040)
- RFM69HCW radio module (915 MHz)
- 16x2 I2C LCD display
- 6 momentary push buttons (Config/Stop, Cycle/Play, Cue A-D)

## Pin Configuration

| Function      | Pin | Description                              |
| ------------- | --- | ---------------------------------------- |
| Config/Stop   | 3   | Hold on boot for USB mode; press to stop |
| Cycle/Play    | 15  | Start playback or cycle to next cue      |
| Cue A         | 6   | Jump to Cue A time                       |
| Cue B         | 7   | Jump to Cue B time                       |
| Cue C         | 8   | Jump to Cue C time                       |
| Cue D         | 9   | Jump to Cue D time                       |
| I2C SDA       | 4   | LCD data                                 |
| I2C SCL       | 5   | LCD clock                                |
| RF69 CS       | 17  | Radio chip select                        |
| RF69 INT      | 20  | Radio interrupt                          |
| RF69 RST      | 21  | Radio reset                              |

## Features

- Broadcasts encrypted timecode at 10Hz (100ms intervals)
- 4 configurable cue points loaded from show.bin
- USB mass storage mode for uploading show files
- 16x2 LCD shows current cue and elapsed time
- Configurable RF bitrate (2, 19, 57, 125, 250 kbps)
- AES encryption for secure communication
- Mesh networking ready (hop count and source ID fields)

## Cue System

Cue times are loaded from the `show.bin` file's CUE block:
- Set cues in PicoLume Studio using Shift+1/2/3/4 at the playhead
- Export `show.bin` which includes the CUE block
- Upload to remote via USB mode

When cycling, undefined cues are automatically skipped.

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

### Boot Sequence

1. **Normal boot**: Loads cues from show.bin, displays "Cues: X/4"
2. **Hold Config/Stop on boot**: Enters USB mass storage mode for file upload

### Controls

| Button      | Action                                          |
| ----------- | ----------------------------------------------- |
| Config/Stop | Stop playback and reset to time 0               |
| Cycle/Play  | Start playing from cue, or cycle to next cue    |
| Cue A-D     | Jump directly to that cue's configured time     |

### LCD Display

```
Line 1: CUE A PLAY  or  STOPPED
Line 2: 12.3 s
```

On boot:
```
Line 1: Master Clock
Line 2: Cues: 2/4
```

## USB Mode

To upload a new show.bin:
1. Hold the Config/Stop button while powering on
2. LCD shows "USB Mode - Upload show.bin"
3. Connect USB and copy show.bin to the drive
4. Disconnect USB - remote reboots and loads new cues

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
- Arduino-Pico core (includes FatFS)

## Version

v0.1.0
