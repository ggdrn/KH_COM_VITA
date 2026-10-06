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
#include <psp2/kernel/clib.h>

#include <string.h>

#include "port.h"
#include "ppu.h"
#include "vita_host.h"

/* The frame is drawn, and enlarged, by three threads taking chunks of lines
 * in turn (see below). */
#define RENDER_CHUNK 8 /* lines drawn per turn */
#define SCALE_CHUNK 4  /* lines enlarged per turn */

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
static int sScaledSlot[2] = { -1, -1 }; /* slot holding the upscale of sRgba[i] */
static int sScaledMode[2];              /* ... made with this UPSCALE_* */
static int sScaledTarget[2];            /* ... and UPSCALE_TARGET_* */

typedef void (*UpscaleFn)(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst,
                          int dstStride, int y0, int y1);
static uint8_t sMask[2][GBA_SCREEN_HEIGHT * PORT_MAX_SCREEN_WIDTH]; /* sprite in front, per pixel of sRgba[i] */
static int sTargetMode; /* UPSCALE_TARGET_* of the frame being enlarged */

/*
 * The upscaler's output for the last frame, kept in cached memory: a line
 * whose source lines (and, smoothing sprites or scenery alone, mask lines)
 * up to 3 away are unchanged from the previous frame is copied from here
 * instead of being enlarged again, which makes menus, dialogue and scenes
 * with a still camera nearly free. Every line then goes to the texture with
 * one block copy (the upscalers write here, not to the uncached texture).
 */
#define UP_STRIDE (PORT_MAX_SCREEN_WIDTH * 3 + 8)
#define UP_REACH 3 /* farthest source line an output line depends on (MMPX) */
static uint32_t sUpCache[(GBA_SCREEN_HEIGHT * 3 + 2) * UP_STRIDE] __attribute__((aligned(64)));
static int sUpCacheValid, sUpCacheMode, sUpCacheTarget;
static uint8_t sRowNeed[GBA_SCREEN_HEIGHT]; /* line y must be enlarged again */
static volatile int sScaleFrame;            /* this frame is enlarged (set at hand-over) */
static volatile int sScaleMode;             /* ... with this UPSCALE_* */
static SceUID sScaleGameSema;               /* the game thread may join the upscale */
static UpscaleFn sUpscale; /* the frame being enlarged's upscaler */

static UpscaleFn UpscalerFor(int mode) {
    return mode == UPSCALE_MMPX ? Mmpx2xRows : mode == UPSCALE_SCALE2X ? Scale2xRows : Scale3xRows;
}
static int sShown[2] = { -1, -1 };      /* slots of the last two presented frames */
static int sTarget;                     /* slot being written */
static int sWideFrame[2];               /* sRgba[i] is a menu, to stretch */
static volatile int sPhase;             /* what the slice thread does next: 0 render, 1 scale */
static int sNextLine;                   /* next chunk to take, drawing or enlarging (atomic) */
static int sWidth;
static int sCapture;          /* snapshot the game thread fills */
static volatile int sPending; /* snapshot handed to the render threads */
static volatile int sBack;    /* RGBA buffer being rendered */
static volatile int sFront;   /* RGBA buffer ready to present */
static SceUID sWorkSema[PPU_MAX_SLICES], sDoneSema, sIdleSema, sFrontMutex;

volatile uint32_t gPortRenderUs;
volatile uint32_t gPortScaleUs; /* Scale3x part of gPortRenderUs */
volatile uint32_t gPortCaptureWaitUs;


/*
 * Logos and title screen: the margins take the scene's background colour,
 * the most common colour of the picture's edge column on that side, so they
 * blend with the scene (and follow its fades) instead of the black backdrop.
 */
static uint32_t EdgeColour(const uint32_t* px, int x) {
    uint32_t best = px[x];
    int bestCount = 0, y, k;

    for (y = 0; y < GBA_SCREEN_HEIGHT; y += 4) {
        uint32_t c = px[y * sWidth + x];
        int count = 0;

        if (c == best && bestCount > 0) {
            continue;
        }
        for (k = 0; k < GBA_SCREEN_HEIGHT; k += 2) {
            count += px[k * sWidth + x] == c;
        }
        if (count > bestCount) {
            best = c;
            bestCount = count;
        }
    }
    return best;
}

static void SceneMargins(uint32_t* px) {
    int xoff = (sWidth - GBA_SCREEN_WIDTH) / 2;
    uint32_t left = EdgeColour(px, xoff), right = EdgeColour(px, xoff + GBA_SCREEN_WIDTH - 1);
    int x, y;

    for (y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        uint32_t* row = px + y * sWidth;

        for (x = 0; x < xoff; x++) {
            row[x] = left;
        }
        for (x = xoff + GBA_SCREEN_WIDTH; x < sWidth; x++) {
            row[x] = right;
        }
    }
}

/* Which lines change since the frame the cache holds (sRgba[sBack ^ 1]). */
static void MarkChangedRows(int mode, int target) {
    const uint32_t* cur = sRgba[sBack];
    const uint32_t* prev = sRgba[sBack ^ 1];
    uint8_t dirty[GBA_SCREEN_HEIGHT];
    int all = !sUpCacheValid || sUpCacheMode != mode || sUpCacheTarget != target;
    int y, k;

    for (y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        dirty[y] = all || memcmp(cur + y * sWidth, prev + y * sWidth, sWidth * 4) != 0 ||
                   (target != UPSCALE_TARGET_ALL &&
                    memcmp(sMask[sBack] + y * sWidth, sMask[sBack ^ 1] + y * sWidth, sWidth) != 0);
    }
    for (y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        sRowNeed[y] = 0;
        for (k = y - UP_REACH; k <= y + UP_REACH; k++) {
            if (k >= 0 && k < GBA_SCREEN_HEIGHT && dirty[k]) {
                sRowNeed[y] = 1;
                break;
            }
        }
    }
}

/* Lines [y0, y1) of the frame being enlarged: the changed ones through the
 * upscaler into the cache, then all of them from the cache to the texture. */
static void UpscaleRows(int y0, int y1) {
    int f = UPSCALE_FACTOR(sScaleMode);
    int extra = f == 2; /* the 2x upscalers also write an edge column and, at the end, a line */
    int y = y0, e;

    while (y < y1) {
        if (!sRowNeed[y]) {
            y++;
            continue;
        }
        for (e = y; e < y1 && sRowNeed[e]; e++) {
        }
        sUpscale(sRgba[sBack], sMask[sBack], sTargetMode, sWidth, GBA_SCREEN_HEIGHT, sUpCache, UP_STRIDE, y, e);
        y = e;
    }
    for (y = y0 * f; y < y1 * f + (extra && y1 == GBA_SCREEN_HEIGHT); y++) {
        sceClibMemcpy(sSlotData[sTarget] + y * sSlotStride, sUpCache + y * UP_STRIDE, (sWidth * f + extra) * 4);
    }
}

/* The next chunk of `size` lines nobody took yet, or -1 once all are. */
static int TakeChunk(int size) {
    int y = __atomic_fetch_add(&sNextLine, size, __ATOMIC_RELAXED);

    return y < GBA_SCREEN_HEIGHT ? y : -1;
}

static int ChunkEnd(int y, int size) {
    return y + size < GBA_SCREEN_HEIGHT ? y + size : GBA_SCREEN_HEIGHT;
}

/* Each drawing thread, with its own renderer context: chunks until none are
 * left. */
static void RenderChunks(const PpuFrame* frame, int slice) {
    int y;

    while ((y = TakeChunk(RENDER_CHUNK)) >= 0) {
        PpuRenderSlice(frame, slice, y, ChunkEnd(y, RENDER_CHUNK));
    }
}

static void UpscaleChunks(void) {
    int y;

    while ((y = TakeChunk(SCALE_CHUNK)) >= 0) {
        UpscaleRows(y, ChunkEnd(y, SCALE_CHUNK));
    }
}

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

/*
 * The frame is drawn by three threads: RenderThread on core 1, SliceThread on
 * core 2 and the game thread itself on core 0, right after it hands the frame
 * over (PortCaptureSubmit): the game logic takes ~2.5 ms of a 16.7 ms frame,
 * so that core has the time, and heavy scenes (windows plus blending, ~16 ms
 * per half before) fit. The upscale is shared the same way.
 *
 * Rather than a fixed third each, the threads take small chunks of lines in
 * turn until none are left: the cost is uneven (a dialogue box at the bottom,
 * blended windows, only the HUD changing for the upscaler's line cache), and
 * with fixed parts the frame waited for the slowest one while the others
 * sat idle. The renderer's only state carried from line to line, the affine
 * reference points, is worked out per line beforehand (PpuPrepareFrame).
 */

/* Core 2. */
static int SliceThread(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelWaitSema(sWorkSema[1], 1, NULL);
        if (sPhase == 0) {
            RenderChunks(&sFrames[sPending], 1);
        } else {
            UpscaleChunks();
        }
        sceKernelSignalSema(sDoneSema, 1);
    }
    return 0;
}

/* Core 1; also owns the frame hand-off. */
static int RenderThread(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    for (;;) {
        const PpuFrame* frame;
        SceUInt64 t0;
        int mode, scale;

        sceKernelWaitSema(sWorkSema[0], 1, NULL);
        mode = sScaleMode;
        scale = sScaleFrame;
        t0 = sceKernelGetProcessTimeWide();
        frame = &sFrames[sPending];
        /* sBack, the output, the decoded sprites and the chunk counter were
         * set up by PortCaptureSubmit, which also takes chunks. */
        sPhase = 0;
        sceKernelSignalSema(sWorkSema[1], 1);
        RenderChunks(frame, 0);
        sceKernelWaitSema(sDoneSema, 2, NULL);

        if (frame->sceneMargins && sWidth > GBA_SCREEN_WIDTH) {
            SceneMargins(sRgba[sBack]);
        }
        sWideFrame[sBack] = frame->wideMenu;

        /* The upscalers read lines beyond their part, so they start once the
         * whole frame is drawn; the three threads share it in chunks. */
        sScaledSlot[sBack] = -1;
        if (scale) {
            SceUInt64 t1 = sceKernelGetProcessTimeWide();

            sceKernelLockMutex(sFrontMutex, 1, NULL);
            sTarget = FreeSlot();
            sceKernelUnlockMutex(sFrontMutex, 1);
            sUpscale = UpscalerFor(mode);
            sTargetMode = gPortConfig.upscaleTarget;
            MarkChangedRows(mode, sTargetMode);
            sPhase = 1;
            sNextLine = 0;
            sceKernelSignalSema(sWorkSema[1], 1);
            sceKernelSignalSema(sScaleGameSema, 1);
            UpscaleChunks();
            sceKernelWaitSema(sDoneSema, 2, NULL);
            sUpCacheValid = 1;
            sUpCacheMode = mode;
            sUpCacheTarget = sTargetMode;
            sScaledSlot[sBack] = sTarget;
            sScaledMode[sBack] = mode;
            sScaledTarget[sBack] = sTargetMode;
            gPortScaleUs = (uint32_t)(sceKernelGetProcessTimeWide() - t1);
        } else {
            gPortScaleUs = 0;
            sUpCacheValid = 0;
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
    sDoneSema = sceKernelCreateSema("khcom_render_done", 0, 0, 2, NULL);
    sScaleGameSema = sceKernelCreateSema("khcom_scale_game", 0, 0, 1, NULL);
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
static int sClipY0, sClipY1; /* PortUiClipRows */

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

static int sSceneMargins;
static int sWideMenus;  /* field menus open (PortWideMenu) */
static int sWideMode;   /* the current mode is a menu screen */

void PortWideMenu(int delta) {
    sWideMenus += delta;
    if (sWideMenus < 0) {
        sWideMenus = 0;
    }
}

void PortModeStart(const char* name) {
    static const char* const sScene[] = { "mode_copyright1", "mode_copyright2", "mode_wLogo", "mode_title" };
    /* Menu screens of their own (from the pause menu and the title). */
    static const char* const sMenus[] = { "mode_status",      "Mode_Deck",         "Mode_MenuLoad",
                                          "mode_allmap",      "mode_mapinspect",   "mode_worldinspect",
                                          "mode_jiminy" };
    unsigned i;

    sSceneMargins = 0;
    sClipY0 = sClipY1 = 0;
    sWideMode = 0;
    sWideMenus = 0;
    for (i = 0; i < 4; i++) {
        sStreams[i].keepMargins = 0;
        sStreams[i].split = 0;
    }
    for (i = 0; name != NULL && i < sizeof(sScene) / sizeof(sScene[0]); i++) {
        sSceneMargins |= !strcmp(name, sScene[i]);
    }
    for (i = 0; name != NULL && i < sizeof(sMenus) / sizeof(sMenus[0]); i++) {
        sWideMode |= !strcmp(name, sMenus[i]);
    }
}

int PortWideMargin(void) {
    return (sWidth - GBA_SCREEN_WIDTH) / 2;
}

int PortFieldHudEnabled(void) {
    return gPortConfig.fieldHud;
}

void PortUiClipRows(int y0, int y1) {
    sClipY0 = y0;
    sClipY1 = y1;
}

int PortUiOverlayActive(void) {
    return sUiOverlays > 0;
}

void PortUiOverlay(int delta) {
    sUiOverlays += delta;
    if (sUiOverlays < 0) {
        sUiOverlays = 0;
    }
}

/* Game thread. */
void PortSetBgPanel(int bg, int on) {
    sStreams[bg].panel = on;
}

void PortBgMargins(int bg, int sides) {
    sStreams[bg].keepMargins = sides;
}

void PortBgSplit(int bg, int split) {
    sStreams[bg].split = split;
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
    f->clipY0 = sClipY0;
    f->clipY1 = sClipY1;
    f->sceneMargins = sSceneMargins;
    f->wideMenu = (sWideMenus > 0 || sWideMode) && gPortConfig.wideMenus;
    memcpy(f->pltt, gGbaPltt, sizeof(f->pltt));
    memcpy(f->oam, gGbaOam, sizeof(f->oam));
    memcpy(f->vram, gGbaVram, sizeof(f->vram));

    t0 = sceKernelGetProcessTimeWide();
    sceKernelWaitSema(sIdleSema, 1, NULL);
    gPortCaptureWaitUs = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    sPending = sCapture;
    sCapture ^= 1;
    /* The render threads are idle: set the frame up for all three threads. */
    sScaleMode = gPortConfig.upscale;
    sScaleFrame = sScaleMode != UPSCALE_OFF && sSlotData[0] != NULL;
    sBack = sFront ^ 1;
    PpuSetOutput(sRgba[sBack], sWidth, sWidth);
    /* The sprite mask is only needed to smooth sprites or scenery alone. */
    PpuSetMask(gPortConfig.upscale != UPSCALE_OFF && gPortConfig.upscaleTarget != UPSCALE_TARGET_ALL ? sMask[sBack]
                                                                                                     : NULL);
    PpuPrepareFrame(&sFrames[sPending]);
    sNextLine = 0;
    sceKernelSignalSema(sWorkSema[0], 1);
    RenderChunks(&sFrames[sPending], 2);
    sceKernelSignalSema(sDoneSema, 1);
    /* ... and the upscale, once the whole frame is drawn. */
    if (sScaleFrame) {
        sceKernelWaitSema(sScaleGameSema, 1, NULL);
        UpscaleChunks();
        sceKernelSignalSema(sDoneSema, 1);
    }
}

const uint32_t* RenderLockFront(int* slot, int* factor, int* wide) {
    int mode = gPortConfig.upscale;

    sceKernelLockMutex(sFrontMutex, 1, NULL);
    *wide = sWideFrame[sFront];
    *slot = -1;
    *factor = 1;
    if (mode == UPSCALE_OFF || sSlotData[0] == NULL) {
        return sRgba[sFront];
    }
    /* Changed in the paused menu: no new frame comes, so enlarge this one. */
    if (sScaledSlot[sFront] < 0 || sScaledMode[sFront] != mode || sScaledTarget[sFront] != gPortConfig.upscaleTarget) {
        int s = FreeSlot();

        UpscalerFor(mode)(sRgba[sFront], sMask[sFront], gPortConfig.upscaleTarget, sWidth, GBA_SCREEN_HEIGHT, sSlotData[s], sSlotStride, 0, GBA_SCREEN_HEIGHT);
        sScaledSlot[sFront] = s;
        sScaledMode[sFront] = mode;
        sScaledTarget[sFront] = gPortConfig.upscaleTarget;
    }
    *slot = sScaledSlot[sFront];
    *factor = UPSCALE_FACTOR(sScaledMode[sFront]);
    if (*slot != sShown[0]) {
        sShown[1] = sShown[0];
        sShown[0] = *slot;
    }
    return sRgba[sFront];
}

void RenderUnlockFront(void) {
    sceKernelUnlockMutex(sFrontMutex, 1);
}
