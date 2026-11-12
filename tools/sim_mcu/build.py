#!/usr/bin/env python3
# Build the simulated MCU (Klipper MCU code as a host process).
#
#   python3 tools/sim_mcu/build.py [--out build/sim_mcu]
#
# Klipper collects "compile time requests" (commands, enumerations,
# constants) from a special ELF section of every object file and turns
# them into the data dictionary with scripts/buildcommands.py.  Mach-O
# section names are limited to 16 characters, so on macOS the requests
# are placed in __DATA,__klipper_ctr and read back with this script.
#
# This file may be distributed under the terms of the GNU GPLv3 license.
import argparse, os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
KLIPPER = os.path.join(ROOT, "third_party", "klipper")
HERE = os.path.dirname(os.path.abspath(__file__))

KLIPPER_SRCS = [
    "sched.c", "command.c", "basecmd.c", "debugcmds.c",
    "initial_pins.c", "gpiocmds.c", "stepper.c", "endstop.c", "trsync.c",
    "generic/crc16_ccitt.c", "generic/alloc.c",
]
# tmcuart_sim.c replaces src/tmcuart.c with emulated TMC2209 drivers
ARCH_SRCS = ["main.c", "timer.c", "console.c", "gpio.c", "tmcuart_sim.c"]
SECTION = "__klipper_ctr"


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit("command failed: %s" % " ".join(cmd))


def macho_section(path, segname, sectname):
    """Return the bytes of one section of a 64-bit Mach-O object file."""
    data = open(path, "rb").read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != 0xfeedfacf:
        sys.exit("%s: not a 64-bit Mach-O object" % path)
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            s = off + 72
            for _ in range(nsects):
                sn = data[s:s + 16].rstrip(b"\0").decode()
                sg = data[s + 16:s + 32].rstrip(b"\0").decode()
                size, offset = struct.unpack_from("<QI", data, s + 40)
                if sn == sectname and sg == segname:
                    return data[offset:offset + size]
                s += 80
        off += cmdsize
    return b""


def elf_section(path, name):
    """Return the bytes of one section of a 64-bit ELF object file."""
    data = open(path, "rb").read()
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3a)
    def sh(i):
        return struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
    stroff = sh(shstrndx)[4]
    for i in range(shnum):
        h = sh(i)
        n = data[stroff + h[0]:data.index(b"\0", stroff + h[0])].decode()
        if n == name:
            return data[h[4]:h[4] + h[5]]
    return b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "sim_mcu"))
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()
    out = args.out
    os.makedirs(os.path.join(out, "board-generic"), exist_ok=True)
    # Same include layout as the Klipper Makefile: "board/x.h" finds the
    # arch directory first, then src/generic
    for link, target in [(os.path.join(out, "board"), os.path.join(HERE, "arch")),
                         (os.path.join(out, "board-generic", "board"),
                          os.path.join(KLIPPER, "src", "generic"))]:
        if os.path.islink(link):
            os.unlink(link)
        os.symlink(target, link)

    if sys.platform == "darwin":
        section_attr = '__attribute__((section("__DATA,%s")))' % SECTION
    else:
        section_attr = '__attribute__((section(".compile_time_request")))'
    cflags = ["-iquote", HERE, "-iquote", out, "-iquote",
              os.path.join(KLIPPER, "src"), "-iquote",
              os.path.join(out, "board-generic"),
              "-std=gnu11", "-O2", "-Wall", "-Wno-unused-function",
              "-Wno-gnu-variable-sized-type-not-at-end",
              "-Wno-address-of-packed-member",
              "-D__section(S)=" + section_attr]

    srcs = [os.path.join(KLIPPER, "src", s) for s in KLIPPER_SRCS]
    srcs += [os.path.join(HERE, "arch", s) for s in ARCH_SRCS]
    objs, ctr = [], b""
    for src in srcs:
        rel = os.path.relpath(src, ROOT).replace("/", "_")
        obj = os.path.join(out, rel + ".o")
        run([args.cc] + cflags + ["-c", src, "-o", obj])
        if sys.platform == "darwin":
            ctr += macho_section(obj, "__DATA", SECTION)
        else:
            ctr += elf_section(obj, ".compile_time_request")
        objs.append(obj)

    # Requests are NUL terminated strings (plus padding) -> one per line
    lines = [l for l in ctr.split(b"\0") if l]
    txt = os.path.join(out, "compile_time_request.txt")
    open(txt, "wb").write(b"\n".join(lines) + b"\n")
    defconfig = os.path.join(out, "defconfig")
    open(defconfig, "w").write("# simulated rp2040\n")
    ctr_c = os.path.join(out, "compile_time_request.c")
    run([sys.executable, os.path.join(KLIPPER, "scripts", "buildcommands.py"),
         "-d", os.path.join(out, "klipper.dict"), "-k", defconfig,
         "-t", "%s;as;ld;objcopy;objdump;strip" % args.cc, txt, ctr_c],
        cwd=KLIPPER)
    obj = os.path.join(out, "compile_time_request.o")
    run([args.cc] + cflags + ["-c", ctr_c, "-o", obj])
    objs.append(obj)
    exe = os.path.join(out, "sim_mcu")
    run([args.cc] + objs + ["-o", exe] + ([] if sys.platform == "darwin"
                                         else ["-lutil"]))
    print("built %s" % exe)


if __name__ == "__main__":
    main()
