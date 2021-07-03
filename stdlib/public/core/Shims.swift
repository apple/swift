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
@usableFromInline
internal var _fastEnumerationStorageMutationsTarget: CUnsignedLong = 0

/// A dummy pointer to be used as `mutationsPtr` in fast enumeration
/// implementations.
@usableFromInline
internal let _fastEnumerationStorageMutationsPtr =
  UnsafeMutablePointer<CUnsignedLong>(Builtin.addressof(&_fastEnumerationStorageMutationsTarget))
#endif




@usableFromInline @_alwaysEmitIntoClient @_effects(readonly)
internal func _mallocSize(ofAllocation ptr: UnsafeRawPointer) -> Int? {
  return _swift_stdlib_has_malloc_size() ? _swift_stdlib_malloc_size(ptr) : nil
}

@_transparent @inlinable @_effects(readonly)
internal func _mallocSizeIfHeap(of object: AnyObject) -> Int? {
  if Bool(Builtin.isOnStack(_nativeObject(fromNative: object))) { return nil }
  return _mallocSize(
    ofAllocation: UnsafeRawPointer(Builtin.bridgeToRawPointer(object))
  )
}

