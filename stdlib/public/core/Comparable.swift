//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A type that can be compared using the relational operators `<`, `<=`, `>=`,
/// and `>`.
///
/// The `Comparable` protocol is used for types that have an inherent order,
/// such as numbers and strings. Many types in the standard library already
/// conform to the `Comparable` protocol. Add `Comparable` conformance to your
/// own custom types when you want to be able to compare instances using
/// relational operators or use standard library methods that are designed for
/// `Comparable` types.
///
/// The most familiar use of relational operators is to compare numbers, as in
/// the following example:
///
///     let currentTemp = 73
///
///     if currentTemp >= 90 {
///         print("It's a scorcher!")
///     } else if currentTemp < 65 {
///         print("Might need a sweater today.")
///     } else {
///         print("Seems like picnic weather!")
///     }
///     // Prints "Seems like picnic weather!"
///
/// You can use special versions of some sequence and collection operations
/// when working with a `Comparable` type. For example, if your array's
/// elements conform to `Comparable`, you can call the `sort()` method without
/// using arguments to sort the elements of your array in ascending order.
///
///     var measurements = [1.1, 1.5, 2.9, 1.2, 1.5, 1.3, 1.2]
///     measurements.sort()
///     print(measurements)
///     // Prints "[1.1, 1.2, 1.2, 1.3, 1.5, 1.5, 2.9]"
///
/// Conforming to the Comparable Protocol
/// =====================================
///
/// Types with Comparable conformance implement the less-than operator (`<`)
/// and the equal-to operator (`==`). These two operations impose a strict
/// total order on the values of a type, in which exactly one of the following
/// must be true for any two values `a` and `b`:
///
/// - `a == b`
/// - `a < b`
/// - `b < a`
///
/// In addition, the following conditions must hold:
///
/// - `a < a` is always `false` (Irreflexivity)
/// - `a < b` implies `!(b < a)` (Asymmetry)
/// - `a < b` and `b < c` implies `a < c` (Transitivity)
///
/// To add `Comparable` conformance to your custom types, define the `<` and
/// `==` operators as static methods of your types. The `==` operator is a
/// requirement of the `Equatable` protocol, which `Comparable` extends---see
/// that protocol's documentation for more information about equality in
/// Swift. Because default implementations of the remainder of the relational
/// operators are provided by the standard library, you'll be able to use
/// `!=`, `>`, `<=`, and `>=` with instances of your type without any further
/// code.
///
/// As an example, here's an implementation of a `Date` structure that stores
/// the year, month, and day of a date:
///
///     struct Date {
///         let year: Int
///         let month: Int
///         let day: Int
///     }
///
/// To add `Comparable` conformance to `Date`, first declare conformance to
/// `Comparable` and implement the `<` operator function.
///
///     extension Date: Comparable {
///         static func < (lhs: Date, rhs: Date) -> Bool {
///             if lhs.year != rhs.year {
///                 return lhs.year < rhs.year
///             } else if lhs.month != rhs.month {
///                 return lhs.month < rhs.month
///             } else {
///                 return lhs.day < rhs.day
///             }
///         }
///
/// This function uses the least specific nonmatching property of the date to
/// determine the result of the comparison. For example, if the two `year`
/// properties are equal but the two `month` properties are not, the date with
/// the lesser value for `month` is the lesser of the two dates.
///
/// Next, implement the `==` operator function, the requirement inherited from
/// the `Equatable` protocol.
///
///         static func == (lhs: Date, rhs: Date) -> Bool {
///             return lhs.year == rhs.year && lhs.month == rhs.month
///                 && lhs.day == rhs.day
///         }
///     }
///
/// Two `Date` instances are equal if each of their corresponding properties is
/// equal.
///
/// Now that `Date` conforms to `Comparable`, you can compare instances of the
/// type with any of the relational operators. The following example compares
/// the date of the first moon landing with the release of David Bowie's song
/// "Space Oddity":
///
///     let spaceOddity = Date(year: 1969, month: 7, day: 11)   // July 11, 1969
///     let moonLanding = Date(year: 1969, month: 7, day: 20)   // July 20, 1969
///     if moonLanding > spaceOddity {
///         print("Major Tom stepped through the door first.")
///     } else {
///         print("David Bowie was following in Neil Armstrong's footsteps.")
///     }
///     // Prints "Major Tom stepped through the door first."
///
/// Note that the `>` operator provided by the standard library is used in this
/// example, not the `<` operator implemented above.
///
/// - Note: A conforming type may contain a subset of values which are treated
///   as exceptional---that is, values that are outside the domain of
///   meaningful arguments for the purposes of the `Comparable` protocol. For
///   example, the special "not a number" value for floating-point types
///   (`FloatingPoint.nan`) compares as neither less than, greater than, nor
///   equal to any normal floating-point value. Exceptional values need not
///   take part in the strict total order.
public protocol Comparable: Equatable {
  /// Returns a Boolean value indicating whether the value of the first
  /// argument is less than that of the second argument.
  ///
  /// This function is the only requirement of the `Comparable` protocol. The
  /// remainder of the relational operator functions are implemented by the
  /// standard library for any type that conforms to `Comparable`.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  static func < (lhs: Self, rhs: Self) -> Bool

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is less than or equal to that of the second argument.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  static func <= (lhs: Self, rhs: Self) -> Bool

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is greater than or equal to that of the second argument.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  static func >= (lhs: Self, rhs: Self) -> Bool

  /// Returns a Boolean value indicating whether the value of the first
  /// argument is greater than that of the second argument.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  static func > (lhs: Self, rhs: Self) -> Bool

  /// Returns a Boolean value indicating whether two values are equal.
  ///
  /// Equality is the inverse of inequality. For any values `a` and `b`,
  /// `a == b` implies that `a != b` is `false`.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  static func == (lhs: Self, rhs: Self) -> Bool
}

extension Comparable {
  /// Returns a Boolean value indicating whether the value of the first argument
  /// is greater than that of the second argument.
  ///
  /// This is the default implementation of the greater-than operator (`>`) for
  /// any type that conforms to `Comparable`.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  @inlinable
  public static func > (lhs: Self, rhs: Self) -> Bool {
    return rhs < lhs
  }

  /// Returns a Boolean value indicating whether the value of the first argument
  /// is less than or equal to that of the second argument.
  ///
  /// This is the default implementation of the less-than-or-equal-to
  /// operator (`<=`) for any type that conforms to `Comparable`.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  @inlinable
  public static func <= (lhs: Self, rhs: Self) -> Bool {
    return !(rhs < lhs)
  }

  /// Returns a Boolean value indicating whether the value of the first argument
  /// is greater than or equal to that of the second argument.
  ///
  /// This is the default implementation of the greater-than-or-equal-to operator
  /// (`>=`) for any type that conforms to `Comparable`.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  /// - Returns: `true` if `lhs` is greater than or equal to `rhs`; otherwise,
  ///   `false`.
  @inlinable
  public static func >= (lhs: Self, rhs: Self) -> Bool {
    return !(lhs < rhs)
  }

  /// Returns a Boolean value indicating whether two values are equal.
  ///
  /// Equality is the inverse of inequality. For any values `a` and `b`,
  /// `a == b` implies that `a != b` is `false`.
  ///
  /// This is the default implementation of the equal-to operator (`!=`) for
  /// any type that conforms to `Comparable`.
  ///
  /// - Parameters:
  ///   - lhs: A value to compare.
  ///   - rhs: Another value to compare.
  public static func == (lhs: Self, rhs: Self) -> Bool {
    return !(lhs < rhs || lhs > rhs)
  }
}
