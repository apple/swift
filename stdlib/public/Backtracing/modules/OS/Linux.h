//===--- Linux.h - Linux specifics ------------------------------*- C++ -*-===//
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
//  Linux specific includes and declarations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BACKTRACING_LINUX_H
#define SWIFT_BACKTRACING_LINUX_H
#ifdef __linux__

#define _GNU_SOURCE
#include <sys/uio.h>
#include <ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t process_vm_readv(pid_t pid,
                         const struct iovec *local_iov,
                         unsigned long liovcnt,
                         const struct iovec *remote_iov,
                         unsigned long riovcnt,
                         unsigned long flags);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // __linux__
#endif // SWIFT_BACKTRACING_LINUX_H

