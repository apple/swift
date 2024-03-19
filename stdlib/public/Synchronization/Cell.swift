//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift Atomics open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Builtin

@available(SwiftStdlib 6.0, *)
@frozen
@usableFromInline
@_rawLayout(like: Value)
internal struct _Cell<Value: ~Copyable>: ~Copyable {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  var address: UnsafeMutablePointer<Value> {
    UnsafeMutablePointer<Value>(rawAddress)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  var rawAddress: Builtin.RawPointer {
    Builtin.unprotectedAddressOfBorrow(self)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  init(_ initialValue: consuming Value) {
    address.initialize(to: initialValue)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @inlinable
  deinit {
    address.deinitialize(count: 1)
  }
}
