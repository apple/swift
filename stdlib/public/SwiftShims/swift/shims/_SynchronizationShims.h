//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_SYNCHRONIZATION_SHIMS_H
#define SWIFT_STDLIB_SYNCHRONIZATION_SHIMS_H

#include "SwiftStdbool.h"
#include "SwiftStdint.h"

#if defined(__linux__)
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline __swift_uint32_t _swift_stdlib_gettid() {
  static __thread tid = 0;

  if (tid == 0) {
    tid = syscall(SYS_gettid);
  }

  return tid;
}

static inline __swift_uint32_t _swift_stdlib_futex_lock(__swift_uint32_t *lock) {
  int ret = syscall(SYS_futex, lock, FUTEX_LOCK_PI_PRIVATE,
                    /* val */ 0, // this value is ignored by this futex op
                    /* timeout */ NULL); // block indefinitely

  if (ret == 0) {
    return ret;
  }

  return errno;
}

static inline __swift_uint32_t _swift_stdlib_futex_trylock(__swift_uint32_t *lock) {
  int ret = syscall(SYS_futex, lock, FUTEX_TRYLOCK_PI);

  if (ret == 0) {
    return ret;
  }

  return errno;
}

static inline __swift_uint32_t _swift_stdlib_futex_unlock(__swift_uint32_t *lock) {
  int ret = syscall(SYS_futex, lock, FUTEX_UNLOCK_PI_PRIVATE);

  if (ret == 0) {
    return ret;
  }

  return errno;
}

#endif // defined(__linux__)

#endif // SWIFT_STDLIB_SYNCHRONIZATION_SHIMS_H
