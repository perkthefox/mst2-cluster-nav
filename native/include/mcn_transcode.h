/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Turning a phone's cluster video into the 1010x376 H.264 the FPK_E cluster decodes.
 *
 * The cluster's VCP3 decoder takes a coded frame no larger than 1024x480 and shows a cropped
 * width of exactly 1010, cropped on the right and bottom only (fpke gc-rivp, decoder research);
 * the stock DisplayManager encoder makes it as 1024x384 coded, cropped {right 14, bottom 8} to
 * 1010x376. CarPlay lets the phone encode exactly that, so stream.c forwards it untouched. Two
 * cases need the head unit to re-encode instead, both producing the same 1024x384/crop-1010x376:
 *   - Android Auto: the phone only encodes 800x480 / 1280x720 / 1920x1080, so we ask for
 *     1280x720 with margins that leave a 1024x384 content area at an aligned origin, decode it
 *     and encode that rectangle 1:1 (no scaling).
 *   - CarPlay zoom-out: to show more map we advertise an alt screen factor N bigger (the phone
 *     then draws a wider view, confirmed in the CarPlay Simulator), decode it and scale down to
 *     1010x376.
 * The decode and encode run on the i.MX6 VPU and the zoom-out scale on the GC320; the CPU only
 * repacks the GC320's YUY2 output into NV12. mcn_geom() is the arithmetic that picks the request
 * size and the source rectangle; it is host-tested. The backend (sal/vpu.c) turns phone access
 * units into 1010x376 ones, published through include/mcn_video.h exactly as stream.c does.
 */
#ifndef MCN_TRANSCODE_H
#define MCN_TRANSCODE_H

#ifdef MCN_HOST
#include <stdint.h>
#else
#include "mcn_qnx.h"
#endif

/* The cluster's coded frame and its cropped, visible picture. */
#define MCN_TC_CODED_W 1024
#define MCN_TC_CODED_H 384
#define MCN_TC_VIS_W   1010
#define MCN_TC_VIS_H   376
#define MCN_TC_CROP_RIGHT  (MCN_TC_CODED_W - MCN_TC_VIS_W) /* 14 */
#define MCN_TC_CROP_BOTTOM (MCN_TC_CODED_H - MCN_TC_VIS_H) /* 8 */

enum mcn_tc_mode {
    MCN_TC_OFF = 0,        /* no transcode: CarPlay 1:1, stream.c forwards the phone's frames */
    MCN_TC_AA,             /* Android Auto: crop a 1:1 content area out of a 1280x720 frame */
    MCN_TC_CARPLAY_ZOOM    /* CarPlay zoom-out: scale an N-times bigger frame down to 1010x376 */
};

/*
 * What to ask the phone for and how to turn its frame into 1010x376. Pixel offsets and sizes
 * meant for the VPU are multiples of 16 across and 2 down (chroma); the advertised screen keeps
 * the 1010:376 aspect so the phone does not letterbox on its own.
 */
struct mcn_geom {
    enum mcn_tc_mode mode;
    int req_w, req_h;          /* alt screen / video size advertised to the phone */
    int margin_w, margin_h;    /* Android Auto width/height margin (0 for zoom) */
    int src_x, src_y;          /* top-left of the content rectangle in the decoded frame */
    int src_w, src_h;          /* its size: equal to the coded target, or scaled down to it */
    int scale;                 /* 1 if src must be scaled to 1024x384, 0 if copied 1:1 */
};

/*
 * Fills g for the mode. factor_milli is the zoom ratio x1000 for MCN_TC_CARPLAY_ZOOM
 * (1000 = 1:1, 1500 = 1.5x, clamped to 1000..1900 for the i.MX6 VPU's ~1920 width). Returns 0
 * on success, -1 for an unknown mode.
 */
int mcn_geom(enum mcn_tc_mode mode, int factor_milli, struct mcn_geom *g);

/*
 * Appends the cluster's video and input Service entries for Android Auto to the
 * ServiceDiscoveryResponse (raw protobuf, mcn_aa_sdr.c), numbered video_id and input_id.
 * Returns the byte count, or 0 if g is not MCN_TC_AA or it does not fit.
 */
int mcn_aa_sdr(uint8_t *out, unsigned cap, int video_id, int input_id, const struct mcn_geom *g);

/*
 * The VPU transcoder (sal/vpu.c): decode the phone's H.264 and re-encode the 1010x376 the
 * cluster takes, publishing into video. mcn_vpu_open returns the owner token of a fresh
 * transcoder (non-zero), or 0 when the VPU is not ready (the caller then publishes nothing: the
 * cluster stays stock, never the mod). Only the current token's holder feeds or closes; a stale
 * token (its transcoder already closed or replaced) is a no-op, so a receiver that ends late
 * never touches its successor's. The calls are serialised inside vpu.c.
 */
struct mcn_video;
unsigned mcn_vpu_open(const struct mcn_geom *geom, struct mcn_video *video);
void mcn_vpu_feed(unsigned token, const uint8_t *au, unsigned n);
void mcn_vpu_close(unsigned token);
/*
 * After a feed: 1 when the decoder lost its reference and the phone should send a key frame
 * (cleared by the call; the transcoder already waits for the next IDR), -1 when this transcoder
 * gave up or the token is stale (publish nothing, stop claiming the cluster), 0 otherwise.
 */
int mcn_vpu_poll(unsigned token);

/*
 * Android Auto cluster video (sal/gal_video.c): offer the cluster screen from the SAL discovery
 * hook (gal.c), drive focus and the transcoder from the SAL worker loop (iap2.c). Offered while
 * the mod is active and the cluster view is not "native arrows only".
 */
void mcn_gal_video_offer(void *router, void *response);
void mcn_gal_video_tick(void);
void mcn_gal_video_stop(void);

/*
 * Bilinear NV12 downscale (common/scale.c): src_y/src_c are the Y and interleaved-CbCr planes of
 * one frame at sstride; it writes dw x dh into dst_y/dst_c at dstride. All dimensions even.
 * Host-tested reference of the zoom-out scale; the unit scales on the GC320 only (mcn_scale).
 */
void mcn_scale_nv12(const uint8_t *src_y, const uint8_t *src_c, int sstride, int sw, int sh,
                    uint8_t *dst_y, uint8_t *dst_c, int dstride, int dw, int dh);

/* YUY2 (the GC320 filter-blit output) to NV12 (the VPU encoder source). Host-tested. */
void mcn_yuy2_to_nv12(const uint8_t *yuy2, int ystride, int w, int h, uint8_t *dst_y,
                      uint8_t *dst_c, int dstride);

/*
 * One NV12 buffer described for the GC320 downscale (sal/scale_gc320.c): its physical base (to
 * hand the 2D blitter) and mapped base (for the CPU finish and fallback), the byte offset of the
 * CbCr plane from the base, the row stride and the picture size.
 */
struct mcn_plane {
    uint32_t phys;
    uint8_t *virt;
    int chroma_off;
    int stride, w, h;
};
/*
 * Downscales the NV12 src to the NV12 dst (dst smaller) for the CarPlay zoom-out. The GC320 2D
 * core does the filtered scale on hardware; a short YUY2->NV12 pass finishes it (the blitter
 * cannot write NV12). Returns 0 on success, -1 if the GC320 path is not available: there is no
 * CPU fallback (a bilinear pass over the uncached decoded frame on the receiver thread would
 * reach only a few frames a second), the caller then shows nothing. mcn_scale_close, after the
 * last blit, releases this session's source pixmaps.
 */
int mcn_scale(const struct mcn_plane *src, const struct mcn_plane *dst);
void mcn_scale_close(void);

#endif
