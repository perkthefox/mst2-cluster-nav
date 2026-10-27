/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * The CarPlay zoom-out downscale on the i.MX6 GC320 2D core (QNX Screen), so the CPU never
 * reads the decoded frame. The blitter filter-scales the phone's NV12 frame into a YUY2
 * scratch at 1010x376 (gco2D_FilterBlit, filtered, because the source is planar); a short
 * YUY2->NV12 pass (common/scale.c) finishes it into the VPU encoder's source, since the GC320
 * cannot write NV12 (gc320 TranslateDestinationFormat, mQbEMU research notes). Buffers are
 * wrapped over our physically contiguous memory (POINTER/SIZE), which the Screen server maps and
 * the gc320 module hands the hardware directly when contiguous. The property and blit-attribute
 * numbers are QNX 6.5 Screen constants confirmed against the gc320 module. Any Screen failure
 * returns -1 and the transcoder shows nothing: there is no CPU fallback (mcn_transcode.h).
 */
#include "sal.h"
#include "mcn_transcode.h"

#define LIBSCREEN "libscreen.so.1"

/* QNX Screen property ids (screen.h) used here. */
#define SCREEN_PROPERTY_BUFFER_SIZE   5
#define SCREEN_PROPERTY_FORMAT        14
#define SCREEN_PROPERTY_PLANAR_OFFSETS 33
#define SCREEN_PROPERTY_POINTER       34
#define SCREEN_PROPERTY_SIZE          40
#define SCREEN_PROPERTY_STRIDE        44
#define SCREEN_PROPERTY_USAGE         48
/* Pixel formats. */
#define SCREEN_FORMAT_NV12            12
#define SCREEN_FORMAT_YUY2            15
/* Usage: the GPU reads and writes these off-screen buffers. */
#define SCREEN_USAGE_NATIVE          0x08
#define SCREEN_USAGE_READ            0x02
#define SCREEN_USAGE_WRITE           0x04
/* Application context and blit. */
#define SCREEN_APPLICATION_CONTEXT   0
#define SCREEN_BLIT_SOURCE_X         1
#define SCREEN_BLIT_SOURCE_Y         2
#define SCREEN_BLIT_SOURCE_WIDTH     3
#define SCREEN_BLIT_SOURCE_HEIGHT    4
#define SCREEN_BLIT_DESTINATION_X    5
#define SCREEN_BLIT_DESTINATION_Y    6
#define SCREEN_BLIT_DESTINATION_WIDTH 7
#define SCREEN_BLIT_DESTINATION_HEIGHT 8
#define SCREEN_BLIT_END              0
#define SCREEN_WAIT_IDLE             1
#define MAX_SRC_CACHE                24 /* >= the VPU decoder pool (MAX_DEC_FB) */
#define YUY2_ROW_MAX                 4096 /* bytes: 2048 pixels */

typedef int (*ctx_fn)(void **ctx, int type);
typedef int (*buf_fn)(void **buf);
typedef int (*pix_fn)(void **pix, void *ctx);
typedef int (*prop_iv_fn)(void *obj, int pname, const int *pval);
typedef int (*prop_pv_fn)(void *obj, int pname, void **pval);
typedef int (*attach_fn)(void *pix, void *buf);
typedef int (*blit_fn)(void *ctx, void *dst, void *src, const int *attribs);
typedef int (*flush_fn)(void *ctx, int flags);
typedef int (*destroy_fn)(void *obj);

static ctx_fn create_context;
static buf_fn create_buffer;
static pix_fn create_pixmap;
static prop_iv_fn buf_prop_iv;
static prop_pv_fn buf_prop_pv;
static prop_iv_fn pix_prop_iv;
static attach_fn attach_buffer;
static blit_fn blit;
static flush_fn flush_blits;
static destroy_fn destroy_pixmap, destroy_buffer;

static void *src_cache_buf(void *pix);

static int resolved, hw_ok, hw_broken;
static void *ctx;
static void *yuy2_buf, *yuy2_pix;
static uint8_t *yuy2_virt;
static int dw, dh; /* the YUY2 scratch's size, fixed at the first use */
/* One realized Screen pixmap+buffer per distinct decoded-frame physical address. */
static struct {
    uint32_t phys;
    void *buf, *pix;
} src_cache[MAX_SRC_CACHE];
static int src_cache_n;

static int resolve(void)
{
    void *lib = dlopen(LIBSCREEN, RTLD_LAZY | RTLD_GLOBAL);

    if (!lib) {
        return 0;
    }
    create_context = (ctx_fn)dlsym(lib, "screen_create_context");
    create_buffer = (buf_fn)dlsym(lib, "screen_create_buffer");
    create_pixmap = (pix_fn)dlsym(lib, "screen_create_pixmap");
    buf_prop_iv = (prop_iv_fn)dlsym(lib, "screen_set_buffer_property_iv");
    buf_prop_pv = (prop_pv_fn)dlsym(lib, "screen_set_buffer_property_pv");
    pix_prop_iv = (prop_iv_fn)dlsym(lib, "screen_set_pixmap_property_iv");
    attach_buffer = (attach_fn)dlsym(lib, "screen_attach_pixmap_buffer");
    blit = (blit_fn)dlsym(lib, "screen_blit");
    flush_blits = (flush_fn)dlsym(lib, "screen_flush_blits");
    destroy_pixmap = (destroy_fn)dlsym(lib, "screen_destroy_pixmap");
    destroy_buffer = (destroy_fn)dlsym(lib, "screen_destroy_buffer");
    return create_context && create_buffer && create_pixmap && buf_prop_iv && buf_prop_pv &&
           pix_prop_iv && attach_buffer && blit && flush_blits && destroy_pixmap && destroy_buffer;
}

/*
 * Wraps a buffer of the given format over existing memory and realizes it through a pixmap.
 * libscreen 0480: attaching records the buffer in the pixmap, and screen_destroy_pixmap then
 * destroys that buffer too (0x10218-0x10234); an unattached buffer needs its own destroy.
 */
static int wrap_buffer(void **buf, void **pix, int format, uint8_t *virt, int stride, int w,
                       int h, int chroma_off, unsigned size)
{
    /* The libscreen PLANAR_OFFSETS handler reads three ints; NV12 uses the first two (luma 0,
     * chroma), the third is the unused V plane. A 2-element array would read one int past it. */
    int wh[2] = {w, h}, offs[3] = {0, chroma_off, 0};
    int usage = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ | SCREEN_USAGE_WRITE;
    int bytes = (int)size;
    void *p = virt;

    if (create_buffer(buf) != 0) {
        return 0;
    }
    buf_prop_iv(*buf, SCREEN_PROPERTY_FORMAT, &format);
    buf_prop_iv(*buf, SCREEN_PROPERTY_BUFFER_SIZE, wh);
    buf_prop_iv(*buf, SCREEN_PROPERTY_STRIDE, &stride);
    buf_prop_iv(*buf, SCREEN_PROPERTY_PLANAR_OFFSETS, offs);
    buf_prop_iv(*buf, SCREEN_PROPERTY_SIZE, &bytes);
    *pix = 0;
    if (buf_prop_pv(*buf, SCREEN_PROPERTY_POINTER, &p) != 0 || create_pixmap(pix, ctx) != 0) {
        goto fail;
    }
    pix_prop_iv(*pix, SCREEN_PROPERTY_USAGE, &usage);
    pix_prop_iv(*pix, SCREEN_PROPERTY_FORMAT, &format);
    pix_prop_iv(*pix, SCREEN_PROPERTY_BUFFER_SIZE, wh);
    if (attach_buffer(*pix, *buf) == 0) {
        return 1;
    }
fail:
    if (*pix) {
        destroy_pixmap(*pix);
    }
    destroy_buffer(*buf);
    *buf = *pix = 0;
    return 0;
}

/* Lazy GC320 set-up for a dw x dh output; returns 0 if Screen is not usable. */
static int hw_open(int out_w, int out_h)
{
    off_t phys = 0;
    size_t contig = 0;
    unsigned ysize = (unsigned)out_w * out_h * 2u; /* YUY2: 2 bytes per pixel */

    if (hw_ok) {
        return 1;
    }
    if (hw_broken || (!resolved && !(resolved = 1, resolve()))) {
        hw_broken = 1;
        return 0;
    }
    dw = out_w;
    dh = out_h;
    /* Uncached, as the stock encoder plug-in maps memory the hardware writes (mmap64 PROT 0xB00,
     * MAP_PHYS|MAP_ANON, imx6_video_encoder.so 0x1f54): the GC320 writes it behind the CPU's
     * caches, so a cached mapping could serve stale lines of the previous frame. */
    yuy2_virt = mmap(0, ysize, PROT_READ | PROT_WRITE | PROT_NOCACHE, MAP_PHYS | MAP_ANON, -1, 0);
    if (yuy2_virt == MAP_FAILED || create_context(&ctx, SCREEN_APPLICATION_CONTEXT) != 0 ||
        mem_offset(yuy2_virt, -1, ysize, &phys, &contig) != 0 || contig < ysize) {
        hw_broken = 1;
        return 0;
    }
    if (!wrap_buffer(&yuy2_buf, &yuy2_pix, SCREEN_FORMAT_YUY2, yuy2_virt, out_w * 2, out_w, out_h,
                     0, ysize)) {
        hw_broken = 1;
        return 0;
    }
    hw_ok = 1;
    return 1;
}

/*
 * The realized source pixmap for a decoded frame's physical address, created once per address
 * and session (mcn_scale_close clears them). Its size runs to the end of the CbCr plane, which
 * starts at chroma_off (the decoder's 16-aligned height), not at stride * h.
 */
static void *src_pixmap(const struct mcn_plane *src)
{
    int i;
    void *buf = 0, *pix = 0;

    for (i = 0; i < src_cache_n; i++) {
        if (src_cache[i].phys == src->phys) {
            return src_cache[i].pix;
        }
    }
    if (src_cache_n >= MAX_SRC_CACHE ||
        !wrap_buffer(&buf, &pix, SCREEN_FORMAT_NV12, src->virt, src->stride, src->w, src->h,
                     src->chroma_off,
                     (unsigned)src->chroma_off + (unsigned)src->stride * (unsigned)(src->h / 2))) {
        return 0;
    }
    src_cache[src_cache_n].phys = src->phys;
    src_cache[src_cache_n].buf = buf;
    src_cache[src_cache_n].pix = pix;
    src_cache_n++;
    return pix;
}

int mcn_scale(const struct mcn_plane *src, const struct mcn_plane *dst)
{
    int attribs[] = {SCREEN_BLIT_SOURCE_X, 0, SCREEN_BLIT_SOURCE_Y, 0,
                     SCREEN_BLIT_SOURCE_WIDTH, src->w, SCREEN_BLIT_SOURCE_HEIGHT, src->h,
                     SCREEN_BLIT_DESTINATION_X, 0, SCREEN_BLIT_DESTINATION_Y, 0,
                     SCREEN_BLIT_DESTINATION_WIDTH, dst->w, SCREEN_BLIT_DESTINATION_HEIGHT, dst->h,
                     SCREEN_BLIT_END};
    static uint8_t rows[2 * YUY2_ROW_MAX]; /* off the thread's stack; calls are serialised */
    unsigned row = (unsigned)dst->w * 2u;
    void *spix;
    int y;

    if (row > YUY2_ROW_MAX || (dst->h & 1) || !hw_open(dst->w, dst->h) || dst->w != dw ||
        dst->h > dh || (spix = src_pixmap(src)) == 0 ||
        blit(ctx, yuy2_buf, src_cache_buf(spix), attribs) != 0 ||
        flush_blits(ctx, SCREEN_WAIT_IDLE) != 0) {
        return -1;
    }
    /* Two rows at a time out of the uncached scratch with libc memcpy (burst reads), repacked
     * from the stack: byte loads straight from uncached memory would cost a bus access each. */
    for (y = 0; y < dst->h; y += 2) {
        memcpy(rows, yuy2_virt + (unsigned)y * row, 2u * row);
        mcn_yuy2_to_nv12(rows, (int)row, dst->w, 2, dst->virt + (unsigned)y * dst->stride,
                         dst->virt + dst->chroma_off + (unsigned)(y / 2) * dst->stride,
                         dst->stride);
    }
    return 0;
}

/* The buffer handle paired with a cached source pixmap (screen_blit takes buffers). */
static void *src_cache_buf(void *pix)
{
    int i;

    for (i = 0; i < src_cache_n; i++) {
        if (src_cache[i].pix == pix) {
            return src_cache[i].buf;
        }
    }
    return 0;
}

void mcn_scale_close(void)
{
    int i;

    /* Every blit was waited for (SCREEN_WAIT_IDLE), so nothing is in flight. The next session's
     * decoded frames get new buffers, perhaps at the same addresses with another stride. The
     * context and the YUY2 scratch stay for the SAL process. */
    for (i = 0; i < src_cache_n; i++) {
        destroy_pixmap(src_cache[i].pix); /* and its attached buffer */
    }
    src_cache_n = 0;
}
