/*
  ESP32-S3 ESP-NOW -> USB-MIDI Receiver

  Purpose:
    - Receives raw MIDI events from the main ESP32-S3 foot controller by
      ESP-NOW on a fixed Wi-Fi channel.
    - Re-emits them as class-compliant USB-MIDI through the native USB port.

  USB-C usage on the receiver board:
    - Right USB-C / CH343P: upload + Serial Monitor
    - Left USB-C / native USB OTG: connect to Raspberry Pi / Linux computer

  Arduino IDE:
    Board: ESP32S3 Dev Module
    USB Mode: USB-OTG / TinyUSB
    USB CDC On Boot: Disabled
    Flash Size: 16MB
    PSRAM: OPI PSRAM

  Additional library:
    - Adafruit NeoPixel

  The onboard WS2812 RGB LED on GPIO48 flashes green whenever a valid
  ESP-NOW MIDI packet is received and forwarded to USB-MIDI.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <Adafruit_NeoPixel.h>
#include "USB.h"
#include "USBMIDI.h"

// Expose only the class-compliant USB-MIDI interface on the native USB port.
// In Arduino IDE, keep both USB CDC On Boot and USB DFU On Boot disabled.
#if defined(ARDUINO_USB_DFU_ON_BOOT) && ARDUINO_USB_DFU_ON_BOOT
#error "Disable Tools > USB DFU On Boot for StageLinkX Rx."
#endif

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#error "Disable Tools > USB CDC On Boot; use the CH343P USB port for Serial Monitor."
#endif

static const char *USB_PRODUCT_NAME = "StageLinkX Rx";
static const char *USB_MANUFACTURER_NAME = "StageLinkX";
static const char *USB_SERIAL_NUMBER = "STAGELINKX-RX-001";

USBMIDI MIDI("StageLinkX Rx");

// -------------------- ONBOARD RGB STATUS LED --------------------
// This ESP32-S3 board uses a single onboard WS2812 RGB LED on GPIO48.
// Keep the brightness modest to reduce power draw from the Raspberry Pi.
static const uint8_t RGB_LED_PIN = 48;
static const uint8_t RGB_LED_COUNT = 1;
static const uint8_t RX_LED_BRIGHTNESS = 24;
static const uint32_t RX_LED_PULSE_MS = 55;

Adafruit_NeoPixel receiverLed(
  RGB_LED_COUNT,
  RGB_LED_PIN,
  NEO_GRB + NEO_KHZ800
);

bool receiverLedOn = false;
uint32_t receiverLedOffAt = 0;

void setReceiverLed(bool on) {
  if (on) {
    receiverLed.setPixelColor(
      0,
      receiverLed.Color(0, RX_LED_BRIGHTNESS, 0)
    );
  } else {
    receiverLed.clear();
  }
  receiverLed.show();
  receiverLedOn = on;
}

void pulseReceiverLed() {
  setReceiverLed(true);
  receiverLedOffAt = millis() + RX_LED_PULSE_MS;
}

void serviceReceiverLed() {
  if (receiverLedOn &&
      (int32_t)(millis() - receiverLedOffAt) >= 0) {
    setReceiverLed(false);
  }
}

static const uint8_t ESPNOW_CHANNEL = 6;
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

// Explicit prototypes are required here because Arduino automatically creates
// prototypes for functions in .ino files. With a user-defined packet type,
// that generated prototype can otherwise be placed before the struct and cause
// "EspNowMidiPacket does not name a type" compilation errors.
bool packetIsValid(const EspNowMidiPacket &packet);
void queueReceivedPacket(const uint8_t *data, int length);
void emitUsbMidi(const EspNowMidiPacket &packet);
bool startEspNowReceiver();

#if ESP_IDF_VERSION_MAJOR >= 5
void onEspNowReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int length
);
#else
void onEspNowReceive(
  const uint8_t *macAddress,
  const uint8_t *data,
  int length
);
#endif

QueueHandle_t midiQueue = nullptr;
volatile uint32_t droppedPackets = 0;
uint16_t lastSequence = 0;

bool packetIsValid(const EspNowMidiPacket &packet) {
  if (packet.magic != ESPNOW_PACKET_MAGIC) return false;
  if (packet.version != ESPNOW_PACKET_VERSION) return false;
  if (packet.type < WIRELESS_CC ||
      packet.type > WIRELESS_PROGRAM_CHANGE) return false;
  if (packet.channel < 1 || packet.channel > 16) return false;
  if (packet.data1 > 127 || packet.data2 > 127) return false;
  return true;
}

void queueReceivedPacket(const uint8_t *data, int length) {
  if (data == nullptr || length != (int)sizeof(EspNowMidiPacket)) return;
  if (midiQueue == nullptr) return;

  EspNowMidiPacket packet{};
  memcpy(&packet, data, sizeof(packet));
  if (!packetIsValid(packet)) return;

  if (xQueueSend(midiQueue, &packet, 0) != pdTRUE) {
    droppedPackets++;
  }
}

#if ESP_IDF_VERSION_MAJOR >= 5
void onEspNowReceive(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int length
) {
  (void)info;
  queueReceivedPacket(data, length);
}
#else
void onEspNowReceive(
  const uint8_t *macAddress,
  const uint8_t *data,
  int length
) {
  (void)macAddress;
  queueReceivedPacket(data, length);
}
#endif

void emitUsbMidi(const EspNowMidiPacket &packet) {
  switch (packet.type) {
    case WIRELESS_CC:
      MIDI.controlChange(packet.data1, packet.data2, packet.channel);
      Serial.printf(
        "RX seq=%u profile=%u CC ch=%u cc=%u value=%u\n",
        packet.sequence,
        packet.profile,
        packet.channel,
        packet.data1,
        packet.data2
      );
      break;

    case WIRELESS_NOTE_ON:
      MIDI.noteOn(packet.data1, packet.data2, packet.channel);
      Serial.printf(
        "RX seq=%u profile=%u NoteOn ch=%u note=%u vel=%u\n",
        packet.sequence,
        packet.profile,
        packet.channel,
        packet.data1,
        packet.data2
      );
      break;

    case WIRELESS_NOTE_OFF:
      MIDI.noteOff(packet.data1, packet.data2, packet.channel);
      Serial.printf(
        "RX seq=%u profile=%u NoteOff ch=%u note=%u\n",
        packet.sequence,
        packet.profile,
        packet.channel,
        packet.data1
      );
      break;

    case WIRELESS_PROGRAM_CHANGE:
      MIDI.programChange(packet.data1, packet.channel);
      Serial.printf(
        "RX seq=%u profile=%u PC ch=%u program=%u\n",
        packet.sequence,
        packet.profile,
        packet.channel,
        packet.data1
      );
      break;

    default:
      break;
  }

  lastSequence = packet.sequence;
  pulseReceiverLed();
}

bool startEspNowReceiver() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);

  // Disable power saving for predictable live-control latency. Power this
  // receiver from a powered hub if the Raspberry Pi USB power is marginal.
  WiFi.setSleep(false);
  delay(30);

  esp_err_t channelResult = esp_wifi_set_channel(
    ESPNOW_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );

  if (channelResult != ESP_OK) {
    Serial.printf("Failed to set channel: %d\n", (int)channelResult);
    return false;
  }

  esp_err_t initResult = esp_now_init();
  if (initResult != ESP_OK) {
    Serial.printf("ESP-NOW init failed: %d\n", (int)initResult);
    return false;
  }

  esp_err_t callbackResult = esp_now_register_recv_cb(onEspNowReceive);
  if (callbackResult != ESP_OK) {
    Serial.printf(
      "ESP-NOW receive callback registration failed: %d\n",
      (int)callbackResult
    );
    esp_now_deinit();
    return false;
  }

  Serial.printf(
    "ESP-NOW receiver ready on channel %u; MAC %s\n",
    ESPNOW_CHANNEL,
    WiFi.macAddress().c_str()
  );
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("ESP32-S3 ESP-NOW to USB-MIDI receiver");

  receiverLed.begin();
  receiverLed.setBrightness(255);
  setReceiverLed(false);

  midiQueue = xQueueCreate(32, sizeof(EspNowMidiPacket));
  if (midiQueue == nullptr) {
    Serial.println("Could not create MIDI packet queue");
    while (true) delay(1000);
  }

  // USB descriptor strings must be set before USB.begin().
  USB.manufacturerName(USB_MANUFACTURER_NAME);
  USB.productName(USB_PRODUCT_NAME);
  USB.serialNumber(USB_SERIAL_NUMBER);

  MIDI.begin();
  USB.begin();
  Serial.printf("USB-MIDI started as: %s\n", USB_PRODUCT_NAME);

  if (!startEspNowReceiver()) {
    Serial.println("ESP-NOW receiver failed to start.");
    while (true) delay(1000);
  }

  Serial.println("Waiting for MIDI packets from the foot controller...");
}

void loop() {
  serviceReceiverLed();

  EspNowMidiPacket packet{};

  while (xQueueReceive(midiQueue, &packet, 0) == pdTRUE) {
    emitUsbMidi(packet);
    serviceReceiverLed();
  }

  static uint32_t lastDropReport = 0;
  if (droppedPackets != lastDropReport) {
    lastDropReport = droppedPackets;
    Serial.printf("Dropped ESP-NOW packets: %u\n", droppedPackets);
  }

  delay(1);
}