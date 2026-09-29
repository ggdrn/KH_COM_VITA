/*
 * Scanline renderer for the GBA picture processing unit.
 *
 * Renders a captured PpuFrame (per-line IO registers plus the frame's palette,
 * OAM and VRAM) to RGBA8888. The output can be wider than the GBA's 240
 * pixels: the extra columns are split evenly left and right of the original
 * screen and show whatever the backgrounds and sprites contain there
 * (widescreen mode).
 *
 * All state lives in a PpuCtx so several threads can render different line
 * ranges of the same frame.
 */
#include "port.h"
#include "ppu.h"

#include <string.h>

#define MAX_W PORT_MAX_SCREEN_WIDTH

enum {
    LAYER_BG0,
    LAYER_BG1,
    LAYER_BG2,
    LAYER_BG3,
    LAYER_OBJ,
    LAYER_BD,
    LAYER_NONE,
};

/* Per-pixel layer sample: 15-bit color, or TRANSPARENT. */
#define TRANSPARENT 0x8000

/* One sprite, decoded once per frame. */
typedef struct PpuObj {
    int16_t x, y;
    uint8_t w, h, bw, bh;
    uint8_t affine, mode, bpp8, prio, mosaic, hflip, vflip;
    uint16_t tile;
    uint16_t palBase;
    int16_t pa, pb, pc, pd;
} PpuObj;

typedef struct PpuCtx {
    const uint8_t* io;
    const uint8_t* pltt;
    const uint8_t* vram;
    uint32_t* out;
    int pitch;
    int width;
    int xoff;
    const PpuObj* objs;
    int objCount;
    int32_t affX[2], affY[2];
    uint32_t latchX[2], latchY[2];
    uint16_t bgLine[4][MAX_W];
    uint16_t objLine[MAX_W];
    uint8_t objPrio[MAX_W];
    uint8_t objSemi[MAX_W];
    uint8_t objWin[MAX_W];
    uint8_t winMask[MAX_W];
    uint16_t top[MAX_W];
    uint16_t below[MAX_W];
    uint8_t topL[MAX_W];
    uint8_t belowL[MAX_W];
} PpuCtx;

#define IO16(c, off) (*(const uint16_t*)&(c)->io[off])
#define IO32(c, off) (*(const uint32_t*)&(c)->io[off])
#define PLTT16(c, i) (((const uint16_t*)(c)->pltt)[i])

static int sWidth = GBA_SCREEN_WIDTH;
static uint32_t* sOut;
static int sPitch;

void PpuSetOutput(uint32_t* out, int width, int pitch) {
    if (width > MAX_W) {
        width = MAX_W;
    }
    sOut = out;
    sWidth = width;
    sPitch = pitch;
}

int PpuGetWidth(void) {
    return sWidth;
}

/* BGR555 -> RGBA8888 (little endian ABGR). */
static inline uint32_t ToRgba(uint32_t c) {
    uint32_t v = ((c & 0x1F) << 3) | ((c & 0x3E0) << 6) | ((c & 0x7C00) << 9);
    return 0xFF000000u | v | ((v >> 5) & 0x070707);
}

static int32_t SignExtend28(uint32_t v) {
    return (int32_t)(v << 4) >> 4;
}

/* Affine reference points reload whenever the game writes BGxX/BGxY. */
static void LatchAffine(PpuCtx* c, int force) {
    int i;

    for (i = 0; i < 2; i++) {
        uint32_t x = IO32(c, 0x28 + i * 0x10);
        uint32_t y = IO32(c, 0x2C + i * 0x10);
        if (force || x != c->latchX[i]) {
            c->latchX[i] = x;
            c->affX[i] = SignExtend28(x);
        }
        if (force || y != c->latchY[i]) {
            c->latchY[i] = y;
            c->affY[i] = SignExtend28(y);
        }
    }
}

static void AdvanceAffine(PpuCtx* c) {
    c->affX[0] += (int16_t)IO16(c, 0x22);
    c->affY[0] += (int16_t)IO16(c, 0x26);
    c->affX[1] += (int16_t)IO16(c, 0x32);
    c->affY[1] += (int16_t)IO16(c, 0x36);
}

/* Backgrounds ----------------------------------------------------------------- */

static void RenderTextBgMosaic(PpuCtx* c, int bg, int y) {
    uint16_t cnt = IO16(c, 0x08 + bg * 2);
    int hofs = IO16(c, 0x10 + bg * 4) & 0x1FF;
    int vofs = IO16(c, 0x12 + bg * 4) & 0x1FF;
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int bpp8 = (cnt >> 7) & 1;
    int size = (cnt >> 14) & 3;
    int wMask = (size & 1) ? 511 : 255;
    int hMask = (size & 2) ? 511 : 255;
    uint16_t m = IO16(c, 0x4C);
    int mh = (m & 0xF) + 1;
    int mv = ((m >> 4) & 0xF) + 1;
    uint16_t* line = c->bgLine[bg];
    int ty = ((y - (y % mv)) + vofs) & hMask;
    int x;

    for (x = 0; x < c->width; x++) {
        int gx = x - c->xoff;
        int sx = gx - (((gx % mh) + mh) % mh);
        int tx = (sx + hofs) & wMask;
        uint32_t block = (tx >= 256 ? 1 : 0) + (ty >= 256 ? ((size == 3) ? 2 : 1) : 0);
        uint16_t entry = *(const uint16_t*)&c->vram[(screenBase + block * 0x800 + (((ty & 255) >> 3) * 32 + ((tx & 255) >> 3)) * 2) & 0xFFFF];
        uint32_t tile = entry & 0x3FF;
        int px = (entry & 0x400) ? 7 - (tx & 7) : (tx & 7);
        int py = (entry & 0x800) ? 7 - (ty & 7) : (ty & 7);
        uint8_t ci;

        if (bpp8) {
            uint32_t addr = charBase + tile * 64 + py * 8 + px;
            ci = addr < 0x10000 ? c->vram[addr] : 0;
            line[x] = ci ? (PLTT16(c, ci) & 0x7FFF) : TRANSPARENT;
        } else {
            uint32_t addr = charBase + tile * 32 + py * 4 + (px >> 1);
            ci = addr < 0x10000 ? c->vram[addr] : 0;
            ci = (px & 1) ? (ci >> 4) : (ci & 0xF);
            line[x] = ci ? (PLTT16(c, (entry >> 12) * 16 + ci) & 0x7FFF) : TRANSPARENT;
        }
    }
}

static void RenderTextBg(PpuCtx* c, int bg, int y) {
    uint16_t cnt = IO16(c, 0x08 + bg * 2);
    int hofs = IO16(c, 0x10 + bg * 4) & 0x1FF;
    int vofs = IO16(c, 0x12 + bg * 4) & 0x1FF;
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int bpp8 = (cnt >> 7) & 1;
    int size = (cnt >> 14) & 3;
    int wMask = (size & 1) ? 511 : 255;
    int hMask = (size & 2) ? 511 : 255;
    uint16_t* line = c->bgLine[bg];
    const uint16_t* pltt = (const uint16_t*)c->pltt;
    int ty, tx, x;
    uint32_t rowBlock, mapRow;

    if (cnt & 0x40) {
        RenderTextBgMosaic(c, bg, y);
        return;
    }
    ty = (y + vofs) & hMask;
    rowBlock = ty >= 256 ? ((size == 3) ? 2 : 1) : 0;
    mapRow = ((ty & 255) >> 3) * 32;
    tx = (hofs - c->xoff) & wMask;

    /* One map entry and one row of tile data per 8 pixels. */
    for (x = 0; x < c->width;) {
        uint32_t block = rowBlock + (tx >= 256 ? 1 : 0);
        uint16_t entry = *(const uint16_t*)&c->vram[(screenBase + block * 0x800 + (mapRow + ((tx & 255) >> 3)) * 2) & 0xFFFF];
        uint32_t tile = entry & 0x3FF;
        int py = (entry & 0x800) ? 7 - (ty & 7) : (ty & 7);
        int hflip = (entry & 0x400) != 0;
        int px = tx & 7;
        int n = 8 - px;
        int i;

        if (n > c->width - x) {
            n = c->width - x;
        }
        if (bpp8) {
            uint32_t addr = charBase + tile * 64 + py * 8;
            if (addr >= 0x10000) {
                for (i = 0; i < n; i++) {
                    line[x + i] = TRANSPARENT;
                }
            } else {
                const uint8_t* row = &c->vram[addr];
                for (i = 0; i < n; i++) {
                    uint8_t ci = row[hflip ? 7 - (px + i) : px + i];
                    line[x + i] = ci ? (pltt[ci] & 0x7FFF) : TRANSPARENT;
                }
            }
        } else {
            uint32_t addr = charBase + tile * 32 + py * 4;
            if (addr >= 0x10000) {
                for (i = 0; i < n; i++) {
                    line[x + i] = TRANSPARENT;
                }
            } else {
                uint32_t row = *(const uint32_t*)&c->vram[addr];
                const uint16_t* pal = &pltt[(entry >> 12) * 16];
                if (row == 0) {
                    for (i = 0; i < n; i++) {
                        line[x + i] = TRANSPARENT;
                    }
                } else {
                    for (i = 0; i < n; i++) {
                        int p = hflip ? 7 - (px + i) : px + i;
                        uint32_t ci = (row >> (p * 4)) & 0xF;
                        line[x + i] = ci ? (pal[ci] & 0x7FFF) : TRANSPARENT;
                    }
                }
            }
        }
        x += n;
        tx = (tx + n) & wMask;
    }
}

static void RenderAffineBg(PpuCtx* c, int bg) {
    uint16_t cnt = IO16(c, 0x08 + bg * 2);
    int idx = bg - 2;
    int16_t pa = (int16_t)IO16(c, 0x20 + idx * 0x10);
    int16_t pc = (int16_t)IO16(c, 0x24 + idx * 0x10);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int sizePx = 128 << ((cnt >> 14) & 3);
    int shift = 4 + ((cnt >> 14) & 3); /* log2(tiles per row) */
    int wrap = (cnt >> 13) & 1;
    uint16_t* line = c->bgLine[bg];
    int32_t cx = c->affX[idx] - pa * c->xoff;
    int32_t cy = c->affY[idx] - pc * c->xoff;
    int x;

    for (x = 0; x < c->width; x++, cx += pa, cy += pc) {
        int tx = cx >> 8;
        int ty = cy >> 8;
        uint8_t tile, ci;

        if (wrap) {
            tx &= sizePx - 1;
            ty &= sizePx - 1;
        } else if ((unsigned)tx >= (unsigned)sizePx || (unsigned)ty >= (unsigned)sizePx) {
            line[x] = TRANSPARENT;
            continue;
        }
        tile = c->vram[(screenBase + ((ty >> 3) << shift) + (tx >> 3)) & 0xFFFF];
        ci = c->vram[(charBase + tile * 64 + (ty & 7) * 8 + (tx & 7)) & 0xFFFF];
        line[x] = ci ? (PLTT16(c, ci) & 0x7FFF) : TRANSPARENT;
    }
}

static void RenderBitmapBg(PpuCtx* c, int mode) {
    int16_t pa = (int16_t)IO16(c, 0x20);
    int16_t pc = (int16_t)IO16(c, 0x24);
    uint32_t frame = (IO16(c, 0x00) & 0x10) ? 0xA000 : 0;
    int bw = mode == 5 ? 160 : 240;
    int bh = mode == 5 ? 128 : 160;
    uint16_t* line = c->bgLine[2];
    int32_t cx = c->affX[0] - pa * c->xoff;
    int32_t cy = c->affY[0] - pc * c->xoff;
    int x;

    for (x = 0; x < c->width; x++, cx += pa, cy += pc) {
        int tx = cx >> 8;
        int ty = cy >> 8;

        if ((unsigned)tx >= (unsigned)bw || (unsigned)ty >= (unsigned)bh) {
            line[x] = TRANSPARENT;
        } else if (mode == 4) {
            uint8_t ci = c->vram[frame + ty * 240 + tx];
            line[x] = ci ? (PLTT16(c, ci) & 0x7FFF) : TRANSPARENT;
        } else if (mode == 3) {
            line[x] = *(const uint16_t*)&c->vram[(ty * 240 + tx) * 2] & 0x7FFF;
        } else {
            line[x] = *(const uint16_t*)&c->vram[frame + (ty * 160 + tx) * 2] & 0x7FFF;
        }
    }
}

/* Sprites ----------------------------------------------------------------------- */

static const uint8_t sObjSize[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

/* Decodes OAM once per frame; returns the number of drawable sprites. */
static int DecodeObjs(const uint8_t* oamMem, int bitmapMode, PpuObj* out) {
    int i, n = 0;

    for (i = 0; i < 128; i++) {
        const uint16_t* oam = (const uint16_t*)&oamMem[i * 8];
        uint16_t a0 = oam[0], a1 = oam[1], a2 = oam[2];
        int affine = (a0 >> 8) & 1;
        int shape = (a0 >> 14) & 3;
        int mode = (a0 >> 10) & 3;
        PpuObj* o = &out[n];

        if ((!affine && (a0 & 0x200)) || shape == 3 || mode == 3) {
            continue;
        }
        o->tile = a2 & 0x3FF;
        if (bitmapMode && o->tile < 512) {
            continue;
        }
        o->w = sObjSize[shape][a1 >> 14][0];
        o->h = sObjSize[shape][a1 >> 14][1];
        o->bw = o->w;
        o->bh = o->h;
        if (affine && (a0 & 0x200)) {
            o->bw *= 2;
            o->bh *= 2;
        }
        o->y = a0 & 0xFF;
        if (o->y + o->bh > 256) {
            o->y -= 256;
        }
        o->x = a1 & 0x1FF;
        if (o->x >= 256) {
            o->x -= 512;
        }
        o->affine = affine;
        o->mode = mode;
        o->bpp8 = (a0 >> 13) & 1;
        o->prio = (a2 >> 10) & 3;
        o->mosaic = (a0 >> 12) & 1;
        o->hflip = !affine && (a1 & 0x1000);
        o->vflip = !affine && (a1 & 0x2000);
        o->palBase = 0x100 + (o->bpp8 ? 0 : (a2 >> 12) * 16);
        o->pa = 0x100;
        o->pb = 0;
        o->pc = 0;
        o->pd = 0x100;
        if (affine) {
            const uint16_t* p = (const uint16_t*)&oamMem[((a1 >> 9) & 0x1F) * 32];
            o->pa = (int16_t)p[3];
            o->pb = (int16_t)p[7];
            o->pc = (int16_t)p[11];
            o->pd = (int16_t)p[15];
        }
        n++;
    }
    return n;
}

static inline uint8_t ObjPixel(const PpuCtx* c, const PpuObj* o, int mapping1d, int tx, int ty) {
    uint32_t tile, addr;
    uint8_t ci;

    if (o->bpp8) {
        tile = mapping1d ? o->tile + ((ty >> 3) * (o->w >> 3) + (tx >> 3)) * 2 : o->tile + (ty >> 3) * 32 + (tx >> 3) * 2;
        addr = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 8 + (tx & 7);
        return c->vram[addr < 0x18000 ? addr : addr - 0x8000];
    }
    tile = mapping1d ? o->tile + (ty >> 3) * (o->w >> 3) + (tx >> 3) : o->tile + (ty >> 3) * 32 + (tx >> 3);
    addr = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 4 + ((tx & 7) >> 1);
    ci = c->vram[addr < 0x18000 ? addr : addr - 0x8000];
    return (tx & 1) ? (ci >> 4) : (ci & 0xF);
}

static int RenderObjs(PpuCtx* c, int y) {
    uint16_t dispcnt = IO16(c, 0x00);
    int mapping1d = (dispcnt >> 6) & 1;
    uint16_t mos = IO16(c, 0x4C);
    int mh = ((mos >> 8) & 0xF) + 1;
    int mv = ((mos >> 12) & 0xF) + 1;
    int anySemi = 0;
    int i;

    for (i = 0; i < c->width; i++) {
        c->objLine[i] = TRANSPARENT;
    }
    memset(c->objPrio, 4, c->width);
    memset(c->objSemi, 0, c->width);
    memset(c->objWin, 0, c->width);

    for (i = 0; i < c->objCount; i++) {
        const PpuObj* o = &c->objs[i];
        int line = y - o->y;
        int x0, x1, x;

        if (line < 0 || line >= o->bh) {
            continue;
        }
        if (o->mosaic) {
            line -= line % mv;
        }
        x0 = o->x + c->xoff;
        x1 = x0 + o->bw;
        if (x1 <= 0 || x0 >= c->width) {
            continue;
        }
        if (o->mode == 1) {
            anySemi = 1;
        }
        if (!o->affine && !o->mosaic) {
            /* Regular sprite: one tile row fetch per 8 pixels. */
            int ty = o->vflip ? o->h - 1 - line : line;
            int start = x0 < 0 ? 0 : x0;
            int end = x1 < c->width ? x1 : c->width;
            uint16_t pal = o->palBase;
            uint8_t semi = o->mode == 1;

            for (x = start; x < end;) {
                int lx = x - x0;
                int tx = o->hflip ? o->w - 1 - lx : lx;
                int col = tx >> 3;
                uint32_t tile;
                uint32_t addr;
                uint8_t px[8];
                int k, n;

                /* Same tile numbering as ObjPixel(). */
                if (o->bpp8) {
                    tile = o->tile + (mapping1d ? ((ty >> 3) * (o->w >> 3) + col) * 2 : (ty >> 3) * 32 + col * 2);
                } else {
                    tile = o->tile + (mapping1d ? (ty >> 3) * (o->w >> 3) + col : (ty >> 3) * 32 + col);
                }
                /* Pixels left in this tile column, in screen order. */
                n = o->hflip ? (tx & 7) + 1 : 8 - (tx & 7);
                if (n > end - x) {
                    n = end - x;
                }
                if (o->bpp8) {
                    addr = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 8;
                    if (addr >= 0x18000) {
                        addr -= 0x8000;
                    }
                    memcpy(px, &c->vram[addr], 8);
                } else {
                    uint32_t row;
                    addr = 0x10000 + ((tile & 0x3FF) * 32) + (ty & 7) * 4;
                    if (addr >= 0x18000) {
                        addr -= 0x8000;
                    }
                    row = *(const uint32_t*)&c->vram[addr];
                    if (row == 0) {
                        x += n;
                        continue;
                    }
                    for (k = 0; k < 8; k++) {
                        px[k] = (row >> (k * 4)) & 0xF;
                    }
                    pal = o->palBase;
                }
                for (k = 0; k < n; k++, x++) {
                    int p = o->hflip ? (tx & 7) - k : (tx & 7) + k;
                    uint8_t ci = px[p];
                    if (ci == 0) {
                        continue;
                    }
                    if (o->mode == 2) {
                        c->objWin[x] = 1;
                    } else if (o->prio < c->objPrio[x]) {
                        c->objLine[x] = PLTT16(c, pal + ci) & 0x7FFF;
                        c->objPrio[x] = o->prio;
                        c->objSemi[x] = semi;
                    }
                }
            }
            continue;
        }
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < c->width; x++) {
            int lx = x - x0;
            int tx, ty;
            uint8_t ci;

            if (o->affine) {
                int dx = lx - o->bw / 2;
                int dy = line - o->bh / 2;
                tx = ((o->pa * dx + o->pb * dy) >> 8) + o->w / 2;
                ty = ((o->pc * dx + o->pd * dy) >> 8) + o->h / 2;
                if ((unsigned)tx >= o->w || (unsigned)ty >= o->h) {
                    continue;
                }
            } else {
                tx = o->hflip ? o->w - 1 - lx : lx;
                ty = o->vflip ? o->h - 1 - line : line;
            }
            if (o->mosaic) {
                tx -= tx % mh;
            }
            ci = ObjPixel(c, o, mapping1d, tx, ty);
            if (ci == 0) {
                continue;
            }
            if (o->mode == 2) {
                c->objWin[x] = 1;
            } else if (o->prio < c->objPrio[x]) {
                c->objLine[x] = PLTT16(c, o->palBase + ci) & 0x7FFF;
                c->objPrio[x] = o->prio;
                c->objSemi[x] = o->mode == 1;
            }
        }
    }
    return anySemi;
}

/* Windows ------------------------------------------------------------------------ */

static int InWindowX(int gx, uint16_t winh) {
    int x1 = winh >> 8;
    int x2 = winh & 0xFF;

    if (x2 > GBA_SCREEN_WIDTH) {
        x2 = GBA_SCREEN_WIDTH;
    }
    /* Windows that touch a screen edge extend into the widescreen margins. */
    if (x1 <= x2) {
        int left = x1 == 0 ? -0x10000 : x1;
        int right = x2 >= GBA_SCREEN_WIDTH ? 0x10000 : x2;
        return gx >= left && gx < right;
    }
    return gx >= x1 || gx < x2;
}

static int InWindowY(int y, uint16_t winv) {
    int y1 = winv >> 8;
    int y2 = winv & 0xFF;

    if (y2 > GBA_SCREEN_HEIGHT && y1 <= y2) {
        y2 = GBA_SCREEN_HEIGHT;
    }
    if (y1 <= y2) {
        return y >= y1 && y < y2;
    }
    return y >= y1 || y < y2;
}

/* Returns 0 when no window is active (every layer and effect enabled). */
static int BuildWindowMask(PpuCtx* c, int y) {
    uint16_t dispcnt = IO16(c, 0x00);
    uint16_t winin = IO16(c, 0x48);
    uint16_t winout = IO16(c, 0x4A);
    int w0, w1, ow, x;
    uint16_t w0h, w1h;

    if (!(dispcnt & 0xE000)) {
        return 0;
    }
    w0 = (dispcnt & 0x2000) && InWindowY(y, IO16(c, 0x44));
    w1 = (dispcnt & 0x4000) && InWindowY(y, IO16(c, 0x46));
    ow = (dispcnt & 0x8000) != 0;
    w0h = IO16(c, 0x40);
    w1h = IO16(c, 0x42);
    for (x = 0; x < c->width; x++) {
        int gx = x - c->xoff;
        if (w0 && InWindowX(gx, w0h)) {
            c->winMask[x] = winin & 0x3F;
        } else if (w1 && InWindowX(gx, w1h)) {
            c->winMask[x] = (winin >> 8) & 0x3F;
        } else if (ow && c->objWin[x]) {
            c->winMask[x] = (winout >> 8) & 0x3F;
        } else {
            c->winMask[x] = winout & 0x3F;
        }
    }
    return 1;
}

/* Composition -------------------------------------------------------------------- */

static inline uint16_t BlendAlpha(uint16_t a, uint16_t b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return r | (g << 5) | (bl << 10);
}

static inline uint16_t Brighten(uint16_t c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

static inline uint16_t Darken(uint16_t c, int evy) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return r | (g << 5) | (b << 10);
}

/* Pushes a layer's opaque pixels under what is already in front of them. */
static void StackLayer(PpuCtx* c, const uint16_t* src, int layer, int useWin, int winBit) {
    int x;

    for (x = 0; x < c->width; x++) {
        uint16_t v = src[x];
        if (v == TRANSPARENT || (useWin && !(c->winMask[x] & winBit))) {
            continue;
        }
        if (c->topL[x] == LAYER_NONE) {
            c->top[x] = v;
            c->topL[x] = layer;
        } else if (c->belowL[x] == LAYER_NONE) {
            c->below[x] = v;
            c->belowL[x] = layer;
        }
    }
}

static void StackObjLayer(PpuCtx* c, int prio, int useWin) {
    int x;

    for (x = 0; x < c->width; x++) {
        if (c->objPrio[x] != prio || (useWin && !(c->winMask[x] & 0x10))) {
            continue;
        }
        if (c->topL[x] == LAYER_NONE) {
            c->top[x] = c->objLine[x];
            c->topL[x] = LAYER_OBJ;
        } else if (c->belowL[x] == LAYER_NONE) {
            c->below[x] = c->objLine[x];
            c->belowL[x] = LAYER_OBJ;
        }
    }
}

static void RenderLine(PpuCtx* c, int y) {
    uint16_t dispcnt = IO16(c, 0x00);
    int mode = dispcnt & 7;
    uint16_t bldcnt = IO16(c, 0x50);
    uint16_t bldalpha = IO16(c, 0x52);
    int eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    int evy = IO16(c, 0x54) & 0x1F;
    int blendMode = (bldcnt >> 6) & 3;
    uint16_t backdrop = PLTT16(c, 0) & 0x7FFF;
    int bgEnabled[4] = { 0, 0, 0, 0 };
    int bgPrio[4];
    int objOn = (dispcnt & 0x1000) != 0;
    uint32_t* out = c->out + y * c->pitch;
    int anySemi = 0;
    int useWin;
    int bg, x, p;

    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;

    if (dispcnt & 0x80) {
        for (x = 0; x < c->width; x++) {
            out[x] = 0xFFFFFFFF;
        }
        return;
    }

    for (bg = 0; bg < 4; bg++) {
        if (!(dispcnt & (0x100 << bg))) {
            continue;
        }
        switch (mode) {
        case 0:
            RenderTextBg(c, bg, y);
            break;
        case 1:
            if (bg == 3) {
                continue;
            }
            if (bg == 2) {
                RenderAffineBg(c, bg);
            } else {
                RenderTextBg(c, bg, y);
            }
            break;
        case 2:
            if (bg < 2) {
                continue;
            }
            RenderAffineBg(c, bg);
            break;
        case 3:
        case 4:
        case 5:
            if (bg != 2) {
                continue;
            }
            RenderBitmapBg(c, mode);
            break;
        default:
            continue;
        }
        bgEnabled[bg] = 1;
    }
    for (bg = 0; bg < 4; bg++) {
        bgPrio[bg] = IO16(c, 0x08 + bg * 2) & 3;
    }
    if (objOn) {
        anySemi = RenderObjs(c, y);
    }
    useWin = BuildWindowMask(c, y);

    /* Fast path: no windows, no blending: paint back to front. */
    if (!useWin && blendMode == 0 && !anySemi) {
        uint16_t* line = c->top;

        for (x = 0; x < c->width; x++) {
            line[x] = backdrop;
        }
        for (p = 3; p >= 0; p--) {
            for (bg = 3; bg >= 0; bg--) {
                const uint16_t* src;
                if (!bgEnabled[bg] || bgPrio[bg] != p) {
                    continue;
                }
                src = c->bgLine[bg];
                for (x = 0; x < c->width; x++) {
                    if (src[x] != TRANSPARENT) {
                        line[x] = src[x];
                    }
                }
            }
            if (objOn) {
                for (x = 0; x < c->width; x++) {
                    if (c->objPrio[x] == p) {
                        line[x] = c->objLine[x];
                    }
                }
            }
        }
        for (x = 0; x < c->width; x++) {
            out[x] = ToRgba(line[x]);
        }
        return;
    }

    /* General path: find the two frontmost layers of every pixel. */
    memset(c->topL, LAYER_NONE, c->width);
    memset(c->belowL, LAYER_NONE, c->width);
    for (p = 0; p < 4; p++) {
        if (objOn) {
            StackObjLayer(c, p, useWin);
        }
        for (bg = 0; bg < 4; bg++) {
            if (bgEnabled[bg] && bgPrio[bg] == p) {
                StackLayer(c, c->bgLine[bg], bg, useWin, 1 << bg);
            }
        }
    }

    for (x = 0; x < c->width; x++) {
        uint16_t top = c->top[x], below = c->below[x];
        int topLayer = c->topL[x], belowLayer = c->belowL[x];

        if (topLayer == LAYER_NONE) {
            top = backdrop;
            topLayer = LAYER_BD;
        }
        if (belowLayer == LAYER_NONE) {
            below = backdrop;
            belowLayer = LAYER_BD;
        }
        if (!useWin || (c->winMask[x] & 0x20)) {
            int topIsFirst = (bldcnt >> topLayer) & 1;
            int belowIsSecond = (bldcnt >> (8 + belowLayer)) & 1;

            if (topLayer == LAYER_OBJ && c->objSemi[x] && belowIsSecond) {
                top = BlendAlpha(top, below, eva, evb);
            } else if (topIsFirst) {
                switch (blendMode) {
                case 1:
                    if (belowIsSecond) {
                        top = BlendAlpha(top, below, eva, evb);
                    }
                    break;
                case 2:
                    top = Brighten(top, evy);
                    break;
                case 3:
                    top = Darken(top, evy);
                    break;
                }
            }
        }
        out[x] = ToRgba(top);
    }
}

/* Frame ---------------------------------------------------------------------------- */

static PpuCtx sCtx[PPU_MAX_SLICES];
static PpuObj sObjs[128];
static int sObjCount;

void PpuPrepareFrame(const PpuFrame* frame) {
    int bitmapMode = (*(const uint16_t*)&frame->io[0][0] & 7) >= 3;

    sObjCount = DecodeObjs(frame->oam, bitmapMode, sObjs);
}

void PpuRenderSlice(const PpuFrame* frame, int slice, int y0, int y1) {
    PpuCtx* c = &sCtx[slice];
    int y;

    c->pltt = frame->pltt;
    c->vram = frame->vram;
    c->out = sOut;
    c->pitch = sPitch;
    c->width = sWidth;
    c->xoff = (sWidth - GBA_SCREEN_WIDTH) / 2;
    c->objs = sObjs;
    c->objCount = sObjCount;

    /* Bring the affine reference points to line y0 without drawing. */
    for (y = 0; y < y0; y++) {
        c->io = frame->io[y];
        LatchAffine(c, y == 0);
        AdvanceAffine(c);
    }
    for (y = y0; y < y1; y++) {
        c->io = frame->io[y];
        LatchAffine(c, y == 0);
        RenderLine(c, y);
        AdvanceAffine(c);
    }
}

void PpuRenderFrame(const PpuFrame* frame) {
    PpuPrepareFrame(frame);
    PpuRenderSlice(frame, 0, 0, GBA_SCREEN_HEIGHT);
}
