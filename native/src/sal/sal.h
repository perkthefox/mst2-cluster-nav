/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * The cluster screen as the phone sees it, and the stream-111 receiver (stream.c).
 */
#ifndef MCN_SAL_H
#define MCN_SAL_H

#include "cf.h"
#include "mcn_hook.h"

/*
 * The cluster's map window (FPK_E MOST video, mQbEMU research notes, cluster-nav-mod.md):
 * 1010 x 376 pixels. The cluster most likely stretches it over its whole 1280 x 480 LG 10.25"
 * panel (its 1010 x 376 and 1280 x 480 window masks match after that scale), so the physical
 * size offered is the panel's, about 244 x 91 mm. In the CarPlay Simulator (Yandex Maps) 192 to
 * 360 mm wide drew the same picture: the size only has to keep the aspect ratio.
 */
#define MCN_CLUSTER_WIDTH 1010
#define MCN_CLUSTER_HEIGHT 376
#define MCN_CLUSTER_WIDTH_MM 244
#define MCN_CLUSTER_HEIGHT_MM 91
#define MCN_CLUSTER_FPS 25
/*
 * One safe area for every cluster view (mQbEMU research notes, gc-map-views.md: the cluster
 * scales the frame to 1244 x 464 at (18, 8); views Klassisch, Erweitert, Modern show the map).
 * x 377..634 is what the round dials of Klassisch leave free, which is also visible in the wider
 * views; y 56..306 is below the status bar (panel lines 0..72: compass, clock or speed sign,
 * temperature) and above the bottom bar (panel 385..420). The height matters to the phone: in
 * the CarPlay Simulator Yandex Maps drew its maneuver card only with about 250 lines (186 or
 * 212 at this width: no card), and in this narrow area it keeps the car marker in the middle.
 */
#define MCN_SAFE_X 377
#define MCN_SAFE_Y 56
#define MCN_SAFE_WIDTH 257
#define MCN_SAFE_HEIGHT 250
/* Ours; must differ from the main screen's, which is how the phone tells 111 from 110. */
#define MCN_CLUSTER_UUID "4D434E00-0001-4000-8000-00000000006F"
/*
 * Scenes the car can ask for (CarPlay Simulator, Content): "Map" — the map with the route and the
 * car marker only; "Navigation App" — the app's own cluster UI, map with its maneuver card, which
 * hides much of the map in the cluster's window. Map is the default; GEM writes "app" to
 * MCN_SCENE_FILE for the card (experimental).
 */
#define MCN_CLUSTER_URL_MAP "maps:/car/instrumentcluster/map"
#define MCN_CLUSTER_URL_APP "maps:/car/instrumentcluster"
/* The scene chosen in GEM, read on every use (applies from the next connection). */
const char *mcn_cluster_url(void);
#define MCN_STREAM_TYPE 111
#define MCN_CLUSTER_PORT 6031
/*
 * Receive buffer of the cluster stream (stock 110: 180224). Smaller than stock: the receiver
 * reads at MCN_CLUSTER_KBPS, and what the buffer holds beyond that is what the phone may burst
 * over USB at once.
 */
#define MCN_SOCKET_BUFFER 65536
#define MCN_SOCKET_QOS 12 /* stock 110: SocketSetQoS(sock, 0xc) */
/*
 * Budget of the cluster stream, kbit/s; MCN_RATE_FILE ("10000", 1000..25000) replaces it. In the
 * car (2026-10-26, iPhone log) the phone sent the cluster 10-13 Mbit/s for seconds, up to 15.5
 * with the main screen, and then the unit stopped taking anything from the phone (all three
 * connections stalled at once, CarPlay dropped). Read slower, the phone's sink overflows and it
 * skips frames before encoding them ("dropping frame because sink says so"), so the stream
 * stays whole, at a lower frame rate; it does not make the frames smaller (the phone gives the
 * main screen and the cluster alike 2187500 B/s, from its own estimate of the link). With the
 * built-in cluster map hidden (KombiMap.java), 10000 ran four drives (3 to 10 min) without a
 * drop at 15-25 fps, DisplayManager waiting at most 13 ms for room in the MOST ring and cutting
 * no frame; 12000 filled that ring sooner (966 waits in 5 s) and looked less smooth, 4000 gave
 * about 5 fps on a moving map.
 */
#define MCN_CLUSTER_KBPS 10000

/* GEM "Native arrows only": no phone (CarPlay or Android Auto) is offered the cluster screen. */
int mcn_arrows_only(void);
/* All plug-in functions the receiver needs resolve: only then is the cluster offered. */
int mcn_stream_ready(void);
/* Address family of the phone's connection, for the listener (as the stock 110 one). */
int mcn_peer_family(void *session);
/* Returns the data port, or -1. Stops a previous stream first. */
int mcn_stream_start(void *session, const uint8_t key[16], uint64_t connection_id, int family);
void mcn_stream_stop(void);
/* The receivers' lock (stream.c), from mcn_init before any thread starts. */
void mcn_stream_init(void);
/* A phone's cluster stream is set up (its receiver runs). */
int mcn_stream_live(void);
/* The shared memory of include/mcn_video.h, created on first use; NULL if that fails. */
struct mcn_video *mcn_video_map(void);
/* After an access unit is published: wakes DisplayManager if it waits (include/mcn_video.h). */
struct mcn_video;
void mcn_video_wake(struct mcn_video *v);
/* Lab input: plays MCN_LAB_VIDEO into the shared memory as if the phone sent it, if it exists. */
void mcn_lab_start(void);
void mcn_route_start_worker(void);
void mcn_route_session(int up);
#define MCN_LAB_VIDEO MCN_DIR "/video.h264"
#define MCN_MODE_FILE MCN_DIR "/mode"
#define MCN_FPS_FILE MCN_DIR "/fps"
#define MCN_SCENE_FILE MCN_DIR "/scene"
#define MCN_KEY_FILE MCN_DIR "/keyframe"
#define MCN_DISPLAY_FILE MCN_DIR "/display"
#define MCN_RATE_FILE MCN_DIR "/kbps"
/*
 * CarPlay zoom-out factor x1000 (MCN_ZOOM_FILE, "1000".."1900"): 1000 = off, the phone encodes
 * 1010x376 and stream.c forwards it 1:1; above that the phone is offered a proportionally bigger
 * alt screen (so it draws more map) and the stream is re-encoded down to 1010x376 (sal/vpu.c).
 */
#define MCN_ZOOM_FILE MCN_DIR "/zoom"
/*
 * Zoom factor x1000 of the session: the one the last /info advertised (latched there), so the
 * receiver decodes exactly the screen size the phone was offered; 1000 when off.
 */
int mcn_cluster_zoom(void);
/* The transcoder's lock (sal/vpu.c), from mcn_init before any thread starts. */
void mcn_vpu_init(void);

#endif
