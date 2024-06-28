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

import SwiftShims

/// String

@_unavailableInEmbedded
internal func _print_unlocked<T>(_ value: T, _ target: inout String) { fatalError() }
@_unavailableInEmbedded
public func _debugPrint_unlocked<T>(_ value: T, _ target: inout String) { fatalError() }

/// Codable

@_unavailableInEmbedded
public protocol Encodable {
  func encode(to encoder: any Encoder) throws
}

@_unavailableInEmbedded
public protocol Decodable {
  init(from decoder: any Decoder) throws
}

@_unavailableInEmbedded
public typealias Codable = Encodable & Decodable

@_unavailableInEmbedded
public protocol CodingKey { }

@_unavailableInEmbedded
public struct KeyedDecodingContainer<K: CodingKey> { }

@_unavailableInEmbedded
public struct KeyedEncodingContainer<K: CodingKey> { }

@_unavailableInEmbedded
public protocol UnkeyedDecodingContainer { 
  mutating func decode<T>(_ type: T.Type) throws -> T
}

@_unavailableInEmbedded
public protocol UnkeyedEncodingContainer { 
  mutating func encode<T>(_ value: T) throws
}

@_unavailableInEmbedded
public protocol SingleValueDecodingContainer { }

@_unavailableInEmbedded
public protocol SingleValueEncodingContainer { }

@_unavailableInEmbedded
public protocol Encoder {
  var codingPath: [any CodingKey] { get }
  func container<Key>(keyedBy type: Key.Type) -> KeyedEncodingContainer<Key>
  func unkeyedContainer() -> any UnkeyedEncodingContainer
  func singleValueContainer() -> any SingleValueEncodingContainer
}

@_unavailableInEmbedded
public protocol Decoder {
  var codingPath: [any CodingKey] { get }
  func container<Key>(keyedBy type: Key.Type) throws -> KeyedDecodingContainer<Key>
  func unkeyedContainer() throws -> any UnkeyedDecodingContainer
  func singleValueContainer() throws -> any SingleValueDecodingContainer
}

@_unavailableInEmbedded
public enum DecodingError: Error {
  public struct Context: Sendable {
    public init(codingPath: [any CodingKey], debugDescription: String, underlyingError: Error? = nil) { fatalError() }
  }
  case typeMismatch(Any.Type, Context)
  case valueNotFound(Any.Type, Context)
  case keyNotFound(any CodingKey, Context)
  case dataCorrupted(Context)
}
