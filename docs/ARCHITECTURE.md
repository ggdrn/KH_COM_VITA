# Code guide

How the PS Vita port is put together, where each piece lives and how to change it safely. The README covers
installing and building; this is for working on the code.

## Two directories

| Directory | What it is |
|---|---|
| `KH-COM-VITA/` (this repository) | The port's own code and the patch of its changes to the decomp. This is what gets published. |
| `khcom/` | A working build tree: the decompilation ([Pheenoh/khcom](https://github.com/Pheenoh/khcom)) with the patch applied and `port/vita`, `tools/vita` copied in. Builds and ROM live only here. |

- `scripts/setup.sh` creates a `khcom/` from this repository (clone the decomp at the pinned commit, apply the
  patch, copy the port in).
- `scripts/export_from_khcom.sh` goes the other way: after working in `khcom/`, it regenerates
  `patches/khcom-vita.patch` and copies `port/vita` and `tools/vita` back here. Review with `git diff`, then
  commit here.

The code is compiled in `khcom/` because the port links the decomp's own C files, headers and generated
assets.

## Map of the code

### `port/vita/` — the platform layer

The game's code calls GBA hardware and BIOS functions; these files provide them on the Vita.

**Game side** (`game/`, compiled with the game's flags and headers):

| File | Role |
|---|---|
| `gba_system.c` | Emulated GBA memory (IO registers, palette, VRAM, OAM, SRAM), BIOS calls (`CpuSet`, `LZ77UnComp`, …), DMA, interrupt dispatch, `GbaPtr()` (GBA address → Vita pointer), and `VBlankIntrWait()`: the per-frame loop that walks the 160 scanlines, raises HBlank/VCount/VBlank interrupts and hands each line to the renderer. |
| `m4a_port.c` | The m4a sound engine's sequencer and mixer (originally ARM assembly), rewritten in C. |
| `widescreen.c` | Tells the renderer which backgrounds continue past the original 240 columns (streamed maps and the card-built field rooms) and where their camera is. |

**Host side** (`host/`, plain Vita code):

| File | Role |
|---|---|
| `vita_main.c` | `main()`: clocks, config, save files (5 banks, written safely within a second of a save), logging, frame pacing, then `AgbMain()` (the game). |
| `vita_rom.c` | Startup: finds the player's ROM, checks its SHA-1 and copies the game's data into the executable (see "ROM data"). |
| `ppu.c` | Software renderer of the GBA picture processor: tile/affine/bitmap backgrounds, sprites, windows, blending, mosaic, widescreen margins. |
| `vita_render.c` | Captures each frame's registers and memory and renders it on two worker threads (cores 1 and 2); with Smooth edges on, the same threads then enlarge it with Scale3x straight into texture memory. |
| `scale3x.c` | The Scale3x pixel-art upscaler (CPU), line by line through a cached buffer. |
| `vita_video.c` | vitaGL: uploads the rendered frame and draws it at 960×544 with the sharp-pixels / GBA-colours shader; draws the port's overlays (menu, moogle points notice, unstock bar). |
| `vita_menu.c` | The port menu (Start + L + R): PICTURE, CONTROLS and GAME tabs, saved to `config.ini`. |
| `vita_notice.c` | Moogle points notice on the field; the unstock progress bar under the stocked cards. |
| `vita_text.c` | 5×7 pixel font for the port's own screens. |
| `vita_audio.c`, `psg.c` | Mixes the m4a output with the four "Game Boy" PSG channels and resamples to 48 kHz for `sceAudioOut`. |
| `vita_input.c` | Button mapping and the control additions (Triangle → L+R, Square → dodge roll, right stick → card controls, rear touch / right stick held → unstock), each switchable in the menu. |
| `vita_fault.c` | Optional kubridge fault handler: performs reads/writes through NULL or raw GBA addresses the way the GBA does; freeze watchdog. |
| `vita_codemem.c` | Executable memory for the movie decoder (ARM code the game copies to RAM and runs). |

**Headers** (`include/`): `port.h` is the boundary between game and host code (primitive types only);
`vita_host.h` is host-internal; `ppu.h` is the renderer's frame format.

### `tools/vita/` — build tools

| File | Role |
|---|---|
| `configure_vita.py` | Generates `build.vita.ninja`: which units to compile, flags, section layout, the whole pipeline up to the VPK. |
| `check_gba_match.sh` | Builds the GBA ROM from the same tree and checks it is byte-identical to the original. Run after touching decomp code. |
| `strip_rom_data.py` | After linking: zeroes every byte that comes from the ROM and writes `rommap.bin`. |
| `audit_rom_free.py` | Fails the build if pieces of the ROM can still be found in the executable. |
| `fix_abs_symbols.py` | Makes the linker's absolute ROM aliases relocatable (vita-elf-create would not relocate them). |
| `rewrite_hwaddr.py` | One-time rewrite of hardware address literals in the decomp into `HW_*` macros. |
| `livearea.py` | Converts the LiveArea images to the sizes and palette format the Vita requires. |
| `parse_core.py` | Reads a Vita crash dump (`.psp2dmp`) and prints registers and source lines. |
| `fault_sites.py` | Maps the `fault:` lines of `log.txt` to source lines. |

### `patches/khcom-vita.patch` — changes to the decomp

Every change is inside `#ifdef PLATFORM_VITA` (or a macro that expands to the original code on GBA), so
the GBA build still produces the original ROM. Two kinds:

1. **Mechanical, across many files:** hardware addresses written as `HW_VRAM(...)`, `HW_PLTT(...)`, … and DMA
   starts as `DMA_READBACK(...)` (`include/gba/hwaddr.h`, `include/gba/io_reg.h`), so they reach the emulated
   hardware.
2. **Behaviour, where the Vita differs from the GBA:**

| File | Why |
|---|---|
| `src/main.c` | Work RAM is cleared by the Vita, not by DMA; interrupt vector; startup trace. |
| `src/map/map_cell.c` | Reads of cells outside the map return the GBA's BIOS open-bus value (`MapGetCellBus`); field room tiles for the widescreen margins. |
| `src/map/map.c` | Field HP display (open/draw/close in the field tasks). |
| `src/btl/btl2.c` | HP display position; field HP display functions. |
| `src/btl/btl.c` | Dodge roll on Square. |
| `src/evt/mode_movie.c`, `src/lib/movie*.c`, `src/lib/snd_stream.c` | Movies: decoder in executable memory, pacing by VBlank, sound routed to the Vita, Start skips, sound-buffer overrun padding. |
| `src/card/card_worldselect.c` | Door card selection shown as a 240-column screen with black bars. |
| `src/card/card_map_anim.c`, `card_card.c`, `card_battle.c`, `card_riku_tutorial.c`, `btl_actor.c`, `engine.c` | Out-of-range or NULL reads that the GBA survives (see comments at each). |
| `src/lib/m4a2.c`, `m4a_catalog_data.c`, `include/m4a.h` | Pointers read from binary sound data go through `GBA_PTR()`. |
| `src/lib/agb_sram.c`, `include/save.h` | Save memory writes are flushed to `ux0:data/khcom/khcom.sav`. |
| `include/types.h` | 64-bit integers aligned like agbcc's so structs keep the GBA layout. |

## How a frame runs

1. The game's main loop (`AgbMain` in `src/main.c`) updates the current mode and calls `VBlankIntrWait()`.
2. `VBlankIntrWait()` (`gba_system.c`) walks the scanlines: raises VCount/HBlank interrupts, runs HBlank DMA
   and snapshots the IO registers of each line (`PortCaptureLine`).
3. The frame is handed to the render threads (`vita_render.c` → `ppu.c`), which draw it at 282×160 (16:9)
   while the game computes the next one.
4. `PortVBlankWait()` (`vita_main.c`) presents the previous frame with vitaGL, polls input, pumps audio and
   paces to 60 Hz; then the VBlank interrupt runs the game's VBlank handler (sound, sprite and VRAM uploads).

## ROM data

The game's graphics, text, music, movies and constant tables are not in the VPK:

- **Build** (`configure_vita.py`): the decomp's asset units are assembled into writable sections
  `.romtext`/`.romrodata`/`.romdata`, and the constant C tables into `.gamerodata`. Each unit keeps its GBA
  address modulo 16, and units that are back to back in the ROM stay back to back.
- **Strip** (`strip_rom_data.py`): every byte equal to the ROM is zeroed, except the pointer words the
  linker relocates. `rommap.bin` lists `(address, ROM offset, length)` runs. The tool checks each run against
  the ROM and proves the result by rebuilding the original executable from the stripped one plus the ROM.
- **Audit** (`audit_rom_free.py`): the build fails if ROM samples are still found in the executable.
- **Startup** (`vita_rom.c`): reads `ux0:data/khcom/rom.gba`, checks the SHA-1, copies every run back
  (adjusting for where the module was loaded), then starts the game.

## Conventions

- Decomp changes go inside `#ifdef PLATFORM_VITA`, with a comment saying what the GBA does and why the Vita
  needs something else. Run `tools/vita/check_gba_match.sh` afterwards: it must still print `OK`.
- Prefer reproducing the GBA's behaviour (open-bus values, ignored writes) over inventing new behaviour.
- Bump `port/vita/VERSION` for every build given to testers; the VPK is named after it.
- To locate a crash, use `parse_core.py` on the dump and `fault_sites.py` on the log of the same build.
