# SPDX-License-Identifier: GPL-3.0-or-later
# Host tools for cross-building WPE WebKit with the webOS SDK (mounted
# read-only at /opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot).
# Nothing is installed on the host.
FROM debian:trixie
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      cmake ninja-build meson pkgconf make patch xz-utils bzip2 file \
      ruby perl python3 gperf unifdef bison flex gettext autoconf automake libtool \
      libglib2.0-dev-bin libglib2.0-bin gawk ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*
