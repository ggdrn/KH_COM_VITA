/*
 * Emulated GBA memory, BIOS calls, DMA and interrupt dispatch.
 *
 * Compiled with the game's struct layout flags; talks to the host side only
 * through port.h.
 */
#include "types.h"
#include "gba/io_reg.h"
#include "gba/syscall.h"
#include "malloc.h"
#include "port.h"

#include <string.h>
#include <math.h>

typedef void (*IntrFunc)(void);
extern IntrFunc gIntrTable[14];

u8 gGbaIo[0x400] __attribute__((aligned(16)));
u8 gGbaPltt[0x400] __attribute__((aligned(16)));
u8 gGbaVram[0x18000] __attribute__((aligned(16)));
u8 gGbaOam[0x400] __attribute__((aligned(16)));
u8 gGbaSram[0x10000] __attribute__((aligned(16)));

/* Linker-provided on the GBA. */
u8 gEwramHeapStart[EWRAM_HEAP_SIZE] __attribute__((aligned(16)));
u8 gIwramHeapStart[IWRAM_HEAP_SIZE] __attribute__((aligned(16)));

/* BIOS-owned IWRAM words on the GBA (0x03007FF0 / 0x03007FF8). */
struct SoundInfo* gSoundInfoPtr;
vu16 gIntrCheck;

/* crt0's interrupt dispatcher; only ever copied around on the GBA. */
u8 IrqHandler[0x800];

#define IO16(off) (*(vu16*)&gGbaIo[off])
#define IO32(off) (*(vu32*)&gGbaIo[off])

/* Address translation --------------------------------------------------------- */

typedef struct RomXlate {
    u32 gba;
    const u8* host;
} RomXlate;

/* Generated from the GBA build's symbol table (build/vita/<version>/romxlate.c). */
extern const RomXlate gRomXlate[];
extern const u32 gRomXlateCount;

static void* RomToHost(u32 addr) {
    u32 lo = 0, hi = gRomXlateCount;

    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (gRomXlate[mid].gba <= addr) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    if (gRomXlateCount == 0 || gRomXlate[lo].gba > addr) {
        PortFatal("GbaPtr: no ROM symbol for %08X", (unsigned)addr);
    }
    return (void*)(gRomXlate[lo].host + (addr - gRomXlate[lo].gba));
}

/*
 * Game code occasionally passes raw GBA bus addresses (from data tables or
 * integer arithmetic). Vita user memory lives far above 0x10000000, so any
 * pointer below that is a GBA address and is redirected to the emulated
 * buffers.
 */
void* GbaPtr(const void* p) {
    u32 addr = (u32)p;

    if (addr >= 0x10000000 || addr < 0x04000000) {
        return (void*)p;
    }
    switch (addr >> 24) {
    case 0x04:
        return &gGbaIo[addr & 0x3FF];
    case 0x05:
        return &gGbaPltt[addr & 0x3FF];
    case 0x06:
        addr &= 0x1FFFF;
        if (addr >= 0x18000) {
            addr -= 0x8000;
        }
        return &gGbaVram[addr];
    case 0x07:
        return &gGbaOam[addr & 0x3FF];
    case 0x08:
    case 0x09:
        return RomToHost(addr);
    case 0x0E:
        return &gGbaSram[addr & 0xFFFF];
    }
    PortFatal("GbaPtr: unmapped GBA address %08X", (unsigned)addr);
}

/* BIOS ------------------------------------------------------------------------ */

void CpuSet(void* src, void* dst, u32 ctrl) {
    u32 count = ctrl & 0x1FFFFF;
    int fixed = (ctrl & CPU_SET_SRC_FIXED) != 0;

    src = GbaPtr(src);
    dst = GbaPtr(dst);
    if (ctrl & CPU_SET_32BIT) {
        u32* s = (u32*)((u32)src & ~3);
        u32* d = (u32*)((u32)dst & ~3);
        if (fixed) {
            u32 v = *s;
            while (count--) {
                *d++ = v;
            }
        } else {
            memmove(d, s, count * 4);
        }
    } else {
        u16* s = (u16*)((u32)src & ~1);
        u16* d = (u16*)((u32)dst & ~1);
        if (fixed) {
            u16 v = *s;
            while (count--) {
                *d++ = v;
            }
        } else {
            memmove(d, s, count * 2);
        }
    }
}

void CpuFastSet(void* src, void* dst, s32 ctrl) {
    u32 count = ((u32)ctrl & 0x1FFFFF);
    u32* s = (u32*)((u32)GbaPtr(src) & ~3);
    u32* d = (u32*)((u32)GbaPtr(dst) & ~3);

    count = (count + 7) & ~7;
    if (ctrl & CPU_SET_SRC_FIXED) {
        u32 v = *s;
        while (count--) {
            *d++ = v;
        }
    } else {
        memmove(d, s, count * 4);
    }
}

static void LZ77UnComp(const void* srcp, void* dstp) {
    const u8* src = GbaPtr(srcp);
    u8* dst = GbaPtr(dstp);
    u32 size = (src[1] | (src[2] << 8) | (src[3] << 16));
    u32 written = 0;

    src += 4;
    while (written < size) {
        u8 flags = *src++;
        int i;

        for (i = 0; i < 8 && written < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                u32 len = (src[0] >> 4) + 3;
                u32 disp = (((src[0] & 0xF) << 8) | src[1]) + 1;
                src += 2;
                while (len-- && written < size) {
                    dst[written] = dst[written - disp];
                    written++;
                }
            } else {
                dst[written++] = *src++;
            }
        }
    }
}

void LZ77UnCompWram(const void* src, void* dst) {
    LZ77UnComp(src, dst);
}

void LZ77UnCompVram(const void* src, void* dst) {
    LZ77UnComp(src, dst);
}

void BgAffineSet(BgAffineSrcData* src, BgAffineDstData* dst, s32 count) {
    while (count-- > 0) {
        float ox = src->texX / 256.0f;
        float oy = src->texY / 256.0f;
        float cx = src->scrX;
        float cy = src->scrY;
        float sx = src->sx / 256.0f;
        float sy = src->sy / 256.0f;
        float theta = (src->alpha >> 8) / 128.0f * (float)M_PI;
        float a, b, c, d, rx, ry;

        a = d = cosf(theta);
        b = c = sinf(theta);
        a *= sx;
        b *= -sx;
        c *= sy;
        d *= sy;
        rx = ox - (a * cx + b * cy);
        ry = oy - (c * cx + d * cy);
        dst->pa = (s16)(a * 256);
        dst->pb = (s16)(b * 256);
        dst->pc = (s16)(c * 256);
        dst->pd = (s16)(d * 256);
        dst->dx = (s32)(rx * 256);
        dst->dy = (s32)(ry * 256);
        src++;
        dst++;
    }
}

u32 Sqrt(u32 value) {
    u32 root = 0;
    u32 bit = 1u << 30;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

void RegisterRamReset(u32 flags) {
    if (flags & 0x04) {
        memset(gGbaPltt, 0, sizeof(gGbaPltt));
    }
    if (flags & 0x08) {
        memset(gGbaVram, 0, sizeof(gGbaVram));
    }
    if (flags & 0x10) {
        memset(gGbaOam, 0, sizeof(gGbaOam));
    }
    if (flags & 0x80) {
        u16 keys = IO16(REG_OFFSET_KEYINPUT);
        memset(gGbaIo, 0, sizeof(gGbaIo));
        IO16(REG_OFFSET_KEYINPUT) = keys;
        IO16(REG_OFFSET_DISPCNT) = DISPCNT_FORCED_BLANK;
        IO16(REG_OFFSET_BG2PA) = 0x100;
        IO16(REG_OFFSET_BG2PD) = 0x100;
        IO16(REG_OFFSET_BG3PA) = 0x100;
        IO16(REG_OFFSET_BG3PD) = 0x100;
    }
}

void PortSoftReset(void);

void SoftReset(s32 flags) {
    PortSoftReset();
}

/* DMA ------------------------------------------------------------------------- */

typedef struct GbaDma {
    u32 src;
    u32 dst;
    u32 count;
    u16 ctrl;
    u8 active;
} GbaDma;

static GbaDma sDma[4];

#define DMA_REG_BASE(ch) (REG_OFFSET_DMA0 + (ch) * 12)

static void DmaTransfer(int ch) {
    GbaDma* dma = &sDma[ch];
    int size = (dma->ctrl & (DMA_32BIT)) ? 4 : 2;
    int dstMode = (dma->ctrl >> 5) & 3;
    int srcMode = (dma->ctrl >> 7) & 3;
    int dstStep = dstMode == 1 ? -size : dstMode == 2 ? 0 : size;
    int srcStep = srcMode == 1 ? -size : srcMode == 2 ? 0 : size;
    u32 count = dma->count;
    u8* src = GbaPtr((void*)dma->src);
    u8* dst = GbaPtr((void*)dma->dst);

    if (size == 4) {
        src = (u8*)((u32)src & ~3);
        dst = (u8*)((u32)dst & ~3);
        if (srcStep == 4 && dstStep == 4) {
            memmove(dst, src, count * 4);
        } else {
            while (count--) {
                *(u32*)dst = *(u32*)src;
                src += srcStep;
                dst += dstStep;
            }
        }
    } else {
        src = (u8*)((u32)src & ~1);
        dst = (u8*)((u32)dst & ~1);
        if (srcStep == 2 && dstStep == 2) {
            memmove(dst, src, count * 2);
        } else {
            while (count--) {
                *(u16*)dst = *(u16*)src;
                src += srcStep;
                dst += dstStep;
            }
        }
    }
    dma->src += srcStep * (s32)dma->count;
    if (dstMode != 3) {
        dma->dst += dstStep * (s32)dma->count;
    }
}

static void DmaStart(int ch) {
    u32 base = DMA_REG_BASE(ch);
    GbaDma* dma = &sDma[ch];
    u16 ctrl = IO16(base + 10);
    u32 count = IO16(base + 8);

    dma->src = IO32(base);
    dma->dst = IO32(base + 4);
    if (count == 0) {
        count = ch == 3 ? 0x10000 : 0x4000;
    }
    dma->count = count;
    dma->ctrl = ctrl;
    dma->active = 1;

    switch ((ctrl >> 12) & 3) {
    case 0:
        DmaTransfer(ch);
        dma->active = 0;
        IO16(base + 10) &= ~DMA_ENABLE;
        break;
    case 3:
        /* Sound FIFO / video capture: the audio path reads the m4a buffers directly. */
        dma->active = 0;
        break;
    }
}

/*
 * Called after the game writes a DMA control register and reads it back,
 * which is how every DMA start in the game is written.
 */
void GbaDmaReadback(volatile void* reg) {
    int ch;

    for (ch = 0; ch < 4; ch++) {
        u32 base = DMA_REG_BASE(ch);
        u16 ctrl = IO16(base + 10);
        if (ctrl & DMA_ENABLE) {
            if (!sDma[ch].active || sDma[ch].ctrl != ctrl || sDma[ch].src != IO32(base)) {
                DmaStart(ch);
            }
        } else {
            sDma[ch].active = 0;
        }
    }
}

static void DmaRunTiming(int timing) {
    int ch;

    for (ch = 0; ch < 4; ch++) {
        GbaDma* dma = &sDma[ch];
        u32 base = DMA_REG_BASE(ch);

        if (!dma->active || ((dma->ctrl >> 12) & 3) != timing) {
            continue;
        }
        if (!(IO16(base + 10) & DMA_ENABLE)) {
            dma->active = 0;
            continue;
        }
        DmaTransfer(ch);
        if (dma->ctrl & DMA_REPEAT) {
            if (((dma->ctrl >> 5) & 3) == 3) {
                dma->dst = IO32(base + 4);
            }
        } else {
            dma->active = 0;
            IO16(base + 10) &= ~DMA_ENABLE;
        }
    }
}

void GbaDmaHBlank(void) {
    DmaRunTiming(2);
}

void GbaDmaVBlank(void) {
    DmaRunTiming(1);
}

/* Interrupts -------------------------------------------------------------------- */

/* gIntrTable is ordered by crt0's dispatch priority, not by IF bit. */
static const u8 sIrqTableIndex[14] = { 1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11, 12, 13 };

volatile uint32_t gPortVBlankIrqs;
volatile uint32_t gPortModeUpdates;

void GbaRaiseIrq(int bit) {
    u16 mask = 1 << bit;
    IntrFunc func;

    if (!(IO16(REG_OFFSET_IE) & mask) || !(IO16(REG_OFFSET_IME) & 1)) {
        return;
    }
    IO16(REG_OFFSET_IF) |= mask;
    func = gIntrTable[sIrqTableIndex[bit]];
    if (bit == 0) {
        gPortVBlankIrqs++;
    }
    IO16(REG_OFFSET_IME) = 0;
    if (func != NULL) {
        func();
    }
    IO16(REG_OFFSET_IME) = 1;
    IO16(REG_OFFSET_IF) &= ~mask;
}

/* Frame loop -------------------------------------------------------------------- */


void VBlankIntrWait(void) {
    int y;
    u16 dispstat;

    PORT_TRACE_EARLY("frame: start (DISPSTAT %04X IE %04X IME %d)", IO16(REG_OFFSET_DISPSTAT),
                     IO16(REG_OFFSET_IE), IO16(REG_OFFSET_IME));
    /* The HBlank of the last VBlank line prepares line 0. */
    IO16(REG_OFFSET_VCOUNT) = 227;
    if (IO16(REG_OFFSET_DISPSTAT) & DISPSTAT_HBLANK_INTR) {
        GbaRaiseIrq(1);
    }

    for (y = 0; y < GBA_SCREEN_HEIGHT; y++) {
        dispstat = IO16(REG_OFFSET_DISPSTAT) & ~(DISPSTAT_VBLANK | DISPSTAT_HBLANK | DISPSTAT_VCOUNT);
        IO16(REG_OFFSET_VCOUNT) = y;
        if ((dispstat >> 8) == y) {
            dispstat |= DISPSTAT_VCOUNT;
            IO16(REG_OFFSET_DISPSTAT) = dispstat;
            if (dispstat & DISPSTAT_VCOUNT_INTR) {
                GbaRaiseIrq(2);
            }
        }
        IO16(REG_OFFSET_DISPSTAT) = dispstat;
        PortCaptureLine(y);
        IO16(REG_OFFSET_DISPSTAT) |= DISPSTAT_HBLANK;
        GbaDmaHBlank();
        if (IO16(REG_OFFSET_DISPSTAT) & DISPSTAT_HBLANK_INTR) {
            GbaRaiseIrq(1);
        }
    }
    PortCaptureSubmit();

    PORT_TRACE_EARLY("frame: present");
    /* Presents the frame, reads input and paces to 60 Hz. */
    PortVBlankWait();
    PORT_TRACE_EARLY("frame: vblank irq");
    IO16(REG_OFFSET_KEYINPUT) = ~PortReadKeys() & 0x3FF;

    IO16(REG_OFFSET_VCOUNT) = GBA_SCREEN_HEIGHT;
    IO16(REG_OFFSET_DISPSTAT) = (IO16(REG_OFFSET_DISPSTAT) & ~DISPSTAT_HBLANK) | DISPSTAT_VBLANK;
    GbaDmaVBlank();
    if (IO16(REG_OFFSET_DISPSTAT) & DISPSTAT_VBLANK_INTR) {
        GbaRaiseIrq(0);
    }
    IO16(REG_OFFSET_DISPSTAT) &= ~DISPSTAT_VBLANK;
    PORT_TRACE_EARLY("frame: done");
    if (gPortTraceFrames > 0) {
        gPortTraceFrames--;
    }
}
