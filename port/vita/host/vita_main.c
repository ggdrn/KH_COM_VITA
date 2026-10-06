/*
 * PS Vita entry point, configuration, logging, SRAM persistence and frame pacing.
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/display.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"
#include "vita_host.h"

/* The game's call depth is modest but the task system nests callbacks. */
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
int _newlib_heap_size_user = 128 * 1024 * 1024;

#define DATA_DIR "ux0:data/khcom"
/* The European version keeps its own saves (they also hold the language):
 * khcom_eu.sav, khcom_eu2.sav... */
#ifdef VERSION_EU
#define SAVE_NAME "khcom_eu"
#else
#define SAVE_NAME "khcom"
#endif
#define LOG_PATH DATA_DIR "/log.txt"
#define CONFIG_PATH DATA_DIR "/config.ini"

PortConfig gPortConfig = {
    .display = DISPLAY_WIDE,
    .filter = FILTER_LINEAR,
    .swapAB = 0,
    /* Picture: crisp pixels and GBA colours; the upscaler is opt-in. */
    .upscale = UPSCALE_OFF,
    /* MMPX on the whole picture costs ~12 ms a frame on the Vita's CPU (the
     * scenery's texture keeps it out of its fast paths): sprites only. */
    .upscaleTarget = UPSCALE_TARGET_SPRITES,
    .sharp = 1,
    .gbaColors = 1,
    .touchUnstock = TOUCH_UNSTOCK_HOLD,
    .unstockHold = 1,
    .squareDodge = 1,
    .triangleLR = 1,
    .rightStick = 1,
    .fieldHud = 1,
    .saveBank = 1,
    .wideMenus = 1,
    .skipIntro = 0,
};

static const char* const sUpscaleNames[UPSCALE_COUNT] = { "off", "scale2x", "scale3x", "mmpx" };
static const char* const sTargetNames[UPSCALE_TARGET_COUNT] = { "all", "sprites", "scenery" };
static const char* const sTouchNames[TOUCH_UNSTOCK_COUNT] = { "off", "hold", "double_tap", "both" };
static const char* const sHoldNames[UNSTOCK_HOLD_STEPS] = { "0.5", "1", "1.5", "2" };

static SceUID sLogFd = -1;
int gPortTraceFrames = 3;
static volatile int sSramDirty;
static int sSramDirtyFrames; /* frames until the pending save is written */
static int sSramMaxFrames;   /* ... at the latest, while the game keeps writing */
static SceUID sSramLock = -1;
/* The save file in use: bank 1 is khcom.sav (as before banks, and the name
 * a save from a GBA emulator is copied to), bank n is khcom<n>.sav. */
static char sSavePath[64];
static char sSaveTmpPath[68];
static int sSaveBank;

void PortLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len < 0) {
        return;
    }
    if (len > (int)sizeof(buf) - 2) {
        len = sizeof(buf) - 2;
    }
    buf[len++] = '\n';
    if (sLogFd < 0) {
        sLogFd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    }
    if (sLogFd >= 0) {
        sceIoWrite(sLogFd, buf, len);
    }
}

void PortFatal(const char* fmt, ...) {
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    PortLog("FATAL: %s", buf);
    VideoShowFatal(buf);
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}

void PortSoftReset(void) {
    int ret;

    PortLog("soft reset requested");
    PortFlushSram();
    ret = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    PortLog("soft reset: sceAppMgrLoadExec failed (%08X)", ret);
    for (;;) {
        sceKernelDelayThread(1000000);
    }
}

/* SRAM ------------------------------------------------------------------------- */

/* Size of a file, or -1. */
static int FileSize(const char* path) {
    SceIoStat st;

    return sceIoGetstat(path, &st) < 0 ? -1 : (int)st.st_size;
}

/* Reads the save file of gPortConfig.saveBank into SRAM. */
static void LoadSram(void) {
    SceUID fd;

    sSaveBank = gPortConfig.saveBank;
    if (sSaveBank <= 1) {
        snprintf(sSavePath, sizeof(sSavePath), "%s/%s.sav", DATA_DIR, SAVE_NAME);
    } else {
        snprintf(sSavePath, sizeof(sSavePath), "%s/%s%d.sav", DATA_DIR, SAVE_NAME, sSaveBank);
    }
    snprintf(sSaveTmpPath, sizeof(sSaveTmpPath), "%s.tmp", sSavePath);

    /* A write cut short (the app closed meanwhile) leaves the new save in the
     * .tmp file: complete, if it has the full size. */
    if (FileSize(sSaveTmpPath) == (int)sizeof(gGbaSram)) {
        if (FileSize(sSavePath) >= 0) {
            sceIoRemove(sSavePath);
        }
        sceIoRename(sSaveTmpPath, sSavePath);
        PortLog("save: recovered %s from an interrupted write", sSavePath);
    } else if (FileSize(sSaveTmpPath) >= 0) {
        sceIoRemove(sSaveTmpPath);
    }

    memset(gGbaSram, 0xFF, sizeof(gGbaSram));
    fd = sceIoOpen(sSavePath, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        int n = sceIoRead(fd, gGbaSram, sizeof(gGbaSram));
        sceIoClose(fd);
        PortLog("save: bank %d, %s (%d bytes)", sSaveBank, sSavePath, n);
    } else {
        PortLog("save: bank %d, %s (new)", sSaveBank, sSavePath);
    }
}

/* Called from the game thread and from the power callback thread. */
void PortFlushSram(void) {
    SceUID fd;

    if (sSramLock >= 0) {
        sceKernelLockMutex(sSramLock, 1, NULL);
    }
    if (sSramDirty) {
        sSramDirty = 0;
        fd = sceIoOpen(sSaveTmpPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd < 0) {
            PortLog("save: cannot open %s (%08X)", sSaveTmpPath, fd);
            sSramDirty = 1;
        } else {
            int n = sceIoWrite(fd, gGbaSram, sizeof(gGbaSram));

            sceIoClose(fd);
            if (n != (int)sizeof(gGbaSram)) {
                PortLog("save: write failed (%08X)", n);
                sSramDirty = 1;
            } else {
                /* The old file is only replaced by a complete new one; if the
                 * app closes in between, LoadSram finishes the job. */
                sceIoRemove(sSavePath);
                sceIoRename(sSaveTmpPath, sSavePath);
                PortLog("save: written to %s", sSavePath);
            }
        }
    }
    if (sSramLock >= 0) {
        sceKernelUnlockMutex(sSramLock, 1);
    }
}

/* Write a pending save before the console suspends (standby, or the PS
 * button then closing the app): the delayed flush may never get to run. */
static int PowerCallback(int notifyId, int count, int powerInfo, void* common) {
    (void)notifyId;
    (void)count;
    (void)common;
    if (powerInfo & (SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_THERMAL_SUSPEND |
                     SCE_POWER_CB_LOW_BATTERY_SUSPEND | SCE_POWER_CB_BUTTON_PS_PRESS |
                     SCE_POWER_CB_BUTTON_POWER_PRESS)) {
        PortLog("power: suspending (%08X), flushing save", powerInfo);
        PortFlushSram();
    }
    return 0;
}

static int PowerThread(SceSize args, void* argp) {
    SceUID cb = sceKernelCreateCallback("khcom_power", 0, PowerCallback, NULL);

    (void)args;
    (void)argp;
    scePowerRegisterCallback(cb);
    for (;;) {
        sceKernelDelayThreadCB(1000000);
    }
    return 0;
}

static void PowerInit(void) {
    SceUID thid;

    sSramLock = sceKernelCreateMutex("khcom_sram", 0, 0, NULL);
    thid = sceKernelCreateThread("khcom_power", PowerThread, 0x10000100, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
}

void PortSramWritten(void) {
    /* The game writes a save in several chunks: write the file once they stop
     * (10 frames), or after 1 s at the latest, so closing the app right after
     * saving keeps the save. */
    if (!sSramDirty || sSramMaxFrames == 0) {
        sSramMaxFrames = 60;
    }
    sSramDirty = 1;
    sSramDirtyFrames = 10;
}

int PortSkipIntro(void) {
    return gPortConfig.skipIntro;
}

/* Game thread (the menu runs there, with the game paused). */
void PortSetSaveBank(int bank) {
    if (bank == sSaveBank) {
        return;
    }
    PortLog("save: switching to bank %d", bank);
    PortFlushSram();
    if (sSramLock >= 0) {
        sceKernelLockMutex(sSramLock, 1, NULL);
    }
    gPortConfig.saveBank = bank;
    LoadSram();
    sSramDirty = 0;
    sSramDirtyFrames = 0;
    sSramMaxFrames = 0;
    if (sSramLock >= 0) {
        sceKernelUnlockMutex(sSramLock, 1);
    }
    /* A new bank gets formatted (its writes go to the new file). */
    PortCheckSaveBank();
    PortSaveConfig();
}

/* Config ------------------------------------------------------------------------ */

void PortSaveConfig(void) {
    static const char* const displays[] = { "wide", "fit", "stretch" };
    char buf[2048];
    int len;
    SceUID fd;

    len = snprintf(buf, sizeof(buf),
                   "# display: wide (16:9, shows more of the scene), fit (original 3:2), stretch\n"
                   "display=%s\n"
                   "# filter: linear or nearest (used when upscale=off and sharp=0)\n"
                   "filter=%s\n"
                   "# swap_ab=1 maps Circle to GBA A and Cross to GBA B\n"
                   "swap_ab=%d\n"
                   "# Picture enhancements (also in the Start + L + R menu):\n"
                   "# upscale: mmpx (sharper, keeps outlines), scale2x or scale3x (rounder) or off\n"
                   "upscale=%s\n"
                   "# upscale_target: all, sprites or scenery (what upscale smooths)\n"
                   "upscale_target=%s\n"
                   "# sharp=1: crisp, even pixels when upscale=off\n"
                   "sharp=%d\n"
                   "# gba_colors=1: colours as on the GBA's screen\n"
                   "gba_colors=%d\n"
                   "# Control additions (also in the Start + L + R menu), 1 = on:\n"
                   "# touch_unstock: off, hold, double_tap or both: the rear touch returns the\n"
                   "# stocked cards to the hand when held, or tapped twice within half a second\n"
                   "touch_unstock=%s\n"
                   "# unstock_hold: 0.5, 1, 1.5 or 2: seconds to hold the rear touch / right stick down\n"
                   "unstock_hold=%s\n"
                   "# Square: dodge roll\n"
                   "square_dodge=%d\n"
                   "# Triangle: L + R (stock a card / sleight)\n"
                   "triangle_lr=%d\n"
                   "# right stick: left/right = L/R, up = L + R, down held = unstock\n"
                   "right_stick=%d\n"
                   "# Game options (also in the Start + L + R menu):\n"
                   "# field_hud=1: HP display while exploring the map\n"
                   "field_hud=%d\n"
                   "# save_bank: 1-5, which save file the game's two slots use (khcom.sav, khcom2.sav...)\n"
                   "save_bank=%d\n"
                   "# wide_menus=1: menu screens (deck, world map, map/world cards, status, journal) stretched to 16:9\n"
                   "wide_menus=%d\n"
                   "# skip_intro=1: launch straight into the title menu (no logos)\n"
                   "skip_intro=%d\n",
                   displays[gPortConfig.display], gPortConfig.filter == FILTER_NEAREST ? "nearest" : "linear",
                   gPortConfig.swapAB, sUpscaleNames[gPortConfig.upscale],
                   sTargetNames[gPortConfig.upscaleTarget], gPortConfig.sharp, gPortConfig.gbaColors,
                   sTouchNames[gPortConfig.touchUnstock], sHoldNames[gPortConfig.unstockHold],
                   gPortConfig.squareDodge, gPortConfig.triangleLR,
                   gPortConfig.rightStick, gPortConfig.fieldHud, gPortConfig.saveBank,
                   gPortConfig.wideMenus, gPortConfig.skipIntro);
    fd = sceIoOpen(CONFIG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

static void LoadConfig(void) {
    char buf[4096];
    SceUID fd = sceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
    int len, i;
    char* line;

    if (fd < 0) {
        PortSaveConfig();
        return;
    }
    len = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (len <= 0) {
        return;
    }
    buf[len] = '\0';
    for (line = strtok(buf, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
        char* eq = strchr(line, '=');
        if (line[0] == '#' || eq == NULL) {
            continue;
        }
        *eq++ = '\0';
        if (!strcmp(line, "display")) {
            if (!strcmp(eq, "fit")) {
                gPortConfig.display = DISPLAY_FIT;
            } else if (!strcmp(eq, "stretch")) {
                gPortConfig.display = DISPLAY_STRETCH;
            } else {
                gPortConfig.display = DISPLAY_WIDE;
            }
        } else if (!strcmp(line, "filter")) {
            gPortConfig.filter = !strcmp(eq, "nearest") ? FILTER_NEAREST : FILTER_LINEAR;
        } else if (!strcmp(line, "swap_ab")) {
            gPortConfig.swapAB = atoi(eq) != 0;
        } else if (!strcmp(line, "upscale")) {
            for (i = 0; i < UPSCALE_COUNT; i++) {
                if (!strcmp(eq, sUpscaleNames[i])) {
                    gPortConfig.upscale = i;
                }
            }
        } else if (!strcmp(line, "upscale_target")) {
            for (i = 0; i < UPSCALE_TARGET_COUNT; i++) {
                if (!strcmp(eq, sTargetNames[i])) {
                    gPortConfig.upscaleTarget = i;
                }
            }
        } else if (!strcmp(line, "sharp")) {
            gPortConfig.sharp = atoi(eq) != 0;
        } else if (!strcmp(line, "gba_colors")) {
            gPortConfig.gbaColors = atoi(eq) != 0;
        } else if (!strcmp(line, "touch_unstock")) {
            /* Also 0 / 1 (off / hold), as before there were more ways. */
            gPortConfig.touchUnstock = atoi(eq) != 0 ? TOUCH_UNSTOCK_HOLD : TOUCH_UNSTOCK_OFF;
            for (i = 0; i < TOUCH_UNSTOCK_COUNT; i++) {
                if (!strcmp(eq, sTouchNames[i])) {
                    gPortConfig.touchUnstock = i;
                }
            }
        } else if (!strcmp(line, "unstock_hold")) {
            for (i = 0; i < UNSTOCK_HOLD_STEPS; i++) {
                if (!strcmp(eq, sHoldNames[i])) {
                    gPortConfig.unstockHold = i;
                }
            }
        } else if (!strcmp(line, "square_dodge")) {
            gPortConfig.squareDodge = atoi(eq) != 0;
        } else if (!strcmp(line, "triangle_lr")) {
            gPortConfig.triangleLR = atoi(eq) != 0;
        } else if (!strcmp(line, "right_stick")) {
            gPortConfig.rightStick = atoi(eq) != 0;
        } else if (!strcmp(line, "skip_intro")) {
            gPortConfig.skipIntro = atoi(eq) != 0;
        } else if (!strcmp(line, "wide_menus")) {
            gPortConfig.wideMenus = atoi(eq) != 0;
        } else if (!strcmp(line, "field_hud")) {
            gPortConfig.fieldHud = atoi(eq) != 0;
        } else if (!strcmp(line, "save_bank")) {
            i = atoi(eq);
            gPortConfig.saveBank = i >= 1 && i <= SAVE_BANKS ? i : 1;
        }
    }
}

/* Frame --------------------------------------------------------------------------- */

/* Per-second averages (microseconds) for the status log. */
static SceUInt64 sLastReturn;
static uint32_t sSumLogic, sSumPresent, sSumRender, sSumWait, sMaxFrame, sSamples;

void PortVBlankWait(void) {
    static unsigned frames;
    SceUInt64 enter = sceKernelGetProcessTimeWide();
    SceUInt64 afterPresent;

    if (sLastReturn != 0) {
        uint32_t logic = (uint32_t)(enter - sLastReturn);
        sSumLogic += logic;
        sSumRender += gPortRenderUs;
        sSumWait += gPortCaptureWaitUs;
        sSamples++;
    }

    /* Status every second for the first minute, then every 10 s. */
    frames++;
    if (frames <= 5 || (frames <= 3600 && frames % 60 == 0) || frames % 600 == 0) {
        const uint16_t* io = (const uint16_t*)gGbaIo;
        uint32_t n = sSamples ? sSamples : 1;
        PortLog("frame %u: vblankIrq=%u modeUpd=%u DISPCNT=%04X BLDCNT=%04X pal0=%04X keys=%03X | "
                "avg us: logic=%u render=%u renderWait=%u present=%u maxFrame=%u scale=%u",
                frames, (unsigned)gPortVBlankIrqs, (unsigned)gPortModeUpdates, io[0], io[0x50 / 2],
                ((const uint16_t*)gGbaPltt)[0], PortReadKeys(), (unsigned)(sSumLogic / n),
                (unsigned)(sSumRender / n), (unsigned)(sSumWait / n), (unsigned)(sSumPresent / n),
                (unsigned)sMaxFrame, (unsigned)gPortScaleUs);
        sSumLogic = sSumPresent = sSumRender = sSumWait = sMaxFrame = sSamples = 0;
    }

    NoticeUpdate();
    PORT_TRACE_EARLY("present: VideoPresent");
    VideoPresent();
    afterPresent = sceKernelGetProcessTimeWide();
    sSumPresent += (uint32_t)(afterPresent - enter);
    if (sLastReturn != 0 && afterPresent - sLastReturn > sMaxFrame) {
        sMaxFrame = (uint32_t)(afterPresent - sLastReturn);
    }
    PORT_TRACE_EARLY("present: InputPoll");
    InputPoll();
    if (InputTakeMenuRequest()) {
        /* The game waits here, paused, until the menu is closed. */
        MenuRun();
        InputPoll();
    }
    AudioPump();
    FaultPoll();
    if (sSramMaxFrames > 0) {
        sSramMaxFrames--;
    }
    if (sSramDirtyFrames > 0 && (--sSramDirtyFrames == 0 || sSramMaxFrames == 0)) {
        sSramDirtyFrames = 0;
        sSramMaxFrames = 0;
        PortFlushSram();
    }
    sLastReturn = sceKernelGetProcessTimeWide();
}

int main(void) {
    sceIoMkdir(DATA_DIR, 0777);
    /* Keep the previous run's log: relaunching after a crash would overwrite it. */
    sceIoRemove(DATA_DIR "/log_prev.txt");
    sceIoRename(LOG_PATH, DATA_DIR "/log_prev.txt");
#ifdef VERSION_EU
    PortLog("KH:COM Vita port v%s starting (Europe)", PORT_VERSION);
#else
    PortLog("KH:COM Vita port v%s starting", PORT_VERSION);
#endif
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    /* Core 0: game logic, core 1: rendering (vita_render.c), core 2: audio. */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    LoadConfig();
    FaultInit();
    LoadSram();
    PowerInit();
    VideoInit();
    RomLoad();
    InputInit();
    AudioInit();

    /* KEYINPUT is active low: start with every button released, or the
     * first frame reads A+B+Start+Select held (the soft-reset combo). */
    ((uint16_t*)gGbaIo)[0x130 / 2] = 0x3FF;
    WatchdogInit();
    PortLog("init done, entering AgbMain");
    AgbMain();
    return 0;
}
