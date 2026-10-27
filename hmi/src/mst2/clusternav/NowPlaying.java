/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

import de.vw.mib.bap.datatypes.BAPEntity;
import de.vw.mib.bap.functions.ArrayListener;
import de.vw.mib.bap.functions.BAPFunction;
import de.vw.mib.bap.functions.BAPFunctionController;
import de.vw.mib.bap.functions.MethodListener;
import de.vw.mib.bap.functions.Property;
import de.vw.mib.bap.functions.PropertyListener;
import de.vw.mib.bap.mqbab2.audiosd.functions.CurrentStationInfo;
import de.vw.mib.bap.mqbab2.common.api.APIFactory;
import de.vw.mib.bap.mqbab2.common.api.asl.ASLDataPoolAdapter;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStage;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStageContext;
import de.vw.mib.bap.mqbab2.common.api.stages.Function;
import de.vw.mib.bap.mqbab2.generated.audiosd.serializer.CurrentStationInfo_Status;

/**
 * The phone's track on the cluster's media view.
 *
 * For a phone source the stock BAP AudioSD CurrentStationInfo (function 21,
 * de.vw.mib.bap.mqbab2.audiosd.functions.CurrentStationInfo.setStationInfoForMirrorLink) sends
 * only the connection's name as primary information, type 72: "Apple CarPlay" for connection
 * type 2, "Android Auto" for 3. This context stands between that function and its stock BAP
 * context: a status saying exactly the name of the phone the track came from gets the phone's
 * title (72), artist (73) and album (74) instead, the types the function uses for USB
 * (setArtistAndAlbum). Every other status passes untouched, and without a track from the phone
 * nothing changes. The function is found among the listeners of the smartphone integration
 * service (property 2910018, which it registers for in init) once its BAP stage is up; that
 * service is real only with App-Connect coded (APIFactory.getSmartphoneIntegrationService,
 * feature flag 452), else the stock dummy stands there and the cluster keeps the stock text.
 */
final class NowPlaying implements BAPStageContext, PropertyListener {
    private static final String CARPLAY = "Apple CarPlay";
    private static final String ANDROID_AUTO = "Android Auto";
    private static final int TIMEOUT_MS = 3000;
    private static final int SMARTPHONE_INTEGRATION = 2910018;

    /* The phone's track: {source name, title, artist, album}, or null; swapped whole. */
    private static volatile String[] track;
    private static long seenMs;
    private static volatile boolean stopped;
    private static boolean dummyLogged;
    private static Field contextField;
    /* NowPlaying contexts in place, one per CurrentStationInfo; ASL thread only. */
    private static final List contexts = new ArrayList();

    private final CurrentStationInfo function;
    private final BAPStageContext stock;
    private PropertyListener listener;

    private NowPlaying(CurrentStationInfo function, BAPStageContext stock) {
        this.function = function;
        this.stock = stock;
    }

    /** A "media=..." datagram from SAL (PhoneLink thread). */
    static void received(String text) {
        String t = null, ar = "", al = "", src = CARPLAY;
        int start = 0;
        while (start < text.length()) {
            int end = text.indexOf('\n', start);
            if (end < 0) {
                end = text.length();
            }
            String line = text.substring(start, end);
            start = end + 1;
            if (line.startsWith("title=")) {
                t = line.substring(6);
            } else if (line.startsWith("artist=")) {
                ar = line.substring(7);
            } else if (line.startsWith("album=")) {
                al = line.substring(6);
            } else if (line.equals("src=aa")) {
                src = ANDROID_AUTO;
            }
        }
        if (t == null || t.length() == 0) {
            clear();
            return;
        }
        seenMs = System.currentTimeMillis();
        String[] now = new String[] {src, t, ar, al};
        String[] old = track;
        if (old == null || !same(old, now)) {
            track = now;
            refresh();
        }
    }

    private static boolean same(String[] a, String[] b) {
        for (int i = 0; i < a.length; i++) {
            if (!a[i].equals(b[i])) {
                return false;
            }
        }
        return true;
    }

    /** Called about once a second: a silent SAL ends the track like "media=0". */
    static void tick(long now) {
        if (track != null && PhoneLink.expired(seenMs, now, TIMEOUT_MS)) {
            clear();
        }
    }

    static void clear() {
        if (track != null) {
            track = null;
            refresh();
        }
    }

    /** Bundle start: refreshes may wrap the station-info contexts again (after an earlier stop). */
    static void start() {
        stopped = false;
    }

    /** Puts the stock contexts back (bundle stop); no refresh after this wraps them again. */
    static void uninstall() {
        stopped = true;
        track = null;
        ClusterCarPlay.safeEnqueue(new Runnable() {
            public void run() {
                for (int i = 0; i < contexts.size(); i++) {
                    NowPlaying c = (NowPlaying)contexts.get(i);
                    try {
                        if (contextField.get(c.function) == c) {
                            contextField.set(c.function, c.stock);
                        }
                        c.function.process(-1);
                    } catch (Throwable t) {
                        Activator.log("cannot restore the station info: " + t);
                    }
                }
                contexts.clear();
            }
        });
    }

    /* On the ASL thread, where the stock function runs: install if needed, then resend. */
    private static void refresh() {
        ClusterCarPlay.safeEnqueue(new Runnable() {
            public void run() {
                if (stopped) {
                    return;
                }
                try {
                    install();
                    for (int i = 0; i < contexts.size(); i++) {
                        ((NowPlaying)contexts.get(i)).function.process(-1);
                    }
                } catch (Throwable t) {
                    Activator.log("now playing: " + t);
                }
            }
        });
    }

    private static void install() throws Exception {
        if (contextField == null) {
            contextField = Function.class.getDeclaredField("context");
            contextField.setAccessible(true);
        }
        Object service = APIFactory.getAPIFactory().getSmartphoneIntegrationService();
        if (!(service instanceof ASLDataPoolAdapter)) {
            if (!dummyLogged) {
                dummyLogged = true;
                Activator.log("now playing: App-Connect not coded, the cluster keeps the stock text");
            }
            return;
        }
        Method delegates = ASLDataPoolAdapter.class.getDeclaredMethod(
            "getRegisteredServiceDelegates", new Class[] {Integer.TYPE});
        delegates.setAccessible(true);
        List listeners = (List)delegates.invoke(service,
            new Object[] {new Integer(SMARTPHONE_INTEGRATION)});
        for (int i = 0; listeners != null && i < listeners.size(); i++) {
            if (!(listeners.get(i) instanceof CurrentStationInfo)) {
                continue;
            }
            CurrentStationInfo f = (CurrentStationInfo)listeners.get(i);
            Object current = contextField.get(f);
            if (current instanceof NowPlaying || current == null) {
                continue;
            }
            /* New, or the stock code gave it a new context: (re)wrap that one. */
            for (int k = contexts.size() - 1; k >= 0; k--) {
                if (((NowPlaying)contexts.get(k)).function == f) {
                    contexts.remove(k);
                }
            }
            NowPlaying c = new NowPlaying(f, (BAPStageContext)current);
            contextField.set(f, c);
            contexts.add(c);
            Activator.log("now playing: the phone's track goes to the cluster");
        }
    }

    public boolean statusProperty(BAPEntity entity, Property property) {
        String[] t = track;
        if (t != null && entity instanceof CurrentStationInfo_Status) {
            CurrentStationInfo_Status s = (CurrentStationInfo_Status)entity;
            if (s.pi_Type == 72 && t[0].equals(s.primaryInformation.toString())) {
                String ar = t[2], al = t[3];
                s.primaryInformation.setContent(t[1]);
                s.secondaryInformation.setContent(ar);
                s.si_Type = ar.length() != 0 ? 73 : 0;
                s.tertiaryInformation.setContent(al);
                s.ti_Type = al.length() != 0 ? 74 : 0;
            }
        }
        return listener.statusProperty(entity, property);
    }

    public boolean statusAckProperty(BAPEntity entity, Property property) {
        return listener.statusAckProperty(entity, property);
    }

    public void requestError(int error, BAPFunction function) {
        listener.requestError(error, function);
    }

    /* The function asks for its listener right before each send (sendCurrentStationInfo). */
    public PropertyListener getPropertyListener(BAPFunctionController controller) {
        listener = stock.getPropertyListener(controller);
        return this;
    }

    public ArrayListener getArrayListener(BAPFunctionController controller) {
        return stock.getArrayListener(controller);
    }

    public MethodListener getMethodListener(BAPFunctionController controller) {
        return stock.getMethodListener(controller);
    }

    public void updateStages(BAPStage stage, int[] ids, Object value) {
        stock.updateStages(stage, ids, value);
    }

    public BAPEntity dequeueBAPEntity(BAPStage stage, Class type) {
        return stock.dequeueBAPEntity(stage, type);
    }

    public int getLogicalControlUnitID() {
        return stock.getLogicalControlUnitID();
    }
}
