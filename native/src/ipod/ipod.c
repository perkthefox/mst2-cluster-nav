/*
 * P0480T iAP2 identification extension: one writable builder slot, no text changes.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 * ABI/offsets: mQbEMU research notes, phone-route-guidance.md E19-E25; rejection
 * counter E32, log framing E36, receive-pool leak E39.
 * Group 30 values are a public protocol hypothesis, still requiring an iPhone test.
 */
#include "mcn_qnx.h"
#include "mcn_hook.h"
#include "mcn_route.h"

#define DRIVER "/lib/dll/ipod-drvr-iap2.so"
#define READY  "/dev/shmem/mst2-cluster-nav-rg-ready"

static int (*location)(void *, void *);
static int (*add_blob)(void *, void *, unsigned, const void *, unsigned);
const char mcn_component[] = "ipod";

/* Every control-session packet passes link_handle_reply (0x142dc) before the message
 * switch. With no msg_info for its ID it logs "Invalid packet" and returns 22, and
 * transport_recv (0x26288) does not give a packet with a positive result back to the
 * receive pool: each 0x52xx from the phone leaks one, the pool runs dry after a few, and
 * the next LocationInformation ACK is lost, so SAL stops iAP2 (E39). A named entry
 * without the reply bit (4) returns 0 there; 0x52xx then falls through the switch to
 * r6 = 0 and the packet goes back. The row keeps a NULL name: print_packet then keeps the
 * "--?-- [msgid=..] CTRL[..]" framing the route reader parses (E36). 0x5200 and 0x5203
 * stay unnamed, so link_send_message sends them as before (no 0x20 "disabled" bit).
 */
static struct mcn_msg_info route_msgs[5] = {
    [1] = {"RouteGuidanceUpdate", 2, 0, NULL},
    [2] = {"RouteGuidanceManeuverUpdate", 2, 0, NULL},
    [4] = {"LaneGuidanceInformation", 2, 0, NULL},
};

_Static_assert(sizeof(void *) != 4 || (sizeof(struct mcn_msg_group) == 12 &&
                                        sizeof(struct mcn_msg_info) == 12),
               "iap2_ctrl_msg_table layout");

int mcn_route_msg_group(struct mcn_msg_group *group)
{
    if (group->name || group->info || group->count) {
        return -1;
    }
    group->info = route_msgs;
    group->count = 5;
    return 0;
}

/*
 * Reception of the phone's iAP2 packets (P0480T). ipod-transport-usbdevice.so queues a "data"
 * event per finished USB transfer (usbdevice_receive_cb 0x1360 -> usbdevice_upcall 0x1ee0, a ring
 * of 8 with no full check: a 9th pending event makes it look empty), and usbdevice_pulse (0x1d08)
 * calls the driver's receive (iap2_drvr + 0x20 = transport_recv_pkt 0x26038) once per event. That
 * call hands over exactly one packet: link_get_transport_data (0x14b50) returns a complete packet
 * already in the link buffer (link = dev + 0x7c, read/write pointers at +0x4c/+0x50) without
 * touching the transport, and otherwise moves all the transport holds into that buffer
 * (transport_receive 0x25f38, timeout 0). Packets that came together, or whose events were lost,
 * so wait there for the phone's next transfer. When one of them is the ACK the driver is waiting
 * for (LocationInformation, 2 s), SAL reports "Device timed out" and stops iAP2: with route
 * guidance (about 18 packets a second while moving) a question of time, while the stock channel
 * is nearly idle. The driver's receive entry is wrapped to read on while the link buffer moves,
 * through the driver's own function and locks. In the car (2026-10-26, a build that counted
 * them) a packet was read past its event this way about once every few minutes of guidance.
 */
#define DRVR_RECV 0xafce8u /* iap2_drvr + 0x20 */
#define RECV_PKT 0x26038u
#define DRAIN_MAX 16

static int (*recv_pkt)(void *);

static long link_pending(void *dev)
{
    const uint8_t *link = *(uint8_t *const *)((const uint8_t *)dev + 0x7c);

    return link ? *(const long *)(link + 0x50) - *(const long *)(link + 0x4c) : 0;
}

static int recv_all(void *dev)
{
    int result = recv_pkt(dev), i;
    long before;

    for (i = 0; i < DRAIN_MAX && (before = link_pending(dev)) > 0; i++) {
        recv_pkt(dev);
        if (link_pending(dev) == before) {
            break; /* only part of a packet: the rest comes with the next transfer */
        }
    }
    return result;
}

static int is_ipod(void)
{
    /* Use a real handle: QNX's special dlsym handles are caller-sensitive,
     * and procfs executable names are unavailable here during early DT_INIT.
     * The main image of mm-ipod exports ipod_log for its driver modules.
     */
    void *main_image = dlopen(NULL, RTLD_LAZY);

    if (!main_image || !dlsym(main_image, "ipod_log")) {
        mcn_log("iAP2 preload: main image has no ipod_log, stock behaviour");
        return 0;
    }
    return 1;
}

static int build(void *dev, void *packet)
{
    uint8_t payload[128];
    unsigned n;
    int fd, result;

    /* Crash guard: busy only while an identification is built (see mcn_init). */
    mcn_busy(1);
    result = location(dev, packet);
    if (result < 0 || !mcn_active()) {
        goto done;
    }
    /* Every 0x1d03 increments this counter before rebuilding (E32, 0x16008).
     * Drop our component on ANY rejection, even if a different group was rejected.
     * Do not guess a new feature bit in dev+0xa4: stock has no group-30 bit.
     */
    if (*(const uint32_t *)((const uint8_t *)dev + 0xa8)) {
        unlink(READY);
        mcn_log("identification retried: group 30 omitted, stock fallback");
        goto done;
    }
    n = mcn_route_component(payload);
    if (add_blob(dev, packet, 30, payload, n) < 0) {
        unlink(READY);
        mcn_log("group 30 did not fit: stock identification retained");
        goto done;
    }
    fd = open(READY, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, "offered\n", 8);
        close(fd);
    }
    mcn_log("identification: route guidance group 30 offered");
done:
    mcn_busy(0);
    return result;
}

/* Checks the driver build and wraps the Location builder and the receive entry. */
static void patch(void)
{
    void *driver, *module, *blob;
    uintptr_t base;
    uint32_t *slot, *recv;
    struct mcn_msg_group *table;

    mcn_log("iAP2 hooks loaded");
    /* Load once, before mm-ipod's iface_find can start identification. Its later dlopen
     * reuses this mapping. Polling a builder table from a worker would race the phone.
     */
    driver = dlopen(DRIVER, RTLD_LAZY | RTLD_GLOBAL);
    module = driver ? dlsym(driver, "ipod_module") : NULL;
    blob = driver ? dlsym(driver, "packet_addparam_blob") : NULL;
    if (!module || !blob || (uintptr_t)module < 0x2d7d8) {
        mcn_log("iAP2 driver unavailable: stock behaviour");
        return;
    }
    base = (uintptr_t)module - 0x2d7d8;
    if ((uintptr_t)blob != base + 0x20c9c ||
        (uintptr_t)dlsym(driver, "ident_info_funcs") != base + 0xafc08) {
        mcn_log("iAP2 driver build mismatch: no data changed");
        return;
    }
    slot = (uint32_t *)(base + 0xafcb8);
    table = dlsym(driver, "iap2_ctrl_msg_table");
    if (slot[0] != 0x16 || slot[1] != base + 0xea08 || (uintptr_t)table != base + 0xb2fc0) {
        mcn_log("iAP2 Location slot mismatch: no data changed");
        return;
    }
    /* Without the 0x52 row every route message leaks a receive packet (see route_msgs):
     * then group 30 is not offered at all.
     */
    if (mcn_route_msg_group(&table[0x52]) < 0) {
        mcn_log("iAP2 message group 0x52 not empty: no data changed");
        return;
    }
    location = (void *)(uintptr_t)slot[1];
    add_blob = blob;
    slot[1] = (uint32_t)(uintptr_t)build;
    mcn_log("iAP2 Location slot checked and wrapped (P0480T)");
    /* Same build: the receive entry the transport calls per data event (see recv_all). */
    recv = (uint32_t *)(base + DRVR_RECV);
    if (*recv == base + RECV_PKT) {
        recv_pkt = (void *)(uintptr_t)*recv;
        *recv = (uint32_t)(uintptr_t)recv_all;
    } else {
        mcn_log("iAP2 receive entry mismatch: stock reception");
    }
}

MCN_EXPORT void mcn_init(void)
{
    /* /bin/on inherits the preload too; only mm-ipod may load the driver. */
    if (!mcn_active() || !is_ipod()) {
        return;
    }
    /*
     * Crash guard (common/hook.c): busy only around our own risky work, patch() and each
     * identification build (build). mm-ipod dying inside, twice in a row, turns these hooks
     * off until the next Install; SAL then still starts it, with this library inert. A plain
     * exit outside them is no strike. The driver's own exits (P0480T) are "Does not support
     * iAP2, exiting." in transport_recv_pkt (0x261fc: a USB device that never speaks iAP2) and
     * the seventh rejection in link_handle_iap2pkt (0x16034); both lie on the receive path,
     * outside the builders, which only ident_info_request runs (called from 0x15fc8, 0x1604c),
     * and the Location builder 0xea08 calls no exit. The receive wrapper is not bracketed: it
     * runs per USB transfer and exit(0) above is reached through it.
     */
    mcn_guard_init();
    if (!mcn_active()) {
        return;
    }
    mcn_busy(1);
    patch();
    mcn_busy(0);
}
