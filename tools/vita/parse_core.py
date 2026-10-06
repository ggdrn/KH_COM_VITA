#!/usr/bin/env python3
"""Summarize a PS Vita crash dump (.psp2dmp) against build/vita/<ver>/khcom.elf.

Prints each thread's registers and, for addresses inside the game module,
the source location (addr2line), accounting for the address the module was
loaded at.

Usage: parse_core.py <dump.psp2dmp> [elf]
"""

import gzip
import os
import struct
import subprocess
import sys

ADDR2LINE = os.path.expanduser("~/Developer/vitasdk/bin/arm-vita-eabi-addr2line")


def notes(core):
    out = {}
    phnum = struct.unpack_from("<H", core, 44)[0]
    for i in range(phnum):
        ptype, off = struct.unpack_from("<II", core, 52 + i * 32)
        if ptype != 4:
            continue
        namesz, descsz, _ = struct.unpack_from("<3I", core, off)
        name = core[off + 12:off + 12 + namesz].rstrip(b"\0").decode()
        doff = off + 12 + ((namesz + 3) & ~3)
        out[name] = core[doff:doff + descsz]
    return out


def module_segments(info):
    """(runtime base, size) of the module's segments, from MODULE_INFO."""
    # The module is named after the ELF it was made from: khcom.elf, or
    # khcom.stripped.elf since the ROM data is stripped after linking.
    for module in (b"khcom.stripped.elf", b"khcom.elf", b"khcom"):
        k = info.find(module)
        if k >= 40:
            break
    words = struct.unpack_from("<60I", info, k - 40)
    segs = []
    for i in range(len(words) - 3):
        if words[i] in (5, 6) and 0x80000000 <= words[i + 1] < 0xA0000000 and words[i + 3] == 0x1000:
            segs.append((words[i + 1], words[i + 2]))
    return segs


def elf_segments(elf):
    out = subprocess.check_output([ADDR2LINE.replace("addr2line", "readelf"), "-lW", elf], text=True)
    segs = []
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD":
            segs.append(int(parts[2], 16))
    return segs


def main():
    dump = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else "build/vita/us/khcom.elf"
    raw = open(dump, "rb").read()
    core = gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw
    n = notes(core)
    runtime = module_segments(n["MODULE_INFO"])
    linked = elf_segments(elf)
    print("module segments (runtime -> link):",
          ", ".join(f"{r:08x}->{l:08x}" for (r, _), l in zip(runtime, linked)))

    def to_link(addr):
        for (base, size), link in zip(runtime, linked):
            if base <= addr < base + size:
                return addr - base + link
        return None

    regs = n["THREAD_REG_INFO"]
    count = struct.unpack_from("<I", regs, 4)[0]
    off = 8
    for _ in range(count):
        size, tid = struct.unpack_from("<II", regs, off)
        r = struct.unpack_from("<17I", regs, off + 8)
        names = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc", "cpsr"]
        print(f"\nthread {tid:08x}")
        print("  " + " ".join(f"{nm}={v:08x}" for nm, v in zip(names, r)))
        for nm in ("pc", "lr"):
            v = r[names.index(nm)]
            link = to_link(v & ~1)
            if link is not None:
                loc = subprocess.check_output([ADDR2LINE, "-f", "-i", "-C", "-e", elf, hex(link)], text=True)
                print(f"  {nm} {v:08x} (link {link:08x}): " + " / ".join(loc.split()))
        off += size


if __name__ == "__main__":
    main()
