/*
 * CarPlay now playing: the stock iAP2 driver's PPS object -> one PhoneLink datagram.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * The driver publishes "attribute:encoding:value" lines to MCN_MEDIA_PPS once [nowplaying] is
 * enabled (mcn_route_config); attribute names from the driver's strings (mQbEMU notes,
 * ev_9b065e54). PlaybackStatus is written as "PlaybackStatus::<name>" (nowplay_update_recv
 * 0x1f3fc-0x1f538, format "%s::%s\n"): 0 Stopped, 1 Playing, 2 Paused, 3 SeekForward,
 * 4 SeekBackward, else Unknown. The driver has no format that deletes an attribute; a "-name"
 * line (PPS deletion) is honoured anyway.
 */
#include "mcn_route.h"

static const char *const keys[] = {"MediaItemTitle", "MediaItemArtist", "MediaItemAlbumTitle",
                                   "PlaybackStatus"};
static const char *const names[] = {"title=", "artist=", "album="};

int mcn_media_text(const char *pps, unsigned len, char *out, unsigned size)
{
    char values[4][MCN_MEDIA_TEXT];
    unsigned pos = 0, used = 0, k;

    memset(values, 0, sizeof(values));
    while (pos < len) {
        unsigned start = pos, end, colon, n;
        int deleted;

        while (pos < len && pps[pos] != '\n') {
            pos++;
        }
        end = pos++;
        if (end > start && pps[end - 1] == '\r') {
            end--;
        }
        deleted = end > start && pps[start] == '-';
        start += (unsigned)deleted;
        for (k = 0; k < 4; k++) {
            n = (unsigned)strlen(keys[k]);
            if (end - start >= n && memcmp(pps + start, keys[k], n) == 0 &&
                (deleted ? end - start == n : end - start > n && pps[start + n] == ':')) {
                break;
            }
        }
        if (k == 4) {
            continue;
        }
        if (deleted) {
            values[k][0] = 0;
            continue;
        }
        for (colon = start + n + 1; colon < end && pps[colon] != ':'; colon++) {
        }
        if (colon == end) {
            continue;
        }
        n = end - colon - 1;
        if (n >= MCN_MEDIA_TEXT) {
            /* Cut on a UTF-8 character boundary; the HMI shortens further for the cluster. */
            n = MCN_MEDIA_TEXT - 1;
            while (n && ((unsigned char)pps[colon + 1 + n] & 0xc0) == 0x80) {
                n--;
            }
        }
        memcpy(values[k], pps + colon + 1, n);
        values[k][n] = 0;
    }
    /* Paused or stopped: nothing is playing, so the cluster shows nothing either. */
    if (!values[0][0] || !strcmp(values[3], "Paused") || !strcmp(values[3], "Stopped")) {
        return 0;
    }
    if (size < 9) {
        return -1;
    }
    memcpy(out, "media=1\n", 9);
    used = 8;
    for (k = 0; k < 3; k++) {
        unsigned a = (unsigned)strlen(names[k]), v = (unsigned)strlen(values[k]);

        if (used + a + v + 2 > size) {
            return -1;
        }
        memcpy(out + used, names[k], a);
        memcpy(out + used + a, values[k], v);
        used += a + v;
        out[used++] = '\n';
        out[used] = 0;
    }
    return (int)used;
}
