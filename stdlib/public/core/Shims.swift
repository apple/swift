//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Additions to 'SwiftShims' that can be written in Swift.
///
//===----------------------------------------------------------------------===//

import SwiftShims

#if _runtime(_ObjC)
@inlinable
internal func _makeSwiftNSFastEnumerationState()
   -> _SwiftNSFastEnumerationState {
  return _SwiftNSFastEnumerationState(
    state: 0, itemsPtr: nil, mutationsPtr: nil,
    extra: (0, 0, 0, 0, 0))
}

/// A dummy value to be used as the target for `mutationsPtr` in fast
/// enumeration implementations.
///
/// Note: This is marked nonisolated unsafe because we _never_ write to this
/// value. It is unfortunate that this is not a let, but we need to take the
/// address of this variable which is currently only possible with vars in
/// Swift.
@usableFromInline
internal nonisolated(unsafe)
var _fastEnumerationStorageMutationsTarget: CUnsignedLong = 0

/// A dummy pointer to be used as `mutationsPtr` in fast enumeration
/// implementations.
@usableFromInline
internal let _fastEnumerationStorageMutationsPtr =
  UnsafeMutablePointer<CUnsignedLong>(Builtin.addressof(&_fastEnumerationStorageMutationsTarget))
#endif

@usableFromInline @_alwaysEmitIntoClient
internal func _mallocSize(ofAllocation ptr: UnsafeRawPointer) -> Int? {
  return _swift_stdlib_has_malloc_size() ? _swift_stdlib_malloc_size(ptr) : nil
}
