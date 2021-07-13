//===--- ThreadLocalStorage.swift -----------------------------------------===//
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

import SwiftShims

// For testing purposes, a thread-safe counter to guarantee that destructors get
// called by pthread.
#if INTERNAL_CHECKS_ENABLED
internal class _TLSAtomicInt {
  internal var value: Int
  internal init() { self.value = 0 }

  internal var valuePtr: UnsafeMutablePointer<Int> {
    return _getUnsafePointerToStoredProperties(self).assumingMemoryBound(
      to: Int.self)
  }

  internal func increment() {
    _ = _swift_stdlib_atomicFetchAddInt(
      object: valuePtr,
      operand: 1)
  }

  internal func load() -> Int {
    return _swift_stdlib_atomicLoadInt(object: valuePtr)
  }
}

internal let _destroyTLSCounter = _TLSAtomicInt()

public // @testable
func _loadDestroyTLSCounter() -> Int {
  return _destroyTLSCounter.load()
}

#endif

// Thread local storage for all of the Swift standard library
//
// @moveonly/@pointeronly: shouldn't be used as a value, only through its
// pointer. Similarly, shouldn't be created, except by
// _initializeThreadLocalStorage.
//
internal struct _ThreadLocalStorage {
  // private: Should only be called by _initializeThreadLocalStorage
  internal init() {}

  // Get the current thread's TLS pointer. On first call for a given thread,
  // creates and initializes a new one.
  internal static func getPointer()
    -> UnsafeMutablePointer<_ThreadLocalStorage>
  {
    return _swift_stdlib_threadLocalStorageGet().assumingMemoryBound(
      to: _ThreadLocalStorage.self)
  }
}

// Destructor to register with pthreads. Responsible for deallocating any memory
// owned.
@_silgen_name("_stdlib_destroyTLS")
internal func _destroyTLS(_ ptr: UnsafeMutableRawPointer?) {
  _internalInvariant(ptr != nil,
    "_destroyTLS was called, but with nil...")
  let tlsPtr = ptr!.assumingMemoryBound(to: _ThreadLocalStorage.self)
  tlsPtr.deinitialize(count: 1)
  tlsPtr.deallocate()

#if INTERNAL_CHECKS_ENABLED
  // Log the fact we've destroyed our storage
  _destroyTLSCounter.increment()
#endif
}

@_silgen_name("_stdlib_createTLS")
internal func _createThreadLocalStorage()
  -> UnsafeMutablePointer<_ThreadLocalStorage>
{
  let tlsPtr: UnsafeMutablePointer<_ThreadLocalStorage>
    = UnsafeMutablePointer<_ThreadLocalStorage>.allocate(
      capacity: 1
  )
  tlsPtr.initialize(to: _ThreadLocalStorage())

  return tlsPtr
}
