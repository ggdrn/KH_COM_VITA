/*
 * MMPX, style-preserving pixel art magnification (2x), on the CPU.
 *
 * Morgan McGuire and Mara Gagiu, "MMPX Style-Preserving Pixel Art
 * Magnification", Journal of Computer Graphics Techniques 10(2), 2021: this
 * follows the paper's listings 3 and 4. Unlike Scale3x it never makes new
 * colours and keeps one-pixel outlines and small details; it only rounds
 * stair steps that are part of a longer line or edge.
 *
 * Each source pixel E becomes the 2x2 block J K / L M, from its 3x3
 * neighbourhood A..I and the pixels two away, P (up), Q (left), R (right),
 * S (down); reads past the frame's edges clamp to it.
 *
 * Speed: on a textured scene most pixels have a differing neighbour, so the
 * per-pixel tests decide the cost. Each line is first classified four pixels
 * at a time with NEON (all neighbours equal, the sprite mask, and an exact
 * prefilter: every rule needs one of 16 simple equalities), and only the
 * pixels that pass go through the rules; runs of four that don't are written
 * as 2x2 blocks with vector stores.
 */
#include <arm_neon.h>
#include <psp2/kernel/clib.h>

#include "port.h"
#include "vita_host.h"

#define LINE_MAX (PORT_MAX_SCREEN_WIDTH * 2 + 8)

/* Neighbour reads: the rows y-3..y+3 and the columns x-3..x+3, clamped to
 * the frame once per line / once per call instead of on every read. */
#define PX(dx, dy) (row[(dy) + 3][xi[x + (dx) + 3]])

/* Fast luminance approximation with transparency (listing 3). The frame is
 * opaque, so the alpha weight is the same for every pixel. */
static inline uint32_t Luma(uint32_t c) {
    uint32_t alpha = (c & 0xFF000000u) >> 24;
    return (((c & 0x00FF0000u) >> 16) + ((c & 0x0000FF00u) >> 8) + (c & 0x000000FFu) + 1u) * (256u - alpha);
}

static inline int AllEq2(uint32_t b, uint32_t a0, uint32_t a1) {
    return ((b ^ a0) | (b ^ a1)) == 0u;
}

static inline int AllEq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) {
    return ((b ^ a0) | (b ^ a1) | (b ^ a2)) == 0u;
}

static inline int AllEq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return ((b ^ a0) | (b ^ a1) | (b ^ a2) | (b ^ a3)) == 0u;
}

static inline int AnyEq3(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2) {
    return b == a0 || b == a1 || b == a2;
}

static inline int NoneEq2(uint32_t b, uint32_t a0, uint32_t a1) {
    return b != a0 && b != a1;
}

static inline int NoneEq4(uint32_t b, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    return b != a0 && b != a1 && b != a2 && b != a3;
}

/* J K / L M for the pixel at x of the line whose rows are row[] (the rules
 * of listing 4, for a pixel that passed the classification). */
static void Rules(const uint32_t* const* row, const int* xi, int x, uint32_t* out) {
    const uint32_t A = PX(-1, -1), B = PX(0, -1), C = PX(+1, -1);
    const uint32_t D = PX(-1, 0), E = PX(0, 0), F = PX(+1, 0);
    const uint32_t G = PX(-1, +1), H = PX(0, +1), I = PX(+1, +1);
    uint32_t J = E, K = E, L = E, M = E;

    {
        const uint32_t P = PX(0, -2), S = PX(0, +2);
        const uint32_t Q = PX(-2, 0), R = PX(+2, 0);
        const uint32_t Bl = Luma(B), Dl = Luma(D), El = Luma(E), Fl = Luma(F), Hl = Luma(H);

        /* 1:1 slope rules, extended from EPX */
        if ((D == B && D != H && D != F) && (El >= Dl || E == A) && AnyEq3(E, A, C, G) &&
            (El < Dl || A != D || E != P || E != Q))
            J = D;
        if ((B == F && B != D && B != H) && (El >= Bl || E == C) && AnyEq3(E, A, C, I) &&
            (El < Bl || C != B || E != P || E != R))
            K = B;
        if ((H == D && H != F && H != B) && (El >= Hl || E == G) && AnyEq3(E, A, G, I) &&
            (El < Hl || G != H || E != S || E != Q))
            L = H;
        if ((F == H && F != B && F != D) && (El >= Fl || E == I) && AnyEq3(E, C, G, I) &&
            (El < Fl || I != H || E != R || E != S))
            M = F;

        /* Intersection rules */
        if ((E != F && AllEq4(E, C, I, D, Q) && AllEq2(F, B, H)) && (F != PX(+3, 0)))
            K = M = F;
        if ((E != D && AllEq4(E, A, G, F, R) && AllEq2(D, B, H)) && (D != PX(-3, 0)))
            J = L = D;
        if ((E != H && AllEq4(E, G, I, B, P) && AllEq2(H, D, F)) && (H != PX(0, +3)))
            L = M = H;
        if ((E != B && AllEq4(E, A, C, H, S) && AllEq2(B, D, F)) && (B != PX(0, -3)))
            J = K = B;

        /* Triangle tip rules */
        if (Bl < El && AllEq4(E, G, H, I, S) && NoneEq4(E, A, D, C, F))
            J = K = B;
        if (Hl < El && AllEq4(E, A, B, C, P) && NoneEq4(E, D, G, I, F))
            L = M = H;
        if (Fl < El && AllEq4(E, A, D, G, Q) && NoneEq4(E, B, C, I, H))
            K = M = F;
        if (Dl < El && AllEq4(E, C, F, I, R) && NoneEq4(E, B, A, G, H))
            J = L = D;

        /* 2:1 edge rules */
        if (H != B) {
            if (H != A && H != E && H != C) {
                if (AllEq3(H, G, F, R) && NoneEq2(H, D, PX(+2, -1)))
                    L = M;
                if (AllEq3(H, I, D, Q) && NoneEq2(H, F, PX(-2, -1)))
                    M = L;
            }
            if (B != I && B != G && B != E) {
                if (AllEq3(B, A, F, R) && NoneEq2(B, D, PX(+2, +1)))
                    J = K;
                if (AllEq3(B, C, D, Q) && NoneEq2(B, F, PX(-2, +1)))
                    K = J;
            }
        }
        if (F != D) {
            if (D != I && D != E && D != C) {
                if (AllEq3(D, A, H, S) && NoneEq2(D, B, PX(+1, +2)))
                    J = L;
                if (AllEq3(D, G, B, P) && NoneEq2(D, H, PX(+1, -2)))
                    L = J;
            }
            if (F != E && F != A && F != G) {
                if (AllEq3(F, C, H, S) && NoneEq2(F, B, PX(-1, +2)))
                    K = M;
                if (AllEq3(F, I, B, P) && NoneEq2(F, H, PX(-1, -2)))
                    M = K;
            }
        }
    }
    out[0] = J;
    out[1] = K;
    out[2] = L;
    out[3] = M;
}

/* The scalar classification of one pixel (the line's ends). */
static int NeedsRules(const uint32_t* up, const uint32_t* mid, const uint32_t* dn, const int* xi, int x) {
    const uint32_t A = up[xi[x + 2]], B = up[xi[x + 3]], C = up[xi[x + 4]];
    const uint32_t D = mid[xi[x + 2]], E = mid[xi[x + 3]], F = mid[xi[x + 4]];
    const uint32_t G = dn[xi[x + 2]], H = dn[xi[x + 3]], I = dn[xi[x + 4]];

    return ((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0u &&
           (D == B || B == F || H == D || F == H || (E == C && E == I) || (E == A && E == G) ||
            (E == G && E == I) || (E == A && E == C) || (H == G && H == F) || (H == I && H == D) ||
            (B == A && B == F) || (B == C && B == D) || (D == A && D == H) || (D == G && D == B) ||
            (F == C && F == H) || (F == I && F == B));
}

/* Classifies the line: work[x] = 1 where the pixel goes through the rules. */
static void Classify(const uint32_t* up, const uint32_t* mid, const uint32_t* dn, const int* xi, int w,
                     const uint8_t* skipRow, int target, uint8_t* work) {
    int x;

    work[0] = (uint8_t)NeedsRules(up, mid, dn, xi, 0);
    /* Four pixels at a time where x-1 .. x+4 are inside the line. */
    for (x = 1; x + 4 < w; x += 4) {
        const uint32x4_t A = vld1q_u32(up + x - 1), B = vld1q_u32(up + x), C = vld1q_u32(up + x + 1);
        const uint32x4_t D = vld1q_u32(mid + x - 1), E = vld1q_u32(mid + x), F = vld1q_u32(mid + x + 1);
        const uint32x4_t G = vld1q_u32(dn + x - 1), H = vld1q_u32(dn + x), I = vld1q_u32(dn + x + 1);
        const uint32x4_t eA = vceqq_u32(E, A), eB = vceqq_u32(E, B), eC = vceqq_u32(E, C), eD = vceqq_u32(E, D);
        const uint32x4_t eF = vceqq_u32(E, F), eG = vceqq_u32(E, G), eH = vceqq_u32(E, H), eI = vceqq_u32(E, I);
        const uint32x4_t db = vceqq_u32(D, B), bf = vceqq_u32(B, F), hd = vceqq_u32(H, D), fh = vceqq_u32(F, H);
        uint32x4_t uni = vandq_u32(vandq_u32(vandq_u32(eA, eB), vandq_u32(eC, eD)),
                                   vandq_u32(vandq_u32(eF, eG), vandq_u32(eH, eI)));
        uint32x4_t pre = vorrq_u32(vorrq_u32(db, bf), vorrq_u32(hd, fh));
        uint16x4_t n16;
        uint8x8_t n8;

        pre = vorrq_u32(pre, vandq_u32(eC, eI));
        pre = vorrq_u32(pre, vandq_u32(eA, eG));
        pre = vorrq_u32(pre, vandq_u32(eG, eI));
        pre = vorrq_u32(pre, vandq_u32(eA, eC));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(H, G), vceqq_u32(H, F)));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(H, I), hd));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(B, A), bf));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(B, C), db));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(D, A), hd));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(D, G), db));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(F, C), fh));
        pre = vorrq_u32(pre, vandq_u32(vceqq_u32(F, I), bf));
        pre = vbicq_u32(pre, uni);
        n16 = vmovn_u32(pre);
        n8 = vmovn_u16(vcombine_u16(n16, n16));
        work[x] = vget_lane_u8(n8, 0) & 1;
        work[x + 1] = vget_lane_u8(n8, 1) & 1;
        work[x + 2] = vget_lane_u8(n8, 2) & 1;
        work[x + 3] = vget_lane_u8(n8, 3) & 1;
    }
    for (; x < w; x++) {
        work[x] = (uint8_t)NeedsRules(up, mid, dn, xi, x);
    }
    if (skipRow != NULL) {
        for (x = 0; x < w; x++) {
            if (UPSCALE_SKIP(skipRow, target, x)) {
                work[x] = 0;
            }
        }
    }
}

void Mmpx2xRows(const uint32_t* src, const uint8_t* mask, int target, int w, int h, uint32_t* dst, int dstStride,
                int y0, int y1) {
    uint32_t lines[2 * LINE_MAX] __attribute__((aligned(64)));
    uint8_t work[PORT_MAX_SCREEN_WIDTH + 4];
    int xi[PORT_MAX_SCREEN_WIDTH + 6];
    const uint32_t* row[7];
    int x, y, i;

    for (i = 0; i < w + 6; i++) {
        int c = i - 3;
        xi[i] = c < 0 ? 0 : c >= w ? w - 1 : c;
    }
    for (y = y0; y < y1; y++) {
        uint32_t* top = lines;
        uint32_t* bot = lines + LINE_MAX;
        const uint32_t* mid;

        for (i = 0; i < 7; i++) {
            int r = y + i - 3;
            row[i] = src + (r < 0 ? 0 : r >= h ? h - 1 : r) * w;
        }
        mid = row[3];
        Classify(row[2], mid, row[4], xi, w, target != UPSCALE_TARGET_ALL ? mask + y * w : NULL, target, work);

        for (x = 0; x < w;) {
            if (x + 4 <= w && (work[x] | work[x + 1] | work[x + 2] | work[x + 3]) == 0) {
                /* Four pixels as they are: E E for each, on both lines. */
                const uint32x4_t e = vld1q_u32(mid + x);
                const uint32x4x2_t z = vzipq_u32(e, e);

                vst1q_u32(top + 2 * x, z.val[0]);
                vst1q_u32(top + 2 * x + 4, z.val[1]);
                vst1q_u32(bot + 2 * x, z.val[0]);
                vst1q_u32(bot + 2 * x + 4, z.val[1]);
                x += 4;
                continue;
            }
            if (work[x]) {
                uint32_t o[4];

                Rules(row, xi, x, o);
                top[2 * x] = o[0];
                top[2 * x + 1] = o[1];
                bot[2 * x] = o[2];
                bot[2 * x + 1] = o[3];
            } else {
                top[2 * x] = top[2 * x + 1] = bot[2 * x] = bot[2 * x + 1] = mid[x];
            }
            x++;
        }
        {
            uint32_t* out = dst + y * 2 * dstStride;

            /* One more column (and, below the last line, one more line)
             * repeating the edge: the 2x frame fills only part of the 3x
             * texture, and the presenter's filtering reads one texel past
             * its edge. */
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
