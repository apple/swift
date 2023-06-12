//===--- Runtime.h - Swift runtime imports ----------------------*- C++ -*-===//
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
//  Things to drag in from the Swift runtime.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BACKTRACING_RUNTIME_H
#define SWIFT_BACKTRACING_RUNTIME_H

#include <stdbool.h>
#include <stdlib.h>

#include "swift/Runtime/CrashInfo.h"

// Can't import swift/Runtime/Debug.h because it assumes C++
void swift_reportWarning(uint32_t flags, const char *message);

// Returns true if the given function is a thunk function
bool _swift_backtrace_isThunkFunction(const char *rawName);

// Demangle the given raw name (supports Swift and C++)
char *_swift_backtrace_demangle(const char *rawName,
                                size_t rawNameLength,
                                char *outputBuffer,
                                size_t *outputBufferSize);

#endif // SWIFT_BACKTRACING_RUNTIME_H
