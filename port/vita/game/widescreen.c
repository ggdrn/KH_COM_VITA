/*
 * Widescreen support: exposes the game's streamed background maps to the
 * renderer.
 *
 * engine.c keeps only a 31x21-tile window of each scrolling map in VRAM
 * (the 256-pixel GBA tilemap cannot hold a wider view) and streams new columns
 * in as the camera moves. The renderer draws the 240 original columns from
 * VRAM as usual and fetches the extra widescreen columns straight from the
 * full map (BgEntry.map), so the scene continues past the original edges.
 *
 * The card-built field rooms (map_cell.c) have no full map: their tiles are
 * computed from the room's cells a strip at a time. For those the margin tiles
 * are computed here each frame, the same way, into a small window of blocks
 * around the camera that the renderer reads like any other stream.
 */
#include "types.h"
#include "engine.h"
#include "display.h"
#include "port.h"

s32 PortFieldView(s32* x, s32* y, u32* flags);
u16 PortFieldTile(s32 bg, s32 xx, s32 yy);

/* The stream is a 4x2-block (1024x512-pixel) map that wraps: a tile at world
 * pixel (x, y) lives in block ((x mod 1024) / 256, (y mod 512) / 256). The
 * margins span well under that, so blocks never collide. Three sets rotate
 * because the renderer reads a frame while the game prepares the next ones. */
#define FIELD_BLOCKS_W 4
#define FIELD_BLOCKS_H 2
#define FIELD_SETS 3

static u16 sFieldBlocks[FIELD_SETS][3][FIELD_BLOCKS_W * FIELD_BLOCKS_H][32 * 32];
static const void* sFieldBlockPtrs[FIELD_SETS][3][FIELD_BLOCKS_W * FIELD_BLOCKS_H];
static int sFieldSet;

static int Mod(int a, int m) {
    a %= m;
    return a < 0 ? a + m : a;
}

static u16* FieldEntry(int set, int slot, int tx, int ty) {
    int px = Mod(tx * 8, FIELD_BLOCKS_W * 256);
    int py = Mod(ty * 8, FIELD_BLOCKS_H * 256);

    return &sFieldBlocks[set][slot][(py >> 8) * FIELD_BLOCKS_W + (px >> 8)][((py & 255) >> 3) * 32 + ((px & 255) >> 3)];
}

/* The field's tiles are only valid on a BG that shows the room: screens
 * drawn over it (the door's card selection, menus) reuse some of its BGs.
 * A BG counts as the room's when it scrolls with the camera and a spread of
 * its visible tiles match the computed ones. */
static int FieldBgMatches(int bg, int camX, int camY, const u16* hofs, const u16* vofs) {
    static const int pts[8][2] = { { 2, 2 }, { 10, 3 }, { 19, 2 }, { 27, 3 }, { 4, 17 }, { 12, 16 }, { 20, 17 }, { 25, 16 } };
    u16 cnt = *(u16*)&gGbaIo[0x08 + bg * 2];
    const u16* screen = (const u16*)&gGbaVram[((cnt >> 8) & 0x1F) * 0x800];
    int i;

    if ((hofs[bg] & 0x1FF) != (camX & 0x1FF) || (vofs[bg] & 0x1FF) != (camY & 0x1FF)) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        int tx = camX / 8 + pts[i][0];
        int ty = camY / 8 + pts[i][1];

        if (screen[(ty & 31) * 32 + (tx & 31)] != PortFieldTile(bg, tx, ty)) {
            return 0;
        }
    }
    return 1;
}

static void FieldColumns(int set, int bgMask, int tx0, int tx1, int ty0, int ty1) {
    static const int bgs[3] = { 1, 2, 3 };
    int s, tx, ty;

    for (s = 0; s < 3; s++) {
        if (!(bgMask & (1 << bgs[s]))) {
            continue;
        }
        for (ty = ty0; ty <= ty1; ty++) {
            for (tx = tx0; tx <= tx1; tx++) {
                *FieldEntry(set, s, tx, ty) = PortFieldTile(bgs[s], tx, ty);
            }
        }
    }
}

/* Returns the BGs (bit mask) given field streams this frame. */
static int CaptureFieldStreams(void) {
    const u16 hofsNow[4] = { gBg0HOfs, gBg1HOfs, gBg2HOfs, gBg3HOfs };
    const u16 vofsNow[4] = { gBg0VOfs, gBg1VOfs, gBg2VOfs, gBg3VOfs };
    const u16* hofs = hofsNow;
    const u16* vofs = vofsNow;
    s32 camX, camY;
    u32 flags;
    int set, mask, s, i;
    int ty0, ty1;

    if (!PortFieldView(&camX, &camY, &flags) || (*(u16*)&gGbaIo[0] & 7) != 0) {
        return 0;
    }
    mask = 0;
    for (s = 1; s <= 3; s++) {
        /* flags bit 0: BG1 shows a fixed overlay map (BgEntry stream) instead. */
        if ((s != 1 || !(flags & 1)) && FieldBgMatches(s, camX, camY, hofs, vofs)) {
            mask |= 1 << s;
        }
    }
    if (mask == 0) {
        return 0;
    }
    set = sFieldSet = (sFieldSet + 1) % FIELD_SETS;

    /* Margins of 21 pixels, plus slack for per-line scroll effects. */
    ty0 = (camY >> 3) - 2;
    ty1 = ((camY + GBA_SCREEN_HEIGHT) >> 3) + 2;
    FieldColumns(set, mask, (camX - 48) >> 3, (camX >> 3) + 1, ty0, ty1);
    FieldColumns(set, mask, ((camX + GBA_SCREEN_WIDTH) >> 3) - 1, (camX + GBA_SCREEN_WIDTH + 48) >> 3, ty0, ty1);

    for (s = 0; s < 3; s++) {
        int bg = s + 1;

        if (!(mask & (1 << bg))) {
            continue;
        }
        for (i = 0; i < FIELD_BLOCKS_W * FIELD_BLOCKS_H; i++) {
            sFieldBlockPtrs[set][s][i] = sFieldBlocks[set][s][i];
        }
        PortSetBgStream(bg, sFieldBlockPtrs[set][s], FIELD_BLOCKS_W, FIELD_BLOCKS_H, camX, camY, hofs[bg],
                        vofs[bg]);
    }
    return mask;
}

/* Called right after the VBlank handler flushed scroll registers and VRAM, so
 * the captured camera matches what the next frame shows. */
void PortCaptureBgStreams(void) {
    const u16* hofs[4] = { &gBg0HOfs, &gBg1HOfs, &gBg2HOfs, &gBg3HOfs };
    const u16* vofs[4] = { &gBg0VOfs, &gBg1VOfs, &gBg2VOfs, &gBg3VOfs };
    int field = CaptureFieldStreams();
    int bg;

    for (bg = 0; bg < 4; bg++) {
        BgEntry* e;

        if (field & (1 << bg)) {
            continue;
        }
        if (gBgWork == 0) {
            PortSetBgStream(bg, 0, 0, 0, 0, 0, 0, 0);
            continue;
        }
        e = &gBgWork->entries[bg];
        /* A one-block-wide map (a 256-pixel panel) would repeat in the margins. */
        if (e->map == 0 || e->dirty || e->width < 2 || e->height == 0) {
            PortSetBgStream(bg, 0, 0, 0, 0, 0, 0, 0);
            continue;
        }
        PortSetBgStream(bg, (const void* const*)e->map, e->width, e->height, e->x, e->y, *hofs[bg], *vofs[bg]);
    }
}
