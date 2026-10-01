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
/* A 240-pixel UI screen is open (+1) or closed (-1): while any is open the
 * widescreen margins are black bars and sprites stay inside the original 240
 * columns, as the UI moves things out of view by placing them off screen. */
void PortUiOverlay(int delta);
int PortUiOverlayActive(void);
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
