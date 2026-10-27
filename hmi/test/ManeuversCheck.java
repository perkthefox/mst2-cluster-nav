/*
 * mst2-cluster-nav - host self-check of the maneuver mapping (JDK 8: hmi/check.sh).
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

public final class ManeuversCheck {
    private static int failures;

    private static void expect(String what, int type, int angle, boolean lht, int main, int dir) {
        int[] got = Maneuvers.map(type, angle, lht);
        if (got[0] != main || got[1] != dir) {
            failures++;
            System.out.println("FAIL " + what + ": got " + got[0] + "/" + got[1]
                + ", want " + main + "/" + dir);
        }
    }

    /*
     * docs/MANEUVERS.md without an angle, every iAP2 type 0..53:
     * {type, right-hand main, direction, left-hand main, direction}.
     */
    private static final int[][] TABLE = {
        { 0, 11, 0, 11, 0 }, { 1, 13, 64, 13, 64 }, { 2, 13, 192, 13, 192 }, { 3, 13, 0, 13, 0 },
        { 4, 25, 64, 25, 192 }, { 5, 11, 0, 11, 0 }, { 6, 13, 192, 13, 64 },
        { 7, 26, 192, 27, 64 }, { 8, 13, 224, 13, 32 }, { 9, 13, 224, 13, 32 },
        { 10, 3, 0, 3, 0 }, { 11, 13, 0, 13, 0 }, { 12, 3, 0, 3, 0 }, { 13, 13, 32, 13, 32 },
        { 14, 13, 224, 13, 224 }, { 15, 11, 0, 11, 0 }, { 16, 13, 0, 13, 0 },
        { 17, 11, 0, 11, 0 }, { 18, 25, 64, 25, 192 }, { 19, 21, 128, 22, 128 },
        { 20, 13, 64, 13, 64 }, { 21, 13, 192, 13, 192 }, { 22, 13, 32, 13, 32 },
        { 23, 13, 224, 13, 224 }, { 24, 3, 64, 3, 64 }, { 25, 3, 192, 3, 192 },
        { 26, 25, 64, 25, 192 }, { 27, 3, 0, 3, 0 },
        /* Roundabout exits 1..19: 1st right, 2nd ahead, 3rd left, later back; mirrored. */
        { 28, 21, 192, 22, 64 }, { 29, 21, 0, 22, 0 }, { 30, 21, 64, 22, 192 },
        { 31, 21, 128, 22, 128 }, { 32, 21, 128, 22, 128 }, { 33, 21, 128, 22, 128 },
        { 34, 21, 128, 22, 128 }, { 35, 21, 128, 22, 128 }, { 36, 21, 128, 22, 128 },
        { 37, 21, 128, 22, 128 }, { 38, 21, 128, 22, 128 }, { 39, 21, 128, 22, 128 },
        { 40, 21, 128, 22, 128 }, { 41, 21, 128, 22, 128 }, { 42, 21, 128, 22, 128 },
        { 43, 21, 128, 22, 128 }, { 44, 21, 128, 22, 128 }, { 45, 21, 128, 22, 128 },
        { 46, 21, 128, 22, 128 },
        { 47, 13, 96, 13, 96 }, { 48, 13, 160, 13, 160 }, { 49, 13, 32, 13, 32 },
        { 50, 13, 224, 13, 224 }, { 51, 13, 0, 13, 0 }, { 52, 12, 64, 12, 64 },
        { 53, 12, 192, 12, 192 },
    };

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL " + what);
        }
    }

    public static void main(String[] args) {
        int none = Maneuvers.NO_ANGLE;
        for (int i = 0; i < TABLE.length; i++) {
            int[] r = TABLE[i];
            check(r[0] == i, "table row " + i + " is type " + i);
            expect("type " + r[0] + " right-hand traffic", r[0], none, false, r[1], r[2]);
            expect("type " + r[0] + " left-hand traffic", r[0], none, true, r[3], r[4]);
            /* An unknown angle (+-1000) is the same as none. */
            expect("type " + r[0] + " angle 1000", r[0], 1000, false, r[1], r[2]);
            expect("type " + r[0] + " angle -1000, lht", r[0], -1000, true, r[3], r[4]);
        }
        check(TABLE.length == 54, "all 54 iAP2 maneuver types");
        expect("unknown type", 99, none, false, 1, 0);
        expect("unknown type 54", 54, none, true, 1, 0);
        expect("no type", -1, none, false, 1, 0);

        /* Angles where the table uses them. */
        expect("left turn", 1, -90, false, 13, 64);
        expect("right turn", 2, 90, false, 13, 192);
        expect("u-turn by angle sign", 4, 170, false, 25, 192);
        expect("u-turn by angle sign, lht", 4, -170, true, 25, 64);
        expect("roundabout exit -90", 30, -90, false, 21, 64);
        expect("roundabout exit +90", 30, 90, false, 21, 192);
        expect("roundabout exit 180", 31, 180, false, 21, 128);
        expect("roundabout exit -67 -> 48", 29, -67, false, 21, 48);
        expect("roundabout exit 2, angle 0", 29, 0, false, 21, 0);
        expect("roundabout exit -90, lht", 30, -90, true, 22, 64);
        expect("start route slight right", 11, 40, false, 13, 224);
        expect("start route sharp left", 11, -150, false, 13, 96);
        expect("start route ahead", 11, 20, false, 13, 0);
        expect("change highway right", 51, 90, false, 13, 192);
        expect("off ramp by sign", 8, -30, false, 13, 32);
        expect("on ramp by sign, lht", 9, 30, true, 13, 224);
        expect("end of road left slight", 20, -40, false, 13, 32);
        expect("end of road right, wrong sign", 21, -40, false, 13, 192);
        expect("end of road right sharp", 21, 150, false, 13, 160);

        String sides = Maneuvers.sideStreets(new int[] { -90, 0, 90, 1000 }, 90);
        check(sides.length() == 2 && sides.charAt(0) == 64 && sides.charAt(1) == 0, "side streets");

        Guidance g = PhoneLink.parse("active=1\ntype=2\nangle=90\nroads=-90,0,90\ndist=100\n"
            + "step=400\nstreet=Седьмая улица\nrtt=600\ndest=2500\n");
        check(g.active && g.mainElement == 13 && g.direction == 192 && g.bargraph == 25
            && g.showBargraph && g.street.equals("Седьмая улица") && g.remainingSeconds == 600
            && g.distanceToDestination == 2500 && g.sideStreets.length() == 2,
            "phone link parse: " + g);
        check(!PhoneLink.parse("active=0").active, "phone link end");
        /* Unknown values stay unknown, never 0 (Android Auto sends no rtt/dest). */
        g = PhoneLink.parse("active=1\ntype=2\n");
        check(g.distanceToManeuver == -1 && g.remainingSeconds == -1
            && g.distanceToDestination == -1 && !g.showBargraph, "missing values: " + g);
        g = PhoneLink.parse("active=1\ntype=2\ndist=-1\nrtt=-1\ndest=-1\nstep=400\n");
        check(g.distanceToManeuver == -1 && g.remainingSeconds == -1
            && g.distanceToDestination == -1 && !g.showBargraph, "unknown values: " + g);

        /* Wall-clock timeouts: a clock set back or far ahead counts as expired. */
        check(!PhoneLink.expired(1000, 3999, 3000) && PhoneLink.expired(1000, 4000, 3000),
            "timeout");
        check(PhoneLink.expired(1000, 999, 3000), "clock set back");
        check(PhoneLink.expired(1000, 1000 + 86400000L * 365 * 24, 3000), "clock set ahead");

        /* Datapool's write rule (ASL thread): own edges only, never over the built-in nav. */
        check(!Datapool.admit(false, false), "inactive without own guidance: nothing written");
        check(Datapool.admit(true, false), "active: written");
        check(Datapool.admit(true, false), "active again: written");
        check(!Datapool.admit(true, true), "built-in navigation guides: not written");
        check(!Datapool.admit(false, true), "phone stops while built-in guides: not written");
        check(!Datapool.admit(false, false), "built-in done, phone inactive: nothing written");
        check(Datapool.admit(true, false), "phone guides again");
        check(Datapool.admit(false, false), "falling edge: inactive written once");
        check(!Datapool.admit(false, false), "repeated inactive: nothing written");

        System.out.println(failures == 0 ? "maneuvers: all 54 types both traffic sides, phone link,"
            + " timeouts, write rule: ok" : "maneuvers: " + failures + " failure(s)");
        System.exit(failures == 0 ? 0 : 1);
    }
}
