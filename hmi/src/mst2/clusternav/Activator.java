/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.io.File;
import java.io.FileOutputStream;

import org.osgi.framework.BundleActivator;
import org.osgi.framework.BundleContext;

/**
 * HMI bundle entry point. Listed in HMI/config/bundles.properties under
 * Bundle.Startup.Lateload, so the stock bundle registry starts it after the GUI is ready
 * (de.vw.mib.osgi.internal.BundleRegistry, de.vw.mib.startup.internal.StartupManager).
 * No stock class is replaced.
 */
public final class Activator implements BundleActivator {
    static final String DATA_DIR = "/tsd/var/mst2-cluster-nav/";

    private Replay replay;
    private PhoneLink phone;
    private Thread phoneThread;

    public void start(BundleContext context) {
        try {
            log("started");
            settle();
            ClusterCarPlay.install();
            NowPlaying.start();
            phone = new PhoneLink();
            phoneThread = new Thread(phone, "mst2-cluster-nav-phone");
            phoneThread.setDaemon(true);
            phoneThread.start();
            if (new File(DATA_DIR + "replay.txt").exists()) {
                replay = new Replay(DATA_DIR + "replay.txt");
                Thread r = new Thread(replay, "mst2-cluster-nav-replay");
                r.setDaemon(true);
                r.start();
                log("replaying " + DATA_DIR + "replay.txt");
            }
        } catch (Throwable t) {
            /* Never let the mod take the HMI down. */
            log("start failed: " + t);
        }
    }

    /*
     * After a minute without trouble, tell the guard in runHMI.sh that the mod has settled: an
     * HMI restart before that turns the mod off (toolbox/custom/mst2-cluster-nav/runHMI.block).
     * The guard's crash mark on the system partition, left before the JVM starts, goes too.
     */
    private static void settle() {
        Thread t = new Thread("mst2-cluster-nav-settle") {
            public void run() {
                try {
                    Thread.sleep(60000);
                    new FileOutputStream("/dev/shmem/mst2-cluster-nav-hmi-ok").close();
                    File busy = new File(DATA_DIR + "hmi-busy");
                    if (busy.exists() && !busy.delete()) {
                        log("cannot remove " + busy);
                    }
                    log("settled");
                } catch (Throwable t) {
                    log("cannot mark settled: " + t);
                }
            }
        };
        t.setDaemon(true);
        t.start();
    }

    public void stop(BundleContext context) {
        try {
            if (phone != null) {
                /* Its cleanup gives the cluster back through the stock notifications first. */
                phone.stop();
                phoneThread.join(3000);
                phone = null;
            }
            if (replay != null) {
                replay.stop();
                replay = null;
            }
        } catch (Throwable t) {
            log("stop: " + t);
        }
        ClusterCarPlay.uninstall();
        NowPlaying.uninstall();
    }

    /* The log in RAM all parts of the mod append to; GEM "Save diagnostics" copies it. */
    private static final String LOG_FILE = "/dev/shmem/mst2-cluster-nav.log";
    private static final long LOG_MAX = 262144;

    static void log(String msg) {
        System.out.println("[mst2-cluster-nav] " + msg);
        try {
            File f = new File(LOG_FILE);
            if (f.length() < LOG_MAX) {
                FileOutputStream out = new FileOutputStream(LOG_FILE, true);
                out.write((System.currentTimeMillis() + " hmi: " + msg + "\n").getBytes());
                out.close();
            }
        } catch (Throwable t) {
            /* logging must never fail the mod */
        }
    }
}
