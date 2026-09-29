#!/usr/bin/env python3
"""Prepare the LiveArea images for the VPK.

The Vita only installs a bubble whose images have the exact sizes below and
are 8-bit palette PNGs. This converts port/vita/sce_sys (any size, RGB or
RGBA) into that form under the build directory, scaling to cover the target
and cropping the centre, so the artwork can be edited freely. Other files are
copied as they are; hidden files (.DS_Store) are left out.

Usage: livearea.py <src sce_sys dir> <dst sce_sys dir>
"""

import shutil
import sys
from pathlib import Path

from PIL import Image

SIZES = {
    "icon0.png": (128, 128),
    "livearea/contents/bg.png": (840, 500),
    "livearea/contents/startup.png": (280, 158),
    "pic0.png": (960, 544),
}


def convert(src, dst, size):
    img = Image.open(src).convert("RGBA")
    scale = max(size[0] / img.width, size[1] / img.height)
    w, h = round(img.width * scale), round(img.height * scale)
    img = img.resize((w, h), Image.LANCZOS)
    left, top = (w - size[0]) // 2, (h - size[1]) // 2
    img = img.crop((left, top, left + size[0], top + size[1]))
    # LiveArea images have no transparency: flatten onto black.
    flat = Image.new("RGB", size, (0, 0, 0))
    flat.paste(img, mask=img.getchannel("A"))
    flat.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG).save(dst, optimize=True)


def main():
    src_dir, dst_dir = Path(sys.argv[1]), Path(sys.argv[2])
    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    for src in sorted(src_dir.rglob("*")):
        rel = src.relative_to(src_dir)
        if not src.is_file() or any(part.startswith(".") for part in rel.parts):
            continue
        dst = dst_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        key = rel.as_posix()
        if key in SIZES:
            convert(src, dst, SIZES[key])
        else:
            shutil.copyfile(src, dst)


if __name__ == "__main__":
    main()
