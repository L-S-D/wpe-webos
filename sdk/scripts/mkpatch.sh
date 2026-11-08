#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# mkpatch.sh NAME FILE...: unified diff of patchwork/<FILE> against the
# pristine source tree src/wpewebkit-<ver>/<FILE> -> patches/webkit/NAME.patch
# (files missing there are new files)
set -e
cd "$(dirname "$0")/.."
V=$(awk '$1 == "wpewebkit" { print $2 }' sources.lock)
name=$1; shift
mkdir -p patches/webkit
out=patches/webkit/$name.patch
: > "$out"
for f in "$@"; do
    a="src/wpewebkit-$V/$f"
    [ -e "$a" ] || a=/dev/null # new file
    diff -u --label "a/$f" --label "b/$f" "$a" "patchwork/$f" >> "$out" || [ $? -eq 1 ]
done
echo "$out: $(grep -c '^@@' "$out") Abschnitte"
