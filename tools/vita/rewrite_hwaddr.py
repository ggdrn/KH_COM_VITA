#!/usr/bin/env python3
"""Rewrite GBA hardware address literals in src/ into HW_* macros.

The macros (include/gba/hwaddr.h) expand to the original integer constants on
GBA builds, so the matching ROM is unaffected, and to emulated buffers on
PLATFORM_VITA builds.

Literals used as CpuSet/CpuFastSet control words or as flag masks are left
alone. Run with --dry-run to review the changes first.
"""

import argparse
import re
import sys
from pathlib import Path

LIT_RE = re.compile(r"0x0([4-7E])0([0-9A-Fa-f]{5})\b")

REGIONS = {
    "5": ("HW_PLTT", 0x400),
    "6": ("HW_VRAM", 0x18000),
    "7": ("HW_OAM", 0x400),
    "E": ("HW_SRAM", 0x10000),
}

CALL_RE = re.compile(r"\b(CpuSet|CpuFastSet)\s*\(")
FLAG_CONTEXT_RE = re.compile(r"(flags|sortKey)\s*(\|=|&=|=|&|\|)\s*~?\(?\s*$")


def split_call_args(text, start):
    """Return [(arg_start, arg_end)] for the call whose '(' is at start."""
    depth = 0
    args = []
    arg_start = start + 1
    i = start
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append((arg_start, i))
                return args
        elif c == "," and depth == 1:
            args.append((arg_start, i))
            arg_start = i + 1
        i += 1
    return None


def control_word_spans(text):
    spans = []
    for m in CALL_RE.finditer(text):
        args = split_call_args(text, m.end() - 1)
        if args and len(args) == 3:
            spans.append(args[2])
    return spans


def rewrite(text):
    spans = control_word_spans(text)
    out = []
    last = 0
    count = 0
    for m in LIT_RE.finditer(text):
        region, off = m.group(1).upper(), int(m.group(2), 16)
        if region == "4":
            continue
        name, size = REGIONS[region]
        if off >= size:
            continue
        pos = m.start()
        if any(a <= pos < b for a, b in spans):
            continue
        line_start = text.rfind("\n", 0, pos) + 1
        if FLAG_CONTEXT_RE.search(text[line_start:pos]):
            continue
        out.append(text[last:pos])
        out.append(f"{name}(0x{off:X})")
        last = m.end()
        count += 1
    out.append(text[last:])
    return "".join(out), count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("paths", nargs="*", default=["src"])
    args = parser.parse_args()
    total = 0
    for root in args.paths:
        for path in sorted(Path(root).rglob("*.c")):
            text = path.read_text()
            new, count = rewrite(text)
            if not count:
                continue
            total += count
            print(f"{path}: {count}")
            if not args.dry_run:
                if '#include "gba/hwaddr.h"' not in new:
                    new = '#include "gba/hwaddr.h"\n' + new
                path.write_text(new)
    print(f"total: {total}", file=sys.stderr)


if __name__ == "__main__":
    main()
