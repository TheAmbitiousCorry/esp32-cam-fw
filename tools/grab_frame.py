#!/usr/bin/env python3
"""Pull one frame off the board over serial and save it as a JPEG.

Run with PlatformIO's Python, which already has pyserial:
  ~/.platformio/penv/bin/python tools/grab_frame.py [port] [outfile]
"""
import base64
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
outfile = sys.argv[2] if len(sys.argv) > 2 else "frame.jpg"

ser = serial.Serial(port, 115200, timeout=0.2)

# Opening the port asserts DTR and RTS, which the MB's auto-reset circuit turns
# into a board reset. Rather than fight that, drive the reset deliberately and
# wait for the sketch to announce itself, so 'p' cannot land mid-boot.
ser.dtr = False
ser.rts = False
time.sleep(0.2)
ser.rts = True
time.sleep(0.1)
ser.reset_input_buffer()
ser.rts = False

banner, deadline = "", time.time() + 10
while time.time() < deadline and "ready." not in banner:
    banner += ser.read(4096).decode("utf-8", "replace")
if "ready." not in banner:
    sys.exit("sketch never reported ready. Is the right firmware flashed?")

ser.write(b"p")

chunks, capturing, pending = [], False, ""
deadline = time.time() + 30
while time.time() < deadline:
    pending += ser.read(4096).decode("ascii", "replace")
    lines = pending.split("\n")
    pending = lines.pop()
    for line in lines:
        line = line.strip()
        if line.startswith("---BEGIN JPEG"):
            capturing = True
        elif line.startswith("---END JPEG"):
            deadline = 0
            break
        elif capturing and line:
            chunks.append(line)

ser.dtr = False
ser.rts = False
ser.close()

if not chunks:
    sys.exit("no frame data received")

data = base64.b64decode("".join(chunks))
with open(outfile, "wb") as f:
    f.write(data)

intact = data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"
print("wrote %s, %d bytes, %s" % (outfile, len(data), "valid" if intact else "CORRUPT"))
