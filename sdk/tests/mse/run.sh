#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline MSE check of the media build (patches 0009/0010, TVF sinks) under qemu, from the host:
#   sh tests/mse/make-content.sh        # once: test content + dash.js into out/mse
#   sh tests/mse/run.sh [BUNDLE]        # default out/tvf-web-sdk-media-armhf
# Serves out/mse on 127.0.0.1:8791 and plays mse.html (dash.js) in a qemu copy of the bundle
# (without libstdc++/libgcc_s, see README "Offline checks"); the stub sinks log and dump what
# arrives (TVF_SINK_DUMP). Afterwards: page and sink summary, and the dumped video stream of
# the first playback compared with the DASH source frame by frame (ffmpeg framemd5).
# Expected (2026-11-02): isTypeSupported avc1/mp4a/hev1/ec-3/ac-3 true, vp9 false; playing ...
# ended at 20 s, seek to 12 s; rectangle 100,100,640,360; 691200 of 921600 pixels not
# transparent (hole 640x360); video 500 frames bit-identical, audio ADTS decodes cleanly.
set -e
H=$(cd "$(dirname "$0")/../.." && pwd)
T=$H/tests/mse
C=$H/out/mse
B=${1:-$H/out/tvf-web-sdk-media-armhf}
Q=$H/out/tvf-web-sdk-media-qemu
[ -f "$C/dash/manifest.mpd" ] || { echo "Testinhalt fehlt: sh tests/mse/make-content.sh"; exit 1; }
[ -x "$B/bin/tvf-web" ] || { echo "kein Bundle in $B"; exit 1; }
rm -rf "$Q" && cp -a "$B" "$Q" && rm -f "$Q/lib/libstdc++.so.6" "$Q/lib/libgcc_s.so.1"
cp "$T/mse.html" "$C/"
rm -rf "$C/dump" && mkdir -p "$C/dump"
python3 -m http.server --bind 127.0.0.1 8791 --directory "$C" > /dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null' EXIT
sleep 1
docker run --rm --network host -e TVF_SINK_DUMP=/o/dump -v "$Q:/b:ro" -v "$T:/t:ro" -v "$C:/o" tvf-web-qemu sh /t/page.sh
echo "== Seite"
grep '^\[console\]' "$C/mse.log" | grep -v 'paused=true rs=4' | sed 's/^\[console\] log //' | head -40
echo "== Sinks"
grep -E '^\[tvfsink\] [a-z]+ (-> |rectangle|volume|mute|EOS|segment|FEHLER)' "$C/mse.log" || true
command -v ffmpeg > /dev/null || exit 0
echo "== Vergleich mit der Quelle"
v=$C/dump/tvf-video-1.h264
a=$C/dump/tvf-audio-1.aac
echo "Video: $(ffmpeg -v error -i "$v" -f null - 2>&1 | wc -l) Fehlerzeilen, Audio: $(ffmpeg -v error -i "$a" -f null - 2>&1 | wc -l) Fehlerzeilen"
ffmpeg -v error -i "$v" -f framemd5 - 2>/dev/null | grep -v '^#' | awk -F, '{ print $6 }' > "$C/dump/got.md5"
ffmpeg -v error -i "$C/video-dash.mp4" -f framemd5 - 2>/dev/null | grep -v '^#' | awk -F, '{ print $6 }' > "$C/dump/want.md5"
if cmp -s "$C/dump/got.md5" "$C/dump/want.md5"; then
    echo "Video: alle $(wc -l < "$C/dump/want.md5") Bilder bit-gleich, gleiche Reihenfolge"
else
    echo "Video: ABWEICHUNG ($(wc -l < "$C/dump/got.md5") Bilder empfangen, $(wc -l < "$C/dump/want.md5") in der Quelle)"
fi
