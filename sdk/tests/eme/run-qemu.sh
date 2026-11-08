#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# EME under qemu, without the TV: the media bundle's WebKit plays Shaka's "Angel One"
# (Widevine, DASH, MSE with dash.js) against TVF's media server with a simulated CDM
# (TVF tools/webmedia-test: fake_cdm.c answers licenses from tests/eme/license.py,
# "decrypts" by tagging; fake_smp.c is the decoder). Checks the whole WebKit side:
# key system access, session, license request and update through libocdm, WebKit's
# decryptor tagging the samples, the sinks' crypto blocks, TVF loading for DRM.
#   in TVF:  sh tools/webmedia-test/build.sh && build/webmedia-test/webmedia-test 300 > /tmp/wm.log 2>&1 &
#   here:    sh tests/eme/run-qemu.sh [BUNDLE]
# Needs network (the content is remote) and tests/mse/make-content.sh run once (dash.js).
set -e
H=$(cd "$(dirname "$0")/../.." && pwd)
T=$H/tests/mse
C=$H/out/mse
B=${1:-$H/out/tvf-web-sdk-media-armhf}
Q=$H/out/tvf-web-sdk-media-qemu
[ -f "$C/dash.all.min.js" ] || { echo "dash.js fehlt: sh tests/mse/make-content.sh"; exit 1; }
[ -x "$B/bin/tvf-web" ] || { echo "kein Bundle in $B"; exit 1; }
grep -qx eme "$B/features" || echo "WARNUNG: Bundle ohne eme"
rm -rf "$Q" && cp -a "$B" "$Q" && rm -f "$Q/lib/libstdc++.so.6" "$Q/lib/libgcc_s.so.1"
cp "$H/tests/mse/eme.html" "$C/"
python3 -m http.server --bind 127.0.0.1 8791 --directory "$C" > /dev/null 2>&1 &
srv=$!
# Angel One: video and audio key ids
python3 -I "$H/tests/eme/license.py" 8792 67b30c86-756f-57c5-a0a3-8a23ac8c9178,800aacaa-5229-58ae-8880-62b5695db6bf \
    > "$C/license.log" 2>&1 &
lic=$!
trap 'kill $srv $lic 2>/dev/null' EXIT
sleep 1
docker run --rm --network host -e TVF_MEDIA_SOCK=@tvf-media-test -e SETTLE=${SETTLE:-60000} \
    -e "URL=http://127.0.0.1:8791/eme.html?wv=http://127.0.0.1:8792/license" \
    -v "$Q:/b:ro" -v "$T:/t:ro" -v "$C:/o" tvf-web-qemu sh /t/page.sh
echo "== Seite"
grep '^\[console\]' "$C/mse.log" | sed 's/^\[console\] log //' | head -40
echo "== OpenCDM und Sinks"
grep -E '^\[ocdm\]|tvfsink\] [a-z]+ (verschluesselt|caps|FEHLER)' "$C/mse.log" | cut -c1-200 | head -20 || true
echo "== Lizenzserver"
cat "$C/license.log"
