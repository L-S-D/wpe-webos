# SPDX-License-Identifier: GPL-3.0-or-later
# Build environment for tvf-web: Debian sid (WPE WebKit 2.5x with the WPE
# Platform API; trixie's 2.48 is built without it). armhf via multiarch for
# the TV, amd64 for host-side tests. Nothing gets installed on the host.
FROM debian:sid
RUN dpkg --add-architecture armhf \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabihf libc6-dev-armhf-cross binutils-arm-linux-gnueabihf \
      gcc libc6-dev pkgconf make file patchelf python3 ca-certificates \
      libwpewebkit-2.0-dev:armhf glib-networking:armhf \
      libwpewebkit-2.0-dev glib-networking \
      fonts-dejavu-core fontconfig-config \
 && rm -rf /var/lib/apt/lists/*
