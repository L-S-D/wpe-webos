/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Force-included into every WebKit compilation (-include, scripts/build-webkit.sh):
 * the webOS SDK has glibc 2.12 and Linux 3.10 headers, the TVs run Linux 4.x/5.x
 * kernels and glibc 2.35. Only constants here - no system headers, so the feature
 * macros of the translation unit keep working, and no functions: code that needs a
 * newer system call calls syscall() with the numbers below (memfd_create in patch
 * 0003).
 */
#pragma once

#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 16
#endif
#ifndef MADV_DODUMP
#define MADV_DODUMP 17
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#if defined(__arm__)
/* ARM EABI syscall numbers newer than the SDK's 3.10 headers */
#ifndef __NR_renameat2
#define __NR_renameat2 382
#endif
#ifndef __NR_getrandom
#define __NR_getrandom 384
#endif
#ifndef __NR_memfd_create
#define __NR_memfd_create 385
#endif
#ifndef __NR_membarrier
#define __NR_membarrier 389
#endif
#ifndef __NR_statx
#define __NR_statx 397
#endif
#endif
