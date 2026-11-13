#!/usr/bin/env python3
"""Draw what the plotter drew on the simulated MCU.

The simulated MCU writes every output pin change to a trace file
(sim_bridge.py --trace FILE).  This tool turns the step / dir pins of
the X and Y steppers back into the carriage position (CoreXY or
Cartesian, from printer.cfg) and the pen pin into pen up / down, then

  - writes a PNG and / or SVG of the pen-down lines:
        python3 tools/plot_view.py build/sim_trace.bin --cfg printer.cfg \\
            --png build/plot.png --svg build/plot.svg
  - or shows the drawing live in the browser while the plotter runs:
        python3 tools/plot_view.py build/sim_trace.bin --cfg printer.cfg \\
            --serve 8770        # then open http://localhost:8770/

Only the Python standard library is used.

The origin: the simulated MCU has no real endstops; its homing trigger
(sim_trigger_on_steps / sim_set_input) fires the X / Y endstop or DIAG
pin, and the trace records it.  At each trigger the axis is placed at
its position_endstop, like the host does.  A trace without triggers is
placed so that the lowest X and Y reached are the position_endstop.

This file may be distributed under the terms of the GNU GPLv3 license.
"""
import argparse, json, os, re, struct, sys, threading, time, zlib
import http.server

CLOCK_FREQ = 12000000
# Changes when the viewer restarts: an open page then reloads itself
SERVER_ID = "%x" % int(time.time() * 1000)
MIN_BOTH_EDGE_DURATION = 0.000000200


# ---------------------------------------------------------------- config

def read_cfg(path):
    """Minimal printer.cfg reader: {section: {key: value}}."""
    sections, cur = {}, None
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.split("#", 1)[0].split(";", 1)[0].rstrip()
        if not line.strip():
            continue
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            cur = sections.setdefault(m.group(1).strip(), {})
            continue
        if cur is None or line[0] in " \t":
            continue  # continuation lines (macros) are not needed here
        m = re.match(r"^([^:=]+)[:=](.*)$", line)
        if m:
            cur[m.group(1).strip().lower()] = m.group(2).strip()
    return sections


def parse_pin(text):
    """'!^gpio10' -> (10, inverted)"""
    t = text.strip()
    inv = False
    while t and t[0] in "!^~":
        if t[0] == "!":
            inv = not inv
        t = t[1:]
    t = t.split(":")[-1]
    m = re.match(r"^gpio(\d+)$", t)
    if not m:
        sys.exit("pin %r: only gpioN pins are supported" % text)
    return int(m.group(1)), inv


class Stepper:
    def __init__(self, cfg, name):
        sec = cfg.get(name)
        if sec is None:
            sys.exit("printer.cfg has no [%s]" % name)
        self.name = name
        self.step_pin, self.step_inv = parse_pin(sec["step_pin"])
        self.dir_pin, self.dir_inv = parse_pin(sec["dir_pin"])
        steps = (int(sec.get("full_steps_per_rotation", 200))
                 * int(sec.get("microsteps", 16)))
        self.step_dist = float(sec["rotation_distance"]) / steps
        self.endstop = float(sec.get("position_endstop", 0))
        self.max = float(sec.get("position_max", 100))
        self.min = float(sec.get("position_min", 0))
        # stepper.py / tmc.py: TMC drivers step on both edges unless a
        # longer step pulse is configured
        pulse = sec.get("step_pulse_duration")
        has_tmc = any(s.split()[-1] == name and s.split()[0].startswith("tmc")
                      for s in cfg if len(s.split()) == 2)
        self.both_edges = has_tmc and (pulse is None
                                       or float(pulse) <= MIN_BOTH_EDGE_DURATION)
        # Homing trigger pin: endstop_pin, or the TMC DIAG pin for a
        # virtual endstop
        self.trig_pin = self.trig_inv = None
        ep = sec.get("endstop_pin", "")
        if "virtual_endstop" in ep:
            tmc = next((cfg[s] for s in cfg if len(s.split()) == 2
                        and s.split()[0].startswith("tmc")
                        and s.split()[1] == name), {})
            ep = tmc.get("diag_pin", "")
        if ep:
            try:
                self.trig_pin, self.trig_inv = parse_pin(ep)
            except SystemExit:
                pass
        self.pos = 0  # steps


def pen_pin(cfg, override):
    if override:
        return parse_pin(override)
    for name, sec in cfg.items():
        kind = name.split()[0]
        if kind in ("pwm_tool", "output_pin") and "pin" in sec:
            return parse_pin(sec["pin"])
    sys.exit("no pen pin found ([pwm_tool] / [output_pin]); use --pen-pin")


# ---------------------------------------------------------------- tracing

class Plot:
    """Pin changes -> carriage path, pen-down polylines."""

    def __init__(self, cfg, pen_override=None, min_seg=0.05):
        kin = cfg.get("printer", {}).get("kinematics", "corexy")
        self.corexy = kin == "corexy"
        self.cfg = cfg
        self.pen_pin, self.pen_inv = pen_pin(cfg, pen_override)
        self.min_seg = min_seg
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        """Start over (a new trace)."""
        self.sx = Stepper(self.cfg, "stepper_x")
        self.sy = Stepper(self.cfg, "stepper_y")
        self.by_step = {self.sx.step_pin: self.sx, self.sy.step_pin: self.sy}
        self.levels = {}
        self.pen = False
        self.lines = []        # [[(x, y), ...], ...] printer coordinates
        self.cur = None        # current pen-down polyline
        self.minx = self.miny = None
        self.ox = self.oy = 0. # raw -> printer offset (set by homing)
        self.anchored = False
        self.homings = 0
        self.records = 0
        self.last_clock = None
        self.time = 0.         # seconds since the trace started
        self.gen = getattr(self, "gen", 0) + 1

    def clear(self):
        """Forget the lines drawn so far (the live view's Clear)."""
        with self.lock:
            self.lines = []
            if self.cur is not None:
                self.cur = [self.pxy()]
                self.lines.append(self.cur)
            self.gen += 1

    def xy(self):
        a = self.sx.pos * self.sx.step_dist
        b = self.sy.pos * self.sy.step_dist
        if self.corexy:
            return (a + b) * .5, (a - b) * .5
        return a, b

    def pxy(self):
        """Position in printer coordinates (before any homing: raw)."""
        x, y = self.xy()
        return x + self.ox, y + self.oy

    def _point(self, force=False):
        x, y = self.pxy()
        if self.minx is None or x < self.minx:
            self.minx = x
        if self.miny is None or y < self.miny:
            self.miny = y
        if self.cur is not None:
            lx, ly = self.cur[-1]
            if force or abs(x - lx) + abs(y - ly) >= self.min_seg:
                self.cur.append((x, y))

    def _home(self, st):
        """Homing trigger: this axis is at its position_endstop now."""
        x, y = self.xy()
        if st is self.sx:
            self.ox = st.endstop - x
        else:
            self.oy = st.endstop - y
        self.anchored = True
        self.homings += 1

    def feed(self, data):
        """Process complete 8-byte records; returns the bytes used."""
        n = len(data) // 8
        with self.lock:
            for i in range(n):
                clock, pin, level, kind = struct.unpack_from("<IBBH", data,
                                                             i * 8)
                self.records += 1
                if self.last_clock is not None:
                    self.time += ((clock - self.last_clock) & 0xffffffff) \
                        / CLOCK_FREQ
                self.last_clock = clock
                if kind in (2, 3):
                    # input forced by the simulation: a homing trigger?
                    if kind == 2:
                        for st in (self.sx, self.sy):
                            if st.trig_pin == pin and level ^ st.trig_inv:
                                self._home(st)
                    continue
                old = self.levels.get(pin)
                self.levels[pin] = level
                if kind != 0:
                    continue  # pin set up (its first level)
                st = self.by_step.get(pin)
                if st is not None:
                    if not st.both_edges and level != (0 if st.step_inv else 1):
                        continue
                    d = self.levels.get(st.dir_pin, 0) ^ st.dir_inv
                    st.pos += 1 if d else -1
                    self._point()
                elif pin == self.pen_pin and old != level:
                    down = bool(level) != self.pen_inv
                    if down and not self.pen:
                        self.cur = [self.pxy()]
                        self.lines.append(self.cur)
                    elif not down and self.pen:
                        self._point(force=True)
                        self.cur = None
                    self.pen = down
        return n * 8

    def origin(self):
        """Extra offset for drawing: none after a homing trigger, else
        the lowest position reached is taken as the homing position."""
        if self.anchored:
            return 0., 0.
        ox = (self.sx.endstop - self.minx) if self.minx is not None else 0.
        oy = (self.sy.endstop - self.miny) if self.miny is not None else 0.
        return ox, oy

    def bed(self):
        return self.sx.min, self.sy.min, self.sx.max, self.sy.max


def read_trace(path, plot):
    data = open(path, "rb").read()
    plot.feed(data)


# ---------------------------------------------------------------- images

def write_svg(plot, path, margin=5.):
    ox, oy = plot.origin()
    x0, y0, x1, y1 = plot.bed()
    w, h = x1 - x0 + 2 * margin, y1 - y0 + 2 * margin
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" '
           'height="%gmm" viewBox="0 0 %g %g">' % (w, h, w, h),
           '<rect width="100%" height="100%" fill="white"/>',
           '<rect x="%g" y="%g" width="%g" height="%g" fill="none" '
           'stroke="#bbb" stroke-width="0.3" stroke-dasharray="2 2"/>'
           % (margin, margin, x1 - x0, y1 - y0),
           '<g fill="none" stroke="#1a237e" stroke-width="0.35" '
           'stroke-linecap="round" stroke-linejoin="round">']
    for line in plot.lines:
        if len(line) < 2:
            continue
        pts = " ".join("%.2f,%.2f" % (x + ox - x0 + margin,
                                      y1 - (y + oy) + margin)  # Y up
                       for x, y in line)
        out.append('<polyline points="%s"/>' % pts)
    out.append("</g></svg>\n")
    open(path, "w").write("\n".join(out))


def write_png(plot, path, ppmm=6., margin=5.):
    ox, oy = plot.origin()
    x0, y0, x1, y1 = plot.bed()
    W = int((x1 - x0 + 2 * margin) * ppmm) + 1
    H = int((y1 - y0 + 2 * margin) * ppmm) + 1
    img = bytearray(b"\xff" * (W * H * 3))

    def to_px(x, y):
        return ((x + ox - x0 + margin) * ppmm,
                (y1 - (y + oy) + margin) * ppmm)

    def plot_px(px, py, c):
        if 0 <= px < W and 0 <= py < H:
            i = (py * W + px) * 3
            img[i:i + 3] = c

    def seg(p, q, c, width=1):
        (ax, ay), (bx, by) = p, q
        n = int(max(abs(bx - ax), abs(by - ay))) + 1
        for k in range(n + 1):
            t = k / n
            px, py = int(ax + (bx - ax) * t), int(ay + (by - ay) * t)
            for dx in range(width):
                for dy in range(width):
                    plot_px(px + dx, py + dy, c)

    # bed outline (dashed)
    corners = [to_px(x0 - ox, y0 - oy), to_px(x1 - ox, y0 - oy),
               to_px(x1 - ox, y1 - oy), to_px(x0 - ox, y1 - oy)]
    for i in range(4):
        (ax, ay), (bx, by) = corners[i], corners[(i + 1) % 4]
        n = int(max(abs(bx - ax), abs(by - ay))) + 1
        for k in range(0, n, 2):
            if (k // 6) % 2 == 0:
                t = k / n
                plot_px(int(ax + (bx - ax) * t), int(ay + (by - ay) * t),
                        b"\xbb\xbb\xbb")
    ink = b"\x1a\x23\x7e"
    for line in plot.lines:
        pts = [to_px(x, y) for x, y in line]
        for p, q in zip(pts, pts[1:]):
            seg(p, q, ink, 2)

    raw = b"".join(b"\x00" + bytes(img[r * W * 3:(r + 1) * W * 3])
                   for r in range(H))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


# ---------------------------------------------------------------- live view

PAGE = r"""<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plotter view</title><style>
:root{--bg:#f4f4f2;--paper:#fff;--ink:#1a237e;--muted:#777;--edge:#bbb}
@media (prefers-color-scheme:dark){:root{--bg:#1d2125;--paper:#2b3036;
--ink:#9fa8ff;--muted:#999;--edge:#555}}
body{margin:0;background:var(--bg);color:var(--muted);
font:13px system-ui,sans-serif;display:flex;flex-direction:column;
height:100vh}
#bar{padding:6px 12px;display:flex;gap:16px;flex-wrap:wrap;
align-items:center}
button{font:inherit;padding:3px 12px;border-radius:5px;cursor:pointer;
border:1px solid var(--edge);background:var(--paper);color:inherit}
canvas{flex:1;min-height:0;width:100%}
</style></head><body>
<div id="bar"><button id="clear">Clear</button><span id="pos"></span>
<span id="pen"></span><span id="info"></span></div><canvas id="c"></canvas>
<script>
const cv=document.getElementById('c'),cx=cv.getContext('2d');
let lines=[],bed=[0,0,100,100],org=[0,0],head=[0,0],pen=false,
    nLines=0,nPts=0,gen=-1,server=null;
function css(v){return getComputedStyle(document.documentElement)
  .getPropertyValue(v).trim()}
function draw(){
  const r=cv.getBoundingClientRect(),d=devicePixelRatio||1;
  cv.width=r.width*d;cv.height=r.height*d;
  const m=10,bw=bed[2]-bed[0],bh=bed[3]-bed[1];
  const s=Math.min((r.width-2*m)/bw,(r.height-2*m)/bh);
  const X=x=>(m+(x+org[0]-bed[0])*s)*d,
        Y=y=>(m+(bed[3]-(y+org[1]))*s)*d;
  cx.fillStyle=css('--bg');cx.fillRect(0,0,cv.width,cv.height);
  cx.fillStyle=css('--paper');
  cx.fillRect(m*d,m*d,bw*s*d,bh*s*d);
  cx.strokeStyle=css('--edge');cx.setLineDash([4*d,4*d]);
  cx.strokeRect(m*d,m*d,bw*s*d,bh*s*d);cx.setLineDash([]);
  cx.strokeStyle=css('--ink');cx.lineWidth=Math.max(1,0.35*s)*d;
  cx.lineCap=cx.lineJoin='round';
  for(const l of lines){if(l.length<2)continue;cx.beginPath();
    cx.moveTo(X(l[0][0]),Y(l[0][1]));
    for(let i=1;i<l.length;i++)cx.lineTo(X(l[i][0]),Y(l[i][1]));
    cx.stroke()}
  // head
  cx.fillStyle=pen?'#e53935':'#888';cx.beginPath();
  cx.arc(X(head[0]),Y(head[1]),4*d,0,7);cx.fill();
}
async function poll(){
  try{
    const r=await fetch('data?lines='+nLines+'&pts='+nPts+'&gen='+gen);
    const j=await r.json();
    if(server&&j.server!==server){location.reload();return}
    server=j.server;
    if(j.reset){lines=[];}
    gen=j.gen;
    if(j.reset||j.org[0]!==org[0]||j.org[1]!==org[1])org=j.org;
    bed=j.bed;head=j.head;pen=j.pen;
    if(j.tail.length&&lines.length)lines[lines.length-1].push(...j.tail);
    for(const l of j.lines)lines.push(l);
    nLines=j.n_lines;nPts=j.n_pts;
    document.getElementById('pos').textContent=
      'X '+(head[0]+org[0]).toFixed(2)+'  Y '+(head[1]+org[1]).toFixed(2);
    document.getElementById('pen').textContent=pen?'pen down':'pen up';
    document.getElementById('info').textContent=
      j.n_lines+' strokes, '+j.time.toFixed(1)+' s';
    draw();
  }catch(e){}
  setTimeout(poll,200);
}
document.getElementById('clear').onclick=async()=>{
  await fetch('clear',{method:'POST'});lines=[];nLines=nPts=0;draw()};
addEventListener('resize',draw);poll();
</script></body></html>"""


def serve(plot, path, port):
    state = {"pos": 0, "inode": None}

    def tail():
        buf = b""
        while True:
            try:
                st = os.stat(path)
                if state["inode"] != (st.st_ino, st.st_dev) \
                        or st.st_size < state["pos"]:
                    # new trace (bridge restarted): start over
                    with plot.lock:
                        plot.reset()
                    state["inode"] = (st.st_ino, st.st_dev)
                    state["pos"] = 0
                    buf = b""
                if st.st_size > state["pos"]:
                    with open(path, "rb") as f:
                        f.seek(state["pos"])
                        data = f.read(st.st_size - state["pos"])
                    state["pos"] += len(data)
                    buf += data
                    used = plot.feed(buf)
                    buf = buf[used:]
            except FileNotFoundError:
                pass
            time.sleep(0.1)

    threading.Thread(target=tail, daemon=True).start()

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path.startswith("/data"):
                q = dict(p.split("=") for p in
                         self.path.partition("?")[2].split("&") if "=" in p)
                n_lines = int(q.get("lines", 0))
                n_pts = int(q.get("pts", 0))
                gen = int(q.get("gen", -1))
                with plot.lock:
                    reset = gen != plot.gen or n_lines > len(plot.lines) or (
                        n_lines and n_pts > len(plot.lines[n_lines - 1]))
                    if reset:
                        n_lines = n_pts = 0
                    tail_pts = (plot.lines[n_lines - 1][n_pts:]
                                if n_lines else [])
                    new = [list(l) for l in plot.lines[n_lines:]]
                    last = plot.lines[-1] if plot.lines else []
                    x, y = plot.pxy()
                    body = {
                        "server": SERVER_ID,
                        "gen": plot.gen, "homings": plot.homings,
                        "reset": reset, "org": plot.origin(),
                        "bed": plot.bed(), "head": (x, y), "pen": plot.pen,
                        "tail": tail_pts, "lines": new,
                        "n_lines": len(plot.lines), "n_pts": len(last),
                        "time": plot.time,
                    }
                data = json.dumps(body).encode()
                ctype = "application/json"
            elif self.path in ("/", "/index.html"):
                data, ctype = PAGE.encode(), "text/html; charset=utf-8"
            else:
                self.send_error(404)
                return
            self.reply(data, ctype)

        def do_POST(self):
            if self.path != "/clear":
                self.send_error(404)
                return
            plot.clear()
            self.reply(b"{}", "application/json")

        def reply(self, data, ctype):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    # On macOS 127.0.0.1:PORT binds next to another server on *:PORT (and
    # the browser reaches that one): refuse a port something answers on
    import socket
    with socket.socket() as probe:
        probe.settimeout(0.5)
        if probe.connect_ex(("127.0.0.1", port)) == 0:
            sys.exit("port %d is in use by another program; pick another "
                     "--serve port" % port)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
    print("live view: http://localhost:%d/  (trace %s)" % (port, path))
    srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace")
    ap.add_argument("--cfg", required=True, help="printer.cfg the board uses")
    ap.add_argument("--pen-pin", help="pen pin (default: [pwm_tool] / "
                                      "[output_pin] of printer.cfg)")
    ap.add_argument("--png")
    ap.add_argument("--svg")
    ap.add_argument("--ppmm", type=float, default=6., help="PNG pixels/mm")
    ap.add_argument("--serve", type=int, metavar="PORT",
                    help="live view in the browser")
    a = ap.parse_args()
    cfg = read_cfg(a.cfg)
    plot = Plot(cfg, a.pen_pin)
    if a.serve:
        serve(plot, a.trace, a.serve)
        return
    read_trace(a.trace, plot)
    ox, oy = plot.origin()
    print("%d pin changes, %.1f s, %d strokes, %d homing triggers%s"
          % (plot.records, plot.time, len(plot.lines), plot.homings,
             "" if plot.anchored else " (origin estimated)"))
    if a.png:
        write_png(plot, a.png, a.ppmm)
        print("wrote", a.png)
    if a.svg:
        write_svg(plot, a.svg)
        print("wrote", a.svg)
    if not a.png and not a.svg:
        print("nothing to write: --png, --svg or --serve")


if __name__ == "__main__":
    main()
