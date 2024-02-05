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
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SWIFT_FUTEX_WAITERS FUTEX_WAITERS

static inline __swift_uint32_t _swift_stdlib_gettid() {
  return syscall(SYS_gettid);
}

static inline __swift_bool _swift_stdlib_wait(__swift_uint32_t *lock) {
  return syscall(SYS_futex, lock, FUTEX_LOCK_PI_PRIVATE,
                 /* val */ 0, // this value is ignored by this futex op
                 /* timeout */ NULL); // block indefinitely
}

static inline __swift_bool _swift_stdlib_wake(__swift_uint32_t *lock) {
  return syscall(SYS_futex, lock, FUTEX_UNLOCK_PI_PRIVATE);
}

#endif // defined(__linux__)

#endif // SWIFT_STDLIB_SYNCHRONIZATION_SHIMS_H
