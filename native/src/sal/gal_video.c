/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Android Auto's map video on the instrument cluster. This version of the receiver library
 * (libext.google.gal.receiver.so, AAP 1.3) has no cluster display in its protobuf, so stock SAL
 * never offers Android Auto a cluster screen. From our hook of MessageRouter::populateService-
 * DiscoveryResponse (gal.c) we register a second, stock-shaped VideoSink endpoint and an input
 * stub on free service ids, then append raw Service entries (mcn_aa_sdr: a cluster video
 * MediaSinkService and, if an id is free, an input service) to the response's unknown-field
 * string before it is serialised (aa-video research: resp+4 is a COW std::string written raw at
 * the tail by ServiceDiscoveryResponse::SerializeWithCachedSizes 0xc57ac and counted by ByteSize
 * 0x10e274; populate runs before marshallProto, so the bytes become trailing top-level Service
 * fields). The phone then opens the video channel on our id; the library's own VideoSink drives
 * our callbacks, whose frames go to the VPU transcoder (sal/vpu.c) and out to the cluster like
 * CarPlay. Android Auto's frame is 1280x720 with margins leaving a 1024x384 content area, so no
 * scaling (mcn_geom). Each frame is acknowledged as stock SAL does (MediaSinkBase::ackFrames
 * (session, 1) after every frame, SAL 0x29b2b4, ev_98a136fb): the sink allows one unacknowledged
 * frame and the library's VideoSink::handleDataAvailable (0x83fe4) acks nothing (ev_8d827a0a).
 *
 * Offered while the mod is active and the cluster view is not "native arrows only". Object
 * layout and callback ABI are taken from the library (evidence ids below). The crash guard
 * (mcn_busy) is held from the registration or the channel's opening
 * until the channel closes or the session ends (GalReceiver::shutdown, gal.c), so repeated
 * faults turn the SAL hooks off.
 *
 * Threads: the library's callbacks (its receive thread), the discovery and shutdown hooks
 * (SAL's threads), mcn_gal_video_tick (the route worker) and our feeder share the state below
 * under one lock; no library or transcoder call is made while it is held (the library holds its
 * sink mutex while it calls on_setup). The receive thread only copies each access unit into a
 * small queue; the feeder hands it to the transcoder (which may take long) and then acks it, as
 * stock SAL does: its frame task (0x29b244) pushes the frame to the player and only then calls
 * ackFrames (ev_44af209e, ev_98a136fb), so the phone is paced by the decoder, not by USB. The
 * transcoder's token makes a feed after its close a no-op.
 *
 * Stream format: Annex-B is expected for the codec configuration (SPS/PPS, sent once per
 * stream) and the access units, as the transcoder takes it; the receiver passes both through
 * untouched (handleCodecConfig 0x84048, handleDataAvailable 0x83fe4), so this is the phone's
 * format and is not proven by the firmware: a configuration that is not Annex-B is logged and
 * skipped, and the cluster then stays stock. The transcoder drops
 * everything before an IDR, so the configuration is put in front of every IDR (as CarPlay's
 * avc_annexb does), never fed alone. This receiver (AAP 1.3) has no key-frame request: its media
 * messages are 0, 1 and 0x8000..0x8008 only (MediaMessageId_IsValid 0x9173c, ev_3387ab5d) and it
 * has no key-frame symbol (ev_46fe684f), so a decoder that lost its reference waits for the
 * phone's next IDR.
 */
#include "sal.h"
#include "mcn_video.h"
#include "mcn_transcode.h"

#define GAL_LIB "libext.google.gal.receiver.so"
#define STDCPP "libstdc++.so.6"
#define VS_VTABLE "_ZTV9VideoSink"
#define PEB_VTABLE "_ZTV20ProtocolEndpointBase"
#define ADD_CONFIG "_ZN9VideoSink25addSupportedConfigurationEiiiiiii"
#define SET_FOCUS "_ZN9VideoSink13setVideoFocusEib"
#define ACK_FRAMES "_ZN13MediaSinkBase9ackFramesEij" /* (int session, unsigned) 0x8b0a8 */
#define REGISTER "_ZN13MessageRouter15registerServiceEP20ProtocolEndpointBase"
#define QUEUE_OUT "_ZN13MessageRouter13queueOutgoingEhPvj"
#define STR_APPEND "_ZNSs6appendEPKcj"
#define MUTEX_CTOR "_ZN5MutexC1Ev"

/* Queue between the receive thread and the feeder. With one unacknowledged frame allowed and
 * the ack sent after the feed, more than two are never waiting; the rest is slack. */
#define QUEUE_SLOTS     4u
#define CONFIG_MAX      512u
#define SLOT_BYTES      (MCN_VIDEO_AU_MAX + CONFIG_MAX)
#define FEED_IDLE_US    5000 /* ponytail: feeder polls an empty queue; a semaphore if it shows */
/*
 * The cluster is the phone's while the video channel is open, frames or not (a still map sends
 * none), as with CarPlay. ponytail: if neither the channel close nor the session end is ever
 * seen, the cluster would keep the last frame; after this long without a frame it goes back to
 * stock and returns with the next frame. Raise it if a parked map flips back too soon.
 */
#define AA_STILL_MS 60000

/* VideoSink object, laid out as SAL's inline constructor does (SAL 0x297dfc: new(0x54)). */
#define VS_SIZE      0x54
#define OFF_OPEN     0x04
#define OFF_CHANNEL  0x05
#define OFF_ROUTER   0x08
#define OFF_ID       0x0c
#define OFF_RAW      0x10
#define OFF_CODEC    0x14
#define OFF_SESSION  0x18
#define OFF_UNACKED  0x1c
#define OFF_AUTOFOCUS 0x2c
#define OFF_REFCOUNT 0x30
#define OFF_CALLBACKS 0x34
#define OFF_SETUPDONE 0x48
#define OFF_MUTEX    0x4c
#define VS_VT_SLOTS  18 /* 2 header words + 16 virtual functions (0x15e7c8, ev_0db6d72b) */
#define PEB_VT_SLOTS 10 /* 2 header words + 8 virtual functions */
#define SLOT_CLOSED  4   /* vtable word for onChannelClosed (0x747a4) */
#define SLOT_OPENED  6   /* vtable word for onChannelOpened (0x74788) */
#define SLOT_DISCOVERY 9 /* vtable word for addDiscoveryInfo (header 2 + slot 7) */
#define SLOT_ROUTE   7   /* vtable word for routeMessage (header 2 + slot 5) */

typedef int (*add_config_fn)(void *vs, int res, int fps, int a3, int a4, int a5, int a6, int a7);
typedef int (*focus_fn)(void *vs, int mode, int unsolicited);
typedef int (*ack_fn)(void *sink, int session, unsigned count);
typedef int (*register_fn)(void *router, void *endpoint);
typedef int (*queue_fn)(void *router, unsigned char channel, void *data, unsigned len);
typedef void *(*append_fn)(void *str, const char *bytes, unsigned len);
typedef void (*mutex_ctor_fn)(void *mutex);
typedef int (*closed_fn)(void *self, unsigned channel);
typedef void (*opened_fn)(void *self, unsigned channel);

static add_config_fn add_config;
static focus_fn set_focus;
static ack_fn ack_frames;
static register_fn register_service;
static queue_fn queue_out;
static append_fn str_append;
static mutex_ctor_fn mutex_ctor;
static closed_fn stock_closed;
static opened_fn stock_opened;

static uint8_t vobj[VS_SIZE];
static uint8_t iobj[0x20];
static void *vs_vtable[VS_VT_SLOTS];
static void *in_vtable[PEB_VT_SLOTS];
static void *vcb_vtable[10];
static void *vcb_obj[1];
static int refcount = 0x40000000;

static int resolved, broken;
static struct mcn_geom geom;

/* Under vlock. */
static volatile unsigned vlock_word;
static int active;          /* our endpoints are registered with a live router */
static int busy_held;
static unsigned token;      /* the transcoder's owner token, 0 = none */
static unsigned last_data_ms, told_ms;
static int told;            /* "video=1" was sent */
static int want_focus;      /* setup or a focus request asks for (more) video focus */
static int started;         /* transcoder started for this channel opening */
static int lost;            /* the transcoder gave up: the cluster goes back to stock */
static uint8_t config[CONFIG_MAX];
static unsigned config_len;
static struct au {
    uint8_t *data;
    unsigned n;
    int session;
} queue[QUEUE_SLOTS];
static unsigned q_head, q_count;
static int q_need_idr;      /* a unit was dropped: drop all up to the next IDR */
static int feeding;         /* the feeder holds the head of the queue */
static int feeder_up;

static void vlock(void)
{
    while (__sync_lock_test_and_set(&vlock_word, 1)) {
        usleep(1000);
    }
}

static void vunlock(void)
{
    __sync_lock_release(&vlock_word);
}

/* Under vlock: the crash guard, taken and given back in pairs. */
static void hold(int on)
{
    if (busy_held != on) {
        busy_held = on;
        mcn_busy(on);
    }
}

static void wr32(uint8_t *o, unsigned off, uint32_t v)
{
    memcpy(o + off, &v, 4);
}

static uint32_t rd32(const uint8_t *o, unsigned off)
{
    uint32_t v;

    memcpy(&v, o + off, 4);
    return v;
}

static void none(void)
{
}

/* Type of the first slice NAL in an Annex-B unit (5 IDR, 1 non-IDR, 0 none); *sps if an SPS
 * comes before it. */
static int first_slice(const uint8_t *p, unsigned n, int *sps)
{
    unsigned i, type;

    *sps = 0;
    for (i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            type = p[i + 3] & 0x1fu;
            if (type == 5 || type == 1) {
                return (int)type;
            }
            *sps |= type == 7;
            i += 2;
        }
    }
    return 0;
}

static int annexb(const uint8_t *p, unsigned n)
{
    return n >= 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1));
}

/* Under vlock: drops queued units (not the one being fed) from the oldest until an IDR leads;
 * returns how many, to be acked. */
static unsigned drop_to_idr(void)
{
    unsigned keep = feeding ? 1u : 0u, dropped = 0, i;
    int sps;

    while (q_count > keep) {
        i = (q_head + keep) % QUEUE_SLOTS;
        if (first_slice(queue[i].data, queue[i].n, &sps) == 5) {
            break;
        }
        /* remove slot i: shift the later ones down */
        for (; i != (q_head + q_count - 1) % QUEUE_SLOTS; i = (i + 1) % QUEUE_SLOTS) {
            struct au t = queue[i];

            queue[i] = queue[(i + 1) % QUEUE_SLOTS];
            queue[(i + 1) % QUEUE_SLOTS] = t;
        }
        q_count--;
        dropped++;
    }
    return dropped;
}

/* Under vlock: queues one access unit for the feeder, the codec configuration in front of an
 * IDR that does not carry its own; 0 if it was dropped (the caller acks it). */
static int enqueue(const uint8_t *d, unsigned len, int session)
{
    int sps, slice = first_slice(d, len, &sps);
    unsigned pre = slice == 5 && !sps ? config_len : 0, dropped = 0;
    struct au *a;

    if (q_count == QUEUE_SLOTS) {
        dropped = drop_to_idr();
        q_need_idr = 1;
    }
    if (dropped) {
        vunlock();
        ack_frames(vobj, session, dropped);
        vlock();
    }
    if (q_count == QUEUE_SLOTS || pre + len > SLOT_BYTES || (q_need_idr && slice != 5)) {
        q_need_idr = 1; /* what follows refers to it: wait for the next key frame */
        return 0;
    }
    q_need_idr = 0;
    a = &queue[(q_head + q_count) % QUEUE_SLOTS];
    memcpy(a->data, config, pre);
    memcpy(a->data + pre, d, len);
    a->n = pre + len;
    a->session = session;
    q_count++;
    return 1;
}

/* The library's VideoSink calls this once per access unit, the frame after a 10-byte header
 * (callbacks slot 3: session, u64 pts in r2:r3, then the shared buffer, data and length;
 * 0x83fe4, ev_8d827a0a). Only copies: the feeder transcodes and acks; a frame not queued is
 * acked here, or the phone would stop sending. */
static void on_data(void *cb, int session, uint64_t ts, void *sp, const uint8_t *d, unsigned len)
{
    int queued = 0;

    (void)cb;
    (void)ts;
    (void)sp;
    if (d && len) {
        vlock();
        last_data_ms = mcn_now_ms();
        if (!last_data_ms) {
            last_data_ms = 1;
        }
        if (token && queue[0].data) {
            queued = enqueue(d, len, session);
        }
        vunlock();
    }
    if (!queued) {
        ack_frames(vobj, session, 1); /* no sink mutex in ackFrames (0x8b0a8) */
    }
}

/* SPS/PPS, once per stream: kept for the IDRs (never fed alone: the transcoder would drop it). */
static void on_codec_config(void *cb, const uint8_t *d, unsigned len)
{
    int bad = 0;

    (void)cb;
    if (!d || !len) {
        return;
    }
    vlock();
    bad = !annexb(d, len) || len > CONFIG_MAX;
    config_len = bad ? 0 : len;
    if (!bad) {
        memcpy(config, d, len);
    }
    vunlock();
    if (bad) {
        mcn_log("aa video: codec configuration not Annex-B or too long, waiting for SPS in-band");
    }
}

static void on_setup(void *cb, int codec)
{
    (void)cb;
    (void)codec;
    vlock();
    want_focus = 1; /* focus must be asked from our thread, not here (sink mutex held) */
    vunlock();
}

static int on_media_config(void *cb, int index)
{
    (void)cb;
    (void)index;
    return 0;
}

static void on_focus_request(void *cb, int mode, int reason)
{
    (void)cb;
    (void)mode;
    (void)reason;
    vlock();
    want_focus = 1;
    vunlock();
}

/* Input stub: answer a KeyBindingRequest (0x8002) with an empty response (0x8003), like the
 * stock InputSource; ignore everything else. Lets the phone's cluster input channel open so it
 * does not go silent (aa-video). */
static int in_route(void *self, unsigned char channel, unsigned short type, const void *buf)
{
    uint8_t *o = (uint8_t *)self;
    /* The message as MessageRouter::marshallProto lays it out (0x8e634: big-endian type, then
     * the protobuf), here 0x8003 with field 1 = 0. The stock InputSource passes its malloc'd
     * buffer to queueOutgoing and frees it right after the call (0x740b4-0x740e8): queueOutgoing
     * cuts it into frames before it returns (ChannelManager::messageToFrames via 0x6be48), so a
     * stack buffer is as good (ev_2698f764, ev_fa648f8f, ev_7cc95573, ev_3387ab5d). */
    unsigned char ok[4] = {0x80, 0x03, 0x08, 0x00};

    (void)buf;
    if (type == 0x8002 && queue_out) {
        queue_out(*(void **)(o + OFF_ROUTER), channel, ok, sizeof(ok));
    }
    return 0;
}

/* Hands queued units to the transcoder and acks each after its feed (stock pacing). */
static void *feeder(void *arg)
{
    struct au a;
    unsigned t;
    int r;

    (void)arg;
    for (;;) {
        vlock();
        if (!q_count) {
            vunlock();
            usleep(FEED_IDLE_US);
            continue;
        }
        a = queue[q_head];
        t = token;
        feeding = 1;
        vunlock();
        r = 0;
        if (t) {
            mcn_vpu_feed(t, a.data, a.n);
            r = mcn_vpu_poll(t);
        }
        ack_frames(vobj, a.session, 1);
        vlock();
        q_head = (q_head + 1) % QUEUE_SLOTS;
        q_count--;
        feeding = 0;
        if (r < 0 && t && token == t) {
            token = 0; /* gave up: publish nothing, the tick gives the cluster back */
            lost = 1;
            q_count = 0;
        }
        vunlock();
        if (r < 0 && t) {
            mcn_vpu_close(t);
        }
        /* r > 0: the decoder waits for the phone's next IDR; AAP 1.3 has no way to ask. */
    }
    return 0;
}

/* Once: the queue's buffers and the feeder thread, kept for the life of SAL. */
static int feeder_start(void)
{
    static uint8_t *mem;
    pthread_t thread;
    unsigned i;

    if (feeder_up) {
        return 1;
    }
    if (!mem) {
        mem = malloc(QUEUE_SLOTS * SLOT_BYTES);
        if (!mem) {
            return 0;
        }
    }
    vlock();
    for (i = 0; i < QUEUE_SLOTS; i++) {
        queue[i].data = mem + i * SLOT_BYTES;
    }
    vunlock();
    if (pthread_create(&thread, NULL, feeder, NULL) != 0) {
        return 0;
    }
    pthread_detach(thread);
    feeder_up = 1;
    return 1;
}

/*
 * The stream ends (channel closed or session over): transcoder closed, the cluster
 * given back to DisplayManager, the HMI told. With end set the registration is over too.
 */
static void close_stream(int end)
{
    unsigned t;
    int was_told, had_stream, release;
    struct mcn_video *v;

    vlock();
    t = token;
    had_stream = t != 0;
    was_told = told;
    token = 0;
    told = 0;
    last_data_ms = 0;
    want_focus = started = lost = 0;
    config_len = 0;
    q_count = feeding ? 1 : 0; /* the feeder pops the unit it holds */
    q_need_idr = 0;
    release = busy_held;
    busy_held = 0;
    if (end) {
        active = 0;
    }
    vunlock();
    if (t) {
        mcn_vpu_close(t);
    }
    v = had_stream ? mcn_video_map() : 0;
    if (v) {
        v->alive_ms = 0;
        v->last_frame_ms = 0;
        mcn_video_wake(v);
    }
    if (was_told) {
        mcn_link_send("video=0");
    }
    if (release) {
        mcn_busy(0);
    }
}

static int vs_closed(void *self, unsigned channel)
{
    int was_open = stock_closed(self, channel);

    close_stream(0);
    mcn_log("aa video: cluster channel closed");
    return was_open;
}

static void vs_opened(void *self, unsigned channel)
{
    stock_opened(self, channel);
    vlock();
    if (active) {
        hold(1);
    }
    vunlock();
    mcn_log("aa video: cluster channel open");
}

static int resolve(void)
{
    void *gal = dlopen(GAL_LIB, RTLD_LAZY | RTLD_GLOBAL);
    void *cpp = dlopen(STDCPP, RTLD_LAZY | RTLD_GLOBAL);
    void **vs = gal ? dlsym(gal, VS_VTABLE) : 0;
    void **peb = gal ? dlsym(gal, PEB_VTABLE) : 0;

    add_config = gal ? (add_config_fn)dlsym(gal, ADD_CONFIG) : 0;
    set_focus = gal ? (focus_fn)dlsym(gal, SET_FOCUS) : 0;
    ack_frames = gal ? (ack_fn)dlsym(gal, ACK_FRAMES) : 0;
    register_service = gal ? (register_fn)dlsym(gal, REGISTER) : 0;
    queue_out = gal ? (queue_fn)dlsym(gal, QUEUE_OUT) : 0;
    str_append = cpp ? (append_fn)dlsym(cpp, STR_APPEND) : 0;
    mutex_ctor = gal ? (mutex_ctor_fn)dlsym(gal, MUTEX_CTOR) : 0;
    if (!vs || !peb || !add_config || !set_focus || !ack_frames || !register_service ||
        !queue_out || !str_append || !mutex_ctor) {
        return 0;
    }
    memcpy(vs_vtable, vs, sizeof(vs_vtable));
    stock_closed = (closed_fn)vs_vtable[SLOT_CLOSED];
    stock_opened = (opened_fn)vs_vtable[SLOT_OPENED];
    vs_vtable[2] = vs_vtable[3] = (void *)none; /* destructors: the object is static */
    vs_vtable[SLOT_CLOSED] = (void *)vs_closed;
    vs_vtable[SLOT_OPENED] = (void *)vs_opened;
    vs_vtable[SLOT_DISCOVERY] = (void *)none; /* we advertise via raw bytes, not addDiscoveryInfo */
    memcpy(in_vtable, peb, sizeof(in_vtable));
    in_vtable[2] = in_vtable[3] = (void *)none;
    in_vtable[SLOT_ROUTE] = (void *)in_route;
    in_vtable[SLOT_DISCOVERY] = (void *)none;
    vcb_vtable[0] = vcb_vtable[1] = (void *)none;
    vcb_vtable[2] = (void *)none; /* the other data variant; VideoSink calls slot 3 */
    vcb_vtable[3] = (void *)on_data;
    vcb_vtable[4] = (void *)on_codec_config;
    vcb_vtable[5] = (void *)on_setup;
    /* VideoSink::playbackStart/Stop call callback slots 6 and 7; the library does invoke them,
     * so they must not stay NULL (verified: playbackStart 0x840f8 -> cb[0x18], stop 0x8411c ->
     * cb[0x1c]). We ignore the session, but the slots must be real. */
    vcb_vtable[6] = vcb_vtable[7] = (void *)none;
    vcb_vtable[8] = (void *)on_media_config;
    vcb_vtable[9] = (void *)on_focus_request;
    vcb_obj[0] = vcb_vtable;
    return 1;
}

static int first_free_id(uint8_t *router, int from)
{
    void **slots = (void **)(router + 0x100);
    int id;

    for (id = from; id < 0xff; id++) {
        if (!slots[id]) {
            return id;
        }
    }
    return -1;
}

/* Registers the cluster video and, if an id is free, the input endpoint with this router. */
static int register_endpoints(uint8_t *router, int *video_id, int *input_id)
{
    int vid = first_free_id(router, 0x20), iid;

    if (vid < 0) {
        return 0;
    }
    memset(vobj, 0, sizeof(vobj));
    wr32(vobj, 0, (uint32_t)(uintptr_t)&vs_vtable[2]);
    wr32(vobj, OFF_ROUTER, (uint32_t)(uintptr_t)router);
    wr32(vobj, OFF_ID, (uint32_t)vid);
    vobj[OFF_CODEC] = 3;
    wr32(vobj, OFF_SESSION, 0xffffffffu);
    vobj[OFF_UNACKED] = 1;
    wr32(vobj, OFF_REFCOUNT, (uint32_t)(uintptr_t)&refcount);
    wr32(vobj, OFF_CALLBACKS, (uint32_t)(uintptr_t)vcb_obj);
    mutex_ctor(vobj + OFF_MUTEX); /* the library's own Mutex ctor, byte-compatible */
    /* The one configuration the raw bytes advertise: 1280x720 (res 2), 30 fps (enum 2),
     * the margins, density, aspect. res 1..3 and fps 1..2 or the library asserts. */
    add_config(vobj, 2, 2, geom.margin_w, geom.margin_h, 160, 0, 10000);
    if (register_service(router, vobj) != 1) {
        wr32(vobj, OFF_ROUTER, 0);
        return 0;
    }
    iid = first_free_id(router, vid + 1);
    if (iid >= 0) {
        memset(iobj, 0, sizeof(iobj));
        wr32(iobj, 0, (uint32_t)(uintptr_t)&in_vtable[2]);
        wr32(iobj, OFF_ROUTER, (uint32_t)(uintptr_t)router);
        wr32(iobj, OFF_ID, (uint32_t)iid);
        if (register_service(router, iobj) != 1) {
            iid = -1;
        }
    }
    *video_id = vid;
    *input_id = iid; /* none free: the input entry is left out (mcn_aa_sdr) */
    return 1;
}

/* Called from our populate hook (gal.c) before the stock populate and the append below. */
void mcn_gal_video_offer(void *router, void *response)
{
    uint8_t *r = (uint8_t *)router;
    void **slots = (void **)(r + 0x100);
    uint8_t out[256];
    int vid, iid, n, again;

    if (broken || mcn_arrows_only()) {
        return;
    }
    if (!resolved) {
        if (!resolve()) {
            broken = 1;
            mcn_log("aa video: receiver symbols missing, stock behaviour");
            return;
        }
        mcn_geom(MCN_TC_AA, 1000, &geom);
        resolved = 1;
    }
    /* Already registered with this router (a new GalReceiver may reuse an old address). */
    vlock();
    again = active && rd32(vobj, OFF_ROUTER) == (uint32_t)(uintptr_t)r &&
            slots[rd32(vobj, OFF_ID) & 0xff] == (void *)vobj;
    vunlock();
    if (again) {
        return;
    }
    close_stream(1); /* a new connection: whatever the previous one left */
    vlock();
    hold(1);
    vunlock();
    if (!register_endpoints(r, &vid, &iid)) {
        close_stream(1);
        mcn_log("aa video: endpoint registration failed");
        return;
    }
    n = mcn_aa_sdr(out, sizeof(out), vid, iid, &geom);
    if (n <= 0) {
        close_stream(1);
        return;
    }
    /* resp+4 is the response's unknown-field std::string; append our Service bytes. */
    str_append((uint8_t *)response + 4, (const char *)out, (unsigned)n);
    vlock();
    active = 1;
    vunlock();
    mcn_log(iid >= 0 ? "aa video: cluster screen offered to the phone"
                     : "aa video: cluster screen offered to the phone, no input id free");
}

/* The Android Auto session of this router ended (gal.c, GalReceiver::shutdown). */
void mcn_gal_video_end(void *router)
{
    int mine;

    vlock();
    mine = active && rd32(vobj, OFF_ROUTER) == (uint32_t)(uintptr_t)router;
    vunlock();
    if (mine) {
        close_stream(1);
        mcn_log("aa video: session ended");
    }
}

/* From the SAL worker loop (iap2.c), a few times a second: answer the phone's focus requests
 * and start the transcoder off the callback thread; keep the cluster the phone's while the
 * channel is open; give it back if the transcoder gave up. */
void mcn_gal_video_tick(void)
{
    unsigned now = mcn_now_ms(), t, last;
    int focus, start, open_now, gone, say;
    struct mcn_video *v;

    vlock();
    if (!active) {
        vunlock();
        return;
    }
    if (busy_held) {
        mcn_busy_tick();
    }
    focus = want_focus; /* every setup or focus request is answered */
    want_focus = 0;
    start = focus && !started;
    if (start) {
        started = 1;
    }
    gone = lost;
    lost = 0;
    say = gone && told;
    if (gone) {
        told = 0;
    }
    vunlock();
    if (gone) {
        v = mcn_video_map();
        if (v) {
            v->alive_ms = 0;
            v->last_frame_ms = 0;
            mcn_video_wake(v);
        }
        if (say) {
            mcn_link_send("video=0");
        }
        mcn_log("aa video: transcoder gave up, cluster back to stock");
    }
    if (start) {
        v = feeder_start() ? mcn_video_map() : 0;
        if (v) {
            v->write_seq = 0; /* readers resynchronise, as for a new CarPlay stream */
            __sync_synchronize();
            v->generation++;
        }
        t = v ? mcn_vpu_open(&geom, v) : 0;
        mcn_log(t ? "aa video: transcoder started"
                  : "aa video: transcoder unavailable, no cluster video");
        vlock();
        if (active && started) {
            token = t;
            t = 0;
        }
        vunlock();
        if (t) {
            mcn_vpu_close(t); /* the stream ended while the transcoder came up */
        }
    }
    if (focus) {
        set_focus(vobj, 1, 0); /* VIDEO_FOCUS_PROJECTED to the phone's (re)quest */
    }
    /* The cluster is the phone's while the channel is open, frames or not (mcn_video.h). */
    vlock();
    t = token;
    last = last_data_ms;
    open_now = vobj[OFF_OPEN] != 0;
    vunlock();
    v = t && open_now && (!last || now - last < AA_STILL_MS) ? mcn_video_map() : 0;
    if (v) {
        v->alive_ms = now ? now : 1;
        vlock();
        start = v->write_seq && now - told_ms >= 1000 && token == t;
        if (start) {
            told_ms = now;
            told = 1;
        }
        vunlock();
        if (start && mcn_active()) {
            mcn_link_send("video=1"); /* the HMI keeps its notice away (as stream.c) */
        }
    }
}

void mcn_gal_video_stop(void)
{
    close_stream(1);
}
