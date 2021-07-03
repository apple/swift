//===----------------------------------------------------------*- swift -*-===//
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
///
/// This file contains Swift wrappers for functions defined in the C++ runtime.
///
//===----------------------------------------------------------------------===//

import SwiftShims

//===----------------------------------------------------------------------===//
// Atomics
//===----------------------------------------------------------------------===//

@_transparent
public // @testable
func _stdlib_atomicCompareExchangeStrongPtr(
  object target: UnsafeMutablePointer<UnsafeRawPointer?>,
  expected: UnsafeMutablePointer<UnsafeRawPointer?>,
  desired: UnsafeRawPointer?
) -> Bool {
  // We use Builtin.Word here because Builtin.RawPointer can't be nil.
  let (oldValue, won) = Builtin.cmpxchg_seqcst_seqcst_Word(
    target._rawValue,
    UInt(bitPattern: expected.pointee)._builtinWordValue,
    UInt(bitPattern: desired)._builtinWordValue)
  expected.pointee = UnsafeRawPointer(bitPattern: Int(oldValue))
  return Bool(won)
}

/// Atomic compare and exchange of `UnsafeMutablePointer<T>` with sequentially
/// consistent memory ordering.  Precise semantics are defined in C++11 or C11.
///
/// - Warning: This operation is extremely tricky to use correctly because of
///   writeback semantics.
///
/// It is best to use it directly on an
/// `UnsafeMutablePointer<UnsafeMutablePointer<T>>` that is known to point
/// directly to the memory where the value is stored.
///
/// In a call like this:
///
///     _stdlib_atomicCompareExchangeStrongPtr(&foo.property1.property2, ...)
///
/// you need to manually make sure that:
///
/// - all properties in the chain are physical (to make sure that no writeback
///   happens; the compare-and-exchange instruction should operate on the
///   shared memory); and
///
/// - the shared memory that you are accessing is located inside a heap
///   allocation (a class instance property, a `_BridgingBuffer`, a pointer to
///   an `Array` element etc.)
///
/// If the conditions above are not met, the code will still compile, but the
/// compare-and-exchange instruction will operate on the writeback buffer, and
/// you will get a *race* while doing writeback into shared memory.
@_transparent
public // @testable
func _stdlib_atomicCompareExchangeStrongPtr<T>(
  object target: UnsafeMutablePointer<UnsafeMutablePointer<T>>,
  expected: UnsafeMutablePointer<UnsafeMutablePointer<T>>,
  desired: UnsafeMutablePointer<T>
) -> Bool {
  let rawTarget = UnsafeMutableRawPointer(target).assumingMemoryBound(
    to: Optional<UnsafeRawPointer>.self)
  let rawExpected = UnsafeMutableRawPointer(expected).assumingMemoryBound(
    to: Optional<UnsafeRawPointer>.self)
  return _stdlib_atomicCompareExchangeStrongPtr(
    object: rawTarget,
    expected: rawExpected,
    desired: UnsafeRawPointer(desired))
}

/// Atomic compare and exchange of `UnsafeMutablePointer<T>` with sequentially
/// consistent memory ordering.  Precise semantics are defined in C++11 or C11.
///
/// - Warning: This operation is extremely tricky to use correctly because of
///   writeback semantics.
///
/// It is best to use it directly on an
/// `UnsafeMutablePointer<UnsafeMutablePointer<T>>` that is known to point
/// directly to the memory where the value is stored.
///
/// In a call like this:
///
///     _stdlib_atomicCompareExchangeStrongPtr(&foo.property1.property2, ...)
///
/// you need to manually make sure that:
///
/// - all properties in the chain are physical (to make sure that no writeback
///   happens; the compare-and-exchange instruction should operate on the
///   shared memory); and
///
/// - the shared memory that you are accessing is located inside a heap
///   allocation (a class instance property, a `_BridgingBuffer`, a pointer to
///   an `Array` element etc.)
///
/// If the conditions above are not met, the code will still compile, but the
/// compare-and-exchange instruction will operate on the writeback buffer, and
/// you will get a *race* while doing writeback into shared memory.
@_transparent
public // @testable
func _stdlib_atomicCompareExchangeStrongPtr<T>(
  object target: UnsafeMutablePointer<UnsafeMutablePointer<T>?>,
  expected: UnsafeMutablePointer<UnsafeMutablePointer<T>?>,
  desired: UnsafeMutablePointer<T>?
) -> Bool {
  let rawTarget = UnsafeMutableRawPointer(target).assumingMemoryBound(
    to: Optional<UnsafeRawPointer>.self)
  let rawExpected = UnsafeMutableRawPointer(expected).assumingMemoryBound(
    to: Optional<UnsafeRawPointer>.self)
  return _stdlib_atomicCompareExchangeStrongPtr(
    object: rawTarget,
    expected: rawExpected,
    desired: UnsafeRawPointer(desired))
}

@_transparent
@discardableResult
public // @testable
func _stdlib_atomicInitializeARCRef(
  object target: UnsafeMutablePointer<AnyObject?>,
  desired: AnyObject
) -> Bool {
  var expected: UnsafeRawPointer?
  let desiredPtr = Unmanaged.passRetained(desired).toOpaque()
  let rawTarget = UnsafeMutableRawPointer(target).assumingMemoryBound(
    to: Optional<UnsafeRawPointer>.self)
  let wonRace = _stdlib_atomicCompareExchangeStrongPtr(
    object: rawTarget, expected: &expected, desired: desiredPtr)
  if !wonRace {
    // Some other thread initialized the value.  Balance the retain that we
    // performed on 'desired'.
    Unmanaged.passUnretained(desired).release()
  }
  return wonRace
}

@_transparent
public // @testable
func _stdlib_atomicLoadARCRef(
  object target: UnsafeMutablePointer<AnyObject?>
) -> AnyObject? {
  let value = Builtin.atomicload_seqcst_Word(target._rawValue)
  if let unwrapped = UnsafeRawPointer(bitPattern: Int(value)) {
    return Unmanaged<AnyObject>.fromOpaque(unwrapped).takeUnretainedValue()
  }
  return nil
}

//===----------------------------------------------------------------------===//
// Conversion of primitive types to `String`
//===----------------------------------------------------------------------===//

/// A 32 byte buffer.
internal struct _Buffer32 {
  internal init() {}

  internal var _x0: UInt8 = 0
  internal var _x1: UInt8 = 0
  internal var _x2: UInt8 = 0
  internal var _x3: UInt8 = 0
  internal var _x4: UInt8 = 0
  internal var _x5: UInt8 = 0
  internal var _x6: UInt8 = 0
  internal var _x7: UInt8 = 0
  internal var _x8: UInt8 = 0
  internal var _x9: UInt8 = 0
  internal var _x10: UInt8 = 0
  internal var _x11: UInt8 = 0
  internal var _x12: UInt8 = 0
  internal var _x13: UInt8 = 0
  internal var _x14: UInt8 = 0
  internal var _x15: UInt8 = 0
  internal var _x16: UInt8 = 0
  internal var _x17: UInt8 = 0
  internal var _x18: UInt8 = 0
  internal var _x19: UInt8 = 0
  internal var _x20: UInt8 = 0
  internal var _x21: UInt8 = 0
  internal var _x22: UInt8 = 0
  internal var _x23: UInt8 = 0
  internal var _x24: UInt8 = 0
  internal var _x25: UInt8 = 0
  internal var _x26: UInt8 = 0
  internal var _x27: UInt8 = 0
  internal var _x28: UInt8 = 0
  internal var _x29: UInt8 = 0
  internal var _x30: UInt8 = 0
  internal var _x31: UInt8 = 0

  internal mutating func withBytes<Result>(
    _ body: (UnsafeMutablePointer<UInt8>) throws -> Result
  ) rethrows -> Result {
    return try withUnsafeMutablePointer(to: &self) {
      try body(UnsafeMutableRawPointer($0).assumingMemoryBound(to: UInt8.self))
    }
  }
}

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
// Note that this takes a Float32 argument instead of Float16, because clang
// doesn't have _Float16 on all platforms yet.
@_silgen_name("swift_float16ToString")
internal func _float16ToStringImpl(
  _ buffer: UnsafeMutablePointer<UTF8.CodeUnit>,
  _ bufferLength: UInt,
  _ value: Float32,
  _ debug: Bool
) -> Int

@available(macOS 11.0, iOS 14.0, watchOS 7.0, tvOS 14.0, *)
internal func _float16ToString(
  _ value: Float16,
  debug: Bool
) -> (buffer: _Buffer32, length: Int) {
  _internalInvariant(MemoryLayout<_Buffer32>.size == 32)
  var buffer = _Buffer32()
  let length = buffer.withBytes { (bufferPtr) in
    _float16ToStringImpl(bufferPtr, 32, Float(value), debug)
  }
  return (buffer, length)
}
#endif

// Returns a UInt64, but that value is the length of the string, so it's
// guaranteed to fit into an Int. This is part of the ABI, so we can't
// trivially change it to Int. Callers can safely convert the result
// to any integer type without checks, however.
@_silgen_name("swift_float32ToString")
internal func _float32ToStringImpl(
  _ buffer: UnsafeMutablePointer<UTF8.CodeUnit>,
  _ bufferLength: UInt,
  _ value: Float32,
  _ debug: Bool
) -> UInt64

internal func _float32ToString(
  _ value: Float32,
  debug: Bool
) -> (buffer: _Buffer32, length: Int) {
  _internalInvariant(MemoryLayout<_Buffer32>.size == 32)
  var buffer = _Buffer32()
  let length = buffer.withBytes { (bufferPtr) in Int(
    truncatingIfNeeded: _float32ToStringImpl(bufferPtr, 32, value, debug)
  )}
  return (buffer, length)
}

// Returns a UInt64, but that value is the length of the string, so it's
// guaranteed to fit into an Int. This is part of the ABI, so we can't
// trivially change it to Int. Callers can safely convert the result
// to any integer type without checks, however.
@_silgen_name("swift_float64ToString")
internal func _float64ToStringImpl(
  _ buffer: UnsafeMutablePointer<UTF8.CodeUnit>,
  _ bufferLength: UInt,
  _ value: Float64,
  _ debug: Bool
) -> UInt64

internal func _float64ToString(
  _ value: Float64,
  debug: Bool
) -> (buffer: _Buffer32, length: Int) {
  _internalInvariant(MemoryLayout<_Buffer32>.size == 32)
  var buffer = _Buffer32()
  let length = buffer.withBytes { (bufferPtr) in Int(
    truncatingIfNeeded: _float64ToStringImpl(bufferPtr, 32, value, debug)
  )}
  return (buffer, length)
}

#if !(os(Windows) || os(Android)) && (arch(i386) || arch(x86_64))
// Returns a UInt64, but that value is the length of the string, so it's
// guaranteed to fit into an Int. This is part of the ABI, so we can't
// trivially change it to Int. Callers can safely convert the result
// to any integer type without checks, however.
@_silgen_name("swift_float80ToString")
internal func _float80ToStringImpl(
  _ buffer: UnsafeMutablePointer<UTF8.CodeUnit>,
  _ bufferLength: UInt,
  _ value: Float80,
  _ debug: Bool
) -> UInt64

internal func _float80ToString(
  _ value: Float80,
  debug: Bool
) -> (buffer: _Buffer32, length: Int) {
  _internalInvariant(MemoryLayout<_Buffer32>.size == 32)
  var buffer = _Buffer32()
  let length = buffer.withBytes { (bufferPtr) in Int(
    truncatingIfNeeded: _float80ToStringImpl(bufferPtr, 32, value, debug)
  )}
  return (buffer, length)
}
#endif

@inlinable
internal func _rawPointerToString(_ value: Builtin.RawPointer) -> String {
  var result = String(
    UInt64(UInt(bitPattern: UnsafeRawPointer(value))),
    radix: 16,
    uppercase: false)
  let count = 2 * MemoryLayout<UnsafeRawPointer>.size - result.utf8.count
  return "0x" + String(repeating: "0", count: count) + result
}

//===----------------------------------------------------------------------===//
// Old entry points preserved for ABI compatibility.
//===----------------------------------------------------------------------===//

// Returns a UInt64, but that value is the length of the string, so it's
// guaranteed to fit into an Int. This is part of the ABI, so we can't
// trivially change it to Int. Callers can safely convert the result
// to any integer type without checks, however.
@_silgen_name("swift_int64ToString")
internal func _int64ToStringImpl(
  _ buffer: UnsafeMutablePointer<UTF8.CodeUnit>,
  _ bufferLength: UInt,
  _ value: Int64,
  _ radix: Int64,
  _ uppercase: Bool
) -> UInt64

// Returns a UInt64, but that value is the length of the string, so it's
// guaranteed to fit into an Int. This is part of the ABI, so we can't
// trivially change it to Int. Callers can safely convert the result
// to any integer type without checks, however.
@_silgen_name("swift_uint64ToString")
internal func _uint64ToStringImpl(
  _ buffer: UnsafeMutablePointer<UTF8.CodeUnit>,
  _ bufferLength: UInt,
  _ value: UInt64,
  _ radix: Int64,
  _ uppercase: Bool
) -> UInt64

@available(*, deprecated, message: "Use 'String(_:radix:uppercase:)' instead")
public // @testable
func _uint64ToString(
  _ value: UInt64,
  radix: Int64 = 10,
  uppercase: Bool = false
) -> String {
  return value._description(
    radix: Int(truncatingIfNeeded: radix),
    uppercase: uppercase)
}

//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

#if _runtime(_ObjC)
// At runtime, these classes are derived from `__SwiftNativeNSXXXBase`,
// which are derived from `NSXXX`.
//
// The @swift_native_objc_runtime_base attribute
// allows us to subclass an Objective-C class and still use the fast Swift
// memory allocator.
//
// NOTE: older runtimes called these _SwiftNativeNSXXX. The two must
// coexist, so they were renamed. The old names must not be used in the
// new runtime.

@_fixed_layout
@usableFromInline
@objc @_swift_native_objc_runtime_base(__SwiftNativeNSArrayBase)
internal class __SwiftNativeNSArray {
  @inlinable
  @nonobjc
  internal init() {}
//  @objc public init(coder: AnyObject) {}
  @inlinable
  deinit {}
}

@_fixed_layout
@usableFromInline
@objc @_swift_native_objc_runtime_base(__SwiftNativeNSMutableArrayBase)
internal class _SwiftNativeNSMutableArray {
  @inlinable
  @nonobjc
  internal init() {}
//  @objc public init(coder: AnyObject) {}
  @inlinable
  deinit {}
}

@_fixed_layout
@usableFromInline
@objc @_swift_native_objc_runtime_base(__SwiftNativeNSDictionaryBase)
internal class __SwiftNativeNSDictionary {
  @nonobjc
  internal init() {}
  @objc public init(coder: AnyObject) {}
  deinit {}
}

@_fixed_layout
@usableFromInline
@objc @_swift_native_objc_runtime_base(__SwiftNativeNSSetBase)
internal class __SwiftNativeNSSet {
  @nonobjc
  internal init() {}
  @objc public init(coder: AnyObject) {}
  deinit {}
}

@objc
@_swift_native_objc_runtime_base(__SwiftNativeNSEnumeratorBase)
internal class __SwiftNativeNSEnumerator {
  @nonobjc
  internal init() {}
  @objc public init(coder: AnyObject) {}
  deinit {}
}

//===----------------------------------------------------------------------===//
// Support for reliable testing of the return-autoreleased optimization
//===----------------------------------------------------------------------===//

@objc
internal class __stdlib_ReturnAutoreleasedDummy {
  @objc
  internal init() {}

  // Use 'dynamic' to force Objective-C dispatch, which uses the
  // return-autoreleased call sequence.
  @objc
  internal dynamic func returnsAutoreleased(_ x: AnyObject) -> AnyObject {
    return x
  }
}

/// This function ensures that the return-autoreleased optimization works.
///
/// On some platforms (for example, x86_64), the first call to
/// `objc_autoreleaseReturnValue` will always autorelease because it would fail
/// to verify the instruction sequence in the caller.  On x86_64 certain PLT
/// entries would be still pointing to the resolver function, and sniffing
/// the call sequence would fail.
///
/// This code should live in the core stdlib dylib because PLT tables are
/// separate for each dylib.
///
/// Call this function in a fresh autorelease pool.
public func _stdlib_initializeReturnAutoreleased() {
#if arch(x86_64)
  // On x86_64 it is sufficient to perform one cycle of return-autoreleased
  // call sequence in order to initialize all required PLT entries.
  let dummy = __stdlib_ReturnAutoreleasedDummy()
  _ = dummy.returnsAutoreleased(dummy)
#endif
}
#else

@_fixed_layout
@usableFromInline
internal class __SwiftNativeNSArray {
  @inlinable
  internal init() {}
  @inlinable
  deinit {}
}
@_fixed_layout
@usableFromInline
internal class __SwiftNativeNSDictionary {
  @inlinable
  internal init() {}
  @inlinable
  deinit {}
}
@_fixed_layout
@usableFromInline
internal class __SwiftNativeNSSet {
  @inlinable
  internal init() {}
  @inlinable
  deinit {}
}

#endif
