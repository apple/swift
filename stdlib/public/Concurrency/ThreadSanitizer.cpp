//===--- ThreadSanitizer.cpp - Thread Sanitizer support -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Thread Sanitizer support for the Swift Task runtime.
//
//===----------------------------------------------------------------------===//

#include "TaskPrivate.h"

// Thread Sanitizer is not supported on Windows.
#if defined(_WIN32) || defined(__wasi__)
void swift::_swift_tsan_acquire(void *addr) {}
void swift::_swift_tsan_release(void *addr) {}
#else
#include <dlfcn.h>

namespace {
using TSanFunc = void(void *);
TSanFunc *tsan_acquire, *tsan_release;
} // anonymous namespace

void swift::_swift_tsan_acquire(void *addr) {
  if (tsan_acquire) {
    tsan_acquire(addr);
  }
}

void swift::_swift_tsan_release(void *addr) {
  if (tsan_release) {
    tsan_release(addr);
  }
}

SWIFT_EXPORT_FROM(swift_Concurrency) SWIFT_CC(c)
void __tsan_on_initialize() {
  tsan_acquire = (TSanFunc *)dlsym(RTLD_DEFAULT, "__tsan_acquire");
  tsan_release = (TSanFunc *)dlsym(RTLD_DEFAULT, "__tsan_release");
}
#endif
