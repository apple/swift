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

/// A value that has a custom representation in `AnyHashable`.
///
/// `Self` should also conform to `Hashable`.
public protocol _HasCustomAnyHashableRepresentation {
  /// Returns a custom representation of `self` as `AnyHashable`.
  /// If returns nil, the default representation is used.
  ///
  /// If your custom representation is a class instance, it
  /// needs to be boxed into `AnyHashable` using the static
  /// type that introduces the `Hashable` conformance.
  ///
  ///     class Base: Hashable {}
  ///     class Derived1: Base {}
  ///     class Derived2: Base, _HasCustomAnyHashableRepresentation {
  ///       func _toCustomAnyHashable() -> AnyHashable? {
  ///         // `Derived2` is canonicalized to `Derived1`.
  ///         let customRepresentation = Derived1()
  ///
  ///         // Wrong:
  ///         // return AnyHashable(customRepresentation)
  ///
  ///         // Correct:
  ///         return AnyHashable(customRepresentation as Base)
  ///       }
  __consuming func _toCustomAnyHashable() -> AnyHashable?
}

@usableFromInline
internal protocol _AnyHashableBox {
  var _canonicalBox: _AnyHashableBox { get }

  /// Determine whether values in the boxes are equivalent.
  ///
  /// - Precondition: `self` and `box` are in canonical form.
  /// - Returns: `nil` to indicate that the boxes store different types, so
  ///   no comparison is possible. Otherwise, contains the result of `==`.
  func _isEqual(to box: _AnyHashableBox) -> Bool?
  var _hashValue: Int { get }
  func _hash(into hasher: inout Hasher)
  func _rawHashValue(_seed: Int) -> Int

  var _base: Any { get }
  func _unbox<T: Hashable>() -> T?
  func _downCastConditional<T>(into result: UnsafeMutablePointer<T>) -> Bool
}

extension _AnyHashableBox {
  var _canonicalBox: _AnyHashableBox {
    return self
  }
}

internal struct _ConcreteHashableBox<Base: Hashable>: _AnyHashableBox {
  internal var _baseHashable: Base

  internal init(_ base: Base) {
    self._baseHashable = base
  }

  internal func _unbox<T: Hashable>() -> T? {
    return (self as _AnyHashableBox as? _ConcreteHashableBox<T>)?._baseHashable
  }

  internal func _isEqual(to rhs: _AnyHashableBox) -> Bool? {
    if let rhs: Base = rhs._unbox() {
      return _baseHashable == rhs
    }
    return nil
  }

  internal var _hashValue: Int {
    return _baseHashable.hashValue
  }

  func _hash(into hasher: inout Hasher) {
    _baseHashable.hash(into: &hasher)
  }

  func _rawHashValue(_seed: Int) -> Int {
    return _baseHashable._rawHashValue(seed: _seed)
  }

  internal var _base: Any {
    return _baseHashable
  }

  internal
  func _downCastConditional<T>(into result: UnsafeMutablePointer<T>) -> Bool {
    guard let value = _baseHashable as? T else { return false }
    result.initialize(to: value)
    return true
  }
}

/// A type-erased hashable value.
///
/// The `AnyHashable` type forwards equality comparisons and hashing operations
/// to an underlying hashable value, hiding the type of the wrapped value.
///
/// Where conversion using `as` or `as?` is possible between two types (such as
/// `Int` and `NSNumber`), `AnyHashable` uses a canonical representation of the
/// type-erased value so that instances wrapping the same value of either type
/// compare as equal. For example, `AnyHashable(42)` compares as equal to
/// `AnyHashable(42 as NSNumber)`.
///
/// You can store mixed-type keys in dictionaries and other collections that
/// require `Hashable` conformance by wrapping mixed-type keys in
/// `AnyHashable` instances:
///
///     let descriptions: [AnyHashable: Any] = [
///         42: "an Int",
///         43 as Int8: "an Int8",
///         ["a", "b"] as Set: "a set of strings"
///     ]
///     print(descriptions[42]!)                // prints "an Int"
///     print(descriptions[42 as Int8]!)        // prints "an Int"
///     print(descriptions[43 as Int8]!)        // prints "an Int8"
///     print(descriptions[44])                 // prints "nil"
///     print(descriptions[["a", "b"] as Set]!) // prints "a set of strings"
///
/// Note that `AnyHashable` does not guarantee that it preserves the hash
/// encoding of wrapped values. Do not rely on `AnyHashable` generating such
/// compatible hashes, as the hash encoding that it uses may change between any
/// two releases of the standard library.
@frozen
public struct AnyHashable {
  internal var _box: _AnyHashableBox

  internal init(_box box: _AnyHashableBox) {
    self._box = box
  }

  /// Creates a type-erased hashable value that wraps the given instance.
  ///
  /// - Parameter base: A hashable value to wrap.
  public init<H: Hashable>(_ base: H) {
    if let custom =
      (base as? _HasCustomAnyHashableRepresentation)?._toCustomAnyHashable() {
      self = custom
      return
    }

    self.init(_box: _ConcreteHashableBox(false)) // Dummy value
    _makeAnyHashableUpcastingToHashableBaseType(
      base,
      storingResultInto: &self)
  }

  internal init<H: Hashable>(_usingDefaultRepresentationOf base: H) {
    self._box = _ConcreteHashableBox(base)
  }

  /// The value wrapped by this instance.
  ///
  /// The `base` property can be cast back to its original type using one of
  /// the type casting operators (`as?`, `as!`, or `as`).
  ///
  ///     let anyMessage = AnyHashable("Hello world!")
  ///     if let unwrappedMessage = anyMessage.base as? String {
  ///         print(unwrappedMessage)
  ///     }
  ///     // Prints "Hello world!"
  public var base: Any {
    return _box._base
  }

  /// Perform a downcast directly on the internal boxed representation.
  ///
  /// This avoids the intermediate re-boxing we would get if we just did
  /// a downcast on `base`.
  internal
  func _downCastConditional<T>(into result: UnsafeMutablePointer<T>) -> Bool {
    // Attempt the downcast.
    if _box._downCastConditional(into: result) { return true }

    #if _runtime(_ObjC)
    // Bridge to Objective-C and then attempt the cast from there.
    // FIXME: This should also work without the Objective-C runtime.
    if let value = _bridgeAnythingToObjectiveC(_box._base) as? T {
      result.initialize(to: value)
      return true
    }
    #endif

    return false
  }
}

extension AnyHashable: Equatable {
  /// Returns a Boolean value indicating whether two type-erased hashable
  /// instances wrap the same value.
  ///
  /// `AnyHashable` considers bridged counterparts (such as a `String` and an
  /// `NSString`) of the same value to be equivalent when type-erased. If those
  /// compatible types use different definitions for equality, values that were
  /// originally distinct might compare as equal when they are converted to
  /// `AnyHashable`:
  ///
  ///     let string1 = "café"
  ///     let string2 = "cafe\u{301}" // U+301 COMBINING ACUTE ACCENT
  ///     let nsString1 = string1 as NSString
  ///     let nsString2 = string2 as NSString
  ///     let typeErased1 = nsString1 as AnyHashable
  ///     let typeErased2 = nsString2 as AnyHashable
  ///     print(string1 == string2)         // prints "true"
  ///     print(nsString1 == nsString2)     // prints "false"
  ///     print(typeErased1 == typeErased2) // prints "true"
  ///
  /// - Parameters:
  ///   - lhs: A type-erased hashable value.
  ///   - rhs: Another type-erased hashable value.
  public static func == (lhs: AnyHashable, rhs: AnyHashable) -> Bool {
    return lhs._box._canonicalBox._isEqual(to: rhs._box._canonicalBox) ?? false
  }
}

extension AnyHashable: Hashable {
  public var hashValue: Int {
    return _box._canonicalBox._hashValue
  }

  public func hash(into hasher: inout Hasher) {
    _box._canonicalBox._hash(into: &hasher)
  }

  public func _rawHashValue(seed: Int) -> Int {
    return _box._canonicalBox._rawHashValue(_seed: seed)
  }
}

#if SWIFT_ENABLE_REFLECTION
extension AnyHashable: CustomStringConvertible {
  public var description: String {
    return String(describing: base)
  }
}

extension AnyHashable: CustomDebugStringConvertible {
  public var debugDescription: String {
    return "AnyHashable(" + String(reflecting: base) + ")"
  }
}

extension AnyHashable: CustomReflectable {
  public var customMirror: Mirror {
    return Mirror(
      self,
      children: ["value": base])
  }
}
#endif

/// Returns a default (non-custom) representation of `self`
/// as `AnyHashable`.
///
/// Completely ignores the `_HasCustomAnyHashableRepresentation`
/// conformance, if it exists.
/// Called by AnyHashableSupport.cpp.
@_silgen_name("_swift_makeAnyHashableUsingDefaultRepresentation")
internal func _makeAnyHashableUsingDefaultRepresentation<H: Hashable>(
  of value: H,
  storingResultInto result: UnsafeMutablePointer<AnyHashable>
) {
  result.pointee = AnyHashable(_usingDefaultRepresentationOf: value)
}

/// Provided by AnyHashable.cpp.
@_silgen_name("_swift_makeAnyHashableUpcastingToHashableBaseType")
internal func _makeAnyHashableUpcastingToHashableBaseType<H: Hashable>(
  _ value: H,
  storingResultInto result: UnsafeMutablePointer<AnyHashable>
)

@inlinable
public // COMPILER_INTRINSIC
func _convertToAnyHashable<H: Hashable>(_ value: H) -> AnyHashable {
  return AnyHashable(value)
}

/// Called by the casting machinery.
@_silgen_name("_swift_convertToAnyHashableIndirect")
internal func _convertToAnyHashableIndirect<H: Hashable>(
  _ value: H,
  _ target: UnsafeMutablePointer<AnyHashable>
) {
  target.initialize(to: AnyHashable(value))
}

/// Called by the casting machinery.
@_silgen_name("_swift_anyHashableDownCastConditionalIndirect")
internal func _anyHashableDownCastConditionalIndirect<T>(
  _ value: UnsafePointer<AnyHashable>,
  _ target: UnsafeMutablePointer<T>
) -> Bool {
  return value.pointee._downCastConditional(into: target)
}
