#!/usr/bin/env python3
"""Fail the build if the executable still carries data from the ROM.

Samples 64-byte pieces of the ROM (skipping padding-like ones) and looks for
each in the stripped ELF that becomes the VPK's eboot.bin.

Usage: audit_rom_free.py <stripped.elf> <rom.gba> <stamp>
"""

import re
import sys
from pathlib import Path

STRIDE = 0x4000
CHUNK = 64


def main():
    elf, rom_path, stamp = sys.argv[1:4]
    # The stripped data is mostly zeros. A sample has at least 8 distinct
    # values, so it never holds 64 zeros in a row: shrinking longer zero runs
    # to 63 bytes keeps every possible match and makes the search much faster.
    blob = re.sub(b"\x00{64,}", b"\x00" * 63, Path(elf).read_bytes())
    rom = Path(rom_path).read_bytes()
    found = []
    tested = 0
    for off in range(0, len(rom) - CHUNK, STRIDE):
        chunk = rom[off:off + CHUNK]
        if len(set(chunk)) < 8:
            continue
        tested += 1
        if chunk in blob:
            found.append(off)
    if found:
        sys.exit(f"error: {len(found)} of {tested} ROM samples are still in {elf} "
                 f"(first at ROM offset {found[0]:#x})")
    Path(stamp).write_text(f"{tested} samples, none found\n")
    print(f"audit: none of {tested} ROM samples found in the executable")


if __name__ == "__main__":
    main()
