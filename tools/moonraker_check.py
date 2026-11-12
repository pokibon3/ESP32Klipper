#!/usr/bin/env python3
"""Checks the Moonraker-compatible API like Mainsail uses it on connect.

    python3 tools/moonraker_check.py [host:port]

Uses only the standard library (a minimal WebSocket client).  Prints each
call's result summary and exits non-zero on a failure.
"""
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request


class WS:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((
            "GET /websocket HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n" % (host, port, key)).encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(4096)
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError("handshake failed: %r" % head)
        self.next_id = 1
        self.notifications = []

    def send(self, obj):
        data = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            hdr = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.s.sendall(hdr + mask + masked)

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        b0, b1 = self._recv_exact(2)
        n = b1 & 0x7f
        if n == 126:
            n = struct.unpack("!H", self._recv_exact(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._recv_exact(8))[0]
        return json.loads(self._recv_exact(n))

    def call(self, method, params=None, timeout=10.):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "method": method, "id": rid}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv()
            if m.get("id") == rid:
                return m
            self.notifications.append(m)
        raise RuntimeError("no reply to " + method)


def main():
    addr = sys.argv[1] if len(sys.argv) > 1 else "localhost:7125"
    host, port = addr.split(":")
    port = int(port)
    ws = WS(host, port)
    failures = 0

    def check(method, params=None, expect_error=False, show=None):
        nonlocal failures
        r = ws.call(method, params)
        bad = ("error" in r) != expect_error
        failures += bad
        res = r.get("result", r.get("error"))
        text = show(res) if show and "result" in r else json.dumps(res)[:100]
        print("%-34s %s  %s" % (method, "FAIL" if bad else "ok", text))
        return res

    # Mainsail's connect sequence (roughly in its order)
    check("server.connection.identify",
          {"client_name": "check", "version": "1", "type": "web",
           "url": "local"})
    check("server.info", show=lambda r: "klippy_state=" + r["klippy_state"])
    check("server.config", show=lambda r: ",".join(r["config"]))
    check("machine.system_info",
          show=lambda r: r["system_info"]["cpu_info"]["processor"])
    check("server.database.list", show=lambda r: str(r["namespaces"]))
    check("server.database.get_item", {"namespace": "mainsail"},
          expect_error=True)
    check("server.database.post_item",
          {"namespace": "mainsail", "key": "general.printername",
           "value": "plotter"})
    check("server.database.get_item", {"namespace": "mainsail"},
          show=lambda r: json.dumps(r["value"]))
    check("printer.info", show=lambda r: r["state"] + ": " + r["state_message"])
    objs = check("printer.objects.list",
                 show=lambda r: "%d objects" % len(r["objects"]))
    sub = {o: None for o in objs["objects"]}
    st = check("printer.objects.subscribe", {"objects": sub},
               show=lambda r: ", ".join(sorted(r["status"])))
    s = st["status"]
    print("  toolhead.axis_maximum", s["toolhead"]["axis_maximum"],
          "homed", repr(s["toolhead"]["homed_axes"]))
    print("  configfile sections:", len(s["configfile"]["settings"]),
          "kinematics", s["configfile"]["settings"]["printer"]["kinematics"])
    check("server.files.roots", show=lambda r: str([x["name"] for x in r]))
    check("server.files.get_directory", {"path": "gcodes", "extended": True},
          show=lambda r: str([f["filename"] for f in r["files"]]))
    check("server.files.list", {"root": "config"},
          show=lambda r: str([f["path"] for f in r]))
    check("server.history.list", {"limit": 50})
    check("server.temperature_store")
    check("printer.gcode.help", show=lambda r: "%d commands" % len(r))

    # Upload a file over HTTP, home, print it, watch status updates
    body_file = b"G90\nG1 X20 Y20 F6000\nSET_PIN PIN=LASER VALUE=1\n" \
        b"G1 X60 Y20\nG1 X60 Y60\nG1 X20 Y60\nG1 X20 Y20\n" \
        b"SET_PIN PIN=LASER VALUE=0\n"
    boundary = "----check%d" % int(time.time())
    data = ("--%s\r\nContent-Disposition: form-data; name=\"file\"; "
            "filename=\"check_square.gcode\"\r\nContent-Type: "
            "application/octet-stream\r\n\r\n" % boundary).encode() + body_file \
        + ("\r\n--%s\r\nContent-Disposition: form-data; name=\"root\"\r\n\r\n"
           "gcodes\r\n--%s--\r\n" % (boundary, boundary)).encode()
    req = urllib.request.Request(
        "http://%s:%d/server/files/upload" % (host, port), data=data,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
    with urllib.request.urlopen(req, timeout=10) as r:
        up = json.load(r)
    print("%-34s %s  %s" % ("HTTP upload", "ok", up["item"]["path"]))
    with urllib.request.urlopen("http://%s:%d/server/files/gcodes/"
                                "check_square.gcode" % (host, port)) as r:
        same = r.read() == body_file
    failures += not same
    print("%-34s %s" % ("HTTP download", "ok" if same else "FAIL"))
    with urllib.request.urlopen("http://%s:%d/printer/objects/query?"
                                "webhooks&toolhead=homed_axes" % (host, port)) as r:
        q = json.load(r)["result"]["status"]
    print("%-34s %s  %s" % ("HTTP objects/query", "ok", json.dumps(q)))

    check("printer.gcode.script", {"script": "G28 X Y"})
    check("printer.print.start", {"filename": "check_square.gcode"})
    end = time.time() + 30
    states, positions = set(), 0
    while time.time() < end:
        m = ws.recv()
        if m.get("method") != "notify_status_update":
            if m.get("method"):
                ws.notifications.append(m)
            continue
        upd = m["params"][0]
        ps = upd.get("print_stats", {}).get("state")
        if ps:
            states.add(ps)
            print("  print_stats.state ->", ps)
        if "motion_report" in upd:
            positions += 1
        if ps == "complete":
            break
    done = "complete" in states
    failures += not done
    print("%-34s %s  live position updates %d" %
          ("print via API", "ok" if done else "FAIL", positions))
    hist = check("server.history.list",
                 show=lambda r: "%d jobs, last %s" % (
                     r["count"], r["jobs"][0]["status"] if r["jobs"] else "-"))
    kinds = sorted(set(n.get("method") for n in ws.notifications))
    print("notifications seen:", ", ".join(kinds))
    check("server.files.delete_file", {"path": "gcodes/check_square.gcode"})
    check("printer.unknown_method", expect_error=True)
    print("FAILURES:", failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
