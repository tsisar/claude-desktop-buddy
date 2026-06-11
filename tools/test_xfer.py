#!/usr/bin/env python3
"""
Prove the Stick's serial file receiver works: stream an existing character
back over serial, watch the acks, verify it reloads.
"""
import sys, json, base64, time, glob, os, serial

CHUNK = 192   # raw bytes per chunk (~256B base64) — same as usb_xfer_send.py
# ESP32-S3 native USB-CDC enumerates as /dev/ttyACM* on Linux — same port
# usb_xfer_send.py / usb_test_bridge.py default to (/dev/ttyACM0).
PORT = (sorted(glob.glob('/dev/ttyACM*')) + ['/dev/ttyACM0'])[0]

# dsrdtr/rtscts off so opening the port doesn't toggle DTR (matches the
# bridges; native USB-CDC doesn't reset on DTR, so no boot wait needed).
s = serial.Serial(PORT, 115200, timeout=2, dsrdtr=False, rtscts=False)
time.sleep(0.2)   # let any stale boot log drain
s.reset_input_buffer()

def send(obj):
    # 64-byte chunks with a 5ms gap — same pacing as usb_xfer_send.py;
    # the ESP32-S3 USB-CDC RX ring buffer is small (~256B) and whole-line
    # writes lose tail bytes.
    line = (json.dumps(obj) + "\n").encode()
    for off in range(0, len(line), 64):
        s.write(line[off:off+64])
        s.flush()
        if off + 64 < len(line):
            time.sleep(0.005)

def wait_ack(what, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = s.readline().decode('utf-8', errors='replace').strip()
        if not line: continue
        if line.startswith('{'):
            try:
                a = json.loads(line)
                if a.get('ack') == what:
                    return a
            except: pass
        else:
            print(f"  (skip: {line[:60]})", file=sys.stderr)
    return None

def send_file(name, path):
    data = open(path, 'rb').read()
    print(f"  {name}: {len(data)} bytes", end='', flush=True)
    send({"cmd":"file", "path":name, "size":len(data)})
    a = wait_ack("file")
    if not a or not a.get('ok'): print(" — open FAILED"); return False

    for i in range(0, len(data), CHUNK):
        chunk = data[i:i+CHUNK]
        send({"cmd":"chunk", "d": base64.b64encode(chunk).decode()})
        a = wait_ack("chunk", timeout=3)
        if not a or not a.get('ok'):
            print(f" — chunk {i} FAILED"); return False
        if i and i % 16384 == 0: print(".", end="", flush=True)

    send({"cmd":"file_end"})
    a = wait_ack("file_end", timeout=10)
    ok = a and a.get('ok') and a.get('n') == len(data)
    print(f" — {'ok' if ok else 'FAILED'} ({a.get('n') if a else '?'} written)")
    return ok

src = sys.argv[1] if len(sys.argv) > 1 else f"{os.path.dirname(__file__)}/../characters/bufo"
name = sys.argv[2] if len(sys.argv) > 2 else "test"

files = sorted(glob.glob(f"{src}/*"))
total = sum(os.path.getsize(f) for f in files)

print(f"installing '{name}' from {src}")
print("waiting for device...", end='', flush=True)
for attempt in range(8):
    s.reset_input_buffer()
    # char_begin carries the total byte count — the firmware uses it for
    # the storage fit-check before wiping /characters/ (src/xfer.h).
    send({"cmd":"char_begin", "name":name, "total":total})
    a = wait_ack("char_begin", timeout=2)
    if a and a.get('ok'):
        print(" ready")
        break
    print(".", end='', flush=True)
    time.sleep(1)
else:
    sys.exit("\nchar_begin: device never responded")

t0 = time.time()
print(f"{len(files)} files, {total} bytes total")

for f in files:
    if not send_file(os.path.basename(f), f):
        sys.exit("transfer failed")

send({"cmd":"char_end"})
a = wait_ack("char_end", timeout=10)
dt = time.time() - t0
print(f"\nchar_end: {a}")
print(f"{total} bytes in {dt:.1f}s = {total/dt/1024:.1f} KB/s")
s.close()
if not (a and a.get('ok')):
    # Firmware runs characterInit on char_end — ok:false means the pack
    # didn't load (bad manifest.json?) and /characters/ was already wiped.
    sys.exit("char_end FAILED: pack did not load — check manifest.json")
