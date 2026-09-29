# KH_COM_VITA

A native **PS Vita port of *Kingdom Hearts: Chain of Memories* (Game Boy Advance)**, built on top of the
community decompilation of the game ([Pheenoh/khcom](https://github.com/Pheenoh/khcom)).

The game's own C code is recompiled for the Vita's ARM CPU. The GBA hardware it talks to (video chip, DMA,
interrupts, sound mixer, BIOS, save memory) is replaced by a platform layer written for the Vita. This is **not
an emulator**: the game logic runs natively, and only the GBA's picture processor is reproduced in software.

> **This repository contains no game data and no decompiled game code.**
> It only holds the Vita platform layer, the build tools, and a patch of the port's changes to the public
> decompilation. To build it you need your **own legally dumped ROM** of the game (USA version).

## Status

Early and experimental (see `port/vita/VERSION`).

| Area | State |
|---|---|
| Boot, title screen, new game, story events, field | Working |
| Frame rate | Stable 60 fps on hardware (rendering takes ~3–6 ms per frame, split over two cores) |
| Battles | The first battle and its tutorial complete; later areas are being tested |
| Opening/ending movies (FMV) | Playing (the game's ARM decoder runs natively); Start skips a movie |
| Music and sound effects | Working: DirectSound channels and the GBA's four PSG ("Game Boy") channels |
| Saving | Working (`ux0:data/khcom/khcom.sav`), also written when the Vita suspends |

## Features

- **16:9 widescreen, full 960x544**: the scene is rendered 282 pixels wide instead of 240 (scaled x3.4), so it
  fills the Vita's screen and shows more of the stage. The game keeps only a 240-pixel window of each
  scrolling map in video memory, so the extra columns are drawn straight from the game's full map: the
  scenery really continues past the original edges, and sprites stay visible there. `fit` (original 3:2 with
  side bars) and `stretch` modes are also available.
- **Triangle = L + R** together (stock a card / sleight), without pressing both shoulder buttons.
  **Hold Triangle** to return the stocked cards to your hand (Sora and Riku), which the GBA version can't do.
- **Square = dodge roll**: rolls toward the direction you are holding, or the way the character faces. On the
  GBA this needs a double tap on the D-pad. Works for Sora and Riku.
- Left analog stick works as the D-pad.
- Uses three CPU cores: game logic on core 0 and rendering on cores 1 and 2 (audio also runs on core 2).

### Controls

| Vita | GBA |
|---|---|
| D-pad / left stick | D-pad |
| Cross / Circle | A / B (swap with `swap_ab=1`) |
| L / R | L / R |
| Triangle | L + R (stock / sleight); hold: unstock the cards |
| Square | Dodge roll |
| Start / Select | Start / Select |

## Installing on the Vita

1. **`libshacccg.suprx` is required** (the Vita's shader compiler, used by vitaGL). Extract it on your own
   console following
   [this guide](https://samilops2.gitbook.io/vita-troubleshooting-guide/shader-compiler/extract-libshacccg.suprx);
   it must end up at `ur0:data/libshacccg.suprx`. It is Sony software and is not distributed here.
2. Copy the built `khcom_us_vX.XX.XX.vpk` to the Vita (e.g. with VitaShell over FTP/USB) and install it.
3. **Recommended: the [kubridge](https://github.com/bythos14/kubridge/releases) kernel plugin**
   (`ur0:tai/kubridge.skprx` under `*KERNEL` in `ur0:tai/config.txt`, then reboot). With it, the port handles
   the game's reads/writes through NULL pointers and raw GBA addresses the way the GBA does instead of
   crashing (see "How it works"). Without it the game still runs, but those spots crash. The log says
   `fault handler: active` when it is in use.
4. Run it. On first launch it creates `ux0:data/khcom/`:

| File | Purpose |
|---|---|
| `config.ini` | `display=wide\|fit\|stretch`, `filter=linear\|nearest`, `swap_ab=0\|1` |
| `khcom.sav` | Save data (the GBA's SRAM) |
| `log.txt` | Startup trace and a status line with frame timings every second |
| `log_prev.txt` | The previous run's log (kept when you relaunch after a crash) |

You do not copy the ROM to the Vita: the game's data is built into the VPK.

## Building

Tested on macOS (Apple Silicon). Linux works with the equivalent packages.

### Requirements

- [VitaSDK](https://vitasdk.org) with **vitaGL**:
  ```sh
  git clone https://github.com/Rinnegatamante/vitaGL && cd vitaGL
  make HAVE_GLSL_SUPPORT=1 && make install
  ```
  For the Vita3K emulator, build it with `NO_SPLASHSCREEN=1` instead: the vitaGL splash screen uses a
  second GPU context, which Vita3K does not support.
- `git`, `ninja`, `python3` with `pyyaml` and `pillow` (LiveArea images)
- `arm-none-eabi-binutils` (used for the reference GBA build)
- `libpng` and `pkg-config` (for the decomp's `gbagfx` tool)
- [agbcc](https://github.com/pret/agbcc) and the decomp's legacy toolchain (see step 3)
- Your own ROM dump: **Kingdom Hearts: Chain of Memories (USA)**, `B8CE`, SHA-1
  `10729bd884f8fdca7a310b6d606c52e46657aa48`

### 1. Set up the source tree

```sh
git clone https://github.com/ggdrn/KH_COM_VITA.git
cd KH_COM_VITA
scripts/setup.sh              # creates ./khcom: the decomp at the right commit + this port
cd khcom
python3 -m venv .venv && .venv/bin/pip install pyyaml pillow -r requirements.txt
export PATH="$PWD/.venv/bin:$PATH"
cp /path/to/your/rom.gba roms/B8CE.gba
```

### 2. Extract the game's assets from your ROM

```sh
sh tools/fetch_gbagfx.sh      # builds gbagfx (needs libpng)
python3 tools/extract_assets.py
```

### 3. Build the reference GBA ROM

The Vita build reads the GBA build's symbol table to map ROM addresses to their new locations, so the
original ROM must be built once. Follow the decomp's README to install agbcc and run
`python3 tools/setup_legacy_toolchain.py`, then:

```sh
tools/vita/check_gba_match.sh # must print "OK: build/us/com_us.gba matches"
```

Notes for macOS:
- The legacy binutils 2.10 build breaks if the path contains **spaces**. Build it in a directory without
  spaces and copy `tools/legacy/` back.
- On 64-bit hosts the legacy assembler fails on `m4a_1.s` and the legacy linker cannot link the runtime
  libraries. `check_gba_match.sh` works around both by assembling that file with the modern
  `arm-none-eabi-as` and linking with `arm-none-eabi-ld`. The result still matches the original ROM.

### 4. Build the Vita port

```sh
export VITASDK=/path/to/vitasdk     # or install it at ~/Developer/vitasdk
python3 tools/vita/configure_vita.py
ninja -f build.vita.ninja
```

The VPK is written to `build/vita/khcom_us_vX.XX.XX.vpk`. After the first configure, `ninja` re-runs the
configure step by itself when `port/vita/VERSION` or the build script changes.

## How it works

| Piece | Where | What it does |
|---|---|---|
| Build | `tools/vita/configure_vita.py` | Compiles the decomp's C with `PLATFORM_VITA` and agbcc-compatible struct layout (`-mstructure-size-boundary=32`, 4-byte-aligned 64-bit integers); assembles the data units as-is; replaces the GBA-only assembly (crt0, BIOS calls, m4a mixer) with the platform layer |
| ROM symbols | `configure_vita.py`, `fix_abs_symbols.py` | Fixed ROM addresses from `symbols.txt` become relocatable linker aliases; a generated table (`romxlate.c`) maps GBA ROM addresses to Vita addresses for pointers stored in binary data |
| Pointer tables | `tools/vita/relocate_gen_c.py` | Rewrites generated byte-array tables that embed GBA pointers into relocatable form |
| Hardware | `port/vita/game/gba_system.c` | Emulated IO registers, palette, VRAM, OAM and SRAM; DMA; interrupt dispatch; BIOS calls; the per-frame scanline/IRQ loop |
| Sound | `port/vita/game/m4a_port.c`, `host/vita_audio.c` | The m4a sequencer and mixer (originally ARM/Thumb assembly) rewritten in C, resampled to 48 kHz for `sceAudioOut` |
| Video | `port/vita/host/ppu.c`, `vita_render.c`, `vita_video.c` | Scanline renderer for the GBA picture processor (tile/affine/bitmap backgrounds, sprites, windows, blending, mosaic), run on two cores and presented with vitaGL |
| Widescreen | `port/vita/game/widescreen.c`, `ppu.c` | Captures each streamed map's camera after VBlank; the renderer fetches the widescreen margins from the full map |
| Input | `port/vita/host/vita_input.c`, dodge hooks in `src/btl/btl.c` | Button mapping, Triangle → L+R, Square → dodge roll |
| GBA-style faults | `port/vita/host/vita_fault.c` | With kubridge, a data-abort handler decodes the faulting Thumb-2 load/store and performs it as the GBA would: BIOS-region reads return the BIOS open-bus value, writes there are dropped, raw GBA addresses are translated. Each emulated site is logged once |
| Freeze watchdog | `port/vita/host/vita_fault.c` | If the game loop stops for 5 s, logs the state and forces a crash dump so a freeze can be located |
| Decomp changes | `patches/khcom-vita.patch` | Hardware addresses routed through macros that still expand to the original values on GBA, plus Vita-only fixes. The GBA build still produces a byte-identical ROM |

## Debugging

- `ux0:data/khcom/log.txt` shows the startup steps, every game mode change and per-second timings
  (`logic`, `render`, `present`, worst frame), plus `fault:` lines for accesses emulated by the fault handler
  and `watchdog:` lines when the game freezes. `FaultInit=` in the log gives the module load address.
- When the game crashes the Vita writes `ux0:data/psp2core-*.psp2dmp`. Wait until the file no longer ends in
  `.tmp` before copying it (a partial `.tmp` still holds the thread registers), then run:
  ```sh
  python3 tools/vita/parse_core.py psp2core-....psp2dmp
  ```
  It prints each thread's registers and the source line of the crash.

## Legal

Kingdom Hearts is © Disney and Square Enix. This project is not affiliated with them. No ROM, extracted asset
or decompiled game code is included in this repository. The decompilation it builds on is a separate project
([Pheenoh/khcom](https://github.com/Pheenoh/khcom), CC0), fetched by `scripts/setup.sh`. You must own the game
and dump your own copy to build this port.

## License

The code in this repository (the Vita platform layer, the build tools and the patch) is released under the
[MIT License](LICENSE). It does not cover the game, its data, or the decompilation, which keeps its own CC0
license.
