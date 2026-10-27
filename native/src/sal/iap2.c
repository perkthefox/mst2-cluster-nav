/*
 * P0480T CarPlay source: spawn boundary, raw iAP2 client, diagnostic hex tail, now playing.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * ABI: phone-route-guidance.md E28, E31-E34; QNX 6.5 libc spawn() reference.
 * The stock cfg is copied to RAM with extra ident IDs; only the child's config argument
 * changes. Stock APS, fd map, inheritance, argv ordering and preload are preserved.
 */
#include "mcn_qnx.h"
#include "mcn_hook.h"
#include "mcn_route.h"
#include "mcn_transcode.h"

#define CLIENT   "libiap2client.so.1"
#define CFG      "/tsd/var/sal/carplay/iap2.cfg"
#define RAM_CFG  "/dev/shmem/mst2-cluster-nav-iap2.cfg"
#define READY    "/dev/shmem/mst2-cluster-nav-rg-ready"
#define LOG      "/dev/shmem/iap2_carplay.log"
#define SAL_LIB  "/libmcn_sal.so"
#define IPOD_LIB "/libmcn_ipod.so"

MCN_HOOK(spawn, "libc.so.3");
MCN_HOOK(iap2_connect, CLIENT);

static volatile unsigned state_lock;
static unsigned epoch;
static int session_up, client_flags;
static char client_path[256];

static void lock(void)
{
    while (__sync_lock_test_and_set(&state_lock, 1)) {
        usleep(1000);
    }
}

static void unlock(void)
{
    __sync_lock_release(&state_lock);
}

void mcn_route_session(int up)
{
    lock();
    session_up = up;
    unlock();
}

MCN_EXPORT void *iap2_connect(const char *path, int flags)
{
    void *(*real)(const char *, int) = MCN_REAL(iap2_connect);
    void *client = real(path, flags);

    if (client && path && strlen(path) < sizeof(client_path)) {
        lock();
        memcpy(client_path, path, strlen(path) + 1);
        client_flags = flags;
        unlock();
    }
    return client;
}

/* The child library lies next to ours: the installer copies both into one directory
 * (car: /tsd/lib/mst2-cluster-nav, mQbEMU: /tsd/var/mst2-cluster-nav), and SAL's own
 * LD_PRELOAD from cpu.conf names ours. A preload that cannot be loaded keeps the child
 * from starting, so the library is offered only when it is there and not empty.
 */
static int ipod_lib(char *out, unsigned size)
{
    const char *list = getenv("LD_PRELOAD"), *end, *start;
    unsigned dir;
    off_t len;
    int fd;

    end = list ? strstr(list, SAL_LIB) : NULL;
    if (!end) {
        return 0;
    }
    for (start = end; start > list && start[-1] != ':'; start--) {
    }
    dir = (unsigned)(end - start);
    if (dir + sizeof(IPOD_LIB) > size) {
        return 0;
    }
    memcpy(out, start, dir);
    memcpy(out + dir, IPOD_LIB, sizeof(IPOD_LIB));
    fd = open(out, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    len = lseek(fd, 0, SEEK_END);
    close(fd);
    return len > 0;
}

static int copy_config(void)
{
    char *input = malloc(32768), *output = malloc(34816);
    int fd = -1, ok = 0, n;
    unsigned used = 0, written = 0;
    ssize_t k = 0;
    const char *step = "allocation or opening the stock cfg";

    if (!input || !output || (fd = open(CFG, O_RDONLY)) < 0) {
        goto done;
    }
    while (used < 32768 && (k = read(fd, input + used, 32768 - used)) > 0) {
        used += (unsigned)k;
    }
    if (used == 32768 || k < 0) {
        step = "reading the stock cfg";
        goto done;
    }
    close(fd);
    fd = -1;
    n = mcn_route_config(input, used, output, 34816);
    step = "rewriting ident or opening the RAM cfg";
    if (n < 0 || (fd = open(RAM_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
        goto done;
    }
    while (written < (unsigned)n &&
           (k = write(fd, output + written, (unsigned)n - written)) > 0) {
        written += (unsigned)k;
    }
    close(fd);
    fd = -1;
    /* The private copy has no reader before spawn; it needs no rename. */
    step = "writing the RAM cfg";
    ok = written == (unsigned)n;
done:
    if (fd >= 0) {
        close(fd);
    }
    free(input);
    free(output);
    if (!ok) {
        mcn_log2("route cfg failed: ", step);
    }
    return ok;
}

/* The QNX spawn ABI has six arguments. The last two stay on the ARM caller's stack;
 * no C++ std::string or LaunchProcess ABI is reconstructed here (E33).
 */
MCN_EXPORT int spawn(const char *path, int fd_count, const int *fd_map,
                     const void *inherit, char *const argv[],
                     char *const envp[])
{
    int (*real)(const char *, int, const int *, const void *, char *const *,
                char *const *) = MCN_REAL(spawn);
    char *args[128], *env[128], preload[2048], config[2048], lib[256];
    unsigned na = 0, ne = 0, i;
    int ipod = 0, cfg_arg = -1, preload_arg = -1, result;

    if (!path || !argv || !envp ||
        (strcmp(path, "/bin/on") && strcmp(path, "/usr/sbin/mm-ipod"))) {
        return real(path, fd_count, fd_map, inherit, argv, envp);
    }
    while (na < 127 && argv[na]) {
        args[na] = argv[na];
        ipod |= strcmp(argv[na], "/usr/sbin/mm-ipod") == 0;
        if (strstr(argv[na], "config=" CFG)) {
            cfg_arg = (int)na;
        }
        na++;
    }
    while (ne < 126 && envp[ne]) {
        env[ne] = envp[ne];
        if (memcmp(envp[ne], "LD_PRELOAD=",
                   strlen(envp[ne]) < 11 ? strlen(envp[ne]) : 11) == 0 &&
            strlen(envp[ne]) >= 11) {
            preload_arg = (int)ne;
        }
        ne++;
    }
    if (!ipod || argv[na]) {
        return real(path, fd_count, fd_map, inherit, argv, envp);
    }
    /* Every new mm-ipod invalidates the previous driver's readiness, including
     * a stock launch after disabling the mod or after a config-copy failure.
     */
    unlink(READY);
    lock();
    epoch++;
    client_path[0] = 0;
    session_up = 0;
    unlock();
    if (!mcn_active() || cfg_arg < 0 || envp[ne]) {
        return real(path, fd_count, fd_map, inherit, argv, envp);
    }
    {
        const char *old = argv[cfg_arg], *at = strstr(old, "config=" CFG) + 7;
        unsigned prefix = (unsigned)(at - old),
                 suffix = strlen(at + sizeof(CFG) - 1);

        if (prefix + sizeof(RAM_CFG) + suffix > sizeof(config)) {
            return real(path, fd_count, fd_map, inherit, argv, envp);
        }
        memcpy(config, old, prefix);
        memcpy(config + prefix, RAM_CFG, sizeof(RAM_CFG) - 1);
        memcpy(config + prefix + sizeof(RAM_CFG) - 1, at + sizeof(CFG) - 1,
               suffix + 1);
    }
    if (!ipod_lib(lib, sizeof(lib))) {
        mcn_log("mm-ipod: libmcn_ipod.so missing or empty: launching stock");
        return real(path, fd_count, fd_map, inherit, argv, envp);
    }
    i = (unsigned)snprintf(preload, sizeof(preload), "%s%s%s",
                           preload_arg >= 0 ? envp[preload_arg] : "LD_PRELOAD=",
                           preload_arg >= 0 && envp[preload_arg][11] ? ":" : "",
                           lib);
    if (i >= sizeof(preload) || !copy_config()) {
        mcn_log("iAP2 config/preload unavailable: launching stock");
        return real(path, fd_count, fd_map, inherit, argv, envp);
    }
    args[na] = NULL;
    args[cfg_arg] = config;
    if (preload_arg >= 0) {
        env[preload_arg] = preload;
    } else {
        env[ne++] = preload;
    }
    env[ne] = NULL;
    mcn_log("mm-ipod: RAM ident cfg + child preload (stock APS/fds retained)");
    result = real(path, fd_count, fd_map, inherit, args, env);
    return result;
}

/* One read: a PPS object without ?wait gives its whole state per read() call. */
static int read_media(char *buf, unsigned bufsize, char *out, unsigned size)
{
    int fd = open(MCN_MEDIA_PPS, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, bufsize);
    close(fd);
    return n > 0 ? mcn_media_text(buf, (unsigned)n, out, size) : 0;
}

static void *route_worker(void *unused)
{
    struct mcn_route *r = malloc(sizeof(*r));
    struct mcn_route_log *l = malloc(sizeof(*l));
    char *out = malloc(MCN_RG_SNAPSHOT);
    void *lib = NULL;
    void *(*connect_client)(const char *, int) = NULL;
    int (*disconnect_client)(void *) = NULL;
    int (*raw_send)(void *, const void *, unsigned) = NULL;
    unsigned generation = ~0u, last_send = 0, attempts = 0, last_publish = 0, last_media = 0;
    int received = 0, was_active = 0, previous_up = 0, guidance_owned = 0, media_owned = 0;
    off_t offset = 0;

    (void)unused;
    if (!r || !l || !out) {
        free(r);
        free(l);
        free(out);
        mcn_log("route guidance: no memory, stock behaviour");
        return NULL;
    }
    mcn_route_reset(r);
    mcn_route_log_reset(l);
    mcn_log("route guidance reader ready");
    for (;;) {
        char path[256], chunk[4096];
        unsigned gen, now = mcn_now_ms();
        int up, flags, fd, active = mcn_active();

        lock();
        gen = epoch;
        up = session_up;
        flags = client_flags;
        memcpy(path, client_path, sizeof(path));
        unlock();
        if (gen != generation || active != was_active) {
            generation = gen;
            received = attempts = 0;
            last_send = now - 2000;
            mcn_route_reset(r);
            mcn_route_log_reset(l);
            if (guidance_owned) {
                mcn_link_send("active=0\n");
                guidance_owned = 0;
            }
            if (media_owned) {
                mcn_link_send("media=0");
                media_owned = 0;
            }
            /*
             * Start at the end, after a spawn too: the new mm-ipod truncates the log only when
             * it parses -l (fopen "w+", ipod_main 0x103fd4), so reading from 0 here could replay
             * the old session. Nothing is lost: its fopen comes before the driver is even
             * initialised (0x1042a8), the phone sends 0x52xx only after our 0x5200, and that
             * goes out only after this loop has read on from here, once SETUP and the new
             * READY mark (written by the new driver's identification) are both there. A log
             * that shrinks below this offset is still seen as truncated below.
             */
            fd = open(LOG, O_RDONLY);
            offset = fd >= 0 ? lseek(fd, 0, SEEK_END) : 0;
            if (fd >= 0) {
                close(fd);
            }
        }
        if (!up && previous_up) {
            mcn_route_reset(r);
            received = attempts = 0;
            last_send = now - 2000;
            if (guidance_owned) {
                mcn_link_send("active=0\n");
                guidance_owned = 0;
            }
            if (media_owned) {
                mcn_link_send("media=0");
                media_owned = 0;
            }
        }
        previous_up = up;
        was_active = active;
        if (active) {
            /* DT_INIT may run before SAL's C++ dependencies finish initializing.
             * Resolve the client only after stock SAL has connected to a phone,
             * never by calling dlopen concurrently with process initialization.
             */
            if (up && path[0] && !lib) {
                lib = dlopen(CLIENT, RTLD_LAZY | RTLD_GLOBAL);
                if (lib) {
                    connect_client = dlsym(lib, "iap2_connect");
                    disconnect_client = dlsym(lib, "iap2_disconnect");
                    raw_send = dlsym(lib, "iap2_raw_msg_send");
                }
            }
            fd = open(LOG, O_RDONLY);
            if (fd >= 0) {
                off_t end = lseek(fd, 0, SEEK_END);
                ssize_t n;
                unsigned budget = 65536;

                if (end < offset || offset < 0) {
                    offset = 0;
                    received = 0;
                    mcn_route_reset(r);
                    mcn_route_log_reset(l);
                    attempts = 0;
                    last_send = now - 2000;
                    if (guidance_owned) {
                        mcn_link_send("active=0\n");
                        guidance_owned = 0;
                    }
                }
                lseek(fd, offset, SEEK_SET);
                while (budget &&
                       (n = read(fd, chunk,
                                 budget < sizeof(chunk) ? budget
                                                        : sizeof(chunk))) > 0) {
                    int messages =
                        mcn_route_log_feed(l, r, chunk, (unsigned)n, now);

                    if (messages & MCN_RG_REIDENT) {
                        received = attempts = 0;
                        last_send = now - 2000;
                        mcn_log("iAP2 identification restarted: route cache "
                                "cleared");
                        if (guidance_owned) {
                            mcn_link_send("active=0\n");
                            guidance_owned = 0;
                        }
                    }
                    messages &= MCN_RG_REIDENT - 1;
                    if (messages && !received) {
                        mcn_log("route guidance: first 0x52xx received");
                    }
                    received |= messages != 0;
                    offset += n;
                    budget -= (unsigned)n;
                }
                close(fd);
            }
            /* Never use or retain SAL's client object. Open a separate, short-lived
             * raw client to its observed device path; disconnect is ABI-verified E34.
             */
            if (up && !received && path[0] && connect_client &&
                disconnect_client && raw_send &&
                now - last_send >= (attempts < 10 ? 2000u : 30000u)) {
                int ready = open(READY, O_RDONLY);

                last_send = now;
                if (ready >= 0) {
                    void *client;
                    uint8_t start[16];
                    unsigned n = mcn_route_start(start);

                    close(ready);
                    attempts++;
                    client = connect_client(path, flags);
                    if (client) {
                        int result = raw_send(client, start, n);

                        disconnect_client(client);
                        if (attempts <= 3 || attempts == 10) {
                            mcn_log(result < 0
                                        ? "0x5200 raw-send failed; will retry"
                                        : "0x5200 sent through the stock iAP2 "
                                          "client");
                        }
                    }
                }
            }
            if (up && r->seen && now - last_publish >= 1000) {
                int n = mcn_route_text(r, now, out, MCN_RG_SNAPSHOT);

                int owns = mcn_route_owns(r, now);

                /* A no-route heartbeat must not continuously clear stock navigation. */
                if (n > 0 && (unsigned)n < MCN_RG_SNAPSHOT &&
                    (owns || guidance_owned)) {
                    mcn_link_send(out);
                }
                guidance_owned = owns;
                last_publish = now;
            }
            /* Now playing, once a second while CarPlay is up: PhoneLink times it out. */
            if (up && now - last_media >= 1000) {
                int n = read_media(chunk, sizeof(chunk), out, MCN_RG_SNAPSHOT);

                last_media = now;
                if (n > 0) {
                    if (!media_owned) {
                        mcn_log("now playing: first track from the phone");
                    }
                    mcn_link_send(out);
                    media_owned = 1;
                } else if (media_owned) {
                    mcn_link_send("media=0");
                    media_owned = 0;
                }
            }
            mcn_gal_tick(now);
            mcn_gal_video_tick();
        }
        usleep(200000);
    }
    return NULL;
}

void mcn_route_start_worker(void)
{
    pthread_t thread;

    if (pthread_create(&thread, NULL, route_worker, NULL) == 0) {
        pthread_detach(thread);
    } else {
        mcn_log("route guidance thread failed: stock behaviour");
    }
}
