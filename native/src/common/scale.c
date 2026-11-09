/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * The CarPlay zoom-out downscale: the phone draws a view N times bigger than 1010x376 (so it
 * shows more map), and this shrinks the decoded NV12 frame back to 1010x376 inside the 1024x384
 * the encoder reads (sal/vpu.c). Bilinear, separable: each output row is first blended from two
 * source rows (four bytes per multiply), then sampled across. For ratios below 2 every source
 * pixel weighs into some output pixel, so thin lines and text strokes stay. The GC320 cannot do
 * this through Screen: its gc320 module sets a 1x1 filter kernel on every scaled blit
 * (screen-gc320v0.so sym.blit, gco2D_SetKernelSize(1, 1)), which drops whole rows and columns.
 *
 * Y is one byte per pixel; NV12 chroma is CbCr interleaved, one Cb,Cr pair per 2x2 luma block,
 * so the chroma grid is (w/2)x(h/2) and is scaled the same way on each of Cb and Cr. Each output
 * row is built in the caller's (cached) work buffer and copied out with one memcpy, so the
 * destination may be uncached memory written in bursts.
 */
#include "mcn_transcode.h"

#ifdef MCN_HOST
#include <string.h>
#endif

/* Shift-subtract divide: Cortex-A9 has no hardware divide, and the freestanding build links only
 * the libc names its stub lists, which do not include __aeabi_uidiv (the unit's libc.so.3 does
 * export it), so this stands in for the few per-plane ratio divisions. */
static unsigned udiv(unsigned n, unsigned d)
{
    unsigned q = 0, rem = 0;
    int i;

    if (!d) {
        return 0;
    }
    for (i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((n >> i) & 1);
        q <<= 1;
        if (rem >= d) {
            rem -= d;
            q |= 1;
        }
    }
    return q;
}

/* Source step per destination sample in 16.16, the last sample landing on the last source one.
 * (s-1)<<16 fits in 32 bits for s <= 1920. */
static unsigned ratio(int s, int d)
{
    return s > 1 && d > 1 ? udiv((unsigned)(s - 1) << 16, (unsigned)(d - 1)) : 0;
}

/*
 * out = (a * (256 - f) + b * f + 128) >> 8 for n bytes, four at a time: two 16-bit lanes per
 * mask, each at most 255 * 256 + 128, so no lane carries into the next. n is a multiple of 4.
 */
static void blend(const uint8_t *a, const uint8_t *b, unsigned f, uint8_t *out, int n)
{
    unsigned g = 256 - f;
    int i;

    for (i = 0; i < n; i += 4) {
        uint32_t x, y, lo, hi;

        __builtin_memcpy(&x, a + i, 4);
        __builtin_memcpy(&y, b + i, 4);
        lo = ((x & 0x00ff00ffu) * g + (y & 0x00ff00ffu) * f + 0x00800080u) >> 8;
        hi = ((x >> 8) & 0x00ff00ffu) * g + ((y >> 8) & 0x00ff00ffu) * f + 0x00800080u;
        x = (lo & 0x00ff00ffu) | (hi & 0xff00ff00u);
        __builtin_memcpy(out + i, &x, 4);
    }
}

/* The source row pair for output position sy (16.16) of a plane h rows high, blended into t. */
static void vrow(const uint8_t *plane, int stride, int h, unsigned sy, uint8_t *t, int n)
{
    unsigned y = sy >> 16;
    const uint8_t *a = plane + y * (unsigned)stride;

    blend(a, (int)y + 1 < h ? a + stride : a, (sy >> 8) & 0xff, t, n);
}

/* n samples step bytes apart out of the blended row t (one spare sample past its end). */
static void hrow(const uint8_t *t, int step, uint8_t *out, int n, unsigned rx)
{
    unsigned sx = 0;
    int i;

    for (i = 0; i < n; i++, sx += rx) {
        unsigned x = (sx >> 16) * (unsigned)step, f = (sx >> 8) & 0xff;

        out[i * step] = (uint8_t)((t[x] * (256 - f) + t[x + step] * f + 128) >> 8);
    }
}

void mcn_scale_nv12(const uint8_t *src_y, const uint8_t *src_c, int sstride, int sw, int sh,
                    uint8_t *dst_y, uint8_t *dst_c, int dstride, int dw, int dh, uint8_t *work)
{
    uint8_t *t = work, *out = work + ((sw + 8 + 3) & ~3);
    unsigned rx = ratio(sw, dw), ry = ratio(sh, dh);
    unsigned rcx = ratio(sw / 2, dw / 2), rcy = ratio(sh / 2, dh / 2);
    int i;

    for (i = 0; i < dh; i++) {
        vrow(src_y, sstride, sh, (unsigned)i * ry, t, sw);
        t[sw] = t[sw - 1];
        hrow(t, 1, out, dw, rx);
        memcpy(dst_y + (unsigned)i * dstride, out, (size_t)dw);
    }
    /* Chroma rows are sw bytes too: sw/2 Cb,Cr pairs. */
    for (i = 0; i < dh / 2; i++) {
        vrow(src_c, sstride, sh / 2, (unsigned)i * rcy, t, sw);
        t[sw] = t[sw - 2];
        t[sw + 1] = t[sw - 1];
        hrow(t, 2, out, dw / 2, rcx);         /* Cb */
        hrow(t + 1, 2, out + 1, dw / 2, rcx); /* Cr */
        memcpy(dst_c + (unsigned)i * dstride, out, (size_t)dw);
    }
}
