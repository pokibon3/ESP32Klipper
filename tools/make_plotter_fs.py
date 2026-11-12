#!/usr/bin/env python3
"""Prepares the files of the plotter's storage partition (FAT, /data):

    www/      Mainsail (text files gzip-compressed, served as-is)
    config/   printer.cfg
    gcodes/   examples/*.gcode (and files given with --gcode)

    python3 tools/make_plotter_fs.py [--mainsail build/mainsail]
        [--config config/skr_pico_corexy_penplotter.cfg] [--out build/plotter_fs]
        [--no-www] [--gcode FILE ...]

CYD (screen + microSD, no WiFi): --no-www.  The G-code files are used
when no microSD card is inserted.

Then build apps/plotter with PLOTTER_FLASH_FS=1 in the environment to
flash the image together with the firmware (this replaces the files on
the board, including uploads and Mainsail settings).
"""
import argparse
import gzip
import os
import shutil
import sys

TEXT = (".html", ".js", ".css", ".svg", ".json", ".webmanifest", ".txt",
        ".map", ".mjs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mainsail", default="build/mainsail")
    ap.add_argument("--config", default="config/skr_pico_corexy_penplotter.cfg")
    ap.add_argument("--out", default="build/plotter_fs")
    ap.add_argument("--no-www", action="store_true",
                    help="without Mainsail (CYD: no WiFi)")
    ap.add_argument("--gcode", action="append", default=[],
                    help="extra G-code file for gcodes/")
    a = ap.parse_args()
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    for d in ("www", "config", "gcodes"):
        os.makedirs(os.path.join(a.out, d))
    shutil.copy(a.config, os.path.join(a.out, "config", "printer.cfg"))
    examples = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "examples")
    gcodes = [os.path.join(examples, f) for f in sorted(os.listdir(examples))
              if f.endswith(".gcode")] + a.gcode
    for g in gcodes:
        shutil.copy(g, os.path.join(a.out, "gcodes", os.path.basename(g)))
    total = 0
    if a.no_www:
        pass
    elif not os.path.isdir(a.mainsail):
        print("no Mainsail in %s: image without www/" % a.mainsail)
    else:
        for root, dirs, files in os.walk(a.mainsail):
            rel = os.path.relpath(root, a.mainsail)
            dst_dir = os.path.join(a.out, "www", rel) if rel != "." else \
                os.path.join(a.out, "www")
            os.makedirs(dst_dir, exist_ok=True)
            names = set(files)
            for f in files:
                src = os.path.join(root, f)
                if f.endswith(".gz") and f[:-3] in names:
                    continue  # use our own compression of the original
                if f.lower().endswith(TEXT) and f != "config.json":
                    dst = os.path.join(dst_dir, f + ".gz")
                    with open(src, "rb") as fi, \
                            gzip.GzipFile(dst, "wb", mtime=0) as fo:
                        fo.write(fi.read())
                else:
                    dst = os.path.join(dst_dir, f)
                    shutil.copy(src, dst)
                total += os.path.getsize(dst)
    print("%s: www %.1f MB" % (a.out, total / 1e6))


if __name__ == "__main__":
    sys.exit(main())
