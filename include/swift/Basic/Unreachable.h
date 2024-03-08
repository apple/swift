//===--- Unreachable.h - Implements swift_unreachable ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines swift_unreachable, which provides the
//  functionality of llvm_unreachable without necessarily depending on
//  the LLVM support libraries.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_UNREACHABLE_H
#define SWIFT_BASIC_UNREACHABLE_H

#ifdef SWIFT_LLVM_SUPPORT_IS_AVAILABLE

// The implementation when LLVM is available.

#include "llvm/Support/ErrorHandling.h"
#define swift_unreachable llvm_unreachable

#else

// The implementation when LLVM is not available.

#include <assert.h>
#include <stdlib.h>

#include "swift/Basic/Compiler.h"

SWIFT_ATTRIBUTE_NORETURN
inline static void swift_unreachable(const char *msg) {
  assert(false && msg);
  (void)msg;
  abort();
}

#endif

#endif
