//===--- Error.cpp - Error handling support code --------------------------===//
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

#include "Debug.h"
#include "Error.h"
#include <cstdarg>
#include <cstdio>

// swift::fatalError is not exported from libswiftCore and not shared, so define another
// internal function instead.
SWIFT_NORETURN void swift::swift_Concurrency_fatalError(uint32_t flags, const char *format, ...) {
  swift_reportError(0, format);
  abort();
}
