/*
 Hardware:
    - ESP32-S3-N16R8 with native USB OTG
    - 10 footswitches: GPIO -> switch -> GND, using INPUT_PULLUP
    - SH1106 1.3-inch I2C OLED: GND, 3V3, SCL=GPIO18, SDA=GPIO17

  USB-C usage on your board:
    - Right USB-C / CH343P: upload + serial monitor
    - Left USB-C / native USB OTG: class-compliant USB-MIDI device

  Arduino IDE settings:
    Board: ESP32S3 Dev Module
    USB Mode: USB-OTG / TinyUSB
    USB CDC On Boot: Disabled
    Flash Size: 16MB
    PSRAM: OPI PSRAM
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "USB.h"
#include "USBMIDI.h"

// ============================================================
// OLED CONFIG
// ============================================================

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C

#define OLED_SDA     17
#define OLED_SCL     18

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ============================================================
// USB MIDI
// ============================================================

USBMIDI MIDI("ESP32-S3 Foot Controller");

// ============================================================
// FOOTSWITCH CONFIG
// ============================================================

static const uint8_t NUM_SWITCHES = 10;

static const uint8_t FS_PINS[NUM_SWITCHES] = {
  4,   // FS1
  5,   // FS2
  6,   // FS3
  7,   // FS4
  8,   // FS5
  9,   // FS6
  10,  // FS7
  11,  // FS8
  12,  // FS9
  13   // FS10
};

static const uint32_t DEBOUNCE_MS = 35;

// Debounce states
bool lastRaw[NUM_SWITCHES];
bool stableState[NUM_SWITCHES];
uint32_t lastChangeTime[NUM_SWITCHES];

// ============================================================
// PROFILE CONFIG
// ============================================================

enum ProfileId {
  PROFILE_MODEP = 0,
  PROFILE_NEURAL_DSP = 1,
  PROFILE_GENERIC = 2
};

ProfileId activeProfile = PROFILE_MODEP;

// MIDI channels are user-facing 1..16 here.
static const uint8_t MODEP_PEDALBOARD_CH = 15;
static const uint8_t MODEP_SNAPSHOT_CH   = 14;
static const uint8_t MODEP_CC_CH         = 14;

static const uint8_t NEURAL_DSP_CH       = 1;
static const uint8_t GENERIC_CH          = 1;

// MODEP mappings inspired by your Pico controller.
static const uint8_t MODEP_CC_TUNER  = 20;
static const uint8_t MODEP_CC_DELAY  = 21;
static const uint8_t MODEP_CC_REVERB = 22;
static const uint8_t MODEP_CC_BOOST  = 23;

// Neural DSP note mappings from your older Arduino Mega-style controller.
static const uint8_t NEURAL_NOTES[8] = {
  36, 37, 38, 39, 40, 41, 42, 43
};

// Generic CC mapping.
static const uint8_t GENERIC_CCS[8] = {
  20, 21, 22, 23, 24, 25, 26, 27
};

// Toggle states
bool modepTuner  = false;
bool modepDelay  = false;
bool modepReverb = false;
bool modepBoost  = false;

bool genericToggle[8] = {false, false, false, false, false, false, false, false};

// For simple test
uint8_t currentPedalboard = 0;
static const uint8_t TOTAL_PEDALBOARDS = 16;

// ============================================================
// HELPERS
// ============================================================

const char* profileName(ProfileId p) {
  switch (p) {
    case PROFILE_MODEP:      return "MODEP";
    case PROFILE_NEURAL_DSP: return "Neural DSP";
    case PROFILE_GENERIC:    return "Generic";
    default:                 return "Unknown";
  }
}

void drawStatus(const char* line1, const char* line2, const char* line3) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("ESP32-S3 MIDI CTRL");

  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.print("Profile: ");
  display.print(profileName(activeProfile));

  display.setCursor(0, 30);
  display.print(line1);

  display.setCursor(0, 42);
  display.print(line2);

  display.setCursor(0, 54);
  display.print(line3);

  display.display();
}

void sendNotePulse(uint8_t channel, uint8_t note) {
  MIDI.noteOn(note, 127, channel);
  delay(20);
  MIDI.noteOff(note, 0, channel);
}

void sendCC(uint8_t channel, uint8_t cc, uint8_t value) {
  MIDI.controlChange(cc, value, channel);
}

void sendProgramChange(uint8_t channel, uint8_t program) {
  MIDI.programChange(program, channel);
}

void nextProfile() {
  if (activeProfile == PROFILE_GENERIC) {
    activeProfile = PROFILE_MODEP;
  } else {
    activeProfile = (ProfileId)((uint8_t)activeProfile + 1);
  }

  drawStatus("Profile changed", "FS10 = next", "");
}

void prevProfile() {
  if (activeProfile == PROFILE_MODEP) {
    activeProfile = PROFILE_GENERIC;
  } else {
    activeProfile = (ProfileId)((uint8_t)activeProfile - 1);
  }

  drawStatus("Profile changed", "FS9 = previous", "");
}

// ============================================================
// PROFILE ACTIONS
// ============================================================

void handleMODEP(uint8_t fsIndex) {
  // fsIndex is 0..9
  // FS1..FS10 correspond to fsIndex 0..9.

  char line1[32];
  char line2[32];
  char line3[32];

  switch (fsIndex) {
    case 0: // FS1: Pedalboard next
      currentPedalboard = (currentPedalboard + 1) % TOTAL_PEDALBOARDS;
      sendProgramChange(MODEP_PEDALBOARD_CH, currentPedalboard);
      snprintf(line1, sizeof(line1), "FS1: PB Next");
      snprintf(line2, sizeof(line2), "PC %u on CH %u", currentPedalboard, MODEP_PEDALBOARD_CH);
      snprintf(line3, sizeof(line3), "");
      break;

    case 1: // FS2: Snapshot 1
      sendProgramChange(MODEP_SNAPSHOT_CH, 1);
      snprintf(line1, sizeof(line1), "FS2: Snapshot 1");
      snprintf(line2, sizeof(line2), "PC 1 on CH %u", MODEP_SNAPSHOT_CH);
      snprintf(line3, sizeof(line3), "");
      break;

    case 2: // FS3: Snapshot 2
      sendProgramChange(MODEP_SNAPSHOT_CH, 2);
      snprintf(line1, sizeof(line1), "FS3: Snapshot 2");
      snprintf(line2, sizeof(line2), "PC 2 on CH %u", MODEP_SNAPSHOT_CH);
      snprintf(line3, sizeof(line3), "");
      break;

    case 3: // FS4: Snapshot 3
      sendProgramChange(MODEP_SNAPSHOT_CH, 3);
      snprintf(line1, sizeof(line1), "FS4: Snapshot 3");
      snprintf(line2, sizeof(line2), "PC 3 on CH %u", MODEP_SNAPSHOT_CH);
      snprintf(line3, sizeof(line3), "");
      break;

    case 4: // FS5: Snapshot 4
      sendProgramChange(MODEP_SNAPSHOT_CH, 4);
      snprintf(line1, sizeof(line1), "FS5: Snapshot 4");
      snprintf(line2, sizeof(line2), "PC 4 on CH %u", MODEP_SNAPSHOT_CH);
      snprintf(line3, sizeof(line3), "");
      break;

    case 5: // FS6: Pedalboard previous
      currentPedalboard = (currentPedalboard == 0) ? TOTAL_PEDALBOARDS - 1 : currentPedalboard - 1;
      sendProgramChange(MODEP_PEDALBOARD_CH, currentPedalboard);
      snprintf(line1, sizeof(line1), "FS6: PB Previous");
      snprintf(line2, sizeof(line2), "PC %u on CH %u", currentPedalboard, MODEP_PEDALBOARD_CH);
      snprintf(line3, sizeof(line3), "");
      break;

    case 6: // FS7: Delay toggle
      modepDelay = !modepDelay;
      sendCC(MODEP_CC_CH, MODEP_CC_DELAY, modepDelay ? 127 : 0);
      snprintf(line1, sizeof(line1), "FS7: Delay");
      snprintf(line2, sizeof(line2), "CC %u = %u", MODEP_CC_DELAY, modepDelay ? 127 : 0);
      snprintf(line3, sizeof(line3), "CH %u", MODEP_CC_CH);
      break;

    case 7: // FS8: Reverb toggle
      modepReverb = !modepReverb;
      sendCC(MODEP_CC_CH, MODEP_CC_REVERB, modepReverb ? 127 : 0);
      snprintf(line1, sizeof(line1), "FS8: Reverb");
      snprintf(line2, sizeof(line2), "CC %u = %u", MODEP_CC_REVERB, modepReverb ? 127 : 0);
      snprintf(line3, sizeof(line3), "CH %u", MODEP_CC_CH);
      break;

    case 8: // FS9: profile previous
      prevProfile();
      return;

    case 9: // FS10: profile next
      nextProfile();
      return;

    default:
      return;
  }

  drawStatus(line1, line2, line3);
}

void handleNeuralDSP(uint8_t fsIndex) {
  if (fsIndex == 8) {
    prevProfile();
    return;
  }

  if (fsIndex == 9) {
    nextProfile();
    return;
  }

  if (fsIndex >= 8) return;

  uint8_t note = NEURAL_NOTES[fsIndex];
  sendNotePulse(NEURAL_DSP_CH, note);

  char line1[32];
  char line2[32];
  char line3[32];

  snprintf(line1, sizeof(line1), "FS%u: Note Pulse", fsIndex + 1);
  snprintf(line2, sizeof(line2), "Note %u", note);
  snprintf(line3, sizeof(line3), "CH %u", NEURAL_DSP_CH);

  drawStatus(line1, line2, line3);
}

void handleGeneric(uint8_t fsIndex) {
  if (fsIndex == 8) {
    prevProfile();
    return;
  }

  if (fsIndex == 9) {
    nextProfile();
    return;
  }

  if (fsIndex >= 8) return;

  genericToggle[fsIndex] = !genericToggle[fsIndex];

  uint8_t cc = GENERIC_CCS[fsIndex];
  uint8_t value = genericToggle[fsIndex] ? 127 : 0;

  sendCC(GENERIC_CH, cc, value);

  char line1[32];
  char line2[32];
  char line3[32];

  snprintf(line1, sizeof(line1), "FS%u: Generic CC", fsIndex + 1);
  snprintf(line2, sizeof(line2), "CC %u = %u", cc, value);
  snprintf(line3, sizeof(line3), "CH %u", GENERIC_CH);

  drawStatus(line1, line2, line3);
}

void handleFootswitchPress(uint8_t fsIndex) {
  switch (activeProfile) {
    case PROFILE_MODEP:
      handleMODEP(fsIndex);
      break;

    case PROFILE_NEURAL_DSP:
      handleNeuralDSP(fsIndex);
      break;

    case PROFILE_GENERIC:
      handleGeneric(fsIndex);
      break;
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  // Hardware serial through right-side CH343P port.
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-S3 10-switch MIDI controller starting...");

  // Start I2C OLED.
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found at 0x3C. Check wiring/address.");
  } else {
    display.clearDisplay();
    display.display();
    drawStatus("Booting...", "USB-MIDI starting", "");
  }

  // Init switches.
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    pinMode(FS_PINS[i], INPUT_PULLUP);
    bool initial = digitalRead(FS_PINS[i]);

    lastRaw[i] = initial;
    stableState[i] = initial;
    lastChangeTime[i] = millis();
  }

  // Start USB-MIDI.
  MIDI.begin();
  USB.begin();

  Serial.println("USB-MIDI started.");
  Serial.println("Connect native USB-C OTG port to Linux and run: aconnect -l / aseqdump");

  drawStatus("Ready", "FS9/FS10 profile", "MODEP default");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  uint32_t now = millis();

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    bool raw = digitalRead(FS_PINS[i]);

    if (raw != lastRaw[i]) {
      lastRaw[i] = raw;
      lastChangeTime[i] = now;
    }

    if ((now - lastChangeTime[i]) >= DEBOUNCE_MS) {
      if (raw != stableState[i]) {
        stableState[i] = raw;

        // Press event only.
        if (raw == LOW) {
          Serial.print("FS");
          Serial.print(i + 1);
          Serial.print(" pressed in profile ");
          Serial.println(profileName(activeProfile));

          handleFootswitchPress(i);
        }
      }
    }
  }

  delay(1);
}