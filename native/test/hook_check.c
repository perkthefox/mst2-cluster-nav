/* Host check of the actual switch logic (common/hook.c): the runHMI.sh block, kill switch,
 * enabled, the trial boot taken by the first part that starts, but never on the boot that armed
 * it, the nesting busy mark and the crash strikes.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE. */
#include "mcn_qnx.h"
#include "mcn_hook.h"

int puts(const char *);
int fork(void);
int waitpid(int pid, int *status, int options);
void exit(int status);
const char mcn_component[] = "check";

#define CHECK(x)                            \
    do {                                    \
        if (!(x)) {                         \
            puts("hook check failed: " #x); \
            abort();                        \
        }                                   \
    } while (0)

/* A tiny file system: the paths that exist, with their content. Descriptor = 20 + slot. */
#define NFILES 12
static struct {
    char path[96];
    char data[512];
    int len, pos, used;
} fs[NFILES];
static long now_s;

static const char hmi_with_block[] = "#!/bin/ksh\nVERSION=H29.319.120\nBOOTCLASSPATH=x\n"
                                     "# >>> mst2-cluster-nav (own HMI bundle)\nMCN=1\n"
                                     "# <<< mst2-cluster-nav\necho run\n";

static int find(const char *path)
{
    int i;

    for (i = 0; i < NFILES; i++) {
        if (fs[i].used && !strcmp(fs[i].path, path)) {
            return i;
        }
    }
    return -1;
}

static int put(const char *path, const char *data)
{
    int i = find(path);

    if (i < 0) {
        for (i = 0; i < NFILES && fs[i].used; i++) {
        }
    }
    CHECK(i < NFILES && strlen(path) < sizeof(fs[i].path));
    memset(&fs[i], 0, sizeof(fs[i]));
    fs[i].used = 1;
    memcpy(fs[i].path, path, strlen(path) + 1);
    fs[i].len = (int)strlen(data);
    CHECK(fs[i].len < (int)sizeof(fs[i].data));
    memcpy(fs[i].data, data, (size_t)fs[i].len);
    return i;
}

/* The decimal content of a state file, -1 when missing. */
static long value(const char *path)
{
    int i = find(path), k;
    long v = 0;

    if (i < 0) {
        return -1;
    }
    for (k = 0; k < fs[i].len && fs[i].data[k] >= '0' && fs[i].data[k] <= '9'; k++) {
        v = v * 10 + (fs[i].data[k] - '0');
    }
    return v;
}

int mcn_test_open(const char *path, int flags, ...)
{
    int i = find(path);

    if (!strcmp(path, MCN_LOG_FILE)) {
        return -1;
    }
    if (flags & O_CREAT) {
        if (i < 0 || (flags & O_TRUNC)) {
            i = put(path, "");
        }
    }
    if (i < 0) {
        return -1;
    }
    fs[i].pos = 0;
    return 20 + i;
}

int mcn_test_close(int fd)
{
    CHECK(fd >= 20 && fd < 20 + NFILES);
    return 0;
}

ssize_t mcn_test_read(int fd, void *buf, size_t n)
{
    int i = fd - 20, left;

    CHECK(i >= 0 && i < NFILES && fs[i].used);
    left = fs[i].len - fs[i].pos;
    if ((int)n > left) {
        n = (size_t)left;
    }
    memcpy(buf, fs[i].data + fs[i].pos, n);
    fs[i].pos += (int)n;
    return (ssize_t)n;
}

ssize_t mcn_test_write(int fd, const void *buf, size_t n)
{
    int i = fd - 20;

    if (fd == 1) {
        return (ssize_t)n; /* the service's stdout */
    }
    CHECK(i >= 0 && i < NFILES && fs[i].used && fs[i].len + (int)n < (int)sizeof(fs[i].data));
    memcpy(fs[i].data + fs[i].len, buf, n);
    fs[i].len += (int)n;
    return (ssize_t)n;
}

int mcn_test_unlink(const char *path)
{
    int i = find(path);

    if (i >= 0) {
        fs[i].used = 0;
    }
    return i >= 0 ? 0 : -1;
}

int mcn_test_clock_gettime(int clock, struct timespec *ts)
{
    (void)clock;
    ts->tv_sec = now_s;
    ts->tv_nsec = 0;
    return 0;
}

static int boot(const char *a, const char *b)
{
    memset(fs, 0, sizeof(fs));
    put(MCN_HMI_SH, hmi_with_block);
    if (a) {
        put(a, "");
    }
    if (b) {
        put(b, "");
    }
    now_s += 3; /* past the 2 s cache of mcn_active */
    return mcn_active();
}

/* In a child: the block check runs once per process. */
static void without_block(const char *hmi)
{
    int status = 1, pid = fork();

    if (pid == 0) {
        memset(fs, 0, sizeof(fs));
        if (hmi) {
            put(MCN_HMI_SH, hmi);
        }
        put(MCN_DIR "/enabled", "");
        put(MCN_BOOT_MARK, "");
        exit(mcn_active() ? 1 : 0);
    }
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && status == 0);
}

int main(void)
{
    char busy[96], strikes[96], off[96];

    /* runHMI.sh without our block (a tool restored an old copy, or a firmware update). */
    without_block(NULL);
    without_block("#!/bin/ksh\nBOOTCLASSPATH=x\necho run\n");
    /* The marker inside a command is not the block's opening line. */
    without_block("#!/bin/ksh\nsed '/^# >>> mst2-cluster-nav/d'\n");

    CHECK(boot(MCN_DIR "/trial", NULL));
    CHECK(find(MCN_DIR "/trial") < 0 && find(MCN_TRIAL_MARK) >= 0);
    CHECK(!boot(MCN_DIR "/trial", MCN_ARMED_MARK)); /* armed now: the next boot's trial */
    CHECK(find(MCN_DIR "/trial") >= 0);
    CHECK(!boot(MCN_DIR "/trial", "/media/mp003/mst2-cluster-nav-disable"));
    CHECK(find(MCN_DIR "/trial") >= 0); /* the kill switch does not use up the trial */
    CHECK(boot(MCN_TRIAL_MARK, NULL));
    CHECK(boot(MCN_DIR "/enabled", NULL));
    CHECK(boot(MCN_BOOT_MARK, NULL));
    CHECK(!boot(MCN_DIR "/enabled", "/media/mp009/mst2-cluster-nav-disable"));
    CHECK(!boot(MCN_DIR "/enabled", MCN_OFF_MARK)); /* runHMI.sh's guard kept the mod out */
    CHECK(!boot(MCN_DIR "/trial", MCN_OFF_MARK) && find(MCN_DIR "/trial") >= 0);
    CHECK(!boot(NULL, NULL));

    /* Busy pairs nest: the mark goes on the first start and off with the last end only. */
    snprintf(busy, sizeof(busy), "%s/busy-%s", MCN_DIR, mcn_component);
    snprintf(strikes, sizeof(strikes), "%s/strikes-%s", MCN_DIR, mcn_component);
    snprintf(off, sizeof(off), "%s/off-%s", MCN_DIR, mcn_component);
    boot(MCN_DIR "/enabled", NULL);
    put(strikes, "1\n");
    now_s = 1000;
    mcn_busy(0); /* unmatched end: ignored */
    CHECK(value(busy) == -1 && value(strikes) == 1);
    mcn_busy(1);
    CHECK(value(busy) == 1000);
    now_s = 1100;
    mcn_busy(1);
    CHECK(value(busy) == 1000);
    mcn_busy(0);
    CHECK(value(busy) == 1000 && value(strikes) == 1); /* one caller still busy */
    mcn_busy(0);
    CHECK(value(busy) == 0 && value(strikes) == 0);
    mcn_busy(0);
    mcn_busy(1); /* the extra end did not leave the count below zero */
    CHECK(value(busy) == 1100);
    mcn_busy(0);
    CHECK(value(busy) == 0);

    /* A start finding a fresh busy mark counts a strike; an old mark (normal switch-off) not. */
    put(busy, "900\n");
    mcn_guard_init();
    CHECK(value(strikes) == 0 && value(busy) == 0 && mcn_active());
    put(busy, "1090\n");
    mcn_guard_init();
    CHECK(value(strikes) == 1 && value(off) == -1 && mcn_active());
    put(busy, "1095\n");
    mcn_guard_init(); /* two quick switch-offs while busy in a row are not enough */
    CHECK(value(strikes) == 2 && value(off) == -1 && mcn_active());
    put(busy, "1098\n");
    mcn_guard_init(); /* the third in a row: this service's hooks off until Install */
    CHECK(value(strikes) == 3 && value(off) == 1 && !mcn_active());
    CHECK(find(MCN_DIR "/history") >= 0); /* and the reason survives the reset */
    puts("hook: runHMI.sh block, kill switch, guard, enabled, trial taken once and never on its "
         "arming boot, nesting busy mark, crash strikes: ok");
    return 0;
}
