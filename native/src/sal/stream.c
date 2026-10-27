/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Receiver of the phone's instrument-cluster video (AirPlay screen stream type 111). The stock
 * plug-in only knows the main screen (110), so this is its counterpart for the second screen,
 * built from the plug-in's own primitives the way it builds 110: ServerSocketOpen for the
 * listener, AirPlay_DeriveAESKeySHA512ForScreen for the stream key, AES_CTR_* to decrypt.
 * Wire format (the stock plug-in's own type-110 receiver on this unit, mQbEMU research notes):
 * a 128-byte header (u32 body size, u8 opcode, 3 flag bytes, 15 x u64 parameters) and a body;
 * opcode 1 = avcC configuration (clear), 0 = one access unit, encrypted by one AES-CTR stream
 * that runs over all opcode-0 bodies, others are keep-alives. Access units go to the shared
 * memory of include/mcn_video.h as Annex-B; DisplayManager's hook sends them to the cluster.
 * One receiver owns that memory at a time (owner, under lock): one that ends late, after a newer
 * one took over, neither publishes nor clears what its successor shows.
 */
#include "sal.h"
#include "avc.h"
#include "mcn_video.h"
#include "mcn_transcode.h"

#define BODY_MAX (1u << 20)
/* Transcoder input: the largest body as Annex-B, plus the SPS/PPS put in front of an IDR. */
#define TBUF_MAX (BODY_MAX + sizeof(((struct avc *)0)->ps))
#define OP_FRAME 0
#define OP_CONFIG 1
#define KEY_GAP_MS 2000
#define STATS_MS 10000
/* Most the receiver reads at once after an idle spell: about one key frame (up to 87 kB). */
#define BURST_MS 250

struct header {
    uint32_t size;
    uint8_t opcode;
    uint8_t flags[3];
    uint64_t params[15];
};
typedef char header_is_128_bytes[sizeof(struct header) == 128 ? 1 : -1];

/* Who frees a receiver: its owner after the join, or the thread itself if the owner gave up. */
enum { RX_RUNNING, RX_DONE, RX_ORPHAN };

struct rx {
    volatile int stop;
    volatile int state;
    int listen_fd;
    volatile int fd;
    pthread_t thread;
    void *session;
    /* AES_CTR_Context: 0x11c bytes in 210.81 (AES_CTR_Final, support.so 0x15e48, clears 0x11c). */
    unsigned char aes[512] __attribute__((aligned(8)));
    struct avc avc;
    int need_idr;
    unsigned told_ms; /* last "video=1" to the HMI bundle */
    int told;         /* "video=1" was sent and not yet taken back */
    unsigned key_ms;  /* last key frame from the phone, or the last time we asked for one */
    unsigned key_period_ms;
    unsigned asked_ms; /* last forceKeyFrame sent */
    unsigned stats_ms, frames, kbytes, keys, asked;
    unsigned kbps;      /* budget, MCN_CLUSTER_KBPS */
    unsigned budget_ms; /* last refill */
    long budget;        /* bytes that may be read now */
    uint8_t *body;
    struct mcn_geom zoom; /* CarPlay zoom-out: re-encode the phone's bigger frame to 1010x376 */
    unsigned vpu;         /* the transcoder's owner token, 0 when forwarding 1:1 */
    int dead;             /* zoom-out without a transcoder: nothing shown, the cluster is stock */
    int want_key;         /* the transcoder's decoder wants the phone's next key frame */
    uint8_t *tbuf;        /* Annex-B scratch fed to the transcoder, TBUF_MAX */
};

static OSStatus (*p_socket_open)(int, int, int, int, int *, int, int *);
static void (*p_derive)(const void *, size_t, uint64_t, uint8_t *, uint8_t *);
static OSStatus (*p_ctr_init)(void *, const uint8_t *, const uint8_t *);
static OSStatus (*p_ctr_update)(void *, const void *, size_t, void *);
static void (*p_ctr_final)(void *);
static OSStatus (*p_send_command)(void *, CFDictionaryRef, void *, void *);
static OSStatus (*p_set_qos)(int, int);
static struct mcn_video *video;
static struct rx *current;
/* The receiver that owns the shared memory; its writes to it happen under lock. */
static pthread_mutex_t lock;
static struct rx *owner;

void mcn_stream_init(void)
{
    pthread_mutex_init(&lock, NULL);
}

/*
 * The session fields read here (peer family at +0x49, server configuration at +0xc, its QoS
 * byte at +0xa4) are those of plug-in 210.81 (libext.apple.carplay.airplay.so, sha256
 * 390fbaf6...): its exports must sit at their 210.81 offsets, or the cluster is not offered.
 */
static int plugin_210_81(void)
{
    static const struct {
        const char *name;
        uintptr_t offset;
    } exports[] = {
        { "AirPlayCopyServerInfo", 0x11c94 },
        { "AirPlayReceiverSessionTearDown", 0x17db8 },
        { "AirPlayReceiverSessionSetup", 0x18738 },
        { "AirPlayReceiverSessionSendCommand", 0x19ce8 },
        { "AirPlayReceiverSessionSetSecurityInfo", 0x1ad0c },
        { "AirPlay_DeriveAESKeySHA512ForScreen", 0x1ed08 },
    };
    static int known = -1;
    uintptr_t base = 0;
    unsigned i;

    for (i = 0; known < 0 && i < sizeof(exports) / sizeof(exports[0]); i++) {
        uintptr_t at = (uintptr_t)cf_sym(AIRPLAY_LIB, exports[i].name);

        if (!at || (i && at - exports[i].offset != base)) {
            mcn_log("CarPlay plug-in is not 210.81: cluster not offered");
            known = 0;
        }
        base = at - exports[i].offset;
    }
    if (known < 0) {
        known = 1;
    }
    return known;
}

static int resolve(void)
{
    if (p_send_command) {
        return 1;
    }
    if (!plugin_210_81()) {
        return 0;
    }
    p_socket_open = cf_sym(SUPPORT_LIB, "ServerSocketOpen");
    p_ctr_init = cf_sym(SUPPORT_LIB, "AES_CTR_Init");
    p_ctr_update = cf_sym(SUPPORT_LIB, "AES_CTR_Update");
    p_ctr_final = cf_sym(SUPPORT_LIB, "AES_CTR_Final");
    p_set_qos = cf_sym(SUPPORT_LIB, "SocketSetQoS");
    p_derive = cf_sym(AIRPLAY_LIB, "AirPlay_DeriveAESKeySHA512ForScreen");
    if (!p_socket_open || !p_set_qos || !p_ctr_init || !p_ctr_update || !p_ctr_final || !p_derive) {
        return 0;
    }
    p_send_command = cf_sym(AIRPLAY_LIB, "AirPlayReceiverSessionSendCommand");
    return p_send_command != NULL;
}

int mcn_stream_ready(void)
{
    return resolve();
}

struct mcn_video *mcn_video_map(void)
{
    int fd;
    void *p;

    if (video) {
        return video;
    }
    fd = shm_open(MCN_VIDEO_SHM, O_RDWR | O_CREAT, 0666);
    if (fd < 0 || ftruncate(fd, sizeof(struct mcn_video)) < 0) {
        mcn_log("cannot create the video shared memory");
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    p = mmap(NULL, sizeof(struct mcn_video), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        mcn_log("cannot map the video shared memory");
        return NULL;
    }
    video = p;
    if (video->magic != MCN_VIDEO_MAGIC || video->version != MCN_VIDEO_VERSION || !video->sem_ok) {
        /* New memory (RAM: every boot). A semaphore a reader may sleep on is never set up again. */
        video->sem_ok = sem_init(&video->frame_ready, 1, 0) == 0;
        if (!video->sem_ok) {
            mcn_log("video: no semaphore, DisplayManager polls");
        }
    }
    video->alive_ms = 0;
    video->width = MCN_CLUSTER_WIDTH;
    video->height = MCN_CLUSTER_HEIGHT;
    video->version = MCN_VIDEO_VERSION;
    __sync_synchronize();
    video->magic = MCN_VIDEO_MAGIC;
    return video;
}

void mcn_video_wake(struct mcn_video *v)
{
    __sync_synchronize();
    if (v->waiting && v->sem_ok) {
        sem_post(&v->frame_ready);
    }
}

/*
 * GEM "Key frame every second / every 2 s" (mcn_set.sh keyframe1/keyframe2) writes 1 or 2 to
 * MCN_KEY_FILE; without it key frames are asked for only when one is needed (0).
 */
static unsigned key_period(void)
{
    char text[4] = { 0 };
    int fd = open(MCN_KEY_FILE, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    read(fd, text, sizeof(text) - 1);
    close(fd);
    return text[0] == '1' ? 1000 : text[0] == '2' ? 2000 : 0;
}

/* Budget of the cluster stream in kbit/s: MCN_RATE_FILE if it holds 1000..25000, else default. */
static unsigned rate_kbps(void)
{
    char text[8] = { 0 };
    unsigned v = 0, i;
    int fd = open(MCN_RATE_FILE, O_RDONLY);

    if (fd < 0) {
        return MCN_CLUSTER_KBPS;
    }
    read(fd, text, sizeof(text) - 1);
    close(fd);
    for (i = 0; text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (unsigned)(text[i] - '0');
    }
    return v >= 1000 && v <= 25000 ? v : MCN_CLUSTER_KBPS;
}

/* Sends {type, params: {uuid: cluster[, url]}} to the phone (the plug-in's command channel). */
static void command(struct rx *rx, const char *type, const char *url)
{
    CFMutableDictionaryRef req = cf_dict_new(), params = cf_dict_new();

    if (req && params) {
        cf_dict_set_str(params, "uuid", MCN_CLUSTER_UUID);
        if (url) {
            cf_dict_set_str(params, "url", url);
        }
        cf_dict_set_str(req, "type", type);
        cf_dict_set(req, "params", params);
        /* forceKeyFrame goes every few seconds: only its refusals are worth the log space. */
        if (p_send_command(rx->session, req, NULL, NULL)) {
            mcn_log2("phone refused ", type);
        } else if (strcmp(type, "forceKeyFrame")) {
            mcn_log2("sent ", type);
        }
    }
    cf_release(params);
    cf_release(req);
}

/*
 * While the phone's connection is open: the connection is alive for DisplayManager (alive_ms),
 * and, once a frame came, for the HMI bundle ("video=1" each second, PhoneLink), frames or not.
 * Not while the mod is switched off or a zoom-out has no transcoder: then the connection is
 * given up at once (alive_ms 0, "video=0") and DisplayManager shows the stock map, while the
 * socket is still read so the plug-in's session goes on undisturbed.
 */
static void connection_tick(struct rx *rx)
{
    unsigned now = mcn_now_ms();

    pthread_mutex_lock(&lock);
    if (owner == rx && !rx->stop) {
        if (mcn_active() && !rx->dead) {
            video->alive_ms = now ? now : 1;
            if (video->write_seq && now - rx->told_ms >= 1000) {
                rx->told_ms = now;
                rx->told = 1;
                mcn_link_send("video=1");
            }
        } else if (rx->told || video->alive_ms) {
            rx->told = 0;
            video->alive_ms = 0;
            mcn_link_send("video=0");
        }
    }
    pthread_mutex_unlock(&lock);
}

/* Waits until fd has data; 0 once the stream is being stopped (TEARDOWN must not hang). */
static int wait_readable(struct rx *rx, int fd)
{
    struct pollfd p;

    while (!rx->stop) {
        if (fd == rx->fd) {
            connection_tick(rx);
        }
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        int r = poll(&p, 1, 250);
        if (r > 0) {
            return 1;
        }
        if (r < 0) {
            usleep(10000);
        }
    }
    return 0;
}

static void refill(struct rx *rx)
{
    unsigned now = mcn_now_ms(), idle = now - rx->budget_ms;
    long cap = (long)rx->kbps / 8 * BURST_MS; /* kbit/s / 8 = bytes a millisecond */

    rx->budget_ms = now;
    rx->budget += (long)(idle < BURST_MS ? idle : BURST_MS) * (long)rx->kbps / 8;
    if (rx->budget > cap) {
        rx->budget = cap;
    }
}

/*
 * Holds the reader to the budget: while it waits, the socket buffer fills, the phone's TCP window
 * closes and the phone skips frames itself. 0 once the stream is being stopped.
 */
static int wait_budget(struct rx *rx)
{
    refill(rx);
    while (rx->budget <= 0 && !rx->stop) {
        connection_tick(rx);
        usleep(10000);
        refill(rx);
    }
    return !rx->stop;
}

static int read_full(struct rx *rx, void *buf, unsigned n)
{
    unsigned got = 0;

    while (got < n && wait_budget(rx) && wait_readable(rx, rx->fd)) {
        ssize_t r = recv(rx->fd, (uint8_t *)buf + got, n - got, 0);
        if (r <= 0) {
            return 0;
        }
        got += (unsigned)r;
        rx->budget -= r;
    }
    return got == n;
}

/* Every STATS_MS: frames, data and key frames of the cluster stream, to see bursts. */
static void stats(struct rx *rx, unsigned size)
{
    unsigned now = mcn_now_ms();
    char line[112];

    if (size) {
        rx->frames++;
        rx->kbytes += (size + 512) / 1024;
    }
    if (!rx->stats_ms) {
        rx->stats_ms = now;
    }
    if (now - rx->stats_ms >= STATS_MS) {
        snprintf(line, sizeof(line),
                 "cluster stream: %u frames, %u kB, %u key frames, %u asked in %u s", rx->frames, rx->kbytes, rx->keys, rx->asked, (now - rx->stats_ms) / 1000);
        mcn_log(line);
        rx->stats_ms = now;
        rx->frames = rx->kbytes = rx->keys = rx->asked = 0;
    }
}

/* Ask the phone for a key frame, at most once a second. */
static void ask_key_frame(struct rx *rx)
{
    unsigned now = mcn_now_ms();

    if (now - rx->key_ms >= 1000) {
        rx->key_ms = now;
        video->want_idr = 1;
    }
}

/* One access unit into the next slot (sequence lock, include/mcn_video.h). */
static void publish(struct rx *rx, const uint8_t *au, unsigned n)
{
    unsigned seq = video->write_seq + 1;
    struct mcn_video_slot *slot = &video->slot[seq % MCN_VIDEO_SLOTS];
    int idr;

    slot->seq = 0;
    __sync_synchronize();
    slot->size = avc_annexb(&rx->avc, au, n, slot->data, MCN_VIDEO_AU_MAX, &idr);
    if (!slot->size) {
        /* Lost or oversized: what follows refers to it, so wait for the next key frame. */
        rx->need_idr = 1;
        ask_key_frame(rx);
        return;
    }
    if (rx->need_idr && !idr) {
        /* The phone sends key frames only on request: a stream may start with P-frames. */
        ask_key_frame(rx);
        return;
    }
    rx->need_idr = 0;
    slot->flags = idr ? MCN_VIDEO_IDR : 0;
    slot->pts_us = (unsigned long long)mcn_now_ms() * 1000u;
    __sync_synchronize();
    slot->seq = seq;
    __sync_synchronize(); /* the slot is complete before the reader can see it as the newest */
    video->write_seq = seq;
    video->last_frame_ms = mcn_now_ms();
    if (idr) {
        rx->keys++;
        rx->key_ms = video->last_frame_ms;
    } else if (rx->key_period_ms && video->last_frame_ms - rx->key_ms >= rx->key_period_ms) {
        /*
         * The phone sends key frames only on request (in a capture of its cluster stream only
         * the first frame was one). Only on request in GEM: after each key frame the phone
         * refines the picture with P-frames of 30-80 KB for a while, about 10 Mbit/s at 30 fps
         * (car, 2026-10-26). Asked for every 2 s, these bursts overflowed DisplayManager's MOST
         * ring ("MostSink: Ring buffer full"), and together with the main screen's on an app
         * switch they stalled the unit's USB (NCM "NTH16 mismatch", iAP2 unread, CarPlay
         * dropped). Lost and oversized frames still ask for one (need_idr, want_idr).
         */
        rx->key_ms = video->last_frame_ms;
        video->want_idr = 1;
    }
    mcn_video_wake(video);
}

/*
 * Zoom-out: the phone's access unit into the transcoder. A unit lost or too big, and every one
 * after it until the next key frame, is not fed (the decoder would show garbage) and the phone
 * is asked for a key frame; so it is when the decoder lost its place. Only the transcoder's
 * encoder answers DisplayManager's want_idr.
 */
static void transcode(struct rx *rx, unsigned size)
{
    int idr;
    unsigned n = avc_annexb(&rx->avc, rx->body, size, rx->tbuf, TBUF_MAX, &idr);

    if (!n || (rx->need_idr && !idr)) {
        rx->need_idr = 1;
        rx->want_key = 1;
        return;
    }
    rx->need_idr = 0;
    if (idr) {
        rx->keys++;
    }
    mcn_vpu_feed(rx->vpu, rx->tbuf, n);
    switch (mcn_vpu_poll(rx->vpu)) {
    case -1:
        rx->dead = 1; /* gave up (logged by the transcoder): the cluster goes back to stock */
        break;
    case 1:
        rx->want_key = 1;
        break;
    default:
        break;
    }
}

/* One decrypted access unit: published (or transcoded) only while this receiver may show it. */
static void frame(struct rx *rx, unsigned size)
{
    pthread_mutex_lock(&lock);
    if (owner != rx || rx->stop || rx->dead || !mcn_active()) {
        rx->need_idr = 1; /* if it is shown again, from a key frame */
    } else if (rx->vpu) {
        transcode(rx, size);
    } else {
        publish(rx, rx->body, size);
    }
    pthread_mutex_unlock(&lock);
}

static void rx_free(struct rx *rx)
{
    if (rx->fd >= 0) {
        close(rx->fd);
    }
    p_ctr_final(rx->aes);
    free(rx->tbuf);
    free(rx->body);
    free(rx);
}

static void receive_stream(struct rx *rx)
{
    struct header h;
    char line[48];

    if (wait_readable(rx, rx->listen_fd)) {
        rx->fd = accept(rx->listen_fd, NULL, NULL);
    }
    close(rx->listen_fd);
    rx->listen_fd = -1;
    if (rx->fd < 0 || rx->stop) {
        mcn_log("cluster stream: no connection");
        return;
    }
    mcn_log("cluster stream: phone connected");
    rx->budget_ms = mcn_now_ms();
    rx->budget = (long)rx->kbps / 8 * BURST_MS;
    snprintf(line, sizeof(line), "cluster stream: budget %u kbit/s", rx->kbps);
    mcn_log(line);
    /* Take the shared memory over: a receiver that ends late leaves it alone from now on. The
     * sequence restarts before the generation changes, so a reader that sees the new
     * generation never sees the old stream's sequence with it. */
    pthread_mutex_lock(&lock);
    owner = rx;
    video->write_seq = 0;
    __sync_synchronize();
    video->generation++;
    pthread_mutex_unlock(&lock);
    mcn_busy(1);
    /*
     * The plug-in has no scene policy for a second screen: ask for the cluster scene now, not
     * after the first frame, in case the phone waits for it before it sends any.
     */
    command(rx, "showUI", mcn_cluster_url());
    /*
     * Zoom-out: the phone was offered a bigger screen (sal.c); re-encode its stream down to
     * 1010x376. Its stream is encoded at that bigger size, which the cluster's decoder does not
     * take, so without a transcoder nothing is shown and the cluster stays stock. The phone
     * starts its stream with a key frame; the transcoder's encoder makes its own key frames on
     * DisplayManager's want_idr, and the phone is asked for one only when the decoder needs it.
     */
    if (rx->zoom.mode == MCN_TC_CARPLAY_ZOOM) {
        rx->tbuf = malloc(TBUF_MAX);
        rx->vpu = rx->tbuf ? mcn_vpu_open(&rx->zoom, video) : 0;
        rx->dead = !rx->vpu;
        mcn_log(rx->vpu ? "cluster stream: zoom-out transcoder started"
                        : "cluster stream: zoom-out transcoder unavailable, cluster stays stock");
    }
    while (read_full(rx, &h, sizeof(h))) {
        mcn_busy_tick();
        if (h.size > BODY_MAX || !read_full(rx, rx->body, h.size)) {
            break;
        }
        if (h.opcode == OP_CONFIG && h.size) {
            if (!avc_config(&rx->avc, rx->body, h.size)) {
                mcn_log("cluster stream: bad avcC");
            }
        } else if (h.opcode == OP_FRAME && h.size) {
            /* Every frame goes through the cipher, kept or not, or the counter drifts. */
            if (p_ctr_update(rx->aes, rx->body, h.size, rx->body)) {
                break;
            }
            frame(rx, h.size);
        }
        /*
         * DisplayManager raises want_idr on every frame while it waits for a key frame; one
         * forceKeyFrame per KEY_GAP_MS is enough, as each key frame starts about a second of
         * refining P-frames that load USB and MOST (car, 2026-10-26). With the transcoder, its
         * encoder answers want_idr, and the phone is asked only when the decoder lost its place
         * (want_key), never on a timer: each request restarts the phone's encoder.
         */
        if (!rx->stop && !rx->dead && (rx->vpu ? rx->want_key : video->want_idr) &&
            mcn_now_ms() - rx->asked_ms >= KEY_GAP_MS) {
            if (rx->vpu) {
                rx->want_key = 0;
            } else {
                video->want_idr = 0;
            }
            rx->asked_ms = mcn_now_ms();
            rx->asked++;
            command(rx, "forceKeyFrame", NULL);
        }
        stats(rx, h.opcode == OP_FRAME ? h.size : 0);
    }
    mcn_log("cluster stream: ended");
    mcn_vpu_close(rx->vpu); /* here, not on the plug-in's thread; a stale token is a no-op */
    pthread_mutex_lock(&lock);
    if (owner == rx) {
        /* Still ours: no newer receiver took over, so the cluster goes back to stock. */
        owner = NULL;
        video->alive_ms = 0;
        video->last_frame_ms = 0;
        mcn_video_wake(video);
        mcn_link_send("video=0");
    }
    pthread_mutex_unlock(&lock);
    mcn_busy(0);
}

static void *receive(void *arg)
{
    struct rx *rx = arg;

    receive_stream(rx);
    if (!__sync_bool_compare_and_swap(&rx->state, RX_RUNNING, RX_DONE)) {
        rx_free(rx); /* the owner stopped waiting for us */
    }
    return NULL;
}

int mcn_stream_live(void)
{
    return current != NULL;
}

int mcn_stream_start(void *session, const uint8_t key[16], uint64_t connection_id, int family)
{
    struct rx *rx;
    uint8_t k[16], iv[16];
    int port = 0;

    mcn_stream_stop();
    if (!resolve() || !mcn_video_map()) {
        return -1;
    }
    rx = malloc(sizeof(*rx));
    if (!rx) {
        return -1;
    }
    memset(rx, 0, sizeof(*rx));
    rx->session = session;
    rx->listen_fd = -1;
    rx->fd = -1;
    rx->need_idr = 1;
    rx->key_period_ms = key_period();
    rx->kbps = rate_kbps();
    {
        int zoom = mcn_cluster_zoom();

        if (zoom > 1000) {
            mcn_geom(MCN_TC_CARPLAY_ZOOM, zoom, &rx->zoom);
        }
    }
    rx->body = malloc(BODY_MAX);
    p_derive(key, 16, connection_id, k, iv);
    if (!rx->body || p_ctr_init(rx->aes, k, iv)) {
        free(rx->body);
        free(rx);
        return -1;
    }
    memset(k, 0, sizeof(k));
    memset(iv, 0, sizeof(iv));
    /*
     * As the stock 110 listener (_ScreenSetup, 210.81): port -6030 there, ours the next one;
     * negative = take any free port if it is busy, so the answered dataPort is what counts.
     */
    if (p_socket_open(family, SOCK_STREAM, IPPROTO_TCP, -MCN_CLUSTER_PORT, &port,
                      -MCN_SOCKET_BUFFER, &rx->listen_fd) ||
        pthread_create(&rx->thread, NULL, receive, rx)) {
        if (rx->listen_fd >= 0) {
            close(rx->listen_fd);
        }
        p_ctr_final(rx->aes);
        free(rx->body);
        free(rx);
        return -1;
    }
    /* Stock skips QoS when the server's configuration turns it off (byte at server +0xa4). */
    if (!*(*(const uint8_t *const *)((const uint8_t *)session + 0xc) + 0xa4)) {
        p_set_qos(rx->listen_fd, MCN_SOCKET_QOS);
    }
    current = rx;
    return port;
}

void mcn_stream_stop(void)
{
    struct rx *rx = current;
    int i;

    if (!rx) {
        return;
    }
    current = NULL;
    rx->stop = 1;
    /*
     * The thread sees stop within 250 ms, unless it is inside a VPU call of the zoom-out
     * transcoder (bounded by the VPU's timeouts, seconds) or a SendCommand, which only queues
     * the request (HTTPClientSendMessage, 210.81) and returns. Called from TEARDOWN on the
     * plug-in's thread, so never wait for it without a limit.
     */
    for (i = 0; i < 300 && rx->state == RX_RUNNING; i++) {
        usleep(10000);
    }
    if (__sync_bool_compare_and_swap(&rx->state, RX_RUNNING, RX_ORPHAN)) {
        pthread_detach(rx->thread);
        mcn_log("cluster stream: receiver still busy, it ends on its own");
        return;
    }
    pthread_join(rx->thread, NULL);
    rx_free(rx);
}
