/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.util.Iterator;

import de.vw.mib.asl.api.carplay.ASLCarPlayFactory;
import de.vw.mib.asl.api.carplay.ASLCarplayServices;
import de.vw.mib.asl.api.carplay.ASLCarplayServicesCallState;
import de.vw.mib.asl.api.carplay.ASLCarplayServicesListener;
import de.vw.mib.bap.mqbab2.common.api.APIFactory;
import de.vw.mib.bap.mqbab2.common.api.asl.ASLDataPoolAdapter;
import de.vw.mib.bap.mqbab2.common.api.carplay.CarPlayService;
import de.vw.mib.bap.mqbab2.common.api.carplay.datatypes.iterator.elements.CarPlayCall;
import de.vw.mib.bap.mqbab2.common.api.smartphoneintegration.SmartphoneIntegrationService;

/**
 * Diagnostics only: which hop of a phone call reaches the cluster's telephone view, logged on
 * change, never a name or a number (only whether one is there).
 *
 * The stock BAP Telephone unit (TelephoneFunctionControlUnit.checkCarPlayActive) takes its CarPlay
 * functions (TelephoneBindingFactoryCarPlay) while the smartphone integration's connection type is
 * 2, else the Bluetooth ones (Android Auto calls go over Bluetooth hands-free); that switch itself
 * is private, so its input is logged. CarPlay calls travel from the CarPlay ASL services
 * (ASLCarplayServices.getCarPlayCallStates) to the BAP CarPlay service
 * (CarPlayASLDataAdapter.getCarPlayCalls), which the CarPlay telephone functions read; both are
 * logged.
 *
 * Polled once a second through those read-only getters instead of registering listeners: both
 * listener lists (ASLCarplayServicesImpl.listeners, the adapter's per-id list) are plain ArrayLists
 * iterated by the CarPlay target's sendCallStatesUpdate, whose thread is not known to be ours, so
 * adding to them could break a stock notification. A failed read only costs this log, and
 * nothing is written into a stock object.
 */
final class CallLog {
    private static final long POLL_MS = 1000;

    /* Set once on the ASL thread (the getters may create a service there, as stock does). */
    private static volatile SmartphoneIntegrationService phone;
    private static volatile CarPlayService bap;
    private static volatile ASLCarplayServices asl;

    /* PhoneLink thread only. */
    private static long polledMs;
    private static int connection = -1;
    private static String aslCalls = "", bapCalls = "", active = "";
    private static boolean aslSeen;

    private CallLog() {
    }

    static void install() {
        ClusterCarPlay.safeEnqueue(new Runnable() {
            public void run() {
                try {
                    Object s = APIFactory.getAPIFactory().getSmartphoneIntegrationService();
                    if (s instanceof ASLDataPoolAdapter) {
                        phone = (SmartphoneIntegrationService)s;
                    }
                    /* ClusterCarPlay stands in the factory's slot when CarPlay is coded. */
                    CarPlayService c = APIFactory.getAPIFactory().getCarPlayService();
                    if (c instanceof ClusterCarPlay || c instanceof ASLCarplayServicesListener) {
                        asl = ASLCarPlayFactory.getCarPlayApi().getServices();
                        bap = c;
                    } else {
                        Activator.log("calls: CarPlay not coded, no CarPlay call log");
                    }
                } catch (Throwable t) {
                    Activator.log("calls: no call log: " + t);
                }
            }
        });
    }

    static void uninstall() {
        phone = null;
        bap = null;
        asl = null;
    }

    /** PhoneLink's loop. */
    static void tick(long now) {
        if (!PhoneLink.expired(polledMs, now, POLL_MS)) {
            return;
        }
        polledMs = now;
        SmartphoneIntegrationService p = phone;
        if (p != null) {
            try {
                int type = p.getSmarphoneIntegrationActiveConnectionType();
                if (type != connection) {
                    connection = type;
                    Activator.log("calls: phone connection type " + type
                        + (type == 2 ? " (BAP telephone: CarPlay functions)"
                                     : " (BAP telephone: Bluetooth functions)"));
                }
            } catch (Throwable t) {
                /* diagnostics only */
            }
        }
        ASLCarplayServices a = asl;
        if (a != null) {
            try {
                String now2 = a.isCarPlayActive() ? "active" : "inactive";
                if (!now2.equals(active)) {
                    active = now2;
                    Activator.log("calls: CarPlay " + now2);
                }
                String calls = a.isReady() ? aslCalls(a.getCarPlayCallStates()) : "";
                if (!calls.equals(aslCalls)) {
                    aslCalls = calls;
                    Activator.log("calls: CarPlay ASL:" + (calls.length() == 0 ? " none" : calls));
                }
            } catch (Throwable t) {
                /* diagnostics only */
            }
        }
        /*
         * The adapter creates its call-id map lazily (getCallIdMap) and fills it when the ASL
         * side notifies it; read only once a call has been there a whole poll, so that map exists
         * and this read never creates it.
         */
        CarPlayService b = bap;
        boolean seen = aslSeen;
        aslSeen = aslCalls.length() > 0;
        if (b != null && (seen && aslSeen || bapCalls.length() > 0)) {
            try {
                String calls = bapCalls(b.getCarPlayCalls());
                if (!calls.equals(bapCalls)) {
                    bapCalls = calls;
                    Activator.log("calls: BAP CarPlay service:"
                        + (calls.length() == 0 ? " none" : calls));
                }
            } catch (Throwable t) {
                /* diagnostics only */
            }
        }
    }

    static String aslCalls(ASLCarplayServicesCallState[] calls) {
        StringBuffer b = new StringBuffer();
        for (int i = 0; calls != null && i < calls.length; i++) {
            if (calls[i] != null) {
                b.append(' ').append(call(calls[i].getStatus(), calls[i].getDirection(),
                    calls[i].getCallerName(), calls[i].getPhoneNumber()));
            }
        }
        return b.toString();
    }

    private static String bapCalls(Iterator i) {
        StringBuffer b = new StringBuffer();
        while (i != null && i.hasNext()) {
            CarPlayCall c = (CarPlayCall)i.next();
            b.append(' ').append(call(c.getStatus(), c.getDirection(), c.getCallerName(),
                c.getPhoneNumber())).append(" id ").append(c.getMappedCallID());
        }
        return b.toString();
    }

    /** One call without its name or number: state/direction, and which of them is present. */
    static String call(int state, int direction, String name, String number) {
        String[] states = {"disconnected", "active", "holding", "connecting"};
        String[] directions = {"in", "out", "unknown"};
        return (state >= 0 && state < states.length ? states[state] : "state" + state) + "/"
            + (direction >= 0 && direction < directions.length ? directions[direction]
                                                               : "direction" + direction)
            + (name != null && name.length() > 0 ? " name" : "")
            + (number != null && number.length() > 0 ? " number" : "");
    }
}
