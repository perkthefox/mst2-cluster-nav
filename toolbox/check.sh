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
        s#/usr/bin/##g; s#/bin/mount#mount_stub#g; s#/bin/date -t#echo \$TNOW#g
        s#/bin/rm #rm #g; s#/bin/cp #cp #g; s#/bin/mv #mv #g; s#/bin/sync#sync#g"
}
mount_stub() {
    [ "$MOUNT_OK" = y ]
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
    cd "$W"
    eval "$2"
    cd "$HERE"
    sed -n '/^# >>> mst2-cluster-nav/,/^# <<< mst2-cluster-nav/p' "$W/runHMI.sh" | map > "$W/block.sh"
    # As in runHMI.sh: not under set -e, and no failure may end it.
    (set +eu; BOOTCLASSPATH=stock; . "$W/block.sh" > /dev/null 2>&1; echo "$BOOTCLASSPATH" > "$W/bcp")
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
    "kill switch on $T/w/media/mp000" 'kill switch'
guard "$T/mod.sh" 'touch var/enabled shm/mst2-cluster-nav-on; echo 99990 > var/hmi-busy' n \
    'the HMI restarted before the mod settled' 'HMI restart before settling'
[ ! -e "$W/var/hmi-strikes" ] && [ ! -e "$W/var/hmi-busy" ] || bad "guard FAILED: restart counted twice"
guard "$T/navignore.sh" 'touch var/enabled jar/NavActiveIgnore.jar' n \
    'the toolbox dual navigation patch is active' 'dual navigation added later'
guard "$T/navignore.sh" 'touch var/enabled' y '' 'dual navigation line left without its jar'
guard "$T/navignore-comment.sh" 'touch var/enabled jar/NavActiveIgnore.jar' y '' \
    'dual navigation line commented out'

# HMI crash mark: a young one counts, two in a row turn the mod off; an old one (switched off
# before settling) or a damaged one does not; a settled start (no mark) clears the count.
guard "$T/mod.sh" 'touch var/enabled; echo 99950 > var/hmi-busy' y '' 'first unsettled start'
[ "$(cat "$W/var/hmi-strikes")" = 1 ] || bad "guard FAILED: no strike"
guard "$T/mod.sh" 'touch var/enabled; echo 99950 > var/hmi-busy; echo 1 > var/hmi-strikes' n \
    'the HMI did not settle twice' 'second unsettled start'
[ ! -e "$W/var/enabled" ] && [ ! -e "$W/var/hmi-busy" ] || bad "guard FAILED: still enabled"
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
    "$T/w/lib/libmcn_sal.so missing, empty or not an ARM library: removed from cpu.conf" \
    'empty preload library'
grep -q libmcn_sal "$W/cpu.conf" || bad "guard FAILED: cpu.conf changed without remount"
guard "$T/mod.sh" 'touch var/enabled; echo text > lib/libmcn_dm.so; chmod 640 cpu.conf; MOUNT_OK=y' \
    n "$T/w/lib/libmcn_dm.so missing, empty or not an ARM library: removed from cpu.conf" \
    'non-ELF preload library, partition writable'
printf '%s\n' 'service iMX6.SAL' '   envar LD_PRELOAD /lib/a.so' 'service iMX6.DispMg' |
    cmp -s - "$W/cpu.conf" || bad "guard FAILED: cpu.conf repair"
[ "$(ls -l "$W/cpu.conf" | cut -c1-10)" = -rw-r----- ] || bad "guard FAILED: cpu.conf mode lost"
[ ! -e "$W/cpu.conf.mcn" ] || bad "guard FAILED: temporary file left"

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
[ "$(gem mcn_install.sh)" = ok ] || bad "install FAILED: $(cat "$W/out")"
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
