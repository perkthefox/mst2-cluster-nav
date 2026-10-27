#!/bin/ksh
# mst2-cluster-nav - remove the mod and its HMI registration.
# Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
. /tsd/etc/persistence/esd/scripts/mcn_common.sh

echo "mst2-cluster-nav: uninstall"
mcn_mount_rw
mcn_remove_all
# The libraries go only after cpu.conf no longer names them.
grep -q 'mst2-cluster-nav/' $CPU_CONF || rm -rf $LIBDIR
mcn_mount_ro
if grep -q 'mst2-cluster-nav' $HMI_SH $BUNDLES $CPU_CONF; then
  # Keep the backups in $MCN/backup (and on the card) until the files are clean.
  echo "Not every trace could be removed; the stock copies stay in $MCN/backup."
  exit 1
fi
rm -rf $MCN
echo "Removed. Reboot the unit."
