/**
 * @file picolume_receiver.ino
 * @brief PicoLume Receiver
 * @version 0.2.0
 *
 * Binary Format V3 (PropConfig LUT):
 * - Per-prop LED count, type, color order, and brightness cap
 * - 8 bytes per prop × 224 props = 1792 byte LUT
 */

#include <SPI.h>
#include <RH_RF69.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <FatFS.h>
#include <FatFSUSB.h>
#include "hardware/watchdog.h"

// Forward declarations (required for Arduino's auto-prototype generator)
struct ShowHeader;
struct PropConfig;

// ====================== DEBUG CONFIG =========================
// Set to 1 for verbose serial output, 0 for production (faster boot)
#define DEBUG_MODE 0

#if DEBUG_MODE
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTHEX(x) Serial.print(x, HEX)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTHEX(x)
#endif

// ====================== HARDWARE CONFIG ======================
#define PIN_LED_DATA 14
#define CONFIG_BUTTON_PIN 3
#define ENTER_BUTTON_PIN 15
#define OLED_SDA_PIN 10
#define OLED_SCL_PIN 11

#define RF69_FREQ 915.0
#define RF69_CS_PIN 17
#define RF69_INT_PIN 20
#define RF69_RST_PIN 21

// ====================== DEFAULT CONFIG =======================
#define DEFAULT_NUM_LEDS 164
#define DEFAULT_BRIGHTNESS 255

// ====================== CUSTOMIZATION =======================
#define RF_BITRATE 19
#define ENCRYPT_KEY "GoMarchingBand!!"

#define TOTAL_PROPS 20
#define MASK_ARRAY_SIZE 7
// Show file format supports 224 props (7 * 32-bit buckets).
// Even if your deployment uses fewer props, the LUT in show.bin is fixed-size.
#define SHOW_TOTAL_PROPS (MASK_ARRAY_SIZE * 32)
#define MAX_EVENTS 512

// ====================== TIMING CONSTANTS =====================
#define LONG_PRESS_MS 3000
#define PACKET_TIMEOUT_MS 3000
#define STRIP_TIMEOUT_MS 1800000UL // 30 minutes

// ====================== EFFECT TYPES =========================
enum EffectType
{
    CMD_OFF = 0,
    CMD_SOLID_COLOR = 1,
    CMD_CAMERA_FLASH = 2,
    CMD_STROBE = 3,
    CMD_RAINBOW_CHASE = 4,
    CMD_RAINBOW_HOLD = 5,
    CMD_CHASE = 6,
    CMD_WIPE = 9,
    CMD_SCANNER = 10,
    CMD_METEOR = 11,
    CMD_FIRE = 12,
    CMD_HEARTBEAT = 13,
    CMD_GLITCH = 14,
    CMD_ENERGY = 15,
    CMD_SPARKLE = 16,
    CMD_BREATHE = 17,
    CMD_ALTERNATE = 18
};

// ====================== LED HARDWARE TYPES ===================
// These must match the Studio's LED_TYPES enum
enum LedType
{
    LED_WS2812B = 0,     // Most common addressable LED (800 KHz)
    LED_SK6812 = 1,      // Similar timing to WS2812B (800 KHz)
    LED_SK6812_RGBW = 2, // 4-channel RGBW (800 KHz)
    LED_WS2811 = 3,      // 400 KHz, often 12V, 3 LEDs per IC
    LED_WS2813 = 4,      // 800 KHz, backup data line
    LED_WS2815 = 5       // 12V version of WS2813, backup data line
};

// Color channel ordering - must match Studio's COLOR_ORDERS enum
// All 6 permutations of RGB supported by NeoPixel library
enum ColorOrder
{
    COLOR_GRB = 0, // WS2812B default (NEO_GRB)
    COLOR_RGB = 1, // NEO_RGB
    COLOR_BRG = 2, // NEO_BRG
    COLOR_RBG = 3, // NEO_RBG
    COLOR_GBR = 4, // NEO_GBR
    COLOR_BGR = 5  // NEO_BGR
};

// ====================== NEOPIXEL TYPE HELPER =================
// Convert LedType + ColorOrder enums to NeoPixel library flags
uint16_t getNeoPixelType(uint8_t ledType, uint8_t colorOrder)
{
    uint16_t colorFlags;
    uint16_t freqFlags = NEO_KHZ800; // Default for most LED types

    // WS2811 uses 400 KHz timing
    if (ledType == LED_WS2811)
    {
        freqFlags = NEO_KHZ400;
    }

    // Handle RGBW types (SK6812 RGBW) - 4 bytes per pixel
    if (ledType == LED_SK6812_RGBW)
    {
        switch (colorOrder)
        {
        case COLOR_GRB:
            colorFlags = NEO_GRBW;
            break;
        case COLOR_RGB:
            colorFlags = NEO_RGBW;
            break;
        case COLOR_BRG:
            colorFlags = NEO_BRGW;
            break;
        case COLOR_RBG:
            colorFlags = NEO_RBGW;
            break;
        case COLOR_GBR:
            colorFlags = NEO_GBRW;
            break;
        case COLOR_BGR:
            colorFlags = NEO_BGRW;
            break;
        default:
            colorFlags = NEO_GRBW;
            break;
        }
    }
    else
    {
        // Standard RGB types (3 bytes per pixel)
        switch (colorOrder)
        {
        case COLOR_GRB:
            colorFlags = NEO_GRB;
            break;
        case COLOR_RGB:
            colorFlags = NEO_RGB;
            break;
        case COLOR_BRG:
            colorFlags = NEO_BRG;
            break;
        case COLOR_RBG:
            colorFlags = NEO_RBG;
            break;
        case COLOR_GBR:
            colorFlags = NEO_GBR;
            break;
        case COLOR_BGR:
            colorFlags = NEO_BGR;
            break;
        default:
            colorFlags = NEO_GRB;
            break;
        }
    }

    return colorFlags + freqFlags;
}

// ====================== DATA STRUCTURES ======================
struct RadioPacket
{
    uint32_t packetCounter;
    uint32_t masterTime;
    uint8_t state;
    uint8_t hopCount;
    uint8_t sourceID;
};

struct __attribute__((packed)) ShowEvent
{
    uint32_t startTime;
    uint32_t duration;
    uint8_t effectType;
    uint8_t speed; // 0-255 mapped to 0.1-5.0
    uint8_t width; // 0-255 mapped to 0.0-1.0
    uint8_t reserved;
    uint32_t color;
    uint32_t color2;
    uint32_t targetMask[MASK_ARRAY_SIZE];
};

struct __attribute__((packed)) ShowHeader
{
    uint32_t magic;
    uint16_t version;
    uint16_t eventCount;
    uint8_t reserved[8]; // Reserved bytes (formerly legacy ledCount/brightness)
};

// PropConfig - V3 per-prop configuration (8 bytes each, 224 entries = 1792 bytes)
struct __attribute__((packed)) PropConfig
{
    uint16_t ledCount;     // Number of LEDs for this prop
    uint8_t ledType;       // LedType enum value
    uint8_t colorOrder;    // ColorOrder enum value
    uint8_t brightnessCap; // Max brightness (0-255) for this prop
    uint8_t reserved[3];   // Future use
};

// ====================== DEBUG HELPERS =========================
static const __FlashStringHelper *ledTypeName(uint8_t ledType)
{
    switch (ledType)
    {
    case LED_WS2812B:
        return F("WS2812B");
    case LED_SK6812:
        return F("SK6812");
    case LED_SK6812_RGBW:
        return F("SK6812_RGBW");
    case LED_WS2811:
        return F("WS2811");
    case LED_WS2813:
        return F("WS2813");
    case LED_WS2815:
        return F("WS2815");
    default:
        return F("UNKNOWN");
    }
}

static const __FlashStringHelper *colorOrderName(uint8_t order)
{
    switch (order)
    {
    case COLOR_GRB:
        return F("GRB");
    case COLOR_RGB:
        return F("RGB");
    case COLOR_BRG:
        return F("BRG");
    case COLOR_RBG:
        return F("RBG");
    case COLOR_GBR:
        return F("GBR");
    case COLOR_BGR:
        return F("BGR");
    default:
        return F("UNKNOWN");
    }
}

static void printShowConfig(const ShowHeader &header, uint8_t id, const PropConfig &cfg)
{
    Serial.println();
    Serial.println(F("=== PicoLume show.bin parsed settings ==="));
    Serial.print(F("Header: magic=0x"));
    Serial.print(header.magic, HEX);
    Serial.print(F(" version="));
    Serial.print(header.version);
    Serial.print(F(" events="));
    Serial.println(header.eventCount);

    Serial.print(F("Prop ID: "));
    Serial.println(id);

    Serial.print(F("PropConfig: ledCount="));
    Serial.print(cfg.ledCount);
    Serial.print(F(" ledType="));
    Serial.print(cfg.ledType);
    Serial.print(F(" ("));
    Serial.print(ledTypeName(cfg.ledType));
    Serial.print(F(") colorOrder="));
    Serial.print(cfg.colorOrder);
    Serial.print(F(" ("));
    Serial.print(colorOrderName(cfg.colorOrder));
    Serial.print(F(") brightnessCap="));
    Serial.println(cfg.brightnessCap);

    const bool rgbw = (cfg.ledType == LED_SK6812_RGBW);
    Serial.print(F("Derived: rgbw="));
    Serial.print(rgbw ? F("true") : F("false"));
    Serial.print(F(" neoPixelType=0x"));
    Serial.println(getNeoPixelType(cfg.ledType, cfg.colorOrder), HEX);
    Serial.println(F("========================================="));
    Serial.println();
}

// ====================== HARDWARE OBJECTS =====================
Adafruit_NeoPixel strip(DEFAULT_NUM_LEDS, PIN_LED_DATA, NEO_RGB + NEO_KHZ800);
Adafruit_SSD1306 display(128, 32, &Wire1, -1);
RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// ====================== SHOW DATA ============================
ShowEvent showSchedule[MAX_EVENTS];
// This prop's hardware configuration (from V3 LUT, or defaults for V1/V2)
PropConfig myConfig = {
    DEFAULT_NUM_LEDS,   // ledCount
    LED_WS2812B,        // ledType
    COLOR_GRB,          // colorOrder
    DEFAULT_BRIGHTNESS, // brightnessCap
    {0, 0, 0}           // reserved
};
int showLength = 0;
uint32_t showEndTime = 0; // Calculated end time of last event
uint16_t numLeds = DEFAULT_NUM_LEDS;

// ====================== RUNTIME STATE ========================
uint8_t propID = 1;
uint32_t currentShowTime = 0;
bool isShowPlaying = false;
bool radioInitialized = false;
bool isRGBW = false; // Set true for SK6812_RGBW strips

// Mode flags
bool inTestMode = false;
bool inSetupMode = false;

// Timing
unsigned long lastPacketTime = 0;
unsigned long lastActiveAnimationTime = 0;
unsigned long lastAnimationTime = 0;
unsigned long lastDisplayUpdateTime = 0;
int16_t lastRSSI = 0;

// Animation state
uint16_t animationStep = 0;
unsigned long lastLocalMillis = 0;
bool packetReceivedThisFrame = false; // Prevents double-advancing time

// Current effect state
uint8_t currentEffectType = CMD_OFF;
uint32_t currentEffectStart = 0;
uint32_t currentEffectDuration = 0;
uint32_t currentEffectColor = 0;
uint32_t currentEffectColor2 = 0;
uint8_t currentEffectSpeed = 0;        // Raw byte 0-255
uint8_t currentEffectWidth = 0;        // Raw byte 0-255
uint8_t lastRenderedEffectType = 0xFF; // Used to detect OFF transitions reliably

// Frame management
bool frameDirty = false;
bool stripIsOff = true;
bool showEndReported = false; // One-shot flag for show end message

static inline void markDirty()
{
    frameDirty = true;
    stripIsOff = false;
}
static inline void markAllOff()
{
    stripIsOff = true;
    frameDirty = true;
}
static inline void showIfDirty()
{
    if (frameDirty)
    {
        strip.show();
        frameDirty = false;
    }
}

// Button state
volatile bool buttonPressFlag = false;
volatile unsigned long buttonPressTime = 0;
unsigned long buttonDownTime = 0;
bool buttonWasDown = false;
bool longPressHandled = false;

// USB state
volatile bool usbWasPlugged = false;
volatile bool usbUnplugged = false;

// ====================== BUTTON ISR ===========================
void button_isr()
{
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 200)
    {
        buttonPressFlag = true;
        buttonPressTime = interrupt_time;
    }
    last_interrupt_time = interrupt_time;
}

// ====================== UTILITY FUNCTIONS ====================
void savePropID(uint8_t id)
{
    EEPROM.write(0, id);
    EEPROM.commit();
}

uint8_t loadPropID()
{
    uint8_t id = EEPROM.read(0);
    if (id < 1 || id > TOTAL_PROPS)
    {
        id = 1;
        savePropID(id);
    }
    return id;
}

uint32_t dimColor(uint32_t color, float brightness)
{
    if (brightness <= 0)
        return 0;
    if (brightness >= 1)
        return color;
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);
    return strip.Color((uint8_t)(r * brightness), (uint8_t)(g * brightness), (uint8_t)(b * brightness));
}

// Convert RGB to RGBW by extracting white component
// For RGBW strips, this makes whites appear more natural
uint32_t makeColor(uint8_t r, uint8_t g, uint8_t b)
{
    if (!isRGBW)
    {
        return strip.Color(r, g, b);
    }
    // Derive white from minimum of R, G, B
    uint8_t w = min(r, min(g, b));
    return strip.Color(r - w, g - w, b - w, w);
}

// Overload for packed RGB color (from event data)
uint32_t makeColorFromPacked(uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return makeColor(r, g, b);
}

// ====================== USB CALLBACKS ========================
void plugCallback(uint32_t data) { usbWasPlugged = true; }
void unplugCallback(uint32_t data) { usbUnplugged = true; }

// ====================== SHOW FILE LOADING ====================
bool loadShowFromFlash()
{
    File f = FatFS.open("/show.bin", "r");
    if (!f)
    {
        Serial.println(F("No show.bin found"));
        return false;
    }

    ShowHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header))
    {
        Serial.println(F("Failed to read header"));
        f.close();
        return false;
    }

    if (header.magic != 0x5049434F)
    {
        Serial.println(F("Invalid file magic"));
        f.close();
        return false;
    }

    // --- VERSION HANDLING (V3 only) ---
    if (header.version != 3)
    {
        Serial.print(F("Unsupported version: "));
        Serial.print(header.version);
        Serial.println(F(" (requires V3)"));
        f.close();
        return false;
    }

    // V3: PropConfig LUT (8 bytes per prop = 1792 bytes total)
    // Seek to our prop's PropConfig entry (propID is 1-based)
    size_t propConfigOffset = sizeof(ShowHeader) + (propID - 1) * sizeof(PropConfig);
    Serial.print(F("Seeking to PropConfig at offset: "));
    Serial.print(propConfigOffset);
    Serial.print(F(" (header="));
    Serial.print(sizeof(ShowHeader));
    Serial.print(F(" + ("));
    Serial.print(propID);
    Serial.print(F("-1) * "));
    Serial.print(sizeof(PropConfig));
    Serial.println(F(")"));

    f.seek(propConfigOffset);

    // Read raw bytes first for debugging
    uint8_t rawConfig[8];
    if (f.read(rawConfig, 8) != 8)
    {
        Serial.println(F("Failed to read PropConfig bytes"));
        f.close();
        return false;
    }

    Serial.print(F("Raw PropConfig bytes: "));
    for (int i = 0; i < 8; i++)
    {
        if (rawConfig[i] < 0x10)
            Serial.print(F("0"));
        Serial.print(rawConfig[i], HEX);
        Serial.print(F(" "));
    }
    Serial.println();
    Serial.print(F("  [0-1] LedCount: "));
    Serial.println(rawConfig[0] | (rawConfig[1] << 8));
    Serial.print(F("  [2]   LedType: "));
    Serial.println(rawConfig[2]);
    Serial.print(F("  [3]   ColorOrder: "));
    Serial.println(rawConfig[3]);
    Serial.print(F("  [4]   BrightnessCap: "));
    Serial.println(rawConfig[4]);

    // Copy to struct
    memcpy(&myConfig, rawConfig, sizeof(PropConfig));
    numLeds = myConfig.ledCount;
    printShowConfig(header, propID, myConfig);

    // Seek past entire LUT to events section
    f.seek(sizeof(ShowHeader) + SHOW_TOTAL_PROPS * sizeof(PropConfig));

    Serial.print(F("Config: LEDs="));
    Serial.print(numLeds);
    Serial.print(F(" Type="));
    Serial.print(myConfig.ledType);
    Serial.print(F(" Order="));
    Serial.print(myConfig.colorOrder);
    Serial.print(F(" Cap="));
    Serial.println(myConfig.brightnessCap);
    showLength = min((int)header.eventCount, MAX_EVENTS);

    Serial.print(F("ID "));
    Serial.print(propID);
    Serial.print(F(" -> "));
    Serial.print(numLeds);
    Serial.println(F(" LEDs"));

    // Read Events and calculate show end time FOR THIS PROP ONLY
    showEndTime = 0;
    uint8_t bucketIndex = (propID - 1) / 32;
    uint32_t bitMask = (1UL << ((propID - 1) % 32));
    int eventsForThisProp = 0;

    Serial.print(F("Prop "));
    Serial.print(propID);
    Serial.print(F(" -> bucket["));
    Serial.print(bucketIndex);
    Serial.print(F("] mask=0x"));
    Serial.println(bitMask, HEX);

    for (int i = 0; i < showLength; i++)
    {
        if (f.read((uint8_t *)&showSchedule[i], sizeof(ShowEvent)) != sizeof(ShowEvent))
        {
            Serial.print(F("Failed to read event "));
            Serial.println(i);
            showLength = i;
            break;
        }
        // Only track end time for events targeting THIS prop
        if (showSchedule[i].targetMask[bucketIndex] & bitMask)
        {
            eventsForThisProp++;
            uint32_t eventEnd = showSchedule[i].startTime + showSchedule[i].duration;
            Serial.print(F("  Event "));
            Serial.print(i);
            Serial.print(F(": start="));
            Serial.print(showSchedule[i].startTime);
            Serial.print(F(" dur="));
            Serial.print(showSchedule[i].duration);
            Serial.print(F(" end="));
            Serial.println(eventEnd);
            if (eventEnd > showEndTime)
            {
                showEndTime = eventEnd;
            }
        }
    }

    Serial.print(F("Events targeting prop "));
    Serial.print(propID);
    Serial.print(F(": "));
    Serial.println(eventsForThisProp);
    Serial.print(F("Show end time: "));
    Serial.print(showEndTime);
    Serial.println(F(" ms"));

    if (showEndTime == 0 && showLength > 0)
    {
        Serial.println(F("WARNING: No events target this prop! Check show.bin targetMask."));
    }

    f.close();
    return true;
}

// Retry wrapper for loadShowFromFlash()
// Helps handle cases where filesystem hasn't fully settled after USB write
bool loadShowFromFlashWithRetry(int maxAttempts = 3)
{
    for (int attempt = 1; attempt <= maxAttempts; attempt++)
    {
        Serial.print(F("Load attempt "));
        Serial.print(attempt);
        Serial.print(F("/"));
        Serial.println(maxAttempts);

        if (loadShowFromFlash())
        {
            if (attempt > 1)
            {
                Serial.print(F("Success on attempt "));
                Serial.println(attempt);
            }
            return true;
        }

        if (attempt < maxAttempts)
        {
            Serial.println(F("Retrying after delay..."));
            delay(200); // Give filesystem time to settle
        }
    }

    Serial.println(F("All load attempts failed"));
    return false;
}

// ====================== USB MASS STORAGE MODE ================
void runUSBMode()
{
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(28, 0);
    display.println(F("USB MODE"));
    display.setCursor(16, 12);
    display.println(F("Copy show.bin"));
    display.setCursor(16, 24);
    display.println(F("then eject"));
    display.display();

    // *** FIX: Handle mount failure by formatting ***
    if (!FatFS.begin())
    {
        Serial.println(F("FatFS mount failed in USB mode, formatting..."));
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("FS Corrupted!"));
        display.setCursor(0, 12);
        display.println(F("Formatting..."));
        display.display();
        strip.setPixelColor(0, strip.Color(255, 165, 0)); // Orange = formatting
        strip.show();

        FatFS.format();
        delay(500);

        if (!FatFS.begin())
        {
            // Still failing after format - likely hardware issue
            Serial.println(F("Format failed - hardware issue?"));
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println(F("FORMAT FAILED!"));
            display.setCursor(0, 16);
            display.println(F("Hardware issue?"));
            display.display();

            // Blink red forever
            while (true)
            {
                strip.setPixelColor(0, strip.Color(50, 0, 0));
                strip.show();
                delay(500);
                strip.setPixelColor(0, 0);
                strip.show();
                delay(500);
            }
        }

        // Format succeeded
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("Format OK!"));
        display.setCursor(0, 12);
        display.println(F("Starting USB..."));
        display.display();
        strip.setPixelColor(0, strip.Color(0, 50, 0)); // Green = success
        strip.show();
        delay(1000);
    }

    FatFSUSB.onPlug(plugCallback);
    FatFSUSB.onUnplug(unplugCallback);

    // Ensure Studio can reliably detect the drive on first use.
    // Studio's uploader looks for INDEX.HTM or an existing show.bin on the mounted volume.
    // Creating these files is non-destructive (we only create them if missing).
    {
        File f = FatFS.open("/show.bin", "r");
        if (!f)
        {
            File wf = FatFS.open("/show.bin", "w");
            if (wf)
                wf.close();
        }
        else
        {
            f.close();
        }

        File idx = FatFS.open("/INDEX.HTM", "r");
        if (!idx)
        {
            File widx = FatFS.open("/INDEX.HTM", "w");
            if (widx)
            {
                widx.println(F("<!doctype html>"));
                widx.println(F("<html><head><meta charset=\"utf-8\"><title>PicoLume Receiver</title></head>"));
                widx.println(F("<body><h1>PicoLume Receiver</h1><p>USB upload volume.</p></body></html>"));
                widx.close();
            }
        }
        else
        {
            idx.close();
        }
    }
    FatFSUSB.begin();

    Serial.println(F("USB Mass Storage mode active"));
    bool ledState = false;
    unsigned long lastBlink = 0;

    while (true)
    {
        if (millis() - lastBlink > 500)
        {
            ledState = !ledState;
            strip.setPixelColor(0, ledState ? strip.Color(0, 0, 50) : 0);
            strip.show();
            lastBlink = millis();
        }

        if (Serial.available())
        {
            char c = Serial.read();
            if (c == 'r' || c == 'R')
            {
                display.clearDisplay();
                display.setTextSize(1);
                display.setCursor(10, 0);
                display.println(F("SYNCING..."));
                display.display();
                strip.setPixelColor(0, strip.Color(255, 255, 0)); // Yellow
                strip.show();

                // Stop USB mass storage - forces host to flush
                FatFSUSB.end();
                delay(500);

                // Unmount filesystem cleanly
                FatFS.end();
                delay(200);

                display.clearDisplay();
                display.setTextSize(2);
                display.setCursor(10, 8);
                display.println(F("REBOOTING"));
                display.display();
                strip.setPixelColor(0, strip.Color(255, 0, 0));
                strip.show();
                delay(500);

                watchdog_reboot(0, 0, 100);
                while (true)
                    ;
            }
        }

        if (usbUnplugged)
        {
            display.clearDisplay();
            display.setTextSize(1);
            display.setCursor(10, 0);
            display.println(F("SYNCING..."));
            display.display();
            strip.setPixelColor(0, strip.Color(255, 255, 0)); // Yellow
            strip.show();

            // Stop USB mass storage - forces host to flush
            FatFSUSB.end();
            delay(500);

            // Unmount filesystem cleanly
            FatFS.end();
            delay(200);

            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(10, 8);
            display.println(F("EJECTED!"));
            display.display();
            strip.setPixelColor(0, strip.Color(0, 50, 0));
            strip.show();
            delay(500);

            watchdog_reboot(0, 0, 100);
            while (true)
            {
                delay(10);
            }
        }
        delay(10);
    }
}

// ====================== SETUP MODE ===========================
void handleSetupMode()
{
    uint8_t tempPropID = propID;
    unsigned long pressStartTime = 0;
    bool isPressed = false;
    bool lastIsPressed = false;

    inSetupMode = true;
    while (true)
    {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(34, 0);
        display.print(F("Setup Mode"));
        display.setTextSize(3);
        if (tempPropID < 10)
        {
            display.setCursor(56, 9);
        }
        else if (tempPropID < 100)
        {
            display.setCursor(47, 9);
        }
        else
        {
            display.setCursor(38, 9);
        }
        display.print(tempPropID);
        display.display();

        isPressed = (digitalRead(CONFIG_BUTTON_PIN) == LOW);

        if (isPressed && !lastIsPressed)
        {
            pressStartTime = millis();
        }

        if (!isPressed && lastIsPressed)
        {
            tempPropID++;
            if (tempPropID > TOTAL_PROPS)
            {
                tempPropID = 1;
            }
        }

        if (isPressed && (millis() - pressStartTime > LONG_PRESS_MS))
        {
            propID = tempPropID;
            savePropID(propID);

            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(18, 8);
            display.print(F("SAVED!"));
            display.display();

            for (int i = 0; i < 3; i++)
            {
                strip.fill(strip.Color(0, 255, 0));
                strip.show();
                delay(150);
                strip.clear();
                strip.show();
                delay(150);
            }
            watchdog_reboot(0, 0, 100);
            while (true)
            {
                delay(10);
            }
        }
        lastIsPressed = isPressed;
        delay(20);
    }
}

// ====================== DISPLAY UPDATE =======================
void updateDisplay()
{
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print(F("ID:"));
    display.print(propID);
    display.print(F(" E:"));
    display.print(showLength);

    display.setCursor(64, 0);
    display.print(F("RSSI:"));
    display.print(lastRSSI);

    display.setCursor(0, 10);
    if (inSetupMode)
    {
        display.print(F("MODE: SETUP"));
    }
    else if (inTestMode)
    {
        display.print(F("MODE: TEST"));
    }
    else if (millis() - lastPacketTime > PACKET_TIMEOUT_MS)
    {
        display.print(F("MODE: NO SIGNAL"));
    }
    else
    {
        display.print(F("MODE: READY"));
    }

    display.setCursor(0, 20);
    uint32_t s = currentShowTime / 1000;
    uint32_t ms = (currentShowTime % 1000) / 10;
    display.print(F("TC: "));
    if (s < 10)
    {
        display.print(F("0"));
    }
    display.print(s);
    display.print(F(":"));
    if (ms < 10)
    {
        display.print(F("0"));
    }
    display.print(ms);

    display.print(isShowPlaying ? F(" PLAY") : F(" STOP"));
    display.display();
}

// ====================== TEST MODE ANIMATION ==================
void animationRainbowChase()
{
    if (millis() - lastAnimationTime < 30)
        return;
    lastAnimationTime = millis();

    const int bandWidth = 20;
    const int numColors = 4;
    // Compute palette dynamically for RGBW support
    uint32_t palette[numColors] = {
        makeColor(255, 0, 0),
        makeColor(255, 255, 0),
        makeColor(0, 0, 255),
        makeColor(255, 255, 255)};
    animationStep++;
    for (uint16_t i = 0; i < strip.numPixels(); i++)
    {
        int colorIndex = ((i + animationStep) / bandWidth) % numColors;
        strip.setPixelColor(i, palette[colorIndex]);
    }
    markDirty();
}

// ====================== SCHEDULER ============================
void checkSchedule()
{
    if (showLength == 0)
    {
        currentEffectType = CMD_OFF;
        return;
    }

    bool eventFound = false;
    int16_t selectedIndex = -1;
    uint8_t bucketIndex = (propID - 1) / 32;
    uint32_t bitMask = (1UL << ((propID - 1) % 32));

    for (int i = 0; i < showLength; i++)
    {
        ShowEvent *e = &showSchedule[i];
        if (currentShowTime >= e->startTime && currentShowTime < (e->startTime + e->duration))
        {
            if (e->targetMask[bucketIndex] & bitMask)
            {
                currentEffectType = e->effectType;
                currentEffectStart = e->startTime;
                currentEffectDuration = e->duration;
                currentEffectColor = e->color;
                currentEffectColor2 = e->color2;
                currentEffectSpeed = e->speed;
                currentEffectWidth = e->width;
                selectedIndex = (int16_t)i;
                eventFound = true;
                lastActiveAnimationTime = millis();

                // Debug: specifically log when OFF event is selected
                if (e->effectType == CMD_OFF)
                {
                    static uint32_t lastOffEventLog = 0;
                    if (currentShowTime - lastOffEventLog > 200)
                    {
                        DEBUG_PRINT(F("[Schedule] t="));
                        DEBUG_PRINT(currentShowTime);
                        DEBUG_PRINT(F("ms -> FOUND OFF EVENT #"));
                        DEBUG_PRINT(i);
                        DEBUG_PRINT(F(" ("));
                        DEBUG_PRINT(e->startTime);
                        DEBUG_PRINT(F("-"));
                        DEBUG_PRINT(e->startTime + e->duration);
                        DEBUG_PRINTLN(F("ms)"));
                        lastOffEventLog = currentShowTime;
                    }
                }
                break;
            }
        }
    }
    if (!eventFound)
    {
        currentEffectType = CMD_OFF;
        currentEffectStart = currentShowTime;
        currentEffectDuration = 0;

        // Debug: log when we fall through to OFF (no matching event)
        static uint32_t lastNoEventLog = 0;
        if (currentShowTime - lastNoEventLog > 500) // Log every 500ms max
        {
            DEBUG_PRINT(F("[Schedule] t="));
            DEBUG_PRINT(currentShowTime);
            DEBUG_PRINTLN(F("ms -> NO MATCHING EVENT, setting OFF"));
            lastNoEventLog = currentShowTime;
        }
    }

#if DEBUG_MODE
    // Log only when the selected event changes (helps debug "gaps not turning off")
    static int16_t lastScheduledEventIndex = -2; // -2=uninitialized, -1=none/off, >=0=showSchedule index
    if (selectedIndex != lastScheduledEventIndex)
    {
        DEBUG_PRINT(F("[Schedule] t="));
        DEBUG_PRINT(currentShowTime);
        DEBUG_PRINT(F("ms prop="));
        DEBUG_PRINT(propID);
        DEBUG_PRINT(F(" -> "));

        if (selectedIndex < 0)
        {
            DEBUG_PRINTLN(F("OFF (no active event)"));
        }
        else
        {
            const ShowEvent *e = &showSchedule[selectedIndex];
            DEBUG_PRINT(F("event#"));
            DEBUG_PRINT(selectedIndex);
            DEBUG_PRINT(F(" start="));
            DEBUG_PRINT(e->startTime);
            DEBUG_PRINT(F(" dur="));
            DEBUG_PRINT(e->duration);
            DEBUG_PRINT(F(" end="));
            DEBUG_PRINT(e->startTime + e->duration);
            DEBUG_PRINT(F(" effect="));
            DEBUG_PRINTLN(e->effectType);
        }

        lastScheduledEventIndex = selectedIndex;
    }
#endif
}

// ====================== EFFECT RENDERERS =====================
// [RENDERERS OMITTED FOR BREVITY - THEY ARE UNCHANGED FROM V15.0]
// Note: They already use strip.numPixels(), so they are dynamic-ready!
// Just paste the render functions from V15.0 here.
// I will include the key render function below to ensure complete file.

void renderSolid(uint32_t color)
{
    strip.fill(color);
    markDirty();
}

void renderCameraFlash(uint32_t localTime, uint32_t color)
{
    if (localTime % 500 < 50)
    {
        strip.fill(makeColor(255, 255, 255));
    }
    else
    {
        strip.clear();
    }
    markDirty();
}

void renderStrobe(uint32_t localTime, uint32_t color)
{
    if ((localTime / 33) % 2 == 0)
    {
        strip.fill(color);
    }
    else
    {
        strip.clear();
    }
    markDirty();
}

void renderRainbowChaseShow(uint32_t localTime)
{
    float speed = (currentEffectSpeed == 0) ? 1.0f : (float)currentEffectSpeed / 50.0f;
    uint32_t period = (uint32_t)(2000.0f / speed);
    if (period == 0)
        period = 1;

    uint16_t hueOffset = (localTime * 65536) / period;
    for (int i = 0; i < strip.numPixels(); i++)
    {
        int pixelHue = hueOffset + (i * 65536L / strip.numPixels());
        strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
    }
    markDirty();
}

void renderRainbowHold()
{
    for (int i = 0; i < strip.numPixels(); i++)
    {
        int pixelHue = (i * 65536L / strip.numPixels());
        strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
    }
    markDirty();
}

void renderWipe(uint32_t localTime, uint32_t color)
{
    float progress = (float)localTime / (float)currentEffectDuration;
    if (progress > 1.0)
        progress = 1.0;
    int litPixels = (int)(progress * strip.numPixels());
    for (int i = 0; i < strip.numPixels(); i++)
    {
        strip.setPixelColor(i, (i < litPixels) ? color : 0);
    }
    markDirty();
}

void renderChase(uint32_t localTime, uint32_t color)
{
    float speed = (currentEffectSpeed == 0) ? 2.0f : (float)currentEffectSpeed / 50.0f;
    float widthRatio = (currentEffectWidth == 0) ? 0.1f : (float)currentEffectWidth / 255.0f;

    float pos = (float)((localTime * (int)(speed * 100)) % 100000) / 100000.0f; // Scale up for smoother int calc
    // Or simpler: float pos = fmod(localTime * speed / 1000.0f, 1.0f);
    // Let's stick to original logic but dynamic:
    float effectiveTime = (float)localTime / 1000.0f * speed;
    float posInCycle = effectiveTime - floor(effectiveTime);

    int center = posInCycle * strip.numPixels();
    int width = max(1, (int)(strip.numPixels() * widthRatio));

    strip.clear();
    for (int i = 0; i < strip.numPixels(); i++)
    {
        // Wrap-around logic for smoother scrolling? Original code didn't wrap.
        // Original: if (abs(i - center) < width)
        if (abs(i - center) < width)
        {
            strip.setPixelColor(i, color);
        }
    }
    markDirty();
}

void renderScanner(uint32_t localTime, uint32_t color)
{
    float speed = (currentEffectSpeed == 0) ? 2.0f : (float)currentEffectSpeed / 50.0f;
    float t = (float)localTime / 1000.0f * speed;
    float pos = (sin(t) + 1.0f) / 2.0f;
    int center = pos * (strip.numPixels() - 1);

    float widthRatio = (currentEffectWidth == 0) ? 0.1f : (float)currentEffectWidth / 255.0f;
    // Scanner "width" acts as the decay distance (original was 5 pixels/units hardcoded)
    // Map 0.0-1.0 width to 1-20 pixels?
    float distLimit = max(2.0f, widthRatio * 50.0f);

    strip.clear();
    for (int i = 0; i < strip.numPixels(); i++)
    {
        float dist = abs(i - center);
        if (dist < distLimit)
        {
            float dim = 1.0 - (dist / distLimit);
            strip.setPixelColor(i, dimColor(color, dim));
        }
    }
    markDirty();
}

void renderMeteor(uint32_t localTime, uint32_t color)
{
    float speed = (currentEffectSpeed == 0) ? 2.0f : (float)currentEffectSpeed / 50.0f;
    float effectiveTime = (float)localTime / 1000.0f * speed;
    float t = effectiveTime - floor(effectiveTime);

    int head = t * strip.numPixels();

    float widthRatio = (currentEffectWidth == 0) ? 0.33f : (float)currentEffectWidth / 255.0f;
    int tailLen = max(1, (int)(strip.numPixels() * widthRatio));

    strip.clear();
    for (int i = 0; i < strip.numPixels(); i++)
    {
        if (i <= head && i > head - tailLen)
        {
            float decay = (float)(head - i) / (float)tailLen;
            strip.setPixelColor(i, dimColor(color, 1.0 - decay));
        }
    }
    markDirty();
}

void renderBreathe(uint32_t localTime, uint32_t color)
{
    float speed = (currentEffectSpeed == 0) ? 2.0f : (float)currentEffectSpeed / 50.0f;
    float t = (float)localTime / 1000.0f * speed;
    float b = (sin(t * 3.14159) + 1.0f) / 2.0f;
    strip.fill(dimColor(color, b));
    markDirty();
}

void renderHeartbeat(uint32_t localTime, uint32_t color)
{
    float t = (float)(localTime % 1000) / 1000.0f;
    float brightness = 0;
    if (t < 0.15)
    {
        brightness = sin(t * 3.14159 / 0.15);
    }
    else if (t > 0.25 && t < 0.45)
    {
        brightness = sin((t - 0.25) * 3.14159 / 0.2) * 0.6;
    }
    strip.fill(dimColor(color, brightness));
    markDirty();
}

void renderSparkle(uint32_t localTime, uint32_t color)
{
    strip.fill(dimColor(color, 0.2));
    srand(localTime / 50);
    uint32_t sparkleWhite = makeColor(255, 255, 255);
    for (int i = 0; i < strip.numPixels(); i++)
    {
        if ((rand() % 100) > 90)
        {
            strip.setPixelColor(i, sparkleWhite);
        }
    }
    markDirty();
}

void renderFire(uint32_t localTime)
{
    srand(localTime / 80);
    // Pre-compute fire colors for RGBW compatibility
    uint32_t fireRed = makeColor(255, 0, 0);
    uint32_t fireYellow = makeColor(255, 255, 0);
    uint32_t fireOrange = makeColor(255, 80, 0);
    for (int i = 0; i < strip.numPixels(); i++)
    {
        int r = rand() % 100;
        uint32_t c = fireRed;
        if (r > 80)
        {
            c = fireYellow;
        }
        else if (r > 50)
        {
            c = fireOrange;
        }
        strip.setPixelColor(i, c);
    }
    markDirty();
}

void renderEnergy(uint32_t localTime, uint32_t color, uint32_t color2)
{
    float t = (float)localTime / 500.0f;
    // Note: color/color2 are already RGBW-converted, extract original RGB from event data
    uint8_t r1 = (currentEffectColor >> 16) & 0xFF, g1 = (currentEffectColor >> 8) & 0xFF, b1 = currentEffectColor & 0xFF;
    uint8_t r2 = (currentEffectColor2 >> 16) & 0xFF, g2 = (currentEffectColor2 >> 8) & 0xFF, b2 = currentEffectColor2 & 0xFF;
    for (int i = 0; i < strip.numPixels(); i++)
    {
        float w1 = sin(i * 0.2 + t);
        float w2 = sin(i * 0.3 - t * 1.5);
        float val = (w1 + w2 + 2.0f) / 4.0f;
        uint8_t r = r1 + (r2 - r1) * val;
        uint8_t g = g1 + (g2 - g1) * val;
        uint8_t b = b1 + (b2 - b1) * val;
        strip.setPixelColor(i, makeColor(r, g, b));
    }
    markDirty();
}

void renderGlitch(uint32_t localTime, uint32_t color, uint32_t color2)
{
    srand(localTime / 50);
    if (rand() % 10 > 7)
    {
        strip.fill(color2);
    }
    else
    {
        strip.fill(color);
    }
    markDirty();
}

void renderAlternate(uint32_t color, uint32_t color2)
{
    for (int i = 0; i < strip.numPixels(); i++)
    {
        strip.setPixelColor(i, (i % 2 == 0) ? color : color2);
    }
    markDirty();
}

void renderFrame()
{
    // Debug: log current effect type periodically
    static uint32_t lastRenderLog = 0;
    static uint8_t lastLoggedEffect = 0xFF;
    if (currentEffectType != lastLoggedEffect || (millis() - lastRenderLog > 1000))
    {
        DEBUG_PRINT(F("[renderFrame] t="));
        DEBUG_PRINT(currentShowTime);
        DEBUG_PRINT(F("ms effect="));
        DEBUG_PRINT(currentEffectType);
        DEBUG_PRINT(F(" lastRendered="));
        DEBUG_PRINT(lastRenderedEffectType);
        DEBUG_PRINT(F(" stripIsOff="));
        DEBUG_PRINTLN(stripIsOff ? F("true") : F("false"));
        lastRenderLog = millis();
        lastLoggedEffect = currentEffectType;
    }

    // Safety: force OFF if we've run past the selected effect's end time.
    // This makes gaps reliably clear even if scheduling hiccups.
    if (currentEffectType != CMD_OFF && currentEffectDuration > 0)
    {
        const uint32_t endTime = currentEffectStart + currentEffectDuration;
        if (currentShowTime >= endTime)
        {
            currentEffectType = CMD_OFF;
            currentEffectStart = currentShowTime;
            currentEffectDuration = 0;
        }
    }

    // Robust OFF handling: ALWAYS clear and show when in OFF state.
    // This ensures LEDs turn off reliably even if previous clear failed.
    if (currentEffectType == CMD_OFF)
    {
        // Explicitly set all pixels to black (more reliable than clear())
        for (uint16_t i = 0; i < strip.numPixels(); i++)
        {
            strip.setPixelColor(i, 0);
        }
        strip.show();

        if (lastRenderedEffectType != CMD_OFF)
        {
            DEBUG_PRINT(F("[renderFrame] OFF transition from effect "));
            DEBUG_PRINTLN(lastRenderedEffectType);
        }

        stripIsOff = true;
        frameDirty = false;
        lastRenderedEffectType = CMD_OFF;
        return;
    }

    uint32_t localTime = currentShowTime - currentEffectStart;
    // Convert packed RGB to strip color (handles RGBW conversion if needed)
    uint32_t c = makeColorFromPacked(currentEffectColor);
    uint32_t c2 = makeColorFromPacked(currentEffectColor2);

    switch (currentEffectType)
    {
    case CMD_SOLID_COLOR:
        renderSolid(c);
        break;
    case CMD_CAMERA_FLASH:
        renderCameraFlash(localTime, c);
        break;
    case CMD_STROBE:
        renderStrobe(localTime, c);
        break;
    case CMD_RAINBOW_CHASE:
        renderRainbowChaseShow(localTime);
        break;
    case CMD_RAINBOW_HOLD:
        renderRainbowHold();
        break;
    case CMD_WIPE:
        renderWipe(localTime, c);
        break;
    case CMD_CHASE:
        renderChase(localTime, c);
        break;
    case CMD_SCANNER:
        renderScanner(localTime, c);
        break;
    case CMD_METEOR:
        renderMeteor(localTime, c);
        break;
    case CMD_BREATHE:
        renderBreathe(localTime, c);
        break;
    case CMD_HEARTBEAT:
        renderHeartbeat(localTime, c);
        break;
    case CMD_SPARKLE:
        renderSparkle(localTime, c);
        break;
    case CMD_FIRE:
        renderFire(localTime);
        break;
    case CMD_ENERGY:
        renderEnergy(localTime, c, c2);
        break;
    case CMD_GLITCH:
        renderGlitch(localTime, c, c2);
        break;
    case CMD_ALTERNATE:
        renderAlternate(c, c2);
        break;
    default:
        if (!stripIsOff)
        {
            strip.clear();
            markAllOff();
        }
        break;
    }

    lastRenderedEffectType = currentEffectType;
}

// ====================== SETUP ================================
void setup()
{
    // Check button FIRST before any delays - critical for USB mode entry
    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    delay(50); // Brief debounce
    bool enterUSBMode = (digitalRead(CONFIG_BUTTON_PIN) == LOW);

    Serial.begin(115200);

#if DEBUG_MODE
    // Only wait for serial if NOT entering USB mode (don't delay USB mode entry)
    if (!enterUSBMode)
    {
        // Wait for USB serial to enumerate (critical for Pico USB CDC)
        // Timeout after 3 seconds to avoid blocking if no serial monitor attached
        unsigned long serialWaitStart = millis();
        while (!Serial && (millis() - serialWaitStart < 3000))
        {
            delay(10);
        }
        delay(100); // Extra settling time
    }

    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F("  PicoLume Receiver v0.2.0 (V3 format)"));
    Serial.println(F("========================================"));
#endif

    EEPROM.begin(256);

    Wire1.setSDA(OLED_SDA_PIN);
    Wire1.setSCL(OLED_SCL_PIN);
    Wire1.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println(F("SSD1306 fail"));
    }
    display.setRotation(2);
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print(F("Booting..."));
    display.display();

    strip.begin();
    strip.setBrightness(DEFAULT_BRIGHTNESS);
    strip.clear();
    markAllOff();
    showIfDirty();

    propID = loadPropID();
    Serial.print(F("Prop ID from EEPROM: "));
    Serial.println(propID);

    if (enterUSBMode)
    {
        runUSBMode();
    }

    display.clearDisplay();
    display.setCursor(0, 0);
    display.print(F("Loading show..."));
    display.display();

    if (!FatFS.begin())
    {
        FatFS.format();
        FatFS.begin();
    }

    bool showLoaded = loadShowFromFlashWithRetry(3);
    FatFS.end();

    if (!showLoaded)
    {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("NO SHOW FILE!"));
        display.println(F(""));
        display.println(F("Hold BTN + power"));
        display.println(F("to upload show.bin"));
        display.display();
        strip.setPixelColor(0, strip.Color(50, 0, 0));
        strip.show();
        delay(2000);
    }

    // CRITICAL: Configure strip based on PropConfig (type, color order, length, brightness)
    isRGBW = (myConfig.ledType == LED_SK6812_RGBW);
    uint16_t neoPixelType = getNeoPixelType(myConfig.ledType, myConfig.colorOrder);

    Serial.println();
    Serial.println(F("=== STRIP CONFIGURATION ==="));
    Serial.print(F("LED Type: "));
    Serial.print(myConfig.ledType);
    Serial.print(F(" ("));
    Serial.print(ledTypeName(myConfig.ledType));
    Serial.println(F(")"));
    Serial.print(F("Color Order: "));
    Serial.print(myConfig.colorOrder);
    Serial.print(F(" ("));
    Serial.print(colorOrderName(myConfig.colorOrder));
    Serial.println(F(")"));
    Serial.print(F("NeoPixel Type Flags: 0x"));
    Serial.println(neoPixelType, HEX);
    Serial.print(F("LED Count: "));
    Serial.println(numLeds);
    Serial.print(F("Brightness Cap: "));
    Serial.println(myConfig.brightnessCap);
    Serial.print(F("RGBW Mode: "));
    Serial.println(isRGBW ? F("YES") : F("NO"));
    Serial.println(F("==========================="));
    Serial.println();

    strip.updateType(neoPixelType);
    strip.updateLength(numLeds);
    strip.setBrightness(myConfig.brightnessCap);
    strip.clear();
    markAllOff();
    showIfDirty();

    attachInterrupt(digitalPinToInterrupt(CONFIG_BUTTON_PIN), button_isr, FALLING);

    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);
    digitalWrite(RF69_RST_PIN, HIGH);
    delay(10);
    digitalWrite(RF69_RST_PIN, LOW);
    delay(10);

    if (!driver.init())
    {
        Serial.println(F("Radio init failed!"));
        radioInitialized = false;
    }
    else if (!driver.setFrequency(RF69_FREQ))
    {
        Serial.println(F("Set frequency failed!"));
        radioInitialized = false;
    }
    else
    {
#if RF_BITRATE == 2
        driver.setModemConfig(RH_RF69::FSK_Rb2Fd5);
#elif RF_BITRATE == 57
        driver.setModemConfig(RH_RF69::GFSK_Rb57_6Fd120);
#elif RF_BITRATE == 125
        driver.setModemConfig(RH_RF69::GFSK_Rb125Fd125);
#elif RF_BITRATE == 250
        driver.setModemConfig(RH_RF69::GFSK_Rb250Fd250);
#else
        driver.setModemConfig(RH_RF69::GFSK_Rb19_2Fd38_4);
#endif
        driver.setEncryptionKey((uint8_t *)ENCRYPT_KEY);
        driver.setTxPower(20, true);
        radioInitialized = true;
    }

    lastLocalMillis = millis();
    updateDisplay();

#if DEBUG_MODE
    Serial.println(F("========================================"));
    Serial.println(F("  SETUP COMPLETE - READY"));
    Serial.println(F("========================================"));
    Serial.print(F("Prop ID: "));
    Serial.println(propID);
    Serial.print(F("Show Events: "));
    Serial.println(showLength);
    Serial.print(F("Radio: "));
    Serial.println(radioInitialized ? F("OK") : F("FAILED"));
    Serial.println(F("========================================"));
    Serial.println();
#endif
}

// ====================== MAIN LOOP ============================
void loop()
{
    unsigned long now = millis();
    unsigned long delta = now - lastLocalMillis;
    lastLocalMillis = now;

    bool buttonIsDown = (digitalRead(CONFIG_BUTTON_PIN) == LOW);
    if (buttonIsDown && !buttonWasDown)
    {
        buttonDownTime = now;
        longPressHandled = false;
    }

    if (buttonIsDown && !longPressHandled && (now - buttonDownTime > LONG_PRESS_MS))
    {
        longPressHandled = true;
        detachInterrupt(digitalPinToInterrupt(CONFIG_BUTTON_PIN));
        handleSetupMode();
    }
    buttonWasDown = buttonIsDown;

    // Reset per-frame state
    packetReceivedThisFrame = false;

    if (radioInitialized)
    {
        if (driver.available())
        {
            uint8_t buf[sizeof(RadioPacket)];
            uint8_t len = sizeof(buf);
            if (driver.recv(buf, &len))
            {
                lastPacketTime = now;
                lastRSSI = driver.lastRssi();
                RadioPacket packet;
                memcpy(&packet, buf, sizeof(packet));
                currentShowTime = packet.masterTime;
                isShowPlaying = (packet.state == 1);
                packetReceivedThisFrame = true; // Don't also increment by delta
            }
        }

        if (buttonPressFlag && !buttonIsDown)
        {
            inTestMode = !inTestMode;
            if (!inTestMode)
            {
                strip.clear();
                markAllOff();
                showIfDirty();
            }
            animationStep = 0;
            buttonPressFlag = false;
        }

        if (inTestMode)
        {
            animationRainbowChase();
        }
        else if (isShowPlaying)
        {
            // Only run show when playing
            // Only increment time locally if we didn't receive a packet this frame
            // (packet.masterTime is authoritative; adding delta would double-advance time)
            if (!packetReceivedThisFrame)
            {
                currentShowTime += delta;
            }

            // Check if we're past the end of all events for this prop
            if (showEndTime > 0 && currentShowTime > showEndTime)
            {
                // Show complete - turn off LEDs (always, for reliability)
                if (!stripIsOff)
                {
                    DEBUG_PRINTLN(F("Show ended - turning off LEDs"));
                }
                for (uint16_t i = 0; i < strip.numPixels(); i++)
                {
                    strip.setPixelColor(i, 0);
                }
                strip.show();
                stripIsOff = true;
                frameDirty = false;
                lastRenderedEffectType = CMD_OFF;
            }
            else
            {
                checkSchedule();
                renderFrame();
            }
        }
        else
        {
            // Standby: keep LEDs off until show starts (always clear for reliability)
            for (uint16_t i = 0; i < strip.numPixels(); i++)
            {
                strip.setPixelColor(i, 0);
            }
            strip.show();
            stripIsOff = true;
            frameDirty = false;
            lastRenderedEffectType = CMD_OFF;
        }
        showIfDirty();

        if (now - lastDisplayUpdateTime > 250)
        {
            updateDisplay();
            lastDisplayUpdateTime = now;
        }
    }
    else
    {
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(5, 0);
        display.print(F("RADIO"));
        display.setCursor(5, 16);
        display.print(F("FAILED"));
        display.display();
        delay(500);
    }
}
