/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Android Auto navigation status (native arrows) and now playing (the cluster's media line),
 * offered at every service discovery while the mod is active (switched off: nothing is
 * registered, pure pass-through). The unit's
 * receiver library (libext.google.gal.receiver.so) has both endpoints with parser and handlers,
 * but stock SAL registers neither, so the phone is never offered the services. Before each
 * service discovery this registers static copies of the library's own endpoints, laid out as
 * SAL's inline constructors lay out theirs (checked on the library, GAL 7af20a37):
 *   both: +0 vptr, +4 channel open, +5 channel, +8 MessageRouter, +0xc service id, +0x10 raw
 *         mode (0) (ProtocolEndpointBase 0x74788..0x747bc, ev_7127b3c3);
 *   navigation: +0x14 minimum interval, +0x24 cluster type (2 = enum), +0x28 callbacks
 *         refcount, +0x2c callbacks (addDiscoveryInfo 0x8a8c0, destructor 0x8aec4, ev_7127b3c3);
 *   media: +0x14 callbacks refcount, +0x18 callbacks (destructor 0x83cec; addDiscoveryInfo
 *         0x83498 reads only the id, ev_1a993af7).
 * Each vtable is copied with four slots changed: no-op destructors (the objects are static) and
 * onChannelClosed/Opened wrapped (the router calls vptr+8 on a close notification only,
 * 0x8e500, ev_b0ebb5be). Opening the navigation channel also sends NavigationStatusStart
 * (nothing in the library calls start). The handlers call our callbacks with the parsed fields
 * (navigation: status slot 2, turn slot 3 (road, side, event, image, turn_angle, turn_number),
 * distance slot 4, ev_2e2700f5, ev_d15c73e6; media: status slot 2 and metadata slot 3 with a
 * struct, ev_1a993af7); they only copy them (common/aa.c) and tell the HMI.
 *
 * The session ends at GalReceiver::shutdown, which SAL calls through its PLT (ev_d6b9ece1,
 * ev_22b15ed5); the receiver's own teardown calls no endpoint, so this hook clears guidance and
 * now playing. In case it is missed, the guidance heartbeat stops after AA_SILENT_MS without a
 * callback (the next callback resumes it). The crash guard (mcn_busy) is held from each
 * registration or channel open to its channel close or session end.
 */
#include "sal.h"
#include "mcn_route.h"
#include "mcn_transcode.h"

#define GAL_LIB "libext.google.gal.receiver.so"
#define NAV_VTABLE "_ZTV24NavigationStatusEndpoint"
#define MEDIA_VTABLE "_ZTV27MediaPlaybackStatusEndpoint"
#define NAV_START "_ZN24NavigationStatusEndpoint5startEv"
#define REGISTER "_ZN13MessageRouter15registerServiceEP20ProtocolEndpointBase"
#define MIN_INTERVAL_MS 500
#define CLUSTER_ENUM 2
/*
 * Guidance heartbeat without news from the phone. Nothing in the receiver sends or asks for a
 * keep-alive, and how often a phone repeats an unchanged distance is not known (no Android Auto
 * log yet), so this only bounds a session whose end was missed; a callback after it resumes the
 * heartbeat with the kept maneuver.
 */
#define AA_SILENT_MS 60000
/* MediaPlaybackStatus.State (0x138e44, ev_907fd535). */
#define MEDIA_STOPPED 1
#define MEDIA_PLAYING 2
#define MEDIA_PAUSED 3

/* common/aa.c */
void mcn_aa_copy(char *dst, unsigned size, const char *src, unsigned len);
int mcn_aa_media_text(const char *title, const char *artist, const char *album, char *out,
                      unsigned size);
/* sal/gal_video.c: the Android Auto session of this router ended. */
void mcn_gal_video_end(void *router);

MCN_HOOK(_ZN13MessageRouter32populateServiceDiscoveryResponseEP24ServiceDiscoveryResponse,
         GAL_LIB);
MCN_HOOK(_ZN11GalReceiver8shutdownEv, GAL_LIB);
/* NavigationStatusEndpoint::handleNavigationDistanceEvent(NavigationNextTurnDistanceEvent const&),
 * spelled out: the name is too long for MCN_HOOK on one line. */
#define DISTANCE_HANDLER                                                                  \
    _ZN24NavigationStatusEndpoint29handleNavigationDistanceEventERK31NavigationNextTurnDistanceEvent
static struct mcn_hook distance_hook = {
    "_ZN24NavigationStatusEndpoint29handleNavigationDistanceEvent"
    "ERK31NavigationNextTurnDistanceEvent",
    GAL_LIB, 0, 0};

static uint32_t nav[0x40 / 4];
static uint32_t media[0x20 / 4];
static void *nav_vtable[10];   /* offset to top, typeinfo, 8 virtual functions */
static void *media_vtable[10]; /* the same shape (0x15e790, ev_b0ebb5be) */
static void *cb_vtable[5];     /* two destructors, status, turn, distance */
static void *cb_object[1];
static void *mcb_vtable[4];    /* two destructors, status, metadata */
static void *mcb_object[1];
static int cb_refs = 0x40000000;
/* ProtocolEndpointBase's, the same in both vtables (0x747a4, 0x74788, ev_b0ebb5be). */
static int (*stock_closed)(void *, unsigned); /* 1 if the channel was open */
static void (*stock_opened)(void *, unsigned);
static int (*register_service)(void *, void *);
static void (*nav_start)(void *);
static int resolved, broken;

static volatile unsigned state_lock;
static struct mcn_aa aa;
static unsigned last_sent, last_news;
static int nav_busy, media_busy;
static int media_state, media_told;
static unsigned media_sent;
static char title[MCN_MEDIA_TEXT], artist[MCN_MEDIA_TEXT], album[MCN_MEDIA_TEXT];

static void lock(void)
{
    while (__sync_lock_test_and_set(&state_lock, 1)) {
        usleep(1000);
    }
}

static void unlock(void)
{
    __sync_lock_release(&state_lock);
}

/* Under the lock: the crash guard, taken and given back in pairs. */
static void hold(int *held, int on)
{
    if (*held != on) {
        *held = on;
        mcn_busy(on);
    }
}

/* Under the lock: the guidance snapshot to the HMI. */
static void publish(void)
{
    char out[MCN_RG_SNAPSHOT];

    if (mcn_active() && mcn_aa_text(&aa, out, sizeof(out)) > 0) {
        mcn_link_send(out);
    }
    last_sent = mcn_now_ms();
}

/* Under the lock: the track to the HMI while one plays, else its end once (paused hides it,
 * as for CarPlay: common/media.c). */
static void media_publish(void)
{
    char out[4 * MCN_MEDIA_TEXT];

    if (media_state == MEDIA_PLAYING && mcn_active() &&
        mcn_aa_media_text(title, artist, album, out, sizeof(out)) > 0) {
        mcn_link_send(out);
        media_told = 1;
    } else if (media_told) {
        mcn_link_send("media=0");
        media_told = 0;
    }
    media_sent = mcn_now_ms();
}

/* Under the lock: guidance ends (channel closed, session over). */
static void nav_end(void)
{
    if (aa.active) {
        mcn_aa_status(&aa, 0);
        publish();
    }
    hold(&nav_busy, 0);
}

static void media_end(void)
{
    media_state = 0;
    title[0] = artist[0] = album[0] = 0;
    media_publish();
    hold(&media_busy, 0);
}

/* About once a second from the route worker: PhoneLink ends silent guidance after 5 s and
 * forgets a track after 3 s. */
void mcn_gal_tick(unsigned now)
{
    lock();
    if (nav_busy || media_busy) {
        mcn_busy_tick();
    }
    if (aa.active && now - last_sent >= 1000 && now - last_news < AA_SILENT_MS) {
        publish();
    }
    if ((media_told || media_state == MEDIA_PLAYING) && now - media_sent >= 1000) {
        media_publish();
    }
    unlock();
}

static void none(void *self)
{
    (void)self;
}

/* An old libstdc++ COW std::string: a pointer to the characters, the length 12 bytes before. */
static unsigned cow_len(const char *s)
{
    int n = s ? ((const int *)s)[-3] : 0;

    return n > 0 && n <= 4096 ? (unsigned)n : 0;
}

static void cb_status(void *self, int status)
{
    (void)self;
    lock();
    last_news = mcn_now_ms();
    if (status == 1 && !aa.active) {
        mcn_log("android auto: guidance started");
    }
    mcn_aa_status(&aa, status);
    publish();
    unlock();
}

static void cb_turn(void *self, const char *const *road, int side, int event,
                    const char *const *image, int angle, int number)
{
    const char *text = road ? *road : 0;

    (void)self;
    (void)image;
    lock();
    last_news = mcn_now_ms();
    mcn_aa_turn(&aa, text, cow_len(text), side, event, number, angle);
    if (aa.active) {
        publish();
    }
    unlock();
}

static void cb_distance(void *self, int meters, int seconds, int display_e3, int unit)
{
    (void)self;
    (void)seconds;
    (void)display_e3;
    (void)unit;
    lock();
    last_news = mcn_now_ms();
    mcn_aa_distance(&aa, meters);
    if (aa.active) {
        publish();
    }
    unlock();
}

/*
 * The library's distance handler calls the callback only when field 4 (unit) is present and not
 * zero (0x8a6c8, ev_2e2700f5), so a phone that leaves the unit out would never show a distance.
 * routeMessage calls the handler through the library's PLT (0x8acac, ev_b473e392), so for our
 * own endpoint we read the parsed message here instead: presence bits at +8, field 1
 * distance_meters at +0x10 (bit 0) (serializer 0xb9948, ev_02873d97). Any other caller passes.
 */
MCN_EXPORT void DISTANCE_HANDLER(void *self, const uint8_t *event)
{
    void (*real)(void *, const uint8_t *) = mcn_hook_enter(&distance_hook);
    uint32_t has;
    int meters;

    if (self != (void *)nav) {
        real(self, event);
        return;
    }
    memcpy(&has, event + 8, 4);
    memcpy(&meters, event + 0x10, 4);
    cb_distance(0, has & 1 ? meters : -1, 0, 0, 0);
}

/* MediaPlaybackStatus as the handler passes it: {state, source, seconds, flags} (ev_1a993af7). */
static void cb_media_status(void *self, const int *status)
{
    int was;

    (void)self;
    lock();
    was = media_state;
    media_state = status[0];
    if (media_state != was) {
        media_publish();
    }
    unlock();
}

/* MediaPlaybackMetadata as the handler passes it: {song, album, artist, art, playlist, ...}
 * (0x836f0, ev_1a993af7). */
static void cb_media_meta(void *self, const char *const *meta)
{
    (void)self;
    lock();
    mcn_aa_copy(title, sizeof(title), meta[0], cow_len(meta[0]));
    mcn_aa_copy(album, sizeof(album), meta[1], cow_len(meta[1]));
    mcn_aa_copy(artist, sizeof(artist), meta[2], cow_len(meta[2]));
    media_publish();
    unlock();
}

static void nav_opened(void *self, unsigned channel)
{
    stock_opened(self, channel);
    lock();
    hold(&nav_busy, 1);
    unlock();
    nav_start(self);
    mcn_log("android auto: navigation channel open, start sent");
}

static int nav_closed(void *self, unsigned channel)
{
    int was_open = stock_closed(self, channel);

    lock();
    nav_end();
    unlock();
    return was_open;
}

static void media_opened(void *self, unsigned channel)
{
    stock_opened(self, channel);
    lock();
    hold(&media_busy, 1);
    unlock();
    mcn_log("android auto: media status channel open");
}

static int media_closed(void *self, unsigned channel)
{
    int was_open = stock_closed(self, channel);

    lock();
    media_end();
    unlock();
    return was_open;
}

/* The library's vtable with destructors made no-ops and the channel slots ours. */
static int copy_vtable(void *lib, const char *name, void **vt, void *opened, void *closed)
{
    void **stock = dlsym(lib, name);

    if (!stock) {
        return 0;
    }
    memcpy(vt, stock, 10 * sizeof(void *));
    stock_closed = (int (*)(void *, unsigned))vt[4];
    stock_opened = (void (*)(void *, unsigned))vt[6];
    vt[2] = vt[3] = (void *)none;
    vt[4] = closed;
    vt[6] = opened;
    return 1;
}

static int resolve(void)
{
    void *lib = dlopen(GAL_LIB, RTLD_LAZY);

    register_service = lib ? dlsym(lib, REGISTER) : 0;
    nav_start = lib ? dlsym(lib, NAV_START) : 0;
    if (!register_service || !nav_start ||
        !copy_vtable(lib, NAV_VTABLE, nav_vtable, (void *)nav_opened, (void *)nav_closed) ||
        !copy_vtable(lib, MEDIA_VTABLE, media_vtable, (void *)media_opened, (void *)media_closed)) {
        return 0;
    }
    cb_vtable[0] = cb_vtable[1] = (void *)none;
    cb_vtable[2] = (void *)cb_status;
    cb_vtable[3] = (void *)cb_turn;
    cb_vtable[4] = (void *)cb_distance;
    cb_object[0] = cb_vtable;
    mcb_vtable[0] = mcb_vtable[1] = (void *)none;
    mcb_vtable[2] = (void *)cb_media_status;
    mcb_vtable[3] = (void *)cb_media_meta;
    mcb_object[0] = mcb_vtable;
    return 1;
}

static uint8_t *router_of(const uint32_t *obj)
{
    return *(uint8_t *const *)((const uint8_t *)obj + 8);
}

/* Registers obj (cleared to its words, vtable vt) with this connection's router under the first
 * free service id, unless it already is; every stock service is registered by now. */
static int offer(uint8_t *router, uint32_t *obj, unsigned words, void **vt, int *busy,
                 void (*end)(void), void (*fill)(uint32_t *))
{
    void **slots = (void **)(router + 0x100); /* MessageRouter: endpoint by service id */
    unsigned id;
    int ok;

    if (router_of(obj) == router && slots[obj[3] & 0xff] == obj) {
        return 0;
    }
    for (id = 1; id < 0xff && slots[id]; id++) {
    }
    if (id == 0xff) {
        return 0;
    }
    lock();
    if (router_of(obj)) {
        end(); /* a new connection: the previous one is over */
    }
    hold(busy, 1);
    unlock();
    memset(obj, 0, words * 4);
    obj[0] = (uint32_t)(uintptr_t)&vt[2];
    *(uint8_t **)((uint8_t *)obj + 8) = router;
    obj[3] = id;
    fill(obj);
    ok = register_service(router, obj) == 1;
    if (!ok) {
        lock();
        hold(busy, 0);
        unlock();
        *(uint8_t **)((uint8_t *)obj + 8) = 0;
    }
    return ok ? (int)id : -1;
}

static void fill_nav(uint32_t *o)
{
    o[5] = MIN_INTERVAL_MS;
    o[9] = CLUSTER_ENUM;
    o[0x28 / 4] = (uint32_t)(uintptr_t)&cb_refs;
    o[0x2c / 4] = (uint32_t)(uintptr_t)cb_object;
    lock();
    mcn_aa_reset(&aa);
    unlock();
}

static void fill_media(uint32_t *o)
{
    o[0x14 / 4] = (uint32_t)(uintptr_t)&cb_refs;
    o[0x18 / 4] = (uint32_t)(uintptr_t)mcb_object;
}

static void report(int id, const char *what)
{
    char line[80];

    if (id) {
        snprintf(line, sizeof(line), id > 0 ? "android auto: %s offered as service %d"
                                            : "android auto: %s registration refused",
                 what, id);
        mcn_log(line);
    }
}

MCN_EXPORT int _ZN13MessageRouter32populateServiceDiscoveryResponseEP24ServiceDiscoveryResponse(
    void *router, void *response)
{
    int (*real)(void *, void *) =
        MCN_REAL(_ZN13MessageRouter32populateServiceDiscoveryResponseEP24ServiceDiscoveryResponse);

    if (mcn_active()) {
        if (!broken && !resolved) {
            resolved = resolve();
            broken = !resolved;
            if (broken) {
                mcn_log("android auto: receiver endpoints not found, stock behaviour");
            }
        }
        if (resolved) {
            report(offer(router, nav, sizeof(nav) / 4, nav_vtable, &nav_busy, nav_end, fill_nav),
                   "navigation");
        }
        if (resolved) {
            report(offer(router, media, sizeof(media) / 4, media_vtable, &media_busy, media_end,
                         fill_media),
                   "media status");
        }
        mcn_gal_video_offer(router, response);
    }
    return real(router, response);
}

/* SAL ends an Android Auto session (GalReceiver+4 is its MessageRouter, 0x720d8/0x72164,
 * ev_b0ebb5be): whatever we registered with that router is over. */
MCN_EXPORT void _ZN11GalReceiver8shutdownEv(void *receiver)
{
    void (*real)(void *) = MCN_REAL(_ZN11GalReceiver8shutdownEv);
    uint8_t *router = (uint8_t *)receiver + 4;

    real(receiver);
    lock();
    if (router_of(nav) == router) {
        nav_end();
        *(uint8_t **)((uint8_t *)nav + 8) = 0;
    }
    if (router_of(media) == router) {
        media_end();
        *(uint8_t **)((uint8_t *)media + 8) = 0;
    }
    unlock();
    mcn_gal_video_end(router);
}
