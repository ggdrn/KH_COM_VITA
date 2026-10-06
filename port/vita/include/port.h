#ifndef GUARD_PORT_H
#define GUARD_PORT_H

/*
 * Interface between the game side (compiled with the GBA struct layout flags)
 * and the host side (compiled with the Vita ABI). Only primitive types cross
 * this boundary.
 */

#include <stdint.h>

#define GBA_SCREEN_WIDTH 240
#define GBA_SCREEN_HEIGHT 160
/* Widest frame the renderer produces (16:9 at 160 lines, rounded up). */
#define PORT_MAX_SCREEN_WIDTH 288

/* Emulated GBA memory (defined on the game side). */
extern uint8_t gGbaIo[0x400];
extern uint8_t gGbaPltt[0x400];
extern uint8_t gGbaVram[0x18000];
extern uint8_t gGbaOam[0x400];
extern uint8_t gGbaSram[0x10000];

/* Game side ---------------------------------------------------------------- */

/* Entry point of the game (src/main.c). Never returns. */
void AgbMain(void);

/* Host side ---------------------------------------------------------------- */

/* One VBlank of 8-bit PCM from the m4a mixer (right = FIFO A, left = FIFO B). */
void PortAudioPush(const int8_t* right, const int8_t* left, int samples, int rate);
/* Bit n: PSG channel n+1 starts a new note in this sound frame. */
void PsgNewNotes(unsigned mask);
/* Executable memory for the FMV codecs (vita_codemem.c); write between
 * PortCodeBeginWrite() and PortCodeEndWrite(). */
void* PortCodeAlloc(uint32_t size);
void PortCodeFree(void* p);
void PortCodeBeginWrite(void);
void PortCodeEndWrite(void);
/* Streamed background map of one BG for the widescreen margins (see
 * port/vita/game/widescreen.c); map == NULL disables it. */
void PortSetBgStream(int bg, const void* const* map, int width, int height, int worldX, int worldY,
                     int shadowHofs, int shadowVofs);
void PortCaptureBgStreams(void);
/* Host side of PortBgPanelMap, every frame: `bg` shows a sliding panel. Its
 * margins stay empty, and while it spans the original screen (a message box
 * fully open) it is stretched to the edges of the wide screen (ppu.c). */
void PortSetBgPanel(int bg, int on);
/* A UI layer the game moved into a margin (16:9 menu layouts): that margin
 * shows the layer's map as drawn instead of staying empty (0 = default). */
#define PORT_MARGIN_LEFT 1
#define PORT_MARGIN_RIGHT 2
void PortBgMargins(int bg, int sides);
/* A UI screen laid out for 16:9 by splitting it at column `split` (0 turns
 * it off): the layer's left part moves into the left margin, its right part
 * into the right one (ppu.c); the game moves its sprites to match. */
void PortBgSplit(int bg, int split);
/* The map now set on `bg` is a sliding panel (a message window), not a
 * world map: the margins must not show more of it (widescreen.c). */
void PortBgPanelMap(int bg, const void* map);
/* Skip the logos and the title's intro at launch, straight to the title menu
 * (port menu option). */
int PortSkipIntro(void);
/* A 240-pixel UI screen is open (+1) or closed (-1): while any is open the
 * widescreen margins are black bars and sprites stay inside the original 240
 * columns, as the UI moves things out of view by placing them off screen. */
void PortUiOverlay(int delta);
int PortUiOverlayActive(void);
/* A UI screen laid out for 16:9 that still places things off screen to hide
 * them on rows [y0, y1) (the door's card tray: cards of other pages): there,
 * sprites stay inside the original 240 columns; y0 = y1 turns it off. */
void PortUiClipRows(int y0, int y1);
/* A menu over the field opened (+1) or closed (-1). While one is open, it is
 * stretched to the whole 16:9 screen (wide_menus, vita_video.c). */
void PortWideMenu(int delta);
/* A game mode (screen) starts: picks its widescreen treatment by name
 * (logos and title: margins in the scene's background colour; menu screens:
 * stretched like the field menus). */
void PortModeStart(const char* name);
/* Columns the widescreen frame adds on each side of the original 240 (0 in
 * the 3:2 modes). */
int PortWideMargin(void);
/* The HP display on the field is turned on in the port menu. */
int PortFieldHudEnabled(void);
/* The rear touch pad was held: return the stocked cards to the hand (card
 * battles; vita_input.c). */
int PortTakeUnstockRequest(void);
/* Stocked cards in a card battle: how many, and the span of their centres in
 * GBA screen pixels; called every frame (vita_notice.c draws the unstock
 * progress bar under them). */
void PortSetStockArea(int count, int x0, int x1, int y);
/* Moogle points picked up on the field: shows them, then the new total
 * (vita_notice.c). */
void PortNotifyMooglePoints(unsigned gained, unsigned total);
/* Player HP display on the field (src/btl/btl2.c), Vita port only. */
void PortFieldHudOpen(void);
void PortFieldHudDraw(void);
void PortFieldHudClose(void);
/* GBA bus address -> host memory, or NULL (used by the fault handler). */
void* GbaPtrQuiet(uint32_t addr);
/* Frame capture for the render thread (see vita_render.c). */
void PortCaptureLine(int y);
void PortCaptureSubmit(void);
/* Called from VBlankIntrWait(): presents the frame, polls input, paces 60 Hz. */
void PortVBlankWait(void);
/* Returns the GBA KEYINPUT bits (active high) for the current frame. */
uint16_t PortReadKeys(void);
/* Nonzero for a few frames after the dodge-roll button was pressed; the
 * battle code clears it once it acts on it. */
extern uint8_t gPortDodgePressed;
/* Persists the emulated SRAM after the game wrote to it. */
void PortSramWritten(void);
/* Game side (src/save.c): checks and repairs SRAM after a save bank swap. */
void PortCheckSaveBank(void);
/* Boss practice (port/vita/game/boss_practice.c), for the port menu: the
 * bosses of Sora's story, each with whether this file got past it. */
enum { PORT_BOSS_OK, PORT_BOSS_NOT_FIELD, PORT_BOSS_RIKU };
int PortBossCount(void);
int PortBossInfo(int i, const char** name, const char** place);
/* Listed under the TUTORIALS tab instead of BOSSES. */
int PortBossIsTutorial(int i);
/* PORT_BOSS_OK while Sora walks a world's rooms, the only place a fight can
 * start from. */
int PortBossAvailable(void);
/* level: 0 for the original strength, else the enemy level (1 to Sora's). */
void PortBossRequest(int i, int level);
int PortBossSoraLevel(void);
/* Game side hooks: the field's main loop (nonzero: a fight was requested),
 * the battle start (nonzero: started), the battle mode's set-up, the
 * battle's end (nonzero: handled, back to the field), and the battle mode's
 * exit. */
int PortBossFieldTick(void);
int PortBossStart(void);
void PortBossBattleInit(void);
int PortBossActive(void);
int PortBossEnd(int won);
void PortBossBattleExit(void);
/* Every enemy's set-up (InitEnemyBtlObj): HP and attack for the chosen level. */
void PortBossScaleEnemy(short* maxHp, short* attack);
/* Restarts the application (BIOS SoftReset). */
void PortSoftReset(void) __attribute__((noreturn));
/* Debug log (printf-style) to ux0:data/khcom/log.txt. */
void PortLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
/* Diagnostics shared with the host's periodic status log. */
extern volatile uint32_t gPortVBlankIrqs;
extern volatile uint32_t gPortModeUpdates;
#define PORT_TRACE(...) PortLog(__VA_ARGS__)
/* Step-by-step tracing of the first few frames (counts down to 0). */
extern int gPortTraceFrames;
#define PORT_TRACE_EARLY(...) do { if (gPortTraceFrames > 0) PortLog(__VA_ARGS__); } while (0)
/* Fatal error: logs, shows the message and stops. */
void PortFatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif /* GUARD_PORT_H */
