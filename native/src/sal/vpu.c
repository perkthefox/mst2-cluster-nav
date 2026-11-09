/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * The cluster transcoder: the phone's H.264 (Android Auto 1280x720, or a bigger CarPlay
 * zoom-out frame) decoded on the i.MX6 VPU and re-encoded as the 1024x384 / crop-1010x376 the
 * FPK_E cluster decodes, published into include/mcn_video.h exactly as stream.c does for the
 * CarPlay 1:1 case. The VPU ABI is mcn_vpu_abi.h (recovered from the stock libmxcvpu.so.1 and
 * its MMF plug-ins). For Android Auto the encoder reads a 1024x384 rectangle straight out of
 * the decoded frame (geometry mcn_geom, src_x/src_y aligned), no pixel through the CPU. The
 * CarPlay zoom-out downscales on the CPU between decode and encode (common/scale.c, bilinear),
 * reading the decoded frame through a cached mapping invalidated before each frame, as the stock
 * libOpenMAXAL GetFrame reads a frame the hardware wrote (msync MS_INVALIDATE). The zoom-out
 * encoder runs with rate control at the stock cluster encoder's 5 Mbit/s.
 *
 * One transcoder at a time, owned by the token mcn_vpu_open returned; every call is serialised
 * by one mutex, so a receiver that ends late (stale token) never feeds, publishes into or closes
 * its successor's. SAL does not otherwise use the VPU, so this is the only in-process user; the
 * cross-process VPU mutex (mm-renderer, DisplayManager) is respected by pairing every
 * StartOneFrame with GetOutputInfo. On any failure the transcoder gives up once, cleanly: it
 * publishes nothing more, so the cluster stays stock, never the whole mod.
 */
#include "sal.h"
#include "mcn_video.h"
#include "mcn_transcode.h"
#include "mcn_vpu_abi.h"

#define LIB "libmxcvpu.so.1"
#define RING_SIZE   0x200000u
#define SLICE_SIZE  0x80000u
#define PS_SIZE     0x80000u
#define ENC_STREAM  0x90000u   /* 1024 * 384 * 3/2 */
#define MAX_DEC_FB  22
#define MAX_ENC_FB  6
#define HEADER_MAX  256
/* Decodes per fed access unit at most: catches up after a picture that ran short of stream. */
#define DRAIN_MAX   3
/* DecOutputInfo.decodingSuccess: bit 0 decoded, 0x10 the decoder ran short of stream (only with
 * bitstreamMode 1; libmxcvpu vpu_DecGetOutputInfo 0x15158-0x1531c, mQbEMU research notes). */
#define DEC_OK      0x01
#define DEC_SHORT   0x10
/* Zoom-out encoder rate: DisplayManager's own cluster encoder runs at 5 000 000 bit/s VBR
 * (tsd.mibstd2.system.displaymanager parameters at 0x1aefbc, mQbEMU research notes). With a
 * constant QP the transcoder's key frames had no bound, and a frame MostSink cuts loses its
 * bottom on the cluster. */
#define ENC_KBPS    5000
/* Cached aliases are made over physical addresses that fit the 32-bit signed off_t of mmap. */
#define PHYS_MAX    0x80000000u

struct fns {
    pfn_vpu_Init Init;
    pfn_vpu_UnInit UnInit;
    pfn_vpu_WaitForInt WaitForInt;
    pfn_vpu_IsBusy IsBusy;
    pfn_IOGetPhyMem GetPhyMem;
    pfn_IOFreePhyMem FreePhyMem;
    pfn_IOGetVirtMem GetVirtMem;
    pfn_vpu_DecOpen DecOpen;
    pfn_vpu_DecSetEscSeqInit DecSetEsc;
    pfn_vpu_DecGetInitialInfo DecInitInfo;
    pfn_vpu_DecRegisterFrameBuffer DecRegFb;
    pfn_vpu_DecGetBitstreamBuffer DecGetBuf;
    pfn_vpu_DecUpdateBitstreamBuffer DecUpdate;
    pfn_vpu_DecStartOneFrame DecStart;
    pfn_vpu_DecGetOutputInfo DecOut;
    pfn_vpu_DecClrDispFlag DecClr;
    pfn_vpu_DecBitBufferFlush DecFlush;
    pfn_vpu_DecClose DecClose;
    pfn_vpu_EncOpen EncOpen;
    pfn_vpu_EncGetInitialInfo EncInitInfo;
    pfn_vpu_EncRegisterFrameBuffer EncRegFb;
    pfn_vpu_EncGiveCommand EncGive;
    pfn_vpu_EncStartOneFrame EncStart;
    pfn_vpu_EncGetOutputInfo EncOut;
    pfn_vpu_EncClose EncClose;
};

struct vpu {
    struct fns f;
    unsigned token;    /* owner of the open transcoder; 0 = none */
    int dead;          /* gave up: nothing is fed or published until the owner closes */
    int need_idr;      /* the decoder waits for the phone's next IDR */
    int want_key;      /* ask the phone for a key frame (mcn_vpu_poll) */
    struct mcn_video *video;
    struct mcn_geom geom;
    vpu_handle_t dec, enc;
    int inited;        /* vpu_Init done */
    int dec_ready;     /* decoder initial info, frame buffers and encoder up */
    int stride, alignh, dec_fb_n, enc_fb_n;
    vpu_mem_desc ring, slice, ps, stream, scaled; /* scaled: zoom-out NV12 encoder source */
    vpu_mem_desc dec_mem[MAX_DEC_FB], enc_mem[MAX_ENC_FB];
    /* Zoom-out: cached read-only aliases of the decoded frames (0 = read the uncached one). */
    uint8_t *dec_cached[MAX_DEC_FB];
    FrameBuffer dec_fb[MAX_DEC_FB], enc_fb[MAX_ENC_FB];
    uint8_t header[HEADER_MAX];
    unsigned header_len;
    unsigned frames;
    unsigned said;     /* kinds of trouble already logged (note) */
    unsigned stats_ms, fed, decoded, encoded, published_kb;
};

/* Kinds of trouble, each logged once per transcoder: the car log must say which stage stops. */
enum { N_INIT = 1, N_LOST = 2, N_DEC = 4, N_ENC = 8, N_NO_DISPLAY = 16, N_UNCACHED = 32 };

static struct vpu g;
static pthread_mutex_t lock;
static unsigned serial;

/*
 * SAL's environment is left alone. libmxcvpu reads VPU_FW_PATH only to load the VPU firmware
 * when the BIT processor is not running yet (vpu_Init 0x7e98 -> LoadBitCodeTable 0x1792c;
 * default /lib/firmware/vpu, absent on the unit, which has /fw/vpu/vpu_fw_imx6q.bin), and SAL
 * already has VPU_FW_PATH=/fw/vpu: the unit's startup script exports it before it starts the
 * service launcher, which hands its environment on to every service it spawns (mQbEMU research
 * notes). Should vpu_Init fail anyway, the transcoder is just unavailable.
 */
void mcn_vpu_init(void)
{
    pthread_mutex_init(&lock, NULL);
}

static int align16(int v)
{
    return (v + 15) & ~15;
}

/* dlsym names, in the order of struct fns. */
static const char *const names[] = {
    "vpu_Init", "vpu_UnInit", "vpu_WaitForInt", "vpu_IsBusy", "IOGetPhyMem", "IOFreePhyMem",
    "IOGetVirtMem", "vpu_DecOpen", "vpu_DecSetEscSeqInit", "vpu_DecGetInitialInfo",
    "vpu_DecRegisterFrameBuffer", "vpu_DecGetBitstreamBuffer", "vpu_DecUpdateBitstreamBuffer",
    "vpu_DecStartOneFrame", "vpu_DecGetOutputInfo", "vpu_DecClrDispFlag",
    "vpu_DecBitBufferFlush", "vpu_DecClose", "vpu_EncOpen", "vpu_EncGetInitialInfo",
    "vpu_EncRegisterFrameBuffer", "vpu_EncGiveCommand", "vpu_EncStartOneFrame",
    "vpu_EncGetOutputInfo", "vpu_EncClose"
};
typedef char names_match_fns[sizeof(names) / sizeof(names[0]) ==
                             sizeof(struct fns) / sizeof(void *) ? 1 : -1];

/* dlsym everything; any missing symbol disables the transcoder. */
static int resolve(void)
{
    void *lib = dlopen(LIB, RTLD_LAZY | RTLD_GLOBAL);
    void **slot = (void **)&g.f;
    unsigned i;

    if (!lib) {
        return 0;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(slot[i] = dlsym(lib, names[i]))) {
            return 0;
        }
    }
    return 1;
}

static int phy_alloc(vpu_mem_desc *d, unsigned size)
{
    d->size = (int)size;
    d->phy_addr = 0;
    if (g.f.GetPhyMem(d) != 0 || !d->phy_addr) {
        return 0;
    }
    g.f.GetVirtMem(d);
    return d->virt_uaddr != 0;
}

static void phy_free(vpu_mem_desc *d)
{
    if (d->phy_addr) {
        g.f.FreePhyMem(d); /* zeroes the descriptor */
    }
}

/* Closes encoder and decoder, frees every block and leaves the VPU (safe cleanup, abi.h 6.). */
static void teardown(void)
{
    EncOutputInfo eo;
    DecOutputInfo dout;
    int i;

    if (g.enc) {
        if (g.f.EncClose(g.enc) == VPU_RET_FRAME_NOT_COMPLETE) {
            memset(&eo, 0, sizeof(eo));
            g.f.EncOut(g.enc, &eo);
            g.f.EncClose(g.enc);
        }
        g.enc = 0;
    }
    if (g.dec) {
        if (g.f.DecClose(g.dec) == VPU_RET_FRAME_NOT_COMPLETE) {
            memset(&dout, 0, sizeof(dout));
            g.f.DecOut(g.dec, &dout);
            g.f.DecClose(g.dec);
        }
        g.dec = 0;
    }
    for (i = 0; i < g.dec_fb_n; i++) {
        if (g.dec_cached[i]) {
            munmap(g.dec_cached[i], (size_t)g.dec_mem[i].size);
            g.dec_cached[i] = 0;
        }
        phy_free(&g.dec_mem[i]);
    }
    for (i = 0; i < g.enc_fb_n; i++) {
        phy_free(&g.enc_mem[i]);
    }
    g.dec_fb_n = g.enc_fb_n = 0;
    phy_free(&g.ring);
    phy_free(&g.slice);
    phy_free(&g.ps);
    phy_free(&g.stream);
    phy_free(&g.scaled);
    if (g.inited) {
        g.f.UnInit();
        g.inited = 0;
    }
    g.dec_ready = 0;
}

static void note(unsigned kind, const char *what, int a, int b)
{
    char line[112];

    if (!(g.said & kind)) {
        g.said |= kind;
        snprintf(line, sizeof(line), "transcode: %s (%d, %d)", what, a, b);
        mcn_log(line);
    }
}

/* Every 10 s while frames come: how far through the transcoder they get. */
static void stats(void)
{
    unsigned now = mcn_now_ms();
    char line[112];

    if (!g.stats_ms) {
        g.stats_ms = now;
    }
    if (now - g.stats_ms >= 10000) {
        snprintf(line, sizeof(line),
                 "transcode: %u fed, %u decoded, %u encoded, %u kB published in 10 s", g.fed,
                 g.decoded, g.encoded, g.published_kb);
        mcn_log(line);
        g.stats_ms = now;
        g.fed = g.decoded = g.encoded = g.published_kb = 0;
    }
}

/* Once per transcoder: the reason goes to the log, the VPU is released, nothing more is shown. */
static void give_up(const char *why)
{
    mcn_log2("transcode: cluster stays stock: ", why);
    teardown();
    g.dead = 1;
}

/*
 * Waits for the current VPU job, then always reads its output info (abi.h pitfalls). The output
 * call releases the VPU to other processes even while it is still busy (libmxcvpu 0x150d8,
 * EncGetOutputInfo has no busy test): a job still running then would collide with the stock
 * decoder's or DisplayManager's next one, so this transcoder gives up instead.
 */
static int wait_done(int enc, void *out)
{
    int i, r, busy;

    g.f.WaitForInt(enc ? 200 : 2000);
    for (i = 0; i < 40 && g.f.IsBusy(); i++) {
        g.f.WaitForInt(50);
    }
    busy = g.f.IsBusy();
    r = enc ? g.f.EncOut(g.enc, (EncOutputInfo *)out) : g.f.DecOut(g.dec, (DecOutputInfo *)out);
    if (busy) {
        give_up(enc ? "encoder job still running" : "decoder job still running");
        return -1;
    }
    return r;
}

/* Bytes in the decoder's ring still to be decoded. */
static unsigned ring_fill(void)
{
    vpu_phys_t rd = 0, wr = 0;
    uint32_t room = 0;

    if (g.f.DecGetBuf(g.dec, &rd, &wr, &room) != 0) {
        return 0;
    }
    return wr >= rd ? wr - rd : RING_SIZE - (rd - wr);
}

/* Copies one access unit into the decoder's ring buffer, wrapping at its end. */
static int feed_ring(const uint8_t *au, unsigned n)
{
    vpu_phys_t rd = 0, wr = 0;
    uint32_t room = 0;
    unsigned off, first;
    uint8_t *base = (uint8_t *)(uintptr_t)g.ring.virt_uaddr;

    if (g.f.DecGetBuf(g.dec, &rd, &wr, &room) != 0 || room < n) {
        return 0;
    }
    off = (unsigned)(wr - g.ring.phy_addr);
    first = RING_SIZE - off < n ? RING_SIZE - off : n;
    memcpy(base + off, au, first);
    if (first < n) {
        memcpy(base, au + first, n - first);
    }
    return g.f.DecUpdate(g.dec, n) == 0;
}

/*
 * The decoder lost its place (a picture failed, or the ring filled up because nothing was
 * decoded): empty the ring, decode again from the phone's next IDR and ask the phone for one.
 * vpu_DecBitBufferFlush takes the VPU mutex, so only between frames (no job pending here).
 */
static void lost(void)
{
    if (!g.dec_ready) {
        /* Before the frame buffers are registered vpu_DecBitBufferFlush does nothing (it
         * returns -10, libmxcvpu 0x168b4): the full ring would stay full for good. */
        give_up("decoder ring full before set-up");
        return;
    }
    note(N_LOST, "decoder lost its place, waiting for a key frame", 0, 0);
    g.f.DecFlush(g.dec);
    g.need_idr = 1;
    g.want_key = 1;
}

/* Annex-B: an IDR slice before the first non-IDR slice (SPS, PPS and SEI may lead). */
static int has_idr(const uint8_t *p, unsigned n)
{
    unsigned i, type;

    for (i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            type = p[i + 3] & 0x1fu;
            if (type == 5) {
                return 1;
            }
            if (type == 1) {
                return 0;
            }
            i += 2;
        }
    }
    return 0;
}

static void layout_nv12(FrameBuffer *fb, vpu_phys_t base, int stride, int alignh, int index)
{
    unsigned luma = (unsigned)stride * (unsigned)alignh;

    fb->strideY = stride;
    fb->strideC = stride / 2;
    fb->myIndex = index;
    fb->bufY = base;
    fb->bufCb = base + luma;
    fb->bufCr = base + luma + luma / 4;
    fb->bufMvCol = base + luma + luma / 2;
}

/*
 * After the first SPS: initial info, decoder and encoder frame buffers, cached SPS+PPS.
 * 1 ready, 0 not enough of the stream yet (called again on the next feed), -1 failed for good:
 * once the initial info is taken a second vpu_DecGetInitialInfo only returns -12, so anything
 * failing after it gives up instead of retrying.
 */
static int bring_up(void)
{
    DecInitialInfo di;
    EncInitialInfo ei;
    EncOpenParam ep;
    DecBufInfo bi;
    EncHeaderParam hp;
    int i, n;

    memset(&di, 0, sizeof(di));
    g.f.DecSetEsc(g.dec, 1);
    n = g.f.DecInitInfo(g.dec, &di);
    g.f.DecSetEsc(g.dec, 0);
    if (n == VPU_RET_CALLED_BEFORE) {
        mcn_log("transcode: decoder init reports called-before");
        return -1;
    }
    /* -1 with no error code: the decoder rolled back and wants more of the stream. Any other
     * failure of SEQ_INIT (error code at +0x7C, libmxcvpu 0x10b64-0x10ba4) is final. */
    if (n == -1 && di.errorcode == 0) {
        note(N_INIT, "decoder needs more stream for its set-up", n, 0);
        return 0;
    }
    if (n != 0 || di.picWidth <= 0 || di.picHeight <= 0) {
        char line[96];

        snprintf(line, sizeof(line), "transcode: decoder set-up failed (%d, error 0x%x, %dx%d)",
                 n, (unsigned)di.errorcode, di.picWidth, di.picHeight);
        mcn_log(line);
        return -1;
    }
    /* The phone must have encoded what it was offered: the rectangle we read must be inside. */
    if (di.picWidth < g.geom.src_x + g.geom.src_w || di.picHeight < g.geom.src_y + g.geom.src_h ||
        di.minFrameBufferCount + 2 > MAX_DEC_FB) {
        char line[96];

        snprintf(line, sizeof(line), "transcode: phone sends %dx%d, %d frames; not usable",
                 di.picWidth, di.picHeight, di.minFrameBufferCount);
        mcn_log(line);
        return -1;
    }
    g.stride = align16(di.picWidth);
    g.alignh = align16(di.picHeight);
    for (i = 0; i < di.minFrameBufferCount + 2; i++) {
        if (!phy_alloc(&g.dec_mem[i], (unsigned)g.stride * (unsigned)g.alignh * 7u / 4u)) {
            return -1;
        }
        g.dec_fb_n = i + 1;
        layout_nv12(&g.dec_fb[i], g.dec_mem[i].phy_addr, g.stride, g.alignh, i);
        if (g.geom.scale && g.dec_mem[i].phy_addr < PHYS_MAX) {
            void *p = mmap(0, (size_t)g.dec_mem[i].size, PROT_READ, MAP_PHYS | MAP_SHARED, -1,
                           (off_t)g.dec_mem[i].phy_addr);

            g.dec_cached[i] = p == MAP_FAILED ? 0 : (uint8_t *)p;
        }
    }
    memset(&bi, 0, sizeof(bi));
    bi.avcSliceBufBase = g.slice.phy_addr;
    bi.avcSliceBufSize = (int)SLICE_SIZE;
    bi.maxMbX = g.stride >> 4;
    bi.maxMbY = g.alignh >> 4;
    bi.maxMbNum = (g.stride * g.alignh) >> 8;
    if (g.f.DecRegFb(g.dec, g.dec_fb, g.dec_fb_n, g.stride, &bi) != 0) {
        return -1;
    }

    memset(&ep, 0, sizeof(ep));
    ep.bitstreamBuffer = g.stream.phy_addr;
    ep.bitstreamBufferSize = (int)ENC_STREAM;
    ep.bitstreamFormat = VPU_STD_AVC;
    ep.picWidth = MCN_TC_CODED_W;
    ep.picHeight = MCN_TC_CODED_H;
    ep.frameRateInfo = MCN_CLUSTER_FPS;
    ep.bitRate = g.geom.scale ? ENC_KBPS : 0; /* 0: constant QP (pr.quantParam) */
    ep.gopSize = 0;             /* key frames only on request (DisplayManager, want_idr) */
    ep.rcIntraQp = -1;
    ep.chromaInterleave = 1;    /* NV12, like the decoded frame */
    ep.avc_deblkFilterOffsetAlpha = 6;
    ep.avc_chromaQpOffset = 10;
    ep.avc_frameCroppingFlag = 1;
    ep.avc_frameCropRight = MCN_TC_CROP_RIGHT;   /* 14 -> visible width 1010 */
    ep.avc_frameCropBottom = MCN_TC_CROP_BOTTOM; /* 8  -> visible height 376 */
    ep.userGamma = 0x6000;
    ep.RcIntervalMode = 1;
    ep.MESearchRange = 3;
    if (g.f.EncOpen(&g.enc, &ep) != 0) {
        g.enc = 0;
        return -1;
    }
    memset(&ei, 0, sizeof(ei));
    if (g.f.EncInitInfo(g.enc, &ei) != 0 || ei.minFrameBufferCount + 2 > MAX_ENC_FB) {
        return -1;
    }
    /* As the stock encoder plug-in: min+2 frames, min registered, the last two subsampling. */
    for (i = 0; i < ei.minFrameBufferCount + 2; i++) {
        if (!phy_alloc(&g.enc_mem[i], ENC_STREAM)) {
            return -1;
        }
        g.enc_fb_n = i + 1;
        layout_nv12(&g.enc_fb[i], g.enc_mem[i].phy_addr, MCN_TC_CODED_W, MCN_TC_CODED_H, i);
    }
    if (g.f.EncRegFb(g.enc, g.enc_fb, ei.minFrameBufferCount, MCN_TC_CODED_W, MCN_TC_CODED_W,
                     g.enc_mem[ei.minFrameBufferCount].phy_addr,
                     g.enc_mem[ei.minFrameBufferCount + 1].phy_addr, NULL) != 0) {
        return -1;
    }
    /* Cache SPS then PPS from the stream buffer start; prepended to every IDR we publish. */
    g.header_len = 0;
    for (i = 0; i <= 1; i++) {
        uint8_t *src = (uint8_t *)(uintptr_t)g.stream.virt_uaddr;

        memset(&hp, 0, sizeof(hp));
        hp.headerType = i;
        if (g.f.EncGive(g.enc, VPU_ENC_PUT_AVC_HEADER, &hp) != 0 || hp.size <= 0 ||
            g.header_len + (unsigned)hp.size > HEADER_MAX) {
            return -1;
        }
        memcpy(g.header + g.header_len, src, (unsigned)hp.size);
        g.header_len += (unsigned)hp.size;
    }
    g.dec_ready = 1;
    return 1;
}

/* One finished cluster access unit into the next shared-memory slot (as stream.c publish). */
static void publish(const uint8_t *au, unsigned n, int idr)
{
    struct mcn_video *v = g.video;
    unsigned seq = v->write_seq + 1;
    struct mcn_video_slot *slot = &v->slot[seq % MCN_VIDEO_SLOTS];
    unsigned total = (idr ? g.header_len : 0) + n;

    if (!total || total > MCN_VIDEO_AU_MAX) {
        v->want_idr = 1;
        return;
    }
    slot->seq = 0;
    __sync_synchronize();
    if (idr) {
        memcpy(slot->data, g.header, g.header_len);
    }
    memcpy(slot->data + (idr ? g.header_len : 0), au, n);
    slot->size = total;
    slot->flags = idr ? MCN_VIDEO_IDR : 0;
    slot->pts_us = (unsigned long long)mcn_now_ms() * 1000u;
    __sync_synchronize();
    slot->seq = seq;
    __sync_synchronize(); /* the slot is complete before the reader can see it as the newest */
    v->write_seq = seq;
    v->last_frame_ms = mcn_now_ms();
    mcn_video_wake(v);
}

/* Encodes the decoded frame fb[k]'s content rectangle and publishes it. */
static void encode_frame(int k)
{
    FrameBuffer src;
    EncParam pr;
    EncOutputInfo eo;
    unsigned luma = (unsigned)g.stride * (unsigned)g.alignh;
    int idr = g.video->want_idr || g.frames == 0, n;

    memset(&src, 0, sizeof(src));
    /* The stock encoder plug-in passes its source with myIndex = the frames it allocated,
     * min+2 (imx6_video_encoder.so 0x1dfc -> ctx+0x28 -> 0x1a84), as enc_fb_n is. */
    src.myIndex = g.enc_fb_n;
    if (g.geom.scale) {
        /* Zoom-out: shrink the whole decoded frame into the 1010x376 top-left of our 1024x384
         * encoder source. The cached alias is invalidated first: the VPU wrote the frame behind
         * the CPU's caches. Without an alias (or if msync fails) the uncached frame is read,
         * correct but slower. ponytail: plain C, about 7M instructions a frame (~7 ms on one
         * A9 core); NEON when the unit's CPU load says so. */
        static uint32_t work[(MCN_SCALE_WORK(1920, MCN_TC_VIS_W) + 3) / 4];
        unsigned dluma = (unsigned)MCN_TC_CODED_W * MCN_TC_CODED_H;
        uint8_t *dst = (uint8_t *)(uintptr_t)g.scaled.virt_uaddr;
        const uint8_t *s = g.dec_cached[k];

        if (!s || msync(g.dec_cached[k], luma * 3u / 2u, MS_INVALIDATE) != 0) {
            note(N_UNCACHED, "zoom-out reads the uncached frame (alias, msync)", s != 0, 0);
            s = (const uint8_t *)(uintptr_t)g.dec_mem[k].virt_uaddr;
        }
        mcn_scale_nv12(s, s + luma, g.stride, g.geom.src_w, g.geom.src_h, dst, dst + dluma,
                       MCN_TC_CODED_W, MCN_TC_VIS_W, MCN_TC_VIS_H, (uint8_t *)work);
        src.strideY = MCN_TC_CODED_W;
        src.strideC = MCN_TC_CODED_W;
        src.bufY = g.scaled.phy_addr;
        src.bufCb = g.scaled.phy_addr + dluma;
    } else {
        /* Android Auto: the content area is already 1024x384; encode that rectangle in place.
         * The source stride is the decoded frame's: vpu_EncStartOneFrame writes the source's
         * own strideY to the stride register for every picture (libmxcvpu 0xcd30-0xcd40),
         * while the 1024 registered (0xb604-0xb610) is the stride of the encoder's own frames. */
        src.strideY = g.stride;
        src.strideC = g.stride;
        src.bufY = g.dec_mem[k].phy_addr + (vpu_phys_t)g.geom.src_y * g.stride + g.geom.src_x;
        src.bufCb = g.dec_mem[k].phy_addr + luma + (vpu_phys_t)(g.geom.src_y / 2) * g.stride +
                    g.geom.src_x;
    }
    src.bufCr = src.bufCb;

    memset(&pr, 0, sizeof(pr));
    pr.sourceFrame = &src;
    pr.forceIPicture = idr;
    pr.quantParam = 24; /* used only while bitRate is 0; low enough for a crisp map */
    pr.enableAutoSkip = 0;
    memset(&eo, 0, sizeof(eo));
    n = g.f.EncStart(g.enc, &pr);
    if (n != 0 || (n = wait_done(1, &eo)) != 0 || eo.bitstreamSize == 0 ||
        eo.bitstreamWrapAround) {
        if (!g.dead) {
            note(N_ENC, "encoder failed (code, bytes)", n, (int)eo.bitstreamSize);
        }
        g.video->want_idr = 1;
        return;
    }
    if (idr) {
        g.video->want_idr = 0;
    }
    publish((const uint8_t *)(uintptr_t)g.stream.virt_uaddr, eo.bitstreamSize, eo.picType == 0);
    g.frames++;
    g.encoded++;
    g.published_kb += (eo.bitstreamSize + 512) / 1024;
}

/*
 * Decodes what the ring holds: one picture per fed access unit normally, a few more when an
 * earlier one ran short of stream (decodingSuccess 0x10) and is complete now, so the delay
 * never grows. vpu_DecStartOneFrame copies a 0x34-byte DecParam from its second argument
 * (libmxcvpu 0x13524-0x13530): always a zeroed one, as the stock decoder plug-in passes.
 */
static void decode(void)
{
    DecParam dp;
    DecOutputInfo out;
    int i, n;

    for (i = 0; i < DRAIN_MAX && !g.dead; i++) {
        memset(&dp, 0, sizeof(dp));
        memset(&out, 0, sizeof(out));
        if ((n = g.f.DecStart(g.dec, &dp)) != 0) {
            note(N_DEC, "decoder start failed", n, 0);
            lost();
            return;
        }
        if ((n = wait_done(0, &out)) != 0) {
            if (!g.dead) {
                note(N_DEC, "decoder output failed", n, 0);
                lost();
            }
            return;
        }
        if (out.indexFrameDecoded >= 0) {
            g.decoded++;
        }
        if (out.indexFrameDisplay >= 0 && out.indexFrameDisplay < g.dec_fb_n) {
            encode_frame(out.indexFrameDisplay);
            if (g.dead) {
                return;
            }
            g.f.DecClr(g.dec, out.indexFrameDisplay);
        }
        if (out.decodingSuccess & DEC_SHORT) {
            return; /* the rest of the picture comes with the next access unit */
        }
        if (!(out.decodingSuccess & DEC_OK)) {
            note(N_DEC, "picture not decoded (success, decoded index)", (int)out.decodingSuccess,
                 out.indexFrameDecoded);
            lost();
            return;
        }
        if (out.indexFrameDisplay < 0 && out.indexFrameDecoded >= 0) {
            note(N_NO_DISPLAY, "decoded but nothing to display (display index)",
                 out.indexFrameDisplay, out.indexFrameDecoded);
        }
        if (out.numOfErrMBs > 0) {
            g.want_key = 1; /* decoded with errors: a key frame clears them */
        }
        if (out.indexFrameDecoded < 0 || ring_fill() < 4) {
            return;
        }
    }
}

static void feed(const uint8_t *au, unsigned n)
{
    int r;

    if (g.need_idr) {
        if (!has_idr(au, n)) {
            g.want_key = 1;
            return;
        }
        g.need_idr = 0;
    }
    if (!feed_ring(au, n)) {
        lost(); /* the decoder is not draining its ring: start again at a key frame */
        return;
    }
    if (!g.dec_ready) {
        r = bring_up();
        if (r < 0) {
            give_up("VPU set-up failed");
            return;
        }
        if (r == 0) {
            return; /* needs more of the stream */
        }
    }
    decode();
}

/* Everything but the decoder's first frames; 1 on success, else the caller tears down. */
static int setup(const struct mcn_geom *geom, struct mcn_video *video)
{
    DecOpenParam dp;

    g.geom = *geom;
    g.video = video;
    g.need_idr = 1;
    if (!resolve()) {
        mcn_log("transcode: libmxcvpu not available");
        return 0;
    }
    if (g.f.Init(0) != 0) {
        mcn_log("transcode: vpu_Init failed");
        return 0;
    }
    g.inited = 1;
    if (!phy_alloc(&g.ring, RING_SIZE) || !phy_alloc(&g.slice, SLICE_SIZE) ||
        !phy_alloc(&g.ps, PS_SIZE) || !phy_alloc(&g.stream, ENC_STREAM) ||
        (geom->scale && !phy_alloc(&g.scaled, ENC_STREAM))) {
        mcn_log("transcode: VPU memory alloc failed");
        return 0;
    }
    memset(&dp, 0, sizeof(dp));
    dp.bitstreamFormat = VPU_STD_AVC;
    dp.bitstreamBuffer = g.ring.phy_addr;
    dp.bitstreamBufferSize = (int)RING_SIZE;
    dp.reorderEnable = 0; /* low latency: show each frame as soon as it decodes */
    dp.chromaInterleave = 1;
    dp.psSaveBuffer = g.ps.phy_addr;
    dp.psSaveBufferSize = (int)PS_SIZE;
    dp.bitstreamMode = 1;
    if (g.f.DecOpen(&g.dec, &dp) != 0) {
        mcn_log("transcode: vpu_DecOpen failed");
        g.dec = 0;
        return 0;
    }
    mcn_log("transcode: VPU ready, waiting for the first key frame");
    return 1;
}

unsigned mcn_vpu_open(const struct mcn_geom *geom, struct mcn_video *video)
{
    unsigned token = 0;

    pthread_mutex_lock(&lock);
    teardown(); /* a previous owner's transcoder: its token goes stale */
    memset(&g, 0, sizeof(g));
    if (setup(geom, video)) {
        if (!++serial) {
            ++serial;
        }
        token = g.token = serial;
    } else {
        teardown();
    }
    pthread_mutex_unlock(&lock);
    return token;
}

void mcn_vpu_feed(unsigned token, const uint8_t *au, unsigned n)
{
    pthread_mutex_lock(&lock);
    if (token && token == g.token && !g.dead && n) {
        g.fed++;
        feed(au, n);
        stats();
    }
    pthread_mutex_unlock(&lock);
}

int mcn_vpu_poll(unsigned token)
{
    int r;

    pthread_mutex_lock(&lock);
    r = !token || token != g.token || g.dead ? -1 : g.want_key;
    if (r > 0) {
        g.want_key = 0;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

void mcn_vpu_close(unsigned token)
{
    pthread_mutex_lock(&lock);
    if (token && token == g.token) {
        teardown();
        g.token = 0;
    }
    pthread_mutex_unlock(&lock);
}
