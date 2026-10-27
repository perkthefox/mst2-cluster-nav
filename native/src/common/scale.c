/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Pixel helpers of the CarPlay zoom-out mode: the phone draws a view N times bigger than
 * 1010x376 (so it shows more map), and the GC320 shrinks the decoded frame back to the cluster's
 * 1010x376 inside the 1024x384 the encoder reads (sal/scale_gc320.c). mcn_yuy2_to_nv12 repacks
 * the blitter's output on the unit. mcn_scale_nv12 is the host-tested reference of that scale
 * and is not used on the unit: over the uncached decoded frame, about six loads a pixel for
 * 380 000 pixels, it would manage a few frames a second at best on the receiver thread.
 *
 * Y is one byte per pixel; NV12 chroma is CbCr interleaved, one Cb,Cr pair per 2x2 luma block,
 * so the chroma grid is (w/2)x(h/2) and is scaled the same way on each of Cb and Cr.
 */
#include "mcn_transcode.h"

#ifdef MCN_HOST
#include <string.h>
#endif

/* Bilinear sample of a single-channel plane at the 16.16 source position (sx16, sy16). 32-bit
 * only (no 64-bit libcalls in the freestanding build): the fractions are taken to 8 bits, so
 * every product fits in 32 bits. stride is the row stride, step the byte distance between
 * samples (1 for Y, 2 for interleaved chroma). (sw-1)<<16 fits in 32 bits for sw <= 1920. */
static unsigned sample(const uint8_t *p, int stride, int step, int w, int h, int sx16, int sy16)
{
    int x = sx16 >> 16, y = sy16 >> 16;
    unsigned fx = (sx16 >> 8) & 0xff, fy = (sy16 >> 8) & 0xff;
    int x1 = x + 1 < w ? x + 1 : w - 1;
    int y1 = y + 1 < h ? y + 1 : h - 1;
    const uint8_t *r0 = p + (unsigned)y * stride;
    const uint8_t *r1 = p + (unsigned)y1 * stride;
    unsigned a = r0[x * step], b = r0[x1 * step], c = r1[x * step], d = r1[x1 * step];
    unsigned top = a * (256 - fx) + b * fx;   /* <= 255*256, 16-bit */
    unsigned bot = c * (256 - fx) + d * fx;

    return (top * (256 - fy) + bot * fy) >> 16; /* <= 65280*256, 32-bit */
}

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

static int ratio(int s, int d)
{
    return s > 1 && d > 1 ? (int)udiv((unsigned)(s - 1) << 16, (unsigned)(d - 1)) : 0;
}

static void scale_plane(const uint8_t *src, int sstride, int step, int sw, int sh,
                        uint8_t *dst, int dstride, int dw, int dh)
{
    int dx, dy, rx = ratio(sw, dw), ry = ratio(sh, dh);

    for (dy = 0; dy < dh; dy++) {
        int sy16 = dy * ry;
        uint8_t *row = dst + (unsigned)dy * dstride;

        for (dx = 0; dx < dw; dx++) {
            row[dx * step] = (uint8_t)sample(src, sstride, step, sw, sh, dx * rx, sy16);
        }
    }
}

/*
 * Scales an NV12 source (sw x sh, plane offsets inside one buffer at the given strides) down to
 * dw x dh into the destination NV12 planes. The destination chroma plane holds dw/2 x dh/2
 * interleaved Cb,Cr pairs at dst_cstride. All dimensions even.
 */
void mcn_scale_nv12(const uint8_t *src_y, const uint8_t *src_c, int sstride, int sw, int sh,
                    uint8_t *dst_y, uint8_t *dst_c, int dstride, int dw, int dh)
{
    scale_plane(src_y, sstride, 1, sw, sh, dst_y, dstride, dw, dh);
    /* Cb at chroma byte 0, Cr at byte 1; each a (sw/2 x sh/2) plane, step 2. */
    scale_plane(src_c + 0, sstride, 2, sw / 2, sh / 2, dst_c + 0, dstride, dw / 2, dh / 2);
    scale_plane(src_c + 1, sstride, 2, sw / 2, sh / 2, dst_c + 1, dstride, dw / 2, dh / 2);
}

void mcn_yuy2_to_nv12(const uint8_t *yuy2, int ystride, int w, int h, uint8_t *dst_y,
                      uint8_t *dst_c, int dstride)
{
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *p = yuy2 + (unsigned)y * ystride;
        uint8_t *ry = dst_y + (unsigned)y * dstride;
        uint8_t *rc = dst_c + (unsigned)(y / 2) * dstride;

        for (x = 0; x < w; x += 2) {
            ry[x] = p[0];
            ry[x + 1] = p[2];
            if ((y & 1) == 0) {
                rc[x] = p[1];     /* Cb */
                rc[x + 1] = p[3]; /* Cr */
            }
            p += 4;
        }
    }
}
