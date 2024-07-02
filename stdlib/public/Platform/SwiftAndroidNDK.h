//===--- SwiftAndroidNDK.h ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ANDROID_NDK_MODULE
#define SWIFT_ANDROID_NDK_MODULE

#include <complex.h>
#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#ifdef __cplusplus
// The Android r26 NDK contains an old libc++ modulemap that requires C++23
// for 'stdatomic', which can't be imported unless we're using C++23. Thus,
// import stdatomic from the NDK directly, bypassing the stdatomic from the libc++.
#pragma clang module import _stdatomic
#else
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <threads.h>
#include <uchar.h>
#include <wchar.h>

// C headers that are included with the compiler.
#include <float.h>
#include <iso646.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <tgmath.h>

#include <alloca.h>
#include <ar.h>
#include <cpio.h>
#include <dirent.h>
#include <dlfcn.h>
#include <err.h>
#include <error.h>
#include <execinfo.h>
#include <fcntl.h>
#include <fenv.h>
#include <fnmatch.h>
#include <fts.h>
#include <ftw.h>
#include <getopt.h>
#include <glob.h>
#include <grp.h>
#include <iconv.h>
#include <ifaddrs.h>
#include <jni.h>
#include <langinfo.h>
#include <libgen.h>
#include <link.h>
#include <mntent.h>
#include <netdb.h>
#include <nl_types.h>
#include <paths.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <pwd.h>
#include <regex.h>
#include <resolv.h>
#include <sched.h>
#include <search.h>
#include <semaphore.h>
#include <spawn.h>
#include <strings.h>
#include <syscall.h>
#include <sysexits.h>
#include <syslog.h>
#include <tar.h>
#include <termio.h>
#include <termios.h>
#include <uconfig_local.h>
#include <ucontext.h>
#include <unistd.h>
#include <utime.h>
#include <utmp.h>
#include <utmpx.h>
#include <wait.h>
#include <xlocale.h>

#include <arpa/inet.h>
#include <linux/if.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/in6.h>
#include <netinet/tcp.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/uio.h>

#include <asm-generic/mman-common.h>
#include <sys/endian.h>
#include <sys/errno.h>
#include <sys/ifunc.h>
#include <sys/ipc.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#include <android/api-level.h>
#include <android/asset_manager_jni.h>
#include <android/asset_manager.h>
#include <android/ndk-version.h>
#include <android/log.h>
#include <android/trace.h>
#include <android/versioning.h>

#endif // SWIFT_ANDROID_NDK_MODULE
