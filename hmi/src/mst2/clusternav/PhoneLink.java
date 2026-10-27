/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 */
package mst2.clusternav;

import java.io.File;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * Guidance from SAL (CarPlay route guidance decoded from the stock iAP2 log, Android Auto's
 * navigation status from its session): one UDP datagram on 127.0.0.1 per update, UTF-8 text, one
 * "key=value" per line:
 *
 *   active=1           0 ends guidance
 *   type=2             iAP2 maneuver type 0..53 (Android Auto's maneuvers are mapped onto them)
 *   angle=90           exit angle, degrees, negative = left (absent: unknown)
 *   lht=0              1 for left-hand traffic, -1 unknown: the HMI decides (TrafficSide)
 *   around=90          Android Auto roundabouts: degrees travelled round, 1..360 (Maneuvers)
 *   exit=2             Android Auto roundabouts: exit number (already in type 28..46; ignored)
 *   roads=-90,0,90     junction road angles (side streets)
 *   dist=250           metres to the maneuver, -1 unknown
 *   step=400           metres from the previous maneuver (for the approach bar)
 *   street=Main St     road the car is on (the app's, else the one the last maneuver led onto)
 *   rtt=900            seconds to the destination, -1 unknown (CarPlay only)
 *   dest=12000         metres to the destination, -1 unknown (CarPlay only)
 *
 * Unknown values are never shown as zero: a missing dist is -1 like a sent one.
 *
 * SAL also sends "video=1" about once a second while the phone's cluster stream connection is
 * open (CarPlay or Android Auto; frames or not: a still map sends none), and "video=0" when it
 * closes; that datagram carries nothing else. While either the video or the guidance is alive
 * the phone owns the cluster (ClusterCarPlay).
 *
 * The phone's track comes once a second while it plays, "media=1" with "title=", "artist=",
 * "album=" lines, "src=aa" for Android Auto (absent: CarPlay) and "art=" with the path of its
 * cover picture (absent: none), and "media=0" when it ends (NowPlaying, ClusterCover).
 *
 * Silence for GUIDANCE_TIMEOUT_MS also ends guidance, and for VIDEO_TIMEOUT_MS the video, so a
 * dead phone or process cannot leave the cluster to it. J9 Foundation 1.1 has no nanoTime, so
 * the timeouts use the wall clock and take a clock that jumps (either way) as expired.
 *
 * The same thread checks the SD-card kill switch every KILL_CHECK_MS, as the native hooks do:
 * while a card carries the file, nothing of the phone (or a replay) reaches the cluster.
 */
final class PhoneLink implements Runnable {
    static final int PORT = 39750;
    private static final int GUIDANCE_TIMEOUT_MS = 5000;
    private static final int VIDEO_TIMEOUT_MS = 3000;
    private static final int KILL_CHECK_MS = 2000;
    private static final String KILL_FILE = "/mst2-cluster-nav-disable";

    private static volatile boolean off;

    private volatile boolean running = true;
    private DatagramSocket socket;
    private boolean active, video;
    private long guidanceMs, videoMs, killMs;

    /** The SD-card kill switch was found on the last check. */
    static boolean switchedOff() {
        return off;
    }

    /** A wall-clock timeout; a negative or oversized delta (clock set) counts as expired. */
    static boolean expired(long since, long now, long limit) {
        long d = now - since;
        return d < 0 || d >= limit;
    }

    void stop() {
        running = false;
        if (socket != null) {
            socket.close();
        }
    }

    public void run() {
        try {
            socket = new DatagramSocket(PORT, InetAddress.getByName("127.0.0.1"));
            socket.setSoTimeout(1000);
            byte[] buf = new byte[4096];
            killMs = System.currentTimeMillis() - KILL_CHECK_MS;
            while (running) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    socket.receive(p);
                    received(new String(p.getData(), 0, p.getLength(), "UTF-8"));
                } catch (java.io.InterruptedIOException timeout) {
                    /* checked below */
                }
                long now = System.currentTimeMillis();
                if (expired(killMs, now, KILL_CHECK_MS)) {
                    killMs = now;
                    checkKillSwitch();
                }
                if (active && expired(guidanceMs, now, GUIDANCE_TIMEOUT_MS)) {
                    Datapool.publish(Guidance.inactive());
                    active = false;
                }
                if (video && expired(videoMs, now, VIDEO_TIMEOUT_MS)) {
                    video = false;
                }
                ClusterCarPlay.setPhoneOwnsCluster(!off && (active || video));
                KombiMap.phoneVideo(!off && video);
                NowPlaying.tick(now);
                ClusterCover.tick(now);
                CallLog.tick(now);
            }
        } catch (Throwable t) {
            if (running) {
                Activator.log("phone link stopped: " + t);
            }
        }
        if (active) {
            Datapool.publish(Guidance.inactive());
        }
        ClusterCarPlay.setPhoneOwnsCluster(false);
        KombiMap.phoneVideo(false);
        NowPlaying.clear();
    }

    private void checkKillSwitch() {
        boolean found = false;
        for (int i = 0; i < 10 && !found; i++) {
            found = new File("/media/mp00" + i + KILL_FILE).exists();
        }
        if (found == off) {
            return;
        }
        off = found;
        Activator.log(found ? "kill switch on a card: phone kept off the cluster"
                            : "kill switch removed: phone back on the cluster");
        if (found) {
            /* Datapool turns everything inactive now, once (only if we had written guidance). */
            Datapool.publish(Guidance.inactive());
            NowPlaying.clear();
        }
    }

    private void received(String text) {
        if (text.startsWith("media=")) {
            if (!off) {
                NowPlaying.received(text);
            }
            return;
        }
        if (text.startsWith("video=")) {
            video = text.startsWith("video=1");
            videoMs = System.currentTimeMillis();
            return;
        }
        if (!text.startsWith("active=") && text.indexOf("\nactive=") < 0) {
            /* Not a guidance snapshot (every one carries active=): never taken as "inactive". */
            return;
        }
        Guidance g = parse(text);
        /* Datapool writes only on our own edges and never over the built-in navigation. */
        Datapool.publish(g);
        active = g.active;
        guidanceMs = System.currentTimeMillis();
    }

    static Guidance parse(String text) {
        Guidance g = new Guidance();
        int type = -1;
        int angle = Maneuvers.NO_ANGLE;
        int lht = 0;
        int around = 0;
        int[] roads = null;
        int step = 0;
        int laneCount = 0;
        boolean laneCountSeen = false;
        String[] laneRows = new String[Lanes.MAX_LANES];
        boolean badLanes = false;
        int start = 0;
        while (start < text.length()) {
            int end = text.indexOf('\n', start);
            if (end < 0) {
                end = text.length();
            }
            String line = text.substring(start, end);
            start = end + 1;
            int eq = line.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            String key = line.substring(0, eq);
            String value = line.substring(eq + 1);
            if (key.equals("active")) {
                g.active = value.equals("1");
            } else if (key.equals("type")) {
                type = number(value, -1);
            } else if (key.equals("angle")) {
                angle = number(value, Maneuvers.NO_ANGLE);
            } else if (key.equals("lht")) {
                lht = number(value, 0);
            } else if (key.equals("around")) {
                around = number(value, 0);
            } else if (key.equals("roads")) {
                roads = numbers(value);
            } else if (key.equals("dist")) {
                g.distanceToManeuver = number(value, -1);
            } else if (key.equals("step")) {
                step = number(value, 0);
            } else if (key.equals("street")) {
                g.street = value;
            } else if (key.equals("rtt")) {
                g.remainingSeconds = number(value, -1);
            } else if (key.equals("dest")) {
                g.distanceToDestination = number(value, -1);
            } else if (key.equals("lanes")) {
                badLanes |= laneCountSeen;
                laneCountSeen = true;
                laneCount = number(value, -1);
            } else if (key.startsWith("lane")) {
                int slot = number(key.substring(4), -1);
                if (slot < 0 || slot >= laneRows.length || laneRows[slot] != null) {
                    badLanes = true;
                } else {
                    laneRows[slot] = value;
                }
            }
        }
        boolean left = lht < 0 ? TrafficSide.leftHand() : lht == 1;
        int[] m = Maneuvers.map(type, angle, left, around);
        g.mainElement = m[0];
        g.direction = m[1];
        g.sideStreets = Maneuvers.sideStreets(roads, angle);
        if (g.active && !badLanes) {
            g.lanes = Lanes.parse(laneRows, laneCount);
        }
        if (step > 0 && g.distanceToManeuver >= 0 && g.distanceToManeuver <= step) {
            g.showBargraph = true;
            g.bargraph = g.distanceToManeuver * 100 / step;
        }
        return g;
    }

    private static int number(String s, int fallback) {
        try {
            return Integer.parseInt(s.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static int[] numbers(String s) {
        int n = s.length() == 0 ? 0 : 1;
        for (int i = 0; i < s.length(); i++) {
            if (s.charAt(i) == ',') {
                n++;
            }
        }
        int[] out = new int[n];
        int start = 0;
        for (int i = 0; i < n; i++) {
            int end = s.indexOf(',', start);
            if (end < 0) {
                end = s.length();
            }
            out[i] = number(s.substring(start, end), 1000);
            start = end + 1;
        }
        return out;
    }
}
