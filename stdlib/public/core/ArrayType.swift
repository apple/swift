//===--- ArrayType.swift - Protocol for Array-like types ------------------===//
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

@usableFromInline
internal protocol _ArrayProtocol
  : RangeReplaceableCollection, ExpressibleByArrayLiteral
where Index == Int {
  /// The number of elements the Array can store without reallocation.
  var capacity: Int { get }

  /// An object that guarantees the lifetime of this array's elements.
  var _owner: AnyObject? { get }

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, `nil`.
  var _baseAddressIfContiguous: UnsafeMutablePointer<Element>? { get }

  //===--- implementation detail  -----------------------------------------===//

  associatedtype _Buffer: _ArrayBufferProtocol where _Buffer.Element == Element
  init(_ buffer: _Buffer)

  // For testing.
  var _buffer: _Buffer { get set }
}

extension _ArrayProtocol {
  /// An object that guarantees the lifetime of this array's elements.
  @inlinable
  public // @testable
  var _owner: AnyObject? {
    @inline(__always)
    get {
      return _buffer.owner      
    }
  }

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, `nil`.
  @inlinable
  public var _baseAddressIfContiguous: UnsafeMutablePointer<Element>? {
    @inline(__always) // FIXME(TODO: JIRA): Hack around test failure
    get { return _buffer.firstElementAddressIfContiguous }
  }

  @inlinable
  internal var _baseAddress: UnsafeMutablePointer<Element> {
    return _buffer.firstElementAddress
  }
  
  @inlinable
  @_semantics("array.get_count")
  internal func _getCount() -> Int {
    return _buffer.count
  }

  @inlinable
  @_semantics("array.get_capacity")
  internal func _getCapacity() -> Int {
    return _buffer.capacity
  }
  
  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeMutableAndUnique() {
    if _slowPath(!_buffer.isMutableAndUniquelyReferenced()) {
      _buffer = _Buffer(copying: _buffer)
    }
  }

  @inlinable
  @_semantics("array.reserve_capacity_for_append")
  internal mutating func reserveCapacityForAppend(newElementsCount: Int) {
    let oldCount = self.count
    let oldCapacity = self.capacity
    let newCount = oldCount + newElementsCount

    // Ensure uniqueness, mutability, and sufficient storage.  Note that
    // for consistency, we need unique self even if newElements is empty.
    self.reserveCapacity(
      newCount > oldCapacity ?
      Swift.max(newCount, _growArrayCapacity(oldCapacity))
      : newCount)
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _appendElementAssumeUniqueAndCapacity(
    _ oldCount: Int,
    newElement: __owned Element
  ) {
    _sanityCheck(_buffer.isMutableAndUniquelyReferenced())
    _sanityCheck(_buffer.capacity >= _buffer.count + 1)

    _buffer.count = oldCount + 1
    (_buffer.firstElementAddress + oldCount).initialize(to: newElement)
  }

  @inline(never)
  @usableFromInline
  internal static func _allocateBufferUninitialized(
    minimumCapacity: Int
  ) -> _Buffer {
    let newBuffer = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: 0, minimumCapacity: minimumCapacity)
    return _Buffer(_buffer: newBuffer, shiftedToStartIndex: 0)
  }

  /// Copy the contents of the current buffer to a new unique mutable buffer.
  /// The count of the new buffer is set to `oldCount`, the capacity of the
  /// new buffer is big enough to hold 'oldCount' + 1 elements.
  @inline(never)
  @inlinable // @specializable
  internal mutating func _copyToNewBuffer(oldCount: Int) {
    let newCount = oldCount + 1
    var newBuffer = _buffer._forceCreateUniqueMutableBuffer(
      countForNewBuffer: oldCount, minNewCapacity: newCount)
    _buffer._arrayOutOfPlaceUpdate(&newBuffer, oldCount, 0)
  }

  @inlinable
  @_semantics("array.make_mutable")
  internal mutating func _makeUniqueAndReserveCapacityIfNotUnique() {
    if _slowPath(!_buffer.isMutableAndUniquelyReferenced()) {
      _copyToNewBuffer(oldCount: _buffer.count)
    }
  }

  @inlinable
  @_semantics("array.mutate_unknown")
  internal mutating func _reserveCapacityAssumingUniqueBuffer(oldCount: Int) {
    // This is a performance optimization. This code used to be in an ||
    // statement in the _sanityCheck below.
    //
    //   _sanityCheck(_buffer.capacity == 0 ||
    //                _buffer.isMutableAndUniquelyReferenced())
    //
    // SR-6437
    let capacity = _buffer.capacity == 0

    // Due to make_mutable hoisting the situation can arise where we hoist
    // _makeMutableAndUnique out of loop and use it to replace
    // _makeUniqueAndReserveCapacityIfNotUnique that preceeds this call. If the
    // array was empty _makeMutableAndUnique does not replace the empty array
    // buffer by a unique buffer (it just replaces it by the empty array
    // singleton).
    // This specific case is okay because we will make the buffer unique in this
    // function because we request a capacity > 0 and therefore _copyToNewBuffer
    // will be called creating a new buffer.
    _sanityCheck(capacity || _buffer.isMutableAndUniquelyReferenced())

    if _slowPath(oldCount + 1 > _buffer.capacity) {
      _copyToNewBuffer(oldCount: oldCount)
    }
  }
  
  // Since RangeReplaceableCollection now has a version of filter that is less
  // efficient, we should make the default implementation coming from Sequence
  // preferred.
  @inlinable
  public __consuming func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> [Element] {
    return try _filter(isIncluded)
  }
}
