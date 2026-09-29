#!/usr/bin/env python3
"""Rebind absolute symbols that point inside allocated sections.

The ROM aliases in romsyms.ld (e.g. gSramFileLarge = gGbaSram + 0x2F20) come
out of ld as SHN_ABS symbols. vita-elf-create never relocates references to
absolute symbols, but the Vita loads each segment at its own address, so those
references would keep link-time addresses. Giving each such symbol the index
of the section containing its value makes the references relocatable.

Usage: fix_abs_symbols.py <elf>   (modified in place)
"""

import struct
import sys

SHN_ABS = 0xFFF1
SHT_SYMTAB = 2
SHF_ALLOC = 0x2


def main():
    path = sys.argv[1]
    data = bytearray(open(path, "rb").read())
    shoff = struct.unpack_from("<I", data, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)

    sections = []
    for i in range(shnum):
        (name, stype, flags, addr, off, size, link, info, align, entsize) = struct.unpack_from(
            "<10I", data, shoff + i * shentsize)
        sections.append((stype, flags, addr, off, size, entsize))

    alloc = [(addr, addr + size, i) for i, (stype, flags, addr, off, size, _) in enumerate(sections)
             if flags & SHF_ALLOC and size > 0]

    fixed = 0
    for stype, flags, addr, off, size, entsize in sections:
        if stype != SHT_SYMTAB:
            continue
        for pos in range(off, off + size, entsize):
            name, value, sym_size, info, other, shndx = struct.unpack_from("<IIIBBH", data, pos)
            if shndx != SHN_ABS:
                continue
            for start, end, index in alloc:
                if start <= value < end:
                    struct.pack_into("<H", data, pos + 14, index)
                    fixed += 1
                    break
    open(path, "wb").write(data)
    print(f"{path}: rebound {fixed} absolute symbols")


if __name__ == "__main__":
    main()
