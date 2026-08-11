/**
 * @file rp2040_remote_complete.ino
 * @brief RP2040 Remote with Radio, Battery Monitoring, and Animation
 * @version 2.8.1
 * @date 2025-10-29
 *
 * V2.8.1 Changes:
 * - Restored battery indicator on LCD (right side of line 1)
 * - Shortened sequence names to fit:
 *   - "Camera Flash #X" → "Camera #X"
 *   - "Rainbow Chase" → "Color Chase"
 *   - "Rainbow Hold" → "Color Hold"
 *
 * V2.8 Changes:
 * - Added mesh repeater support
 * - Updated RadioPacket structure with hopCount and sourceID
 * - Remote always sends hopCount=0, sourceID=0 (remote identifier)
 * - Receivers can now retransmit packets for better field coverage
 *
 * V2.7 Changes:
 * - Updated to 15 sequences (0-14) per band director request
 * - Added 4 additional Camera Flash sequences (now 6 total: seq 1,2,9,10,11,12)
 * - Camera flashes now numbered (#1-#6) on display for clarity
 * - Removed Blue Build animation (seq 7 is now just Blue Glow at full brightness)
 * - Updated remote display: Top="Current: Name", Bottom="Next: Name"
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
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <RH_RF69.h>
#include <Adafruit_MAX1704X.h>

// ====================== PIN DEFINITIONS ===================
#define CYCLE_BUTTON_PIN    6
#define OFF_BUTTON_PIN      7

#define I2C_SDA_PIN         4
#define I2C_SCL_PIN         5

// RFM69 Radio Pins (matching receiver hardware)
#define RF69_CS_PIN         17
#define RF69_INT_PIN        20
#define RF69_RST_PIN        21
#define RF69_FREQ           915.0

// ====================== TIMING ===================
#define DEBOUNCE_DELAY_MS   50
#define HEARTBEAT_INTERVAL  500  // Send every 500ms
#define BATTERY_CHECK_INTERVAL_MS 5000
#define BURST_COUNT         5    // Number of packets to burst on sequence change
#define BURST_DELAY_MS      20   // Delay between burst packets

// ====================== DEVICES ===================
LiquidCrystal_I2C lcd(0x3F, 16, 2);
RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);
Adafruit_MAX17048 maxlipo;

const char ENCRYPTKEY[] = "GoMarchingBand!!";

// ====================== DATA STRUCTURES ===================
struct RadioPacket {
    uint32_t packetCounter;
    uint8_t  sequenceId;
    uint8_t  hopCount;    // 0=from remote, 1=first retransmit, 2+=ignore
    uint8_t  sourceID;    // 0=remote, 1-18=prop ID that sent this packet
};

// Sequence names (shortened to fit with battery indicator)
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

// ====================== STATE ===================
uint8_t currentSequence = 0;
uint32_t packetCounter = 0;
unsigned long lastHeartbeatTime = 0;

byte cycleButtonState = HIGH;
byte lastCycleButtonState = HIGH;
unsigned long lastCycleDebounceTime = 0;

byte offButtonState = HIGH;
byte lastOffButtonState = HIGH;
unsigned long lastOffDebounceTime = 0;

bool ledIsON = false;
unsigned long ledOnTime = 0;

// Battery monitoring
unsigned long lastBatteryCheckTime = 0;
float lastBatteryPercent = 0.0;
bool isCharging = false;

// Custom LCD battery icons
byte battery_100[8] = { 0b01110, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b01110 };
byte battery_75[8]  = { 0b01110, 0b10001, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b01110 };
byte battery_50[8]  = { 0b01110, 0b10001, 0b10001, 0b10001, 0b11111, 0b11111, 0b11111, 0b01110 };
byte battery_25[8]  = { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111, 0b01110 };
byte battery_0[8]   = { 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 };

// ========================== RADIO ========================

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
    ledIsON = true;
    ledOnTime = millis();
}

// Burst transmission for sequence changes (send multiple packets rapidly)
void transmitSequenceBurst() {
    Serial.print("TX BURST: Seq=");
    Serial.print(currentSequence);
    Serial.print(" (");
    Serial.print(sequenceNames[currentSequence]);
    Serial.print(") - Sending ");
    Serial.print(BURST_COUNT);
    Serial.println(" packets");
    
    for (int i = 0; i < BURST_COUNT; i++) {
        sendSinglePacket();
        
        // Small delay between burst packets
        if (i < BURST_COUNT - 1) {
            delay(BURST_DELAY_MS);
        }
    }
    
    Serial.print("  Burst complete - Packets ");
    Serial.print(packetCounter - BURST_COUNT + 1);
    Serial.print(" to ");
    Serial.println(packetCounter);
}

// Single transmission for heartbeat (keep receivers in sync)
void transmitSequenceHeartbeat() {
    sendSinglePacket();
    
    // Less verbose logging for heartbeat
    Serial.print("TX HEARTBEAT: Seq=");
    Serial.print(currentSequence);
    Serial.print(" Pkt=");
    Serial.println(packetCounter);
}

// ========================== DISPLAY ========================
void updateDisplay() {
    lcd.clear();
    
    // Line 1: Current sequence + battery icon
    lcd.setCursor(0, 0);
    lcd.print("Cur:");
    lcd.print(sequenceNames[currentSequence]);
    
    // Battery icon at far right of line 1
    float percent = maxlipo.cellPercent();
    lcd.setCursor(15, 0);
    if (percent > 85)      lcd.write(byte(0));  // 100%
    else if (percent > 60) lcd.write(byte(1));  // 75%
    else if (percent > 40) lcd.write(byte(2));  // 50%
    else if (percent > 15) lcd.write(byte(3));  // 25%
    else                   lcd.write(byte(4));  // 0%
    
    // Line 2: Next sequence
    lcd.setCursor(0, 1);
    lcd.print("Nxt:");
    uint8_t nextSeq = (currentSequence + 1) % NUM_SEQUENCES;
    lcd.print(sequenceNames[nextSeq]);
}

// ========================== BUTTON HANDLING ========================
bool checkCycleButtonPress() {
    int reading = digitalRead(CYCLE_BUTTON_PIN);
    
    if (reading != lastCycleButtonState) {
        lastCycleDebounceTime = millis();
    }
    
    bool pressed = false;
    if ((millis() - lastCycleDebounceTime) > DEBOUNCE_DELAY_MS) {
        if (reading != cycleButtonState) {
            cycleButtonState = reading;
            if (cycleButtonState == LOW) {
                pressed = true;
            }
        }
    }
    
    lastCycleButtonState = reading;
    return pressed;
}

bool checkOffButtonPress() {
    int reading = digitalRead(OFF_BUTTON_PIN);
    
    if (reading != lastOffButtonState) {
        lastOffDebounceTime = millis();
    }
    
    bool pressed = false;
    if ((millis() - lastOffDebounceTime) > DEBOUNCE_DELAY_MS) {
        if (reading != offButtonState) {
            offButtonState = reading;
            if (offButtonState == LOW) {
                pressed = true;
            }
        }
    }
    
    lastOffButtonState = reading;
    return pressed;
}

// =============================== SETUP =============================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== RP2040 Remote v2.8.1 - Mesh Repeater + Battery Display ===");
    Serial.println("Sequences 0-14 for band show");
    Serial.println("6 Camera Flash sequences (numbered)");
    Serial.println("Mesh repeater support enabled");
    Serial.println("Battery indicator on display");
    Serial.println("Now with packet bursting for reliability!");
    
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(CYCLE_BUTTON_PIN, INPUT_PULLUP);
    pinMode(OFF_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);
    
    // I2C Initialization
    Wire.setSDA(I2C_SDA_PIN);
    Wire.setSCL(I2C_SCL_PIN);
    Wire.begin();
    Serial.println("I2C initialized");

    lcd.init();
    lcd.backlight();
    Serial.println("LCD initialized");
    
    // Load battery icons into LCD memory
    lcd.createChar(0, battery_100);
    lcd.createChar(1, battery_75);
    lcd.createChar(2, battery_50);
    lcd.createChar(3, battery_25);
    lcd.createChar(4, battery_0);
    
    // Splash screen
    lcd.setCursor(0, 0);
    lcd.print(F("Summerville Band"));
    lcd.setCursor(0, 1);
    lcd.print(F("Remote v2.8.1"));
    delay(1500);
    lcd.clear();
    
    // Initialize Battery Gauge
    Serial.print("Battery gauge...");
    if (!maxlipo.begin()) {
        Serial.println(" FAILED!");
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
    Serial.println(" OK");
    
    lastBatteryPercent = maxlipo.cellPercent();
    Serial.print("Battery: ");
    Serial.print(lastBatteryPercent);
    Serial.println("%");
    
    // Initialize Radio
    Serial.print("Radio init...");
    digitalWrite(RF69_RST_PIN, HIGH); 
    delay(10);
    digitalWrite(RF69_RST_PIN, LOW);  
    delay(10);

    if (!driver.init()) { 
        Serial.println(" FAILED!");
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
    Serial.println(" OK");
    
    Serial.print("Setting frequency...");
    if (!driver.setFrequency(RF69_FREQ)) { 
        Serial.println(" FAILED!");
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
    Serial.println(" OK");
    
    driver.setTxPower(20, true);
    driver.setEncryptionKey((uint8_t*)ENCRYPTKEY);
    Serial.println("Radio ready: 915MHz, 20dBm");
    
    // Display initial state
    updateDisplay();
    
    lastHeartbeatTime = millis();
    
    Serial.println("=== READY ===");
    Serial.println("Cycle button: Next sequence (BURST MODE)");
    Serial.println("Off button: LEDs Off (BURST MODE)");
    Serial.println("Heartbeat: Every 500ms");
    Serial.println("Repeater props: 1, 6, 12, 18 will retransmit\n");
}

// ================================ LOOP =============================
void loop() {
    bool needsDisplayUpdate = false;
    
    // Check Cycle Button
    if (checkCycleButtonPress()) {
        Serial.println(">>> Cycle button pressed");
        
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
    
    // Check Off Button
    if (checkOffButtonPress()) {
        Serial.println(">>> Off button pressed");
        
        if (currentSequence != 0) {
            currentSequence = 0;
            
            // BURST TRANSMISSION on sequence change
            transmitSequenceBurst();
            lastHeartbeatTime = millis();
            needsDisplayUpdate = true;
        } else {
            Serial.println("Already at LEDs Off");
        }
    }
    
    // Heartbeat transmission (keep receivers in sync)
    if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
        lastHeartbeatTime = millis();
        transmitSequenceHeartbeat();
    }
    
    // Battery check
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
        
        Serial.print("Battery: ");
        Serial.print(currentPercent);
        Serial.print("%");
        if (isCharging) Serial.print(" (CHARGING)");
        Serial.println();
        
        // Update display to refresh battery icon
        needsDisplayUpdate = true;
    }
    
    // Turn off LED after transmission
    if (ledIsON && (millis() - ledOnTime >= 50)) {
        digitalWrite(LED_BUILTIN, LOW);
        ledIsON = false;
    }
    
    // Update display if needed
    if (needsDisplayUpdate) {
        updateDisplay();
    }
}
