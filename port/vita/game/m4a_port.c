/*
 * C implementation of asm/m4a_1.s (the m4a sound engine's sequencer and
 * DirectSound mixer), following the original instruction by instruction where
 * behaviour matters (envelopes, byte-wrapping mix, loop handling).
 *
 * Song streams and voicegroups are binary ROM data, so every pointer read from
 * them goes through GBA_PTR().
 */
#include "m4a.h"
#include "gba/io_reg.h"
#include "gba/romptr.h"
#include "port.h"

#include <string.h>

#define RHYTHM 0x80
#define SPLIT 0x40

/* m4aSoundInit() copies this "code" into SoundMainRAM_Buffer; nothing runs it. */
char SoundMainRAM[0x400];

static void SoundMainRAM_Mix(SoundInfo* si, s8* buf);
static void ChnVolSet(SoundChannel* chan, MusicPlayerTrack* track);

u32 umul3232H32(u32 a, u32 b) {
    return (u32)(((u64)a * b) >> 32);
}

/* Sequencer ------------------------------------------------------------------ */

void SoundMain(void) {
    SoundInfo* si = gSoundInfoPtr;
    s8* buf;
    int counter;

    if (si->ident != ID_NUMBER) {
        return;
    }
    si->ident++;

    if (si->MPlayMainHead != NULL) {
        si->MPlayMainHead(si->musicPlayerHead);
    }
    if (si->cgbChans != NULL) {
        unsigned fresh = 0;
        int i;

        for (i = 0; i < 4; i++) {
            u8 flags = si->cgbChans[i].statusFlags;

            if ((flags & SOUND_CHANNEL_SF_START) && !(flags & SOUND_CHANNEL_SF_STOP)) {
                fresh |= 1u << i;
            }
        }
        PsgNewNotes(fresh);
    }
    si->CgbSound();

    buf = si->pcmBuffer;
    counter = si->pcmDmaCounter;
    if (counter > 1) {
        buf += si->pcmSamplesPerVBlank * (si->pcmDmaPeriod - (counter - 1));
    }
    SoundMainRAM_Mix(si, buf);
    PortAudioPush(buf, buf + PCM_DMA_BUF_SIZE, si->pcmSamplesPerVBlank, si->pcmFreq);

    si->ident = ID_NUMBER;
}

void SoundMainBTM(void* work) {
    memset(work, 0, 64);
}

void RealClearChain(void* work) {
    SoundChannel* chan = work;
    MusicPlayerTrack* track = chan->track;
    SoundChannel* next;
    SoundChannel* prev;

    if (track == NULL) {
        return;
    }
    next = chan->nextChannelPointer;
    prev = chan->prevChannelPointer;
    if (prev != NULL) {
        prev->nextChannelPointer = next;
    } else {
        track->chan = next;
    }
    if (next != NULL) {
        next->prevChannelPointer = prev;
    }
    chan->track = NULL;
}

/* Walks a track's channel list the way the asm does: a self-link ends it. */
static SoundChannel* NextChannel(SoundChannel* chan) {
    SoundChannel* next = chan->nextChannelPointer;

    if (next == chan) {
        chan->nextChannelPointer = NULL;
        next = NULL;
    }
    return next;
}

void ply_fine(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    SoundChannel* chan = track->chan;

    while (chan != NULL) {
        if (chan->statusFlags & SOUND_CHANNEL_SF_ON) {
            chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
        }
        RealClearChain(chan);
        chan = NextChannel(chan);
    }
    track->flags = 0;
}

void MPlayJumpTableCopy(MPlayFunc* mplayJumpTable) {
    int i;

    for (i = 0; i < 36; i++) {
        mplayJumpTable[i] = gMPlayJumpTableTemplate[i];
    }
}

static u8 ReadCmdByte(MusicPlayerTrack* track) {
    return *track->cmdPtr++;
}

static u32 ReadCmdWord(const u8* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

void ply_goto(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->cmdPtr = GBA_PTR((u8*)ReadCmdWord(track->cmdPtr));
}

void ply_patt(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    if (track->patternLevel >= 3) {
        ply_fine(mplayInfo, track);
        return;
    }
    track->patternStack[track->patternLevel] = track->cmdPtr + 4;
    track->patternLevel++;
    ply_goto(mplayInfo, track);
}

void ply_pend(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    if (track->patternLevel != 0) {
        track->patternLevel--;
        track->cmdPtr = track->patternStack[track->patternLevel];
    }
}

void ply_rept(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 count = *track->cmdPtr;

    if (count == 0) {
        track->cmdPtr++;
        ply_goto(mplayInfo, track);
        return;
    }
    track->repN++;
    track->cmdPtr++;
    if (track->repN < count) {
        ply_goto(mplayInfo, track);
    } else {
        track->repN = 0;
        track->cmdPtr += 4;
    }
}

void ply_prio(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->priority = ReadCmdByte(track);
}

void ply_tempo(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u32 tempo = ReadCmdByte(track) << 1;

    mplayInfo->tempoD = tempo;
    mplayInfo->tempoI = (tempo * mplayInfo->tempoU) >> 8;
}

void ply_keysh(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->keyShift = ReadCmdByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_voice(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 index = ReadCmdByte(track);
    const ToneData* tone = &mplayInfo->tone[index];

    /*
     * The track keeps its own copy of the tone. wav (and, for split tones,
     * the key-split table stored over attack..release) are ROM pointers,
     * translated once here.
     */
    track->tone = *tone;
    track->tone.wav = GBA_PTR(tone->wav);
    if (tone->type & SPLIT) {
        u8* table = *(u8**)&track->tone.attack;
        table = GBA_PTR(table);
        memcpy(&track->tone.attack, &table, sizeof(table));
    }
}

void ply_vol(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->vol = ReadCmdByte(track);
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_pan(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->pan = ReadCmdByte(track) - 0x40;
    track->flags |= MPT_FLG_VOLCHG;
}

void ply_bend(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->bend = ReadCmdByte(track) - 0x40;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_bendr(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->bendRange = ReadCmdByte(track);
    track->flags |= MPT_FLG_PITCHG;
}

void ply_lfodl(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->lfoDelay = ReadCmdByte(track);
}

void ply_modt(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 modT = ReadCmdByte(track);

    if (track->modT != modT) {
        track->modT = modT;
        track->flags |= MPT_FLG_VOLCHG | MPT_FLG_PITCHG;
    }
}

void ply_tune(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->tune = ReadCmdByte(track) - 0x40;
    track->flags |= MPT_FLG_PITCHG;
}

void ply_port(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 reg = ReadCmdByte(track);
    u8 value = ReadCmdByte(track);

    ((vu8*)&REG_SOUND1CNT_L)[reg] = value;
}

static void clear_modM(void* unused, MusicPlayerTrack* track) {
    track->modM = 0;
    track->lfoSpeedC = 0;
    track->flags |= track->modT ? MPT_FLG_VOLCHG : MPT_FLG_PITCHG;
}

void ply_lfos(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->lfoSpeed = ReadCmdByte(track);
    if (track->lfoSpeed == 0) {
        clear_modM(NULL, track);
    }
}

void ply_mod(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    track->mod = ReadCmdByte(track);
    if (track->mod == 0) {
        clear_modM(NULL, track);
    }
}

void ply_endtie(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    u8 key;
    SoundChannel* chan;

    if (*track->cmdPtr < 0x80) {
        key = *track->cmdPtr++;
        track->key = key;
    } else {
        key = track->key;
    }
    for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
        u8 flags = chan->statusFlags;
        if ((flags & (SOUND_CHANNEL_SF_START | SOUND_CHANNEL_SF_ENV)) && !(flags & SOUND_CHANNEL_SF_STOP)
            && chan->midiKey == key) {
            chan->statusFlags = flags | SOUND_CHANNEL_SF_STOP;
            return;
        }
    }
}

void TrackStop(MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    SoundChannel* chan;

    if (!(track->flags & MPT_FLG_EXIST)) {
        return;
    }
    for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
        if (chan->statusFlags != 0) {
            u8 cgbType = chan->type & 7;
            if (cgbType != 0) {
                gSoundInfoPtr->CgbOscOff(cgbType);
            }
            chan->statusFlags = 0;
        }
        chan->track = NULL;
    }
    track->chan = NULL;
}

static void ChnVolSet(SoundChannel* chan, MusicPlayerTrack* track) {
    s32 pan = (s8)chan->rhythmPan;
    u32 v;

    v = ((0x80 + pan) * chan->velocity * track->volMR) >> 14;
    chan->rightVolume = v > 0xFF ? 0xFF : v;
    v = ((0x7F - pan) * chan->velocity * track->volML) >> 14;
    chan->leftVolume = v > 0xFF ? 0xFF : v;
}

void ply_note(u32 noteCmd, MusicPlayerInfo* mplayInfo, MusicPlayerTrack* track) {
    SoundInfo* si = gSoundInfoPtr;
    const u8* p;
    s32 rhythmPan = 0;
    ToneData tone;
    u8 key;
    u32 priority;
    u8 cgbType;
    SoundChannel* chan;
    s32 k;

    track->gateTime = gClockTable[noteCmd];
    p = track->cmdPtr;
    if (*p < 0x80) {
        track->key = *p++;
        if (*p < 0x80) {
            track->velocity = *p++;
            if (*p < 0x80) {
                track->gateTime += *p++;
            }
        }
        track->cmdPtr = (u8*)p;
    }

    if (track->tone.type & (RHYTHM | SPLIT)) {
        u8 index = track->key;
        const ToneData* sub;

        if (track->tone.type & SPLIT) {
            const u8* keySplit = *(u8**)&track->tone.attack;
            index = keySplit[track->key];
        }
        sub = &((const ToneData*)track->tone.wav)[index];
        if (sub->type & (RHYTHM | SPLIT)) {
            return;
        }
        tone = *sub;
        tone.wav = GBA_PTR(sub->wav);
        if (track->tone.type & RHYTHM) {
            if (tone.pan_sweep & 0x80) {
                rhythmPan = ((s32)tone.pan_sweep - 0xC0) * 2;
            }
            key = tone.key;
        } else {
            key = track->key;
        }
    } else {
        tone = track->tone;
        key = track->key;
    }

    priority = mplayInfo->priority + track->priority;
    if (priority > 0xFF) {
        priority = 0xFF;
    }

    cgbType = tone.type & 7;
    if (cgbType != 0) {
        if (si->cgbChans == NULL) {
            return;
        }
        chan = (SoundChannel*)&si->cgbChans[cgbType - 1];
        if ((chan->statusFlags & SOUND_CHANNEL_SF_ON) && !(chan->statusFlags & SOUND_CHANNEL_SF_STOP)) {
            if (chan->priority > priority) {
                return;
            }
            if (chan->priority == priority && (u32)chan->track < (u32)track) {
                return;
            }
        }
    } else {
        u32 lowPrio = priority;
        MusicPlayerTrack* lowTrack = track;
        int foundStopping = 0;
        SoundChannel* c = si->chans;
        int i;

        chan = NULL;
        for (i = si->maxChans; i > 0; i--, c++) {
            u8 flags = c->statusFlags;

            if (!(flags & SOUND_CHANNEL_SF_ON)) {
                chan = c;
                break;
            }
            if (flags & SOUND_CHANNEL_SF_STOP) {
                if (!foundStopping) {
                    foundStopping = 1;
                    lowPrio = c->priority;
                    lowTrack = c->track;
                    chan = c;
                    continue;
                }
            } else if (foundStopping) {
                continue;
            }
            if (c->priority < lowPrio) {
                lowPrio = c->priority;
                lowTrack = c->track;
                chan = c;
            } else if (c->priority == lowPrio) {
                if ((u32)c->track > (u32)lowTrack) {
                    lowTrack = c->track;
                    chan = c;
                } else if (c->track == lowTrack) {
                    chan = c;
                }
            }
        }
        if (chan == NULL) {
            return;
        }
    }

    ClearChain(chan);
    chan->prevChannelPointer = NULL;
    chan->nextChannelPointer = track->chan;
    if (track->chan != NULL) {
        track->chan->prevChannelPointer = chan;
    }
    track->chan = chan;
    chan->track = track;

    track->lfoDelayC = track->lfoDelay;
    if (track->lfoDelay != 0) {
        clear_modM(NULL, track);
    }
    TrkVolPitSet(mplayInfo, track);

    chan->gateTime = track->gateTime;
    chan->midiKey = track->key;
    chan->velocity = track->velocity;
    chan->priority = priority;
    chan->key = key;
    chan->rhythmPan = rhythmPan;
    chan->type = tone.type;
    chan->wav = tone.wav;
    chan->attack = tone.attack;
    chan->decay = tone.decay;
    chan->sustain = tone.sustain;
    chan->release = tone.release;
    chan->pseudoEchoVolume = track->pseudoEchoVolume;
    chan->pseudoEchoLength = track->pseudoEchoLength;
    ChnVolSet(chan, track);

    k = chan->key + (s8)track->keyM;
    if (k < 0) {
        k = 0;
    }
    if (cgbType != 0) {
        CgbChannel* cgb = (CgbChannel*)chan;
        u8 ps = tone.pan_sweep;

        cgb->length = tone.length;
        cgb->sweep = ((ps & 0x80) || !(ps & 0x70)) ? 8 : ps;
        chan->frequency = si->MidiKeyToCgbFreq(cgbType, k, track->pitM);
    } else {
        chan->frequency = MidiKeyToFreq(tone.wav, k, track->pitM);
    }
    chan->statusFlags = SOUND_CHANNEL_SF_START;
    track->flags &= 0xF0;
}

void MPlayMain(MusicPlayerInfo* mplayInfo) {
    SoundInfo* si;
    u32 tempoC;

    if (mplayInfo->ident != ID_NUMBER) {
        return;
    }
    mplayInfo->ident++;

    if (mplayInfo->MPlayMainNext != NULL) {
        mplayInfo->MPlayMainNext(mplayInfo->musicPlayerNext);
    }
    if ((s32)mplayInfo->status < 0) {
        goto done;
    }
    si = gSoundInfoPtr;
    FadeOutBody(mplayInfo);
    if ((s32)mplayInfo->status < 0) {
        goto done;
    }

    tempoC = mplayInfo->tempoC + mplayInfo->tempoI;
    mplayInfo->tempoC = tempoC;
    while (tempoC >= 150) {
        MusicPlayerTrack* track = mplayInfo->tracks;
        int count = mplayInfo->trackCount;
        u32 bit = 1;
        u32 active = 0;

        for (; count > 0; count--, track++, bit <<= 1) {
            SoundChannel* chan;

            if (!(track->flags & MPT_FLG_EXIST)) {
                continue;
            }
            active |= bit;

            for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
                if (chan->statusFlags & SOUND_CHANNEL_SF_ON) {
                    if (chan->gateTime != 0 && --chan->gateTime == 0) {
                        chan->statusFlags |= SOUND_CHANNEL_SF_STOP;
                    }
                } else {
                    ClearChain(chan);
                }
            }

            if (track->flags & MPT_FLG_START) {
                Clear64byte(track);
                track->flags = MPT_FLG_EXIST;
                track->bendRange = 2;
                track->volX = 64;
                track->lfoSpeed = 22;
                track->tone.type = 1;
            }

            while (track->wait == 0) {
                u8 cmd = *track->cmdPtr;

                if (cmd < 0x80) {
                    cmd = track->runningStatus;
                } else {
                    track->cmdPtr++;
                    if (cmd >= 0xBD) {
                        track->runningStatus = cmd;
                    }
                }
                if (cmd >= 0xCF) {
                    si->plynote(cmd - 0xCF, mplayInfo, track);
                } else if (cmd > 0xB0) {
                    mplayInfo->cmd = cmd - 0xB1;
                    ((void (*)(MusicPlayerInfo*, MusicPlayerTrack*))si->MPlayJumpTable[cmd - 0xB1])(mplayInfo, track);
                    if (track->flags == 0) {
                        goto next_track;
                    }
                } else {
                    track->wait = gClockTable[cmd - 0x80];
                }
            }

            track->wait--;
            if (track->lfoSpeed != 0 && track->mod != 0) {
                if (track->lfoDelayC != 0) {
                    track->lfoDelayC--;
                } else {
                    u8 c;
                    s32 v;

                    track->lfoSpeedC += track->lfoSpeed;
                    c = track->lfoSpeedC;
                    if ((s8)(c - 0x40) < 0) {
                        v = (s8)c;
                    } else {
                        v = 0x80 - c;
                    }
                    v = (track->mod * v) >> 6;
                    if ((u8)(track->modM ^ v) != 0) {
                        track->modM = v;
                        track->flags |= track->modT ? MPT_FLG_VOLCHG : MPT_FLG_PITCHG;
                    }
                }
            }
        next_track:;
        }

        mplayInfo->clock++;
        if (active == 0) {
            mplayInfo->status = MUSICPLAYER_STATUS_PAUSE;
            goto done;
        }
        mplayInfo->status = active;
        tempoC = (u16)(mplayInfo->tempoC - 150);
        mplayInfo->tempoC = tempoC;
    }

    {
        MusicPlayerTrack* track = mplayInfo->tracks;
        int count = mplayInfo->trackCount;

        for (; count > 0; count--, track++) {
            SoundChannel* chan;

            if (!(track->flags & MPT_FLG_EXIST) || !(track->flags & 0xF)) {
                continue;
            }
            TrkVolPitSet(mplayInfo, track);
            for (chan = track->chan; chan != NULL; chan = NextChannel(chan)) {
                u8 cgbType;

                if (!(chan->statusFlags & SOUND_CHANNEL_SF_ON)) {
                    ClearChain(chan);
                    continue;
                }
                cgbType = chan->type & 7;
                if (track->flags & MPT_FLG_VOLCHG) {
                    ChnVolSet(chan, track);
                    if (cgbType != 0) {
                        ((CgbChannel*)chan)->modify |= CGB_CHANNEL_MO_VOL;
                    }
                }
                if (track->flags & MPT_FLG_PITCHG) {
                    s32 k = chan->key + (s8)track->keyM;
                    if (k < 0) {
                        k = 0;
                    }
                    if (cgbType != 0) {
                        ((CgbChannel*)chan)->frequency = si->MidiKeyToCgbFreq(cgbType, k, track->pitM);
                        ((CgbChannel*)chan)->modify |= CGB_CHANNEL_MO_PIT;
                    } else {
                        chan->frequency = MidiKeyToFreq(chan->wav, k, track->pitM);
                    }
                }
            }
            track->flags &= 0xF0;
        }
    }

done:
    mplayInfo->ident = ID_NUMBER;
}

/* Mixer ---------------------------------------------------------------------------- */

static inline void MixSample(s8* right, s8* left, s32 sample, u32 volR, u32 volL) {
    *right = (s8)(*right + ((sample * (s32)volR) >> 8));
    *left = (s8)(*left + ((sample * (s32)volL) >> 8));
}

static void SoundMainRAM_Mix(SoundInfo* si, s8* buf) {
    s32 samples = si->pcmSamplesPerVBlank;
    SoundChannel* chan;
    int i;

    if (si->reverb != 0) {
        const s8* src = (si->pcmDmaCounter == 2) ? si->pcmBuffer : buf + samples;
        for (i = 0; i < samples; i++) {
            s32 v = buf[i + PCM_DMA_BUF_SIZE] + buf[i] + src[i + PCM_DMA_BUF_SIZE] + src[i];
            v = (v * si->reverb) >> 9;
            if (v & 0x80) {
                v++;
            }
            buf[i + PCM_DMA_BUF_SIZE] = v;
            buf[i] = v;
        }
    } else {
        memset(buf, 0, samples);
        memset(buf + PCM_DMA_BUF_SIZE, 0, samples);
    }

    chan = si->chans;
    for (i = si->maxChans; i > 0; i--, chan++) {
        WaveData* wav = chan->wav;
        u8 flags = chan->statusFlags;
        u32 env;
        u32 volR, volL;
        const s8* loopStart = NULL;
        s32 loopLen = 0;
        s8* out;
        s32 n;

        if (!(flags & SOUND_CHANNEL_SF_ON)) {
            continue;
        }

        if (flags & SOUND_CHANNEL_SF_START) {
            if (flags & SOUND_CHANNEL_SF_STOP) {
                chan->statusFlags = 0;
                continue;
            }
            flags = SOUND_CHANNEL_SF_ENV_ATTACK;
            chan->currentPointer = wav->data;
            chan->count = wav->size;
            chan->fw = 0;
            env = 0;
            if (((const u8*)wav)[3] & 0xC0) {
                flags |= 0x10;
            }
            chan->statusFlags = flags;
            goto attack;
        }

        env = chan->envelopeVolume;
        if (flags & SOUND_CHANNEL_SF_IEC) {
            u8 len = chan->pseudoEchoLength--;
            if (len <= 1) {
                chan->statusFlags = 0;
                continue;
            }
        } else if (flags & SOUND_CHANNEL_SF_STOP) {
            env = (env * chan->release) >> 8;
            if (env <= chan->pseudoEchoVolume) {
            echo:
                env = chan->pseudoEchoVolume;
                if (env == 0) {
                    chan->statusFlags = 0;
                    continue;
                }
                flags |= SOUND_CHANNEL_SF_IEC;
                chan->statusFlags = flags;
            }
        } else if ((flags & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_DECAY) {
            env = (env * chan->decay) >> 8;
            if (env <= chan->sustain) {
                env = chan->sustain;
                if (env == 0) {
                    goto echo;
                }
                flags--;
                chan->statusFlags = flags;
            }
        } else if ((flags & SOUND_CHANNEL_SF_ENV) == SOUND_CHANNEL_SF_ENV_ATTACK) {
        attack:
            env += chan->attack;
            if (env >= 0xFF) {
                env = 0xFF;
                flags--;
                chan->statusFlags = flags;
            }
        }

        chan->envelopeVolume = env;
        env = ((si->masterVolume + 1) * env) >> 4;
        volR = (chan->rightVolume * env) >> 8;
        volL = (chan->leftVolume * env) >> 8;
        chan->envelopeVolumeRight = volR;
        chan->envelopeVolumeLeft = volL;

        if (flags & 0x10) {
            loopStart = wav->data + wav->loopStart;
            loopLen = wav->size - wav->loopStart;
        }

        out = buf;
        n = samples;
        if (chan->type & TONEDATA_TYPE_FIX) {
            const s8* cur = chan->currentPointer;
            s32 count = chan->count;

            while (n > 0) {
                MixSample(out, out + PCM_DMA_BUF_SIZE, *cur++, volR, volL);
                out++;
                n--;
                if (--count == 0) {
                    if (loopLen != 0) {
                        cur = loopStart;
                        count = loopLen;
                    } else {
                        chan->statusFlags = 0;
                        break;
                    }
                }
            }
            chan->count = count;
            chan->currentPointer = (s8*)cur;
        } else {
            const s8* cur = chan->currentPointer;
            s32 count = chan->count;
            u32 fw = chan->fw;
            u32 step = si->divFreq * chan->frequency;
            s32 s0 = cur[0];
            s32 delta;
            int stopped = 0;

            cur++;
            delta = *cur - s0;
            while (n > 0) {
                u32 adv;

                MixSample(out, out + PCM_DMA_BUF_SIZE, s0 + (((s32)fw * delta) >> 23), volR, volL);
                out++;
                n--;
                fw += step;
                adv = fw >> 23;
                if (adv == 0) {
                    continue;
                }
                fw &= 0x7FFFFF;
                count -= adv;
                if (count <= 0) {
                    s32 over;

                    if (loopLen == 0) {
                        chan->statusFlags = 0;
                        stopped = 1;
                        break;
                    }
                    over = -count;
                    count += loopLen;
                    while (count <= 0) {
                        over -= loopLen;
                        count += loopLen;
                    }
                    cur = loopStart + over;
                    s0 = *cur;
                } else if (adv == 1) {
                    s0 = *cur;
                } else {
                    cur += adv - 1;
                    s0 = *cur;
                }
                cur++;
                delta = *cur - s0;
            }
            if (!stopped) {
                chan->fw = fw;
                chan->count = count;
                chan->currentPointer = (s8*)(cur - 1);
            }
        }
    }
}
