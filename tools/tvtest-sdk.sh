#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# tvf-web test on the TV (non-visual), webOS-SDK bundle: render pages into PNG files.
# sh tvtest.sh fetch | run NAME URL [extra args...]
B=/media/developer/temp/tvf-web-sdk
SRV=${TVF_SRV:?set TVF_SRV=http://<PC-IP>:8765}
UA='Mozilla/5.0 (Linux; armv7l) AppleWebKit/605.1.15 (KHTML, like Gecko) HbbTV/1.5.1 (+DRM;LGE;OLED77G4;WEBOS25;;;) TVF/0.1'
case "$1" in
fetch)
    mkdir -p $B && cd $B && rm -rf tvf-web-sdk-armhf && curl -s $SRV/tvf-web-sdk-armhf.tgz | tar xzf - && du -sh tvf-web-sdk-armhf
    ;;
run)
    name=$2; url=$3; shift 3
    mkdir -p $B/png $B/data
    min=999999999
    TVF_WEB_DEBUG=1 nice -n 19 $B/tvf-web-sdk-armhf/run.sh --url "$url" --png $B/png/$name.png --data-dir $B/data \
        --user-agent "$UA" --bg 404040 "$@" > $B/png/$name.log 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        a=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
        [ "$a" -lt "$min" ] && min=$a
        if [ "$a" -lt 200000 ]; then
            echo "WATCHDOG: MemAvailable $a kB - Abbruch"
            kill $pid; pkill -f $B/tvf-web-sdk-armhf/; break
        fi
        sleep 0.3
    done
    wait $pid 2>/dev/null
    pkill -f $B/tvf-web-sdk-armhf/ 2>/dev/null
    grep -v '^$' $B/png/$name.log | grep -v 'posix_spawn ->\|execve ->' | tail -40
    echo "min. MemAvailable waehrend des Tests: $min kB"
    ;;
get)
    base64 $B/png/$2.png
    ;;
esac
