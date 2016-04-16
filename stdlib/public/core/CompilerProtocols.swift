//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// Intrinsic protocols shared with the compiler
//===----------------------------------------------------------------------===//

/// A type that represents a Boolean value.
///
/// Types that conform to the `Boolean` protocol can be used as
/// the condition in control statements (`if`, `while`, C-style `for`)
/// and other logical value contexts (e.g., `case` statement guards).
///
/// Only three types provided by Swift, `Bool`, `DarwinBoolean`, and `ObjCBool`,
/// conform to `Boolean`. Expanding this set to include types that
/// represent more than simple boolean values is discouraged.
public protocol Boolean {
  /// The value of `self`, expressed as a `Bool`.
  var boolValue: Bool { get }
}

/// A type that can be converted to an associated "raw" type, then
/// converted back to produce an instance equivalent to the original.
public protocol RawRepresentable {
  /// The "raw" type that can be used to represent all values of `Self`.
  ///
  /// Every distinct value of `self` has a corresponding unique
  /// value of `RawValue`, but `RawValue` may have representations
  /// that do not correspond to a value of `Self`.
  associatedtype RawValue

  /// Convert from a value of `RawValue`, yielding `nil` iff
  /// `rawValue` does not correspond to a value of `Self`.
  init?(rawValue: RawValue)

  /// The corresponding value of the "raw" type.
  ///
  /// `Self(rawValue: self.rawValue)!` is equivalent to `self`.
  var rawValue: RawValue { get }
}

/// Returns `true` iff `lhs.rawValue == rhs.rawValue`.
@warn_unused_result
public func == <
  T : RawRepresentable where T.RawValue : Equatable
>(lhs: T, rhs: T) -> Bool {
  return lhs.rawValue == rhs.rawValue
}

/// Returns `true` iff `lhs.rawValue != rhs.rawValue`.
@warn_unused_result
public func != <
  T : RawRepresentable where T.RawValue : Equatable
>(lhs: T, rhs: T) -> Bool {
  return lhs.rawValue != rhs.rawValue
}

// This overload is needed for ambiguity resolution against the
// implementation of != for T : Equatable
/// Returns `true` iff `lhs.rawValue != rhs.rawValue`.
@warn_unused_result
public func != <
  T : Equatable where T : RawRepresentable, T.RawValue : Equatable
>(lhs: T, rhs: T) -> Bool {
  return lhs.rawValue != rhs.rawValue
}

/// Conforming types can be initialized with `nil`.
public protocol NilLiteralConvertible {
  /// Create an instance initialized with `nil`.
  init(nilLiteral: ())
}

public protocol _BuiltinIntegerLiteralConvertible {
  init(_builtinIntegerLiteral value: _MaxBuiltinIntegerType)
}

/// Conforming types can be initialized with integer literals.
public protocol IntegerLiteralConvertible {
  associatedtype IntegerLiteralType : _BuiltinIntegerLiteralConvertible
  /// Create an instance initialized to `value`.
  init(integerLiteral value: IntegerLiteralType)
}

public protocol _BuiltinFloatLiteralConvertible {
  init(_builtinFloatLiteral value: _MaxBuiltinFloatType)
}

/// Conforming types can be initialized with floating point literals.
public protocol FloatLiteralConvertible {
  associatedtype FloatLiteralType : _BuiltinFloatLiteralConvertible
  /// Create an instance initialized to `value`.
  init(floatLiteral value: FloatLiteralType)
}

public protocol _BuiltinBooleanLiteralConvertible {
  init(_builtinBooleanLiteral value: Builtin.Int1)
}

/// Conforming types can be initialized with the Boolean literals
/// `true` and `false`.
public protocol BooleanLiteralConvertible {
  associatedtype BooleanLiteralType : _BuiltinBooleanLiteralConvertible
  /// Create an instance initialized to `value`.
  init(booleanLiteral value: BooleanLiteralType)
}

public protocol _BuiltinUnicodeScalarLiteralConvertible {
  init(_builtinUnicodeScalarLiteral value: Builtin.Int32)
}

/// Conforming types can be initialized with string literals
/// containing a single [Unicode scalar value](http://www.unicode.org/glossary/#unicode_scalar_value).
public protocol UnicodeScalarLiteralConvertible {
  associatedtype UnicodeScalarLiteralType : _BuiltinUnicodeScalarLiteralConvertible
  /// Create an instance initialized to `value`.
  init(unicodeScalarLiteral value: UnicodeScalarLiteralType)
}

public protocol _BuiltinExtendedGraphemeClusterLiteralConvertible
  : _BuiltinUnicodeScalarLiteralConvertible {

  init(
    _builtinExtendedGraphemeClusterLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1)
}

/// Conforming types can be initialized with string literals
/// containing a single [Unicode extended grapheme cluster](http://www.unicode.org/glossary/#extended_grapheme_cluster).
public protocol ExtendedGraphemeClusterLiteralConvertible
  : UnicodeScalarLiteralConvertible {

  associatedtype ExtendedGraphemeClusterLiteralType
    : _BuiltinExtendedGraphemeClusterLiteralConvertible
  /// Create an instance initialized to `value`.
  init(extendedGraphemeClusterLiteral value: ExtendedGraphemeClusterLiteralType)
}

public protocol _BuiltinStringLiteralConvertible
  : _BuiltinExtendedGraphemeClusterLiteralConvertible {

  init(
    _builtinStringLiteral start: Builtin.RawPointer,
    utf8CodeUnitCount: Builtin.Word,
    isASCII: Builtin.Int1)
}

public protocol _BuiltinUTF16StringLiteralConvertible
  : _BuiltinStringLiteralConvertible {

  init(
    _builtinUTF16StringLiteral start: Builtin.RawPointer,
    utf16CodeUnitCount: Builtin.Word)
}

/// Conforming types can be initialized with arbitrary string literals.
public protocol StringLiteralConvertible
  : ExtendedGraphemeClusterLiteralConvertible {
  // FIXME: when we have default function implementations in protocols, provide
  // an implementation of init(extendedGraphemeClusterLiteral:).

  associatedtype StringLiteralType : _BuiltinStringLiteralConvertible
  /// Create an instance initialized to `value`.
  init(stringLiteral value: StringLiteralType)
}

/// Conforming types can be initialized with array literals.
public protocol ArrayLiteralConvertible {
  associatedtype Element
  /// Create an instance initialized with `elements`.
  init(arrayLiteral elements: Element...)
}

/// Conforming types can be initialized with dictionary literals.
public protocol DictionaryLiteralConvertible {
  associatedtype Key
  associatedtype Value
  /// Create an instance initialized with `elements`.
  init(dictionaryLiteral elements: (Key, Value)...)
}

/// Conforming types can be initialized with string interpolations
/// containing `\(`...`)` clauses.
public protocol StringInterpolationConvertible {
  /// Create an instance by concatenating the elements of `strings`.
  init(stringInterpolation strings: Self...)
  /// Create an instance containing `expr`'s `print` representation.
  init<T>(stringInterpolationSegment expr: T)
}

/// Conforming types can be initialized with color literals (e.g.
/// `#colorLiteral(red: 1, green: 0, blue: 0, alpha: 1)`).
public protocol _ColorLiteralConvertible {
  init(red: Float, green: Float, blue: Float, alpha: Float)
}

/// Conforming types can be initialized with image literals (e.g.
/// `#imageLiteral(resourceName: "hi.png")`).
public protocol _ImageLiteralConvertible {
  init(resourceName: String)
}

/// Conforming types can be initialized with strings (e.g.
/// `#fileLiteral(resourceName: "resource.txt")`).
public protocol _FileReferenceLiteralConvertible {
  init(resourceName: String)
}

/// A container is destructor safe if whether it may store to memory on
/// destruction only depends on its type parameters destructors.
/// For example, whether `Array<Element>` may store to memory on destruction
/// depends only on `Element`.
/// If `Element` is an `Int` we know the `Array<Int>` does not store to memory
/// during destruction. If `Element` is an arbitrary class
/// `Array<MemoryUnsafeDestructorClass>` then the compiler will deduce may
/// store to memory on destruction because `MemoryUnsafeDestructorClass`'s
/// destructor may store to memory on destruction.
/// If in this example during `Array`'s destructor we would call a method on any
/// type parameter - say `Element.extraCleanup()` - that could store to memory,
/// then Array would no longer be a _DestructorSafeContainer.
public protocol _DestructorSafeContainer {
}

@available(*, unavailable, renamed: "Boolean")
public typealias BooleanType = Boolean
