//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

/// A pointer for accessing "volatile" memory, e.g. memory-mapped I/O registers.
///
/// Do not use for inter-thread synchronization. This is only meaningful for
/// low-level operations on special memory addresses performed from OS kernels,
/// embedded firmware, and similar environments.
///
/// The semantics of volatile load and volatile store operations match the LLVM
/// volatile semantics. Notably, a volatile operation cannot be added, removed,
/// or reordered with other volatile operations by the compiler. They may be
/// reordered with non-volatile operations. For details, see
/// <https://llvm.org/docs/LangRef.html#volatile-memory-accesses>.
public struct UnsafeVolatilePointer<Pointee> {
  public var _rawPointer: Builtin.RawPointer
  public init(bitPattern: UInt) {
    _rawPointer = UnsafeRawPointer(bitPattern: bitPattern)!._rawValue
  }
}

extension UnsafeVolatilePointer where Pointee == UInt8 {
  /// Perform an 8-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func load() -> Pointee { 
    UInt8(Builtin.atomicload_monotonic_volatile_Int8(_rawPointer))
  }

  /// Perform an 8-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func store(_ value: Pointee) {
    Builtin.atomicstore_monotonic_volatile_Int8(_rawPointer, value._value)
  }
}

extension UnsafeVolatilePointer where Pointee == UInt16 {
  /// Perform a 16-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func load() -> Pointee {
    UInt16(Builtin.atomicload_monotonic_volatile_Int16(_rawPointer))
  }
  
  /// Perform a 16-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func store(_ value: Pointee) {
    Builtin.atomicstore_monotonic_volatile_Int16(_rawPointer, value._value)
  }
}

extension UnsafeVolatilePointer where Pointee == UInt32 {
  /// Perform a 32-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func load() -> Pointee {
    UInt32(Builtin.atomicload_monotonic_volatile_Int32(_rawPointer))
  }
  
  /// Perform a 32-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func store(_ value: Pointee) {
    Builtin.atomicstore_monotonic_volatile_Int32(_rawPointer, value._value)
  }
}

extension UnsafeVolatilePointer where Pointee == UInt64 {
  /// Perform a 64-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func load() -> Pointee {
    UInt64(Builtin.atomicload_monotonic_volatile_Int64(_rawPointer))
  }
  
  /// Perform a 64-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func store(_ value: Pointee) {
    Builtin.atomicstore_monotonic_volatile_Int64(_rawPointer, value._value)
  }
}



extension UnsafeMutablePointer where Pointee == UInt8 {
  /// Perform a 8-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileLoad() -> Pointee {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).load()
  }

  /// Perform a 8-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileStore(_ value: Pointee) {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).store(value)
  }
}

extension UnsafeMutablePointer where Pointee == UInt16 {
  /// Perform a 16-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileLoad() -> Pointee {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).load()
  }

  /// Perform a 16-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileStore(_ value: Pointee) {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).store(value)
  }
}

extension UnsafeMutablePointer where Pointee == UInt32 {
  /// Perform a 32-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileLoad() -> Pointee {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).load()
  }

  /// Perform a 32-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileStore(_ value: Pointee) {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).store(value)
  }
}

extension UnsafeMutablePointer where Pointee == UInt64 {
  /// Perform a 64-bit volatile load operation from the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileLoad() -> Pointee {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).load()
  }

  /// Perform a 64-bit volatile store operation on the target pointer.
  ///
  /// Do not use for inter-thread synchronization.
  @_transparent
  public func volatileStore(_ value: Pointee) {
    UnsafeVolatilePointer(bitPattern: UInt(bitPattern: self)).store(value)
  }
}
