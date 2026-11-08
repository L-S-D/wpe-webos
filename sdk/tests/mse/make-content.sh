#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Test content for tests/mse (host tools: ffmpeg, curl, sha256sum) into out/mse:
# 20 s H.264 High 720p25 (B-frames, GOP 2 s) + AAC-LC 48 kHz stereo as MP4 and as DASH
# (2 s segments, avc1.64001f / mp4a.40.2), the video track's segments as one fMP4 (reference
# for the frame comparison) and dash.js 4.7.4.
#   sh tests/mse/make-content.sh [DIR]
set -e
T=$(cd "$(dirname "$0")" && pwd)
D=${1:-$T/../../out/mse}
mkdir -p "$D/dash"
cd "$D"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=25 \
    -f lavfi -i sine=frequency=1000:sample_rate=48000 -t 20 \
    -c:v libx264 -profile:v high -preset veryfast -g 50 -keyint_min 50 -sc_threshold 0 -b:v 2M \
    -c:a aac -b:a 128k -ac 2 -movflags +faststart test.mp4
(cd dash && ffmpeg -hide_banner -loglevel error -y -i ../test.mp4 -map 0:v -map 0:a -c copy -f dash \
    -seg_duration 2 -use_template 1 -use_timeline 0 -init_seg_name 'init-$RepresentationID$.m4s' \
    -media_seg_name 'seg-$RepresentationID$-$Number$.m4s' manifest.mpd)
# shellcheck disable=SC2046
cat dash/init-0.m4s $(ls dash/seg-0-*.m4s | sort -t- -k3 -n) > video-dash.mp4
curl -sfL -o dash.all.min.js https://cdn.dashjs.org/v4.7.4/dash.all.min.js
echo "3a1db51ed00412c16f9edb304db69ac84f1ffce8aa2d61caf4895aa9c9bdf0df  dash.all.min.js" | sha256sum -c -
cp "$T/mse.html" .
echo "Testinhalt in $(pwd)"
