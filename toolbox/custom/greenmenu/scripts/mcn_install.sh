#!/bin/ksh
# mst2-cluster-nav - install, or update, and arm one trial boot.
# Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
. /tsd/etc/persistence/esd/scripts/mcn_common.sh

echo "mst2-cluster-nav: install"
mcn_check_version || exit 1
CARD=$(mcn_card) || { echo "Aborted: no SD card with custom/mst2-cluster-nav."; exit 1; }
SRC=$CARD/custom/mst2-cluster-nav
mcn_check_unit $SRC/preload.list || exit 1

if mcn_navignore_lines > /dev/null; then
  echo "Aborted: the toolbox 'dual navigation' patch (NavActiveIgnore.jar) is installed."
  echo "It replaces stock HMI classes and conflicts with this mod. Disable it first:"
  echo "Customization -> Navigation -> Disable dual navigation, then reboot."
  echo "Found in $HMI_SH, with $NAVIGNORE_JAR present:"
  mcn_navignore_lines
  exit 1
fi

# From Install on, cpu.conf preloads the libraries on every boot, before the HMI and its guard
# start: one that cannot be loaded resets the unit in a loop. So every file is checked against
# the package's MANIFEST before anything is changed.
if ! mcn_verify_card $SRC; then
  echo "Aborted: the package on the card is incomplete or damaged; nothing was changed."
  exit 1
fi

# Stock copies, on the unit and, so that they survive a unit whose HMI no longer starts, on the
# card. While the mod is not registered they are taken again from the current files (a firmware
# update may have replaced them), with any part of the mod left elsewhere stripped.
mkdir -p $MCN/backup
if ! mcn_installed && ! { mcn_backup runHMI.sh $HMI_SH mcn_strip_block &&
    mcn_backup bundles.properties $BUNDLES mcn_bundles_remove &&
    mcn_backup cpu.conf $CPU_CONF mcn_unpreload_conf; }; then
  echo "Aborted: could not back up the stock files to $MCN/backup; nothing was changed."
  exit 1
fi
if ! mcn_backup_card $CARD/mst2-cluster-nav-backup; then
  echo "Aborted: could not copy the stock files to $CARD/mst2-cluster-nav-backup/"
  echo "(card full or write-protected?); nothing was changed."
  exit 1
fi
echo "Stock files backed up to $CARD/mst2-cluster-nav-backup/ (keep this copy)."

# All files are copied under a new name and compared first; only then do they all take their
# names, so a failed copy leaves the previous version complete.
LIBS=$(awk '!/^#/ && NF == 2 { print $2 }' $SRC/preload.list)
mcn_mount_rw
mkdir -p $LIBDIR
ok=1
mcn_put $SRC/mst2-cluster-nav.jar $MCN/mst2-cluster-nav.jar 644 || ok=0
for lib in $LIBS; do
  [ $ok = 1 ] && { mcn_put $SRC/$lib $LIBDIR/$lib 755 || ok=0; }
done
if [ $ok = 1 ]; then
  mcn_put_commit $MCN/mst2-cluster-nav.jar
  for lib in $LIBS; do
    mcn_put_commit $LIBDIR/$lib
  done
fi
rm -f $MCN/mst2-cluster-nav.jar.new
for lib in $LIBS; do
  rm -f $LIBDIR/$lib.new
done
mcn_mount_ro
if [ $ok != 1 ]; then
  echo "Aborted: the files could not be copied intact (unit storage full?); the stock files"
  echo "are unchanged."
  exit 1
fi
[ -e $MCN/mode ] || echo auto > $MCN/mode

mcn_mount_rw
# The jar goes to the END of the boot class path, after the stock line that defines it.
mcn_rewrite $HMI_SH mcn_strip_block mcn_hmi_with_block $SRC/runHMI.block &&
  mcn_rewrite $BUNDLES mcn_bundles_remove mcn_bundles_with_block $SRC/bundles.block &&
  mcn_rewrite $CPU_CONF mcn_unpreload_conf mcn_conf_with_preload $SRC/preload.list
mcn_mount_ro
if ! mcn_installed || ! grep -q "^Bundle\.Startup\.Lateload = .*,$BUNDLE_ID\$" $BUNDLES ||
    ! grep -q "$LIBDIR/" $CPU_CONF; then
  echo "Registration failed; taking the mod out of the stock files again."
  mcn_mount_rw
  mcn_remove_all
  mcn_mount_ro
  if grep -q 'mst2-cluster-nav' $HMI_SH $BUNDLES $CPU_CONF; then
    echo "Not every trace could be removed: the stock copies are in $MCN/backup and on the card."
  fi
  exit 1
fi
rm -f $MCN/off-reason $MCN/hmi-busy $MCN/hmi-strikes
for c in sal dm ipod; do
  rm -f $MCN/off-$c $MCN/strikes-$c $MCN/busy-$c
done

rm -f $MCN/enabled
# The trial is the next boot's: the running hooks must not take it (native/include/mcn_hook.h).
touch /dev/shmem/mst2-cluster-nav-armed $MCN/trial
echo "Installed."
echo "##########################################################################"
echo "#  TRIAL BOOT: reboot now. The mod runs for THIS ONE NEXT BOOT ONLY.     #"
echo "#  After that boot it is OFF again, unless you press                     #"
echo "#  'ENABLE PERMANENTLY' in this menu while the trial boot is running.    #"
echo "#                                                                        #"
echo "#  Why: the trial boot lets you check that everything works and nothing  #"
echo "#  breaks (CarPlay, the cluster, the unit itself). If anything is wrong, #"
echo "#  just reboot: the unit comes back stock on its own.                    #"
echo "##########################################################################"
echo "Emergency: an SD card with an empty file 'mst2-cluster-nav-disable' stops the mod"
echo "before the HMI starts."
