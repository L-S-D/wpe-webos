#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The TVF sinks without WebKit, in the qemu container: MP4 -> qtdemux -> sinks, directly (avc and
# raw AAC, as MSE delivers them) and through the parsers; the converted streams go to /c/dump-*.
#   docker run --rm -v $PWD/stage-gst:/g:ro -v $PWD/sysroot:/s:ro -v $PWD/tests/mse:/t:ro \
#       -v $PWD/out/mse:/c tvf-web-qemu sh /t/gst-direct.sh
# Check on the host: ffmpeg -i out/mse/dump-direct/tvf-video-1.h264 -f framemd5 - against test.mp4.
export GST_PLUGIN_SYSTEM_PATH_1_0=/p GST_REGISTRY_1_0=/tmp/reg.bin GST_REGISTRY_FORK=no
mkdir -p /p && cp /g/usr/lib/gstreamer-1.0/*.so /p/
for p in coreelements typefindfunctions playback; do cp /s/usr/lib/gstreamer-1.0/libgst$p.so /p/; done
L=/g/usr/lib:/s/usr/lib
run() { # name pipeline...
    n=$1; shift
    rm -rf "/c/dump-$n" && mkdir -p "/c/dump-$n"
    echo "== $n"
    # shellcheck disable=SC2068
    qemu-arm -cpu cortex-a15 -E LD_LIBRARY_PATH=$L -E TVF_SINK_DUMP="/c/dump-$n" /s/usr/bin/gst-launch-1.0 -q $@ 2>&1 |
        grep -E '\] [a-z]+ (caps|-> |EOS|FEHLER)' | cut -c1-160
}
run direct filesrc location=/c/test.mp4 ! qtdemux name=d d.video_0 ! queue ! tvfvideosink sync=false \
    d.audio_0 ! queue ! tvfaudiosink sync=false
run parsed filesrc location=/c/test.mp4 ! qtdemux name=d d.video_0 ! queue ! h264parse ! tvfvideosink sync=false \
    d.audio_0 ! queue ! aacparse ! tvfaudiosink sync=false
