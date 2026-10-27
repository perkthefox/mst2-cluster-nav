/* Host replay of the actual SAL spawn interposer, with a mock QNX process/filesystem API.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE. */
#include "mcn_qnx.h"
#include "mcn_hook.h"

int puts(const char *text);
extern int spawn(const char *, int, const int *, const void *, char *const *,
                 char *const *);

static int active = 1, expect_mod = 1, config_failure, called, invalidated;
static off_t child_size = 4096; /* libmcn_ipod.so next to libmcn_sal.so */
static char output[4096];
static unsigned used, read_pos;
static const char config[] =
    "[accessory]\nserial=original\n[ident]\nsendmsgs=0x5000\n";
static const int map[] = {0, 1, 2};
static int inheritance;

#define CHECK(x)                              \
    do {                                      \
        if (!(x)) {                           \
            puts("launch check failed: " #x); \
            abort();                          \
        }                                     \
    } while (0)

static int real_spawn(const char *path, int count, const int *fds,
                      const void *inherit, char *const argv[],
                      char *const envp[])
{
    CHECK(!strcmp(path, "/bin/on"));
    CHECK(count == 3 && fds == map && inherit == &inheritance);
    CHECK(!strcmp(argv[1], "-Xaps=Connectivity"));
    CHECK(!strcmp(argv[2], "/usr/sbin/mm-ipod"));
    CHECK(!strcmp(argv[3], "-d"));
    CHECK(!strcmp(argv[5], "-vvvvvv") && !argv[6]);
    CHECK(!strcmp(envp[1], "PATH=/proc/boot:/bin") && !envp[2]);
    if (expect_mod) {
        CHECK(!strcmp(
            argv[4], "iap2,config=/dev/shmem/mst2-cluster-nav-iap2.cfg,probe"));
        CHECK(!strcmp(envp[0], "LD_PRELOAD=/lib/libmmappriofix.so.1:"
                               "/tsd/lib/mst2-cluster-nav/libmcn_ipod.so"));
        CHECK(strstr(output, "serial=original\n") &&
              strstr(output, "0x5000,0x5200,0x5203"));
        CHECK(strstr(output, "recvmsgs=0x5201,0x5202,0x5204"));
    } else {
        CHECK(!strcmp(argv[4],
                      "iap2,config=/tsd/var/sal/carplay/iap2.cfg,probe"));
        CHECK(!strcmp(envp[0], "LD_PRELOAD=/lib/libmmappriofix.so.1"));
    }
    called++;
    return 1234;
}

void *mcn_hook_enter(struct mcn_hook *hook)
{
    CHECK(!strcmp(hook->name, "spawn"));
    return real_spawn;
}

int mcn_active(void)
{
    return active;
}

void mcn_log(const char *text)
{
    (void)text;
}

void mcn_log2(const char *a, const char *b)
{
    (void)a;
    (void)b;
}

void mcn_link_send(const char *text)
{
    (void)text;
}

/* Android Auto (sal/gal.c) is not part of this check. */
void mcn_gal_tick(unsigned now)
{
    (void)now;
}

void mcn_gal_video_tick(void)
{
}

/* SAL's own environment from cpu.conf: the stock preload, then ours. */
char *mcn_test_getenv(const char *name)
{
    CHECK(!strcmp(name, "LD_PRELOAD"));
    return "/lib/libmmappriofix.so.1:/tsd/lib/mst2-cluster-nav/libmcn_sal.so";
}

off_t mcn_test_lseek(int fd, off_t off, int whence)
{
    CHECK(fd == 13 && off == 0 && whence == SEEK_END);
    return child_size;
}

int mcn_test_open(const char *path, int flags, ...)
{
    (void)flags;
    if (!strcmp(path, "/tsd/lib/mst2-cluster-nav/libmcn_ipod.so")) {
        return 13;
    }
    if (!strcmp(path, "/tsd/var/sal/carplay/iap2.cfg")) {
        read_pos = 0;
        return config_failure ? -1 : 10;
    }
    if (!strcmp(path, "/dev/shmem/mst2-cluster-nav-iap2.cfg")) {
        used = 0;
        return config_failure == 3 ? -1 : 11;
    }
    return -1;
}

int mcn_test_close(int fd)
{
    CHECK(fd == 10 || fd == 11 || fd == 13);
    return 0;
}

ssize_t mcn_test_read(int fd, void *buf, size_t n)
{
    unsigned left = sizeof(config) - 1 - read_pos;

    CHECK(fd == 10);
    if (n > left) {
        n = left;
    }
    if (n > 7) {
        n = 7; /* exercise partial reads */
    }
    memcpy(buf, config + read_pos, n);
    read_pos += n;
    return (ssize_t)n;
}

ssize_t mcn_test_write(int fd, const void *buf, size_t n)
{
    CHECK(fd == 11 && n + used < sizeof(output));
    if (config_failure == 2) {
        return 0;
    }
    if (n > 5) {
        n = 5; /* exercise partial writes */
    }
    memcpy(output + used, buf, n);
    used += n;
    output[used] = 0;
    return (ssize_t)n;
}

int mcn_test_unlink(const char *path)
{
    CHECK(!strcmp(path, "/dev/shmem/mst2-cluster-nav-rg-ready"));
    invalidated++;
    return 0;
}

int main(void)
{
    char *args[] = {"/bin/on",
                    "-Xaps=Connectivity",
                    "/usr/sbin/mm-ipod",
                    "-d",
                    "iap2,config=/tsd/var/sal/carplay/iap2.cfg,probe",
                    "-vvvvvv",
                    NULL};
    char *env[] = {"LD_PRELOAD=/lib/libmmappriofix.so.1",
                   "PATH=/proc/boot:/bin", NULL};

    CHECK(spawn(args[0], 3, map, &inheritance, args, env) == 1234);
    CHECK(!strcmp(args[4], "iap2,config=/tsd/var/sal/carplay/iap2.cfg,probe"));
    CHECK(!strcmp(env[0], "LD_PRELOAD=/lib/libmmappriofix.so.1"));
    active = expect_mod = 0;
    CHECK(spawn(args[0], 3, map, &inheritance, args, env) == 1234);
    active = config_failure = 1;
    CHECK(spawn(args[0], 3, map, &inheritance, args, env) == 1234);
    config_failure = 2;
    CHECK(spawn(args[0], 3, map, &inheritance, args, env) == 1234);
    config_failure = 3;
    CHECK(spawn(args[0], 3, map, &inheritance, args, env) == 1234);
    config_failure = 0;
    child_size = 0; /* an empty child library must never reach LD_PRELOAD */
    CHECK(spawn(args[0], 3, map, &inheritance, args, env) == 1234);
    CHECK(called == 6 && invalidated == 6);
    puts("launch: stock APS/fds/argv/env, RAM cfg, child library next to SAL's, "
         "disabled, empty library and I/O failure fallback: ok");
    return 0;
}
