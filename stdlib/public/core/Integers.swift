//===--- Integers.swift ---------------------------------------------------===//
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

//===----------------------------------------------------------------------===//
//===--- Bits for the Stdlib ----------------------------------------------===//
//===----------------------------------------------------------------------===//

// FIXME(integers): This should go in the stdlib separately, probably.
extension ExpressibleByIntegerLiteral
  where Self: _ExpressibleByBuiltinIntegerLiteral {
  @_transparent
  public init(integerLiteral value: Self) {
    self = value
  }
}

//===----------------------------------------------------------------------===//
//===--- AdditiveArithmetic -----------------------------------------------===//
//===----------------------------------------------------------------------===//

/// A type with values that support addition and subtraction.
///
/// The `AdditiveArithmetic` protocol provides a suitable basis for additive
/// arithmetic on scalar values, such as integers and floating-point numbers,
/// or vectors. You can write generic methods that operate on any numeric type
/// in the standard library by using the `AdditiveArithmetic` protocol as a
/// generic constraint.
///
/// The following code declares a method that calculates the total of any
/// sequence with `AdditiveArithmetic` elements.
///
///     extension Sequence where Element: AdditiveArithmetic {
///         func sum() -> Element {
///             return reduce(.zero, +)
///         }
///     }
///
/// The `sum()` method is now available on any sequence with values that
/// conform to `AdditiveArithmetic`, whether it is an array of `Double` or a
/// range of `Int`.
///
///     let arraySum = [1.1, 2.2, 3.3, 4.4, 5.5].sum()
///     // arraySum == 16.5
///
///     let rangeSum = (1..<10).sum()
///     // rangeSum == 45
///
/// Conforming to the AdditiveArithmetic Protocol
/// =============================================
///
/// To add `AdditiveArithmetic` protocol conformance to your own custom type,
/// implement the required operators, and provide a static `zero` property.
public protocol AdditiveArithmetic: Equatable {
  /// The zero value.
  ///
  /// Zero is the identity element for addition. For any value,
  /// `x + .zero == x` and `.zero + x == x`.
  static var zero: Self { get }

  /// Adds two values and produces their sum.
  ///
  /// The addition operator (`+`) calculates the sum of its two arguments. For
  /// example:
  ///
  ///     1 + 2                   // 3
  ///     -10 + 15                // 5
  ///     -15 + -5                // -20
  ///     21.5 + 3.25             // 24.75
  ///
  /// You cannot use `+` with arguments of different types. To add values of
  /// different types, convert one of the values to the other value's type.
  ///
  ///     let x: Int8 = 21
  ///     let y: Int = 1000000
  ///     Int(x) + y              // 1000021
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  static func +(lhs: Self, rhs: Self) -> Self

  /// Adds two values and stores the result in the left-hand-side variable.
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  static func +=(lhs: inout Self, rhs: Self)

  /// Subtracts one value from another and produces their difference.
  ///
  /// The subtraction operator (`-`) calculates the difference of its two
  /// arguments. For example:
  ///
  ///     8 - 3                   // 5
  ///     -10 - 5                 // -15
  ///     100 - -5                // 105
  ///     10.5 - 100.0            // -89.5
  ///
  /// You cannot use `-` with arguments of different types. To subtract values
  /// of different types, convert one of the values to the other value's type.
  ///
  ///     let x: UInt8 = 21
  ///     let y: UInt = 1000000
  ///     y - UInt(x)             // 999979
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  static func -(lhs: Self, rhs: Self) -> Self

  /// Subtracts the second value from the first and stores the difference in the
  /// left-hand-side variable.
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  static func -=(lhs: inout Self, rhs: Self)
}

public extension AdditiveArithmetic {
  @_alwaysEmitIntoClient
  static func +=(lhs: inout Self, rhs: Self) {
    lhs = lhs + rhs
  }

  @_alwaysEmitIntoClient
  static func -=(lhs: inout Self, rhs: Self) {
    lhs = lhs - rhs
  }
}

public extension AdditiveArithmetic where Self: ExpressibleByIntegerLiteral {
  /// The zero value.
  ///
  /// Zero is the identity element for addition. For any value,
  /// `x + .zero == x` and `.zero + x == x`.
  @inlinable @inline(__always)
  static var zero: Self {
    return 0
  }
}

//===----------------------------------------------------------------------===//
//===--- Numeric ----------------------------------------------------------===//
//===----------------------------------------------------------------------===//

/// A type with values that support multiplication.
///
/// The `Numeric` protocol provides a suitable basis for arithmetic on
/// scalar values, such as integers and floating-point numbers. You can write
/// generic methods that operate on any numeric type in the standard library
/// by using the `Numeric` protocol as a generic constraint.
///
/// The following example extends `Sequence` with a method that returns an
/// array with the sequence's values multiplied by two.
///
///     extension Sequence where Element: Numeric {
///         func doublingAll() -> [Element] {
///             return map { $0 * 2 }
///         }
///     }
///
/// With this extension, any sequence with elements that conform to `Numeric`
/// has the `doublingAll()` method. For example, you can double the elements of
/// an array of doubles or a range of integers:
///
///     let observations = [1.5, 2.0, 3.25, 4.875, 5.5]
///     let doubledObservations = observations.doublingAll()
///     // doubledObservations == [3.0, 4.0, 6.5, 9.75, 11.0]
///
///     let integers = 0..<8
///     let doubledIntegers = integers.doublingAll()
///     // doubledIntegers == [0, 2, 4, 6, 8, 10, 12, 14]
///
/// Conforming to the Numeric Protocol
/// ==================================
///
/// To add `Numeric` protocol conformance to your own custom type, implement
/// the required initializer and operators, and provide a `magnitude` property
/// using a type that can represent the magnitude of any value of your custom
/// type.
public protocol Numeric: AdditiveArithmetic, ExpressibleByIntegerLiteral {
  /// Creates a new instance from the given integer, if it can be represented
  /// exactly.
  ///
  /// If the value passed as `source` is not representable exactly, the result
  /// is `nil`. In the following example, the constant `x` is successfully
  /// created from a value of `100`, while the attempt to initialize the
  /// constant `y` from `1_000` fails because the `Int8` type can represent
  /// `127` at maximum:
  ///
  ///     let x = Int8(exactly: 100)
  ///     // x == Optional(100)
  ///     let y = Int8(exactly: 1_000)
  ///     // y == nil
  ///
  /// - Parameter source: A value to convert to this type.
  init?<T: BinaryInteger>(exactly source: T)

  /// A type that can represent the absolute value of any possible value of the
  /// conforming type.
  associatedtype Magnitude: Comparable, Numeric

  /// The magnitude of this value.
  ///
  /// For any numeric value `x`, `x.magnitude` is the absolute value of `x`.
  /// You can use the `magnitude` property in operations that are simpler to
  /// implement in terms of unsigned values, such as printing the value of an
  /// integer, which is just printing a '-' character in front of an absolute
  /// value.
  ///
  ///     let x = -200
  ///     // x.magnitude == 200
  ///
  /// The global `abs(_:)` function provides more familiar syntax when you need
  /// to find an absolute value. In addition, because `abs(_:)` always returns
  /// a value of the same type, even in a generic context, using the function
  /// instead of the `magnitude` property is encouraged.
  var magnitude: Magnitude { get }

  /// Multiplies two values and produces their product.
  ///
  /// The multiplication operator (`*`) calculates the product of its two
  /// arguments. For example:
  ///
  ///     2 * 3                   // 6
  ///     100 * 21                // 2100
  ///     -10 * 15                // -150
  ///     3.5 * 2.25              // 7.875
  ///
  /// You cannot use `*` with arguments of different types. To multiply values
  /// of different types, convert one of the values to the other value's type.
  ///
  ///     let x: Int8 = 21
  ///     let y: Int = 1000000
  ///     Int(x) * y              // 21000000
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  static func *(lhs: Self, rhs: Self) -> Self

  /// Multiplies two values and stores the result in the left-hand-side
  /// variable.
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  static func *=(lhs: inout Self, rhs: Self)
}

/// A numeric type with a negation operation.
///
/// The `SignedNumeric` protocol extends the operations defined by the
/// `Numeric` protocol to include a value's additive inverse.
///
/// Conforming to the SignedNumeric Protocol
/// ========================================
///
/// Because the `SignedNumeric` protocol provides default implementations of
/// both of its required methods, you don't need to do anything beyond
/// declaring conformance to the protocol and ensuring that the values of your
/// type support negation. To customize your type's implementation, provide
/// your own mutating `negate()` method.
///
/// When the additive inverse of a value is unrepresentable in a conforming
/// type, the operation should either trap or return an exceptional value. For
/// example, using the negation operator (prefix `-`) with `Int.min` results in
/// a runtime error.
///
///     let x = Int.min
///     let y = -x
///     // Overflow error
public protocol SignedNumeric: Numeric {
  /// Returns the additive inverse of the specified value.
  ///
  /// The negation operator (prefix `-`) returns the additive inverse of its
  /// argument.
  ///
  ///     let x = 21
  ///     let y = -x
  ///     // y == -21
  ///
  /// The resulting value must be representable in the same type as the
  /// argument. In particular, negating a signed, fixed-width integer type's
  /// minimum results in a value that cannot be represented.
  ///
  ///     let z = -Int8.min
  ///     // Overflow error
  ///
  /// - Returns: The additive inverse of this value.
  static prefix func - (_ operand: Self) -> Self

  /// Replaces this value with its additive inverse.
  ///
  /// The following example uses the `negate()` method to negate the value of
  /// an integer `x`:
  ///
  ///     var x = 21
  ///     x.negate()
  ///     // x == -21
  ///
  /// The resulting value must be representable within the value's type. In
  /// particular, negating a signed, fixed-width integer type's minimum
  /// results in a value that cannot be represented.
  ///
  ///     var y = Int8.min
  ///     y.negate()
  ///     // Overflow error
  mutating func negate()
}

extension SignedNumeric {
  /// Returns the additive inverse of the specified value.
  ///
  /// The negation operator (prefix `-`) returns the additive inverse of its
  /// argument.
  ///
  ///     let x = 21
  ///     let y = -x
  ///     // y == -21
  ///
  /// The resulting value must be representable in the same type as the
  /// argument. In particular, negating a signed, fixed-width integer type's
  /// minimum results in a value that cannot be represented.
  ///
  ///     let z = -Int8.min
  ///     // Overflow error
  ///
  /// - Returns: The additive inverse of the argument.
  @_transparent
  public static prefix func - (_ operand: Self) -> Self {
    var result = operand
    result.negate()
    return result
  }

  /// Replaces this value with its additive inverse.
  ///
  /// The following example uses the `negate()` method to negate the value of
  /// an integer `x`:
  ///
  ///     var x = 21
  ///     x.negate()
  ///     // x == -21
  ///
  /// The resulting value must be representable within the value's type. In
  /// particular, negating a signed, fixed-width integer type's minimum
  /// results in a value that cannot be represented.
  ///
  ///     var y = Int8.min
  ///     y.negate()
  ///     // Overflow error
  @_transparent
  public mutating func negate() {
    self = 0 - self
  }
}

/// Returns the absolute value of the given number.
///
/// The absolute value of `x` must be representable in the same type. In
/// particular, the absolute value of a signed, fixed-width integer type's
/// minimum cannot be represented.
///
///     let x = Int8.min
///     // x == -128
///     let y = abs(x)
///     // Overflow error
///
/// - Parameter x: A signed number.
/// - Returns: The absolute value of `x`.
@inlinable
public func abs<T: SignedNumeric & Comparable>(_ x: T) -> T {
  if T.self == T.Magnitude.self {
    return unsafeBitCast(x.magnitude, to: T.self)
  }

  return x < (0 as T) ? -x : x
}

extension AdditiveArithmetic {
  /// Returns the given number unchanged.
  ///
  /// You can use the unary plus operator (`+`) to provide symmetry in your
  /// code for positive numbers when also using the unary minus operator.
  ///
  ///     let x = -21
  ///     let y = +21
  ///     // x == -21
  ///     // y == 21
  ///
  /// - Returns: The given argument without any changes.
  @_transparent
  public static prefix func + (x: Self) -> Self {
    return x
  }
}

#if !$Embedded
public typealias _CustomStringConvertibleOrNone = CustomStringConvertible
#else
public typealias _CustomStringConvertibleOrNone = Any
#endif

//===----------------------------------------------------------------------===//
//===--- BinaryInteger ----------------------------------------------------===//
//===----------------------------------------------------------------------===//

/// An integer type with a binary representation.
///
/// The `BinaryInteger` protocol is the basis for all the integer types
/// provided by the standard library. All of the standard library's integer
/// types, such as `Int` and `UInt32`, conform to `BinaryInteger`.
///
/// Converting Between Numeric Types
/// ================================
///
/// You can create new instances of a type that conforms to the `BinaryInteger`
/// protocol from a floating-point number or another binary integer of any
/// type. The `BinaryInteger` protocol provides initializers for four
/// different kinds of conversion.
///
/// Range-Checked Conversion
/// ------------------------
///
/// You use the default `init(_:)` initializer to create a new instance when
/// you're sure that the value passed is representable in the new type. For
/// example, an instance of `Int16` can represent the value `500`, so the
/// first conversion in the code sample below succeeds. That same value is too
/// large to represent as an `Int8` instance, so the second conversion fails,
/// triggering a runtime error.
///
///     let x: Int = 500
///     let y = Int16(x)
///     // y == 500
///
///     let z = Int8(x)
///     // Error: Not enough bits to represent...
///
/// When you create a binary integer from a floating-point value using the
/// default initializer, the value is rounded toward zero before the range is
/// checked. In the following example, the value `127.75` is rounded to `127`,
/// which is representable by the `Int8` type.  `128.25` is rounded to `128`,
/// which is not representable as an `Int8` instance, triggering a runtime
/// error.
///
///     let e = Int8(127.75)
///     // e == 127
///
///     let f = Int8(128.25)
///     // Error: Double value cannot be converted...
///
///
/// Exact Conversion
/// ----------------
///
/// Use the `init?(exactly:)` initializer to create a new instance after
/// checking whether the passed value is representable. Instead of trapping on
/// out-of-range values, using the failable `init?(exactly:)`
/// initializer results in `nil`.
///
///     let x = Int16(exactly: 500)
///     // x == Optional(500)
///
///     let y = Int8(exactly: 500)
///     // y == nil
///
/// When converting floating-point values, the `init?(exactly:)` initializer
/// checks both that the passed value has no fractional part and that the
/// value is representable in the resulting type.
///
///     let e = Int8(exactly: 23.0)       // integral value, representable
///     // e == Optional(23)
///
///     let f = Int8(exactly: 23.75)      // fractional value, representable
///     // f == nil
///
///     let g = Int8(exactly: 500.0)      // integral value, nonrepresentable
///     // g == nil
///
/// Clamping Conversion
/// -------------------
///
/// Use the `init(clamping:)` initializer to create a new instance of a binary
/// integer type where out-of-range values are clamped to the representable
/// range of the type. For a type `T`, the resulting value is in the range
/// `T.min...T.max`.
///
///     let x = Int16(clamping: 500)
///     // x == 500
///
///     let y = Int8(clamping: 500)
///     // y == 127
///
///     let z = UInt8(clamping: -500)
///     // z == 0
///
/// Bit Pattern Conversion
/// ----------------------
///
/// Use the `init(truncatingIfNeeded:)` initializer to create a new instance
/// with the same bit pattern as the passed value, extending or truncating the
/// value's representation as necessary. Note that the value may not be
/// preserved, particularly when converting between signed to unsigned integer
/// types or when the destination type has a smaller bit width than the source
/// type. The following example shows how extending and truncating work for
/// nonnegative integers:
///
///     let q: Int16 = 850
///     // q == 0b00000011_01010010
///
///     let r = Int8(truncatingIfNeeded: q)      // truncate 'q' to fit in 8 bits
///     // r == 82
///     //   == 0b01010010
///
///     let s = Int16(truncatingIfNeeded: r)     // extend 'r' to fill 16 bits
///     // s == 82
///     //   == 0b00000000_01010010
///
/// Any padding is performed by *sign-extending* the passed value. When
/// nonnegative integers are extended, the result is padded with zeroes. When
/// negative integers are extended, the result is padded with ones. This
/// example shows several extending conversions of a negative value---note
/// that negative values are sign-extended even when converting to an unsigned
/// type.
///
///     let t: Int8 = -100
///     // t == -100
///     // t's binary representation == 0b10011100
///
///     let u = UInt8(truncatingIfNeeded: t)
///     // u == 156
///     // u's binary representation == 0b10011100
///
///     let v = Int16(truncatingIfNeeded: t)
///     // v == -100
///     // v's binary representation == 0b11111111_10011100
///
///     let w = UInt16(truncatingIfNeeded: t)
///     // w == 65436
///     // w's binary representation == 0b11111111_10011100
///
///
/// Comparing Across Integer Types
/// ==============================
///
/// You can use relational operators, such as the less-than and equal-to
/// operators (`<` and `==`), to compare instances of different binary integer
/// types. The following example compares instances of the `Int`, `UInt`, and
/// `UInt8` types:
///
///     let x: Int = -23
///     let y: UInt = 1_000
///     let z: UInt8 = 23
///
///     if x < y {
///         print("\(x) is less than \(y).")
///     }
///     // Prints "-23 is less than 1000."
///
///     if z > x {
///         print("\(z) is greater than \(x).")
///     }
///     // Prints "23 is greater than -23."
public protocol BinaryInteger :
  Hashable, Numeric, _CustomStringConvertibleOrNone, Strideable
  where Magnitude: BinaryInteger, Magnitude.Magnitude == Magnitude
{
  /// A Boolean value indicating whether this type is a signed integer type.
  ///
  /// *Signed* integer types can represent both positive and negative values.
  /// *Unsigned* integer types can represent only nonnegative values.
  static var isSigned: Bool { get }

  /// Creates an integer from the given floating-point value, if it can be
  /// represented exactly.
  ///
  /// If the value passed as `source` is not representable exactly, the result
  /// is `nil`. In the following example, the constant `x` is successfully
  /// created from a value of `21.0`, while the attempt to initialize the
  /// constant `y` from `21.5` fails:
  ///
  ///     let x = Int(exactly: 21.0)
  ///     // x == Optional(21)
  ///     let y = Int(exactly: 21.5)
  ///     // y == nil
  ///
  /// - Parameter source: A floating-point value to convert to an integer.
  init?<T: BinaryFloatingPoint>(exactly source: T)

  /// Creates an integer from the given floating-point value, rounding toward
  /// zero.
  ///
  /// Any fractional part of the value passed as `source` is removed, rounding
  /// the value toward zero.
  ///
  ///     let x = Int(21.5)
  ///     // x == 21
  ///     let y = Int(-21.5)
  ///     // y == -21
  ///
  /// If `source` is outside the bounds of this type after rounding toward
  /// zero, a runtime error may occur.
  ///
  ///     let z = UInt(-21.5)
  ///     // Error: ...the result would be less than UInt.min
  ///
  /// - Parameter source: A floating-point value to convert to an integer.
  ///   `source` must be representable in this type after rounding toward
  ///   zero.
  init<T: BinaryFloatingPoint>(_ source: T)

  /// Creates a new instance from the given integer.
  ///
  /// If the value passed as `source` is not representable in this type, a
  /// runtime error may occur.
  ///
  ///     let x = -500 as Int
  ///     let y = Int32(x)
  ///     // y == -500
  ///
  ///     // -500 is not representable as a 'UInt32' instance
  ///     let z = UInt32(x)
  ///     // Error
  ///
  /// - Parameter source: An integer to convert. `source` must be representable
  ///   in this type.
  init<T: BinaryInteger>(_ source: T)

  /// Creates a new instance from the bit pattern of the given instance by
  /// sign-extending or truncating to fit this type.
  ///
  /// When the bit width of `T` (the type of `source`) is equal to or greater
  /// than this type's bit width, the result is the truncated
  /// least-significant bits of `source`. For example, when converting a
  /// 16-bit value to an 8-bit type, only the lower 8 bits of `source` are
  /// used.
  ///
  ///     let p: Int16 = -500
  ///     // 'p' has a binary representation of 11111110_00001100
  ///     let q = Int8(truncatingIfNeeded: p)
  ///     // q == 12
  ///     // 'q' has a binary representation of 00001100
  ///
  /// When the bit width of `T` is less than this type's bit width, the result
  /// is *sign-extended* to fill the remaining bits. That is, if `source` is
  /// negative, the result is padded with ones; otherwise, the result is
  /// padded with zeros.
  ///
  ///     let u: Int8 = 21
  ///     // 'u' has a binary representation of 00010101
  ///     let v = Int16(truncatingIfNeeded: u)
  ///     // v == 21
  ///     // 'v' has a binary representation of 00000000_00010101
  ///
  ///     let w: Int8 = -21
  ///     // 'w' has a binary representation of 11101011
  ///     let x = Int16(truncatingIfNeeded: w)
  ///     // x == -21
  ///     // 'x' has a binary representation of 11111111_11101011
  ///     let y = UInt16(truncatingIfNeeded: w)
  ///     // y == 65515
  ///     // 'y' has a binary representation of 11111111_11101011
  ///
  /// - Parameter source: An integer to convert to this type.
  init<T: BinaryInteger>(truncatingIfNeeded source: T)

  /// Creates a new instance with the representable value that's closest to the
  /// given integer.
  ///
  /// If the value passed as `source` is greater than the maximum representable
  /// value in this type, the result is the type's `max` value. If `source` is
  /// less than the smallest representable value in this type, the result is
  /// the type's `min` value.
  ///
  /// In this example, `x` is initialized as an `Int8` instance by clamping
  /// `500` to the range `-128...127`, and `y` is initialized as a `UInt`
  /// instance by clamping `-500` to the range `0...UInt.max`.
  ///
  ///     let x = Int8(clamping: 500)
  ///     // x == 127
  ///     // x == Int8.max
  ///
  ///     let y = UInt(clamping: -500)
  ///     // y == 0
  ///
  /// - Parameter source: An integer to convert to this type.
  init<T: BinaryInteger>(clamping source: T)

  /// A type that represents the words of a binary integer.
  ///
  /// The `Words` type must conform to the `RandomAccessCollection` protocol
  /// with an `Element` type of `UInt` and `Index` type of `Int`.
  associatedtype Words: RandomAccessCollection
      where Words.Element == UInt, Words.Index == Int

  /// A collection containing the words of this value's binary
  /// representation, in order from the least significant to most significant.
  ///
  /// Negative values are returned in two's complement representation,
  /// regardless of the type's underlying implementation.
  var words: Words { get }

  /// The least significant word in this value's binary representation.
  var _lowWord: UInt { get }

  /// The number of bits in the current binary representation of this value.
  ///
  /// This property is a constant for instances of fixed-width integer
  /// types.
  var bitWidth: Int { get }

  /// Returns the integer binary logarithm of this value.
  ///
  /// If the value is negative or zero, a runtime error will occur.
  func _binaryLogarithm() -> Int

  /// The number of trailing zeros in this value's binary representation.
  ///
  /// For example, in a fixed-width integer type with a `bitWidth` value of 8,
  /// the number -8 has three trailing zeros.
  ///
  ///     let x = Int8(bitPattern: 0b1111_1000)
  ///     // x == -8
  ///     // x.trailingZeroBitCount == 3
  ///
  /// If the value is zero, then `trailingZeroBitCount` is equal to `bitWidth`.
  var trailingZeroBitCount: Int { get }

  /// Returns the quotient of dividing the first value by the second.
  ///
  /// For integer types, any remainder of the division is discarded.
  ///
  ///     let x = 21 / 5
  ///     // x == 4
  ///
  /// - Parameters:
  ///   - lhs: The value to divide.
  ///   - rhs: The value to divide `lhs` by. `rhs` must not be zero.
  static func /(lhs: Self, rhs: Self) -> Self

  /// Divides the first value by the second and stores the quotient in the
  /// left-hand-side variable.
  ///
  /// For integer types, any remainder of the division is discarded.
  ///
  ///     var x = 21
  ///     x /= 5
  ///     // x == 4
  ///
  /// - Parameters:
  ///   - lhs: The value to divide.
  ///   - rhs: The value to divide `lhs` by. `rhs` must not be zero.
  static func /=(lhs: inout Self, rhs: Self)

  /// Returns the remainder of dividing the first value by the second.
  ///
  /// The result of the remainder operator (`%`) has the same sign as `lhs` and
  /// has a magnitude less than `rhs.magnitude`.
  ///
  ///     let x = 22 % 5
  ///     // x == 2
  ///     let y = 22 % -5
  ///     // y == 2
  ///     let z = -22 % -5
  ///     // z == -2
  ///
  /// For any two integers `a` and `b`, their quotient `q`, and their remainder
  /// `r`, `a == b * q + r`.
  ///
  /// - Parameters:
  ///   - lhs: The value to divide.
  ///   - rhs: The value to divide `lhs` by. `rhs` must not be zero.
  static func %(lhs: Self, rhs: Self) -> Self

  /// Divides the first value by the second and stores the remainder in the
  /// left-hand-side variable.
  ///
  /// The result has the same sign as `lhs` and has a magnitude less than
  /// `rhs.magnitude`.
  ///
  ///     var x = 22
  ///     x %= 5
  ///     // x == 2
  ///
  ///     var y = 22
  ///     y %= -5
  ///     // y == 2
  ///
  ///     var z = -22
  ///     z %= -5
  ///     // z == -2
  ///
  /// - Parameters:
  ///   - lhs: The value to divide.
  ///   - rhs: The value to divide `lhs` by. `rhs` must not be zero.
  static func %=(lhs: inout Self, rhs: Self)

  /// Adds two values and produces their sum.
  ///
  /// The addition operator (`+`) calculates the sum of its two arguments. For
  /// example:
  ///
  ///     1 + 2                   // 3
  ///     -10 + 15                // 5
  ///     -15 + -5                // -20
  ///     21.5 + 3.25             // 24.75
  ///
  /// You cannot use `+` with arguments of different types. To add values of
  /// different types, convert one of the values to the other value's type.
  ///
  ///     let x: Int8 = 21
  ///     let y: Int = 1000000
  ///     Int(x) + y              // 1000021
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  override static func +(lhs: Self, rhs: Self) -> Self

  /// Adds two values and stores the result in the left-hand-side variable.
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  override static func +=(lhs: inout Self, rhs: Self)

  /// Subtracts one value from another and produces their difference.
  ///
  /// The subtraction operator (`-`) calculates the difference of its two
  /// arguments. For example:
  ///
  ///     8 - 3                   // 5
  ///     -10 - 5                 // -15
  ///     100 - -5                // 105
  ///     10.5 - 100.0            // -89.5
  ///
  /// You cannot use `-` with arguments of different types. To subtract values
  /// of different types, convert one of the values to the other value's type.
  ///
  ///     let x: UInt8 = 21
  ///     let y: UInt = 1000000
  ///     y - UInt(x)             // 999979
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  override static func -(lhs: Self, rhs: Self) -> Self

  /// Subtracts the second value from the first and stores the difference in the
  /// left-hand-side variable.
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  override static func -=(lhs: inout Self, rhs: Self)

  /// Multiplies two values and produces their product.
  ///
  /// The multiplication operator (`*`) calculates the product of its two
  /// arguments. For example:
  ///
  ///     2 * 3                   // 6
  ///     100 * 21                // 2100
  ///     -10 * 15                // -150
  ///     3.5 * 2.25              // 7.875
  ///
  /// You cannot use `*` with arguments of different types. To multiply values
  /// of different types, convert one of the values to the other value's type.
  ///
  ///     let x: Int8 = 21
  ///     let y: Int = 1000000
  ///     Int(x) * y              // 21000000
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  override static func *(lhs: Self, rhs: Self) -> Self

  /// Multiplies two values and stores the result in the left-hand-side
  /// variable.
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  override static func *=(lhs: inout Self, rhs: Self)

  /// Returns the inverse of the bits set in the argument.
  ///
  /// The bitwise NOT operator (`~`) is a prefix operator that returns a value
  /// in which all the bits of its argument are flipped: Bits that are `1` in
  /// the argument are `0` in the result, and bits that are `0` in the argument
  /// are `1` in the result. This is equivalent to the inverse of a set. For
  /// example:
  ///
  ///     let x: UInt8 = 5        // 0b00000101
  ///     let notX = ~x           // 0b11111010
  ///
  /// Performing a bitwise NOT operation on 0 returns a value with every bit
  /// set to `1`.
  ///
  ///     let allOnes = ~UInt8.min   // 0b11111111
  ///
  /// - Complexity: O(1).
  static prefix func ~ (_ x: Self) -> Self

  /// Returns the result of performing a bitwise AND operation on the two given
  /// values.
  ///
  /// A bitwise AND operation results in a value that has each bit set to `1`
  /// where *both* of its arguments have that bit set to `1`. For example:
  ///
  ///     let x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     let z = x & y             // 0b00000100
  ///     // z == 4
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  static func &(lhs: Self, rhs: Self) -> Self

  /// Stores the result of performing a bitwise AND operation on the two given
  /// values in the left-hand-side variable.
  ///
  /// A bitwise AND operation results in a value that has each bit set to `1`
  /// where *both* of its arguments have that bit set to `1`. For example:
  ///
  ///     var x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     x &= y                    // 0b00000100
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  static func &=(lhs: inout Self, rhs: Self)

  /// Returns the result of performing a bitwise OR operation on the two given
  /// values.
  ///
  /// A bitwise OR operation results in a value that has each bit set to `1`
  /// where *one or both* of its arguments have that bit set to `1`. For
  /// example:
  ///
  ///     let x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     let z = x | y             // 0b00001111
  ///     // z == 15
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  static func |(lhs: Self, rhs: Self) -> Self

  /// Stores the result of performing a bitwise OR operation on the two given
  /// values in the left-hand-side variable.
  ///
  /// A bitwise OR operation results in a value that has each bit set to `1`
  /// where *one or both* of its arguments have that bit set to `1`. For
  /// example:
  ///
  ///     var x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     x |= y                    // 0b00001111
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  static func |=(lhs: inout Self, rhs: Self)

  /// Returns the result of performing a bitwise XOR operation on the two given
  /// values.
  ///
  /// A bitwise XOR operation, also known as an exclusive OR operation, results
  /// in a value that has each bit set to `1` where *one or the other but not
  /// both* of its arguments had that bit set to `1`. For example:
  ///
  ///     let x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     let z = x ^ y             // 0b00001011
  ///     // z == 11
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  static func ^(lhs: Self, rhs: Self) -> Self

  /// Stores the result of performing a bitwise XOR operation on the two given
  /// values in the left-hand-side variable.
  ///
  /// A bitwise XOR operation, also known as an exclusive OR operation, results
  /// in a value that has each bit set to `1` where *one or the other but not
  /// both* of its arguments had that bit set to `1`. For example:
  ///
  ///     var x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     x ^= y                    // 0b00001011
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  static func ^=(lhs: inout Self, rhs: Self)

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the right.
  ///
  /// The `>>` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a left shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*. An overshift results in `-1` for a
  ///   negative value of `lhs` or `0` for a nonnegative value.
  /// - Using any other value for `rhs` performs a right shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted right by two bits.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x >> 2
  ///     // y == 7                         // 0b00000111
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     let z = x >> 11
  ///     // z == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a left shift
  /// using `abs(rhs)`.
  ///
  ///     let a = x >> -3
  ///     // a == 240                       // 0b11110000
  ///     let b = x << 3
  ///     // b == 240                       // 0b11110000
  ///
  /// Right shift operations on negative values "fill in" the high bits with
  /// ones instead of zeros.
  ///
  ///     let q: Int8 = -30                 // 0b11100010
  ///     let r = q >> 2
  ///     // r == -8                        // 0b11111000
  ///
  ///     let s = q >> 11
  ///     // s == -1                        // 0b11111111
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right.
  static func >> <RHS: BinaryInteger>(lhs: Self, rhs: RHS) -> Self

  /// Stores the result of shifting a value's binary representation the
  /// specified number of digits to the right in the left-hand-side variable.
  ///
  /// The `>>=` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a left shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*. An overshift results in `-1` for a
  ///   negative value of `lhs` or `0` for a nonnegative value.
  /// - Using any other value for `rhs` performs a right shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted right by two bits.
  ///
  ///     var x: UInt8 = 30                 // 0b00011110
  ///     x >>= 2
  ///     // x == 7                         // 0b00000111
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     var y: UInt8 = 30                 // 0b00011110
  ///     y >>= 11
  ///     // y == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a left shift
  /// using `abs(rhs)`.
  ///
  ///     var a: UInt8 = 30                 // 0b00011110
  ///     a >>= -3
  ///     // a == 240                       // 0b11110000
  ///
  ///     var b: UInt8 = 30                 // 0b00011110
  ///     b <<= 3
  ///     // b == 240                       // 0b11110000
  ///
  /// Right shift operations on negative values "fill in" the high bits with
  /// ones instead of zeros.
  ///
  ///     var q: Int8 = -30                 // 0b11100010
  ///     q >>= 2
  ///     // q == -8                        // 0b11111000
  ///
  ///     var r: Int8 = -30                 // 0b11100010
  ///     r >>= 11
  ///     // r == -1                        // 0b11111111
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right.
  static func >>= <RHS: BinaryInteger>(lhs: inout Self, rhs: RHS)

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left.
  ///
  /// The `<<` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a right shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*, resulting in zero.
  /// - Using any other value for `rhs` performs a left shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted left by two bits.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x << 2
  ///     // y == 120                       // 0b01111000
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     let z = x << 11
  ///     // z == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a right shift
  /// with `abs(rhs)`.
  ///
  ///     let a = x << -3
  ///     // a == 3                         // 0b00000011
  ///     let b = x >> 3
  ///     // b == 3                         // 0b00000011
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left.
  static func << <RHS: BinaryInteger>(lhs: Self, rhs: RHS) -> Self

  /// Stores the result of shifting a value's binary representation the
  /// specified number of digits to the left in the left-hand-side variable.
  ///
  /// The `<<` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a right shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*, resulting in zero.
  /// - Using any other value for `rhs` performs a left shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted left by two bits.
  ///
  ///     var x: UInt8 = 30                 // 0b00011110
  ///     x <<= 2
  ///     // x == 120                       // 0b01111000
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     var y: UInt8 = 30                 // 0b00011110
  ///     y <<= 11
  ///     // y == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a right shift
  /// with `abs(rhs)`.
  ///
  ///     var a: UInt8 = 30                 // 0b00011110
  ///     a <<= -3
  ///     // a == 3                         // 0b00000011
  ///
  ///     var b: UInt8 = 30                 // 0b00011110
  ///     b >>= 3
  ///     // b == 3                         // 0b00000011
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left.
  static func <<=<RHS: BinaryInteger>(lhs: inout Self, rhs: RHS)

  /// Returns the quotient and remainder of this value divided by the given
  /// value.
  ///
  /// Use this method to calculate the quotient and remainder of a division at
  /// the same time.
  ///
  ///     let x = 1_000_000
  ///     let (q, r) = x.quotientAndRemainder(dividingBy: 933)
  ///     // q == 1071
  ///     // r == 757
  ///
  /// - Parameter rhs: The value to divide this value by.
  /// - Returns: A tuple containing the quotient and remainder of this value
  ///   divided by `rhs`. The remainder has the same sign as `lhs`.
  func quotientAndRemainder(dividingBy rhs: Self)
    -> (quotient: Self, remainder: Self)

  /// Returns `true` if this value is a multiple of the given value, and `false`
  /// otherwise.
  ///
  /// For two integers *a* and *b*, *a* is a multiple of *b* if there exists a
  /// third integer *q* such that _a = q*b_. For example, *6* is a multiple of
  /// *3* because _6 = 2*3_. Zero is a multiple of everything because _0 = 0*x_
  /// for any integer *x*.
  ///
  /// Two edge cases are worth particular attention:
  /// - `x.isMultiple(of: 0)` is `true` if `x` is zero and `false` otherwise.
  /// - `T.min.isMultiple(of: -1)` is `true` for signed integer `T`, even
  ///   though the quotient `T.min / -1` isn't representable in type `T`.
  ///
  /// - Parameter other: The value to test.
  func isMultiple(of other: Self) -> Bool

  /// Returns `-1` if this value is negative and `1` if it's positive;
  /// otherwise, `0`.
  ///
  /// - Returns: The sign of this number, expressed as an integer of the same
  ///   type.
  func signum() -> Self
}

extension BinaryInteger {
  /// Creates a new value equal to zero.
  @_transparent
  public init() {
    self = 0
  }

  /// Returns `-1` if this value is negative and `1` if it's positive;
  /// otherwise, `0`.
  ///
  /// - Returns: The sign of this number, expressed as an integer of the same
  ///   type.
  @inlinable
  public func signum() -> Self {
    return (self > (0 as Self) ? 1 : 0) - (self < (0 as Self) ? 1 : 0)
  }

  @_transparent
  public var _lowWord: UInt {
    var it = words.makeIterator()
    return it.next() ?? 0
  }

  @inlinable
  public func _binaryLogarithm() -> Int {
    _precondition(self > (0 as Self))
    var (quotient, remainder) =
      (bitWidth &- 1).quotientAndRemainder(dividingBy: UInt.bitWidth)
    remainder = remainder &+ 1
    var word = UInt(truncatingIfNeeded: self >> (bitWidth &- remainder))
    // If, internally, a variable-width binary integer uses digits of greater
    // bit width than that of Magnitude.Words.Element (i.e., UInt), then it is
    // possible that `word` could be zero. Additionally, a signed variable-width
    // binary integer may have a leading word that is zero to store a clear sign
    // bit.
    while word == 0 {
      quotient = quotient &- 1
      remainder = remainder &+ UInt.bitWidth
      word = UInt(truncatingIfNeeded: self >> (bitWidth &- remainder))
    }
    // Note that the order of operations below is important to guarantee that
    // we won't overflow.
    return UInt.bitWidth &* quotient &+
        (UInt.bitWidth &- (word.leadingZeroBitCount &+ 1))
  }

  /// Returns the quotient and remainder of this value divided by the given
  /// value.
  ///
  /// Use this method to calculate the quotient and remainder of a division at
  /// the same time.
  ///
  ///     let x = 1_000_000
  ///     let (q, r) = x.quotientAndRemainder(dividingBy: 933)
  ///     // q == 1071
  ///     // r == 757
  ///
  /// - Parameter rhs: The value to divide this value by.
  /// - Returns: A tuple containing the quotient and remainder of this value
  ///   divided by `rhs`.
  @inlinable
  public func quotientAndRemainder(dividingBy rhs: Self)
    -> (quotient: Self, remainder: Self) {
    return (self / rhs, self % rhs)
  }
  
  @inlinable
  public func isMultiple(of other: Self) -> Bool {
    // Nothing but zero is a multiple of zero.
    if other == 0 { return self == 0 }
    // Do the test in terms of magnitude, which guarantees there are no other
    // edge cases. If we write this as `self % other` instead, it could trap
    // for types that are not symmetric around zero.
    return self.magnitude % other.magnitude == 0
  }

//===----------------------------------------------------------------------===//
//===--- Homogeneous ------------------------------------------------------===//
//===----------------------------------------------------------------------===//
  /// Returns the result of performing a bitwise AND operation on the two given
  /// values.
  ///
  /// A bitwise AND operation results in a value that has each bit set to `1`
  /// where *both* of its arguments have that bit set to `1`. For example:
  ///
  ///     let x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     let z = x & y             // 0b00000100
  ///     // z == 4
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  @_transparent
  public static func & (lhs: Self, rhs: Self) -> Self {
    var lhs = lhs
    lhs &= rhs
    return lhs
  }

  /// Returns the result of performing a bitwise OR operation on the two given
  /// values.
  ///
  /// A bitwise OR operation results in a value that has each bit set to `1`
  /// where *one or both* of its arguments have that bit set to `1`. For
  /// example:
  ///
  ///     let x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     let z = x | y             // 0b00001111
  ///     // z == 15
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  @_transparent
  public static func | (lhs: Self, rhs: Self) -> Self {
    var lhs = lhs
    lhs |= rhs
    return lhs
  }

  /// Returns the result of performing a bitwise XOR operation on the two given
  /// values.
  ///
  /// A bitwise XOR operation, also known as an exclusive OR operation, results
  /// in a value that has each bit set to `1` where *one or the other but not
  /// both* of its arguments had that bit set to `1`. For example:
  ///
  ///     let x: UInt8 = 5          // 0b00000101
  ///     let y: UInt8 = 14         // 0b00001110
  ///     let z = x ^ y             // 0b00001011
  ///     // z == 11
  ///
  /// - Parameters:
  ///   - lhs: An integer value.
  ///   - rhs: Another integer value.
  @_transparent
  public static func ^ (lhs: Self, rhs: Self) -> Self {
    var lhs = lhs
    lhs ^= rhs
    return lhs
  }

//===----------------------------------------------------------------------===//
//===--- Heterogeneous non-masking shift in terms of shift-assignment -----===//
//===----------------------------------------------------------------------===//
  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the right.
  ///
  /// The `>>` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a left shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*. An overshift results in `-1` for a
  ///   negative value of `lhs` or `0` for a nonnegative value.
  /// - Using any other value for `rhs` performs a right shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted right by two bits.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x >> 2
  ///     // y == 7                         // 0b00000111
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     let z = x >> 11
  ///     // z == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a left shift
  /// using `abs(rhs)`.
  ///
  ///     let a = x >> -3
  ///     // a == 240                       // 0b11110000
  ///     let b = x << 3
  ///     // b == 240                       // 0b11110000
  ///
  /// Right shift operations on negative values "fill in" the high bits with
  /// ones instead of zeros.
  ///
  ///     let q: Int8 = -30                 // 0b11100010
  ///     let r = q >> 2
  ///     // r == -8                        // 0b11111000
  ///
  ///     let s = q >> 11
  ///     // s == -1                        // 0b11111111
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func >> <RHS: BinaryInteger>(lhs: Self, rhs: RHS) -> Self {
    var r = lhs
    r >>= rhs
    return r
  }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left.
  ///
  /// The `<<` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a right shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*, resulting in zero.
  /// - Using any other value for `rhs` performs a left shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted left by two bits.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x << 2
  ///     // y == 120                       // 0b01111000
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     let z = x << 11
  ///     // z == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a right shift
  /// with `abs(rhs)`.
  ///
  ///     let a = x << -3
  ///     // a == 3                         // 0b00000011
  ///     let b = x >> 3
  ///     // b == 3                         // 0b00000011
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func << <RHS: BinaryInteger>(lhs: Self, rhs: RHS) -> Self {
    var r = lhs
    r <<= rhs
    return r
  }
}


//===----------------------------------------------------------------------===//
//===--- CustomStringConvertible conformance ------------------------------===//
//===----------------------------------------------------------------------===//

@_unavailableInEmbedded
extension BinaryInteger {
  internal func _description(radix: Int, uppercase: Bool) -> String {
    _precondition(2...36 ~= radix, "Radix must be between 2 and 36")

    if bitWidth <= 64 {
      let radix_ = Int64(radix)
      return Self.isSigned
        ? _int64ToString(
          Int64(truncatingIfNeeded: self), radix: radix_, uppercase: uppercase)
        : _uint64ToString(
          UInt64(truncatingIfNeeded: self), radix: radix_, uppercase: uppercase)
    }

    if self == (0 as Self) { return "0" }

    // Bit shifting can be faster than division when `radix` is a power of two
    // (although not necessarily the case for builtin types).
    let isRadixPowerOfTwo = radix.nonzeroBitCount == 1
    let radix_ = Magnitude(radix)
    func _quotientAndRemainder(_ value: Magnitude) -> (Magnitude, Magnitude) {
      return isRadixPowerOfTwo
        ? (value >> radix.trailingZeroBitCount, value & (radix_ - 1))
        : value.quotientAndRemainder(dividingBy: radix_)
    }

    let hasLetters = radix > 10
    func _ascii(_ digit: UInt8) -> UInt8 {
      let base: UInt8
      if !hasLetters || digit < 10 {
        base = UInt8(("0" as Unicode.Scalar).value)
      } else if uppercase {
        base = UInt8(("A" as Unicode.Scalar).value) &- 10
      } else {
        base = UInt8(("a" as Unicode.Scalar).value) &- 10
      }
      return base &+ digit
    }

    let isNegative = Self.isSigned && self < (0 as Self)
    var value = magnitude

    // TODO(FIXME JIRA): All current stdlib types fit in small. Use a stack
    // buffer instead of an array on the heap.

    var result: [UInt8] = []
    while value != 0 {
      let (quotient, remainder) = _quotientAndRemainder(value)
      result.append(_ascii(UInt8(truncatingIfNeeded: remainder)))
      value = quotient
    }

    if isNegative {
      result.append(UInt8(("-" as Unicode.Scalar).value))
    }

    result.reverse()
    return result.withUnsafeBufferPointer {
      return String._fromASCII($0)
    }
  }

  /// A textual representation of this value.
  @_semantics("binaryInteger.description")
  public var description: String {
    return _description(radix: 10, uppercase: false)
  }
}


//===----------------------------------------------------------------------===//
//===--- Strideable conformance -------------------------------------------===//
//===----------------------------------------------------------------------===//

extension BinaryInteger {
  /// Returns the distance from this value to the given value, expressed as a
  /// stride.
  ///
  /// For two values `x` and `y`, and a distance `n = x.distance(to: y)`,
  /// `x.advanced(by: n) == y`.
  ///
  /// - Parameter other: The value to calculate the distance to.
  /// - Returns: The distance from this value to `other`.
  @inlinable
  @inline(__always)
  public func distance(to other: Self) -> Int {
    if !Self.isSigned {
      if self > other {
        if let result = Int(exactly: self - other) {
          return -result
        }
      } else {
        if let result = Int(exactly: other - self) {
          return result
        }
      }
    } else {
      let isNegative = self < (0 as Self)
      if isNegative == (other < (0 as Self)) {
        if let result = Int(exactly: other - self) {
          return result
        }
      } else {
        if let result = Int(exactly: self.magnitude + other.magnitude) {
          return isNegative ? result : -result
        }
      }
    }
    _preconditionFailure("Distance is not representable in Int")
  }

  /// Returns a value that is offset the specified distance from this value.
  ///
  /// Use the `advanced(by:)` method in generic code to offset a value by a
  /// specified distance. If you're working directly with numeric values, use
  /// the addition operator (`+`) instead of this method.
  ///
  /// For a value `x`, a distance `n`, and a value `y = x.advanced(by: n)`,
  /// `x.distance(to: y) == n`.
  ///
  /// - Parameter n: The distance to advance this value.
  /// - Returns: A value that is offset from this value by `n`.
  @inlinable
  @inline(__always)
  public func advanced(by n: Int) -> Self {
    if Self.isSigned {
      return self.bitWidth < n.bitWidth
        ? Self(Int(truncatingIfNeeded: self) + n)
        : self + Self(truncatingIfNeeded: n)
    } else {
      return n < (0 as Int)
        ? self - Self(UInt(bitPattern: ~n &+ 1))
        : self + Self(UInt(bitPattern: n))
    }
  }
}

//===----------------------------------------------------------------------===//
//===--- Heterogeneous comparison -----------------------------------------===//
//===----------------------------------------------------------------------===//

extension BinaryInteger {
  /// Returns a Boolean value indicating whether the two given values are
  /// equal.
  ///
  /// You can check the equality of instances of any `BinaryInteger` types
  /// using the equal-to operator (`==`). For example, you can test whether
  /// the first `UInt8` value in a string's UTF-8 encoding is equal to the
  /// first `UInt32` value in its Unicode scalar view:
  ///
  ///     let gameName = "Red Light, Green Light"
  ///     if let firstUTF8 = gameName.utf8.first,
  ///         let firstScalar = gameName.unicodeScalars.first?.value {
  ///         print("First code values are equal: \(firstUTF8 == firstScalar)")
  ///     }
  ///     // Prints "First code values are equal: true"
  ///
  /// - Parameters:
  ///   - lhs: An integer to compare.
  ///   - rhs: Another integer to compare.
  @_transparent
  public static func == <
    Other: BinaryInteger
  >(lhs: Self, rhs: Other) -> Bool {
    // Use bit pattern conversion to widen the comparand with smaller bit width.
    if Self.isSigned == Other.isSigned {
      return lhs.bitWidth >= rhs.bitWidth ?
        lhs == Self(truncatingIfNeeded: rhs) :
        Other(truncatingIfNeeded: lhs) == rhs
    }
    // If `Self` is signed but `Other` is unsigned, then we have to
    // be a little more careful about widening, since:
    // (1) a fixed-width signed type can't represent the largest values of
    //     a fixed-width unsigned type of equal bit width; and
    // (2) an unsigned type (obviously) can't represent a negative value.
    if Self.isSigned {    
      return lhs.bitWidth > rhs.bitWidth ? // (1)
        lhs == Self(truncatingIfNeeded: rhs) :
        (lhs >= (0 as Self) && Other(truncatingIfNeeded: lhs) == rhs) // (2)
    }
    // Analogous reasoning applies if `Other` is signed but `Self` is not.
    return lhs.bitWidth < rhs.bitWidth ?
      Other(truncatingIfNeeded: lhs) == rhs :
      (rhs >= (0 as Other) && lhs == Self(truncatingIfNeeded: rhs))
  }

  /// Returns a Boolean value indicating whether the two given values are not
  /// equal.
  ///
  /// You can check the inequality of instances of any `BinaryInteger` types
  /// using the not-equal-to operator (`!=`). For example, you can test
  /// whether the first `UInt8` value in a string's UTF-8 encoding is not
  /// equal to the first `UInt32` value in its Unicode scalar view:
  ///
  ///     let gameName = "Red Light, Green Light"
  ///     if let firstUTF8 = gameName.utf8.first,
  ///         let firstScalar = gameName.unicodeScalars.first?.value {
  ///         print("First code values are different: \(firstUTF8 != firstScalar)")
  ///     }
  ///     // Prints "First code values are different: false"
  ///
  /// - Parameters:
  ///   - lhs: An integer to compare.
  ///   - rhs: Another integer to compare.
  @_transparent
  public static func != <
    Other: BinaryInteger
  >(lhs: Self, rhs: Other) -> Bool {
    return !(lhs == rhs)
  }

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is less than that of the second argument.
  ///
  /// You can compare instances of any `BinaryInteger` types using the
  /// less-than operator (`<`), even if the two instances are of different
  /// types.
  ///
  /// - Parameters:
  ///   - lhs: An integer to compare.
  ///   - rhs: Another integer to compare.
  @_transparent
  public static func < <Other: BinaryInteger>(lhs: Self, rhs: Other) -> Bool {
    // Use bit pattern conversion to widen the comparand with smaller bit width.
    if Self.isSigned == Other.isSigned {
      return lhs.bitWidth >= rhs.bitWidth ?
        lhs < Self(truncatingIfNeeded: rhs) :
        Other(truncatingIfNeeded: lhs) < rhs
    }
    // If `Self` is signed but `Other` is unsigned, then we have to
    // be a little more careful about widening, since:
    // (1) a fixed-width signed type can't represent the largest values of
    //     a fixed-width unsigned type of equal bit width; and
    // (2) an unsigned type (obviously) can't represent a negative value.
    if Self.isSigned {
      return lhs.bitWidth > rhs.bitWidth ? // (1)
        lhs < Self(truncatingIfNeeded: rhs) :
        (lhs < (0 as Self) || Other(truncatingIfNeeded: lhs) < rhs) // (2)
    }
    // Analogous reasoning applies if `Other` is signed but `Self` is not.
    return lhs.bitWidth < rhs.bitWidth ?
      Other(truncatingIfNeeded: lhs) < rhs :
      (rhs > (0 as Other) && lhs < Self(truncatingIfNeeded: rhs))
  }

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is less than or equal to that of the second argument.
  ///
  /// You can compare instances of any `BinaryInteger` types using the
  /// less-than-or-equal-to operator (`<=`), even if the two instances are of
  /// different types.
  ///
  /// - Parameters:
  ///   - lhs: An integer to compare.
  ///   - rhs: Another integer to compare.
  @_transparent
  public static func <= <Other: BinaryInteger>(lhs: Self, rhs: Other) -> Bool {
    return !(rhs < lhs)
  }

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is greater than or equal to that of the second argument.
  ///
  /// You can compare instances of any `BinaryInteger` types using the
  /// greater-than-or-equal-to operator (`>=`), even if the two instances are
  /// of different types.
  ///
  /// - Parameters:
  ///   - lhs: An integer to compare.
  ///   - rhs: Another integer to compare.
  @_transparent
  public static func >= <Other: BinaryInteger>(lhs: Self, rhs: Other) -> Bool {
    return !(lhs < rhs)
  }

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is greater than that of the second argument.
  ///
  /// You can compare instances of any `BinaryInteger` types using the
  /// greater-than operator (`>`), even if the two instances are of different
  /// types.
  ///
  /// - Parameters:
  ///   - lhs: An integer to compare.
  ///   - rhs: Another integer to compare.
  @_transparent
  public static func > <Other: BinaryInteger>(lhs: Self, rhs: Other) -> Bool {
    return rhs < lhs
  }
}

//===----------------------------------------------------------------------===//
//===--- Ambiguity breakers -----------------------------------------------===//
//
// These two versions of the operators are not ordered with respect to one
// another, but the compiler choses the second one, and that results in infinite
// recursion.
//
//     <T: Comparable>(T, T) -> Bool
//     <T: BinaryInteger, U: BinaryInteger>(T, U) -> Bool
//
// so we define:
//
//     <T: BinaryInteger>(T, T) -> Bool
//
//===----------------------------------------------------------------------===//

extension BinaryInteger {
  @_transparent
  public static func != (lhs: Self, rhs: Self) -> Bool {
    return !(lhs == rhs)
  }

  @_transparent
  public static func <= (lhs: Self, rhs: Self) -> Bool {
    return !(rhs < lhs)
  }

  @_transparent
  public static func >= (lhs: Self, rhs: Self) -> Bool {
    return !(lhs < rhs)
  }

  @_transparent
  public static func > (lhs: Self, rhs: Self) -> Bool {
    return rhs < lhs
  }
}

#if !$Embedded
public typealias _LosslessStringConvertibleOrNone = LosslessStringConvertible
#else
public protocol _LosslessStringConvertibleOrNone {}
#endif

//===----------------------------------------------------------------------===//
//===--- FixedWidthInteger ------------------------------------------------===//
//===----------------------------------------------------------------------===//

/// An integer type that uses a fixed size for every instance.
///
/// The `FixedWidthInteger` protocol adds binary bitwise operations, bit
/// shifts, and overflow handling to the operations supported by the
/// `BinaryInteger` protocol.
///
/// Use the `FixedWidthInteger` protocol as a constraint or extension point
/// when writing operations that depend on bit shifting, performing bitwise
/// operations, catching overflows, or having access to the maximum or minimum
/// representable value of a type. For example, the following code provides a
/// `binaryString` property on every fixed-width integer that represents the
/// number's binary representation, split into 8-bit chunks.
///
///     extension FixedWidthInteger {
///         var binaryString: String {
///             var result: [String] = []
///             for i in 0..<(Self.bitWidth / 8) {
///                 let byte = UInt8(truncatingIfNeeded: self >> (i * 8))
///                 let byteString = String(byte, radix: 2)
///                 let padding = String(repeating: "0",
///                                      count: 8 - byteString.count)
///                 result.append(padding + byteString)
///             }
///             return "0b" + result.reversed().joined(separator: "_")
///         }
///     }
///
///     print(Int16.max.binaryString)
///     // Prints "0b01111111_11111111"
///     print((101 as UInt8).binaryString)
///     // Prints "0b01100101"
///
/// The `binaryString` implementation uses the static `bitWidth` property and
/// the right shift operator (`>>`), both of which are available to any type
/// that conforms to the `FixedWidthInteger` protocol.
///
/// The next example declares a generic `squared` function, which accepts an
/// instance `x` of any fixed-width integer type. The function uses the
/// `multipliedReportingOverflow(by:)` method to multiply `x` by itself and
/// check whether the result is too large to represent in the same type.
///
///     func squared<T: FixedWidthInteger>(_ x: T) -> T? {
///         let (result, overflow) = x.multipliedReportingOverflow(by: x)
///         if overflow {
///             return nil
///         }
///         return result
///     }
///
///     let (x, y): (Int8, Int8) = (9, 123)
///     print(squared(x))
///     // Prints "Optional(81)"
///     print(squared(y))
///     // Prints "nil"
///
/// Conforming to the FixedWidthInteger Protocol
/// ============================================
///
/// To make your own custom type conform to the `FixedWidthInteger` protocol,
/// declare the required initializers, properties, and methods. The required
/// methods that are suffixed with `ReportingOverflow` serve as the
/// customization points for arithmetic operations. When you provide just those
/// methods, the standard library provides default implementations for all
/// other arithmetic methods and operators.
public protocol FixedWidthInteger: BinaryInteger, _LosslessStringConvertibleOrNone
where Magnitude: FixedWidthInteger & UnsignedInteger,
      Stride: FixedWidthInteger & SignedInteger {
  /// The number of bits used for the underlying binary representation of
  /// values of this type.
  ///
  /// An unsigned, fixed-width integer type can represent values from 0 through
  /// `(2 ** bitWidth) - 1`, where `**` is exponentiation. A signed,
  /// fixed-width integer type can represent values from
  /// `-(2 ** (bitWidth - 1))` through `(2 ** (bitWidth - 1)) - 1`. For example,
  /// the `Int8` type has a `bitWidth` value of 8 and can store any integer in
  /// the range `-128...127`.
  static var bitWidth: Int { get }

  /// The maximum representable integer in this type.
  ///
  /// For unsigned integer types, this value is `(2 ** bitWidth) - 1`, where
  /// `**` is exponentiation. For signed integer types, this value is
  /// `(2 ** (bitWidth - 1)) - 1`.
  static var max: Self { get }

  /// The minimum representable integer in this type.
  ///
  /// For unsigned integer types, this value is always `0`. For signed integer
  /// types, this value is `-(2 ** (bitWidth - 1))`, where `**` is
  /// exponentiation.
  static var min: Self { get }

  /// Returns the sum of this value and the given value, along with a Boolean
  /// value indicating whether overflow occurred in the operation.
  ///
  /// - Parameter rhs: The value to add to this value.
  /// - Returns: A tuple containing the result of the addition along with a
  ///   Boolean value indicating whether overflow occurred. If the `overflow`
  ///   component is `false`, the `partialValue` component contains the entire
  ///   sum. If the `overflow` component is `true`, an overflow occurred and
  ///   the `partialValue` component contains the truncated sum of this value
  ///   and `rhs`.
  func addingReportingOverflow(
    _ rhs: Self
  ) -> (partialValue: Self, overflow: Bool)

  /// Returns the difference obtained by subtracting the given value from this
  /// value, along with a Boolean value indicating whether overflow occurred in
  /// the operation.
  ///
  /// - Parameter rhs: The value to subtract from this value.
  /// - Returns: A tuple containing the result of the subtraction along with a
  ///   Boolean value indicating whether overflow occurred. If the `overflow`
  ///   component is `false`, the `partialValue` component contains the entire
  ///   difference. If the `overflow` component is `true`, an overflow occurred
  ///   and the `partialValue` component contains the truncated result of `rhs`
  ///   subtracted from this value.
  func subtractingReportingOverflow(
    _ rhs: Self
  ) -> (partialValue: Self, overflow: Bool)

  /// Returns the product of this value and the given value, along with a
  /// Boolean value indicating whether overflow occurred in the operation.
  ///
  /// - Parameter rhs: The value to multiply by this value.
  /// - Returns: A tuple containing the result of the multiplication along with
  ///   a Boolean value indicating whether overflow occurred. If the `overflow`
  ///   component is `false`, the `partialValue` component contains the entire
  ///   product. If the `overflow` component is `true`, an overflow occurred and
  ///   the `partialValue` component contains the truncated product of this
  ///   value and `rhs`.
  func multipliedReportingOverflow(
    by rhs: Self
  ) -> (partialValue: Self, overflow: Bool)

  /// Returns the quotient obtained by dividing this value by the given value,
  /// along with a Boolean value indicating whether overflow occurred in the
  /// operation.
  ///
  /// Dividing by zero is not an error when using this method. For a value `x`,
  /// the result of `x.dividedReportingOverflow(by: 0)` is `(x, true)`.
  ///
  /// - Parameter rhs: The value to divide this value by.
  /// - Returns: A tuple containing the result of the division along with a
  ///   Boolean value indicating whether overflow occurred. If the `overflow`
  ///   component is `false`, the `partialValue` component contains the entire
  ///   quotient. If the `overflow` component is `true`, an overflow occurred
  ///   and the `partialValue` component contains either the truncated quotient
  ///   or, if the quotient is undefined, the dividend.
  func dividedReportingOverflow(
    by rhs: Self
  ) -> (partialValue: Self, overflow: Bool)

  /// Returns the remainder after dividing this value by the given value, along
  /// with a Boolean value indicating whether overflow occurred during division.
  ///
  /// Dividing by zero is not an error when using this method. For a value `x`,
  /// the result of `x.remainderReportingOverflow(dividingBy: 0)` is
  /// `(x, true)`.
  ///
  /// - Parameter rhs: The value to divide this value by.
  /// - Returns: A tuple containing the result of the operation along with a
  ///   Boolean value indicating whether overflow occurred. If the `overflow`
  ///   component is `false`, the `partialValue` component contains the entire
  ///   remainder. If the `overflow` component is `true`, an overflow occurred
  ///   during division and the `partialValue` component contains either the
  ///   entire remainder or, if the remainder is undefined, the dividend.
  func remainderReportingOverflow(
    dividingBy rhs: Self
  ) -> (partialValue: Self, overflow: Bool)

  /// Returns a tuple containing the high and low parts of the result of
  /// multiplying this value by the given value.
  ///
  /// Use this method to calculate the full result of a product that would
  /// otherwise overflow. Unlike traditional truncating multiplication, the
  /// `multipliedFullWidth(by:)` method returns a tuple containing both the
  /// `high` and `low` parts of the product of this value and `other`. The
  /// following example uses this method to multiply two `Int8` values that
  /// normally overflow when multiplied:
  ///
  ///     let x: Int8 = 48
  ///     let y: Int8 = -40
  ///     let result = x.multipliedFullWidth(by: y)
  ///     // result.high == -8
  ///     // result.low  == 128
  ///
  /// The product of `x` and `y` is `-1920`, which is too large to represent in
  /// an `Int8` instance. The `high` and `low` components of the `result` value
  /// represent `-1920` when concatenated to form a double-width integer; that
  /// is, using `result.high` as the high byte and `result.low` as the low byte
  /// of an `Int16` instance.
  ///
  ///     let z = Int16(result.high) << 8 | Int16(result.low)
  ///     // z == -1920
  ///
  /// - Parameter other: The value to multiply this value by.
  /// - Returns: A tuple containing the high and low parts of the result of
  ///   multiplying this value and `other`.
  func multipliedFullWidth(by other: Self) -> (high: Self, low: Self.Magnitude)

  /// Returns a tuple containing the quotient and remainder obtained by dividing
  /// the given value by this value.
  ///
  /// The resulting quotient must be representable within the bounds of the
  /// type. If the quotient is too large to represent in the type, a runtime
  /// error may occur.
  ///
  /// The following example divides a value that is too large to be represented
  /// using a single `Int` instance by another `Int` value. Because the quotient
  /// is representable as an `Int`, the division succeeds.
  ///
  ///     // 'dividend' represents the value 0x506f70652053616e74612049494949
  ///     let dividend = (22640526660490081, 7959093232766896457 as UInt)
  ///     let divisor = 2241543570477705381
  ///
  ///     let (quotient, remainder) = divisor.dividingFullWidth(dividend)
  ///     // quotient == 186319822866995413
  ///     // remainder == 0
  ///
  /// - Parameter dividend: A tuple containing the high and low parts of a
  ///   double-width integer.
  /// - Returns: A tuple containing the quotient and remainder obtained by
  ///   dividing `dividend` by this value.
  func dividingFullWidth(_ dividend: (high: Self, low: Self.Magnitude))
    -> (quotient: Self, remainder: Self)

  init(_truncatingBits bits: UInt)

  /// The number of bits equal to 1 in this value's binary representation.
  ///
  /// For example, in a fixed-width integer type with a `bitWidth` value of 8,
  /// the number *31* has five bits equal to *1*.
  ///
  ///     let x: Int8 = 0b0001_1111
  ///     // x == 31
  ///     // x.nonzeroBitCount == 5
  var nonzeroBitCount: Int { get }

  /// The number of leading zeros in this value's binary representation.
  ///
  /// For example, in a fixed-width integer type with a `bitWidth` value of 8,
  /// the number *31* has three leading zeros.
  ///
  ///     let x: Int8 = 0b0001_1111
  ///     // x == 31
  ///     // x.leadingZeroBitCount == 3
  ///
  /// If the value is zero, then `leadingZeroBitCount` is equal to `bitWidth`.
  var leadingZeroBitCount: Int { get }

  /// Creates an integer from its big-endian representation, changing the byte
  /// order if necessary.
  ///
  /// - Parameter value: A value to use as the big-endian representation of the
  ///   new integer.
  init(bigEndian value: Self)

  /// Creates an integer from its little-endian representation, changing the
  /// byte order if necessary.
  ///
  /// - Parameter value: A value to use as the little-endian representation of
  ///   the new integer.
  init(littleEndian value: Self)

  /// The big-endian representation of this integer.
  ///
  /// If necessary, the byte order of this value is reversed from the typical
  /// byte order of this integer type. On a big-endian platform, for any
  /// integer `x`, `x == x.bigEndian`.
  var bigEndian: Self { get }

  /// The little-endian representation of this integer.
  ///
  /// If necessary, the byte order of this value is reversed from the typical
  /// byte order of this integer type. On a little-endian platform, for any
  /// integer `x`, `x == x.littleEndian`.
  var littleEndian: Self { get }

  /// A representation of this integer with the byte order swapped.
  var byteSwapped: Self { get }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the right, masking the shift amount to the
  /// type's bit width.
  ///
  /// Use the masking right shift operator (`&>>`) when you need to perform a
  /// shift and are sure that the shift amount is in the range
  /// `0..<lhs.bitWidth`. Before shifting, the masking right shift operator
  /// masks the shift to this range. The shift is performed using this masked
  /// value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x &>> 2
  ///     // y == 7                         // 0b00000111
  ///
  /// However, if you use `8` as the shift amount, the method first masks the
  /// shift amount to zero, and then performs the shift, resulting in no change
  /// to the original value.
  ///
  ///     let z = x &>> 8
  ///     // z == 30                        // 0b00011110
  ///
  /// If the bit width of the shifted integer type is a power of two, masking
  /// is performed using a bitmask; otherwise, masking is performed using a
  /// modulo operation.
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  static func &>>(lhs: Self, rhs: Self) -> Self

  /// Calculates the result of shifting a value's binary representation the
  /// specified number of digits to the right, masking the shift amount to the
  /// type's bit width, and stores the result in the left-hand-side variable.
  ///
  /// The `&>>=` operator performs a *masking shift*, where the value passed as
  /// `rhs` is masked to produce a value in the range `0..<lhs.bitWidth`. The
  /// shift is performed using this masked value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     var x: UInt8 = 30                 // 0b00011110
  ///     x &>>= 2
  ///     // x == 7                         // 0b00000111
  ///
  /// However, if you use `19` as `rhs`, the operation first bitmasks `rhs` to
  /// `3`, and then uses that masked value as the number of bits to shift `lhs`.
  ///
  ///     var y: UInt8 = 30                 // 0b00011110
  ///     y &>>= 19
  ///     // y == 3                         // 0b00000011
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  static func &>>=(lhs: inout Self, rhs: Self)

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left, masking the shift amount to the
  /// type's bit width.
  ///
  /// Use the masking left shift operator (`&<<`) when you need to perform a
  /// shift and are sure that the shift amount is in the range
  /// `0..<lhs.bitWidth`. Before shifting, the masking left shift operator
  /// masks the shift to this range. The shift is performed using this masked
  /// value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x &<< 2
  ///     // y == 120                       // 0b01111000
  ///
  /// However, if you use `8` as the shift amount, the method first masks the
  /// shift amount to zero, and then performs the shift, resulting in no change
  /// to the original value.
  ///
  ///     let z = x &<< 8
  ///     // z == 30                        // 0b00011110
  ///
  /// If the bit width of the shifted integer type is a power of two, masking
  /// is performed using a bitmask; otherwise, masking is performed using a
  /// modulo operation.
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  static func &<<(lhs: Self, rhs: Self) -> Self

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left, masking the shift amount to the
  /// type's bit width, and stores the result in the left-hand-side variable.
  ///
  /// The `&<<=` operator performs a *masking shift*, where the value used as
  /// `rhs` is masked to produce a value in the range `0..<lhs.bitWidth`. The
  /// shift is performed using this masked value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     var x: UInt8 = 30                 // 0b00011110
  ///     x &<<= 2
  ///     // x == 120                       // 0b01111000
  ///
  /// However, if you pass `19` as `rhs`, the method first bitmasks `rhs` to
  /// `3`, and then uses that masked value as the number of bits to shift `lhs`.
  ///
  ///     var y: UInt8 = 30                 // 0b00011110
  ///     y &<<= 19
  ///     // y == 240                       // 0b11110000
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  static func &<<=(lhs: inout Self, rhs: Self)
  
  /// Returns the product of the two given values, wrapping the result in case
  /// of any overflow.
  ///
  /// The overflow multiplication operator (`&*`) discards any bits that
  /// overflow the fixed width of the integer type. In the following example,
  /// the product of `10` and `50` is greater than the maximum representable
  /// `Int8` value, so the result is the partial value after discarding the
  /// overflowing bits.
  ///
  ///     let x: Int8 = 10 &* 5
  ///     // x == 50
  ///     let y: Int8 = 10 &* 50
  ///     // y == -12 (after overflow)
  ///
  /// For more about arithmetic with overflow operators, see [Overflow
  /// Operators][overflow] in *[The Swift Programming Language][tspl]*.
  ///
  /// [overflow]: https://docs.swift.org/swift-book/LanguageGuide/AdvancedOperators.html#ID37
  /// [tspl]: https://docs.swift.org/swift-book/
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  @available(SwiftStdlib 6.0, *)
  static func &*(lhs: Self, rhs: Self) -> Self
}

extension FixedWidthInteger {
  /// The number of bits in the binary representation of this value.
  @inlinable
  public var bitWidth: Int { return Self.bitWidth }

  @inlinable
  public func _binaryLogarithm() -> Int {
    _precondition(self > (0 as Self))
    return Self.bitWidth &- (leadingZeroBitCount &+ 1)
  }

  /// Creates an integer from its little-endian representation, changing the
  /// byte order if necessary.
  ///
  /// - Parameter value: A value to use as the little-endian representation of
  ///   the new integer.
  @inlinable
  public init(littleEndian value: Self) {
#if _endian(little)
    self = value
#else
    self = value.byteSwapped
#endif
  }

  /// Creates an integer from its big-endian representation, changing the byte
  /// order if necessary.
  ///
  /// - Parameter value: A value to use as the big-endian representation of the
  ///   new integer.
  @inlinable
  public init(bigEndian value: Self) {
#if _endian(big)
    self = value
#else
    self = value.byteSwapped
#endif
  }

  /// The little-endian representation of this integer.
  ///
  /// If necessary, the byte order of this value is reversed from the typical
  /// byte order of this integer type. On a little-endian platform, for any
  /// integer `x`, `x == x.littleEndian`.
  @inlinable
  public var littleEndian: Self {
#if _endian(little)
    return self
#else
    return byteSwapped
#endif
  }

  /// The big-endian representation of this integer.
  ///
  /// If necessary, the byte order of this value is reversed from the typical
  /// byte order of this integer type. On a big-endian platform, for any
  /// integer `x`, `x == x.bigEndian`.
  @inlinable
  public var bigEndian: Self {
#if _endian(big)
    return self
#else
    return byteSwapped
#endif
  }
  
  // Default implementation of multipliedFullWidth.
  //
  // This implementation is mainly intended for [U]Int64 on 32b platforms. It
  // will not be especially efficient for other types that do not provide their
  // own implementation, but neither will it be catastrophically bad. It can
  // surely be improved on even for Int64, but that is mostly an optimization
  // problem; the basic algorithm here gives the compiler all the information
  // that it needs to generate efficient code.
  @_alwaysEmitIntoClient
  public func multipliedFullWidth(by other: Self) -> (high: Self, low: Magnitude) {
    // We define a utility function for splitting an integer into high and low
    // halves. Note that the low part is always unsigned, while the high part
    // matches the signedness of the input type. Both result types are the
    // full width of the original number; this may be surprising at first, but
    // there are two reasons for it:
    //
    // - we're going to use these as inputs to a multiplication operation, and
    //   &* is quite a bit less verbose than `multipliedFullWidth`, so it makes
    //   the rest of the code in this function somewhat easier to read.
    //
    // - there's no "half width type" that we can get at from this generic
    //   context, so there's not really another option anyway.
    //
    // Fortunately, the compiler is pretty good about propagating the necessary
    // information to optimize away unnecessary arithmetic.
    func split<T: FixedWidthInteger>(_ x: T) -> (high: T, low: T.Magnitude) {
      let n = T.bitWidth/2
      return (x >> n, T.Magnitude(truncatingIfNeeded: x) & ((1 &<< n) &- 1))
    }
    // Split `self` and `other` into high and low parts, compute the partial
    // products carrying high words in as we go. We use the wrapping operators
    // and `truncatingIfNeeded` inits purely as an optimization hint to the
    // compiler; none of these operations will ever wrap due to the constraints
    // on the arithmetic. The bounds are documented before each line for signed
    // types. For unsigned types, the bounds are much more well known and
    // easier to derive, so I haven't bothered to document them here, but they
    // all boil down to the fact that a*b + c + d cannot overflow a double-
    // width result with unsigned a, b, c, d.
    let (x1, x0) = split(self)
    let (y1, y0) = split(other)
    // If B is 2^bitWidth/2, x0 and y0 are in 0 ... B-1, so their product is
    // in 0 ... B^2-2B+1. For further analysis, we'll need the fact that
    // the high word is in 0 ... B-2.
    let p00 = x0 &* y0
    // x1 is in -B/2 ... B/2-1, so the product x1*y0 is in
    // -(B^2-B)/2 ... (B^2-3B+2)/2; after adding the high word of p00, the
    // result is in -(B^2-B)/2 ... (B^2-B-2)/2.
    let p01 = x1 &* Self(y0) &+ Self(split(p00).high)
    // The previous analysis holds for this product as well, and the sum is
    // in -(B^2-B)/2 ... (B^2-B)/2.
    let p10 = Self(x0) &* y1 &+ Self(split(p01).low)
    // No analysis is necessary for this term, because we know the product as
    // a whole cannot overflow, and this term is the final high word of the
    // product.
    let p11 = x1 &* y1 &+ split(p01).high &+ split(p10).high
    // Now we only need to assemble the low word of the product.
    return (p11, split(p10).low << (bitWidth/2) | split(p00).low)
  }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the right, masking the shift amount to the
  /// type's bit width.
  ///
  /// Use the masking right shift operator (`&>>`) when you need to perform a
  /// shift and are sure that the shift amount is in the range
  /// `0..<lhs.bitWidth`. Before shifting, the masking right shift operator
  /// masks the shift to this range. The shift is performed using this masked
  /// value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x &>> 2
  ///     // y == 7                         // 0b00000111
  ///
  /// However, if you use `8` as the shift amount, the method first masks the
  /// shift amount to zero, and then performs the shift, resulting in no change
  /// to the original value.
  ///
  ///     let z = x &>> 8
  ///     // z == 30                        // 0b00011110
  ///
  /// If the bit width of the shifted integer type is a power of two, masking
  /// is performed using a bitmask; otherwise, masking is performed using a
  /// modulo operation.
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func &>> (lhs: Self, rhs: Self) -> Self {
    var lhs = lhs
    lhs &>>= rhs
    return lhs
  }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the right, masking the shift amount to the
  /// type's bit width.
  ///
  /// Use the masking right shift operator (`&>>`) when you need to perform a
  /// shift and are sure that the shift amount is in the range
  /// `0..<lhs.bitWidth`. Before shifting, the masking right shift operator
  /// masks the shift to this range. The shift is performed using this masked
  /// value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x &>> 2
  ///     // y == 7                         // 0b00000111
  ///
  /// However, if you use `8` as the shift amount, the method first masks the
  /// shift amount to zero, and then performs the shift, resulting in no change
  /// to the original value.
  ///
  ///     let z = x &>> 8
  ///     // z == 30                        // 0b00011110
  ///
  /// If the bit width of the shifted integer type is a power of two, masking
  /// is performed using a bitmask; otherwise, masking is performed using a
  /// modulo operation.
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func &>> <
    Other: BinaryInteger
  >(lhs: Self, rhs: Other) -> Self {
    return lhs &>> Self(truncatingIfNeeded: rhs)
  }

  /// Calculates the result of shifting a value's binary representation the
  /// specified number of digits to the right, masking the shift amount to the
  /// type's bit width, and stores the result in the left-hand-side variable.
  ///
  /// The `&>>=` operator performs a *masking shift*, where the value passed as
  /// `rhs` is masked to produce a value in the range `0..<lhs.bitWidth`. The
  /// shift is performed using this masked value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     var x: UInt8 = 30                 // 0b00011110
  ///     x &>>= 2
  ///     // x == 7                         // 0b00000111
  ///
  /// However, if you use `19` as `rhs`, the operation first bitmasks `rhs` to
  /// `3`, and then uses that masked value as the number of bits to shift `lhs`.
  ///
  ///     var y: UInt8 = 30                 // 0b00011110
  ///     y &>>= 19
  ///     // y == 3                         // 0b00000011
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func &>>= <
    Other: BinaryInteger
  >(lhs: inout Self, rhs: Other) {
    lhs = lhs &>> rhs
  }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left, masking the shift amount to the
  /// type's bit width.
  ///
  /// Use the masking left shift operator (`&<<`) when you need to perform a
  /// shift and are sure that the shift amount is in the range
  /// `0..<lhs.bitWidth`. Before shifting, the masking left shift operator
  /// masks the shift to this range. The shift is performed using this masked
  /// value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x &<< 2
  ///     // y == 120                       // 0b01111000
  ///
  /// However, if you use `8` as the shift amount, the method first masks the
  /// shift amount to zero, and then performs the shift, resulting in no change
  /// to the original value.
  ///
  ///     let z = x &<< 8
  ///     // z == 30                        // 0b00011110
  ///
  /// If the bit width of the shifted integer type is a power of two, masking
  /// is performed using a bitmask; otherwise, masking is performed using a
  /// modulo operation.
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func &<< (lhs: Self, rhs: Self) -> Self {
    var lhs = lhs
    lhs &<<= rhs
    return lhs
  }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left, masking the shift amount to the
  /// type's bit width.
  ///
  /// Use the masking left shift operator (`&<<`) when you need to perform a
  /// shift and are sure that the shift amount is in the range
  /// `0..<lhs.bitWidth`. Before shifting, the masking left shift operator
  /// masks the shift to this range. The shift is performed using this masked
  /// value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x &<< 2
  ///     // y == 120                       // 0b01111000
  ///
  /// However, if you use `8` as the shift amount, the method first masks the
  /// shift amount to zero, and then performs the shift, resulting in no change
  /// to the original value.
  ///
  ///     let z = x &<< 8
  ///     // z == 30                        // 0b00011110
  ///
  /// If the bit width of the shifted integer type is a power of two, masking
  /// is performed using a bitmask; otherwise, masking is performed using a
  /// modulo operation.
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func &<< <
    Other: BinaryInteger
  >(lhs: Self, rhs: Other) -> Self {
    return lhs &<< Self(truncatingIfNeeded: rhs)
  }

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left, masking the shift amount to the
  /// type's bit width, and stores the result in the left-hand-side variable.
  ///
  /// The `&<<=` operator performs a *masking shift*, where the value used as
  /// `rhs` is masked to produce a value in the range `0..<lhs.bitWidth`. The
  /// shift is performed using this masked value.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the shift amount requires no masking.
  ///
  ///     var x: UInt8 = 30                 // 0b00011110
  ///     x &<<= 2
  ///     // x == 120                       // 0b01111000
  ///
  /// However, if you pass `19` as `rhs`, the method first bitmasks `rhs` to
  /// `3`, and then uses that masked value as the number of bits to shift `lhs`.
  ///
  ///     var y: UInt8 = 30                 // 0b00011110
  ///     y &<<= 19
  ///     // y == 240                       // 0b11110000
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left. If `rhs` is
  ///     outside the range `0..<lhs.bitWidth`, it is masked to produce a
  ///     value within that range.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func &<<= <
    Other: BinaryInteger
  >(lhs: inout Self, rhs: Other) {
    lhs = lhs &<< rhs
  }
}

extension FixedWidthInteger {
  /// Returns a random value within the specified range, using the given
  /// generator as a source for randomness.
  ///
  /// Use this method to generate an integer within a specific range when you
  /// are using a custom random number generator. This example creates three
  /// new values in the range `1..<100`.
  ///
  ///     for _ in 1...3 {
  ///         print(Int.random(in: 1..<100, using: &myGenerator))
  ///     }
  ///     // Prints "7"
  ///     // Prints "44"
  ///     // Prints "21"
  ///
  /// - Note: The algorithm used to create random values may change in a future
  ///   version of Swift. If you're passing a generator that results in the
  ///   same sequence of integer values each time you run your program, that
  ///   sequence may change when your program is compiled using a different
  ///   version of Swift.
  ///
  /// - Parameters:
  ///   - range: The range in which to create a random value.
  ///     `range` must not be empty.
  ///   - generator: The random number generator to use when creating the
  ///     new random value.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random<T: RandomNumberGenerator>(
    in range: Range<Self>,
    using generator: inout T
  ) -> Self {
    _precondition(
      !range.isEmpty,
      "Can't get random value with an empty range"
    )

    // Compute delta, the distance between the lower and upper bounds. This
    // value may not representable by the type Bound if Bound is signed, but
    // is always representable as Bound.Magnitude.
    let delta = Magnitude(truncatingIfNeeded: range.upperBound &- range.lowerBound)
    // The mathematical result we want is lowerBound plus a random value in
    // 0 ..< delta. We need to be slightly careful about how we do this
    // arithmetic; the Bound type cannot generally represent the random value,
    // so we use a wrapping addition on Bound.Magnitude. This will often
    // overflow, but produces the correct bit pattern for the result when
    // converted back to Bound.
    return Self(truncatingIfNeeded:
      Magnitude(truncatingIfNeeded: range.lowerBound) &+
      generator.next(upperBound: delta)
    )
  }
  
  /// Returns a random value within the specified range.
  ///
  /// Use this method to generate an integer within a specific range. This
  /// example creates three new values in the range `1..<100`.
  ///
  ///     for _ in 1...3 {
  ///         print(Int.random(in: 1..<100))
  ///     }
  ///     // Prints "53"
  ///     // Prints "64"
  ///     // Prints "5"
  ///
  /// This method is equivalent to calling the version that takes a generator,
  /// passing in the system's default random generator.
  ///
  /// - Parameter range: The range in which to create a random value.
  ///   `range` must not be empty.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random(in range: Range<Self>) -> Self {
    var g = SystemRandomNumberGenerator()
    return Self.random(in: range, using: &g)
  }

  /// Returns a random value within the specified range, using the given
  /// generator as a source for randomness.
  ///
  /// Use this method to generate an integer within a specific range when you
  /// are using a custom random number generator. This example creates three
  /// new values in the range `1...100`.
  ///
  ///     for _ in 1...3 {
  ///         print(Int.random(in: 1...100, using: &myGenerator))
  ///     }
  ///     // Prints "7"
  ///     // Prints "44"
  ///     // Prints "21"
  ///
  /// - Parameters:
  ///   - range: The range in which to create a random value.
  ///   - generator: The random number generator to use when creating the
  ///     new random value.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random<T: RandomNumberGenerator>(
    in range: ClosedRange<Self>,
    using generator: inout T
  ) -> Self {
    // Compute delta, the distance between the lower and upper bounds. This
    // value may not representable by the type Bound if Bound is signed, but
    // is always representable as Bound.Magnitude.
    var delta = Magnitude(truncatingIfNeeded: range.upperBound &- range.lowerBound)
    // Subtle edge case: if the range is the whole set of representable values,
    // then adding one to delta to account for a closed range will overflow.
    // If we used &+ instead, the result would be zero, which isn't helpful,
    // so we actually need to handle this case separately.
    if delta == Magnitude.max {
      return Self(truncatingIfNeeded: generator.next() as Magnitude)
    }
    // Need to widen delta to account for the right-endpoint of a closed range.
    delta += 1
    // The mathematical result we want is lowerBound plus a random value in
    // 0 ..< delta. We need to be slightly careful about how we do this
    // arithmetic; the Bound type cannot generally represent the random value,
    // so we use a wrapping addition on Bound.Magnitude. This will often
    // overflow, but produces the correct bit pattern for the result when
    // converted back to Bound.
    return Self(truncatingIfNeeded:
      Magnitude(truncatingIfNeeded: range.lowerBound) &+
      generator.next(upperBound: delta)
    )
  }
  
  /// Returns a random value within the specified range.
  ///
  /// Use this method to generate an integer within a specific range. This
  /// example creates three new values in the range `1...100`.
  ///
  ///     for _ in 1...3 {
  ///         print(Int.random(in: 1...100))
  ///     }
  ///     // Prints "53"
  ///     // Prints "64"
  ///     // Prints "5"
  ///
  /// This method is equivalent to calling `random(in:using:)`, passing in the
  /// system's default random generator.
  ///
  /// - Parameter range: The range in which to create a random value.
  /// - Returns: A random value within the bounds of `range`.
  @inlinable
  public static func random(in range: ClosedRange<Self>) -> Self {
    var g = SystemRandomNumberGenerator()
    return Self.random(in: range, using: &g)
  }
}

//===----------------------------------------------------------------------===//
//===--- Operators on FixedWidthInteger -----------------------------------===//
//===----------------------------------------------------------------------===//

extension FixedWidthInteger {
  /// Returns the inverse of the bits set in the argument.
  ///
  /// The bitwise NOT operator (`~`) is a prefix operator that returns a value
  /// in which all the bits of its argument are flipped: Bits that are `1` in
  /// the argument are `0` in the result, and bits that are `0` in the argument
  /// are `1` in the result. This is equivalent to the inverse of a set. For
  /// example:
  ///
  ///     let x: UInt8 = 5        // 0b00000101
  ///     let notX = ~x           // 0b11111010
  ///
  /// Performing a bitwise NOT operation on 0 returns a value with every bit
  /// set to `1`.
  ///
  ///     let allOnes = ~UInt8.min   // 0b11111111
  ///
  /// - Complexity: O(1).
  @_transparent
  public static prefix func ~ (x: Self) -> Self {
    return 0 &- x &- 1
  }

//===----------------------------------------------------------------------===//
//=== "Smart right shift", supporting overshifts and negative shifts ------===//
//===----------------------------------------------------------------------===//

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the right.
  ///
  /// The `>>` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a left shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*. An overshift results in `-1` for a
  ///   negative value of `lhs` or `0` for a nonnegative value.
  /// - Using any other value for `rhs` performs a right shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted right by two bits.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x >> 2
  ///     // y == 7                         // 0b00000111
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     let z = x >> 11
  ///     // z == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a left shift
  /// using `abs(rhs)`.
  ///
  ///     let a = x >> -3
  ///     // a == 240                       // 0b11110000
  ///     let b = x << 3
  ///     // b == 240                       // 0b11110000
  ///
  /// Right shift operations on negative values "fill in" the high bits with
  /// ones instead of zeros.
  ///
  ///     let q: Int8 = -30                 // 0b11100010
  ///     let r = q >> 2
  ///     // r == -8                        // 0b11111000
  ///
  ///     let s = q >> 11
  ///     // s == -1                        // 0b11111111
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the right.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func >> <
    Other: BinaryInteger
  >(lhs: Self, rhs: Other) -> Self {
    var lhs = lhs
    _nonMaskingRightShiftGeneric(&lhs, rhs)
    return lhs
  }

  @_transparent
  @_semantics("optimize.sil.specialize.generic.partial.never")
  public static func >>= <
    Other: BinaryInteger
  >(lhs: inout Self, rhs: Other) {
    _nonMaskingRightShiftGeneric(&lhs, rhs)
  }

  @_transparent
  public static func _nonMaskingRightShiftGeneric <
    Other: BinaryInteger
  >(_ lhs: inout Self, _ rhs: Other) {
    let shift = rhs < -Self.bitWidth ? -Self.bitWidth
                : rhs > Self.bitWidth ? Self.bitWidth
                : Int(rhs)
    lhs = _nonMaskingRightShift(lhs, shift)
  }

  @_transparent
  public static func _nonMaskingRightShift(_ lhs: Self, _ rhs: Int) -> Self {
    let overshiftR = Self.isSigned ? lhs &>> (Self.bitWidth - 1) : 0
    let overshiftL: Self = 0
    if _fastPath(rhs >= 0) {
      if _fastPath(rhs < Self.bitWidth) {
        return lhs &>> Self(truncatingIfNeeded: rhs)
      }
      return overshiftR
    }

    if _slowPath(rhs <= -Self.bitWidth) {
      return overshiftL
    }
    return lhs &<< -rhs
  }

//===----------------------------------------------------------------------===//
//=== "Smart left shift", supporting overshifts and negative shifts -------===//
//===----------------------------------------------------------------------===//

  /// Returns the result of shifting a value's binary representation the
  /// specified number of digits to the left.
  ///
  /// The `<<` operator performs a *smart shift*, which defines a result for a
  /// shift of any value.
  ///
  /// - Using a negative value for `rhs` performs a right shift using
  ///   `abs(rhs)`.
  /// - Using a value for `rhs` that is greater than or equal to the bit width
  ///   of `lhs` is an *overshift*, resulting in zero.
  /// - Using any other value for `rhs` performs a left shift on `lhs` by that
  ///   amount.
  ///
  /// The following example defines `x` as an instance of `UInt8`, an 8-bit,
  /// unsigned integer type. If you use `2` as the right-hand-side value in an
  /// operation on `x`, the value is shifted left by two bits.
  ///
  ///     let x: UInt8 = 30                 // 0b00011110
  ///     let y = x << 2
  ///     // y == 120                       // 0b01111000
  ///
  /// If you use `11` as `rhs`, `x` is overshifted such that all of its bits
  /// are set to zero.
  ///
  ///     let z = x << 11
  ///     // z == 0                         // 0b00000000
  ///
  /// Using a negative value as `rhs` is the same as performing a right shift
  /// with `abs(rhs)`.
  ///
  ///     let a = x << -3
  ///     // a == 3                         // 0b00000011
  ///     let b = x >> 3
  ///     // b == 3                         // 0b00000011
  ///
  /// - Parameters:
  ///   - lhs: The value to shift.
  ///   - rhs: The number of bits to shift `lhs` to the left.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @_transparent
  public static func << <
    Other: BinaryInteger
  >(lhs: Self, rhs: Other) -> Self {
    var lhs = lhs
    _nonMaskingLeftShiftGeneric(&lhs, rhs)
    return lhs
  }

  @_transparent
  @_semantics("optimize.sil.specialize.generic.partial.never")
  public static func <<= <
    Other: BinaryInteger
  >(lhs: inout Self, rhs: Other) {
    _nonMaskingLeftShiftGeneric(&lhs, rhs)
  }

  @_transparent
  public static func _nonMaskingLeftShiftGeneric <
    Other: BinaryInteger
  >(_ lhs: inout Self, _ rhs: Other) {
    let shift = rhs < -Self.bitWidth ? -Self.bitWidth
                : rhs > Self.bitWidth ? Self.bitWidth
                : Int(rhs)
    lhs = _nonMaskingLeftShift(lhs, shift)
  }

  @_transparent
  public static func _nonMaskingLeftShift(_ lhs: Self, _ rhs: Int) -> Self {
    let overshiftR = Self.isSigned ? lhs &>> (Self.bitWidth - 1) : 0
    let overshiftL: Self = 0
    if _fastPath(rhs >= 0) {
      if _fastPath(rhs < Self.bitWidth) {
        return lhs &<< Self(truncatingIfNeeded: rhs)
      }
      return overshiftL
    }

    if _slowPath(rhs <= -Self.bitWidth) {
      return overshiftR
    }
    return lhs &>> -rhs
  }
}

extension FixedWidthInteger {
  @inlinable
  @_semantics("optimize.sil.specialize.generic.partial.never")
  public // @testable
  static func _convert<Source: BinaryFloatingPoint>(
    from source: Source
  ) -> (value: Self?, exact: Bool) {
    guard _fastPath(!source.isZero) else { return (0, true) }
    guard _fastPath(source.isFinite) else { return (nil, false) }
    guard Self.isSigned || source > -1 else { return (nil, false) }
    let exponent = source.exponent
    if _slowPath(Self.bitWidth <= exponent) { return (nil, false) }
    let minBitWidth = source.significandWidth
    let isExact = (minBitWidth <= exponent)
    let bitPattern = source.significandBitPattern
    // Determine the actual number of fractional significand bits.
    // `Source.significandBitCount` would not reflect the actual number of
    // fractional significand bits if `Source` is not a fixed-width floating-point
    // type; we can compute this value as follows if `source` is finite:
    let bitWidth = minBitWidth &+ bitPattern.trailingZeroBitCount
    let shift = exponent - Source.Exponent(bitWidth)
    // Use `Self.Magnitude` to prevent sign extension if `shift < 0`.
    let shiftedBitPattern = Self.Magnitude.bitWidth > bitWidth
      ? Self.Magnitude(truncatingIfNeeded: bitPattern) << shift
      : Self.Magnitude(truncatingIfNeeded: bitPattern << shift)
    if _slowPath(Self.isSigned && Self.bitWidth &- 1 == exponent) {
      return source < 0 && shiftedBitPattern == 0
        ? (Self.min, isExact)
        : (nil, false)
    }
    let magnitude = ((1 as Self.Magnitude) << exponent) | shiftedBitPattern
    return (
      Self.isSigned && source < 0 ? 0 &- Self(magnitude) : Self(magnitude),
      isExact)
  }

  /// Creates an integer from the given floating-point value, rounding toward
  /// zero. Any fractional part of the value passed as `source` is removed.
  ///
  ///     let x = Int(21.5)
  ///     // x == 21
  ///     let y = Int(-21.5)
  ///     // y == -21
  ///
  /// If `source` is outside the bounds of this type after rounding toward
  /// zero, a runtime error may occur.
  ///
  ///     let z = UInt(-21.5)
  ///     // Error: ...outside the representable range
  ///
  /// - Parameter source: A floating-point value to convert to an integer.
  ///   `source` must be representable in this type after rounding toward
  ///   zero.
  @inlinable
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @inline(__always)
  public init<T: BinaryFloatingPoint>(_ source: T) {
    guard let value = Self._convert(from: source).value else {
      #if !$Embedded
      fatalError("""
        \(T.self) value cannot be converted to \(Self.self) because it is \
        outside the representable range
        """)
      #else
      fatalError("value not representable")
      #endif
    }
    self = value
  }

  /// Creates an integer from the given floating-point value, if it can be
  /// represented exactly.
  ///
  /// If the value passed as `source` is not representable exactly, the result
  /// is `nil`. In the following example, the constant `x` is successfully
  /// created from a value of `21.0`, while the attempt to initialize the
  /// constant `y` from `21.5` fails:
  ///
  ///     let x = Int(exactly: 21.0)
  ///     // x == Optional(21)
  ///     let y = Int(exactly: 21.5)
  ///     // y == nil
  ///
  /// - Parameter source: A floating-point value to convert to an integer.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @inlinable
  public init?<T: BinaryFloatingPoint>(exactly source: T) {
    let (temporary, exact) = Self._convert(from: source)
    guard exact, let value = temporary else {
      return nil
    }
    self = value
  }

  /// Creates a new instance with the representable value that's closest to the
  /// given integer.
  ///
  /// If the value passed as `source` is greater than the maximum representable
  /// value in this type, the result is the type's `max` value. If `source` is
  /// less than the smallest representable value in this type, the result is
  /// the type's `min` value.
  ///
  /// In this example, `x` is initialized as an `Int8` instance by clamping
  /// `500` to the range `-128...127`, and `y` is initialized as a `UInt`
  /// instance by clamping `-500` to the range `0...UInt.max`.
  ///
  ///     let x = Int8(clamping: 500)
  ///     // x == 127
  ///     // x == Int8.max
  ///
  ///     let y = UInt(clamping: -500)
  ///     // y == 0
  ///
  /// - Parameter source: An integer to convert to this type.
  @inlinable
  @_semantics("optimize.sil.specialize.generic.partial.never")
  public init<Other: BinaryInteger>(clamping source: Other) {
    if _slowPath(source < Self.min) {
      self = Self.min
    }
    else if _slowPath(source > Self.max) {
      self = Self.max
    }
    else { self = Self(truncatingIfNeeded: source) }
  }

  /// Creates a new instance from the bit pattern of the given instance by
  /// truncating or sign-extending if needed to fit this type.
  ///
  /// When the bit width of `T` (the type of `source`) is equal to or greater
  /// than this type's bit width, the result is the truncated
  /// least-significant bits of `source`. For example, when converting a
  /// 16-bit value to an 8-bit type, only the lower 8 bits of `source` are
  /// used.
  ///
  ///     let p: Int16 = -500
  ///     // 'p' has a binary representation of 11111110_00001100
  ///     let q = Int8(truncatingIfNeeded: p)
  ///     // q == 12
  ///     // 'q' has a binary representation of 00001100
  ///
  /// When the bit width of `T` is less than this type's bit width, the result
  /// is *sign-extended* to fill the remaining bits. That is, if `source` is
  /// negative, the result is padded with ones; otherwise, the result is
  /// padded with zeros.
  ///
  ///     let u: Int8 = 21
  ///     // 'u' has a binary representation of 00010101
  ///     let v = Int16(truncatingIfNeeded: u)
  ///     // v == 21
  ///     // 'v' has a binary representation of 00000000_00010101
  ///
  ///     let w: Int8 = -21
  ///     // 'w' has a binary representation of 11101011
  ///     let x = Int16(truncatingIfNeeded: w)
  ///     // x == -21
  ///     // 'x' has a binary representation of 11111111_11101011
  ///     let y = UInt16(truncatingIfNeeded: w)
  ///     // y == 65515
  ///     // 'y' has a binary representation of 11111111_11101011
  ///
  /// - Parameter source: An integer to convert to this type.
  @inlinable // FIXME(inline-always)
  @inline(__always)
  public init<T: BinaryInteger>(truncatingIfNeeded source: T) {
    if Self.bitWidth <= Int.bitWidth {
      self = Self(_truncatingBits: source._lowWord)
    }
    else {
      self = Self._truncatingInit(source)
    }
  }

  @_alwaysEmitIntoClient
  internal static func _truncatingInit<T: BinaryInteger>(_ source: T) -> Self {
    let neg = source < (0 as T)
    var result: Self = neg ? ~0 : 0
    var shift: Self = 0
    let width = Self(_truncatingBits: Self.bitWidth._lowWord)
    for word in source.words {
      guard shift < width else { break }
      // Masking shift is OK here because we have already ensured
      // that shift < Self.bitWidth. Not masking results in
      // infinite recursion.
      result ^= Self(_truncatingBits: neg ? ~word : word) &<< shift
      shift += Self(_truncatingBits: Int.bitWidth._lowWord)
    }
    return result
  }

  @_transparent
  public // transparent
  static var _highBitIndex: Self {
    return Self.init(_truncatingBits: UInt(Self.bitWidth._value) &- 1)
  }

  /// Returns the sum of the two given values, wrapping the result in case of
  /// any overflow.
  ///
  /// The overflow addition operator (`&+`) discards any bits that overflow the
  /// fixed width of the integer type. In the following example, the sum of
  /// `100` and `121` is greater than the maximum representable `Int8` value,
  /// so the result is the partial value after discarding the overflowing
  /// bits.
  ///
  ///     let x: Int8 = 10 &+ 21
  ///     // x == 31
  ///     let y: Int8 = 100 &+ 121
  ///     // y == -35 (after overflow)
  ///
  /// For more about arithmetic with overflow operators, see [Overflow
  /// Operators][overflow] in *[The Swift Programming Language][tspl]*.
  ///
  /// [overflow]: https://docs.swift.org/swift-book/LanguageGuide/AdvancedOperators.html#ID37
  /// [tspl]: https://docs.swift.org/swift-book/
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  @_transparent
  public static func &+ (lhs: Self, rhs: Self) -> Self {
    return lhs.addingReportingOverflow(rhs).partialValue
  }

  /// Adds two values and stores the result in the left-hand-side variable,
  /// wrapping any overflow.
  ///
  /// The masking addition assignment operator (`&+=`) silently wraps any
  /// overflow that occurs during the operation. In the following example, the
  /// sum of `100` and `121` is greater than the maximum representable `Int8`
  /// value, so the result is the partial value after discarding the
  /// overflowing bits.
  ///
  ///     var x: Int8 = 10
  ///     x &+= 21
  ///     // x == 31
  ///     var y: Int8 = 100
  ///     y &+= 121
  ///     // y == -35 (after overflow)
  ///
  /// For more about arithmetic with overflow operators, see [Overflow
  /// Operators][overflow] in *[The Swift Programming Language][tspl]*.
  ///
  /// [overflow]: https://docs.swift.org/swift-book/LanguageGuide/AdvancedOperators.html#ID37
  /// [tspl]: https://docs.swift.org/swift-book/
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  @_transparent
  public static func &+= (lhs: inout Self, rhs: Self) {
    lhs = lhs &+ rhs
  }

  /// Returns the difference of the two given values, wrapping the result in
  /// case of any overflow.
  ///
  /// The overflow subtraction operator (`&-`) discards any bits that overflow
  /// the fixed width of the integer type. In the following example, the
  /// difference of `10` and `21` is less than zero, the minimum representable
  /// `UInt` value, so the result is the partial value after discarding the
  /// overflowing bits.
  ///
  ///     let x: UInt8 = 21 &- 10
  ///     // x == 11
  ///     let y: UInt8 = 10 &- 21
  ///     // y == 245 (after overflow)
  ///
  /// For more about arithmetic with overflow operators, see [Overflow
  /// Operators][overflow] in *[The Swift Programming Language][tspl]*.
  ///
  /// [overflow]: https://docs.swift.org/swift-book/LanguageGuide/AdvancedOperators.html#ID37
  /// [tspl]: https://docs.swift.org/swift-book/
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  @_transparent
  public static func &- (lhs: Self, rhs: Self) -> Self {
    return lhs.subtractingReportingOverflow(rhs).partialValue
  }

  /// Subtracts the second value from the first and stores the difference in the
  /// left-hand-side variable, wrapping any overflow.
  ///
  /// The masking subtraction assignment operator (`&-=`) silently wraps any
  /// overflow that occurs during the operation. In the following example, the
  /// difference of `10` and `21` is less than zero, the minimum representable
  /// `UInt` value, so the result is the result is the partial value after
  /// discarding the overflowing bits.
  ///
  ///     var x: Int8 = 21
  ///     x &-= 10
  ///     // x == 11
  ///     var y: UInt8 = 10
  ///     y &-= 21
  ///     // y == 245 (after overflow)
  ///
  /// For more about arithmetic with overflow operators, see [Overflow
  /// Operators][overflow] in *[The Swift Programming Language][tspl]*.
  ///
  /// [overflow]: https://docs.swift.org/swift-book/LanguageGuide/AdvancedOperators.html#ID37
  /// [tspl]: https://docs.swift.org/swift-book/
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  @_transparent
  public static func &-= (lhs: inout Self, rhs: Self) {
    lhs = lhs &- rhs
  }

  @_transparent
  public static func &*(lhs: Self, rhs: Self) -> Self {
    return rhs.multipliedReportingOverflow(by: lhs).partialValue
  }

  /// Multiplies two values and stores the result in the left-hand-side
  /// variable, wrapping any overflow.
  ///
  /// The masking multiplication assignment operator (`&*=`) silently wraps
  /// any overflow that occurs during the operation. In the following example,
  /// the product of `10` and `50` is greater than the maximum representable
  /// `Int8` value, so the result is the partial value after discarding the
  /// overflowing bits.
  ///
  ///     var x: Int8 = 10
  ///     x &*= 5
  ///     // x == 50
  ///     var y: Int8 = 10
  ///     y &*= 50
  ///     // y == -12 (after overflow)
  ///
  /// For more about arithmetic with overflow operators, see [Overflow
  /// Operators][overflow] in *[The Swift Programming Language][tspl]*.
  ///
  /// [overflow]: https://docs.swift.org/swift-book/LanguageGuide/AdvancedOperators.html#ID37
  /// [tspl]: https://docs.swift.org/swift-book/
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  @_transparent
  public static func &*= (lhs: inout Self, rhs: Self) {
    lhs = lhs &* rhs
  }
}

extension FixedWidthInteger {
  @inlinable
  public static func _random<R: RandomNumberGenerator>(
    using generator: inout R
  ) -> Self {
    if bitWidth <= UInt64.bitWidth {
      return Self(truncatingIfNeeded: generator.next())
    }

    let (quotient, remainder) = bitWidth.quotientAndRemainder(
      dividingBy: UInt64.bitWidth
    )
    var tmp: Self = 0
    for i in 0 ..< quotient + remainder.signum() {
      let next: UInt64 = generator.next()
      tmp += Self(truncatingIfNeeded: next) &<< (UInt64.bitWidth * i)
    }
    return tmp
  }
}

//===----------------------------------------------------------------------===//
//===--- UnsignedInteger --------------------------------------------------===//
//===----------------------------------------------------------------------===//

// Implementor's note: UnsignedInteger should have required Magnitude == Self,
// because it can necessarily represent the magnitude of every value and every
// recursive generic constraint should terminate unless there is a good
// semantic reason for it not to do so.
//
// However, we cannot easily add this constraint because it changes the
// mangling of generics constrained on <T: FixedWidthInteger & UnsignedInteger>
// to be <T: FixedWidthInteger where T.Magnitude == T>. As a practical matter,
// every unsigned type will satisfy this constraint, so converting between
// Magnitude and Self in generic code is acceptable.

/// An integer type that can represent only nonnegative values.
public protocol UnsignedInteger: BinaryInteger { }

extension UnsignedInteger {
  /// The magnitude of this value.
  ///
  /// Every unsigned integer is its own magnitude, so for any value `x`,
  /// `x == x.magnitude`.
  ///
  /// The global `abs(_:)` function provides more familiar syntax when you need
  /// to find an absolute value. In addition, because `abs(_:)` always returns
  /// a value of the same type, even in a generic context, using the function
  /// instead of the `magnitude` property is encouraged.
  @inlinable // FIXME(inline-always)
  public var magnitude: Self {
    @inline(__always)
    get { return self }
  }

  /// A Boolean value indicating whether this type is a signed integer type.
  ///
  /// This property is always `false` for unsigned integer types.
  @inlinable // FIXME(inline-always)
  public static var isSigned: Bool {
    @inline(__always)
    get { return false }
  }
}

extension UnsignedInteger where Self: FixedWidthInteger {
  /// Creates a new instance from the given integer.
  ///
  /// Use this initializer to convert from another integer type when you know
  /// the value is within the bounds of this type. Passing a value that can't
  /// be represented in this type results in a runtime error.
  ///
  /// In the following example, the constant `y` is successfully created from
  /// `x`, an `Int` instance with a value of `100`. Because the `Int8` type
  /// can represent `127` at maximum, the attempt to create `z` with a value
  /// of `1000` results in a runtime error.
  ///
  ///     let x = 100
  ///     let y = Int8(x)
  ///     // y == 100
  ///     let z = Int8(x * 10)
  ///     // Error: Not enough bits to represent the given value
  ///
  /// - Parameter source: A value to convert to this type of integer. The value
  ///   passed as `source` must be representable in this type.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @inlinable // FIXME(inline-always)
  @inline(__always)
  public init<T: BinaryInteger>(_ source: T) {
    // This check is potentially removable by the optimizer
    if T.isSigned {
      _precondition(source >= (0 as T), "Negative value is not representable")
    }
    // This check is potentially removable by the optimizer
    if source.bitWidth >= Self.bitWidth {
      _precondition(source <= Self.max,
        "Not enough bits to represent the passed value")
    }
    self.init(truncatingIfNeeded: source)
  }

  /// Creates a new instance from the given integer, if it can be represented
  /// exactly.
  ///
  /// If the value passed as `source` is not representable exactly, the result
  /// is `nil`. In the following example, the constant `x` is successfully
  /// created from a value of `100`, while the attempt to initialize the
  /// constant `y` from `1_000` fails because the `Int8` type can represent
  /// `127` at maximum:
  ///
  ///     let x = Int8(exactly: 100)
  ///     // x == Optional(100)
  ///     let y = Int8(exactly: 1_000)
  ///     // y == nil
  ///
  /// - Parameter source: A value to convert to this type of integer.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @inlinable // FIXME(inline-always)
  @inline(__always)
  public init?<T: BinaryInteger>(exactly source: T) {
    // This check is potentially removable by the optimizer
    if T.isSigned && source < (0 as T) {
      return nil
    }
    // The width check can be eliminated by the optimizer
    if source.bitWidth >= Self.bitWidth &&
       source > Self.max {
      return nil
    }
    self.init(truncatingIfNeeded: source)
  }

  /// The maximum representable integer in this type.
  ///
  /// For unsigned integer types, this value is `(2 ** bitWidth) - 1`, where
  /// `**` is exponentiation.
  @_transparent
  public static var max: Self { return ~0 }

  /// The minimum representable integer in this type.
  ///
  /// For unsigned integer types, this value is always `0`.
  @_transparent
  public static var min: Self { return 0 }
  
  @_alwaysEmitIntoClient
  public func dividingFullWidth(
    _ dividend: (high: Self, low: Magnitude)
  ) -> (quotient: Self, remainder: Self) {
    // Validate preconditions to guarantee that the quotient is representable.
    precondition(self != .zero, "Division by zero")
    precondition(dividend.high < self,
                 "Dividend.high must be smaller than divisor")
    // UnsignedInteger should have a Magnitude = Self constraint, but does not,
    // so we have to do this conversion (we can't easily add the constraint
    // because it changes how generic signatures constrained to
    // <FixedWidth & Unsigned> are minimized, which changes the mangling).
    // In practice, "every" UnsignedInteger type will satisfy this, and if one
    // somehow manages not to in a way that would break this conversion then
    // a default implementation of this method never could have worked anyway.
    let low = Self(dividend.low)
    
    // The basic algorithm is taken from Knuth (TAoCP, Vol 2, §4.3.1), using
    // words that are half the size of Self (so the dividend has four words
    // and the divisor has two). The fact that the denominator has exactly
    // two words allows for a slight simplification vs. Knuth's Algorithm D,
    // in that our computed quotient digit is always exactly right, while
    // in the more general case it can be one too large, requiring a subsequent
    // borrow.
    //
    // Knuth's algorithm (and any long division, really), requires that the
    // divisor (self) be normalized (meaning that the high-order bit is set).
    // We begin by counting the leading zeros so we know how many bits we
    // have to shift to normalize.
    let lz = leadingZeroBitCount
    
    // If the divisor is actually a power of two, division is just a shift,
    // which we can handle much more efficiently. So we do a check for that
    // case and early-out if possible.
    if (self &- 1) & self == .zero {
      let shift = Self.bitWidth - 1 - lz
      let q = low &>> shift | dividend.high &<< -shift
      let r = low & (self &- 1)
      return (q, r)
    }
    
    // Shift the divisor left by lz bits to normalize it. We shift the
    // dividend left by the same amount so that we get the quotient is
    // preserved (we will have to shift right to recover the remainder).
    // Note that the right shift `low >> (Self.bitWidth - lz)` is
    // deliberately a non-masking shift because lz might be zero.
    let v = self &<< lz
    let uh = dividend.high &<< lz | low >> (Self.bitWidth - lz)
    let ul = low &<< lz
    
    // Now we have a normalized dividend (uh:ul) and divisor (v). Split
    // v into half-words (vh:vl) so that we can use the "normal" division
    // on Self as a word / halfword -> halfword division get one halfword
    // digit of the quotient at a time.
    let n_2 = Self.bitWidth/2
    let mask = Self(1) &<< n_2 &- 1
    let vh = v &>> n_2
    let vl = v & mask
    
    // For the (fairly-common) special case where vl is zero, we can simplify
    // the arithmetic quite a bit:
    if vl == .zero {
      let qh = uh / vh
      let residual = (uh &- qh &* vh) &<< n_2 | ul &>> n_2
      let ql = residual / vh
      
      return (
        // Assemble quotient from half-word digits
        quotient: qh &<< n_2 | ql,
        // Compute remainder (we can re-use the residual to make this simpler).
        remainder: ((residual &- ql &* vh) &<< n_2 | ul & mask) &>> lz
      )
    }
    
    // Helper function: performs a (1½ word)/word division to produce a
    // half quotient word q. We'll need to use this twice to generate the
    // full quotient.
    //
    // high is the high word of the quotient for this sub-division.
    // low is the low half-word of the quotient for this sub-division (the
    //     high half of low must be zero).
    //
    // returns the quotient half-word digit. In a more general setting, this
    // computed digit might be one too large, which has to be accounted for
    // later on (see Knuth, Algorithm D), but when the divisor is only two
    // half-words (as here), that can never happen, because we use the full
    // divisor in the check for the while loop.
    func generateHalfDigit(high: Self, low: Self) -> Self {
      // Get q̂ satisfying a = vh q̂ + r̂ with 0 ≤ r̂ < vh:
      var (q̂, r̂) = high.quotientAndRemainder(dividingBy: vh)
      // Knuth's "Theorem A" establishes that q̂ is an approximation to
      // the quotient digit q, satisfying q ≤ q̂ ≤ q + 2. We adjust it
      // downward as needed until we have the correct q.
      while q̂ > mask || q̂ &* vl > (r̂ &<< n_2 | low) {
        q̂ &-= 1
        r̂ &+= vh
        if r̂ > mask { break }
      }
      return q̂
    }
    
    // Generate the first quotient digit, subtract off its product with the
    // divisor to generate the residual, then compute the second quotient
    // digit from that.
    let qh = generateHalfDigit(high: uh, low: ul &>> n_2)
    let residual = (uh &<< n_2 | ul &>> n_2) &- (qh &* v)
    let ql = generateHalfDigit(high: residual, low: ul & mask)
    
    return (
      // Assemble quotient from half-word digits
      quotient: qh &<< n_2 | ql,
      // Compute remainder (we can re-use the residual to make this simpler).
      remainder: ((residual &<< n_2 | ul & mask) &- (ql &* v)) &>> lz
    )
  }
}

//===----------------------------------------------------------------------===//
//===--- SignedInteger ----------------------------------------------------===//
//===----------------------------------------------------------------------===//

/// An integer type that can represent both positive and negative values.
public protocol SignedInteger: BinaryInteger, SignedNumeric {
  // These requirements are needed for binary compatibility; the following:
  //
  // func foo<T>(_ a: T) -> T
  // where T: SignedInteger & FixedWidthInteger {
  //   a &+ 1
  // }
  //
  // generated a call to `static Swift.SignedInteger._maskingAdd(A, A) -> A`
  // when compiled with Swift 5.5 and earlier.
  @available(*, deprecated, message: "Use &+ instead.")
  static func _maskingAdd(_ lhs: Self, _ rhs: Self) -> Self
  
  @available(*, deprecated, message: "Use &- instead.")
  static func _maskingSubtract(_ lhs: Self, _ rhs: Self) -> Self
}

extension SignedInteger {
  /// A Boolean value indicating whether this type is a signed integer type.
  ///
  /// This property is always `true` for signed integer types.
  @inlinable // FIXME(inline-always)
  public static var isSigned: Bool {
    @inline(__always)
    get { return true }
  }
}

extension SignedInteger where Self: FixedWidthInteger {
  /// Creates a new instance from the given integer.
  ///
  /// Use this initializer to convert from another integer type when you know
  /// the value is within the bounds of this type. Passing a value that can't
  /// be represented in this type results in a runtime error.
  ///
  /// In the following example, the constant `y` is successfully created from
  /// `x`, an `Int` instance with a value of `100`. Because the `Int8` type
  /// can represent `127` at maximum, the attempt to create `z` with a value
  /// of `1000` results in a runtime error.
  ///
  ///     let x = 100
  ///     let y = Int8(x)
  ///     // y == 100
  ///     let z = Int8(x * 10)
  ///     // Error: Not enough bits to represent the given value
  ///
  /// - Parameter source: A value to convert to this type of integer. The value
  ///   passed as `source` must be representable in this type.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @inlinable // FIXME(inline-always)
  @inline(__always)
  public init<T: BinaryInteger>(_ source: T) {
    // This check is potentially removable by the optimizer
    if T.isSigned && source.bitWidth > Self.bitWidth {
      _precondition(source >= Self.min,
        "Not enough bits to represent a signed value")
    }
    // This check is potentially removable by the optimizer
    if (source.bitWidth > Self.bitWidth) ||
       (source.bitWidth == Self.bitWidth && !T.isSigned) {
      _precondition(source <= Self.max,
        "Not enough bits to represent the passed value")
    }
    self.init(truncatingIfNeeded: source)
  }

  /// Creates a new instance from the given integer, if it can be represented
  /// exactly.
  ///
  /// If the value passed as `source` is not representable exactly, the result
  /// is `nil`. In the following example, the constant `x` is successfully
  /// created from a value of `100`, while the attempt to initialize the
  /// constant `y` from `1_000` fails because the `Int8` type can represent
  /// `127` at maximum:
  ///
  ///     let x = Int8(exactly: 100)
  ///     // x == Optional(100)
  ///     let y = Int8(exactly: 1_000)
  ///     // y == nil
  ///
  /// - Parameter source: A value to convert to this type of integer.
  @_semantics("optimize.sil.specialize.generic.partial.never")
  @inlinable // FIXME(inline-always)
  @inline(__always)
  public init?<T: BinaryInteger>(exactly source: T) {
    // This check is potentially removable by the optimizer
    if T.isSigned && source.bitWidth > Self.bitWidth && source < Self.min {
      return nil
    }
    // The width check can be eliminated by the optimizer
    if (source.bitWidth > Self.bitWidth ||
        (source.bitWidth == Self.bitWidth && !T.isSigned)) &&
       source > Self.max {
      return nil
    }
    self.init(truncatingIfNeeded: source)
  }

  /// The maximum representable integer in this type.
  ///
  /// For signed integer types, this value is `(2 ** (bitWidth - 1)) - 1`,
  /// where `**` is exponentiation.
  @_transparent
  public static var max: Self { return ~min }

  /// The minimum representable integer in this type.
  ///
  /// For signed integer types, this value is `-(2 ** (bitWidth - 1))`, where
  /// `**` is exponentiation.
  @_transparent
  public static var min: Self {
    return (-1 as Self) &<< Self._highBitIndex
  }
  
  @inlinable
  public func isMultiple(of other: Self) -> Bool {
    // Nothing but zero is a multiple of zero.
    if other == 0 { return self == 0 }
    // Special case to avoid overflow on .min / -1 for signed types.
    if other == -1 { return true }
    // Having handled those special cases, this is safe.
    return self % other == 0
  }
  
  @_alwaysEmitIntoClient
  public func dividingFullWidth(
    _ dividend: (high: Self, low: Magnitude)
  ) -> (quotient: Self, remainder: Self) {
    // Get magnitude of dividend:
    var magnitudeHigh = Magnitude(truncatingIfNeeded: dividend.high)
    var magnitudeLow  = dividend.low
    if dividend.high < .zero {
      let carry: Bool
      (magnitudeLow, carry) = (~magnitudeLow).addingReportingOverflow(1)
      magnitudeHigh = ~magnitudeHigh &+ (carry ? 1 : 0)
    }
    // Do division on magnitudes (using unsigned implementation):
    let (unsignedQuotient, unsignedRemainder) = magnitude.dividingFullWidth(
      (high: magnitudeHigh, low: magnitudeLow)
    )
    // Fixup sign: quotient is negative if dividend and divisor disagree.
    // We will also trap here if the quotient does not fit in Self.
    let quotient: Self
    if self ^ dividend.high < .zero {
      // It is possible that the quotient is representable but its magnitude
      // is not representable as Self (if quotient is Self.min), so we have
      // to handle that case carefully here.
      precondition(unsignedQuotient <= Self.min.magnitude,
                   "Quotient is not representable.")
      quotient = Self(truncatingIfNeeded: 0 &- unsignedQuotient)
    } else {
      quotient = Self(unsignedQuotient)
    }
    var remainder = Self(unsignedRemainder)
    if dividend.high < .zero { remainder = 0 &- remainder }
    return (quotient, remainder)
  }
}

/// Returns the given integer as the equivalent value in a different integer
/// type.
///
/// Calling the `numericCast(_:)` function is equivalent to calling an
/// initializer for the destination type. `numericCast(_:)` traps on overflow 
/// in `-O` and `-Onone` builds.
///
/// - Parameter x: The integer to convert, an instance of type `T`.
/// - Returns: The value of `x` converted to type `U`.
@inlinable
public func numericCast<T: BinaryInteger, U: BinaryInteger>(_ x: T) -> U {
  return U(x)
}

// Needed to support user-defined types conformance to SignedInteger.
// We need these defaults to exist, but they are not called.
extension SignedInteger {
  @available(*, deprecated, message: "Use &+ instead.")
  public static func _maskingAdd(_ lhs: Self, _ rhs: Self) -> Self {
    fatalError("Should be overridden in a more specific type")
  }
  
  @available(*, deprecated, message: "Use &- instead.")
  public static func _maskingSubtract(_ lhs: Self, _ rhs: Self) -> Self {
    fatalError("Should be overridden in a more specific type")
  }
}

// These symbols have to exist for ABI compatibility, but should not be used
// any longer; we want to find the FixedWidthInteger definitions instead.
extension SignedInteger where Self: FixedWidthInteger {
  @available(*, unavailable)
  public static func &+(lhs: Self, rhs: Self) -> Self {
    lhs.addingReportingOverflow(rhs).partialValue
  }
  
  // This may be called in rare situations by binaries compiled with
  // Swift 5.5 and earlier, so we need to keep it around for compatibility.
  // We can't mark it unavailable, because then the concrete signed integer
  // types in the standard library would not satisfy the protocol requirements.
  @available(*, deprecated, message: "Use &+ instead.")
  public static func _maskingAdd(_ lhs: Self, _ rhs: Self) -> Self {
    lhs.addingReportingOverflow(rhs).partialValue
  }

  @available(*, unavailable)
  public static func &-(lhs: Self, rhs: Self) -> Self {
    lhs.subtractingReportingOverflow(rhs).partialValue
  }
  
  // This may be called in rare situations by binaries compiled with
  // Swift 5.5 and earlier, so we need to keep it around for compatibility.
  // We can't mark it unavailable, because then the concrete signed integer
  // types in the standard library would not satisfy the protocol requirements.
  @available(*, deprecated, message: "Use &- instead.")
  public static func _maskingSubtract(_ lhs: Self, _ rhs: Self) -> Self {
    lhs.subtractingReportingOverflow(rhs).partialValue
  }
}
