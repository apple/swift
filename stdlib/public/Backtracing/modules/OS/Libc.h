//===--- Libc.h - Imports from the C library --------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Imported functions from the C library.  We can't use Darwin, Glibc or
//  MSVCRT from here because that would create dependency issues.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BACKTRACING_LIBC_H
#define SWIFT_BACKTRACING_LIBC_H

#include <sys/types.h>
#include <sys/stat.h>

#if __has_include(<dlfcn.h>)
#include <dlfcn.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef _WIN32
#include "swift/Runtime/Win32.h"

#include <io.h>
#include <stdarg.h>

// Work around the fact that MSVCRT renamed all the POSIX functions and
// doesn't actually implement all of them anyway.
#ifdef __cplusplus
extern "C" {
#endif

typedef __int64 off_t;
typedef int ssize_t;

#define O_APPEND _O_APPEND
#define O_BINARY _O_BINARY
#define O_CREAT _O_CREAT
#define O_EXCL _O_EXCL
#define O_RDONLY _O_RDONLY
#define O_RDWR _O_RDWR
#define O_TEXT _O_TEXT
#define O_TRUNC _O_TRUNC
#define O_WRONLY _O_WRONLY

static inline int open(const char *filename, int oflag, ...) {
  wchar_t *wide = _swift_win32_copyWideFromUTF8(path);
  int pmode = 0;
  if (oflag & O_CREAT) {
    va_list val;
    va_start(val, oflag);
    pmode = va_arg(val, int);
    va_end(val);
  }
  int fd = _wopen(wpath, oflag, pmode);
  free(wide);
  return fd;
}

static inline int close(int fd) {
  return _close(fd);
}

static inline off_t lseek(int fd, off_t offset, int whence) {
  return _lseeki64(fd, offset, whence);
}

static inline ssize_t read(int fd, void *buf, size_t nbyte) {
  return _read(fd, buf, nbyte);
}

static inline ssize_t write(int fd, void *buf, size_t nbyte) {
  return _write(fd, buf, nbyte);
}

ssize_t pread(int fd, void *buf, size_t nbyte, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t nbyte, off_t offset);

#ifdef __cplusplus
}
#endif

#else
#include <unistd.h>
#endif

// .. Swift affordances ........................................................

#ifdef __cplusplus
extern "C" {
#endif

/* open() is usually declared as a variadic function; these don't import into
   Swift. */
static inline int _swift_open(const char *filename, int oflag, int mode) {
  return open(filename, oflag, mode);
}

/* errno is typically not going to be easily accessible (it's often a macro),
   so add a get_errno() function to do that. */
static inline int _swift_get_errno() { return errno; }
static void _swift_set_errno(int err) { errno = err; }

#ifdef __cplusplus
}
#endif

#endif // SWIFT_BACKTRACING_LIBC_H
