#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# One non-visual diagnostic run of the webOS-SDK bundle on the TV (blank frames on Mali).
# sh tvdiag-sdk.sh     - fetches the bundle, runs 5 variants of a local test page, uploads PNGs + logs
B=/media/developer/temp/tvf-web-sdk
SRV=${TVF_SRV:?set TVF_SRV=http://<PC-IP>:8765}
PUT=${TVF_PUT:?set TVF_PUT=http://<PC-IP>:8766}
PAGE='data:text/html,<body style="margin:0;background:%232050a0"><div style="position:absolute;left:100px;top:300px;width:400px;height:200px;background:%23e03030"></div><p style="position:absolute;left:600px;top:300px;color:%23fff;font:48px sans-serif">Hallo TVF</p></body>'
mkdir -p $B/diag $B/data && cd $B || exit 1
rm -rf tvf-web-sdk-armhf && curl -s $SRV/tvf-web-sdk-armhf.tgz | tar xzf - || { echo "Download fehlgeschlagen"; exit 1; }
rm -f diag/*
run() { # name env...
    name=$1; shift
    a0=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
    env TVF_WEB_DEBUG=1 TVF_WEB_GL_DEBUG=1 "$@" nice -n 19 $B/tvf-web-sdk-armhf/run.sh --url "$PAGE" \
        --png $B/diag/$name.png --data-dir $B/data --settle 4000 --timeout 30000 > $B/diag/$name.log 2>&1 &
    pid=$!; min=$a0
    while kill -0 $pid 2>/dev/null; do
        a=$(awk '/MemAvailable/{print $2}' /proc/meminfo); [ "$a" -lt "$min" ] && min=$a
        if [ "$a" -lt 200000 ]; then echo "WATCHDOG $a kB" >> $B/diag/$name.log; kill $pid; fi
        sleep 0.3
    done
    wait $pid 2>/dev/null; pkill -f $B/tvf-web-sdk-armhf/ 2>/dev/null
    echo "MemAvailable min $min kB" >> $B/diag/$name.log
    echo "== $name: $(grep -c '\[tvf-gl\]' $B/diag/$name.log) tvf-gl, $(grep -m1 '^PNG' $B/diag/$name.log)"
}
run skia-gpu TVF_WEB_TESTPATTERN=1
run skia-nomsaa WEBKIT_SKIA_MSAA_SAMPLE_COUNT=0
run skia-cpu TVF_WEB_CPU=1
run texmap-gpu WEBKIT_USE_SKIA_FOR_COMPOSITION=0
run texmap-cpu WEBKIT_USE_SKIA_FOR_COMPOSITION=0 TVF_WEB_CPU=1
for f in diag/*.png diag/*.log; do curl -s -T $f $PUT/diag-$(basename $f) > /dev/null; done
echo fertig
