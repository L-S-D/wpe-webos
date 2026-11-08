#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Non-visual tvf-web tests on the living room TV: webOS-SDK bundle (Mali GL) and, for the comparison,
# the Debian bundle (llvmpipe). Everything lives below /media/developer/temp/tvf-web-sdk; TVF's own
# helper in /media/developer/temp/tvf-web is not touched (other path, other uid).
#   sh tvrun.sh fetch | fetchdeb | run sdk|deb NAME URL [tvf-web args] | jsc
# Results (PNG, -max.png, log) go to the host's PUT receiver as NAME.*.
B=/media/developer/temp/tvf-web-sdk
SRV=${TVF_SRV:?set TVF_SRV=http://<PC-IP>:8765}
PUT=${TVF_PUT:?set TVF_PUT=http://<PC-IP>:8766}
UA='Mozilla/5.0 (Linux; armv7l) AppleWebKit/605.1.15 (KHTML, like Gecko) HbbTV/1.5.1 (+DRM;LGE;OLED77G4;WEBOS25;;;) TVF/0.1'
mkdir -p $B && cd $B || exit 1
put() { for f in "$@"; do [ -f "$f" ] && curl -s -T "$f" "$PUT/$(basename "$f")" > /dev/null; done; }
case "$1" in
fetch)
    rm -rf tvf-web-sdk-armhf && curl -s $SRV/tvf-web-sdk-armhf.tgz | tar xzf - || { echo "Download fehlgeschlagen"; exit 1; }
    for f in polyfill-test.js fib.js jit-check.js bench.js; do curl -s -o $f $SRV/$f; done
    du -sh tvf-web-sdk-armhf; df -k $B | tail -1
    ;;
fetchdeb)
    mkdir -p deb && cd deb && rm -rf tvf-web-armhf && curl -s $SRV/deb/tvf-web-armhf.tgz | tar xzf - && du -sh tvf-web-armhf
    ;;
run)
    case "$2" in sdk) R=$B/tvf-web-sdk-armhf ;; deb) R=$B/deb/tvf-web-armhf ;; *) echo "sdk|deb?"; exit 2 ;; esac
    bundle=$2; name=$3; url=$4; shift 4
    mkdir -p res data-$bundle
    rm -f res/$name.png res/$name-max.png res/$name.log
    a0=$(awk '/MemAvailable/{print $2}' /proc/meminfo); min=$a0
    TVF_WEB_DEBUG=1 nice -n 19 $R/run.sh --url "$url" --png $B/res/$name.png --data-dir $B/data-$bundle \
        --user-agent "$UA" --bg 404040 "$@" > res/$name.log 2>&1 &
    pid=$!; mali=0
    while kill -0 $pid 2>/dev/null; do
        a=$(awk '/MemAvailable/{print $2}' /proc/meminfo); [ "$a" -lt "$min" ] && min=$a
        if [ "$a" -lt 200000 ]; then echo "WATCHDOG: MemAvailable $a kB - Abbruch" >> res/$name.log; kill $pid; break; fi
        w=$(pgrep -f "$R/libexec/WPEWebProcess" | head -1)
        [ -n "$w" ] && [ "$mali" = 0 ] && mali=$(grep -c libmali /proc/$w/maps 2>/dev/null); mali=${mali:-0}
        sleep 0.3
    done
    wait $pid 2>/dev/null
    pkill -f "$R/" 2>/dev/null
    echo "libmali-Mappings im WebProcess: $mali; MemAvailable vorher $a0 kB, min $min kB" >> res/$name.log
    grep -v '^$' res/$name.log | grep -v 'posix_spawn ->\|execve ->\|\] Bild [0-9]* nach' | tail -${TAIL:-30}
    put res/$name.png res/$name-max.png res/$name.log
    ;;
proto)  # proto sdk|deb NAME URL: protocol client (TVF's part) with the TV's python3
    case "$2" in sdk) R=$B/tvf-web-sdk-armhf ;; deb) R=$B/deb/tvf-web-armhf ;; *) echo "sdk|deb?"; exit 2 ;; esac
    name=$3; url=$4; mkdir -p res data-$2
    [ -f proto_client.py ] || curl -s -o proto_client.py $SRV/proto_client.py
    nice -n 19 python3 proto_client.py $R/run.sh "$url" $B/res/$name.png --data-dir $B/data-$2 > res/$name.log 2>&1 &
    pid=$!; min=999999999
    while kill -0 $pid 2>/dev/null; do
        a=$(awk '/MemAvailable/{print $2}' /proc/meminfo); [ "$a" -lt "$min" ] && min=$a
        if [ "$a" -lt 200000 ]; then echo "WATCHDOG: MemAvailable $a kB - Abbruch" >> res/$name.log; kill $pid; break; fi
        sleep 0.3
    done
    wait $pid 2>/dev/null; pkill -f "$R/" 2>/dev/null
    echo "MemAvailable min $min kB" >> res/$name.log
    grep -v '^\[tvf-web\]\|^$' res/$name.log | tail -${TAIL:-30}
    put res/$name.png res/$name.log
    ;;
jsc)
    cd tvf-web-sdk-armhf || exit 1
    for f in fib.js jit-check.js; do
        echo "== $f"; LD_LIBRARY_PATH=lib nice -n 19 bin/jsc $B/$f
        echo "== $f --useJIT=false"; LD_LIBRARY_PATH=lib nice -n 19 bin/jsc --useJIT=false $B/$f
    done 2>&1
    ;;
esac
