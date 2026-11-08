#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Pointer and wheel (cmd pointer/scroll, PROTOCOL.md) under qemu: tvf-web's test mode
# (--keys) moves over the button, clicks it and scrolls the list one step down.
#   sh tests/pointer/run-qemu.sh [BUNDLE] [HELPER]     KEYS=... for other steps
# Expected: over 200,150, down button=0 buttons=1 detail=1, up, click 200,150 hover=true,
# wheel deltaY=80 on the list and scrollTop=80 (w1 = up: deltaY<0). Steps sent at once
# are coalesced by WebKit (one wheel event with the net delta).
set -e
H=$(cd "$(dirname "$0")/../.." && pwd)
B=${1:-$H/out/tvf-web-sdk-media-armhf}
W=${2:-$H/build/helper/tvf-web}
Q=$H/out/tvf-web-sdk-media-qemu
O=$H/out/pointer
[ -x "$B/bin/tvf-web" ] || { echo "kein Bundle in $B"; exit 1; }
rm -rf "$Q" && cp -a "$B" "$Q" && rm -f "$Q/lib/libstdc++.so.6" "$Q/lib/libgcc_s.so.1"
[ -x "$W" ] && cp "$W" "$Q/bin/tvf-web"
rm -rf "$O" && mkdir -p "$O" && cp "$H/tests/pointer/pointer.html" "$O/"
python3 -m http.server --bind 127.0.0.1 8795 --directory "$O" > /dev/null 2>&1 &
s=$!
trap 'kill $s 2>/dev/null' EXIT
sleep 1
KEYS=${KEYS:-m50:50,m200:150,c200:150,w-1:750:250}
docker run --rm --network host -e "KEYS=$KEYS" -v "$Q:/b:ro" -v "$O:/o" tvf-web-qemu sh -c '
export EGL_PLATFORM=surfaceless TVF_WEB_EXEC_PREFIX="/usr/bin/qemu-arm -cpu cortex-a15"
env TVF_WEB_EGL_DEFAULT=1 qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=/b/lib /b/bin/tvf-web \
    --url http://127.0.0.1:8795/pointer.html --data-dir /tmp/d-ptr --settle 20000 --timeout 300000 \
    --keys "$KEYS" > /o/pointer.log 2>&1
echo "rc=$?"'
grep -E '^\[console\]' "$O/pointer.log" | sed 's/^\[console\] [a-z]* //'
