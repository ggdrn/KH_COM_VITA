/*
 * Audio output: the m4a mixer produces one frame of 8-bit stereo PCM per
 * VBlank (PortAudioPush); an audio thread resamples it to 48 kHz and feeds
 * sceAudioOut. The resampling ratio is nudged by the buffer fill level so the
 * game's 60 Hz frame clock and the audio clock never drift apart.
 */
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

#include "port.h"
#include "vita_host.h"

#define OUT_RATE 48000
#define OUT_GRAIN 768
#define RING_SIZE 16384 /* stereo frames, power of two */
#define TARGET_FILL 2048

static int16_t sRing[RING_SIZE * 2];
static volatile uint32_t sWritePos;
static volatile uint32_t sReadPos;
static volatile int sSrcRate = 15768;
static int sPort = -1;
static SceUID sThread = -1;

static int16_t Clamp16(int v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

void PortAudioPush(const int8_t* right, const int8_t* left, int samples, int rate) {
    static int16_t psg[RING_SIZE / 4 * 2];
    uint32_t w = sWritePos;
    int i;

    if (sPort < 0) {
        return;
    }
    if (rate > 0) {
        sSrcRate = rate;
    }
    if (samples > RING_SIZE / 4) {
        samples = RING_SIZE / 4;
    }
    /* The PSG channels advance even when the frame is dropped below. */
    PsgRender(psg, samples, sSrcRate);
    /* Drop the frame instead of overwriting unread audio. */
    if ((uint32_t)(w - sReadPos) + samples >= RING_SIZE) {
        return;
    }
    for (i = 0; i < samples; i++, w++) {
        uint32_t idx = (w & (RING_SIZE - 1)) * 2;
        sRing[idx] = Clamp16(left[i] * 256 + psg[i * 2]);
        sRing[idx + 1] = Clamp16(right[i] * 256 + psg[i * 2 + 1]);
    }
    sWritePos = w;
}

static int AudioThread(SceSize args, void* argp) {
    static int16_t out[OUT_GRAIN * 2];
    uint32_t frac = 0; /* 16.16 position between the two current source frames */
    int16_t prev[2] = { 0, 0 };

    (void)args;
    (void)argp;
    for (;;) {
        uint32_t avail = sWritePos - sReadPos;
        /* Base step in 16.16, corrected by up to +-0.5% toward the target fill. */
        int64_t step = ((int64_t)sSrcRate << 16) / OUT_RATE;
        int i;

        step += step * ((int32_t)avail - TARGET_FILL) / (TARGET_FILL * 200);

        for (i = 0; i < OUT_GRAIN; i++) {
            uint32_t r = sReadPos;
            int16_t cur[2];

            if (sWritePos == r) {
                /* Underrun: hold the last sample to avoid clicks. */
                out[i * 2] = prev[0];
                out[i * 2 + 1] = prev[1];
                continue;
            }
            cur[0] = sRing[(r & (RING_SIZE - 1)) * 2];
            cur[1] = sRing[(r & (RING_SIZE - 1)) * 2 + 1];
            out[i * 2] = prev[0] + (((cur[0] - prev[0]) * (int32_t)frac) >> 16);
            out[i * 2 + 1] = prev[1] + (((cur[1] - prev[1]) * (int32_t)frac) >> 16);
            frac += (uint32_t)step;
            while (frac >= 0x10000 && sWritePos != sReadPos) {
                frac -= 0x10000;
                prev[0] = sRing[(sReadPos & (RING_SIZE - 1)) * 2];
                prev[1] = sRing[(sReadPos & (RING_SIZE - 1)) * 2 + 1];
                sReadPos = sReadPos + 1;
            }
            if (frac >= 0x10000) {
                frac = 0xFFFF;
            }
        }
        sceAudioOutOutput(sPort, out);
    }
    return 0;
}

void AudioInit(void) {
    sPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (sPort < 0) {
        PortLog("audio: sceAudioOutOpenPort failed (%08X)", sPort);
        return;
    }
    sThread = sceKernelCreateThread("khcom_audio", AudioThread, 0x10000100 - 16, 0x4000, 0,
                                    SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (sThread < 0) {
        PortLog("audio: thread creation failed (%08X)", sThread);
        return;
    }
    sceKernelStartThread(sThread, 0, NULL);
}

void AudioPump(void) {
}
