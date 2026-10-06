/*
 * Maps the Vita controls to the GBA keypad.
 *
 *   D-pad / left stick  -> D-pad
 *   Cross / Circle      -> A / B (swap with swap_ab=1 in config.ini)
 *   L / R               -> L / R
 *   Start / Select      -> Start / Select
 *   Start + L + R       -> port menu (vita_menu.c); Start is not passed to
 *                          the game while L and R are held
 *
 * Additions, each can be turned off in the menu's controls tab:
 *   Triangle            -> L + R together (stock a card / sleight)
 *   Square              -> dodge roll (see btl.c, task_btl_sora_1)
 *   Rear touch          -> card battles: the stocked cards go back to the
 *                          hand (PortTakeUnstockRequest), when held 0.5-2 s
 *                          or tapped twice within half a second (menu)
 *   Right stick         -> left / right: L / R (previous / next card),
 *                          up: L + R, down held as long: as the rear touch
 */
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include "port.h"
#include "vita_host.h"

#define GBA_A 0x0001
#define GBA_B 0x0002
#define GBA_SELECT 0x0004
#define GBA_START 0x0008
#define GBA_RIGHT 0x0010
#define GBA_LEFT 0x0020
#define GBA_UP 0x0040
#define GBA_DOWN 0x0080
#define GBA_R 0x0100
#define GBA_L 0x0200

#define STICK_DEADZONE 48

uint8_t gPortDodgePressed;

static uint16_t sKeys;
static uint32_t sPrevButtons;
static int sMenuRequest;

#define MENU_CHORD (SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)

/* Start is held back from the game for a few frames: pressed a moment before
 * the shoulders, the Start of the menu chord used to reach the game too and
 * open its own pause menu under the port menu (which then also left the
 * field's main loop, so boss practice could not start). A Start that turns
 * out to be part of the chord never reaches the game; a short tap is passed
 * on after its release. */
#define START_HOLD_BACK 4
static int sStartFrames; /* frames Start has been held */
static int sStartChord;  /* this press of Start went with a shoulder */
static int sStartPulse;  /* frames left of a released tap to pass on */

/* Unstock: frames the rear touch / right stick down were held, and a request
 * waiting for a battle to take it (it expires so one made outside a battle
 * doesn't fire later). */
#define UNSTOCK_PENDING_FRAMES 6
static int sRearFrames;
static int sStickDownFrames;
static int sUnstockPending;

/* Double tap on the rear touch: two touches, each short, the second starting
 * within DOUBLE_TAP_FRAMES of the first. */
#define DOUBLE_TAP_FRAMES 30
static int sTouchDown;    /* the rear pad is being touched */
static int sTouchFrames;  /* ... for this many frames */
static int sTapAge = -1;  /* frames since the last short tap began, -1: none */

/* Right stick: a direction is taken once the stick is pushed past
 * RSTICK_PUSH, from the axis it is pushed further along, and kept until the
 * stick is back near the centre, so sliding from the side to the top doesn't
 * press L or R first. Sticks rarely travel straight, so the first frames
 * past the threshold can lean the wrong way: for RSTICK_SETTLE frames the
 * direction still follows the stick, and only then is it kept. */
#define RSTICK_PUSH 64
#define RSTICK_RELEASE 40
#define RSTICK_SETTLE 4
enum { RSTICK_NONE, RSTICK_LEFT, RSTICK_RIGHT, RSTICK_UP, RSTICK_DOWN };
static int sStickDir;
static int sStickFrames; /* frames since the stick left the centre */

/* Frames the current unstock hold has lasted (0 when none). */
int InputUnstockHoldFrames(void) {
    return sRearFrames > sStickDownFrames ? sRearFrames : sStickDownFrames;
}

int PortTakeUnstockRequest(void) {
    int r = sUnstockPending > 0;

    sUnstockPending = 0;
    return r;
}

int InputTakeMenuRequest(void) {
    int r = sMenuRequest;

    sMenuRequest = 0;
    return r;
}

void InputInit(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

/* Counts a hold; fires the unstock request once per hold. */
static void HoldUnstock(int held, int* frames) {
    if (!held) {
        *frames = 0;
    } else if (++*frames == UNSTOCK_HOLD_FRAMES) {
        sUnstockPending = UNSTOCK_PENDING_FRAMES;
    }
}

static void PollRearTouch(void) {
    SceTouchData touch;
    int mode = gPortConfig.touchUnstock;
    int held = 0;

    if (mode != TOUCH_UNSTOCK_OFF && sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) >= 1) {
        held = touch.reportNum > 0;
    }
    HoldUnstock(held && (mode == TOUCH_UNSTOCK_HOLD || mode == TOUCH_UNSTOCK_BOTH), &sRearFrames);

    if (sTapAge >= 0 && ++sTapAge > DOUBLE_TAP_FRAMES) {
        sTapAge = -1;
    }
    if (held && !sTouchDown) {
        /* A new touch: the second of a double tap, or maybe the first. */
        if (sTapAge >= 0 && (mode == TOUCH_UNSTOCK_DOUBLE_TAP || mode == TOUCH_UNSTOCK_BOTH)) {
            sUnstockPending = UNSTOCK_PENDING_FRAMES;
            sTapAge = -1;
        } else {
            sTapAge = 0;
        }
        sTouchFrames = 0;
    }
    if (held && ++sTouchFrames > DOUBLE_TAP_FRAMES) {
        sTapAge = -1; /* a hold, not a tap */
    }
    sTouchDown = held;
}

static uint16_t PollRightStick(const SceCtrlData* pad) {
    int dx = (int)pad->rx - 128, dy = (int)pad->ry - 128;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;

    int prev = sStickDir;

    if (!gPortConfig.rightStick || (ax < RSTICK_RELEASE && ay < RSTICK_RELEASE)) {
        sStickDir = RSTICK_NONE;
        sStickFrames = 0;
    } else if (ax >= RSTICK_PUSH || ay >= RSTICK_PUSH) {
        if (sStickDir == RSTICK_NONE || sStickFrames < RSTICK_SETTLE) {
            if (ax >= ay) {
                sStickDir = dx < 0 ? RSTICK_LEFT : RSTICK_RIGHT;
            } else {
                sStickDir = dy < 0 ? RSTICK_UP : RSTICK_DOWN;
            }
        }
        sStickFrames++;
    }
    if (sStickDir != prev) {
        static const char* const names[] = { "none", "left", "right", "up", "down" };
        PortLog("input: right stick %s (rx %d ry %d)", names[sStickDir], pad->rx, pad->ry);
    }
    /* Nothing reaches the game until the direction has settled (4 frames). */
    if (sStickFrames < RSTICK_SETTLE) {
        HoldUnstock(0, &sStickDownFrames);
        return 0;
    }
    HoldUnstock(sStickDir == RSTICK_DOWN, &sStickDownFrames);
    switch (sStickDir) {
    case RSTICK_LEFT:
        return GBA_L;
    case RSTICK_RIGHT:
        return GBA_R;
    case RSTICK_UP:
        return GBA_L | GBA_R;
    }
    return 0;
}

void InputPoll(void) {
    SceCtrlData pad;
    uint16_t keys = 0;
    uint32_t b;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) {
        return;
    }
    b = pad.buttons;

    if (b & SCE_CTRL_UP) keys |= GBA_UP;
    if (b & SCE_CTRL_DOWN) keys |= GBA_DOWN;
    if (b & SCE_CTRL_LEFT) keys |= GBA_LEFT;
    if (b & SCE_CTRL_RIGHT) keys |= GBA_RIGHT;
    if (pad.lx < 128 - STICK_DEADZONE) keys |= GBA_LEFT;
    if (pad.lx > 128 + STICK_DEADZONE) keys |= GBA_RIGHT;
    if (pad.ly < 128 - STICK_DEADZONE) keys |= GBA_UP;
    if (pad.ly > 128 + STICK_DEADZONE) keys |= GBA_DOWN;

    if (b & SCE_CTRL_CROSS) keys |= gPortConfig.swapAB ? GBA_B : GBA_A;
    if (b & SCE_CTRL_CIRCLE) keys |= gPortConfig.swapAB ? GBA_A : GBA_B;
    if (b & SCE_CTRL_LTRIGGER) keys |= GBA_L;
    if (b & SCE_CTRL_RTRIGGER) keys |= GBA_R;
    /* Start with a shoulder held may be the menu chord (see START_HOLD_BACK). */
    if (b & SCE_CTRL_START) {
        sStartFrames++;
        if (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
            sStartChord = 1;
        }
        if (!sStartChord && sStartFrames > START_HOLD_BACK) {
            keys |= GBA_START;
        }
    } else {
        if (sStartFrames > 0 && sStartFrames <= START_HOLD_BACK && !sStartChord) {
            sStartPulse = 2;
        }
        sStartFrames = 0;
        sStartChord = 0;
    }
    if (sStartPulse > 0) {
        keys |= GBA_START;
        sStartPulse--;
    }
    if ((b & MENU_CHORD) == MENU_CHORD && (sPrevButtons & MENU_CHORD) != MENU_CHORD) {
        sMenuRequest = 1;
    }
    if (b & SCE_CTRL_SELECT) keys |= GBA_SELECT;
    if ((b & SCE_CTRL_TRIANGLE) && gPortConfig.triangleLR) keys |= GBA_L | GBA_R;
    if (sUnstockPending > 0) {
        sUnstockPending--;
    }
    keys |= PollRightStick(&pad);
    PollRearTouch();

    /* Opposite directions cancel out, as on the real pad. */
    if ((keys & (GBA_LEFT | GBA_RIGHT)) == (GBA_LEFT | GBA_RIGHT)) {
        keys &= ~(GBA_LEFT | GBA_RIGHT);
    }
    if ((keys & (GBA_UP | GBA_DOWN)) == (GBA_UP | GBA_DOWN)) {
        keys &= ~(GBA_UP | GBA_DOWN);
    }

    /* Stays pending for a few frames so a lag frame in the game loop can't drop it. */
    if ((b & SCE_CTRL_SQUARE) && !(sPrevButtons & SCE_CTRL_SQUARE) && gPortConfig.squareDodge) {
        gPortDodgePressed = 6;
    } else if (gPortDodgePressed != 0) {
        gPortDodgePressed--;
    }
    sPrevButtons = b;
    sKeys = keys;
}

uint16_t PortReadKeys(void) {
    return sKeys;
}
