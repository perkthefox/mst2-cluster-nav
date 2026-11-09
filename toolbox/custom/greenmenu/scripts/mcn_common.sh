#!/bin/ksh
# mst2-cluster-nav - shared definitions for the Green Engineering Menu scripts.
# Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.

MCN=/tsd/var/mst2-cluster-nav
# The libraries LD_PRELOAD names live on the system partition, next to cpu.conf: a service whose
# preload is missing does not start (DisplayManager: the unit resets), so they must not depend on
# /tsd/var. They change only together with cpu.conf, by this installer or a firmware update.
LIBDIR=/tsd/lib/mst2-cluster-nav
HMI_SH=/tsd/hmi/runHMI.sh
BUNDLES=/tsd/hmi/HMI/config/bundles.properties
CPU_CONF=/tsd/etc/system/cpu.conf
BUNDLE_ID=58
# Scratch space in RAM for comparisons.
MCN_TMP=/dev/shmem
# HMI builds this mod was verified on; datapool ids differ between builds.
HMI_VERSIONS="H29.319.120"

# The toolbox card that holds custom/mst2-cluster-nav. A card with the package's MANIFEST comes
# first: the other slot may hold an older copy, or the repository's source tree (no MANIFEST).
mcn_card() {
  for f in MANIFEST .; do
    for v in /media/mp00?; do
      if [ -e $v/custom/mst2-cluster-nav/$f ]; then
        echo $v
        return 0
      fi
    done
  done
  return 1
}

# One line in the event history the guards keep across boots (runHMI.block rotates it).
mcn_history() {
  echo "$(/bin/date -t) gem: $*" >> $MCN/history
}

mcn_hmi_version() {
  sed -n 's/^VERSION=//p' $HMI_SH | head -1
}

mcn_check_version() {
  v=$(mcn_hmi_version)
  for ok in $HMI_VERSIONS; do
    [ "$v" = "$ok" ] && return 0
  done
  if [ -e $MCN/force-version ]; then
    echo "HMI $v is not verified; continuing because $MCN/force-version exists."
    return 0
  fi
  echo "Aborted: HMI version $v is not verified for this mod (verified: $HMI_VERSIONS)."
  return 1
}

# The mod hooks services of the navigation build of MIB2 Standard (SAL runs CarPlay there,
# DisplayManager encodes the cluster map). The radio build (Bolero) has the same HMI version but
# no SAL: CarPlay runs elsewhere and nothing draws a cluster map. $1 is the card's preload.list.
mcn_check_unit() {
  if [ ! -x /tsd/bin/sal/tsd.mibstd2.sal.main ]; then
    echo "Aborted: this unit has no /tsd/bin/sal/tsd.mibstd2.sal.main. The mod needs the navigation"
    echo "build of MIB2 Standard (e.g. Amundsen); radio units (Bolero) are not supported."
    return 1
  fi
  for svc in $(awk '!/^#/ && NF == 2 && $1 !~ /:/ { print $1 }' "$1"); do
    if ! grep -q "^service $svc\$" $CPU_CONF; then
      echo "Aborted: $CPU_CONF has no service $svc; this unit is not supported."
      return 1
    fi
  done
  return 0
}

# The toolbox's "dual navigation" patch puts $NAVIGNORE_JAR ($MIBJAR in runHMI.sh) on the boot
# class path. It is active only while a line outside our block that is not a comment names it
# and the jar is there: its "Disable" restores an older runHMI.sh copy, which may still carry the
# line, and a commented line loads nothing. Prints those lines; fails when the patch is off.
NAVIGNORE_JAR=/tsd/hmi/HMI/jar/NavActiveIgnore.jar
mcn_navignore_lines() {
  [ -e $NAVIGNORE_JAR ] || return 1
  mcn_strip_block $HMI_SH | grep -v '^[ 	]*#' | grep 'NavActiveIgnore.jar'
}

mcn_installed() {
  grep -q '>>> mst2-cluster-nav' $HMI_SH
}

# The toolbox's own helpers remount the system partition (/dev/hd0t177).
mcn_mount_rw() {
  . /tsd/etc/persistence/esd/scripts/util_mount.sh
}

mcn_mount_ro() {
  sync
  . /tsd/etc/persistence/esd/scripts/util_mount_ro.sh
}

# Append $MCN/<lib> to LD_PRELOAD of each service in preload.list ($1), or give the service an
# LD_PRELOAD line when it has none. Writes the result to stdout.
mcn_preload_conf() {
  awk -v list="$1" -v dir="$LIBDIR" '
    BEGIN { while ((getline l < list) > 0) if (l !~ /^#/ && split(l, f) == 2) lib[f[1]] = dir "/" f[2] }
    function flush() { if (svc != "" && (svc in lib) && !done) print "   envar LD_PRELOAD " lib[svc] }
    /^service / { flush(); svc = $2; done = 0; print; next }
    (svc in lib) && $1 == "envar" && $2 == "LD_PRELOAD" && !done { print $0 ":" lib[svc]; done = 1; next }
    { print }
    END { flush() }' ${2:-$CPU_CONF}
}

# Remove our libraries from every LD_PRELOAD in cpu.conf ($1, default $CPU_CONF; any earlier
# location too). To stdout.
mcn_unpreload_conf() {
  sed -e '\#^   envar LD_PRELOAD /tsd/[a-z]*/mst2-cluster-nav/[^:]*$#d' \
      -e 's#:/tsd/[a-z]*/mst2-cluster-nav/[^: ]*##g' "${1:-$CPU_CONF}"
}

# The packaged files' check value (package.sh writes the same into the card's MANIFEST): the
# trailer gzip appends, CRC-32 and length of the input (RFC 1952), as hex bytes. The unit has no
# cksum or md5; gzip, tail and od are there.
mcn_sum() {
  gzip -c "$1" | tail -c 8 | od -A n -t x1 | awk '{ for (i = 1; i <= NF; i++) s = s $i } END { print s }'
}

# A shared library the unit's loader can map: 32-bit little-endian ELF (bytes 0-5), shared
# object for ARM (e_type 3, e_machine 40 at bytes 16-19). Never ldd: it would run the library.
mcn_arm_so() {
  case $(od -A n -t x1 -N 20 "$1" | awk '{ for (i = 1; i <= NF; i++) s = s $i } END { print s }') in
    7f454c460101????????????????????03002800) return 0 ;;
  esac
  return 1
}

# Every file the card's MANIFEST names is there, not empty and matches it, and every library
# preload.list names is one of them and a shared library for ARM. $1 is the card's directory.
# Prints what is wrong; changes nothing.
mcn_verify_card() {
  if [ ! -s "$1/MANIFEST" ]; then
    echo "$1/MANIFEST is missing. The folder holds:"
    ls "$1"
    if [ ! -e "$1/libmcn_sal.so" ]; then
      echo "No libraries either: this is the source code, not the package."
      echo "Download mst2-cluster-nav-<version>.zip from the release's"
      echo "Assets (not 'Source code') and unzip it onto the card."
    else
      echo "Copy the whole package to the card again."
    fi
    return 1
  fi
  bad=0
  while read f sum; do
    case "$f" in ''|'#'*) continue ;; esac
    if [ ! -s "$1/$f" ]; then
      echo "$f is missing or empty on the card."
      bad=1
    elif [ "$(mcn_sum "$1/$f")" != "$sum" ]; then
      echo "$f on the card is damaged (check value differs from MANIFEST)."
      bad=1
    fi
  done < "$1/MANIFEST"
  for lib in $(awk '!/^#/ && NF == 2 { print $2 }' "$1/preload.list"); do
    if ! grep -q "^$lib " "$1/MANIFEST"; then
      echo "$lib is not in MANIFEST."
      bad=1
    elif [ -s "$1/$lib" ] && ! mcn_arm_so "$1/$lib"; then
      echo "$lib is not an ARM shared library."
      bad=1
    fi
  done
  return $bad
}

# Stock copy $MCN/backup/$1 of file $2, with the mod's parts stripped by $3 (from an earlier
# install left in the file).
mcn_backup() {
  cp -p $2 $MCN/backup/$1.new && $3 $2 > $MCN/backup/$1.new && [ -s $MCN/backup/$1.new ] &&
    mv -f $MCN/backup/$1.new $MCN/backup/$1
  rc=$?
  rm -f $MCN/backup/$1.new
  return $rc
}

# The three stock copies, complete, to directory $1 on the card.
mcn_backup_card() {
  mkdir -p $1 || return 1
  for f in runHMI.sh bundles.properties cpu.conf; do
    [ -s $MCN/backup/$f ] && cp -p $MCN/backup/$f $1/ && cmp -s $MCN/backup/$f $1/$f || return 1
  done
  return 0
}

# Copy $1 to $2.new and check it byte for byte; mcn_put_commit renames it into place later,
# so a set of files changes together. Running services keep the old file mapped.
mcn_put() {
  cp $1 $2.new && cmp -s $1 $2.new && chmod $3 $2.new
}

mcn_put_commit() {
  mv -f $1.new $1
}

# Our marked block removed from a file (the stock content around it is untouched), to stdout.
# The markers start their lines; the block's own guard quotes them inside a command.
mcn_strip_block() {
  sed '/^# >>> mst2-cluster-nav/,/^# <<< mst2-cluster-nav/d' "$1"
}

# bundles.properties ($1, default $BUNDLES) without our block and our id in the Lateload list,
# wherever it stands there (first, middle or last).
mcn_bundles_remove() {
  mcn_strip_block "${1:-$BUNDLES}" | sed \
    -e 's/^\(Bundle\.Startup\.Lateload = \)'$BUNDLE_ID',/\1/' \
    -e 's/^\(Bundle\.Startup\.Lateload = .*\),'$BUNDLE_ID',/\1,/' \
    -e 's/^\(Bundle\.Startup\.Lateload = .*\),'$BUNDLE_ID'$/\1/'
}

# The three files with the mod in, built from their content without it, so installing again
# (an update) replaces our parts instead of adding them twice. All write to stdout.
mcn_hmi_with_block() {
  mcn_strip_block $HMI_SH | sed '/^BOOTCLASSPATH="\$IFSDIR\/tsd.mibstd2.hmi.v2.jxe:/r '"$1"
}

mcn_bundles_with_block() {
  mcn_bundles_remove | sed 's/^\(Bundle\.Startup\.Lateload = .*\)$/\1,'$BUNDLE_ID'/' && cat "$1"
}

mcn_conf_with_preload() {
  mcn_unpreload_conf > $MCN/cpu.conf.mcn && mcn_preload_conf "$1" $MCN/cpu.conf.mcn
  rc=$?
  rm -f $MCN/cpu.conf.mcn
  return $rc
}

# Replace file $1 by the output of the command that follows ($2 strips the mod's parts from a
# file given as its argument). The new content is written in full next to it, with the old
# file's mode, and renamed over it only when, with the mod's parts stripped, it is byte for byte
# the old file stripped the same way: nothing of the stock content may be lost or changed. A power
# cut leaves either the old or the new file, never a half-written one.
mcn_rewrite() {
  f=$1
  strip=$2
  shift 2
  cp -p "$f" "$f.mcn" && "$@" > "$f.mcn" && [ -s "$f.mcn" ] &&
    $strip "$f" > $MCN_TMP/mcn-old && $strip "$f.mcn" > $MCN_TMP/mcn-new &&
    cmp -s $MCN_TMP/mcn-old $MCN_TMP/mcn-new && mv -f "$f.mcn" "$f"
  rc=$?
  rm -f "$f.mcn" $MCN_TMP/mcn-old $MCN_TMP/mcn-new
  [ $rc = 0 ] || echo "$f: not changed (the new content would not keep the stock lines intact)."
  return $rc
}

# The three stock files without any part of the mod; Uninstall, and Install when it fails.
# Each is tried even if another fails.
mcn_remove_all() {
  all=0
  mcn_rewrite $HMI_SH mcn_strip_block mcn_strip_block $HMI_SH || all=1
  mcn_rewrite $BUNDLES mcn_bundles_remove mcn_bundles_remove || all=1
  mcn_rewrite $CPU_CONF mcn_unpreload_conf mcn_unpreload_conf || all=1
  return $all
}
