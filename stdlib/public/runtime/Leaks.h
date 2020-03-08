//===--- Leaks.h ------------------------------------------------*- C++ -*-===//
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
// This is a very simple leak detector implementation that detects objects that
// are allocated but not deallocated in a region. It is purposefully behind a
// flag since it is not meant to be used in production yet.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_RUNTIME_LEAKS_H
#define SWIFT_STDLIB_RUNTIME_LEAKS_H

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER

#include "../SwiftShims/Visibility.h"

#include "llvm/Support/Compiler.h"
#include "swift/Runtime/Config.h"

namespace swift {
struct HeapObject;
}

SWIFT_CC SWIFT_RUNTIME_EXPORT LLVM_ATTRIBUTE_NOINLINE LLVM_ATTRIBUTE_USED
void _swift_leaks_startTrackingObjects(const char *);
SWIFT_CC SWIFT_RUNTIME_EXPORT LLVM_ATTRIBUTE_NOINLINE LLVM_ATTRIBUTE_USED
int _swift_leaks_stopTrackingObjects(const char *);
SWIFT_RUNTIME_EXPORT LLVM_ATTRIBUTE_NOINLINE LLVM_ATTRIBUTE_USED
void _swift_leaks_startTrackingObject(swift::HeapObject *);
SWIFT_RUNTIME_EXPORT LLVM_ATTRIBUTE_NOINLINE LLVM_ATTRIBUTE_USED
void _swift_leaks_stopTrackingObject(swift::HeapObject *);

#define SWIFT_LEAKS_START_TRACKING_OBJECT(obj)                                 \
  _swift_leaks_startTrackingObject(obj)
#define SWIFT_LEAKS_STOP_TRACKING_OBJECT(obj)                                  \
  _swift_leaks_stopTrackingObject(obj)

// SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
#else
// not SWIFT_RUNTIME_ENABLE_LEAK_CHECKER

#define SWIFT_LEAKS_START_TRACKING_OBJECT(obj)
#define SWIFT_LEAKS_STOP_TRACKING_OBJECT(obj)

#endif

#endif
