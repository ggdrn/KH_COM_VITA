/*
 * Presents the software-rendered GBA frame with vitaGL.
 *
 * DISPLAY_WIDE renders 282 columns (240 + 21 on each side), which at the
 * 3.4x scale that fills the 544-line screen gives a 16:9 picture.
 */
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

#include "port.h"
#include "ppu.h"
#include "vita_host.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define WIDE_W 282

static GLuint sTexture;
static int sFrameWidth;

void VideoInit(void) {
    GLint filter;

    /* The first argument sizes the vertex pool used by glBegin/glEnd. */
    vglInitExtended(0x100000, SCREEN_W, SCREEN_H, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);

    sFrameWidth = gPortConfig.display == DISPLAY_WIDE ? WIDE_W : GBA_SCREEN_WIDTH;
    RenderInit(sFrameWidth);

    glClearColor(0, 0, 0, 1);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glGenTextures(1, &sTexture);
    glBindTexture(GL_TEXTURE_2D, sTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sFrameWidth, GBA_SCREEN_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    filter = gPortConfig.filter == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(GL_TEXTURE_2D);
    PortLog("video: vitaGL ready, frame %dx%d, display mode %d", sFrameWidth, GBA_SCREEN_HEIGHT,
            gPortConfig.display);
}

void VideoPresent(void) {
    float x0, x1;

    switch (gPortConfig.display) {
    case DISPLAY_FIT: {
        /* 240x160 scaled by 3.4 to 816x544, centered. */
        float w = GBA_SCREEN_WIDTH * (SCREEN_H / (float)GBA_SCREEN_HEIGHT);
        x0 = (SCREEN_W - w) / 2;
        x1 = x0 + w;
        break;
    }
    default:
        x0 = 0;
        x1 = SCREEN_W;
        break;
    }

    glBindTexture(GL_TEXTURE_2D, sTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sFrameWidth, GBA_SCREEN_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                    RenderLockFront());
    RenderUnlockFront();

    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(x0, 0, 0);
    glTexCoord2f(1, 0);
    glVertex3f(x1, 0, 0);
    glTexCoord2f(1, 1);
    glVertex3f(x1, SCREEN_H, 0);
    glTexCoord2f(0, 1);
    glVertex3f(x0, SCREEN_H, 0);
    glEnd();
    /* vglWaitVblankStart(GL_TRUE) makes the swap wait for vblank: 60 Hz pacing. */
    vglSwapBuffers(GL_FALSE);
    {
        static int sLogged;
        GLenum err = glGetError();
        if (err != GL_NO_ERROR && sLogged < 10) {
            sLogged++;
            PortLog("video: GL error %04X", err);
        }
    }
}

void VideoShowFatal(const char* msg) {
    int i;

    /* Clear to dark red so a fatal stop is visible even without the log. */
    (void)msg;
    glClearColor(0.5f, 0, 0, 1);
    for (i = 0; i < 2; i++) {
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);
    }
}
