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
    UPSCALE_SCALE2X,
    UPSCALE_SCALE3X,
    UPSCALE_MMPX,
    UPSCALE_COUNT,
};
/* How much each upscaler enlarges: 3 for Scale3x, 2 for the others; 1 with
 * the upscaler off (the frame is copied as it is, see Copy1xRows). */
#define UPSCALE_FACTOR(mode) ((mode) == UPSCALE_SCALE3X ? 3 : (mode) == UPSCALE_OFF ? 1 : 2)

typedef struct PortConfig {
    int display;
    int filter;   /* linear/nearest, used when the enhancements below are off */
    int swapAB;
    /* Picture enhancements (Start + L + R menu, vita_menu.c). */
    int upscale;  /* UPSCALE_* */
    int upscaleTarget; /* UPSCALE_TARGET_* */
    int sharp;    /* sharp bilinear: even, crisp pixels (when upscale is off) */
    int gbaColors;/* GBA LCD colour correction */
    /* Control additions (Start + L + R menu, controls tab; vita_input.c). */
    int touchUnstock;  /* TOUCH_UNSTOCK_*: rear touch returns the stocked cards to the hand */
    int unstockHold;   /* 0-3: hold the rear touch / right stick down 0.5, 1, 1.5 or 2 s */
    int squareDodge;   /* Square: dodge roll */
    int triangleLR;    /* Triangle: L + R (stock a card / sleight) */
    int rightStick;    /* right stick: L / R / L + R, down held unstocks */
    /* Game options (menu, game tab). */
    int fieldHud;      /* HP display while exploring the map (src/btl/btl2.c) */
    int saveBank;      /* 1..SAVE_BANKS: which save file holds the game's two slots */
    int wideMenus;     /* pause / save menus widened to the whole screen */
    int skipIntro;     /* launch straight into the title menu */
    int dumpFrames;    /* config.ini only: save some frames' renderer input (vita_render.c) */
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
void Scale3xRows(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst, int dstStride,
                 int y0, int y1);
/* Scale2x of rows [y0, y1) into a 2w x 2h frame (scale2x.c). */
void Scale2xRows(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst, int dstStride,
                 int y0, int y1);
/* MMPX of rows [y0, y1) into a 2w x 2h frame (mmpx.c). */
void Mmpx2xRows(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst, int dstStride,
                int y0, int y1);
/* What the upscaler smooths: everything, or only the pixels where a sprite is
 * (or is not) in front, per the renderer's mask; the rest stays sharp. */
enum { UPSCALE_TARGET_ALL, UPSCALE_TARGET_SPRITES, UPSCALE_TARGET_SCENERY, UPSCALE_TARGET_COUNT };
/* Whether pixel i is one the upscaler leaves alone. */
#define UPSCALE_SKIP(mask, target, i) \
    ((target) != UPSCALE_TARGET_ALL && (((mask)[i] != 0) != ((target) == UPSCALE_TARGET_SPRITES)))
/* Scale3x frames go straight into these textures' memory (vita_video.c). */
#define SCALED_SLOTS 4
void RenderSetScaledSlots(uint32_t* const data[SCALED_SLOTS], int stride);
/* Returns the latest frame and sets *slot to the texture slot holding it
 * enlarged by *factor (UPSCALE_FACTOR; the slots are sized for 3x), or -1
 * when the upscaler is off, and *wide when it is a menu to stretch to the
 * whole screen; valid until RenderUnlockFront. */
const uint32_t* RenderLockFront(int* slot, int* factor, int* wide);
void RenderUnlockFront(void);
extern volatile uint32_t gPortRenderUs;
extern volatile uint32_t gPortScaleUs;
/* Frames the game submitted unchanged, so not drawn again (vita_render.c). */
extern volatile uint32_t gPortSkippedFrames;
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
/* How the rear touch unstocks: held (unstockHold), tapped twice within half a
 * second, or both. */
enum { TOUCH_UNSTOCK_OFF, TOUCH_UNSTOCK_HOLD, TOUCH_UNSTOCK_DOUBLE_TAP, TOUCH_UNSTOCK_BOTH, TOUCH_UNSTOCK_COUNT };
#define UNSTOCK_HOLD_STEPS 4
#define UNSTOCK_HOLD_FRAMES ((gPortConfig.unstockHold + 1) * 30)

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
