#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build image with Debian sid's WPE WebKit (armhf + amd64), cross compiler and
# Mesa. Nothing is installed on the host. Re-run to pick up newer packages.
set -e
cd "$(dirname "$0")/docker"
nice -n 19 docker build --pull -t tvf-web-build .
docker run --rm tvf-web-build dpkg-query -W -f '${Package}:${Architecture} ${Version}\n' \
    libwpewebkit-2.0-1:armhf mesa-libgallium:armhf libllvm22:armhf libc6:armhf
