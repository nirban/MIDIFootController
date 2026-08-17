
/*
  ESP32-S3 USB-MIDI Foot Controller + On-demand Web Config + ESP-NOW
  + 10 LEDs + Short/Long Press

  Hardware:
    - ESP32-S3-N16R8 with native USB OTG
    - 10 footswitches: GPIO -> normally-open switch -> GND, INPUT_PULLUP
    - 10 individual LEDs: GPIO -> resistor -> LED anode; LED cathode -> GND
    - SH1106 1.3-inch I2C OLED: SDA=GPIO17, SCL=GPIO18
    - Expression pedal intentionally disabled/not connected in this version
    - FS2 + FS7 held together: toggle Wi-Fi web configuration AP
    - FS3 + FS8 held together: toggle ESP-NOW MIDI transmitter
    - Web configuration and ESP-NOW are mutually exclusive to reduce power

  USB-C usage:
    - Right USB-C / CH343P: upload + Serial Monitor
    - Left USB-C / native USB OTG: class-compliant USB-MIDI

  IMPORTANT:
    - Verify LED_PINS[] against your actual wiring.
    - Use one resistor per LED (typically 470 ohm to 1 kohm).
    - GPIO35, GPIO36 and GPIO37 are not used because the N16R8 module uses
      octal PSRAM/flash connections.
    - GPIO19/GPIO20 remain free for native USB.

  Arduino IDE:
    Board: ESP32S3 Dev Module
    USB Mode: USB-OTG / TinyUSB
    USB CDC On Boot: Disabled
    Flash Size: 16MB
    PSRAM: OPI PSRAM

  Library:
    - U8g2
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "USB.h"
#include "USBMIDI.h"

// ============================================================
// USB MIDI
// ============================================================

USBMIDI MIDI("ESP32-S3 MIDI FootCtrl");

// ============================================================
// OLED
// ============================================================

#define OLED_SDA          17
#define OLED_SCL          18
#define OLED_ADDR_7BIT_A  0x3C
#define OLED_ADDR_7BIT_B  0x3D
#define OLED_ADDR_8BIT_A  0x78
#define OLED_ADDR_8BIT_B  0x7A

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(
  U8G2_R0,
  U8X8_PIN_NONE,
  OLED_SCL,
  OLED_SDA
);

bool oledOK = false;
uint8_t oledAddr8 = OLED_ADDR_8BIT_A;

// ============================================================
// WEB CONFIGURATION ACCESS POINT
// ============================================================

WebServer server(80);
Preferences prefs;

const char *AP_SSID = "ESP32-MIDI-CONFIG";
const char *AP_PASS = "esp32midi";

// Wi-Fi is OFF at boot. The two wireless modes are runtime-only and are not
// saved, so every power-up starts in the lowest-power USB-MIDI mode.
static const uint8_t ESPNOW_CHANNEL = 6;
static const uint32_t MODE_COMBO_HOLD_MS = 450;

// Broadcast keeps the first ESP-NOW test plug-and-play. The receiver listens
// on the same fixed channel. Later this can be changed to the receiver's MAC.
static const uint8_t ESPNOW_BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const uint32_t ESPNOW_PACKET_MAGIC = 0x4D494449;  // "MIDI"
static const uint8_t ESPNOW_PACKET_VERSION = 1;

enum WirelessMidiType : uint8_t {
  WIRELESS_CC = 1,
  WIRELESS_NOTE_ON = 2,
  WIRELESS_NOTE_OFF = 3,
  WIRELESS_PROGRAM_CHANGE = 4
};

struct __attribute__((packed)) EspNowMidiPacket {
  uint32_t magic;
  uint16_t sequence;
  uint8_t version;
  uint8_t type;
  uint8_t channel;  // User-facing MIDI channel 1..16
  uint8_t data1;
  uint8_t data2;
  uint8_t profile;
};

bool webConfigEnabled = false;
bool webRoutesRegistered = false;
bool espNowEnabled = false;
uint16_t espNowSequence = 0;

// ============================================================
// FOOTSWITCHES
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

// Direct-GPIO LED mapping.
// This safe default avoids native USB, OLED, strapping pins and N16R8
// flash/PSRAM pins. Change only this array if your physical wiring differs.
static const uint8_t LED_PINS[NUM_SWITCHES] = {
  42,  // LED for FS1
  41,  // LED for FS2
  40,  // LED for FS3
  39,  // LED for FS4
  38,  // LED for FS5
  21,  // LED for FS6
  16,  // LED for FS7
  15,  // LED for FS8
  14,  // LED for FS9
  47   // LED for FS10
};

static const bool LED_ACTIVE_HIGH = true;
static const uint32_t DEBOUNCE_MS = 35;

// Debounced input state.
bool lastRaw[NUM_SWITCHES];
bool stableState[NUM_SWITCHES];
uint32_t lastChangeTime[NUM_SWITCHES];

// Press/long-press state.
uint32_t pressStartedAt[NUM_SWITCHES];
bool longActionFired[NUM_SWITCHES];
bool shortActionStartedOnPress[NUM_SWITCHES];

// Global system-mode combinations. These combinations work in every profile.
// Their normal switch actions are suppressed only when the combination fires.
bool comboSuppressed[NUM_SWITCHES];

struct ModeComboState {
  bool tracking;
  bool fired;
  uint32_t startedAt;
};

ModeComboState webModeCombo = {false, false, 0};
ModeComboState espNowModeCombo = {false, false, 0};

// ============================================================
// DEFAULT MIDI VALUES
// ============================================================

static const uint8_t MODEP_PEDALBOARD_CH = 15;
static const uint8_t MODEP_SNAPSHOT_CH   = 14;
static const uint8_t MODEP_CC_CH         = 14;

static const uint8_t MODEP_CC_TUNER      = 20;
static const uint8_t MODEP_CC_DELAY      = 21;
static const uint8_t MODEP_CC_REVERB     = 22;
static const uint8_t MODEP_CC_BOOST      = 23;

static const uint8_t NEURAL_DSP_CH       = 1;
static const uint8_t GENERIC_CH          = 1;

// ============================================================
// CONFIGURATION MODEL
// ============================================================

// Changed from MID1 to MID2 because the stored structure now includes
// short action, long action and LED configuration.
static const uint32_t CFG_MAGIC = 0x4D494432;  // "MID2"

static const uint8_t NUM_PROFILES    = 3;
static const uint8_t PROFILE_MODEP   = 0;
static const uint8_t PROFILE_NEURAL  = 1;
static const uint8_t PROFILE_GENERIC = 2;

enum MidiAction : uint8_t {
  ACT_NONE = 0,
  ACT_CC_TOGGLE = 1,
  ACT_CC_MOMENTARY = 2,
  ACT_NOTE_PULSE = 3,
  ACT_NOTE_MOMENTARY = 4,
  ACT_PROGRAM_CHANGE = 5,
  ACT_BANK_UP = 6,
  ACT_BANK_DOWN = 7,
  ACT_PROFILE_NEXT = 8,
  ACT_PROFILE_PREV = 9
};

enum LedMode : uint8_t {
  LED_MODE_OFF = 0,
  LED_MODE_ALWAYS_ON = 1,
  LED_MODE_MOMENTARY = 2,
  LED_MODE_TOGGLE = 3,
  LED_MODE_EXCLUSIVE = 4,
  LED_MODE_FOLLOW_CC_TOGGLE = 5
};

enum LedTrigger : uint8_t {
  LED_TRIGGER_SHORT = 0,
  LED_TRIGGER_LONG = 1,
  LED_TRIGGER_BOTH = 2
};

struct ActionConfig {
  uint8_t action;
  uint8_t channel;   // User-facing MIDI channel 1..16
  uint8_t data1;     // CC / Note / Program
  uint8_t data2;     // ON value / velocity
  uint8_t offValue;  // OFF value
};

struct LedConfig {
  uint8_t mode;
  uint8_t trigger;
  uint8_t group;       // 1..8 for exclusive mode; 0 means no group
  uint8_t blinkCount;  // Number of complete off/on flashes after action
  uint16_t blinkMs;    // Duration of each off/on half-cycle
};

struct ButtonConfig {
  char label[20];
  ActionConfig shortAction;
  ActionConfig longAction;
  uint16_t longPressMs;
  LedConfig led;
};

struct ProfileConfig {
  char name[20];
  uint8_t defaultChannel;
  ButtonConfig buttons[NUM_SWITCHES];
};

struct AppConfig {
  uint32_t magic;
  uint8_t activeProfile;
  uint8_t totalPedalboards;
  uint8_t reserved[2];
  ProfileConfig profiles[NUM_PROFILES];
};

AppConfig cfg;

// Runtime action toggle states: [profile][switch][0=short,1=long].
bool actionToggleState[NUM_PROFILES][NUM_SWITCHES][2];

// Runtime LED latching states per profile.
bool ledLatchedState[NUM_PROFILES][NUM_SWITCHES];

uint8_t currentPedalboard = 0;

// ============================================================
// NON-BLOCKING LED BLINK ENGINE
// ============================================================

struct LedBlinkState {
  bool active;
  bool outputLevel;
  uint8_t edgesRemaining;
  uint16_t intervalMs;
  uint32_t nextAt;
};

LedBlinkState ledBlink[NUM_SWITCHES];

// ============================================================
// UTILITIES
// ============================================================

void safeCopy(char *dst, const String &src, size_t maxLen) {
  if (maxLen == 0) return;
  strncpy(dst, src.c_str(), maxLen - 1);
  dst[maxLen - 1] = '\0';
}

uint8_t clampU8(int value, int lo, int hi) {
  if (value < lo) return (uint8_t)lo;
  if (value > hi) return (uint8_t)hi;
  return (uint8_t)value;
}

uint16_t clampU16(int value, int lo, int hi) {
  if (value < lo) return (uint16_t)lo;
  if (value > hi) return (uint16_t)hi;
  return (uint16_t)value;
}

const char *actionName(uint8_t action) {
  switch (action) {
    case ACT_NONE:           return "None";
    case ACT_CC_TOGGLE:      return "CC Toggle";
    case ACT_CC_MOMENTARY:   return "CC Momentary";
    case ACT_NOTE_PULSE:     return "Note Pulse";
    case ACT_NOTE_MOMENTARY: return "Note Momentary";
    case ACT_PROGRAM_CHANGE: return "Program Change";
    case ACT_BANK_UP:        return "Bank/PB Up";
    case ACT_BANK_DOWN:      return "Bank/PB Down";
    case ACT_PROFILE_NEXT:   return "Profile Next";
    case ACT_PROFILE_PREV:   return "Profile Prev";
    default:                 return "Unknown";
  }
}

const char *ledModeName(uint8_t mode) {
  switch (mode) {
    case LED_MODE_OFF:              return "Off";
    case LED_MODE_ALWAYS_ON:        return "Always ON";
    case LED_MODE_MOMENTARY:        return "While held";
    case LED_MODE_TOGGLE:           return "Toggle";
    case LED_MODE_EXCLUSIVE:        return "Exclusive group";
    case LED_MODE_FOLLOW_CC_TOGGLE: return "Follow CC toggle";
    default:                        return "Unknown";
  }
}

String htmlEscape(const char *s) {
  String out;
  while (*s) {
    char c = *s++;
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

// ============================================================
// OLED
// ============================================================

bool i2cDevicePresent(uint8_t addr7) {
  Wire.beginTransmission(addr7);
  return Wire.endTransmission() == 0;
}

void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);

  if (i2cDevicePresent(OLED_ADDR_7BIT_A)) {
    oledAddr8 = OLED_ADDR_8BIT_A;
  } else if (i2cDevicePresent(OLED_ADDR_7BIT_B)) {
    oledAddr8 = OLED_ADDR_8BIT_B;
  } else {
    oledOK = false;
    Serial.println("OLED not found at 0x3C or 0x3D. Continuing without display.");
    return;
  }

  display.setI2CAddress(oledAddr8);
  display.begin();
  display.setFont(u8g2_font_6x12_tr);
  oledOK = true;

  Serial.print("SH1106 OLED found. U8g2 address = 0x");
  Serial.println(oledAddr8, HEX);
}

void oledStatus(const String &line1, const String &line2 = "", const String &line3 = "") {
  if (!oledOK) return;

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);

  display.setCursor(0, 10);
  display.print("ESP32-S3 USB MIDI");
  display.drawHLine(0, 12, 128);

  display.setCursor(0, 24);
  display.print("Profile: ");
  display.print(cfg.profiles[cfg.activeProfile].name);

  display.setCursor(0, 38);
  display.print(line1);

  display.setCursor(0, 50);
  display.print(line2);

  display.setCursor(0, 62);
  display.print(line3);

  display.sendBuffer();
}

// ============================================================
// LED CONTROL
// ============================================================

void writeLedHardware(uint8_t index, bool on) {
  if (index >= NUM_SWITCHES) return;
  bool level = LED_ACTIVE_HIGH ? on : !on;
  digitalWrite(LED_PINS[index], level ? HIGH : LOW);
}

bool ledBaseStateFor(uint8_t profileIndex, uint8_t switchIndex) {
  const LedConfig &lc = cfg.profiles[profileIndex].buttons[switchIndex].led;

  switch (lc.mode) {
    case LED_MODE_OFF:
      return false;

    case LED_MODE_ALWAYS_ON:
      return true;

    case LED_MODE_MOMENTARY:
      // Stable switch state is LOW while pressed.
      return stableState[switchIndex] == LOW;

    case LED_MODE_TOGGLE:
    case LED_MODE_EXCLUSIVE:
    case LED_MODE_FOLLOW_CC_TOGGLE:
      return ledLatchedState[profileIndex][switchIndex];

    default:
      return false;
  }
}

void renderLed(uint8_t index) {
  if (index >= NUM_SWITCHES) return;

  if (ledBlink[index].active) {
    writeLedHardware(index, ledBlink[index].outputLevel);
  } else {
    writeLedHardware(index, ledBaseStateFor(cfg.activeProfile, index));
  }
}

void refreshAllLeds() {
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    renderLed(i);
  }
}

void clearExclusiveGroup(uint8_t profileIndex, uint8_t group, uint8_t exceptIndex) {
  if (group == 0) return;

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    const LedConfig &other = cfg.profiles[profileIndex].buttons[i].led;
    if (i != exceptIndex &&
        other.mode == LED_MODE_EXCLUSIVE &&
        other.group == group) {
      ledLatchedState[profileIndex][i] = false;
      if (profileIndex == cfg.activeProfile) renderLed(i);
    }
  }
}

bool ledTriggerMatches(const LedConfig &lc, bool isLong) {
  if (lc.trigger == LED_TRIGGER_BOTH) return true;
  if (isLong) return lc.trigger == LED_TRIGGER_LONG;
  return lc.trigger == LED_TRIGGER_SHORT;
}

void startLedBlink(uint8_t index, uint8_t completeFlashes, uint16_t intervalMs) {
  if (index >= NUM_SWITCHES || completeFlashes == 0) return;

  if (intervalMs < 40) intervalMs = 40;

  LedBlinkState &bs = ledBlink[index];
  bs.active = true;

  // Start opposite to the resting/base state so an always-on LED visibly blinks off.
  bs.outputLevel = !ledBaseStateFor(cfg.activeProfile, index);
  bs.edgesRemaining = (uint8_t)(completeFlashes * 2);
  bs.intervalMs = intervalMs;
  bs.nextAt = millis() + intervalMs;

  renderLed(index);
}

void serviceLedBlinks(uint32_t now) {
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    LedBlinkState &bs = ledBlink[i];
    if (!bs.active) continue;

    if ((int32_t)(now - bs.nextAt) >= 0) {
      if (bs.edgesRemaining > 0) {
        bs.outputLevel = !bs.outputLevel;
        bs.edgesRemaining--;
        bs.nextAt = now + bs.intervalMs;
      }

      if (bs.edgesRemaining == 0) {
        bs.active = false;
      }

      renderLed(i);
    }
  }
}

void updateLedAfterAction(
  uint8_t fsIndex,
  bool isLong,
  const ActionConfig &action,
  bool resultingToggleState
) {
  LedConfig &lc = cfg.profiles[cfg.activeProfile].buttons[fsIndex].led;

  if (!ledTriggerMatches(lc, isLong)) return;

  switch (lc.mode) {
    case LED_MODE_TOGGLE:
      ledLatchedState[cfg.activeProfile][fsIndex] =
        !ledLatchedState[cfg.activeProfile][fsIndex];
      break;

    case LED_MODE_EXCLUSIVE:
      clearExclusiveGroup(cfg.activeProfile, lc.group, fsIndex);
      ledLatchedState[cfg.activeProfile][fsIndex] = true;
      break;

    case LED_MODE_FOLLOW_CC_TOGGLE:
      if (action.action == ACT_CC_TOGGLE) {
        ledLatchedState[cfg.activeProfile][fsIndex] = resultingToggleState;
      } else {
        ledLatchedState[cfg.activeProfile][fsIndex] =
          !ledLatchedState[cfg.activeProfile][fsIndex];
      }
      break;

    case LED_MODE_OFF:
    case LED_MODE_ALWAYS_ON:
    case LED_MODE_MOMENTARY:
    default:
      break;
  }

  renderLed(fsIndex);

  if (lc.blinkCount > 0) {
    startLedBlink(fsIndex, lc.blinkCount, lc.blinkMs);
  }
}

// ============================================================
// CONFIGURATION DEFAULTS
// ============================================================

void setAction(
  ActionConfig &action,
  uint8_t type,
  uint8_t channel,
  uint8_t data1,
  uint8_t data2 = 127,
  uint8_t offValue = 0
) {
  action.action = type;
  action.channel = channel;
  action.data1 = data1;
  action.data2 = data2;
  action.offValue = offValue;
}

void setLed(
  LedConfig &led,
  uint8_t mode,
  uint8_t trigger = LED_TRIGGER_SHORT,
  uint8_t group = 0,
  uint8_t blinkCount = 0,
  uint16_t blinkMs = 120
) {
  led.mode = mode;
  led.trigger = trigger;
  led.group = group;
  led.blinkCount = blinkCount;
  led.blinkMs = blinkMs;
}

void setButton(
  ButtonConfig &button,
  const char *label,
  uint8_t shortType,
  uint8_t shortChannel,
  uint8_t shortData1,
  uint8_t shortData2 = 127,
  uint8_t shortOff = 0
) {
  strncpy(button.label, label, sizeof(button.label) - 1);
  button.label[sizeof(button.label) - 1] = '\0';

  setAction(
    button.shortAction,
    shortType,
    shortChannel,
    shortData1,
    shortData2,
    shortOff
  );

  setAction(button.longAction, ACT_NONE, shortChannel, 0, 127, 0);
  button.longPressMs = 650;
  setLed(button.led, LED_MODE_OFF);
}

void loadDefaultConfig() {
  memset(&cfg, 0, sizeof(cfg));
  memset(actionToggleState, 0, sizeof(actionToggleState));
  memset(ledLatchedState, 0, sizeof(ledLatchedState));
  memset(ledBlink, 0, sizeof(ledBlink));

  cfg.magic = CFG_MAGIC;
  cfg.activeProfile = PROFILE_MODEP;
  cfg.totalPedalboards = 16;

  // ---------------- MODEP ----------------
  safeCopy(
    cfg.profiles[PROFILE_MODEP].name,
    String("MODEP"),
    sizeof(cfg.profiles[PROFILE_MODEP].name)
  );
  cfg.profiles[PROFILE_MODEP].defaultChannel = MODEP_SNAPSHOT_CH;

  setButton(cfg.profiles[PROFILE_MODEP].buttons[0], "PB Up",
            ACT_BANK_UP, MODEP_PEDALBOARD_CH, 0);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[1], "Snapshot 1",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 1);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[2], "Snapshot 2",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 2);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[3], "Snapshot 3",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 3);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[4], "Snapshot 4",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 4);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[5], "PB Down",
            ACT_BANK_DOWN, MODEP_PEDALBOARD_CH, 0);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[6], "Snapshot 5",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 5);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[7], "Snapshot 6",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 6);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[8], "Snapshot 7",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 7);
  setButton(cfg.profiles[PROFILE_MODEP].buttons[9], "Snapshot 8",
            ACT_PROGRAM_CHANGE, MODEP_SNAPSHOT_CH, 8);

  // FS1 and FS6 LEDs are always on and blink when pedalboard changes.
  setLed(cfg.profiles[PROFILE_MODEP].buttons[0].led,
         LED_MODE_ALWAYS_ON, LED_TRIGGER_SHORT, 0, 2, 110);
  setLed(cfg.profiles[PROFILE_MODEP].buttons[5].led,
         LED_MODE_ALWAYS_ON, LED_TRIGGER_SHORT, 0, 2, 110);

  // Snapshot LEDs are mutually exclusive: only the selected snapshot stays on.
  const uint8_t SNAPSHOT_LED_GROUP = 1;
  const uint8_t snapshotIndices[] = {1, 2, 3, 4, 6, 7, 8, 9};
  for (uint8_t index : snapshotIndices) {
    setLed(cfg.profiles[PROFILE_MODEP].buttons[index].led,
           LED_MODE_EXCLUSIVE, LED_TRIGGER_SHORT, SNAPSHOT_LED_GROUP, 0, 100);
  }

  // Long-press defaults copied from the earlier MODEP controller behaviour.
  setAction(cfg.profiles[PROFILE_MODEP].buttons[1].longAction,
            ACT_CC_TOGGLE, MODEP_CC_CH, MODEP_CC_TUNER, 127, 0);
  setAction(cfg.profiles[PROFILE_MODEP].buttons[2].longAction,
            ACT_CC_TOGGLE, MODEP_CC_CH, MODEP_CC_DELAY, 127, 0);
  setAction(cfg.profiles[PROFILE_MODEP].buttons[3].longAction,
            ACT_CC_TOGGLE, MODEP_CC_CH, MODEP_CC_REVERB, 127, 0);
  setAction(cfg.profiles[PROFILE_MODEP].buttons[4].longAction,
            ACT_CC_TOGGLE, MODEP_CC_CH, MODEP_CC_BOOST, 127, 0);

  // ---------------- NEURAL DSP ----------------
  safeCopy(
    cfg.profiles[PROFILE_NEURAL].name,
    String("NEURAL DSP"),
    sizeof(cfg.profiles[PROFILE_NEURAL].name)
  );
  cfg.profiles[PROFILE_NEURAL].defaultChannel = NEURAL_DSP_CH;

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    char label[20];
    snprintf(label, sizeof(label), "Note %u", 36 + i);
    setButton(
      cfg.profiles[PROFILE_NEURAL].buttons[i],
      label,
      ACT_NOTE_PULSE,
      NEURAL_DSP_CH,
      36 + i,
      127,
      0
    );
    setLed(
      cfg.profiles[PROFILE_NEURAL].buttons[i].led,
      LED_MODE_TOGGLE,
      LED_TRIGGER_SHORT,
      0,
      0,
      100
    );
  }

  // ---------------- GENERIC ----------------
  safeCopy(
    cfg.profiles[PROFILE_GENERIC].name,
    String("GENERIC"),
    sizeof(cfg.profiles[PROFILE_GENERIC].name)
  );
  cfg.profiles[PROFILE_GENERIC].defaultChannel = GENERIC_CH;

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    char label[20];
    snprintf(label, sizeof(label), "CC %u", 20 + i);
    setButton(
      cfg.profiles[PROFILE_GENERIC].buttons[i],
      label,
      ACT_CC_TOGGLE,
      GENERIC_CH,
      20 + i,
      127,
      0
    );
    setLed(
      cfg.profiles[PROFILE_GENERIC].buttons[i].led,
      LED_MODE_FOLLOW_CC_TOGGLE,
      LED_TRIGGER_SHORT,
      0,
      0,
      100
    );
  }

  currentPedalboard = 0;
}

void saveConfig() {
  prefs.begin("midicfg", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}

bool validateConfig() {
  if (cfg.magic != CFG_MAGIC) return false;
  if (cfg.activeProfile >= NUM_PROFILES) return false;
  if (cfg.totalPedalboards == 0 || cfg.totalPedalboards > 127) return false;

  for (uint8_t p = 0; p < NUM_PROFILES; p++) {
    for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
      const ButtonConfig &b = cfg.profiles[p].buttons[i];
      if (b.shortAction.action > ACT_PROFILE_PREV) return false;
      if (b.longAction.action > ACT_PROFILE_PREV) return false;
      if (b.shortAction.channel < 1 || b.shortAction.channel > 16) return false;
      if (b.longAction.channel < 1 || b.longAction.channel > 16) return false;
      if (b.led.mode > LED_MODE_FOLLOW_CC_TOGGLE) return false;
      if (b.led.trigger > LED_TRIGGER_BOTH) return false;
    }
  }

  return true;
}

bool loadConfig() {
  prefs.begin("midicfg", true);
  size_t length = prefs.getBytesLength("cfg");

  if (length != sizeof(cfg)) {
    prefs.end();
    return false;
  }

  prefs.getBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();

  return validateConfig();
}

// ============================================================
// ON-DEMAND WI-FI WEB CONFIGURATION AND ESP-NOW
// ============================================================

String wirelessStatusText() {
  if (webConfigEnabled) return "WEB CONFIG ON";
  if (espNowEnabled) return "ESP-NOW ON CH " + String(ESPNOW_CHANNEL);
  return "WiFi/ESP-NOW OFF";
}

void registerWebRoutesOnce();
void startWebConfig();
void stopWebConfig();
void stopEspNow();

bool sendEspNowMidi(
  uint8_t type,
  uint8_t channel,
  uint8_t data1,
  uint8_t data2
) {
  if (!espNowEnabled) return false;

  EspNowMidiPacket packet{};
  packet.magic = ESPNOW_PACKET_MAGIC;
  packet.sequence = ++espNowSequence;
  packet.version = ESPNOW_PACKET_VERSION;
  packet.type = type;
  packet.channel = channel;
  packet.data1 = data1;
  packet.data2 = data2;
  packet.profile = cfg.activeProfile;

  esp_err_t result = esp_now_send(
    ESPNOW_BROADCAST_MAC,
    reinterpret_cast<const uint8_t *>(&packet),
    sizeof(packet)
  );

  if (result != ESP_OK) {
    Serial.printf("ESP-NOW send error: %d\n", (int)result);
    return false;
  }

  return true;
}

bool startEspNow() {
  if (espNowEnabled) return true;

  // Keep only one radio mode active to reduce current and simplify channel use.
  if (webConfigEnabled) stopWebConfig();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  WiFi.setSleep(false);  // Lower latency while ESP-NOW is intentionally active.
  delay(30);

  esp_err_t channelResult = esp_wifi_set_channel(
    ESPNOW_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );

  if (channelResult != ESP_OK) {
    Serial.printf("Could not set ESP-NOW channel: %d\n", (int)channelResult);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  esp_err_t initResult = esp_now_init();
  if (initResult != ESP_OK) {
    Serial.printf("ESP-NOW init failed: %d\n", (int)initResult);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, ESPNOW_BROADCAST_MAC, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;

  if (!esp_now_is_peer_exist(ESPNOW_BROADCAST_MAC)) {
    esp_err_t peerResult = esp_now_add_peer(&peer);
    if (peerResult != ESP_OK) {
      Serial.printf("ESP-NOW peer add failed: %d\n", (int)peerResult);
      esp_now_deinit();
      WiFi.mode(WIFI_OFF);
      return false;
    }
  }

  espNowEnabled = true;
  Serial.printf(
    "ESP-NOW transmitter ON, channel %u, sender MAC %s\n",
    ESPNOW_CHANNEL,
    WiFi.macAddress().c_str()
  );

  startLedBlink(2, 2, 90);  // FS3 LED
  startLedBlink(7, 2, 90);  // FS8 LED
  oledStatus("ESP-NOW enabled", "Channel " + String(ESPNOW_CHANNEL), "USB also active");
  return true;
}

void stopEspNow() {
  if (!espNowEnabled) return;

  esp_now_deinit();
  espNowEnabled = false;

  if (!webConfigEnabled) {
    WiFi.setSleep(true);
    WiFi.mode(WIFI_OFF);
  }

  Serial.println("ESP-NOW transmitter OFF");
  startLedBlink(2, 1, 120);
  startLedBlink(7, 1, 120);
  oledStatus("ESP-NOW disabled", cfg.profiles[cfg.activeProfile].name, "USB-MIDI active");
}

void toggleEspNowMode() {
  if (espNowEnabled) {
    stopEspNow();
  } else if (!startEspNow()) {
    oledStatus("ESP-NOW failed", "Check Serial Monitor", "");
  }
}

bool isSystemComboMember(uint8_t fsIndex) {
  return fsIndex == 1 || fsIndex == 6 ||  // FS2 + FS7
         fsIndex == 2 || fsIndex == 7;    // FS3 + FS8
}

void suppressComboSwitches(uint8_t first, uint8_t second) {
  comboSuppressed[first] = true;
  comboSuppressed[second] = true;
  longActionFired[first] = true;
  longActionFired[second] = true;
  shortActionStartedOnPress[first] = false;
  shortActionStartedOnPress[second] = false;
}

void serviceOneModeCombo(
  ModeComboState &state,
  uint8_t first,
  uint8_t second,
  uint32_t now,
  bool webCombo
) {
  bool bothPressed = stableState[first] == LOW &&
                     stableState[second] == LOW;

  if (bothPressed) {
    if (!state.tracking) {
      state.tracking = true;
      state.fired = false;
      state.startedAt = now;
    } else if (!state.fired &&
               (uint32_t)(now - state.startedAt) >= MODE_COMBO_HOLD_MS) {
      state.fired = true;
      suppressComboSwitches(first, second);

      if (webCombo) {
        if (webConfigEnabled) {
          stopWebConfig();
        } else {
          // Web configuration and ESP-NOW are intentionally exclusive.
          if (espNowEnabled) stopEspNow();
          startWebConfig();
        }
      } else {
        toggleEspNowMode();
      }
    }
  } else if (stableState[first] == HIGH && stableState[second] == HIGH) {
    state.tracking = false;
    state.fired = false;
    state.startedAt = 0;
  }
}

void serviceModeCombos(uint32_t now) {
  serviceOneModeCombo(webModeCombo, 1, 6, now, true);   // FS2 + FS7
  serviceOneModeCombo(espNowModeCombo, 2, 7, now, false); // FS3 + FS8
}

// ============================================================
// MIDI SENDERS
// ============================================================

void sendCC(uint8_t channel, uint8_t cc, uint8_t value) {
  MIDI.controlChange(cc, value, channel);
  sendEspNowMidi(WIRELESS_CC, channel, cc, value);
  Serial.printf("MIDI CC ch=%u cc=%u value=%u\n", channel, cc, value);
}

void sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  MIDI.noteOn(note, velocity, channel);
  sendEspNowMidi(WIRELESS_NOTE_ON, channel, note, velocity);
  Serial.printf(
    "MIDI NoteOn ch=%u note=%u velocity=%u\n",
    channel,
    note,
    velocity
  );
}

void sendNoteOff(uint8_t channel, uint8_t note) {
  MIDI.noteOff(note, 0, channel);
  sendEspNowMidi(WIRELESS_NOTE_OFF, channel, note, 0);
  Serial.printf("MIDI NoteOff ch=%u note=%u\n", channel, note);
}

void sendPC(uint8_t channel, uint8_t program) {
  MIDI.programChange(program, channel);
  sendEspNowMidi(WIRELESS_PROGRAM_CHANGE, channel, program, 0);
  Serial.printf("MIDI PC ch=%u program=%u\n", channel, program);
}

// ============================================================
// PROFILE AND PEDALBOARD OPERATIONS
// ============================================================

void changeProfile(int delta) {
  int nextProfile = (int)cfg.activeProfile + delta;

  if (nextProfile < 0) nextProfile = NUM_PROFILES - 1;
  if (nextProfile >= NUM_PROFILES) nextProfile = 0;

  cfg.activeProfile = (uint8_t)nextProfile;
  saveConfig();

  memset(ledBlink, 0, sizeof(ledBlink));
  refreshAllLeds();

  oledStatus(
    "Profile changed",
    cfg.profiles[cfg.activeProfile].name,
    "Saved"
  );

  Serial.printf(
    "Active profile: %s\n",
    cfg.profiles[cfg.activeProfile].name
  );
}

void bankMove(int delta, const ActionConfig &action) {
  int total = cfg.totalPedalboards;
  if (total <= 0) total = 1;

  int next = (int)currentPedalboard + delta;
  while (next < 0) next += total;
  next %= total;

  currentPedalboard = (uint8_t)next;
  sendPC(action.channel, currentPedalboard);
}

// ============================================================
// ACTION EXECUTION
// ============================================================

bool isMomentaryAction(const ActionConfig &action) {
  return action.action == ACT_CC_MOMENTARY ||
         action.action == ACT_NOTE_MOMENTARY;
}

bool executeAction(
  uint8_t fsIndex,
  const ActionConfig &action,
  bool isLong,
  bool forcePulseForMomentary = false
) {
  uint8_t toggleSlot = isLong ? 1 : 0;
  bool resultingToggle = actionToggleState[cfg.activeProfile][fsIndex][toggleSlot];

  Serial.printf(
    "FS%u %s action=%s ch=%u d1=%u d2=%u off=%u\n",
    fsIndex + 1,
    isLong ? "LONG" : "SHORT",
    actionName(action.action),
    action.channel,
    action.data1,
    action.data2,
    action.offValue
  );

  switch (action.action) {
    case ACT_NONE:
      return resultingToggle;

    case ACT_CC_TOGGLE:
      resultingToggle = !resultingToggle;
      actionToggleState[cfg.activeProfile][fsIndex][toggleSlot] = resultingToggle;
      sendCC(
        action.channel,
        action.data1,
        resultingToggle ? action.data2 : action.offValue
      );
      break;

    case ACT_CC_MOMENTARY:
      sendCC(action.channel, action.data1, action.data2);
      if (forcePulseForMomentary) {
        delay(12);
        sendCC(action.channel, action.data1, action.offValue);
      }
      break;

    case ACT_NOTE_PULSE:
      sendNoteOn(action.channel, action.data1, action.data2);
      delay(12);
      sendNoteOff(action.channel, action.data1);
      break;

    case ACT_NOTE_MOMENTARY:
      sendNoteOn(action.channel, action.data1, action.data2);
      if (forcePulseForMomentary) {
        delay(12);
        sendNoteOff(action.channel, action.data1);
      }
      break;

    case ACT_PROGRAM_CHANGE:
      sendPC(action.channel, action.data1);
      break;

    case ACT_BANK_UP:
      bankMove(+1, action);
      break;

    case ACT_BANK_DOWN:
      bankMove(-1, action);
      break;

    case ACT_PROFILE_NEXT:
      changeProfile(+1);
      return resultingToggle;

    case ACT_PROFILE_PREV:
      changeProfile(-1);
      return resultingToggle;

    default:
      break;
  }

  updateLedAfterAction(fsIndex, isLong, action, resultingToggle);

  ButtonConfig &button =
    cfg.profiles[cfg.activeProfile].buttons[fsIndex];

  String prefix = isLong ? "LONG FS" : "FS";
  String line1 = prefix + String(fsIndex + 1) + ": " + String(button.label);
  String line2 = String(actionName(action.action)) +
                 " CH " + String(action.channel);
  String line3;

  if (action.action == ACT_BANK_UP || action.action == ACT_BANK_DOWN) {
    line1 = "Pedalboard changed";
    line2 = "PB " + String(currentPedalboard);
    line3 = button.label;
  } else if (action.action == ACT_PROGRAM_CHANGE) {
    line3 = "Program " + String(action.data1);
  } else {
    line3 = "D1 " + String(action.data1) +
            " D2 " + String(action.data2);
  }

  oledStatus(line1, line2, line3);

  return resultingToggle;
}

void finishMomentaryAction(
  const ActionConfig &action
) {
  if (action.action == ACT_CC_MOMENTARY) {
    sendCC(action.channel, action.data1, action.offValue);
  } else if (action.action == ACT_NOTE_MOMENTARY) {
    sendNoteOff(action.channel, action.data1);
  }
}

// ============================================================
// FOOTSWITCH EVENTS
// ============================================================

void onSwitchPressed(uint8_t fsIndex, uint32_t now) {
  ButtonConfig &button =
    cfg.profiles[cfg.activeProfile].buttons[fsIndex];

  pressStartedAt[fsIndex] = now;
  longActionFired[fsIndex] = false;
  shortActionStartedOnPress[fsIndex] = false;

  if (button.led.mode == LED_MODE_MOMENTARY) {
    renderLed(fsIndex);
  }

  // With no long action, run the short action immediately for lowest latency.
  // With a long action configured, defer the short action until release so a
  // long press does not also send the short command.
  if (button.longAction.action == ACT_NONE && !isSystemComboMember(fsIndex)) {
    executeAction(fsIndex, button.shortAction, false, false);
    shortActionStartedOnPress[fsIndex] = true;
  }
}

void onSwitchReleased(uint8_t fsIndex) {
  ButtonConfig &button =
    cfg.profiles[cfg.activeProfile].buttons[fsIndex];

  if (comboSuppressed[fsIndex]) {
    comboSuppressed[fsIndex] = false;
    longActionFired[fsIndex] = false;
    shortActionStartedOnPress[fsIndex] = false;
    if (button.led.mode == LED_MODE_MOMENTARY) renderLed(fsIndex);
    return;
  }

  if (longActionFired[fsIndex]) {
    finishMomentaryAction(button.longAction);
  } else if (button.longAction.action != ACT_NONE || isSystemComboMember(fsIndex)) {
    // The short action was deferred while waiting for a long press or a
    // system-mode combination. For a momentary short action, emit a pulse now.
    executeAction(
      fsIndex,
      button.shortAction,
      false,
      isMomentaryAction(button.shortAction)
    );
  } else if (shortActionStartedOnPress[fsIndex]) {
    finishMomentaryAction(button.shortAction);
  }

  if (button.led.mode == LED_MODE_MOMENTARY) {
    renderLed(fsIndex);
  }
}

void serviceLongPresses(uint32_t now) {
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    if (stableState[i] != LOW) continue;
    if (longActionFired[i]) continue;
    if (comboSuppressed[i]) continue;

    ButtonConfig &button =
      cfg.profiles[cfg.activeProfile].buttons[i];

    if (button.longAction.action == ACT_NONE) continue;

    if ((uint32_t)(now - pressStartedAt[i]) >= button.longPressMs) {
      longActionFired[i] = true;
      executeAction(i, button.longAction, true, false);
    }
  }
}

// ============================================================
// WEB UI HELPERS
// ============================================================

String optionTag(
  uint8_t value,
  uint8_t selected,
  const char *label
) {
  String result = "<option value='" + String(value) + "'";
  if (value == selected) result += " selected";
  result += ">";
  result += label;
  result += "</option>";
  return result;
}

String actionSelect(
  const String &name,
  uint8_t current
) {
  String html = "<select name='" + name + "'>";
  html += optionTag(ACT_NONE, current, "None");
  html += optionTag(ACT_CC_TOGGLE, current, "CC Toggle");
  html += optionTag(ACT_CC_MOMENTARY, current, "CC Momentary");
  html += optionTag(ACT_NOTE_PULSE, current, "Note Pulse");
  html += optionTag(ACT_NOTE_MOMENTARY, current, "Note Momentary");
  html += optionTag(ACT_PROGRAM_CHANGE, current, "Program Change");
  html += optionTag(ACT_BANK_UP, current, "Bank/Pedalboard Up");
  html += optionTag(ACT_BANK_DOWN, current, "Bank/Pedalboard Down");
  html += optionTag(ACT_PROFILE_NEXT, current, "Profile Next");
  html += optionTag(ACT_PROFILE_PREV, current, "Profile Previous");
  html += "</select>";
  return html;
}

String ledModeSelect(
  const String &name,
  uint8_t current
) {
  String html = "<select name='" + name + "'>";
  html += optionTag(LED_MODE_OFF, current, "Off");
  html += optionTag(LED_MODE_ALWAYS_ON, current, "Always ON");
  html += optionTag(LED_MODE_MOMENTARY, current, "While switch held");
  html += optionTag(LED_MODE_TOGGLE, current, "Toggle");
  html += optionTag(LED_MODE_EXCLUSIVE, current, "Exclusive group");
  html += optionTag(
    LED_MODE_FOLLOW_CC_TOGGLE,
    current,
    "Follow CC toggle"
  );
  html += "</select>";
  return html;
}

String ledTriggerSelect(
  const String &name,
  uint8_t current
) {
  String html = "<select name='" + name + "'>";
  html += optionTag(
    LED_TRIGGER_SHORT,
    current,
    "Short action"
  );
  html += optionTag(
    LED_TRIGGER_LONG,
    current,
    "Long action"
  );
  html += optionTag(
    LED_TRIGGER_BOTH,
    current,
    "Both actions"
  );
  html += "</select>";
  return html;
}

String profileLinks() {
  String html = "<div class='profiles'>";

  for (uint8_t p = 0; p < NUM_PROFILES; p++) {
    html += "<a class='btn";
    if (p == cfg.activeProfile) html += " active";
    html += "' href='/profile?p=" + String(p) + "'>";
    html += htmlEscape(cfg.profiles[p].name);
    html += "</a>";
  }

  html += "</div>";
  return html;
}

String numberInput(
  const String &name,
  int value,
  int minimum,
  int maximum,
  const String &extraClass = ""
) {
  return "<input class='" + extraClass +
         "' type='number' name='" + name +
         "' min='" + String(minimum) +
         "' max='" + String(maximum) +
         "' value='" + String(value) + "'>";
}

String actionEditor(
  uint8_t switchIndex,
  const char *prefix,
  const char *heading,
  const ActionConfig &action
) {
  String i = String(switchIndex);
  String p = String(prefix);

  String html;
  html += "<div class='actionbox'><b>" + String(heading) + "</b>";
  html += "<div class='grid'>";
  html += "<label>Action" +
          actionSelect(p + "a" + i, action.action) +
          "</label>";
  html += "<label>Channel" +
          numberInput(p + "ch" + i, action.channel, 1, 16) +
          "</label>";
  html += "<label>Data 1" +
          numberInput(p + "d1" + i, action.data1, 0, 127) +
          "</label>";
  html += "<label>Data 2" +
          numberInput(p + "d2" + i, action.data2, 0, 127) +
          "</label>";
  html += "<label>Off" +
          numberInput(p + "off" + i, action.offValue, 0, 127) +
          "</label>";
  html += "</div></div>";

  return html;
}

// ============================================================
// WEB ROUTES
// ============================================================

void handleRoot() {
  uint8_t profileIndex = cfg.activeProfile;

  if (server.hasArg("p")) {
    profileIndex = clampU8(
      server.arg("p").toInt(),
      0,
      NUM_PROFILES - 1
    );
  }

  ProfileConfig &profile = cfg.profiles[profileIndex];

  String html;
  html.reserve(50000);

  html +=
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 MIDI Foot Controller</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:system-ui,Arial;margin:18px;background:#111;color:#eee}"
    ".card{background:#1d1d1d;padding:14px;border-radius:14px;margin:12px 0;"
    "box-shadow:0 0 0 1px #333}"
    ".fs{border:1px solid #3a3a3a;border-radius:12px;padding:12px;margin:12px 0;"
    "background:#181818}"
    ".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));"
    "gap:10px;margin:8px 0}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(115px,1fr));"
    "gap:8px;margin-top:8px}"
    ".actionbox{background:#222;padding:10px;border-radius:10px;margin:8px 0}"
    "label{display:flex;flex-direction:column;gap:4px;color:#bbb;font-size:13px}"
    "input,select{background:#292929;color:#fff;border:1px solid #555;"
    "border-radius:7px;padding:7px;width:100%}"
    "a{color:#8fd3ff}"
    ".btn{display:inline-block;padding:10px 12px;margin:4px;background:#333;"
    "border-radius:10px;text-decoration:none;color:#fff}"
    ".btn.active{background:#0877d8}"
    ".save{background:#0a7a35;color:white;border:0;padding:12px 16px;"
    "border-radius:12px;font-weight:700;font-size:16px}"
    ".danger{background:#722}"
    ".small{color:#aaa;font-size:13px}"
    ".badge{display:inline-block;background:#303030;padding:3px 7px;"
    "border-radius:8px;font-size:12px;margin-left:6px}"
    "</style></head><body>";

  html += "<h2>ESP32-S3 USB-MIDI Foot Controller</h2>";

  html += "<div class='card'><b>Active profile:</b> ";
  html += htmlEscape(cfg.profiles[cfg.activeProfile].name);
  html +=
    "<br><span class='small'>USB-MIDI uses the native USB-C port. "
    "Hold <b>FS2+FS7</b> for web configuration; hold <b>FS3+FS8</b> "
    "for ESP-NOW. Current mode: <b>";
  html += htmlEscape(wirelessStatusText().c_str());
  html += "</b>. Configuration AP: <b>";
  html += AP_SSID;
  html +=
    "</b>; open <b>192.168.4.1</b>.</span>";
  html += profileLinks();
  html +=
    "<a class='btn' href='/led-all?state=1'>All LEDs ON</a>"
    "<a class='btn' href='/led-all?state=0'>Restore LEDs</a>"
    "<a class='btn danger' href='/reset' "
    "onclick='return confirm(\"Reset all mappings to defaults?\")'>"
    "Reset defaults</a>";
  html += "</div>";

  html += "<form method='POST' action='/save'>";
  html += "<input type='hidden' name='p' value='" +
          String(profileIndex) + "'>";

  html += "<div class='card'><h3>Edit profile: ";
  html += htmlEscape(profile.name);
  html += "</h3><div class='row'>";
  html += "<label>Profile name<input name='pname' value='" +
          htmlEscape(profile.name) + "'></label>";
  html += "<label>Default channel" +
          numberInput(
            "defch",
            profile.defaultChannel,
            1,
            16
          ) +
          "</label>";
  html += "<label>Total pedalboards" +
          numberInput(
            "totalpb",
            cfg.totalPedalboards,
            1,
            127
          ) +
          "</label>";
  html += "</div></div>";

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    ButtonConfig &button = profile.buttons[i];
    String index = String(i);

    html += "<div class='fs'><h3>FS" + String(i + 1);
    html += "<span class='badge'>GPIO " +
            String(FS_PINS[i]) + "</span>";
    html += "<span class='badge'>LED GPIO " +
            String(LED_PINS[i]) + "</span></h3>";

    html += "<div class='row'>";
    html += "<label>Label<input name='l" + index +
            "' value='" + htmlEscape(button.label) + "'></label>";
    html += "<label>Long-press time (ms)" +
            numberInput(
              "longms" + index,
              button.longPressMs,
              250,
              5000
            ) +
            "</label>";
    html += "</div>";

    html += actionEditor(i, "s", "Short press", button.shortAction);
    html += actionEditor(i, "l", "Long press", button.longAction);

    html += "<div class='actionbox'><b>LED behaviour</b><div class='grid'>";
    html += "<label>LED mode" +
            ledModeSelect("lm" + index, button.led.mode) +
            "</label>";
    html += "<label>Action controlling LED" +
            ledTriggerSelect("lt" + index, button.led.trigger) +
            "</label>";
    html += "<label>Exclusive group (0-8)" +
            numberInput(
              "lg" + index,
              button.led.group,
              0,
              8
            ) +
            "</label>";
    html += "<label>Blinks after action (0-6)" +
            numberInput(
              "lbc" + index,
              button.led.blinkCount,
              0,
              6
            ) +
            "</label>";
    html += "<label>Blink half-period (ms)" +
            numberInput(
              "lbm" + index,
              button.led.blinkMs,
              40,
              1000
            ) +
            "</label>";
    html += "</div>";
    html += "<a class='btn' href='/led-test?i=" +
            String(i) + "'>Test this LED</a>";
    html += "</div></div>";
  }

  html +=
    "<div class='card'><button class='save' type='submit'>"
    "Save profile</button></div></form>";

  html +=
    "<div class='card'><h3>How the LED modes work</h3>"
    "<p><b>Always ON:</b> intended for FS1/FS6 bank navigation. "
    "Set blinks to 2 for a visible bank-change flash.</p>"
    "<p><b>Exclusive group:</b> ideal for MODEP snapshots. Give all "
    "snapshot switches the same non-zero group; selecting one turns the "
    "others in that group off.</p>"
    "<p><b>Follow CC toggle:</b> tracks the ON/OFF state of a CC Toggle "
    "command.</p>"
    "<p><b>While held:</b> follows the physical footswitch state.</p>"
    "<p><b>Action controlling LED:</b> selects whether the short action, "
    "long action, or both update/blink the LED.</p>"
    "<p><b>Long press:</b> when configured, the short action is delayed "
    "until release. Holding past the threshold sends only the long action. "
    "This prevents a snapshot from being sent before a long-press pedal "
    "parameter command.</p>"
    "</div>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleSetProfile() {
  if (server.hasArg("p")) {
    cfg.activeProfile = clampU8(
      server.arg("p").toInt(),
      0,
      NUM_PROFILES - 1
    );

    saveConfig();
    memset(ledBlink, 0, sizeof(ledBlink));
    refreshAllLeds();

    oledStatus(
      "Profile selected",
      cfg.profiles[cfg.activeProfile].name,
      ""
    );
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void readActionFromRequest(
  ActionConfig &action,
  const char *prefix,
  const String &index
) {
  String p = String(prefix);

  if (server.hasArg(p + "a" + index)) {
    action.action = clampU8(
      server.arg(p + "a" + index).toInt(),
      ACT_NONE,
      ACT_PROFILE_PREV
    );
  }

  if (server.hasArg(p + "ch" + index)) {
    action.channel = clampU8(
      server.arg(p + "ch" + index).toInt(),
      1,
      16
    );
  }

  if (server.hasArg(p + "d1" + index)) {
    action.data1 = clampU8(
      server.arg(p + "d1" + index).toInt(),
      0,
      127
    );
  }

  if (server.hasArg(p + "d2" + index)) {
    action.data2 = clampU8(
      server.arg(p + "d2" + index).toInt(),
      0,
      127
    );
  }

  if (server.hasArg(p + "off" + index)) {
    action.offValue = clampU8(
      server.arg(p + "off" + index).toInt(),
      0,
      127
    );
  }
}

void handleSave() {
  if (!server.hasArg("p")) {
    server.send(400, "text/plain", "Missing profile index");
    return;
  }

  uint8_t profileIndex = clampU8(
    server.arg("p").toInt(),
    0,
    NUM_PROFILES - 1
  );

  ProfileConfig &profile = cfg.profiles[profileIndex];

  if (server.hasArg("pname")) {
    safeCopy(
      profile.name,
      server.arg("pname"),
      sizeof(profile.name)
    );
  }

  if (server.hasArg("defch")) {
    profile.defaultChannel = clampU8(
      server.arg("defch").toInt(),
      1,
      16
    );
  }

  if (server.hasArg("totalpb")) {
    cfg.totalPedalboards = clampU8(
      server.arg("totalpb").toInt(),
      1,
      127
    );
  }

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    ButtonConfig &button = profile.buttons[i];
    String index = String(i);

    if (server.hasArg("l" + index)) {
      safeCopy(
        button.label,
        server.arg("l" + index),
        sizeof(button.label)
      );
    }

    readActionFromRequest(button.shortAction, "s", index);
    readActionFromRequest(button.longAction, "l", index);

    if (server.hasArg("longms" + index)) {
      button.longPressMs = clampU16(
        server.arg("longms" + index).toInt(),
        250,
        5000
      );
    }

    if (server.hasArg("lm" + index)) {
      button.led.mode = clampU8(
        server.arg("lm" + index).toInt(),
        LED_MODE_OFF,
        LED_MODE_FOLLOW_CC_TOGGLE
      );
    }

    if (server.hasArg("lt" + index)) {
      button.led.trigger = clampU8(
        server.arg("lt" + index).toInt(),
        LED_TRIGGER_SHORT,
        LED_TRIGGER_BOTH
      );
    }

    if (server.hasArg("lg" + index)) {
      button.led.group = clampU8(
        server.arg("lg" + index).toInt(),
        0,
        8
      );
    }

    if (server.hasArg("lbc" + index)) {
      button.led.blinkCount = clampU8(
        server.arg("lbc" + index).toInt(),
        0,
        6
      );
    }

    if (server.hasArg("lbm" + index)) {
      button.led.blinkMs = clampU16(
        server.arg("lbm" + index).toInt(),
        40,
        1000
      );
    }
  }

  cfg.activeProfile = profileIndex;
  saveConfig();

  memset(ledBlink, 0, sizeof(ledBlink));
  refreshAllLeds();

  oledStatus(
    "Configuration saved",
    cfg.profiles[cfg.activeProfile].name,
    "LED + short + long"
  );

  server.sendHeader(
    "Location",
    "/?p=" + String(profileIndex)
  );
  server.send(303);
}

void handleReset() {
  loadDefaultConfig();
  saveConfig();
  refreshAllLeds();

  oledStatus(
    "Defaults restored",
    cfg.profiles[cfg.activeProfile].name,
    ""
  );

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLedTest() {
  uint8_t index = 0;

  if (server.hasArg("i")) {
    index = clampU8(
      server.arg("i").toInt(),
      0,
      NUM_SWITCHES - 1
    );
  }

  startLedBlink(index, 3, 100);

  server.sendHeader(
    "Location",
    "/?p=" + String(cfg.activeProfile)
  );
  server.send(303);
}

void handleLedAll() {
  bool turnOn = server.hasArg("state") &&
                server.arg("state").toInt() != 0;

  if (turnOn) {
    for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
      ledBlink[i].active = false;
      writeLedHardware(i, true);
    }
  } else {
    memset(ledBlink, 0, sizeof(ledBlink));
    refreshAllLeds();
  }

  server.sendHeader(
    "Location",
    "/?p=" + String(cfg.activeProfile)
  );
  server.send(303);
}

void registerWebRoutesOnce() {
  if (webRoutesRegistered) return;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/profile", HTTP_GET, handleSetProfile);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_GET, handleReset);
  server.on("/led-test", HTTP_GET, handleLedTest);
  server.on("/led-all", HTTP_GET, handleLedAll);
  webRoutesRegistered = true;
}

void startWebConfig() {
  if (webConfigEnabled) return;
  if (espNowEnabled) stopEspNow();

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(true);
  bool started = WiFi.softAP(
    AP_SSID,
    AP_PASS,
    ESPNOW_CHANNEL,
    false,
    2
  );

  if (!started) {
    Serial.println("Could not start configuration AP");
    WiFi.mode(WIFI_OFF);
    oledStatus("Web AP failed", "Check Serial Monitor", "");
    return;
  }

  registerWebRoutesOnce();
  server.begin();
  webConfigEnabled = true;

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("Config AP ON: %s password: %s\n", AP_SSID, AP_PASS);
  Serial.print("Web UI: http://");
  Serial.println(ip);

  startLedBlink(1, 2, 90);  // FS2 LED
  startLedBlink(6, 2, 90);  // FS7 LED
  oledStatus("Web config ON", AP_SSID, ip.toString());
}

void stopWebConfig() {
  if (!webConfigEnabled) return;

  server.stop();
  WiFi.softAPdisconnect(true);
  webConfigEnabled = false;

  if (!espNowEnabled) WiFi.mode(WIFI_OFF);

  Serial.println("Web configuration AP OFF");
  startLedBlink(1, 1, 120);
  startLedBlink(6, 1, 120);
  oledStatus("Web config OFF", cfg.profiles[cfg.activeProfile].name, "USB-MIDI active");
}

// ============================================================
// SETUP AND LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println(
    "ESP32-S3 USB-MIDI + On-demand Web + ESP-NOW + LEDs"
  );

  if (!loadConfig()) {
    Serial.println(
      "No compatible MID2 configuration. Loading new defaults."
    );
    loadDefaultConfig();
    saveConfig();
  } else {
    Serial.println("Loaded saved MID2 configuration.");
    memset(actionToggleState, 0, sizeof(actionToggleState));
    memset(ledLatchedState, 0, sizeof(ledLatchedState));
    memset(ledBlink, 0, sizeof(ledBlink));
  }

  // Initialise LEDs first and keep them off until configuration is ready.
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    writeLedHardware(i, false);
  }

  initOLED();
  oledStatus("Booting", "USB-MIDI + Web", "");

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    pinMode(FS_PINS[i], INPUT_PULLUP);

    bool initial = digitalRead(FS_PINS[i]);
    lastRaw[i] = initial;
    stableState[i] = initial;
    lastChangeTime[i] = millis();
    pressStartedAt[i] = 0;
    longActionFired[i] = false;
    shortActionStartedOnPress[i] = false;
    comboSuppressed[i] = false;
  }

  webModeCombo = {false, false, 0};
  espNowModeCombo = {false, false, 0};
  WiFi.mode(WIFI_OFF);

  refreshAllLeds();

  MIDI.begin();
  USB.begin();
  Serial.println("USB-MIDI started.");

  oledStatus(
    "Ready: USB-MIDI",
    cfg.profiles[cfg.activeProfile].name,
    "FS2+7 Web | FS3+8 NOW"
  );

  Serial.println("Wi-Fi is OFF at boot to reduce power.");
  Serial.println("Hold FS2+FS7: toggle web configuration AP.");
  Serial.println("Hold FS3+FS8: toggle ESP-NOW transmitter.");
}

void loop() {
  if (webConfigEnabled) server.handleClient();

  uint32_t now = millis();

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    bool raw = digitalRead(FS_PINS[i]);

    if (raw != lastRaw[i]) {
      lastRaw[i] = raw;
      lastChangeTime[i] = now;
    }

    if ((uint32_t)(now - lastChangeTime[i]) >= DEBOUNCE_MS) {
      if (raw != stableState[i]) {
        stableState[i] = raw;

        if (raw == LOW) {
          onSwitchPressed(i, now);
        } else {
          onSwitchReleased(i);
        }

        // Required for LED_MODE_MOMENTARY.
        renderLed(i);
      }
    }
  }

  serviceModeCombos(now);
  serviceLongPresses(now);
  serviceLedBlinks(now);

  delay(1);
}