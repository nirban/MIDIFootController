# MIDI Foot Controller

A USB-MIDI foot controller project with two firmware options:

- Raspberry Pi Pico + MicroPython, 6 switches, focused on MODEP
- ESP32-S3 + Arduino, 10 switches, OLED display, and multiple MIDI profiles

Both versions appear as class-compliant USB MIDI devices and send Program Change (PC), Control Change (CC), or MIDI note messages depending on the firmware/profile.

## Project Structure

```text
MIDIFootController/
├── README.md
├── ESP32_S3_MIDI/
│   └── ESP_MIDI.ino
└── MicroPython/
    ├── README.md
    └── main.py
```

## Firmware Options

| Folder | Board | Switches | Toolchain |
| --- | --- | ---: | --- |
| `MicroPython/` | Raspberry Pi Pico | 6 | MicroPython + `mpremote` |
| `ESP32_S3_MIDI/` | ESP32-S3-N16R8 with native USB OTG | 10 | Arduino IDE / Arduino CLI |

## Raspberry Pi Pico Firmware

The Pico implementation is in `MicroPython/main.py`.

Target board:

- Raspberry Pi Pico
- MicroPython firmware with USB device support
- `usb-device-midi` MicroPython package

Main features:

- 6 momentary footswitches
- USB MIDI output
- Pedalboard next/previous controls
- Snapshot 1-4 selection
- Long-press CC toggles for tuner, delay, reverb, and boost/OD
- Combo long-press reset for the pedalboard counter
- On-board LED feedback when MIDI messages are sent

### Pico Switch Layout

Physical switch numbering used by the firmware:

```text
Top row:     2 -- 1 -- 0
Bottom row:  5 -- 4 -- 3
```

GPIO mapping:

| Switch | Pico GPIO |
| --- | --- |
| 0 | GP2 |
| 1 | GP3 |
| 2 | GP4 |
| 3 | GP5 |
| 4 | GP6 |
| 5 | GP28 |

### Pico Schematic Structure

Each footswitch is wired as an active-low input.

```text
Raspberry Pi Pico

GP2  ---- Switch 0 ---- GND
GP3  ---- Switch 1 ---- GND
GP4  ---- Switch 2 ---- GND
GP5  ---- Switch 3 ---- GND
GP6  ---- Switch 4 ---- GND
GP28 ---- Switch 5 ---- GND

USB  ---- Host device running MODEP
```

The firmware uses the Pico internal pull-up resistors:

```python
Pin(gp, Pin.IN, Pin.PULL_UP)
```

No external pull-up resistors are required. Pressing a switch connects the GPIO pin to ground.

### Pico MIDI Mapping

Default channels in `main.py`:

| Purpose | MODEP channel number | MIDI channel index |
| --- | ---: | ---: |
| Pedalboard Program Change | 15 | 14 |
| Snapshot Program Change | 14 | 13 |
| CC toggles | 14 | 13 |

Short press actions:

| Switch | Action |
| --- | --- |
| 0 | Next pedalboard |
| 3 | Previous pedalboard |
| 2 | Snapshot 1 |
| 1 | Snapshot 2 |
| 5 | Snapshot 3 |
| 4 | Snapshot 4 |

Long press actions:

| Switch | CC | Action |
| --- | ---: | --- |
| 2 | 20 | Toggle tuner bypass |
| 1 | 21 | Toggle delay bypass |
| 5 | 22 | Toggle reverb bypass |
| 4 | 23 | Toggle boost/OD bypass |

Combo action:

| Switches | Action |
| --- | --- |
| 0 + 3 long press | Reset pedalboard counter and optionally send PC 0 |

### Pico Install

1. Download the latest MicroPython UF2 for Raspberry Pi Pico.
2. Hold `BOOTSEL` while plugging the Pico into USB.
3. Copy the UF2 file to the `RPI-RP2` drive.
4. Unplug and reconnect the Pico after flashing.
5. Install `mpremote` on your computer:

```bash
python3 -m pip install --user mpremote
```

Check that the Pico is detected:

```bash
mpremote connect list
```

Install the USB MIDI package:

```bash
mpremote connect /dev/ttyACM0 mip install usb-device-midi
```

Upload the firmware from the project root:

```bash
cd MicroPython
mpremote connect /dev/ttyACM0 resume fs cp ./main.py :main.py
mpremote connect /dev/ttyACM0 resume reset
```

The `resume` command is important because this firmware enables runtime USB MIDI. Without it, `mpremote` may reset the Pico and temporarily lose the serial connection during file copy.

### Pico Test

Run the firmware before installing it permanently:

```bash
cd MicroPython
mpremote connect /dev/ttyACM0 run --no-follow main.py
```

Expected startup output:

```text
MODEP 6-switch controller running
PB CH = 15 SS CH = 14 CC CH = 14
TOTAL_PBS = 16
```

On Linux, check that the USB MIDI device appears:

```bash
aconnect -l
```

Then press each switch and confirm the expected PC or CC message is received by MODEP or a MIDI monitor.

## ESP32-S3 Firmware

The ESP32-S3 implementation is in `ESP32_S3_MIDI/ESP_MIDI.ino`.

Target board:

- ESP32-S3-N16R8 board with native USB OTG
- Arduino ESP32 core with TinyUSB support
- 10 normally-open momentary footswitches
- 128x64 I2C OLED display at address `0x3C`

Main features:

- 10 active-low footswitch inputs
- Native USB MIDI output from the ESP32-S3 USB OTG port
- Serial monitor and firmware upload through the CH343P USB port
- OLED status display
- Three profiles: MODEP, Neural DSP, and Generic
- FS9/FS10 profile previous/next controls

### ESP32-S3 USB Ports

The sketch is written for a board with two USB-C ports:

| Port | Use |
| --- | --- |
| Right USB-C / CH343P | Arduino upload and serial monitor |
| Left USB-C / native USB OTG | Class-compliant USB MIDI device |

Upload the sketch through the CH343P port. Connect the native USB OTG port to the computer, MODEP host, or tablet that should receive MIDI.

### ESP32-S3 Schematic Structure

Each footswitch is wired as an active-low input.

```text
ESP32-S3

GPIO4  ---- FS1  ---- GND
GPIO5  ---- FS2  ---- GND
GPIO6  ---- FS3  ---- GND
GPIO7  ---- FS4  ---- GND
GPIO8  ---- FS5  ---- GND
GPIO9  ---- FS6  ---- GND
GPIO10 ---- FS7  ---- GND
GPIO11 ---- FS8  ---- GND
GPIO12 ---- FS9  ---- GND
GPIO13 ---- FS10 ---- GND
```

The firmware uses internal pull-up resistors:

```cpp
pinMode(FS_PINS[i], INPUT_PULLUP);
```

No external pull-up resistors are required. Pressing a switch connects the GPIO pin to ground.

OLED wiring:

```text
ESP32-S3 3V3  ---- OLED VCC
ESP32-S3 GND  ---- OLED GND
GPIO17 / SDA  ---- OLED SDA
GPIO18 / SCL  ---- OLED SCL
```

USB wiring:

```text
CH343P USB-C          ---- Computer running Arduino IDE
Native USB OTG USB-C  ---- MIDI host / MODEP device
```

Recommended hardware:

- 1 x ESP32-S3-N16R8 board with native USB OTG
- 10 x normally-open momentary footswitches
- 1 x 128x64 I2C OLED display at `0x3C`
- 2 x USB-C cables if uploading and testing MIDI at the same time
- Enclosure, hookup wire, and optional panel connectors

### ESP32-S3 MIDI Profiles

The ESP32-S3 firmware starts in MODEP profile.

Profile controls:

| Switch | Action |
| --- | --- |
| FS9 | Previous profile |
| FS10 | Next profile |

Profiles cycle in this order:

```text
MODEP -> Neural DSP -> Generic -> MODEP
```

#### MODEP Profile

Default MODEP channels:

| Purpose | MIDI channel |
| --- | ---: |
| Pedalboard Program Change | 15 |
| Snapshot Program Change | 14 |
| CC toggles | 14 |

MODEP actions:

| Switch | Action |
| --- | --- |
| FS1 | Next pedalboard |
| FS2 | Snapshot 1 |
| FS3 | Snapshot 2 |
| FS4 | Snapshot 3 |
| FS5 | Snapshot 4 |
| FS6 | Previous pedalboard |
| FS7 | Toggle delay, CC 21 |
| FS8 | Toggle reverb, CC 22 |
| FS9 | Previous profile |
| FS10 | Next profile |

The sketch also defines tuner and boost CC numbers for future mapping:

```cpp
MODEP_CC_TUNER  = 20
MODEP_CC_BOOST  = 23
```

#### Neural DSP Profile

Neural DSP uses MIDI channel 1.

| Switch | Note |
| --- | ---: |
| FS1 | 36 |
| FS2 | 37 |
| FS3 | 38 |
| FS4 | 39 |
| FS5 | 40 |
| FS6 | 41 |
| FS7 | 42 |
| FS8 | 43 |
| FS9 | Previous profile |
| FS10 | Next profile |

Each press sends a short note-on/note-off pulse.

#### Generic Profile

Generic mode uses MIDI channel 1 and sends toggle CC values.

| Switch | CC |
| --- | ---: |
| FS1 | 20 |
| FS2 | 21 |
| FS3 | 22 |
| FS4 | 23 |
| FS5 | 24 |
| FS6 | 25 |
| FS7 | 26 |
| FS8 | 27 |
| FS9 | Previous profile |
| FS10 | Next profile |

Each press toggles between value `127` and `0`.

### ESP32-S3 Install

Install Arduino ESP32 support:

1. Open `File > Preferences` in Arduino IDE.
2. Add this board manager URL if ESP32 support is not already installed:

```text
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

3. Open `Tools > Board > Boards Manager`.
4. Install `esp32` by Espressif Systems.

Install these libraries from `Tools > Manage Libraries`:

- `Adafruit GFX Library`
- `Adafruit SSD1306`

Open this sketch:

```text
ESP32_S3_MIDI/ESP_MIDI.ino
```

Use these Arduino IDE settings:

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| USB Mode | USB-OTG / TinyUSB |
| USB CDC On Boot | Disabled |
| Flash Size | 16MB |
| PSRAM | OPI PSRAM |

Connect the right USB-C / CH343P port to your computer, select its serial port, and upload.

After upload, open the serial monitor at:

```text
115200 baud
```

Expected serial output:

```text
ESP32-S3 10-switch MIDI controller starting...
USB-MIDI started.
Connect native USB-C OTG port to Linux and run: aconnect -l / aseqdump
```

### ESP32-S3 Use And Test

1. Keep the CH343P USB-C connected if you want serial monitoring.
2. Connect the native USB OTG USB-C port to your MIDI host.
3. The OLED should show `ESP32-S3 MIDI CTRL` and the active profile.
4. Press FS1-FS8 to send MIDI messages for the current profile.
5. Press FS9 or FS10 to change profiles.

On Linux, check that the MIDI device appears:

```bash
aconnect -l
```

Monitor incoming MIDI events:

```bash
aseqdump -l
aseqdump -p <client:port>
```

Replace `<client:port>` with the port shown for `ESP32-S3 Foot Controller`.

### ESP32-S3 Configuration

Edit these values in `ESP32_S3_MIDI/ESP_MIDI.ino` to change behavior:

```cpp
static const uint8_t FS_PINS[NUM_SWITCHES] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

static const uint8_t MODEP_PEDALBOARD_CH = 15;
static const uint8_t MODEP_SNAPSHOT_CH   = 14;
static const uint8_t MODEP_CC_CH         = 14;

static const uint8_t NEURAL_DSP_CH       = 1;
static const uint8_t GENERIC_CH          = 1;

static const uint8_t TOTAL_PEDALBOARDS = 16;
```

OLED pins and address:

```cpp
#define OLED_ADDR 0x3C
#define OLED_SDA  17
#define OLED_SCL  18
```

Upload the sketch again after changing these values.

## Troubleshooting

### Pico Upload Issues

If upload fails with an input/output error, the Pico may have disconnected while switching USB modes. Try:

```bash
mpremote connect /dev/ttyACM0 resume fs cp ./main.py :main.py
```

If `/dev/ttyACM0` is busy, close Thonny, serial monitors, `screen`, `picocom`, or other programs using the port.

To see what is using the port:

```bash
lsof /dev/ttyACM0
```

### ESP32-S3 Upload Issues

If upload fails:

- Make sure the CH343P USB-C port is connected, not only the native USB OTG port.
- Check that the correct serial port is selected in Arduino IDE.
- Put the board into bootloader mode if your board requires it.

If MIDI does not appear:

- Make sure the native USB OTG port is connected to the MIDI host.
- Confirm `USB Mode` is set to `USB-OTG / TinyUSB`.
- Try reconnecting the native USB cable after upload.
- On Linux, run `aconnect -l` again after reconnecting.

If the OLED is blank:

- Check `3V3`, `GND`, `SDA`, and `SCL` wiring.
- Confirm the display address is `0x3C`.
- The code uses the Adafruit SSD1306 library; if your module is a SH1106-only display, use a compatible display library or driver.
