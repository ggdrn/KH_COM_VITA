#!/usr/bin/env python3
"""Generate build/vita/build.ninja for the PS Vita port.

The game sources are compiled unchanged (with PLATFORM_VITA), the asset/data
units are assembled as plain data, and the GBA code-only assembly (crt0, BIOS
calls, the m4a mixer) is replaced by the platform layer in port/vita.

Absolute symbols from config/<version>/symbols.txt and the regional sidecars
point into ROM data, emulated hardware memory or constants; they are emitted as
linker-script aliases in build/vita/<version>/romsyms.ld. Resolving aliases that point into
compiled C data needs the matching GBA ELF (build/<version>/com_<version>.elf),
produced by tools/vita/check_gba_match.sh.
"""

import argparse
import bisect
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
sys.path.append(str(ROOT / "tools"))

import ninja_syntax  # noqa: E402
from asset_objects import materialize_assets  # noqa: E402
from assetgen import plan as asset_plan  # noqa: E402
from regional_data import asset_symbols, load_sidecars  # noqa: E402

VERSIONS = {"us": "B8CE", "jp": "B8CJ", "eu": "B8CP"}

# GBA code units replaced by the platform layer.
REPLACED_UNITS = {
    "header.s",
    "crt0.s",
    "libagbsyscall.s",
    "m4a_1.s",
    "transform_veneers.s",
}
# Generated tables that store pointer-carrying structs as byte arrays.
RELOCATED_GEN_C = {"event_backgrounds.c", "map_rooms.c"}
# ARM code that runs unchanged on the Vita's Cortex-A9.
NATIVE_ARM_UNITS = {"transform.s"}

HW_REGIONS = [
    (0x05000000, 0x400, "gGbaPltt"),
    (0x06000000, 0x18000, "gGbaVram"),
    (0x07000000, 0x400, "gGbaOam"),
    (0x0E000000, 0x10000, "gGbaSram"),
]

GAME_CFLAGS = [
    "-std=gnu89",
    "-fpermissive",
    "-O2",
    "-g",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-common",
    "-fno-toplevel-reorder",
    "-mstructure-size-boundary=32",
    "-w",
]
PORT_CFLAGS = ["-std=gnu11", "-O3", "-g", "-Wall", "-Wno-unused-function"]


def parse_symbols(version):
    symbols = []
    for line in Path(f"config/{version}/symbols.txt").read_text().splitlines():
        line = line.split("#")[0].strip()
        if line:
            name, addr = (x.strip() for x in line.split("="))
            symbols.append((name, int(addr, 16)))
    return symbols


def blob_ranges(version):
    """(start, end, label) for every address-bounded .incbin data unit."""
    ranges = []
    for path in sorted(Path(f"asm/{version}").glob("*.s")) + sorted(Path("asm").glob("*.s")):
        text = path.read_text()
        m = re.search(r'\.incbin\s+"assets/' + version + r'/([0-9A-F]{8})-([0-9A-F]{8})\.bin"', text)
        if not m:
            continue
        label = re.search(r"^(\w+):", text, re.M)
        if label:
            ranges.append((int(m[1], 16), int(m[2], 16), label[1]))
    return ranges


def gba_elf_symbols(version, absolute):
    elf = Path(f"build/{version}/com_{version}.elf")
    if not elf.exists():
        return None
    out = subprocess.check_output(["arm-none-eabi-nm", "-n", str(elf)], text=True)
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] in "aA" or parts[2] in absolute or parts[2].startswith("."):
            continue
        addr = int(parts[0], 16)
        if 0x08000000 <= addr < 0x0A000000:
            syms.append((addr, parts[2]))
    syms.sort()
    return syms


def write_romsyms(path, version, symbols):
    absolute = {name for name, _ in symbols}
    blobs = blob_ranges(version)
    elf_syms = None
    lines = []
    unresolved = []
    for name, addr in symbols:
        target = None
        if addr < 0x02000000:
            target = f"0x{addr:X}"
        elif 0x08000000 <= addr < 0x0A000000:
            for start, end, label in blobs:
                if start <= addr < end:
                    target = f"{label} + 0x{addr - start:X}"
                    break
            else:
                if elf_syms is None:
                    elf_syms = gba_elf_symbols(version, absolute) or []
                    elf_addrs = [a for a, _ in elf_syms]
                i = bisect.bisect_right(elf_addrs, addr) - 1 if elf_syms else -1
                if i >= 0:
                    base, sym = elf_syms[i]
                    target = f"{sym} + 0x{addr - base:X}"
        else:
            for base, size, array in HW_REGIONS:
                if base <= addr < base + size:
                    target = f"{array} + 0x{addr - base:X}"
        if target is None:
            unresolved.append((name, addr))
            continue
        lines.append(f"{name} = {target};")
    if unresolved:
        for name, addr in unresolved:
            print(f"error: cannot resolve {name} = 0x{addr:08X}", file=sys.stderr)
        sys.exit("error: build the GBA ROM first (tools/vita/check_gba_match.sh) to resolve ROM aliases")
    path.write_text("\n".join(lines) + "\n")


def write_romxlate(path, version, gba_objs, aliases):
    """C table mapping GBA ROM addresses of global data symbols to their Vita
    addresses, for GbaPtr() to translate pointers stored in binary ROM data.
    Only symbols defined by units the Vita build links are listed; functions
    are left out (their Thumb bit would make the offsets ambiguous)."""
    elf = Path(f"build/{version}/com_{version}.elf")
    out = subprocess.check_output(["arm-none-eabi-readelf", "-sW", str(elf)], text=True)
    kinds = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 8 and parts[4] == "GLOBAL":
            kinds[parts[7]] = (int(parts[1], 16), parts[3])
    linked = set(aliases)
    existing = [o for o in gba_objs if Path(o).exists()]
    for i in range(0, len(existing), 200):
        nm = subprocess.check_output(["arm-none-eabi-nm", "-g", "--defined-only", *existing[i:i + 200]], text=True)
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3:
                linked.add(parts[2])
    entries = {}
    for name, (addr, kind) in kinds.items():
        if name not in linked or kind == "FUNC" or not (0x08000000 <= addr < 0x0A000000):
            continue
        if addr not in entries or name < entries[addr]:
            entries[addr] = name
    rows = sorted(entries.items())
    lines = ["/* Generated by tools/vita/configure_vita.py. */", ""]
    lines += [f"extern char {name}[];" for _, name in rows]
    lines += ["", "const struct { unsigned gba; const void* host; } gRomXlate[] = {"]
    lines += [f"    {{ 0x{addr:08X}, {name} }}," for addr, name in rows]
    lines += ["};", f"const unsigned gRomXlateCount = {len(rows)};", ""]
    text = "\n".join(lines)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    return linked


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", choices=VERSIONS, default="us")
    parser.add_argument("--vitasdk", default=os.environ.get("VITASDK", str(Path.home() / "Developer/vitasdk")))
    args = parser.parse_args()
    version = args.version
    gba_build = f"build/{version}"
    out_dir = Path(f"build/vita/{version}")
    out_dir.mkdir(parents=True, exist_ok=True)
    sdk_bin = Path(args.vitasdk) / "bin"
    if not (sdk_bin / "arm-vita-eabi-gcc").exists():
        sys.exit(f"error: VitaSDK not found at {args.vitasdk} (set VITASDK)")

    symbols = parse_symbols(version)
    regional_plan = load_sidecars("config")["regions"][version]
    symbols.extend(asset_symbols(regional_plan, symbols))
    # Weak import stubs for optional kernel plugins (port/vita/stubs/*.yml).
    stub_dir = out_dir / "stubs"
    stub_dir.mkdir(parents=True, exist_ok=True)
    for yml in sorted(Path("port/vita/stubs").glob("*.yml")):
        env = dict(os.environ, VITASDK=args.vitasdk, PATH=f"{sdk_bin}:{os.environ['PATH']}")
        subprocess.run([str(sdk_bin / "vita-libs-gen"), str(yml), str(stub_dir)], check=True, env=env,
                       stdout=subprocess.DEVNULL)
        subprocess.run(["make", "-s", "-C", str(stub_dir)], check=True, env=env, stdout=subprocess.DEVNULL)

    romsyms = out_dir / "romsyms.ld"
    write_romsyms(romsyms, version, symbols)

    groups = asset_plan(version)
    units_file = Path(f"config/{version}/units.txt")
    listed = {l.split()[0] for l in units_file.read_text().splitlines() if l.strip() and not l.startswith("#")}
    groups = {g: v for g, v in groups.items() if not v["objects"] or any(u in listed for u in v["objects"])}
    generated = {u: (g, unit) for g, v in groups.items() for u, unit in v["objects"].items()}

    sources = {p.name: p for p in sorted(Path("src").rglob("*.c"))}
    units = []
    for line in units_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("@"):
            continue
        name = line.split()[0]
        if name in REPLACED_UNITS:
            continue
        if name in generated:
            src = Path(f"{gba_build}/gen") / name
            obj = out_dir / "gen" / (src.stem + ".o")
        elif name.endswith(".c"):
            src = sources.get(name, Path("src") / name)
            obj = out_dir / "src" / (src.stem + ".o")
        else:
            src = Path(f"asm/{version}") / name
            if not src.exists():
                src = Path("asm") / name
            obj = out_dir / "asm" / (src.stem + ".o")
        units.append((src, str(obj), None))
    units = materialize_assets(regional_plan, units, version, gba_build)
    gba_objs = [str(obj).replace(str(out_dir), gba_build, 1) for _src, obj, _flags in units]
    romxlate = out_dir / "romxlate.c"
    linked = write_romxlate(romxlate, version, gba_objs, [name for name, _ in symbols])
    linked_path = out_dir / "linked_symbols.txt"
    linked_path.write_text("\n".join(sorted(linked)) + "\n")

    port_game = sorted(Path("port/vita/game").glob("*.c"))
    port_host = sorted(Path("port/vita/host").glob("*.c"))
    headers = sorted(str(p) for p in Path("include").rglob("*.h"))

    port_version = Path("port/vita/VERSION").read_text().strip()
    major, minor, patch_level = port_version.split(".")
    vd = f"-DVERSION_{version.upper()} -DPORT_VERSION=\\\"{port_version}\\\""
    with open("build.vita.ninja", "w") as f:
        n = ninja_syntax.Writer(f)
        n.variable("ninja_required_version", "1.3")
        n.variable("builddir", str(out_dir))
        n.variable("cc", str(sdk_bin / "arm-vita-eabi-gcc"))
        n.variable("as", str(sdk_bin / "arm-vita-eabi-as"))
        n.variable("sdkbin", str(sdk_bin))
        n.variable("game_cflags", " ".join(GAME_CFLAGS + [vd, "-DPLATFORM_VITA", "-Iinclude",
                                                         f"-I{gba_build}/gen", "-Iport/vita/include"]))
        n.variable("port_cflags", " ".join(PORT_CFLAGS + [vd, "-DPLATFORM_VITA", "-Iport/vita/include"]))
        n.variable("asflags", f"-I . -I include --defsym VERSION_{version.upper()}=1")
        n.newline()
        n.rule("cc_game", "$cc $game_cflags -MMD -MF $out.d -c $in -o $out", depfile="$out.d", deps="gcc",
               description="CC $out")
        n.rule("cc_port", "$cc $port_cflags -MMD -MF $out.d -c $in -o $out", depfile="$out.d", deps="gcc",
               description="CC $out")
        n.rule("as", "$as $asflags -o $out $in", description="AS $out")
        n.rule("assetgen", "python3 tools/assetgen.py $version $manifest", description="ASSETGEN $manifest",
               restat=True)
        n.rule("link", "$cc -Wl,-q -Wl,--no-warn-rwx-segments -o $out @$out.rsp $libs"
               " && python3 tools/vita/fix_abs_symbols.py $out",
               rspfile="$out.rsp", rspfile_content="$in", description="LINK $out")
        n.rule("velf", "$sdkbin/vita-elf-create $in $out", description="VELF $out")
        n.rule("eboot", "$sdkbin/vita-make-fself -s -c $in $out", description="FSELF $out")
        n.rule("sfo", '$sdkbin/vita-mksfoex -s TITLE_ID=$titleid -s APP_VER=$appver -d ATTRIBUTE2=12 "$title" $out',
               description="SFO $out")
        n.rule("vpk", "$sdkbin/vita-pack-vpk -s $sfo -b $eboot $extra $out", description="VPK $out")
        n.newline()

        manifests = sorted(os.path.relpath(g["manifest"].path) for g in groups.values())
        for g in groups.values():
            outputs = [os.path.relpath(u["source"]) for u in g["objects"].values()] + [os.path.relpath(g["header"])]
            n.build(outputs, "assetgen",
                    implicit=manifests + ["tools/assetgen.py", "tools/sprite_sheet.py", "tools/gbagfx/gbagfx"]
                    + [os.path.relpath(p) for p in g["sources"]],
                    implicit_outputs=[os.path.relpath(p) for p in g["binaries"]],
                    variables={"version": version, "manifest": os.path.relpath(g["manifest"].path)})
        gen_headers = sorted(os.path.relpath(g["header"]) for g in groups.values())

        objs = []
        n.rule("relocate", f"python3 tools/vita/relocate_gen_c.py $in $out build/{version}/com_{version}.elf "
               f"{linked_path}", description="RELOC $out")
        for src, obj, _flags in units:
            src = Path(src)
            if src.suffix == ".c" and src.name in RELOCATED_GEN_C:
                # Byte-array tables that embed GBA pointers (see relocate_gen_c.py).
                fixed = str(out_dir / "reloc" / src.name)
                n.build(fixed, "relocate", str(src), implicit=["tools/vita/relocate_gen_c.py", str(linked_path)])
                n.build(obj, "cc_game", fixed, order_only=gen_headers)
            elif src.suffix == ".c":
                n.build(obj, "cc_game", str(src), order_only=gen_headers)
            else:
                deps = [os.path.relpath(p) for p in generated[src.name][1]["binaries"]] if src.name in generated else []
                n.build(obj, "as", str(src), implicit=deps)
            objs.append(obj)
        for src in port_game:
            obj = str(out_dir / "port" / (src.stem + ".o"))
            n.build(obj, "cc_game", str(src), order_only=gen_headers)
            objs.append(obj)
        for src in port_host:
            obj = str(out_dir / "port" / (src.stem + ".o"))
            n.build(obj, "cc_port", str(src))
            objs.append(obj)
        romxlate_obj = str(out_dir / "romxlate.o")
        n.build(romxlate_obj, "cc_port", str(romxlate))
        objs.append(romxlate_obj)
        for src in sorted(Path("port/vita").glob("*.S")) + sorted(Path("port/vita").glob("*.s")):
            obj = str(out_dir / "port" / (src.stem + ".o"))
            n.build(obj, "as", str(src))
            objs.append(obj)
        objs.append(str(romsyms))
        n.newline()

        libs = [f"-L{stub_dir}", "-lkubridge_stub_weak", "-lvitaGL", "-lvitashark", "-lSceShaccCgExt", "-lmathneon", "-ltaihen_stub",
                "-lSceShaccCg_stub", "-lSceKernelDmacMgr_stub", "-lSceGxm_stub", "-lSceDisplay_stub",
                "-lSceCtrl_stub", "-lSceTouch_stub", "-lSceAudio_stub", "-lSceAppMgr_stub",
                "-lSceAppUtil_stub", "-lSceCommonDialog_stub", "-lSceSysmodule_stub", "-lScePower_stub",
                "-lSceIofilemgr_stub", "-lstdc++", "-lm", "-lc"]
        elf = str(out_dir / "khcom.elf")
        velf = str(out_dir / "khcom.velf")
        eboot = str(out_dir / "eboot.bin")
        sfo = str(out_dir / "param.sfo")
        vpk = f"build/vita/khcom_{version}_v{port_version}.vpk"
        # Relink when vitaGL is rebuilt (e.g. with or without its splash screen).
        n.build(elf, "link", objs, implicit=[str(Path(args.vitasdk) / "arm-vita-eabi/lib/libvitaGL.a")],
                variables={"libs": " ".join(libs)})
        n.build(velf, "velf", elf)
        n.build(eboot, "eboot", velf)
        # APP_VER only holds XX.YY, so it carries the last two version fields.
        n.build(sfo, "sfo", implicit=["tools/vita/configure_vita.py", "port/vita/VERSION"],
                variables={"titleid": "KHCOM0001", "title": "Kingdom Hearts: Chain of Memories",
                           "appver": f"{int(minor):02d}.{int(patch_level):02d}"})
        extra = []
        for p in sorted(Path("port/vita/sce_sys").rglob("*")):
            if p.is_file():
                extra.append(f"-a {p}={p.relative_to('port/vita')}")
        n.build(vpk, "vpk", [eboot, sfo], variables={"sfo": sfo, "eboot": eboot, "extra": " ".join(extra)})
        n.build("vpk", "phony", vpk)
        # Re-run this script when the version or the script itself changes.
        n.rule("configure", "python3 tools/vita/configure_vita.py --version $version",
               generator=True, description="CONFIGURE")
        n.build("build.vita.ninja", "configure",
                implicit=["tools/vita/configure_vita.py", "port/vita/VERSION", "config/" + version + "/units.txt"],
                variables={"version": version})
        n.default("vpk")

    print(f"configured Vita build for {version}; run: ninja -f build.vita.ninja")


if __name__ == "__main__":
    main()
