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

//===----------------------------------------------------------------------===//
// Codable
//===----------------------------------------------------------------------===//

/// A type that can encode itself to an external representation.
public protocol Encodable {
  /// Encodes this value into the given encoder.
  ///
  /// If the value fails to encode anything, `encoder` will encode an empty
  /// keyed container in its place.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  func encode(to encoder: any Encoder) throws
}

/// A type that can decode itself from an external representation.
public protocol Decodable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  init(from decoder: any Decoder) throws
}

/// A type that can convert itself into and out of an external representation.
///
/// `Codable` is a type alias for the `Encodable` and `Decodable` protocols.
/// When you use `Codable` as a type or a generic constraint, it matches
/// any type that conforms to both protocols.
public typealias Codable = Encodable & Decodable

//===----------------------------------------------------------------------===//
// CodingKey
//===----------------------------------------------------------------------===//

/// A type that can be used as a key for encoding and decoding.
public protocol CodingKey: Sendable,
                           CustomStringConvertible,
                           CustomDebugStringConvertible {
  /// The string to use in a named collection (e.g. a string-keyed dictionary).
  var stringValue: String { get }

  /// Creates a new instance from the given string.
  ///
  /// If the string passed as `stringValue` does not correspond to any instance
  /// of this type, the result is `nil`.
  ///
  /// - parameter stringValue: The string value of the desired key.
  init?(stringValue: String)

  /// The value to use in an integer-indexed collection (e.g. an int-keyed
  /// dictionary).
  var intValue: Int? { get }

  /// Creates a new instance from the specified integer.
  ///
  /// If the value passed as `intValue` does not correspond to any instance of
  /// this type, the result is `nil`.
  ///
  /// - parameter intValue: The integer value of the desired key.
  init?(intValue: Int)
}

extension CodingKey {
  /// A textual representation of this key.
  public var description: String {
    let intValue = self.intValue?.description ?? "nil"
    return "\(type(of: self))(stringValue: \"\(stringValue)\", intValue: \(intValue))"
  }

  /// A textual representation of this key, suitable for debugging.
  public var debugDescription: String {
    return description
  }
}

//===----------------------------------------------------------------------===//
// Encoder & Decoder
//===----------------------------------------------------------------------===//

/// A type that can encode values into a native format for external
/// representation.
public protocol Encoder {
  /// The path of coding keys taken to get to this point in encoding.
  var codingPath: [any CodingKey] { get }

  /// Any contextual information set by the user for encoding.
  var userInfo: [CodingUserInfoKey: Any] { get }

  /// Returns an encoding container appropriate for holding multiple values
  /// keyed by the given key type.
  ///
  /// You must use only one kind of top-level encoding container. This method
  /// must not be called after a call to `unkeyedContainer()` or after
  /// encoding a value through a call to `singleValueContainer()`
  ///
  /// - parameter type: The key type to use for the container.
  /// - returns: A new keyed encoding container.
  func container<Key>(keyedBy type: Key.Type) -> KeyedEncodingContainer<Key>

  /// Returns an encoding container appropriate for holding multiple unkeyed
  /// values.
  ///
  /// You must use only one kind of top-level encoding container. This method
  /// must not be called after a call to `container(keyedBy:)` or after
  /// encoding a value through a call to `singleValueContainer()`
  ///
  /// - returns: A new empty unkeyed container.
  func unkeyedContainer() -> any UnkeyedEncodingContainer

  /// Returns an encoding container appropriate for holding a single primitive
  /// value.
  ///
  /// You must use only one kind of top-level encoding container. This method
  /// must not be called after a call to `unkeyedContainer()` or
  /// `container(keyedBy:)`, or after encoding a value through a call to
  /// `singleValueContainer()`
  ///
  /// - returns: A new empty single value container.
  func singleValueContainer() -> any SingleValueEncodingContainer
}

/// A type that can decode values from a native format into in-memory
/// representations.
public protocol Decoder {
  /// The path of coding keys taken to get to this point in decoding.
  var codingPath: [any CodingKey] { get }

  /// Any contextual information set by the user for decoding.
  var userInfo: [CodingUserInfoKey: Any] { get }

  /// Returns the data stored in this decoder as represented in a container
  /// keyed by the given key type.
  ///
  /// - parameter type: The key type to use for the container.
  /// - returns: A keyed decoding container view into this decoder.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not a keyed container.
  func container<Key>(
    keyedBy type: Key.Type
  ) throws -> KeyedDecodingContainer<Key>

  /// Returns the data stored in this decoder as represented in a container
  /// appropriate for holding values with no keys.
  ///
  /// - returns: An unkeyed container view into this decoder.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not an unkeyed container.
  func unkeyedContainer() throws -> any UnkeyedDecodingContainer

  /// Returns the data stored in this decoder as represented in a container
  /// appropriate for holding a single primitive value.
  ///
  /// - returns: A single value container view into this decoder.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not a single value container.
  func singleValueContainer() throws -> any SingleValueDecodingContainer
}

//===----------------------------------------------------------------------===//
// Keyed Encoding Containers
//===----------------------------------------------------------------------===//

/// A type that provides a view into an encoder's storage and is used to hold
/// the encoded properties of an encodable type in a keyed manner.
///
/// Encoders should provide types conforming to
/// `KeyedEncodingContainerProtocol` for their format.
public protocol KeyedEncodingContainerProtocol {
  associatedtype Key: CodingKey

  /// The path of coding keys taken to get to this point in encoding.
  var codingPath: [any CodingKey] { get }

  /// Encodes a null value for the given key.
  ///
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if a null value is invalid in the
  ///   current context for this format.
  mutating func encodeNil(forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Bool, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: String, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Double, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Float, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int8, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int16, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int32, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int64, forKey key: Key) throws
  
  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  mutating func encode(_ value: Int128, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt8, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt16, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt32, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt64, forKey key: Key) throws
  
  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  mutating func encode(_ value: UInt128, forKey key: Key) throws

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode<T: Encodable>(_ value: T, forKey key: Key) throws

  /// Encodes a reference to the given object only if it is encoded
  /// unconditionally elsewhere in the payload (previously, or in the future).
  ///
  /// For encoders which don't support this feature, the default implementation
  /// encodes the given object unconditionally.
  ///
  /// - parameter object: The object to encode.
  /// - parameter key: The key to associate the object with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeConditional<T: AnyObject & Encodable>(
    _ object: T,
    forKey key: Key
  ) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Bool?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: String?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Double?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Float?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Int?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Int8?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Int16?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Int32?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: Int64?, forKey key: Key) throws
  
  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  mutating func encodeIfPresent(_ value: Int128?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: UInt?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: UInt8?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: UInt16?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: UInt32?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent(_ value: UInt64?, forKey key: Key) throws
  
  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  mutating func encodeIfPresent(_ value: UInt128?, forKey key: Key) throws

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeIfPresent<T: Encodable>(
    _ value: T?,
    forKey key: Key
  ) throws

  /// Stores a keyed encoding container for the given key and returns it.
  ///
  /// - parameter keyType: The key type to use for the container.
  /// - parameter key: The key to encode the container for.
  /// - returns: A new keyed encoding container.
  mutating func nestedContainer<NestedKey>(
    keyedBy keyType: NestedKey.Type,
    forKey key: Key
  ) -> KeyedEncodingContainer<NestedKey>

  /// Stores an unkeyed encoding container for the given key and returns it.
  ///
  /// - parameter key: The key to encode the container for.
  /// - returns: A new unkeyed encoding container.
  mutating func nestedUnkeyedContainer(
    forKey key: Key
  ) -> any UnkeyedEncodingContainer

  /// Stores a new nested container for the default `super` key and returns a
  /// new encoder instance for encoding `super` into that container.
  ///
  /// Equivalent to calling `superEncoder(forKey:)` with
  /// `Key(stringValue: "super", intValue: 0)`.
  ///
  /// - returns: A new encoder to pass to `super.encode(to:)`.
  mutating func superEncoder() -> any Encoder

  /// Stores a new nested container for the given key and returns a new encoder
  /// instance for encoding `super` into that container.
  ///
  /// - parameter key: The key to encode `super` for.
  /// - returns: A new encoder to pass to `super.encode(to:)`.
  mutating func superEncoder(forKey key: Key) -> any Encoder
}

// An implementation of _KeyedEncodingContainerBase and
// _KeyedEncodingContainerBox are given at the bottom of this file.

/// A concrete container that provides a view into an encoder's storage, making
/// the encoded properties of an encodable type accessible by keys.
public struct KeyedEncodingContainer<K: CodingKey> :
  KeyedEncodingContainerProtocol
{
  public typealias Key = K

  /// The container for the concrete encoder.
  internal var _box: _KeyedEncodingContainerBase

  /// Creates a new instance with the given container.
  ///
  /// - parameter container: The container to hold.
  public init<Container: KeyedEncodingContainerProtocol>(
    _ container: Container
  ) where Container.Key == Key {
    _box = _KeyedEncodingContainerBox(container)
  }

  /// The path of coding keys taken to get to this point in encoding.
  public var codingPath: [any CodingKey] {
    return _box.codingPath
  }

  /// Encodes a null value for the given key.
  ///
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if a null value is invalid in the
  ///   current context for this format.
  public mutating func encodeNil(forKey key: Key) throws {
    try _box.encodeNil(forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Bool, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: String, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Double, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Float, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Int, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Int8, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Int16, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Int32, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: Int64, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }
  
  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: Int128, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: UInt, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: UInt8, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: UInt16, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: UInt32, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode(_ value: UInt64, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }
  
  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: UInt128, forKey key: Key) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes the given value for the given key.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encode<T: Encodable>(
    _ value: T,
    forKey key: Key
  ) throws {
    try _box.encode(value, forKey: key)
  }

  /// Encodes a reference to the given object only if it is encoded
  /// unconditionally elsewhere in the payload (previously, or in the future).
  ///
  /// For encoders which don't support this feature, the default implementation
  /// encodes the given object unconditionally.
  ///
  /// - parameter object: The object to encode.
  /// - parameter key: The key to associate the object with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeConditional<T: AnyObject & Encodable>(
    _ object: T,
    forKey key: Key
  ) throws {
    try _box.encodeConditional(object, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Bool?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: String?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Double?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Float?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Int?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Int8?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Int16?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Int32?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: Int64?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }
  
  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  public mutating func encodeIfPresent(
    _ value: Int128?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: UInt?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: UInt8?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: UInt16?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: UInt32?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent(
    _ value: UInt64?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }
  
  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  public mutating func encodeIfPresent(
    _ value: UInt128?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Encodes the given value for the given key if it is not `nil`.
  ///
  /// - parameter value: The value to encode.
  /// - parameter key: The key to associate the value with.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  public mutating func encodeIfPresent<T: Encodable>(
    _ value: T?,
    forKey key: Key
  ) throws {
    try _box.encodeIfPresent(value, forKey: key)
  }

  /// Stores a keyed encoding container for the given key and returns it.
  ///
  /// - parameter keyType: The key type to use for the container.
  /// - parameter key: The key to encode the container for.
  /// - returns: A new keyed encoding container.
  public mutating func nestedContainer<NestedKey>(
    keyedBy keyType: NestedKey.Type,
    forKey key: Key
  ) -> KeyedEncodingContainer<NestedKey> {
    return _box.nestedContainer(keyedBy: NestedKey.self, forKey: key)
  }

  /// Stores an unkeyed encoding container for the given key and returns it.
  ///
  /// - parameter key: The key to encode the container for.
  /// - returns: A new unkeyed encoding container.
  public mutating func nestedUnkeyedContainer(
    forKey key: Key
  ) -> any UnkeyedEncodingContainer {
    return _box.nestedUnkeyedContainer(forKey: key)
  }

  /// Stores a new nested container for the default `super` key and returns a
  /// new encoder instance for encoding `super` into that container.
  ///
  /// Equivalent to calling `superEncoder(forKey:)` with
  /// `Key(stringValue: "super", intValue: 0)`.
  ///
  /// - returns: A new encoder to pass to `super.encode(to:)`.
  public mutating func superEncoder() -> any Encoder {
    return _box.superEncoder()
  }

  /// Stores a new nested container for the given key and returns a new encoder
  /// instance for encoding `super` into that container.
  ///
  /// - parameter key: The key to encode `super` for.
  /// - returns: A new encoder to pass to `super.encode(to:)`.
  public mutating func superEncoder(forKey key: Key) -> any Encoder {
    return _box.superEncoder(forKey: key)
  }
}

@available(*, unavailable)
extension KeyedEncodingContainer: Sendable {}

/// A type that provides a view into a decoder's storage and is used to hold
/// the encoded properties of a decodable type in a keyed manner.
///
/// Decoders should provide types conforming to `UnkeyedDecodingContainer` for
/// their format.
public protocol KeyedDecodingContainerProtocol {
  associatedtype Key: CodingKey

  /// The path of coding keys taken to get to this point in decoding.
  var codingPath: [any CodingKey] { get }

  /// All the keys the `Decoder` has for this container.
  ///
  /// Different keyed containers from the same `Decoder` may return different
  /// keys here; it is possible to encode with multiple key types which are
  /// not convertible to one another. This should report all keys present
  /// which are convertible to the requested type.
  var allKeys: [Key] { get }

  /// Returns a Boolean value indicating whether the decoder contains a value
  /// associated with the given key.
  ///
  /// The value associated with `key` may be a null value as appropriate for
  /// the data format.
  ///
  /// - parameter key: The key to search for.
  /// - returns: Whether the `Decoder` has an entry for the given key.
  func contains(_ key: Key) -> Bool

  /// Decodes a null value for the given key.
  ///
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: Whether the encountered value was null.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  func decodeNil(forKey key: Key) throws -> Bool

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Bool.Type, forKey key: Key) throws -> Bool

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: String.Type, forKey key: Key) throws -> String

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Double.Type, forKey key: Key) throws -> Double

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Float.Type, forKey key: Key) throws -> Float

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Int.Type, forKey key: Key) throws -> Int

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Int8.Type, forKey key: Key) throws -> Int8

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Int16.Type, forKey key: Key) throws -> Int16

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Int32.Type, forKey key: Key) throws -> Int32

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: Int64.Type, forKey key: Key) throws -> Int64
  
  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  @available(SwiftStdlib 6.0, *)
  func decode(_ type: Int128.Type, forKey key: Key) throws -> Int128

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: UInt.Type, forKey key: Key) throws -> UInt

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: UInt8.Type, forKey key: Key) throws -> UInt8

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: UInt16.Type, forKey key: Key) throws -> UInt16

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: UInt32.Type, forKey key: Key) throws -> UInt32

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode(_ type: UInt64.Type, forKey key: Key) throws -> UInt64
  
  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  @available(SwiftStdlib 6.0, *)
  func decode(_ type: UInt128.Type, forKey key: Key) throws -> UInt128

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func decode<T: Decodable>(_ type: T.Type, forKey key: Key) throws -> T

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Bool.Type, forKey key: Key) throws -> Bool?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: String.Type, forKey key: Key) throws -> String?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Double.Type, forKey key: Key) throws -> Double?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Float.Type, forKey key: Key) throws -> Float?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Int.Type, forKey key: Key) throws -> Int?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Int8.Type, forKey key: Key) throws -> Int8?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Int16.Type, forKey key: Key) throws -> Int16?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Int32.Type, forKey key: Key) throws -> Int32?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: Int64.Type, forKey key: Key) throws -> Int64?
  
  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  @available(SwiftStdlib 6.0, *)
  func decodeIfPresent(_ type: Int128.Type, forKey key: Key) throws -> Int128?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: UInt.Type, forKey key: Key) throws -> UInt?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: UInt8.Type, forKey key: Key) throws -> UInt8?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: UInt16.Type, forKey key: Key) throws -> UInt16?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: UInt32.Type, forKey key: Key) throws -> UInt32?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent(_ type: UInt64.Type, forKey key: Key) throws -> UInt64?
  
  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  @available(SwiftStdlib 6.0, *)
  func decodeIfPresent(_ type: UInt128.Type, forKey key: Key) throws -> UInt128?

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  func decodeIfPresent<T: Decodable>(
    _ type: T.Type,
    forKey key: Key
  ) throws -> T?

  /// Returns the data stored for the given key as represented in a container
  /// keyed by the given key type.
  ///
  /// - parameter type: The key type to use for the container.
  /// - parameter key: The key that the nested container is associated with.
  /// - returns: A keyed decoding container view into `self`.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not a keyed container.
  func nestedContainer<NestedKey>(
    keyedBy type: NestedKey.Type,
    forKey key: Key
  ) throws -> KeyedDecodingContainer<NestedKey>

  /// Returns the data stored for the given key as represented in an unkeyed
  /// container.
  ///
  /// - parameter key: The key that the nested container is associated with.
  /// - returns: An unkeyed decoding container view into `self`.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not an unkeyed container.
  func nestedUnkeyedContainer(
    forKey key: Key
  ) throws -> any UnkeyedDecodingContainer

  /// Returns a `Decoder` instance for decoding `super` from the container
  /// associated with the default `super` key.
  ///
  /// Equivalent to calling `superDecoder(forKey:)` with
  /// `Key(stringValue: "super", intValue: 0)`.
  ///
  /// - returns: A new `Decoder` to pass to `super.init(from:)`.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the default `super` key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the default `super` key.
  func superDecoder() throws -> any Decoder

  /// Returns a `Decoder` instance for decoding `super` from the container
  /// associated with the given key.
  ///
  /// - parameter key: The key to decode `super` for.
  /// - returns: A new `Decoder` to pass to `super.init(from:)`.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  func superDecoder(forKey key: Key) throws -> any Decoder
}

// An implementation of _KeyedDecodingContainerBase and
// _KeyedDecodingContainerBox are given at the bottom of this file.

/// A concrete container that provides a view into a decoder's storage, making
/// the encoded properties of a decodable type accessible by keys.
public struct KeyedDecodingContainer<K: CodingKey> :
  KeyedDecodingContainerProtocol
{
  public typealias Key = K

  /// The container for the concrete decoder.
  internal var _box: _KeyedDecodingContainerBase

  /// Creates a new instance with the given container.
  ///
  /// - parameter container: The container to hold.
  public init<Container: KeyedDecodingContainerProtocol>(
    _ container: Container
  ) where Container.Key == Key {
    _box = _KeyedDecodingContainerBox(container)
  }

  /// The path of coding keys taken to get to this point in decoding.
  public var codingPath: [any CodingKey] {
    return _box.codingPath
  }

  /// All the keys the decoder has for this container.
  ///
  /// Different keyed containers from the same decoder may return different
  /// keys here, because it is possible to encode with multiple key types
  /// which are not convertible to one another. This should report all keys
  /// present which are convertible to the requested type.
  public var allKeys: [Key] {
    return _box.allKeys as! [Key]
  }

  /// Returns a Boolean value indicating whether the decoder contains a value
  /// associated with the given key.
  ///
  /// The value associated with the given key may be a null value as
  /// appropriate for the data format.
  ///
  /// - parameter key: The key to search for.
  /// - returns: Whether the `Decoder` has an entry for the given key.
  public func contains(_ key: Key) -> Bool {
    return _box.contains(key)
  }

  /// Decodes a null value for the given key.
  ///
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: Whether the encountered value was null.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  public func decodeNil(forKey key: Key) throws -> Bool {
    return try _box.decodeNil(forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Bool.Type, forKey key: Key) throws -> Bool {
    return try _box.decode(Bool.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: String.Type, forKey key: Key) throws -> String {
    return try _box.decode(String.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Double.Type, forKey key: Key) throws -> Double {
    return try _box.decode(Double.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Float.Type, forKey key: Key) throws -> Float {
    return try _box.decode(Float.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Int.Type, forKey key: Key) throws -> Int {
    return try _box.decode(Int.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Int8.Type, forKey key: Key) throws -> Int8 {
    return try _box.decode(Int8.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Int16.Type, forKey key: Key) throws -> Int16 {
    return try _box.decode(Int16.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Int32.Type, forKey key: Key) throws -> Int32 {
    return try _box.decode(Int32.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: Int64.Type, forKey key: Key) throws -> Int64 {
    return try _box.decode(Int64.self, forKey: key)
  }
  
  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  @available(SwiftStdlib 6.0, *)
  public func decode(_ type: Int128.Type, forKey key: Key) throws -> Int128 {
    return try _box.decode(Int128.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: UInt.Type, forKey key: Key) throws -> UInt {
    return try _box.decode(UInt.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: UInt8.Type, forKey key: Key) throws -> UInt8 {
    return try _box.decode(UInt8.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: UInt16.Type, forKey key: Key) throws -> UInt16 {
    return try _box.decode(UInt16.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: UInt32.Type, forKey key: Key) throws -> UInt32 {
    return try _box.decode(UInt32.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode(_ type: UInt64.Type, forKey key: Key) throws -> UInt64 {
    return try _box.decode(UInt64.self, forKey: key)
  }
  
  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  @available(SwiftStdlib 6.0, *)
  public func decode(_ type: UInt128.Type, forKey key: Key) throws -> UInt128 {
    return try _box.decode(UInt128.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func decode<T: Decodable>(
    _ type: T.Type,
    forKey key: Key
  ) throws -> T {
    return try _box.decode(T.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Bool.Type,
    forKey key: Key
  ) throws -> Bool? {
    return try _box.decodeIfPresent(Bool.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: String.Type,
    forKey key: Key
  ) throws -> String? {
    return try _box.decodeIfPresent(String.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Double.Type,
    forKey key: Key
  ) throws -> Double? {
    return try _box.decodeIfPresent(Double.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Float.Type,
    forKey key: Key
  ) throws -> Float? {
    return try _box.decodeIfPresent(Float.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Int.Type,
    forKey key: Key
  ) throws -> Int? {
    return try _box.decodeIfPresent(Int.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Int8.Type,
    forKey key: Key
  ) throws -> Int8? {
    return try _box.decodeIfPresent(Int8.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Int16.Type,
    forKey key: Key
  ) throws -> Int16? {
    return try _box.decodeIfPresent(Int16.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Int32.Type,
    forKey key: Key
  ) throws -> Int32? {
    return try _box.decodeIfPresent(Int32.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: Int64.Type,
    forKey key: Key
  ) throws -> Int64? {
    return try _box.decodeIfPresent(Int64.self, forKey: key)
  }
  
  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  @available(SwiftStdlib 6.0, *)
  public func decodeIfPresent(
    _ type: Int128.Type,
    forKey key: Key
  ) throws -> Int128? {
    return try _box.decodeIfPresent(Int128.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: UInt.Type,
    forKey key: Key
  ) throws -> UInt? {
    return try _box.decodeIfPresent(UInt.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: UInt8.Type,
    forKey key: Key
  ) throws -> UInt8? {
    return try _box.decodeIfPresent(UInt8.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: UInt16.Type,
    forKey key: Key
  ) throws -> UInt16? {
    return try _box.decodeIfPresent(UInt16.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: UInt32.Type,
    forKey key: Key
  ) throws -> UInt32? {
    return try _box.decodeIfPresent(UInt32.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent(
    _ type: UInt64.Type,
    forKey key: Key
  ) throws -> UInt64? {
    return try _box.decodeIfPresent(UInt64.self, forKey: key)
  }
  
  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  @available(SwiftStdlib 6.0, *)
  public func decodeIfPresent(
    _ type: UInt128.Type,
    forKey key: Key
  ) throws -> UInt128? {
    return try _box.decodeIfPresent(UInt128.self, forKey: key)
  }

  /// Decodes a value of the given type for the given key, if present.
  ///
  /// This method returns `nil` if the container does not have a value
  /// associated with `key`, or if the value is null. The difference between
  /// these states can be distinguished with a `contains(_:)` call.
  ///
  /// - parameter type: The type of value to decode.
  /// - parameter key: The key that the decoded value is associated with.
  /// - returns: A decoded value of the requested type, or `nil` if the
  ///   `Decoder` does not have an entry associated with the given key, or if
  ///   the value is a null value.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  public func decodeIfPresent<T: Decodable>(
    _ type: T.Type,
    forKey key: Key
  ) throws -> T? {
    return try _box.decodeIfPresent(T.self, forKey: key)
  }

  /// Returns the data stored for the given key as represented in a container
  /// keyed by the given key type.
  ///
  /// - parameter type: The key type to use for the container.
  /// - parameter key: The key that the nested container is associated with.
  /// - returns: A keyed decoding container view into `self`.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not a keyed container.
  public func nestedContainer<NestedKey>(
    keyedBy type: NestedKey.Type,
    forKey key: Key
  ) throws -> KeyedDecodingContainer<NestedKey> {
    return try _box.nestedContainer(keyedBy: NestedKey.self, forKey: key)
  }

  /// Returns the data stored for the given key as represented in an unkeyed
  /// container.
  ///
  /// - parameter key: The key that the nested container is associated with.
  /// - returns: An unkeyed decoding container view into `self`.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not an unkeyed container.
  public func nestedUnkeyedContainer(
    forKey key: Key
  ) throws -> any UnkeyedDecodingContainer {
    return try _box.nestedUnkeyedContainer(forKey: key)
  }

  /// Returns a `Decoder` instance for decoding `super` from the container
  /// associated with the default `super` key.
  ///
  /// Equivalent to calling `superDecoder(forKey:)` with
  /// `Key(stringValue: "super", intValue: 0)`.
  ///
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the default `super` key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the default `super` key.
  public func superDecoder() throws -> any Decoder {
    return try _box.superDecoder()
  }

  /// Returns a `Decoder` instance for decoding `super` from the container
  /// associated with the given key.
  ///
  /// - parameter key: The key to decode `super` for.
  /// - returns: A new `Decoder` to pass to `super.init(from:)`.
  /// - throws: `DecodingError.keyNotFound` if `self` does not have an entry
  ///   for the given key.
  /// - throws: `DecodingError.valueNotFound` if `self` has a null entry for
  ///   the given key.
  public func superDecoder(forKey key: Key) throws -> any Decoder {
    return try _box.superDecoder(forKey: key)
  }
}

@available(*, unavailable)
extension KeyedDecodingContainer: Sendable {}

//===----------------------------------------------------------------------===//
// Unkeyed Encoding Containers
//===----------------------------------------------------------------------===//

/// A type that provides a view into an encoder's storage and is used to hold
/// the encoded properties of an encodable type sequentially, without keys.
///
/// Encoders should provide types conforming to `UnkeyedEncodingContainer` for
/// their format.
public protocol UnkeyedEncodingContainer {
  /// The path of coding keys taken to get to this point in encoding.
  var codingPath: [any CodingKey] { get }

  /// The number of elements encoded into the container.
  var count: Int { get }

  /// Encodes a null value.
  ///
  /// - throws: `EncodingError.invalidValue` if a null value is invalid in the
  ///   current context for this format.
  mutating func encodeNil() throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Bool) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: String) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Double) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Float) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int8) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int16) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int32) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: Int64) throws
  
  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  mutating func encode(_ value: Int128) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt8) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt16) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt32) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode(_ value: UInt64) throws
  
  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  @available(SwiftStdlib 6.0, *)
  mutating func encode(_ value: UInt128) throws

  /// Encodes the given value.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encode<T: Encodable>(_ value: T) throws

  /// Encodes a reference to the given object only if it is encoded
  /// unconditionally elsewhere in the payload (previously, or in the future).
  ///
  /// For encoders which don't support this feature, the default implementation
  /// encodes the given object unconditionally.
  ///
  /// For formats which don't support this feature, the default implementation
  /// encodes the given object unconditionally.
  ///
  /// - parameter object: The object to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  mutating func encodeConditional<T: AnyObject & Encodable>(_ object: T) throws

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Bool

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == String

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Double

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Float

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int8

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int16

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int32

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int64
  
  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  @available(SwiftStdlib 6.0, *)
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int128

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt8

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt16

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt32

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt64
  
  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  @available(SwiftStdlib 6.0, *)
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt128

  /// Encodes the elements of the given sequence.
  ///
  /// - parameter sequence: The sequences whose contents to encode.
  /// - throws: An error if any of the contained values throws an error.
  mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element: Encodable

  /// Encodes a nested container keyed by the given type and returns it.
  ///
  /// - parameter keyType: The key type to use for the container.
  /// - returns: A new keyed encoding container.
  mutating func nestedContainer<NestedKey>(
    keyedBy keyType: NestedKey.Type
  ) -> KeyedEncodingContainer<NestedKey>

  /// Encodes an unkeyed encoding container and returns it.
  ///
  /// - returns: A new unkeyed encoding container.
  mutating func nestedUnkeyedContainer() -> any UnkeyedEncodingContainer

  /// Encodes a nested container and returns an `Encoder` instance for encoding
  /// `super` into that container.
  ///
  /// - returns: A new encoder to pass to `super.encode(to:)`.
  mutating func superEncoder() -> any Encoder
}

/// A type that provides a view into a decoder's storage and is used to hold
/// the encoded properties of a decodable type sequentially, without keys.
///
/// Decoders should provide types conforming to `UnkeyedDecodingContainer` for
/// their format.
public protocol UnkeyedDecodingContainer {
  /// The path of coding keys taken to get to this point in decoding.
  var codingPath: [any CodingKey] { get }

  /// The number of elements contained within this container.
  ///
  /// If the number of elements is unknown, the value is `nil`.
  var count: Int? { get }

  /// A Boolean value indicating whether there are no more elements left to be
  /// decoded in the container.
  var isAtEnd: Bool { get }

  /// The current decoding index of the container (i.e. the index of the next
  /// element to be decoded.) Incremented after every successful decode call.
  var currentIndex: Int { get }

  /// Decodes a null value.
  ///
  /// If the value is not null, does not increment currentIndex.
  ///
  /// - returns: Whether the encountered value was null.
  /// - throws: `DecodingError.valueNotFound` if there are no more values to
  ///   decode.
  mutating func decodeNil() throws -> Bool

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Bool.Type) throws -> Bool

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: String.Type) throws -> String

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Double.Type) throws -> Double

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Float.Type) throws -> Float

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Int.Type) throws -> Int

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Int8.Type) throws -> Int8

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Int16.Type) throws -> Int16

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Int32.Type) throws -> Int32

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: Int64.Type) throws -> Int64
  
  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  @available(SwiftStdlib 6.0, *)
  mutating func decode(_ type: Int128.Type) throws -> Int128

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: UInt.Type) throws -> UInt

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: UInt8.Type) throws -> UInt8

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: UInt16.Type) throws -> UInt16

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: UInt32.Type) throws -> UInt32

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode(_ type: UInt64.Type) throws -> UInt64
  
  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  @available(SwiftStdlib 6.0, *)
  mutating func decode(_ type: UInt128.Type) throws -> UInt128

  /// Decodes a value of the given type.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A value of the requested type, if present for the given key
  ///   and convertible to the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func decode<T: Decodable>(_ type: T.Type) throws -> T

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Bool.Type) throws -> Bool?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: String.Type) throws -> String?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Double.Type) throws -> Double?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Float.Type) throws -> Float?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Int.Type) throws -> Int?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Int8.Type) throws -> Int8?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Int16.Type) throws -> Int16?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Int32.Type) throws -> Int32?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: Int64.Type) throws -> Int64?
  
  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  @available(SwiftStdlib 6.0, *)
  mutating func decodeIfPresent(_ type: Int128.Type) throws -> Int128?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: UInt.Type) throws -> UInt?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: UInt8.Type) throws -> UInt8?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: UInt16.Type) throws -> UInt16?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: UInt32.Type) throws -> UInt32?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent(_ type: UInt64.Type) throws -> UInt64?
  
  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  @available(SwiftStdlib 6.0, *)
  mutating func decodeIfPresent(_ type: UInt128.Type) throws -> UInt128?

  /// Decodes a value of the given type, if present.
  ///
  /// This method returns `nil` if the container has no elements left to
  /// decode, or if the value is null. The difference between these states can
  /// be distinguished by checking `isAtEnd`.
  ///
  /// - parameter type: The type of value to decode.
  /// - returns: A decoded value of the requested type, or `nil` if the value
  ///   is a null value, or if there are no more elements to decode.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   is not convertible to the requested type.
  mutating func decodeIfPresent<T: Decodable>(_ type: T.Type) throws -> T?

  /// Decodes a nested container keyed by the given type.
  ///
  /// - parameter type: The key type to use for the container.
  /// - returns: A keyed decoding container view into `self`.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not a keyed container.
  mutating func nestedContainer<NestedKey>(
    keyedBy type: NestedKey.Type
  ) throws -> KeyedDecodingContainer<NestedKey>

  /// Decodes an unkeyed nested container.
  ///
  /// - returns: An unkeyed decoding container view into `self`.
  /// - throws: `DecodingError.typeMismatch` if the encountered stored value is
  ///   not an unkeyed container.
  mutating func nestedUnkeyedContainer() throws -> any UnkeyedDecodingContainer

  /// Decodes a nested container and returns a `Decoder` instance for decoding
  /// `super` from that container.
  ///
  /// - returns: A new `Decoder` to pass to `super.init(from:)`.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null, or of there are no more values to decode.
  mutating func superDecoder() throws -> any Decoder
}

//===----------------------------------------------------------------------===//
// Single Value Encoding Containers
//===----------------------------------------------------------------------===//

/// A container that can support the storage and direct encoding of a single
/// non-keyed value.
public protocol SingleValueEncodingContainer {
  /// The path of coding keys taken to get to this point in encoding.
  var codingPath: [any CodingKey] { get }

  /// Encodes a null value.
  ///
  /// - throws: `EncodingError.invalidValue` if a null value is invalid in the
  ///   current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encodeNil() throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Bool) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: String) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Double) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Float) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Int) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Int8) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Int16) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Int32) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: Int64) throws
  
  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  @available(SwiftStdlib 6.0, *)
  mutating func encode(_ value: Int128) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: UInt) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: UInt8) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: UInt16) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: UInt32) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode(_ value: UInt64) throws
  
  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  @available(SwiftStdlib 6.0, *)
  mutating func encode(_ value: UInt128) throws

  /// Encodes a single value of the given type.
  ///
  /// - parameter value: The value to encode.
  /// - throws: `EncodingError.invalidValue` if the given value is invalid in
  ///   the current context for this format.
  /// - precondition: May not be called after a previous `self.encode(_:)`
  ///   call.
  mutating func encode<T: Encodable>(_ value: T) throws
}

/// A container that can support the storage and direct decoding of a single
/// nonkeyed value.
public protocol SingleValueDecodingContainer {
  /// The path of coding keys taken to get to this point in encoding.
  var codingPath: [any CodingKey] { get }

  /// Decodes a null value.
  ///
  /// - returns: Whether the encountered value was null.
  func decodeNil() -> Bool

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Bool.Type) throws -> Bool

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: String.Type) throws -> String

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Double.Type) throws -> Double

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Float.Type) throws -> Float

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Int.Type) throws -> Int

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Int8.Type) throws -> Int8

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Int16.Type) throws -> Int16

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Int32.Type) throws -> Int32

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: Int64.Type) throws -> Int64
  
  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  @available(SwiftStdlib 6.0, *)
  func decode(_ type: Int128.Type) throws -> Int128

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: UInt.Type) throws -> UInt

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: UInt8.Type) throws -> UInt8

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: UInt16.Type) throws -> UInt16

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: UInt32.Type) throws -> UInt32

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode(_ type: UInt64.Type) throws -> UInt64
  
  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  @available(SwiftStdlib 6.0, *)
  func decode(_ type: UInt128.Type) throws -> UInt128

  /// Decodes a single value of the given type.
  ///
  /// - parameter type: The type to decode as.
  /// - returns: A value of the requested type.
  /// - throws: `DecodingError.typeMismatch` if the encountered encoded value
  ///   cannot be converted to the requested type.
  /// - throws: `DecodingError.valueNotFound` if the encountered encoded value
  ///   is null.
  func decode<T: Decodable>(_ type: T.Type) throws -> T
}

//===----------------------------------------------------------------------===//
// User Info
//===----------------------------------------------------------------------===//

/// A user-defined key for providing context during encoding and decoding.
public struct CodingUserInfoKey: RawRepresentable, Equatable, Hashable, Sendable {
  public typealias RawValue = String

  /// The key's string value.
  public let rawValue: String

  /// Creates a new instance with the given raw value.
  ///
  /// - parameter rawValue: The value of the key.
  public init?(rawValue: String) {
    self.rawValue = rawValue
  }

  /// Returns a Boolean value indicating whether the given keys are equal.
  ///
  /// - parameter lhs: The key to compare against.
  /// - parameter rhs: The key to compare with.
  public static func ==(
    lhs: CodingUserInfoKey,
    rhs: CodingUserInfoKey
  ) -> Bool {
    return lhs.rawValue == rhs.rawValue
  }

  /// The key's hash value.
  public var hashValue: Int {
    return self.rawValue.hashValue
  }

  /// Hashes the essential components of this value by feeding them into the
  /// given hasher.
  ///
  /// - Parameter hasher: The hasher to use when combining the components
  ///   of this instance.
  public func hash(into hasher: inout Hasher) {
    hasher.combine(self.rawValue)
  }
}

//===----------------------------------------------------------------------===//
// Errors
//===----------------------------------------------------------------------===//

/// An error that occurs during the encoding of a value.
public enum EncodingError: Error {
  /// The context in which the error occurred.
  public struct Context: Sendable {
    /// The path of coding keys taken to get to the point of the failing encode
    /// call.
    public let codingPath: [any CodingKey]

    /// A description of what went wrong, for debugging purposes.
    public let debugDescription: String

    /// The underlying error which caused this error, if any.
    public let underlyingError: Error?

    /// Creates a new context with the given path of coding keys and a
    /// description of what went wrong.
    ///
    /// - parameter codingPath: The path of coding keys taken to get to the
    ///   point of the failing encode call.
    /// - parameter debugDescription: A description of what went wrong, for
    ///   debugging purposes.
    /// - parameter underlyingError: The underlying error which caused this
    ///   error, if any.
    public init(
      codingPath: [any CodingKey],
      debugDescription: String,
      underlyingError: Error? = nil
    ) {
      self.codingPath = codingPath
      self.debugDescription = debugDescription
      self.underlyingError = underlyingError
    }
  }

  /// An indication that an encoder or its containers could not encode the
  /// given value.
  ///
  /// As associated values, this case contains the attempted value and context
  /// for debugging.
  case invalidValue(Any, Context)

  // MARK: - NSError Bridging

  // CustomNSError bridging applies only when the CustomNSError conformance is
  // applied in the same module as the declared error type. Since we cannot
  // access CustomNSError (which is defined in Foundation) from here, we can
  // use the "hidden" entry points.

  public var _domain: String {
    return "NSCocoaErrorDomain"
  }

  public var _code: Int {
    switch self {
    case .invalidValue: return 4866
    }
  }

  public var _userInfo: AnyObject? {
    // The error dictionary must be returned as an AnyObject. We can do this
    // only on platforms with bridging, unfortunately.
    #if _runtime(_ObjC)
      let context: Context
      switch self {
      case .invalidValue(_, let c): context = c
      }

      var userInfo: [String: Any] = [
        "NSCodingPath": context.codingPath,
        "NSDebugDescription": context.debugDescription
      ]

      if let underlyingError = context.underlyingError {
        userInfo["NSUnderlyingError"] = underlyingError
      }

      return userInfo as AnyObject
    #else
      return nil
    #endif
  }
}

/// An error that occurs during the decoding of a value.
public enum DecodingError: Error {
  /// The context in which the error occurred.
  public struct Context: Sendable {
    /// The path of coding keys taken to get to the point of the failing decode
    /// call.
    public let codingPath: [any CodingKey]

    /// A description of what went wrong, for debugging purposes.
    public let debugDescription: String

    /// The underlying error which caused this error, if any.
    public let underlyingError: Error?

    /// Creates a new context with the given path of coding keys and a
    /// description of what went wrong.
    ///
    /// - parameter codingPath: The path of coding keys taken to get to the
    ///   point of the failing decode call.
    /// - parameter debugDescription: A description of what went wrong, for
    ///   debugging purposes.
    /// - parameter underlyingError: The underlying error which caused this
    ///   error, if any.
    public init(
      codingPath: [any CodingKey],
      debugDescription: String,
      underlyingError: Error? = nil
    ) {
      self.codingPath = codingPath
      self.debugDescription = debugDescription
      self.underlyingError = underlyingError
    }
  }

  /// An indication that a value of the given type could not be decoded because
  /// it did not match the type of what was found in the encoded payload.
  ///
  /// As associated values, this case contains the attempted type and context
  /// for debugging.
  case typeMismatch(Any.Type, Context)

  /// An indication that a non-optional value of the given type was expected,
  /// but a null value was found.
  ///
  /// As associated values, this case contains the attempted type and context
  /// for debugging.
  case valueNotFound(Any.Type, Context)

  /// An indication that a keyed decoding container was asked for an entry for
  /// the given key, but did not contain one.
  ///
  /// As associated values, this case contains the attempted key and context
  /// for debugging.
  case keyNotFound(any CodingKey, Context)

  /// An indication that the data is corrupted or otherwise invalid.
  ///
  /// As an associated value, this case contains the context for debugging.
  case dataCorrupted(Context)

  // MARK: - NSError Bridging

  // CustomNSError bridging applies only when the CustomNSError conformance is
  // applied in the same module as the declared error type. Since we cannot
  // access CustomNSError (which is defined in Foundation) from here, we can
  // use the "hidden" entry points.

  public var _domain: String {
    return "NSCocoaErrorDomain"
  }

  public var _code: Int {
    switch self {
    case .keyNotFound, .valueNotFound: return 4865
    case .typeMismatch, .dataCorrupted:  return 4864
    }
  }

  public var _userInfo: AnyObject? {
    // The error dictionary must be returned as an AnyObject. We can do this
    // only on platforms with bridging, unfortunately.
    #if _runtime(_ObjC)
      let context: Context
      switch self {
      case .keyNotFound(_,   let c): context = c
      case .valueNotFound(_, let c): context = c
      case .typeMismatch(_,  let c): context = c
      case .dataCorrupted(   let c): context = c
      }

      var userInfo: [String: Any] = [
        "NSCodingPath": context.codingPath,
        "NSDebugDescription": context.debugDescription
      ]

      if let underlyingError = context.underlyingError {
        userInfo["NSUnderlyingError"] = underlyingError
      }

      return userInfo as AnyObject
    #else
      return nil
    #endif
  }
}

// The following extensions allow for easier error construction.

internal struct _GenericIndexKey: CodingKey, Sendable {
  internal var stringValue: String
  internal var intValue: Int?

  internal init?(stringValue: String) {
    return nil
  }

  internal init?(intValue: Int) {
    self.stringValue = "Index \(intValue)"
    self.intValue = intValue
  }
}

extension DecodingError {
  /// Returns a new `.dataCorrupted` error using a constructed coding path and
  /// the given debug description.
  ///
  /// The coding path for the returned error is constructed by appending the
  /// given key to the given container's coding path.
  ///
  /// - param key: The key which caused the failure.
  /// - param container: The container in which the corrupted data was
  ///   accessed.
  /// - param debugDescription: A description of the error to aid in debugging.
  ///
  /// - Returns: A new `.dataCorrupted` error with the given information.
  public static func dataCorruptedError<C: KeyedDecodingContainerProtocol>(
    forKey key: C.Key,
    in container: C,
    debugDescription: String
  ) -> DecodingError {
    let context = DecodingError.Context(
      codingPath: container.codingPath + [key],
      debugDescription: debugDescription)
    return .dataCorrupted(context)
  }

  /// Returns a new `.dataCorrupted` error using a constructed coding path and
  /// the given debug description.
  ///
  /// The coding path for the returned error is constructed by appending the
  /// given container's current index to its coding path.
  ///
  /// - param container: The container in which the corrupted data was
  ///   accessed.
  /// - param debugDescription: A description of the error to aid in debugging.
  ///
  /// - Returns: A new `.dataCorrupted` error with the given information.
  public static func dataCorruptedError(
    in container: any UnkeyedDecodingContainer,
    debugDescription: String
  ) -> DecodingError {
    let context = DecodingError.Context(
      codingPath: container.codingPath +
        [_GenericIndexKey(intValue: container.currentIndex)!],
      debugDescription: debugDescription)
    return .dataCorrupted(context)
  }

  /// Returns a new `.dataCorrupted` error using a constructed coding path and
  /// the given debug description.
  ///
  /// The coding path for the returned error is the given container's coding
  /// path.
  ///
  /// - param container: The container in which the corrupted data was
  ///   accessed.
  /// - param debugDescription: A description of the error to aid in debugging.
  ///
  /// - Returns: A new `.dataCorrupted` error with the given information.
  public static func dataCorruptedError(
    in container: any SingleValueDecodingContainer,
    debugDescription: String
  ) -> DecodingError {
    let context = DecodingError.Context(codingPath: container.codingPath,
      debugDescription: debugDescription)
    return .dataCorrupted(context)
  }
}

//===----------------------------------------------------------------------===//
// Keyed Encoding Container Implementations
//===----------------------------------------------------------------------===//

internal class _KeyedEncodingContainerBase {
  internal init(){}

  deinit {}

  // These must all be given a concrete implementation in _*Box.
  internal var codingPath: [CodingKey] {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeNil<K: CodingKey>(forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Bool, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: String, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Double, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Float, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Int, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Int8, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Int16, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Int32, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: Int64, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func encode<K: CodingKey>(_ value: Int128, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: UInt, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: UInt8, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: UInt16, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: UInt32, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<K: CodingKey>(_ value: UInt64, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func encode<K: CodingKey>(_ value: UInt128, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encode<T: Encodable, K: CodingKey>(_ value: T, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeConditional<T: AnyObject & Encodable, K: CodingKey>(
    _ object: T,
    forKey key: K
  ) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Bool?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: String?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Double?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Float?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Int?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Int8?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Int16?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Int32?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: Int64?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func encodeIfPresent<K: CodingKey>(_ value: Int128?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: UInt?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: UInt8?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: UInt16?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: UInt32?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<K: CodingKey>(_ value: UInt64?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func encodeIfPresent<K: CodingKey>(_ value: UInt128?, forKey key: K) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func encodeIfPresent<T: Encodable, K: CodingKey>(
    _ value: T?,
    forKey key: K
  ) throws {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func nestedContainer<NestedKey, K: CodingKey>(
    keyedBy keyType: NestedKey.Type,
    forKey key: K
  ) -> KeyedEncodingContainer<NestedKey> {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func nestedUnkeyedContainer<K: CodingKey>(
    forKey key: K
  ) -> UnkeyedEncodingContainer {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func superEncoder() -> Encoder {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }

  internal func superEncoder<K: CodingKey>(forKey key: K) -> Encoder {
    fatalError("_KeyedEncodingContainerBase cannot be used directly.")
  }
}

internal final class _KeyedEncodingContainerBox<
  Concrete: KeyedEncodingContainerProtocol
>: _KeyedEncodingContainerBase {
  typealias Key = Concrete.Key

  internal var concrete: Concrete

  internal init(_ container: Concrete) {
    concrete = container
  }

  override internal var codingPath: [CodingKey] {
    return concrete.codingPath
  }

  override internal func encodeNil<K: CodingKey>(forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeNil(forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Bool, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: String, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Double, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Float, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Int, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Int8, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Int16, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Int32, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: Int64, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func encode<K: CodingKey>(_ value: Int128, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: UInt, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: UInt8, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: UInt16, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: UInt32, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<K: CodingKey>(_ value: UInt64, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func encode<K: CodingKey>(_ value: UInt128, forKey key: K) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encode<T: Encodable, K: CodingKey>(
    _ value: T,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encode(value, forKey: key)
  }

  override internal func encodeConditional<T: AnyObject & Encodable, K: CodingKey>(
    _ object: T,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeConditional(object, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Bool?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: String?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Double?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Float?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Int?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Int8?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Int16?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Int32?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Int64?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func encodeIfPresent<K: CodingKey>(
    _ value: Int128?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: UInt?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: UInt8?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: UInt16?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: UInt32?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<K: CodingKey>(
    _ value: UInt64?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func encodeIfPresent<K: CodingKey>(
    _ value: UInt128?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func encodeIfPresent<T: Encodable, K: CodingKey>(
    _ value: T?,
    forKey key: K
  ) throws {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    try concrete.encodeIfPresent(value, forKey: key)
  }

  override internal func nestedContainer<NestedKey, K: CodingKey>(
    keyedBy keyType: NestedKey.Type,
    forKey key: K
  ) -> KeyedEncodingContainer<NestedKey> {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return concrete.nestedContainer(keyedBy: NestedKey.self, forKey: key)
  }

  override internal func nestedUnkeyedContainer<K: CodingKey>(
    forKey key: K
  ) -> UnkeyedEncodingContainer {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return concrete.nestedUnkeyedContainer(forKey: key)
  }

  override internal func superEncoder() -> Encoder {
    return concrete.superEncoder()
  }

  override internal func superEncoder<K: CodingKey>(forKey key: K) -> Encoder {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return concrete.superEncoder(forKey: key)
  }
}

internal class _KeyedDecodingContainerBase {
  internal init(){}

  deinit {}

  internal var codingPath: [CodingKey] {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal var allKeys: [CodingKey] {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func contains<K: CodingKey>(_ key: K) -> Bool {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeNil<K: CodingKey>(forKey key: K) throws -> Bool {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Bool.Type,
    forKey key: K
  ) throws -> Bool {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: String.Type,
    forKey key: K
  ) throws -> String {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Double.Type,
    forKey key: K
  ) throws -> Double {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Float.Type,
    forKey key: K
  ) throws -> Float {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Int.Type,
    forKey key: K
  ) throws -> Int {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Int8.Type,
    forKey key: K
  ) throws -> Int8 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Int16.Type,
    forKey key: K
  ) throws -> Int16 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Int32.Type,
    forKey key: K
  ) throws -> Int32 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: Int64.Type,
    forKey key: K
  ) throws -> Int64 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func decode<K: CodingKey>(
    _ type: Int128.Type,
    forKey key: K
  ) throws -> Int128 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: UInt.Type,
    forKey key: K
  ) throws -> UInt {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: UInt8.Type,
    forKey key: K
  ) throws -> UInt8 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: UInt16.Type,
    forKey key: K
  ) throws -> UInt16 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: UInt32.Type,
    forKey key: K
  ) throws -> UInt32 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<K: CodingKey>(
    _ type: UInt64.Type,
    forKey key: K
  ) throws -> UInt64 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func decode<K: CodingKey>(
    _ type: UInt128.Type,
    forKey key: K
  ) throws -> UInt128 {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decode<T: Decodable, K: CodingKey>(
    _ type: T.Type,
    forKey key: K
  ) throws -> T {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Bool.Type,
    forKey key: K
  ) throws -> Bool? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: String.Type,
    forKey key: K
  ) throws -> String? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Double.Type,
    forKey key: K
  ) throws -> Double? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Float.Type,
    forKey key: K
  ) throws -> Float? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Int.Type,
    forKey key: K
  ) throws -> Int? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Int8.Type,
    forKey key: K
  ) throws -> Int8? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Int16.Type,
    forKey key: K
  ) throws -> Int16? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Int32.Type,
    forKey key: K
  ) throws -> Int32? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: Int64.Type,
    forKey key: K
  ) throws -> Int64? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func decodeIfPresent<K: CodingKey>(
    _ type: Int128.Type,
    forKey key: K
  ) throws -> Int128? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt.Type,
    forKey key: K
  ) throws -> UInt? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt8.Type,
    forKey key: K
  ) throws -> UInt8? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt16.Type,
    forKey key: K
  ) throws -> UInt16? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt32.Type,
    forKey key: K
  ) throws -> UInt32? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt64.Type,
    forKey key: K
  ) throws -> UInt64? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }
  
  @available(SwiftStdlib 6.0, *)
  internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt128.Type,
    forKey key: K
  ) throws -> UInt128? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func decodeIfPresent<T: Decodable, K: CodingKey>(
    _ type: T.Type,
    forKey key: K
  ) throws -> T? {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func nestedContainer<NestedKey, K: CodingKey>(
    keyedBy type: NestedKey.Type,
    forKey key: K
  ) throws -> KeyedDecodingContainer<NestedKey> {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func nestedUnkeyedContainer<K: CodingKey>(
    forKey key: K
  ) throws -> UnkeyedDecodingContainer {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func superDecoder() throws -> Decoder {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }

  internal func superDecoder<K: CodingKey>(forKey key: K) throws -> Decoder {
    fatalError("_KeyedDecodingContainerBase cannot be used directly.")
  }
}

internal final class _KeyedDecodingContainerBox<
  Concrete: KeyedDecodingContainerProtocol
>: _KeyedDecodingContainerBase {
  typealias Key = Concrete.Key

  internal var concrete: Concrete

  internal init(_ container: Concrete) {
    concrete = container
  }

  override var codingPath: [CodingKey] {
    return concrete.codingPath
  }

  override var allKeys: [CodingKey] {
    return concrete.allKeys
  }

  override internal func contains<K: CodingKey>(_ key: K) -> Bool {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return concrete.contains(key)
  }

  override internal func decodeNil<K: CodingKey>(forKey key: K) throws -> Bool {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeNil(forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Bool.Type,
    forKey key: K
  ) throws -> Bool {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Bool.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: String.Type,
    forKey key: K
  ) throws -> String {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(String.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Double.Type,
    forKey key: K
  ) throws -> Double {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Double.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Float.Type,
    forKey key: K
  ) throws -> Float {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Float.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Int.Type,
    forKey key: K
  ) throws -> Int {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Int.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Int8.Type,
    forKey key: K
  ) throws -> Int8 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Int8.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Int16.Type,
    forKey key: K
  ) throws -> Int16 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Int16.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Int32.Type,
    forKey key: K
  ) throws -> Int32 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Int32.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: Int64.Type,
    forKey key: K
  ) throws -> Int64 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Int64.self, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func decode<K: CodingKey>(
    _ type: Int128.Type,
    forKey key: K
  ) throws -> Int128 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(Int128.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: UInt.Type,
    forKey key: K
  ) throws -> UInt {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(UInt.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: UInt8.Type,
    forKey key: K
  ) throws -> UInt8 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(UInt8.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: UInt16.Type,
    forKey key: K
  ) throws -> UInt16 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(UInt16.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: UInt32.Type,
    forKey key: K
  ) throws -> UInt32 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(UInt32.self, forKey: key)
  }

  override internal func decode<K: CodingKey>(
    _ type: UInt64.Type,
    forKey key: K
  ) throws -> UInt64 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(UInt64.self, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func decode<K: CodingKey>(
    _ type: UInt128.Type,
    forKey key: K
  ) throws -> UInt128 {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(UInt128.self, forKey: key)
  }

  override internal func decode<T: Decodable, K: CodingKey>(
    _ type: T.Type,
    forKey key: K
  ) throws -> T {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decode(T.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Bool.Type,
    forKey key: K
  ) throws -> Bool? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Bool.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: String.Type,
    forKey key: K
  ) throws -> String? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(String.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Double.Type,
    forKey key: K
  ) throws -> Double? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Double.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Float.Type,
    forKey key: K
  ) throws -> Float? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Float.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Int.Type,
    forKey key: K
  ) throws -> Int? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Int.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Int8.Type,
    forKey key: K
  ) throws -> Int8? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Int8.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Int16.Type,
    forKey key: K
  ) throws -> Int16? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Int16.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Int32.Type,
    forKey key: K
  ) throws -> Int32? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Int32.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Int64.Type,
    forKey key: K
  ) throws -> Int64? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Int64.self, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func decodeIfPresent<K: CodingKey>(
    _ type: Int128.Type,
    forKey key: K
  ) throws -> Int128? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(Int128.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt.Type,
    forKey key: K
  ) throws -> UInt? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(UInt.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt8.Type,
    forKey key: K
  ) throws -> UInt8? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(UInt8.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt16.Type,
    forKey key: K
  ) throws -> UInt16? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(UInt16.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt32.Type,
    forKey key: K
  ) throws -> UInt32? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(UInt32.self, forKey: key)
  }

  override internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt64.Type,
    forKey key: K
  ) throws -> UInt64? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(UInt64.self, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  override internal func decodeIfPresent<K: CodingKey>(
    _ type: UInt128.Type,
    forKey key: K
  ) throws -> UInt128? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(UInt128.self, forKey: key)
  }

  override internal func decodeIfPresent<T: Decodable, K: CodingKey>(
    _ type: T.Type,
    forKey key: K
  ) throws -> T? {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.decodeIfPresent(T.self, forKey: key)
  }

  override internal func nestedContainer<NestedKey, K: CodingKey>(
    keyedBy type: NestedKey.Type,
    forKey key: K
  ) throws -> KeyedDecodingContainer<NestedKey> {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.nestedContainer(keyedBy: NestedKey.self, forKey: key)
  }

  override internal func nestedUnkeyedContainer<K: CodingKey>(
    forKey key: K
  ) throws -> UnkeyedDecodingContainer {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.nestedUnkeyedContainer(forKey: key)
  }

  override internal func superDecoder() throws -> Decoder {
    return try concrete.superDecoder()
  }

  override internal func superDecoder<K: CodingKey>(forKey key: K) throws -> Decoder {
    _internalInvariant(K.self == Key.self)
    let key = unsafeBitCast(key, to: Key.self)
    return try concrete.superDecoder(forKey: key)
  }
}

//===----------------------------------------------------------------------===//
// Primitive and RawRepresentable Extensions
//===----------------------------------------------------------------------===//

extension Bool: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Bool.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Bool> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Bool`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Bool> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Bool`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension String: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(String.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<String> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `String`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<String> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `String`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension Double: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Double.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Double> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Double`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Double> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Double`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension Float: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Float.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Float> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Float`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Float> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Float`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
@available(SwiftStdlib 5.3, *)
extension Float16: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let floatValue = try Float(from: decoder)
    self = Float16(floatValue)
    if isInfinite && floatValue.isFinite {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Parsed JSON number \(floatValue) does not fit in Float16."
        )
      )
    }
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    try Float(self).encode(to: encoder)
  }
}
#endif

extension Int: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Int.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Int> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Int`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Int> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Int`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension Int8: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Int8.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Int8> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Int8`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Int8> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Int8`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension Int16: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Int16.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Int16> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Int16`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Int16> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Int16`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension Int32: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Int32.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Int32> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Int32`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Int32> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Int32`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension Int64: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Int64.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<Int64> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Int64`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<Int64> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Int64`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

@available(SwiftStdlib 6.0, *)
extension Int128: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(Int128.self)
  }
  
  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

@available(SwiftStdlib 6.0, *)
extension RawRepresentable<Int128> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `Int128`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  @available(SwiftStdlib 6.0, *)
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

@available(SwiftStdlib 6.0, *)
extension RawRepresentable<Int128> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `Int128`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  @available(SwiftStdlib 6.0, *)
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }
    
    self = value
  }
}

extension UInt: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(UInt.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<UInt> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `UInt`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<UInt> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `UInt`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension UInt8: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(UInt8.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<UInt8> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `UInt8`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<UInt8> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `UInt8`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension UInt16: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(UInt16.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<UInt16> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `UInt16`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<UInt16> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `UInt16`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension UInt32: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(UInt32.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<UInt32> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `UInt32`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<UInt32> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `UInt32`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

extension UInt64: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(UInt64.self)
  }

  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

extension RawRepresentable<UInt64> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `UInt64`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

extension RawRepresentable<UInt64> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `UInt64`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }

    self = value
  }
}

@available(SwiftStdlib 6.0, *)
extension UInt128: Codable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self = try decoder.singleValueContainer().decode(UInt128.self)
  }
  
  /// Encodes this value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self)
  }
}

@available(SwiftStdlib 6.0, *)
extension RawRepresentable<UInt128> where Self: Encodable {
  /// Encodes this value into the given encoder, when the type's `RawValue`
  /// is `UInt128`.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  @available(SwiftStdlib 6.0, *)
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.rawValue)
  }
}

@available(SwiftStdlib 6.0, *)
extension RawRepresentable<UInt128> where Self: Decodable {
  /// Creates a new instance by decoding from the given decoder, when the
  /// type's `RawValue` is `UInt128`.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  @available(SwiftStdlib 6.0, *)
  public init(from decoder: any Decoder) throws {
    let decoded = try decoder.singleValueContainer().decode(RawValue.self)
    guard let value = Self(rawValue: decoded) else {
      throw DecodingError.dataCorrupted(
        DecodingError.Context(
          codingPath: decoder.codingPath,
          debugDescription: "Cannot initialize \(Self.self) from invalid \(RawValue.self) value \(decoded)"
        )
      )
    }
    
    self = value
  }
}


//===----------------------------------------------------------------------===//
// Optional/Collection Type Conformances
//===----------------------------------------------------------------------===//

extension Optional: Encodable where Wrapped: Encodable {
  /// Encodes this optional value into the given encoder.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.singleValueContainer()
    switch self {
    case .none: try container.encodeNil()
    case .some(let wrapped): try container.encode(wrapped)
    }
  }
}

extension Optional: Decodable where Wrapped: Decodable {
  /// Creates a new instance by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    let container = try decoder.singleValueContainer()
    if container.decodeNil() {
      self = .none
    }  else {
      let element = try container.decode(Wrapped.self)
      self = .some(element)
    }
  }
}

extension Array: Encodable where Element: Encodable {
  /// Encodes the elements of this array into the given encoder in an unkeyed
  /// container.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.unkeyedContainer()
    for element in self {
      try container.encode(element)
    }
  }
}

extension Array: Decodable where Element: Decodable {
  /// Creates a new array by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self.init()

    var container = try decoder.unkeyedContainer()
    while !container.isAtEnd {
      let element = try container.decode(Element.self)
      self.append(element)
    }
  }
}

extension ContiguousArray: Encodable where Element: Encodable {
  /// Encodes the elements of this contiguous array into the given encoder
  /// in an unkeyed container.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.unkeyedContainer()
    for element in self {
      try container.encode(element)
    }
  }
}

extension ContiguousArray: Decodable where Element: Decodable {
  /// Creates a new contiguous array by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self.init()

    var container = try decoder.unkeyedContainer()
    while !container.isAtEnd {
      let element = try container.decode(Element.self)
      self.append(element)
    }
  }
}

extension Set: Encodable where Element: Encodable {
  /// Encodes the elements of this set into the given encoder in an unkeyed
  /// container.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    var container = encoder.unkeyedContainer()
    for element in self {
      try container.encode(element)
    }
  }
}

extension Set: Decodable where Element: Decodable {
  /// Creates a new set by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self.init()

    var container = try decoder.unkeyedContainer()
    while !container.isAtEnd {
      let element = try container.decode(Element.self)
      self.insert(element)
    }
  }
}

/// A wrapper for dictionary keys which are Strings or Ints.
internal struct _DictionaryCodingKey: CodingKey {
  internal let stringValue: String
  internal let intValue: Int?

  internal init(stringValue: String) {
    self.stringValue = stringValue
    self.intValue = Int(stringValue)
  }

  internal init(intValue: Int) {
    self.stringValue = "\(intValue)"
    self.intValue = intValue
  }

  fileprivate init(codingKey: any CodingKey) {
    self.stringValue = codingKey.stringValue
    self.intValue = codingKey.intValue
  }
}

/// A type that can be converted to and from a coding key.
///
/// With a `CodingKeyRepresentable` type, you can losslessly convert between a
/// custom type and a `CodingKey` type.
///
/// Conforming a type to `CodingKeyRepresentable` lets you opt in to encoding
/// and decoding `Dictionary` values keyed by the conforming type to and from
/// a keyed container, rather than encoding and decoding the dictionary as an
/// unkeyed container of alternating key-value pairs.
@available(SwiftStdlib 5.6, *)
public protocol CodingKeyRepresentable {
  @available(SwiftStdlib 5.6, *)
  var codingKey: CodingKey { get }
  @available(SwiftStdlib 5.6, *)
  init?<T: CodingKey>(codingKey: T)
}

@available(SwiftStdlib 5.6, *)
extension RawRepresentable
where Self: CodingKeyRepresentable, RawValue == String {
  @available(SwiftStdlib 5.6, *)
  public var codingKey: CodingKey {
    _DictionaryCodingKey(stringValue: rawValue)
  }
  @available(SwiftStdlib 5.6, *)
  public init?<T: CodingKey>(codingKey: T) {
    self.init(rawValue: codingKey.stringValue)
  }
}

@available(SwiftStdlib 5.6, *)
extension RawRepresentable where Self: CodingKeyRepresentable, RawValue == Int {
  @available(SwiftStdlib 5.6, *)
  public var codingKey: CodingKey {
    _DictionaryCodingKey(intValue: rawValue)
  }
  @available(SwiftStdlib 5.6, *)
  public init?<T: CodingKey>(codingKey: T) {
    if let intValue = codingKey.intValue {
      self.init(rawValue: intValue)
    } else {
      return nil
    }
  }
}

@available(SwiftStdlib 5.6, *)
extension Int: CodingKeyRepresentable {
  @available(SwiftStdlib 5.6, *)
  public var codingKey: CodingKey {
    _DictionaryCodingKey(intValue: self)
  }
  @available(SwiftStdlib 5.6, *)
  public init?<T: CodingKey>(codingKey: T) {
    if let intValue = codingKey.intValue {
      self = intValue
    } else {
      return nil
    }
  }
}

@available(SwiftStdlib 5.6, *)
extension String: CodingKeyRepresentable {
  @available(SwiftStdlib 5.6, *)
  public var codingKey: CodingKey {
    _DictionaryCodingKey(stringValue: self)
  }
  @available(SwiftStdlib 5.6, *)
  public init?<T: CodingKey>(codingKey: T) {
    self = codingKey.stringValue
  }
}

extension Dictionary: Encodable where Key: Encodable, Value: Encodable {
  /// Encodes the contents of this dictionary into the given encoder.
  ///
  /// If the dictionary uses keys that are `String`, `Int`, or a type conforming
  /// to `CodingKeyRepresentable`, the contents are encoded in a keyed
  /// container. Otherwise, the contents are encoded as alternating key-value
  /// pairs in an unkeyed container.
  ///
  /// This function throws an error if any values are invalid for the given
  /// encoder's format.
  ///
  /// - Parameter encoder: The encoder to write data to.
  public func encode(to encoder: any Encoder) throws {
    if Key.self == String.self {
      // Since the keys are already Strings, we can use them as keys directly.
      var container = encoder.container(keyedBy: _DictionaryCodingKey.self)
      for (key, value) in self {
        let codingKey = _DictionaryCodingKey(stringValue: key as! String)
        try container.encode(value, forKey: codingKey)
      }
    } else if Key.self == Int.self {
      // Since the keys are already Ints, we can use them as keys directly.
      var container = encoder.container(keyedBy: _DictionaryCodingKey.self)
      for (key, value) in self {
        let codingKey = _DictionaryCodingKey(intValue: key as! Int)
        try container.encode(value, forKey: codingKey)
      }
    } else if #available(SwiftStdlib 5.6, *),
              Key.self is CodingKeyRepresentable.Type {
      // Since the keys are CodingKeyRepresentable, we can use the `codingKey`
      // to create `_DictionaryCodingKey` instances.
      var container = encoder.container(keyedBy: _DictionaryCodingKey.self)
      for (key, value) in self {
        let codingKey = (key as! CodingKeyRepresentable).codingKey
        let dictionaryCodingKey = _DictionaryCodingKey(codingKey: codingKey)
        try container.encode(value, forKey: dictionaryCodingKey)
      }
    } else {
      // Keys are Encodable but not Strings or Ints, so we cannot arbitrarily
      // convert to keys. We can encode as an array of alternating key-value
      // pairs, though.
      var container = encoder.unkeyedContainer()
      for (key, value) in self {
        try container.encode(key)
        try container.encode(value)
      }
    }
  }
}

extension Dictionary: Decodable where Key: Decodable, Value: Decodable {
  /// Creates a new dictionary by decoding from the given decoder.
  ///
  /// This initializer throws an error if reading from the decoder fails, or
  /// if the data read is corrupted or otherwise invalid.
  ///
  /// - Parameter decoder: The decoder to read data from.
  public init(from decoder: any Decoder) throws {
    self.init()

    if Key.self == String.self {
      // The keys are Strings, so we should be able to expect a keyed container.
      let container = try decoder.container(keyedBy: _DictionaryCodingKey.self)
      for key in container.allKeys {
        let value = try container.decode(Value.self, forKey: key)
        self[key.stringValue as! Key] = value
      }
    } else if Key.self == Int.self {
      // The keys are Ints, so we should be able to expect a keyed container.
      let container = try decoder.container(keyedBy: _DictionaryCodingKey.self)
      for key in container.allKeys {
        guard key.intValue != nil else {
          // We provide stringValues for Int keys; if an encoder chooses not to
          // use the actual intValues, we've encoded string keys.
          // So on init, _DictionaryCodingKey tries to parse string keys as
          // Ints. If that succeeds, then we would have had an intValue here.
          // We don't, so this isn't a valid Int key.
          var codingPath = decoder.codingPath
          codingPath.append(key)
          throw DecodingError.typeMismatch(
            Int.self,
            DecodingError.Context(
              codingPath: codingPath,
              debugDescription: "Expected Int key but found String key instead."
            )
          )
        }

        let value = try container.decode(Value.self, forKey: key)
        self[key.intValue! as! Key] = value
      }
    } else if #available(SwiftStdlib 5.6, *),
              let keyType = Key.self as? CodingKeyRepresentable.Type {
      // The keys are CodingKeyRepresentable, so we should be able to expect
      // a keyed container.
      let container = try decoder.container(keyedBy: _DictionaryCodingKey.self)
      for codingKey in container.allKeys {
        guard let key: Key = keyType.init(codingKey: codingKey) as? Key else {
          throw DecodingError.dataCorruptedError(
            forKey: codingKey,
            in: container,
            debugDescription: "Could not convert key to type \(Key.self)"
          )
        }
        let value: Value = try container.decode(Value.self, forKey: codingKey)
        self[key] = value
      }
    } else {
      // We should have encoded as an array of alternating key-value pairs.
      var container = try decoder.unkeyedContainer()

      // We're expecting to get pairs. If the container has a known count, it
      // had better be even; no point in doing work if not.
      if let count = container.count {
        guard count % 2 == 0 else {
          throw DecodingError.dataCorrupted(
            DecodingError.Context(
              codingPath: decoder.codingPath,
              debugDescription: "Expected collection of key-value pairs; encountered odd-length array instead."
            )
          )
        }
      }

      while !container.isAtEnd {
        let key = try container.decode(Key.self)

        guard !container.isAtEnd else {
          throw DecodingError.dataCorrupted(
            DecodingError.Context(
              codingPath: decoder.codingPath,
              debugDescription: "Unkeyed container reached end before value in key-value pair."
            )
          )
        }

        let value = try container.decode(Value.self)
        self[key] = value
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Convenience Default Implementations
//===----------------------------------------------------------------------===//

// Default implementation of encodeConditional(_:forKey:) in terms of
// encode(_:forKey:)
extension KeyedEncodingContainerProtocol {
  public mutating func encodeConditional<T: AnyObject & Encodable>(
    _ object: T,
    forKey key: Key
  ) throws {
    try encode(object, forKey: key)
  }
}

// Default implementation of encodeIfPresent(_:forKey:) in terms of
// encode(_:forKey:)
extension KeyedEncodingContainerProtocol {
  public mutating func encodeIfPresent(
    _ value: Bool?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: String?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Double?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Float?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Int?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Int8?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Int16?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Int32?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: Int64?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encodeIfPresent(
    _ value: Int128?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: UInt?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: UInt8?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: UInt16?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: UInt32?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent(
    _ value: UInt64?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encodeIfPresent(
    _ value: UInt128?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }

  public mutating func encodeIfPresent<T: Encodable>(
    _ value: T?,
    forKey key: Key
  ) throws {
    guard let value = value else { return }
    try encode(value, forKey: key)
  }
}

// Default implementations for types with stricter availability than KECP.
extension KeyedEncodingContainerProtocol {
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: Int128, forKey key: Key) throws {
    throw EncodingError.invalidValue(
      value,
      EncodingError.Context(
        codingPath: codingPath + [key],
        debugDescription: "Encoder has not implemented support for Int128"
      )
    )
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: UInt128, forKey key: Key) throws {
    throw EncodingError.invalidValue(
      value,
      EncodingError.Context(
        codingPath: codingPath + [key],
        debugDescription: "Encoder has not implemented support for UInt128"
      )
    )
  }
}

// Default implementation of decodeIfPresent(_:forKey:) in terms of
// decode(_:forKey:) and decodeNil(forKey:)
extension KeyedDecodingContainerProtocol {
  public func decodeIfPresent(
    _ type: Bool.Type,
    forKey key: Key
  ) throws -> Bool? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Bool.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: String.Type,
    forKey key: Key
  ) throws -> String? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(String.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Double.Type,
    forKey key: Key
  ) throws -> Double? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Double.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Float.Type,
    forKey key: Key
  ) throws -> Float? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Float.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Int.Type,
    forKey key: Key
  ) throws -> Int? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Int.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Int8.Type,
    forKey key: Key
  ) throws -> Int8? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Int8.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Int16.Type,
    forKey key: Key
  ) throws -> Int16? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Int16.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Int32.Type,
    forKey key: Key
  ) throws -> Int32? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Int32.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: Int64.Type,
    forKey key: Key
  ) throws -> Int64? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(Int64.self, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  public func decodeIfPresent(
    _ type: Int128.Type,
    forKey key: Key
  ) throws -> Int128? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
    else { return nil }
    return try self.decode(Int128.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: UInt.Type,
    forKey key: Key
  ) throws -> UInt? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(UInt.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: UInt8.Type,
    forKey key: Key
  ) throws -> UInt8? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(UInt8.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: UInt16.Type,
    forKey key: Key
  ) throws -> UInt16? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(UInt16.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: UInt32.Type,
    forKey key: Key
  ) throws -> UInt32? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(UInt32.self, forKey: key)
  }

  public func decodeIfPresent(
    _ type: UInt64.Type,
    forKey key: Key
  ) throws -> UInt64? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(UInt64.self, forKey: key)
  }
  
  @available(SwiftStdlib 6.0, *)
  public func decodeIfPresent(
    _ type: UInt128.Type,
    forKey key: Key
  ) throws -> UInt128? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
    else { return nil }
    return try self.decode(UInt128.self, forKey: key)
  }

  public func decodeIfPresent<T: Decodable>(
    _ type: T.Type,
    forKey key: Key
  ) throws -> T? {
    guard try self.contains(key) && !self.decodeNil(forKey: key)
      else { return nil }
    return try self.decode(T.self, forKey: key)
  }
}

// Default implementations for types with stricter availability than KDCP.
extension KeyedDecodingContainerProtocol {
  @available(SwiftStdlib 6.0, *)
  public func decode(
    _ type: Int128.Type,
    forKey key: Key
  ) throws -> Int128 {
    throw DecodingError.typeMismatch(
      Int128.self,
      DecodingError.Context(
        codingPath: codingPath + [key],
        debugDescription: "Decoder has not implemented support for Int128"
      )
    )
  }
  
  @available(SwiftStdlib 6.0, *)
  public func decode(
    _ type: UInt128.Type,
    forKey key: Key
  ) throws -> UInt128 {
    throw DecodingError.typeMismatch(
      UInt128.self,
      DecodingError.Context(
        codingPath: codingPath + [key],
        debugDescription: "Decoder has not implemented support for UInt128"
      )
    )
  }
}

// Default implementation of encodeConditional(_:) in terms of encode(_:),
// and encode(contentsOf:) in terms of encode(_:) loop.
extension UnkeyedEncodingContainer {
  public mutating func encodeConditional<T: AnyObject & Encodable>(
    _ object: T
  ) throws {
    try self.encode(object)
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Bool {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == String {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Double {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Float {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int8 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int16 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int32 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int64 {
    for element in sequence {
      try self.encode(element)
    }
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == Int128 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt8 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt16 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt32 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt64 {
    for element in sequence {
      try self.encode(element)
    }
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element == UInt128 {
    for element in sequence {
      try self.encode(element)
    }
  }

  public mutating func encode<T: Sequence>(
    contentsOf sequence: T
  ) throws where T.Element: Encodable {
    for element in sequence {
      try self.encode(element)
    }
  }
}

// Default implementations for types with stricter availability than UEC.
extension UnkeyedEncodingContainer {
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: Int128) throws {
    throw EncodingError.invalidValue(
      value,
      EncodingError.Context(
        codingPath: codingPath,
        debugDescription: "Encoder has not implemented support for Int128"
      )
    )
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: UInt128) throws {
    throw EncodingError.invalidValue(
      value,
      EncodingError.Context(
        codingPath: codingPath,
        debugDescription: "Encoder has not implemented support for UInt128"
      )
    )
  }
}

// Default implementation of decodeIfPresent(_:) in terms of decode(_:) and
// decodeNil()
extension UnkeyedDecodingContainer {
  public mutating func decodeIfPresent(
    _ type: Bool.Type
  ) throws -> Bool? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Bool.self)
  }

  public mutating func decodeIfPresent(
    _ type: String.Type
  ) throws -> String? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(String.self)
  }

  public mutating func decodeIfPresent(
    _ type: Double.Type
  ) throws -> Double? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Double.self)
  }

  public mutating func decodeIfPresent(
    _ type: Float.Type
  ) throws -> Float? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Float.self)
  }

  public mutating func decodeIfPresent(
    _ type: Int.Type
  ) throws -> Int? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Int.self)
  }

  public mutating func decodeIfPresent(
    _ type: Int8.Type
  ) throws -> Int8? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Int8.self)
  }

  public mutating func decodeIfPresent(
    _ type: Int16.Type
  ) throws -> Int16? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Int16.self)
  }

  public mutating func decodeIfPresent(
    _ type: Int32.Type
  ) throws -> Int32? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Int32.self)
  }

  public mutating func decodeIfPresent(
    _ type: Int64.Type
  ) throws -> Int64? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Int64.self)
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func decodeIfPresent(
    _ type: Int128.Type
  ) throws -> Int128? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(Int128.self)
  }

  public mutating func decodeIfPresent(
    _ type: UInt.Type
  ) throws -> UInt? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(UInt.self)
  }

  public mutating func decodeIfPresent(
    _ type: UInt8.Type
  ) throws -> UInt8? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(UInt8.self)
  }

  public mutating func decodeIfPresent(
    _ type: UInt16.Type
  ) throws -> UInt16? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(UInt16.self)
  }

  public mutating func decodeIfPresent(
    _ type: UInt32.Type
  ) throws -> UInt32? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(UInt32.self)
  }

  public mutating func decodeIfPresent(
    _ type: UInt64.Type
  ) throws -> UInt64? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(UInt64.self)
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func decodeIfPresent(
    _ type: UInt128.Type
  ) throws -> UInt128? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(UInt128.self)
  }

  public mutating func decodeIfPresent<T: Decodable>(
    _ type: T.Type
  ) throws -> T? {
    guard try !self.isAtEnd && !self.decodeNil() else { return nil }
    return try self.decode(T.self)
  }
}

// Default implementations for types with stricter availability than UDC.
extension UnkeyedDecodingContainer {
  @available(SwiftStdlib 6.0, *)
  public mutating func decode(_ type: Int128.Type) throws -> Int128 {
    throw DecodingError.typeMismatch(
      Int128.self,
      DecodingError.Context(
        codingPath: codingPath,
        debugDescription: "Decoder has not implemented support for Int128"
      )
    )
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func decode(_ type: UInt128.Type) throws -> UInt128 {
    throw DecodingError.typeMismatch(
      UInt128.self,
      DecodingError.Context(
        codingPath: codingPath,
        debugDescription: "Decoder has not implemented support for UInt128"
      )
    )
  }
}

// Default implementations for types with stricter availability than SVEC.
extension SingleValueEncodingContainer {
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: Int128) throws {
    throw EncodingError.invalidValue(
      value,
      EncodingError.Context(
        codingPath: codingPath,
        debugDescription: "Encoder has not implemented support for Int128"
      )
    )
  }
  
  @available(SwiftStdlib 6.0, *)
  public mutating func encode(_ value: UInt128) throws {
    throw EncodingError.invalidValue(
      value,
      EncodingError.Context(
        codingPath: codingPath,
        debugDescription: "Encoder has not implemented support for UInt128"
      )
    )
  }
}

// Default implementations for types with stricter availability than SVDC.
extension SingleValueDecodingContainer {
  @available(SwiftStdlib 6.0, *)
  public func decode(_ type: Int128.Type) throws -> Int128 {
    throw DecodingError.typeMismatch(
      Int128.self,
      DecodingError.Context(
        codingPath: codingPath,
        debugDescription: "Decoder has not implemented support for Int128"
      )
    )
  }
  
  @available(SwiftStdlib 6.0, *)
  public func decode(_ type: UInt128.Type) throws -> UInt128 {
    throw DecodingError.typeMismatch(
      UInt128.self,
      DecodingError.Context(
        codingPath: codingPath,
        debugDescription: "Decoder has not implemented support for UInt128"
      )
    )
  }
}
