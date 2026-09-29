/*
 * Widescreen support: exposes the game's streamed background maps to the
 * renderer.
 *
 * engine.c keeps only a 31x21-tile window of each scrolling map in VRAM
 * (the 256-pixel GBA tilemap cannot hold a wider view) and streams new columns
 * in as the camera moves. The renderer draws the 240 original columns from
 * VRAM as usual and fetches the extra widescreen columns straight from the
 * full map (BgEntry.map), so the scene continues past the original edges.
 */
#include "types.h"
#include "engine.h"
#include "display.h"
#include "port.h"

/* Called right after the VBlank handler flushed scroll registers and VRAM, so
 * the captured camera matches what the next frame shows. */
void PortCaptureBgStreams(void) {
    const u16* hofs[4] = { &gBg0HOfs, &gBg1HOfs, &gBg2HOfs, &gBg3HOfs };
    const u16* vofs[4] = { &gBg0VOfs, &gBg1VOfs, &gBg2VOfs, &gBg3VOfs };
    int bg;

    for (bg = 0; bg < 4; bg++) {
        BgEntry* e;

        if (gBgWork == 0) {
            PortSetBgStream(bg, 0, 0, 0, 0, 0, 0, 0);
            continue;
        }
        e = &gBgWork->entries[bg];
        if (e->map == 0 || e->dirty || e->width == 0 || e->height == 0) {
            PortSetBgStream(bg, 0, 0, 0, 0, 0, 0, 0);
            continue;
        }
        PortSetBgStream(bg, (const void* const*)e->map, e->width, e->height, e->x, e->y, *hofs[bg], *vofs[bg]);
    }
}
