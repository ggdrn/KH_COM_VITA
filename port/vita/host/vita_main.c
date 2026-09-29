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
#define SAVE_PATH DATA_DIR "/khcom.sav"
#define LOG_PATH DATA_DIR "/log.txt"
#define CONFIG_PATH DATA_DIR "/config.ini"

PortConfig gPortConfig = {
    .display = DISPLAY_WIDE,
    .filter = FILTER_LINEAR,
    .swapAB = 0,
};

static SceUID sLogFd = -1;
int gPortTraceFrames = 3;
static int sSramDirty;
static int sSramDirtyFrames;

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

static void LoadSram(void) {
    SceUID fd = sceIoOpen(SAVE_PATH, SCE_O_RDONLY, 0);

    memset(gGbaSram, 0xFF, sizeof(gGbaSram));
    if (fd >= 0) {
        sceIoRead(fd, gGbaSram, sizeof(gGbaSram));
        sceIoClose(fd);
    }
}

void PortFlushSram(void) {
    SceUID fd;

    if (!sSramDirty) {
        return;
    }
    fd = sceIoOpen(SAVE_PATH ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        PortLog("save: cannot open %s (%08X)", SAVE_PATH ".tmp", fd);
        return;
    }
    sceIoWrite(fd, gGbaSram, sizeof(gGbaSram));
    sceIoClose(fd);
    sceIoRemove(SAVE_PATH);
    sceIoRename(SAVE_PATH ".tmp", SAVE_PATH);
    sSramDirty = 0;
}

void PortSramWritten(void) {
    sSramDirty = 1;
    /* The game writes a save in several chunks; flush once it settles. */
    sSramDirtyFrames = 30;
}

/* Config ------------------------------------------------------------------------ */

static void LoadConfig(void) {
    char buf[1024];
    SceUID fd = sceIoOpen(CONFIG_PATH, SCE_O_RDONLY, 0);
    int len;
    char* line;

    if (fd < 0) {
        fd = sceIoOpen(CONFIG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            static const char defaults[] =
                "# display: wide (16:9, shows more of the scene), fit (original 3:2), stretch\n"
                "display=wide\n"
                "# filter: linear or nearest\n"
                "filter=linear\n"
                "# swap_ab=1 maps Circle to GBA A and Cross to GBA B\n"
                "swap_ab=0\n";
            sceIoWrite(fd, defaults, sizeof(defaults) - 1);
            sceIoClose(fd);
        }
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
                "avg us: logic=%u render=%u renderWait=%u present=%u maxFrame=%u",
                frames, (unsigned)gPortVBlankIrqs, (unsigned)gPortModeUpdates, io[0], io[0x50 / 2],
                ((const uint16_t*)gGbaPltt)[0], PortReadKeys(), (unsigned)(sSumLogic / n),
                (unsigned)(sSumRender / n), (unsigned)(sSumWait / n), (unsigned)(sSumPresent / n),
                (unsigned)sMaxFrame);
        sSumLogic = sSumPresent = sSumRender = sSumWait = sMaxFrame = sSamples = 0;
    }

    PORT_TRACE_EARLY("present: VideoPresent");
    VideoPresent();
    afterPresent = sceKernelGetProcessTimeWide();
    sSumPresent += (uint32_t)(afterPresent - enter);
    if (sLastReturn != 0 && afterPresent - sLastReturn > sMaxFrame) {
        sMaxFrame = (uint32_t)(afterPresent - sLastReturn);
    }
    PORT_TRACE_EARLY("present: InputPoll");
    InputPoll();
    AudioPump();
    FaultPoll();
    if (sSramDirtyFrames > 0 && --sSramDirtyFrames == 0) {
        PortFlushSram();
    }
    sLastReturn = sceKernelGetProcessTimeWide();
}

int main(void) {
    sceIoMkdir(DATA_DIR, 0777);
    /* Keep the previous run's log: relaunching after a crash would overwrite it. */
    sceIoRemove(DATA_DIR "/log_prev.txt");
    sceIoRename(LOG_PATH, DATA_DIR "/log_prev.txt");
    PortLog("KH:COM Vita port v%s starting", PORT_VERSION);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    /* Core 0: game logic, core 1: rendering (vita_render.c), core 2: audio. */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    LoadConfig();
    FaultInit();
    LoadSram();
    VideoInit();
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
