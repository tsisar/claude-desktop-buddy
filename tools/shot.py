#!/usr/bin/env python3
"""
shot.py — trigger an on-device screenshot over USB-CDC.

Sends {"cmd":"shot"} and waits for the ack. The device saves the current
frame as a 24-bit BMP onto its storage (SD if a card is mounted, else
LittleFS) under /screenshots/shot-NNNN.bmp and reports the path back.
Nothing is downloaded — the file stays on the card (pop it out to view,
or pull it later). Same effect as holding BOOT on the device, just
host-triggered.

Mutually exclusive with `make monitor` (single process per port).

Usage:
  tools/shot.sh
  tools/shot.sh --port /dev/ttyACM0
"""

import argparse
import json
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="seconds to wait for the ack (default 10)")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2,
                            dsrdtr=False, rtscts=False)
    except serial.SerialException as e:
        print(f"open {args.port}: {e}")
        if "Permission denied" in str(e):
            print("hint: sudo usermod -aG dialout $USER && newgrp dialout")
        elif "Device or resource busy" in str(e):
            print("hint: close `make monitor` — it's holding the port")
        return 1

    ser.reset_input_buffer()
    ser.write(b'{"cmd":"shot"}\n')
    ser.flush()

    # The SD write takes ~0.3s; scan device output until the ack lands.
    t0 = time.time()
    while time.time() - t0 < args.timeout:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if not line.startswith("{"):
            print(f"[device] {line}")
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            print(f"[device] {line}")
            continue
        if msg.get("ack") == "shot":
            if msg.get("ok"):
                print(f"saved on device: {msg.get('path', '(unknown)')}")
                return 0
            print("device reported failure (no storage / write error)")
            return 1
    print(f"no ack within {args.timeout}s")
    return 1


if __name__ == "__main__":
    sys.exit(main())
