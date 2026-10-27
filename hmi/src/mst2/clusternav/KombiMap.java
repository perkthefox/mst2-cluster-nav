/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import de.vw.mib.asl.framework.api.framework.ASLFrameworkFactory;
import de.vw.mib.asl.framework.internal.framework.ServiceManager;

/**
 * The built-in navigation's cluster map, hidden while the phone's video owns the cluster.
 *
 * Without this the navigation keeps drawing the cluster map (and DisplayManager keeps encoding
 * it) next to CarPlay while the phone's frames replace it on the way to the cluster. The events
 * are the stock ones the HMI sends when the cluster changes DataRate
 * (de.vw.mib.asl.internal.mostkombi.streamsink.api.navimap.NavigationMapAdapter.setMapInvisible
 * and setMapVisible): MapViewerKombiStateMain goes to MapViewerKombiStateInvisible, whose start
 * calls MapViewerKombiNotifierDSI.viewSetInvisible. Nothing in BAP reads the map's visibility
 * (datapool 1110045), so the cluster is not told. A DataRate change while hidden may show the map
 * again (ChangeDataRateSequence), so it is hidden again every RESEND_MS; the invisible state
 * ignores a repeat.
 */
final class KombiMap {
    private static final int SET_VISIBLE = 1074841845;
    private static final int SET_INVISIBLE = 1074841846;
    private static final long RESEND_MS = 2000;

    private static boolean hidden;
    private static long sentMs;

    private KombiMap() {
    }

    /** PhoneLink's thread, about once a second: is the phone's cluster video alive. */
    static void phoneVideo(boolean video) {
        long now = System.currentTimeMillis();

        if (video && (!hidden || PhoneLink.expired(sentMs, now, RESEND_MS))) {
            if (!hidden) {
                Activator.log("built-in cluster map hidden");
            }
            hidden = true;
            sentMs = now;
            send(SET_INVISIBLE);
        } else if (!video && hidden) {
            /*
             * ponytail: shown even if the cluster left its map view meanwhile (DataRate 0); the
             * navigation then draws an unseen map until the next DataRate change. Read datapool
             * 2110011 (0 = map off) first if that ever costs.
             */
            hidden = false;
            Activator.log("built-in cluster map shown");
            send(SET_VISIBLE);
        }
    }

    /* On the ASL thread, as the stock adapter sends them. */
    private static void send(final int event) {
        try {
            ASLFrameworkFactory.getASLFrameworkAPI().getServices().getASLThreadSwitch().enqueue(
                new Runnable() {
                    public void run() {
                        try {
                            ServiceManager.eventMain.getEventDispatcher().sendSafe(
                                ServiceManager.mGenericEventFactory.newEvent(event));
                        } catch (Throwable t) {
                            Activator.log("cluster map event not sent: " + t);
                        }
                    }
                });
        } catch (Throwable t) {
            Activator.log("ASL thread not available: " + t);
        }
    }
}
