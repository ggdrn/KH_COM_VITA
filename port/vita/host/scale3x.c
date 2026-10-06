/*
 * Scale3x (EPX), on the CPU: every GBA pixel E becomes 3x3 pixels, taking a
 * neighbour's colour in the corners and edges where two neighbours along a
 * diagonal agree, which turns stair steps into smoother slopes.
 *
 *   A B C      E0 E1 E2
 *   D E F  ->  E3 E4 E5
 *   G H I      E6 E7 E8
 *
 * When B == H or D == F (no diagonal edge through E, most of the picture) all
 * nine are E, so that case is checked first.
 *
 * The output goes to texture memory, which the CPU does not cache: there,
 * every 4-byte store is a slow bus write. So each source line is enlarged
 * into three lines of a small cached buffer, then copied out in one block
 * (sceClibMemcpy writes in large bursts), which is several times faster.
 */
#include <psp2/kernel/clib.h>

#include "port.h"
#include "vita_host.h"

#define LINE_MAX (PORT_MAX_SCREEN_WIDTH * 3)

void Scale3xRows(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst, int dstStride,
                 int y0, int y1) {
    uint32_t lines[3 * LINE_MAX] __attribute__((aligned(64)));
    int dw = w * 3;
    int x, y;

    for (y = y0; y < y1; y++) {
        const uint32_t* up = src + (y > 0 ? y - 1 : y) * w;
        const uint32_t* mid = src + y * w;
        const uint32_t* dn = src + (y < h - 1 ? y + 1 : y) * w;
        uint32_t* o0 = lines;
        uint32_t* o1 = o0 + LINE_MAX;
        uint32_t* o2 = o1 + LINE_MAX;

        for (x = 0; x < w; x++, o0 += 3, o1 += 3, o2 += 3) {
            int xl = x > 0 ? x - 1 : x;
            int xr = x < w - 1 ? x + 1 : x;
            uint32_t B = up[x], D = mid[xl], E = mid[x], F = mid[xr], H = dn[x];

            if (B == H || D == F || UPSCALE_SKIP(mask, target, y * w + x)) {
                o0[0] = o0[1] = o0[2] = E;
                o1[0] = o1[1] = o1[2] = E;
                o2[0] = o2[1] = o2[2] = E;
            } else {
                uint32_t A = up[xl], C = up[xr], G = dn[xl], I = dn[xr];
                int db = D == B, bf = B == F, dh = D == H, hf = H == F;

                o0[0] = db ? D : E;
                o0[1] = (db && E != C) || (bf && E != A) ? B : E;
                o0[2] = bf ? F : E;
                o1[0] = (db && E != G) || (dh && E != A) ? D : E;
                o1[1] = E;
                o1[2] = (bf && E != I) || (hf && E != C) ? F : E;
                o2[0] = dh ? D : E;
                o2[1] = (dh && E != I) || (hf && E != G) ? H : E;
                o2[2] = hf ? F : E;
            }
        }
        {
            uint32_t* out = dst + y * 3 * dstStride;

            sceClibMemcpy(out, lines, dw * 4);
            sceClibMemcpy(out + dstStride, lines + LINE_MAX, dw * 4);
            sceClibMemcpy(out + 2 * dstStride, lines + 2 * LINE_MAX, dw * 4);
        }
    }
}
