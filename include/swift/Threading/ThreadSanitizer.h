//===--- ThreadSanitizer.h - Thread Sanitizer support --------- -*- C++ -*-===//
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
// Helper functions for code that needs to integrate with the thread
// sanitizer.  In particular, TSan can't see inside the runtime libraries,
// so we occasionally need to give it a hint that we're doing synchronization
// in order to avoid false positives.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_THREADING_THREAD_SANITIZER_H
#define SWIFT_THREADING_THREAD_SANITIZER_H

#include "swift/shims/Visibility.h"

namespace swift {

#if SWIFT_THREADING_NONE                                                \
  || defined(_WIN32) || defined(__wasi__)                               \
  || !__has_include(<dlfcn.h>)                                          \
  || (defined(SWIFT_STDLIB_HAS_DLSYM) && !SWIFT_STDLIB_HAS_DLSYM)

#define SWIFT_THREADING_TSAN_SUPPORT 0

namespace tsan {

inline bool enabled() { return false; }
template <typename T> T *acquire(T *ptr) { return ptr; }
template <typename T> T *release(T *ptr) { return ptr; }

} // namespace tsan

#else

#define SWIFT_THREADING_TSAN_SUPPORT 1

// If we're static linking to libswiftThreading.a, these symbols can come
// from there.  If, on the other hand, we're dynamically linked, we want
// to get them from libswiftCore.dylib instead.
#if SWIFT_THREADING_STATIC
#define SWIFT_THREADING_EXPORT extern "C"
#else
#define SWIFT_THREADING_EXPORT SWIFT_RUNTIME_EXPORT
#endif

namespace threading_impl {

SWIFT_THREADING_EXPORT bool _swift_tsan_enabled;
SWIFT_THREADING_EXPORT void (*_swift_tsan_acquire)(const void *ptr);
SWIFT_THREADING_EXPORT void (*_swift_tsan_release)(const void *ptr);

} // namespace threading_impl

namespace tsan {

/// Returns true if TSan is enabled
inline bool enabled() {
  return threading_impl::_swift_tsan_enabled;
}

/// Inform TSan about a synchronization operation.
///
/// This is used when TSan cannot see the synchronization operation, for
/// example, if it is using a custom primitive for which TSan doesn't have
/// a built-in interceptor.  This does not necessarily mean a lock or a C(++)
/// atomic operation - it could be any kind of synchronization mechanism.
///
/// An acquire-release pair using the same address establishes an ordering
/// constraint in TSan's happens-before graph, which TSan uses to determine
/// whether two memory accesses from different threads have a well-defined
/// order.
///
/// For instance, in
///
///     Thread 1                                Thread 2
///
///     access to y
///     tsan::release(x)
///     lock given away
///
///                      --> sync point -->
///
///                                             lock taken
///                                             tsan::acquire(x)
///                                             access to y
///
/// the access to y from Thread 2 is safe relative to the preceding access to
/// y on Thread 1 because it is preceded by an acquire of x that was itself
/// preceded by a release of x.
template <typename T>
T *acquire(T *ptr) {
  if (threading_impl::_swift_tsan_acquire) {
    threading_impl::_swift_tsan_acquire(ptr);
  }
  return ptr;
}

/// Inform TSan about a synchronization operation.
///
/// This is the counterpart to tsan::acquire.
template <typename T>
T *release(T *ptr) {
  if (threading_impl::_swift_tsan_release) {
    threading_impl::_swift_tsan_release(ptr);
  }
  return ptr;
}

} // namespace tsan

#endif

} // namespace swift

#endif
