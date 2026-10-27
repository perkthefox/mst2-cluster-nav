/*
 * mst2-cluster-nav - host self-check of the phone track substitution (JDK 8: hmi/check.sh).
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.lang.reflect.Constructor;

import de.vw.mib.bap.datatypes.BAPEntity;
import de.vw.mib.bap.functions.ArrayListener;
import de.vw.mib.bap.functions.BAPFunction;
import de.vw.mib.bap.functions.BAPFunctionController;
import de.vw.mib.bap.functions.MethodListener;
import de.vw.mib.bap.functions.Property;
import de.vw.mib.bap.functions.PropertyListener;
import de.vw.mib.bap.mqbab2.audiosd.functions.CurrentStationInfo;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStage;
import de.vw.mib.bap.mqbab2.common.api.stages.BAPStageContext;
import de.vw.mib.bap.mqbab2.generated.audiosd.serializer.CurrentStationInfo_Status;

public final class NowPlayingCheck implements BAPStageContext, PropertyListener {
    private CurrentStationInfo_Status sent;

    public boolean statusProperty(BAPEntity e, Property p) {
        sent = (CurrentStationInfo_Status)e;
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

    private static CurrentStationInfo_Status status(String primary) {
        CurrentStationInfo_Status s = new CurrentStationInfo_Status();
        s.primaryInformation.setContent(primary);
        s.pi_Type = 72;
        return s;
    }

    private static void expect(String what, boolean ok) {
        if (!ok) {
            System.out.println("FAIL " + what);
            System.exit(1);
        }
    }

    public static void main(String[] args) throws Exception {
        NowPlayingCheck stock = new NowPlayingCheck();
        Constructor c = NowPlaying.class.getDeclaredConstructor(
            new Class[] {CurrentStationInfo.class, BAPStageContext.class});
        c.setAccessible(true);
        NowPlaying np = (NowPlaying)c.newInstance(new Object[] {null, stock});

        np.getPropertyListener(null).statusProperty(status("Apple CarPlay"), null);
        expect("no track: stock text", "Apple CarPlay".equals(stock.sent.primaryInformation.toString()));

        NowPlaying.received("media=1\ntitle=Song\nartist=Band\nalbum=\n");
        np.getPropertyListener(null).statusProperty(status("Apple CarPlay"), null);
        expect("title", "Song".equals(stock.sent.primaryInformation.toString()));
        expect("artist", "Band".equals(stock.sent.secondaryInformation.toString())
            && stock.sent.si_Type == 73);
        expect("no album", stock.sent.ti_Type == 0);

        np.getPropertyListener(null).statusProperty(status("Android Auto"), null);
        expect("other source untouched", "Android Auto".equals(stock.sent.primaryInformation.toString()));

        NowPlaying.tick(System.currentTimeMillis() + 4000);
        np.getPropertyListener(null).statusProperty(status("Apple CarPlay"), null);
        expect("timeout: stock text", "Apple CarPlay".equals(stock.sent.primaryInformation.toString()));

        NowPlaying.received("media=1\ntitle=Song\nartist=Band\nalbum=\n");
        NowPlaying.tick(System.currentTimeMillis() - 60000);
        np.getPropertyListener(null).statusProperty(status("Apple CarPlay"), null);
        expect("clock set back: stock text", "Apple CarPlay".equals(stock.sent.primaryInformation.toString()));

        NowPlaying.received("media=1\ntitle=Track\nartist=Artist\nalbum=Album\nsrc=aa\n");
        np.getPropertyListener(null).statusProperty(status("Android Auto"), null);
        expect("android auto title", "Track".equals(stock.sent.primaryInformation.toString()));
        expect("android auto album", "Album".equals(stock.sent.tertiaryInformation.toString())
            && stock.sent.ti_Type == 74);
        np.getPropertyListener(null).statusProperty(status("Apple CarPlay"), null);
        expect("android auto track not on CarPlay", "Apple CarPlay".equals(stock.sent.primaryInformation.toString()));

        NowPlaying.received("media=0");
        np.getPropertyListener(null).statusProperty(status("Android Auto"), null);
        expect("media=0: stock text", "Android Auto".equals(stock.sent.primaryInformation.toString()));

        NowPlaying.received("media=1\ntitle=Song\nartist=Band\nalbum=\n");
        NowPlaying.received("media=1\ntitle=\n");
        np.getPropertyListener(null).statusProperty(status("Apple CarPlay"), null);
        expect("empty title: stock text", "Apple CarPlay".equals(stock.sent.primaryInformation.toString()));
        System.out.println("now playing: CarPlay and Android Auto substitution, other sources,"
            + " media=0, timeout, clock set back: ok");
    }
}
