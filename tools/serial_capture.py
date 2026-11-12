#!/usr/bin/env python3
"""Reset an ESP32 board and capture its serial output to a file.

Used to collect M1 benchmark results:

    python tools/serial_capture.py /dev/cu.usbserial-XXXX build/esp32/bench_result.txt

The board is reset through RTS (EN) with DTR (IO0) released, as the usual
ESP32 auto-reset circuit expects.  Capture stops when the line
"M1 benchmark done" arrives or after --timeout seconds.

This file may be distributed under the terms of the GNU GPLv3 license.
"""
import argparse
import sys
import time

import serial  # pyserial (included in the ESP-IDF Python environment)

DONE_MARK = "M1 benchmark done"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("outfile")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--no-reset", action="store_true",
                    help="do not reset the board (USB Serial/JTAG boards)")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.5)
    if not args.no_reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    deadline = time.time() + args.timeout
    with open(args.outfile, "w", encoding="utf-8") as out:
        buf = b""
        while time.time() < deadline:
            buf += ser.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                print(text, flush=True)
                out.write(text + "\n")
                out.flush()
                if DONE_MARK in text:
                    return 0
    print("timeout: '%s' not seen" % DONE_MARK, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
