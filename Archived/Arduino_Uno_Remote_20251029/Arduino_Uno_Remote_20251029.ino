/**
 * @file uno_remote_radiohead_v11.0.ino
 * @brief Arduino Uno Remote updated to match RP2040 v2.8.1 functionality
 * @version 11.0
 * @date 2025-10-30
 *
 * V11.0 Changes:
 * - Updated to match RP2040 v2.8.1 standard
 * - Increased to 15 sequences (0-14) matching RP2040
 * - Added mesh repeater support (hopCount and sourceID in RadioPacket)
 * - Added burst transmission (5 packets on sequence change)
 * - Updated display format: "Cur:[name]" / "Nxt:[name]"
 * - Shortened sequence names to match RP2040
 * - Changed Purple Glow/Build to Blue Glow
 * - Updated Rainbow names to Color Chase/Hold
 * - 6 Camera Flash sequences numbered #1-#6
 * - Removed dot animation (no longer needed with burst mode)
 *
 * Sequence List (0-14):
 * 0:  LEDs Off
 * 1:  Camera #1
 * 2:  Camera #2
 * 3:  Red Glow
 * 4:  LEDs Off
 * 5:  Red Flash
 * 6:  LEDs Off
 * 7:  Blue Glow
 * 8:  LEDs Off
 * 9:  Camera #3
 * 10: Camera #4
 * 11: Camera #5
 * 12: Camera #6
 * 13: Color Chase
 * 14: Color Hold
 */

// ============================ LIBRARIES ============================
#include <SPI.h>
#include <RH_RF69.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MAX1704X.h>
#include <stdio.h>

// ====================== HARDWARE & SYSTEM CONFIG ===================
#define CYCLE_BUTTON_PIN    3
#define OFF_BUTTON_PIN      7
#define HEARTBEAT_INTERVAL  500       // ms
#define DEBOUNCE_DELAY_MS   50        // ms
#define BATTERY_CHECK_INTERVAL_MS 5000 // Check battery every 5 seconds
#define BURST_COUNT         5         // Number of packets to burst on sequence change
#define BURST_DELAY_MS      20        // Delay between burst packets

// --- RFM69 Radio ---
#define RF69_FREQ           915.0
#define RF69_CS_PIN         10
#define RF69_INT_PIN        2
#define RF69_RST_PIN        9
const char ENCRYPTKEY[] = "GoMarchingBand!!";
RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// --- I2C Devices ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_MAX17048 maxlipo;

// =========================== DATA & STATE ==========================
struct RadioPacket {
    uint32_t packetCounter;
    uint8_t  sequenceId;
    uint8_t  hopCount;    // 0=from remote, 1=first retransmit, 2+=ignore
    uint8_t  sourceID;    // 0=remote, 1-18=prop ID that sent this packet
};

// --- Sequence Definitions (matching RP2040 v2.8.1) ---
const char* sequenceNames[15] = {
  "LEDs Off",      // 0
  "Camera #1",     // 1
  "Camera #2",     // 2
  "Red Glow",      // 3
  "LEDs Off",      // 4
  "Red Flash",     // 5
  "LEDs Off",      // 6
  "Blue Glow",     // 7
  "LEDs Off",      // 8
  "Camera #3",     // 9
  "Camera #4",     // 10
  "Camera #5",     // 11
  "Camera #6",     // 12
  "Color Chase",   // 13
  "Color Hold"     // 14
};
const int NUM_SEQUENCES = 15;

uint8_t currentSequence = 0;
uint32_t packetCounter = 0;
unsigned long lastHeartbeatTime = 0;

// --- Display ---
unsigned long lastBatteryCheckTime = 0;
float lastBatteryPercent = 0.0; 
bool isCharging = false;        

// --- Button Debouncing (Cycle Button) ---
byte cycleButtonState = HIGH;
byte lastCycleButtonState = HIGH;
unsigned long lastCycleDebounceTime = 0;

// --- Button Debouncing (Off Button) ---
byte offButtonState = HIGH;
byte lastOffButtonState = HIGH;
unsigned long lastOffDebounceTime = 0;

// --- Built-in LED blink ---
bool ledIsOn = false;
unsigned long ledOnTime = 0;

// --- Custom LCD Characters for Battery ---
byte battery_100[8] = { 0b01110, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b01110 };
byte battery_75[8]  = { 0b01110, 0b10001, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b01110 };
byte battery_50[8]  = { 0b01110, 0b10001, 0b10001, 0b10001, 0b11111, 0b11111, 0b11111, 0b01110 };
byte battery_25[8]  = { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111, 0b01110 };
byte battery_0[8]   = { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 };

// ========================= TRANSMISSION ============================

// Send a single packet (used by both burst and heartbeat)
void sendSinglePacket() {
    RadioPacket packet;
    packet.sequenceId = currentSequence;
    packet.packetCounter = ++packetCounter;
    packet.hopCount = 0;   // Remote always sends with hopCount=0
    packet.sourceID = 0;   // Remote ID = 0
    
    driver.send((uint8_t*)&packet, sizeof(packet));
    driver.waitPacketSent();
    
    // Blink LED to show transmission
    digitalWrite(LED_BUILTIN, HIGH);
    ledIsOn = true;
    ledOnTime = millis();
}

// Burst transmission for sequence changes (send multiple packets rapidly)
void transmitSequenceBurst() {
    Serial.print(F("TX BURST: Seq="));
    Serial.print(currentSequence);
    Serial.print(F(" ("));
    Serial.print(sequenceNames[currentSequence]);
    Serial.print(F(") - Sending "));
    Serial.print(BURST_COUNT);
    Serial.println(F(" packets"));
    
    for (int i = 0; i < BURST_COUNT; i++) {
        sendSinglePacket();
        
        // Small delay between burst packets
        if (i < BURST_COUNT - 1) {
            delay(BURST_DELAY_MS);
        }
    }
    
    Serial.print(F("  Burst complete - Packets "));
    Serial.print(packetCounter - BURST_COUNT + 1);
    Serial.print(F(" to "));
    Serial.println(packetCounter);
}

// Single transmission for heartbeat (keep receivers in sync)
void transmitSequenceHeartbeat() {
    sendSinglePacket();
    
    // Less verbose logging for heartbeat
    Serial.print(F("TX HEARTBEAT: Seq="));
    Serial.print(currentSequence);
    Serial.print(F(" Pkt="));
    Serial.println(packetCounter);
}

// ======================== DISPLAY & UI =============================
void updateDisplay() {
    lcd.clear();
    
    // --- Line 1: Current sequence + battery icon ---
    lcd.setCursor(0, 0);
    lcd.print(F("Cur:"));
    lcd.print(sequenceNames[currentSequence]);
    
    // Battery icon at far right of line 1
    float percent = maxlipo.cellPercent();
    lcd.setCursor(15, 0);
    if (percent > 85)      lcd.write(byte(0));  // 100%
    else if (percent > 60) lcd.write(byte(1));  // 75%
    else if (percent > 40) lcd.write(byte(2));  // 50%
    else if (percent > 15) lcd.write(byte(3));  // 25%
    else                   lcd.write(byte(4));  // 0%

    // --- Line 2: Next sequence ---
    lcd.setCursor(0, 1);
    lcd.print(F("Nxt:"));
    uint8_t nextSeq = (currentSequence + 1) % NUM_SEQUENCES;
    lcd.print(sequenceNames[nextSeq]);
}

// ========================== BUTTON HANDLING ========================
bool checkCycleButtonPress() {
    bool pressed = false;
    int reading = digitalRead(CYCLE_BUTTON_PIN);
    if (reading != lastCycleButtonState) { lastCycleDebounceTime = millis(); }
    if ((millis() - lastCycleDebounceTime) > DEBOUNCE_DELAY_MS) {
        if (reading != cycleButtonState) {
            cycleButtonState = reading;
            if (cycleButtonState == LOW) { pressed = true; }
        }
    }
    lastCycleButtonState = reading;
    return pressed;
}

bool checkOffButtonPress() {
    bool pressed = false;
    int reading = digitalRead(OFF_BUTTON_PIN);
    if (reading != lastOffButtonState) { lastOffDebounceTime = millis(); }
    if ((millis() - lastOffDebounceTime) > DEBOUNCE_DELAY_MS) {
        if (reading != offButtonState) {
            offButtonState = reading;
            if (offButtonState == LOW) { pressed = true; }
        }
    }
    lastOffButtonState = reading;
    return pressed;
}

// =============================== SETUP =============================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("\n=== Arduino Uno Remote v11.0 - RP2040 v2.8.1 Standard ==="));
    Serial.println(F("Sequences 0-14 for band show"));
    Serial.println(F("6 Camera Flash sequences (numbered)"));
    Serial.println(F("Mesh repeater support enabled"));
    Serial.println(F("Now with packet bursting for reliability!"));
    
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(CYCLE_BUTTON_PIN, INPUT_PULLUP);
    pinMode(OFF_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);

    lcd.init();
    lcd.backlight();
    Serial.println(F("LCD initialized"));
    
    // Load all battery icons into LCD memory
    lcd.createChar(0, battery_100);
    lcd.createChar(1, battery_75);
    lcd.createChar(2, battery_50);
    lcd.createChar(3, battery_25);
    lcd.createChar(4, battery_0);
    
    lcd.setCursor(0, 0);
    lcd.print(F("Summerville Band"));
    lcd.setCursor(0, 1);
    lcd.print(F("Remote v11.0"));
    delay(1500);
    lcd.clear();

    // Initialize Battery Fuel Gauge
    Serial.print(F("Battery gauge..."));
    if (!maxlipo.begin()) {
        Serial.println(F(" FAILED!"));
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("BATTERY FAIL!"));
        while (true) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(200);
            digitalWrite(LED_BUILTIN, LOW);
            delay(200);
        }
    }
    Serial.println(F(" OK"));
    
    lastBatteryPercent = maxlipo.cellPercent(); 
    Serial.print(F("Battery: "));
    Serial.print(lastBatteryPercent);
    Serial.println(F("%"));

    // --- RadioHead Initialization Sequence ---
    Serial.print(F("Radio init..."));
    digitalWrite(RF69_RST_PIN, HIGH); delay(10);
    digitalWrite(RF69_RST_PIN, LOW);  delay(10);

    if (!driver.init()) { 
        Serial.println(F(" FAILED!"));
        lcd.clear(); 
        lcd.setCursor(0, 0);
        lcd.print(F("RADIO INIT FAIL!")); 
        while (true) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(500);
            digitalWrite(LED_BUILTIN, LOW);
            delay(500);
        }
    }
    Serial.println(F(" OK"));
    
    Serial.print(F("Setting frequency..."));
    if (!driver.setFrequency(RF69_FREQ)) { 
        Serial.println(F(" FAILED!"));
        lcd.clear(); 
        lcd.setCursor(0, 0);
        lcd.print(F("FREQ SET FAIL!")); 
        while (true) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(1000);
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
        }
    }
    Serial.println(F(" OK"));
    
    driver.setTxPower(20, true);
    driver.setEncryptionKey((uint8_t*)ENCRYPTKEY);
    Serial.println(F("Radio ready: 915MHz, 20dBm"));

    // Display initial state
    updateDisplay();
    lastHeartbeatTime = millis();
    
    Serial.println(F("=== READY ==="));
    Serial.println(F("Cycle button: Next sequence (BURST MODE)"));
    Serial.println(F("Off button: LEDs Off (BURST MODE)"));
    Serial.println(F("Heartbeat: Every 500ms"));
    Serial.println(F("Repeater props: 1, 6, 12, 18 will retransmit\n"));
}

// ================================ LOOP =============================
void loop() {
    bool needsDisplayUpdate = false;
    
    // 1. Handle Cycle Button Input
    if (checkCycleButtonPress()) {
        Serial.println(F(">>> Cycle button pressed"));
        
        // Cycle through sequences 0-14
        currentSequence++;
        if (currentSequence >= NUM_SEQUENCES) {
            currentSequence = 0;
        }
        
        // BURST TRANSMISSION on sequence change
        transmitSequenceBurst();
        lastHeartbeatTime = millis();
        needsDisplayUpdate = true;
    }

    // 2. Handle Off Button Input
    if (checkOffButtonPress()) {
        Serial.println(F(">>> Off button pressed"));
        
        if (currentSequence != 0) {
            currentSequence = 0;
            
            // BURST TRANSMISSION on sequence change
            transmitSequenceBurst();
            lastHeartbeatTime = millis();
            needsDisplayUpdate = true;
        } else {
            Serial.println(F("Already at LEDs Off"));
        }
    }

    // 3. Handle Heartbeat Transmission (keep receivers in sync)
    if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
        lastHeartbeatTime = millis();
        transmitSequenceHeartbeat();
    }

    // 4. Handle Battery Check
    if (millis() - lastBatteryCheckTime > BATTERY_CHECK_INTERVAL_MS) {
        lastBatteryCheckTime = millis();
        float currentPercent = maxlipo.cellPercent();
        
        // Detect charging (battery percentage increasing)
        if (currentPercent > lastBatteryPercent + 0.01) {
            isCharging = true;
        } else {
            isCharging = false;
        }
        lastBatteryPercent = currentPercent;

        Serial.print(F("Battery: "));
        Serial.print(currentPercent);
        Serial.print(F("%"));
        if (isCharging) Serial.print(F(" (CHARGING)"));
        Serial.println();

        // Update display to refresh battery icon
        needsDisplayUpdate = true;
    }
    
    // 5. Turn off LED after transmission blink
    if (ledIsOn && (millis() - ledOnTime >= 50)) {
        digitalWrite(LED_BUILTIN, LOW);
        ledIsOn = false;
    }

    // 6. Update Display (if needed)
    if (needsDisplayUpdate) {
        updateDisplay();
    }
}
