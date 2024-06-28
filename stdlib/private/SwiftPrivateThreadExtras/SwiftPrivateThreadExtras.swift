//===--- SwiftPrivateThreadExtras.swift -----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains wrappers for pthread APIs that are less painful to use
// than the C APIs.
//
//===----------------------------------------------------------------------===//

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif canImport(Android)
import Android
#elseif os(WASI)
import WASILibc
#elseif os(Windows)
import CRT
import WinSDK
#endif

/// An abstract base class to encapsulate the context necessary to invoke
/// a block from pthread_create.
internal class ThreadBlockContext {
  /// Execute the block, and return an `UnsafeMutablePointer` to memory
  /// allocated with `UnsafeMutablePointer.alloc` containing the result of the
  /// block.
  func run() -> UnsafeMutableRawPointer { fatalError("abstract") }
}

internal class ThreadBlockContextImpl<Argument, Result>: ThreadBlockContext {
  let block: (Argument) -> Result
  let arg: Argument

  init(block: @escaping (Argument) -> Result, arg: Argument) {
    self.block = block
    self.arg = arg
    super.init()
  }

  override func run() -> UnsafeMutableRawPointer {
    let result = UnsafeMutablePointer<Result>.allocate(capacity: 1)
    result.initialize(to: block(arg))
    return UnsafeMutableRawPointer(result)
  }
}

/// Entry point for `pthread_create` that invokes a block context.
internal func invokeBlockContext(
  _ contextAsVoidPointer: UnsafeMutableRawPointer?
) -> UnsafeMutableRawPointer! {
  // The context is passed in +1; we're responsible for releasing it.
  let context = Unmanaged<ThreadBlockContext>
    .fromOpaque(contextAsVoidPointer!)
    .takeRetainedValue()

  return context.run()
}

#if os(Windows)
public typealias ThreadHandle = HANDLE
#else
public typealias ThreadHandle = pthread_t

#if (os(Linux) && !canImport(Musl)) || os(Android)
internal func _make_pthread_t() -> pthread_t {
  return pthread_t()
}
#else
internal func _make_pthread_t() -> pthread_t? {
  return nil
}
#endif
#endif

/// Block-based wrapper for `pthread_create`.
public func _stdlib_thread_create_block<Argument, Result>(
  _ start_routine: @escaping (Argument) -> Result,
  _ arg: Argument
) -> (CInt, ThreadHandle?) {
  let context = ThreadBlockContextImpl(block: start_routine, arg: arg)
  // We hand ownership off to `invokeBlockContext` through its void context
  // argument.
  let contextAsVoidPointer = Unmanaged.passRetained(context).toOpaque()

#if os(Windows)
  let threadID =
      _beginthreadex(nil, 0, { invokeBlockContext($0)!
                                  .assumingMemoryBound(to: UInt32.self).pointee },
                     contextAsVoidPointer, 0, nil)
  if threadID == 0 {
    return (errno, nil)
  } else {
    return (0, ThreadHandle(bitPattern: threadID))
  }
#elseif os(WASI)
  // WASI environment is single-threaded
  return (0, nil)
#else
  var threadID = _make_pthread_t()
  let result = pthread_create(&threadID, nil,
    { invokeBlockContext($0) }, contextAsVoidPointer)
  if result == 0 {
    return (result, threadID)
  } else {
    return (result, nil)
  }
#endif
}

/// Block-based wrapper for `pthread_join`.
public func _stdlib_thread_join<Result>(
  _ thread: ThreadHandle,
  _ resultType: Result.Type
) -> (CInt, Result?) {
#if os(Windows)
  let result = WaitForSingleObject(thread, INFINITE)
  guard result == WAIT_OBJECT_0 else { return (CInt(result), nil) }

  var dwResult: DWORD = 0
  GetExitCodeThread(thread, &dwResult)
  CloseHandle(thread)

  let value: Result = withUnsafePointer(to: &dwResult) {
    $0.withMemoryRebound(to: Result.self, capacity: 1) {
      $0.pointee
    }
  }
  return (CInt(result), value)
#elseif os(WASI)
   // WASI environment has a only single thread
   return (0, nil)
#else
  var threadResultRawPtr: UnsafeMutableRawPointer?
  let result = pthread_join(thread, &threadResultRawPtr)
  if result == 0 {
    let threadResultPtr = threadResultRawPtr!.assumingMemoryBound(
      to: Result.self)
    let threadResult = threadResultPtr.pointee
    threadResultPtr.deinitialize(count: 1)
    threadResultPtr.deallocate()
    return (result, threadResult)
  } else {
    return (result, nil)
  }
#endif
}

public class _stdlib_Barrier {
  var _threadBarrier: _stdlib_thread_barrier_t

  var _threadBarrierPtr: UnsafeMutablePointer<_stdlib_thread_barrier_t> {
    return _getUnsafePointerToStoredProperties(self)
      .assumingMemoryBound(to: _stdlib_thread_barrier_t.self)
  }

  public init(threadCount: Int) {
    self._threadBarrier = _stdlib_thread_barrier_t()
    let ret = _stdlib_thread_barrier_init(
      _threadBarrierPtr, CUnsignedInt(threadCount))
    if ret != 0 {
      fatalError("_stdlib_thread_barrier_init() failed")
    }
  }

  deinit {
    _stdlib_thread_barrier_destroy(_threadBarrierPtr)
  }

  public func wait() {
    let ret = _stdlib_thread_barrier_wait(_threadBarrierPtr)
    if !(ret == 0 || ret == _stdlib_THREAD_BARRIER_SERIAL_THREAD) {
      fatalError("_stdlib_thread_barrier_wait() failed")
    }
  }
}
