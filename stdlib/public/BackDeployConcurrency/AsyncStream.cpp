//===--- AsyncStream.cpp - Multi-resume locking interface -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include <new>

#include "swift/Runtime/Config.h"
#include "swift/Threading/Mutex.h"

namespace swift {
// return the size in words for the given mutex primitive
SWIFT_CC(swift)
extern "C"
size_t _swift_async_stream_lock_size() {
  size_t words = sizeof(Mutex) / sizeof(void *);
  if (words < 1) { return 1; }
  return words;
}

SWIFT_CC(swift)
extern "C" void _swift_async_stream_lock_init(Mutex &lock) {
  new (&lock) Mutex();
}

SWIFT_CC(swift)
extern "C" void _swift_async_stream_lock_lock(Mutex &lock) { lock.lock(); }

SWIFT_CC(swift)
extern "C" void _swift_async_stream_lock_unlock(Mutex &lock) { lock.unlock(); }
}
