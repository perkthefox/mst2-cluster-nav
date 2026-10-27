/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * H.264 framing: the phone sends the decoder configuration as an avcC record
 * (ISO/IEC 14496-15, AVCDecoderConfigurationRecord) and every access unit as length-prefixed
 * NAL units; the cluster path takes Annex-B (ITU-T H.264 Annex B) with SPS and PPS in front of
 * each IDR. No dependencies, so the host check (native/test/avc_check.c) builds the same file.
 */
#include "avc.h"

static void put_start(uint8_t *out)
{
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
}

int avc_config(struct avc *a, const uint8_t *p, unsigned n)
{
    struct avc c; /* parsed here, kept only when the whole record is valid */
    unsigned pos = 6, len = 0, set, count, i, size;

    if (n < 7 || p[0] != 1) {
        return 0;
    }
    c.len_size = (p[4] & 3u) + 1;
    if (c.len_size == 3) {
        return 0;
    }
    /* Two lists: the SPS count sits in p[5], the PPS count in the byte after the SPS. */
    for (set = 0, count = p[5] & 0x1fu; set < 2; set++) {
        for (i = 0; i < count; i++) {
            if (pos + 2 > n) {
                return 0;
            }
            size = (unsigned)p[pos] << 8 | p[pos + 1];
            pos += 2;
            if (!size || pos + size > n || len + 4 + size > sizeof(c.ps)) {
                return 0;
            }
            put_start(c.ps + len);
            for (unsigned k = 0; k < size; k++) {
                c.ps[len + 4 + k] = p[pos + k];
            }
            len += 4 + size;
            pos += size;
        }
        if (set == 0) {
            if (pos >= n) {
                return 0;
            }
            count = p[pos++];
        }
    }
    if (!len) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        a->ps[i] = c.ps[i];
    }
    a->ps_len = len;
    a->len_size = c.len_size;
    return 1;
}

unsigned avc_annexb(const struct avc *a, const uint8_t *in, unsigned n, uint8_t *out,
                    unsigned cap, int *idr)
{
    unsigned pos, size, k, len = 0, nals = 0, has_ps = 0;

    *idr = 0;
    if (!a->len_size) {
        return 0;
    }
    /* First pass: validate the lengths and find out whether the unit is a key frame. */
    for (pos = 0; pos < n; pos += size) {
        if (pos + a->len_size > n) {
            return 0;
        }
        for (size = 0, k = 0; k < a->len_size; k++) {
            size = size << 8 | in[pos + k];
        }
        pos += a->len_size;
        if (!size || size > n - pos) {
            return 0;
        }
        if ((in[pos] & 0x1f) == 5) {
            *idr = 1;
        } else if ((in[pos] & 0x1f) == 7) {
            has_ps = 1;
        }
        nals++;
    }
    if (!nals) {
        return 0;
    }
    if (*idr && !has_ps) {
        if (a->ps_len > cap) {
            return 0;
        }
        for (; len < a->ps_len; len++) {
            out[len] = a->ps[len];
        }
    }
    for (pos = 0; pos < n; pos += size) {
        for (size = 0, k = 0; k < a->len_size; k++) {
            size = size << 8 | in[pos + k];
        }
        pos += a->len_size;
        if (len + 4 + size > cap) {
            return 0;
        }
        put_start(out + len);
        for (k = 0; k < size; k++) {
            out[len + 4 + k] = in[pos + k];
        }
        len += 4 + size;
    }
    return len;
}
