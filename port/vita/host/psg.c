/*
 * The GBA's four PSG ("Game Boy") sound channels: two square waves (the first
 * with frequency sweep), the programmable wave channel and noise.
 *
 * The game drives them through the sound registers the m4a engine writes in
 * CgbSound() (emulated in gGbaIo). Once per sound frame PsgRender() picks up
 * those writes: a set bit 7 in NRx4 is a trigger (it is write-only on the GBA,
 * so it is cleared again here), frequencies, duty and panning are read live,
 * and the channel status bits in NR52 are kept current because CgbSound() reads
 * them to find notes the hardware length counter has ended.
 */
#include <string.h>

#include "port.h"
#include "vita_host.h"

#define IO(off) (gGbaIo[(off)])

#define NR10 0x60
#define NR11 0x62
#define NR12 0x63
#define NR13 0x64
#define NR14 0x65
#define NR21 0x68
#define NR22 0x69
#define NR23 0x6C
#define NR24 0x6D
#define NR30 0x70
#define NR31 0x72
#define NR32 0x73
#define NR33 0x74
#define NR34 0x75
#define NR41 0x78
#define NR42 0x79
#define NR43 0x7C
#define NR44 0x7D
#define NR50 0x80
#define NR51 0x81
#define SOUNDCNT_H 0x82
#define NR52 0x84
#define WAVE_RAM 0x90

#define OVERSAMPLE 8

typedef struct {
    int on;
    int length;       /* remaining length counter ticks (256 Hz) */
    int volume;       /* 0-15 (square/noise) */
    int envPeriod;    /* 0 = envelope off */
    int envUp;
    int envTimer;
    uint32_t phase;   /* 0.32 fixed point position in the waveform */
    /* Channel 1 sweep */
    int sweepPeriod;
    int sweepDown;
    int sweepShift;
    int sweepTimer;
    int sweepFreq;
    /* Channel 4 */
    uint16_t lfsr;
    uint32_t noiseAcc;
} PsgChannel;

static PsgChannel sCh[4];
static uint32_t sSeqAcc; /* frame sequencer (512 Hz) phase, 0.32 fixed point */
static unsigned sSeqStep;
static unsigned sNewNotes;

static const uint8_t sDuty[4] = { 0x01, 0x81, 0x87, 0x7E }; /* 12.5%, 25%, 50%, 75% of 8 steps */

void PsgNewNotes(unsigned mask) {
    sNewNotes |= mask;
}

static int Freq(int lo, int hi) {
    return IO(lo) | ((IO(hi) & 7) << 8);
}

static void Trigger(int n) {
    PsgChannel* c = &sCh[n];
    static const int nrx1[4] = { NR11, NR21, NR31, NR41 };
    static const int nrx2[4] = { NR12, NR22, 0, NR42 };
    int fresh = (sNewNotes >> n) & 1;

    /* The hardware loads the length counter when NRx1 is written: the m4a
     * engine does that only when a note starts; later triggers (envelope
     * updates) keep counting. */
    if (fresh || c->length == 0) {
        c->length = n == 2 ? 256 - IO(NR31) : 64 - (IO(nrx1[n]) & 63);
    }
    if (n != 2) {
        int env = IO(nrx2[n]);

        c->volume = env >> 4;
        c->envUp = (env >> 3) & 1;
        c->envPeriod = env & 7;
        c->envTimer = c->envPeriod;
        /* Upper five bits clear: the channel's DAC is off. */
        c->on = (env & 0xF8) != 0;
    } else {
        c->on = (IO(NR30) & 0x80) != 0;
        c->phase = 0;
    }
    if (n == 0) {
        c->sweepPeriod = (IO(NR10) >> 4) & 7;
        c->sweepDown = (IO(NR10) >> 3) & 1;
        c->sweepShift = IO(NR10) & 7;
        c->sweepTimer = c->sweepPeriod ? c->sweepPeriod : 8;
        c->sweepFreq = Freq(NR13, NR14);
    }
    if (n == 3) {
        c->lfsr = 0x7FFF;
        c->noiseAcc = 0;
    }
}

static void PollRegisters(void) {
    static const int nrx4[4] = { NR14, NR24, NR34, NR44 };
    int n;

    for (n = 0; n < 4; n++) {
        if (IO(nrx4[n]) & 0x80) {
            IO(nrx4[n]) &= 0x7F;
            Trigger(n);
        }
    }
    sNewNotes = 0;
    if (!(IO(NR30) & 0x80)) {
        sCh[2].on = 0;
    }
    if (!(IO(NR52) & 0x80)) {
        memset(sCh, 0, sizeof(sCh));
    }
}

/* One frame sequencer step: length at 256 Hz, sweep at 128 Hz, envelopes at 64 Hz. */
static void SequencerStep(void) {
    static const int nrx4[4] = { NR14, NR24, NR34, NR44 };
    unsigned step = sSeqStep++ & 7;
    int n;

    if (!(step & 1)) {
        for (n = 0; n < 4; n++) {
            PsgChannel* c = &sCh[n];

            if ((IO(nrx4[n]) & 0x40) && c->length > 0 && --c->length == 0) {
                c->on = 0;
            }
        }
    }
    if (step == 2 || step == 6) {
        PsgChannel* c = &sCh[0];

        if (c->on && c->sweepPeriod && --c->sweepTimer <= 0) {
            c->sweepTimer = c->sweepPeriod;
            if (c->sweepShift) {
                int delta = c->sweepFreq >> c->sweepShift;
                int f = c->sweepDown ? c->sweepFreq - delta : c->sweepFreq + delta;

                if (f > 2047) {
                    c->on = 0;
                } else if (f >= 0) {
                    c->sweepFreq = f;
                    IO(NR13) = f & 0xFF;
                    IO(NR14) = (IO(NR14) & ~7) | (f >> 8);
                }
            }
        }
    }
    if (step == 7) {
        for (n = 0; n < 4; n++) {
            PsgChannel* c = &sCh[n];

            if (n == 2 || !c->envPeriod || --c->envTimer > 0) {
                continue;
            }
            c->envTimer = c->envPeriod;
            if (c->envUp && c->volume < 15) {
                c->volume++;
            } else if (!c->envUp && c->volume > 0) {
                c->volume--;
            }
        }
    }
}

/* Channel outputs at one point in time, -15..15 (wave channel scaled alike). */
static void Sample(int out[4], uint32_t dt, uint32_t noiseRate) {
    static const int waveShift[4] = { 8, 0, 1, 2 }; /* NR32 0%, 100%, 50%, 25% */
    int n;

    for (n = 0; n < 2; n++) {
        PsgChannel* c = &sCh[n];
        int duty = IO(n == 0 ? NR11 : NR21) >> 6;
        int f = Freq(n == 0 ? NR13 : NR23, n == 0 ? NR14 : NR24);
        uint32_t step = (uint32_t)((uint64_t)dt * 131072 / (2048 - f));

        c->phase += step;
        out[n] = c->on ? ((sDuty[duty] >> (c->phase >> 29)) & 1 ? c->volume : -c->volume) : 0;
    }
    {
        PsgChannel* c = &sCh[2];
        int f = Freq(NR33, NR34);
        uint32_t step = (uint32_t)((uint64_t)dt * 65536 / (2048 - f));
        int idx, nib, vol;

        c->phase += step;
        idx = c->phase >> 27;
        nib = IO(WAVE_RAM + idx / 2);
        nib = idx & 1 ? nib & 15 : nib >> 4;
        vol = IO(NR32);
        nib = (nib * 2 - 15);
        if (vol & 0x80) {
            nib = nib * 3 / 4;
        } else {
            nib = waveShift[(vol >> 5) & 3] == 8 ? 0 : nib >> waveShift[(vol >> 5) & 3];
        }
        out[2] = c->on ? nib : 0;
    }
    {
        PsgChannel* c = &sCh[3];
        int width7 = (IO(NR43) >> 3) & 1;
        int clocks;

        c->noiseAcc += noiseRate;
        clocks = c->noiseAcc >> 16;
        c->noiseAcc &= 0xFFFF;
        if (clocks > 256) {
            clocks = 256;
        }
        while (clocks-- > 0) {
            int bit = (c->lfsr ^ (c->lfsr >> 1)) & 1;

            c->lfsr = (c->lfsr >> 1) | (bit << 14);
            if (width7) {
                c->lfsr = (c->lfsr & ~0x40) | (bit << 6);
            }
        }
        out[3] = c->on ? (c->lfsr & 1 ? -c->volume : c->volume) : 0;
    }
}

void PsgRender(int16_t* out, int samples, int rate) {
    /* Time per oversampled step, in 1/2^32 s units scaled so that
     * phase += dt * freq wraps once per waveform period. */
    uint32_t dt = (uint32_t)(((uint64_t)1 << 32) / ((uint64_t)rate * OVERSAMPLE));
    uint32_t seqStep = (uint32_t)(((uint64_t)512 << 32) / ((uint64_t)rate * OVERSAMPLE));
    int nr43 = IO(NR43);
    int r = nr43 & 7;
    int s = nr43 >> 4;
    /* LFSR clock: 524288 / r / 2^(s+1) Hz (r = 0 counts as 0.5); 16.16 clocks per step. */
    uint64_t noiseHz = (r ? 524288 / r : 1048576) >> (s + 1);
    uint32_t noiseRate = (uint32_t)((noiseHz << 16) / ((uint64_t)rate * OVERSAMPLE));
    int nr50 = IO(NR50);
    int nr51 = IO(NR51);
    int ratio = IO(SOUNDCNT_H) & 3; /* 0: 25%, 1: 50%, 2: 100% */
    int shift = ratio >= 2 ? 0 : 2 - ratio;
    int volR = (nr50 & 7) + 1;
    int volL = ((nr50 >> 4) & 7) + 1;
    int i, k, n;

    PollRegisters();
    for (i = 0; i < samples; i++) {
        int sumL = 0, sumR = 0;

        for (k = 0; k < OVERSAMPLE; k++) {
            int ch[4];
            uint32_t prev = sSeqAcc;

            sSeqAcc += seqStep;
            if (sSeqAcc < prev) {
                SequencerStep();
            }
            Sample(ch, dt, noiseRate);
            for (n = 0; n < 4; n++) {
                if (nr51 & (1 << n)) {
                    sumR += ch[n];
                }
                if (nr51 & (0x10 << n)) {
                    sumL += ch[n];
                }
            }
        }
        /* GBA mixer units (DirectSound at 100% spans +-512), then 16-bit
         * like PortAudioPush's DirectSound samples (x64); halved because the
         * channels here swing both ways around the bias. */
        out[i * 2] = (int16_t)(((sumL * volL * 32 / OVERSAMPLE) >> shift));
        out[i * 2 + 1] = (int16_t)(((sumR * volR * 32 / OVERSAMPLE) >> shift));
    }

    /* Channel status for CgbSound(); bit 7 (master enable) is the game's. */
    IO(NR52) = (IO(NR52) & 0x80) | (sCh[0].on ? 1 : 0) | (sCh[1].on ? 2 : 0) | (sCh[2].on ? 4 : 0) |
               (sCh[3].on ? 8 : 0);
}
