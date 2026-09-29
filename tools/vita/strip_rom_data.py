#!/usr/bin/env python3
"""Take the ROM's data out of the Vita executable.

The game's asset units (graphics, maps, text, music, movies) are linked into
the Vita ELF byte for byte as they are in the ROM, except for the words the
linker relocates (pointers, which hold Vita addresses). This zeroes every
byte that comes from the ROM and writes rommap.bin, the list of
(address, ROM offset, length) runs that port/vita/host/vita_rom.c copies back
from the player's own ROM at startup. The map holds only numbers.

Every run is checked against the ROM before it is blanked, and the result is
verified by rebuilding the original ELF from the stripped one and the ROM.

Usage: strip_rom_data.py <in.elf> <vita.map> <gba.map> <rom.gba> <out.elf> <rommap.bin> <anchor-symbol> <gba.elf>
"""

import bisect
import hashlib
import re
import struct
import subprocess
import sys
from pathlib import Path

SDK = Path.home() / "Developer/vitasdk/bin"
ROM_BASE = 0x08000000
# Vita section -> the GBA section its units came from (configure_vita.py ROM_SECTIONS).
SECTIONS = {".romtext": ".text", ".romrodata": ".rodata", ".romdata": ".data"}
# Sections holding decomp C data, matched against the ROM symbol by symbol.
SYMBOL_SECTIONS = (".gamerodata", ".data")
MAGIC = b"KHRM"
FORMAT_VERSION = 1


def parse_map(path, wanted):
    """{(section, object): (address, size)} for the input sections in `wanted`."""
    out = {}
    lines = Path(path).read_text().splitlines()
    rx_full = re.compile(r"^ (\.[\w.]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (\S+\.o)$")
    rx_name = re.compile(r"^ (\.[\w.]+)$")
    rx_rest = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (\S+\.o)$")
    i = 0
    while i < len(lines):
        m = rx_full.match(lines[i])
        if m:
            sec, addr, size, obj = m[1], int(m[2], 16), int(m[3], 16), m[4]
        else:
            m = rx_name.match(lines[i])
            r = rx_rest.match(lines[i + 1]) if m and i + 1 < len(lines) else None
            if not r:
                i += 1
                continue
            sec, addr, size, obj = m[1], int(r[1], 16), int(r[2], 16), r[3]
            i += 1
        if sec in wanted and size:
            out[(sec, Path(obj).name)] = (addr, size)
        i += 1
    return out


def elf_sections(data):
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    raw = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    strtab = raw[shstrndx]
    names = data[strtab[4]:strtab[4] + strtab[5]]
    secs = {}
    for s in raw:
        name = names[s[0]:names.index(b"\0", s[0])].decode()
        secs[name] = {"type": s[1], "addr": s[3], "offset": s[4], "size": s[5], "info": s[7]}
    return secs


def reloc_words(data, secs, name):
    """Addresses of the 4-byte words relocations patch inside section `name`."""
    rel = secs.get(".rel" + name)
    if rel is None:
        return set()
    out = set()
    for i in range(0, rel["size"], 8):
        r_offset, r_info = struct.unpack_from("<II", data, rel["offset"] + i)
        rtype = r_info & 0xFF
        if rtype not in (2, 38):  # R_ARM_ABS32, R_ARM_TARGET1
            sys.exit(f"error: unexpected relocation type {rtype} at {r_offset:08X} in {name}")
        out.add(r_offset)
    return out


def object_symbols(elf, readelf):
    """{name: (address, size, section index)} for OBJECT symbols with a size,
    leaving out names defined more than once."""
    seen = {}
    dup = set()
    for line in subprocess.check_output([readelf, "-sW", elf], text=True).splitlines():
        parts = line.split()
        if len(parts) != 8 or parts[3] != "OBJECT" or parts[6] in ("UND", "ABS", "COM"):
            continue
        size = int(parts[2], 0)
        if size == 0:
            continue
        name = parts[7]
        if name in seen:
            dup.add(name)
        seen[name] = (int(parts[1], 16), size, int(parts[6]))
    return {k: v for k, v in seen.items() if k not in dup}


def symbol_address(elf, name):
    for line in subprocess.check_output([str(SDK / "arm-vita-eabi-nm"), elf], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"error: {name} not in {elf}")


def main():
    in_elf, vita_map, gba_map, rom_path, out_elf, map_out, anchor = sys.argv[1:8]
    data = bytearray(Path(in_elf).read_bytes())
    original = bytes(data)
    rom = Path(rom_path).read_bytes()
    secs = elf_sections(data)
    vita = parse_map(vita_map, set(SECTIONS))
    gba = parse_map(gba_map, set(SECTIONS.values()))

    runs = []
    blanked = mismatched = 0

    def pieces_of(sec, start, end):
        cuts = [w for w in sec["relocs"][bisect.bisect_left(sec["relocs"], start - 3):] if w < end]
        out = []
        pos = start
        for w in cuts:
            if w > pos:
                out.append((pos, w))
            pos = max(pos, w + 4)
        if pos < end:
            out.append((pos, end))
        return out
    for (vsec, obj), (vaddr, vsize) in sorted(vita.items(), key=lambda kv: kv[1][0]):
        gba_key = (SECTIONS[vsec], obj)
        if gba_key not in gba:
            sys.exit(f"error: {obj} {vsec} has no GBA counterpart")
        gaddr, gsize = gba[gba_key]
        # The Vita copy may start with alignment padding (align_blob_unit).
        pad = vsize - gsize
        if not 0 <= pad < 16:
            sys.exit(f"error: {obj} {vsec}: Vita size {vsize:#x} vs GBA {gsize:#x}")
        sec = secs[vsec]
        if "relocs" not in sec:
            sec["relocs"] = sorted(reloc_words(data, secs, vsec))
        start = vaddr + pad
        end = start + gsize
        file_base = sec["offset"] - sec["addr"]
        rom_base = gaddr - ROM_BASE - start
        pieces = pieces_of(sec, start, end)
        for a, b in pieces:
            fo, ro = a + file_base, a + rom_base
            if data[fo:b + file_base] == rom[ro:b + rom_base]:
                data[fo:b + file_base] = bytes(b - a)
                runs.append((a, ro, b - a))
                blanked += b - a
                continue
            # Some bytes differ: blank only the ones that match.
            run = None
            for x in range(a, b):
                if data[x + file_base] == rom[x + rom_base]:
                    data[x + file_base] = 0
                    blanked += 1
                    if run is None:
                        run = x
                else:
                    mismatched += 1
                    if run is not None:
                        runs.append((run, run + rom_base, x - run))
                        run = None
            if run is not None:
                runs.append((run, run + rom_base, b - run))

    # Decomp C tables: whole symbols whose bytes equal the ROM's.
    sec_by_index = {i: name for i, name in enumerate(secs)}
    gba_syms = object_symbols(sys.argv[8], "arm-none-eabi-readelf")
    vita_syms = object_symbols(in_elf, str(SDK / "arm-vita-eabi-readelf"))
    sym_count = sym_bytes = sym_skipped = 0
    for name, (vaddr, vsize, shndx) in sorted(vita_syms.items(), key=lambda kv: kv[1][0]):
        sname = sec_by_index.get(shndx)
        if sname not in SYMBOL_SECTIONS or name not in gba_syms:
            continue
        gaddr, gsize, _ = gba_syms[name]
        if gsize != vsize or not ROM_BASE <= gaddr < ROM_BASE + len(rom):
            continue
        sec = secs[sname]
        if "relocs" not in sec:
            sec["relocs"] = sorted(reloc_words(data, secs, sname))
        file_base = sec["offset"] - sec["addr"]
        rom_base = gaddr - ROM_BASE - vaddr
        pieces = pieces_of(sec, vaddr, vaddr + vsize)
        if not all(data[a + file_base:b + file_base] == rom[a + rom_base:b + rom_base] for a, b in pieces):
            sym_skipped += 1
            continue
        for a, b in pieces:
            data[a + file_base:b + file_base] = bytes(b - a)
            runs.append((a, a + rom_base, b - a))
            sym_bytes += b - a
        sym_count += 1
    blanked += sym_bytes
    runs.sort()

    # Merge runs that continue each other in both the executable and the ROM.
    merged = []
    for r in runs:
        if merged and merged[-1][0] + merged[-1][2] == r[0] and merged[-1][1] + merged[-1][2] == r[1]:
            merged[-1] = (merged[-1][0], merged[-1][1], merged[-1][2] + r[2])
        else:
            merged.append(r)

    # Self-check: the stripped ELF plus the ROM must give back the original.
    check = bytearray(data)
    for addr, off, length in merged:
        sec = next(s for s in secs.values() if s["addr"] <= addr < s["addr"] + s["size"] and s["type"] == 1
                   and s["addr"] != 0)
        fo = sec["offset"] + (addr - sec["addr"])
        check[fo:fo + length] = rom[off:off + length]
    if bytes(check) != original:
        sys.exit("error: rebuilding the executable from the ROM does not give the original back")

    Path(out_elf).write_bytes(data)
    header = MAGIC + struct.pack("<III", FORMAT_VERSION, symbol_address(in_elf, anchor), len(merged))
    header += hashlib.sha1(rom).digest() + struct.pack("<I", len(rom))
    body = b"".join(struct.pack("<III", *r) for r in merged)
    Path(map_out).write_bytes(header + body)
    print(f"stripped {blanked / 1e6:.1f} MB of ROM data in {len(merged)} runs"
          f" ({mismatched} bytes differ from the ROM and were kept);"
          f" C tables: {sym_count} symbols, {sym_bytes / 1e3:.0f} KB ({sym_skipped} differ and were kept)")


if __name__ == "__main__":
    main()
