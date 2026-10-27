/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import de.vw.mib.asl.api.navbap.ASLNavBAPAPI;
import de.vw.mib.asl.api.navbap.ASLNavBAPFactory;
import de.vw.mib.asl.api.navigation.guidance.ASLCalculationState;
import de.vw.mib.asl.api.navigation.guidance.ASLNavigationGuidanceAPI;
import de.vw.mib.asl.framework.api.framework.ASLFrameworkFactory;
import de.vw.mib.asl.framework.api.framework.Services;
import de.vw.mib.asl.framework.internal.framework.GenericCollector;
import de.vw.mib.asl.internal.navigation.guidance.ASLNavGuidanceDP;

/**
 * Writes guidance into the HMI datapool exactly where the built-in navigation puts it
 * (de.vw.mib.asl.internal.navigation.bap.HsmTargetBap); the stock NavigationASLDataAdapter
 * turns it into BAP NavSD for the cluster and NavInfoAdapter into MOST NavInfo.
 * All writes run on the ASL task thread, like the stock producer.
 *
 * The built-in navigation always wins: HsmTargetBap writes the route guidance state only when
 * its own state changes and the maneuver only when a new one comes, so anything written over
 * it would stay until its next change. While it calculates or guides (the same state its 732
 * writes come from) nothing is written, and an inactive snapshot is written only when the last
 * write of ours was active, never on its own. The same rule holds for live and replayed
 * guidance, which both come through here.
 */
final class Datapool {
    /* ASL property: route guidance state (0 off, 1 active, 2 calculating). */
    private static final int ROUTE_GUIDANCE_STATE = 732;
    /* ASL list: BAP ManeuverDescriptor (main element, direction, z-level, side streets). */
    private static final int MANEUVER_DESCRIPTOR = 751;
    /* Distance unit as HsmTargetBap passes it: let the HMI pick km or miles. */
    private static final int UNIT_AUTO = 255;

    /* Our last write left guidance active; ASL thread only. */
    private static boolean published;
    private static boolean yielding;

    static void publish(final Guidance g) {
        try {
            services().getASLThreadSwitch().enqueue(new Runnable() {
                public void run() {
                    try {
                        write(g);
                    } catch (Throwable t) {
                        Activator.log("guidance not written: " + t);
                    }
                }
            });
        } catch (Throwable t) {
            /* The callers' threads must survive an ASL that is not up (yet). */
            Activator.log("ASL thread not available: " + t);
        }
    }

    private static Services services() {
        return ASLFrameworkFactory.getASLFrameworkAPI().getServices();
    }

    /*
     * Calculating or guiding: the state HsmTargetGuidance keeps in its datapool and sends as the
     * events HsmTargetBap turns into 732 = 2 and 1. Read from the datapool singleton, so no
     * navigation service is ever created by the mod; without navigation it stays idle.
     */
    private static boolean builtInGuides() {
        try {
            ASLNavGuidanceDP nav = ASLNavGuidanceDP.getInstance();
            if (nav.getGuidanceState() == ASLNavigationGuidanceAPI.GUIDANCE_STATE_GUIDING) {
                return true;
            }
            ASLCalculationState[] calc = nav.getCalculationState();
            return calc != null && calc.length > 0 && calc[0] != null
                && calc[0].getCalculationState()
                    == ASLNavigationGuidanceAPI.CALCULATION_STATE_CALCULATING;
        } catch (Throwable t) {
            /* Never let the check stop the phone's guidance. */
            return false;
        }
    }

    /** ASL thread: whether a snapshot is written, keeping the edge state (host-checked). */
    static boolean admit(boolean active, boolean builtInGuides) {
        if (builtInGuides) {
            /*
             * ponytail: the phone's last lanes or street may stay until the built-in navigation
             * writes its own; clearing them here could wipe what it already wrote.
             */
            if (!yielding) {
                Activator.log("built-in navigation guides: phone guidance not written");
            }
            yielding = true;
            published = false;
            return false;
        }
        if (yielding) {
            yielding = false;
            Activator.log("built-in navigation idle: phone guidance written again");
        }
        if (!active && !published) {
            return false;
        }
        published = active;
        return true;
    }

    private static void write(Guidance g) {
        boolean active = g.active && !PhoneLink.switchedOff();
        if (!admit(active, builtInGuides())) {
            return;
        }

        Services services = services();
        ASLNavBAPAPI bap = ASLNavBAPFactory.getNavBAPApi();

        GenericCollector maneuver = new GenericCollector();
        maneuver.setIntItem(0, active ? g.mainElement : Guidance.NO_SYMBOL);
        maneuver.setIntItem(1, active ? g.direction : 0);
        maneuver.setIntItem(2, 0);
        maneuver.setStringItem(3, active ? g.sideStreets : "");
        services.getASLListManager().getGenericASLList(MANEUVER_DESCRIPTOR)
            .updateList(new GenericCollector[] { maneuver });

        if (active) {
            /* A distance of 0 or less is invalid to BAP NavSD DistanceToNextManeuver. */
            bap.updateBapDistanceToNextManeuver(g.distanceToManeuver, 0, g.showBargraph, g.bargraph);
            bap.updateBapTurnToInfo(g.street, "");
            if (g.remainingSeconds >= 0) {
                bap.updateBapTimeToDestinationRTT(g.remainingSeconds);
            } else {
                /* HsmTargetBap.updateDistanceTime's invalid time: RTT and ETA not valid. */
                bap.updateBapTimeToDestination2(-1, -1, 0);
            }
            /*
             * BAP NavSD has no invalid destination distance (valid while 732 is 1, -1 shows
             * 0.0 km), so an unknown one leaves list 749 as it is.
             */
            if (g.distanceToDestination >= 0) {
                bap.updateBapDistanceToDestination(g.distanceToDestination, UNIT_AUTO, false);
            }
        } else {
            bap.updateBapDistanceToNextManeuver(-1, 0, false, 0);
            bap.updateBapTurnToInfo("", "");
            bap.updateBapTimeToDestination2(-1, -1, 0);
            bap.updateBapDistanceToDestination(-1, UNIT_AUTO, false);
        }
        bap.updateBapLaneGuidance(active ? g.lanes : Lanes.EMPTY, active && g.lanes.length > 0);
        /* Last, so every consumer sees complete data when guidance turns active. */
        services.getASLPropertyManager().valueChangedInteger(ROUTE_GUIDANCE_STATE, active ? 1 : 0);
    }

    private Datapool() {
    }
}
