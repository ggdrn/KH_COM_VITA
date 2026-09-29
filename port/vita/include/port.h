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
/* Streamed background map of one BG for the widescreen margins (see
 * port/vita/game/widescreen.c); map == NULL disables it. */
void PortSetBgStream(int bg, const void* const* map, int width, int height, int worldX, int worldY,
                     int shadowHofs, int shadowVofs);
void PortCaptureBgStreams(void);
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
