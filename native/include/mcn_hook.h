/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Pass-through interposition: MCN_OBSERVE(name, lib) defines an exported function `name` that
 * notes the call (mcn_hook_enter) and then jumps to the next definition with the caller's
 * registers and stack untouched, so it is safe for any signature. The real function is found
 * on first use through dlopen(lib) + dlsym: on QNX 6.5 RTLD_NEXT depends on the caller's
 * return address and fails from preloaded libraries.
 */
#ifndef MCN_HOOK_H
#define MCN_HOOK_H

struct mcn_hook {
    const char *name;
    const char *lib;
    void *real;
    unsigned calls;
};

/* Returns the real function for the hook; never NULL (aborts the process otherwise). */
void *mcn_hook_enter(struct mcn_hook *hook);
/*
 * Called once, from the first hook call, when set (by mcn_init). Nothing may run on a thread of
 * ours before the service's own code calls us: DT_INIT runs before the executable's C++ static
 * constructors, and SAL exports its own malloc, which locks a tsd Mutex built by one of them
 * (tsd.mibstd2.sal.main malloc 0x14d264 -> 0x14d008). A thread of ours allocating that early
 * raced its construction, and SAL's main thread died with "Failed to aquire mutex: Invalid
 * argument" (mQbEMU, 3 of 7 boots).
 */
extern void (*mcn_first_call)(void);
void mcn_log(const char *msg);
void mcn_log2(const char *a, const char *b);
/* One "key=value" datagram to the HMI bundle (PhoneLink, UDP 127.0.0.1:MCN_LINK_PORT). */
void mcn_link_send(const char *text);
#define MCN_LINK_PORT 39750

#define MCN_DIR "/tsd/var/mst2-cluster-nav"
/* The mod's log in RAM (all its parts append), saved to the SD card from the GEM. */
#define MCN_LOG_FILE "/dev/shmem/mst2-cluster-nav.log"
#define MCN_LOG_MAX 262144
/* The part writing to the log: "sal" or "dm", defined by each library. */
extern const char mcn_component[];
/* Made by runHMI.sh when the mod loads on this boot (RAM: gone after the next reset). */
#define MCN_BOOT_MARK "/dev/shmem/mst2-cluster-nav-on"
/*
 * This boot is the trial boot: the first part to start takes MCN_DIR/trial and leaves this
 * instead, so a reset before the HMI comes up (runHMI.sh, which also takes the trial) ends the
 * trial rather than repeating it on every boot.
 */
#define MCN_TRIAL_MARK "/dev/shmem/mst2-cluster-nav-trial"
/* Made by runHMI.sh's guard when it keeps the mod out of this boot (off-reason says why). */
#define MCN_OFF_MARK "/dev/shmem/mst2-cluster-nav-off"
/*
 * The guard's block in runHMI.sh (path from cpu.conf: "on -p 20 /bin/sh /tsd/hmi/runHMI.sh"),
 * found by the line that opens it. Without it the hooks stay pass-through for the whole process.
 */
#define MCN_HMI_SH "/tsd/hmi/runHMI.sh"
#define MCN_HMI_BLOCK "# >>> mst2-cluster-nav"
/* Made by GEM when it arms the trial: that trial belongs to the next boot, not to this one. */
#define MCN_ARMED_MARK "/dev/shmem/mst2-cluster-nav-armed"
/*
 * Whether the hooks may do anything: runHMI.sh carries the mod's block (checked once per
 * process), no SD card carries the kill switch file, and the mod is enabled, on its trial boot,
 * or loaded on this boot. Checked again every 2 s, so a card put
 * in later still switches the hooks to plain pass-through.
 */
int mcn_active(void);
/*
 * Crash guard of each service. mcn_guard_init (from mcn_init) turns the hooks of this service
 * off for good after STRIKES_OFF runs in a row ended while busy, that is while a stream or the
 * cluster replacement ran, followed by a quick restart; mcn_busy(1/0) brackets such work (pairs
 * may nest and come from several threads: the mark stays while any is open) and mcn_busy_tick
 * refreshes its mark once a minute. State in MCN_DIR/{busy,strikes,off}-<component>.
 */
void mcn_guard_init(void);
void mcn_busy(int on);
void mcn_busy_tick(void);

/* A hook with the real signature: defined here, it takes the callers of the exported name. */
#define MCN_EXPORT __attribute__((visibility("default")))
#define MCN_HOOK(fn, library) static struct mcn_hook mcn_hook_##fn = { #fn, library, 0, 0 }
#define MCN_REAL(fn) mcn_hook_enter(&mcn_hook_##fn)

#define MCN_STR_(x) #x
#define MCN_STR(x) MCN_STR_(x)

#define MCN_OBSERVE(fn, library) \
    __attribute__((used)) struct mcn_hook mcn_hook_##fn = { #fn, library, 0, 0 }; \
    __asm__(".text\n" \
            ".global " #fn "\n" \
            ".type " #fn ", %function\n" \
            ".arm\n" \
            #fn ":\n" \
            "    push {r0-r6, lr}\n" \
            "    ldr r0, 2f\n" \
            "1:  add r0, pc, r0\n" \
            "    bl mcn_hook_enter\n" \
            "    mov ip, r0\n" \
            "    pop {r0-r6, lr}\n" \
            "    bx ip\n" \
            "2:  .word mcn_hook_" #fn " - (1b + 8)\n" \
            ".size " #fn ", . - " #fn "\n")

#endif
