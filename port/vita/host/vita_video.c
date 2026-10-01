/*
 * Presents the software-rendered GBA frame with vitaGL.
 *
 * DISPLAY_WIDE renders 282 columns (240 + 21 on each side), which at the
 * 3.4x scale that fills the 544-line screen gives a 16:9 picture.
 *
 * The frame is drawn with a fragment shader set by the picture settings
 * (config.ini, Start + L + R menu): sharp bilinear, which keeps every GBA
 * pixel the same size and crisp with only a sub-pixel blend at the pixel
 * edges (3.4 is not an integer scale, so plain nearest makes pixels 3 or 4
 * screen pixels wide and plain bilinear blurs them), plus optional GBA colour
 * correction. The shader is Cg, compiled at startup by libshacccg; if it
 * fails, the frame is drawn with plain filtering.
 *
 * With the Scale3x upscaler on, the render threads hand over a frame already
 * enlarged 3x (scale3x.c), and the same shader only resizes it to the screen.
 * The upscaler used to run in the shader, per screen pixel, which the GPU
 * could not keep at 60 fps.
 */
#include <vitaGL.h>

#include <stdio.h>
#include <string.h>

#include "port.h"
#include "ppu.h"
#include "vita_host.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define WIDE_W 282

static GLuint sTexture;
static GLuint sScaledTex[SCALED_SLOTS]; /* Scale3x frames, written by the render threads */
static GLuint sOverlayTex;
static const uint32_t* sOverlay;
static GLuint sNoticeTex;
static const uint32_t* sNotice;
static int sNoticeChanged;
static int sBarX0, sBarX1, sBarY;
static float sBarProgress = -1.0f;
static int sFrameWidth;

/* Shaders ---------------------------------------------------------------------- */

typedef struct {
    GLuint program;
    GLint texSize, outScale, sharp, gbaColors;
} Program;

static Program sSharp;

static const char sVertexShader[] =
    "void main(float2 position, float2 texcoord,\n"
    "          float2 out vTexcoord : TEXCOORD0, float4 out gl_Position : POSITION) {\n"
    "    vTexcoord = texcoord;\n"
    "    gl_Position = float4(position, 0.0, 1.0);\n"
    "}\n";

/* Sampling and the colour stage. */
static const char sCommon[] =
    "uniform sampler2D tex : TEXUNIT0;\n"
    "uniform float2 texSize;\n"   /* frame size in GBA pixels */
    "uniform float2 outScale;\n"  /* screen pixels per GBA pixel */
    "uniform float sharp;\n"
    "uniform float gbaColors;\n"
    "\n"
    /* GBA screen colours: its LCD was darker and less saturated than a modern
     * screen, and the games' colours were picked for it. Applied at half
     * strength; gamma 2 (square / square root) stands in for 2.2, which
     * would cost two pow() per pixel. */
    "float3 post(float3 c) {\n"
    "    if (gbaColors > 0.5) {\n"
    "        float3 l = c * c;\n"
    "        float3 m = float3(dot(l, float3(0.82, 0.24, -0.06)),\n"
    "                          dot(l, float3(0.125, 0.665, 0.21)),\n"
    "                          dot(l, float3(0.195, 0.075, 0.73)));\n"
    "        c = lerp(c, sqrt(saturate(m * 0.94)), 0.5);\n"
    "    }\n"
    "    return c;\n"
    "}\n";

static const char sSharpMain[] =
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 coord = uv;\n"
    "    if (sharp > 0.5) {\n"
    "        float2 texel = uv * texSize;\n"
    "        float2 base = floor(texel);\n"
    "        float2 d = texel - base - 0.5;\n"
    "        float2 region = float2(0.5, 0.5) - 0.5 / outScale;\n"
    "        float2 f = (d - clamp(d, -region, region)) * outScale + 0.5;\n"
    "        coord = (base + f) / texSize;\n"
    "    }\n"
    "    return float4(post(tex2D(tex, coord).rgb), 1.0);\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char* a, const char* b, const char* what) {
    GLuint shader = glCreateShader(type);
    const char* parts[2] = { a, b };
    GLint lens[2] = { (GLint)strlen(a), b ? (GLint)strlen(b) : 0 };
    GLint ok = 0;

    glShaderSource(shader, b ? 2 : 1, parts, lens);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        PortLog("video: %s shader failed to compile: %s", what, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void BuildProgram(Program* p, const char* main, const char* name) {
    GLuint vs = CompileShader(GL_CG_VERTEX_SHADER_EXT, sVertexShader, NULL, name);
    GLuint fs = CompileShader(GL_CG_FRAGMENT_SHADER_EXT, sCommon, main, name);
    GLint ok = 0;

    memset(p, 0, sizeof(*p));
    if (vs == 0 || fs == 0) {
        return;
    }
    p->program = glCreateProgram();
    glAttachShader(p->program, vs);
    glAttachShader(p->program, fs);
    glBindAttribLocation(p->program, 0, "position");
    glBindAttribLocation(p->program, 1, "texcoord");
    glLinkProgram(p->program);
    glGetProgramiv(p->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        PortLog("video: %s shader failed to link", name);
        glDeleteProgram(p->program);
        p->program = 0;
        return;
    }
    p->texSize = glGetUniformLocation(p->program, "texSize");
    p->outScale = glGetUniformLocation(p->program, "outScale");
    p->sharp = glGetUniformLocation(p->program, "sharp");
    p->gbaColors = glGetUniformLocation(p->program, "gbaColors");
    PortLog("video: %s shader ready", name);
}

/* Setup ------------------------------------------------------------------------ */

void VideoInit(void) {
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    {
        uint32_t* data[SCALED_SLOTS];
        int i;

        glGenTextures(SCALED_SLOTS, sScaledTex);
        for (i = 0; i < SCALED_SLOTS; i++) {
            glBindTexture(GL_TEXTURE_2D, sScaledTex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sFrameWidth * 3, GBA_SCREEN_HEIGHT * 3, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            data[i] = vglGetTexDataPointer(GL_TEXTURE_2D);
            if (data[i] == NULL) {
                PortLog("video: no memory for the Scale3x frames, upscaler disabled");
                break;
            }
        }
        /* vitaGL lays texture lines out 8-pixel aligned. */
        if (i == SCALED_SLOTS) {
            RenderSetScaledSlots(data, (sFrameWidth * 3 + 7) & ~7);
        }
    }

    glGenTextures(1, &sOverlayTex);
    glBindTexture(GL_TEXTURE_2D, sOverlayTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MENU_W, MENU_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &sNoticeTex);
    glBindTexture(GL_TEXTURE_2D, sNoticeTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NOTICE_W, NOTICE_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);

    BuildProgram(&sSharp, sSharpMain, "sharp");
    PortLog("video: vitaGL ready, frame %dx%d, display mode %d", sFrameWidth, GBA_SCREEN_HEIGHT,
            gPortConfig.display);
}

void VideoSetOverlay(const uint32_t* rgba) {
    sOverlay = rgba;
}

void VideoSetUnstockBar(int x0, int x1, int y, float progress) {
    sBarX0 = x0;
    sBarX1 = x1;
    sBarY = y;
    sBarProgress = progress;
}

void VideoSetNotice(const uint32_t* rgba, int changed) {
    sNotice = rgba;
    sNoticeChanged |= changed;
}

/* Drawing ---------------------------------------------------------------------- */

static void DrawFixed(float x0, float x1, int linear) {
    GLint filter = linear ? GL_LINEAR : GL_NEAREST;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
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
}

/* Draws the bound frame texture, w x h texels; returns 0 when the shader is
 * not needed or unavailable. */
static int DrawShaded(float x0, float x1, int w, int h, int scaled) {
    const PortConfig* cfg = &gPortConfig;
    const Program* p = &sSharp;
    float pos[8], uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    float nx0 = x0 / (SCREEN_W / 2) - 1, nx1 = x1 / (SCREEN_W / 2) - 1;
    /* The Scale3x frame is near screen size: always resized the sharp way. */
    int sharp = scaled || cfg->sharp;

    if (p->program == 0 || (!sharp && !cfg->gbaColors)) {
        return 0;
    }
    /* Triangle strip: top-left, top-right, bottom-left, bottom-right. */
    pos[0] = nx0, pos[1] = 1, pos[2] = nx1, pos[3] = 1;
    pos[4] = nx0, pos[5] = -1, pos[6] = nx1, pos[7] = -1;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUseProgram(p->program);
    glUniform2f(p->texSize, (float)w, (float)h);
    glUniform2f(p->outScale, (x1 - x0) / w, (float)SCREEN_H / h);
    glUniform1f(p->sharp, sharp ? 1.0f : 0.0f);
    glUniform1f(p->gbaColors, cfg->gbaColors ? 1.0f : 0.0f);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glUseProgram(0);
    return 1;
}

/* Alpha-blends the bound texture over the screen rectangle. */
static void DrawBlended(float x0, float y0, float x1, float y1) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(x0, y0, 0);
    glTexCoord2f(1, 0);
    glVertex3f(x1, y0, 0);
    glTexCoord2f(1, 1);
    glVertex3f(x1, y1, 0);
    glTexCoord2f(0, 1);
    glVertex3f(x0, y1, 0);
    glEnd();
    glDisable(GL_BLEND);
}

static void DrawOverlay(void) {
    glBindTexture(GL_TEXTURE_2D, sOverlayTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MENU_W, MENU_H, GL_RGBA, GL_UNSIGNED_BYTE, sOverlay);
    DrawBlended(0, 0, SCREEN_W, SCREEN_H);
}

static void FillQuad(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex3f(x0, y0, 0);
    glVertex3f(x1, y0, 0);
    glVertex3f(x1, y1, 0);
    glVertex3f(x0, y1, 0);
    glEnd();
}

/* The unstock progress bar, given in GBA screen pixels: mapped through the
 * frame's placement (x0..x1 on screen, the widescreen margin). */
static void DrawUnstockBar(float x0, float x1) {
    float sx = (x1 - x0) / sFrameWidth, sy = (float)SCREEN_H / GBA_SCREEN_HEIGHT;
    int margin = (sFrameWidth - GBA_SCREEN_WIDTH) / 2;
    float l = x0 + (sBarX0 + margin) * sx, r = x0 + (sBarX1 + margin) * sx;
    float t = sBarY * sy, b = t + 3 * sy;
    float fill = l + 2 + (r - l - 4) * (sBarProgress > 1 ? 1 : sBarProgress);

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    FillQuad(l, t, r, b, 0.94f, 0.94f, 0.94f, 1.0f);              /* border */
    FillQuad(l + 2, t + 2, r - 2, b - 2, 0.03f, 0.05f, 0.16f, 0.9f); /* track */
    FillQuad(l + 2, t + 2, fill, b - 2, 1.0f, 0.84f, 0.25f, 1.0f);   /* filled part */
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);
    glEnable(GL_TEXTURE_2D);
}

/* Bottom-right corner, 2x, 16 screen pixels from the edges. */
static void DrawNotice(void) {
    const float x1 = SCREEN_W - 16, y1 = SCREEN_H - 16;

    glBindTexture(GL_TEXTURE_2D, sNoticeTex);
    if (sNoticeChanged) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, NOTICE_W, NOTICE_H, GL_RGBA, GL_UNSIGNED_BYTE, sNotice);
        sNoticeChanged = 0;
    }
    DrawBlended(x1 - NOTICE_W * 2, y1 - NOTICE_H * 2, x1, y1);
}

void VideoPresent(void) {
    float x0, x1;
    int w, h, slot;

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

    {
        const uint32_t* frame = RenderLockFront(&slot);

        if (slot >= 0) {
            /* Already in the texture's memory. */
            w = sFrameWidth * 3;
            h = GBA_SCREEN_HEIGHT * 3;
            glBindTexture(GL_TEXTURE_2D, sScaledTex[slot]);
        } else {
            w = sFrameWidth;
            h = GBA_SCREEN_HEIGHT;
            glBindTexture(GL_TEXTURE_2D, sTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frame);
        }
        RenderUnlockFront();
    }

    glClear(GL_COLOR_BUFFER_BIT);
    if (!DrawShaded(x0, x1, w, h, slot >= 0)) {
        DrawFixed(x0, x1, gPortConfig.filter != FILTER_NEAREST);
    }
    if (sBarProgress >= 0) {
        DrawUnstockBar(x0, x1);
    }
    if (sNotice != NULL) {
        DrawNotice();
    }
    if (sOverlay != NULL) {
        DrawOverlay();
    }
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
