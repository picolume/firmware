/**
 * @file picolume_remote.ino
 * @brief PicoLume Master Clock Transmitter
 * @version 0.1.0
 * 
 * Broadcasts timecode to all receivers for synchronized show playback.
 * 
 * CUSTOMIZATION FOR OTHER BANDS:
 *   Update RF_BITRATE and ENCRYPT_KEY below to match your receivers.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <RH_RF69.h>

// ====================== CUSTOMIZATION =======================
// Other bands: Update these values to match your receivers
#define RF_BITRATE 19                       // Options: 2, 19, 57, 125, 250
#define ENCRYPT_KEY "GoMarchingBand!!"      // Exactly 16 characters

// ====================== PIN DEFINITIONS ===================
#define CYCLE_BUTTON_PIN 6
#define OFF_BUTTON_PIN 7

#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

#define RF69_CS_PIN 17
#define RF69_INT_PIN 20
#define RF69_RST_PIN 21
#define RF69_FREQ 915.0

// ====================== DATA STRUCTURES ===================
struct RadioPacket {
    uint32_t packetCounter;
    uint32_t masterTime;
    uint8_t  state;       // 0 = STOPPED, 1 = PLAYING
    uint8_t  hopCount;    // For mesh networking (0 = from remote)
    uint8_t  sourceID;    // 0 = remote, 1-224 = prop ID
};

// ====================== DEVICES ===================
LiquidCrystal_I2C lcd(0x3F, 16, 2);
RH_RF69 driver(RF69_CS_PIN, RF69_INT_PIN);

// ====================== STATE ===================
uint32_t masterTime = 0;
bool isPlaying = false;
uint32_t packetCount = 0;

unsigned long lastLoopTime = 0;
unsigned long lastTxTime = 0;

// Button Debounce
byte lastPlayState = HIGH;
byte lastResetState = HIGH;
unsigned long debounceTime = 0;

// ====================== SETUP ===================
void setup()
{
    Serial.begin(115200);

    // I2C / LCD
    Wire.setSDA(I2C_SDA_PIN);
    Wire.setSCL(I2C_SCL_PIN);
    Wire.begin();
    lcd.init();
    lcd.backlight();
    lcd.print("Master Clock");

    pinMode(CYCLE_BUTTON_PIN, INPUT_PULLUP);
    pinMode(OFF_BUTTON_PIN, INPUT_PULLUP);

    pinMode(RF69_RST_PIN, OUTPUT);
    digitalWrite(RF69_RST_PIN, LOW);
    delay(10);
    digitalWrite(RF69_RST_PIN, HIGH);
    delay(10);
    digitalWrite(RF69_RST_PIN, LOW);
    delay(10);

    if (!driver.init())
    {
        Serial.println("Radio Init Failed");
        lcd.setCursor(0, 1);
        lcd.print("Radio Fail");
    }

// --- RF BITRATE CONFIGURATION ---
#if RF_BITRATE == 2
    Serial.println(F("Radio: 2kbps (FSK_Rb2Fd5)"));
    if (!driver.setModemConfig(RH_RF69::FSK_Rb2Fd5))
        Serial.println("Config Fail");

#elif RF_BITRATE == 57
    Serial.println(F("Radio: 57.6kbps (GFSK_Rb57_6Fd120)"));
    if (!driver.setModemConfig(RH_RF69::GFSK_Rb57_6Fd120))
        Serial.println("Config Fail");

#elif RF_BITRATE == 125
    Serial.println(F("Radio: 125kbps (GFSK_Rb125Fd125)"));
    if (!driver.setModemConfig(RH_RF69::GFSK_Rb125Fd125))
        Serial.println("Config Fail");

#elif RF_BITRATE == 250
    Serial.println(F("Radio: 250kbps (GFSK_Rb250Fd250)"));
    if (!driver.setModemConfig(RH_RF69::GFSK_Rb250Fd250))
        Serial.println("Config Fail");

#else
    // Default to 19.2kbps (The "Sweet Spot") if 19 is selected or ID is unknown
    Serial.println(F("Radio: 19.2kbps (GFSK_Rb19_2Fd38_4)"));
    if (!driver.setModemConfig(RH_RF69::GFSK_Rb19_2Fd38_4))
        Serial.println("Config Fail");
#endif

    driver.setFrequency(RF69_FREQ);
    driver.setTxPower(20, true);
    driver.setEncryptionKey((uint8_t *)ENCRYPT_KEY);

    lastLoopTime = millis();
}

// ====================== LOOP ===================
void loop()
{
    unsigned long now = millis();
    unsigned long delta = now - lastLoopTime;
    lastLoopTime = now;

    if (isPlaying)
        masterTime += delta;

    // Buttons
    int playRead = digitalRead(CYCLE_BUTTON_PIN);
    int resetRead = digitalRead(OFF_BUTTON_PIN);

    if (playRead == LOW && lastPlayState == HIGH && (now - debounceTime > 200))
    {
        isPlaying = !isPlaying;
        debounceTime = now;
    }
    lastPlayState = playRead;

    if (resetRead == LOW && lastResetState == HIGH && (now - debounceTime > 200))
    {
        isPlaying = false;
        masterTime = 0;
        debounceTime = now;
    }
    lastResetState = resetRead;

    // Broadcast Timecode (10Hz)
    if (now - lastTxTime > 100)
    {
        RadioPacket packet;
        packet.packetCounter = packetCount++;
        packet.masterTime = masterTime;
        packet.state = isPlaying ? 1 : 0;
        packet.hopCount = 0;
        packet.sourceID = 0;

        driver.send((uint8_t *)&packet, sizeof(packet));
        driver.waitPacketSent();
        lastTxTime = now;
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }

    // LCD
    static unsigned long lastLcd = 0;
    if (now - lastLcd > 200)
    {
        lcd.setCursor(0, 0);
        lcd.print(isPlaying ? "PLAYING " : "STOPPED ");
        lcd.setCursor(0, 1);
        unsigned long s = masterTime / 1000;
        unsigned long ms = (masterTime % 1000) / 100;
        if (s < 10)
            lcd.print("0");
        lcd.print(s);
        lcd.print(".");
        lcd.print(ms);
        lcd.print(" s    ");
        lastLcd = now;
    }
}
