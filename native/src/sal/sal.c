/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Hooks in SAL (tsd.mibstd2.sal.main), the process that runs CarPlay. The CarPlay plug-in
 * libext.apple.carplay.airplay.so 210.81 calls these exports of its own through the PLT
 * (mQbEMU research notes, carplay-airplay-lib.md), so the
 * definitions here take its calls and pass them on. What they add is a second screen:
 * - /info (AirPlayCopyServerInfo): the cluster display next to the main one;
 * - control SETUP (the first, which answers timingPort and eventPort): "altScreen" and
 *   "viewAreas" in the response's enabledFeatures, which is what the phone reads to enable the
 *   second screen;
 * - media SETUP with a type-111 stream: our receiver (stream.c) and its descriptor;
 * - TEARDOWN of the whole session or of stream 111: the receiver stops.
 * The stock plug-in skips type 111 ("Unsupported stream type") and has none of these keys.
 */
#include "sal.h"
#include "mcn_transcode.h"

MCN_HOOK(AirPlayCopyServerInfo, AIRPLAY_LIB);
MCN_HOOK(AirPlayReceiverSessionSetSecurityInfo, AIRPLAY_LIB);
MCN_HOOK(AirPlayReceiverSessionSetup, AIRPLAY_LIB);
MCN_HOOK(AirPlayReceiverSessionTearDown, AIRPLAY_LIB);

/* The session key the phone set up (SetSecurityInfo); stream keys are derived from it. */
static void *key_session;
static uint8_t session_key[16];

/*
 * The stock 110 setup (_ScreenSetup, 210.81) opens its listener in the family of the phone's
 * address, the sa_family byte of the session's peer sockaddr at +0x48.
 */
int mcn_peer_family(void *session)
{
    int family = ((const uint8_t *)session)[0x49];

    return family == AF_INET ? AF_INET : AF_INET6;
}

static int64_t stream_type(CFDictionaryRef desc)
{
    int64_t type = 0;

    return cf_dict_get_i64(desc, "type", &type) ? type : 0;
}

static CFMutableArrayRef video_streams(CFTypeRef uuid)
{
    CFMutableArrayRef streams = cf_array_new();
    CFMutableDictionaryRef s = cf_dict_new();

    cf_dict_set_i64(s, "originXPixels", 0);
    cf_dict_set_i64(s, "originYPixels", 0);
    cf_dict_set(s, "uuid", uuid);
    cf_dict_set_i64(s, "zIndex", 0);
    cf_array_append(streams, s);
    cf_release(s);
    return streams;
}

static void set_area(CFMutableDictionaryRef d, int x, int y, int w, int h)
{
    cf_dict_set_i64(d, "originXPixels", x);
    cf_dict_set_i64(d, "originYPixels", y);
    cf_dict_set_i64(d, "widthPixels", w);
    cf_dict_set_i64(d, "heightPixels", h);
}

const char *mcn_cluster_url(void)
{
    char text[4];
    int fd = open(MCN_SCENE_FILE, O_RDONLY);

    if (fd < 0) {
        return MCN_CLUSTER_URL_MAP;
    }
    memset(text, 0, sizeof(text));
    read(fd, text, sizeof(text) - 1);
    close(fd);
    return text[0] == 'a' && text[1] == 'p' && text[2] == 'p' ? MCN_CLUSTER_URL_APP
                                                             : MCN_CLUSTER_URL_MAP;
}

/*
 * Frame rate offered to the phone for the cluster: GEM writes 10, 20, 25 or 30 to MCN_FPS_FILE.
 * 25 by default: with 30 offered, a moving map came at 9-14 Mbit/s and about 24 frames a second
 * reached the cluster through the MOST channel (car, 2026-10-26); the frames beyond that only
 * load the phone's encoder and the USB link.
 */
static int cluster_fps(void)
{
    char text[4] = { 0 };
    int fd = open(MCN_FPS_FILE, O_RDONLY);

    if (fd < 0) {
        return MCN_CLUSTER_FPS;
    }
    read(fd, text, sizeof(text) - 1);
    close(fd);
    if (text[0] == '1' && text[1] == '0') {
        return 10;
    }
    if (text[0] == '2' && text[1] == '0') {
        return 20;
    }
    if (text[0] == '3' && text[1] == '0') {
        return 30;
    }
    return MCN_CLUSTER_FPS;
}

/*
 * Geometry offered to the phone: physical size (the phone scales its UI from pixels per mm, so
 * this sets how big the car marker and the cards come out) and the safe area. Defaults in
 * sal.h; MCN_DISPLAY_FILE ("width_mm height_mm safe_x safe_y safe_width safe_height") replaces
 * them for calibration in a car. Values that do not fit the window are ignored as a whole.
 * In the car (Yandex Maps, 2026-10-26) the safe area moved the car marker sideways only (x 377
 * put it 70 px left of the window's middle, x 447 in it); neither the safe area's top or
 * bottom nor the physical size changed the picture, and a view area shorter than the screen
 * made the phone close the session at once.
 */
struct geometry {
    int mm_w, mm_h, x, y, w, h;
};

static struct geometry cluster_geometry(void)
{
    struct geometry g = { MCN_CLUSTER_WIDTH_MM, MCN_CLUSTER_HEIGHT_MM, MCN_SAFE_X, MCN_SAFE_Y,
                          MCN_SAFE_WIDTH, MCN_SAFE_HEIGHT };
    int v[6], n = 0, fd = open(MCN_DISPLAY_FILE, O_RDONLY);
    char text[64], *p;

    if (fd < 0) {
        return g;
    }
    memset(v, 0, sizeof(v));
    memset(text, 0, sizeof(text));
    read(fd, text, sizeof(text) - 1);
    close(fd);
    for (p = text; *p && n < 6; n++) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p < '0' || *p > '9') {
            break;
        }
        while (*p >= '0' && *p <= '9') {
            v[n] = v[n] * 10 + (*p++ - '0');
        }
    }
    if (n == 6 && v[0] >= 50 && v[0] <= 400 && v[1] >= 20 && v[1] <= 200 && v[4] > 0 && v[5] > 0 &&
        v[2] + v[4] <= MCN_CLUSTER_WIDTH && v[3] + v[5] <= MCN_CLUSTER_HEIGHT) {
        g.mm_w = v[0];
        g.mm_h = v[1];
        g.x = v[2];
        g.y = v[3];
        g.w = v[4];
        g.h = v[5];
    } else {
        mcn_log("display file ignored: width_mm height_mm safe_x safe_y safe_w safe_h expected");
    }
    return g;
}

/* The zoom /info advertised last (sal.h); read from MCN_ZOOM_FILE only there. */
static volatile int session_zoom = 1000;

int mcn_cluster_zoom(void)
{
    return session_zoom;
}

/* MCN_ZOOM_FILE; 1000 (off) when absent or out of 1000..1900 (the i.MX6 VPU width). */
static int zoom_file(void)
{
    char text[16] = { 0 };
    int fd = open(MCN_ZOOM_FILE, O_RDONLY), v = 0, i;

    if (fd < 0) {
        return 1000;
    }
    read(fd, text, sizeof(text) - 1);
    close(fd);
    for (i = 0; text[i] >= '0' && text[i] <= '9'; i++) {
        v = v * 10 + (text[i] - '0');
    }
    return v >= 1000 && v <= 1900 ? v : 1000;
}

static CFMutableDictionaryRef cluster_display(void)
{
    struct geometry g = cluster_geometry();
    CFMutableDictionaryRef d = cf_dict_new(), view = cf_dict_new(), safe = cf_dict_new();
    CFMutableArrayRef views = cf_array_new(), properties = cf_array_new();
    CFStringRef uuid = cf_str(MCN_CLUSTER_UUID);
    CFMutableArrayRef streams = video_streams(uuid);
    int zoom = zoom_file(), cw = MCN_CLUSTER_WIDTH, ch = MCN_CLUSTER_HEIGHT;

    if (zoom > 1000) {
        /* Offer a proportionally bigger screen; the safe area scales with it so the car marker
         * stays put. The map comes back down to 1010x376 in sal/vpu.c. */
        struct mcn_geom zg;

        mcn_geom(MCN_TC_CARPLAY_ZOOM, zoom, &zg);
        cw = zg.req_w;
        ch = zg.req_h;
        /* Scale the safe area by the real advertised ratio (cw is 16-aligned, so not exactly
         * zoom/1000), or the car marker drifts from where the comment says it stays. */
        g.x = g.x * cw / MCN_CLUSTER_WIDTH;
        g.w = g.w * cw / MCN_CLUSTER_WIDTH;
        g.y = g.y * ch / MCN_CLUSTER_HEIGHT;
        g.h = g.h * ch / MCN_CLUSTER_HEIGHT;
    }
    /* The phone encodes what is offered here: the stream that follows is decoded at this size,
     * whatever the file says by then (GEM may change it meanwhile). */
    session_zoom = zoom;

    cf_dict_set(d, "uuid", uuid);
    cf_dict_set(d, "uid", uuid);
    cf_dict_set_str(d, "name", "Instrument Cluster");
    cf_array_append_str(properties, "Instrument Cluster");
    cf_dict_set(d, "properties", properties);
    cf_dict_set_i64(d, "widthPixels", cw);
    cf_dict_set_i64(d, "heightPixels", ch);
    cf_dict_set_i64(d, "widthPhysical", g.mm_w);
    cf_dict_set_i64(d, "heightPhysical", g.mm_h);
    cf_dict_set_i64(d, "maxFPS", cluster_fps());
    cf_dict_set_i64(d, "features", 0); /* no touch, no knob */
    cf_dict_set(d, "initialVideoStreams", streams);
    set_area(view, 0, 0, cw, ch);
    set_area(safe, g.x, g.y, g.w, g.h);
    /*
     * The map fills the window; the phone keeps the car marker and its cards in the safe area.
     * Receivers differ in where they put this flag (the view area or its safe area), and newer
     * plug-ins do not emit it at all. Both places, so either reading works.
     */
    cf_dict_set_bool(view, "drawUIOutsideSafeArea", 1);
    cf_dict_set_bool(safe, "drawUIOutsideSafeArea", 1);
    cf_dict_set(view, "safeArea", safe);
    cf_array_append(views, view);
    cf_dict_set(d, "viewAreas", views);
    cf_dict_set_i64(d, "initialViewArea", 0);
    cf_dict_set_str(d, "initialURL", mcn_cluster_url());
    cf_release(streams);
    cf_release(uuid);
    cf_release(properties);
    cf_release(views);
    cf_release(safe);
    cf_release(view);
    return d;
}

/* GEM "Native arrows only" (mcn_set.sh arrows): neither phone is offered the cluster screen. */
int mcn_arrows_only(void)
{
    char mode[8] = { 0 };
    int fd = open(MCN_MODE_FILE, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    read(fd, mode, sizeof(mode) - 1);
    close(fd);
    return strstr(mode, "arrows") != NULL;
}

/*
 * The plug-in lists one display, in the old schema; the phone keys displays by "uuid" and
 * display panels by "uid" with their video streams, and takes a second, different uuid as
 * the second screen. The main entry keeps its own uuid: the stock decoder waits on it.
 */
static void advertise(CFMutableDictionaryRef info)
{
    CFArrayRef stock = cf_dict_get(info, "displays");
    CFMutableDictionaryRef main_display, cluster;
    CFMutableArrayRef displays, streams, features;
    CFTypeRef uuid;

    if (!mcn_active() || cf_array_count(stock) != 1 || !mcn_stream_ready()) {
        return;
    }
    if (mcn_arrows_only()) {
        mcn_log("/info: native arrows only, cluster not offered");
        return;
    }
    main_display = cf_dict_clone(cf_array_get(stock, 0));
    uuid = cf_dict_get(main_display, "uuid");
    if (!uuid) {
        mcn_log("main display has no uuid: cluster not offered");
        cf_release(main_display);
        return;
    }
    cf_dict_set(main_display, "uid", uuid);
    streams = video_streams(uuid);
    cf_dict_set(main_display, "initialVideoStreams", streams);
    cluster = cluster_display();
    displays = cf_array_new();
    cf_array_append(displays, main_display);
    cf_array_append(displays, cluster);
    cf_dict_set(info, "displays", displays);
    cf_dict_set(info, "displayPanels", displays);
    features = cf_array_new();
    cf_array_append_str(features, "altScreen");
    cf_array_append_str(features, "viewAreas");
    cf_dict_set(info, "extendedFeatures", features);
    cf_release(features);
    cf_release(displays);
    cf_release(cluster);
    cf_release(streams);
    cf_release(main_display);
    mcn_log("/info: cluster display offered");
}

MCN_EXPORT CFDictionaryRef AirPlayCopyServerInfo(void *session, CFArrayRef properties, void *a2,
                                                 void *a3)
{
    CFDictionaryRef (*real)(void *, CFArrayRef, void *, void *) =
        MCN_REAL(AirPlayCopyServerInfo);
    CFDictionaryRef info = real(session, properties, a2, a3);

    /* 210.81 builds the result with CFDictionaryCreateMutable. */
    if (info && cf_ready()) {
        advertise((CFMutableDictionaryRef)info);
    }
    return info;
}

MCN_EXPORT OSStatus AirPlayReceiverSessionSetSecurityInfo(void *session, const uint8_t key[16],
                                                          const uint8_t iv[16])
{
    OSStatus (*real)(void *, const uint8_t *, const uint8_t *) =
        MCN_REAL(AirPlayReceiverSessionSetSecurityInfo);

    memcpy(session_key, key, sizeof(session_key));
    key_session = session;
    return real(session, key, iv);
}

/* Adds to the response's enabledFeatures, keeping what is there (each name enables a feature). */
static void enable_features(CFMutableDictionaryRef response)
{
    CFMutableArrayRef features = cf_array_copy(cf_dict_get(response, "enabledFeatures"));

    cf_array_append_str(features, "altScreen");
    cf_array_append_str(features, "viewAreas");
    cf_dict_set(response, "enabledFeatures", features);
    cf_release(features);
}

static void setup_cluster(void *session, CFDictionaryRef desc, CFMutableDictionaryRef response)
{
    CFMutableArrayRef streams;
    CFMutableDictionaryRef answer;
    int64_t connection_id = 0;
    int port;

    /* Only a checked plug-in build may have its session read (mcn_peer_family). */
    if (!mcn_stream_ready() || !cf_dict_get_i64(desc, "streamConnectionID", &connection_id) ||
        !connection_id || key_session != session) {
        mcn_log("cluster stream: no connection id or session key");
        return;
    }
    port = mcn_stream_start(session, session_key, (uint64_t)connection_id, mcn_peer_family(session));
    if (port <= 0) {
        mcn_log("cluster stream: receiver failed");
        return;
    }
    answer = cf_dict_new();
    cf_dict_set_i64(answer, "type", MCN_STREAM_TYPE);
    cf_dict_set_i64(answer, "dataPort", port);
    streams = cf_array_copy(cf_dict_get(response, "streams"));
    cf_array_append(streams, answer);
    cf_dict_set(response, "streams", streams);
    cf_release(streams);
    cf_release(answer);
    mcn_log("cluster stream: set up");
}

MCN_EXPORT OSStatus AirPlayReceiverSessionSetup(void *session, CFDictionaryRef request,
                                                CFDictionaryRef *response)
{
    OSStatus (*real)(void *, CFDictionaryRef, CFDictionaryRef *) =
        MCN_REAL(AirPlayReceiverSessionSetup);
    OSStatus err = real(session, request, response);
    CFArrayRef streams;
    CFIndex i;

    if (err || !response || !*response || !mcn_active() || !cf_ready()) {
        return err;
    }
    /* The first SETUP of a session sets up timing and events and answers their ports. */
    if (cf_dict_get(*response, "eventPort") && !mcn_arrows_only()) {
        enable_features((CFMutableDictionaryRef)*response);
    }
    if (cf_dict_get(*response, "eventPort")) {
        mcn_route_session(1);
    }
    streams = cf_dict_get(request, "streams");
    for (i = 0; i < cf_array_count(streams); i++) {
        if (stream_type(cf_array_get(streams, i)) == MCN_STREAM_TYPE) {
            setup_cluster(session, cf_array_get(streams, i), (CFMutableDictionaryRef)*response);
        }
    }
    return err;
}

MCN_EXPORT void AirPlayReceiverSessionTearDown(void *session, CFDictionaryRef request,
                                               OSStatus reason, Boolean *done)
{
    void (*real)(void *, CFDictionaryRef, OSStatus, Boolean *) =
        MCN_REAL(AirPlayReceiverSessionTearDown);
    CFArrayRef streams = request && cf_ready() ? cf_dict_get(request, "streams") : NULL;
    CFIndex i, n = cf_array_count(streams);
    int ours = n == 0; /* no stream list: the whole session goes */

    for (i = 0; i < n && !ours; i++) {
        ours = stream_type(cf_array_get(streams, i)) == MCN_STREAM_TYPE;
    }
    if (ours) {
        mcn_stream_stop();
    }
    if (!n) {
        mcn_route_session(0);
    }
    real(session, request, reason, done);
}

const char mcn_component[] = "sal";

MCN_EXPORT void mcn_init(void)
{
    mcn_log("SAL hooks loaded");
    mcn_guard_init();
    mcn_stream_init();
    mcn_vpu_init();
    mcn_lab_start();
    mcn_route_start_worker();
}
