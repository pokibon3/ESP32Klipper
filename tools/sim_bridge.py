#!/usr/bin/env python3
"""Connect an ESP32 board's console UART to the simulated Klipper MCU.

    python tools/sim_bridge.py /dev/cu.usbserial-XXXX [--sim build/sim_mcu/sim_mcu]

The link_test firmware prints "@@LINK_START@@", then speaks the Klipper
protocol on the same UART (250000 baud) until "@@LINK_END@@", then prints
its report.  This script resets the board, shows the console text, starts
the simulated MCU at LINK_START and relays bytes in both directions until
LINK_END, then prints the rest of the report.

This file may be distributed under the terms of the GNU GPLv3 license.
"""
import argparse
import os
import selectors
import socket
import subprocess
import sys
import time

import serial  # pyserial (included in the ESP-IDF Python environment)

START = b"@@LINK_START@@"
END = b"@@LINK_END@@"
DONE = b"link test done"
# Text that only appears when the board crashed or rebooted during the link
CRASH_MARKS = [b"Guru Meditation", b"Backtrace:", b"rst:0x", b"abort() was",
               b"Stack canary", b"***ERROR***"]


def print_text(data, out):
    text = data.decode("utf-8", "replace").replace("\r", "")
    sys.stdout.write(text)
    sys.stdout.flush()
    out.write(text)
    out.flush()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--sim", default=os.path.join(root, "build", "sim_mcu",
                                                  "sim_mcu"))
    ap.add_argument("--baud", type=int, default=250000)
    ap.add_argument("--log", default=os.path.join(root, "build",
                                                  "link_test_result.txt"))
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--trace", default="",
                    help="write the simulated MCU's output pin changes to "
                         "this file (tools/plot_view.py draws them)")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0)
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    out = open(args.log, "w", encoding="utf-8")
    deadline = time.time() + args.timeout

    # Phase 1: console text until LINK_START
    buf = b""
    while START not in buf:
        if time.time() > deadline:
            print("\ntimeout waiting for %s" % START.decode(), file=sys.stderr)
            return 1
        data = ser.read(4096)
        if not data:
            time.sleep(0.01)
            continue
        buf += data
        if START not in buf and b"\n" in buf:
            line, buf = buf.rsplit(b"\n", 1)
            print_text(line + b"\n", out)
    before, after = buf.split(START, 1)
    print_text(before + START + b"\n", out)
    after = after.lstrip(b"\r\n")

    # Phase 2: relay between the UART and the simulated MCU
    host_end, sim_end = socket.socketpair()
    env = dict(os.environ)
    if args.trace:
        open(args.trace, "wb").close() # new trace for this link
        env["SIM_TRACE"] = os.path.abspath(args.trace)
    sim = subprocess.Popen([args.sim, "-F", str(sim_end.fileno())],
                           pass_fds=[sim_end.fileno()],
                           stderr=subprocess.PIPE, env=env)
    sim_end.close()
    host_end.setblocking(False)
    sel = selectors.DefaultSelector()
    sel.register(host_end, selectors.EVENT_READ)
    to_mcu = after
    tail = b""
    raw = open(args.log + ".raw", "wb")  # everything the board sent
    crash = None
    tx = rx = 0
    report = b""
    while True:
        if time.time() > deadline:
            print("\ntimeout during link", file=sys.stderr)
            break
        if to_mcu:
            host_end.sendall(to_mcu)
            tx += len(to_mcu)
            to_mcu = b""
        for key, _ in sel.select(timeout=0.001):
            data = host_end.recv(4096)
            if data:
                ser.write(data)
                rx += len(data)
        data = ser.read(4096)
        if data:
            raw.write(data)
            scan = tail + data
            hit = [m for m in CRASH_MARKS if m in scan]
            if hit:
                # The board crashed: stop relaying and show its output
                crash = scan[scan.find(hit[0]):]
                deadline2 = time.time() + 2.
                while time.time() < deadline2:
                    more = ser.read(4096)
                    if more:
                        crash += more
                        raw.write(more)
                    else:
                        time.sleep(0.02)
                break
            if END in scan:
                pre, report = scan.split(END, 1)
                to_mcu = pre[len(tail):]
                if to_mcu:
                    host_end.sendall(to_mcu)
                break
            to_mcu = data
            tail = scan[-len(END):]
    raw.close()
    if crash is not None:
        print_text(b"\n!! board crashed during the link:\n", out)
        print_text(crash, out)
        sim.terminate()
        return 1
    print_text(b"%s (relayed %d bytes to the MCU, %d bytes back)\n"
               % (END, tx, rx), out)
    sim.terminate()
    try:
        _, err = sim.communicate(timeout=2)
        print_text(err, out)
    except subprocess.TimeoutExpired:
        sim.kill()

    # Phase 3: report text
    buf = report
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            buf += data
        elif DONE in buf and buf.endswith(b"\n"):
            break
        else:
            time.sleep(0.02)
    print_text(buf.lstrip(b"\r\n"), out)
    return 0 if b"PASS" in buf else 1


if __name__ == "__main__":
    sys.exit(main())
