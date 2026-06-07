# MIDI Foot Controller

A 6-switch USB-MIDI foot controller for MODEP, built with a Raspberry Pi Pico running MicroPython.

The controller appears as a USB MIDI device and sends Program Change (PC) and Control Change (CC) messages for pedalboard navigation, snapshot selection, and effect bypass toggles.

## Project Structure

```text
MIDIFootController/
├── README.md
└── MicroPython/
    ├── README.md
    └── main.py
```

## Current Firmware

The current implementation is in `MicroPython/main.py`.

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

## Switch Layout

Physical switch numbering used by the firmware:

```text
Top row:     2 -- 1 -- 0
Bottom row:  5 -- 4 -- 3
```

GPIO mapping:

```python
SW_PINS = [2, 3, 4, 5, 6, 28]
```

This means:

| Switch | Pico GPIO |
| --- | --- |
| 0 | GP2 |
| 1 | GP3 |
| 2 | GP4 |
| 3 | GP5 |
| 4 | GP6 |
| 5 | GP28 |

## Schematic Structure

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

The firmware configures each switch pin with the Pico's internal pull-up resistor:

```python
Pin(gp, Pin.IN, Pin.PULL_UP)
```

So no external pull-up resistors are required for the switches. Pressing a switch connects the GPIO pin to ground.

Recommended hardware:

- 1 x Raspberry Pi Pico
- 6 x normally-open momentary footswitches
- 1 x USB cable for power and USB MIDI
- Enclosure, hookup wire, and optional panel connectors

## MIDI Mapping

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

## Install

### 1. Install MicroPython on the Pico

1. Download the latest MicroPython UF2 for Raspberry Pi Pico from the MicroPython website.
2. Hold the Pico `BOOTSEL` button while plugging it into USB.
3. Copy the UF2 file to the `RPI-RP2` drive.
4. Unplug and reconnect the Pico after flashing.

### 2. Install `mpremote`

On your computer:

```bash
python3 -m pip install --user mpremote
```

Check that the Pico is detected:

```bash
mpremote connect list
```

On Linux, the device commonly appears as:

```text
/dev/ttyACM0
```

### 3. Install the USB MIDI package

```bash
mpremote connect /dev/ttyACM0 mip install usb-device-midi
```

If your serial device is different, replace `/dev/ttyACM0` with the device shown by `mpremote connect list`.

### 4. Upload the firmware

From the project root:

```bash
cd MicroPython
mpremote connect /dev/ttyACM0 resume fs cp ./main.py :main.py
```

The `resume` command is important because this firmware enables runtime USB MIDI. Without it, `mpremote` may reset the Pico and temporarily lose the serial connection during file copy.

Verify the file is on the Pico:

```bash
mpremote connect /dev/ttyACM0 resume fs ls :
```

Restart the Pico:

```bash
mpremote connect /dev/ttyACM0 resume reset
```

You can also unplug and reconnect the Pico.

## Test

### Serial startup test

Run the firmware from your computer before installing it permanently:

```bash
cd MicroPython
mpremote connect /dev/ttyACM0 run --no-follow main.py
```

Or open the REPL and reset the board:

```bash
mpremote connect /dev/ttyACM0 repl
```

Expected startup output:

```text
MODEP 6-switch controller running
PB CH = 15 SS CH = 14 CC CH = 14
TOTAL_PBS = 16
```

### MIDI device test

After the Pico restarts, check that your host sees a USB MIDI device.

On Linux:

```bash
aconnect -l
```

You can also use a MIDI monitor such as `aseqdump`, `MIDI Monitor`, or the MIDI learn page in MODEP.

### Footswitch test

1. Connect the Pico to the MODEP host by USB.
2. Open MODEP and enable MIDI learn or MIDI input monitoring.
3. Press each switch and confirm the expected PC or CC message is received.
4. Hold switches 2, 1, 5, or 4 for at least 600 ms to test long-press CC toggles.
5. Hold switches 0 and 3 together for at least 650 ms to test pedalboard counter reset.

The Pico LED should blink when a MIDI message is sent.

## Configuration

Edit these values in `MicroPython/main.py` to match your setup:

```python
PEDALBOARD_CH_JSON = 15
SNAPSHOT_CH_JSON   = 14
TOTAL_PBS = 16

CC_TUNER  = 20
CC_DELAY  = 21
CC_REVERB = 22
CC_BOOST  = 23
```

After changing the file, upload it again:

```bash
cd MicroPython
mpremote connect /dev/ttyACM0 resume fs cp ./main.py :main.py
mpremote connect /dev/ttyACM0 resume reset
```

## Troubleshooting

If upload fails with an input/output error, the Pico may have disconnected while switching USB modes. Try:

```bash
mpremote connect /dev/ttyACM0 resume fs cp ./main.py :main.py
```

If `/dev/ttyACM0` is busy, close Thonny, serial monitors, `screen`, `picocom`, or other programs using the port.

To see what is using the port:

```bash
lsof /dev/ttyACM0
```

# ESP32 Version

An ESP32-based version is planned but not implemented yet.
