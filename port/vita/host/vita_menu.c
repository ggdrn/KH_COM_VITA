/*
 * Port menu, opened with Start + L + R (vita_input.c), in two tabs switched
 * with L / R:
 *   PICTURE   the picture enhancements (vita_video.c, vita_render.c),
 *   CONTROLS  the control additions, each of which can be turned off
 *             (vita_input.c).
 *
 * The game is paused while it is open (it runs inside the frame wait, see
 * vita_main.c): the last frame stays on screen with the menu drawn over it,
 * so every picture change is visible immediately. Closing it saves the
 * choices to config.ini.
 */
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

#include "port.h"
#include "vita_host.h"

volatile int gPortMenuOpen;

static uint32_t sCanvas[MENU_W * MENU_H];

#define FillRect(x, y, w, h, rgba) TextFill(sCanvas, MENU_W, MENU_H, x, y, w, h, rgba)
#define DrawText(x, y, s, rgba, scale) TextDraw(sCanvas, MENU_W, MENU_H, x, y, s, rgba, scale)

/* Menu ------------------------------------------------------------------------------ */

#define COL_PANEL RGBA(8, 12, 40, 225)
#define COL_BORDER RGBA(120, 150, 255, 255)
#define COL_TEXT RGBA(235, 235, 235, 255)
#define COL_DIM RGBA(140, 140, 160, 255)
#define COL_SELECT RGBA(255, 214, 64, 255)
#define COL_TAB RGBA(40, 56, 130, 255)

enum { TAB_PICTURE, TAB_CONTROLS, TAB_COUNT };

static const char* const sTabNames[TAB_COUNT] = { "PICTURE", "CONTROLS" };

/* An on/off option, or (toggle == NULL) a special item handled by ItemValue /
 * ChangeItem below. */
typedef struct {
    const char* name;
    const char* help; /* shown under the list while selected */
    int* toggle;
} Item;

enum { ITEM_UPSCALE = 1, ITEM_CLOSE };

#define MAX_ITEMS 5

static const Item sPictureItems[] = {
    { "SMOOTH EDGES", "SCALE3X: ROUNDS THE STAIR STEPS OF THE PIXEL ART", NULL },
    { "SHARP PIXELS", "EVEN CRISP PIXELS (WHEN SMOOTH EDGES IS OFF)", &gPortConfig.sharp },
    { "GBA COLORS", "COLORS AS ON THE GBA SCREEN", &gPortConfig.gbaColors },
    { "CLOSE", NULL, NULL },
};

static const Item sControlItems[] = {
    { "REAR TOUCH: CLEAR CARDS", "HOLD 1S: STOCKED CARDS BACK TO THE HAND", &gPortConfig.touchUnstock },
    { "SQUARE: DODGE ROLL", "SQUARE: DODGE ROLL IN BATTLE", &gPortConfig.squareDodge },
    { "TRIANGLE: SELECT CARDS", "TRIANGLE = L + R: STOCK A CARD / SLEIGHT", &gPortConfig.triangleLR },
    { "RIGHT STICK: CARDS", "LEFT/RIGHT: L/R  UP: L+R  DOWN 1S: CLEAR", &gPortConfig.rightStick },
    { "CLOSE", NULL, NULL },
};

static const Item* TabItems(int tab, int* count) {
    if (tab == TAB_CONTROLS) {
        *count = sizeof(sControlItems) / sizeof(sControlItems[0]);
        return sControlItems;
    }
    *count = sizeof(sPictureItems) / sizeof(sPictureItems[0]);
    return sPictureItems;
}

/* ITEM_UPSCALE, ITEM_CLOSE or 0 (an on/off option). */
static int ItemKind(const Item* it) {
    if (it->toggle != NULL) {
        return 0;
    }
    return it == &sPictureItems[0] ? ITEM_UPSCALE : ITEM_CLOSE;
}

static const char* const sUpscaleLabels[UPSCALE_COUNT] = { "OFF", "SCALE3X" };

static const char* ItemValue(const Item* it) {
    switch (ItemKind(it)) {
    case ITEM_UPSCALE:
        return sUpscaleLabels[gPortConfig.upscale];
    case ITEM_CLOSE:
        return "";
    }
    /* Sharp bilinear only applies when no upscaler redraws the pixels. */
    if (it->toggle == &gPortConfig.sharp && gPortConfig.upscale != UPSCALE_OFF) {
        return *it->toggle ? "ON (NOT USED)" : "OFF (NOT USED)";
    }
    return *it->toggle ? "ON" : "OFF";
}

static void ChangeItem(const Item* it, int dir) {
    if (ItemKind(it) == ITEM_UPSCALE) {
        gPortConfig.upscale = (gPortConfig.upscale + UPSCALE_COUNT + dir) % UPSCALE_COUNT;
    } else if (it->toggle != NULL) {
        *it->toggle = !*it->toggle;
    }
}

static void DrawMenu(int tab, int selected) {
    static const char title[] = "PORT OPTIONS";
    const int pw = MENU_W - 120, ph = 196;
    const int px = (MENU_W - pw) / 2, py = (MENU_H - ph) / 2;
    const Item* items;
    int count, i, x;

    items = TabItems(tab, &count);
    memset(sCanvas, 0, sizeof(sCanvas));
    FillRect(px - 2, py - 2, pw + 4, ph + 4, COL_BORDER);
    FillRect(px, py, pw, ph, COL_PANEL);
    DrawText((MENU_W - TextWidth(title, 2)) / 2, py + 10, title, COL_TEXT, 2);

    /* Tabs, centred, the open one on a lighter background. */
    for (i = 0, x = 0; i < TAB_COUNT; i++) {
        x += TextWidth(sTabNames[i], 1) + 16 + (i > 0 ? 8 : 0);
    }
    x = (MENU_W - x) / 2;
    for (i = 0; i < TAB_COUNT; i++) {
        int w = TextWidth(sTabNames[i], 1) + 16;

        if (i == tab) {
            FillRect(x, py + 32, w, 15, COL_TAB);
        }
        DrawText(x + 8, py + 36, sTabNames[i], i == tab ? COL_SELECT : COL_DIM, 1);
        x += w + 8;
    }
    FillRect(px + 8, py + 48, pw - 16, 1, COL_TAB);

    for (i = 0; i < count; i++) {
        int y = py + 58 + i * 18;
        uint32_t col = i == selected ? COL_SELECT : COL_TEXT;
        const char* v = ItemValue(&items[i]);

        if (i == selected) {
            DrawText(px + 12, y, ">", col, 1);
        }
        DrawText(px + 24, y, items[i].name, col, 1);
        DrawText(px + pw - 16 - TextWidth(v, 1), y, v, col, 1);
    }
    if (items[selected].help != NULL) {
        DrawText(px + 12, py + ph - 44, items[selected].help, COL_TEXT, 1);
    }
    DrawText(px + 12, py + ph - 26, "L/R: TAB  UP/DOWN: SELECT  LEFT/RIGHT/X: CHANGE", COL_DIM, 1);
    DrawText(px + 12, py + ph - 14, "O OR START+L+R: CLOSE", COL_DIM, 1);
}

#define MENU_BUTTONS                                                                                   \
    (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE |    \
     SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START |       \
     SCE_CTRL_SELECT)

static uint32_t ReadButtons(void) {
    SceCtrlData pad;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) {
        return 0;
    }
    return pad.buttons & MENU_BUTTONS;
}

void MenuRun(void) {
    const uint32_t chord = SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    uint32_t prev = ReadButtons();
    static int sTab; /* reopens on the last tab */
    int selected = 0;
    int done = 0;

    gPortMenuOpen = 1;
    PortLog("menu: opened");
    while (!done) {
        uint32_t b = ReadButtons();
        uint32_t pressed = b & ~prev;
        int count;
        const Item* items = TabItems(sTab, &count);

        if ((pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) && (b & chord) != chord) {
            sTab = (sTab + TAB_COUNT + ((pressed & SCE_CTRL_LTRIGGER) ? -1 : 1)) % TAB_COUNT;
            items = TabItems(sTab, &count);
            selected = 0;
        }
        if (pressed & SCE_CTRL_UP) {
            selected = (selected + count - 1) % count;
        }
        if (pressed & SCE_CTRL_DOWN) {
            selected = (selected + 1) % count;
        }
        if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
            if (ItemKind(&items[selected]) == ITEM_CLOSE) {
                done = (pressed & SCE_CTRL_CROSS) != 0;
            } else {
                ChangeItem(&items[selected], (pressed & SCE_CTRL_LEFT) ? -1 : 1);
            }
        }
        if ((pressed & SCE_CTRL_CIRCLE) || ((b & chord) == chord && (prev & chord) != chord)) {
            done = 1;
        }
        prev = b;
        DrawMenu(sTab, selected);
        VideoSetOverlay(sCanvas);
        VideoPresent();
    }
    VideoSetOverlay(NULL);

    /* Let go of every button first, so none of them reaches the game. */
    while (ReadButtons() != 0) {
        VideoPresent();
    }
    PortSaveConfig();
    PortLog("menu: closed (upscale %d, sharp %d, gba colors %d; touch %d, square %d, triangle %d, rstick %d)",
            gPortConfig.upscale, gPortConfig.sharp, gPortConfig.gbaColors, gPortConfig.touchUnstock,
            gPortConfig.squareDodge, gPortConfig.triangleLR, gPortConfig.rightStick);
    gPortMenuOpen = 0;
}
