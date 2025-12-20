/**
 * @file picolume_receiver.ino
 * @brief PicoLume Receiver
 * @version 0.1.0
 * - Added support for Binary Format Version 2 (Heterogeneous Hardware)
 * - Added Look-Up Table (LUT) for per-prop LED counts
 * - Strip length is now set dynamically based on Prop ID
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

// ====================== HARDWARE CONFIG ======================
#define LED_PIN 14
#define CONFIG_BUTTON_PIN 22
#define OLED_SDA_PIN 10
#define OLED_SCL_PIN 11

#define RF69_FREQ 915.0
#define RF69_CS_PIN 17
#define RF69_INT_PIN 21
#define RF69_RST_PIN 20

// ====================== DEFAULT CONFIG =======================
#define DEFAULT_NUM_LEDS 164
#define DEFAULT_BRIGHTNESS 255

// ====================== CUSTOMIZATION =======================
#define RF_BITRATE 19
#define ENCRYPT_KEY "GoMarchingBand!!"

#define TOTAL_PROPS 224
#define MASK_ARRAY_SIZE 7
#define MAX_EVENTS 512

// ====================== TIMING CONSTANTS =====================
#define LONG_PRESS_MS 3000
#define PACKET_TIMEOUT_MS 3000
#define STRIP_TIMEOUT_MS 1800000UL  // 30 minutes

// ====================== EFFECT TYPES =========================
enum EffectType {
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

// ====================== DATA STRUCTURES ======================
struct RadioPacket {
    uint32_t packetCounter;
    uint32_t masterTime;
    uint8_t  state;
    uint8_t  hopCount;
    uint8_t  sourceID;
};

struct __attribute__((packed)) ShowEvent {
    uint32_t startTime;
    uint32_t duration;
    uint8_t  effectType;
    uint8_t  _pad[3];
    uint32_t color;
    uint32_t color2;
    uint32_t targetMask[MASK_ARRAY_SIZE];
};

struct __attribute__((packed)) ShowHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t eventCount;
    uint16_t ledCount;    // Global default (Legacy/Fallback)
    uint8_t  brightness;
    uint8_t  _reserved1;
    uint8_t  reserved[4];
};

// ====================== HARDWARE OBJECTS =====================
Adafruit_NeoPixel strip(DEFAULT_NUM_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);
Adafruit_SSD1306 display(128, 32, &Wire1, -1);
RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// ====================== SHOW DATA ============================
ShowEvent showSchedule[MAX_EVENTS];
uint16_t propLengths[TOTAL_PROPS]; // <-- NEW: Look-Up Table
int showLength = 0;
uint16_t numLeds = DEFAULT_NUM_LEDS;
uint8_t maxBrightness = DEFAULT_BRIGHTNESS;

// ====================== RUNTIME STATE ========================
uint8_t propID = 1;
uint32_t currentShowTime = 0;
bool isShowPlaying = false;
bool radioInitialized = false;

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

// Current effect state
uint8_t currentEffectType = CMD_OFF;
uint32_t currentEffectStart = 0;
uint32_t currentEffectDuration = 0;
uint32_t currentEffectColor = 0;
uint32_t currentEffectColor2 = 0;

// Frame management
bool frameDirty = false;
bool stripIsOff = true;

static inline void markDirty()   { frameDirty = true; stripIsOff = false; }
static inline void markAllOff()  { stripIsOff = true; frameDirty = true; }
static inline void showIfDirty() { if (frameDirty) { strip.show(); frameDirty = false; } }

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
void button_isr() {
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 200) {
        buttonPressFlag = true;
        buttonPressTime = interrupt_time;
    }
    last_interrupt_time = interrupt_time;
}

// ====================== UTILITY FUNCTIONS ====================
void savePropID(uint8_t id) {
    EEPROM.write(0, id);
    EEPROM.commit();
}

uint8_t loadPropID() {
    uint8_t id = EEPROM.read(0);
    if (id < 1 || id > TOTAL_PROPS) {
        id = 1;
        savePropID(id);
    }
    return id;
}

uint32_t dimColor(uint32_t color, float brightness) {
    if (brightness <= 0) return 0;
    if (brightness >= 1) return color;
    uint8_t r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t b = (uint8_t)(color & 0xFF);
    return strip.Color((uint8_t)(r * brightness), (uint8_t)(g * brightness), (uint8_t)(b * brightness));
}

// ====================== USB CALLBACKS ========================
void plugCallback(uint32_t data) { usbWasPlugged = true; }
void unplugCallback(uint32_t data) { usbUnplugged = true; }

// ====================== SHOW FILE LOADING ====================
bool loadShowFromFlash() {
    File f = FatFS.open("/show.bin", "r");
    if (!f) {
        Serial.println(F("No show.bin found"));
        return false;
    }

    ShowHeader header;
    if (f.read((uint8_t*)&header, sizeof(header)) != sizeof(header)) {
        Serial.println(F("Failed to read header"));
        f.close();
        return false;
    }

    if (header.magic != 0x5049434F) {
        Serial.println(F("Invalid file magic"));
        f.close();
        return false;
    }

    // --- VERSION HANDLING ---
    if (header.version == 1) {
        // V1: Homogeneous - Global LED count applies to everyone
        Serial.println(F("Format: V1 (Global)"));
        numLeds = header.ledCount;
        // Fill LUT with global default for safety
        for (int i = 0; i < TOTAL_PROPS; i++) {
            propLengths[i] = numLeds;
        }
    } else if (header.version == 2) {
        // V2: Heterogeneous - Read Look-Up Table
        Serial.println(F("Format: V2 (Per-Prop)"));

        // Read alignment byte (1 byte padding after header)
        uint8_t padding;
        f.read(&padding, 1);

        // Read LUT (224 * 2 bytes = 448 bytes)
        if (f.read((uint8_t*)propLengths, sizeof(propLengths)) != sizeof(propLengths)) {
            Serial.println(F("Failed to read LUT"));
            f.close();
            return false;
        }

        // Set MY specific LED count based on stored propID
        // propID is 1-based, array is 0-based
        numLeds = propLengths[propID - 1];
    } else {
        Serial.print(F("Unknown version: "));
        Serial.println(header.version);
        f.close();
        return false;
    }

    maxBrightness = header.brightness;
    showLength = min((int)header.eventCount, MAX_EVENTS);

    Serial.print(F("ID "));
    Serial.print(propID);
    Serial.print(F(" -> "));
    Serial.print(numLeds);
    Serial.println(F(" LEDs"));

    // Read Events
    for (int i = 0; i < showLength; i++) {
        if (f.read((uint8_t*)&showSchedule[i], sizeof(ShowEvent)) != sizeof(ShowEvent)) {
            Serial.print(F("Failed to read event "));
            Serial.println(i);
            showLength = i;
            break;
        }
    }

    f.close();
    return true;
}

// ====================== USB MASS STORAGE MODE ================
void runUSBMode() {
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
    if (!FatFS.begin()) {
        Serial.println(F("FatFS mount failed in USB mode, formatting..."));
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("FS Corrupted!"));
        display.setCursor(0, 12);
        display.println(F("Formatting..."));
        display.display();
        strip.setPixelColor(0, strip.Color(255, 165, 0));  // Orange = formatting
        strip.show();

        FatFS.format();
        delay(500);

        if (!FatFS.begin()) {
            // Still failing after format - likely hardware issue
            Serial.println(F("Format failed - hardware issue?"));
            display.clearDisplay();
            display.setCursor(0, 0);
            display.println(F("FORMAT FAILED!"));
            display.setCursor(0, 16);
            display.println(F("Hardware issue?"));
            display.display();

            // Blink red forever
            while (true) {
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
        strip.setPixelColor(0, strip.Color(0, 50, 0));  // Green = success
        strip.show();
        delay(1000);
    }

    FatFSUSB.onPlug(plugCallback);
    FatFSUSB.onUnplug(unplugCallback);
    FatFSUSB.begin();

    Serial.println(F("USB Mass Storage mode active"));
    bool ledState = false;
    unsigned long lastBlink = 0;

    while (true) {
        if (millis() - lastBlink > 500) {
            ledState = !ledState;
            strip.setPixelColor(0, ledState ? strip.Color(0, 0, 50) : 0);
            strip.show();
            lastBlink = millis();
        }

        if (Serial.available()) {
            char c = Serial.read();
            if (c == 'r' || c == 'R') {
                display.clearDisplay();
                display.setTextSize(1);
                display.setCursor(10, 0);
                display.println(F("SYNCING..."));
                display.display();
                strip.setPixelColor(0, strip.Color(255, 255, 0));  // Yellow
                strip.show();
                
                // Stop USB mass storage - forces host to flush
                FatFSUSB.end();
                delay(500);
                
                // Unmount filesystem cleanly
                FatFS.end();
                delay(100);
                
                display.clearDisplay();
                display.setTextSize(2);
                display.setCursor(10, 8);
                display.println(F("REBOOTING"));
                display.display();
                strip.setPixelColor(0, strip.Color(255, 0, 0));
                strip.show();
                delay(100);
                
                watchdog_reboot(0, 0, 100);
                while(true);
            }
        }

        if (usbUnplugged) {
            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(10, 8);
            display.println(F("EJECTED!"));
            display.display();
            strip.setPixelColor(0, strip.Color(0, 50, 0));
            strip.show();
            delay(1000);
            watchdog_reboot(0, 0, 100);
            while (true) { delay(10); }
        }
        delay(10);
    }
}

// ====================== SETUP MODE ===========================
void handleSetupMode() {
    uint8_t tempPropID = propID;
    unsigned long pressStartTime = 0;
    bool isPressed = false;
    bool lastIsPressed = false;

    inSetupMode = true;
    while (true) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(34, 0);
        display.print(F("Setup Mode"));
        display.setTextSize(3);
        if (tempPropID < 10) {
            display.setCursor(56, 9);
        } else if (tempPropID < 100) {
            display.setCursor(47, 9);
        } else {
            display.setCursor(38, 9);
        }
        display.print(tempPropID);
        display.display();

        isPressed = (digitalRead(CONFIG_BUTTON_PIN) == LOW);

        if (isPressed && !lastIsPressed) {
            pressStartTime = millis();
        }

        if (!isPressed && lastIsPressed) {
            tempPropID++;
            if (tempPropID > TOTAL_PROPS) {
                tempPropID = 1;
            }
        }

        if (isPressed && (millis() - pressStartTime > LONG_PRESS_MS)) {
            propID = tempPropID;
            savePropID(propID);

            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(18, 8);
            display.print(F("SAVED!"));
            display.display();

            for (int i = 0; i < 3; i++) {
                strip.fill(strip.Color(0, 255, 0));
                strip.show();
                delay(150);
                strip.clear();
                strip.show();
                delay(150);
            }
            watchdog_reboot(0, 0, 100);
            while (true) { delay(10); }
        }
        lastIsPressed = isPressed;
        delay(20);
    }
}

// ====================== DISPLAY UPDATE =======================
void updateDisplay() {
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
    if (inSetupMode) {
        display.print(F("MODE: SETUP"));
    } else if (inTestMode) {
        display.print(F("MODE: TEST"));
    } else if (millis() - lastPacketTime > PACKET_TIMEOUT_MS) {
        display.print(F("MODE: NO SIGNAL"));
    } else {
        display.print(F("MODE: READY"));
    }

    display.setCursor(0, 20);
    uint32_t s = currentShowTime / 1000;
    uint32_t ms = (currentShowTime % 1000) / 10;
    display.print(F("TC: "));
    if (s < 10) {
        display.print(F("0"));
    }
    display.print(s);
    display.print(F(":"));
    if (ms < 10) {
        display.print(F("0"));
    }
    display.print(ms);

    display.print(isShowPlaying ? F(" PLAY") : F(" STOP"));
    display.display();
}

// ====================== TEST MODE ANIMATION ==================
void animationRainbowChase() {
    if (millis() - lastAnimationTime < 30) return;
    lastAnimationTime = millis();

    const int bandWidth = 20;
    const int numColors = 4;
    static uint32_t palette[numColors] = {
        strip.Color(255, 0, 0),
        strip.Color(255, 255, 0),
        strip.Color(0, 0, 255),
        strip.Color(255, 255, 255)
    };
    animationStep++;
    for (uint16_t i = 0; i < strip.numPixels(); i++) {
        int colorIndex = ((i + animationStep) / bandWidth) % numColors;
        strip.setPixelColor(i, palette[colorIndex]);
    }
    markDirty();
}

// ====================== SCHEDULER ============================
void checkSchedule() {
    if (showLength == 0) {
        currentEffectType = CMD_OFF;
        return;
    }

    bool eventFound = false;
    uint8_t bucketIndex = (propID - 1) / 32;
    uint32_t bitMask = (1UL << ((propID - 1) % 32));

    for (int i = 0; i < showLength; i++) {
        ShowEvent* e = &showSchedule[i];
        if (currentShowTime >= e->startTime && currentShowTime < (e->startTime + e->duration)) {
            if (e->targetMask[bucketIndex] & bitMask) {
                currentEffectType = e->effectType;
                currentEffectStart = e->startTime;
                currentEffectDuration = e->duration;
                currentEffectColor = e->color;
                currentEffectColor2 = e->color2;
                eventFound = true;
                lastActiveAnimationTime = millis();
                break;
            }
        }
    }
    if (!eventFound) {
        currentEffectType = CMD_OFF;
    }
}

// ====================== EFFECT RENDERERS =====================
// [RENDERERS OMITTED FOR BREVITY - THEY ARE UNCHANGED FROM V15.0]
// Note: They already use strip.numPixels(), so they are dynamic-ready!
// Just paste the render functions from V15.0 here.
// I will include the key render function below to ensure complete file.

void renderSolid(uint32_t color) {
    strip.fill(color);
    markDirty();
}

void renderCameraFlash(uint32_t localTime, uint32_t color) {
    if (localTime % 500 < 50) {
        strip.fill(strip.Color(255, 255, 255));
    } else {
        strip.clear();
    }
    markDirty();
}

void renderStrobe(uint32_t localTime, uint32_t color) {
    if ((localTime / 33) % 2 == 0) {
        strip.fill(color);
    } else {
        strip.clear();
    }
    markDirty();
}

void renderRainbowChaseShow(uint32_t localTime) {
    uint16_t hueOffset = (localTime * 65536) / 2000;
    for (int i = 0; i < strip.numPixels(); i++) {
        int pixelHue = hueOffset + (i * 65536L / strip.numPixels());
        strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
    }
    markDirty();
}

void renderRainbowHold() {
    for (int i = 0; i < strip.numPixels(); i++) {
        int pixelHue = (i * 65536L / strip.numPixels());
        strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
    }
    markDirty();
}

void renderWipe(uint32_t localTime, uint32_t color) {
    float progress = (float)localTime / (float)currentEffectDuration;
    if (progress > 1.0) progress = 1.0;
    int litPixels = (int)(progress * strip.numPixels());
    for (int i = 0; i < strip.numPixels(); i++) {
        strip.setPixelColor(i, (i < litPixels) ? color : 0);
    }
    markDirty();
}

void renderChase(uint32_t localTime, uint32_t color) {
    float speed = 2.0;
    float pos = (float)((localTime * (int)speed) % 1000) / 1000.0f;
    int center = pos * strip.numPixels();
    int width = strip.numPixels() / 10;
    strip.clear();
    for (int i = 0; i < strip.numPixels(); i++) {
        if (abs(i - center) < width) {
            strip.setPixelColor(i, color);
        }
    }
    markDirty();
}

void renderScanner(uint32_t localTime, uint32_t color) {
    float speed = 2.0;
    float t = (float)localTime / 1000.0f * speed;
    float pos = (sin(t) + 1.0f) / 2.0f;
    int center = pos * (strip.numPixels() - 1);
    strip.clear();
    for (int i = 0; i < strip.numPixels(); i++) {
        float dist = abs(i - center);
        if (dist < 5) {
            float dim = 1.0 - (dist / 5.0);
            strip.setPixelColor(i, dimColor(color, dim));
        }
    }
    markDirty();
}

void renderMeteor(uint32_t localTime, uint32_t color) {
    float speed = 2.0;
    float t = (float)((localTime * (int)speed) % 1000) / 1000.0f;
    int head = t * strip.numPixels();
    int tailLen = strip.numPixels() / 3;
    strip.clear();
    for (int i = 0; i < strip.numPixels(); i++) {
        if (i <= head && i > head - tailLen) {
            float decay = (float)(head - i) / (float)tailLen;
            strip.setPixelColor(i, dimColor(color, 1.0 - decay));
        }
    }
    markDirty();
}

void renderBreathe(uint32_t localTime, uint32_t color) {
    float speed = 2.0;
    float t = (float)localTime / 1000.0f * speed;
    float b = (sin(t * 3.14159) + 1.0f) / 2.0f;
    strip.fill(dimColor(color, b));
    markDirty();
}

void renderHeartbeat(uint32_t localTime, uint32_t color) {
    float t = (float)(localTime % 1000) / 1000.0f;
    float brightness = 0;
    if (t < 0.15) {
        brightness = sin(t * 3.14159 / 0.15);
    } else if (t > 0.25 && t < 0.45) {
        brightness = sin((t - 0.25) * 3.14159 / 0.2) * 0.6;
    }
    strip.fill(dimColor(color, brightness));
    markDirty();
}

void renderSparkle(uint32_t localTime, uint32_t color) {
    strip.fill(dimColor(color, 0.2));
    srand(localTime / 50);
    for (int i = 0; i < strip.numPixels(); i++) {
        if ((rand() % 100) > 90) {
            strip.setPixelColor(i, 255, 255, 255);
        }
    }
    markDirty();
}

void renderFire(uint32_t localTime) {
    srand(localTime / 80);
    for (int i = 0; i < strip.numPixels(); i++) {
        int r = rand() % 100;
        uint32_t c = strip.Color(255, 0, 0);
        if (r > 80) {
            c = strip.Color(255, 255, 0);
        } else if (r > 50) {
            c = strip.Color(255, 80, 0);
        }
        strip.setPixelColor(i, c);
    }
    markDirty();
}

void renderEnergy(uint32_t localTime, uint32_t color, uint32_t color2) {
    float t = (float)localTime / 500.0f;
    uint8_t r1 = (color >> 16) & 0xFF, g1 = (color >> 8) & 0xFF, b1 = color & 0xFF;
    uint8_t r2 = (color2 >> 16) & 0xFF, g2 = (color2 >> 8) & 0xFF, b2 = color2 & 0xFF;
    for (int i = 0; i < strip.numPixels(); i++) {
        float w1 = sin(i * 0.2 + t);
        float w2 = sin(i * 0.3 - t * 1.5);
        float val = (w1 + w2 + 2.0f) / 4.0f;
        uint8_t r = r1 + (r2 - r1) * val;
        uint8_t g = g1 + (g2 - g1) * val;
        uint8_t b = b1 + (b2 - b1) * val;
        strip.setPixelColor(i, strip.Color(r, g, b));
    }
    markDirty();
}

void renderGlitch(uint32_t localTime, uint32_t color, uint32_t color2) {
    srand(localTime / 50);
    if (rand() % 10 > 7) {
        strip.fill(color2);
    } else {
        strip.fill(color);
    }
    markDirty();
}

void renderAlternate(uint32_t color, uint32_t color2) {
    for (int i = 0; i < strip.numPixels(); i++) {
        strip.setPixelColor(i, (i % 2 == 0) ? color : color2);
    }
    markDirty();
}

void renderFrame() {
    uint32_t localTime = currentShowTime - currentEffectStart;
    uint8_t r = (uint8_t)((currentEffectColor >> 16) & 0xFF);
    uint8_t g = (uint8_t)((currentEffectColor >> 8) & 0xFF);
    uint8_t b = (uint8_t)(currentEffectColor & 0xFF);
    uint32_t c = strip.Color(r, g, b);
    uint8_t r2 = (uint8_t)((currentEffectColor2 >> 16) & 0xFF);
    uint8_t g2 = (uint8_t)((currentEffectColor2 >> 8) & 0xFF);
    uint8_t b2 = (uint8_t)(currentEffectColor2 & 0xFF);
    uint32_t c2 = strip.Color(r2, g2, b2);
    switch (currentEffectType) {
        case CMD_OFF:
            if (!stripIsOff) {
                strip.clear();
                markAllOff();
            }
            break;
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
            if (!stripIsOff) {
                strip.clear();
                markAllOff();
            }
            break;
    }
}

// ====================== SETUP ================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println(F("PicoLume Receiver v16.0"));
    Serial.println(F("USB Mass Storage + Version 2 Binary Support"));

    EEPROM.begin(256);

    Wire1.setSDA(OLED_SDA_PIN);
    Wire1.setSCL(OLED_SCL_PIN);
    Wire1.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
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

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    delay(100);

    propID = loadPropID();

    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        runUSBMode();
    }

    display.clearDisplay();
    display.setCursor(0, 0);
    display.print(F("Loading show..."));
    display.display();

    if (!FatFS.begin()) {
        FatFS.format();
        FatFS.begin();
    }

    bool showLoaded = loadShowFromFlash();
    FatFS.end();

    if (!showLoaded) {
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

    // CRITICAL: Update strip length based on what we loaded!
    strip.updateLength(numLeds);
    strip.setBrightness(maxBrightness);
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

    if (!driver.init()) {
        Serial.println(F("Radio init failed!"));
        radioInitialized = false;
    } else if (!driver.setFrequency(RF69_FREQ)) {
        Serial.println(F("Set frequency failed!"));
        radioInitialized = false;
    } else {
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
        driver.setEncryptionKey((uint8_t*)ENCRYPT_KEY);
        driver.setTxPower(20, true);
        radioInitialized = true;
    }

    lastLocalMillis = millis();
    updateDisplay();
}

// ====================== MAIN LOOP ============================
void loop() {
    unsigned long now = millis();
    unsigned long delta = now - lastLocalMillis;
    lastLocalMillis = now;

    bool buttonIsDown = (digitalRead(CONFIG_BUTTON_PIN) == LOW);
    if (buttonIsDown && !buttonWasDown) {
        buttonDownTime = now;
        longPressHandled = false;
    }

    if (buttonIsDown && !longPressHandled && (now - buttonDownTime > LONG_PRESS_MS)) {
        longPressHandled = true;
        detachInterrupt(digitalPinToInterrupt(CONFIG_BUTTON_PIN));
        handleSetupMode();
    }
    buttonWasDown = buttonIsDown;

    if (radioInitialized) {
        if (driver.available()) {
            uint8_t buf[sizeof(RadioPacket)];
            uint8_t len = sizeof(buf);
            if (driver.recv(buf, &len)) {
                lastPacketTime = now;
                lastRSSI = driver.lastRssi();
                RadioPacket packet;
                memcpy(&packet, buf, sizeof(packet));
                currentShowTime = packet.masterTime;
                isShowPlaying = (packet.state == 1);
            }
        }

        if (buttonPressFlag && !buttonIsDown) {
            inTestMode = !inTestMode;
            if (!inTestMode) {
                strip.clear();
                markAllOff();
                showIfDirty();
            }
            animationStep = 0;
            buttonPressFlag = false;
        }

        if (inTestMode) {
            animationRainbowChase();
        } else {
            if (isShowPlaying) {
                currentShowTime += delta;
            }
            checkSchedule();
            renderFrame();
        }
        showIfDirty();

        if (now - lastDisplayUpdateTime > 250) {
            updateDisplay();
            lastDisplayUpdateTime = now;
        }
    } else {
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
