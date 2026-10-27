#!/bin/sh
# Host checks of the bundle's Java logic (JDK 8 and stock API interfaces; no HMI services).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
JDK="${JDK:-/Library/Java/JavaVirtualMachines/zulu-8.jdk/Contents/Home}"
OUT="$HERE/build/check"
rm -rf "$OUT"
mkdir -p "$OUT"
"$JDK/bin/javac" -cp "${MIBHMI:?set MIBHMI to the converted MIBHMI.jar}" -source 1.3 -target 1.3 -nowarn -d "$OUT" \
    -encoding UTF-8 "$HERE"/src/mst2/clusternav/*.java \
    "$HERE"/test/*Check.java
"$JDK/bin/java" -Dfile.encoding=UTF-8 -cp "$OUT:$MIBHMI" mst2.clusternav.ManeuversCheck
"$JDK/bin/java" -Dfile.encoding=UTF-8 -cp "$OUT:$MIBHMI" mst2.clusternav.LanesCheck \
    "$HERE/../native/build/check/lanes-message.txt"
"$JDK/bin/java" -Dfile.encoding=UTF-8 -cp "$OUT:$MIBHMI" mst2.clusternav.NowPlayingCheck
"$JDK/bin/java" -Dfile.encoding=UTF-8 -cp "$OUT:$MIBHMI" mst2.clusternav.CoverCheck
