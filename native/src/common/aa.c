/*
 * Android Auto navigation status and now playing -> PhoneLink snapshots, independent of the
 * receiver ABI.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * Enum values and field numbers: the constants of the unit's libext.google.gal.receiver.so
 * (TurnSide LEFT 1, RIGHT 2, UNSPECIFIED 3; NextTurnEnum 0..19; ev_2f3ab597). The AA event and
 * side are turned into the iAP2 maneuver types the HMI already draws (Maneuvers.java).
 *
 * Traffic side: nothing in the library says whether a roundabout's turn_side is the way traffic
 * goes round it, nor what turn_angle is measured from (only the enum and field constants exist),
 * so right-hand traffic is assumed (lht=0) and turn_side is not read as handedness. turn_angle
 * of ROUNDABOUT_ENTER_AND_EXIT is taken as the public description of the field has it: degrees
 * travelled round the roundabout from the entry to the exit, 0..359 (counter-clockwise under
 * right-hand traffic), -1 when absent.
 */
#include "mcn_route.h"

enum { LEFT = 1, RIGHT = 2 };

/*
 * Copies len bytes of src into dst[size] as one line of text: cut on a UTF-8 character
 * boundary, line breaks turned into spaces (they would end the PhoneLink line).
 */
void mcn_aa_copy(char *dst, unsigned size, const char *src, unsigned len)
{
    unsigned i;

    if (!size) {
        return;
    }
    if (!src) {
        len = 0;
    }
    if (len >= size) {
        len = size - 1;
        while (len && ((unsigned char)src[len] & 0xc0) == 0x80) {
            len--;
        }
    }
    for (i = 0; i < len; i++) {
        dst[i] = src[i] == '\n' || src[i] == '\r' ? ' ' : src[i];
    }
    dst[len] = 0;
}

void mcn_aa_reset(struct mcn_aa *a)
{
    memset(a, 0, sizeof(*a));
    a->meters = -1;
    a->number = a->angle = -1;
}

void mcn_aa_status(struct mcn_aa *a, int status)
{
    a->active = status == 1; /* ACTIVE 1, INACTIVE 2, UNAVAILABLE 0 */
    if (!a->active) {
        a->event = 0;
        a->road[0] = a->street[0] = 0;
        a->meters = -1;
    }
}

void mcn_aa_turn(struct mcn_aa *a, const char *road, unsigned len, int side, int event,
                 int number, int angle)
{
    char text[MCN_RG_TEXT];

    mcn_aa_copy(text, sizeof(text), road, len);
    if (event == a->event && side == a->side && number == a->number && angle == a->angle &&
        strcmp(text, a->road) == 0) {
        return;
    }
    /* A new maneuver this close to the old one: the old one was passed, its road is ours. */
    if (a->road[0] && a->meters >= 0 && a->meters <= MCN_RG_PASSED_M && strcmp(text, a->road)) {
        memcpy(a->street, a->road, sizeof(a->street));
    }
    memcpy(a->road, text, sizeof(a->road));
    a->event = event;
    a->side = side;
    a->number = number;
    a->angle = angle;
    a->meters = -1;
    a->step = 0;
}

void mcn_aa_distance(struct mcn_aa *a, int meters)
{
    a->meters = meters;
    if (meters > a->step) {
        a->step = meters;
    }
}

/* iAP2 maneuver type for the HMI, -1 for none; *angle (iAP2 degrees, negative = left, 0 = none)
 * only where the picture needs it. */
static int aa_type(const struct mcn_aa *a, int *angle)
{
    int left = a->side == LEFT, right = a->side == RIGHT;

    *angle = 0;
    switch (a->event) {
    case 1:
        return 11; /* DEPART: start route */
    case 2:
    case 10:
        return 5; /* NAME_CHANGE, MERGE: follow the road */
    case 3:
        return left ? 49 : right ? 50 : 5; /* SLIGHT_TURN */
    case 4:
        return left ? 1 : right ? 2 : 5; /* TURN */
    case 5:
        return left ? 47 : right ? 48 : 5; /* SHARP_TURN */
    case 6:
        /* U_TURN: to the side given, else the HMI's default for right-hand traffic (left). */
        *angle = left ? -180 : right ? 180 : 0;
        return 4;
    case 7:
        *angle = left ? -45 : 45;
        return 9; /* ON_RAMP */
    case 8:
        return left ? 22 : right ? 23 : 8; /* OFF_RAMP */
    case 9:
        return left ? 13 : right ? 14 : 5; /* FORK: keep left or right */
    case 11:
        return 6; /* ROUNDABOUT_ENTER */
    case 12:
        return 7; /* ROUNDABOUT_EXIT */
    case 13: /* ROUNDABOUT_ENTER_AND_EXIT: exit 1..19, its direction from turn_angle */
        if (a->number < 1 || a->number > 19) {
            return 6;
        }
        if (a->angle >= 0 && a->angle < 360) {
            *angle = 180 - a->angle; /* 90 travelled: right, 180: ahead, 270: left */
        }
        return 27 + a->number;
    case 14:
        return 3; /* STRAIGHT */
    case 16:
    case 17:
        return 15; /* FERRY_BOAT, FERRY_TRAIN */
    case 19:
        return left ? 24 : right ? 25 : 12; /* DESTINATION */
    default:
        return -1;
    }
}

int mcn_aa_text(const struct mcn_aa *a, char *out, unsigned size)
{
    int angle, type, n;

    if (!a->active) {
        n = snprintf(out, size, "active=0\n");
        return n > 0 && (unsigned)n < size ? n : -1;
    }
    type = aa_type(a, &angle);
    n = snprintf(out, size, "active=1\nlht=0\nstreet=%s\n", a->street);
    if (n > 0 && (unsigned)n < size && type >= 0) {
        n += snprintf(out + n, size - (unsigned)n, angle ? "type=%d\nangle=%d\n" : "type=%d\n",
                      type, angle);
    }
    /* Always a distance, -1 when unknown: without it the HMI would keep 0 (no bar, no text). */
    if (n > 0 && (unsigned)n < size) {
        n += snprintf(out + n, size - (unsigned)n, "dist=%d\nstep=%d\n",
                      a->meters >= 0 ? a->meters : -1, a->meters >= 0 ? a->step : 0);
    }
    return n > 0 && (unsigned)n < size ? n : -1;
}

/*
 * PhoneLink now-playing datagram of Android Auto: "media=1" with title, artist and album (each
 * already one line, mcn_aa_copy) and "src=aa"; 0 without a title, -1 if out is too small.
 */
int mcn_aa_media_text(const char *title, const char *artist, const char *album, char *out,
                      unsigned size)
{
    int n;

    if (!title[0]) {
        return 0;
    }
    n = snprintf(out, size, "media=1\ntitle=%s\nartist=%s\nalbum=%s\nsrc=aa\n", title, artist,
                 album);
    return n > 0 && (unsigned)n < size ? n : -1;
}
