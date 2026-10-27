/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * The two extra services the head unit offers Android Auto for the instrument cluster, as raw
 * protobuf appended to the ServiceDiscoveryResponse. This version of libext.google.gal.receiver.so
 * speaks AAP 1.3, whose MediaSinkService has no display fields, so the cluster video sink cannot
 * be described by its own protobuf model; its protobuf-lite keeps unknown fields and serialises
 * them, so we append complete Service entries built here (sal/gal_video.c). Field numbers
 * and enum values are the public AAP schema, verified against the library's own constants
 * (mQbEMU research notes): MediaSinkService.available_type=1(VIDEO=3), video_configs=4,
 * display_id=6, display_type=7 (CLUSTER=1); VideoConfiguration resolution=1 (1280x720=2),
 * frame_rate=2 (30fps=2), width_margin=3, height_margin=4, density=5, pixel_aspect_ratio=8;
 * InputSourceService keycodes=1 (packed), display_id=5. Host-tested by decoding it back.
 */
#include "mcn_transcode.h"

#ifdef MCN_HOST
#include <string.h>
#endif

#define AA_RES_1280x720 2
#define AA_FPS_30       2
#define AA_TYPE_VIDEO   3
#define AA_DISPLAY_CLUSTER 1
#define AA_DENSITY      160
#define AA_ASPECT_E4    10000
/* InstrumentClusterInput up/down/left/right/enter/back are one key each; the cluster needs
 * none, but Android Auto is reported to stay silent on other head units without a cluster input
 * service. */
#define AA_KEY_ENTER    23
#define AA_KEY_HOME     65536

struct pb {
    uint8_t *data;
    unsigned used, cap;
    int bad;
};

static void pb_raw(struct pb *b, const uint8_t *p, unsigned n)
{
    if (b->bad || n > b->cap - b->used) {
        b->bad = 1;
        return;
    }
    memcpy(b->data + b->used, p, n);
    b->used += n;
}

static void pb_varint(struct pb *b, uint32_t v)
{
    uint8_t one;

    do {
        one = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) {
            one |= 0x80;
        }
        pb_raw(b, &one, 1);
    } while (v);
}

static void pb_int(struct pb *b, unsigned field, uint32_t v)
{
    pb_varint(b, field << 3); /* wire type 0 */
    pb_varint(b, v);
}

/* A length-delimited field (wire type 2) whose body is already in src[0..n). */
static void pb_bytes(struct pb *b, unsigned field, const uint8_t *src, unsigned n)
{
    pb_varint(b, (field << 3) | 2);
    pb_varint(b, n);
    pb_raw(b, src, n);
}

/*
 * Appends two Service entries (ServiceDiscoveryResponse.services = field 1) for the cluster to
 * out[0..cap): a video MediaSinkService with the margins from g, and an input service, numbered
 * video_id and input_id (none if input_id < 0). Returns the length, or 0 if it does not fit or
 * the mode has no margins.
 */
int mcn_aa_sdr(uint8_t *out, unsigned cap, int video_id, int input_id, const struct mcn_geom *g)
{
    uint8_t vcbuf[32], msbuf[64], inbuf[32], svcbuf[80];
    struct pb result = {out, 0, cap, 0};
    struct pb vc = {vcbuf, 0, sizeof(vcbuf), 0};
    struct pb ms = {msbuf, 0, sizeof(msbuf), 0};
    struct pb in = {inbuf, 0, sizeof(inbuf), 0};
    struct pb svc = {svcbuf, 0, sizeof(svcbuf), 0};
    uint8_t keys[8];
    struct pb kb = {keys, 0, sizeof(keys), 0};

    if (g->mode != MCN_TC_AA) {
        return 0;
    }
    /* VideoConfiguration: 1280x720, 30 fps, the margins that leave a 1024x384 content area. */
    pb_int(&vc, 1, AA_RES_1280x720);
    pb_int(&vc, 2, AA_FPS_30);
    pb_int(&vc, 3, (uint32_t)g->margin_w);
    pb_int(&vc, 4, (uint32_t)g->margin_h);
    pb_int(&vc, 5, AA_DENSITY);
    pb_int(&vc, 8, AA_ASPECT_E4);

    /* MediaSinkService: video, that one configuration, the cluster display. */
    pb_int(&ms, 1, AA_TYPE_VIDEO);
    pb_bytes(&ms, 4, vc.data, vc.used);
    pb_int(&ms, 6, 1); /* display_id */
    pb_int(&ms, 7, AA_DISPLAY_CLUSTER);

    /* Service { channel id; media_sink } */
    pb_int(&svc, 1, (uint32_t)video_id);
    pb_bytes(&svc, 3, ms.data, ms.used);
    pb_bytes(&result, 1, svc.data, svc.used);

    /* InputSourceService: the two cluster keycodes (packed), the cluster display; left out
     * when no service id was free for it (input_id < 0). */
    if (input_id >= 0) {
        pb_varint(&kb, AA_KEY_ENTER);
        pb_varint(&kb, AA_KEY_HOME);
        pb_bytes(&in, 1, kb.data, kb.used);
        pb_int(&in, 5, 1); /* display_id */

        svc.used = 0;
        pb_int(&svc, 1, (uint32_t)input_id);
        pb_bytes(&svc, 4, in.data, in.used);
        pb_bytes(&result, 1, svc.data, svc.used);
    }

    return result.bad || vc.bad || ms.bad || in.bad || svc.bad || kb.bad ? 0 : (int)result.used;
}
