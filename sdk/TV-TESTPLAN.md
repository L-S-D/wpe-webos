# TV tests of the SDK bundle

Done on 2026-11-02 (results in `README.md`, PNGs and logs in `out/host/tv2/`). To repeat - living room TV
only, non-visual, only below /media/developer/temp/tvf-web-sdk (TVF's own helper in
/media/developer/temp/tvf-web stays untouched), only when nobody watches and the user allows it:

    python3 -I -m http.server 8765 --bind <PC-IP> --directory sdk/out &
    python3 -I tools/putrecv.py 8766 sdk/out/host/tv2 <PC-IP> &
    # for the Debian comparison: mkdir -p sdk/out/deb && ln -s ../../../out/tvf-web-armhf.tgz sdk/out/deb/

    T='ares-novacom -d <tv> --run'
    $T 'mkdir -p /media/developer/temp/tvf-web-sdk && cd /media/developer/temp/tvf-web-sdk &&
        curl -s -o tvrun.sh http://<PC-IP>:8765/tvrun-sdk.sh && sh tvrun.sh fetch'
    P=/media/developer/temp/tvf-web-sdk
    $T "cd $P && TVF_WEB_GL_DEBUG=1 sh tvrun.sh run sdk diag http://<PC-IP>:8765/qemu/rich.html --settle 4000"
    $T "cd $P && sh tvrun.sh run sdk example https://example.org/ --settle 3000"
    $T "cd $P && sh tvrun.sh run sdk orfportal http://orfhbbtv.orf.at/orf/redbutton/index.html --keys 403 \
        --user-script $P/polyfill-test.js --settle 12000"
    $T "cd $P && sh tvrun.sh run sdk zdf http://hbbtv.zdf.de/zdfstart/index.php --settle 8000"
    $T "cd $P && sh tvrun.sh proto sdk proto http://<PC-IP>:8765/host/pages/keys.html"
    $T "cd $P && sh tvrun.sh jsc"
    # same with "deb" after "sh tvrun.sh fetchdeb"; JS in a page: --js "$(cat $P/bench.js)"

Each run has a MemAvailable watchdog (< 200 MB -> kill), prints first frame, key -> visible, PSS per
process and the number of libmali mappings in the WebProcess, and uploads NAME.png, NAME-max.png and
NAME.log. Look at the pixels (`-max.png` = most visible frame), not only the non-transparent count.
Afterwards: stop both servers, remove `sdk/out/deb` and the Debian copy on the TV (`rm -rf $P/deb $P/data-deb`).
