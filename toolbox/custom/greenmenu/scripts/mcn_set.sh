#!/bin/ksh
# mst2-cluster-nav - settings: mcn_set.sh enable|disable|trial|arrows|auto|fps10|fps20|fps25|fps30|
#   keyframe0|keyframe1|keyframe2|scene-map|scene-app|display|display-reset|
#   zoom-off|zoom1250|zoom1500|status|diag
# Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
. /tsd/etc/persistence/esd/scripts/mcn_common.sh

if [ ! -e $MCN/mst2-cluster-nav.jar ]; then
  echo "mst2-cluster-nav is not installed."
  exit 1
fi
NEXT="from the next phone connection"
case "$1" in
  enable)
    if grep -q "$LIBDIR/" $CPU_CONF && mcn_installed; then
      touch $MCN/enabled
      rm -f $MCN/off-reason $MCN/hmi-busy $MCN/hmi-strikes
      echo "ENABLED PERMANENTLY: the mod loads on EVERY boot from now on."
    else
      echo "The mod is no longer registered in the stock files ($(cat $MCN/off-reason 2>/dev/null)). Use Install."
    fi
    ;;
  disable) rm -f $MCN/enabled $MCN/trial; echo "Not loaded from the next boot." ;;
  trial)
    rm -f $MCN/hmi-busy $MCN/hmi-strikes
    touch /dev/shmem/mst2-cluster-nav-armed $MCN/trial
    echo "TRIAL BOOT armed: the NEXT boot loads the mod ONCE; then press 'ENABLE PERMANENTLY'."
    ;;
  # "map" (an older menu entry) always behaved as auto; SAL only tells "arrows" apart.
  arrows|auto) echo $1 > $MCN/mode; echo "Cluster mode: $1 ($NEXT)" ;;
  fps10|fps20|fps25|fps30) echo ${1#fps} > $MCN/fps; echo "Cluster video: ${1#fps} fps ($NEXT)" ;;
  # A frame cut short on the way to the cluster (MOST channel full) spoils the picture until the
  # next key frame; regular ones recover it sooner, but each starts a burst of refining frames.
  keyframe0) rm -f $MCN/keyframe; echo "Cluster video: a key frame only when needed ($NEXT)" ;;
  keyframe1|keyframe2) echo ${1#keyframe} > $MCN/keyframe; echo "Cluster video: a key frame every ${1#keyframe} s ($NEXT)" ;;
  scene-map) rm -f $MCN/scene; echo "Cluster: phone map only ($NEXT)" ;;
  scene-app) echo app > $MCN/scene; echo "Cluster: phone map with its maneuver card, experimental ($NEXT)" ;;
  display)
    # Calibration from the card: "width_mm height_mm safe_x safe_y safe_width safe_height".
    card=$(mcn_card) || { echo "Toolbox SD card not found."; exit 1; }
    if [ -s $card/mst2-cluster-nav-display.txt ]; then
      cp $card/mst2-cluster-nav-display.txt $MCN/display
      echo "Cluster geometry: $(cat $MCN/display) ($NEXT)"
    else
      echo "Put mst2-cluster-nav-display.txt on the card, e.g. 244 91 377 56 257 250"
    fi
    ;;
  display-reset) rm -f $MCN/display; echo "Cluster geometry: default (244 91 377 56 257 250)" ;;
  zoom-off) rm -f $MCN/zoom; echo "CarPlay cluster map zoom-out off ($NEXT)" ;;
  zoom1250|zoom1500) echo ${1#zoom} > $MCN/zoom; echo "CarPlay cluster map zoom-out x${1#zoom} / 1000 ($NEXT)" ;;
  status) ;;
  diag)
    # Run while CarPlay is connected. Contains the route, the phone's identity and the unit's
    # network details: the menu entry says so.
    card=$(mcn_card) || { echo "Toolbox SD card not found."; exit 1; }
    out=$card/mst2-cluster-nav-diag.txt
    {
      for c in "pfctl -s info" "pfctl -s rules" "pfctl -a port_rules -s rules" \
        "cat /tsd/var/networking/port_rules.conf" "ifconfig -a" "netstat -an" "ls -l /dev/shmem" \
        "cat /dev/shmem/mst2-cluster-nav.log" "cat /tsd/var/sal/carplay/iap2.cfg" \
        "cat /dev/shmem/mst2-cluster-nav-iap2.cfg" \
        "ls -l /dev/shmem/mst2-cluster-nav-rg-ready" \
        "ls -l $MCN $LIBDIR"; do
        echo "### $c"
        $c 2>&1
      done
      for f in mode fps keyframe scene display kbps zoom enabled \
        trial off-reason hmi-busy hmi-strikes off-sal off-dm off-ipod strikes-sal strikes-dm \
        strikes-ipod busy-sal busy-dm busy-ipod; do
        [ -e $MCN/$f ] && echo "### $f: $(cat $MCN/$f)"
      done
      echo "### runHMI.sh block: $(mcn_installed && echo yes || echo no)"
    } > $out
    # The iAP2 driver's own log (mm-ipod -vvvvvv): every message, unknown ones in hex.
    cp /dev/shmem/iap2_carplay.log $card/mst2-cluster-nav-iap2.log 2>/dev/null
    echo "Saved $out"
    ;;
  *) echo "unknown setting $1"; exit 1 ;;
esac
if [ -e $MCN/enabled ]; then
  echo "STATE: ENABLED PERMANENTLY (loads on every boot)"
elif [ -e /dev/shmem/mst2-cluster-nav-trial ] || { [ -e /dev/shmem/mst2-cluster-nav-on ] &&
    [ ! -e $MCN/trial ]; }; then
  echo "STATE: TRIAL BOOT RUNNING NOW - after the next reboot the mod is OFF."
  echo "       All good? -> press 'ENABLE PERMANENTLY' now."
elif [ -e $MCN/trial ]; then
  echo "STATE: TRIAL BOOT ARMED - the NEXT boot loads the mod ONCE."
else
  echo "STATE: OFF (not loaded at boot). 'Arm another trial boot' or 'ENABLE PERMANENTLY'."
fi
echo "HMI $(mcn_hmi_version); registered: $(mcn_installed && echo yes || echo no)"
[ -e $MCN/off-reason ] && echo "turned itself off: $(cat $MCN/off-reason)"
for c in sal dm ipod; do
  [ -e $MCN/off-$c ] && echo "$c hooks off after repeated crashes (Install turns them on again)"
done
[ -e $MCN/hmi-busy ] && echo "HMI with the mod not settled yet (or went down before settling)"
[ -s $MCN/hmi-strikes ] && echo "HMI starts that did not settle: $(cat $MCN/hmi-strikes) (2 turn the mod off)"
echo "permanent: $([ -e $MCN/enabled ] && echo yes || echo no);" \
  "trial armed: $([ -e $MCN/trial ] && echo yes || echo no);" \
  "mode: $(sed 's/^map$/auto/' $MCN/mode 2>/dev/null); fps: $(cat $MCN/fps 2>/dev/null || echo 25);" \
  "key frame: $([ -e $MCN/keyframe ] && echo "every $(cat $MCN/keyframe) s" || echo "when needed");" \
  "scene: $([ -e $MCN/scene ] && echo map+card || echo map);" \
  "geometry: $(cat $MCN/display 2>/dev/null || echo default);" \
  "kbps: $(cat $MCN/kbps 2>/dev/null || echo default); zoom: $(cat $MCN/zoom 2>/dev/null || echo off)"
