#!/bin/sh
# Check and build the mod and lay out the toolbox SD card: build/sd/custom/ goes to the root of
# the card next to the MIB STD2 toolbox's own custom/ directory, then GEM -> Customization ->
# Cluster navigation -> Install. The same tree, zipped with a top-level custom/, is
# build/mst2-cluster-nav-<VERSION>.zip, byte for byte the same for the same commit.
#   MIBHMI=<MIBHMI.jar converted from your unit's MIBHMI.jxe> JAVAC=<JDK 8 javac> ./package.sh
# Refuses a tree with uncommitted changes unless FORCE=1.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION="$(cat "$HERE/VERSION")"
if [ "${FORCE:-0}" != 1 ] && [ -n "$(git -C "$HERE" status --porcelain)" ]; then
    echo "The tree has uncommitted changes; commit them, or FORCE=1 for a local test package." >&2
    exit 1
fi

sh "$HERE/native/check.sh"
if [ -n "${MIBHMI:-}" ]; then
    sh "$HERE/hmi/check.sh"
else
    echo "warning: MIBHMI is not set, hmi/check.sh skipped" >&2
fi
sh "$HERE/toolbox/check.sh"
sh "$HERE/hmi/build.sh"
sh "$HERE/native/build.sh"

SD="$HERE/build/sd"
PKG="$SD/custom/mst2-cluster-nav"
rm -rf "$SD"
mkdir -p "$SD"
cp -R "$HERE/toolbox/custom" "$SD/custom"

# The jar again with fixed entry order and times (jar(1) stamps the build time).
STAMP="$(TZ=UTC git -C "$HERE" log -1 --format=%cd --date=format-local:%Y%m%d%H%M.%S 2>/dev/null ||
    echo 202601010000.00)"
J="$HERE/build/jar"
rm -rf "$J"
mkdir -p "$J"
(cd "$J" && unzip -q "$HERE/hmi/build/mst2-cluster-nav.jar" &&
    find . -exec env TZ=UTC touch -t "$STAMP" {} + &&
    { echo META-INF/MANIFEST.MF; find . -type f ! -path ./META-INF/MANIFEST.MF | sed 's#^\./##' |
        LC_ALL=C sort; } | TZ=UTC zip -q -X -D "$PKG/mst2-cluster-nav.jar" -@)
for lib in $(awk '!/^#/ && NF == 2 { print $2 }' "$PKG/preload.list"); do
    cp "$HERE/native/build/$lib" "$PKG/"
done

# What Install checks before it changes anything (mcn_verify_card): each file's gzip trailer,
# CRC-32 and length, as the unit computes it (mcn_sum).
(cd "$PKG" && {
    echo "# mst2-cluster-nav $VERSION"
    for f in $(ls | LC_ALL=C sort); do
        [ "$f" = MANIFEST ] && continue
        echo "$f $(gzip -c "$f" | tail -c 8 | od -A n -t x1 |
            awk '{ for (i = 1; i <= NF; i++) s = s $i } END { print s }')"
    done
} > MANIFEST)

find "$SD" -type d -exec chmod 755 {} +
find "$SD" -type f -exec chmod 644 {} +
find "$SD" -type f -name '*.sh' -exec chmod 755 {} +
find "$SD" -exec env TZ=UTC touch -t "$STAMP" {} +
ZIP="$HERE/build/mst2-cluster-nav-$VERSION.zip"
rm -f "$ZIP"
(cd "$SD" && find custom -type f | LC_ALL=C sort | TZ=UTC zip -q -X -D "$ZIP" -@)
echo "$ZIP"
