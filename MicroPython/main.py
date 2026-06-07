# main.py (Raspberry Pi Pico, MicroPython)
# MODEP 6-switch USB-MIDI controller
#
# Layout:
#   2 -- 1 -- 0
#   5 -- 4 -- 3
#
# SW_PINS = [2, 3, 4, 5, 6, 28]  # switch 0..5
#
# Short press:
#   0 PB next, 3 PB prev
#   2/1/5/4 -> Snapshots 1/2/3/4
#
# Long press:
#   2 toggles Tuner bypass (CC)
#   1 toggles Delay bypass (CC)
#   5 toggles Reverb bypass (CC)
#   4 toggles Boost/OD bypass (CC)
#
# Combo long press:
#   0 + 3 together resets pedalboard counter (optionally sends PC=0)

import time
from machine import Pin

import usb.device
from usb.device.midi import MIDIInterface

# -------------------- MODEP CHANNELS (profile json uses 1..16) --------------------
PEDALBOARD_CH_JSON = 15
SNAPSHOT_CH_JSON   = 14

PEDALBOARD_CH = PEDALBOARD_CH_JSON - 1  # 0..15
SNAPSHOT_CH   = SNAPSHOT_CH_JSON   - 1  # 0..15

# We'll send CC toggles on the SNAPSHOT channel (easy to "learn" in UI)
CC_CH = SNAPSHOT_CH

# -------------------- TIMING --------------------
DEBOUNCE_MS     = 35
LONGPRESS_MS    = 600
COMBOPRESS_MS   = 650

PB_COOLDOWN_MS  = 1500   # pedalboard loads can be heavy
SS_COOLDOWN_MS  = 180    # snapshots are lighter

# -------------------- PEDALBOARD COUNT --------------------
# Set to the number of pedalboards you want to cycle through (ideally: your LIVE bank length)
TOTAL_PBS = 16

# If True: when you reset (0+3), also send PC=0 to actually load PB#0
SEND_PC_ON_RESET = True

# -------------------- CC ASSIGNMENTS (choose any numbers you like) --------------------
CC_TUNER  = 20
CC_DELAY  = 21
CC_REVERB = 22
CC_BOOST  = 23

CC_OFF = 0
CC_ON  = 127  # MODEP bypass treats <64 off, >=64 on

# -------------------- GPIO pins in switch-number order --------------------
SW_PINS = [2, 3, 4, 5, 6, 28]  # switch 0..5

# -------------------- USB MIDI --------------------
class MIDITx(MIDIInterface):
    def program_change(self, channel: int, program: int) -> bool:
        channel &= 0x0F
        program &= 0x7F
        w = self._tx.pend_write()
        if len(w) < 4:
            return False
        w[0] = 0x0C                 # CIN for Program Change
        w[1] = 0xC0 | channel
        w[2] = program
        w[3] = 0
        self._tx.finish_write(4)
        self._tx_xfer()
        return True

    def control_change(self, channel: int, cc: int, value: int) -> bool:
        channel &= 0x0F
        cc &= 0x7F
        value &= 0x7F
        w = self._tx.pend_write()
        if len(w) < 4:
            return False
        w[0] = 0x0B                 # CIN for Control Change (3 bytes)
        w[1] = 0xB0 | channel
        w[2] = cc
        w[3] = value
        self._tx.finish_write(4)
        self._tx_xfer()
        return True

midi = MIDITx()
# builtin_driver=True keeps CDC REPL too (as shown in MicroPython USB MIDI example style)
usb.device.get().init(midi, builtin_driver=True)

# -------------------- LED --------------------
try:
    led = Pin("LED", Pin.OUT)
except Exception:
    led = Pin(25, Pin.OUT)

def blink(ms=70, times=1):
    for _ in range(times):
        led.off()
        time.sleep_ms(ms)
        led.on()
        time.sleep_ms(30)

led.on()

# -------------------- SWITCHES --------------------
switches = [Pin(gp, Pin.IN, Pin.PULL_UP) for gp in SW_PINS]
last_val = [sw.value() for sw in switches]
last_change_t = [time.ticks_ms() for _ in switches]
press_t = [0 for _ in switches]
long_fired = [False for _ in switches]

# Pedalboard counter
current_pb = 0

# Cooldowns
next_pb_ok = 0
next_ss_ok = 0

# Toggle states
tuner_on  = False
delay_on  = False
reverb_on = False
boost_on  = False

# Combo state
combo_started_t = 0
combo_fired = False

def _cooldown_ok(next_ok_ms):
    now = time.ticks_ms()
    return time.ticks_diff(now, next_ok_ms) >= 0, now

def send_pc(ch, program):
    blink()
    if not midi.program_change(ch, program):
        time.sleep_ms(2)
        midi.program_change(ch, program)

def send_cc(ch, cc, value):
    blink()
    if not midi.control_change(ch, cc, value):
        time.sleep_ms(2)
        midi.control_change(ch, cc, value)

def pedalboard_select(idx):
    global current_pb, next_pb_ok
    ok, now = _cooldown_ok(next_pb_ok)
    if not ok:
        return
    current_pb = idx % TOTAL_PBS
    next_pb_ok = time.ticks_add(now, PB_COOLDOWN_MS)
    send_pc(PEDALBOARD_CH, current_pb)

def pedalboard_next():
    pedalboard_select(current_pb + 1)

def pedalboard_prev():
    pedalboard_select(current_pb - 1)

def snapshot_select(n_1_to_4):
    global next_ss_ok
    ok, now = _cooldown_ok(next_ss_ok)
    if not ok:
        return
    next_ss_ok = time.ticks_add(now, SS_COOLDOWN_MS)
    # MODEP snapshot nav is typically PC=1..4 for snapshots 1..4
    send_pc(SNAPSHOT_CH, n_1_to_4)

def reset_pedalboard_counter():
    global current_pb
    current_pb = 0
    blink(ms=50, times=2)
    if SEND_PC_ON_RESET:
        send_pc(PEDALBOARD_CH, 0)

def handle_shortpress(i):
    if i == 0:
        pedalboard_next()
    elif i == 3:
        pedalboard_prev()
    elif i == 2:
        snapshot_select(1)
    elif i == 1:
        snapshot_select(2)
    elif i == 5:
        snapshot_select(3)
    elif i == 4:
        snapshot_select(4)

def handle_longpress(i):
    global tuner_on, delay_on, reverb_on, boost_on
    if i == 2:
        tuner_on = not tuner_on
        send_cc(CC_CH, CC_TUNER, CC_ON if tuner_on else CC_OFF)
    elif i == 1:
        delay_on = not delay_on
        send_cc(CC_CH, CC_DELAY, CC_ON if delay_on else CC_OFF)
    elif i == 5:
        reverb_on = not reverb_on
        send_cc(CC_CH, CC_REVERB, CC_ON if reverb_on else CC_OFF)
    elif i == 4:
        boost_on = not boost_on
        send_cc(CC_CH, CC_BOOST, CC_ON if boost_on else CC_OFF)

print("MODEP 6-switch controller running")
print("PB CH =", PEDALBOARD_CH_JSON, "SS CH =", SNAPSHOT_CH_JSON, "CC CH =", (CC_CH + 1))
print("TOTAL_PBS =", TOTAL_PBS)

while True:
    now = time.ticks_ms()

    # -------- Combo detection (0 + 3 held) --------
    s0_down = (switches[0].value() == 0)
    s3_down = (switches[3].value() == 0)

    if s0_down and s3_down:
        if combo_started_t == 0:
            combo_started_t = now
            combo_fired = False
        elif (not combo_fired) and time.ticks_diff(now, combo_started_t) >= COMBOPRESS_MS:
            combo_fired = True
            # prevent their individual actions
            long_fired[0] = True
            long_fired[3] = True
            reset_pedalboard_counter()
    else:
        combo_started_t = 0
        combo_fired = False

    # -------- Individual switch handling --------
    for i, sw in enumerate(switches):
        v = sw.value()

        # debounce on edges
        if v != last_val[i] and time.ticks_diff(now, last_change_t[i]) >= DEBOUNCE_MS:
            last_change_t[i] = now
            last_val[i] = v

            if v == 0:   # pressed
                press_t[i] = now
                long_fired[i] = False
            else:        # released
                # if longpress already fired (or combo suppressed it), do nothing
                if not long_fired[i]:
                    handle_shortpress(i)

        # long-press detect while held (ignore if combo already fired)
        if last_val[i] == 0 and (not long_fired[i]) and (not combo_fired):
            if time.ticks_diff(now, press_t[i]) >= LONGPRESS_MS:
                long_fired[i] = True
                handle_longpress(i)

    time.sleep_ms(2)