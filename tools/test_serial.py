#!/usr/bin/env python3
"""Prove the Stick reacts to serial JSON. Cycles states every 3s."""
import json, time, serial, glob, sys

# ESP32-S3 native USB-CDC enumerates as /dev/ttyACM* on Linux — same port
# usb_test_bridge.py / usb_xfer_send.py default to (/dev/ttyACM0).
ports = sorted(glob.glob('/dev/ttyACM*')) or ['/dev/ttyACM0']
# dsrdtr/rtscts off so opening the port doesn't toggle DTR (matches the
# bridges; native USB-CDC ignores it anyway).
s = serial.Serial(ports[0], 115200, dsrdtr=False, rtscts=False)
print(f"writing to {ports[0]} — watch the device\n")

states = [
    {"total": 0, "running": 0, "waiting": 0},  # → sleep
    {"total": 2, "running": 1, "waiting": 0},  # → idle (rotation: wizard, welding, ...)
    {"total": 4, "running": 3, "waiting": 0},  # → busy
    {"total": 2, "running": 1, "waiting": 1},  # → attention, LED blinks
]
for i in range(20):
    st = states[i % len(states)]
    s.write((json.dumps(st) + "\n").encode())
    print(f"  → {st}")
    time.sleep(3)
