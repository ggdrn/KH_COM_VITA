#ifndef GUARD_PPU_H
#define GUARD_PPU_H

#include <stdint.h>

#include "port.h"

/* Number of IO register bytes the renderer needs per line (DISPCNT..BLDY). */
#define PPU_LINE_IO_SIZE 0x60

/* Everything the renderer reads for one frame, captured by the game thread. */
typedef struct PpuFrame {
    uint8_t io[GBA_SCREEN_HEIGHT][PPU_LINE_IO_SIZE];
    uint8_t pltt[0x400];
    uint8_t oam[0x400];
    uint8_t vram[0x18000];
} PpuFrame;

/* Sets the RGBA8888 frame the renderer writes into and its width in pixels. */
void PpuSetOutput(uint32_t* out, int width, int pitch);
int PpuGetWidth(void);
void PpuRenderFrame(const PpuFrame* frame);

/* Split rendering: decode the frame once, then render line ranges (slices)
 * concurrently, each slice index on its own thread. */
#define PPU_MAX_SLICES 2
void PpuPrepareFrame(const PpuFrame* frame);
void PpuRenderSlice(const PpuFrame* frame, int slice, int y0, int y1);

#endif /* GUARD_PPU_H */
