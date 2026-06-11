#!/usr/bin/env python3
"""
ble_test_bridge.py — fake Claude Desktop bridge over BLE for buddy firmware.

Acts as a Nordic UART client: scans for "Claude-XXXX", connects, and runs
one of several scripted scenarios so you can exercise the device's
heartbeat, prompt approval, and stats flows without the macOS/Windows
desktop app. Useful on Linux where the real desktop isn't available.

Setup (one-time):
  python3 -m venv tools/.venv
  source tools/.venv/bin/activate
  pip install bleak

Usage:
  tools/ble_test_bridge.py --list                     # discover devices
  tools/ble_test_bridge.py --scenario idle
  tools/ble_test_bridge.py --scenario working
  tools/ble_test_bridge.py --scenario prompt          # inject approval prompts
  tools/ble_test_bridge.py --scenario cycle           # full demo loop

Filter to a specific device by prefix (handy if more than one is in range):
  tools/ble_test_bridge.py --device Claude-0C89 --scenario prompt
"""

import argparse
import asyncio
import json
import time
from typing import Optional

from bleak import BleakClient, BleakScanner

# Nordic UART Service — matches src/ble_bridge_nimble.cpp
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # desktop → device (write)
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device → desktop (notify)


class LineReader:
    """Reassemble newline-delimited JSON from BLE notify fragments."""

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
    def __init__(self, client: BleakClient, args):
        self.c = client
        self.args = args
        self.reader = LineReader()
        self.pending_prompt_id: Optional[str] = None
        self.last_decision: Optional[str] = None
        self.tokens = 100_000
        self.tokens_today = 24_000

    async def send(self, obj):
        line = (json.dumps(obj) + "\n").encode("utf-8")
        await self.c.write_gatt_char(NUS_RX_UUID, line, response=False)

    def on_notify(self, _sender, data: bytearray):
        for line in self.reader.feed(bytes(data)):
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                print(f"[recv] non-json: {line!r}")
                continue
            print(f"[recv] {msg}")
            if msg.get("cmd") == "permission":
                pid = msg.get("id")
                dec = msg.get("decision")
                if pid and pid == self.pending_prompt_id:
                    self.last_decision = dec
                    self.pending_prompt_id = None

    async def time_sync(self):
        # The device wants seconds EAST of UTC. tm_gmtoff already is that,
        # for the *current* moment — so DST is accounted for only when it's
        # actually in effect (time.daylight merely means the zone HAS DST
        # rules).
        tz_east = time.localtime().tm_gmtoff
        await self.send({"time": [int(time.time()), int(tz_east)]})

    async def owner(self, name):
        await self.send({"cmd": "owner", "name": name})

    async def heartbeat(self, **fields):
        msg = {"tokens": self.tokens, "tokens_today": self.tokens_today}
        msg.update(fields)
        await self.send(msg)

    # ── scenarios ──────────────────────────────────────────────────────────
    async def scenario_idle(self):
        while True:
            await self.heartbeat(
                total=1, running=0, waiting=0,
                msg="1 session idle",
                entries=["ready"],
            )
            await asyncio.sleep(self.args.interval)

    async def scenario_working(self):
        i = 0
        msgs = ["reading repo", "running tests", "drafting PR", "thinking"]
        history = []
        while True:
            self.tokens += 800
            self.tokens_today += 800
            history.append(f"line {i}: {msgs[i % len(msgs)]}")
            # Roll a longer backlog so the full-screen log has history to
            # scroll. Device keeps the last TAMA_MAX_LINES (16); send a few
            # under that.
            entries = history[-14:]
            await self.heartbeat(
                total=3, running=3, waiting=0,
                msg=msgs[i % len(msgs)],
                entries=entries,
            )
            i += 1
            await asyncio.sleep(self.args.interval)

    async def scenario_prompt(self):
        """Cycle: 5 working ticks → inject prompt → wait → continue."""
        i = 0
        round_num = 0
        tools = ["Bash", "Read", "Write", "Edit"]
        hints = [
            "rm -rf /tmp/foo",
            "git push origin main",
            "drop table users;",
            "echo hello",
        ]
        while True:
            round_num += 1
            for _ in range(5):
                self.tokens += 600
                self.tokens_today += 600
                await self.heartbeat(
                    total=2, running=2, waiting=0,
                    msg=f"working {i}",
                    entries=[f"step {i}"],
                )
                i += 1
                await asyncio.sleep(self.args.interval)

            tool = tools[round_num % len(tools)]
            hint = hints[round_num % len(hints)]
            pid = f"req_{int(time.time()*1000) & 0xFFFFFF:06x}"
            self.pending_prompt_id = pid
            self.last_decision = None
            print(f"\n[bridge] ── INJECT PROMPT {pid}  tool={tool}  hint={hint!r}")
            await self.heartbeat(
                total=2, running=1, waiting=1,
                msg=f"approve: {tool}",
                entries=[f"step {i}"],
                prompt={"id": pid, "tool": tool, "hint": hint},
            )

            t0 = time.time()
            while self.pending_prompt_id and time.time() - t0 < 30:
                await asyncio.sleep(0.4)
            if self.pending_prompt_id:
                print("[bridge] prompt timed out (no decision in 30s)\n")
                self.pending_prompt_id = None
            else:
                print(f"[bridge] decided: {self.last_decision}\n")

            await self.heartbeat(
                total=2, running=0, waiting=0, completed=True, msg="done",
            )
            await asyncio.sleep(self.args.interval)

    async def scenario_cycle(self):
        """idle → working → prompt → completed → repeat."""
        round_num = 0
        i = 0
        while True:
            round_num += 1
            print(f"\n[bridge] ── round {round_num}: idle ─────────────")
            for _ in range(5):
                await self.heartbeat(
                    total=1, running=0, waiting=0,
                    msg="1 session idle",
                    entries=["ready"],
                )
                await asyncio.sleep(self.args.interval)

            print(f"[bridge] ── round {round_num}: working ──────────")
            for k in range(8):
                self.tokens += 500
                self.tokens_today += 500
                await self.heartbeat(
                    total=3, running=3, waiting=0,
                    msg=f"working {k}",
                    entries=[f"line {k-1}", f"line {k}"],
                )
                i += 1
                await asyncio.sleep(self.args.interval)

            print(f"[bridge] ── round {round_num}: prompt ───────────")
            pid = f"req_cycle_{int(time.time()) & 0xFFFFFF:06x}"
            self.pending_prompt_id = pid
            self.last_decision = None
            await self.heartbeat(
                total=2, running=1, waiting=1,
                msg="approve: Bash",
                prompt={"id": pid, "tool": "Bash", "hint": "git push origin main"},
            )
            t0 = time.time()
            while self.pending_prompt_id and time.time() - t0 < 30:
                await asyncio.sleep(0.4)
            print(f"[bridge] decided: {self.last_decision or 'TIMEOUT'}")
            self.pending_prompt_id = None

            print(f"[bridge] ── round {round_num}: completed ────────")
            await self.heartbeat(
                total=2, running=0, waiting=0, completed=True, msg="done",
            )
            await asyncio.sleep(self.args.interval)


async def list_devices(timeout: float):
    print(f"scanning for {timeout}s…")
    devices = await BleakScanner.discover(timeout=timeout)
    if not devices:
        print("nothing found")
        return 0
    for d in devices:
        name = d.name or "(no name)"
        star = "★" if "Claude" in name else " "
        print(f"  {star} {d.address}  {name}")
    return 0


async def run(args):
    print(f"scanning for name~='{args.device}'…")
    target = None
    devices = await BleakScanner.discover(timeout=args.scan_timeout)
    for d in devices:
        if d.name and d.name.startswith(args.device):
            target = d
            break
    if not target:
        print(f"no device matching '{args.device}' in range; try --list")
        return 1
    print(f"connecting to {target.address}  '{target.name}'…")

    async with BleakClient(target.address) as client:
        bridge = FakeBridge(client, args)
        await client.start_notify(NUS_TX_UUID, bridge.on_notify)
        print("connected. sending time sync + owner…")
        await bridge.time_sync()
        await bridge.owner(args.owner)

        scenarios = {
            "idle":    bridge.scenario_idle,
            "working": bridge.scenario_working,
            "prompt":  bridge.scenario_prompt,
            "cycle":   bridge.scenario_cycle,
        }
        print(f"running scenario: {args.scenario}  (Ctrl-C to stop)\n")
        try:
            await scenarios[args.scenario]()
        finally:
            try:
                await client.stop_notify(NUS_TX_UUID)
            except Exception:
                pass
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Fake Claude Desktop BLE bridge for buddy firmware tests.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See module docstring for setup details.",
    )
    ap.add_argument("--scenario", default="cycle",
                    choices=["idle", "working", "prompt", "cycle"])
    ap.add_argument("--device", default="Claude",
                    help="prefix-match device name (default: Claude)")
    ap.add_argument("--owner", default="LinuxDev",
                    help="owner name to push on connect")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="heartbeat interval in seconds (default: 2.0)")
    ap.add_argument("--scan-timeout", type=float, default=8.0)
    ap.add_argument("--list", action="store_true",
                    help="list discovered BLE devices and exit")
    args = ap.parse_args()

    try:
        if args.list:
            return asyncio.run(list_devices(args.scan_timeout))
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nbye")
        return 0


if __name__ == "__main__":
    raise SystemExit(main() or 0)
