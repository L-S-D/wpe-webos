#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Bundle tvf-web with its Debian userland: out/tvf-web-<arch>/ and for the TV
# out/tvf-web-armhf.tgz. Run build-helper.sh first.
set -e
cd "$(dirname "$0")"
for a in ${*:-armhf amd64}; do
    nice -n 19 docker run --rm -v "$PWD":/work -w /work tvf-web-build python3 -I bundle.py "$a" "/work/out/tvf-web-$a"
done
case " ${*:-armhf} " in *" armhf "*)
    (cd out && nice -n 19 tar cf - tvf-web-armhf | nice -n 19 gzip -4 > tvf-web-armhf.tgz && du -sh tvf-web-armhf tvf-web-armhf.tgz) ;;
esac
