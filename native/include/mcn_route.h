/*
 * CarPlay route guidance, independent of the QNX transport.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * Wire schema: the public iAP2 route-guidance description; P0480T ABI evidence: mQbEMU
 * research notes (phone-route-guidance.md, sections 8-11).
 */
#ifndef MCN_ROUTE_H
#define MCN_ROUTE_H

#ifdef MCN_HOST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#define MCN_DIRENT_NAME(e) (((struct dirent *)(e))->d_name)
#else
#include "mcn_qnx.h"
#endif

#define MCN_RG_COMPONENT   0x10
#define MCN_RG_CACHE       6
#define MCN_RG_ROADS       16
#define MCN_RG_LANES       16
#define MCN_RG_LANE_ANGLES 16
#define MCN_RG_PACKET      8192
#define MCN_RG_SNAPSHOT    4096
#define MCN_RG_TEXT        256
#define MCN_RG_EMPTY_MS    3000
/* The phone sends route guidance only when it changes: Yandex Maps, parked, every ~6 s (car,
 * 2026-10-26). Short silence is no end of guidance; the arrival notice times out after
 * MCN_RG_ARRIVED_MS. */
#define MCN_RG_ARRIVED_MS  5000
/*
 * A route whose app has been silent this long is no longer shown (it comes back, with its
 * cached maneuvers, on the next status): the phone gone without a TEARDOWN, mm-ipod dead, the
 * app killed. 20 times the parked interval above; while moving the app sends several a second.
 */
#define MCN_RG_SILENT_MS   120000
#define MCN_RG_REIDENT     0x10000

struct mcn_maneuver {
    int valid, index, type, angle, lht, step;
    int roads[MCN_RG_ROADS], road_count;
    char street[MCN_RG_TEXT];
    char description[MCN_RG_TEXT];
};

struct mcn_lane {
    int index, status, highlight, angle_count;
    int angles[MCN_RG_LANE_ANGLES];
};

struct mcn_lane_info {
    int valid, index, count;
    struct mcn_lane lanes[MCN_RG_LANES];
};

/*
 * A maneuver counts as passed when the next one replaces it this close (metres to the maneuver
 * in the last status before): Yandex Maps switches at 10 m, at times 50 (car, 2026-10-26).
 */
#define MCN_RG_PASSED_M 50

struct mcn_route {
    struct mcn_maneuver maneuvers[MCN_RG_CACHE];
    struct mcn_lane_info lanes[MCN_RG_CACHE];
    unsigned next, lane_next, updated_ms, empty_ms;
    int seen, active, state, current, empty, distance, remaining, destination;
    int lane_index, show_lanes;
    /* The app named a road after a maneuver in this route (0x5202 field 4). */
    int after_roads;
    char road[MCN_RG_TEXT]; /* the app's current road (0x5201 field 3) */
    /* The road the last passed maneuver led onto: where the car is when the app names none. */
    char passed[MCN_RG_TEXT];
    int rerouted; /* state 5 since the current maneuver changed */
    /* A maneuver left within MCN_RG_PASSED_M whose successor is not cached yet: passed or not
     * is decided when it is (Yandex Maps repeats one maneuver under new indices). */
    int pending;
    struct mcn_maneuver left;
};

/* Complete, validated control messages only; invalid TLVs leave the state untouched. */
void mcn_route_reset(struct mcn_route *r);
int mcn_route_message(struct mcn_route *r, const uint8_t *msg, unsigned n,
                      unsigned now);
/* Guidance is on: a route is active, its last status younger than MCN_RG_SILENT_MS and, once
 * arrived, for MCN_RG_ARRIVED_MS more. */
int mcn_route_owns(const struct mcn_route *r, unsigned now);
int mcn_route_text(const struct mcn_route *r, unsigned now, char *out,
                   unsigned size);

/* Byte-stream reader of the driver's ASCII hex payload, including split reads/lines. */
struct mcn_route_log {
    uint8_t packet[MCN_RG_PACKET];
    unsigned used, want;
    int high;
    unsigned digits;
    unsigned start_match;
};
void mcn_route_log_reset(struct mcn_route_log *l);
int mcn_route_log_feed(struct mcn_route_log *l, struct mcn_route *r,
                       const char *text, unsigned n, unsigned now);
unsigned mcn_route_component(uint8_t *out);
unsigned mcn_route_start(uint8_t *out);
/*
 * Rewrites only ident sendmsgs/recvmsgs, preserving pre-existing IDs and other sections, and
 * enables [nowplaying] with the attributes media.c reads.
 */
int mcn_route_config(const char *in, unsigned len, char *out, unsigned size);

/*
 * Android Auto navigation status (NavigationStatusEndpoint of the unit's receiver, gal.c):
 * status, the next turn (event and side enums, road, roundabout exit number) and the distance.
 * The receiver's protocol 1.3 carries no time or distance to the destination.
 */
struct mcn_aa {
    int active, event, side, number, angle, meters, step;
    char road[MCN_RG_TEXT];   /* the road of the next turn, as the phone names it */
    char street[MCN_RG_TEXT]; /* the road of the turn passed last: the one the car is on */
};
void mcn_aa_reset(struct mcn_aa *a);
void mcn_aa_status(struct mcn_aa *a, int status);
void mcn_aa_turn(struct mcn_aa *a, const char *road, unsigned len, int side, int event,
                 int number, int angle);
void mcn_aa_distance(struct mcn_aa *a, int meters);
/* PhoneLink snapshot ("active=0" when inactive); -1 if out is too small. */
int mcn_aa_text(const struct mcn_aa *a, char *out, unsigned size);
/* len bytes of src as one line in dst[size], cut on a UTF-8 boundary. */
void mcn_aa_copy(char *dst, unsigned size, const char *src, unsigned len);
/* Android Auto now playing: "media=1" .. ["art=" art] "src=aa"; 0 without a title, -1 if out is
 * too small. art: the cover file, NULL or "" for none. */
int mcn_aa_media_text(const char *title, const char *artist, const char *album, const char *art,
                      char *out, unsigned size);
/* SAL (sal/gal.c): resends live Android Auto guidance about once a second. */
void mcn_gal_tick(unsigned now);

/* The stock driver's now-playing PPS object (directory string + "nowplaying", ev_9b065e54). */
#define MCN_MEDIA_PPS  "/pps/services/multimedia/iap2/nowplaying"
#define MCN_MEDIA_TEXT 200
/* "media=1\ntitle=..\nartist=..\nalbum=..\n" from the PPS object; 0 without a title, -1 if
 * out is too small. */
int mcn_media_text(const char *pps, unsigned len, char *out, unsigned size);

/*
 * Cover art for the HMI: a JPEG or PNG written atomically (temporary file, then rename) as
 * <dir>/mst2-cluster-nav-cover-<seq>.<jpg|png>, seq growing with each image (the first one is
 * now_ms, so a restarted SAL never reuses a path); the file two images back is deleted.
 */
#define MCN_COVER_DIR  "/dev/shmem"
#define MCN_COVER_MAX  (1024u * 1024u)
#define MCN_COVER_PATH 96
/* 0 and the path in out, -1 if data is no JPEG/PNG, bigger than MCN_COVER_MAX or not written. */
int mcn_cover_save(const char *dir, const uint8_t *data, unsigned n, unsigned now_ms, char *out,
                   unsigned size);
/* Removes every cover file in dir (SAL start, end of a session); nothing else is touched. */
void mcn_cover_clean(const char *dir);

/*
 * CarPlay artwork in the driver's PPS object (MediaItemArtworkFileTransferIdentifier and
 * MediaItemArtworkSize, written when a transfer completes). The object only ever gains lines, so
 * a pair counts for the current track only if it changed after the track's title appeared.
 */
struct mcn_art {
    unsigned prev_id, prev_size; /* the pair at the last poll */
    unsigned base_id, base_size; /* the pair before the current title appeared */
    unsigned file_id, file_size; /* the pair fetched last (path "" if that failed) */
    unsigned tries, tried_ms;    /* reads of that pair that failed, and when it was last asked */
    char title[MCN_MEDIA_TEXT];
    char path[MCN_COVER_PATH];
};
/* A failed fetch of the current pair is asked again every MCN_ART_RETRY_MS while it stays
 * current, until MCN_ART_TRIES reads of it failed. */
#define MCN_ART_RETRY_MS 5000
#define MCN_ART_TRIES    3
void mcn_art_reset(struct mcn_art *s);
/* One poll of the PPS object: the size to fetch now (the current track's artwork), else 0. */
unsigned mcn_art_poll(struct mcn_art *s, const char *pps, unsigned len, unsigned now_ms);
/* The fetch asked for by mcn_art_poll is done: the written file, or NULL if it failed; tried:
 * the driver had the image and the read failed (counts against MCN_ART_TRIES). */
void mcn_art_fetched(struct mcn_art *s, const char *path, int tried);
/* The current track's cover file, "" for none. */
const char *mcn_art_current(const struct mcn_art *s);

/* iap2_ctrl_msg_table row (indexed by the message ID's high byte) and its msg_info
 * entries (indexed by the low byte); 12 bytes each on the 32-bit QNX ABI (E23, E24). */
struct mcn_msg_info {
    const char *name;
    uint16_t flags, reply;
    const void *params;
};
struct mcn_msg_group {
    const char *name;
    struct mcn_msg_info *info;
    uint32_t count;
};
/* Declares the received 0x52xx messages in an empty group 0x52 row; -1 if it is not empty. */
int mcn_route_msg_group(struct mcn_msg_group *group);

#endif
