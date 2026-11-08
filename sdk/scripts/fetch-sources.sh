#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Download all pinned sources into src/ and verify them against sources.lock.
# A "-" checksum is filled in on first download (trust on first use) - the
# lock file then pins it for every later run.
set -e
cd "$(dirname "$0")/.."
mkdir -p src
tmp=$(mktemp)
while read -r name ver url sum; do
    case "$name" in ''|'#'*) echo "$name $ver $url $sum" >> "$tmp"; continue ;; esac
    f="src/$name-$ver.$(echo "$url" | sed -E 's/.*\.(tar\.(xz|gz|bz2))$/\1/')"
    [ -f "$f" ] || nice -n 19 curl -sfL -o "$f" "$url"
    have=$(sha256sum "$f" | cut -d' ' -f1)
    if [ "$sum" = "-" ]; then sum=$have; echo "gepinnt: $name $ver $sum"
    elif [ "$sum" != "$have" ]; then echo "PRUEFSUMME FALSCH: $f" >&2; exit 1; fi
    echo "$name $ver $url $sum" >> "$tmp"
done < sources.lock
mv "$tmp" sources.lock
echo "alle Quellen geprueft"
