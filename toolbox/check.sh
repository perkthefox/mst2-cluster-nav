#!/bin/sh
# Host check of the runHMI.sh guard, the installer and the uninstaller, each run as packaged with
# the unit's paths mapped into a temporary directory. Synthetic runHMI.sh, bundles.properties and
# cpu.conf stand in for the stock ones.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
SCRIPTS="$HERE/custom/greenmenu/scripts"
PKG="$HERE/custom/mst2-cluster-nav"
BLOCK="$PKG/runHMI.block"
STRIP="$(sed -n "s/^  sed '\(.*\)' \"\$1\"$/\1/p" "$SCRIPTS/mcn_common.sh")" # mcn_strip_block's
printf '%s\n' '#!/bin/ksh' 'VERSION=H29.319.120' \
    'BOOTCLASSPATH="$IFSDIR/tsd.mibstd2.hmi.v2.jxe:$J9LIB/x.zip"' 'echo run' > "$T/stock.sh"
sed '/^BOOTCLASSPATH="\$IFSDIR\/tsd.mibstd2.hmi.v2.jxe:/r '"$BLOCK" "$T/stock.sh" > "$T/mod.sh"
sed 's#^\(BOOTCLASSPATH=.*\)$#\1\
BOOTCLASSPATH="$BOOTCLASSPATH -Xbootclasspath/p:$MIBJAR/NavActiveIgnore.jar"#' "$T/mod.sh" \
    > "$T/navignore.sh"
sed 's#^\(BOOTCLASSPATH=.*\)$#\1\
\# BOOTCLASSPATH="$BOOTCLASSPATH -Xbootclasspath/p:$MIBJAR/NavActiveIgnore.jar"#' "$T/mod.sh" \
    > "$T/navignore-comment.sh"
printf '%s\n' 'Bundle.Name.61 = A' 'Bundle.Startup.Lateload = 61,62,75' 'Bundle.Name.62 = B' \
    > "$T/stock.properties"
printf '%s\n' 'service iMX6.DispMg' '   envar LD_PRELOAD /lib/libmmappriofix.so.1' \
    '   command /tsd/bin/dm' '' 'service iMX6.SAL' '   command /tsd/bin/sal' '' \
    'service iMX6.Other' '   envar LD_PRELOAD /lib/a.so:/lib/b.so' > "$T/stock.conf"
# A file whose first 20 bytes are those of an ARM shared object, then $2.
elf() {
    printf '\177ELF\001\001\001\000\000\000\000\000\000\000\000\000\003\000\050\000%s' "$2" > "$1"
}
fail=0
bad() {
    echo "$1"
    fail=1
}

# The unit's paths, mapped into $W (a script or the guard block, on stdin).
map() {
    sed -e "
        s#/tsd/etc/persistence/esd/scripts#$W/scripts#g
        s#/tsd/var/mst2-cluster-nav#$W/var#g; s#/dev/shmem#$W/shm#g
        s#/media/mp00?#$W/media/mp00?#g; s#/tsd/etc/system/cpu.conf#$W/cpu.conf#g
        s#/tsd/hmi/runHMI.sh#$W/runHMI.sh#g; s#/tsd/\[a-z\]\*/mst2-cluster-nav#$W/lib#g
        s#/tsd/lib/mst2-cluster-nav#$W/lib#g; s#/tsd/hmi/HMI/jar#$W/jar#g
        s#/tsd/hmi/HMI/config/bundles.properties#$W/bundles.properties#g
        s#/tsd/bin/sal/tsd.mibstd2.sal.main#$W/sal.main#g
        s#/usr/bin/sleep#sleep_stub#g; s#/usr/bin/##g; s#/bin/mount#mount_stub#g; s#/bin/date -t#echo \$TNOW#g
        s#/bin/rm #rm #g; s#/bin/cp #cp #g; s#/bin/mv #mv #g; s#/bin/sync#sync#g"
}
mount_stub() {
    [ "$MOUNT_OK" = y ]
}
# The guard's watcher sleeps. UP=y: this boot stays up (the watcher runs its course); UP=n: the
# unit goes down at its first sleep. KILL_LATER=<dir>: a kill-switch card is mounted then.
sleep_stub() {
    [ -z "$KILL_LATER" ] || touch "$KILL_LATER/mst2-cluster-nav-disable"
    [ "$UP" = y ] || exit 0
}

# guard <runHMI.sh> <setup> <expect loaded y|n> <expect reason> <name>
guard() {
    W="$T/w"
    rm -rf "$W"
    mkdir -p "$W/var" "$W/shm" "$W/media/mp000" "$W/lib" "$W/jar"
    echo x > "$W/var/mst2-cluster-nav.jar"
    elf "$W/lib/libmcn_sal.so" sal
    printf '%s\n' 'service iMX6.SAL' "   envar LD_PRELOAD /lib/a.so:$W/lib/libmcn_sal.so" \
        'service iMX6.DispMg' "   envar LD_PRELOAD $W/lib/libmcn_dm.so" > "$W/cpu.conf"
    elf "$W/lib/libmcn_dm.so" dm
    cp "$1" "$W/runHMI.sh"
    MOUNT_OK=n
    TNOW=100000
    UP=y
    KILL_LATER=
    cd "$W"
    eval "$2"
    cd "$HERE"
    sed -n '/^# >>> mst2-cluster-nav/,/^# <<< mst2-cluster-nav/p' "$W/runHMI.sh" | map > "$W/block.sh"
    # As in runHMI.sh: not under set -e, and no failure may end it.
    (set +eu; BOOTCLASSPATH=stock; . "$W/block.sh" > /dev/null 2>&1; echo "$BOOTCLASSPATH" > "$W/bcp"
     wait)
    loaded=n
    [ "$(cat "$W/bcp")" != stock ] && loaded=y
    reason="$(cat "$W/var/off-reason" 2>/dev/null || true)"
    if [ "$loaded" != "$3" ] || [ "$reason" != "$4" ]; then
        bad "guard FAILED ($5): loaded=$loaded reason=$reason"
    fi
}
guard "$T/mod.sh" 'touch var/enabled' y '' 'enabled, the block inside runHMI.sh'
[ "$(cat "$W/var/hmi-busy")" = 100000 ] || bad "guard FAILED: no HMI crash mark"
guard "$T/mod.sh" 'touch var/trial' y '' 'trial boot'
guard "$T/mod.sh" 'touch var/trial shm/mst2-cluster-nav-armed' n '' 'trial armed on this boot'
guard "$T/mod.sh" 'touch var/enabled media/mp000/mst2-cluster-nav-disable' n \
    "kill switch on $T/w/media/mp000: could not be removed from cpu.conf" 'kill switch, read-only'
guard "$T/mod.sh" 'touch var/enabled media/mp000/mst2-cluster-nav-disable; MOUNT_OK=y' n \
    "kill switch on $T/w/media/mp000: removed from cpu.conf" 'kill switch'
guard "$T/mod.sh" 'touch var/enabled shm/mst2-cluster-nav-on; echo 99990 > var/hmi-busy' n \
    'the HMI restarted before the mod settled' 'HMI restart before settling'
[ ! -e "$W/var/hmi-strikes" ] && [ ! -e "$W/var/hmi-busy" ] || bad "guard FAILED: restart counted twice"
guard "$T/navignore.sh" 'touch var/enabled jar/NavActiveIgnore.jar' n \
    'the toolbox dual navigation patch is active' 'dual navigation added later'
guard "$T/navignore.sh" 'touch var/enabled' y '' 'dual navigation line left without its jar'
guard "$T/navignore-comment.sh" 'touch var/enabled jar/NavActiveIgnore.jar' y '' \
    'dual navigation line commented out'

# HMI crash mark: a young one counts, three in a row turn the mod off; an old one (switched off
# before settling) or a damaged one does not; a settled start (no mark) clears the count. Every
# count and the switch-off reach the history that survives the reset.
guard "$T/mod.sh" 'touch var/enabled; echo 99950 > var/hmi-busy' y '' 'first unsettled start'
[ "$(cat "$W/var/hmi-strikes")" = 1 ] || bad "guard FAILED: no strike"
grep -q '^100000 hmi: start 1 in a row 50 s after one that did not settle$' "$W/var/history" ||
    bad "guard FAILED: strike not in the history"
guard "$T/mod.sh" 'touch var/enabled; echo 99950 > var/hmi-busy; echo 1 > var/hmi-strikes' y '' \
    'second unsettled start'
[ "$(cat "$W/var/hmi-strikes")" = 2 ] && [ -e "$W/var/enabled" ] || bad "guard FAILED: off at 2"
guard "$T/mod.sh" 'touch var/enabled; echo 99950 > var/hmi-busy; echo 2 > var/hmi-strikes' n \
    'the HMI did not settle 3 times in a row' 'third unsettled start'
[ ! -e "$W/var/enabled" ] && [ ! -e "$W/var/hmi-busy" ] || bad "guard FAILED: still enabled"
grep -q '^100000 hmi: the HMI did not settle 3 times in a row: off$' "$W/var/history" ||
    bad "guard FAILED: switch-off not in the history"
guard "$T/mod.sh" 'touch var/enabled; awk "BEGIN { for (i = 0; i < 61; i++) print i }" > var/history' \
    y '' 'long history'
[ "$(wc -l < "$W/var/history.old")" -eq 61 ] && [ ! -e "$W/var/history" ] ||
    bad "guard FAILED: history not rotated"
guard "$T/mod.sh" 'touch var/enabled; echo 1000 > var/hmi-busy; echo 1 > var/hmi-strikes' y '' \
    'switched off before settling, long ago'
[ "$(cat "$W/var/hmi-strikes")" = 1 ] || bad "guard FAILED: old mark counted"
guard "$T/mod.sh" 'touch var/enabled; echo x9 > var/hmi-busy; echo 1 > var/hmi-strikes' y '' \
    'damaged mark'
guard "$T/mod.sh" 'touch var/enabled; echo 1 > var/hmi-strikes' y '' 'settled last time'
[ ! -e "$W/var/hmi-strikes" ] || bad "guard FAILED: strikes not cleared"

# A preload library that cannot be loaded: every entry of ours leaves cpu.conf. Without a
# writable partition the mod is still off; with one, cpu.conf keeps its mode and stock bytes.
guard "$T/mod.sh" 'touch var/enabled; : > lib/libmcn_sal.so' n \
    "$T/w/lib/libmcn_sal.so missing, empty or not an ARM library: could not be removed from cpu.conf" \
    'empty preload library'
grep -q libmcn_sal "$W/cpu.conf" || bad "guard FAILED: cpu.conf changed without remount"
guard "$T/mod.sh" 'touch var/enabled; echo text > lib/libmcn_dm.so; chmod 640 cpu.conf; MOUNT_OK=y' \
    n "$T/w/lib/libmcn_dm.so missing, empty or not an ARM library: removed from cpu.conf" \
    'non-ELF preload library, partition writable'
printf '%s\n' 'service iMX6.SAL' '   envar LD_PRELOAD /lib/a.so' 'service iMX6.DispMg' |
    cmp -s - "$W/cpu.conf" || bad "guard FAILED: cpu.conf repair"
[ "$(ls -l "$W/cpu.conf" | cut -c1-10)" = -rw-r----- ] || bad "guard FAILED: cpu.conf mode lost"
[ ! -e "$W/cpu.conf.mcn" ] || bad "guard FAILED: temporary file left"

# Boot-loop breaker, whatever the mod's state while cpu.conf preloads us: a boot that stays up
# clears its mark, one that goes down leaves it; the fourth boot in a row after one that did not
# stay up takes the libraries out. An old mark (a normal short drive), an HMI restart within one
# boot, or a cpu.conf without us does not count. A kill-switch card mounted after the start is
# seen by the watcher.
STRIPPED="$(printf '%s\n' 'service iMX6.SAL' '   envar LD_PRELOAD /lib/a.so' 'service iMX6.DispMg')"
guard "$T/mod.sh" '' n '' 'boot that stays up, mod off'
[ ! -e "$W/var/boot-up" ] && grep -q libmcn_dm "$W/cpu.conf" || bad "guard FAILED: boot-up kept"
guard "$T/mod.sh" 'UP=n' n '' 'boot that goes down'
[ "$(cat "$W/var/boot-up" 2>/dev/null)" = 100000 ] || bad "guard FAILED: no boot-up mark"
guard "$T/mod.sh" 'UP=n; echo 99970 > var/boot-up' n '' 'second short boot'
[ "$(cat "$W/var/boot-loops" 2>/dev/null)" = 1 ] &&
    grep -q '^100000 hmi: boot 1 in a row after one that did not stay up 120 s$' "$W/var/history" ||
    bad "guard FAILED: short boot not counted"
guard "$T/mod.sh" 'UP=n; touch var/enabled; echo 99970 > var/boot-up; echo 1 > var/boot-loops' y \
    '' 'third short boot, mod on'
[ "$(cat "$W/var/boot-loops")" = 2 ] || bad "guard FAILED: third short boot"
guard "$T/mod.sh" 'UP=n; touch var/enabled; echo 99970 > var/boot-up; echo 2 > var/boot-loops' n \
    'the unit restarted 4 times in a row within 120 s: could not be removed from cpu.conf' \
    'fourth short boot, read-only'
guard "$T/mod.sh" 'UP=n; echo 99970 > var/boot-up; echo 2 > var/boot-loops; MOUNT_OK=y' n \
    'the unit restarted 4 times in a row within 120 s: removed from cpu.conf' 'fourth short boot'
[ "$(cat "$W/cpu.conf")" = "$STRIPPED" ] && [ ! -e "$W/var/boot-up" ] &&
    [ ! -e "$W/var/boot-loops" ] && [ -e "$W/shm/mst2-cluster-nav-off" ] ||
    bad "guard FAILED: loop not broken"
grep -q '^100000 hmi: the unit restarted 4 times in a row within 120 s: removed from cpu.conf, off$' \
    "$W/var/history" || bad "guard FAILED: loop break not in the history"
guard "$T/mod.sh" 'UP=n; echo x > var/boot-up; echo 2 > var/boot-loops; MOUNT_OK=y' n \
    'the unit restarted 4 times in a row within 120 s: removed from cpu.conf' 'unknown clock'
guard "$T/mod.sh" 'UP=n; echo 1000 > var/boot-up; echo 2 > var/boot-loops; MOUNT_OK=y' n '' \
    'last start long ago'
[ ! -e "$W/var/boot-loops" ] && grep -q libmcn_dm "$W/cpu.conf" || bad "guard FAILED: old mark counted"
guard "$T/mod.sh" \
    'UP=n; touch shm/mst2-cluster-nav-booted; echo 99970 > var/boot-up; echo 2 > var/boot-loops' \
    n '' 'HMI restart within one boot'
[ "$(cat "$W/var/boot-loops")" = 2 ] && [ "$(cat "$W/var/boot-up")" = 99970 ] ||
    bad "guard FAILED: HMI restart counted as a boot"
guard "$T/mod.sh" "UP=n; printf '%s\\n' 'service iMX6.SAL' > cpu.conf" n '' 'not preloaded'
[ ! -e "$W/var/boot-up" ] || bad "guard FAILED: counted without preload"
guard "$T/mod.sh" 'KILL_LATER=$W/media/mp000; MOUNT_OK=y' n \
    "kill switch on $T/w/media/mp000: removed from cpu.conf" 'kill-switch card mounted later'
[ "$(cat "$W/cpu.conf")" = "$STRIPPED" ] || bad "guard FAILED: late kill switch left cpu.conf"
guard "$T/mod.sh" "touch media/mp000/mst2-cluster-nav-disable; MOUNT_OK=y; printf '%s\\n' 'service iMX6.SAL' > cpu.conf; cp cpu.conf cpu.before" \
    n '' 'kill switch, nothing preloaded'
cmp -s "$W/cpu.conf" "$W/cpu.before" && ! grep -q 'kill switch' "$W/var/history" 2>/dev/null ||
    bad "guard FAILED: kill switch rewrote a cpu.conf without us"

# The unit's awk (GNU Awk 3.1.5) crashes on an input file it cannot open, and the unit resets on
# any crashed process: every awk program on the unit reads stdin ("}' < file", a pipe, $(...)).
for s in "$SCRIPTS"/*.sh "$BLOCK"; do
    operand=$(sed -e ':a' -e '/\\$/{N;s/\\\n//;ba' -e '}' "$s" | grep -n "}' *[^ <)|]" || true)
    [ -z "$operand" ] || bad "awk FAILED: ${s##*/} gives awk a file: $operand"
done

# The installer's conflict check (mcn_navignore_lines): only active lines outside our block, and
# only with the jar present.
# install <runHMI.sh> <jar present y|n> <expect conflict y|n> <name>
install() {
    rm -f "$T/NavActiveIgnore.jar"
    [ "$2" = y ] && : > "$T/NavActiveIgnore.jar"
    got=$( (. "$SCRIPTS/mcn_common.sh"
            HMI_SH="$1" NAVIGNORE_JAR="$T/NavActiveIgnore.jar"
            mcn_navignore_lines > /dev/null) && echo y || echo n)
    [ "$got" = "$3" ] || bad "install check FAILED ($4): conflict=$got"
}
install "$T/mod.sh" y n 'own block names the jar'
install "$T/navignore.sh" y y 'patch'
install "$T/navignore.sh" n n 'line left without the jar'
install "$T/navignore-comment.sh" y n 'commented line'

# Uninstall (mcn_strip_block) gives the stock file back byte for byte.
sed "$STRIP" "$T/mod.sh" | cmp -s - "$T/stock.sh" || bad "uninstall FAILED: not stock"
[ -n "$STRIP" ] || bad "check FAILED: mcn_strip_block not found"

# The unit: stock files, the toolbox card with the package, the scripts as GEM runs them.
unit() {
    W="$T/u"
    rm -rf "$W"
    mkdir -p "$W/scripts" "$W/shm" "$W/media/mp001/custom/mst2-cluster-nav" "$W/jar"
    for s in "$SCRIPTS"/*.sh; do
        map < "$s" > "$W/scripts/${s##*/}"
        chmod 755 "$W/scripts/${s##*/}"
    done
    : > "$W/scripts/util_mount.sh"
    : > "$W/scripts/util_mount_ro.sh"
    printf '#!/bin/sh\n' > "$W/sal.main"
    chmod 755 "$W/sal.main"
    cp "$T/stock.sh" "$W/runHMI.sh"
    cp "$T/stock.properties" "$W/bundles.properties"
    cp "$T/stock.conf" "$W/cpu.conf"
    chmod 640 "$W/cpu.conf"
    C="$W/media/mp001/custom/mst2-cluster-nav"
    cp "$PKG/runHMI.block" "$PKG/bundles.block" "$PKG/preload.list" "$C/"
    echo jar > "$C/mst2-cluster-nav.jar"
    for lib in sal dm ipod; do
        elf "$C/libmcn_$lib.so" "$lib payload"
    done
    # The unit's firmware files the hooks are made for, and the card's list of them.
    mkdir -p "$W/fw"
    echo displaymanager > "$W/fw/displaymanager"
    echo openmaxal > "$W/fw/libOpenMAXAL.so.1"
    (. "$W/scripts/mcn_common.sh"
     { echo "# firmware test"
       for f in "$W/fw/displaymanager" "$W/fw/libOpenMAXAL.so.1"; do echo "$f $(mcn_sum "$f")"; done
     } > "$C/firmware.list")
    manifest
}
manifest() {
    (. "$W/scripts/mcn_common.sh"
     cd "$C" && { echo "# mst2-cluster-nav test"
        for f in *; do [ "$f" = MANIFEST ] || echo "$f $(mcn_sum "$f")"; done; } > MANIFEST)
}
gem() {
    ksh "$W/scripts/$1" > "$W/out" 2>&1 && echo ok || echo failed
}
stock() {
    cmp -s "$W/runHMI.sh" "$T/stock.sh" && cmp -s "$W/bundles.properties" "$T/stock.properties" &&
        cmp -s "$W/cpu.conf" "$T/stock.conf"
}

# The check value is gzip's trailer: CRC-32 and length, little-endian (RFC 1952).
W="$T/u"
unit
printf 123456789 > "$T/crc"
[ "$( (. "$W/scripts/mcn_common.sh"; mcn_sum "$T/crc") )" = 2639f4cb09000000 ] ||
    bad "install FAILED: mcn_sum is not the gzip trailer"

unit
mkdir -p "$W/var"
echo 2 > "$W/var/boot-loops"
echo 99970 > "$W/var/boot-up"
[ "$(gem mcn_install.sh)" = ok ] || bad "install FAILED: $(cat "$W/out")"
[ ! -e "$W/var/boot-loops" ] && [ ! -e "$W/var/boot-up" ] || bad "install FAILED: boot-loop count kept"
grep -q '^# >>> mst2-cluster-nav' "$W/runHMI.sh" && [ -e "$W/var/trial" ] &&
    grep -q '^Bundle.Startup.Lateload = 61,62,75,58$' "$W/bundles.properties" &&
    grep -q "^   envar LD_PRELOAD /lib/libmmappriofix.so.1:$W/lib/libmcn_dm.so\$" "$W/cpu.conf" &&
    grep -q "^   envar LD_PRELOAD $W/lib/libmcn_sal.so\$" "$W/cpu.conf" &&
    cmp -s "$W/lib/libmcn_ipod.so" "$C/libmcn_ipod.so" ||
    bad "install FAILED: not registered"
[ "$(ls -l "$W/cpu.conf" | cut -c1-10)" = -rw-r----- ] || bad "install FAILED: cpu.conf mode lost"
cmp -s "$W/var/backup/cpu.conf" "$T/stock.conf" &&
    cmp -s "$W/media/mp001/mst2-cluster-nav-backup/runHMI.sh" "$T/stock.sh" ||
    bad "install FAILED: backups"
[ "$(gem mcn_install.sh)" = ok ] || bad "update FAILED: $(cat "$W/out")"
[ "$(grep -c libmcn_ "$W/cpu.conf")" = 2 ] && [ "$(grep -c ',58' "$W/bundles.properties")" = 1 ] ||
    bad "update FAILED: entries added twice"
[ "$(gem mcn_set_zoom1250.sh)" = ok ] && [ "$(cat "$W/var/zoom")" = 1250 ] ||
    bad "settings FAILED: zoom"
[ "$(gem mcn_set_status.sh)" = ok ] && grep -q 'zoom: 1250' "$W/out" || bad "settings FAILED: status"
[ "$(gem mcn_set_diag.sh)" = ok ] &&
    grep -q '^all as the mod was made and tested for$' "$W/media/mp001/mst2-cluster-nav-diag.txt" ||
    bad "settings FAILED: diag: $(cat "$W/out")"
[ "$(gem mcn_uninstall.sh)" = ok ] && stock && [ ! -e "$W/var" ] && [ ! -e "$W/lib" ] ||
    bad "uninstall FAILED: $(cat "$W/out")"
[ "$(ls -l "$W/cpu.conf" | cut -c1-10)" = -rw-r----- ] || bad "uninstall FAILED: cpu.conf mode lost"

# A damaged package changes nothing: wrong check value, empty file, not an ARM library.
for damage in 'printf x >> "$C/libmcn_dm.so"' ': > "$C/libmcn_sal.so"; manifest' \
    'echo text > "$C/libmcn_ipod.so"; manifest' 'rm "$C/MANIFEST"' \
    'printf "\\177ELF\\001\\001\\001%09d\\003\\000\\003\\000x86" 0 > "$C/libmcn_dm.so"; manifest' \
    'grep -v libmcn_dm "$C/MANIFEST" > "$T/m"; mv "$T/m" "$C/MANIFEST"'; do
    unit
    eval "$damage"
    [ "$(gem mcn_install.sh)" = failed ] && stock && [ ! -e "$W/lib" ] && [ ! -e "$W/var/backup" ] ||
        bad "install FAILED: damaged package accepted ($damage)"
done

# The repository's source tree (no MANIFEST, no libraries) in the first slot: Install takes the
# package from the other card; with the source tree alone it stops and says what it found.
source_tree() {
    S="$W/media/mp000/custom/mst2-cluster-nav"
    mkdir -p "$S"
    cp "$PKG/runHMI.block" "$PKG/bundles.block" "$PKG/preload.list" "$S/"
}
unit
source_tree
[ "$(gem mcn_install.sh)" = ok ] && cmp -s "$W/lib/libmcn_sal.so" "$C/libmcn_sal.so" ||
    bad "install FAILED: took the source tree over the package: $(cat "$W/out")"
unit
source_tree
rm -r "$W/media/mp001/custom"
[ "$(gem mcn_install.sh)" = failed ] && stock && grep -q "^preload.list$" "$W/out" &&
    grep -q "(not 'Source code')" "$W/out" || bad "install FAILED: source tree not explained"

# Another firmware build, or a cpu.conf another tool edited: nothing changed.
for other in 'echo other > "$W/fw/displaymanager"' 'rm "$W/fw/libOpenMAXAL.so.1"' \
    'rm "$C/firmware.list"; manifest' \
    'sed "s#^\(   envar LD_PRELOAD /lib/libmmappriofix.so.1\)\$#\1 #" "$T/stock.conf" > "$W/cpu.conf"' \
    'sed "s#\$#$(printf "\r")#" "$T/stock.conf" > "$W/cpu.conf"'; do
    unit
    eval "$other"
    cp "$W/cpu.conf" "$T/before.conf"
    [ "$(gem mcn_install.sh)" = failed ] && cmp -s "$W/cpu.conf" "$T/before.conf" &&
        cmp -s "$W/runHMI.sh" "$T/stock.sh" && [ ! -e "$W/lib" ] ||
        bad "install FAILED: went on ($other): $(cat "$W/out")"
done
unit
echo other > "$W/fw/displaymanager"
gem mcn_install.sh > /dev/null
grep -q "^$W/fw/displaymanager is another build.\$" "$W/out" &&
    grep -q "^Aborted: this unit's firmware is not the build" "$W/out" ||
    bad "install FAILED: firmware mismatch not explained"

# No backup on the card (full, write-protected): nothing changed.
unit
: > "$W/media/mp001/mst2-cluster-nav-backup"
[ "$(gem mcn_install.sh)" = failed ] && stock && [ ! -e "$W/lib" ] ||
    bad "install FAILED: went on without the card backup"

# Backups are taken again while the mod is not registered (stale after a firmware update), with
# any leftover of the mod stripped; here an older runHMI.sh came back over ours (a toolbox restore).
unit
mkdir -p "$W/var/backup"
echo old > "$W/var/backup/runHMI.sh"
(. "$W/scripts/mcn_common.sh"; mcn_conf_with_preload "$C/preload.list") > "$T/conf"
cp "$T/conf" "$W/cpu.conf"
[ "$(gem mcn_install.sh)" = ok ] && cmp -s "$W/var/backup/runHMI.sh" "$T/stock.sh" &&
    cmp -s "$W/var/backup/cpu.conf" "$T/stock.conf" || bad "install FAILED: stale backups kept"

# The toolbox's dual navigation patch, as its activate/deactivate_navignore.sh do it: Enable
# copies runHMI.sh to runHMI.sh.bak once, when there is none, and adds its line after every
# BOOTCLASSPATH= line; Disable copies runHMI.sh.bak back. A copy older than Install gets our block,
# so Disable keeps the guard; Uninstall leaves both stock.
dualnav_on() {
    grep -qF '$BOOTCLASSPATH -Xbootclasspath/p:$MIBJAR/NavActiveIgnore.jar' "$W/runHMI.sh" && return
    [ -f "$W/runHMI.sh.bak" ] || cp "$W/runHMI.sh" "$W/runHMI.sh.bak"
    awk '{ print } /^BOOTCLASSPATH=/ {
        print "BOOTCLASSPATH=\"$BOOTCLASSPATH -Xbootclasspath/p:$MIBJAR/NavActiveIgnore.jar\"" }' \
        "$W/runHMI.sh" > "$T/dualnav" && cat "$T/dualnav" > "$W/runHMI.sh"
    : > "$W/jar/NavActiveIgnore.jar"
}
dualnav_off() {
    rm -f "$W/jar/NavActiveIgnore.jar"
    [ ! -f "$W/runHMI.sh.bak" ] || cp -f "$W/runHMI.sh.bak" "$W/runHMI.sh"
}
has_block() {
    grep -q '^# >>> mst2-cluster-nav' "$1" && sed "$STRIP" "$1" | cmp -s - "$T/stock.sh"
}
unit
dualnav_on
cp "$W/runHMI.sh" "$T/dualnav-on"
[ "$(gem mcn_install.sh)" = failed ] && cmp -s "$W/runHMI.sh" "$T/dualnav-on" && [ ! -e "$W/lib" ] ||
    bad "install FAILED: went on with dual navigation"
dualnav_off
[ "$(gem mcn_install.sh)" = ok ] && has_block "$W/runHMI.sh.bak" ||
    bad "install FAILED: dual navigation copy without the block: $(cat "$W/out")"
dualnav_on
dualnav_off
has_block "$W/runHMI.sh" || bad "dual navigation FAILED: Disable dropped the block"
[ "$(gem mcn_uninstall.sh)" = ok ] && stock && cmp -s "$W/runHMI.sh.bak" "$T/stock.sh" ||
    bad "uninstall FAILED: dual navigation copy: $(cat "$W/out")"
unit
[ "$(gem mcn_install.sh)" = ok ] && [ ! -e "$W/runHMI.sh.bak" ] || bad "install FAILED: made a copy"
dualnav_on
dualnav_off
has_block "$W/runHMI.sh" && has_block "$W/runHMI.sh.bak" ||
    bad "dual navigation FAILED: enabled after Install, block lost"
cp "$T/stock.sh" "$W/runHMI.sh.bak"
[ "$(gem mcn_install.sh)" = ok ] && has_block "$W/runHMI.sh.bak" ||
    bad "update FAILED: dual navigation copy without the block"
[ "$(gem mcn_uninstall.sh)" = ok ] && stock && cmp -s "$W/runHMI.sh.bak" "$T/stock.sh" ||
    bad "uninstall FAILED: dual navigation copy: $(cat "$W/out")"
unit
printf '%s\n' '#!/bin/ksh' 'echo other' > "$W/runHMI.sh.bak"
cp "$W/runHMI.sh.bak" "$T/bak"
[ "$(gem mcn_install.sh)" = failed ] && stock && cmp -s "$W/runHMI.sh.bak" "$T/bak" &&
    grep -q "could not take the mod's block" "$W/out" ||
    bad "install FAILED: went on with a dual navigation copy it cannot fix: $(cat "$W/out")"

# An update whose second library cannot be copied keeps the old set complete.
unit
[ "$(gem mcn_install.sh)" = ok ] || bad "install FAILED: $(cat "$W/out")"
cp "$W/lib/libmcn_sal.so" "$T/old-sal"
for lib in sal dm ipod; do
    elf "$C/libmcn_$lib.so" "$lib new"
done
manifest
mkdir "$W/lib/libmcn_dm.so.new"
[ "$(gem mcn_install.sh)" = failed ] && cmp -s "$W/lib/libmcn_sal.so" "$T/old-sal" ||
    bad "update FAILED: libraries of two versions"
rm -rf "$W/lib/libmcn_dm.so.new"

# Rewrites keep every stock line: output that loses one is refused, the file stays as it was.
unit
cp "$W/bundles.properties" "$T/before"
(. "$W/scripts/mcn_common.sh"; mcn_rewrite "$W/bundles.properties" mcn_bundles_remove head -n 1 \
    "$W/bundles.properties") > /dev/null && bad "rewrite FAILED: lost line accepted"
cmp -s "$W/bundles.properties" "$T/before" || bad "rewrite FAILED: file changed"

# Our bundle id leaves the Lateload list wherever it stands, and only as a whole element.
for l in '58,61,62' '61,58,62' '61,62,58' '61,580,58' '58'; do
    printf 'Bundle.Startup.Lateload = %s\n' "$l" > "$T/b"
    printf '%s\n' "$l" > "$T/l"
    got=$( (. "$W/scripts/mcn_common.sh"; mcn_bundles_remove "$T/b") | sed 's/^.* = //')
    case "$l" in
    58,61,62 | 61,58,62 | 61,62,58) want=61,62 ;;
    61,580,58) want=61,580 ;;
    *) want=58 ;; # never empties the list
    esac
    [ "$got" = "$want" ] || bad "bundles FAILED: $l -> $got"
done

# Arming a trial clears an old reason (it would read as the trial's own failure), and the
# history keeps the install and the trial.
unit
[ "$(gem mcn_install.sh)" = ok ] || bad "trial FAILED: install $(cat "$W/out")"
echo 'the HMI did not settle 3 times in a row' > "$W/var/off-reason"
[ "$(gem mcn_set_trial.sh)" = ok ] && [ ! -e "$W/var/off-reason" ] && [ -e "$W/var/trial" ] ||
    bad "trial FAILED: old reason kept"
grep -q 'gem: installed mst2-cluster-nav test$' "$W/var/history" &&
    grep -q 'gem: trial boot armed$' "$W/var/history" || bad "trial FAILED: history"

# GEM runs a script by its path alone (no arguments, as in every Toolbox screen): each menu
# entry names one packaged script, without spaces, and every wrapper script has its entry.
for path in $(sed -n 's/^ *value *sys 1 0x0100 "\(.*\)"$/\1/p' "$HERE/custom/greenmenu/"*.esd | tr ' ' '?'); do
    case "$path" in
    *'?'*) bad "esd FAILED: arguments in $path" ;;
    /tsd/etc/persistence/esd/scripts/*)
        [ -f "$SCRIPTS/${path##*/}" ] || bad "esd FAILED: $path missing" ;;
    esac
done
for s in "$SCRIPTS"/mcn_set_*.sh; do
    grep -q "scripts/${s##*/}\"" "$HERE/custom/greenmenu/"*.esd || bad "esd FAILED: no entry for ${s##*/}"
done

[ "$fail" = 0 ] && echo "toolbox: guard, install, update, uninstall, damaged package and menu scripts: ok"
exit "$fail"
