#!/bin/sh
# Host protocol and launch-boundary checks. All generated files stay in native/build.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build/check"
mkdir -p "$OUT"
python3 "$HERE/test/route_check.py"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$HERE/include" -Dopen=mcn_test_open -Dclose=mcn_test_close \
    -Dread=mcn_test_read -Dwrite=mcn_test_write -Dunlink=mcn_test_unlink \
    -Dgetenv=mcn_test_getenv -Dlseek=mcn_test_lseek \
    -c "$HERE/src/sal/iap2.c" -o "$OUT/iap2.o"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$HERE/include" -c "$HERE/test/launch_check.c" -o "$OUT/launch.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" \
    -c "$HERE/src/common/route.c" -o "$OUT/route.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" \
    -c "$HERE/src/common/media.c" -o "$OUT/media.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" \
    -c "$HERE/src/common/transcode.c" -o "$OUT/transcode.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" \
    -c "$HERE/src/common/aa_sdr.c" -o "$OUT/aa_sdr.o"
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" \
    -c "$HERE/src/common/scale.c" -o "$OUT/scale.o"
clang "$OUT/iap2.o" "$OUT/launch.o" "$OUT/route.o" "$OUT/media.o" "$OUT/transcode.o" "$OUT/aa_sdr.o" "$OUT/scale.o" -o "$OUT/launch-check"
"$OUT/launch-check"
# The QNX pointer ABI is 32-bit. These host checks stop before dereferencing any
# firmware pointer; only the process-name gate and absent-driver path run here.
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -I"$HERE/include" \
    -Ddlopen=mcn_test_dlopen -Ddlsym=mcn_test_dlsym \
    -c "$HERE/src/ipod/ipod.c" -o "$OUT/ipod.o"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$HERE/include" -c "$HERE/test/ipod_check.c" -o "$OUT/ipod-check.o"
clang "$OUT/ipod.o" "$OUT/ipod-check.o" "$OUT/route.o" -o "$OUT/ipod-check"
"$OUT/ipod-check"
# runHMI.sh block, kill switch, enabled and trial boot of the actual common/hook.c.
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I"$HERE/include" \
    -Dopen=mcn_test_open -Dclose=mcn_test_close -Dunlink=mcn_test_unlink \
    -Dread=mcn_test_read -Dwrite=mcn_test_write -Dclock_gettime=mcn_test_clock_gettime \
    -c "$HERE/src/common/hook.c" -o "$OUT/hook.o"
clang -std=c99 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I"$HERE/include" \
    -Dclock_gettime=mcn_test_clock_gettime -c "$HERE/test/hook_check.c" -o "$OUT/hook-check.o"
clang "$OUT/hook.o" "$OUT/hook-check.o" -o "$OUT/hook-check"
"$OUT/hook-check"
# avcC and Annex-B framing of the phone's stream (sal/avc.c), malformed records included.
clang -std=c99 -Wall -Wextra -Werror -DMCN_HOST -I"$HERE/include" -I"$HERE/src/sal" \
    "$HERE/test/avc_check.c" "$HERE/src/sal/avc.c" -o "$OUT/avc-check"
"$OUT/avc-check"
