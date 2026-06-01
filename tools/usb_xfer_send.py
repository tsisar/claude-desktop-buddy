#!/usr/bin/env python3
"""
usb_xfer_send.py — push a character-pack folder to the device over USB-CDC.

Mimics what Claude Desktop sends when you drag a folder onto the Hardware
Buddy window:
  • {"cmd":"char_begin","name":"<folder>","total":<bytes>}
  • for each file:
      {"cmd":"file","path":"<name>","size":N}
      {"cmd":"chunk","d":"<base64>"}*   (~256B raw per chunk)
      {"cmd":"file_end"}
  • {"cmd":"char_end"}

Waits for the matching ack before sending the next command — protocol is
strictly sequential. Mutually exclusive with `make monitor` (single
process per port).

Usage:
  tools/usb_xfer_send.sh ./my-axolotl
  tools/usb_xfer_send.sh ./my-axolotl --port /dev/ttyACM0

Quick sanity test (any non-GIF bytes are fine for verifying the receive
path lands files on storage; the renderer in Stage 4d will be the one
that cares about format):
  mkdir -p /tmp/testpack
  echo 'hi' > /tmp/testpack/sleep.gif
  echo '{"name":"test"}' > /tmp/testpack/manifest.json
  tools/usb_xfer_send.sh /tmp/testpack
"""

import argparse
import base64
import json
import os
import queue
import sys
import threading
import time

import serial


class LineReader:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        lines = []
        for b in data:
            if b in (10, 13):
                if self.buf:
                    try:
                        lines.append(self.buf.decode("utf-8"))
                    except UnicodeDecodeError:
                        pass
                    self.buf.clear()
            else:
                self.buf.append(b)
        return lines


def list_files(folder):
    """One level deep, dotfiles skipped, files only — matches the desktop's
    sender. Returns [(filename, abspath, size_bytes), ...]."""
    out = []
    for name in sorted(os.listdir(folder)):
        if name.startswith("."):
            continue
        p = os.path.join(folder, name)
        if os.path.isfile(p):
            out.append((name, p, os.path.getsize(p)))
    return out


def chunk_write(ser, line: bytes):
    """64-byte chunks with a 5ms gap — same shape as usb_test_bridge,
    keeps the ESP32-S3 USB-CDC RX ring buffer happy on long JSON lines."""
    CHUNK = 64
    for off in range(0, len(line), CHUNK):
        ser.write(line[off:off + CHUNK])
        ser.flush()
        if off + CHUNK < len(line):
            time.sleep(0.005)


def send_and_ack(ser, obj, ack_queue, expected, timeout=10.0):
    """Send one JSON object, wait for ack:'<expected>'. Returns the ack
    dict. Raises TimeoutError if no matching ack arrives."""
    chunk_write(ser, (json.dumps(obj) + "\n").encode("utf-8"))
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            msg = ack_queue.get(timeout=0.2)
        except queue.Empty:
            continue
        if msg.get("ack") == expected:
            return msg
        # Anything else (status, foreign acks) just gets dropped — the
        # reader thread already printed it.
    raise TimeoutError(f"no ack:'{expected}' within {timeout}s")


def reader_thread(ser, ack_queue, stop_flag, line_reader):
    while not stop_flag[0]:
        try:
            data = ser.read(128)
        except serial.SerialException:
            return
        if not data:
            continue
        for line in line_reader.feed(data):
            if line.startswith("{"):
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    print(f"[device] {line}")
                    continue
                if "ack" in msg:
                    ack_queue.put(msg)
                    print(f"[ack ] {msg}")
                else:
                    print(f"[recv] {msg}")
            else:
                print(f"[device] {line}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("folder", help="character-pack folder (manifest.json + GIFs)")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--chunk", type=int, default=192,
                    help="raw bytes per chunk (default 192 → ~256B base64)")
    args = ap.parse_args()

    if not os.path.isdir(args.folder):
        print(f"not a directory: {args.folder}")
        return 1

    files = list_files(args.folder)
    if not files:
        print(f"no files in {args.folder} (dotfiles and subdirs are skipped)")
        return 1
    total = sum(sz for _, _, sz in files)
    name = os.path.basename(os.path.abspath(args.folder)).strip()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1,
                            dsrdtr=False, rtscts=False)
    except serial.SerialException as e:
        print(f"open {args.port}: {e}")
        if "Permission denied" in str(e):
            print("hint: sudo usermod -aG dialout $USER && newgrp dialout")
        elif "Device or resource busy" in str(e):
            print("hint: close `make monitor` — it's holding the port")
        return 1

    ack_queue = queue.Queue()
    line_reader = LineReader()
    stop_flag = [False]
    rt = threading.Thread(target=reader_thread,
                          args=(ser, ack_queue, stop_flag, line_reader),
                          daemon=True)
    rt.start()
    time.sleep(0.2)   # let any stale boot log drain

    print(f"[push] pack='{name}'  files={len(files)}  total={total}B")
    try:
        ack = send_and_ack(ser, {"cmd": "char_begin", "name": name,
                                 "total": total}, ack_queue, "char_begin")
        if not ack.get("ok"):
            print(f"[push] char_begin DENIED: {ack.get('error', '(no detail)')}")
            return 1

        for i, (fname, fpath, fsize) in enumerate(files, 1):
            print(f"[push] [{i}/{len(files)}] file '{fname}' ({fsize}B)")
            send_and_ack(ser, {"cmd": "file", "path": fname, "size": fsize},
                         ack_queue, "file")
            with open(fpath, "rb") as f:
                while True:
                    raw = f.read(args.chunk)
                    if not raw:
                        break
                    b64 = base64.b64encode(raw).decode("ascii")
                    send_and_ack(ser, {"cmd": "chunk", "d": b64},
                                 ack_queue, "chunk")
            ack = send_and_ack(ser, {"cmd": "file_end"}, ack_queue, "file_end")
            if not ack.get("ok"):
                print(f"[push] file_end mismatch for {fname}: {ack}")
                return 1

        ack = send_and_ack(ser, {"cmd": "char_end"}, ack_queue, "char_end")
        if ack.get("ok"):
            print(f"[push] DONE — character '{name}' installed and active")
        else:
            print(f"[push] receive OK, files are on storage")
            print(f"[push] char_end ack=false because the renderer is stubbed")
            print(f"[push] (Stage 4d will wire characterInit to read them)")
        return 0
    except (TimeoutError, KeyboardInterrupt) as e:
        print(f"[push] aborted: {e}")
        return 1
    finally:
        stop_flag[0] = True
        try: ser.close()
        except: pass


if __name__ == "__main__":
    sys.exit(main())
