/*
 * CarPlay now playing: the stock iAP2 driver's PPS object -> one PhoneLink datagram.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * The driver publishes "attribute:encoding:value" lines to MCN_MEDIA_PPS once [nowplaying] is
 * enabled (mcn_route_config); attribute names from the driver's strings (mQbEMU notes,
 * ev_9b065e54). PlaybackStatus is written as "PlaybackStatus::<name>" (nowplay_update_recv
 * 0x1f3fc-0x1f538, format "%s::%s\n"): 0 Stopped, 1 Playing, 2 Paused, 3 SeekForward,
 * 4 SeekBackward, else Unknown. The driver has no format that deletes an attribute; a "-name"
 * line (PPS deletion) is honoured anyway.
 *
 * Artwork: with ArtworkFileTransferIdentifier in [nowplaying] metadata= the driver itself
 * receives the image over the iAP2 file-transfer session (opened when nowplaying is on,
 * link_setup 0x1522c-0x15240, ev_06f4e0c8) and, once the whole file is in
 * (file_xfer_data 0xa874: received == size), publishes
 * "MediaItemArtworkFileTransferIdentifier::<id>" and "MediaItemArtworkSize::<size>"
 * (format "%s::%d", ev_9caef67c; size 0 for an empty transfer, file_xfer_setup 0xabc0-0xac44,
 * ev_e8f98e8a). The PPS writes only merge lines (pps_update_bulk), so the pair of an earlier
 * track stays until a new transfer completes: mcn_art_poll takes a pair as the current track's
 * only when it changed after the track's title did.
 */
#include "mcn_route.h"

static const char *const keys[] = {"MediaItemTitle", "MediaItemArtist", "MediaItemAlbumTitle",
                                   "PlaybackStatus", "MediaItemArtworkFileTransferIdentifier",
                                   "MediaItemArtworkSize"};
#define KEYS 6
static const char *const names[] = {"title=", "artist=", "album="};

/* The value of each of keys[] in the object, "" when absent. */
static void pps_values(const char *pps, unsigned len, char values[KEYS][MCN_MEDIA_TEXT])
{
    unsigned pos = 0, k;

    memset(values, 0, KEYS * MCN_MEDIA_TEXT);
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
        for (k = 0; k < KEYS; k++) {
            n = (unsigned)strlen(keys[k]);
            if (end - start >= n && memcmp(pps + start, keys[k], n) == 0 &&
                (deleted ? end - start == n : end - start > n && pps[start + n] == ':')) {
                break;
            }
        }
        if (k == KEYS) {
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
}

int mcn_media_text(const char *pps, unsigned len, char *out, unsigned size)
{
    char values[KEYS][MCN_MEDIA_TEXT];
    unsigned used = 0, k;

    pps_values(pps, len, values);
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

/* Decimal digits only, else 0 (an absent attribute). */
static unsigned number(const char *s)
{
    unsigned v = 0;

    for (; *s >= '0' && *s <= '9' && v < 100000000u; s++) {
        v = v * 10 + (unsigned)(*s - '0');
    }
    return *s ? 0 : v;
}

void mcn_art_reset(struct mcn_art *s)
{
    memset(s, 0, sizeof(*s));
}

unsigned mcn_art_poll(struct mcn_art *s, const char *pps, unsigned len, unsigned now_ms)
{
    char values[KEYS][MCN_MEDIA_TEXT];
    unsigned id, size;

    pps_values(pps, len, values);
    id = number(values[4]);
    size = number(values[5]);
    if (strcmp(values[0], s->title)) {
        memcpy(s->title, values[0], sizeof(s->title));
        s->base_id = s->prev_id;
        s->base_size = s->prev_size;
    }
    s->prev_id = id;
    s->prev_size = size;
    if (!size || (id == s->base_id && size == s->base_size)) {
        return 0;
    }
    if (id != s->file_id || size != s->file_size) {
        s->file_id = id;
        s->file_size = size;
        s->path[0] = 0;
        s->tries = size <= MCN_COVER_MAX ? 0 : MCN_ART_TRIES; /* too big: never fetched */
    } else if (s->path[0] || s->tries >= MCN_ART_TRIES ||
               now_ms - s->tried_ms < MCN_ART_RETRY_MS) {
        return 0;
    }
    /*
     * A new pair, or the same one again: the object only gains lines, so after a reconnect or
     * a new mm-ipod the old pair is still there before the new driver has that image, and the
     * resumed track's own transfer can publish the very same pair again.
     */
    if (s->tries >= MCN_ART_TRIES) {
        return 0;
    }
    s->tried_ms = now_ms;
    return size;
}

void mcn_art_fetched(struct mcn_art *s, const char *path, int tried)
{
    unsigned n = path ? (unsigned)strlen(path) : 0;

    if (path && n < sizeof(s->path)) {
        memcpy(s->path, path, n + 1);
        return;
    }
    s->path[0] = 0;
    /* Only a read that started counts; a driver without the image yet is asked again. */
    s->tries += tried ? 1u : 0u;
}

const char *mcn_art_current(const struct mcn_art *s)
{
    int current = s->prev_size && (s->prev_id != s->base_id || s->prev_size != s->base_size) &&
                  s->prev_id == s->file_id && s->prev_size == s->file_size;

    return current ? s->path : "";
}

#define COVER_PREFIX "mst2-cluster-nav-cover-"

static int cover_name(char *out, unsigned size, const char *dir, unsigned seq, const char *ext)
{
    int n = snprintf(out, size, "%s/" COVER_PREFIX "%u.%s", dir, seq, ext);

    return n > 0 && (unsigned)n < size;
}

/* Of the files this one replaces, the one two images back goes (the HMI may still be reading
 * the one before). */
static void cover_rotate(const char *dir, unsigned seq)
{
    static const char *const ext[] = {"jpg", "png", "tmp"};
    char path[MCN_COVER_PATH];
    unsigned k;

    for (k = 0; k < 3; k++) {
        if (cover_name(path, sizeof(path), dir, seq - 2, ext[k])) {
            unlink(path);
        }
    }
}

int mcn_cover_save(const char *dir, const uint8_t *data, unsigned n, unsigned now_ms, char *out,
                   unsigned size)
{
    static volatile unsigned seq;
    static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    const char *ext;
    char tmp[MCN_COVER_PATH];
    unsigned s, done = 0;
    int fd = -1, k = 0, ok = 0;

    if (n >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
        ext = "jpg";
    } else if (n >= 8 && memcmp(data, png, 8) == 0) {
        ext = "png";
    } else {
        return -1;
    }
    if (n > MCN_COVER_MAX) {
        return -1;
    }
    /* Two writers (CarPlay's worker, Android Auto's receiver thread) each get their own seq. */
    __sync_bool_compare_and_swap(&seq, 0u, now_ms ? now_ms : 1u);
    s = __sync_add_and_fetch(&seq, 1u);
    if (cover_name(tmp, sizeof(tmp), dir, s, "tmp") && cover_name(out, size, dir, s, ext)) {
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd >= 0) {
        while (done < n && (k = (int)write(fd, data + done, n - done)) > 0) {
            done += (unsigned)k;
        }
        close(fd);
        ok = done == n && rename(tmp, out) == 0;
        if (!ok) {
            unlink(tmp);
        }
    }
    cover_rotate(dir, s); /* on every path: a failed seq must not keep an older file alive */
    return ok ? 0 : -1;
}

void mcn_cover_clean(const char *dir)
{
    DIR *d = opendir(dir);
    char path[MCN_COVER_PATH];
    void *e;

    while (d && (e = readdir(d)) != NULL) {
        const char *name = MCN_DIRENT_NAME(e);

        if (strlen(name) >= sizeof(COVER_PREFIX) - 1 &&
            memcmp(name, COVER_PREFIX, sizeof(COVER_PREFIX) - 1) == 0 &&
            snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path)) {
            unlink(path);
        }
    }
    if (d) {
        closedir(d);
    }
}
