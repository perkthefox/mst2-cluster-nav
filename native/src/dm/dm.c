/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Hooks in DisplayManager (tsd.mibstd2.system.displaymanager), which encodes the cluster map
 * and sends it to the cluster. Its reader thread pulls the encoder's output through the
 * OpenMAX AL buffer-queue interface of libOpenMAXAL.so.1 (vtable at 0x28cb8: GetFormat, Get,
 * GetAndWait, Done; DisplayManager calls entry 2). BufferQueueGetAndWait (local, 0x1ac24) calls
 * mmfbq_get_and_wait through libOpenMAXAL's PLT and BufferQueueDone calls mmfbq_done
 * (mQbEMU research notes, carplay-airplay-lib.md §4); the reader
 * hands every element's data (+4) and size (+8) to the TS packer. libOpenMAXAL also waits on
 * other queues (AcquireBuffer, local_read) whose elements have another layout, so only calls
 * returning into BufferQueueGetAndWait are looked at; the rest pass through untouched. While the
 * phone's cluster stream is alive the reader gets the phone's access units instead; the stock
 * frames are returned to the encoder at once so it keeps running. TS packing, the ring to the
 * J5, MOST and the cluster stay stock, except that MostSink waits for room in the ring.
 *
 * MostSink::write (0x126f04) copies each TS packet into the RingIO to the J5 with
 * RingIO::writeAquire(this, &p, &n, true) (libtsd.common.so), which hands out what is free
 * (header {write, read, size} at this+4; the J5's M31 moves read) and 0 when the ring is full;
 * MostSink then drops the rest of the frame ("MostSink: Ring buffer full! Lost ..."). The
 * phone's 30 fps reach 13-19 Mbit/s for a second after a key frame and when the map moves,
 * above what the channel drains, so the cluster's picture broke up. While the phone owns the
 * cluster, a full ring is waited for (RING_WAIT_MS) instead: a burst arrives a little late and
 * whole. A wait that runs out drops as stock and asks for a key frame at once; a reader that
 * does not move at all (cluster asleep) is not waited for again until a write gets through.
 *
 * Queue facts (mmfbq_*, 0480 libOpenMAXAL.so.1): get_and_wait(q, abs deadline or NULL) returns an
 * element or NULL on timeout or flush (flag at q+0x2c); done(q, e) looks e up in the queue's own
 * array and calls the producer's callback even when it is not there, so elements made here never
 * reach it.
 */
#include "mcn_qnx.h"
#include "mcn_hook.h"
#include "mcn_video.h"

#define LIB "libOpenMAXAL.so.1"
/* 0480 libOpenMAXAL.so.1 (sha256 c31fe983...): the exports, checked against each other, and the
 * return address of "bl mmfbq_get_and_wait" in BufferQueueGetAndWait (0x1ac38), whose next
 * instruction is pop {r4, pc}. Another build leaves the cluster video stock. */
#define OMX_GET 0x893cu
#define OMX_DONE 0x8b9cu
#define OMX_GET_AND_WAIT 0x8e4cu
#define OMX_SINK_RETURN 0x1ac3cu
#define ARM_POP_R4_PC 0xe8bd8010u
/* SAL marks the phone's open stream connection a few times a second (mcn_video.h alive_ms). */
#define ALIVE_MS 1500
/* Longest single sleep on the frame semaphore: the flush flag, the caller's deadline and the
 * switch are looked at again after it. */
#define WAIT_MS 50
#define CLOCK_REALTIME 0
/* DisplayManager is ET_EXEC: MostSink::write's "bl RingIO::writeAquire" at 0x126f74 returns to
 * "subs sb, r0, #0" at 0x126f78 (0480 tsd.mibstd2.system.displaymanager, sha256 932efd26...). */
#define COMMON_LIB "libtsd.common.so"
#define RING_ACQUIRE "_ZN3tsd6common9messaging6RingIO11writeAquireERPvRjb"
#define MOST_SINK_CALL 0x126f74u
#define MOST_SINK_RETURN 0x126f78u
#define ARM_BL_ACQUIRE 0xebff9ffcu
#define ARM_SUBS_SB_R0 0xe2509000u
#define RING_WAIT_MS 300
#define RING_REPORT_MS 10000

MCN_HOOK(mmfbq_get_and_wait, LIB);
MCN_HOOK(mmfbq_done, LIB);

/* An element as the reader reads it: data at +4, size at +8; the queue links at +0x18, +0x1c. */
struct element {
    uint32_t word0;
    uint8_t *data;
    uint32_t size;
    uint32_t producer[3];
    uint32_t link[2];
};

static struct element ours;
static uint8_t frame[MCN_VIDEO_SLOT_BYTES];
static struct mcn_video *video;
static void *cluster_queue; /* the queue that carries H.264: the cluster encoder's */
static unsigned next_seq, generation, last_open_ms;
static int phone, need_idr, wait_stock_idr;

static void *(*real_get_and_wait)(void *, const struct timespec *);
static void *(*real_get)(void *);
static int (*real_done)(void *, void *);
static uintptr_t sink_return; /* 0: unknown build, nothing is replaced */

static struct mcn_hook hook_ring = { RING_ACQUIRE, COMMON_LIB, 0, 0 };
static int (*real_acquire)(void *, void **, unsigned *, int);
static int most_sink = -1; /* MostSink's call site checked: 1 ours, 0 another build */
static int ring_stalled;
static unsigned ring_waits, ring_wait_max, ring_lost, ring_report_ms;

/*
 * Plain dlsym, not MCN_REAL: that aborts on a missing name, which would take DisplayManager
 * down (and the unit with it). A name missing here leaves the cluster video stock; the two
 * hooks then pass through to whatever the run-time linker finds (mcn_hook_enter).
 */
static void resolve(void)
{
    static volatile int done;
    void *lib;
    uintptr_t base;

    if (done) {
        __sync_synchronize(); /* another reader thread resolved: see its pointers */
        return;
    }
    lib = dlopen(LIB, RTLD_LAZY | RTLD_GLOBAL);
    if (lib) {
        real_get_and_wait = dlsym(lib, "mmfbq_get_and_wait");
        real_get = dlsym(lib, "mmfbq_get");
        real_done = dlsym(lib, "mmfbq_done");
    }
    base = (uintptr_t)real_get_and_wait - OMX_GET_AND_WAIT;
    if (real_get_and_wait && (uintptr_t)real_get == base + OMX_GET &&
        (uintptr_t)real_done == base + OMX_DONE &&
        *(const uint32_t *)(base + OMX_SINK_RETURN) == ARM_POP_R4_PC) {
        sink_return = base + OMX_SINK_RETURN;
    } else {
        mcn_log("libOpenMAXAL build mismatch: cluster video stays stock");
    }
    if (!real_get_and_wait) {
        real_get_and_wait = MCN_REAL(mmfbq_get_and_wait);
    }
    if (!real_done) {
        real_done = MCN_REAL(mmfbq_done);
    }
    __sync_synchronize();
    done = 1;
}

static struct mcn_video *video_open(void)
{
    unsigned now = mcn_now_ms();
    int fd;
    void *p;

    if (video || now - last_open_ms < 1000) {
        return video;
    }
    last_open_ms = now;
    fd = shm_open(MCN_VIDEO_SHM, O_RDWR, 0);
    if (fd < 0) {
        return NULL;
    }
    p = mmap(NULL, sizeof(struct mcn_video), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p != MAP_FAILED && ((struct mcn_video *)p)->magic == MCN_VIDEO_MAGIC &&
        ((struct mcn_video *)p)->version == MCN_VIDEO_VERSION) {
        video = p;
        mcn_log("DisplayManager: phone video found");
    }
    return video;
}

/*
 * The phone owns the cluster while its stream connection is open and it has sent a frame on it.
 * A still map sends no frames: the cluster then keeps showing the last one (its PTS are taken
 * from the clock when packed, so a pause is no error). A SAL that stops marking the connection
 * (closed, or SAL gone) gives the cluster back within ALIVE_MS.
 */
static int stream_alive(void)
{
    unsigned alive;

    if (!mcn_active() || !video_open()) {
        return 0;
    }
    alive = video->alive_ms;
    return alive && video->write_seq && mcn_now_ms() - alive < ALIVE_MS;
}

/* Sleeps until SAL publishes an access unit after `head` (or WAIT_MS), without polling. */
static void wait_frame(unsigned head)
{
    struct timespec t;

    if (!video->sem_ok) {
        usleep(5000);
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_nsec += WAIT_MS * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    video->waiting = 1;
    __sync_synchronize();
    /* A unit published before "waiting" was seen is caught here, not slept through. */
    if (video->write_seq == head) {
        sem_timedwait_monotonic(&video->frame_ready, &t);
    }
    video->waiting = 0;
}

/* Annex-B with a NAL of the given type (5 IDR slice, 7 SPS) anywhere in it. */
static int has_nal(const uint8_t *p, uint32_t n, int type)
{
    uint32_t i;

    for (i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 0x1f) == type) {
            return 1;
        }
    }
    return 0;
}

/*
 * Where to start reading after a new stream or an overrun: just before the newest key frame
 * still in the ring, else before the newest unit. The phone's first key frame is usually
 * published before this reader notices the stream, and skipping it cost a forceKeyFrame (and
 * its burst) on every connection: "3 key frames, 2 asked" in the first 10 s of each one in the
 * car logs of 2026-10-26. Only the SLOTS-1 newest are looked at; the writer fills the oldest.
 */
static unsigned resync(unsigned head)
{
    unsigned s;

    for (s = head; s && head - s < MCN_VIDEO_SLOTS - 1; s--) {
        const struct mcn_video_slot *slot = &video->slot[s % MCN_VIDEO_SLOTS];

        if (slot->seq == s && (slot->flags & MCN_VIDEO_IDR)) {
            return s - 1;
        }
    }
    return head ? head - 1 : 0;
}

/* The next access unit of the phone into `frame`; 0 when none is ready. */
static uint32_t take_phone_frame(void)
{
    unsigned gen = video->generation, head, seq, size, flags;
    struct mcn_video_slot *slot;

    __sync_synchronize(); /* SAL resets write_seq before it changes the generation */
    head = video->write_seq;
    __sync_synchronize(); /* nothing of a slot is read before the head that covers it */
    if (gen != generation || head - next_seq >= MCN_VIDEO_SLOTS) {
        /* New stream, or we fell behind the ring: start again at a key frame. */
        generation = gen;
        next_seq = resync(head);
        need_idr = 1;
    }
    while (next_seq != head) {
        seq = ++next_seq;
        slot = &video->slot[seq % MCN_VIDEO_SLOTS];
        if (slot->seq != seq) {
            need_idr = 1;
            continue;
        }
        __sync_synchronize(); /* the slot's contents are read after its sequence */
        size = slot->size;
        flags = slot->flags;
        if (size > sizeof(frame)) {
            need_idr = 1; /* torn: overwritten while read */
            continue;
        }
        memcpy(frame, slot->data, size);
        __sync_synchronize();
        if (slot->seq != seq) {
            need_idr = 1; /* overwritten while copying */
            continue;
        }
        if (need_idr && !(flags & MCN_VIDEO_IDR)) {
            video->want_idr = 1;
            continue;
        }
        need_idr = 0;
        return size;
    }
    if (need_idr) {
        video->want_idr = 1;
    }
    return 0;
}

static int deadline_passed(const struct timespec *deadline)
{
    struct timespec now;

    if (!deadline) {
        return 0;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

MCN_EXPORT void *mmfbq_get_and_wait(void *q, const struct timespec *deadline)
{
    void *e;
    uint32_t size;
    unsigned head;

    resolve();
    if (!sink_return || (uintptr_t)__builtin_return_address(0) != sink_return) {
        /* Not the buffer-queue interface DisplayManager reads (other element layouts). */
        return real_get_and_wait(q, deadline);
    }
    if (q && q == cluster_queue && stream_alive()) {
        if (!phone) {
            phone = 1;
            need_idr = 1;
            mcn_busy(1);
            mcn_log("cluster: phone map");
        }
        mcn_busy_tick();
        /* Until the stream stops, or the queue is flushed (stop), or the caller's deadline. */
        while (!((volatile int *)q)[0xb] && !deadline_passed(deadline) && stream_alive()) {
            while ((e = real_get(q)) != NULL) {
                real_done(q, e); /* the stock frame goes back to the encoder unused */
            }
            head = video->write_seq;
            size = take_phone_frame();
            if (size) {
                ours.data = frame;
                ours.size = size;
                return &ours;
            }
            wait_frame(head);
        }
        if (deadline_passed(deadline) && stream_alive()) {
            return NULL;
        }
    }
    if (phone) {
        phone = 0;
        wait_stock_idr = 1; /* stock P-frames refer to pictures the cluster no longer has */
        mcn_busy(0);
        mcn_log("cluster: stock map");
    }
    for (;;) {
        const struct element *el = e = real_get_and_wait(q, deadline);

        if (!el) {
            return e;
        }
        if (has_nal(el->data, el->size, 7)) {
            /* SPS: this queue carries the cluster's H.264 (again, after an encoder restart). */
            if (cluster_queue != q) {
                cluster_queue = q;
                mcn_log("cluster: encoder queue found");
            }
            wait_stock_idr = 0;
        }
        if (q == cluster_queue && wait_stock_idr && !has_nal(el->data, el->size, 5)) {
            real_done(q, e);
            continue;
        }
        wait_stock_idr = 0;
        return e;
    }
}

static void ring_report(unsigned now)
{
    char line[96];

    if (now - ring_report_ms < RING_REPORT_MS || (!ring_waits && !ring_lost)) {
        return;
    }
    snprintf(line, sizeof(line), "MOST ring full: %u waits (longest %u ms), %u frames cut",
             ring_waits, ring_wait_max, ring_lost);
    mcn_log(line);
    ring_report_ms = now;
    ring_waits = ring_wait_max = ring_lost = 0;
}

int ring_acquire(void *ring, void **p, unsigned *n, int partial) __asm__(RING_ACQUIRE);

MCN_EXPORT int ring_acquire(void *ring, void **p, unsigned *n, int partial)
{
    uintptr_t from = (uintptr_t)__builtin_return_address(0);
    unsigned want = *n, start, now;
    int r;

    if (!real_acquire) {
        real_acquire = mcn_hook_enter(&hook_ring);
    }
    r = real_acquire(ring, p, n, partial);
    if (from != MOST_SINK_RETURN) {
        return r; /* other rings and writers leave MostSink's stall mark alone */
    }
    if (r || !phone) {
        ring_stalled = ring_stalled && !r;
        return r;
    }
    if (most_sink < 0) {
        /* Mapped: we return there. */
        most_sink = *(const uint32_t *)MOST_SINK_CALL == ARM_BL_ACQUIRE &&
                    *(const uint32_t *)MOST_SINK_RETURN == ARM_SUBS_SB_R0;
        if (!most_sink) {
            mcn_log("DisplayManager build mismatch: MOST ring not waited for");
        }
    }
    if (!most_sink || ring_stalled) {
        return r;
    }
    start = mcn_now_ms();
    do {
        usleep(2000);
        *n = want;
        r = real_acquire(ring, p, n, partial);
        now = mcn_now_ms();
    } while (!r && now - start < RING_WAIT_MS);
    ring_waits++;
    if (now - start > ring_wait_max) {
        ring_wait_max = now - start;
    }
    if (!r) {
        /* MostSink cuts this frame: skip to the next key frame and ask for it now. */
        ring_stalled = 1;
        ring_lost++;
        need_idr = 1;
        if (video) {
            video->want_idr = 1;
        }
    }
    ring_report(now);
    return r;
}

MCN_EXPORT int mmfbq_done(void *q, void *e)
{
    resolve();
    if (e == &ours) {
        return 0;
    }
    return real_done(q, e);
}

const char mcn_component[] = "dm";

MCN_EXPORT void mcn_init(void)
{
    mcn_log("DisplayManager hooks loaded");
    mcn_guard_init();
}
