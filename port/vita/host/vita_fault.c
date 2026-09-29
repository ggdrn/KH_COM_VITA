/*
 * Data-abort handler that gives GBA semantics to accesses the Vita rejects.
 *
 * The game was written for hardware where reading through a NULL pointer
 * hits the BIOS (it returns the last BIOS opcode, never faults) and where
 * writes there are ignored, and it stores raw GBA bus addresses in some data.
 * Both crash on the Vita. With the kubridge kernel plugin installed, this
 * handler decodes the faulting Thumb load/store, performs it the way the GBA
 * would, and resumes at the next instruction:
 *
 *   address < 0x02000000   BIOS / unmapped: loads return the BIOS open-bus
 *                          value, stores are dropped
 *   0x04..0x0E region      translated to the emulated IO/VRAM/ROM/SRAM
 *
 * Any other fault (a real bug of the port) goes to the default handler and
 * still produces a crash dump. Each emulated code location is logged once.
 */
#include <psp2/kernel/threadmgr.h>

#include <string.h>

#include "port.h"
#include "vita_host.h"

/* kubridge (https://github.com/bythos14/kubridge) exception interface. */
#define KU_EXCEPTION_DATA_ABORT 0

typedef struct KuExceptionContext {
    uint32_t r[16]; /* r0-r12, sp, lr, pc */
    uint64_t vfp[32];
    uint32_t spsr;
    uint32_t fpscr;
    uint32_t fpexc;
    uint32_t fsr;
    uint32_t far;
    uint32_t exceptionType;
} KuExceptionContext;

typedef void (*KuExceptionHandler)(KuExceptionContext*);

typedef struct KuExceptionHandlerOpt {
    uint32_t size;
} KuExceptionHandlerOpt;

int kuKernelRegisterExceptionHandler(uint32_t type, KuExceptionHandler handler, KuExceptionHandler* old,
                                     KuExceptionHandlerOpt* opt);

#define REG_SP 13
#define REG_LR 14
#define REG_PC 15
#define SPSR_THUMB 0x20

/* What the GBA reads from the BIOS region outside the BIOS (last opcode after a SWI returns). */
#define BIOS_OPEN_BUS 0xE3A02004u

static KuExceptionHandler sOldHandler;

/* Emulated sites, logged from the main loop. */
#define MAX_SITES 64
static volatile uint32_t sSitePc[MAX_SITES];
static volatile uint32_t sSiteAddr[MAX_SITES];
static volatile int sSiteCount;
static int sSitesLogged;
static volatile uint32_t sEmulated;

static void NoteSite(uint32_t pc, uint32_t addr) {
    int i, n = sSiteCount;

    sEmulated++;
    for (i = 0; i < n; i++) {
        if (sSitePc[i] == pc) {
            return;
        }
    }
    if (n < MAX_SITES) {
        sSitePc[n] = pc;
        sSiteAddr[n] = addr;
        sSiteCount = n + 1;
    }
}

/* Addresses this handler takes care of; everything else is a real crash. */
static int IsGbaAddress(uint32_t addr) {
    return addr < 0x10000000;
}

static uint32_t Load(uint32_t addr, int size, int* ok) {
    uint8_t* p;

    *ok = 1;
    if (addr >= 0x10000000) {
        /* Host memory, e.g. the other words of an LDM that crossed into a valid page. */
        p = (uint8_t*)addr;
    } else if (addr < 0x02000000 || (p = GbaPtrQuiet(addr)) == NULL) {
        /* The open-bus word, rotated to the byte lane being read. */
        uint32_t sh = (addr & 3) * 8;
        uint32_t v = sh ? (BIOS_OPEN_BUS >> sh) | (BIOS_OPEN_BUS << (32 - sh)) : BIOS_OPEN_BUS;
        return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
    }
    switch (size) {
    case 1:
        return *p;
    case 2:
        return *(uint16_t*)((uintptr_t)p & ~1);
    default:
        return *(uint32_t*)((uintptr_t)p & ~3);
    }
}

static void Store(uint32_t addr, int size, uint32_t value) {
    uint8_t* p;

    if (addr >= 0x10000000) {
        p = (uint8_t*)addr;
    } else if (addr < 0x02000000 || (p = GbaPtrQuiet(addr)) == NULL) {
        return;
    }
    switch (size) {
    case 1:
        *p = value;
        break;
    case 2:
        *(uint16_t*)((uintptr_t)p & ~1) = value;
        break;
    default:
        *(uint32_t*)((uintptr_t)p & ~3) = value;
        break;
    }
}

static uint32_t Extend(uint32_t v, int size, int sign) {
    if (!sign) {
        return v;
    }
    return size == 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
}

static void WriteReg(KuExceptionContext* c, int rt, uint32_t v) {
    if (rt == REG_PC) {
        /* Loading PC is a branch; bit 0 selects Thumb. */
        if (v & 1) {
            c->spsr |= SPSR_THUMB;
        } else {
            c->spsr &= ~SPSR_THUMB;
        }
        c->r[REG_PC] = v & ~1u;
    } else {
        c->r[rt] = v;
    }
}

/* Advances the IT state in SPSR after an instruction inside an IT block. */
static void AdvanceIt(KuExceptionContext* c) {
    uint32_t it = ((c->spsr >> 25) & 3) | ((c->spsr >> 8) & 0xFC);

    if (it == 0) {
        return;
    }
    if ((it & 7) == 0) {
        it = 0;
    } else {
        it = (it & 0xE0) | ((it << 1) & 0x1F);
    }
    c->spsr &= ~((3u << 25) | (0x3Fu << 10));
    c->spsr |= ((it & 3) << 25) | ((it >> 2) << 10);
}

static int DoSingle(KuExceptionContext* c, int load, int size, int sign, int rt, uint32_t addr) {
    int ok;

    if (load) {
        WriteReg(c, rt, Extend(Load(addr, size, &ok), size, sign));
    } else {
        Store(addr, size, c->r[rt]);
    }
    return 1;
}

static void DoMultiple(KuExceptionContext* c, int load, uint32_t addr, uint16_t list) {
    int i, ok;

    for (i = 0; i < 16; i++) {
        if (!(list & (1 << i))) {
            continue;
        }
        if (load) {
            WriteReg(c, i, Load(addr, 4, &ok));
        } else {
            Store(addr, 4, c->r[i]);
        }
        addr += 4;
    }
}

/* Returns the instruction length (2 or 4) when emulated, 0 when not handled. */
static int Emulate(KuExceptionContext* c) {
    uint16_t* pc = (uint16_t*)c->r[REG_PC];
    uint16_t hw1 = pc[0];
    uint16_t hw2;

    /* 16-bit encodings */
    if ((hw1 & 0xF000) == 0x5000) { /* LDR/STR (register) */
        static const struct { uint8_t load, size, sign; } ops[8] = {
            { 0, 4, 0 }, { 0, 2, 0 }, { 0, 1, 0 }, { 1, 1, 1 }, { 1, 4, 0 }, { 1, 2, 0 }, { 1, 1, 0 }, { 1, 2, 1 },
        };
        int op = (hw1 >> 9) & 7;
        uint32_t addr = c->r[(hw1 >> 3) & 7] + c->r[(hw1 >> 6) & 7];
        return DoSingle(c, ops[op].load, ops[op].size, ops[op].sign, hw1 & 7, addr) ? 2 : 0;
    }
    if ((hw1 & 0xE000) == 0x6000) { /* LDR/STR(B) (immediate) */
        int byte = (hw1 >> 12) & 1;
        int imm = ((hw1 >> 6) & 0x1F) * (byte ? 1 : 4);
        return DoSingle(c, (hw1 >> 11) & 1, byte ? 1 : 4, 0, hw1 & 7, c->r[(hw1 >> 3) & 7] + imm) ? 2 : 0;
    }
    if ((hw1 & 0xF000) == 0x8000) { /* LDRH/STRH (immediate) */
        int imm = ((hw1 >> 6) & 0x1F) * 2;
        return DoSingle(c, (hw1 >> 11) & 1, 2, 0, hw1 & 7, c->r[(hw1 >> 3) & 7] + imm) ? 2 : 0;
    }
    if ((hw1 & 0xF000) == 0xC000) { /* LDMIA/STMIA */
        int rn = (hw1 >> 8) & 7;
        int load = (hw1 >> 11) & 1;
        uint16_t list = hw1 & 0xFF;
        uint32_t addr = c->r[rn];
        DoMultiple(c, load, addr, list);
        if (!load || !(list & (1 << rn))) {
            c->r[rn] = addr + 4 * __builtin_popcount(list);
        }
        return 2;
    }
    if ((hw1 & 0xE000) != 0xE000 || (hw1 & 0x1800) == 0) {
        return 0;
    }

    /* 32-bit encodings */
    hw2 = pc[1];
    if ((hw1 & 0xFE00) == 0xF800) { /* single load/store */
        int sign = (hw1 >> 8) & 1;
        int size = 1 << ((hw1 >> 5) & 3);
        int load = (hw1 >> 4) & 1;
        int rn = hw1 & 0xF;
        int rt = hw2 >> 12;
        uint32_t base = c->r[rn];
        uint32_t addr;

        if (rn == REG_PC || size == 8 || (sign && !load)) {
            return 0;
        }
        if (hw1 & 0x80) {
            addr = base + (hw2 & 0xFFF);
        } else if (hw2 & 0x800) {
            int p = (hw2 >> 10) & 1, u = (hw2 >> 9) & 1, w = (hw2 >> 8) & 1;
            uint32_t off = hw2 & 0xFF;
            uint32_t target = u ? base + off : base - off;
            addr = p ? target : base;
            if (w) {
                c->r[rn] = target;
            }
        } else if ((hw2 & 0xFC0) == 0) {
            addr = base + (c->r[hw2 & 0xF] << ((hw2 >> 4) & 3));
        } else {
            return 0;
        }
        return DoSingle(c, load, size, sign, rt, addr) ? 4 : 0;
    }
    if ((hw1 & 0xFE40) == 0xE840 && (hw1 & 0x0120)) { /* LDRD/STRD */
        int p = (hw1 >> 8) & 1, u = (hw1 >> 7) & 1, w = (hw1 >> 5) & 1, load = (hw1 >> 4) & 1;
        int rn = hw1 & 0xF, rt = hw2 >> 12, rt2 = (hw2 >> 8) & 0xF;
        uint32_t off = (hw2 & 0xFF) * 4;
        uint32_t base = c->r[rn];
        uint32_t target = u ? base + off : base - off;
        uint32_t addr = p ? target : base;
        int ok;

        if (rn == REG_PC) {
            return 0;
        }
        if (load) {
            uint32_t lo = Load(addr, 4, &ok), hi = Load(addr + 4, 4, &ok);
            c->r[rt] = lo;
            c->r[rt2] = hi;
        } else {
            Store(addr, 4, c->r[rt]);
            Store(addr + 4, 4, c->r[rt2]);
        }
        if (w) {
            c->r[rn] = target;
        }
        return 4;
    }
    if ((hw1 & 0xFF90) == 0xE880 || (hw1 & 0xFF90) == 0xE900) { /* LDM/STM IA, DB */
        int db = (hw1 & 0xFF90) == 0xE900;
        int w = (hw1 >> 5) & 1, load = (hw1 >> 4) & 1, rn = hw1 & 0xF;
        uint32_t n = __builtin_popcount(hw2);
        uint32_t base = c->r[rn];
        uint32_t addr = db ? base - 4 * n : base;

        DoMultiple(c, load, addr, hw2);
        if (w && !(load && (hw2 & (1 << rn)))) {
            c->r[rn] = db ? base - 4 * n : base + 4 * n;
        }
        return 4;
    }
    if ((hw1 & 0xFF20) == 0xED00 && (hw2 & 0x0E00) == 0x0A00) { /* VLDR/VSTR */
        int u = (hw1 >> 7) & 1, load = (hw1 >> 4) & 1, rn = hw1 & 0xF;
        int dbl = (hw2 >> 8) & 1;
        uint32_t off = (hw2 & 0xFF) * 4;
        uint32_t addr = (rn == REG_PC ? (c->r[REG_PC] + 4) & ~3u : c->r[rn]) + (u ? off : -off);
        int ok;

        if (dbl) {
            int d = ((hw1 >> 6) & 1) << 4 | (hw2 >> 12);
            if (load) {
                uint64_t lo = Load(addr, 4, &ok), hi = Load(addr + 4, 4, &ok);
                c->vfp[d] = lo | (hi << 32);
            } else {
                Store(addr, 4, (uint32_t)c->vfp[d]);
                Store(addr + 4, 4, (uint32_t)(c->vfp[d] >> 32));
            }
        } else {
            int s = (hw2 >> 12) << 1 | ((hw1 >> 6) & 1);
            uint32_t* half = (uint32_t*)&c->vfp[s >> 1] + (s & 1);
            if (load) {
                *half = Load(addr, 4, &ok);
            } else {
                Store(addr, 4, *half);
            }
        }
        return 4;
    }
    return 0;
}

static void FaultHandler(KuExceptionContext* c) {
    uint32_t pc = c->r[REG_PC];
    int len;

    if ((c->spsr & SPSR_THUMB) && IsGbaAddress(c->far)) {
        len = Emulate(c);
        if (len != 0) {
            NoteSite(pc, c->far);
            AdvanceIt(c);
            /* A load into PC already set the new PC. */
            if (c->r[REG_PC] == pc) {
                c->r[REG_PC] = pc + len;
            }
            return;
        }
    }
    if (sOldHandler != NULL) {
        sOldHandler(c);
    }
}

void FaultInit(void) {
    KuExceptionHandlerOpt opt;
    int ret;

    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    ret = kuKernelRegisterExceptionHandler(KU_EXCEPTION_DATA_ABORT, FaultHandler, &sOldHandler, &opt);
    if (ret < 0) {
        PortLog("fault handler: unavailable (%08X); install kubridge for GBA-style NULL accesses", ret);
    } else {
        /* FaultInit's address lets the logged PCs be mapped back to the ELF. */
        PortLog("fault handler: active (kubridge), FaultInit=%p", (void*)FaultInit);
    }
}

/* Main loop: reports newly emulated sites. */
void FaultPoll(void) {
    while (sSitesLogged < sSiteCount) {
        int i = sSitesLogged++;
        PortLog("fault: emulated GBA access at pc=%08X addr=%08X (total %u)", (unsigned)sSitePc[i],
                (unsigned)sSiteAddr[i], (unsigned)sEmulated);
    }
}

/*
 * Watchdog: when the game loop stops advancing for WATCHDOG_SECONDS, log the
 * fault-handler state and crash on purpose. The resulting core dump holds the
 * registers of every thread, so a freeze can be located like a crash.
 */
#define WATCHDOG_SECONDS 5

static int WatchdogThread(SceSize args, void* argp) {
    uint32_t last = gPortVBlankIrqs;
    uint32_t lastEmulated = sEmulated;
    int stalled = 0;
    int i;

    (void)args;
    (void)argp;
    for (;;) {
        sceKernelDelayThread(1000 * 1000);
        if (gPortVBlankIrqs != last) {
            last = gPortVBlankIrqs;
            lastEmulated = sEmulated;
            stalled = 0;
            continue;
        }
        if (++stalled < WATCHDOG_SECONDS) {
            continue;
        }
        PortLog("watchdog: game loop stalled for %d s at frame %u; emulated accesses %u (+%u while stalled)",
                WATCHDOG_SECONDS, (unsigned)last, (unsigned)sEmulated, (unsigned)(sEmulated - lastEmulated));
        for (i = sSitesLogged; i < sSiteCount; i++) {
            PortLog("watchdog: emulated site pc=%08X addr=%08X", (unsigned)sSitePc[i], (unsigned)sSiteAddr[i]);
        }
        PortLog("watchdog: forcing a crash dump (FaultInit=%p)", (void*)FaultInit);
        PortFlushSram();
        /* An address the fault handler does not emulate: a regular crash. */
        *(volatile uint32_t*)0xDEAD0000 = 0;
    }
    return 0;
}

void WatchdogInit(void) {
    SceUID t = sceKernelCreateThread("khcom_watchdog", WatchdogThread, 0x10000100 - 20, 0x2000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (t >= 0) {
        sceKernelStartThread(t, 0, NULL);
    }
}
