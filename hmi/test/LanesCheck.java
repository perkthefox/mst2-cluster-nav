/*
 * Lane snapshot contract and mapping checks, including an actual native snapshot.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.io.File;
import de.vw.mib.asl.api.navbap.INavLaneGuidanceDataNavBap;

public final class LanesCheck {
    private static void check(boolean ok, String name) {
        if (!ok) {
            throw new RuntimeException(name);
        }
    }

    private static Guidance guidance(String rows) {
        return PhoneLink.parse("active=1\ntype=2\n" + rows);
    }

    public static void main(String[] args) throws Exception {
        Guidance g = guidance("lanes=3\nlane0=2,2,90,0\nlane1=0,0,1000,-90\n"
            + "lane2=1,1,1000,0\n");
        check(g.lanes.length == 3, "three lanes");
        check(g.lanes[0].getLaneDirection() == 64 && g.lanes[0].getGuidanceInfo() == 0,
            "leftmost: left, not recommended");
        check(g.lanes[1].getLaneDirection() == 0 && g.lanes[1].getGuidanceInfo() == 1,
            "middle: ahead, recommended");
        INavLaneGuidanceDataNavBap right = g.lanes[2];
        check(right.getLaneDirection() == 192 && right.getGuidanceInfo() == 2,
            "rightmost: right, best recommendation");
        check(right.getLaneSideStreets().length == 1 && right.getLaneSideStreets()[0] == 0,
            "straight option preserved beside highlighted right exit");
        check(right.getLaneType() == 0 && right.getLaneDescription() == 15
            && right.getLaneMarkingLeft() == 0 && right.getLaneMarkingRight() == 0,
            "do not invent road type, paint or merging");
        check(Lanes.direction(-180) == 128 && Lanes.direction(180) == 128
            && Lanes.direction(-45) == 32 && Lanes.direction(45) == 224,
            "16-sector geometry");
        check(Lanes.direction(1000) == 255 && Lanes.direction(-1000) == 255
            && Lanes.direction(181) == 255 && Lanes.direction(-181) == 255,
            "unknown before quantization");
        check(guidance("lanes=1\nlane0=0,2,1000,0,90\n").lanes[0].getGuidanceInfo() == 0,
            "ambiguous multi-exit lane has no fabricated recommendation");
        check(guidance("lanes=1\nlane0=0,2,1000,1000,-1000\n").lanes.length == 0,
            "all angles unknown hides lanes");
        check(guidance("lanes=0\n").lanes.length == 0, "hide");
        check(PhoneLink.parse("active=0\nlanes=1\nlane0=0,2,0\n").lanes.length == 0, "stop");
        check(guidance("lanes=2\nlane0=0,2,0\n").lanes.length == 0, "missing row");
        check(guidance("lanes=2\nlane0=0,2,0\nlane1=0,1,90\n").lanes.length == 0,
            "duplicate position");
        check(guidance("lanes=1\nlane0=0,2,0\nlane0=1,2,90\n").lanes.length == 0,
            "duplicate row");
        check(guidance("lanes=17\n").lanes.length == 0, "bounded rows");
        check(guidance("lanes=1\nlane0=0,2,0\nlane1=1,2,90\n").lanes.length == 0,
            "count must describe the complete batch");
        check(guidance("lanes=0\nlanes=1\nlane0=0,2,0\n").lanes.length == 0,
            "duplicate count cannot revive a hidden batch");
        check(guidance("lanes=1\nlane0=65535,2,0\n").lanes.length == 0,
            "unknown position");
        check(guidance("lanes=1\nlane0=0,nonsense,0\n").lanes.length == 0,
            "malformed field");
        if (args.length == 0 || !new File(args[0]).isFile()) {
            throw new RuntimeException("native lane snapshot missing (run native/check.sh first): "
                + (args.length > 0 ? args[0] : "no path"));
        }
        Guidance nativeSnapshot = PhoneLink.parse(Replay.read(args[0]));
        check(nativeSnapshot.lanes.length == 3, "native to HMI lane count");
        check(nativeSnapshot.lanes[2].getLaneDirection() == 192
            && nativeSnapshot.lanes[2].getGuidanceInfo() == 2
            && nativeSnapshot.lanes[2].getLaneSideStreets().length == 1,
            "native to HMI highlight and side exit");
        System.out.println("lanes: native snapshot -> PhoneLink -> NavBAP interface: ok");
        System.out.println("lanes: mapping, sorting, unknowns, hide and malformed batches: ok");
    }
}
