/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

/**
 * CarPlay route guidance maneuver (iAP2 ManeuverUpdate: type 0..53, exit angle, driving side,
 * junction type, junction element angles) to the cluster's BAP ManeuverDescriptor (main element,
 * direction, side streets). The type semantics follow the public iAP2 route-guidance
 * descriptions (see docs/MANEUVERS.md for the table and its sources).
 *
 * Angles: iAP2 degrees, negative = left. BAP direction: 0..255 counter-clockwise, 0 ahead,
 * 64 left, 128 back, 192 right (verified on the cluster firmware in mQbEMU). An angle of +-1000 means
 * "unknown"; NO_ANGLE means the field was absent.
 *
 * Android Auto describes a roundabout exit by "around": degrees travelled round the roundabout
 * from the entry to the exit, 1..360, 180 = straight across. Traffic goes round counter-clockwise
 * under right-hand traffic (first exit on the right at about 90) and clockwise under left-hand
 * traffic, so the exit angle is 180 - around or around - 180 (aroundToAngle).
 */
final class Maneuvers {
    static final int NO_ANGLE = Integer.MIN_VALUE;

    /* BAP ManeuverDescriptor main elements. */
    static final int NO_INFO = 1;
    static final int ARRIVED = 3;
    static final int FOLLOW_STREET = 11;
    static final int CHANGE_LANE = 12;
    static final int TURN = 13;
    static final int ROUNDABOUT_RIGHT_TRAFFIC = 21;
    static final int ROUNDABOUT_LEFT_TRAFFIC = 22;
    static final int UTURN = 25;
    static final int EXIT_ROUNDABOUT_RIGHT_TRAFFIC = 26;
    static final int EXIT_ROUNDABOUT_LEFT_TRAFFIC = 27;

    static final int AHEAD = 0;
    static final int SLIGHT_LEFT = 32;
    static final int LEFT = 64;
    static final int SHARP_LEFT = 96;
    static final int BACK = 128;
    static final int SHARP_RIGHT = 160;
    static final int RIGHT = 192;
    static final int SLIGHT_RIGHT = 224;

    /** @return {main element, direction} */
    static int[] map(int type, int angle, boolean leftHandTraffic) {
        return map(type, angle, leftHandTraffic, 0);
    }

    /** @param around Android Auto's roundabout travel, 1..360, else unknown */
    static int[] map(int type, int angle, boolean leftHandTraffic, int around) {
        boolean known = angle != NO_ANGLE && angle != 1000 && angle != -1000;
        int round = leftHandTraffic ? ROUNDABOUT_LEFT_TRAFFIC : ROUNDABOUT_RIGHT_TRAFFIC;
        switch (type) {
        case 0: case 5: case 15: case 17:
            return pair(FOLLOW_STREET, AHEAD);
        case 1:
            return pair(TURN, LEFT);
        case 2:
            return pair(TURN, RIGHT);
        case 3:
            return pair(TURN, AHEAD);
        case 4: case 18: case 26: {
            /* A U-turn is a loop to one side, never "back". */
            boolean left = known && angle != 0 ? angle < 0 : !leftHandTraffic;
            return pair(UTURN, left ? LEFT : RIGHT);
        }
        case 6:
            /* Entering a roundabout; Android Auto may say how far round without an exit number. */
            if (around >= 1 && around <= 360) {
                return pair(round, sixteenths(aroundToAngle(around, leftHandTraffic)));
            }
            return pair(TURN, leftHandTraffic ? LEFT : RIGHT);
        case 7:
            return leftHandTraffic ? pair(EXIT_ROUNDABOUT_LEFT_TRAFFIC, LEFT)
                                   : pair(EXIT_ROUNDABOUT_RIGHT_TRAFFIC, RIGHT);
        case 8: case 9: {
            boolean left = known && angle != 0 ? angle < 0 : leftHandTraffic;
            return pair(TURN, left ? SLIGHT_LEFT : SLIGHT_RIGHT);
        }
        case 10: case 12: case 27:
            return pair(ARRIVED, AHEAD);
        case 11: case 16: case 51:
            return pair(TURN, known ? quantizeTurn(angle) : AHEAD);
        case 13: case 49: case 22:
            return pair(TURN, SLIGHT_LEFT);
        case 14: case 50: case 23:
            return pair(TURN, SLIGHT_RIGHT);
        case 19:
            return pair(round, BACK);
        case 20:
            return pair(TURN, endOfRoad(angle, known, true));
        case 21:
            return pair(TURN, endOfRoad(angle, known, false));
        case 24:
            return pair(ARRIVED, LEFT);
        case 25:
            return pair(ARRIVED, RIGHT);
        case 47:
            return pair(TURN, SHARP_LEFT);
        case 48:
            return pair(TURN, SHARP_RIGHT);
        case 52:
            return pair(CHANGE_LANE, LEFT);
        case 53:
            return pair(CHANGE_LANE, RIGHT);
        default:
            if (type >= 28 && type <= 46) {
                /* Roundabout exit 1..19: the exit's angle in 22.5 degree steps; without an
                 * angle (some apps send none or 0) the exit number gives the usual four-arm
                 * picture: 1st right, 2nd ahead, 3rd left, later ones back (mirrored for
                 * left-hand traffic). Android Auto's travel round it says the exit's angle even
                 * straight across, where an angle of 0 would read as none. */
                if (around >= 1 && around <= 360) {
                    return pair(round, sixteenths(aroundToAngle(around, leftHandTraffic)));
                }
                if (known && angle != 0) {
                    return pair(round, sixteenths(angle));
                }
                return pair(round, exitByNumber(type - 27, leftHandTraffic));
            }
            return pair(NO_INFO, AHEAD);
        }
    }

    /** Exit angle (iAP2 degrees, negative = left) of a roundabout exit "around" degrees round. */
    static int aroundToAngle(int around, boolean leftHandTraffic) {
        return leftHandTraffic ? around - 180 : 180 - around;
    }

    /** Side streets of the junction: every road angle except the exit and unknown ones. */
    static String sideStreets(int[] roads, int exitAngle) {
        StringBuffer s = new StringBuffer();
        for (int i = 0; roads != null && i < roads.length && s.length() < 17; i++) {
            int a = roads[i];
            if (a == exitAngle || a < -180 || a > 180) {
                continue;
            }
            s.append((char) sixteenths(a));
        }
        return s.toString();
    }

    /** iAP2 degrees (negative = left) to BAP direction in 22.5 degree steps. */
    static int sixteenths(int angle) {
        int steps = (int) Math.floor(-angle / 22.5 + 0.5);
        return (steps * 16) & 0xff;
    }

    /** Generic turn: straight, slight, normal or sharp to the side of the angle. */
    static int quantizeTurn(int angle) {
        int a = Math.abs(angle);
        if (a <= 22) {
            return AHEAD;
        }
        boolean left = angle < 0;
        if (a <= 67) {
            return left ? SLIGHT_LEFT : SLIGHT_RIGHT;
        }
        if (a <= 112) {
            return left ? LEFT : RIGHT;
        }
        return left ? SHARP_LEFT : SHARP_RIGHT;
    }

    private static int exitByNumber(int exit, boolean leftHandTraffic) {
        switch (exit) {
        case 1:
            return leftHandTraffic ? LEFT : RIGHT;
        case 2:
            return AHEAD;
        case 3:
            return leftHandTraffic ? RIGHT : LEFT;
        default:
            return BACK;
        }
    }

    private static int endOfRoad(int angle, boolean known, boolean left) {
        if (!known || angle == 0 || (angle < 0) != left) {
            return left ? LEFT : RIGHT;
        }
        return quantizeTurn(angle);
    }

    private static int[] pair(int main, int direction) {
        return new int[] { main, direction };
    }

    private Maneuvers() {
    }
}
