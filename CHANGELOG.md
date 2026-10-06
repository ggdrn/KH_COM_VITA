# Changelog — KH:COM Vita

A native PS Vita port of *Kingdom Hearts: Chain of Memories* (GBA, USA and Europe), built on the community
decompilation ([Pheenoh/khcom](https://github.com/Pheenoh/khcom)). The game's code runs natively on the
Vita; only the GBA's picture processor is reproduced in software.

Newest version first.

---

## v0.08.00 — European ROM, boss practice, new upscalers

### New
- **European ROM** (English, French, German, Italian, Spanish) in the same VPK, detected from the ROM;
  separate saves (`khcom_eu*.sav`).
- **Boss practice** (BOSSES tab): refight beaten bosses at the original strength or a chosen enemy level
  up to Sora's; no XP or rewards, back to where you were.
- **Tutorial replay** (TUTORIALS tab).
- **Scale2x and MMPX** smooth edges, for everything, sprites only or scenery only.
- **Clear stocked cards:** hold time 0.5–2 s, or double tap on the rear touch pad.
- **16:9 layouts** for the pause, save, door and level-up screens; more wide menus.

### Fixed
- Pause menu crash after the first Riku fight.
- Leon's tutorial softlocks.
- Heavy-scene slowdown (rendering and upscaling on three cores).
- The Start + L + R chord no longer opens the game's pause menu.

---

## v0.06.00 — Full-width dialogue boxes, boss arena and cutscene fixes, skip intro

### Fixed
- **Boss arenas no longer show black space at their edges:** in widescreen the battle camera stops a little
  earlier, so the whole view stays inside the arena.
- **No leftover bits of the dialogue box at the screen edges** during cutscenes and conversations.

### New
- **Dialogue boxes fill the 16:9 screen** once open: the rounded end and the open side move out to the
  edges and the frame's pattern fills the gap, with no distortion; the text and portrait stay in place.
- **Skip intro** (GAME tab, off by default): start straight at the title menu, without the logos and the
  title animation.

---

## v0.05.00 — 10 save slots, full-screen menus, safer saving

### Fixed
- **Saves are no longer lost when you close the game right after saving.** The save reaches the memory
  card within 1 s and is written to a temporary file first. If the app closes mid-write, the next launch
  finishes the job.
- **No more cyan flashes in the widescreen margins** during room transitions and when opening the door's
  card selection. The background colour the GBA hides no longer shows in the margins.
- **The HP display lines up with the card hand** in widescreen, both in the left margin.

### New
- **10 save slots:** 5 banks of the game's 2 slots (`khcom.sav`, `khcom2.sav` … `khcom5.sav`). Pick the bank
  in the port menu; switching takes effect immediately without restarting the game, and a new bank is
  formatted automatically. Bank 1 is still the `khcom.sav` from before, so emulator saves keep working.
- **GAME tab in the menu (Start + L + R):**
  - **Field HP display:** turn Sora's HP display outside battle on or off.
  - **Save bank.**
  - **Wide menus:** the pause, save, status and deck menus and the title's load screen are stretched to
    16:9 with even pixels, instead of showing black bars. Optional.
- **Logos and title screen:** the margins take each scene's background colour instead of black.

### Technical
- The log shows the name of each game screen (`ModeStart mode_title …`) and every save write.

---

## v0.04.00 — Options menu, new card controls, game-over fix

### Fixed
- **The game no longer freezes at game over.** The Continue screen was removed on its first frame, because
  of an original function with no `return`, and the game waited forever. The same kind of bug was fixed in
  two enemy card animations.

### New
- **Options menu (Start + L + R),** with the game paused and choices saved to `config.ini`:
  - **PICTURE:**
    - **Sharp pixels:** crisp pixels, all the same size at the 3.4× scale. On.
    - **GBA colors:** colours as on the GBA's screen. On.
    - **Smooth edges (Scale3x):** smooths the edges of the pixel art. Off.
  - **CONTROLS:** turn each of the port's extra controls on or off.
- **Right stick for the cards:**
  - left/right = L/R, to change card;
  - up = L+R, to stock a card or perform a sleight;
  - down held for 1 s = return the stocked cards.
- **Return the stocked cards to the hand (Sora):** hold the rear touch pad or the right stick down for 1 s.
  - The cards go back to their original places in the deck and the cursor does not move.
  - A **progress bar** under the stocked cards shows the hold.
- **Moogle points notice on the field:** the amount collected appears in the bottom-right corner, then the
  total.

### Technical
- Scale3x is computed by the render threads and written straight into texture memory.
- The XBR filter and the per-pixel GPU upscalers were removed, as they were too heavy.

---

## v0.02.03 — Movie fix

### Fixed
- **The game no longer freezes when a movie starts** (opening, after loading a save, endings). Moving the
  game's data out of the VPK had left the movie decoder with a wrong size.

---

## v0.02.02 — Sprites in the margins, HUD behind menus

### Fixed
- **Items, characters and effects stay visible in the widescreen margins** instead of vanishing at the
  GBA's original screen edge.
- **The HP display goes behind the pause and save menus** instead of covering them.

---

## v0.02.00 — Game data loaded from the player's ROM

### New
- **The VPK contains no game data.** Players put their own dump at `ux0:data/khcom/rom.gba`. The port
  checks its SHA-1 at launch and loads the graphics, text, music and movies from it. If the ROM is missing
  or wrong, a message explains what to do.
- **Sora's HP display also on the map,** outside battle. In battle the display moved down so the portrait
  isn't cut off.

### Removed
- Returning the cards by holding Triangle (it came back in v0.04.00 on the rear touch pad and the right
  stick).
- Dialogue boxes stretched to the screen edges.

### Technical
- The build zeroes every byte that comes from the ROM in the executable, writes `rommap.bin`, and fails if
  any piece of the ROM is left.

---

## v0.01.33 — Boss, map and graphics fixes

### Fixed
- **Crash at the first boss.**
- **Freeze when opening the map in Pooh's world.**
- **Wrong colours on the trampolines** (data that must stay adjacent in memory).
- **Misplaced selector hand and broken edges in the door's card selection,** which now has black bars.

### New
- New LiveArea images, converted automatically at build time.

---

## v0.01.27 — Movies, full sound and scenery in the margins

### New
- **Opening and ending movies play, with sound.** The original decoder runs natively. **Start skips a
  movie.**
- **The four "Game Boy" sound channels (PSG) play,** completing the music.
- **Widescreen in the card-built rooms** (e.g. Traverse Town): the scenery really continues into the
  margins.
- **The save is also written when the Vita suspends.**

### Fixed
- Crash when skipping a movie.
- Start in the opening movie reset the game.
- Enemies and barrels showing up white.

---

## v0.01.10 and earlier — The base of the port

- **Native port:** the game's C code, from the decompilation, recompiled for the Vita's ARM processor. The
  GBA hardware (registers, video, DMA, interrupts, BIOS, SRAM) is replaced by a layer written for the Vita.
  It is not an emulator.
- **Renderer for the GBA's picture processor** (tiled, affine and bitmap backgrounds, sprites, windows,
  transparency, mosaic), split across two CPU cores, at a **stable 60 fps**.
- **16:9 widescreen at 960×544,** with 282 columns instead of 240, showing more of each scene. `fit`
  (original 3:2) and `stretch` modes are also available.
- **Sound:** the m4a sound engine, originally in assembly, rewritten in C.
- **Controls:**
  - left stick as the D-pad;
  - **Triangle = L + R** (stock a card / sleight);
  - **Square = dodge roll**, which on the GBA takes a double tap on the D-pad;
  - option to swap A/B in `config.ini`.
- **Saves** in `ux0:data/khcom/khcom.sav`.
- **GBA-style fault handling** (with the kubridge plugin): reads and writes through null addresses behave
  as on the GBA instead of crashing.
- **Freeze watchdog:** if the game stops for 5 s, it writes the log and creates a dump for diagnosis.
- **Log** at `ux0:data/khcom/log.txt`, with each frame's timings.
