/*
 * Loads the game's data from the player's own ROM.
 *
 * The executable ships with every byte that comes from the ROM zeroed
 * (tools/vita/strip_rom_data.py); app0:rommap.bin lists where each run of
 * those bytes lives in the ROM. At startup this reads the ROM, checks that it
 * is the expected dump (SHA-1) and copies the runs back into place, before
 * any game code runs.
 */
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vitaGL.h>

#include "port.h"
#include "vita_host.h"

#define ROM_DIR "ux0:data/khcom"
#define ROM_PATH ROM_DIR "/rom.gba"
#define MAP_PATH "app0:rommap.bin"
#define GAME_NAME "Kingdom Hearts: Chain of Memories (USA)"

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t anchor;
    uint32_t runCount;
    uint8_t sha1[20];
    uint32_t romSize;
} RomMapHeader;

typedef struct {
    uint32_t addr;
    uint32_t romOffset;
    uint32_t length;
} RomRun;

/* SHA-1 ------------------------------------------------------------------------ */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void Sha1Block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20) {
            f = (b & c) | (~b & d), k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d, k = 0xCA62C1D6;
        }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d, d = c, c = ROL(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

static void Sha1(const uint8_t* data, uint32_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t tail[128];
    uint32_t i, rest = len % 64, tailLen = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;

    for (i = 0; i + 64 <= len; i += 64) {
        Sha1Block(h, data + i);
    }
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    for (i = 0; i < 8; i++) {
        tail[tailLen - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    for (i = 0; i < tailLen; i += 64) {
        Sha1Block(h, tail + i);
    }
    for (i = 0; i < 20; i++) {
        out[i] = (uint8_t)(h[i / 4] >> (24 - (i % 4) * 8));
    }
}

/* Messages ---------------------------------------------------------------------- */

/* Shows a system message box and exits once it is closed. */
static void RomFail(const char* fmt, ...) {
    static char text[512];
    SceMsgDialogParam param;
    SceMsgDialogUserMessageParam msg;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    PortLog("rom: %s", text);

    sceMsgDialogParamInit(&param);
    memset(&msg, 0, sizeof(msg));
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg.msg = (const SceChar8*)text;
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    if (sceMsgDialogInit(&param) >= 0) {
        while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_TRUE);
        }
        sceMsgDialogTerm();
    }
    sceKernelExitProcess(0);
}

/* Loading ------------------------------------------------------------------------- */

static uint8_t* ReadWhole(const char* path, uint32_t* size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    uint8_t* buf;
    int64_t len;
    uint32_t done = 0;

    if (fd < 0) {
        return NULL;
    }
    len = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (len <= 0 || len > 64 * 1024 * 1024 || (buf = malloc((size_t)len)) == NULL) {
        sceIoClose(fd);
        return NULL;
    }
    while (done < (uint32_t)len) {
        int n = sceIoRead(fd, buf + done, (uint32_t)len - done > 0x100000 ? 0x100000 : (uint32_t)len - done);

        if (n <= 0) {
            free(buf);
            sceIoClose(fd);
            return NULL;
        }
        done += n;
    }
    sceIoClose(fd);
    *size = (uint32_t)len;
    return buf;
}

/* Finds the ROM: rom.gba, or any .gba file in the folder with the right SHA-1. */
static uint8_t* FindRom(const RomMapHeader* hdr, uint32_t* size) {
    uint8_t sha[20];
    uint8_t* rom = ReadWhole(ROM_PATH, size);
    SceUID dir;
    SceIoDirent ent;

    if (rom != NULL) {
        Sha1(rom, *size, sha);
        if (*size == hdr->romSize && !memcmp(sha, hdr->sha1, 20)) {
            return rom;
        }
        free(rom);
        RomFail("%s is not a copy of " GAME_NAME ", or it is modified or incomplete.\n\n"
                "Dump your own cartridge and copy it there unchanged.", ROM_PATH);
    }
    dir = sceIoDopen(ROM_DIR);
    if (dir >= 0) {
        while (sceIoDread(dir, &ent) > 0) {
            char path[512];
            size_t n = strlen(ent.d_name);

            if (n < 4 || strcasecmp(ent.d_name + n - 4, ".gba") != 0) {
                continue;
            }
            snprintf(path, sizeof(path), ROM_DIR "/%s", ent.d_name);
            rom = ReadWhole(path, size);
            if (rom == NULL) {
                continue;
            }
            Sha1(rom, *size, sha);
            if (*size == hdr->romSize && !memcmp(sha, hdr->sha1, 20)) {
                PortLog("rom: using %s", path);
                sceIoDclose(dir);
                return rom;
            }
            free(rom);
        }
        sceIoDclose(dir);
    }
    RomFail("The game data was not found.\n\nCopy your own dump of " GAME_NAME " to\n%s", ROM_PATH);
    return NULL;
}

void RomLoad(void) {
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    SceCommonDialogConfigParam config;
    uint32_t mapSize, romSize, i;
    uint8_t* map;
    uint8_t* rom;
    const RomMapHeader* hdr;
    const RomRun* runs;
    uintptr_t slide;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();

    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    sceCommonDialogConfigParamInit(&config);
    sceCommonDialogSetConfigParam(&config);

    map = ReadWhole(MAP_PATH, &mapSize);
    hdr = (const RomMapHeader*)map;
    if (map == NULL || mapSize < sizeof(*hdr) || memcmp(hdr->magic, "KHRM", 4) != 0 || hdr->version != 1 ||
        mapSize < sizeof(*hdr) + hdr->runCount * sizeof(RomRun)) {
        RomFail("This installation is damaged (%s is missing). Reinstall the VPK.", MAP_PATH);
    }
    rom = FindRom(hdr, &romSize);

    /* The data segment is loaded as a whole: one slide for every address. */
    slide = (uintptr_t)gGbaIo - hdr->anchor;
    runs = (const RomRun*)(map + sizeof(*hdr));
    for (i = 0; i < hdr->runCount; i++) {
        if (runs[i].romOffset + runs[i].length > romSize) {
            RomFail("This installation is damaged (bad %s). Reinstall the VPK.", MAP_PATH);
        }
        memcpy((void*)(runs[i].addr + slide), rom + runs[i].romOffset, runs[i].length);
    }
    PortLog("rom: loaded %u runs in %u ms", (unsigned)hdr->runCount,
            (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
    free(rom);
    free(map);
}
