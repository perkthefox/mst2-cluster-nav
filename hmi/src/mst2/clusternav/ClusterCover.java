/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.io.File;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

import org.dsi.ifc.global.ResourceLocator;

import de.vw.mib.asl.framework.internal.framework.ServiceManager;
import de.vw.mib.asl.internal.kombipictureserver.common.api.media.asl.MediaCoverArtService;
import de.vw.mib.asl.internal.kombipictureserver.controller.PictureServerController;
import de.vw.mib.asl.internal.kombipictureserver.usecaces.CoverArt;
import de.vw.mib.bap.datatypes.BAPEntity;
import de.vw.mib.bap.functions.ArrayListener;
import de.vw.mib.bap.functions.BAPFunction;
import de.vw.mib.bap.functions.BAPFunctionController;
import de.vw.mib.bap.functions.MethodListener;
import de.vw.mib.bap.functions.Property;
import de.vw.mib.bap.functions.PropertyListener;
import de.vw.mib.bap.mqbab2.audiosd.functions.ActiveSource;
import de.vw.mib.bap.mqbab2.audiosd.functions.CurrentStationHandle;
import de.vw.mib.bap.mqbab2.common.api.adapter.AbstractBAPDataPool;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStage;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStageContext;
import de.vw.mib.bap.mqbab2.common.api.stages.Function;
import de.vw.mib.bap.mqbab2.generated.audiosd.serializer.ActiveSource_Status;
import de.vw.mib.bap.mqbab2.generated.audiosd.serializer.CurrentStation_Handle_Status;
import de.vw.mib.genericevents.Target;

/**
 * The phone track's cover picture on the cluster's media view.
 *
 * Stock path for a local source: BAP AudioSD ActiveSource (function 16) and CurrentStationHandle
 * (22) go to the cluster; the picture server's BapAudioSdAdapter watches what is sent (the AudioSD
 * data pool's value observers, LCU 49) and maps the source type for its CoverArt use case
 * (kombipictureserver.usecaces.CoverArt, category 1480001 of target 1480000), which answers the
 * cluster with DSIKombiPictureServer.responseCoverArt(handle, 0, source type, 1, cover) from the
 * media cover of ASL list 58 (MediaCoverArtAdapter). A phone gets none: the adapter maps source
 * types 37 (CarPlay) and 39 (Android Auto) to 255, and CoverArt serves only 6-8, 10, 11, 15, 16,
 * 18-22, so it answers (-1, 0, 0, 0, null). The cluster itself is reported to draw a cover only
 * for local media sources.
 *
 * So while a phone track with a picture plays and the stock ActiveSource names that phone as the
 * source, the cluster is told it is USB: this context stands between ActiveSource /
 * CurrentStationHandle and their stock data pool (as NowPlaying does for CurrentStationInfo) and
 * hands the data pool a copy with source type 19 and handle 1 instead; the function itself, and
 * the other stages it updates, keep the stock values. CoverArt's private media service is wrapped
 * to name the phone's picture as the cover while that holds, and CoverArt.updateMediaCoverArt is
 * called for every new picture, the stock notification of a new cover. The cluster then shows a
 * USB source icon with the phone's cover; the text stays the phone's track (NowPlaying). With no
 * picture, media=0, a timeout, the kill switch or the bundle stopped, everything passes through
 * and the stock values are sent again. Nothing is substituted until CoverArt's media service is
 * ours (else the cluster could get a stale local cover of list 58); a missing piece is retried
 * every RETRY_MS while a picture waits.
 */
final class ClusterCover implements BAPStageContext, PropertyListener {
    /* BAP AudioSD source types (SourceList.setSmartPhoneSourceType: connection type 2 / 3). */
    static final int CARPLAY = 37;
    static final int ANDROID_AUTO = 39;
    static final int USB = 19;
    private static final int ACTIVE_SOURCE = 16;
    private static final int CURRENT_STATION_HANDLE = 22;
    private static final int PICTURE_SERVER = 1480000;
    private static final int COVER_ART = 1480001;

    /* While a phone track with a picture plays: its source type and the picture; else 0, null. */
    private static volatile int phone;
    private static volatile String art;
    private static volatile boolean stopped;
    /* AudioSD functions wrapped and CoverArt's media service in place. */
    private static volatile boolean ready;
    private static final long RETRY_MS = 2000;
    private static long retryMs;

    /* ASL thread only. */
    private static int stockSource = -1;
    private static final List contexts = new ArrayList();
    private static Field contextField, mediaField;
    private static CoverArt coverArt;
    private static MediaCoverArtService stockMedia;
    private static Media media;
    private static boolean missingLogged;

    private final BAPStage function;
    private final BAPStageContext stock;
    private PropertyListener listener;

    private ClusterCover(BAPStage function, BAPStageContext stock) {
        this.function = function;
        this.stock = stock;
    }

    /** The phone's source type (CARPLAY, ANDROID_AUTO, 0 none) and picture path (PhoneLink). */
    static void set(int source, String path) {
        if (path != null && !path.equals(art) && !new File(path).isFile()) {
            path = null;
        }
        if (source == 0 || path == null) {
            source = 0;
            path = null;
        }
        if (source == phone && (path == null ? art == null : path.equals(art))) {
            return;
        }
        if ((source != 0) != (phone != 0)) {
            Activator.log(source != 0 ? "cover: the phone's picture goes to the cluster (as USB)"
                                      : "cover: stock source back on the cluster");
        }
        art = path;
        phone = source;
        refresh();
    }

    static void start() {
        stopped = false;
    }

    /** PhoneLink's loop: while a picture waits for a missing piece, try again every RETRY_MS. */
    static void tick(long now) {
        if (phone != 0 && !ready && PhoneLink.expired(retryMs, now, RETRY_MS)) {
            retryMs = now;
            refresh();
        }
    }

    /** Puts the stock contexts and media service back and resends the stock values. */
    static void uninstall() {
        stopped = true;
        phone = 0;
        art = null;
        ClusterCarPlay.safeEnqueue(new Runnable() {
            public void run() {
                restore();
            }
        });
    }

    /* ASL thread; each item on its own, so one failure leaves no other one wrapped. */
    static void restore() {
        for (int i = 0; i < contexts.size(); i++) {
            ClusterCover c = (ClusterCover)contexts.get(i);
            try {
                if (contextField.get(c.function) == c) {
                    c.function.setDelegate(c.stock);
                }
            } catch (Throwable t) {
                Activator.log("cannot restore a cover context: " + t);
            }
        }
        for (int i = 0; i < contexts.size(); i++) {
            try {
                ((ClusterCover)contexts.get(i)).function.process(-1);
            } catch (Throwable t) {
                Activator.log("cover: stock value not resent: " + t);
            }
        }
        contexts.clear();
        try {
            if (coverArt != null && mediaField.get(coverArt) == media) {
                mediaField.set(coverArt, stockMedia);
            }
        } catch (Throwable t) {
            Activator.log("cannot restore the cover art: " + t);
        }
        coverArt = null;
        ready = false;
    }

    /* ASL thread: install if needed, resend source and handle, announce the picture. */
    private static void refresh() {
        ClusterCarPlay.safeEnqueue(new Runnable() {
            public void run() {
                if (stopped) {
                    return;
                }
                try {
                    install();
                    /* ActiveSource first: the handle follows its source. */
                    for (int i = 0; i < contexts.size(); i++) {
                        ((ClusterCover)contexts.get(i)).function.process(-1);
                    }
                    if (wired() && phone != 0) {
                        coverArt.updateMediaCoverArt(media, 0);
                    }
                } catch (Throwable t) {
                    Activator.log("cover: " + t);
                }
            }
        });
    }

    private static void install() throws Exception {
        BAPStageContext pool = NowPlaying.audioContext();
        if (!(pool instanceof AbstractBAPDataPool)) {
            missing("no AudioSD functions (App-Connect not coded or not up yet)");
            return;
        }
        if (!wired()) {
            installCoverArt();
        }
        wrap(((AbstractBAPDataPool)pool).getStage(ACTIVE_SOURCE));
        wrap(((AbstractBAPDataPool)pool).getStage(CURRENT_STATION_HANDLE));
        ready = wired() && contexts.size() == 2;
    }

    private static void missing(String what) {
        if (!missingLogged) {
            missingLogged = true;
            Activator.log("cover: " + what + ", the cluster shows no cover yet");
        }
    }

    /* ASL thread (host-checked). */
    static void wrap(BAPStage stage) throws Exception {
        if (!(stage instanceof ActiveSource) && !(stage instanceof CurrentStationHandle)) {
            return;
        }
        if (contextField == null) {
            contextField = Function.class.getDeclaredField("context");
            contextField.setAccessible(true);
        }
        Object current = contextField.get(stage);
        if (current instanceof ClusterCover || current == null) {
            return;
        }
        /* New, or the stock code gave it a new context: (re)wrap that one. */
        for (int k = contexts.size() - 1; k >= 0; k--) {
            if (((ClusterCover)contexts.get(k)).function == stage) {
                contexts.remove(k);
            }
        }
        ClusterCover c = new ClusterCover(stage, (BAPStageContext)current);
        stage.setDelegate(c);
        if (stage instanceof ActiveSource) {
            contexts.add(0, c);
        } else {
            contexts.add(c);
        }
    }

    /*
     * The CoverArt use case of the running picture server, reached as the stock controller does
     * (Controller.getCategory); ASL thread (host-checked without a picture server).
     */
    static void installCoverArt() {
        try {
            Target t = ServiceManager.eventMain == null ? null
                : ServiceManager.eventMain.getEventDispatcher().getTarget(PICTURE_SERVER);
            if (!(t instanceof PictureServerController)) {
                missing("no picture server");
                return;
            }
            Object c = ((PictureServerController)t).getCategory(COVER_ART);
            if (!(c instanceof CoverArt)) {
                missing("no cover art use case");
                return;
            }
            useCoverArt((CoverArt)c);
        } catch (Throwable t) {
            missing("picture server: " + t);
        }
    }

    /*
     * Wraps CoverArt's media service. It is set by the use case's first sequence; only a set one
     * is wrapped, so the stock lazy setup (which registers it for list 58) is never skipped.
     */
    static boolean useCoverArt(CoverArt c) throws Exception {
        Field f = CoverArt.class.getDeclaredField("_mediaCoverArtService");
        f.setAccessible(true);
        Object s = f.get(c);
        if (!(s instanceof MediaCoverArtService)) {
            missing("picture server not started");
            return false;
        }
        if (!(s instanceof Media)) {
            stockMedia = (MediaCoverArtService)s;
            media = new Media(stockMedia);
            f.set(c, media);
        }
        mediaField = f;
        coverArt = c;
        Activator.log("cover: picture server found");
        return true;
    }

    /* Our media service stands in CoverArt: only then may the cluster be told USB. */
    private static boolean wired() {
        try {
            return coverArt != null && mediaField.get(coverArt) == media;
        } catch (Throwable t) {
            return false;
        }
    }

    /** The phone's picture while the cluster is told USB, else null; ASL thread. */
    static String activeArt() {
        String p = art;
        int s = phone;
        return s != 0 && s == stockSource && wired() ? p : null;
    }

    /**
     * What the data pool gets instead of a stock status: a USB copy while the phone with a picture
     * is the stock source, else the stock status itself (host-checked, never changes its input).
     * Values of the USB status as reported to work with this cluster; stock would send type of
     * number 0 for number 0.
     */
    static BAPEntity substitute(BAPEntity e, int phone, int stockSource) {
        if (phone == 0 || stockSource != phone) {
            return e;
        }
        if (e instanceof ActiveSource_Status) {
            ActiveSource_Status s = (ActiveSource_Status)e;
            if (s.sourceType != phone) {
                return e;
            }
            ActiveSource_Status u = new ActiveSource_Status();
            u.sourceType = USB;
            u.sourceList_Reference = 0;
            u.typeOfNumber = 5;
            u.number = 0;
            u.list_State = 3;
            u.listAvailable.mediaBrowserListAvailable = true;
            u.listAvailable2.lastStationListAvailable = s.listAvailable2.lastStationListAvailable;
            return u;
        }
        if (e instanceof CurrentStation_Handle_Status) {
            CurrentStation_Handle_Status h = new CurrentStation_Handle_Status();
            h.fsgHandle = 1;
            h.fsgHandle_absolutePosition = 1;
            return h;
        }
        return e;
    }

    public boolean statusProperty(BAPEntity entity, Property property) {
        if (entity instanceof ActiveSource_Status) {
            boolean before = activeArt() != null;
            stockSource = ((ActiveSource_Status)entity).sourceType;
            if ((activeArt() != null) != before) {
                /* The handle went out under the old source: send it again under the new one. */
                handleAgain();
            }
        }
        return listener.statusProperty(substitute(entity, wired() ? phone : 0, stockSource),
            property);
    }

    private static void handleAgain() {
        ClusterCarPlay.safeEnqueue(new Runnable() {
            public void run() {
                for (int i = 0; i < contexts.size(); i++) {
                    BAPStage f = ((ClusterCover)contexts.get(i)).function;
                    if (f instanceof CurrentStationHandle) {
                        try {
                            f.process(-1);
                        } catch (Throwable t) {
                            Activator.log("cover: " + t);
                        }
                    }
                }
            }
        });
    }

    public boolean statusAckProperty(BAPEntity entity, Property property) {
        return listener.statusAckProperty(entity, property);
    }

    public void requestError(int error, BAPFunction function) {
        listener.requestError(error, function);
    }

    /* The functions ask for the listener right before each send. */
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

    /** CoverArt's media service: the phone's picture while the cluster is told USB. */
    private static final class Media implements MediaCoverArtService {
        private final MediaCoverArtService stock;

        Media(MediaCoverArtService stock) {
            this.stock = stock;
        }

        public ResourceLocator getCoverArt() {
            String p = activeArt();
            return p != null ? new ResourceLocator(p) : stock.getCoverArt();
        }

        public boolean isCoverValid() {
            return activeArt() != null || stock.isCoverValid();
        }
    }
}
