//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Builtin

/// An atomic value.
@available(SwiftStdlib 6.0, *)
@frozen
@_rawLayout(like: Value.AtomicRepresentation)
@_staticExclusiveOnly
public struct Atomic<Value: AtomicRepresentable>: ~Copyable {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  var address: UnsafeMutablePointer<Value.AtomicRepresentation> {
    UnsafeMutablePointer<Value.AtomicRepresentation>(rawAddress)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  var rawAddress: Builtin.RawPointer {
    Builtin.unprotectedAddressOfBorrow(self)
  }

  /// Initializes a value of this atomic with the given initial value.
  ///
  /// - Parameter initialValue: The initial value to set this atomic.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(_ initialValue: consuming Value) {
    address.initialize(to: Value.encodeAtomicRepresentation(initialValue))
  }

  // Deinit's can't be marked @_transparent. Do these things need all of these
  // attributes..?
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @inlinable
  deinit {
    let oldValue = Value.decodeAtomicRepresentation(address.pointee)
    _ = consume oldValue

    address.deinitialize(count: 1)
  }
}

@available(SwiftStdlib 6.0, *)
extension Atomic: @unchecked Sendable where Value: Sendable {}

@available(SwiftStdlib 6.0, *)
extension Atomic: ExpressibleByNilLiteral where Value: ExpressibleByNilLiteral {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(nilLiteral: ()) {
    self.init(Value(nilLiteral: ()))
  }
}

@available(SwiftStdlib 6.0, *)
extension Atomic: ExpressibleByBooleanLiteral where Value: ExpressibleByBooleanLiteral {
  @available(SwiftStdlib 6.0, *)
  public typealias BooleanLiteralType = Value.BooleanLiteralType

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(booleanLiteral value: BooleanLiteralType) {
    let initialValue = Value(booleanLiteral: value)
    address.initialize(to: Value.encodeAtomicRepresentation(initialValue))
  }
}

@available(SwiftStdlib 6.0, *)
extension Atomic: ExpressibleByIntegerLiteral where Value: ExpressibleByIntegerLiteral {
  @available(SwiftStdlib 6.0, *)
  public typealias IntegerLiteralType = Value.IntegerLiteralType

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(integerLiteral value: IntegerLiteralType) {
    let initialValue = Value(integerLiteral: value)
    address.initialize(to: Value.encodeAtomicRepresentation(initialValue))
  }
}

@available(SwiftStdlib 6.0, *)
extension Atomic: ExpressibleByFloatLiteral where Value: ExpressibleByFloatLiteral {
  @available(SwiftStdlib 6.0, *)
  public typealias FloatLiteralType = Value.FloatLiteralType

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(floatLiteral value: FloatLiteralType) {
    let initialValue = Value(floatLiteral: value)
    address.initialize(to: Value.encodeAtomicRepresentation(initialValue))
  }
}
