/*
 * CarPlay Route Guidance TLVs -> PhoneLink snapshots. No link-layer injection.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * Public wire schema and remaining on-car unknowns: mcn_route.h, architecture document.
 */
#include "mcn_route.h"

static unsigned be16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Saturate u32/u64 into the HMI's signed numeric range; never wrap unknown sentinels. */
static int number(const uint8_t *p, unsigned n)
{
    unsigned v = 0, i;

    for (i = 0; i < n; i++) {
        if (v > 0x7fffffu) {
            return 0x7fffffff;
        }
        v = (v << 8) | p[i];
        if (v > 0x7fffffffu) {
            return 0x7fffffff;
        }
    }
    return (int)v;
}

static int valid(const uint8_t *p, unsigned n)
{
    unsigned k;

    while (n) {
        if (n < 4 || (k = be16(p)) < 4 || k > n) {
            return 0;
        }
        p += k;
        n -= k;
    }
    return 1;
}

static const uint8_t *field(const uint8_t *p, unsigned n, unsigned id,
                            unsigned *size)
{
    while (n) {
        unsigned k = be16(p);

        if (be16(p + 2) == id) {
            *size = k - 4;
            return p + 4;
        }
        p += k;
        n -= k;
    }
    *size = 0;
    return NULL;
}

static int value(const uint8_t *p, unsigned n, unsigned id, unsigned width,
                 int fallback)
{
    unsigned size;
    const uint8_t *v = field(p, n, id, &size);

    return v && size == width ? number(v, size) : fallback;
}

static void string(char *out, const uint8_t *p, unsigned n)
{
    unsigned i = 0;

    while (i < n && i < MCN_RG_TEXT - 1 && p[i]) {
        /* PhoneLink is line-oriented. Never let a road name introduce another key. */
        out[i] = p[i] < 32 || p[i] == 127 ? ' ' : (char)p[i];
        i++;
    }
    /* Do not split a UTF-8 character at the byte limit. */
    if (i < n && i == MCN_RG_TEXT - 1) {
        while (i && (p[i] & 0xc0) == 0x80) {
            i--;
        }
    }
    out[i] = 0;
}

void mcn_route_reset(struct mcn_route *r)
{
    memset(r, 0, sizeof(*r));
    r->current = r->lane_index = -1;
    r->distance = r->remaining = r->destination = -1;
}

static struct mcn_maneuver *maneuver(struct mcn_route *r, int index)
{
    unsigned i;

    for (i = 0; i < MCN_RG_CACHE; i++) {
        if (r->maneuvers[i].valid && r->maneuvers[i].index == index) {
            return &r->maneuvers[i];
        }
    }
    return NULL;
}

/*
 * The road a maneuver leads onto: field 4. Yandex Maps leaves it out and names the road in the
 * description ("Первая ул.", car, 2026-10-26); that is used only when the app named no road
 * after any maneuver of this route, because Apple Maps puts instructions ("Поверните направо")
 * there.
 */
static const char *target(const struct mcn_route *r, const struct mcn_maneuver *m)
{
    if (m->street[0]) {
        return m->street;
    }
    return !r->after_roads ? m->description : "";
}

static int same(const struct mcn_maneuver *a, const struct mcn_maneuver *b)
{
    return b && a->type == b->type && !strcmp(a->street, b->street) &&
           !strcmp(a->description, b->description);
}

/* A maneuver left near its point is passed once the current one is known and differs. */
static void settle(struct mcn_route *r)
{
    const struct mcn_maneuver *cur = maneuver(r, r->current);

    if (r->pending && cur) {
        if (!same(&r->left, cur)) {
            snprintf(r->passed, sizeof(r->passed), "%s", target(r, &r->left));
        }
        r->pending = 0;
    }
}

int mcn_route_message(struct mcn_route *r, const uint8_t *msg, unsigned n,
                      unsigned now)
{
    const uint8_t *p, *f;
    unsigned id, size, left;
    int component = 0, ours = 0;

    if (n < 6 || msg[0] != 0x40 || msg[1] != 0x40 || be16(msg + 2) != n ||
        !valid(msg + 6, n - 6)) {
        return 0;
    }
    id = be16(msg + 4);
    if (id != 0x5201 && id != 0x5202 && id != 0x5204) {
        return 0;
    }
    p = msg + 6;
    left = n - 6;
    /* An omitted component means all displays; repeated component IDs form a set. */
    while (left) {
        size = be16(p);
        if (be16(p + 2) == 0) {
            if (size != 6) {
                return 0;
            }
            component = 1;
            ours |= be16(p + 4) == MCN_RG_COMPONENT;
        }
        if (id == 0x5204 && be16(p + 2) == 2 && !valid(p + 4, size - 4)) {
            return 0;
        }
        p += size;
        left -= size;
    }
    if (component && !ours) {
        return 0;
    }
    p = msg + 6;
    left = n - 6;
    /* Validate variable-width fields before changing the route. */
    f = field(p, left, 13, &size);
    if (id == 0x5201 && f && (size & 1)) {
        return 0;
    }
    if (id == 0x5201) {
        int state = value(p, left, 1, 1, r->state);
        int before = r->distance;

        if (state > 6) {
            return 0;
        }
        if (!state) {
            char passed[MCN_RG_TEXT];

            /*
             * A reroute stops guidance and starts it again (Yandex Maps: state 0, 3, then 1 with
             * the same maneuver under a new index, hang1 log); the car is still on its road.
             */
            memcpy(passed, r->passed, sizeof(passed));
            mcn_route_reset(r);
            memcpy(r->passed, passed, sizeof(passed));
        }
        r->state = state;
        r->rerouted |= state == 5;
        r->pending &= state != 5; /* a reroute replaced it, not the road */
        r->active = state != 0;
        r->distance = value(p, left, 10, 4, r->distance);
        r->remaining = value(p, left, 6, 8, r->remaining);
        r->destination = value(p, left, 7, 4, r->destination);
        f = field(p, left, 3, &size);
        if (f) {
            char road[MCN_RG_TEXT];

            /* Apple Maps sends it empty now and then (28 of 59, ring-wait log): keep the last. */
            string(road, f, size);
            if (road[0]) {
                memcpy(r->road, road, sizeof(road));
            }
        }
        f = field(p, left, 13, &size);
        if (f) {
            if (size & 1) {
                return 0;
            }
            if (size) {
                int next = (int)be16(f);

                if (next != r->current) {
                    const struct mcn_maneuver *was = maneuver(r, r->current);

                    /*
                     * Passed: replaced within MCN_RG_PASSED_M by a different maneuver, not by a
                     * reroute. Yandex Maps gives every update a new index, the same maneuver
                     * included (car, 2026-10-26: one turn as 19, 20, 21, 22 from 890 m to 80 m),
                     * so "different" waits for the new index's 0x5202 (settle). An unnamed road
                     * clears the street rather than keep the one left behind.
                     */
                    if (was && !r->rerouted && before >= 0 && before <= MCN_RG_PASSED_M) {
                        memcpy(&r->left, was, sizeof(r->left));
                        r->pending = 1;
                    }
                    r->rerouted = 0;
                }
                r->current = next;
                r->empty = 0;
            } else if (!r->empty) {
                r->empty = 1;
                r->empty_ms = now;
            }
        }
        r->lane_index = value(p, left, 16, 2, r->lane_index);
        r->show_lanes = value(p, left, 18, 1, r->show_lanes) == 1;
    } else if (id == 0x5202) {
        int index = value(p, left, 1, 2, -1);
        struct mcn_maneuver *m;

        if (index < 0) {
            return 0;
        }
        m = maneuver(r, index);
        if (!m) {
            m = &r->maneuvers[r->next++ % MCN_RG_CACHE];
            memset(m, 0, sizeof(*m));
            m->index = index;
            m->type = -1;
            m->angle = 1000;
            m->valid = 1;
        }
        m->type = value(p, left, 3, 1, m->type);
        m->lht = value(p, left, 8, 1, m->lht) == 1;
        m->step = value(p, left, 5, 4, m->step);
        f = field(p, left, 11, &size);
        if (f && size == 2) {
            m->angle = (int)(int16_t)be16(f);
        }
        f = field(p, left, 4, &size);
        if (f) {
            string(m->street, f, size);
            r->after_roads |= m->street[0] != 0;
        }
        f = field(p, left, 2, &size);
        if (f) {
            string(m->description, f, size);
        }
        /* Angle fields can repeat; absence means an incremental update. */
        f = field(p, left, 10, &size);
        if (f) {
            m->road_count = 0;
            while (left) {
                size = be16(p);
                if (be16(p + 2) == 10 && size == 6 &&
                    m->road_count < MCN_RG_ROADS) {
                    m->roads[m->road_count++] = (int)(int16_t)be16(p + 4);
                }
                p += size;
                left -= size;
            }
        }
    } else {
        int index = value(p, left, 1, 2, -1);
        struct mcn_lane_info *l = NULL;
        unsigned i;

        if (index < 0) {
            return 0;
        }
        for (i = 0; i < MCN_RG_CACHE; i++) {
            if (r->lanes[i].valid && r->lanes[i].index == index) {
                l = &r->lanes[i];
            }
        }
        if (!l) {
            l = &r->lanes[r->lane_next++ % MCN_RG_CACHE];
        }
        memset(l, 0, sizeof(*l));
        l->valid = 1;
        l->index = index;
        while (left) {
            size = be16(p);
            if (be16(p + 2) == 2 && l->count >= MCN_RG_LANES) {
                l->valid =
                    0; /* Never silently present a truncated road layout. */
            }
            if (be16(p + 2) == 2 && l->count < MCN_RG_LANES) {
                struct mcn_lane *lane = &l->lanes[l->count++];

                lane->index = value(p + 4, size - 4, 0, 2, -1);
                lane->status = value(p + 4, size - 4, 1, 1, 0);
                lane->highlight =
                    (int)(int16_t)value(p + 4, size - 4, 3, 2, 1000);
                {
                    const uint8_t *a = p + 4;
                    unsigned remaining = size - 4;

                    while (remaining) {
                        unsigned length = be16(a);

                        if (be16(a + 2) == 2 && length == 6) {
                            if (lane->angle_count < MCN_RG_LANE_ANGLES) {
                                lane->angles[lane->angle_count++] =
                                    (int)(int16_t)be16(a + 4);
                            } else {
                                l->valid = 0;
                            }
                        }
                        a += length;
                        remaining -= length;
                    }
                }
            }
            p += size;
            left -= size;
        }
    }
    settle(r);
    /* Only route status is a heartbeat: stale cached maneuvers cannot keep a route alive. */
    if (id == 0x5201) {
        r->seen = 1;
        r->updated_ms = now;
    }
    return 1;
}

/*
 * The cluster's street line: the road the car is on. The app's current road first; Yandex Maps
 * sends none (car, 2026-10-26), so then the road the last passed maneuver led onto. Before the
 * first maneuver is passed nothing is shown: the next maneuver's road is not where the car is.
 */
static const char *street(const struct mcn_route *r)
{
    return r->road[0] ? r->road : r->passed;
}

int mcn_route_owns(const struct mcn_route *r, unsigned now)
{
    /*
     * The silence limit is the fallback end of guidance. A phone pulled out normally ends the
     * AirPlay session through TEARDOWN (the plug-in's connection close calls it with no request,
     * reason kConnectionErr: libext.apple.carplay.airplay.so 0x20874-0x20894), which the SAL
     * hook turns into the end of the session.
     */
    return r->seen && r->active && now - r->updated_ms < MCN_RG_SILENT_MS &&
           (r->state != 2 || now - r->updated_ms < MCN_RG_ARRIVED_MS);
}

int mcn_route_text(const struct mcn_route *r, unsigned now, char *out,
                   unsigned size)
{
    const struct mcn_maneuver *m = NULL;
    char roads[128];
    unsigned i, used = 0;
    int n;

    roads[0] = 0;
    if (!mcn_route_owns(r, now)) {
        return snprintf(out, size, "active=0\n");
    }
    if (!r->empty || now - r->empty_ms < MCN_RG_EMPTY_MS) {
        for (i = 0; i < MCN_RG_CACHE; i++) {
            if (r->maneuvers[i].valid && r->maneuvers[i].index == r->current) {
                m = &r->maneuvers[i];
            }
        }
    }
    if (m) {
        for (i = 0; i < (unsigned)m->road_count; i++) {
            n = snprintf(roads + used, sizeof(roads) - used, "%s%d",
                         i ? "," : "", m->roads[i]);
            if (n < 0 || (unsigned)n >= sizeof(roads) - used) {
                break;
            }
            used += (unsigned)n;
        }
    }
    n = snprintf(
        out, size,
        "active=1\ntype=%d\nangle=%d\nlht=%d\nroads=%s\ndist=%d\nstep=%d\n"
        "street=%s\nrtt=%d\ndest=%d\n",
        r->state == 2 ? 10 : (m ? m->type : -1), m ? m->angle : 1000,
        m ? m->lht : 0, roads, r->distance, m ? m->step : 0,
        street(r), r->remaining, r->destination);
    if (n < 0 || (unsigned)n >= size) {
        return n;
    }
    used = (unsigned)n;
    {
        const struct mcn_lane_info *lanes = NULL;

        /* Lane instructions have their own current index and visibility flag.
         * An empty maneuver list must not hide highway pre-positioning lanes.
         */
        if (r->show_lanes && r->state != 2) {
            for (i = 0; i < MCN_RG_CACHE; i++) {
                if (r->lanes[i].valid && r->lanes[i].index == r->lane_index) {
                    lanes = &r->lanes[i];
                }
            }
        }
        n = snprintf(out + used, size - used, "lanes=%d\n",
                     lanes ? lanes->count : 0);
        if (n < 0 || (unsigned)n >= size - used) {
            return -1;
        }
        used += (unsigned)n;
        if (lanes) {
            for (i = 0; i < (unsigned)lanes->count; i++) {
                const struct mcn_lane *lane = &lanes->lanes[i];
                unsigned j;

                n = snprintf(out + used, size - used, "lane%u=%d,%d,%d", i,
                             lane->index, lane->status, lane->highlight);
                if (n < 0 || (unsigned)n >= size - used) {
                    return -1;
                }
                used += (unsigned)n;
                for (j = 0; j < (unsigned)lane->angle_count; j++) {
                    n = snprintf(out + used, size - used, ",%d",
                                 lane->angles[j]);
                    if (n < 0 || (unsigned)n >= size - used) {
                        return -1;
                    }
                    used += (unsigned)n;
                }
                if (size - used < 2) {
                    return -1;
                }
                out[used++] = '\n';
                out[used] = 0;
            }
        }
    }
    return (int)used;
}

void mcn_route_log_reset(struct mcn_route_log *l)
{
    memset(l, 0, sizeof(*l));
    l->high = -1;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int log_byte(struct mcn_route_log *l, struct mcn_route *r, unsigned b,
                    unsigned now)
{
    if (l->used < 2 && b != 0x40) {
        l->used = 0;
        return 0;
    }
    l->packet[l->used++] = (uint8_t)b;
    if (l->used == 4) {
        l->want = be16(l->packet + 2);
        if (l->want < 6 || l->want > sizeof(l->packet)) {
            l->used = l->want = 0;
        }
    }
    if (l->want && l->used == l->want) {
        int result = mcn_route_message(r, l->packet, l->used, now);

        l->used = l->want = 0;
        return result;
    }
    return 0;
}

int mcn_route_log_feed(struct mcn_route_log *l, struct mcn_route *r,
                       const char *text, unsigned n, unsigned now)
{
    unsigned i;
    int count = 0;

    for (i = 0; i < n; i++) {
        const char start[] = "StartIdentification";
        int h = hex(text[i]);

        /* The stock known-message printer names 0x1d00 even though its dump is
         * not an unknown 0x52xx packet. Match across read boundaries as well.
         */
        if (text[i] == start[l->start_match]) {
            l->start_match++;
            if (l->start_match == sizeof(start) - 1) {
                mcn_route_reset(r);
                l->used = l->want = l->start_match = 0;
                count = MCN_RG_REIDENT;
            }
        } else {
            l->start_match = text[i] == start[0] ? 1 : 0;
        }
        if (h >= 0) {
            if (l->digits < 2) {
                l->high = l->high < 0 ? h : (l->high << 4) | h;
            }
            if (l->digits < 3) {
                l->digits++;
            }
        } else {
            int space = text[i] == ' ' || text[i] == '\r' || text[i] == '\n' ||
                        text[i] == '\t';

            /* print_packet frames the payload as " CTRL[40 40 .. XX] CKSUM[YY] )": the
             * last byte ends at the bracket, not at a space (phone-route-guidance.md §10).
             */
            if (l->digits == 2 && (space || text[i] == ']')) {
                count += log_byte(l, r, (unsigned)l->high, now);
            }
            if (!space || (l->digits && l->digits != 2)) {
                /* A label or the closing bracket ends a partial dump. Do not join
                 * unrelated packets.
                 */
                l->used = l->want = 0;
            }
            l->high = -1;
            l->digits = 0;
        }
    }
    return count;
}

unsigned mcn_route_component(uint8_t *out)
{
    const char name[] = "Instrument Cluster";
    unsigned n = 6, i;

    put16(out, 6);
    put16(out + 2, 0);
    put16(out + 4, MCN_RG_COMPONENT);
    put16(out + n, 4 + sizeof(name));
    put16(out + n + 2, 1);
    memcpy(out + n + 4, name, sizeof(name));
    n += 4 + sizeof(name);
    for (i = 2; i <= 8; i++) {
        put16(out + n, 6);
        put16(out + n + 2, i);
        put16(out + n + 4, i == 6 || i == 8 ? MCN_RG_CACHE : MCN_RG_TEXT - 1);
        n += 6;
    }
    return n;
}

unsigned mcn_route_start(uint8_t *out)
{
    const uint8_t start[] = {0x40, 0x40, 0, 12, 0x52, 0,
                             0,    6,    0, 0,  0,    MCN_RG_COMPONENT};

    memcpy(out, start, sizeof(start));
    return sizeof(start);
}

static int append(char *out, unsigned size, unsigned *used, const char *p,
                  unsigned n)
{
    if (*used >= size || n >= size - *used) {
        return 0;
    }
    memcpy(out + *used, p, n);
    *used += n;
    out[*used] = 0;
    return 1;
}

/* Keep existing message IDs, adding each required ID only once (hex or decimal). */
static int add_ids(char *out, unsigned size, unsigned *used, const char *p,
                   unsigned n, const unsigned *ids, unsigned count)
{
    unsigned pos = 0, seen = 0, i;

    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r')) {
        n--;
    }
    while (pos < n) {
        unsigned v = 0, base = 10;

        while (pos < n && (p[pos] == ' ' || p[pos] == '\t' || p[pos] == ',')) {
            pos++;
        }
        if (pos + 1 < n && p[pos] == '0' &&
            (p[pos + 1] == 'x' || p[pos + 1] == 'X')) {
            base = 16;
            pos += 2;
        }
        while (pos < n && hex(p[pos]) >= 0 && (unsigned)hex(p[pos]) < base) {
            v = v * base + (unsigned)hex(p[pos++]);
        }
        for (i = 0; i < count; i++) {
            if (v == ids[i]) {
                seen |= 1u << i;
            }
        }
        while (pos < n && p[pos] != ',') {
            pos++;
        }
    }
    if (!append(out, size, used, p, n)) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (!(seen & (1u << i))) {
            char text[16];
            int k =
                snprintf(text, sizeof(text), "%s0x%04x", n ? "," : "", ids[i]);

            if (!append(out, size, used, text, (unsigned)k)) {
                return 0;
            }
            n = 1;
        }
    }
    return append(out, size, used, "\n", 1);
}

static int config_ids(char *out, unsigned size, unsigned *used,
                      const char *send, unsigned ns, const char *recv,
                      unsigned nr)
{
    const unsigned sends[] = {0x5200, 0x5203},
                   recvs[] = {0x5201, 0x5202, 0x5204};

    return append(out, size, used, "sendmsgs=", 9) &&
           add_ids(out, size, used, send, ns, sends, 2) &&
           append(out, size, used, "recvmsgs=", 9) &&
           add_ids(out, size, used, recv, nr, recvs, 3);
}

/*
 * Now playing: with [nowplaying] enable=yes the stock driver announces 0x5000 in its
 * identification (feature bit 0x80, MessagesSent builder; mQbEMU notes ev_e1dd2d9c), asks the
 * phone for these attributes and publishes them to PPS for media.c. No artwork yet: it comes
 * by file transfer over the same iAP2 link.
 * ID budget (P0480T): ident_info_sendmsgs/recvmsgs (0xe270/0xdae8) list the stock IDs of each
 * enabled feature, then the [ident] ones, in 0x80 bytes, and drop cfg IDs past it. With every
 * feature bit on the stock lists hold 45 sent and 24 received IDs (NowPlaying 0x5000/02/03 and
 * 0x5001 among them), so ours, 2 and 3, still fit: 94 and 54 of 128 bytes. 0x5001 is a named
 * message: link_handle_iap2pkt calls nowplay_update_recv (0x16074), keeps r6 = 0 and the
 * packet goes back to the receive pool; recv_all drains it like any other packet.
 */
static const char *const np_keys[] = {"enable", "metadata", "playback"};
static const char *const np_lines[] = {
    "enable=yes\n", "metadata=Title,Artist,AlbumTitle\n", "playback=Status\n"};

/* Index in np_keys of the "key =" line at p, or -1. */
static int np_key(const char *p, unsigned n)
{
    unsigned k;

    for (k = 0; k < 3; k++) {
        unsigned len = (unsigned)strlen(np_keys[k]), at = len;

        if (n <= len || memcmp(p, np_keys[k], len) != 0) {
            continue;
        }
        while (at < n && (p[at] == ' ' || p[at] == '\t')) {
            at++;
        }
        if (at < n && p[at] == '=') {
            return (int)k;
        }
    }
    return -1;
}

int mcn_route_config(const char *in, unsigned len, char *out, unsigned size)
{
    unsigned pos = 0, used = 0, ns = 0, nr = 0, k, np_seen = 0;
    const char *send = "", *recv = "";
    int ident = 0, found = 0, nowplaying = 0, np_found = 0;

    while (pos < len) {
        unsigned start = pos, end, key;
        int np;

        while (pos < len && in[pos] != '\n') {
            pos++;
        }
        end = pos;
        if (pos < len) {
            pos++;
        }
        key = start;
        while (key < end && (in[key] == ' ' || in[key] == '\t')) {
            key++;
        }
        if (key < end && in[key] == '[') {
            if (ident && !config_ids(out, size, &used, send, ns, recv, nr)) {
                return -1;
            }
            ident = end - key >= 7 && memcmp(in + key, "[ident]", 7) == 0;
            if (ident && found) {
                return -1; /* Ambiguous repeated sections: leave the stock config alone. */
            }
            found |= ident;
            /* Leaving a [nowplaying] section: add any of enable/metadata/playback the stock
             * section did not have, before this new section header, or the driver never gets
             * the attribute list and now playing stays silent (code review). */
            if (nowplaying) {
                for (k = 0; k < 3; k++) {
                    if (!(np_seen & (1u << k)) &&
                        !append(out, size, &used, np_lines[k], (unsigned)strlen(np_lines[k]))) {
                        return -1;
                    }
                }
            }
            nowplaying = end - key >= 12 && memcmp(in + key, "[nowplaying]", 12) == 0;
            if (nowplaying) {
                np_seen = 0;
            }
            np_found |= nowplaying;
        }
        np = nowplaying ? np_key(in + key, end - key) : -1;
        if (np >= 0) {
            np_seen |= 1u << np;
            if (!append(out, size, &used, np_lines[np], (unsigned)strlen(np_lines[np]))) {
                return -1;
            }
            continue;
        }
        if (ident && end - key >= 8 &&
            (memcmp(in + key, "sendmsgs", 8) == 0 ||
             memcmp(in + key, "recvmsgs", 8) == 0)) {
            unsigned eq = key + 8;

            while (eq < end && (in[eq] == ' ' || in[eq] == '\t')) {
                eq++;
            }
            if (eq < end && in[eq] == '=') {
                unsigned v = eq + 1, stop = v;

                while (stop < end && in[stop] != '#' && in[stop] != ';' &&
                       in[stop] != '\r') {
                    stop++;
                }
                while (v < stop && (in[v] == ' ' || in[v] == '\t')) {
                    v++;
                }
                if (in[key] == 's') {
                    send = in + v;
                    ns = stop - v;
                } else {
                    recv = in + v;
                    nr = stop - v;
                }
                continue;
            }
        }
        if (!append(out, size, &used, in + start, pos - start)) {
            return -1;
        }
    }
    if (used && out[used - 1] != '\n' && !append(out, size, &used, "\n", 1)) {
        return -1;
    }
    /* A real [nowplaying] section was the last one: add its missing attribute lines now, before
     * the [ident] section we append below, so they belong to [nowplaying]. */
    if (nowplaying) {
        for (k = 0; k < 3; k++) {
            if (!(np_seen & (1u << k)) &&
                !append(out, size, &used, np_lines[k], (unsigned)strlen(np_lines[k]))) {
                return -1;
            }
        }
    }
    if (!found && !append(out, size, &used, "[ident]\n", 8)) {
        return -1;
    }
    if ((!found || ident) &&
        !config_ids(out, size, &used, send, ns, recv, nr)) {
        return -1;
    }
    /* No [nowplaying] section at all: add a complete one at the end. */
    if (!np_found) {
        if (!append(out, size, &used, "[nowplaying]\n", 13)) {
            return -1;
        }
        for (k = 0; k < 3; k++) {
            if (!append(out, size, &used, np_lines[k], (unsigned)strlen(np_lines[k]))) {
                return -1;
            }
        }
    }
    return (int)used;
}
