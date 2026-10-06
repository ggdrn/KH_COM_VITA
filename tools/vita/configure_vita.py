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
from textgen import load_pools as load_text_pools  # noqa: E402

VERSIONS = {"us": "B8CE", "jp": "B8CJ", "eu": "B8CP"}

# GBA code units replaced by the platform layer.
REPLACED_UNITS = {
    "header.s",
    "crt0.s",
    "libagbsyscall.s",
    "m4a_1.s",
    "transform_veneers.s",
}
# ARM code that runs unchanged on the Vita's Cortex-A9.
NATIVE_ARM_UNITS = {"transform.s"}

# ROM data units are moved to writable sections of their own (keeping the
# GBA's split between .text, .rodata and .data blobs), so their contents can
# be filled in from the user's ROM at startup (tools/vita/strip_rom_data.py,
# port/vita/host/vita_rom.c).
ROM_SECTIONS = {".text": ".romtext", ".rodata": ".romrodata", ".data": ".romdata"}

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
        # Only global symbols: a linker script cannot name a file-local static.
        if len(parts) != 3 or not parts[1].isupper() or parts[1] == "A" or parts[2] in absolute \
                or parts[2].startswith("."):
            continue
        addr = int(parts[0], 16)
        if 0x08000000 <= addr < 0x0A000000:
            syms.append((addr, parts[2]))
    syms.sort()
    return syms


def write_romsyms(path, version, symbols):
    absolute = {name for name, _ in symbols}
    blobs = blob_ranges(version)
    # An address right at the end of a data unit (e.g. a codec's end marker)
    # that is not the start of another data unit is made relative to that unit,
    # not to the code that follows it in the ROM: the data units live in their
    # own sections on the Vita, away from the code.
    data_secs = [v for obj, secs in gba_sections(version).items() if "/gen/" in obj or "/asm/" in obj
                 for v in secs.values()]
    unit_starts = {start for start, _ in data_secs}
    unit_ends = {start + size for start, size in data_secs} - unit_starts
    elf_syms = None
    lines = []
    unresolved = []
    for name, addr in symbols:
        target = None
        if addr < 0x02000000:
            target = f"0x{addr:X}"
        elif 0x08000000 <= addr < 0x0A000000:
            for start, end, label in blobs:
                if start <= addr < end or (addr == end and addr in unit_ends):
                    target = f"{label} + 0x{addr - start:X}"
                    break
            else:
                if elf_syms is None:
                    elf_syms = gba_elf_symbols(version, absolute) or []
                    elf_addrs = [a for a, _ in elf_syms]
                if addr in unit_ends:
                    i = bisect.bisect_left(elf_addrs, addr) - 1 if elf_syms else -1
                else:
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


def align_blob_unit(src, dst_dir, anchors):
    """Copy of a ROM data unit whose sections start at the same address modulo
    16 as on the GBA.

    Tables inside the units are word aligned and read with LDM/LDRD, which
    fault on unaligned addresses on the Vita, and some blobs start at odd GBA
    addresses (e.g. map_room_tables at 0x0984C3CF). `anchors` maps each GBA
    section of the unit that needs it to its GBA start address; a prelude pads
    each one, then switches back to .text, the assembler's default, so the
    unit's own directives work unchanged. Returns the path to assemble, or
    None when the unit needs no change."""
    if not anchors:
        return None
    prelude = ""
    for sec, start in sorted(anchors.items()):
        prelude += f"\t.section {sec}\n\t.balign 16\n\t.space {start & 15}\n"
    out = prelude + "\t.text\n" + Path(src).read_text()
    dst = Path(dst_dir) / Path(src).name
    if not dst.exists() or dst.read_text() != out:
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(out)
    return str(dst)


def gba_sections(version):
    """{GBA object path: {section: (start, size)}} from the GBA link map."""
    rx = re.compile(r"^ (\.\w+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (\S+\.o)$")
    out = {}
    for line in Path(f"build/{version}/com_{version}.map").read_text().splitlines():
        m = rx.match(line)
        if m and int(m[3], 16):
            out.setdefault(m[4], {})[m[1]] = (int(m[2], 16), int(m[3], 16))
    return out


def unit_anchors(units, version, gba_build, out_dir):
    """{Vita object: {GBA section: GBA start}} for the data unit sections that
    need an alignment anchor.

    A section that directly follows another data unit's in the GBA ROM needs
    none: code sometimes reaches past the end of one table into the next unit
    (a palette at gUnk_099910C4 + 0x240 lives in the following unit), so those
    stay back to back on the Vita too, and inherit the previous unit's
    alignment."""
    sections = gba_sections(version)
    last = {}  # section -> (end address, is a data unit)
    out = {}
    for src, obj, _flags in units:
        gba_obj = str(obj).replace(str(out_dir), gba_build, 1)
        is_data = Path(src).suffix != ".c"
        for sec, (start, size) in sorted(sections.get(gba_obj, {}).items(), key=lambda kv: kv[1][0]):
            prev = last.get(sec)
            if is_data and sec in (".text", ".rodata", ".data") and not (prev and prev[1] and prev[0] == start):
                out.setdefault(str(obj), {})[sec] = start
            last[sec] = (start + size, is_data)
    return out


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


def configure_version(version, args):
    """Writes build.vita.<version>.ninja; returns the files the VPK takes from it."""
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
    text_pools = load_text_pools()
    text_objects = {p.object(version)["name"]: p for p in text_pools if p.object(version) is not None}

    sources = {p.name: p for p in sorted(Path("src").rglob("*.c"))}
    units = []
    for line in units_file.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line.startswith("@"):
            continue
        name = line.split()[0]
        if name in REPLACED_UNITS:
            continue
        if name in generated or name in text_objects:
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
    write_romxlate(romxlate, version, gba_objs, [name for name, _ in symbols])

    port_game = sorted(Path("port/vita/game").glob("*.c"))
    port_host = sorted(Path("port/vita/host").glob("*.c"))
    headers = sorted(str(p) for p in Path("include").rglob("*.h"))
    # As configure.py: game code includes the per-subsystem header folders directly.
    include_dirs = ["include"] + sorted(str(p) for p in Path("include").iterdir() if p.is_dir() and p.name != "gba")

    port_version = Path("port/vita/VERSION").read_text().strip()
    major, minor, patch_level = port_version.split(".")
    vd = f"-DVERSION_{version.upper()} -DPORT_VERSION=\\\"{port_version}\\\""
    with open(f"build.vita.{version}.ninja", "w") as f:
        n = ninja_syntax.Writer(f)
        n.variable("ninja_required_version", "1.3")
        n.variable("builddir", str(out_dir))
        n.variable("cc", str(sdk_bin / "arm-vita-eabi-gcc"))
        n.variable("as", str(sdk_bin / "arm-vita-eabi-as"))
        n.variable("sdkbin", str(sdk_bin))
        n.variable("game_cflags", " ".join(GAME_CFLAGS + [vd, "-DPLATFORM_VITA"] +
                                           [f"-I{d}" for d in include_dirs] +
                                           [f"-I{gba_build}/gen", "-Iport/vita/include"]))
        n.variable("port_cflags", " ".join(PORT_CFLAGS + [vd, "-DPLATFORM_VITA", "-Iport/vita/include"]))
        n.variable("asflags", f"-I . -I include --defsym VERSION_{version.upper()}=1")
        n.newline()
        n.rule("cc_game", "$cc $game_cflags -MMD -MF $out.d -c $in -o $out", depfile="$out.d", deps="gcc",
               description="CC $out")
        # Decomp C: constant tables go to a writable section, so the ones equal
        # to the ROM can be filled in from it at startup as well.
        n.rule("cc_game_rom", "$cc $game_cflags -MMD -MF $out.d -c $in -o $out && "
               "$sdkbin/arm-vita-eabi-objcopy --rename-section .rodata=.gamerodata,alloc,load,contents,data $out",
               depfile="$out.d", deps="gcc", description="CC $out")
        n.rule("cc_port", "$cc $port_cflags -MMD -MF $out.d -c $in -o $out", depfile="$out.d", deps="gcc",
               description="CC $out")
        n.rule("as", "$as $asflags -o $out $in", description="AS $out")
        rename = " ".join(f"--rename-section {a}={b},alloc,load,contents,data" for a, b in ROM_SECTIONS.items())
        n.rule("as_rom", f"$as $asflags -o $out $in && $sdkbin/arm-vita-eabi-objcopy {rename} $out",
               description="AS $out")
        n.rule("assetgen", "python3 tools/assetgen.py $version $manifest", description="ASSETGEN $manifest",
               restat=True)
        n.rule("textgen", "python3 tools/textgen.py $version $manifest", description="TEXTGEN $manifest",
               restat=True)
        n.rule("link", "$cc -Wl,-q -Wl,--no-warn-rwx-segments -Wl,-Map=$out.map -o $out @$out.rsp $libs"
               " && python3 tools/vita/fix_abs_symbols.py $out",
               rspfile="$out.rsp", rspfile_content="$in", description="LINK $out")
        n.rule("velf", "$sdkbin/vita-elf-create $in $out", description="VELF $out")
        n.rule("strip_rom", f"python3 tools/vita/strip_rom_data.py $in $in.map build/{version}/com_{version}.map "
               f"roms/{VERSIONS[version]}.gba $out $rommap gGbaIo build/{version}/com_{version}.elf",
               description="STRIP ROM DATA $out")
        n.rule("eboot", "$sdkbin/vita-make-fself -s -c $in $out", description="FSELF $out")
        n.rule("sfo", '$sdkbin/vita-mksfoex -s TITLE_ID=$titleid -s APP_VER=$appver -d ATTRIBUTE2=12 "$title" $out',
               description="SFO $out")
        n.rule("vpk", "$sdkbin/vita-pack-vpk -s $sfo -b $eboot $extra $out", description="VPK $out")
        n.newline()

        manifests = sorted(os.path.relpath(g["manifest"].path) for g in groups.values())
        for g in groups.values():
            outputs = [os.path.relpath(u["source"]) for u in g["objects"].values()] + [os.path.relpath(g["header"])]
            n.build(outputs, "assetgen",
                    implicit=manifests + ["tools/assetgen.py", "tools/m4a_assets.py", "tools/sprite_sheet.py",
                                          "tools/gbagfx/gbagfx"]
                    + [os.path.relpath(p) for p in g["sources"]],
                    implicit_outputs=[os.path.relpath(p) for p in g["binaries"]],
                    variables={"version": version, "manifest": os.path.relpath(g["manifest"].path)})
        for pool in text_pools:
            n.build([os.path.relpath(p) for p in pool.outputs(version)], "textgen",
                    implicit=[os.path.relpath(pool.path), f"config/charmaps/{version}.yaml", "tools/textgen.py"]
                    + ([os.path.relpath(pool.source(version))] if pool.present(version) else []),
                    variables={"version": version, "manifest": os.path.relpath(pool.path)})
        # Generated headers and the text fragments C sources #include.
        gen_headers = sorted([os.path.relpath(g["header"]) for g in groups.values()]
                             + [os.path.relpath(p) for pool in text_pools
                                for p in [pool.header(version)] + pool.fragment_paths(version)])

        objs = []
        anchors = unit_anchors(units, version, gba_build, out_dir)
        for src, obj, _flags in units:
            src = Path(src)
            if src.suffix == ".c":
                n.build(obj, "cc_game_rom", str(src), order_only=gen_headers)
            else:
                deps = [os.path.relpath(p) for p in generated[src.name][1]["binaries"]] if src.name in generated else []
                aligned = align_blob_unit(src, out_dir / "aligned", anchors.get(str(obj)))
                rule = "as" if src.name in NATIVE_ARM_UNITS else "as_rom"
                n.build(obj, rule, aligned or str(src), implicit=deps)
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
        vpk = None
        # Relink when vitaGL is rebuilt (e.g. with or without its splash screen).
        n.build(elf, "link", objs, implicit=[str(Path(args.vitasdk) / "arm-vita-eabi/lib/libvitaGL.a")],
                variables={"libs": " ".join(libs)})
        # The ROM's data leaves the executable; the player's ROM supplies it at startup.
        stripped = str(out_dir / "khcom.stripped.elf")
        rommap = str(out_dir / "rommap.bin")
        n.build(stripped, "strip_rom", elf, implicit=["tools/vita/strip_rom_data.py", f"build/{version}/com_{version}.map",
                                                      f"roms/{VERSIONS[version]}.gba"],
                implicit_outputs=[rommap], variables={"rommap": rommap})
        n.rule("audit_rom", f"python3 tools/vita/audit_rom_free.py $in roms/{VERSIONS[version]}.gba $out",
               description="AUDIT $in")
        audit = str(out_dir / "rom_audit.txt")
        n.build(audit, "audit_rom", stripped, implicit=["tools/vita/audit_rom_free.py"])
        n.build(velf, "velf", stripped, order_only=[audit])
        n.build(eboot, "eboot", velf)
        # APP_VER only holds XX.YY, so it carries the last two version fields.
        n.build(sfo, "sfo", implicit=["tools/vita/configure_vita.py", "port/vita/VERSION"],
                variables={"titleid": "KHCOM0001", "title": "Kingdom Hearts: Chain of Memories",
                           "appver": f"{int(minor):02d}.{int(patch_level):02d}"})
        # LiveArea images, converted to the sizes and 8-bit palette format the
        # Vita requires (tools/vita/livearea.py); hidden files are left out.
        sce_src = [p for p in sorted(Path("port/vita/sce_sys").rglob("*"))
                   if p.is_file() and not any(part.startswith(".") for part in p.relative_to("port/vita").parts)]
        sce_out = [str(out_dir / p.relative_to("port/vita")) for p in sce_src]
        n.rule("livearea", "python3 tools/vita/livearea.py port/vita/sce_sys $outdir", description="LIVEAREA")
        n.build(sce_out, "livearea", implicit=[str(p) for p in sce_src] + ["tools/vita/livearea.py"],
                variables={"outdir": str(out_dir / "sce_sys")})
        extra = [f"-a {o}={p.relative_to('port/vita')}" for p, o in zip(sce_src, sce_out)]
        extra.append(f"-a {rommap}=rommap.bin")
        n.build(f"eboot_{version}", "phony", [eboot, rommap])

    return {"eboot": eboot, "sfo": sfo, "rommap": rommap, "sce": list(zip(sce_src, sce_out)),
            "units": f"config/{version}/units.txt", "vpk": vpk}


def main():
    """One VPK for every version whose ROM is in roms/: the USA executable
    starts and hands over to eboot_eu.self for the European ROM
    (port/vita/host/vita_rom.c)."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--vitasdk", default=os.environ.get("VITASDK", str(Path.home() / "Developer/vitasdk")))
    args = parser.parse_args()
    versions = [v for v in ("us", "eu") if Path(f"roms/{VERSIONS[v]}.gba").exists()]
    if "us" not in versions:
        sys.exit("error: roms/B8CE.gba (USA) is required; the European ROM (roms/B8CP.gba) is optional")
    parts = {v: configure_version(v, args) for v in versions}
    sdk_bin = Path(args.vitasdk) / "bin"
    port_version = Path("port/vita/VERSION").read_text().strip()
    us = parts["us"]
    vpk = f"build/vita/khcom_v{port_version}.vpk"
    with open("build.vita.ninja", "w") as f:
        n = ninja_syntax.Writer(f)
        n.variable("ninja_required_version", "1.3")
        n.variable("sdkbin", str(sdk_bin))
        for v in versions:
            n.subninja(f"build.vita.{v}.ninja")
        n.rule("vpk", "$sdkbin/vita-pack-vpk -s $sfo -b $eboot $extra $out", description="VPK $out")
        # Everything at the root: the USA executable, LiveArea and ROM map, and
        # each other version's executable (eboot_<v>.self, started with
        # sceAppMgrLoadExec, which refuses SELF paths in subfolders) and ROM map
        # (rommap_<v>.bin).
        extra = [f"-a {o}={p.relative_to('port/vita')}" for p, o in us["sce"]]
        extra.append(f"-a {us['rommap']}=rommap.bin")
        implicit = [o for _p, o in us["sce"]] + [us["rommap"]]
        for v in versions:
            if v != "us":
                extra += [f"-a {parts[v]['eboot']}=eboot_{v}.self", f"-a {parts[v]['rommap']}=rommap_{v}.bin"]
                implicit += [parts[v]["eboot"], parts[v]["rommap"]]
        n.build(vpk, "vpk", [us["eboot"], us["sfo"]], implicit=implicit,
                variables={"sfo": us["sfo"], "eboot": us["eboot"], "extra": " ".join(extra)})
        n.build("vpk", "phony", vpk)
        # Re-run this script when the version or the script itself changes.
        n.rule("configure", "python3 tools/vita/configure_vita.py", generator=True, description="CONFIGURE")
        n.build("build.vita.ninja", "configure",
                implicit=["tools/vita/configure_vita.py", "port/vita/VERSION"] + [parts[v]["units"] for v in versions])
        n.default("vpk")

    print(f"configured Vita build for {', '.join(versions)}; run: ninja -f build.vita.ninja")


if __name__ == "__main__":
    main()
