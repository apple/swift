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
  var _address: UnsafeMutablePointer<Value.AtomicRepresentation> {
    UnsafeMutablePointer<Value.AtomicRepresentation>(_rawAddress)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  var _rawAddress: Builtin.RawPointer {
    Builtin.unprotectedAddressOfBorrow(self)
  }

  /// Initializes a value of this atomic with the given initial value.
  ///
  /// - Parameter initialValue: The initial value to set this atomic.
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  public init(_ initialValue: consuming Value) {
    _address.initialize(to: Value.encodeAtomicRepresentation(initialValue))
  }

  // Deinit's can't be marked @_transparent. Do these things need all of these
  // attributes..?
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @inlinable
  deinit {
    let oldValue = Value.decodeAtomicRepresentation(_address.pointee)
    _ = consume oldValue

    _address.deinitialize(count: 1)
  }
}

@available(SwiftStdlib 6.0, *)
extension Atomic: @unchecked Sendable where Value: Sendable {}
