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
#include <FatFS.h>
#include <FatFSUSB.h>

// ====================== CUSTOMIZATION =======================
// Other bands: Update these values to match your receivers
#define RF_BITRATE 19                       // Options: 2, 19, 57, 125, 250
#define ENCRYPT_KEY "GoMarchingBand!!"      // Exactly 16 characters

// ====================== PIN DEFINITIONS ===================
#define CONFIG_STOP_PIN 3   // Hold on boot for USB mode, press to stop
#define PLAY_PAUSE_PIN 15   // Play/Pause toggle

#define CUE_A_PIN 6
#define CUE_B_PIN 7
#define CUE_C_PIN 8
#define CUE_D_PIN 9

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

// CUE block structure (32 bytes, appended to show.bin)
struct CueBlock {
    uint32_t magic;      // "CUE1" = 0x31455543 (little-endian)
    uint16_t version;    // 1
    uint16_t count;      // 4
    uint32_t times[4];   // Cue A, B, C, D times in ms (0xFFFFFFFF = unused)
    uint8_t reserved[8];
};

#define CUE_MAGIC 0x31455543
#define CUE_UNUSED 0xFFFFFFFF

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
byte lastCueAState = HIGH;
byte lastCueBState = HIGH;
byte lastCueCState = HIGH;
byte lastCueDState = HIGH;
byte lastStopState = HIGH;
byte lastPlayState = HIGH;
unsigned long debounceTime = 0;
uint8_t activeCue = 0;  // 0 = none, 1-4 = Cue A-D

// Cue times loaded from show.bin
uint32_t cueTimes[4] = {CUE_UNUSED, CUE_UNUSED, CUE_UNUSED, CUE_UNUSED};
bool cuesLoaded = false;
uint8_t cuesConfigured = 0;  // Count of configured cues (for LCD)

// USB mode flag
volatile bool usbUnplugged = false;

// ====================== CUE LOADING ===================
bool loadCuesFromFlash() {
    File f = FatFS.open("/show.bin", "r");
    if (!f) {
        Serial.println("No show.bin found");
        return false;
    }

    // Seek to last 32 bytes (CUE block location)
    size_t fileSize = f.size();
    if (fileSize < 32) {
        Serial.println("show.bin too small");
        f.close();
        return false;
    }

    f.seek(fileSize - 32);

    CueBlock cueBlock;
    if (f.read((uint8_t*)&cueBlock, sizeof(cueBlock)) != sizeof(cueBlock)) {
        Serial.println("Failed to read CUE block");
        f.close();
        return false;
    }
    f.close();

    // Check magic "CUE1"
    if (cueBlock.magic != CUE_MAGIC) {
        Serial.println("No CUE block in show.bin");
        return false;
    }

    // Copy cue times and count configured cues
    cuesConfigured = 0;
    Serial.println("Cues loaded:");
    for (int i = 0; i < 4; i++) {
        cueTimes[i] = cueBlock.times[i];
        Serial.print("  Cue ");
        Serial.print((char)('A' + i));
        Serial.print(": ");
        if (cueTimes[i] != CUE_UNUSED) {
            Serial.print(cueTimes[i]);
            Serial.println(" ms");
            cuesConfigured++;
        } else {
            Serial.println("not set");
        }
    }

    return true;
}

// ====================== USB MODE ===================
void onUSBPlug() {
    Serial.println("USB connected");
}

void onUSBUnplug() {
    Serial.println("USB disconnected");
    usbUnplugged = true;
}

void enterUSBMode() {
    lcd.clear();
    lcd.print("USB Mode");
    lcd.setCursor(0, 1);
    lcd.print("Upload show.bin");

    usbUnplugged = false;

    FatFS.begin();
    FatFSUSB.onPlug(onUSBPlug);
    FatFSUSB.onUnplug(onUSBUnplug);
    FatFSUSB.begin();

    // Wait for USB to be unplugged
    while (!usbUnplugged) {
        delay(100);
    }

    FatFSUSB.end();
    FatFS.end();

    lcd.clear();
    lcd.print("Rebooting...");
    delay(500);

    // Reboot to reload cues
    rp2040.reboot();
}

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

    pinMode(CONFIG_STOP_PIN, INPUT_PULLUP);
    pinMode(PLAY_PAUSE_PIN, INPUT_PULLUP);
    pinMode(CUE_A_PIN, INPUT_PULLUP);
    pinMode(CUE_B_PIN, INPUT_PULLUP);
    pinMode(CUE_C_PIN, INPUT_PULLUP);
    pinMode(CUE_D_PIN, INPUT_PULLUP);

    // Check if CONFIG button is held on boot to enter USB mode
    if (digitalRead(CONFIG_STOP_PIN) == LOW) {
        enterUSBMode();
        // enterUSBMode() reboots, so we won't reach here
    }

    // Load cues from show.bin
    lcd.setCursor(0, 1);
    lcd.print("Loading...");
    if (FatFS.begin()) {
        cuesLoaded = loadCuesFromFlash();
        FatFS.end();
    } else {
        Serial.println("FatFS mount failed");
    }

    // Show cue status briefly
    lcd.setCursor(0, 1);
    if (cuesLoaded) {
        lcd.print("Cues: ");
        lcd.print(cuesConfigured);
        lcd.print("/4       ");
    } else {
        lcd.print("No cues      ");
    }
    delay(1000);

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

    // Control Buttons
    int stopRead = digitalRead(CONFIG_STOP_PIN);
    int playRead = digitalRead(PLAY_PAUSE_PIN);

    // Stop button - stops playback and resets to 0
    if (stopRead == LOW && lastStopState == HIGH && (now - debounceTime > 200))
    {
        isPlaying = false;
        masterTime = 0;
        activeCue = 0;
        debounceTime = now;
    }
    lastStopState = stopRead;

    // Play/Pause button - toggle play/pause state
    if (playRead == LOW && lastPlayState == HIGH && (now - debounceTime > 200))
    {
        // Simple toggle: if playing, pause; if paused/stopped, play
        isPlaying = !isPlaying;
        debounceTime = now;
    }
    lastPlayState = playRead;

    // Cue Buttons
    int cueARead = digitalRead(CUE_A_PIN);
    int cueBRead = digitalRead(CUE_B_PIN);
    int cueCRead = digitalRead(CUE_C_PIN);
    int cueDRead = digitalRead(CUE_D_PIN);

    // Cue A - only act if cue is defined
    if (cueARead == LOW && lastCueAState == HIGH && (now - debounceTime > 200))
    {
        if (cueTimes[0] != CUE_UNUSED) {
            activeCue = 1;
            isPlaying = true;
            masterTime = cueTimes[0];
        }
        debounceTime = now;
    }
    lastCueAState = cueARead;

    // Cue B - only act if cue is defined
    if (cueBRead == LOW && lastCueBState == HIGH && (now - debounceTime > 200))
    {
        if (cueTimes[1] != CUE_UNUSED) {
            activeCue = 2;
            isPlaying = true;
            masterTime = cueTimes[1];
        }
        debounceTime = now;
    }
    lastCueBState = cueBRead;

    // Cue C - only act if cue is defined
    if (cueCRead == LOW && lastCueCState == HIGH && (now - debounceTime > 200))
    {
        if (cueTimes[2] != CUE_UNUSED) {
            activeCue = 3;
            isPlaying = true;
            masterTime = cueTimes[2];
        }
        debounceTime = now;
    }
    lastCueCState = cueCRead;

    // Cue D - only act if cue is defined
    if (cueDRead == LOW && lastCueDState == HIGH && (now - debounceTime > 200))
    {
        if (cueTimes[3] != CUE_UNUSED) {
            activeCue = 4;
            isPlaying = true;
            masterTime = cueTimes[3];
        }
        debounceTime = now;
    }
    lastCueDState = cueDRead;

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
        if (isPlaying)
        {
            if (activeCue > 0) {
                lcd.print("CUE ");
                lcd.print((char)('A' + activeCue - 1));
                lcd.print(" PLAY   ");
            } else {
                lcd.print("PLAYING         ");
            }
        }
        else
        {
            if (masterTime == 0) {
                lcd.print("STOPPED         ");
            } else {
                lcd.print("PAUSED          ");
            }
        }
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
