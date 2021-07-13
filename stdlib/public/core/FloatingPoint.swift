//===--- FloatingPoint.swift ----------------------------------*- swift -*-===//
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

/// A floating-point numeric type.
///
/// Floating-point types are used to represent fractional numbers, like 5.5,
/// 100.0, or 3.14159274. Each floating-point type has its own possible range
/// and precision. The floating-point types in the standard library are
/// `Float`, `Double`, and `Float80` where available.
///
/// Create new instances of floating-point types using integer or
/// floating-point literals. For example:
///
///     let temperature = 33.2
///     let recordHigh = 37.5
///
/// The `FloatingPoint` protocol declares common arithmetic operations, so you
/// can write functions and algorithms that work on any floating-point type.
/// The following example declares a function that calculates the length of
/// the hypotenuse of a right triangle given its two perpendicular sides.
/// Because the `hypotenuse(_:_:)` function uses a generic parameter
/// constrained to the `FloatingPoint` protocol, you can call it using any
/// floating-point type.
///
///     func hypotenuse<T: FloatingPoint>(_ a: T, _ b: T) -> T {
///         return (a * a + b * b).squareRoot()
///     }
///
///     let (dx, dy) = (3.0, 4.0)
///     let distance = hypotenuse(dx, dy)
///     // distance == 5.0
///
/// Floating-point values are represented as a *sign* and a *magnitude*, where
/// the magnitude is calculated using the type's *radix* and the instance's
/// *significand* and *exponent*. This magnitude calculation takes the
/// following form for a floating-point value `x` of type `F`, where `**` is
/// exponentiation:
///
///     x.significand * F.radix ** x.exponent
///
/// Here's an example of the number -8.5 represented as an instance of the
/// `Double` type, which defines a radix of 2.
///
///     let y = -8.5
///     // y.sign == .minus
///     // y.significand == 1.0625
///     // y.exponent == 3
///
///     let magnitude = 1.0625 * Double(2 ** 3)
///     // magnitude == 8.5
///
/// Types that conform to the `FloatingPoint` protocol provide most basic
/// (clause 5) operations of the [IEEE 754 specification][spec]. The base,
/// precision, and exponent range are not fixed in any way by this protocol,
/// but it enforces the basic requirements of any IEEE 754 floating-point
/// type.
///
/// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
///
/// Additional Considerations
/// =========================
///
/// In addition to representing specific numbers, floating-point types also
/// have special values for working with overflow and nonnumeric results of
/// calculation.
///
/// Infinity
/// --------
///
/// Any value whose magnitude is so great that it would round to a value
/// outside the range of representable numbers is rounded to *infinity*. For a
/// type `F`, positive and negative infinity are represented as `F.infinity`
/// and `-F.infinity`, respectively. Positive infinity compares greater than
/// every finite value and negative infinity, while negative infinity compares
/// less than every finite value and positive infinity. Infinite values with
/// the same sign are equal to each other.
///
///     let values: [Double] = [10.0, 25.0, -10.0, .infinity, -.infinity]
///     print(values.sorted())
///     // Prints "[-inf, -10.0, 10.0, 25.0, inf]"
///
/// Operations with infinite values follow real arithmetic as much as possible:
/// Adding or subtracting a finite value, or multiplying or dividing infinity
/// by a nonzero finite value, results in infinity.
///
/// NaN ("not a number")
/// --------------------
///
/// Floating-point types represent values that are neither finite numbers nor
/// infinity as NaN, an abbreviation for "not a number." Comparing a NaN with
/// any value, including another NaN, results in `false`.
///
///     let myNaN = Double.nan
///     print(myNaN > 0)
///     // Prints "false"
///     print(myNaN < 0)
///     // Prints "false"
///     print(myNaN == .nan)
///     // Prints "false"
///
/// Because testing whether one NaN is equal to another NaN results in `false`,
/// use the `isNaN` property to test whether a value is NaN.
///
///     print(myNaN.isNaN)
///     // Prints "true"
///
/// NaN propagates through many arithmetic operations. When you are operating
/// on many values, this behavior is valuable because operations on NaN simply
/// forward the value and don't cause runtime errors. The following example
/// shows how NaN values operate in different contexts.
///
/// Imagine you have a set of temperature data for which you need to report
/// some general statistics: the total number of observations, the number of
/// valid observations, and the average temperature. First, a set of
/// observations in Celsius is parsed from strings to `Double` values:
///
///     let temperatureData = ["21.5", "19.25", "27", "no data", "28.25", "no data", "23"]
///     let tempsCelsius = temperatureData.map { Double($0) ?? .nan }
///     print(tempsCelsius)
///     // Prints "[21.5, 19.25, 27, nan, 28.25, nan, 23.0]"
///
///
/// Note that some elements in the `temperatureData ` array are not valid
/// numbers. When these invalid strings are parsed by the `Double` failable
/// initializer, the example uses the nil-coalescing operator (`??`) to
/// provide NaN as a fallback value.
///
/// Next, the observations in Celsius are converted to Fahrenheit:
///
///     let tempsFahrenheit = tempsCelsius.map { $0 * 1.8 + 32 }
///     print(tempsFahrenheit)
///     // Prints "[70.7, 66.65, 80.6, nan, 82.85, nan, 73.4]"
///
/// The NaN values in the `tempsCelsius` array are propagated through the
/// conversion and remain NaN in `tempsFahrenheit`.
///
/// Because calculating the average of the observations involves combining
/// every value of the `tempsFahrenheit` array, any NaN values cause the
/// result to also be NaN, as seen in this example:
///
///     let badAverage = tempsFahrenheit.reduce(0.0, +) / Double(tempsFahrenheit.count)
///     // badAverage.isNaN == true
///
/// Instead, when you need an operation to have a specific numeric result,
/// filter out any NaN values using the `isNaN` property.
///
///     let validTemps = tempsFahrenheit.filter { !$0.isNaN }
///     let average = validTemps.reduce(0.0, +) / Double(validTemps.count)
///
/// Finally, report the average temperature and observation counts:
///
///     print("Average: \(average)°F in \(validTemps.count) " +
///           "out of \(tempsFahrenheit.count) observations.")
///     // Prints "Average: 74.84°F in 5 out of 7 observations."
public protocol FloatingPoint: SignedNumeric, Strideable, Hashable
                                where Magnitude == Self {

  /// A type that can represent any written exponent.
  associatedtype Exponent: SignedInteger

  /// Creates a new value from the given sign, exponent, and significand.
  ///
  /// The following example uses this initializer to create a new `Double`
  /// instance. `Double` is a binary floating-point type that has a radix of
  /// `2`.
  ///
  ///     let x = Double(sign: .plus, exponent: -2, significand: 1.5)
  ///     // x == 0.375
  ///
  /// This initializer is equivalent to the following calculation, where `**`
  /// is exponentiation, computed as if by a single, correctly rounded,
  /// floating-point operation:
  ///
  ///     let sign: FloatingPointSign = .plus
  ///     let exponent = -2
  ///     let significand = 1.5
  ///     let y = (sign == .minus ? -1 : 1) * significand * Double.radix ** exponent
  ///     // y == 0.375
  ///
  /// As with any basic operation, if this value is outside the representable
  /// range of the type, overflow or underflow occurs, and zero, a subnormal
  /// value, or infinity may result. In addition, there are two other edge
  /// cases:
  ///
  /// - If the value you pass to `significand` is zero or infinite, the result
  ///   is zero or infinite, regardless of the value of `exponent`.
  /// - If the value you pass to `significand` is NaN, the result is NaN.
  ///
  /// For any floating-point value `x` of type `F`, the result of the following
  /// is equal to `x`, with the distinction that the result is canonicalized
  /// if `x` is in a noncanonical encoding:
  ///
  ///     let x0 = F(sign: x.sign, exponent: x.exponent, significand: x.significand)
  ///
  /// This initializer implements the `scaleB` operation defined by the [IEEE
  /// 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - sign: The sign to use for the new value.
  ///   - exponent: The new value's exponent.
  ///   - significand: The new value's significand.
  init(sign: FloatingPointSign, exponent: Exponent, significand: Self)

  /// Creates a new floating-point value using the sign of one value and the
  /// magnitude of another.
  ///
  /// The following example uses this initializer to create a new `Double`
  /// instance with the sign of `a` and the magnitude of `b`:
  ///
  ///     let a = -21.5
  ///     let b = 305.15
  ///     let c = Double(signOf: a, magnitudeOf: b)
  ///     print(c)
  ///     // Prints "-305.15"
  ///
  /// This initializer implements the IEEE 754 `copysign` operation.
  ///
  /// - Parameters:
  ///   - signOf: A value from which to use the sign. The result of the
  ///     initializer has the same sign as `signOf`.
  ///   - magnitudeOf: A value from which to use the magnitude. The result of
  ///     the initializer has the same magnitude as `magnitudeOf`.
  init(signOf: Self, magnitudeOf: Self)
  
  /// Creates a new value, rounded to the closest possible representation.
  ///
  /// If two representable values are equally close, the result is the value
  /// with more trailing zeros in its significand bit pattern.
  ///
  /// - Parameter value: The integer to convert to a floating-point value.
  init(_ value: Int)

  /// Creates a new value, rounded to the closest possible representation.
  ///
  /// If two representable values are equally close, the result is the value
  /// with more trailing zeros in its significand bit pattern.
  ///
  /// - Parameter value: The integer to convert to a floating-point value.
  init<Source: BinaryInteger>(_ value: Source)

  /// Creates a new value, if the given integer can be represented exactly.
  ///
  /// If the given integer cannot be represented exactly, the result is `nil`.
  ///
  /// - Parameter value: The integer to convert to a floating-point value.
  init?<Source: BinaryInteger>(exactly value: Source)

  /// The radix, or base of exponentiation, for a floating-point type.
  ///
  /// The magnitude of a floating-point value `x` of type `F` can be calculated
  /// by using the following formula, where `**` is exponentiation:
  ///
  ///     let magnitude = x.significand * F.radix ** x.exponent
  ///
  /// A conforming type may use any integer radix, but values other than 2 (for
  /// binary floating-point types) or 10 (for decimal floating-point types)
  /// are extraordinarily rare in practice.
  static var radix: Int { get }

  /// A quiet NaN ("not a number").
  ///
  /// A NaN compares not equal, not greater than, and not less than every
  /// value, including itself. Passing a NaN to an operation generally results
  /// in NaN.
  ///
  ///     let x = 1.21
  ///     // x > Double.nan == false
  ///     // x < Double.nan == false
  ///     // x == Double.nan == false
  ///
  /// Because a NaN always compares not equal to itself, to test whether a
  /// floating-point value is NaN, use its `isNaN` property instead of the
  /// equal-to operator (`==`). In the following example, `y` is NaN.
  ///
  ///     let y = x + Double.nan
  ///     print(y == Double.nan)
  ///     // Prints "false"
  ///     print(y.isNaN)
  ///     // Prints "true"
  static var nan: Self { get }

  /// A signaling NaN ("not a number").
  ///
  /// The default IEEE 754 behavior of operations involving a signaling NaN is
  /// to raise the Invalid flag in the floating-point environment and return a
  /// quiet NaN.
  ///
  /// Operations on types conforming to the `FloatingPoint` protocol should
  /// support this behavior, but they might also support other options. For
  /// example, it would be reasonable to implement alternative operations in
  /// which operating on a signaling NaN triggers a runtime error or results
  /// in a diagnostic for debugging purposes. Types that implement alternative
  /// behaviors for a signaling NaN must document the departure.
  ///
  /// Other than these signaling operations, a signaling NaN behaves in the
  /// same manner as a quiet NaN.
  static var signalingNaN: Self { get }

  /// Positive infinity.
  ///
  /// Infinity compares greater than all finite numbers and equal to other
  /// infinite values.
  ///
  ///     let x = Double.greatestFiniteMagnitude
  ///     let y = x * 2
  ///     // y == Double.infinity
  ///     // y > x
  static var infinity: Self { get }

  /// The greatest finite number representable by this type.
  ///
  /// This value compares greater than or equal to all finite numbers, but less
  /// than `infinity`.
  ///
  /// This value corresponds to type-specific C macros such as `FLT_MAX` and
  /// `DBL_MAX`. The naming of those macros is slightly misleading, because
  /// `infinity` is greater than this value.
  static var greatestFiniteMagnitude: Self { get }

  /// The mathematical constant pi.
  ///
  /// This value should be rounded toward zero to keep user computations with
  /// angles from inadvertently ending up in the wrong quadrant. A type that
  /// conforms to the `FloatingPoint` protocol provides the value for `pi` at
  /// its best possible precision.
  ///
  ///     print(Double.pi)
  ///     // Prints "3.14159265358979"
  static var pi: Self { get }

  // NOTE: Rationale for "ulp" instead of "epsilon":
  // We do not use that name because it is ambiguous at best and misleading
  // at worst:
  //
  // - Historically several definitions of "machine epsilon" have commonly
  //   been used, which differ by up to a factor of two or so. By contrast
  //   "ulp" is a term with a specific unambiguous definition.
  //
  // - Some languages have used "epsilon" to refer to wildly different values,
  //   such as `leastNonzeroMagnitude`.
  //
  // - Inexperienced users often believe that "epsilon" should be used as a
  //   tolerance for floating-point comparisons, because of the name. It is
  //   nearly always the wrong value to use for this purpose.

  /// The unit in the last place of this value.
  ///
  /// This is the unit of the least significant digit in this value's
  /// significand. For most numbers `x`, this is the difference between `x`
  /// and the next greater (in magnitude) representable number. There are some
  /// edge cases to be aware of:
  ///
  /// - If `x` is not a finite number, then `x.ulp` is NaN.
  /// - If `x` is very small in magnitude, then `x.ulp` may be a subnormal
  ///   number. If a type does not support subnormals, `x.ulp` may be rounded
  ///   to zero.
  /// - `greatestFiniteMagnitude.ulp` is a finite number, even though the next
  ///   greater representable value is `infinity`.
  ///
  /// See also the `ulpOfOne` static property.
  var ulp: Self { get }

  /// The unit in the last place of 1.0.
  ///
  /// The positive difference between 1.0 and the next greater representable
  /// number. `ulpOfOne` corresponds to the value represented by the C macros
  /// `FLT_EPSILON`, `DBL_EPSILON`, etc, and is sometimes called *epsilon* or
  /// *machine epsilon*. Swift deliberately avoids using the term "epsilon"
  /// because:
  ///
  /// - Historically "epsilon" has been used to refer to several different
  ///   concepts in different languages, leading to confusion and bugs.
  ///
  /// - The name "epsilon" suggests that this quantity is a good tolerance to
  ///   choose for approximate comparisons, but it is almost always unsuitable
  ///   for that purpose.
  ///
  /// See also the `ulp` member property.
  static var ulpOfOne: Self { get }

  /// The least positive normal number.
  ///
  /// This value compares less than or equal to all positive normal numbers.
  /// There may be smaller positive numbers, but they are *subnormal*, meaning
  /// that they are represented with less precision than normal numbers.
  ///
  /// This value corresponds to type-specific C macros such as `FLT_MIN` and
  /// `DBL_MIN`. The naming of those macros is slightly misleading, because
  /// subnormals, zeros, and negative numbers are smaller than this value.
  static var leastNormalMagnitude: Self { get }

  /// The least positive number.
  ///
  /// This value compares less than or equal to all positive numbers, but
  /// greater than zero. If the type supports subnormal values,
  /// `leastNonzeroMagnitude` is smaller than `leastNormalMagnitude`;
  /// otherwise they are equal.
  static var leastNonzeroMagnitude: Self { get }

  /// The sign of the floating-point value.
  ///
  /// The `sign` property is `.minus` if the value's signbit is set, and
  /// `.plus` otherwise. For example:
  ///
  ///     let x = -33.375
  ///     // x.sign == .minus
  ///
  /// Don't use this property to check whether a floating point value is
  /// negative. For a value `x`, the comparison `x.sign == .minus` is not
  /// necessarily the same as `x < 0`. In particular, `x.sign == .minus` if
  /// `x` is -0, and while `x < 0` is always `false` if `x` is NaN, `x.sign`
  /// could be either `.plus` or `.minus`.
  var sign: FloatingPointSign { get }

  /// The exponent of the floating-point value.
  ///
  /// The *exponent* of a floating-point value is the integer part of the
  /// logarithm of the value's magnitude. For a value `x` of a floating-point
  /// type `F`, the magnitude can be calculated as the following, where `**`
  /// is exponentiation:
  ///
  ///     let magnitude = x.significand * F.radix ** x.exponent
  ///
  /// In the next example, `y` has a value of `21.5`, which is encoded as
  /// `1.34375 * 2 ** 4`. The significand of `y` is therefore 1.34375.
  ///
  ///     let y: Double = 21.5
  ///     // y.significand == 1.34375
  ///     // y.exponent == 4
  ///     // Double.radix == 2
  ///
  /// The `exponent` property has the following edge cases:
  ///
  /// - If `x` is zero, then `x.exponent` is `Int.min`.
  /// - If `x` is +/-infinity or NaN, then `x.exponent` is `Int.max`
  ///
  /// This property implements the `logB` operation defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  var exponent: Exponent { get }

  /// The significand of the floating-point value.
  ///
  /// The magnitude of a floating-point value `x` of type `F` can be calculated
  /// by using the following formula, where `**` is exponentiation:
  ///
  ///     let magnitude = x.significand * F.radix ** x.exponent
  ///
  /// In the next example, `y` has a value of `21.5`, which is encoded as
  /// `1.34375 * 2 ** 4`. The significand of `y` is therefore 1.34375.
  ///
  ///     let y: Double = 21.5
  ///     // y.significand == 1.34375
  ///     // y.exponent == 4
  ///     // Double.radix == 2
  ///
  /// If a type's radix is 2, then for finite nonzero numbers, the significand
  /// is in the range `1.0 ..< 2.0`. For other values of `x`, `x.significand`
  /// is defined as follows:
  ///
  /// - If `x` is zero, then `x.significand` is 0.0.
  /// - If `x` is infinite, then `x.significand` is infinity.
  /// - If `x` is NaN, then `x.significand` is NaN.
  /// - Note: The significand is frequently also called the *mantissa*, but
  ///   significand is the preferred terminology in the [IEEE 754
  ///   specification][spec], to allay confusion with the use of mantissa for
  ///   the fractional part of a logarithm.
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  var significand: Self { get }

  /// Adds two values and produces their sum, rounded to a
  /// representable value.
  ///
  /// The addition operator (`+`) calculates the sum of its two arguments. For
  /// example:
  ///
  ///     let x = 1.5
  ///     let y = x + 2.25
  ///     // y == 3.75
  ///
  /// The `+` operator implements the addition operation defined by the
  /// [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  override static func +(lhs: Self, rhs: Self) -> Self

  /// Adds two values and stores the result in the left-hand-side variable,
  /// rounded to a representable value.
  ///
  /// - Parameters:
  ///   - lhs: The first value to add.
  ///   - rhs: The second value to add.
  override static func +=(lhs: inout Self, rhs: Self)

  /// Calculates the additive inverse of a value.
  ///
  /// The unary minus operator (prefix `-`) calculates the negation of its
  /// operand. The result is always exact.
  ///
  ///     let x = 21.5
  ///     let y = -x
  ///     // y == -21.5
  ///
  /// - Parameter operand: The value to negate.
  override static prefix func - (_ operand: Self) -> Self

  /// Replaces this value with its additive inverse.
  ///
  /// The result is always exact. This example uses the `negate()` method to
  /// negate the value of the variable `x`:
  ///
  ///     var x = 21.5
  ///     x.negate()
  ///     // x == -21.5
  override mutating func negate()

  /// Subtracts one value from another and produces their difference, rounded
  /// to a representable value.
  ///
  /// The subtraction operator (`-`) calculates the difference of its two
  /// arguments. For example:
  ///
  ///     let x = 7.5
  ///     let y = x - 2.25
  ///     // y == 5.25
  ///
  /// The `-` operator implements the subtraction operation defined by the
  /// [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  override static func -(lhs: Self, rhs: Self) -> Self

  /// Subtracts the second value from the first and stores the difference in
  /// the left-hand-side variable, rounding to a representable value.
  ///
  /// - Parameters:
  ///   - lhs: A numeric value.
  ///   - rhs: The value to subtract from `lhs`.
  override static func -=(lhs: inout Self, rhs: Self)

  /// Multiplies two values and produces their product, rounding to a
  /// representable value.
  ///
  /// The multiplication operator (`*`) calculates the product of its two
  /// arguments. For example:
  ///
  ///     let x = 7.5
  ///     let y = x * 2.25
  ///     // y == 16.875
  ///
  /// The `*` operator implements the multiplication operation defined by the
  /// [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  override static func *(lhs: Self, rhs: Self) -> Self

  /// Multiplies two values and stores the result in the left-hand-side
  /// variable, rounding to a representable value.
  ///
  /// - Parameters:
  ///   - lhs: The first value to multiply.
  ///   - rhs: The second value to multiply.
  override static func *=(lhs: inout Self, rhs: Self)

  /// Returns the quotient of dividing the first value by the second, rounded
  /// to a representable value.
  ///
  /// The division operator (`/`) calculates the quotient of the division if
  /// `rhs` is nonzero. If `rhs` is zero, the result of the division is
  /// infinity, with the sign of the result matching the sign of `lhs`.
  ///
  ///     let x = 16.875
  ///     let y = x / 2.25
  ///     // y == 7.5
  ///
  ///     let z = x / 0
  ///     // z.isInfinite == true
  ///
  /// The `/` operator implements the division operation defined by the [IEEE
  /// 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - lhs: The value to divide.
  ///   - rhs: The value to divide `lhs` by.
  static func /(lhs: Self, rhs: Self) -> Self

  /// Divides the first value by the second and stores the quotient in the
  /// left-hand-side variable, rounding to a representable value.
  ///
  /// - Parameters:
  ///   - lhs: The value to divide.
  ///   - rhs: The value to divide `lhs` by.
  static func /=(lhs: inout Self, rhs: Self)

  /// Returns the remainder of this value divided by the given value.
  ///
  /// For two finite values `x` and `y`, the remainder `r` of dividing `x` by
  /// `y` satisfies `x == y * q + r`, where `q` is the integer nearest to
  /// `x / y`. If `x / y` is exactly halfway between two integers, `q` is
  /// chosen to be even. Note that `q` is *not* `x / y` computed in
  /// floating-point arithmetic, and that `q` may not be representable in any
  /// available integer type.
  ///
  /// The following example calculates the remainder of dividing 8.625 by 0.75:
  ///
  ///     let x = 8.625
  ///     print(x / 0.75)
  ///     // Prints "11.5"
  ///
  ///     let q = (x / 0.75).rounded(.toNearestOrEven)
  ///     // q == 12.0
  ///     let r = x.remainder(dividingBy: 0.75)
  ///     // r == -0.375
  ///
  ///     let x1 = 0.75 * q + r
  ///     // x1 == 8.625
  ///
  /// If this value and `other` are finite numbers, the remainder is in the
  /// closed range `-abs(other / 2)...abs(other / 2)`. The
  /// `remainder(dividingBy:)` method is always exact. This method implements
  /// the remainder operation defined by the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameter other: The value to use when dividing this value.
  /// - Returns: The remainder of this value divided by `other`.
  func remainder(dividingBy other: Self) -> Self

  /// Replaces this value with the remainder of itself divided by the given
  /// value.
  ///
  /// For two finite values `x` and `y`, the remainder `r` of dividing `x` by
  /// `y` satisfies `x == y * q + r`, where `q` is the integer nearest to
  /// `x / y`. If `x / y` is exactly halfway between two integers, `q` is
  /// chosen to be even. Note that `q` is *not* `x / y` computed in
  /// floating-point arithmetic, and that `q` may not be representable in any
  /// available integer type.
  ///
  /// The following example calculates the remainder of dividing 8.625 by 0.75:
  ///
  ///     var x = 8.625
  ///     print(x / 0.75)
  ///     // Prints "11.5"
  ///
  ///     let q = (x / 0.75).rounded(.toNearestOrEven)
  ///     // q == 12.0
  ///     x.formRemainder(dividingBy: 0.75)
  ///     // x == -0.375
  ///
  ///     let x1 = 0.75 * q + x
  ///     // x1 == 8.625
  ///
  /// If this value and `other` are finite numbers, the remainder is in the
  /// closed range `-abs(other / 2)...abs(other / 2)`. The
  /// `formRemainder(dividingBy:)` method is always exact.
  ///
  /// - Parameter other: The value to use when dividing this value.
  mutating func formRemainder(dividingBy other: Self)

  /// Returns the remainder of this value divided by the given value using
  /// truncating division.
  ///
  /// Performing truncating division with floating-point values results in a
  /// truncated integer quotient and a remainder. For values `x` and `y` and
  /// their truncated integer quotient `q`, the remainder `r` satisfies
  /// `x == y * q + r`.
  ///
  /// The following example calculates the truncating remainder of dividing
  /// 8.625 by 0.75:
  ///
  ///     let x = 8.625
  ///     print(x / 0.75)
  ///     // Prints "11.5"
  ///
  ///     let q = (x / 0.75).rounded(.towardZero)
  ///     // q == 11.0
  ///     let r = x.truncatingRemainder(dividingBy: 0.75)
  ///     // r == 0.375
  ///
  ///     let x1 = 0.75 * q + r
  ///     // x1 == 8.625
  ///
  /// If this value and `other` are both finite numbers, the truncating
  /// remainder has the same sign as this value and is strictly smaller in
  /// magnitude than `other`. The `truncatingRemainder(dividingBy:)` method
  /// is always exact.
  ///
  /// - Parameter other: The value to use when dividing this value.
  /// - Returns: The remainder of this value divided by `other` using
  ///   truncating division.
  func truncatingRemainder(dividingBy other: Self) -> Self

  /// Replaces this value with the remainder of itself divided by the given
  /// value using truncating division.
  ///
  /// Performing truncating division with floating-point values results in a
  /// truncated integer quotient and a remainder. For values `x` and `y` and
  /// their truncated integer quotient `q`, the remainder `r` satisfies
  /// `x == y * q + r`.
  ///
  /// The following example calculates the truncating remainder of dividing
  /// 8.625 by 0.75:
  ///
  ///     var x = 8.625
  ///     print(x / 0.75)
  ///     // Prints "11.5"
  ///
  ///     let q = (x / 0.75).rounded(.towardZero)
  ///     // q == 11.0
  ///     x.formTruncatingRemainder(dividingBy: 0.75)
  ///     // x == 0.375
  ///
  ///     let x1 = 0.75 * q + x
  ///     // x1 == 8.625
  ///
  /// If this value and `other` are both finite numbers, the truncating
  /// remainder has the same sign as this value and is strictly smaller in
  /// magnitude than `other`. The `formTruncatingRemainder(dividingBy:)`
  /// method is always exact.
  ///
  /// - Parameter other: The value to use when dividing this value.
  mutating func formTruncatingRemainder(dividingBy other: Self)

  /// Returns the square root of the value, rounded to a representable value.
  ///
  /// The following example declares a function that calculates the length of
  /// the hypotenuse of a right triangle given its two perpendicular sides.
  ///
  ///     func hypotenuse(_ a: Double, _ b: Double) -> Double {
  ///         return (a * a + b * b).squareRoot()
  ///     }
  ///
  ///     let (dx, dy) = (3.0, 4.0)
  ///     let distance = hypotenuse(dx, dy)
  ///     // distance == 5.0
  ///
  /// - Returns: The square root of the value.
  func squareRoot() -> Self

  /// Replaces this value with its square root, rounded to a representable
  /// value.
  mutating func formSquareRoot()

  /// Returns the result of adding the product of the two given values to this
  /// value, computed without intermediate rounding.
  ///
  /// This method is equivalent to the C `fma` function and implements the
  /// `fusedMultiplyAdd` operation defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - lhs: One of the values to multiply before adding to this value.
  ///   - rhs: The other value to multiply.
  /// - Returns: The product of `lhs` and `rhs`, added to this value.
  func addingProduct(_ lhs: Self, _ rhs: Self) -> Self

  /// Adds the product of the two given values to this value in place, computed
  /// without intermediate rounding.
  ///
  /// - Parameters:
  ///   - lhs: One of the values to multiply before adding to this value.
  ///   - rhs: The other value to multiply.
  mutating func addProduct(_ lhs: Self, _ rhs: Self)

  /// Returns the lesser of the two given values.
  ///
  /// This method returns the minimum of two values, preserving order and
  /// eliminating NaN when possible. For two values `x` and `y`, the result of
  /// `minimum(x, y)` is `x` if `x <= y`, `y` if `y < x`, or whichever of `x`
  /// or `y` is a number if the other is a quiet NaN. If both `x` and `y` are
  /// NaN, or either `x` or `y` is a signaling NaN, the result is NaN.
  ///
  ///     Double.minimum(10.0, -25.0)
  ///     // -25.0
  ///     Double.minimum(10.0, .nan)
  ///     // 10.0
  ///     Double.minimum(.nan, -25.0)
  ///     // -25.0
  ///     Double.minimum(.nan, .nan)
  ///     // nan
  ///
  /// The `minimum` method implements the `minNum` operation defined by the
  /// [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - x: A floating-point value.
  ///   - y: Another floating-point value.
  /// - Returns: The minimum of `x` and `y`, or whichever is a number if the
  ///   other is NaN.
  static func minimum(_ x: Self, _ y: Self) -> Self

  /// Returns the greater of the two given values.
  ///
  /// This method returns the maximum of two values, preserving order and
  /// eliminating NaN when possible. For two values `x` and `y`, the result of
  /// `maximum(x, y)` is `x` if `x > y`, `y` if `x <= y`, or whichever of `x`
  /// or `y` is a number if the other is a quiet NaN. If both `x` and `y` are
  /// NaN, or either `x` or `y` is a signaling NaN, the result is NaN.
  ///
  ///     Double.maximum(10.0, -25.0)
  ///     // 10.0
  ///     Double.maximum(10.0, .nan)
  ///     // 10.0
  ///     Double.maximum(.nan, -25.0)
  ///     // -25.0
  ///     Double.maximum(.nan, .nan)
  ///     // nan
  ///
  /// The `maximum` method implements the `maxNum` operation defined by the
  /// [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - x: A floating-point value.
  ///   - y: Another floating-point value.
  /// - Returns: The greater of `x` and `y`, or whichever is a number if the
  ///   other is NaN.
  static func maximum(_ x: Self, _ y: Self) -> Self

  /// Returns the value with lesser magnitude.
  ///
  /// This method returns the value with lesser magnitude of the two given
  /// values, preserving order and eliminating NaN when possible. For two
  /// values `x` and `y`, the result of `minimumMagnitude(x, y)` is `x` if
  /// `x.magnitude <= y.magnitude`, `y` if `y.magnitude < x.magnitude`, or
  /// whichever of `x` or `y` is a number if the other is a quiet NaN. If both
  /// `x` and `y` are NaN, or either `x` or `y` is a signaling NaN, the result
  /// is NaN.
  ///
  ///     Double.minimumMagnitude(10.0, -25.0)
  ///     // 10.0
  ///     Double.minimumMagnitude(10.0, .nan)
  ///     // 10.0
  ///     Double.minimumMagnitude(.nan, -25.0)
  ///     // -25.0
  ///     Double.minimumMagnitude(.nan, .nan)
  ///     // nan
  ///
  /// The `minimumMagnitude` method implements the `minNumMag` operation
  /// defined by the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - x: A floating-point value.
  ///   - y: Another floating-point value.
  /// - Returns: Whichever of `x` or `y` has lesser magnitude, or whichever is
  ///   a number if the other is NaN.
  static func minimumMagnitude(_ x: Self, _ y: Self) -> Self

  /// Returns the value with greater magnitude.
  ///
  /// This method returns the value with greater magnitude of the two given
  /// values, preserving order and eliminating NaN when possible. For two
  /// values `x` and `y`, the result of `maximumMagnitude(x, y)` is `x` if
  /// `x.magnitude > y.magnitude`, `y` if `x.magnitude <= y.magnitude`, or
  /// whichever of `x` or `y` is a number if the other is a quiet NaN. If both
  /// `x` and `y` are NaN, or either `x` or `y` is a signaling NaN, the result
  /// is NaN.
  ///
  ///     Double.maximumMagnitude(10.0, -25.0)
  ///     // -25.0
  ///     Double.maximumMagnitude(10.0, .nan)
  ///     // 10.0
  ///     Double.maximumMagnitude(.nan, -25.0)
  ///     // -25.0
  ///     Double.maximumMagnitude(.nan, .nan)
  ///     // nan
  ///
  /// The `maximumMagnitude` method implements the `maxNumMag` operation
  /// defined by the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - x: A floating-point value.
  ///   - y: Another floating-point value.
  /// - Returns: Whichever of `x` or `y` has greater magnitude, or whichever is
  ///   a number if the other is NaN.
  static func maximumMagnitude(_ x: Self, _ y: Self) -> Self

  /// Returns this value rounded to an integral value using the specified
  /// rounding rule.
  ///
  /// The following example rounds a value using four different rounding rules:
  ///
  ///     let x = 6.5
  ///
  ///     // Equivalent to the C 'round' function:
  ///     print(x.rounded(.toNearestOrAwayFromZero))
  ///     // Prints "7.0"
  ///
  ///     // Equivalent to the C 'trunc' function:
  ///     print(x.rounded(.towardZero))
  ///     // Prints "6.0"
  ///
  ///     // Equivalent to the C 'ceil' function:
  ///     print(x.rounded(.up))
  ///     // Prints "7.0"
  ///
  ///     // Equivalent to the C 'floor' function:
  ///     print(x.rounded(.down))
  ///     // Prints "6.0"
  ///
  /// For more information about the available rounding rules, see the
  /// `FloatingPointRoundingRule` enumeration. To round a value using the
  /// default "schoolbook rounding", you can use the shorter `rounded()`
  /// method instead.
  ///
  ///     print(x.rounded())
  ///     // Prints "7.0"
  ///
  /// - Parameter rule: The rounding rule to use.
  /// - Returns: The integral value found by rounding using `rule`.
  func rounded(_ rule: FloatingPointRoundingRule) -> Self

  /// Rounds the value to an integral value using the specified rounding rule.
  ///
  /// The following example rounds a value using four different rounding rules:
  ///
  ///     // Equivalent to the C 'round' function:
  ///     var w = 6.5
  ///     w.round(.toNearestOrAwayFromZero)
  ///     // w == 7.0
  ///
  ///     // Equivalent to the C 'trunc' function:
  ///     var x = 6.5
  ///     x.round(.towardZero)
  ///     // x == 6.0
  ///
  ///     // Equivalent to the C 'ceil' function:
  ///     var y = 6.5
  ///     y.round(.up)
  ///     // y == 7.0
  ///
  ///     // Equivalent to the C 'floor' function:
  ///     var z = 6.5
  ///     z.round(.down)
  ///     // z == 6.0
  ///
  /// For more information about the available rounding rules, see the
  /// `FloatingPointRoundingRule` enumeration. To round a value using the
  /// default "schoolbook rounding", you can use the shorter `round()` method
  /// instead.
  ///
  ///     var w1 = 6.5
  ///     w1.round()
  ///     // w1 == 7.0
  ///
  /// - Parameter rule: The rounding rule to use.
  mutating func round(_ rule: FloatingPointRoundingRule)

  /// The least representable value that compares greater than this value.
  ///
  /// For any finite value `x`, `x.nextUp` is greater than `x`. For `nan` or
  /// `infinity`, `x.nextUp` is `x` itself. The following special cases also
  /// apply:
  ///
  /// - If `x` is `-infinity`, then `x.nextUp` is `-greatestFiniteMagnitude`.
  /// - If `x` is `-leastNonzeroMagnitude`, then `x.nextUp` is `-0.0`.
  /// - If `x` is zero, then `x.nextUp` is `leastNonzeroMagnitude`.
  /// - If `x` is `greatestFiniteMagnitude`, then `x.nextUp` is `infinity`.
  var nextUp: Self { get }

  /// The greatest representable value that compares less than this value.
  ///
  /// For any finite value `x`, `x.nextDown` is less than `x`. For `nan` or
  /// `-infinity`, `x.nextDown` is `x` itself. The following special cases
  /// also apply:
  ///
  /// - If `x` is `infinity`, then `x.nextDown` is `greatestFiniteMagnitude`.
  /// - If `x` is `leastNonzeroMagnitude`, then `x.nextDown` is `0.0`.
  /// - If `x` is zero, then `x.nextDown` is `-leastNonzeroMagnitude`.
  /// - If `x` is `-greatestFiniteMagnitude`, then `x.nextDown` is `-infinity`.
  var nextDown: Self { get }

  /// Returns a Boolean value indicating whether this instance is equal to the
  /// given value.
  ///
  /// This method serves as the basis for the equal-to operator (`==`) for
  /// floating-point values. When comparing two values with this method, `-0`
  /// is equal to `+0`. NaN is not equal to any value, including itself. For
  /// example:
  ///
  ///     let x = 15.0
  ///     x.isEqual(to: 15.0)
  ///     // true
  ///     x.isEqual(to: .nan)
  ///     // false
  ///     Double.nan.isEqual(to: .nan)
  ///     // false
  ///
  /// The `isEqual(to:)` method implements the equality predicate defined by
  /// the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameter other: The value to compare with this value.
  /// - Returns: `true` if `other` has the same value as this instance;
  ///   otherwise, `false`. If either this value or `other` is NaN, the result
  ///   of this method is `false`.
  func isEqual(to other: Self) -> Bool

  /// Returns a Boolean value indicating whether this instance is less than the
  /// given value.
  ///
  /// This method serves as the basis for the less-than operator (`<`) for
  /// floating-point values. Some special cases apply:
  ///
  /// - Because NaN compares not less than nor greater than any value, this
  ///   method returns `false` when called on NaN or when NaN is passed as
  ///   `other`.
  /// - `-infinity` compares less than all values except for itself and NaN.
  /// - Every value except for NaN and `+infinity` compares less than
  ///   `+infinity`.
  ///
  ///     let x = 15.0
  ///     x.isLess(than: 20.0)
  ///     // true
  ///     x.isLess(than: .nan)
  ///     // false
  ///     Double.nan.isLess(than: x)
  ///     // false
  ///
  /// The `isLess(than:)` method implements the less-than predicate defined by
  /// the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameter other: The value to compare with this value.
  /// - Returns: `true` if this value is less than `other`; otherwise, `false`.
  ///   If either this value or `other` is NaN, the result of this method is
  ///   `false`.
  func isLess(than other: Self) -> Bool

  /// Returns a Boolean value indicating whether this instance is less than or
  /// equal to the given value.
  ///
  /// This method serves as the basis for the less-than-or-equal-to operator
  /// (`<=`) for floating-point values. Some special cases apply:
  ///
  /// - Because NaN is incomparable with any value, this method returns `false`
  ///   when called on NaN or when NaN is passed as `other`.
  /// - `-infinity` compares less than or equal to all values except NaN.
  /// - Every value except NaN compares less than or equal to `+infinity`.
  ///
  ///     let x = 15.0
  ///     x.isLessThanOrEqualTo(20.0)
  ///     // true
  ///     x.isLessThanOrEqualTo(.nan)
  ///     // false
  ///     Double.nan.isLessThanOrEqualTo(x)
  ///     // false
  ///
  /// The `isLessThanOrEqualTo(_:)` method implements the less-than-or-equal
  /// predicate defined by the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameter other: The value to compare with this value.
  /// - Returns: `true` if `other` is greater than this value; otherwise,
  ///   `false`. If either this value or `other` is NaN, the result of this
  ///   method is `false`.
  func isLessThanOrEqualTo(_ other: Self) -> Bool

  /// Returns a Boolean value indicating whether this instance should precede
  /// or tie positions with the given value in an ascending sort.
  ///
  /// This relation is a refinement of the less-than-or-equal-to operator
  /// (`<=`) that provides a total order on all values of the type, including
  /// signed zeros and NaNs.
  ///
  /// The following example uses `isTotallyOrdered(belowOrEqualTo:)` to sort an
  /// array of floating-point values, including some that are NaN:
  ///
  ///     var numbers = [2.5, 21.25, 3.0, .nan, -9.5]
  ///     numbers.sort { !$1.isTotallyOrdered(belowOrEqualTo: $0) }
  ///     print(numbers)
  ///     // Prints "[-9.5, 2.5, 3.0, 21.25, nan]"
  ///
  /// The `isTotallyOrdered(belowOrEqualTo:)` method implements the total order
  /// relation as defined by the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameter other: A floating-point value to compare to this value.
  /// - Returns: `true` if this value is ordered below or the same as `other`
  ///   in a total ordering of the floating-point type; otherwise, `false`.
  func isTotallyOrdered(belowOrEqualTo other: Self) -> Bool

  /// A Boolean value indicating whether this instance is normal.
  ///
  /// A *normal* value is a finite number that uses the full precision
  /// available to values of a type. Zero is neither a normal nor a subnormal
  /// number.
  var isNormal: Bool { get }

  /// A Boolean value indicating whether this instance is finite.
  ///
  /// All values other than NaN and infinity are considered finite, whether
  /// normal or subnormal.  For NaN, both `isFinite` and `isInfinite` are false.
  var isFinite: Bool { get }

  /// A Boolean value indicating whether the instance is equal to zero.
  ///
  /// The `isZero` property of a value `x` is `true` when `x` represents either
  /// `-0.0` or `+0.0`. `x.isZero` is equivalent to the following comparison:
  /// `x == 0.0`.
  ///
  ///     let x = -0.0
  ///     x.isZero        // true
  ///     x == 0.0        // true
  var isZero: Bool { get }

  /// A Boolean value indicating whether the instance is subnormal.
  ///
  /// A *subnormal* value is a nonzero number that has a lesser magnitude than
  /// the smallest normal number. Subnormal values don't use the full
  /// precision available to values of a type.
  ///
  /// Zero is neither a normal nor a subnormal number. Subnormal numbers are
  /// often called *denormal* or *denormalized*---these are different names
  /// for the same concept.
  var isSubnormal: Bool { get }

  /// A Boolean value indicating whether the instance is infinite.
  ///
  /// For NaN, both `isFinite` and `isInfinite` are false.
  var isInfinite: Bool { get }

  /// A Boolean value indicating whether the instance is NaN ("not a number").
  ///
  /// Because NaN is not equal to any value, including NaN, use this property
  /// instead of the equal-to operator (`==`) or not-equal-to operator (`!=`)
  /// to test whether a value is or is not NaN. For example:
  ///
  ///     let x = 0.0
  ///     let y = x * .infinity
  ///     // y is a NaN
  ///
  ///     // Comparing with the equal-to operator never returns 'true'
  ///     print(x == Double.nan)
  ///     // Prints "false"
  ///     print(y == Double.nan)
  ///     // Prints "false"
  ///
  ///     // Test with the 'isNaN' property instead
  ///     print(x.isNaN)
  ///     // Prints "false"
  ///     print(y.isNaN)
  ///     // Prints "true"
  ///
  /// This property is `true` for both quiet and signaling NaNs.
  var isNaN: Bool { get }

  /// A Boolean value indicating whether the instance is a signaling NaN.
  ///
  /// Signaling NaNs typically raise the Invalid flag when used in general
  /// computing operations.
  var isSignalingNaN: Bool { get }

  /// The classification of this value.
  ///
  /// A value's `floatingPointClass` property describes its "class" as
  /// described by the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  var floatingPointClass: FloatingPointClassification { get }

  /// A Boolean value indicating whether the instance's representation is in
  /// its canonical form.
  ///
  /// The [IEEE 754 specification][spec] defines a *canonical*, or preferred,
  /// encoding of a floating-point value. On platforms that fully support
  /// IEEE 754, every `Float` or `Double` value is canonical, but
  /// non-canonical values can exist on other platforms or for other types.
  /// Some examples:
  ///
  /// - On platforms that flush subnormal numbers to zero (such as armv7
  ///   with the default floating-point environment), Swift interprets
  ///   subnormal `Float` and `Double` values as non-canonical zeros.
  ///   (In Swift 5.1 and earlier, `isCanonical` is `true` for these
  ///   values, which is the incorrect value.)
  ///
  /// - On i386 and x86_64, `Float80` has a number of non-canonical
  ///   encodings. "Pseudo-NaNs", "pseudo-infinities", and "unnormals" are
  ///   interpreted as non-canonical NaN encodings. "Pseudo-denormals" are
  ///   interpreted as non-canonical encodings of subnormal values.
  ///
  /// - Decimal floating-point types admit a large number of non-canonical
  ///   encodings. Consult the IEEE 754 standard for additional details.
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  var isCanonical: Bool { get }
}

/// The sign of a floating-point value.
@frozen
public enum FloatingPointSign: Int, Sendable {
  /// The sign for a positive value.
  case plus

  /// The sign for a negative value.
  case minus

  // Explicit declarations of otherwise-synthesized members to make them
  // @inlinable, promising that we will never change the implementation.

  @inlinable
  public init?(rawValue: Int) {
    switch rawValue {
    case 0: self = .plus
    case 1: self = .minus
    default: return nil
    }
  }

  @inlinable
  public var rawValue: Int {
    switch self {
    case .plus: return 0
    case .minus: return 1
    }
  }

  @_transparent
  @inlinable
  public static func ==(a: FloatingPointSign, b: FloatingPointSign) -> Bool {
    return a.rawValue == b.rawValue
  }

  @inlinable
  public var hashValue: Int { return rawValue.hashValue }

  @inlinable
  public func hash(into hasher: inout Hasher) {
    hasher.combine(rawValue)
  }

  @inlinable
  public func _rawHashValue(seed: Int) -> Int {
    return rawValue._rawHashValue(seed: seed)
  }
}

/// The IEEE 754 floating-point classes.
@frozen
public enum FloatingPointClassification: Sendable {
  /// A signaling NaN ("not a number").
  ///
  /// A signaling NaN sets the floating-point exception status when used in
  /// many floating-point operations.
  case signalingNaN

  /// A silent NaN ("not a number") value.
  case quietNaN

  /// A value equal to `-infinity`.
  case negativeInfinity

  /// A negative value that uses the full precision of the floating-point type.
  case negativeNormal

  /// A negative, nonzero number that does not use the full precision of the
  /// floating-point type.
  case negativeSubnormal

  /// A value equal to zero with a negative sign.
  case negativeZero

  /// A value equal to zero with a positive sign.
  case positiveZero

  /// A positive, nonzero number that does not use the full precision of the
  /// floating-point type.
  case positiveSubnormal

  /// A positive value that uses the full precision of the floating-point type.
  case positiveNormal

  /// A value equal to `+infinity`.
  case positiveInfinity
}

/// A rule for rounding a floating-point number.
public enum FloatingPointRoundingRule: Sendable {
  /// Round to the closest allowed value; if two values are equally close, the
  /// one with greater magnitude is chosen.
  ///
  /// This rounding rule is also known as "schoolbook rounding." The following
  /// example shows the results of rounding numbers using this rule:
  ///
  ///     (5.2).rounded(.toNearestOrAwayFromZero)
  ///     // 5.0
  ///     (5.5).rounded(.toNearestOrAwayFromZero)
  ///     // 6.0
  ///     (-5.2).rounded(.toNearestOrAwayFromZero)
  ///     // -5.0
  ///     (-5.5).rounded(.toNearestOrAwayFromZero)
  ///     // -6.0
  ///
  /// This rule is equivalent to the C `round` function and implements the
  /// `roundToIntegralTiesToAway` operation defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  case toNearestOrAwayFromZero

  /// Round to the closest allowed value; if two values are equally close, the
  /// even one is chosen.
  ///
  /// This rounding rule is also known as "bankers rounding," and is the
  /// default IEEE 754 rounding mode for arithmetic. The following example
  /// shows the results of rounding numbers using this rule:
  ///
  ///     (5.2).rounded(.toNearestOrEven)
  ///     // 5.0
  ///     (5.5).rounded(.toNearestOrEven)
  ///     // 6.0
  ///     (4.5).rounded(.toNearestOrEven)
  ///     // 4.0
  ///
  /// This rule implements the `roundToIntegralTiesToEven` operation defined by
  /// the [IEEE 754 specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  case toNearestOrEven

  /// Round to the closest allowed value that is greater than or equal to the
  /// source.
  ///
  /// The following example shows the results of rounding numbers using this
  /// rule:
  ///
  ///     (5.2).rounded(.up)
  ///     // 6.0
  ///     (5.5).rounded(.up)
  ///     // 6.0
  ///     (-5.2).rounded(.up)
  ///     // -5.0
  ///     (-5.5).rounded(.up)
  ///     // -5.0
  ///
  /// This rule is equivalent to the C `ceil` function and implements the
  /// `roundToIntegralTowardPositive` operation defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  case up

  /// Round to the closest allowed value that is less than or equal to the
  /// source.
  ///
  /// The following example shows the results of rounding numbers using this
  /// rule:
  ///
  ///     (5.2).rounded(.down)
  ///     // 5.0
  ///     (5.5).rounded(.down)
  ///     // 5.0
  ///     (-5.2).rounded(.down)
  ///     // -6.0
  ///     (-5.5).rounded(.down)
  ///     // -6.0
  ///
  /// This rule is equivalent to the C `floor` function and implements the
  /// `roundToIntegralTowardNegative` operation defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  case down

  /// Round to the closest allowed value whose magnitude is less than or equal
  /// to that of the source.
  ///
  /// The following example shows the results of rounding numbers using this
  /// rule:
  ///
  ///     (5.2).rounded(.towardZero)
  ///     // 5.0
  ///     (5.5).rounded(.towardZero)
  ///     // 5.0
  ///     (-5.2).rounded(.towardZero)
  ///     // -5.0
  ///     (-5.5).rounded(.towardZero)
  ///     // -5.0
  ///
  /// This rule is equivalent to the C `trunc` function and implements the
  /// `roundToIntegralTowardZero` operation defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  case towardZero

  /// Round to the closest allowed value whose magnitude is greater than or
  /// equal to that of the source.
  ///
  /// The following example shows the results of rounding numbers using this
  /// rule:
  ///
  ///     (5.2).rounded(.awayFromZero)
  ///     // 6.0
  ///     (5.5).rounded(.awayFromZero)
  ///     // 6.0
  ///     (-5.2).rounded(.awayFromZero)
  ///     // -6.0
  ///     (-5.5).rounded(.awayFromZero)
  ///     // -6.0
  case awayFromZero
}

extension FloatingPoint {
  @_transparent
  public static func == (lhs: Self, rhs: Self) -> Bool {
    return lhs.isEqual(to: rhs)
  }

  @_transparent
  public static func < (lhs: Self, rhs: Self) -> Bool {
    return lhs.isLess(than: rhs)
  }

  @_transparent
  public static func <= (lhs: Self, rhs: Self) -> Bool {
    return lhs.isLessThanOrEqualTo(rhs)
  }

  @_transparent
  public static func > (lhs: Self, rhs: Self) -> Bool {
    return rhs.isLess(than: lhs)
  }

  @_transparent
  public static func >= (lhs: Self, rhs: Self) -> Bool {
    return rhs.isLessThanOrEqualTo(lhs)
  }
}

/// A radix-2 (binary) floating-point type.
///
/// The `BinaryFloatingPoint` protocol extends the `FloatingPoint` protocol
/// with operations specific to floating-point binary types, as defined by the
/// [IEEE 754 specification][spec]. `BinaryFloatingPoint` is implemented in
/// the standard library by `Float`, `Double`, and `Float80` where available.
///
/// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
public protocol BinaryFloatingPoint: FloatingPoint, ExpressibleByFloatLiteral {

  /// A type that represents the encoded significand of a value.
  associatedtype RawSignificand: UnsignedInteger

  /// A type that represents the encoded exponent of a value.
  associatedtype RawExponent: UnsignedInteger

  /// Creates a new instance from the specified sign and bit patterns.
  ///
  /// The values passed as `exponentBitPattern` and `significandBitPattern` are
  /// interpreted in the binary interchange format defined by the [IEEE 754
  /// specification][spec].
  ///
  /// [spec]: http://ieeexplore.ieee.org/servlet/opac?punumber=4610933
  ///
  /// - Parameters:
  ///   - sign: The sign of the new value.
  ///   - exponentBitPattern: The bit pattern to use for the exponent field of
  ///     the new value.
  ///   - significandBitPattern: The bit pattern to use for the significand
  ///     field of the new value.
  init(sign: FloatingPointSign,
       exponentBitPattern: RawExponent,
       significandBitPattern: RawSignificand)

  /// Creates a new instance from the given value, rounded to the closest
  /// possible representation.
  ///
  /// - Parameter value: A floating-point value to be converted.
  init(_ value: Float)

  /// Creates a new instance from the given value, rounded to the closest
  /// possible representation.
  ///
  /// - Parameter value: A floating-point value to be converted.
  init(_ value: Double)

#if !(os(Windows) || os(Android) || os(FreeBSD)) && (arch(i386) || arch(x86_64))
  /// Creates a new instance from the given value, rounded to the closest
  /// possible representation.
  ///
  /// - Parameter value: A floating-point value to be converted.
  init(_ value: Float80)
#endif

  /// Creates a new instance from the given value, rounded to the closest
  /// possible representation.
  ///
  /// If two representable values are equally close, the result is the value
  /// with more trailing zeros in its significand bit pattern.
  ///
  /// - Parameter value: A floating-point value to be converted.
  init<Source: BinaryFloatingPoint>(_ value: Source)

  /// Creates a new instance from the given value, if it can be represented
  /// exactly.
  ///
  /// If the given floating-point value cannot be represented exactly, the
  /// result is `nil`. A value that is NaN ("not a number") cannot be
  /// represented exactly if its payload cannot be encoded exactly.
  ///
  /// - Parameter value: A floating-point value to be converted.
  init?<Source: BinaryFloatingPoint>(exactly value: Source)

  /// The number of bits used to represent the type's exponent.
  ///
  /// A binary floating-point type's `exponentBitCount` imposes a limit on the
  /// range of the exponent for normal, finite values. The *exponent bias* of
  /// a type `F` can be calculated as the following, where `**` is
  /// exponentiation:
  ///
  ///     let bias = 2 ** (F.exponentBitCount - 1) - 1
  ///
  /// The least normal exponent for values of the type `F` is `1 - bias`, and
  /// the largest finite exponent is `bias`. An all-zeros exponent is reserved
  /// for subnormals and zeros, and an all-ones exponent is reserved for
  /// infinity and NaN.
  ///
  /// For example, the `Float` type has an `exponentBitCount` of 8, which gives
  /// an exponent bias of `127` by the calculation above.
  ///
  ///     let bias = 2 ** (Float.exponentBitCount - 1) - 1
  ///     // bias == 127
  ///     print(Float.greatestFiniteMagnitude.exponent)
  ///     // Prints "127"
  ///     print(Float.leastNormalMagnitude.exponent)
  ///     // Prints "-126"
  static var exponentBitCount: Int { get }

  /// The available number of fractional significand bits.
  ///
  /// For fixed-width floating-point types, this is the actual number of
  /// fractional significand bits.
  ///
  /// For extensible floating-point types, `significandBitCount` should be the
  /// maximum allowed significand width (without counting any leading integral
  /// bit of the significand). If there is no upper limit, then
  /// `significandBitCount` should be `Int.max`.
  ///
  /// Note that `Float80.significandBitCount` is 63, even though 64 bits are
  /// used to store the significand in the memory representation of a
  /// `Float80` (unlike other floating-point types, `Float80` explicitly
  /// stores the leading integral significand bit, but the
  /// `BinaryFloatingPoint` APIs provide an abstraction so that users don't
  /// need to be aware of this detail).
  static var significandBitCount: Int { get }

  /// The raw encoding of the value's exponent field.
  ///
  /// This value is unadjusted by the type's exponent bias.
  var exponentBitPattern: RawExponent { get }

  /// The raw encoding of the value's significand field.
  ///
  /// The `significandBitPattern` property does not include the leading
  /// integral bit of the significand, even for types like `Float80` that
  /// store it explicitly.
  var significandBitPattern: RawSignificand { get }

  /// The floating-point value with the same sign and exponent as this value,
  /// but with a significand of 1.0.
  ///
  /// A *binade* is a set of binary floating-point values that all have the
  /// same sign and exponent. The `binade` property is a member of the same
  /// binade as this value, but with a unit significand.
  ///
  /// In this example, `x` has a value of `21.5`, which is stored as
  /// `1.34375 * 2**4`, where `**` is exponentiation. Therefore, `x.binade` is
  /// equal to `1.0 * 2**4`, or `16.0`.
  ///
  ///     let x = 21.5
  ///     // x.significand == 1.34375
  ///     // x.exponent == 4
  ///
  ///     let y = x.binade
  ///     // y == 16.0
  ///     // y.significand == 1.0
  ///     // y.exponent == 4
  var binade: Self { get }

  /// The number of bits required to represent the value's significand.
  ///
  /// If this value is a finite nonzero number, `significandWidth` is the
  /// number of fractional bits required to represent the value of
  /// `significand`; otherwise, `significandWidth` is -1. The value of
  /// `significandWidth` is always -1 or between zero and
  /// `significandBitCount`. For example:
  ///
  /// - For any representable power of two, `significandWidth` is zero, because
  ///   `significand` is `1.0`.
  /// - If `x` is 10, `x.significand` is `1.01` in binary, so
  ///   `x.significandWidth` is 2.
  /// - If `x` is Float.pi, `x.significand` is `1.10010010000111111011011` in
  ///   binary, and `x.significandWidth` is 23.
  var significandWidth: Int { get }
}

extension FloatingPoint {

  @inlinable // FIXME(sil-serialize-all)
  public static var ulpOfOne: Self {
    return (1 as Self).ulp
  }

  @_transparent
  public func rounded(_ rule: FloatingPointRoundingRule) -> Self {
    var lhs = self
    lhs.round(rule)
    return lhs
  }

  @_transparent
  public func rounded() -> Self {
    return rounded(.toNearestOrAwayFromZero)
  }

  @_transparent
  public mutating func round() {
    round(.toNearestOrAwayFromZero)
  }

  @inlinable // FIXME(inline-always)
  public var nextDown: Self {
    @inline(__always)
    get {
      return -(-self).nextUp
    }
  }

  @inlinable // FIXME(inline-always)
  @inline(__always)
  public func truncatingRemainder(dividingBy other: Self) -> Self {
    var lhs = self
    lhs.formTruncatingRemainder(dividingBy: other)
    return lhs
  }

  @inlinable // FIXME(inline-always)
  @inline(__always)
  public func remainder(dividingBy other: Self) -> Self {
    var lhs = self
    lhs.formRemainder(dividingBy: other)
    return lhs
  }

  @_transparent
  public func squareRoot( ) -> Self {
    var lhs = self
    lhs.formSquareRoot( )
    return lhs
  }

  @_transparent
  public func addingProduct(_ lhs: Self, _ rhs: Self) -> Self {
    var addend = self
    addend.addProduct(lhs, rhs)
    return addend
  }

  @inlinable
  public static func minimum(_ x: Self, _ y: Self) -> Self {
    if x <= y || y.isNaN { return x }
    return y
  }

  @inlinable
  public static func maximum(_ x: Self, _ y: Self) -> Self {
    if x > y || y.isNaN { return x }
    return y
  }

  @inlinable
  public static func minimumMagnitude(_ x: Self, _ y: Self) -> Self {
    if x.magnitude <= y.magnitude || y.isNaN { return x }
    return y
  }

  @inlinable
  public static func maximumMagnitude(_ x: Self, _ y: Self) -> Self {
    if x.magnitude > y.magnitude || y.isNaN { return x }
    return y
  }

  @inlinable
  public var floatingPointClass: FloatingPointClassification {
    if isSignalingNaN { return .signalingNaN }
    if isNaN { return .quietNaN }
    if isInfinite { return sign == .minus ? .negativeInfinity : .positiveInfinity }
    if isNormal { return sign == .minus ? .negativeNormal : .positiveNormal }
    if isSubnormal { return sign == .minus ? .negativeSubnormal : .positiveSubnormal }
    return sign == .minus ? .negativeZero : .positiveZero
  }
}

extension BinaryFloatingPoint {

  @inlinable @inline(__always)
  public static var radix: Int { return 2 }

  @inlinable
  public init(signOf: Self, magnitudeOf: Self) {
    self.init(
      sign: signOf.sign,
      exponentBitPattern: magnitudeOf.exponentBitPattern,
      significandBitPattern: magnitudeOf.significandBitPattern
    )
  }

  public // @testable
  static func _convert<Source: BinaryFloatingPoint>(
    from source: Source
  ) -> (value: Self, exact: Bool) {
    guard _fastPath(!source.isZero) else {
      return (source.sign == .minus ? -0.0 : 0, true)
    }

    guard _fastPath(source.isFinite) else {
      if source.isInfinite {
        return (source.sign == .minus ? -.infinity : .infinity, true)
      }
      // IEEE 754 requires that any NaN payload be propagated, if possible.
      let payload_ =
        source.significandBitPattern &
          ~(Source.nan.significandBitPattern |
            Source.signalingNaN.significandBitPattern)
      let mask =
        Self.greatestFiniteMagnitude.significandBitPattern &
          ~(Self.nan.significandBitPattern |
            Self.signalingNaN.significandBitPattern)
      let payload = Self.RawSignificand(truncatingIfNeeded: payload_) & mask
      // Although .signalingNaN.exponentBitPattern == .nan.exponentBitPattern,
      // we do not *need* to rely on this relation, and therefore we do not.
      let value = source.isSignalingNaN
        ? Self(
          sign: source.sign,
          exponentBitPattern: Self.signalingNaN.exponentBitPattern,
          significandBitPattern: payload |
            Self.signalingNaN.significandBitPattern)
        : Self(
          sign: source.sign,
          exponentBitPattern: Self.nan.exponentBitPattern,
          significandBitPattern: payload | Self.nan.significandBitPattern)
      // We define exactness by equality after roundtripping; since NaN is never
      // equal to itself, it can never be converted exactly.
      return (value, false)
    }

    let exponent = source.exponent
    var exemplar = Self.leastNormalMagnitude
    let exponentBitPattern: Self.RawExponent
    let leadingBitIndex: Int
    let shift: Int
    let significandBitPattern: Self.RawSignificand

    if exponent < exemplar.exponent {
      // The floating-point result is either zero or subnormal.
      exemplar = Self.leastNonzeroMagnitude
      let minExponent = exemplar.exponent
      if exponent + 1 < minExponent {
        return (source.sign == .minus ? -0.0 : 0, false)
      }
      if _slowPath(exponent + 1 == minExponent) {
        // Although the most significant bit (MSB) of a subnormal source
        // significand is explicit, Swift BinaryFloatingPoint APIs actually
        // omit any explicit MSB from the count represented in
        // significandWidth. For instance:
        //
        //   Double.leastNonzeroMagnitude.significandWidth == 0
        //
        // Therefore, we do not need to adjust our work here for a subnormal
        // source.
        return source.significandWidth == 0
          ? (source.sign == .minus ? -0.0 : 0, false)
          : (source.sign == .minus ? -exemplar : exemplar, false)
      }

      exponentBitPattern = 0 as Self.RawExponent
      leadingBitIndex = Int(Self.Exponent(exponent) - minExponent)
      shift =
        leadingBitIndex &-
        (source.significandWidth &+
          source.significandBitPattern.trailingZeroBitCount)
      let leadingBit = source.isNormal
        ? (1 as Self.RawSignificand) << leadingBitIndex
        : 0
      significandBitPattern = leadingBit | (shift >= 0
        ? Self.RawSignificand(source.significandBitPattern) << shift
        : Self.RawSignificand(source.significandBitPattern >> -shift))
    } else {
      // The floating-point result is either normal or infinite.
      exemplar = Self.greatestFiniteMagnitude
      if exponent > exemplar.exponent {
        return (source.sign == .minus ? -.infinity : .infinity, false)
      }

      exponentBitPattern = exponent < 0
        ? (1 as Self).exponentBitPattern - Self.RawExponent(-exponent)
        : (1 as Self).exponentBitPattern + Self.RawExponent(exponent)
      leadingBitIndex = exemplar.significandWidth
      shift =
        leadingBitIndex &-
          (source.significandWidth &+
            source.significandBitPattern.trailingZeroBitCount)
      let sourceLeadingBit = source.isSubnormal
        ? (1 as Source.RawSignificand) <<
          (source.significandWidth &+
            source.significandBitPattern.trailingZeroBitCount)
        : 0
      significandBitPattern = shift >= 0
        ? Self.RawSignificand(
          sourceLeadingBit ^ source.significandBitPattern) << shift
        : Self.RawSignificand(
          (sourceLeadingBit ^ source.significandBitPattern) >> -shift)
    }

    let value = Self(
      sign: source.sign,
      exponentBitPattern: exponentBitPattern,
      significandBitPattern: significandBitPattern)

    if source.significandWidth <= leadingBitIndex {
      return (value, true)
    }
    // We promise to round to the closest representation. Therefore, we must
    // take a look at the bits that we've just truncated.
    let ulp = (1 as Source.RawSignificand) << -shift
    let truncatedBits = source.significandBitPattern & (ulp - 1)
    if truncatedBits < ulp / 2 {
      return (value, false)
    }
    let rounded = source.sign == .minus ? value.nextDown : value.nextUp
    if _fastPath(truncatedBits > ulp / 2) {
      return (rounded, false)
    }
    // If two representable values are equally close, we return the value with
    // more trailing zeros in its significand bit pattern.
    return
      significandBitPattern.trailingZeroBitCount >
        rounded.significandBitPattern.trailingZeroBitCount
      ? (value, false)
      : (rounded, false)
  }

  /// Creates a new instance from the given value, rounded to the closest
  /// possible representation.
  ///
  /// If two representable values are equally close, the result is the value
  /// with more trailing zeros in its significand bit pattern.
  ///
  /// - Parameter value: A floating-point value to be converted.
  @inlinable
  public init<Source: BinaryFloatingPoint>(_ value: Source) {
    // If two IEEE 754 binary interchange formats share the same exponent bit
    // count and significand bit count, then they must share the same encoding
    // for finite and infinite values.
    switch (Source.exponentBitCount, Source.significandBitCount) {
#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
    case (5, 10):
      guard #available(macOS 11.0, iOS 14.0, watchOS 7.0, tvOS 14.0, *) else {
        // Convert signaling NaN to quiet NaN by multiplying by 1.
        self = Self._convert(from: value).value * 1
        break
      }
      let value_ = value as? Float16 ?? Float16(
        sign: value.sign,
        exponentBitPattern:
          UInt(truncatingIfNeeded: value.exponentBitPattern),
        significandBitPattern:
          UInt16(truncatingIfNeeded: value.significandBitPattern))
      self = Self(Float(value_))
#endif
    case (8, 23):
      let value_ = value as? Float ?? Float(
        sign: value.sign,
        exponentBitPattern:
          UInt(truncatingIfNeeded: value.exponentBitPattern),
        significandBitPattern:
          UInt32(truncatingIfNeeded: value.significandBitPattern))
      self = Self(value_)
    case (11, 52):
      let value_ = value as? Double ?? Double(
        sign: value.sign,
        exponentBitPattern:
          UInt(truncatingIfNeeded: value.exponentBitPattern),
        significandBitPattern:
          UInt64(truncatingIfNeeded: value.significandBitPattern))
      self = Self(value_)
#if !(os(Windows) || os(Android) || os(FreeBSD)) && (arch(i386) || arch(x86_64))
    case (15, 63):
      let value_ = value as? Float80 ?? Float80(
        sign: value.sign,
        exponentBitPattern:
          UInt(truncatingIfNeeded: value.exponentBitPattern),
        significandBitPattern:
          UInt64(truncatingIfNeeded: value.significandBitPattern))
      self = Self(value_)
#endif
    default:
      // Convert signaling NaN to quiet NaN by multiplying by 1.
      self = Self._convert(from: value).value * 1
    }
  }

  /// Creates a new instance from the given value, if it can be represented
  /// exactly.
  ///
  /// If the given floating-point value cannot be represented exactly, the
  /// result is `nil`.
  ///
  /// - Parameter value: A floating-point value to be converted.
  @inlinable
  public init?<Source: BinaryFloatingPoint>(exactly value: Source) {
    // We define exactness by equality after roundtripping; since NaN is never
    // equal to itself, it can never be converted exactly.
    if value.isNaN { return nil }
    
    if (Source.exponentBitCount > Self.exponentBitCount
        || Source.significandBitCount > Self.significandBitCount)
      && value.isFinite && !value.isZero {
      let exponent = value.exponent
      if exponent < Self.leastNormalMagnitude.exponent {
        if exponent < Self.leastNonzeroMagnitude.exponent { return nil }
        if value.significandWidth >
          Int(Self.Exponent(exponent) - Self.leastNonzeroMagnitude.exponent) {
          return nil
        }
      } else {
        if exponent > Self.greatestFiniteMagnitude.exponent { return nil }
        if value.significandWidth >
          Self.greatestFiniteMagnitude.significandWidth {
          return nil
        }
      }
    }
    
    self = Self(value)
  }
  
  @inlinable
  public func isTotallyOrdered(belowOrEqualTo other: Self) -> Bool {
    // Quick return when possible.
    if self < other { return true }
    if other > self { return false }
    // Self and other are either equal or unordered.
    // Every negative-signed value (even NaN) is less than every positive-
    // signed value, so if the signs do not match, we simply return the
    // sign bit of self.
    if sign != other.sign { return sign == .minus }
    // Sign bits match; look at exponents.
    if exponentBitPattern > other.exponentBitPattern { return sign == .minus }
    if exponentBitPattern < other.exponentBitPattern { return sign == .plus }
    // Signs and exponents match, look at significands.
    if significandBitPattern > other.significandBitPattern {
      return sign == .minus
    }
    if significandBitPattern < other.significandBitPattern {
      return sign == .plus
    }
    //  Sign, exponent, and significand all match.
    return true
  }
}

extension BinaryFloatingPoint where Self.RawSignificand: FixedWidthInteger {
  
  public // @testable
  static func _convert<Source: BinaryInteger>(
    from source: Source
  ) -> (value: Self, exact: Bool) {
    //  Useful constants:
    let exponentBias = (1 as Self).exponentBitPattern
    let significandMask = ((1 as RawSignificand) << Self.significandBitCount) &- 1
    //  Zero is really extra simple, and saves us from trying to normalize a
    //  value that cannot be normalized.
    if _fastPath(source == 0) { return (0, true) }
    //  We now have a non-zero value; convert it to a strictly positive value
    //  by taking the magnitude.
    let magnitude = source.magnitude
    var exponent = magnitude._binaryLogarithm()
    //  If the exponent would be larger than the largest representable
    //  exponent, the result is just an infinity of the appropriate sign.
    guard exponent <= Self.greatestFiniteMagnitude.exponent else {
      return (Source.isSigned && source < 0 ? -.infinity : .infinity, false)
    }
    //  If exponent <= significandBitCount, we don't need to round it to
    //  construct the significand; we just need to left-shift it into place;
    //  the result is always exact as we've accounted for exponent-too-large
    //  already and no rounding can occur.
    if exponent <= Self.significandBitCount {
      let shift = Self.significandBitCount &- exponent
      let significand = RawSignificand(magnitude) &<< shift
      let value = Self(
        sign: Source.isSigned && source < 0 ? .minus : .plus,
        exponentBitPattern: exponentBias + RawExponent(exponent),
        significandBitPattern: significand
      )
      return (value, true)
    }
    //  exponent > significandBitCount, so we need to do a rounding right
    //  shift, and adjust exponent if needed
    let shift = exponent &- Self.significandBitCount
    let halfway = (1 as Source.Magnitude) << (shift - 1)
    let mask = 2 * halfway - 1
    let fraction = magnitude & mask
    var significand = RawSignificand(truncatingIfNeeded: magnitude >> shift) & significandMask
    if fraction > halfway || (fraction == halfway && significand & 1 == 1) {
      var carry = false
      (significand, carry) = significand.addingReportingOverflow(1)
      if carry || significand > significandMask {
        exponent += 1
        guard exponent <= Self.greatestFiniteMagnitude.exponent else {
          return (Source.isSigned && source < 0 ? -.infinity : .infinity, false)
        }
      }
    }
    return (Self(
      sign: Source.isSigned && source < 0 ? .minus : .plus,
      exponentBitPattern: exponentBias + RawExponent(exponent),
      significandBitPattern: significand
    ), fraction == 0)
  }
  
  /// Creates a new value, rounded to the closest possible representation.
  ///
  /// If two representable values are equally close, the result is the value
  /// with more trailing zeros in its significand bit pattern.
  ///
  /// - Parameter value: The integer to convert to a floating-point value.
  @inlinable
  public init<Source: BinaryInteger>(_ value: Source) {
    self = Self._convert(from: value).value
  }
  
  /// Creates a new value, if the given integer can be represented exactly.
  ///
  /// If the given integer cannot be represented exactly, the result is `nil`.
  ///
  /// - Parameter value: The integer to convert to a floating-point value.
  @inlinable
  public init?<Source: BinaryInteger>(exactly value: Source) {
    let (value_, exact) = Self._convert(from: value)
    guard exact else { return nil }
    self = value_
  }
}
