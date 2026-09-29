#ifndef GUARD_VITA_HOST_H
#define GUARD_VITA_HOST_H

#include <stdint.h>

/* Host-side (Vita ABI) modules of the port. */

enum {
    DISPLAY_WIDE,
    DISPLAY_FIT,
    DISPLAY_STRETCH,
};

enum {
    FILTER_LINEAR,
    FILTER_NEAREST,
};

typedef struct PortConfig {
    int display;
    int filter;
    int swapAB;
} PortConfig;

extern PortConfig gPortConfig;

void PortFlushSram(void);

void RenderInit(int width);
const uint32_t* RenderLockFront(void);
void RenderUnlockFront(void);
extern volatile uint32_t gPortRenderUs;
extern volatile uint32_t gPortCaptureWaitUs;

void VideoInit(void);
void VideoPresent(void);
void VideoShowFatal(const char* msg);

void FaultInit(void);
void FaultPoll(void);
void WatchdogInit(void);

void InputInit(void);
void InputPoll(void);

void AudioInit(void);
/* Fills the executable's ROM data from the player's ROM (vita_rom.c). */
void RomLoad(void);
void PsgRender(int16_t* out, int samples, int rate);
void AudioPump(void);

#endif /* GUARD_VITA_HOST_H */
