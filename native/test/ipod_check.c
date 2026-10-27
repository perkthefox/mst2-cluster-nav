/* Host check of the actual preload's executable gate and missing-driver fallback.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE. */
#include "mcn_qnx.h"
#include "mcn_route.h"

int puts(const char *);
void mcn_init(void);
static const char *executable;
static int loads, enabled = 1;

#define CHECK(x)                            \
    do {                                    \
        if (!(x)) {                         \
            puts("ipod check failed: " #x); \
            abort();                        \
        }                                   \
    } while (0)

void *mcn_test_dlopen(const char *path, int flags)
{
    (void)flags;
    if (!path) {
        return (void *)1;
    }
    CHECK(!strcmp(path, "/lib/dll/ipod-drvr-iap2.so"));
    loads++;
    return NULL;
}

void *mcn_test_dlsym(void *handle, const char *name)
{
    CHECK(handle == (void *)1 && !strcmp(name, "ipod_log"));
    return executable && strstr(executable, "mm-ipod") ? (void *)1 : NULL;
}

static int busy, busy_calls, guarded;

void mcn_guard_init(void)
{
    guarded++;
}

/* Pairs only: the guard counts nested brackets (common/hook.c). */
void mcn_busy(int on)
{
    busy += on ? 1 : -1;
    busy_calls++;
    CHECK(busy >= 0 && busy <= 1);
}

int mcn_active(void)
{
    return enabled;
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

int main(void)
{
    struct mcn_msg_group group = {0}, used = {"Now Playing", 0, 4};
    unsigned lo;

    CHECK(mcn_route_msg_group(&group) == 0 && group.info && group.count == 5);
    CHECK(!group.name); /* print_packet keeps the "--?-- CTRL[..]" framing */
    for (lo = 0; lo < group.count; lo++) {
        /* link_handle_reply: a name, and no reply bit, for each received ID only */
        CHECK(!group.info[lo].name == !(lo == 1 || lo == 2 || lo == 4));
        CHECK(!(group.info[lo].flags & (4 | 0x20)) && !group.info[lo].params);
    }
    CHECK(mcn_route_msg_group(&used) < 0 && !used.info && used.count == 4);

    executable = "/bin/on";
    mcn_init();
    CHECK(loads == 0);
    CHECK(guarded == 0); /* /bin/on keeps no guard state */
    executable = "/usr/sbin/mm-ipod\n";
    mcn_init();
    CHECK(loads == 1 && guarded == 1);
    /* Busy only around patching: a missing driver is a clean end, and mm-ipod exiting later
     * (phone gone before identification, a device without iAP2) is no suspected crash. */
    CHECK(!busy && busy_calls == 2);
    enabled = 0;
    mcn_init();
    CHECK(loads == 1);
    enabled = 1;
    executable = NULL;
    mcn_init();
    CHECK(loads == 1);
    puts("ipod: message group 0x52, executable gate, disabled, unrecognized "
         "executable/missing driver fallback, busy only while patching: ok");
    return 0;
}
