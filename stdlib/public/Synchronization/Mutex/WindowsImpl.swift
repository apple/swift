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

import WinSDK.core.synch

@available(SwiftStdlib 6.0, *)
@frozen
@usableFromInline
@_staticExclusiveOnly
internal struct _MutexHandle: ~Copyable {
  @usableFromInline
  let value: _Cell<SRWLOCK>

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  init() {
    value = _Cell(SRWLOCK())
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  borrowing func lock() {
    AcquireSRWLockExclusive(value.address)
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  borrowing func tryLock() -> Bool {
    // Windows BOOLEAN gets imported as 'UInt8'...
    TryAcquireSRWLockExclusive(value.address) != 0
  }

  @available(SwiftStdlib 6.0, *)
  @_alwaysEmitIntoClient
  @_transparent
  borrowing func unlock() {
    ReleaseSRWLockExclusive(value.address)
  }
}
