/*
 * mst2-cluster-nav - host self-check of the cover substitution (JDK 8: hmi/check.sh).
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

import org.dsi.ifc.global.ResourceLocator;

import de.vw.mib.asl.internal.kombipictureserver.common.api.media.asl.MediaCoverArtService;
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
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStage;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStageContext;
import de.vw.mib.bap.mqbab2.common.api.stages.Function;
import de.vw.mib.bap.mqbab2.generated.audiosd.serializer.ActiveSource_Status;
import de.vw.mib.bap.mqbab2.generated.audiosd.serializer.CurrentStation_Handle_Status;

public final class CoverCheck implements BAPStageContext, PropertyListener {
    private BAPEntity sent;

    public boolean statusProperty(BAPEntity e, Property p) {
        sent = e;
        return true;
    }
    public boolean statusAckProperty(BAPEntity e, Property p) { return true; }
    public void requestError(int i, BAPFunction f) { }
    public PropertyListener getPropertyListener(BAPFunctionController c) { return this; }
    public ArrayListener getArrayListener(BAPFunctionController c) { return null; }
    public MethodListener getMethodListener(BAPFunctionController c) { return null; }
    public void updateStages(BAPStage s, int[] ids, Object v) { }
    public BAPEntity dequeueBAPEntity(BAPStage s, Class t) { return null; }
    public int getLogicalControlUnitID() { return 0; }

    private static ActiveSource_Status source(int type) {
        ActiveSource_Status s = new ActiveSource_Status();
        s.sourceType = type;
        s.sourceList_Reference = 7;
        s.listAvailable.receptionListAvailable = true;
        return s;
    }

    private static CurrentStation_Handle_Status handle() {
        CurrentStation_Handle_Status h = new CurrentStation_Handle_Status();
        h.fsgHandle = -1;
        return h;
    }

    private static void expect(String what, boolean ok) {
        if (!ok) {
            System.out.println("FAIL " + what);
            System.exit(1);
        }
    }

    public static void main(String[] args) throws Exception {
        int cp = ClusterCover.CARPLAY, aa = ClusterCover.ANDROID_AUTO;

        /* The pure substitution. */
        ActiveSource_Status s = source(cp);
        ActiveSource_Status u = (ActiveSource_Status)ClusterCover.substitute(s, cp, cp);
        expect("CarPlay with art: USB copy", u != s && u.sourceType == 19
            && u.sourceList_Reference == 0 && u.list_State == 3 && u.typeOfNumber == 5
            && u.number == 0 && u.listAvailable.mediaBrowserListAvailable
            && !u.listAvailable.receptionListAvailable);
        expect("stock status untouched", s.sourceType == cp && s.sourceList_Reference == 7
            && s.listAvailable.receptionListAvailable);
        expect("no art: stock", ClusterCover.substitute(s, 0, cp) == s);
        expect("other phone: stock", ClusterCover.substitute(s, aa, cp) == s);
        ActiveSource_Status fm = source(1);
        expect("radio playing: stock", ClusterCover.substitute(fm, cp, 1) == fm);
        expect("Android Auto", ((ActiveSource_Status)ClusterCover.substitute(source(aa), aa, aa))
            .sourceType == 19);
        CurrentStation_Handle_Status h = handle();
        CurrentStation_Handle_Status hu =
            (CurrentStation_Handle_Status)ClusterCover.substitute(h, cp, cp);
        expect("handle 1 under the phone", hu != h && hu.fsgHandle == 1 && h.fsgHandle == -1);
        expect("handle stock under another source", ClusterCover.substitute(h, cp, 19) == h);

        /* The context in front of the data pool, driven by "media=" datagrams. */
        File art = File.createTempFile("cover", ".jpg");
        art.deleteOnExit();
        CoverCheck pool = new CoverCheck();
        Constructor c = ClusterCover.class.getDeclaredConstructor(
            new Class[] {BAPStage.class, BAPStageContext.class});
        c.setAccessible(true);
        ClusterCover cover = (ClusterCover)c.newInstance(new Object[] {null, pool});

        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("no track: stock source", ((ActiveSource_Status)pool.sent).sourceType == cp);

        /* No picture server (host: no event dispatcher): never USB, else a stale cover. */
        ClusterCover.installCoverArt();
        NowPlaying.received("media=1\ntitle=Song\nart=" + art.getPath() + "\n");
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("no picture server: stock", ((ActiveSource_Status)pool.sent).sourceType == cp
            && ClusterCover.activeArt() == null);
        cover.getPropertyListener(null).statusProperty(handle(), null);
        expect("no picture server: stock handle",
            ((CurrentStation_Handle_Status)pool.sent).fsgHandle == -1);

        /* A cover art use case before its first sequence: not wrapped. */
        CoverArt coverArt = new CoverArt();
        expect("picture server not started", !ClusterCover.useCoverArt(coverArt));
        expect("still stock", ClusterCover.activeArt() == null);
        Field mediaField = CoverArt.class.getDeclaredField("_mediaCoverArtService");
        mediaField.setAccessible(true);
        MediaCoverArtService stockMedia = new MediaCoverArtService() {
            public ResourceLocator getCoverArt() {
                return new ResourceLocator("/stale/usb/cover.jpg");
            }
            public boolean isCoverValid() {
                return true;
            }
        };
        mediaField.set(coverArt, stockMedia);
        expect("picture server started", ClusterCover.useCoverArt(coverArt));
        MediaCoverArtService media = (MediaCoverArtService)mediaField.get(coverArt);
        expect("media service wrapped", media != stockMedia);
        expect("art named", art.getPath().equals(ClusterCover.activeArt())
            && art.getPath().equals(media.getCoverArt().getUrl()) && media.isCoverValid());
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("CarPlay track with art: USB", ((ActiveSource_Status)pool.sent).sourceType == 19);
        cover.getPropertyListener(null).statusProperty(handle(), null);
        expect("handle 1", ((CurrentStation_Handle_Status)pool.sent).fsgHandle == 1);

        cover.getPropertyListener(null).statusProperty(source(1), null);
        expect("switched to radio: stock", ((ActiveSource_Status)pool.sent).sourceType == 1
            && ClusterCover.activeArt() == null);
        cover.getPropertyListener(null).statusProperty(handle(), null);
        expect("radio handle stock", ((CurrentStation_Handle_Status)pool.sent).fsgHandle == -1);
        cover.getPropertyListener(null).statusProperty(source(cp), null);

        NowPlaying.received("media=1\ntitle=Song\n");
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("track without art: stock", ((ActiveSource_Status)pool.sent).sourceType == cp);

        NowPlaying.received("media=1\ntitle=Song\nart=/no/such/cover.jpg\n");
        expect("missing file: no art", ClusterCover.activeArt() == null);

        NowPlaying.received("media=1\ntitle=Song\nart=" + art.getPath() + "\n");
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("art again: USB", ((ActiveSource_Status)pool.sent).sourceType == 19);
        NowPlaying.received("media=0");
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("media=0: stock", ((ActiveSource_Status)pool.sent).sourceType == cp);

        NowPlaying.received("media=1\ntitle=Song\nart=" + art.getPath() + "\n");
        NowPlaying.tick(System.currentTimeMillis() + 4000);
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("timeout: stock", ((ActiveSource_Status)pool.sent).sourceType == cp);

        NowPlaying.received("media=1\ntitle=Track\nsrc=aa\nart=" + art.getPath() + "\n");
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("Android Auto art not on CarPlay",
            ((ActiveSource_Status)pool.sent).sourceType == cp);
        cover.getPropertyListener(null).statusProperty(source(aa), null);
        expect("Android Auto art on Android Auto",
            ((ActiveSource_Status)pool.sent).sourceType == 19);
        cover.getPropertyListener(null).statusProperty(source(1), null);
        expect("radio: stock cover",
            "/stale/usb/cover.jpg".equals(media.getCoverArt().getUrl()));

        /* Kill switch: PhoneLink clears the track and drops later media datagrams. */
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        NowPlaying.received("media=1\ntitle=Song\nart=" + art.getPath() + "\n");
        expect("before the kill switch", art.getPath().equals(ClusterCover.activeArt()));
        Field off = PhoneLink.class.getDeclaredField("off");
        off.setAccessible(true);
        off.setBoolean(null, true);
        NowPlaying.clear();
        Method received = PhoneLink.class.getDeclaredMethod("received", new Class[] {String.class});
        received.setAccessible(true);
        received.invoke(new PhoneLink(),
            new Object[] {"media=1\ntitle=Song\nart=" + art.getPath() + "\n"});
        cover.getPropertyListener(null).statusProperty(source(cp), null);
        expect("kill switch: stock", ((ActiveSource_Status)pool.sent).sourceType == cp
            && ClusterCover.activeArt() == null);
        off.setBoolean(null, false);

        /* Uninstall: every stock context and the media service back, though process() throws
         * on the host (no HMI services). */
        ActiveSource activeSource = new ActiveSource();
        CurrentStationHandle stationHandle = new CurrentStationHandle();
        activeSource.setDelegate(pool);
        stationHandle.setDelegate(pool);
        ClusterCover.wrap(activeSource);
        ClusterCover.wrap(stationHandle);
        Field context = Function.class.getDeclaredField("context");
        context.setAccessible(true);
        expect("both wrapped", context.get(activeSource) instanceof ClusterCover
            && context.get(stationHandle) instanceof ClusterCover);
        ClusterCover.restore();
        expect("contexts restored", context.get(activeSource) == pool
            && context.get(stationHandle) == pool);
        expect("media service restored", mediaField.get(coverArt) == stockMedia);
        NowPlaying.received("media=1\ntitle=Song\nart=" + art.getPath() + "\n");
        expect("after uninstall: no art", ClusterCover.activeArt() == null);
        NowPlaying.clear();
        System.out.println("cover: USB substitution for CarPlay and Android Auto, stock source,"
            + " no picture server, not started, no art, missing file, media=0, timeout,"
            + " kill switch, uninstall: ok");
        System.exit(0);
    }
}
