# SPDX-License-Identifier: GPL-3.0-or-later
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
$SDK/bin/arm-webos-linux-gnueabi-g++ -O2 -mthumb -fno-exceptions -S -o - /work/probe/pcstest.cpp | sed -n '/^_ZL5firstP6ParamsP5StageDv4_fS3_S3_S3_:/,/\.size/p' | head -40
