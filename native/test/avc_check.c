/*
 * mst2-cluster-nav - host check of the avcC to Annex-B conversion (src/sal/avc.c).
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * cc -DMCN_HOST -I src/sal test/avc_check.c src/sal/avc.c -o build/avc_check && build/avc_check
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "avc.h"

int main(void)
{
    /* avcC: version 1, High@3.1, 4-byte lengths, one SPS (3 bytes), one PPS (2 bytes). */
    static const uint8_t cfg[] = {1, 100, 0, 31, 0xff, 0xe1, 0, 3, 0x67, 0xaa, 0xbb,
                                  1, 0, 2, 0x68, 0xcc};
    static const uint8_t idr[] = {0, 0, 0, 2, 0x65, 0x11, 0, 0, 0, 1, 0x06};
    static const uint8_t p[] = {0, 0, 0, 3, 0x41, 0x22, 0x33};
    static const uint8_t want_idr[] = {0, 0, 0, 1, 0x67, 0xaa, 0xbb, 0, 0, 0, 1, 0x68, 0xcc,
                                       0, 0, 0, 1, 0x65, 0x11, 0, 0, 0, 1, 0x06};
    static const uint8_t want_p[] = {0, 0, 0, 1, 0x41, 0x22, 0x33};
    static const uint8_t bad[] = {0, 0, 0, 9, 0x41};
    static const uint8_t bad_pps[] = {1, 100, 0, 31, 0xfd, 0xe1, 0, 3, 0x67, 0x11, 0x22,
                                      1, 0, 5, 0x68};
    static const uint8_t len3[] = {1, 100, 0, 31, 0xfe, 0xe1, 0, 3, 0x67, 0x11, 0x22,
                                   1, 0, 2, 0x68, 0xcc};
    struct avc a = {0};
    uint8_t out[64];
    int key;
    unsigned n;

    assert(avc_annexb(&a, p, sizeof(p), out, sizeof(out), &key) == 0); /* no configuration */
    assert(avc_config(&a, cfg, sizeof(cfg)) && a.len_size == 4 && a.ps_len == 13);
    n = avc_annexb(&a, idr, sizeof(idr), out, sizeof(out), &key);
    assert(key && n == sizeof(want_idr) && !memcmp(out, want_idr, n));
    n = avc_annexb(&a, p, sizeof(p), out, sizeof(out), &key);
    assert(!key && n == sizeof(want_p) && !memcmp(out, want_p, n));
    assert(avc_annexb(&a, bad, sizeof(bad), out, sizeof(out), &key) == 0);
    assert(avc_annexb(&a, idr, sizeof(idr), out, 10, &key) == 0); /* does not fit */
    assert(!avc_config(&a, cfg, 9) && a.ps_len == 13);              /* truncated keeps old */
    /* A failure after the SPS was parsed (PPS cut short), with 2-byte lengths and another SPS,
     * and lengthSizeMinusOne = 2 (3-byte lengths, not allowed): both leave everything as it was. */
    assert(!avc_config(&a, bad_pps, sizeof(bad_pps)));
    assert(a.len_size == 4 && a.ps_len == 13 && !memcmp(a.ps, want_idr, 13));
    assert(!avc_config(&a, len3, sizeof(len3)));
    assert(a.len_size == 4 && a.ps_len == 13 && !memcmp(a.ps, want_idr, 13));
    n = avc_annexb(&a, idr, sizeof(idr), out, sizeof(out), &key);
    assert(key && n == sizeof(want_idr) && !memcmp(out, want_idr, n));
    puts("avc: ok");
    return 0;
}
