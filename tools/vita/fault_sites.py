#!/usr/bin/env python3
"""List the places where the game read or wrote through NULL/raw GBA addresses.

The kubridge fault handler (port/vita/host/vita_fault.c) emulates those
accesses and logs each new site to ux0:data/khcom/log.txt. This maps every
logged pc to its function and source line so the access can be fixed in the
code (then the game no longer needs kubridge there).

The log must come from the same build as the ELF: its first line names the
port version, and the module base is recovered from the FaultInit= address.

Usage: fault_sites.py <log.txt> [build/vita/us/khcom.elf]
"""

import os
import re
import subprocess
import sys

SDK_BIN = os.path.join(os.environ.get("VITASDK", os.path.expanduser("~/Developer/vitasdk")), "bin")
SITE_RE = re.compile(r"(?:emulated GBA access at|emulated site|unhandled data abort at) pc=([0-9A-F]{8}) addr=([0-9A-F]{8})")
BASE_RE = re.compile(r"FaultInit=(?:0x)?([0-9a-fA-F]+)")


def symbol_address(elf, name):
    out = subprocess.check_output([os.path.join(SDK_BIN, "arm-vita-eabi-nm"), elf], text=True)
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"error: {name} not found in {elf}")


def main():
    log = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else "build/vita/us/khcom.elf"
    text = open(log, errors="replace").read()
    first = text.splitlines()[0] if text else ""
    base = BASE_RE.search(text)
    if not base:
        sys.exit("error: no FaultInit= line in the log (kubridge not active?)")
    # Drop the Thumb bit: it is set on the runtime pointer and may not be in nm's output.
    slide = (int(base[1], 16) & ~1) - (symbol_address(elf, "FaultInit") & ~1)
    sites = {}
    for m in SITE_RE.finditer(text):
        sites.setdefault(int(m[1], 16), set()).add(int(m[2], 16))
    print(f"{first}\n{len(sites)} site(s), module slide {slide:+#x}\n")
    for pc, addrs in sorted(sites.items()):
        link = pc - slide
        loc = subprocess.check_output([os.path.join(SDK_BIN, "arm-vita-eabi-addr2line"), "-f", "-i", "-C",
                                       "-e", elf, hex(link)], text=True).split("\n")
        where = " <- ".join(f"{loc[i]} ({os.path.relpath(loc[i + 1]) if loc[i + 1].startswith('/') else loc[i + 1]})"
                            for i in range(0, len(loc) - 1, 2))
        print(f"pc {pc:08X} (link {link:08X}) addr {', '.join(f'{a:08X}' for a in sorted(addrs))}\n    {where}")


if __name__ == "__main__":
    main()
