# Architecture

Reference: Škoda MIB2 Standard `MST2_EU_SK_ZR_P0480T` (i.MX6 QNX 6.5 + TI Jacinto 5), cluster
Continental AID 10" `5E0920790A`.

## Principles

1. **No firmware file is modified or replaced.** The install edits three text files and adds
   our own directory: `cpu.conf` (our library appended to the `LD_PRELOAD` line of SAL and of
   DisplayManager), `runHMI.sh` (a marked block: the guard and our jar *appended* to the boot
   class path), `bundles.properties` (a marked block with our HMI bundle and its ID in the late
   start list). Uninstall gives the three files back byte for byte.
2. **Stock paths only.** Data enters the firmware where the built-in navigation puts it (HMI
   datapool → BAP NavSD / MOST NavInfo) and video enters after the stock encoder (stock MPEG-TS,
   ring buffer, J5, MOST). Nothing the cluster sees is invented.
3. **Fail to stock.** If any part stops, the unit behaves as stock: no phone stream → stock
   cluster map; our bundle fails → HMI keeps running; switched off → no effect at all.
4. **Runtime interposition only where the firmware has no extension point** (CarPlay alt screen
   in SAL, route guidance in the iAP2 driver, encoder output in DisplayManager), via the
   dynamic linker's `LD_PRELOAD`, which the firmware itself already uses for SAL.
5. **No vendor code in this repository.** Our classes call stock APIs; they never ship copies of
   stock classes.

## Switching off and self-repair

The mod must never cost the unit its HMI: whatever goes wrong, it takes itself out and the unit
runs stock.

- **Kill switch.** Each part runs only when no mounted card (`/media/mp000`…`mp009`) carries
  `mst2-cluster-nav-disable`, and the mod is enabled, on its trial boot, or loaded on this boot
  (`/dev/shmem/mst2-cluster-nav-on`). The native hooks and the HMI bundle check again every 2 s;
  switched off, the hooks pass every call straight to the firmware and the bundle writes nothing
  (it hands the cluster notice and the built-in map back and clears its guidance once). The
  native hooks also stay pass-through when `runHMI.sh` no longer carries our block (a tool that
  restores an old `runHMI.sh` removes the guard; the hooks then do not run without it).
- **One trial boot.** The first part that starts on the trial boot (a native service or
  `runHMI.sh`) takes `trial` and leaves `/dev/shmem/mst2-cluster-nav-trial`, so a reset before the
  HMI comes up ends the trial instead of repeating it. GEM arms a trial together with
  `/dev/shmem/mst2-cluster-nav-armed`: that trial belongs to the next boot, and the running
  hooks leave it alone.
- **Libraries next to `cpu.conf`.** A service whose `LD_PRELOAD` library cannot be loaded does
  not start; for DisplayManager the unit then resets within 1.5 s (mQbEMU: an empty
  `libmcn_dm.so` → `iMX6.DispMg` exit 1 → "About to reset"). So the libraries live on the system
  partition (`/tsd/lib/mst2-cluster-nav`). Install first checks every file of the package on the
  card against its `MANIFEST` (size and CRC-32, written by `package.sh`; the unit has `gzip` but
  no `cksum` or `md5`), refuses an empty file and a library that is not a 32-bit ARM ELF, and
  changes nothing if any check fails. All files are then copied under a new name and compared
  byte for byte, and only then renamed into place together, and `cpu.conf` names them only after
  that. The DisplayManager hooks load about 4 s after boot and the HMI much later, so a library
  that cannot be loaded must never reach `cpu.conf`: the guard below would come too late.
  `libmcn_ipod.so`
  has no `cpu.conf` line: SAL looks for it next to its own library (from SAL's `LD_PRELOAD`) and
  hands it to `mm-ipod` only when it is there and not empty; otherwise `mm-ipod` starts stock.
- **Guard in `runHMI.sh`** (every HMI start, before the JVM; it can only ever turn the mod off):
  a preload library missing, empty or not an ARM ELF → all of ours removed from `cpu.conf`
  (atomic rewrite, mode kept, `sync` before the partition goes read-only again) and the mod
  disabled; the HMI started again on the same boot before the mod settled (one minute,
  `/dev/shmem/mst2-cluster-nav-hmi-ok` from the bundle) → disabled. A unit that resets before
  the mod settled loses those RAM marks, so the guard also leaves `hmi-busy` on `/tsd/var` (the
  bundle removes it when it settles): found again within 3 minutes at the next HMI start, it
  counts a strike, and two strikes disable the mod; a start that finds no mark clears them. The
  reason goes to `off-reason`, shown by GEM Status. Either way the boot marks in RAM go too, so
  the native hooks turn to pass-through within 2 s on this boot, not only from the next one.
- **Crash guard in each native part.** While a hook does risky work (SAL: the phone's cluster
  stream and the Android Auto endpoints; DisplayManager: phone frames to the cluster; `mm-ipod`:
  patching the driver and each identification it builds) it keeps a time mark on `/tsd/var`,
  refreshed every minute and cleared at a clean end; nested work counts, the mark goes with the
  last. A mark younger than 3 minutes at the next start means the service died at work and the
  unit came straight back; two such starts in a row turn that service's hooks off until the
  next Install. An ordinary switch-off, or `mm-ipod` ending because a phone was unplugged before
  identification, leaves no fresh mark.
- **Build checks.** Every part that relies on offsets checks the build first and otherwise stays
  stock: SAL the exports of CarPlay plug-in 210.81 (before it reads any session field),
  DisplayManager the exports of `libOpenMAXAL.so.1` and the encoder sink's call site, `mm-ipod`
  the driver's exports and its Location builder slot.
- **Files.** The three stock files are written atomically (new file, same mode, renamed over the
  old), and a rewrite is renamed into place only if, with our lines taken out, it is byte for byte
  the old file with our lines taken out. They are never changed unless their copies exist in
  `/tsd/var/mst2-cluster-nav/backup` and on the card (`mst2-cluster-nav-backup/`, copied and
  compared before anything else: a card that cannot take them stops the install). While the mod
  is not installed, the backups are taken again from the current files (a firmware update
  replaces them); a failed install takes our lines out again instead of copying backups back.
  Uninstall restores the files byte for byte (checked on copies of the 0480 files) and keeps the
  backups while any trace is left.

All parts append to one log in RAM, `/dev/shmem/mst2-cluster-nav.log` (256 KiB at most), which
GEM "Save diagnostics" copies to the card with the iAP2 driver's own log
(`/dev/shmem/iap2_carplay.log`) and the generated `iap2.cfg`.

## Components

```
iPhone ─CarPlay─┬─ stream 111 (H.264) ─► [A] SAL hook ─► shared memory ─► [B] DisplayManager hook
                │                                                │ (replaces encoder output)
                │                          stock TsEncoder → RingIO → J5 → MOST → cluster video
                └─ iAP2 Route Guidance ─► [A] SAL log reader ─┐
Android ─AA──── navigation / media / video ──► [A] SAL hook ─┤ local socket
                                                         ▼
                        [D] HMI bundle "PhoneNav" ─► HMI datapool (as built-in navigation)
                              ─► stock BAP NavSD (LSG 0x32) + MOST NavInfo ─► native cluster arrows
```

| | Process | Mechanism | Job |
|---|---|---|---|
| A | `tsd.mibstd2.sal.main` | `LD_PRELOAD` | advertise the CarPlay instrument-cluster screen (size, safe area, frame rate), receive stream 111, request key frames, optionally re-encode it (zoom-out); iAP2 raw start, log reader, now playing; Android Auto navigation, now playing and cluster video endpoints |
| B | `tsd.mibstd2.system.displaymanager` | `LD_PRELOAD` | while the cluster map is shown and the phone stream is alive, hand the phone's access units to the stock TS encoder instead of the re-encoded capture |
| C | `mm-ipod` (iAP2) | child `LD_PRELOAD` from SAL | add group 30 through the checked Location builder slot; omit it after any identification rejection |
| D | HMI (J9) | own bundle in `bundles.properties` | write phone guidance into the datapool the built-in navigation uses (never while the built-in navigation guides); keep the cluster's "navigation on mobile device" notice away while the phone owns the cluster; the phone's track in the cluster's media view |

## HMI entry point (D)

`de.vw.mib.osgi.internal.BundleRegistry` reads `HMI/config/bundles.properties`: up to 150
bundles (`Bundle.Name.<id>`, `Bundle.Activator.<id>`) and start lists. Activators are loaded
with `Class.forName` from the boot class path and must implement
`org.osgi.framework.BundleActivator` (`BundleInfo.loadBundleActivator`).
`Bundle.Startup.Lateload` bundles start in their own thread after the "GUI ready" startup event
(`StartupManager`, event 8); a failing start is logged and the HMI continues. Stock IDs end at
76, and 51–59 are free in the HMI range, so PhoneNav takes its own ID at the end of
`Bundle.Startup.Lateload`, and its jar is appended to the boot class path, so no stock class
is shadowed.

## Data into the cluster (D)

The built-in navigation reaches the cluster through `HsmTargetBap`, which writes the ASL
datapool: property 732 (route guidance state), lists 751 (maneuver descriptor), 741
(distance to next maneuver), 742 (lanes), 744 (turn-to street), 749/750 (distance / time to
destination; the remaining time goes to list 1110060 through `updateBapRTT`).
`NavigationASLDataAdapter` turns them into BAP NavSD functions 17, 18, 20–24 and
`NavInfoAdapter` into MOST NavInfo. PhoneNav writes the same entries on the ASL thread, through
the same `ASLNavBAPAPI` calls. (Stock also copies the maneuver descriptor to list 838; its only
reader is the head unit's own map maneuver view, so PhoneNav leaves it.)

Two rules keep the phone from fighting the built-in navigation:

- **The built-in navigation goes first.** While it calculates or guides (its guidance datapool,
  the state `HsmTargetBap` turns into 732 = 2 and 1) PhoneNav writes nothing. Stock hands route
  guidance back to the built-in navigation when the driver starts it during CarPlay guidance;
  without this rule the phone's updates, once a second, would overwrite it, and its final "off"
  would clear the new route from the cluster until the built-in navigation changed state again
  (it writes 732 only on state changes and 751 only on new maneuvers).
- **"Off" only once.** An inactive snapshot is written only right after our own active one, and
  a datagram without an `active=` line is ignored.

Unknown values stay invalid: an unknown remaining time is written as stock does for invalid
time (`updateBapTimeToDestination2(-1, -1, 0)`; `-1` minutes would read as a valid 0:00), an
unknown distance to the destination is not written (the BAP value has no invalid encoding and
is valid exactly while guidance is active), and an unknown distance to the maneuver is `-1`
(stock treats ≤ 0 as invalid). The HMI's timeouts treat a clock that jumps (backwards, or by
hours: the unit's clock moves between sessions) as expired.

The cluster draws arrows itself from `ManeuverDescriptor` (FctId 23: main element, direction,
z-level, side streets). `ActiveRgType` (FctId 39) selects arrows (0) or the MOST video map (3);
`InfoStates` (FctId 38) = 6 is the "navigation on mobile device" screen.

## The cluster's "navigation on mobile device" notice (D)

When CarPlay takes over route guidance the stock HMI sets its active navigation to 2, and
InfoStates sends 6: the cluster shows the notice instead of the map or the arrows. InfoStates
is the only reader of `CarPlayService.isCarPlayRouteGuidanceActive`, and it fetches the service
from `APIFactory` on every update. So while the phone owns the cluster — its cluster video runs
(SAL sends `video=1` over the local link about once a second while the stream connection is
open, frames or not) or its guidance feeds the arrows —
`ClusterCarPlay` stands in the factory's private slot as a delegate of the stock service that
answers false there and passes everything else through; it then fires the stock notification
InfoStates listens to. InfoStates falls back to the built-in navigation's state, as without
CarPlay. With no video and no guidance for a few seconds the answer is stock again; when CarPlay
is not coded the factory holds its dummy and nothing is replaced. Android Auto's guidance state
(`AndroidAutoASLDataAdapter`, property 2910005, read by the same InfoStates through
`getAndroidAutoService`) gets the same stand-in while the phone owns the cluster.

Nothing else on the way to the cluster map knows about CarPlay: the stream-sink state machine
(`asl.internal.mostkombi`) switches DisplayManager to context 70 of the cluster display and sets
its frame rate when the cluster asks for the map (DataRate 2 → 10 fps), as with the stock map.

## Video (A + B)

DisplayManager's cluster stream: H.264 Baseline 3.1, 1024×384 cropped to 1010×376, 10 fps
(`ChangeDataRateSequence` in the HMI), packed as MPEG-TS (video PID 0x11) and written to a
ring buffer the J5 forwards to MOST. The cluster's decoder accepts Baseline/Main/High up to
level 4.2, frames up to 250 kB, and has no frame-rate limit of its own; its panel runs at 75 Hz
(`FPKE_1280x480_75Hz_LG`) and the decoder queues up to 16 frames, so a stream faster than the
cluster shows it would back up (an often-quoted "about 25 Hz" ceiling is not in the firmware).
The phone sends key frames only on request, so SAL asks for one only when one is needed: when
DisplayManager starts reading or lost a frame, and when a frame was lost or too big on the way
in (at most one request every 2 s); GEM can add a fixed period (every 2 s or every second, if
the picture breaks up). In the car the windows after the first carried no request at all
("0 asked"). The phone encodes the
window size it is offered, 1010×376, so in H.264 macroblocks 1024×384 cropped to 1010×376 — the
geometry of the stock cluster stream — and it is passed through, with SPS/PPS before every IDR
and key frames requested when the cluster view opens. Profile, level and bitrate of stream 111
are the phone's choice: the receiver offers no codec, profile or level (plug-in 210.81 has only
`maxFPS`). A capture of CarPlay Simulator with an iPhone, with the same cluster window offered
(the header and the avcC travel in clear; analysed with mQbEMU's capture tool), shows High 3.1, 1024×384 cropped to 1010×376, 4
reference frames, 25–30 fps, on average 2.9 Mbit/s with frames up to 87 kB, peaks of 13.3 Mbit/s
over a second during the first seconds, a key frame only at the start, and a keep-alive once a
second whether frames come or not.

The rate is the phone's. `TsEncoder` stamps each frame with the clock when it packs it (PTS =
tick × 90 + 100 ms), not with the HMI's 10 fps, so the phone's pace and pauses reach the cluster
as they are; the offered maximum is 25 fps (GEM: 30, 20, 10). The channel's width is not known from
the firmware; another make's head unit is reported to feed the same cluster a TS of a constant
12.288 Mbit/s (32
bytes a MOST frame at 48 kHz), and the same width is assumed here, so the phone's peaks exceed it
by a few per cent, which the ring to the J5 takes up. A frame that does not
fit the ring to the J5 would be cut short there (`MostSink` drops, never blocks), so for the
phone's stream DisplayManager waits for room in that ring (up to 300 ms) instead; only a ring
that does not drain at all costs a frame, and the picture comes back with the next key frame.
In the car (2026-10-26), with 30 fps offered, a moving map came at 9-14 Mbit/s and about 24 frames
a second reached the cluster; hence 25 fps offered by default.

SAL reads the stream within a budget, 10 Mbit/s by default (`/tsd/var/mst2-cluster-nav/kbps`,
1000–25000, from the next connection): a short burst of 250 ms of it after an idle spell, a 64 kB
socket buffer. Read slower than it sends, the phone's sink overflows and the phone skips frames
before it encodes them, so the stream stays whole. Without a budget the phone sent the cluster
10–13 Mbit/s for seconds (up to 15.5 with the main screen), and then the unit stopped taking
anything from the phone at once — both screens' connections and the control channel stalled
together in the iPhone's log — and CarPlay dropped with `kTimeoutErr`. The phone sizes its
encoders from its own estimate of the link, the same 2187500 B/s for the main screen and the
cluster, and every `forceKeyFrame` re-creates the encoder ("restart bitstream"), so a key frame
starts a burst. With the budget at 10 Mbit/s four drives of 3–10 minutes ran without a drop at
15–25 fps, DisplayManager waiting at most 13 ms for room in the MOST ring; 12 Mbit/s filled that
ring sooner and looked less smooth.

While the phone's video owns the cluster, the HMI bundle (`KombiMap`) hides the built-in cluster
map with the events the HMI itself sends when the cluster asks for no map (1074841846 /
1074841845, `NavigationMapAdapter.setMapInvisible`/`setMapVisible`): `MapViewerKombiStateMain`
goes to `MapViewerKombiStateInvisible`, whose start calls `viewSetInvisible`. In the car the
navigation process then stayed below 2 % CPU instead of 7–8 %. Nothing in BAP reads that
visibility; a DataRate change while hidden may show the map again, so the event is repeated
every 2 s, and the map is shown when the video ends.

The phone owns the cluster while its stream connection is open and it has sent a frame on it:
SAL marks the open connection in the shared memory a few times a second, frames or not, so a
still map (no frames) keeps the phone's last picture instead of flipping to the stock map, and
"video=1" keeps the HMI's notice away. When SAL stops marking (connection closed, SAL gone),
DisplayManager gives the cluster back within 1.5 s, from the stock encoder's next key frame.
DisplayManager's reader sleeps on a process-shared semaphore in the same memory and gets each
access unit as SAL publishes it (polling every 5 ms only if the semaphore cannot be set up).
It starts — on a new stream or after falling a whole ring behind — at the newest key frame still
in the ring, so the phone's first key frame is used rather than asked for again. Each slot is a
sequence lock with barriers on both sides (ARM may reorder the stores and loads otherwise).

A receiver that TEARDOWN could not wait for (it gives up after 3 s and leaves the thread to end
on its own) never touches its successor: only the receiver that currently owns the stream may
mark it alive, end it or tell the HMI. Switched off mid-stream (kill switch), SAL stops
publishing and marking at once — DisplayManager returns to the stock map — but keeps reading the
connection until TEARDOWN, and resumes from a key frame when switched on again.

Instrument-cluster screen advertised to the phone: 1010×376 px as 244×91 mm, the whole 1280×480
10.25" panel the cluster most likely stretches the window over (in the CarPlay Simulator 192 to
360 mm drew the same picture; only the aspect ratio matters), 25 fps (GEM: 30, 20 or 10 — the
width of the MOST channel the J5 opens is not known from the firmware, so this is the knob if the
video stutters), no input, scene
`maps:/car/instrumentcluster/map` (CarPlay Simulator: Content "Map" — map, route and car marker
only). GEM can switch to `maps:/car/instrumentcluster` ("Navigation App": the app's own cluster UI
with its maneuver card), marked experimental because the card covers much of the map in the
cluster's window. One view area over the whole window
with the safe area x 377, y 56, 257×250 for every cluster view. The cluster scales the frame to
1244×464 at (18, 8) (mQbEMU research on the cluster firmware); x 377…634 is
what the round dials of Klassisch leave free and is visible in Erweitert and Modern too; y 56…306 lies
below the status bar and above the bottom bar. Yandex Maps needs about 250 lines of height to draw
its maneuver card (none at 186 or 212 lines in this width) and keeps the car marker in the middle of
such a narrow area, so the card, the car and, if enabled, the ETA bar all stay inside (checked in the
CarPlay Simulator against the cluster's masks and photos of the car's cluster). Size and safe area come from
`/tsd/var/mst2-cluster-nav/display` when present (GEM copies `mst2-cluster-nav-display.txt` from
the card: `width_mm height_mm safe_x safe_y safe_width safe_height`), for calibration in a car.

How SAL gets a second screen (`native/src/sal/`; stock behaviour of plug-in 210.81 checked in
mQbEMU):

1. `/info`: the stock single display plus ours, under `displays` and `displayPanels`
   (`uid`, `initialVideoStreams`), and `extendedFeatures` `altScreen`, `viewAreas`.
2. The first SETUP of a session (the one answering `eventPort`): `enabledFeatures` gains
   `altScreen` and `viewAreas` — what the phone reads to use the second screen.
3. A SETUP with a type-111 stream (stock skips it): our receiver opens its listener the way the
   stock 110 one does (`ServerSocketOpen` in the phone's address family, port -6031, QoS 12),
   derives the stream key with the plug-in's own `AirPlay_DeriveAESKeySHA512ForScreen` from the
   session key (taken in `AirPlayReceiverSessionSetSecurityInfo`), and answers
   `{type: 111, dataPort}`.
4. Frames: 128-byte header + body; avcC (opcode 1) and AES-CTR access units (opcode 0) become
   Annex-B in the shared memory of `native/include/mcn_video.h` (at most 250 000 bytes, the
   cluster decoder's limit); `showUI` with the map scene as soon as the phone connects to the
   stream, `forceKeyFrame` for the cluster's uuid when a key frame is needed (above) or the
   GEM key-frame period runs out.
5. TEARDOWN of the session or of stream 111 stops the receiver; other per-stream teardowns
   leave it running.

Scenes on the cluster (the three the CarPlay Simulator offers): `maps:/car/instrumentcluster` (the
app's own cluster UI), `…/map` (map only), `…/instructioncard` (maneuver card only; Yandex Maps draws
nothing for it).
The card and ETA position is a query on the URL — `?showETA=yes&maneuverLayout=leftAligned`,
`rightAligned` or `topAligned`; without one the phone puts them on the driver's side. Switching
scene is another `showUI` with the new URL and a key frame, without reconnecting. The steering
wheel's map zoom maps to `changeMapZoomLevel {uuid, zoomDirection}`, the dictionary Apple's own
`AirPlayReceiverSessionChangeMapZoomLevel` builds (CarPlay Simulator's CarPlaySDK); the phone
forwards it to the app like `showUI`. Apple Maps zooms its cluster map on it, Yandex Maps does not
(CarPlay Simulator, 2026-10-26).

### Zoom-out (a setting, off by default)

GEM can offer the phone a cluster screen 1.25× or 1.5× bigger (`/tsd/var/mst2-cluster-nav/zoom`,
1000–1900 = factor × 1000): the app then draws more map around the car, and SAL turns the bigger
picture back into the 1010×376 the cluster decodes — decoded on the i.MX6 VPU (`libmxcvpu.so.1`),
scaled on the GC320 2D core through QNX Screen into an uncached scratch buffer, converted to NV12
and encoded on the VPU as 1024×384 cropped to 1010×376 (`native/src/sal/vpu.c`,
`scale_gc320.c`). The safe area scales with the screen, so the car marker stays where it was.
The factor is fixed when `/info` is answered and the same one is used for the session's stream;
the decoder must deliver at least the rectangle that is scaled, or nothing is sent.

There is no CPU fallback (a software scale reads uncached VPU memory, a few frames a second at
best): if the VPU or the GC320 path cannot be set up, or gives up later, SAL publishes nothing
and the cluster keeps its stock map — never a stream the cluster cannot decode. One transcoder
at a time, owned by the receiver that opened it (a stale owner's calls do nothing). The phone is
asked for a key frame only after a loss: a frame too big for the input buffer, a decoder error,
or a full input ring, which is then flushed and decoding restarts from the next key frame. The
VPU firmware is found through `VPU_FW_PATH=/fw/vpu`, which SAL inherits from the unit's startup
script like every service; the mod never touches SAL's environment. Not yet run in a car.

## CarPlay arrows (A + C)

Code: `native/src/sal/iap2.c`, `native/src/ipod/ipod.c`, `native/src/common/route.c`. The P0480T
ABI comes from mQbEMU's research of this firmware. The numbers of group 30 and of the `0x52xx`
messages follow the public protocol description; whether an iPhone accepts them is checked in the
car.

1. SAL interposes **`spawn`**, which the stock `LaunchProcessImpl` calls through its PLT. The
   path `/bin/on`, the APS partition, the fd map, the inheritance and the argv order stay
   stock. Only for `/usr/sbin/mm-ipod`: a RAM copy of the stock `/tsd/var/sal/carplay/iap2.cfg`,
   `/dev/shmem/mst2-cluster-nav-iap2.cfg`, gains `[ident] sendmsgs=0x5200,0x5203` and
   `recvmsgs=0x5201,0x5202,0x5204` (existing IDs are kept) and `[nowplaying]` switched on
   (`metadata=Title,Artist,AlbumTitle`, `playback=Status`; see Now playing), and the child's
   `config=` argument
   names it; the stock cfg is never rewritten. The child's **explicit envp** gets
   `libmcn_ipod.so` appended to its stock `LD_PRELOAD` — `LaunchProcess` env 2 does not pass
   SAL's environment on. The library is taken from the directory of SAL's own `libmcn_sal.so`
   (SAL's `LD_PRELOAD`: `/tsd/lib/mst2-cluster-nav` in a car) and only when it exists and is not
   empty; otherwise `mm-ipod` starts stock.
2. `libmcn_ipod.so` acts only in a process whose main image exports `ipod_log` (not in
   `/bin/on`). In DT_INIT it loads the stock driver before `mm-ipod` does, checks the three
   exports `ipod_module`, `packet_addparam_blob`, `ident_info_funcs` against each other and then
   the pair `{0x16, base+0xea08}` at `base+0xafcb8`; any mismatch leaves everything as it was. A
   match replaces the pointer of the Location builder and fills the driver's empty message-table
   row 0x52 (below): the original builder runs first, then
   `packet_addparam_blob(..., 30, ...)` adds the component (ID 16, name "Instrument Cluster",
   text lengths 255, six maneuvers and six lane guidance entries cached). **Any non-zero
   rejection counter at `dev+0xa8`** leaves group 30 out of a repeated identification (the
   driver exits after the seventh rejection, and the counter is never reset). The RAM ready
   mark goes on rejection and on every new `mm-ipod`. The crash guard covers this library while
   it patches the driver and while it builds an identification — not the rest of `mm-ipod`'s
   life: its two `exit` calls (an accessory that does not speak iAP2, the seventh rejection) are
   outside both, so an unplugged phone is no strike.
3. After the first AirPlay SETUP with `eventPort`, SAL sends the complete control message
   `40 40 00 0c 52 00 00 06 00 00 00 10` through `iap2_raw_msg_send`, over a short-lived
   client of its own to the device path and flags seen in the stock `iap2_connect` (SAL's client
   object is never borrowed); the driver adds sequence numbers and checksum. Sending starts
   only after group 30 was offered (RAM mark), every 2 s for ten tries, then every 30 s until the
   first valid `0x52xx`. When the phone accepted the identification is not visible from SAL.
4. A worker thread in SAL reads `/dev/shmem/iap2_carplay.log` (`mm-ipod -vvvvvv`), where the
   driver prints every unknown message as `--?-- [msgid=5201] CTRL[40 40 .. XX] CKSUM[YY] )`, and
   sends PhoneLink a full snapshot once a second. After a new `mm-ipod` it starts at the end of
   the log (the new driver truncates it before it starts; no `0x52xx` comes before our `0x5200`).
   Reads may split anywhere; a label or the
   closing bracket ends a dump, control messages are limited to 8192 bytes, and a broken
   structure, another component ID or an oversized message is ignored. This is a diagnostic
   log, not a delivery guarantee; whether the driver prints one message split over several link
   packets is still unknown.
5. The phone's iAP2 packets reach the driver one per transport event: the transport keeps a
   ring of 8 events with no full check, and the driver's receive (`iap2_drvr + 0x20`) hands
   over one packet per call, leaving any others in the link buffer until the phone's next
   transfer. With route guidance (about 18 packets a second while moving) the ACK of
   LocationInformation can wait there past its 2 s, and SAL stops iAP2 ("Device timed out").
   `libmcn_ipod.so` wraps that entry to call it again while the link buffer still moves (at most
   16 times, through the driver's own function and locks); in the car a packet was read this way
   about once every few minutes of guidance.

`0x5201` carries the state, distances, time left and the current index; `0x5202` is cached by
index (type 0–53, road after, distance between maneuvers, driving side, junction and exit
angles); a status before its maneuver is fine. An empty current list keeps the last maneuver for
3 s, then hides the arrow; an index that comes back uses the cache. State 2 shows the
destination, which ends guidance 5 s later; state 0 ends guidance and clears the cache. A short
silence keeps guidance (Yandex Maps, parked, sends every ~6 s; while moving the gaps stay below
0.3 s): only 120 s without any status hide it, the cache is kept and the next status brings it
back. AirPlay's own TEARDOWN on a lost connection ends it at once (the plug-in calls TEARDOWN
when the connection closes). No repeated `active=0` that would fight the built-in navigation.
Road names lose their line breaks.
The street line is the road the car is on: the app's current road (`0x5201` field 3; an empty
one keeps the last), else the road the last passed maneuver led onto (field 4, or the
description for Yandex Maps, which sends neither). A maneuver is passed when a different one
replaces it within 50 m and no reroute (state 5) came in between, decided only once the new
maneuver itself has arrived (a status may name an index before its `0x5202`): Yandex Maps gives every update
a new index, the same maneuver included, and switches at 10–50 m. A reroute through state 0
keeps the road; past an unnamed road the line is empty, as it is before the first maneuver.
`0x5204` caches independent lane instructions, including repeated angles and a separate highlighted
exit. The current lane index and visibility flag from `0x5201` select a full PhoneLink snapshot.
`Lanes` maps it to the stock `updateBapLaneGuidance` API (ASL list 742, BAP NavSD 0x18), preserving
road order, secondary directions and recommendations. Hide, stop, arrival and timeouts clear it.
The stock BAP stage limits the visible set to six lanes. Contract, limits and checks: [lanes](LANES.md).

Disabling, TEARDOWN of the whole session, a new `mm-ipod`, a truncated log and the printed
`StartIdentification` reset the state and the retries; the builder checks the kill switch too.
Row 0x52 of the driver's message table is empty in stock, so every `0x52xx` from the phone would
cost a receive packet: `link_handle_reply` (0x14370–0x143b4) logs "Invalid packet" and returns 22
for an ID with no named entry, `link_handle_iap2pkt` (0x15ce8–0x15cf0) passes that on, and
`transport_recv_pkt` (0x26288) then does not give the packet back to the receive pool, which runs
dry after a few — and the next LocationInformation ACK is lost. `libmcn_ipod.so` fills row 0x52
(entries 1, 2 and 4 without the reply bit, no name): `link_handle_reply` returns 0, the packet
goes back, and with no name the driver still prints the `--?-- [msgid=..] CTRL[..]` framing the
reader parses. `0x5200` and `0x5203` keep no entry, so the driver sends them as before. There is
no receive callback.

Checks: `sh native/check.sh` — the decoder with UBSan, every split point of the hex stream in the
stock `CTRL[…]` framing, cache, empty list, stop/arrival, timeout, foreign display, broken TLVs,
idempotent cfg; a host replay of the **real spawn hook** with partial reads and writes, stock
APS/fds/env kept, the child library next to SAL's, an empty child library, the switch off and a
failing cfg read; the `mm-ipod` gate and its busy window; the kill switch, trial, `runHMI.sh`
block and nested crash-guard logic of `common/hook.c`; the avcC/Annex-B framing with malformed
records. `sh toolbox/check.sh` runs the `runHMI.sh` guard from inside a `runHMI.sh` (kill switch,
trial, HMI restart, `hmi-busy` strikes, dual navigation added later, a missing or non-ARM
library, the `cpu.conf` repair with the mode kept) and runs Install, update, Uninstall and every
menu script in a sandbox: damaged packages, a card that cannot take the backup, stale backups, a
rewrite that loses a line, the late-start list in any order, and Uninstall giving the stock files
back byte for byte. `sh hmi/check.sh` checks all 54 maneuvers for both traffic sides, lanes from
a native snapshot, the write rules and now playing.
`native/build.sh` builds the three libraries with `-Werror`; `package.sh` ships the child library
from `preload.list` (`child:mm-ipod`, no `cpu.conf` service). Host checks do not prove the
interposition and the early driver load on QNX; without an iPhone mQbEMU checks SAL and the
reader loading, not identification or a real CarPlay stream.

The DisplayManager hook was checked in mQbEMU, inside the real DisplayManager process, with test
threads playing both ends around it — DisplayManager's reader,
calling libOpenMAXAL's own `BufferQueueGetAndWait` and `BufferQueueDone` on a queue made with
`mmfbq_create` and fed like the stock encoder (a frame each 100 ms, every fifth an IDR), and
SAL's receiver, writing the shared memory as `stream.c` does. It checks that another caller
passes through, the encoder queue is found by its SPS, the phone's units replace stock frames
(which go back to the encoder) and reach the reader on the semaphore, `done` on the phone's unit
stays in the hook, a still map (connection open, no frames for 3 s) keeps the phone's picture,
a vanished SAL gives the cluster back within 1.5 s from the next stock IDR, and the switch-off
gives it back at once.

## Now playing (A + D)

CarPlay: with `[nowplaying]` switched on in the RAM cfg the stock driver starts NowPlaying updates
itself after identification and publishes them in its PPS object `/pps/services/multimedia/iap2/
nowplaying` as `attribute::value` lines (`MediaItemTitle`, `MediaItemArtist`,
`MediaItemAlbumTitle`, `PlaybackStatus`). Nothing in stock reads that object. SAL reads it once a
second while CarPlay is up and sends `media=1` with title, artist and album (cut on a UTF-8
boundary), or `media=0` when the phone reports Paused or Stopped or the track goes. The extra
message IDs fit the identification's 128-byte ID lists with room to spare (94 and 54 bytes).

Android Auto: the receiver library has a
media playback status endpoint that stock SAL never registers; SAL registers a copy of it like
the navigation endpoint below and sends the same datagram with `src=aa`.

`NowPlaying` (HMI) puts the track where the stock HMI writes the phone's station text for the
cluster's media view — "Apple CarPlay" (connection type 2) or "Android Auto" (type 3), info
type 72 — only over the text of the same source, and gives the stock text back on `media=0`,
after 3 s of silence or when switched off. It needs App-Connect coded (feature 452): without it
the HMI has no smartphone-integration service and the cluster keeps its stock text.

## Android Auto (A)

The receiver library `libext.google.gal.receiver.so` (AAP 1.3) has a navigation status and a
media playback status endpoint, but stock SAL registers neither, so the phone is never offered
them. SAL interposes `MessageRouter::populateServiceDiscoveryResponse` (called through the
library's PLT) and, while the mod is active, registers a static copy of each endpoint,
laid out as SAL's own constructors lay out theirs, on the first free service ID, with a copy of
the library's vtable whose channel callbacks start the endpoint and end its data:

- **Arrows**: the library's handlers pass the parsed next-turn event (road, side,
  event, turn number, turn angle) and distance; `common/aa.c` turns them into the same snapshot
  as CarPlay's. The distance handler is interposed too, for our endpoint only, because the
  library skips the callback when the phone leaves out the unit. AA sends no time or distance to
  the destination; those stay invalid. Roundabouts are drawn for right-hand traffic: nothing in
  the library says which way a roundabout turns.
- **Now playing**: above.
- **Cluster video** (not with "Native arrows only"): a second video sink and an input stub are
  appended to the discovery response as raw protobuf fields; the phone's 1280×720 frames are
  copied into a small queue on the library's receive thread and transcoded by a thread of our
  own — the zoom-out transcoder, its 1024×384 middle, no scaling — and each one is acknowledged
  after it was fed, as stock SAL does, so the decoder paces the phone. The codec configuration
  goes in front of every key frame; AAP 1.3 has no key-frame request, so after a loss the
  decoder waits for the phone's next one. A transcoder that cannot start or gives up leaves the
  cluster stock.

The channel close callback is not called when a session ends, so SAL also interposes
`GalReceiver::shutdown` (an import of SAL): after the stock call it ends guidance, now playing
and video of that session. As a last resort the arrows' heartbeat stops after 60 s without any
message from the phone. Switched off, the hooks only pass every call on.

## Local link to the HMI (PhoneLink)

UDP datagrams on 127.0.0.1:39750, UTF-8 text, one `key=value` per line:

| Datagram | Sender | Meaning |
|---|---|---|
| `active=1` + `type`, `angle`, `lht`, `roads`, `dist`, `step`, `street`, `rtt`, `dest`, `lanes`, `laneN` | SAL (CarPlay route reader, AA arrows) | one complete guidance snapshot, about once a second; `-1` = unknown (AA sends no `rtt`/`dest`) |
| `active=0` | SAL | guidance ended (sent once) |
| `video=1` / `video=0` | SAL (CarPlay stream, AA video) | the phone's cluster video is alive (about once a second) / ended |
| `media=1` + `title`, `artist`, `album`[, `src=aa`] | SAL | the playing track, once a second; no `src` = CarPlay |
| `media=0` | SAL | nothing playing |

A datagram that is none of these (no `active=`, `video=` or `media=` line) is ignored. Silence
ends guidance after 5 s and video after 3 s, so a dead process cannot leave the cluster to the
phone.

