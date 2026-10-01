/*
 * Moogle points notice, Vita port only: when Sora picks up moogle points on
 * the field (src/map/map_tasks.c, src/poo/poo.c), the bottom-right corner
 * shows what was picked up, then the player's total, 3 s each.
 *
 *   +25 MOOGLE POINTS      (more pickups meanwhile add up and restart it)
 *   MOOGLE POINTS: 1234
 *
 * It is drawn into a small RGBA canvas that vita_video.c lays over the game,
 * and hidden while a 240-pixel UI screen (pause, save...) is open.
 *
 * Also the unstock progress bar of card battles (UpdateUnstockBar).
 */
#include <stdio.h>
#include <string.h>

#include "port.h"
#include "vita_host.h"

#define SHOW_FRAMES (3 * 60)
#define COL_PANEL RGBA(8, 12, 40, 200)
#define COL_BORDER RGBA(120, 150, 255, 255)
#define COL_TEXT RGBA(235, 235, 235, 255)
#define COL_GAIN RGBA(255, 214, 64, 255)

enum { NOTICE_NONE, NOTICE_GAINED, NOTICE_TOTAL };

static uint32_t sCanvas[NOTICE_W * NOTICE_H];
static int sState;
static int sTimer;
static unsigned sGained;
static unsigned sTotal;
static int sDirty; /* the canvas needs redrawing (and uploading) */

/* Unstock progress bar: under the stocked cards while the rear touch / right
 * stick down is held, filling up over the hold (vita_input.c). The card task
 * reports the stock every frame of a card battle (src/card/card_card.c). */
#define BAR_SHOW_FRAMES 4 /* a brief touch shows nothing */
#define CARD_HALF_W 9     /* stocked card art, 70% size: half width */
#define CARD_BOTTOM 13    /* and bottom, from its centre */
static unsigned sFrame;
static unsigned sStockFrame;
static int sStockCount, sStockX0, sStockX1, sStockY;

void PortSetStockArea(int count, int x0, int x1, int y) {
    sStockFrame = sFrame;
    sStockCount = count;
    sStockX0 = x0;
    sStockX1 = x1;
    sStockY = y;
}

static void UpdateUnstockBar(void) {
    int held = InputUnstockHoldFrames();

    if (sFrame - sStockFrame > 1 || sStockCount == 0 || held <= BAR_SHOW_FRAMES || held > UNSTOCK_HOLD_FRAMES) {
        VideoSetUnstockBar(0, 0, 0, -1.0f);
        return;
    }
    VideoSetUnstockBar(sStockX0 - CARD_HALF_W, sStockX1 + CARD_HALF_W, sStockY + CARD_BOTTOM + 2,
                       (float)held / UNSTOCK_HOLD_FRAMES);
}

/* Game thread, at the pickup; `total` is the new total. */
void PortNotifyMooglePoints(unsigned gained, unsigned total) {
    sGained = sState == NOTICE_GAINED ? sGained + gained : gained;
    sTotal = total;
    sState = NOTICE_GAINED;
    sTimer = SHOW_FRAMES;
    sDirty = 1;
}

static void Draw(const char* text, uint32_t col) {
    int w = TextWidth(text, 1) + 12;
    int x = NOTICE_W - w;

    memset(sCanvas, 0, sizeof(sCanvas));
    TextFill(sCanvas, NOTICE_W, NOTICE_H, x, 0, w, NOTICE_H, COL_BORDER);
    TextFill(sCanvas, NOTICE_W, NOTICE_H, x + 1, 1, w - 2, NOTICE_H - 2, COL_PANEL);
    TextDraw(sCanvas, NOTICE_W, NOTICE_H, x + 6, (NOTICE_H - 7) / 2, text, col, 1);
}

/* Game thread, once per frame before presenting it. */
void NoticeUpdate(void) {
    char text[32];

    sFrame++;
    UpdateUnstockBar();
    if (sState != NOTICE_NONE && --sTimer <= 0) {
        sState = sState == NOTICE_GAINED ? NOTICE_TOTAL : NOTICE_NONE;
        sTimer = SHOW_FRAMES;
        sDirty = 1;
    }
    if (sState == NOTICE_NONE || PortUiOverlayActive()) {
        VideoSetNotice(NULL, 0);
        return;
    }
    if (sDirty) {
        if (sState == NOTICE_GAINED) {
            snprintf(text, sizeof(text), "+%u MOOGLE POINTS", sGained);
            Draw(text, COL_GAIN);
        } else {
            snprintf(text, sizeof(text), "MOOGLE POINTS: %u", sTotal);
            Draw(text, COL_TEXT);
        }
        VideoSetNotice(sCanvas, 1);
        sDirty = 0;
        return;
    }
    VideoSetNotice(sCanvas, 0);
}
