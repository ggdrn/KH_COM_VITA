/*
 * Renders frames on the other two CPU cores.
 *
 * The game thread captures the IO registers of every line while it runs the
 * HBlank interrupts/DMA (so raster effects are kept), then the palette, OAM and
 * VRAM at the end of the frame, and hands the snapshot over. Two render
 * threads draw the top and bottom halves of that frame while the game thread
 * already runs the next one; the video module always presents the latest
 * finished frame. With the Scale3x upscaler on, the same two threads then
 * enlarge one half each (scale3x.c), so the GPU only has to resize it.
 */
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <string.h>

#include "port.h"
#include "ppu.h"
#include "vita_host.h"

#define SPLIT_LINE (GBA_SCREEN_HEIGHT / 2)

static PpuFrame sFrames[2] __attribute__((aligned(64)));
static uint32_t sRgba[2][GBA_SCREEN_HEIGHT * PORT_MAX_SCREEN_WIDTH] __attribute__((aligned(64)));
/*
 * Scale3x frames are written straight into the memory of SCALED_SLOTS
 * textures, so presenting one is binding it: no upload. (An upload of a
 * texture the GPU drew from recently makes vitaGL reallocate it and copy the
 * old contents back from GPU memory first, which for this 3x frame took
 * longer than a whole frame.) A slot is only reused once it is neither the
 * front frame's nor one of the last two presented, so the GPU is done with it.
 */
static uint32_t* sSlotData[SCALED_SLOTS];
static int sSlotStride;
static int sScaledSlot[2] = { -1, -1 }; /* slot holding the Scale3x of sRgba[i] */
static int sShown[2] = { -1, -1 };      /* slots of the last two presented frames */
static int sTarget;                     /* slot being written */
static volatile int sPhase;             /* what the slice thread does next: 0 render, 1 scale */
static int sWidth;
static int sCapture;          /* snapshot the game thread fills */
static volatile int sPending; /* snapshot handed to the render threads */
static volatile int sBack;    /* RGBA buffer being rendered */
static volatile int sFront;   /* RGBA buffer ready to present */
static SceUID sWorkSema[PPU_MAX_SLICES], sDoneSema, sIdleSema, sFrontMutex;

volatile uint32_t gPortRenderUs;
volatile uint32_t gPortScaleUs; /* Scale3x part of gPortRenderUs */
volatile uint32_t gPortCaptureWaitUs;


/* With the front mutex held: a slot the GPU is not using. */
static int FreeSlot(void) {
    int s;

    for (s = 0; s < SCALED_SLOTS; s++) {
        if (s != sScaledSlot[sFront] && s != sShown[0] && s != sShown[1]) {
            break;
        }
    }
    return s;
}

/* Game thread, at startup. */
void RenderSetScaledSlots(uint32_t* const data[SCALED_SLOTS], int stride) {
    int s;

    for (s = 0; s < SCALED_SLOTS; s++) {
        sSlotData[s] = data[s];
    }
    sSlotStride = stride;
}

/* Slice 1: bottom half, on core 2. */
static int SliceThread(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelWaitSema(sWorkSema[1], 1, NULL);
        if (sPhase == 0) {
            PpuRenderSlice(&sFrames[sPending], 1, SPLIT_LINE, GBA_SCREEN_HEIGHT);
        } else {
            Scale3xRows(sRgba[sBack], sWidth, GBA_SCREEN_HEIGHT, sSlotData[sTarget], sSlotStride, SPLIT_LINE,
                        GBA_SCREEN_HEIGHT);
        }
        sceKernelSignalSema(sDoneSema, 1);
    }
    return 0;
}

/* Slice 0: top half, on core 1; also owns the frame hand-off. */
static int RenderThread(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    for (;;) {
        const PpuFrame* frame;
        SceUInt64 t0;
        int scale = gPortConfig.upscale == UPSCALE_SCALE3X && sSlotData[0] != NULL;

        sceKernelWaitSema(sWorkSema[0], 1, NULL);
        t0 = sceKernelGetProcessTimeWide();
        frame = &sFrames[sPending];
        sBack = sFront ^ 1;
        PpuSetOutput(sRgba[sBack], sWidth, sWidth);
        PpuPrepareFrame(frame);
        sPhase = 0;
        sceKernelSignalSema(sWorkSema[1], 1);
        PpuRenderSlice(frame, 0, 0, SPLIT_LINE);
        sceKernelWaitSema(sDoneSema, 1, NULL);

        /* Scale3x reads one line past its half, so it starts once both are drawn. */
        sScaledSlot[sBack] = -1;
        if (scale) {
            SceUInt64 t1 = sceKernelGetProcessTimeWide();

            sceKernelLockMutex(sFrontMutex, 1, NULL);
            sTarget = FreeSlot();
            sceKernelUnlockMutex(sFrontMutex, 1);
            sPhase = 1;
            sceKernelSignalSema(sWorkSema[1], 1);
            Scale3xRows(sRgba[sBack], sWidth, GBA_SCREEN_HEIGHT, sSlotData[sTarget], sSlotStride, 0, SPLIT_LINE);
            sceKernelWaitSema(sDoneSema, 1, NULL);
            sScaledSlot[sBack] = sTarget;
            gPortScaleUs = (uint32_t)(sceKernelGetProcessTimeWide() - t1);
        } else {
            gPortScaleUs = 0;
        }

        sceKernelLockMutex(sFrontMutex, 1, NULL);
        sFront = sBack;
        sceKernelUnlockMutex(sFrontMutex, 1);
        gPortRenderUs = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        sceKernelSignalSema(sIdleSema, 1);
    }
    return 0;
}

void RenderInit(int width) {
    SceUID thread;

    sWidth = width;
    sWorkSema[0] = sceKernelCreateSema("khcom_render_work0", 0, 0, 1, NULL);
    sWorkSema[1] = sceKernelCreateSema("khcom_render_work1", 0, 0, 1, NULL);
    sDoneSema = sceKernelCreateSema("khcom_render_done", 0, 0, 1, NULL);
    sIdleSema = sceKernelCreateSema("khcom_render_idle", 0, 1, 1, NULL);
    sFrontMutex = sceKernelCreateMutex("khcom_render_front", 0, 0, NULL);

    thread = sceKernelCreateThread("khcom_render0", RenderThread, 0x10000100, 0x10000, 0,
                                   SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (thread < 0) {
        PortFatal("render: thread creation failed (%08X)", thread);
    }
    sceKernelStartThread(thread, 0, NULL);
    thread = sceKernelCreateThread("khcom_render1", SliceThread, 0x10000100, 0x10000, 0,
                                   SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (thread < 0) {
        PortFatal("render: thread creation failed (%08X)", thread);
    }
    sceKernelStartThread(thread, 0, NULL);
}

/* Streams captured after the last VBlank; they describe the frame being captured now. */
static PpuBgStream sStreams[4];

/* Game thread. */
void PortSetBgStream(int bg, const void* const* map, int width, int height, int worldX, int worldY,
                     int shadowHofs, int shadowVofs) {
    PpuBgStream* s = &sStreams[bg];
    int i;

    s->valid = 0;
    if (map == NULL || width * height > PPU_STREAM_MAX_BLOCKS) {
        return;
    }
    for (i = 0; i < width * height; i++) {
        s->blocks[i] = (const uint16_t*)map[i];
        /* A block the game hasn't pointed at real memory yet: skip the stream. */
        if ((uintptr_t)s->blocks[i] < 0x10000000) {
            return;
        }
    }
    s->width = width;
    s->height = height;
    s->worldX = worldX;
    s->worldY = worldY;
    s->shadowHofs = shadowHofs;
    s->shadowVofs = shadowVofs;
    s->valid = 1;
}

static int sUiOverlays;

int PortUiOverlayActive(void) {
    return sUiOverlays > 0;
}

void PortUiOverlay(int delta) {
    sUiOverlays += delta;
    if (sUiOverlays < 0) {
        sUiOverlays = 0;
    }
}

/* Game thread: called for each visible line, before its HBlank. */
void PortCaptureLine(int y) {
    memcpy(sFrames[sCapture].io[y], gGbaIo, PPU_LINE_IO_SIZE);
}

/* Game thread: called once all lines were captured. */
void PortCaptureSubmit(void) {
    PpuFrame* f = &sFrames[sCapture];
    SceUInt64 t0;

    memcpy(f->streams, sStreams, sizeof(f->streams));
    f->clipObjs = sUiOverlays > 0;
    memcpy(f->pltt, gGbaPltt, sizeof(f->pltt));
    memcpy(f->oam, gGbaOam, sizeof(f->oam));
    memcpy(f->vram, gGbaVram, sizeof(f->vram));

    t0 = sceKernelGetProcessTimeWide();
    sceKernelWaitSema(sIdleSema, 1, NULL);
    gPortCaptureWaitUs = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    sPending = sCapture;
    sCapture ^= 1;
    sceKernelSignalSema(sWorkSema[0], 1);
}

const uint32_t* RenderLockFront(int* slot) {
    sceKernelLockMutex(sFrontMutex, 1, NULL);
    *slot = -1;
    if (gPortConfig.upscale != UPSCALE_SCALE3X || sSlotData[0] == NULL) {
        return sRgba[sFront];
    }
    /* Turned on in the paused menu: no new frame comes, so scale this one. */
    if (sScaledSlot[sFront] < 0) {
        int s = FreeSlot();

        Scale3xRows(sRgba[sFront], sWidth, GBA_SCREEN_HEIGHT, sSlotData[s], sSlotStride, 0, GBA_SCREEN_HEIGHT);
        sScaledSlot[sFront] = s;
    }
    *slot = sScaledSlot[sFront];
    if (*slot != sShown[0]) {
        sShown[1] = sShown[0];
        sShown[0] = *slot;
    }
    return sRgba[sFront];
}

void RenderUnlockFront(void) {
    sceKernelUnlockMutex(sFrontMutex, 1);
}
