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
@_rawLayout(like: Value, movesAsLike)
internal struct _Cell<Value: ~Copyable>: ~Copyable {
  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  internal var _address: UnsafeMutablePointer<Value> {
    UnsafeMutablePointer<Value>(_rawAddress)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  internal var _rawAddress: Builtin.RawPointer {
#if $BuiltinAddressOfRawLayout
    Builtin.addressOfRawLayout(self)
#else
    fatalError()
#endif
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  internal init(_ initialValue: consuming Value) {
    _address.initialize(to: initialValue)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @inlinable
  deinit {
    _address.deinitialize(count: 1)
  }
}
