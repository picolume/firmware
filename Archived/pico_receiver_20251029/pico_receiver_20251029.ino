/**
 * @file pico_receiver_show_animations.ino
 * @brief Receiver with custom animations for band show (sequences 0-14)
 * @version 14.4
 * @date 2025-10-29
 *
 * V14.4 Changes:
 * - Added mesh repeater support for better field coverage
 * - Designated repeater props: 1, 6, 12, 18
 * - Repeaters retransmit packets from remote (hopCount=0) with random delay
 * - Single hop only (hopCount max = 1) to prevent loops
 * - Duplicate packet prevention using last retransmitted tracker
 * - Only retransmit on sequence changes (not heartbeats)
 *
 * V14.3 Changes:
 * - Updated to 15 sequences (0-14) per band director request
 * - Added 4 additional Camera Flash sequences (now at 1,2,9,10,11,12)
 * - Changed Blue Glow (seq 7) to FULL brightness (255) instead of medium (150)
 * - Removed Blue Build animation (no longer needed)
 *
 * Sequence List (0-14):
 * 0:  LEDs Off
 * 1:  Camera Flash #1
 * 2:  Camera Flash #2
 * 3:  Red Glow
 * 4:  LEDs Off
 * 5:  Red Flash
 * 6:  LEDs Off
 * 7:  Blue Glow (FULL brightness)
 * 8:  LEDs Off
 * 9:  Camera Flash #3
 * 10: Camera Flash #4
 * 11: Camera Flash #5
 * 12: Camera Flash #6
 * 13: Rainbow Chase
 * 14: Rainbow Hold
 */

// ============================ LIBRARIES ============================
#include <SPI.h>
#include <RH_RF69.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include "hardware/watchdog.h"

// ====================== HARDWARE & SYSTEM CONFIG ===================
#define LED_PIN           22
#define NUM_LEDS          164
#define TOTAL_PROPS       18
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_RGB + NEO_KHZ800);

#define OLED_SDA_PIN      10
#define OLED_SCL_PIN      11
Adafruit_SSD1306 display(128, 32, &Wire1, -1);

#define CONFIG_BUTTON_PIN 28

// --- RFM69 Radio (RadioHead Configuration) ---
#define RF69_FREQ         915.0
#define RF69_CS_PIN       17
#define RF69_INT_PIN      21
#define RF69_RST_PIN      20
const char ENCRYPTKEY[] = "GoMarchingBand!!";

RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// =========================== DATA & STATE ==========================
struct RadioPacket {
    uint32_t packetCounter;
    uint8_t  sequenceId;
    uint8_t  hopCount;    // 0=from remote, 1=first retransmit, 2+=ignore
    uint8_t  sourceID;    // 0=remote, 1-18=prop ID that sent this packet
};

uint8_t propID = 1;

// ====================== MESH REPEATER CONFIG =======================
// Designated repeater props for better field coverage
const uint8_t REPEATER_PROPS[] = {1, 6, 12, 18};
const uint8_t NUM_REPEATERS = sizeof(REPEATER_PROPS) / sizeof(REPEATER_PROPS[0]);

// Check if this prop is a designated repeater
bool isRepeater() {
    for (uint8_t i = 0; i < NUM_REPEATERS; i++) {
        if (propID == REPEATER_PROPS[i]) {
            return true;
        }
    }
    return false;
}

// ====================== REST OF STATE ==========================
bool inTestMode = false;
bool inSetupMode = false;
bool radioInitialized = false;
uint8_t currentSequence = 0;

volatile bool buttonPressFlag = false;
volatile unsigned long buttonPressTime = 0;

unsigned long lastPacketTime = 0;
#define PACKET_TIMEOUT_MS 3000
#define STRIP_TIMEOUT_MS 1800000UL // 30 minutes
unsigned long lastActiveAnimationTime = 0;

uint16_t animationStep = 0;
unsigned long lastAnimationTime = 0;
unsigned long lastDisplayUpdateTime = 0;
int16_t lastRSSI = 0;

// Animation-specific state
unsigned long sequenceStartTime = 0;
uint8_t lastSequenceRun = 255; // Track sequence changes to reset animation state

// State flags for static animations (reset when sequence changes)
bool redGlow_initialized = false;
bool blueGlow_initialized = false;
bool rainbowHold_frozen = false;

bool frameDirty = false;
bool stripIsOff = true;
static inline void markDirty()   { frameDirty = true; stripIsOff = false; }
static inline void markAllOff()  { stripIsOff = true; frameDirty = true;  }
static inline void showIfDirty() { if (frameDirty) { strip.show(); frameDirty = false; } }

// ====================== INTERRUPT SERVICE (button) =================
void button_isr() {
    static unsigned long last_interrupt_time = 0;
    unsigned long interrupt_time = millis();
    if (interrupt_time - last_interrupt_time > 200) {
        buttonPressFlag = true;
        buttonPressTime = interrupt_time;
    }
    last_interrupt_time = interrupt_time;
}

// ============================== UTILS ==============================
void savePropID(uint8_t id) { EEPROM.write(0, id); EEPROM.commit(); }
uint8_t loadPropID() {
    uint8_t id = EEPROM.read(0);
    if (id < 1 || id > TOTAL_PROPS) { id = 1; savePropID(id); }
    return id;
}

// ====================== MESH REPEATER FUNCTIONS ====================
struct LastRetransmit {
    uint32_t packetCounter;
    uint8_t  sequenceId;
    unsigned long timestamp;
};
LastRetransmit lastRetransmittedPacket = {0, 255, 0};

// Retransmit a received packet (only for designated repeater props)
void retransmitPacket(RadioPacket originalPacket) {
    // Only retransmit if:
    // 1. This prop is a designated repeater
    // 2. Packet came from remote (hopCount == 0)
    // 3. Haven't already retransmitted this exact packet
    if (!isRepeater()) return;
    if (originalPacket.hopCount != 0) return;
    
    // Check if we already retransmitted this packet
    if (lastRetransmittedPacket.packetCounter == originalPacket.packetCounter &&
        lastRetransmittedPacket.sequenceId == originalPacket.sequenceId) {
        return; // Already retransmitted this one
    }
    
    // Random delay to prevent collision between repeaters (30-50ms)
    uint32_t randomDelay = 30 + (random(0, 21)); // 30-50ms
    delay(randomDelay);
    
    // Prepare retransmit packet
    RadioPacket retransmit = originalPacket;
    retransmit.hopCount = 1;      // Mark as first retransmit
    retransmit.sourceID = propID; // Identify which prop retransmitted
    
    // Send the retransmission
    driver.send((uint8_t*)&retransmit, sizeof(retransmit));
    driver.waitPacketSent();
    
    // Track this retransmission to prevent duplicates
    lastRetransmittedPacket.packetCounter = originalPacket.packetCounter;
    lastRetransmittedPacket.sequenceId = originalPacket.sequenceId;
    lastRetransmittedPacket.timestamp = millis();
    
    Serial.print(F("RETRANSMIT: Seq="));
    Serial.print(retransmit.sequenceId);
    Serial.print(F(" Pkt="));
    Serial.print(retransmit.packetCounter);
    Serial.print(F(" (delay="));
    Serial.print(randomDelay);
    Serial.println(F("ms)"));
}

// ============================ ANIMATIONS ===========================

// Sequence 0, 4, 6, 8: All LEDs Off
void animationOff() {
    if (!stripIsOff) {
        strip.clear();
        markAllOff();
    }
}

// Sequence 1, 2, 9, 10, 11, 12: Camera Flash (SHORT - less white, faster fade)
void animationCameraFlash() {
    unsigned long elapsed = millis() - sequenceStartTime;
    
    if (elapsed < 700) {  // 0.7 seconds total
        if (millis() - lastAnimationTime > 50) {
            lastAnimationTime = millis();
            
            // Fast fade - less white light time
            for (uint16_t i = 0; i < strip.numPixels(); i++) {
                uint32_t c = strip.getPixelColor(i);
                uint8_t r = (c >> 16) & 0xFF;
                uint8_t g = (c >> 8) & 0xFF;
                uint8_t b = c & 0xFF;
                r = (r * 120) >> 8;  // Fast fade
                g = (g * 120) >> 8;
                b = (b * 120) >> 8;
                strip.setPixelColor(i, r, g, b);
            }
            
            // Predetermined flash pattern - 14 frames over 0.7 seconds
            // Only 2 flashes per prop for quick effect
            const uint8_t flashPattern[] = {
                // Prop 1-6 (2 flashes each)
                0, 8,
                1, 9,
                2, 10,
                0, 11,
                1, 12,
                2, 13,
                // Prop 7-12
                3, 9,
                0, 7,
                1, 10,
                2, 8,
                3, 11,
                0, 6,
                // Prop 13-18
                1, 8,
                2, 9,
                3, 10,
                0, 11,
                1, 12,
                2, 6
            };
            
            // Calculate which frame we're on (0-13)
            uint8_t frameNumber = elapsed / 50;
            if (frameNumber >= 14) frameNumber = 13;
            
            // Check if this prop should flash this frame
            uint8_t propIndex = (propID - 1) * 2;
            bool shouldFlash = false;
            
            for (int i = 0; i < 2; i++) {
                if (flashPattern[propIndex + i] == frameNumber) {
                    shouldFlash = true;
                    break;
                }
            }
            
            if (shouldFlash) {
                // Flash entire strip white - will fade quickly
                strip.fill(strip.Color(255, 255, 255));
            }
            
            markDirty();
        }
    } else {
        // After 0.7 seconds, turn off and stay off
        strip.clear();
        markAllOff();
    }
}

// Sequence 3: Red Glow (static red)
void animationRedGlow() {
    if (!redGlow_initialized) {
        strip.fill(strip.Color(200, 0, 0)); // Medium red
        markDirty();
        redGlow_initialized = true;
    }
}

// Sequence 5: Red Flash (red strobe)
void animationRedFlash() {
    if (millis() - lastAnimationTime < 250) return; // 250ms strobe
    lastAnimationTime = millis();
    
    static bool on = false;
    on = !on;
    if (on) {
        strip.fill(strip.Color(255, 0, 0)); // Bright red
    } else {
        strip.clear();
    }
    markDirty();
}

// Sequence 7: Blue Glow FULL BRIGHTNESS (static blue)
void animationBlueGlowFull() {
    if (!blueGlow_initialized) {
        strip.fill(strip.Color(0, 0, 255)); // FULL brightness blue
        markDirty();
        blueGlow_initialized = true;
    }
}

// Sequence 13: Rainbow Chase (using palette rainbow)
void animationRainbowChase() {
    if (millis() - lastAnimationTime < 30) return;  // Good timing for signal integrity
    lastAnimationTime = millis();
    
    const int bandWidth = 20;
    const int numColors = 4;
    static uint32_t palette[numColors] = { 
        strip.Color(255, 0, 0),     // Red
        strip.Color(255, 255, 0),   // Yellow
        strip.Color(0, 0, 255),     // Blue
        strip.Color(255, 255, 255)  // White
    };
    
    animationStep++;
    for (uint16_t i = 0; i < strip.numPixels(); i++) {
        int colorIndex = ((i + animationStep) / bandWidth) % numColors;
        strip.setPixelColor(i, palette[colorIndex]);
        
        // Brief pause every 50 LEDs to improve signal integrity on long wire runs
        if (i % 50 == 49) {
            delayMicroseconds(10);
        }
    }
    markDirty();
}

// Sequence 14: Rainbow Hold (freeze current palette rainbow pattern)
void animationRainbowHold() {
    if (!rainbowHold_frozen) {
        // Keep the current colors from sequence 13 (palette rainbow)
        // Just stop updating - the pattern is already on the strip
        rainbowHold_frozen = true;
        
        // If coming from a different sequence, set up palette rainbow pattern
        bool needsPattern = stripIsOff;
        for (uint16_t i = 0; i < NUM_LEDS && !needsPattern; i++) {
            if (strip.getPixelColor(i) == 0) {
                needsPattern = true;
                break;
            }
        }
        
        if (needsPattern) {
            // Create the palette rainbow pattern to freeze
            const int bandWidth = 20;
            const int numColors = 4;
            uint32_t palette[numColors] = { 
                strip.Color(255, 0, 0),     // Red
                strip.Color(255, 255, 0),   // Yellow
                strip.Color(0, 0, 255),     // Blue
                strip.Color(255, 255, 255)  // White
            };
            
            for (uint16_t i = 0; i < strip.numPixels(); i++) {
                int colorIndex = (i / bandWidth) % numColors;
                strip.setPixelColor(i, palette[colorIndex]);
            }
            markDirty();
        }
    }
    // Pattern stays frozen - no updates needed
}

void runSequence() {
    // Force reset of animation state when sequence changes
    if (currentSequence != lastSequenceRun) {
        lastSequenceRun = currentSequence;
        
        // Clear any animation state
        animationStep = 0;
        lastAnimationTime = 0;
        
        // Reset all static animation flags
        redGlow_initialized = false;
        blueGlow_initialized = false;
        rainbowHold_frozen = false;
    }
    
    switch (currentSequence) {
        case 0:  animationOff();           break; // LEDs Off
        case 1:  animationCameraFlash();   break; // Camera Flash #1
        case 2:  animationCameraFlash();   break; // Camera Flash #2
        case 3:  animationRedGlow();       break; // Red Glow
        case 4:  animationOff();           break; // LEDs Off
        case 5:  animationRedFlash();      break; // Red Flash
        case 6:  animationOff();           break; // LEDs Off
        case 7:  animationBlueGlowFull();  break; // Blue Glow (FULL brightness)
        case 8:  animationOff();           break; // LEDs Off
        case 9:  animationCameraFlash();   break; // Camera Flash #3
        case 10: animationCameraFlash();   break; // Camera Flash #4
        case 11: animationCameraFlash();   break; // Camera Flash #5
        case 12: animationCameraFlash();   break; // Camera Flash #6
        case 13: animationRainbowChase();  break; // Rainbow Chase
        case 14: animationRainbowHold();   break; // Rainbow Hold
        default: animationOff();           break;
    }
}

// ====================== SYSTEM & UI FUNCTIONS ======================
void updateDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    display.print(F("ID:"));
    display.print(propID);
    if (isRepeater()) display.print(F("*")); // Mark repeaters with *
    display.print(F(" S:"));
    display.print(currentSequence);

    display.setCursor(64, 0);
    display.print(F("RSSI:"));
    display.print(lastRSSI);

    display.setCursor(0, 10);
    if (inSetupMode)        display.print(F("MODE: SETUP"));
    else if (inTestMode)    display.print(F("MODE: TEST"));
    else if (millis() - lastPacketTime > PACKET_TIMEOUT_MS) display.print(F("MODE: NO SIGNAL"));
    else                    display.print(F("MODE: READY"));

    display.display();
}

void handleSetupMode() {
    uint8_t tempPropID = propID;
    unsigned long pressStartTime = 0;
    bool isPressed = false;
    bool lastIsPressed = false;
    
    while (true) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(34, 0);
        display.print(F("Setup Mode"));
        display.setTextSize(3);
        if (tempPropID < 10) display.setCursor(56, 9);
        else                 display.setCursor(47, 9);
        display.print(tempPropID);
        display.display();

        isPressed = (digitalRead(CONFIG_BUTTON_PIN) == LOW);

        if (isPressed && !lastIsPressed) pressStartTime = millis();

        if (!isPressed && lastIsPressed) {
            tempPropID++;
            if (tempPropID > TOTAL_PROPS) tempPropID = 1;
        }
        
        if (isPressed && (millis() - pressStartTime > 3000)) {
            propID = tempPropID;
            savePropID(propID);
            
            display.clearDisplay();
            display.setTextSize(2);
            display.setCursor(18, 8);
            display.print("SAVED!");
            display.display();

            for (int i = 0; i < 3; i++) {
                strip.fill(strip.Color(0, 255, 0), 0, NUM_LEDS); strip.show(); delay(150);
                strip.clear(); strip.show(); delay(150);
            }
            watchdog_reboot(0, 0, 100);
            
            // Wait for reboot - prevent button release from incrementing display
            while(true) { delay(10); }
        }

        lastIsPressed = isPressed;
        delay(20);
    }
}

// =============================== SETUP =============================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("Receiver v14.4 - Mesh Repeater Support"));
    Serial.println(F("15 sequences (0-14)"));
    Serial.println(F("6 Camera Flash sequences"));
    Serial.println(F("Blue Glow at full brightness"));
    EEPROM.begin(256);

    Wire1.setSDA(OLED_SDA_PIN);
    Wire1.setSCL(OLED_SCL_PIN);
    Wire1.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
    }
    display.setRotation(2);
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,0);
    display.print("Receiver Boot...");
    display.display();

    strip.begin();
    strip.setBrightness(255); // Maximum brightness
    strip.clear();
    markAllOff();
    showIfDirty();

    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    delay(100);

    propID = loadPropID();
    
    // Log repeater status
    Serial.print(F("Prop ID: "));
    Serial.print(propID);
    if (isRepeater()) {
        Serial.println(F(" - REPEATER ENABLED"));
    } else {
        Serial.println(F(" - Standard receiver"));
    }

    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        inSetupMode = true;
        handleSetupMode();
    }

    attachInterrupt(digitalPinToInterrupt(CONFIG_BUTTON_PIN), button_isr, FALLING);

    // --- RadioHead Initialization ---
    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);
    digitalWrite(RF69_RST_PIN, HIGH); delay(10);
    digitalWrite(RF69_RST_PIN, LOW);  delay(10);

    if (!driver.init()) {
        Serial.println(F("Radio init failed!"));
        radioInitialized = false;
    } else if (!driver.setFrequency(RF69_FREQ)) {
        Serial.println(F("Set frequency failed!"));
        radioInitialized = false;
    } else {
        driver.setEncryptionKey((uint8_t*)ENCRYPTKEY);
        driver.setTxPower(20, true);
        radioInitialized = true;
        Serial.println(F("Radio initialized OK"));
    }
    updateDisplay();
}

// ================================ LOOP ============================
void loop() {
    if (radioInitialized) {
        // --- NORMAL OPERATION ---
        if (driver.available()) {
            uint8_t buf[sizeof(RadioPacket)];
            uint8_t len = sizeof(buf);

            if (driver.recv(buf, &len)) {
                lastPacketTime = millis();
                lastRSSI = driver.lastRssi();
                RadioPacket packet;
                memcpy(&packet, buf, sizeof(packet));
                
                // Ignore packets with hopCount > 1 (prevents loops)
                if (packet.hopCount > 1) {
                    return;
                }
                
                bool sequenceChanged = false;
                if (currentSequence != packet.sequenceId) {
                    currentSequence = packet.sequenceId;
                    animationStep = 0;
                    sequenceStartTime = millis(); // Reset timer for timed animations
                    lastSequenceRun = 255; // Force animation reset
                    strip.clear();
                    markDirty();
                    if (currentSequence > 0) {
                        lastActiveAnimationTime = millis();
                    }
                    sequenceChanged = true;
                    
                    Serial.print(F("Prop "));
                    Serial.print(propID);
                    Serial.print(F(" - Seq: "));
                    Serial.print(currentSequence);
                    Serial.print(F(" | RSSI: "));
                    Serial.print(lastRSSI);
                    Serial.print(F(" | Pkt: "));
                    Serial.print(packet.packetCounter);
                    Serial.print(F(" | Hop: "));
                    Serial.print(packet.hopCount);
                    Serial.print(F(" | Src: "));
                    Serial.println(packet.sourceID == 0 ? "Remote" : String(packet.sourceID).c_str());
                }
                
                // MESH REPEATER: Retransmit if sequence changed and from remote
                if (sequenceChanged && packet.hopCount == 0) {
                    retransmitPacket(packet);
                }
                
                // Update last packet time even if sequence didn't change
                lastPacketTime = millis();
            }
        }

        if (millis() - lastPacketTime > PACKET_TIMEOUT_MS) {
            if (currentSequence != 0) {
                currentSequence = 0;
                animationStep = 0;
            }
        }
        
        if (currentSequence != 0 && (millis() - lastActiveAnimationTime > STRIP_TIMEOUT_MS)) {
            currentSequence = 0;
            animationStep = 0;
        }

        if (buttonPressFlag) {
            if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
                // Future: diagnostic mode
            } else {
                // Short press toggles Test Mode
                inTestMode = !inTestMode;
                if (!inTestMode) {
                    animationOff();
                }
                currentSequence = 0;
                animationStep = 0;
                buttonPressFlag = false;
                
                Serial.print(F("Test mode: "));
                Serial.println(inTestMode ? "ON" : "OFF");
            }
        }

        if (inTestMode)      animationRainbowChase(); // Test animation
        else                 runSequence();
        showIfDirty();
        
        if (millis() - lastDisplayUpdateTime > 250) {
            updateDisplay();
            lastDisplayUpdateTime = millis();
        }
    } else {
        // --- RADIO FAILURE MODE ---
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(5, 0);
        display.print(F("RADIO\nFAILED"));
        display.display();
        delay(500);
    }
}
