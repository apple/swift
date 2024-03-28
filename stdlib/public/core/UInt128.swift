//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// MARK: - Memory layout

/// A 128-bit unsigned integer type.
@available(SwiftStdlib 6.0, *)
@frozen
public struct UInt128: Sendable {
#if _pointerBitWidth(_64) || arch(arm64_32)
  //  On 64-bit platforms (including arm64_32 and any similar targets with
  //  32b pointers but HW-backed 64b integers), the layout is simply that
  //  of `Builtin.Int128`.
  public var _value: Builtin.Int128
  
  @_transparent
  public init(_ _value: Builtin.Int128) {
    self._value = _value
  }
  
  @usableFromInline @_transparent
  internal var low: UInt64 {
    UInt64(truncatingIfNeeded: self)
  }
  
  @usableFromInline @_transparent
  internal var high: UInt64 {
    UInt64(truncatingIfNeeded: self &>> 64)
  }
  
  @usableFromInline @_transparent
  internal init(low: UInt64, high: UInt64) {
#if _endian(little)
    self = unsafeBitCast((low, high), to: Self.self)
#else
    self = unsafeBitCast((high, low), to: Self.self)
#endif
  }
  
#else
  //  On 32-bit platforms, we don't want to use Builtin.Int128 for layout
  //  because it would be 16B aligned, which is excessive for such targets
  //  (and generally incompatible with C's `_BitInt(128)`). Instead we lay
  //  out the type as two `UInt64` fields--note that we have to be careful
  //  about endianness in this case.
#if _endian(little)
  @usableFromInline internal var low: UInt64
  @usableFromInline internal var high: UInt64
#else
  @usableFromInline internal var high: UInt64
  @usableFromInline internal var low: UInt64
#endif
  
  @usableFromInline @_transparent
  internal init(low: UInt64, high: UInt64) {
    self.low = low
    self.high = high
  }
  
  public var _value: Builtin.Int128 {
    @_transparent get { unsafeBitCast(self, to: Builtin.Int128.self) }
    @_transparent set { self = Self(newValue) }
  }
  
  @_transparent
  public init(_ _value: Builtin.Int128) {
    self = unsafeBitCast(_value, to: Self.self)
  }
#endif
  
  @_transparent
  public init(bitPattern: Int128) {
    self.init(bitPattern._value)
  }
}

// MARK: - Constants
@available(SwiftStdlib 6.0, *)
extension UInt128 {
  @inlinable
  public static var zero: Self {
    Self(Builtin.zeroInitializer())
  }
  
  @inlinable
  public static var min: Self { zero }
  
  @inlinable
  public static var max: Self {
    Self(low: .max, high: .max)
  }
}

// MARK: - Conversions from other integers
@available(SwiftStdlib 6.0, *)
extension UInt128: ExpressibleByIntegerLiteral,
                   _ExpressibleByBuiltinIntegerLiteral {
  
  public typealias IntegerLiteralType = Self
  
  @inlinable
  public init(_builtinIntegerLiteral x: Builtin.IntLiteral) {
    self.init(Builtin.s_to_u_checked_trunc_IntLiteral_Int128(x).0)
  }
  
  @inlinable
  public init?<T>(exactly source: T) where T: BinaryInteger {
    guard let high = UInt64(exactly: source >> 64) else { return nil }
    let low = UInt64(truncatingIfNeeded: source)
    self.init(low: low, high: high)
  }
  
  @inlinable
  public init<T>(_ source: T) where T: BinaryInteger {
    guard let value = Self(exactly: source) else {
      fatalError("value cannot be converted to UInt128 because it is outside the representable range")
    }
    self = value
  }
  
  @inlinable
  public init<T>(clamping source: T) where T: BinaryInteger {
    guard let value = Self(exactly: source) else {
      self = source < .zero ? .zero : .max
      return
    }
    self = value
  }
  
  @inlinable
  public init<T>(truncatingIfNeeded source: T) where T: BinaryInteger {
    let high = UInt64(truncatingIfNeeded: source >> 64)
    let low = UInt64(truncatingIfNeeded: source)
    self.init(low: low, high: high)
  }
  
  @inlinable
  public init(_truncatingBits source: UInt) {
    self.init(low: UInt64(source), high: .zero)
  }
}

// MARK: - Conversions from Binary floating-point
@available(SwiftStdlib 6.0, *)
extension UInt128 {
  @inlinable
  public init?<T>(exactly source: T) where T: BinaryFloatingPoint {
    let highAsFloat = (source * 0x1.0p-64).rounded(.towardZero)
    guard let high = UInt64(exactly: highAsFloat) else { return nil }
    guard let low = UInt64(
      exactly: high == 0 ? source : source - 0x1.0p64*highAsFloat
    ) else { return nil }
    self.init(low: low, high: high)
  }
  
  @inlinable
  public init<T>(_ source: T) where T: BinaryFloatingPoint {
    guard let value = Self(exactly: source.rounded(.towardZero)) else {
      fatalError("value cannot be converted to UInt128 because it is outside the representable range")
    }
    self = value
  }
}

// MARK: - Non-arithmetic utility conformances
@available(SwiftStdlib 6.0, *)
extension UInt128: Equatable {
  @inlinable
  public static func ==(a: Self, b: Self) -> Bool {
    Bool(Builtin.cmp_eq_Int128(a._value, b._value))
  }
}

@available(SwiftStdlib 6.0, *)
extension UInt128: Comparable {
  @inlinable
  public static func <(a: Self, b: Self) -> Bool {
    Bool(Builtin.cmp_ult_Int128(a._value, b._value))
  }
}

@available(SwiftStdlib 6.0, *)
extension UInt128: Hashable {
  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(low)
    hasher.combine(high)
  }
}

// MARK: - Overflow-reporting arithmetic
@available(SwiftStdlib 6.0, *)
extension UInt128 {
  @inlinable
  public func addingReportingOverflow(
    _ other: Self
  ) -> (partialValue: Self, overflow: Bool) {
    let (result, overflow) = Builtin.uadd_with_overflow_Int128(
      self._value, other._value, Builtin.zeroInitializer()
    )
    return (Self(result), Bool(overflow))
  }
  
  @inlinable
  public func subtractingReportingOverflow(
    _ other: Self
  ) -> (partialValue: Self, overflow: Bool) {
    let (result, overflow) = Builtin.usub_with_overflow_Int128(
      self._value, other._value, Builtin.zeroInitializer()
    )
    return (Self(result), Bool(overflow))
  }
  
  @inlinable
  public func multipliedReportingOverflow(
    by other: Self
  ) -> (partialValue: Self, overflow: Bool) {
    let (result, overflow) = Builtin.umul_with_overflow_Int128(
      self._value, other._value, Builtin.zeroInitializer()
    )
    return (Self(result), Bool(overflow))
  }
  
  @inlinable
  public func dividedReportingOverflow(
    by other: Self
  ) -> (partialValue: Self, overflow: Bool) {
    precondition(other != .zero, "Division by zero.")
    // Unsigned divide never overflows.
    return (Self(Builtin.udiv_Int128(self._value, other._value)), false)
  }
  
  @inlinable
  public func remainderReportingOverflow(
    dividingBy other: Self
  ) -> (partialValue: Self, overflow: Bool) {
    precondition(other != .zero, "Remainder dividing by zero.")
    // Unsigned divide never overflows.
    return (Self(Builtin.urem_Int128(self._value, other._value)), false)
  }
}

// MARK: - AdditiveArithmetic conformance
@available(SwiftStdlib 6.0, *)
extension UInt128: AdditiveArithmetic {
  @inlinable
  public static func +(a: Self, b: Self) -> Self {
    let (result, overflow) = a.addingReportingOverflow(b)
    // On arm64, this check materializes the carryout in register, then does
    // a TBNZ, where we should get a b.cs instead. I filed rdar://115387277
    // to track this, but it only costs us one extra instruction, so we'll
    // keep it as is for now.
    precondition(!overflow)
    return result
  }
  
  @inlinable
  public static func -(a: Self, b: Self) -> Self {
    let (result, overflow) = a.subtractingReportingOverflow(b)
    precondition(!overflow)
    return result
  }
}

// MARK: - Multiplication and division
@available(SwiftStdlib 6.0, *)
extension UInt128 {
  @inlinable
  public static func *(a: Self, b: Self) -> Self {
    let (result, overflow) = a.multipliedReportingOverflow(by: b)
    precondition(!overflow)
    return result
  }
  
  @inlinable
  public static func *=(a: inout Self, b: Self) { a = a * b }
  
  @inlinable
  public static func /(a: Self, b: Self) -> Self {
    return a.dividedReportingOverflow(by: b).partialValue
  }
  
  @inlinable
  public static func /=(a: inout Self, b: Self) { a = a / b }
  
  @inlinable
  public static func %(a: Self, b: Self) -> Self {
    return a.remainderReportingOverflow(dividingBy: b).partialValue
  }
  
  @inlinable
  public static func %=(a: inout Self, b: Self) { a = a % b }
}

// MARK: - Numeric conformance
@available(SwiftStdlib 6.0, *)
extension UInt128: Numeric {
  public typealias Magnitude = Self
  
  @inlinable
  public var magnitude: Self { self }
}

// MARK: - BinaryInteger conformance
@available(SwiftStdlib 6.0, *)
extension UInt128: BinaryInteger {
  
  public struct Words {
    @usableFromInline
    let _value: UInt128
    @usableFromInline
    init(_value: UInt128) { self._value = _value }
  }
  
  @inlinable
  public var words: Words { Words(_value: self) }
  
  @inlinable
  public static func &=(a: inout Self, b: Self) {
    a._value = Builtin.and_Int128(a._value, b._value)
  }
  
  @inlinable
  public static func |=(a: inout Self, b: Self) {
    a._value = Builtin.or_Int128(a._value, b._value)
  }
  
  @inlinable
  public static func ^=(a: inout Self, b: Self) {
    a._value = Builtin.xor_Int128(a._value, b._value)
  }
  
  @inlinable
  public static func &>>=(a: inout Self, b: Self) {
    let masked = b & 127
    a._value = Builtin.lshr_Int128(a._value, masked._value)
  }
  
  @inlinable
  public static func &<<=(a: inout Self, b: Self) {
    let masked = b & 127
    a._value = Builtin.shl_Int128(a._value, masked._value)
  }
  
  @inlinable
  public var trailingZeroBitCount: Int {
    low == 0 ? 64 + high.trailingZeroBitCount : low.trailingZeroBitCount
  }
}

@available(SwiftStdlib 6.0, *)
extension UInt128.Words: RandomAccessCollection {
  public typealias Element = UInt
  public typealias Index = Int
  public typealias SubSequence = Slice<Self>
  public typealias Indices = Range<Int>
  
  @inlinable public var count: Int { 128 / UInt.bitWidth }
  @inlinable public var startIndex: Int { 0 }
  @inlinable public var endIndex: Int { count }
  @inlinable public var indices: Indices { startIndex ..< endIndex }
  @inlinable public func index(after i: Int) -> Int { i + 1 }
  @inlinable public func index(before i: Int) -> Int { i - 1 }
  
  public subscript(position: Int) -> UInt {
    @inlinable
    get {
      precondition(position >= 0 && position < count)
      var value = _value
#if _endian(little)
      let index = position
#else
      let index = count - 1 - position
#endif
      return _withUnprotectedUnsafePointer(to: &value) {
        $0.withMemoryRebound(to: UInt.self, capacity: count) { $0[index] }
      }
    }
  }
}

// MARK: - FixedWidthInteger conformance
@available(SwiftStdlib 6.0, *)
extension UInt128: FixedWidthInteger, UnsignedInteger {
  
  @inlinable
  static public var bitWidth: Int { 128 }
  
  @inlinable
  public var nonzeroBitCount: Int {
    high.nonzeroBitCount &+ low.nonzeroBitCount
  }
  
  @inlinable
  public var leadingZeroBitCount: Int {
    high == 0 ? 64 + low.leadingZeroBitCount : high.leadingZeroBitCount
  }
  
  @inlinable
  public var byteSwapped: Self {
    return Self(low: high.byteSwapped, high: low.byteSwapped)
  }
}

