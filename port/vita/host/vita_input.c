/*
 * Maps the Vita controls to the GBA keypad.
 *
 *   D-pad / left stick  -> D-pad
 *   Cross / Circle      -> A / B (swap with swap_ab=1 in config.ini)
 *   L / R               -> L / R
 *   Triangle            -> L + R together (sleights)
 *   Square              -> dodge roll (see btl.c, task_btl_sora_1)
 *   Start / Select      -> Start / Select
 */
#include <psp2/ctrl.h>

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

void InputInit(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
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
    if (b & SCE_CTRL_START) keys |= GBA_START;
    if (b & SCE_CTRL_SELECT) keys |= GBA_SELECT;
    if (b & SCE_CTRL_TRIANGLE) keys |= GBA_L | GBA_R;

    /* Opposite directions cancel out, as on the real pad. */
    if ((keys & (GBA_LEFT | GBA_RIGHT)) == (GBA_LEFT | GBA_RIGHT)) {
        keys &= ~(GBA_LEFT | GBA_RIGHT);
    }
    if ((keys & (GBA_UP | GBA_DOWN)) == (GBA_UP | GBA_DOWN)) {
        keys &= ~(GBA_UP | GBA_DOWN);
    }

    /* Stays pending for a few frames so a lag frame in the game loop can't drop it. */
    if ((b & SCE_CTRL_SQUARE) && !(sPrevButtons & SCE_CTRL_SQUARE)) {
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
