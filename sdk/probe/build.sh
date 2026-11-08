# SPDX-License-Identifier: GPL-3.0-or-later
set -e
SDK=/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot
CXX=$SDK/bin/arm-webos-linux-gnueabi-g++
cd /work/probe
F="-O2 -mthumb -march=armv7-a -mfpu=neon-fp16 -mfloat-abi=softfp -static-libstdc++ -static-libgcc -fno-exceptions"
$CXX $F -Wall pcstest.cpp -o pcstest-vfp 2>&1 | head -20
$CXX $F -Wall -DNO_PCS pcstest.cpp -o pcstest-base 2>&1 | head -20
$CXX -dumpversion; $CXX -Q --help=target -O2 2>/dev/null | grep -E "mfloat-abi|mfpu|march|mcpu|mthumb" | head
