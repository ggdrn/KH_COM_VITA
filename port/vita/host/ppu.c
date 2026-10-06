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
    const PpuBgStream* streams;
    int clipObjs;
    int clipY0, clipY1;
    uint8_t* mask;
    int sceneMargins;
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
static uint8_t* sMask; /* per pixel: 1 where a sprite is in front (PpuSetMask) */

void PpuSetMask(uint8_t* mask) {
    sMask = mask;
}

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

static void RenderStreamMargin(PpuCtx* c, int bg, int y, int x0, int x1);

static int Mod8(int a) {
    return a & 7;
}

/*
 * A sliding panel (the message box, PortSetBgPanel). Its map holds nothing
 * for the margins, so they stay empty. On lines where the panel spans the
 * original screen (the box fully open: it reaches within a tile of both
 * edges), it is stretched to the edges of the wide screen, on its own tile
 * grid: the tile at each end (the box's rounded end, or its open side, which
 * on the GBA continues past the screen) moves out to the edge, the gap is
 * filled by repeating the tile next to it, and the rest stays in place, so
 * the text, drawn with sprites, stays inside the box. The box doesn't
 * stretch while it slides in or out.
 */
static void RenderPanelMargins(PpuCtx* c, int bg) {
    uint16_t* line = c->bgLine[bg];
    uint16_t src[GBA_SCREEN_WIDTH];
    int m = c->xoff, w = GBA_SCREEN_WIDTH;
    int hofs = IO16(c, 0x10 + bg * 4) & 0x1FF;
    int g = (8 - (hofs & 7)) & 7;               /* first tile boundary on screen */
    int lastTile = g + ((w - g) / 8 - 1) * 8;   /* last tile fully on screen */
    int left = -1, right = -1, gx;

    for (gx = 0; gx < m; gx++) {
        line[gx] = TRANSPARENT;
        line[m + w + gx] = TRANSPARENT;
    }
    for (gx = 0; gx < w; gx++) {
        if (line[m + gx] != TRANSPARENT) {
            if (left < 0) {
                left = gx;
            }
            right = gx;
        }
    }
    if (left < 0 || left > g + 7 || right < w - 9 || lastTile - g < 32) {
        return;
    }
    memcpy(src, line + m, sizeof(src));
    for (gx = -m; gx < w + m; gx++) {
        int s;

        if (gx < -m + 8) {
            s = g + (gx + m);                           /* left end tile */
        } else if (gx < g + 8) {
            s = g + 8 + Mod8(gx - (g + 8));             /* next tile, repeated */
        } else if (gx >= w + m - 8) {
            s = lastTile + (gx - (w + m - 8));          /* right end tile */
        } else if (gx >= lastTile) {
            s = lastTile - 8 + Mod8(gx - (lastTile - 8));
        } else {
            s = gx;                                     /* in place */
        }
        line[m + gx] = src[s];
    }
}

/*
 * A UI screen split for 16:9 (PortBgSplit): what is left of column `split`
 * moves into the left margin, what is right of it into the right one, each
 * at its own size; the gap they open in the middle repeats the column on each
 * side of the split (so a line running across continues, and space between
 * boxes stays empty). The game moves the sprites over each side the same way.
 */
static void RenderSplitMargins(PpuCtx* c, int bg, int split) {
    uint16_t* line = c->bgLine[bg];
    uint16_t src[GBA_SCREEN_WIDTH];
    int m = c->xoff, w = GBA_SCREEN_WIDTH;
    int gx;

    memcpy(src, line + m, sizeof(src));
    for (gx = -m; gx < w + m; gx++) {
        int s;

        if (gx < split - m) {
            s = gx + m;
        } else if (gx < split) {
            s = split - 1;
        } else if (gx < split + m) {
            s = split;
        } else {
            s = gx - m;
        }
        line[m + gx] = s >= 0 && s < w ? src[s] : TRANSPARENT;
    }
}

/*
 * Widescreen margins of a text background. A streamed map continues there;
 * a 512-pixel-wide tilemap (battle backgrounds) holds real content past the
 * original screen and is left as drawn; anything else is a 256-pixel map whose
 * columns past 240 are unused or wrap around (UI panels, frames), so the
 * margins stay transparent and show the layers below.
 */
static void RenderTextMargins(PpuCtx* c, int bg, int y, int size) {
    uint16_t* line = c->bgLine[bg];
    int x;

    if (c->xoff <= 0) {
        return;
    }
    if (c->streams[bg].split > 0) {
        RenderSplitMargins(c, bg, c->streams[bg].split);
    } else if (c->streams[bg].keepMargins) {
        /* A UI layer moved into a margin: that side keeps the map as drawn. */
        if (!(c->streams[bg].keepMargins & PORT_MARGIN_LEFT)) {
            for (x = 0; x < c->xoff; x++) {
                line[x] = TRANSPARENT;
            }
        }
        if (!(c->streams[bg].keepMargins & PORT_MARGIN_RIGHT)) {
            for (x = c->xoff + GBA_SCREEN_WIDTH; x < c->width; x++) {
                line[x] = TRANSPARENT;
            }
        }
    } else if (c->streams[bg].panel) {
        RenderPanelMargins(c, bg);
    } else if (c->streams[bg].valid) {
        RenderStreamMargin(c, bg, y, 0, c->xoff);
        RenderStreamMargin(c, bg, y, c->xoff + GBA_SCREEN_WIDTH, c->width);
    } else if (!(size & 1)) {
        for (x = 0; x < c->xoff; x++) {
            line[x] = TRANSPARENT;
        }
        for (x = c->xoff + GBA_SCREEN_WIDTH; x < c->width; x++) {
            line[x] = TRANSPARENT;
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
        RenderTextMargins(c, bg, y, size);
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

    RenderTextMargins(c, bg, y, size);
}

/*
 * Widescreen margins of a streamed background: fetch tiles from the game's
 * full map instead of the 256-pixel VRAM window, which only holds the
 * original 240 columns (plus one) of the scene.
 */
static void RenderStreamMargin(PpuCtx* c, int bg, int y, int x0, int x1) {
    const PpuBgStream* s = &c->streams[bg];
    uint16_t cnt = IO16(c, 0x08 + bg * 2);
    int hofs = IO16(c, 0x10 + bg * 4);
    int vofs = IO16(c, 0x12 + bg * 4);
    uint32_t charBase = ((cnt >> 2) & 3) * 0x4000;
    int bpp8 = (cnt >> 7) & 1;
    int mapW = s->width * 256, mapH = s->height * 256;
    /* Per-line scroll effects move the view relative to the camera (mod 256). */
    int wx0 = s->worldX + (int8_t)(hofs - s->shadowHofs) + (x0 - c->xoff);
    int wy = s->worldY + (int8_t)(vofs - s->shadowVofs) + y;
    const uint16_t* pltt = (const uint16_t*)c->pltt;
    uint16_t* line = c->bgLine[bg];
    int x;

    wy %= mapH;
    if (wy < 0) {
        wy += mapH;
    }
    for (x = x0; x < x1; x++) {
        int wx = (wx0 + (x - x0)) % mapW;
        const uint16_t* block;
        uint16_t entry;
        uint32_t tile, addr;
        int px, py;
        uint8_t ci;

        if (wx < 0) {
            wx += mapW;
        }
        block = s->blocks[(wy >> 8) * s->width + (wx >> 8)];
        entry = block[((wy & 255) >> 3) * 32 + ((wx & 255) >> 3)];
        tile = entry & 0x3FF;
        px = (entry & 0x400) ? 7 - (wx & 7) : (wx & 7);
        py = (entry & 0x800) ? 7 - (wy & 7) : (wy & 7);
        if (bpp8) {
            addr = charBase + tile * 64 + py * 8 + px;
            ci = addr < 0x10000 ? c->vram[addr] : 0;
            line[x] = ci ? (pltt[ci] & 0x7FFF) : TRANSPARENT;
        } else {
            addr = charBase + tile * 32 + py * 4 + (px >> 1);
            ci = addr < 0x10000 ? c->vram[addr] : 0;
            ci = (px & 1) ? (ci >> 4) : (ci & 0xF);
            line[x] = ci ? (pltt[(entry >> 12) * 16 + ci] & 0x7FFF) : TRANSPARENT;
        }
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
        /* OAM x is 9-bit signed. The game keeps sprites in the widescreen
         * margins (x up to 263, engine.c), so 256..319 are right of the
         * screen; nothing it draws starts left of -88. */
        if (o->x >= 320) {
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
    /* Columns sprites may cover: all, or the original 240 under a UI overlay
     * (or on its clipped rows, PortUiClipRows). */
    int clip = c->clipObjs || (y >= c->clipY0 && y < c->clipY1);
    int lo = clip ? c->xoff : 0;
    int hi = clip ? c->xoff + GBA_SCREEN_WIDTH : c->width;
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
        if (x1 <= lo || x0 >= hi) {
            continue;
        }
        if (o->mode == 1) {
            anySemi = 1;
        }
        if (!o->affine && !o->mosaic) {
            /* Regular sprite: one tile row fetch per 8 pixels. */
            int ty = o->vflip ? o->h - 1 - line : line;
            int start = x0 < lo ? lo : x0;
            int end = x1 < hi ? x1 : hi;
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
        for (x = x0 < lo ? lo : x0; x < x1 && x < hi; x++) {
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

/* The frame columns [*a0, *a1) and [*b0, *b1) a window covers on this line
 * (as up to two ranges, since a window can wrap). An empty window covers
 * nothing; one that touches a screen edge extends into the widescreen
 * margins on that side. */
static void WindowRanges(const PpuCtx* c, uint16_t winh, int* a0, int* a1, int* b0, int* b1) {
    int x1 = winh >> 8, x2 = winh & 0xFF;

    *a0 = *a1 = *b0 = *b1 = 0;
    if (x2 > GBA_SCREEN_WIDTH) {
        x2 = GBA_SCREEN_WIDTH;
    }
    if (x1 == x2) {
        return;
    }
    if (x1 < x2) {
        *a0 = x1 == 0 ? 0 : x1 + c->xoff;
        *a1 = x2 >= GBA_SCREEN_WIDTH ? c->width : x2 + c->xoff;
    } else {
        *a0 = x1 + c->xoff;
        *a1 = c->width;
        *b0 = 0;
        *b1 = x2 + c->xoff;
    }
    if (*a1 > c->width) *a1 = c->width;
    if (*b1 > c->width) *b1 = c->width;
}

static void FillMask(uint8_t* mask, int x0, int x1, uint8_t v, const uint8_t* claimed) {
    int x;

    for (x = x0; x < x1; x++) {
        if (!claimed[x]) {
            mask[x] = v;
        }
    }
}

/* Returns 0 when no window is active (every layer and effect enabled). */
static int BuildWindowMask(PpuCtx* c, int y) {
    uint16_t dispcnt = IO16(c, 0x00);
    uint16_t winin = IO16(c, 0x48);
    uint16_t winout = IO16(c, 0x4A);
    uint8_t claimed[MAX_W];
    int w0, w1, ow, x, a0, a1, b0, b1;

    if (!(dispcnt & 0xE000)) {
        return 0;
    }
    w0 = (dispcnt & 0x2000) && InWindowY(y, IO16(c, 0x44));
    w1 = (dispcnt & 0x4000) && InWindowY(y, IO16(c, 0x46));
    ow = (dispcnt & 0x8000) != 0;

    /* Outside every window first, then the object window, window 1 and
     * window 0 on top (the GBA's priority), each only where nothing higher
     * has claimed the pixel. */
    memset(claimed, 0, c->width);
    if (w0) {
        WindowRanges(c, IO16(c, 0x40), &a0, &a1, &b0, &b1);
        for (x = a0; x < a1; x++) { c->winMask[x] = winin & 0x3F; claimed[x] = 1; }
        for (x = b0; x < b1; x++) { c->winMask[x] = winin & 0x3F; claimed[x] = 1; }
    }
    if (w1) {
        WindowRanges(c, IO16(c, 0x42), &a0, &a1, &b0, &b1);
        FillMask(c->winMask, a0, a1, (winin >> 8) & 0x3F, claimed);
        FillMask(c->winMask, b0, b1, (winin >> 8) & 0x3F, claimed);
        for (x = a0; x < a1; x++) claimed[x] = 1;
        for (x = b0; x < b1; x++) claimed[x] = 1;
    }
    for (x = 0; x < c->width; x++) {
        if (!claimed[x]) {
            c->winMask[x] = (ow && c->objWin[x]) ? (winout >> 8) & 0x3F : winout & 0x3F;
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

/* Whether any layer covers pixel x (the backdrop shows otherwise). */
static int HasLayer(const PpuCtx* c, const int* bgEnabled, int objOn, int x) {
    int bg;

    if (objOn && c->objPrio[x] < 4) {
        return 1;
    }
    for (bg = 0; bg < 4; bg++) {
        if (bgEnabled[bg] && c->bgLine[bg][x] != TRANSPARENT) {
            return 1;
        }
    }
    return 0;
}

/*
 * Widescreen margins where no layer reaches show the backdrop colour. On a
 * line whose original 240 columns show no backdrop at all, that colour is
 * one the GBA never shows (in the field it is cyan, hidden under the scenery,
 * and it flashed in the margins of room transitions, whose dark overlay
 * layer only covers the original width): such margin pixels are black.
 */
static void HideUnseenBackdrop(PpuCtx* c, uint32_t* out, const int* bgEnabled, int objOn) {
    int x, any = 0;

    if (c->xoff <= 0 || c->clipObjs || c->sceneMargins) {
        return;
    }
    for (x = 0; x < c->width && !any; x++) {
        if (x == c->xoff) {
            x = c->xoff + GBA_SCREEN_WIDTH;
        }
        any = x < c->width && !HasLayer(c, bgEnabled, objOn, x);
    }
    if (!any) {
        return;
    }
    for (x = c->xoff; x < c->xoff + GBA_SCREEN_WIDTH; x++) {
        if (!HasLayer(c, bgEnabled, objOn, x)) {
            return; /* the backdrop is part of the picture */
        }
    }
    for (x = 0; x < c->width; x++) {
        if (x == c->xoff) {
            x = c->xoff + GBA_SCREEN_WIDTH;
            if (x >= c->width) {
                break;
            }
        }
        if (!HasLayer(c, bgEnabled, objOn, x)) {
            out[x] = 0xFF000000;
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
        if (c->mask != NULL) {
            /* A sprite is in front where no opaque BG has a higher priority. */
            uint8_t* m = c->mask + y * c->pitch;

            for (x = 0; x < c->width; x++) {
                int front = objOn && c->objPrio[x] < 4;

                for (bg = 0; front && bg < 4; bg++) {
                    if (bgEnabled[bg] && bgPrio[bg] < c->objPrio[x] && c->bgLine[bg][x] != TRANSPARENT) {
                        front = 0;
                    }
                }
                m[x] = (uint8_t)front;
            }
        }
        HideUnseenBackdrop(c, out, bgEnabled, objOn);
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
        if (c->mask != NULL) {
            c->mask[y * c->pitch + x] = topLayer == LAYER_OBJ;
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
    HideUnseenBackdrop(c, out, bgEnabled, objOn);
}

/* Frame ---------------------------------------------------------------------------- */

static PpuCtx sCtx[PPU_MAX_SLICES];
static PpuObj sObjs[128];
static int sObjCount;

/* The affine reference points at the start of each line, before its own
 * latch: they carry over from line to line, and the lines are drawn in
 * chunks by several threads, in any order. */
typedef struct AffineState {
    int32_t affX[2], affY[2];
    uint32_t latchX[2], latchY[2];
} AffineState;

static AffineState sAffine[GBA_SCREEN_HEIGHT];

void PpuPrepareFrame(const PpuFrame* frame) {
    int bitmapMode = (*(const uint16_t*)&frame->io[0][0] & 7) >= 3;
    PpuCtx* c = &sCtx[0];
    int y;

    sObjCount = DecodeObjs(frame->oam, bitmapMode, sObjs);
    for (y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        AffineState* a = &sAffine[y];

        if (y > 0) {
            memcpy(a->affX, c->affX, sizeof(a->affX));
            memcpy(a->affY, c->affY, sizeof(a->affY));
            memcpy(a->latchX, c->latchX, sizeof(a->latchX));
            memcpy(a->latchY, c->latchY, sizeof(a->latchY));
        }
        c->io = frame->io[y];
        LatchAffine(c, y == 0);
        AdvanceAffine(c);
    }
}

void PpuRenderSlice(const PpuFrame* frame, int slice, int y0, int y1) {
    PpuCtx* c = &sCtx[slice];
    int y;

    c->pltt = frame->pltt;
    c->vram = frame->vram;
    c->out = sOut;
    c->pitch = sPitch;
    c->mask = sMask;
    c->width = sWidth;
    c->xoff = (sWidth - GBA_SCREEN_WIDTH) / 2;
    c->objs = sObjs;
    c->objCount = sObjCount;
    c->streams = frame->streams;
    c->clipObjs = frame->clipObjs;
    c->clipY0 = frame->clipY0;
    c->clipY1 = frame->clipY1;
    c->sceneMargins = frame->sceneMargins;

    if (y0 > 0) {
        const AffineState* a = &sAffine[y0];

        memcpy(c->affX, a->affX, sizeof(c->affX));
        memcpy(c->affY, a->affY, sizeof(c->affY));
        memcpy(c->latchX, a->latchX, sizeof(c->latchX));
        memcpy(c->latchY, a->latchY, sizeof(c->latchY));
    }
    for (y = y0; y < y1; y++) {
        c->io = frame->io[y];
        LatchAffine(c, y == 0);
        RenderLine(c, y);
        AdvanceAffine(c);
        if (c->clipObjs && c->xoff > 0) {
            /* UI overlay: black bars, like a 3:2 screen (PortUiOverlay). */
            uint32_t* out = c->out + y * c->pitch;
            int x;

            for (x = 0; x < c->xoff; x++) {
                out[x] = 0xFF000000;
            }
            for (x = c->xoff + GBA_SCREEN_WIDTH; x < c->width; x++) {
                out[x] = 0xFF000000;
            }
        }
    }
}

void PpuRenderFrame(const PpuFrame* frame) {
    PpuPrepareFrame(frame);
    PpuRenderSlice(frame, 0, 0, GBA_SCREEN_HEIGHT);
}
