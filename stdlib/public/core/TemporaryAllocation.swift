//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

// MARK: Support functions

/// What is the byte count required for an allocation with the specified
/// capacity and stride?
///
/// - Parameters:
///   - byteCount: The number of bytes to temporarily allocate. `byteCount` must
///     not be negative.
///   - alignment: The alignment of the temporary allocation. `alignment` must
///     be a whole power of 2.
///
/// - Returns: Whether or not there is sufficient space on the stack to allocate
///   `byteCount` bytes of memory.
@_alwaysEmitIntoClient @_transparent
internal func _byteCountForTemporaryAllocation<T: ~Copyable>(
  of type: T.Type,
  capacity: Int
) -> Int {
  // PRECONDITIONS: Negatively-sized buffers obviously cannot be allocated on
  // the stack (or anywhere else.)
  //
  // NOTE: This function only makes its precondition checks for non-constant
  // inputs. If it makes them for constant inputs, it prevents the compiler from
  // emitting equivalent compile-time diagnostics because the call to
  // Builtin.stackAlloc() becomes unreachable.
  if _isComputed(capacity) {
    _precondition(capacity >= 0, "Allocation capacity must be greater than or equal to zero")
  }
  let stride = MemoryLayout<T>.stride
  let (byteCount, overflow) = capacity.multipliedReportingOverflow(by: stride)
  if _isComputed(capacity) {
    _precondition(!overflow, "Allocation byte count too large")
  }
  return byteCount
}

/// Will an allocation of the specified size fit on the stack?
///
/// - Parameters:
///   - byteCount: The number of bytes to temporarily allocate. `byteCount` must
///     not be negative.
///   - alignment: The alignment of the temporary allocation. `alignment` must
///     be a whole power of 2.
///
/// - Returns: Whether or not there is sufficient space on the stack to allocate
///   `byteCount` bytes of memory.
@_alwaysEmitIntoClient @_transparent
internal func _isStackAllocationSafe(byteCount: Int, alignment: Int) -> Bool {
#if compiler(>=5.5) && $BuiltinStackAlloc
  // PRECONDITIONS: Non-positive alignments are nonsensical, as are
  // non-power-of-two alignments.
  if _isComputed(alignment) {
    _precondition(alignment > 0, "Alignment value must be greater than zero")
    _precondition(_isPowerOf2(alignment), "Alignment value must be a power of two")
  }

  // If the alignment is larger than MaximumAlignment, the allocation is always
  // performed on the heap. There are two reasons why:
  // 1. llvm's alloca instruction can take any power-of-two alignment value, but
  //    will produce unsafe assembly when that value is very large (i.e. it
  //    risks a stack overflow.)
  // 2. For non-constant values, we have no way to know what value to pass to
  //    alloca and always pass MaximumAlignment. This may be incorrect if the
  //    caller really wants a larger allocation.
  if alignment > _minAllocationAlignment() {
    return false
  }

  // Allocations smaller than this limit are reasonable to allocate on the stack
  // without worrying about running out of space, and the compiler would emit
  // such allocations on the stack anyway when they represent structures or
  // stack-promoted objects.
  if _fastPath(byteCount <= 1024) {
    return true
  }

#if !$Embedded
  // Finally, take a slow path through the standard library to see if the
  // current environment can accept a larger stack allocation.
  guard #available(macOS 12.3, iOS 15.4, watchOS 8.5, tvOS 15.4, *) //SwiftStdlib 5.6
  else {
    return false
  }
  return swift_stdlib_isStackAllocationSafe(byteCount, alignment)
#else
  return false
#endif

#else
  fatalError("unsupported compiler")
#endif
}

/// Provides scoped access to a raw buffer pointer with the specified byte count
/// and alignment.
///
/// - Parameters:
///   - type: The type of the elements in the buffer being temporarily
///     allocated. For untyped buffers, use `Int8.self`.
///   - stride: The element stride. `stride` must not be negative.
///   - alignment: The alignment of the new, temporary region of allocated
///     memory, in bytes. `alignment` must be a whole power of 2.
///   - body: A closure to invoke and to which the allocated buffer pointer
///     should be passed.
///
/// - Returns: Whatever is returned by `body`.
///
/// - Throws: Whatever is thrown by `body`.
///
/// This function encapsulates the various calls to builtins required by
/// `withUnsafeTemporaryAllocation()`.
@_alwaysEmitIntoClient @_transparent
internal func _withUnsafeTemporaryAllocation<
  T: ~Copyable, R: ~Copyable
>(
  of type: T.Type,
  capacity: Int,
  alignment: Int,
  _ body: (Builtin.RawPointer) throws -> R
) rethrows -> R {
  // How many bytes do we need to allocate?
  let byteCount = _byteCountForTemporaryAllocation(of: type, capacity: capacity)

  guard _isStackAllocationSafe(byteCount: byteCount, alignment: alignment) else {
    return try _fallBackToHeapAllocation(byteCount: byteCount, alignment: alignment, body)
  }

  // This declaration must come BEFORE Builtin.stackAlloc() or
  // Builtin.stackDealloc() will end up blowing it away (and the verifier will
  // notice and complain.)
  let result: R
  
#if compiler(>=5.5) && $BuiltinStackAlloc
  let stackAddress = Builtin.stackAlloc(
    capacity._builtinWordValue,
    MemoryLayout<T>.stride._builtinWordValue,
    alignment._builtinWordValue
  )
  
  // The multiple calls to Builtin.stackDealloc() are because defer { } produces
  // a child function at the SIL layer and that conflicts with the verifier's
  // idea of a stack allocation's lifetime.
  do {
    result = try body(stackAddress)
    Builtin.stackDealloc(stackAddress)
    return result

  } catch {
    Builtin.stackDealloc(stackAddress)
    throw error
  }
#else
  fatalError("unsupported compiler")
#endif
}

@_alwaysEmitIntoClient @_transparent
internal func _withUnprotectedUnsafeTemporaryAllocation<
  T: ~Copyable, R: ~Copyable
>(
  of type: T.Type,
  capacity: Int,
  alignment: Int,
  _ body: (Builtin.RawPointer) throws -> R
) rethrows -> R {
  // How many bytes do we need to allocate?
  let byteCount = _byteCountForTemporaryAllocation(of: type, capacity: capacity)

  guard _isStackAllocationSafe(byteCount: byteCount, alignment: alignment) else {
    return try _fallBackToHeapAllocation(byteCount: byteCount, alignment: alignment, body)
  }

  // This declaration must come BEFORE Builtin.unprotectedStackAlloc() or
  // Builtin.stackDealloc() will end up blowing it away (and the verifier will
  // notice and complain.)
  let result: R

#if $BuiltinUnprotectedStackAlloc
  let stackAddress = Builtin.unprotectedStackAlloc(
    capacity._builtinWordValue,
    MemoryLayout<T>.stride._builtinWordValue,
    alignment._builtinWordValue
  )
#else
  let stackAddress = Builtin.stackAlloc(
    capacity._builtinWordValue,
    MemoryLayout<T>.stride._builtinWordValue,
    alignment._builtinWordValue
  )
#endif

  // The multiple calls to Builtin.stackDealloc() are because defer { } produces
  // a child function at the SIL layer and that conflicts with the verifier's
  // idea of a stack allocation's lifetime.
  do {
    result = try body(stackAddress)
    Builtin.stackDealloc(stackAddress)
    return result

  } catch {
    Builtin.stackDealloc(stackAddress)
    throw error
  }
}

@_alwaysEmitIntoClient @_transparent
internal func _fallBackToHeapAllocation<R: ~Copyable>(
  byteCount: Int,
  alignment: Int,
  _ body: (Builtin.RawPointer) throws -> R
) rethrows -> R {
  let buffer = UnsafeMutableRawPointer.allocate(
    byteCount: byteCount,
    alignment: alignment
  )
  defer {
    buffer.deallocate()
  }
  return try body(buffer._rawValue)
}

// MARK: - Public interface

/// Provides scoped access to a raw buffer pointer with the specified byte count
/// and alignment.
///
/// - Parameters:
///   - byteCount: The number of bytes to temporarily allocate. `byteCount` must
///     not be negative.
///   - alignment: The alignment of the new, temporary region of allocated
///     memory, in bytes. `alignment` must be a whole power of 2.
///   - body: A closure to invoke and to which the allocated buffer pointer
///     should be passed.
///
/// - Returns: Whatever is returned by `body`.
///
/// - Throws: Whatever is thrown by `body`.
///
/// This function is useful for cheaply allocating raw storage for a brief
/// duration. Storage may be allocated on the heap or on the stack, depending on
/// the required size and alignment.
///
/// When `body` is called, the contents of the buffer pointer passed to it are
/// in an unspecified, uninitialized state. `body` is responsible for
/// initializing the buffer pointer before it is used _and_ for deinitializing
/// it before returning, but deallocation is automatic.
///
/// The implementation may allocate a larger buffer pointer than is strictly
/// necessary to contain `byteCount` bytes. The behavior of a program that
/// attempts to access any such additional storage is undefined.
///
/// The buffer pointer passed to `body` (as well as any pointers to elements in
/// the buffer) must not escape. It will be deallocated when `body` returns and
/// cannot be used afterward.
@_alwaysEmitIntoClient @_transparent
public func withUnsafeTemporaryAllocation<R: ~Copyable>(
  byteCount: Int,
  alignment: Int,
  _ body: (UnsafeMutableRawBufferPointer) throws -> R
) rethrows -> R {
  return try _withUnsafeTemporaryAllocation(
    of: Int8.self,
    capacity: byteCount,
    alignment: alignment
  ) { pointer in
    let buffer = UnsafeMutableRawBufferPointer(
      start: .init(pointer),
      count: byteCount
    )
    return try body(buffer)
  }
}

/// Provides scoped access to a raw buffer pointer with the specified byte count
/// and alignment.
///
/// This function is similar to `withUnsafeTemporaryAllocation`, except that it
/// doesn't trigger stack protection for the stack allocated memory.
@_alwaysEmitIntoClient @_transparent
public func _withUnprotectedUnsafeTemporaryAllocation<R: ~Copyable>(
  byteCount: Int,
  alignment: Int,
  _ body: (UnsafeMutableRawBufferPointer) throws -> R
) rethrows -> R {
  return try _withUnprotectedUnsafeTemporaryAllocation(
    of: Int8.self,
    capacity: byteCount,
    alignment: alignment
  ) { pointer in
    let buffer = UnsafeMutableRawBufferPointer(
      start: .init(pointer),
      count: byteCount
    )
    return try body(buffer)
  }
}

/// Provides scoped access to a buffer pointer to memory of the specified type
/// and with the specified capacity.
///
/// - Parameters:
///   - type: The type of the elements in the buffer being temporarily
///     allocated.
///   - capacity: The capacity of the buffer pointer being temporarily
///     allocated.
///   - body: A closure to invoke and to which the allocated buffer pointer
///     should be passed.
///
/// - Returns: Whatever is returned by `body`.
///
/// - Throws: Whatever is thrown by `body`.
///
/// This function is useful for cheaply allocating storage for a sequence of
/// values for a brief duration. Storage may be allocated on the heap or on the
/// stack, depending on the required size and alignment.
///
/// When `body` is called, the contents of the buffer pointer passed to it are
/// in an unspecified, uninitialized state. `body` is responsible for
/// initializing the buffer pointer before it is used _and_ for deinitializing
/// it before returning, but deallocation is automatic.
///
/// The implementation may allocate a larger buffer pointer than is strictly
/// necessary to contain `capacity` values of type `type`. The behavior of a
/// program that attempts to access any such additional storage is undefined.
///
/// The buffer pointer passed to `body` (as well as any pointers to elements in
/// the buffer) must not escape. It will be deallocated when `body` returns and
/// cannot be used afterward.
@_alwaysEmitIntoClient @_transparent
public func withUnsafeTemporaryAllocation<
  T: ~Copyable,R: ~Copyable
>(
  of type: T.Type,
  capacity: Int,
  _ body: (UnsafeMutableBufferPointer<T>) throws -> R
) rethrows -> R {
  return try _withUnsafeTemporaryAllocation(
    of: type,
    capacity: capacity,
    alignment: MemoryLayout<T>.alignment
  ) { pointer in
    Builtin.bindMemory(pointer, capacity._builtinWordValue, type)
    let buffer = UnsafeMutableBufferPointer<T>(
      start: .init(pointer),
      count: capacity
    )
    return try body(buffer)
  }
}

/// Provides scoped access to a buffer pointer to memory of the specified type
/// and with the specified capacity.
///
/// This function is similar to `withUnsafeTemporaryAllocation`, except that it
/// doesn't trigger stack protection for the stack allocated memory.
@_alwaysEmitIntoClient @_transparent
public func _withUnprotectedUnsafeTemporaryAllocation<
  T: ~Copyable, R: ~Copyable
>(
  of type: T.Type,
  capacity: Int,
  _ body: (UnsafeMutableBufferPointer<T>) throws -> R
) rethrows -> R {
  return try _withUnprotectedUnsafeTemporaryAllocation(
    of: type,
    capacity: capacity,
    alignment: MemoryLayout<T>.alignment
  ) { pointer in
    Builtin.bindMemory(pointer, capacity._builtinWordValue, type)
    let buffer = UnsafeMutableBufferPointer<T>(
      start: .init(pointer),
      count: capacity
    )
    return try body(buffer)
  }
}
