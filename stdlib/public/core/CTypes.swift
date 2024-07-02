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
// C Primitive Types
//===----------------------------------------------------------------------===//

/// The C 'char' type.
///
/// This will be the same as either `CSignedChar` (in the common
/// case) or `CUnsignedChar`, depending on the platform.
public typealias CChar = Int8

/// The C 'unsigned char' type.
public typealias CUnsignedChar = UInt8

/// The C 'unsigned short' type.
public typealias CUnsignedShort = UInt16

/// The C 'unsigned int' type.
public typealias CUnsignedInt = UInt32

/// The C 'unsigned long' type.
#if os(Windows) && (arch(x86_64) || arch(arm64))
public typealias CUnsignedLong = UInt32
#else
public typealias CUnsignedLong = UInt
#endif

/// The C 'unsigned long long' type.
public typealias CUnsignedLongLong = UInt64

/// The C 'signed char' type.
public typealias CSignedChar = Int8

/// The C 'short' type.
public typealias CShort = Int16

/// The C 'int' type.
public typealias CInt = Int32

/// The C 'long' type.
#if os(Windows) && (arch(x86_64) || arch(arm64))
public typealias CLong = Int32
#else
public typealias CLong = Int
#endif

/// The C 'long long' type.
public typealias CLongLong = Int64

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
/// The C '_Float16' type.
@available(SwiftStdlib 5.3, *)
public typealias CFloat16 = Float16
#endif

/// The C 'float' type.
public typealias CFloat = Float

/// The C 'double' type.
public typealias CDouble = Double

/// The C 'long double' type.
#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS) || os(visionOS)
// On Darwin, long double is Float80 on x86, and Double otherwise.
#if arch(x86_64) || arch(i386)
public typealias CLongDouble = Float80
#else
public typealias CLongDouble = Double
#endif
#elseif os(Windows)
// On Windows, long double is always Double.
public typealias CLongDouble = Double
#elseif os(Linux)
// On Linux/x86, long double is Float80.
// TODO: Fill in definitions for additional architectures as needed. IIRC
// armv7 should map to Double, but arm64 and ppc64le should map to Float128,
// which we don't yet have in Swift.
#if arch(x86_64) || arch(i386)
public typealias CLongDouble = Float80
#elseif arch(s390x)
// On s390x '-mlong-double-64' option with size of 64-bits makes the
// Long Double type equivalent to Double type.
public typealias CLongDouble = Double
#endif
#elseif os(Android)
// On Android, long double is Float128 for AAPCS64, which we don't have yet in
// Swift (https://github.com/apple/swift/issues/51573); and Double for ARMv7.
#if arch(arm)
public typealias CLongDouble = Double
#endif
#elseif os(OpenBSD)
#if arch(x86_64)
public typealias CLongDouble = Float80
#else
#error("CLongDouble needs to be defined for this OpenBSD architecture")
#endif
#elseif os(FreeBSD)
#if arch(x86_64) || arch(i386)
public typealias CLongDouble = Float80
#else
#error("CLongDouble needs to be defined for this FreeBSD architecture")
#endif
#elseif $Embedded
#if arch(x86_64) || arch(i386)
public typealias CLongDouble = Double
#endif
#else
// TODO: define CLongDouble for other OSes
#endif

// FIXME: Is it actually UTF-32 on Darwin?
//
/// The C++ 'wchar_t' type.
#if os(Windows)
public typealias CWideChar = UInt16
#else
public typealias CWideChar = Unicode.Scalar
#endif

// FIXME: Swift should probably have a UTF-16 type other than UInt16.
//
/// The C++11 'char16_t' type, which has UTF-16 encoding.
public typealias CChar16 = UInt16

/// The C++11 'char32_t' type, which has UTF-32 encoding.
public typealias CChar32 = Unicode.Scalar

/// The C '_Bool' and C++ 'bool' type.
public typealias CBool = Bool

#if arch(arm64) || arch(powerpc64) || arch(powerpc64le) || arch(s390x) || arch(x86_64)
public typealias CIntPtr = Int64
public typealias CUIntPtr = UInt64
#else
public typealias CIntPtr = Int32
public typealias CUIntPtr = UInt32
#endif

/// A wrapper around an opaque C pointer.
///
/// Opaque pointers are used to represent C pointers to types that
/// cannot be represented in Swift, such as incomplete struct types.
@frozen
public struct OpaquePointer {
  @usableFromInline
  internal var _rawValue: Builtin.RawPointer

  @usableFromInline @_transparent
  internal init(_ v: Builtin.RawPointer) {
    self._rawValue = v
  }
}

@available(*, unavailable)
extension OpaquePointer: Sendable {}

extension OpaquePointer {
  /// Creates a new `OpaquePointer` from the given address, specified as a bit
  /// pattern.
  ///
  /// - Parameter bitPattern: A bit pattern to use for the address of the new
  ///   pointer. If `bitPattern` is zero, the result is `nil`.
  @_transparent
  public init?(bitPattern: Int) {
    if bitPattern == 0 { return nil }
    self._rawValue = Builtin.inttoptr_Word(bitPattern._builtinWordValue)
  }

  /// Creates a new `OpaquePointer` from the given address, specified as a bit
  /// pattern.
  ///
  /// - Parameter bitPattern: A bit pattern to use for the address of the new
  ///   pointer. If `bitPattern` is zero, the result is `nil`.
  @_transparent
  public init?(bitPattern: UInt) {
    if bitPattern == 0 { return nil }
    self._rawValue = Builtin.inttoptr_Word(bitPattern._builtinWordValue)
  }
}

extension OpaquePointer {
  /// Converts a typed `UnsafePointer` to an opaque C pointer.
  @_transparent
  @_preInverseGenerics
  public init<T: ~Copyable>(@_nonEphemeral _ from: UnsafePointer<T>) {
    self._rawValue = from._rawValue
  }

  /// Converts a typed `UnsafePointer` to an opaque C pointer.
  ///
  /// The result is `nil` if `from` is `nil`.
  @_transparent
  @_preInverseGenerics
  public init?<T: ~Copyable>(@_nonEphemeral _ from: UnsafePointer<T>?) {
    guard let unwrapped = from else { return nil }
    self.init(unwrapped)
  }
}

extension OpaquePointer {
  /// Converts a typed `UnsafeMutablePointer` to an opaque C pointer.
  @_transparent
  @_preInverseGenerics
  public init<T: ~Copyable>(@_nonEphemeral _ from: UnsafeMutablePointer<T>) {
    self._rawValue = from._rawValue
  }

  /// Converts a typed `UnsafeMutablePointer` to an opaque C pointer.
  ///
  /// The result is `nil` if `from` is `nil`.
  @_transparent
  @_preInverseGenerics
  public init?<T: ~Copyable>(@_nonEphemeral _ from: UnsafeMutablePointer<T>?) {
    guard let unwrapped = from else { return nil }
    self.init(unwrapped)
  }
}

extension OpaquePointer: Equatable {
  @inlinable // unsafe-performance
  public static func == (lhs: OpaquePointer, rhs: OpaquePointer) -> Bool {
    return Bool(Builtin.cmp_eq_RawPointer(lhs._rawValue, rhs._rawValue))
  }
}

extension OpaquePointer: Hashable {
  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(Int(Builtin.ptrtoint_Word(_rawValue)))
  }
}

@_unavailableInEmbedded
extension OpaquePointer: CustomDebugStringConvertible {
  /// A textual representation of the pointer, suitable for debugging.
  public var debugDescription: String {
    return _rawPointerToString(_rawValue)
  }
}

extension Int {
  /// Creates a new value with the bit pattern of the given pointer.
  ///
  /// The new value represents the address of the pointer passed as `pointer`.
  /// If `pointer` is `nil`, the result is `0`.
  ///
  /// - Parameter pointer: The pointer to use as the source for the new
  ///   integer.
  @inlinable // unsafe-performance
  public init(bitPattern pointer: OpaquePointer?) {
    self.init(bitPattern: UnsafeRawPointer(pointer))
  }
}

extension UInt {
  /// Creates a new value with the bit pattern of the given pointer.
  ///
  /// The new value represents the address of the pointer passed as `pointer`.
  /// If `pointer` is `nil`, the result is `0`.
  ///
  /// - Parameter pointer: The pointer to use as the source for the new
  ///   integer.
  @inlinable // unsafe-performance
  public init(bitPattern pointer: OpaquePointer?) {
    self.init(bitPattern: UnsafeRawPointer(pointer))
  }
}

/// A wrapper around a C `va_list` pointer.
#if arch(arm64) && !(os(macOS) || os(iOS) || os(tvOS) || os(watchOS) || os(visionOS) ||  os(Windows))
@frozen
public struct CVaListPointer {
  @usableFromInline // unsafe-performance
  internal var _value: (__stack: UnsafeMutablePointer<Int>?,
                        __gr_top: UnsafeMutablePointer<Int>?,
                        __vr_top: UnsafeMutablePointer<Int>?,
                        __gr_off: Int32,
                        __vr_off: Int32)

  @inlinable // unsafe-performance
  public // @testable
  init(__stack: UnsafeMutablePointer<Int>?,
       __gr_top: UnsafeMutablePointer<Int>?,
       __vr_top: UnsafeMutablePointer<Int>?,
       __gr_off: Int32,
       __vr_off: Int32) {
    _value = (__stack, __gr_top, __vr_top, __gr_off, __vr_off)
  }
}

@_unavailableInEmbedded
extension CVaListPointer: CustomDebugStringConvertible {
  public var debugDescription: String {
    return "(\(_value.__stack.debugDescription), " +
           "\(_value.__gr_top.debugDescription), " +
           "\(_value.__vr_top.debugDescription), " +
           "\(_value.__gr_off), " +
           "\(_value.__vr_off))"
  }
}

#else

@frozen
public struct CVaListPointer {
  @usableFromInline // unsafe-performance
  internal var _value: UnsafeMutableRawPointer

  @inlinable // unsafe-performance
  public // @testable
  init(_fromUnsafeMutablePointer from: UnsafeMutableRawPointer) {
    _value = from
  }
}

@_unavailableInEmbedded
extension CVaListPointer: CustomDebugStringConvertible {
  /// A textual representation of the pointer, suitable for debugging.
  public var debugDescription: String {
    return _value.debugDescription
  }
}

#endif

@available(*, unavailable)
extension CVaListPointer: Sendable { }

/// Copy `size` bytes of memory from `src` into `dest`.
///
/// The memory regions `src..<src + size` and
/// `dest..<dest + size` should not overlap.
@inlinable
internal func _memcpy(
  dest destination: UnsafeMutableRawPointer,
  src: UnsafeRawPointer,
  size: UInt
) {
  let dest = destination._rawValue
  let src = src._rawValue
  let size = UInt64(size)._value
  Builtin.int_memcpy_RawPointer_RawPointer_Int64(
    dest, src, size,
    /*volatile:*/ false._value)
}

/// Copy `size` bytes of memory from `src` into `dest`.
///
/// The memory regions `src..<src + size` and
/// `dest..<dest + size` may overlap.
@inlinable
internal func _memmove(
  dest destination: UnsafeMutableRawPointer,
  src: UnsafeRawPointer,
  size: UInt
) {
  let dest = destination._rawValue
  let src = src._rawValue
  let size = UInt64(size)._value
  Builtin.int_memmove_RawPointer_RawPointer_Int64(
    dest, src, size,
    /*volatile:*/ false._value)
}
