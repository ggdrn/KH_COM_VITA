/*
 * Scale2x (EPX / AdvMAME2x), on the CPU: every GBA pixel E becomes 2x2
 * pixels, taking a neighbour's colour in a corner where the two neighbours
 * beside it agree, which turns stair steps into smoother slopes. Lighter
 * than Scale3x (four pixels per source pixel instead of nine) and with the
 * same 2x layout as MMPX.
 *
 *     B        E0 E1
 *   D E F  ->  E2 E3
 *     H
 *
 *   E0 = D == B ? D : E      E1 = B == F ? F : E
 *   E2 = D == H ? D : E      E3 = H == F ? F : E
 *
 * all four E when B == H or D == F (no diagonal edge through E).
 *
 * The rules are a handful of comparisons and selects, so the line is done
 * four pixels at a time with NEON, without branches; its two ends, and the
 * pixels the sprite mask leaves alone, are done one by one. As with the other
 * upscalers, the lines are built in a cached buffer and copied out to the
 * texture (uncached memory) in one block.
 */
#include <arm_neon.h>
#include <psp2/kernel/clib.h>

#include "port.h"
#include "vita_host.h"

#define LINE_MAX (PORT_MAX_SCREEN_WIDTH * 2 + 8)

static inline void Pixel(const uint32_t* up, const uint32_t* mid, const uint32_t* dn, int w, int x, uint32_t* top,
                         uint32_t* bot) {
    const uint32_t B = up[x], E = mid[x], H = dn[x];
    const uint32_t D = mid[x > 0 ? x - 1 : x], F = mid[x < w - 1 ? x + 1 : x];

    if (B == H || D == F) {
        top[2 * x] = top[2 * x + 1] = bot[2 * x] = bot[2 * x + 1] = E;
    } else {
        top[2 * x] = D == B ? D : E;
        top[2 * x + 1] = B == F ? F : E;
        bot[2 * x] = D == H ? D : E;
        bot[2 * x + 1] = H == F ? F : E;
    }
}

void Scale2xRows(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst, int dstStride,
                 int y0, int y1) {
    uint32_t lines[2 * LINE_MAX] __attribute__((aligned(64)));
    int x, y;

    for (y = y0; y < y1; y++) {
        const uint32_t* up = src + (y > 0 ? y - 1 : y) * w;
        const uint32_t* mid = src + y * w;
        const uint32_t* dn = src + (y < h - 1 ? y + 1 : y) * w;
        uint32_t* top = lines;
        uint32_t* bot = lines + LINE_MAX;

        Pixel(up, mid, dn, w, 0, top, bot);
        /* Four pixels at a time where x-1 .. x+4 are inside the line. */
        for (x = 1; x + 4 < w; x += 4) {
            const uint32x4_t B = vld1q_u32(up + x), H = vld1q_u32(dn + x);
            const uint32x4_t D = vld1q_u32(mid + x - 1), E = vld1q_u32(mid + x), F = vld1q_u32(mid + x + 1);
            /* Lanes with an edge through E: B != H and D != F. */
            const uint32x4_t edge = vbicq_u32(vmvnq_u32(vceqq_u32(B, H)), vceqq_u32(D, F));
            const uint32x4_t e0 = vbslq_u32(vandq_u32(edge, vceqq_u32(D, B)), D, E);
            const uint32x4_t e1 = vbslq_u32(vandq_u32(edge, vceqq_u32(B, F)), F, E);
            const uint32x4_t e2 = vbslq_u32(vandq_u32(edge, vceqq_u32(D, H)), D, E);
            const uint32x4_t e3 = vbslq_u32(vandq_u32(edge, vceqq_u32(H, F)), F, E);
            uint32x4x2_t t, b;

            t.val[0] = e0;
            t.val[1] = e1;
            b.val[0] = e2;
            b.val[1] = e3;
            vst2q_u32(top + 2 * x, t);
            vst2q_u32(bot + 2 * x, b);
        }
        for (; x < w; x++) {
            Pixel(up, mid, dn, w, x, top, bot);
        }
        if (target != UPSCALE_TARGET_ALL) {
            const uint8_t* skipRow = mask + y * w;

            for (x = 0; x < w; x++) {
                if (UPSCALE_SKIP(skipRow, target, x)) {
                    top[2 * x] = top[2 * x + 1] = bot[2 * x] = bot[2 * x + 1] = mid[x];
                }
            }
        }
        {
            uint32_t* out = dst + y * 2 * dstStride;

            /* One more column (and, below the last line, one more line)
             * repeating the edge, as MMPX does: the 2x frame fills only part
             * of the 3x texture and the presenter's filtering reads one
             * texel past its edge. */
            top[2 * w] = top[2 * w - 1];
            bot[2 * w] = bot[2 * w - 1];
            sceClibMemcpy(out, top, (w * 2 + 1) * 4);
            sceClibMemcpy(out + dstStride, bot, (w * 2 + 1) * 4);
            if (y == h - 1) {
                sceClibMemcpy(out + 2 * dstStride, bot, (w * 2 + 1) * 4);
            }
        }
    }
}
