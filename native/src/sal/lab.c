/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Lab input for the cluster video: when MCN_LAB_VIDEO exists (an H.264 Annex-B recording of a
 * phone's cluster stream, SPS and PPS in front of every IDR), it is played into the shared
 * memory at MCN_CLUSTER_FPS in a loop, the way stream.c publishes the phone's access units,
 * so DisplayManager and the HMI bundle see a phone without one. A key-frame request goes
 * back to the latest IDR. Never present on a car.
 */
#include "sal.h"
#include "mcn_video.h"

#define MAP_PRIVATE 0x0002
#define MAX_UNITS 65536

static const uint8_t *file;
static unsigned file_size, units;
static unsigned unit_start[MAX_UNITS + 1];
static uint8_t unit_idr[MAX_UNITS];

/* Splits the recording into access units (H.264 7.4.1.2.3: a new one starts at an AUD, SEI,
 * SPS or PPS, or at a slice with first_mb_in_slice 0, once the current one has a slice). */
static void split(void)
{
    unsigned i;
    int vcl = 0;

    units = 0;
    for (i = 0; i + 4 < file_size && units < MAX_UNITS; i++) {
        unsigned type;
        int starts;

        if (file[i] || file[i + 1] || file[i + 2] != 1) {
            continue;
        }
        type = file[i + 3] & 0x1f;
        starts = units == 0 || (vcl && (type == 9 || type == 6 || type == 7 || type == 8 ||
                                        ((type == 1 || type == 5) && (file[i + 4] & 0x80))));
        if (starts) {
            /* Include the zero byte of a 4-byte start code. */
            unit_start[units] = i && !file[i - 1] ? i - 1 : i;
            unit_idr[units] = 0;
            units++;
            vcl = 0;
        }
        if (type == 1 || type == 5) {
            vcl = 1;
        }
        if (type == 5) {
            unit_idr[units - 1] = 1;
        }
        i += 2;
    }
    unit_start[units] = file_size;
}

static void put(struct mcn_video *video, unsigned u)
{
    unsigned seq = video->write_seq + 1, size = unit_start[u + 1] - unit_start[u];
    struct mcn_video_slot *slot = &video->slot[seq % MCN_VIDEO_SLOTS];

    if (size > MCN_VIDEO_AU_MAX) {
        return;
    }
    slot->seq = 0;
    __sync_synchronize();
    memcpy(slot->data, file + unit_start[u], size);
    slot->size = size;
    slot->flags = unit_idr[u] ? MCN_VIDEO_IDR : 0;
    slot->pts_us = (unsigned long long)mcn_now_ms() * 1000u;
    __sync_synchronize();
    slot->seq = seq;
    __sync_synchronize(); /* the slot is complete before the reader can see it as the newest */
    video->write_seq = seq;
    video->last_frame_ms = mcn_now_ms();
    video->alive_ms = video->last_frame_ms; /* the recording's "connection" is open while it plays */
    mcn_video_wake(video);
}

static void *play(void *arg)
{
    struct mcn_video *video;
    unsigned u = 0, told = 0;

    (void)arg;
    usleep(20 * 1000 * 1000); /* let SAL come up first */
    video = mcn_video_map();
    if (!video) {
        return NULL;
    }
    video->write_seq = 0;
    __sync_synchronize(); /* as stream.c: the sequence restarts before the generation changes */
    video->generation++;
    mcn_log("lab video: playing");
    for (;;) {
        if (!mcn_active() || mcn_stream_live()) {
            /* Never mixed with a real phone's stream. */
            usleep(1000000);
            continue;
        }
        if (video->want_idr) {
            video->want_idr = 0;
            while (u && !unit_idr[u]) {
                u--;
            }
        }
        put(video, u);
        if (video->last_frame_ms - told >= 1000) {
            told = video->last_frame_ms;
            mcn_link_send("video=1");
        }
        u = u + 1 < units ? u + 1 : 0;
        usleep(1000000 / MCN_CLUSTER_FPS);
    }
    return NULL;
}

void mcn_lab_start(void)
{
    pthread_t thread;
    int fd = open(MCN_LAB_VIDEO, O_RDONLY);
    off_t size;
    void *p;

    if (fd < 0) {
        return;
    }
    size = lseek(fd, 0, SEEK_END);
    p = size > 0 ? mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) {
        mcn_log("lab video: cannot map " MCN_LAB_VIDEO);
        return;
    }
    file = p;
    file_size = (unsigned)size;
    split();
    if (!units || !unit_idr[0] || pthread_create(&thread, NULL, play, NULL)) {
        mcn_log("lab video: no key frame at the start, not played");
        return;
    }
    pthread_detach(thread);
}
