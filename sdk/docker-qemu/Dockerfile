# SPDX-License-Identifier: GPL-3.0-or-later
# Offline smoke tests of the SDK build (ARMv7 softfp) with qemu user mode -
# no host install, no binfmt registration (qemu-arm is called explicitly).
# The SDK's own glibc 2.12 crashes under qemu, so Debian's armel glibc stands
# in for the TV's: same soft-float calling convention (EABI, ld-linux.so.3).
# Mesa (armel, software rendering) stands in for the TV's Mali EGL/GLES.
FROM debian:trixie
RUN dpkg --add-architecture armel \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends qemu-user binutils libc6:armel \
      libegl1:armel libegl-mesa0:armel libgles2:armel mesa-libgallium:armel libgl1-mesa-dri:armel \
 && rm -rf /var/lib/apt/lists/*
