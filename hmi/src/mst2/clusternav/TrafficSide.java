/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import org.dsi.ifc.global.NavLocation;
import org.dsi.ifc.navigation.CountryInfo;

import de.vw.mib.asl.framework.internal.framework.ServiceManager;
import de.vw.mib.asl.internal.navigation.gateway.NavGateway;
import de.vw.mib.configuration.ConfigurationManager;

/**
 * The traffic side for guidance that does not carry one (Android Auto: lht=-1), in this order:
 *
 * 1. The built-in navigation's country: the vehicle position's country
 *    (NavGateway.getSoPosPositionDescription().countryAbbreviation, DSI attribute 29) looked up
 *    in its country table (NavGateway.getCountryInfo(), attribute 84,
 *    CountryInfo.rightHandTraffic). NavGateway takes every DSINavigation attribute
 *    (setNotification without a list); whether the navigation fills attribute 84 without a
 *    request is not known, so a missing entry falls through.
 *    Read only: NavGateway.isInstance() first, so no navigation object is ever created here.
 * 2. The steering wheel side from the unit's coding (CodingSetter.processSteeringWheelSide: coding
 *    value 80, 0 left, 1 right -> ConfigurationManagerDiag.setDriverSide 1 / 2): a right-hand drive
 *    car drives on the left.
 * 3. Right-hand traffic.
 *
 * Resolved at most every CACHE_MS; never throws.
 */
final class TrafficSide {
    private static final long CACHE_MS = 60000;

    private static long resolvedMs;
    private static boolean left;
    private static String source = "";

    /** Left-hand traffic where the phone does not say (PhoneLink and Replay threads). */
    static synchronized boolean leftHand() {
        long now = System.currentTimeMillis();
        if (resolvedMs != 0 && !PhoneLink.expired(resolvedMs, now, CACHE_MS)) {
            return left;
        }
        int nav = navRightHand();
        int driver = driverSide();
        boolean l = decide(nav, driver);
        String s = nav >= 0 ? "navigation country"
                 : driver == ConfigurationManager.DRIVERSIDE_LEFT
                   || driver == ConfigurationManager.DRIVERSIDE_RIGHT ? "steering wheel coding"
                 : "default";
        if (resolvedMs == 0 || l != left || !s.equals(source)) {
            Activator.log("traffic side for Android Auto: " + (l ? "left" : "right")
                + " (" + s + ")");
        }
        resolvedMs = now;
        left = l;
        source = s;
        return l;
    }

    /**
     * @param navRightHand 1 / 0 from the navigation's country, -1 unknown
     * @param driverSide ConfigurationManager.DRIVERSIDE_*
     */
    static boolean decide(int navRightHand, int driverSide) {
        if (navRightHand >= 0) {
            return navRightHand == 0;
        }
        return driverSide == ConfigurationManager.DRIVERSIDE_RIGHT;
    }

    /** 1 right-hand, 0 left-hand traffic in the country with this code, -1 not in the table. */
    static int rightHand(String code, CountryInfo[] countries) {
        if (code == null || code.length() == 0 || countries == null) {
            return -1;
        }
        for (int i = 0; i < countries.length; i++) {
            if (countries[i] != null && code.equals(countries[i].countryAbbreviation)) {
                return countries[i].rightHandTraffic ? 1 : 0;
            }
        }
        return -1;
    }

    private static int navRightHand() {
        try {
            if (!NavGateway.isInstance()) {
                return -1;
            }
            NavGateway nav = NavGateway.getInstance();
            if (!nav.isSoPosPositionDescriptionValid() || !nav.isCountryInfoValid()) {
                return -1;
            }
            NavLocation here = nav.getSoPosPositionDescription();
            return here == null ? -1 : rightHand(here.countryAbbreviation, nav.getCountryInfo());
        } catch (Throwable t) {
            return -1;
        }
    }

    private static int driverSide() {
        try {
            return ServiceManager.configManagerDiag.getDriverSide();
        } catch (Throwable t) {
            return ConfigurationManager.DRIVERSIDE_UNDEFINED;
        }
    }

    private TrafficSide() {
    }
}
