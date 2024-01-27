//===--- StringInterpolation.swift - String Interpolation -----------------===//
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

/// Represents a string literal with interpolations while it is being built up.
/// 
/// Do not create an instance of this type directly. It is used by the compiler
/// when you create a string using string interpolation. Instead, use string
/// interpolation to create a new string by including values, literals,
/// variables, or expressions enclosed in parentheses, prefixed by a
/// backslash (`\(`...`)`).
///
///     let price = 2
///     let number = 3
///     let message = """
///                   If one cookie costs \(price) dollars, \
///                   \(number) cookies cost \(price * number) dollars.
///                   """
///     print(message)
///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
/// 
/// When implementing an `ExpressibleByStringInterpolation` conformance,
/// set the `StringInterpolation` associated type to
/// `DefaultStringInterpolation` to get the same interpolation behavior as
/// Swift's built-in `String` type and construct a `String` with the results.
/// If you don't want the default behavior or don't want to construct a
/// `String`, use a custom type conforming to `StringInterpolationProtocol`
/// instead.
/// 
/// Extending default string interpolation behavior
/// ===============================================
/// 
/// Code outside the standard library can extend string interpolation on
/// `String` and many other common types by extending
/// `DefaultStringInterpolation` and adding an `appendInterpolation(...)`
/// method. For example:
/// 
///     extension DefaultStringInterpolation {
///         fileprivate mutating func appendInterpolation(
///                  escaped value: String, asASCII forceASCII: Bool = false) {
///             for char in value.unicodeScalars {
///                 appendInterpolation(char.escaped(asASCII: forceASCII))
///             }
///         }
///     }
///     
///     print("Escaped string: \(escaped: string)")
/// 
/// See `StringInterpolationProtocol` for details on `appendInterpolation`
/// methods.
/// 
/// `DefaultStringInterpolation` extensions should add only `mutating` members
/// and should not copy `self` or capture it in an escaping closure.
@frozen
public struct DefaultStringInterpolation: StringInterpolationProtocol, Sendable {
  /// The string contents accumulated by this instance.
  @usableFromInline
  internal var _storage: String
  
  /// Creates a string interpolation with storage pre-sized for a literal
  /// with the indicated attributes.
  /// 
  /// Do not call this initializer directly. It is used by the compiler when
  /// interpreting string interpolations.
  @inlinable
  public init(literalCapacity: Int, interpolationCount: Int) {
    let capacityPerInterpolation = 2
    let initialCapacity = literalCapacity +
      interpolationCount * capacityPerInterpolation
    _storage = String._createEmpty(withInitialCapacity: initialCapacity)
  }
  
  /// Appends a literal segment of a string interpolation.
  /// 
  /// Do not call this method directly. It is used by the compiler when
  /// interpreting string interpolations.
  @inlinable
  public mutating func appendLiteral(_ literal: String) {
    literal.write(to: &self)
  }
  
  /// Interpolates the given value's textual representation into the
  /// string literal being created.
  /// 
  /// Do not call this method directly. It is used by the compiler when
  /// interpreting string interpolations. Instead, use string
  /// interpolation to create a new string by including values, literals,
  /// variables, or expressions enclosed in parentheses, prefixed by a
  /// backslash (`\(`...`)`).
  ///
  ///     let price = 2
  ///     let number = 3
  ///     let message = """
  ///                   If one cookie costs \(price) dollars, \
  ///                   \(number) cookies cost \(price * number) dollars.
  ///                   """
  ///     print(message)
  ///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
  @inlinable
  public mutating func appendInterpolation<T>(_ value: T)
    where T: TextOutputStreamable, T: CustomStringConvertible
  {
    value.write(to: &self)
  }
  
  /// Interpolates the given value's textual representation into the
  /// string literal being created.
  /// 
  /// Do not call this method directly. It is used by the compiler when
  /// interpreting string interpolations. Instead, use string
  /// interpolation to create a new string by including values, literals,
  /// variables, or expressions enclosed in parentheses, prefixed by a
  /// backslash (`\(`...`)`).
  ///
  ///     let price = 2
  ///     let number = 3
  ///     let message = "If one cookie costs \(price) dollars, " +
  ///                   "\(number) cookies cost \(price * number) dollars."
  ///     print(message)
  ///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
  @inlinable
  public mutating func appendInterpolation<T>(_ value: T)
    where T: TextOutputStreamable
  {
    value.write(to: &self)
  }
  
  /// Interpolates the given value's textual representation into the
  /// string literal being created.
  /// 
  /// Do not call this method directly. It is used by the compiler when
  /// interpreting string interpolations. Instead, use string
  /// interpolation to create a new string by including values, literals,
  /// variables, or expressions enclosed in parentheses, prefixed by a
  /// backslash (`\(`...`)`).
  ///
  ///     let price = 2
  ///     let number = 3
  ///     let message = """
  ///                   If one cookie costs \(price) dollars, \
  ///                   \(number) cookies cost \(price * number) dollars.
  ///                   """
  ///     print(message)
  ///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
  @inlinable
  public mutating func appendInterpolation<T>(_ value: T)
    where T: CustomStringConvertible
  {
    value.description.write(to: &self)
  }
  
  /// Interpolates the given value's textual representation into the
  /// string literal being created.
  /// 
  /// Do not call this method directly. It is used by the compiler when
  /// interpreting string interpolations. Instead, use string
  /// interpolation to create a new string by including values, literals,
  /// variables, or expressions enclosed in parentheses, prefixed by a
  /// backslash (`\(`...`)`).
  ///
  ///     let price = 2
  ///     let number = 3
  ///     let message = """
  ///                   If one cookie costs \(price) dollars, \
  ///                   \(number) cookies cost \(price * number) dollars.
  ///                   """
  ///     print(message)
  ///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
  @inlinable
  public mutating func appendInterpolation<T>(_ value: T) {
    //_print_unlocked(value, &self)
  }

  @_alwaysEmitIntoClient
  @_unavailableInEmbedded
  public mutating func appendInterpolation(_ value: Any.Type) {
	  _typeName(value, qualified: false).write(to: &self)
  }

  /// Creates a string from this instance, consuming the instance in the
  /// process.
  @inlinable
  internal __consuming func make() -> String {
    return _storage
  }
}

extension DefaultStringInterpolation: CustomStringConvertible {
  @inlinable
  public var description: String {
    return _storage
  }
}

extension DefaultStringInterpolation: TextOutputStream {
  @inlinable
  public mutating func write(_ string: String) {
    _storage.append(string)
  }
  
  public mutating func _writeASCII(_ buffer: UnsafeBufferPointer<UInt8>) {
    _storage._guts.append(_StringGuts(buffer, isASCII: true))
  }
}

// While not strictly necessary, declaring these is faster than using the
// default implementation.
extension String {
  /// Creates a new instance from an interpolated string literal.
  /// 
  /// Do not call this initializer directly. It is used by the compiler when
  /// you create a string using string interpolation. Instead, use string
  /// interpolation to create a new string by including values, literals,
  /// variables, or expressions enclosed in parentheses, prefixed by a
  /// backslash (`\(`...`)`).
  ///
  ///     let price = 2
  ///     let number = 3
  ///     let message = """
  ///                   If one cookie costs \(price) dollars, \
  ///                   \(number) cookies cost \(price * number) dollars.
  ///                   """
  ///     print(message)
  ///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
  @inlinable
  @_effects(readonly)
  public init(stringInterpolation: DefaultStringInterpolation) {
    self = stringInterpolation.make()
  }
}

extension Substring {
  /// Creates a new instance from an interpolated string literal.
  /// 
  /// Do not call this initializer directly. It is used by the compiler when
  /// you create a string using string interpolation. Instead, use string
  /// interpolation to create a new string by including values, literals,
  /// variables, or expressions enclosed in parentheses, prefixed by a
  /// backslash (`\(`...`)`).
  ///
  ///     let price = 2
  ///     let number = 3
  ///     let message = """
  ///                   If one cookie costs \(price) dollars, \
  ///                   \(number) cookies cost \(price * number) dollars.
  ///                   """
  ///     print(message)
  ///     // Prints "If one cookie costs 2 dollars, 3 cookies cost 6 dollars."
  @inlinable
  @_effects(readonly)
  public init(stringInterpolation: DefaultStringInterpolation) {
    self.init(stringInterpolation.make())
  }
}
