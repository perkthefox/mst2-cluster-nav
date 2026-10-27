/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

/**
 * One route-guidance snapshot from the phone, already in the units the head unit's
 * navigation-to-cluster path expects (BAP NavSD ManeuverDescriptor encoding, metres,
 * seconds).
 */
final class Guidance {
    /* BAP NavSD ManeuverDescriptor main elements used by this mod. */
    static final int NO_SYMBOL = 0;
    static final int ARRIVED = 3;
    static final int FOLLOW_STREET = 11;
    static final int TURN = 13;
    static final int ROUNDABOUT_RIGHT = 21;
    static final int UTURN = 25;

    boolean active;
    int mainElement;
    /* Exit / turn angle, 0..255; the scale is calibrated in mQbEMU (docs/MANEUVERS.md). */
    int direction;
    /* One character per side street, same encoding as direction. */
    String sideStreets = "";
    /* Metres; -1 unknown (BAP NavSD shows no distance for 0 or less). */
    int distanceToManeuver = -1;
    boolean showBargraph;
    /* 0..100, how much of the approach to the maneuver is left. */
    int bargraph;
    String street = "";
    long remainingSeconds = -1;
    int distanceToDestination = -1;
    de.vw.mib.asl.api.navbap.INavLaneGuidanceDataNavBap[] lanes = Lanes.EMPTY;

    static Guidance inactive() {
        return new Guidance();
    }

    public String toString() {
        return "active=" + active + " element=" + mainElement + " direction=" + direction
            + " sides=" + sideStreets.length() + " distance=" + distanceToManeuver
            + " bargraph=" + (showBargraph ? bargraph : -1) + " street=" + street
            + " rtt=" + remainingSeconds + " dest=" + distanceToDestination;
    }
}
