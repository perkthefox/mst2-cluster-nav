/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.lang.reflect.Field;
import java.util.Iterator;

import de.vw.mib.asl.api.carplay.ASLCarplayServicesListener;
import de.vw.mib.asl.framework.api.framework.ASLFrameworkFactory;
import de.vw.mib.bap.mqbab2.common.api.APIFactory;
import de.vw.mib.bap.mqbab2.common.api.androidauto.AndroidAutoASLDataAdapter;
import de.vw.mib.bap.mqbab2.common.api.androidauto.AndroidAutoService;
import de.vw.mib.bap.mqbab2.common.api.androidauto.AndroidAutoServiceListener;
import de.vw.mib.bap.mqbab2.common.api.carplay.CarPlayService;
import de.vw.mib.bap.mqbab2.common.api.carplay.CarPlayServiceListener;
import de.vw.mib.bap.mqbab2.common.api.carplay.datatypes.CarPlayDeviceState;
import de.vw.mib.bap.mqbab2.common.api.carplay.datatypes.iterator.elements.CarPlayCall;

/**
 * The cluster's view of CarPlay while the phone draws or guides on the cluster.
 *
 * When CarPlay takes over route guidance the stock HMI sets its active navigation to 2, and
 * BAP NavSD InfoStates (de.vw.mib.bap.mqbab2.navsd.functions.InfoStates) then sends 6,
 * "navigation in mobile device": the cluster shows that notice instead of the map or the
 * arrows. InfoStates is the only reader of CarPlayService.isCarPlayRouteGuidanceActive, and
 * it asks APIFactory for the service on every update. So while the phone owns the cluster,
 * this delegate stands in the factory's slot and answers false there; everything else goes to
 * the stock service, and InfoStates falls back to the built-in navigation's state as without
 * CarPlay. Nothing is replaced when CarPlay is not coded (the factory holds its dummy then).
 * Android Auto's guidance (AndroidAutoASLDataAdapter, property 2910005) gets the same stand-in;
 * verified safe like CarPlay: the only reader of getAndroidAutoService() (navsd InfoStates) uses
 * the interface alone, so a plain AndroidAutoService delegate causes no ClassCastException.
 */
final class ClusterCarPlay implements CarPlayService {
    private static final int AA_GUIDANCE = 2910005;
    private static Field slot, aaSlot;
    private static CarPlayService stock;
    private static AndroidAutoASLDataAdapter aaStock;
    private static volatile boolean phoneOwnsCluster;
    /* Set by uninstall: an install still queued on the ASL thread must not run after it. */
    private static volatile boolean stopped;

    private ClusterCarPlay() {
    }

    /** Puts the delegate into APIFactory, on the ASL thread like the stock factory calls. */
    static void install() {
        stopped = false;
        safeEnqueue(new Runnable() {
            public void run() {
                if (stopped) {
                    return;
                }
                try {
                    Field[] fields = APIFactory.class.getDeclaredFields();
                    for (int i = 0; i < fields.length; i++) {
                        if (fields[i].getType() == CarPlayService.class) {
                            slot = fields[i];
                        } else if (fields[i].getType() == AndroidAutoService.class) {
                            aaSlot = fields[i];
                        }
                    }
                    installAndroidAuto();
                    slot.setAccessible(true);
                    /* The stock getter creates the service if nobody has asked yet. */
                    CarPlayService current = APIFactory.getAPIFactory().getCarPlayService();
                    if (!(current instanceof ASLCarplayServicesListener)) {
                        Activator.log("CarPlay not coded, cluster notice left alone");
                        return;
                    }
                    stock = current;
                    slot.set(null, new ClusterCarPlay());
                    Activator.log("CarPlay route guidance hidden from the cluster while the phone owns it");
                } catch (Throwable t) {
                    Activator.log("cannot take the CarPlay service: " + t);
                }
            }
        });
    }

    private static void installAndroidAuto() {
        try {
            AndroidAutoService current = APIFactory.getAPIFactory().getAndroidAutoService();
            if (!(current instanceof AndroidAutoASLDataAdapter)) {
                Activator.log("Android Auto not coded, its cluster notice left alone");
                return;
            }
            aaSlot.setAccessible(true);
            aaStock = (AndroidAutoASLDataAdapter)current;
            aaSlot.set(null, new AndroidAuto());
        } catch (Throwable t) {
            Activator.log("cannot take the Android Auto service: " + t);
        }
    }

    static void uninstall() {
        stopped = true;
        safeEnqueue(new Runnable() {
            public void run() {
                try {
                    /* stock stays set: a caller may still hold the delegate. */
                    phoneOwnsCluster = false;
                    if (stock != null) {
                        slot.set(null, stock);
                    }
                    if (aaStock != null) {
                        aaSlot.set(null, aaStock);
                    }
                    /* InfoStates asks the stock services again. */
                    notifyInfoStates();
                } catch (Throwable t) {
                    Activator.log("cannot restore the CarPlay service: " + t);
                }
            }
        });
    }

    /** The phone's cluster video is running, or its guidance feeds the arrows. */
    static void setPhoneOwnsCluster(boolean owns) {
        if (owns == phoneOwnsCluster) {
            return;
        }
        phoneOwnsCluster = owns;
        Activator.log(owns ? "cluster: phone" : "cluster: stock");
        safeEnqueue(new Runnable() {
            public void run() {
                try {
                    notifyInfoStates();
                } catch (Throwable t) {
                    Activator.log("cluster notice not refreshed: " + t);
                }
            }
        });
    }

    /* ASL thread. */
    private static void notifyInfoStates() {
        /*
         * The stock notification InfoStates listens to (CarPlayASLDataAdapter); the adapter
         * ignores the argument and only notifies its listeners.
         */
        if (stock != null) {
            ((ASLCarplayServicesListener)stock).updateCarPlayRouteGuidanceActive(false);
        }
        /* Android Auto's adapter tells its listeners (InfoStates) the same way. */
        if (aaStock != null) {
            aaStock.datapoolValueChanged(AA_GUIDANCE);
        }
    }

    /* Callers are the bundle's own threads: an ASL failure is logged, never thrown at them. */
    static void safeEnqueue(Runnable r) {
        try {
            ASLFrameworkFactory.getASLFrameworkAPI().getServices().getASLThreadSwitch().enqueue(r);
        } catch (Throwable t) {
            Activator.log("ASL thread not available: " + t);
        }
    }

    public boolean isCarPlayRouteGuidanceActive() {
        return !phoneOwnsCluster && stock.isCarPlayRouteGuidanceActive();
    }

    public void addCarPlayServiceListener(CarPlayServiceListener listener, int[] ids) {
        stock.addCarPlayServiceListener(listener, ids);
    }

    public void removeCarPlayServiceListener(CarPlayServiceListener listener, int[] ids) {
        stock.removeCarPlayServiceListener(listener, ids);
    }

    public boolean isCarPlayActive() {
        return stock.isCarPlayActive();
    }

    public CarPlayDeviceState getCarPlayDeviceState() {
        return stock.getCarPlayDeviceState();
    }

    public Iterator getCarPlayCalls() {
        return stock.getCarPlayCalls();
    }

    public void hangUpCall(CarPlayCall call) {
        stock.hangUpCall(call);
    }

    public void acceptCall(int i) {
        stock.acceptCall(i);
    }

    private static final class AndroidAuto implements AndroidAutoService {
        public boolean isAndroidAutoRouteGuidanceActive() {
            return !phoneOwnsCluster && aaStock.isAndroidAutoRouteGuidanceActive();
        }

        public void addAndroidAutoServiceListener(AndroidAutoServiceListener l, int[] ids) {
            aaStock.addAndroidAutoServiceListener(l, ids);
        }

        public void removeAndroidAutoServiceListener(AndroidAutoServiceListener l, int[] ids) {
            aaStock.removeAndroidAutoServiceListener(l, ids);
        }
    }
}
