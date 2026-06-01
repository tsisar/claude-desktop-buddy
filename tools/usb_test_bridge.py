#!/usr/bin/env python3
"""
usb_test_bridge.py — fake Claude Desktop bridge over USB-CDC serial.

The device parses heartbeat JSON identically from BLE *and* from its USB
console (data_amoled.h: `_usbLine.feed(Serial, ...)`), and echoes permission
decisions to both. So we can drive every scenario via /dev/ttyACM0 — handy
when Linux BLE radio doesn't reach the device, or when you're already
plugged in over USB anyway. Bonus: device serial logs come back on the same
port, so this script shows them inline with `[device] …`.

Mutually exclusive with `make monitor`: only one process at a time can
hold the serial port. Close the monitor before running this.

Setup is shared with ble_test_bridge.sh (same venv, just adds pyserial):
  ./tools/usb_test_bridge.sh --scenario idle

Usage:
  tools/usb_test_bridge.sh --scenario idle
  tools/usb_test_bridge.sh --scenario working
  tools/usb_test_bridge.sh --scenario prompt
  tools/usb_test_bridge.sh --scenario cycle
  tools/usb_test_bridge.sh --port /dev/ttyACM0 --baud 115200
"""

import argparse
import json
import sys
import threading
import time
from typing import Optional

import serial


class LineReader:
    """Reassemble newline-delimited UTF-8 lines from byte chunks."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        lines = []
        for b in data:
            if b in (10, 13):  # \n / \r
                if self.buf:
                    try:
                        lines.append(self.buf.decode("utf-8"))
                    except UnicodeDecodeError:
                        pass
                    self.buf.clear()
            else:
                self.buf.append(b)
        return lines


class FakeBridge:
    def __init__(self, ser: serial.Serial, args):
        self.s = ser
        self.args = args
        self.reader = LineReader()
        self.pending_prompt_id: Optional[str] = None
        self.last_decision: Optional[str] = None
        self.tokens = 100_000
        self.tokens_today = 24_000
        self.stop = False

    def send(self, obj):
        line = (json.dumps(obj) + "\n").encode("utf-8")
        # Chunk writes to 64 B (USB-CDC packet boundary) with a tiny pause
        # between chunks. The ESP32-S3 native USB-CDC RX ring buffer is
        # small (~256 B); without pacing, anything bigger than that loses
        # tail bytes — the device sees a broken JSON line and drops it.
        # Symptom before this fix: short prompts arrived, anything >~250 B
        # silently never showed up at the device.
        CHUNK = 64
        try:
            for off in range(0, len(line), CHUNK):
                self.s.write(line[off:off + CHUNK])
                self.s.flush()
                if off + CHUNK < len(line):
                    time.sleep(0.005)   # 5 ms gives the device's loop one tick
        except serial.SerialException as e:
            print(f"[bridge] serial write failed: {e}")
            self.stop = True

    def reader_thread(self):
        """Pump device → host: print everything, latch permission decisions."""
        while not self.stop:
            try:
                data = self.s.read(128)
                if not data:
                    continue
                for line in self.reader.feed(data):
                    print(f"[device] {line}")
                    if line.startswith("{"):
                        try:
                            msg = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        if msg.get("cmd") == "permission":
                            pid = msg.get("id")
                            dec = msg.get("decision")
                            if pid and pid == self.pending_prompt_id:
                                self.last_decision = dec
                                self.pending_prompt_id = None
            except serial.SerialException as e:
                if not self.stop:
                    print(f"[reader] {e}")
                return

    def time_sync(self):
        tz_east = -time.timezone if not time.daylight else -time.altzone
        self.send({"time": [int(time.time()), int(tz_east)]})

    def owner(self, name):
        self.send({"cmd": "owner", "name": name})

    def heartbeat(self, **fields):
        msg = {"tokens": self.tokens, "tokens_today": self.tokens_today}
        msg.update(fields)
        self.send(msg)

    # ── scenarios (parity with ble_test_bridge.py) ─────────────────────────
    def scenario_idle(self):
        while not self.stop:
            self.heartbeat(
                total=1, running=0, waiting=0,
                msg="1 session idle",
                entries=["ready"],
            )
            time.sleep(self.args.interval)

    def scenario_working(self):
        i = 0
        msgs = ["reading repo", "running tests", "drafting PR", "thinking"]
        while not self.stop:
            self.tokens += 800
            self.tokens_today += 800
            entries = [f"line {max(0, i-2)}", f"line {max(0, i-1)}", f"line {i}"]
            self.heartbeat(
                total=3, running=3, waiting=0,
                msg=msgs[i % len(msgs)],
                entries=entries,
            )
            i += 1
            time.sleep(self.args.interval)

    def scenario_prompt(self):
        i = 0
        round_num = 0
        tools = ["Bash", "Read", "Write", "Edit"]
        hints = [
            "rm -rf /tmp/foo",
            "git push origin main",
            "drop table users;",
            "echo hello",
        ]
        while not self.stop:
            round_num += 1
            for _ in range(5):
                if self.stop: return
                self.tokens += 600
                self.tokens_today += 600
                self.heartbeat(
                    total=2, running=2, waiting=0,
                    msg=f"working {i}",
                    entries=[f"step {i}"],
                )
                i += 1
                time.sleep(self.args.interval)

            tool = tools[round_num % len(tools)]
            hint = hints[round_num % len(hints)]
            pid = f"req_{int(time.time()*1000) & 0xFFFFFF:06x}"
            self.pending_prompt_id = pid
            self.last_decision = None
            print(f"\n[bridge] ── INJECT PROMPT {pid}  tool={tool}  hint={hint!r}")
            self.heartbeat(
                total=2, running=1, waiting=1,
                msg=f"approve: {tool}",
                entries=[f"step {i}"],
                prompt={"id": pid, "tool": tool, "hint": hint},
            )

            t0 = time.time()
            while self.pending_prompt_id and time.time() - t0 < 30 and not self.stop:
                time.sleep(0.4)
            if self.pending_prompt_id:
                print("[bridge] prompt timed out (no decision in 30s)\n")
                self.pending_prompt_id = None
            else:
                print(f"[bridge] decided: {self.last_decision}\n")

            self.heartbeat(
                total=2, running=0, waiting=0, completed=True, msg="done",
            )
            time.sleep(self.args.interval)

    def scenario_levelup(self):
        """+10K tokens per heartbeat. 50K per level → CELEBRATE every ~10s."""
        i = 0
        while not self.stop:
            self.tokens += 10_000
            self.tokens_today += 10_000
            self.heartbeat(
                total=2, running=2, waiting=0,
                msg=f"farming tokens {i}",
                entries=[f"+10K  total={self.tokens}"],
            )
            i += 1
            time.sleep(self.args.interval)

    def scenario_long_hint(self):
        """Stress word-wrap + tool-name auto-sizing."""
        cases = [
            ("ShortTool",
             "rm -rf /tmp/foo"),
            ("MediumLengthTool",
             "git push origin main --force-with-lease"),
            ("VeryLongToolNameOverflow",
             "rsync -avz --delete /src/ user@host:/dst/ && echo done && "
             "tail -f /var/log/syslog | grep -i error"),
            ("Bash",
             "find . -name '*.tmp' -mtime +7 -exec rm {} \\; "
             "&& du -sh /var/cache/* | sort -h | tail -20"),
        ]
        i = 0
        for _ in range(3):
            if self.stop: return
            self.heartbeat(total=1, running=1, waiting=0, msg="warming up",
                           entries=["short"])
            time.sleep(self.args.interval)

        for tool, hint in cases:
            if self.stop: return
            pid = f"req_{int(time.time()*1000) & 0xFFFFFF:06x}"
            self.pending_prompt_id = pid
            self.last_decision = None
            print(f"\n[bridge] ── INJECT  tool={tool!r}  hint={hint!r}")
            self.heartbeat(
                total=1, running=0, waiting=1,
                msg=f"approve: {tool}",
                entries=[f"len(tool)={len(tool)}  len(hint)={len(hint)}"],
                prompt={"id": pid, "tool": tool, "hint": hint},
            )
            t0 = time.time()
            while self.pending_prompt_id and time.time() - t0 < 60 and not self.stop:
                time.sleep(0.4)
            print(f"[bridge] decided: {self.last_decision or 'TIMEOUT'}\n")
            self.pending_prompt_id = None
            self.heartbeat(total=1, running=0, waiting=0, msg="done",
                           entries=[f"case {i+1}/{len(cases)} done"])
            i += 1
            time.sleep(self.args.interval)
        print("[bridge] long_hint scenario done — Ctrl-C to exit")
        while not self.stop:
            time.sleep(0.5)

    def scenario_cyrillic(self):
        """Cyrillic + emoji in every text field → tests _asciiCopy strip."""
        msgs = ["читаю репо", "тести бігають ⚙", "пишу PR ✍", "думаю 🤔"]
        entries_pool = [
            "10:42 коміт у main гілку",
            "10:43 yarn тестує модулі",
            "10:44 reading code 📖",
            "10:45 пишу assistant'у",
            "Привіт, світе! 🌍",
        ]
        i = 0
        round_num = 0
        while not self.stop:
            round_num += 1
            for _ in range(4):
                if self.stop: return
                self.tokens += 400
                self.tokens_today += 400
                self.heartbeat(
                    total=2, running=2, waiting=0,
                    msg=msgs[i % len(msgs)],
                    entries=[entries_pool[(i + k) % len(entries_pool)] for k in range(3)],
                )
                i += 1
                time.sleep(self.args.interval)

            pid = f"req_cy_{int(time.time()) & 0xFFFFFF:06x}"
            self.pending_prompt_id = pid
            self.last_decision = None
            print(f"\n[bridge] ── INJECT cyrillic prompt {pid}")
            self.heartbeat(
                total=2, running=1, waiting=1,
                msg="approve: Башик",
                entries=["видалити ➔ /tmp/тест"],
                prompt={"id": pid, "tool": "ВидалитиФайл",
                        "hint": "rm -rf /tmp/тестова_папка з пробілами 🔥"},
            )
            t0 = time.time()
            while self.pending_prompt_id and time.time() - t0 < 30 and not self.stop:
                time.sleep(0.4)
            print(f"[bridge] decided: {self.last_decision or 'TIMEOUT'}\n")
            self.pending_prompt_id = None

    def scenario_cycle(self):
        round_num = 0
        i = 0
        while not self.stop:
            round_num += 1
            print(f"\n[bridge] ── round {round_num}: idle ─────────────")
            for _ in range(5):
                if self.stop: return
                self.heartbeat(
                    total=1, running=0, waiting=0,
                    msg="1 session idle",
                    entries=["ready"],
                )
                time.sleep(self.args.interval)

            print(f"[bridge] ── round {round_num}: working ──────────")
            for k in range(8):
                if self.stop: return
                self.tokens += 500
                self.tokens_today += 500
                self.heartbeat(
                    total=3, running=3, waiting=0,
                    msg=f"working {k}",
                    entries=[f"line {k-1}", f"line {k}"],
                )
                i += 1
                time.sleep(self.args.interval)

            print(f"[bridge] ── round {round_num}: prompt ───────────")
            pid = f"req_cycle_{int(time.time()) & 0xFFFFFF:06x}"
            self.pending_prompt_id = pid
            self.last_decision = None
            self.heartbeat(
                total=2, running=1, waiting=1,
                msg="approve: Bash",
                prompt={"id": pid, "tool": "Bash", "hint": "git push origin main"},
            )
            t0 = time.time()
            while self.pending_prompt_id and time.time() - t0 < 30 and not self.stop:
                time.sleep(0.4)
            print(f"[bridge] decided: {self.last_decision or 'TIMEOUT'}")
            self.pending_prompt_id = None

            print(f"[bridge] ── round {round_num}: completed ────────")
            self.heartbeat(
                total=2, running=0, waiting=0, completed=True, msg="done",
            )
            time.sleep(self.args.interval)


def main():
    ap = argparse.ArgumentParser(
        description="Fake Claude Desktop bridge over USB-CDC for buddy firmware.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See module docstring for setup details. "
               "Cannot run alongside `make monitor` — it holds the port.",
    )
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--scenario", default="cycle",
                    choices=["idle", "working", "prompt", "cycle",
                             "levelup", "long_hint", "cyrillic"])
    ap.add_argument("--owner", default="LinuxDev")
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    try:
        # dsrdtr/rtscts off so opening the port doesn't drop DTR and reset
        # the board (ESP32-S3 native USB-CDC ignores DTR anyway, but
        # belt-and-braces costs nothing).
        ser = serial.Serial(
            args.port, args.baud, timeout=0.1,
            dsrdtr=False, rtscts=False,
        )
    except serial.SerialException as e:
        print(f"failed to open {args.port}: {e}")
        if "Permission denied" in str(e):
            print("hint: add yourself to dialout group:\n"
                  "  sudo usermod -aG dialout $USER  &&  newgrp dialout")
        elif "Device or resource busy" in str(e):
            print("hint: `make monitor` is probably holding the port — close it.")
        return 1

    bridge = FakeBridge(ser, args)
    rt = threading.Thread(target=bridge.reader_thread, daemon=True)
    rt.start()

    print(f"connected: {args.port} @ {args.baud}")
    print("sending time sync + owner…")
    bridge.time_sync()
    bridge.owner(args.owner)

    scenarios = {
        "idle":      bridge.scenario_idle,
        "working":   bridge.scenario_working,
        "prompt":    bridge.scenario_prompt,
        "cycle":     bridge.scenario_cycle,
        "levelup":   bridge.scenario_levelup,
        "long_hint": bridge.scenario_long_hint,
        "cyrillic":  bridge.scenario_cyrillic,
    }
    print(f"scenario: {args.scenario}  (Ctrl-C to stop)\n")
    try:
        scenarios[args.scenario]()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        bridge.stop = True
        try:
            ser.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
