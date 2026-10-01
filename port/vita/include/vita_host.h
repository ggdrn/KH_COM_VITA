#ifndef GUARD_VITA_HOST_H
#define GUARD_VITA_HOST_H

#include <stdint.h>

/* Host-side (Vita ABI) modules of the port. */

enum {
    DISPLAY_WIDE,
    DISPLAY_FIT,
    DISPLAY_STRETCH,
};

enum {
    FILTER_LINEAR,
    FILTER_NEAREST,
};

/* Pixel-art upscalers applied when presenting the frame (vita_video.c). */
enum {
    UPSCALE_OFF,
    UPSCALE_SCALE3X,
    UPSCALE_COUNT,
};

typedef struct PortConfig {
    int display;
    int filter;   /* linear/nearest, used when the enhancements below are off */
    int swapAB;
    /* Picture enhancements (Start + L + R menu, vita_menu.c). */
    int upscale;  /* UPSCALE_* */
    int sharp;    /* sharp bilinear: even, crisp pixels (when upscale is off) */
    int gbaColors;/* GBA LCD colour correction */
    /* Control additions (Start + L + R menu, controls tab; vita_input.c). */
    int touchUnstock;  /* rear touch held 1 s: stocked cards back to the hand */
    int squareDodge;   /* Square: dodge roll */
    int triangleLR;    /* Triangle: L + R (stock a card / sleight) */
    int rightStick;    /* right stick: L / R / L + R, down held 1 s unstocks */
    /* Game options (menu, game tab). */
    int fieldHud;      /* HP display while exploring the map (src/btl/btl2.c) */
    int saveBank;      /* 1..SAVE_BANKS: which save file holds the game's two slots */
    int wideMenus;     /* pause / save menus widened to the whole screen */
} PortConfig;

/* The game has two save slots; the port keeps SAVE_BANKS files of them, so
 * 10 slots in all. The bank is chosen in the menu and swapped into SRAM right
 * away (vita_main.c): the game keeps running, its save and load screens use
 * the new bank from then on. */
#define SAVE_BANKS 5
void PortSetSaveBank(int bank);

extern PortConfig gPortConfig;

void PortFlushSram(void);
void PortSaveConfig(void);

void RenderInit(int width);
/* Scale3x of rows [y0, y1) of a w x h frame into a 3w x 3h one, dstStride
 * pixels per line (scale3x.c). */
void Scale3xRows(const uint32_t* src, int w, int h, uint32_t* dst, int dstStride, int y0, int y1);
/* Scale3x frames go straight into these textures' memory (vita_video.c). */
#define SCALED_SLOTS 4
void RenderSetScaledSlots(uint32_t* const data[SCALED_SLOTS], int stride);
/* Returns the latest frame and sets *slot to the texture slot holding its
 * Scale3x, or -1 when the upscaler is off, and *wide when it is a menu to
 * stretch to the whole screen; valid until RenderUnlockFront. */
const uint32_t* RenderLockFront(int* slot, int* wide);
void RenderUnlockFront(void);
extern volatile uint32_t gPortRenderUs;
extern volatile uint32_t gPortScaleUs;
extern volatile uint32_t gPortCaptureWaitUs;

void VideoInit(void);
void VideoPresent(void);
/* RGBA overlay drawn over the game (MENU_W x MENU_H, scaled 2x), or NULL. */
#define MENU_W 480
#define MENU_H 272
void VideoSetOverlay(const uint32_t* rgba);
void VideoShowFatal(const char* msg);
/* Moogle points notice (vita_notice.c): NOTICE_W x NOTICE_H canvas drawn 2x
 * in the bottom-right corner, or NULL; `changed` when it must be re-uploaded. */
#define NOTICE_W 160
#define NOTICE_H 15
void VideoSetNotice(const uint32_t* rgba, int changed);
/* Unstock progress bar: span and top in GBA screen pixels, progress 0..1;
 * progress < 0 hides it. */
void VideoSetUnstockBar(int x0, int x1, int y, float progress);
void NoticeUpdate(void);
int InputUnstockHoldFrames(void);
#define UNSTOCK_HOLD_FRAMES 60

/* Text on RGBA canvases, cw x ch pixels (vita_text.c). Characters are
 * 6 x 8 cells at scale 1. */
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
void TextFill(uint32_t* canvas, int cw, int ch, int x, int y, int w, int h, uint32_t rgba);
void TextDraw(uint32_t* canvas, int cw, int ch, int x, int y, const char* s, uint32_t rgba, int scale);
int TextWidth(const char* s, int scale);

void FaultInit(void);
void FaultPoll(void);
void WatchdogInit(void);

void InputInit(void);
void InputPoll(void);
/* Start + L + R was pressed: open the enhancements menu (once per press). */
int InputTakeMenuRequest(void);
/* Enhancements menu: runs until closed, the game paused meanwhile. */
void MenuRun(void);
extern volatile int gPortMenuOpen;

void AudioInit(void);
/* Fills the executable's ROM data from the player's ROM (vita_rom.c). */
void RomLoad(void);
void PsgRender(int16_t* out, int samples, int rate);
void AudioPump(void);

#endif /* GUARD_VITA_HOST_H */
