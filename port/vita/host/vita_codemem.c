/*
 * Executable memory for the FMV codecs.
 *
 * The movie player copies ARM machine code from ROM to work RAM, patches it
 * for the video size and calls it (src/lib/movie_player.c). The code is plain
 * position-independent ARM that the Vita's Cortex-A9 runs as is, so it only
 * needs memory it may execute: a VM block, writable while the VM domain is
 * open and synced to the instruction cache when it is closed.
 */
#include <psp2/kernel/sysmem.h>

#include "port.h"

#define CODE_BLOCK_SIZE 0x100000

static SceUID sBlock = -1;
static uint8_t* sBase;
static uint32_t sUsed;
static int sLive;

void* PortCodeAlloc(uint32_t size) {
    void* p;

    if (sBlock < 0) {
        sBlock = sceKernelAllocMemBlockForVM("khcom_codec", CODE_BLOCK_SIZE);
        if (sBlock < 0 || sceKernelGetMemBlockBase(sBlock, (void**)&sBase) < 0) {
            PortFatal("movie: cannot allocate executable memory (%08X)", sBlock);
        }
    }
    size = (size + 63) & ~63u;
    if (sUsed + size > CODE_BLOCK_SIZE) {
        PortFatal("movie: executable memory exhausted (%u + %u)", (unsigned)sUsed, (unsigned)size);
    }
    p = sBase + sUsed;
    sUsed += size;
    sLive++;
    return p;
}

void PortCodeFree(void* p) {
    (void)p;
    /* The codecs are freed together when a movie closes. */
    if (--sLive <= 0) {
        sLive = 0;
        sUsed = 0;
    }
}

void PortCodeBeginWrite(void) {
    sceKernelOpenVMDomain();
}

void PortCodeEndWrite(void) {
    sceKernelCloseVMDomain();
    if (sBlock >= 0 && sUsed != 0) {
        sceKernelSyncVMDomain(sBlock, sBase, sUsed);
    }
}
