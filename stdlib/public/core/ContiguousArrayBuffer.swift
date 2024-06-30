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

import SwiftShims

#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
@_silgen_name("swift_COWChecksEnabled")
public func _COWChecksEnabled() -> Bool
#endif

/// Class used whose sole instance is used as storage for empty
/// arrays.  The instance is defined in the runtime and statically
/// initialized.  See stdlib/runtime/GlobalObjects.cpp for details.
/// Because it's statically referenced, it requires non-lazy realization
/// by the Objective-C runtime.
///
/// NOTE: older runtimes called this _EmptyArrayStorage. The two must
/// coexist, so it was renamed. The old name must not be used in the new
/// runtime.
@_fixed_layout
@usableFromInline
@_objc_non_lazy_realization
internal final class __EmptyArrayStorage
  : __ContiguousArrayStorageBase {

  @inlinable
  @nonobjc
  internal init(_doNotCallMe: ()) {
    _internalInvariantFailure("creating instance of __EmptyArrayStorage")
  }
  
#if _runtime(_ObjC)
  override internal func _withVerbatimBridgedUnsafeBuffer<R>(
    _ body: (UnsafeBufferPointer<AnyObject>) throws -> R
  ) rethrows -> R? {
    return try body(UnsafeBufferPointer(start: nil, count: 0))
  }

  override internal func _getNonVerbatimBridgingBuffer() -> _BridgingBuffer {
    return _BridgingBuffer(0)
  }
#endif

  @inlinable
  override internal func canStoreElements(ofDynamicType _: Any.Type) -> Bool {
    return false
  }

  /// A type that every element in the array is.
  @inlinable
  @_unavailableInEmbedded
  override internal var staticElementType: Any.Type {
    return Void.self
  }
}

@available(*, unavailable)
extension __EmptyArrayStorage: Sendable {}

#if $Embedded
// In embedded Swift, the stdlib is a .swiftmodule only without any .o/.a files,
// to allow consuming it by clients with different LLVM codegen setting (-mcpu
// flags, etc.), which means we cannot declare the singleton in a C/C++ file.
//
// TODO: We should figure out how to make this a constant so that it's placed in
// non-writable memory (can't be a let, Builtin.addressof below requires a var).
public var _swiftEmptyArrayStorage: (Int, Int, Int, Int) =
    (/*isa*/0, /*refcount*/-1, /*count*/0, /*flags*/1)
#endif

/// The storage for static read-only arrays.
///
/// In contrast to `_ContiguousArrayStorage` this class is _not_ generic over
/// the element type, because the metatype for static read-only arrays cannot
/// be instantiated at runtime.
///
/// Static read-only arrays can only contain non-verbatim bridged element types.
@_fixed_layout
@usableFromInline
@_objc_non_lazy_realization
internal final class __StaticArrayStorage
  : __ContiguousArrayStorageBase {

  @inlinable
  @nonobjc
  internal init(_doNotCallMe: ()) {
    _internalInvariantFailure("creating instance of __StaticArrayStorage")
  }

#if _runtime(_ObjC)
  override internal func _withVerbatimBridgedUnsafeBuffer<R>(
    _ body: (UnsafeBufferPointer<AnyObject>) throws -> R
  ) rethrows -> R? {
    return nil
  }

  override internal func _getNonVerbatimBridgingBuffer() -> _BridgingBuffer {
    fatalError("__StaticArrayStorage._withVerbatimBridgedUnsafeBuffer must not be called")
  }
#endif

  @inlinable
  override internal func canStoreElements(ofDynamicType _: Any.Type) -> Bool {
    return false
  }

  @inlinable
  @_unavailableInEmbedded
  override internal var staticElementType: Any.Type {
    fatalError("__StaticArrayStorage.staticElementType must not be called")
  }
}

@available(*, unavailable)
extension __StaticArrayStorage: Sendable {}

/// The empty array prototype.  We use the same object for all empty
/// `[Native]Array<Element>`s.
@inlinable
internal var _emptyArrayStorage: __EmptyArrayStorage {
  return Builtin.bridgeFromRawPointer(
    Builtin.addressof(&_swiftEmptyArrayStorage))
}

// The class that implements the storage for a ContiguousArray<Element>
@_fixed_layout
@usableFromInline
internal final class _ContiguousArrayStorage<
  Element
>: __ContiguousArrayStorageBase {

  @inlinable
  deinit {
    _elementPointer.deinitialize(count: countAndCapacity.count)
    _fixLifetime(self)
  }

#if _runtime(_ObjC)
  
  internal final override func withUnsafeBufferOfObjects<R>(
    _ body: (UnsafeBufferPointer<AnyObject>) throws -> R
  ) rethrows -> R {
    _internalInvariant(_isBridgedVerbatimToObjectiveC(Element.self))
    let count = countAndCapacity.count
    let elements = UnsafeRawPointer(_elementPointer)
      .assumingMemoryBound(to: AnyObject.self)
    defer { _fixLifetime(self) }
    return try body(UnsafeBufferPointer(start: elements, count: count))
  }
  
  @objc(countByEnumeratingWithState:objects:count:)
  @_effects(releasenone)
  internal final override func countByEnumerating(
    with state: UnsafeMutablePointer<_SwiftNSFastEnumerationState>,
    objects: UnsafeMutablePointer<AnyObject>?, count: Int
  ) -> Int {
    var enumerationState = state.pointee
    
    if enumerationState.state != 0 {
      return 0
    }
    
    return withUnsafeBufferOfObjects {
      objects in
      enumerationState.mutationsPtr = _fastEnumerationStorageMutationsPtr
      enumerationState.itemsPtr =
        AutoreleasingUnsafeMutablePointer(objects.baseAddress)
      enumerationState.state = 1
      state.pointee = enumerationState
      return objects.count
    }
  }
  
  @inline(__always)
  @_effects(readonly)
  @nonobjc private func _objectAt(_ index: Int) -> Unmanaged<AnyObject> {
    return withUnsafeBufferOfObjects {
      objects in
      _precondition(
        _isValidArraySubscript(index, count: objects.count),
        "Array index out of range")
      return Unmanaged.passUnretained(objects[index])
    }
  }
  
  @objc(objectAtIndexedSubscript:)
  @_effects(readonly)
  final override internal func objectAtSubscript(_ index: Int) -> Unmanaged<AnyObject> {
    return _objectAt(index)
  }
  
  @objc(objectAtIndex:)
  @_effects(readonly)
  final override internal func objectAt(_ index: Int) -> Unmanaged<AnyObject> {
    return _objectAt(index)
  }
  
  @objc internal override final var count: Int {
    @_effects(readonly) get {
      return withUnsafeBufferOfObjects { $0.count }
    }
  }

  @_effects(releasenone)
  @objc internal override final func getObjects(
    _ aBuffer: UnsafeMutablePointer<AnyObject>, range: _SwiftNSRange
  ) {
    return withUnsafeBufferOfObjects {
      objects in
      _precondition(
        _isValidArrayIndex(range.location, count: objects.count),
        "Array index out of range")

      _precondition(
        _isValidArrayIndex(
          range.location + range.length, count: objects.count),
        "Array index out of range")

      if objects.isEmpty { return }

      // These objects are "returned" at +0, so treat them as pointer values to
      // avoid retains. Copy bytes via a raw pointer to circumvent reference
      // counting while correctly aliasing with all other pointer types.
      UnsafeMutableRawPointer(aBuffer).copyMemory(
        from: objects.baseAddress! + range.location,
        byteCount: range.length * MemoryLayout<AnyObject>.stride)
    }
  }
  
  /// If the `Element` is bridged verbatim, invoke `body` on an
  /// `UnsafeBufferPointer` to the elements and return the result.
  /// Otherwise, return `nil`.
  internal final override func _withVerbatimBridgedUnsafeBuffer<R>(
    _ body: (UnsafeBufferPointer<AnyObject>) throws -> R
  ) rethrows -> R? {
    var result: R?
    try self._withVerbatimBridgedUnsafeBufferImpl {
      result = try body($0)
    }
    return result
  }

  /// If `Element` is bridged verbatim, invoke `body` on an
  /// `UnsafeBufferPointer` to the elements.
  internal final func _withVerbatimBridgedUnsafeBufferImpl(
    _ body: (UnsafeBufferPointer<AnyObject>) throws -> Void
  ) rethrows {
    if _isBridgedVerbatimToObjectiveC(Element.self) {
      let count = countAndCapacity.count
      let elements = UnsafeRawPointer(_elementPointer)
        .assumingMemoryBound(to: AnyObject.self)
      defer { _fixLifetime(self) }
      try body(UnsafeBufferPointer(start: elements, count: count))
    }
  }

  /// Bridge array elements and return a new buffer that owns them.
  ///
  /// - Precondition: `Element` is bridged non-verbatim.
  override internal func _getNonVerbatimBridgingBuffer() -> _BridgingBuffer {
    _internalInvariant(
      !_isBridgedVerbatimToObjectiveC(Element.self),
      "Verbatim bridging should be handled separately")
    let count = countAndCapacity.count
    let result = _BridgingBuffer(count)
    let resultPtr = result.baseAddress
    let p = _elementPointer
    for i in 0..<count {
      (resultPtr + i).initialize(to: _bridgeAnythingToObjectiveC(p[i]))
    }
    _fixLifetime(self)
    return result
  }
#endif

  /// Returns `true` if the `proposedElementType` is `Element` or a subclass of
  /// `Element`.  We can't store anything else without violating type
  /// safety; for example, the destructor has static knowledge that
  /// all of the elements can be destroyed as `Element`.
  @inlinable
  internal override func canStoreElements(
    ofDynamicType proposedElementType: Any.Type
  ) -> Bool {
#if _runtime(_ObjC)
    return proposedElementType is Element.Type
#else
    // FIXME: Dynamic casts don't currently work without objc. 
    // rdar://problem/18801510
    return false
#endif
  }

  /// A type that every element in the array is.
  @inlinable
  @_unavailableInEmbedded
  internal override var staticElementType: Any.Type {
    return Element.self
  }

  @inlinable
  internal final var _elementPointer: UnsafeMutablePointer<Element> {
    return UnsafeMutablePointer(Builtin.projectTailElems(self, Element.self))
  }
}

@available(*, unavailable)
extension _ContiguousArrayStorage: Sendable {}

@_alwaysEmitIntoClient
@inline(__always)
internal func _uncheckedUnsafeBitCast<T, U>(_ x: T, to type: U.Type) -> U {
  return Builtin.reinterpretCast(x)
}

@_alwaysEmitIntoClient
@inline(never)
@_effects(readonly)
@_semantics("array.getContiguousArrayStorageType")
func getContiguousArrayStorageType<Element>(
  for: Element.Type
) -> _ContiguousArrayStorage<Element>.Type {
    // We can only reset the type metadata to the correct metadata when bridging
    // on the current OS going forward.
    if #available(macOS 13.0, iOS 16.0, watchOS 9.0, tvOS 16.0, *) { // SwiftStdlib 5.7
      if Element.self is AnyObject.Type {
        return _uncheckedUnsafeBitCast(
          _ContiguousArrayStorage<AnyObject>.self,
          to: _ContiguousArrayStorage<Element>.Type.self)
      }
    }
    return _ContiguousArrayStorage<Element>.self
}

@usableFromInline
@frozen
internal struct _ContiguousArrayBuffer<Element>: _ArrayBufferProtocol {
  @usableFromInline
  internal var _storage: __ContiguousArrayStorageBase

  /// Make a buffer with uninitialized elements.  After using this
  /// method, you must either initialize the `count` elements at the
  /// result's `.firstElementAddress` or set the result's `.count`
  /// to zero.
  @inlinable
  internal init(
    _uninitializedCount uninitializedCount: Int,
    minimumCapacity: Int
  ) {
    let realMinimumCapacity = Swift.max(uninitializedCount, minimumCapacity)
    if realMinimumCapacity == 0 {
      self = _ContiguousArrayBuffer<Element>()
    }
    else {
      #if !$Embedded
      _storage = Builtin.allocWithTailElems_1(
         getContiguousArrayStorageType(for: Element.self), realMinimumCapacity._builtinWordValue, Element.self)
      #else
      _storage = Builtin.allocWithTailElems_1(
         _ContiguousArrayStorage<Element>.self, realMinimumCapacity._builtinWordValue, Element.self)
      #endif

      let storageAddr = UnsafeMutableRawPointer(Builtin.bridgeToRawPointer(_storage))
      let allocSize: Int?
      #if !$Embedded
      allocSize = _mallocSize(ofAllocation: storageAddr)
      #else
      allocSize = nil
      #endif
      if let allocSize {
        let endAddr = storageAddr + allocSize
        let realCapacity = endAddr.assumingMemoryBound(to: Element.self) - firstElementAddress
        _initStorageHeader(
          count: uninitializedCount, capacity: realCapacity)
      } else {
        _initStorageHeader(
          count: uninitializedCount, capacity: realMinimumCapacity)
      }
    }
  }

  /// Initialize using the given uninitialized `storage`.
  /// The storage is assumed to be uninitialized. The returned buffer has the
  /// body part of the storage initialized, but not the elements.
  ///
  /// - Warning: The result has uninitialized elements.
  /// 
  /// - Warning: storage may have been stack-allocated, so it's
  ///   crucial not to call, e.g., `malloc_size` on it.
  @inlinable
  internal init(count: Int, storage: _ContiguousArrayStorage<Element>) {
    _storage = storage

    _initStorageHeader(count: count, capacity: count)
  }

  @inlinable
  internal init(_ storage: __ContiguousArrayStorageBase) {
    _storage = storage
  }

  /// Initialize the body part of our storage.
  ///
  /// - Warning: does not initialize elements
  @inlinable
  internal func _initStorageHeader(count: Int, capacity: Int) {
#if _runtime(_ObjC)
    let verbatim = _isBridgedVerbatimToObjectiveC(Element.self)
#else
    let verbatim = false
#endif

    // We can initialize by assignment because _ArrayBody is a trivial type,
    // i.e. contains no references.
    _storage.countAndCapacity = _ArrayBody(
      count: count,
      capacity: capacity,
      elementTypeIsBridgedVerbatim: verbatim)
  }

  /// True, if the array is native and does not need a deferred type check.
  @inlinable
  internal var arrayPropertyIsNativeTypeChecked: Bool {
    return true
  }

  /// A pointer to the first element.
  @inlinable
  internal var firstElementAddress: UnsafeMutablePointer<Element> {
    return UnsafeMutablePointer(Builtin.projectTailElems(_storage,
                                                         Element.self))
  }

  /// A mutable pointer to the first element.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  internal var mutableFirstElementAddress: UnsafeMutablePointer<Element> {
    return UnsafeMutablePointer(Builtin.projectTailElems(mutableOrEmptyStorage,
                                                         Element.self))
  }

  @inlinable
  internal var firstElementAddressIfContiguous: UnsafeMutablePointer<Element>? {
    return firstElementAddress
  }

  /// Call `body(p)`, where `p` is an `UnsafeBufferPointer` over the
  /// underlying contiguous storage.
  @inlinable
  internal func withUnsafeBufferPointer<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    defer { _fixLifetime(self) }
    return try body(UnsafeBufferPointer(start: firstElementAddress,
      count: count))
  }

  /// Call `body(p)`, where `p` is an `UnsafeMutableBufferPointer`
  /// over the underlying contiguous storage.
  @inlinable
  internal mutating func withUnsafeMutableBufferPointer<R>(
    _ body: (UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    defer { _fixLifetime(self) }
    return try body(
      UnsafeMutableBufferPointer(start: firstElementAddress, count: count))
  }

  //===--- _ArrayBufferProtocol conformance -----------------------------------===//
  /// Create an empty buffer.
  @inlinable
  internal init() {
    _storage = _emptyArrayStorage
  }

  @inlinable
  internal init(_buffer buffer: _ContiguousArrayBuffer, shiftedToStartIndex: Int) {
    _internalInvariant(shiftedToStartIndex == 0, "shiftedToStartIndex must be 0")
    self = buffer
  }

  @inlinable
  internal mutating func requestUniqueMutableBackingBuffer(
    minimumCapacity: Int
  ) -> _ContiguousArrayBuffer<Element>? {
    if _fastPath(isUniquelyReferenced() && capacity >= minimumCapacity) {
      return self
    }
    return nil
  }

  @inlinable
  internal mutating func isMutableAndUniquelyReferenced() -> Bool {
    return isUniquelyReferenced()
  }

  /// If this buffer is backed by a `_ContiguousArrayBuffer`
  /// containing the same number of elements as `self`, return it.
  /// Otherwise, return `nil`.
  @inlinable
  internal func requestNativeBuffer() -> _ContiguousArrayBuffer<Element>? {
    return self
  }

  @inlinable
  @inline(__always)
  internal func getElement(_ i: Int) -> Element {
    _internalInvariant(i >= 0 && i < count, "Array index out of range")
    let addr = UnsafePointer<Element>(
      Builtin.projectTailElems(immutableStorage, Element.self))
    return addr[i]
  }

  /// The storage of an immutable buffer.
  ///
  /// - Precondition: The buffer must be immutable.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal var immutableStorage : __ContiguousArrayStorageBase {
#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
    _internalInvariant(isImmutable, "Array storage is not immutable")
#endif
    return Builtin.COWBufferForReading(_storage)
  }

  /// The storage of a mutable buffer.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal var mutableStorage : __ContiguousArrayStorageBase {
#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
    _internalInvariant(isMutable, "Array storage is immutable")
#endif
    return _storage
  }

  /// The storage of a mutable or empty buffer.
  ///
  /// - Precondition: The buffer must be mutable or the empty array singleton.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal var mutableOrEmptyStorage : __ContiguousArrayStorageBase {
#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
    _internalInvariant(isMutable || _storage.countAndCapacity.capacity == 0,
                       "Array storage is immutable and not empty")
#endif
    return _storage
  }

#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
  @_alwaysEmitIntoClient
  internal var isImmutable: Bool {
    get {
      if (_COWChecksEnabled()) {
        return capacity == 0 || _swift_isImmutableCOWBuffer(_storage)
      }
      return true
    }
    nonmutating set {
      if (_COWChecksEnabled()) {
        // Make sure to not modify the empty array singleton (which has a
        // capacity of 0).
        if capacity > 0 {
          let wasImmutable = _swift_setImmutableCOWBuffer(_storage, newValue)
          if newValue {
            _internalInvariant(!wasImmutable,
              "re-setting immutable array buffer to immutable")
          } else {
            _internalInvariant(wasImmutable,
              "re-setting mutable array buffer to mutable")
          }
        }
      }
    }
  }
  
  @_alwaysEmitIntoClient
  internal var isMutable: Bool {
    if (_COWChecksEnabled()) {
      return !_swift_isImmutableCOWBuffer(_storage)
    }
    return true
  }
#endif

  /// Get or set the value of the ith element.
  @inlinable
  internal subscript(i: Int) -> Element {
    @inline(__always)
    get {
      return getElement(i)
    }
    @inline(__always)
    nonmutating set {
      _internalInvariant(i >= 0 && i < count, "Array index out of range")

      // FIXME: Manually swap because it makes the ARC optimizer happy.  See
      // <rdar://problem/16831852> check retain/release order
      // firstElementAddress[i] = newValue
      var nv = newValue
      let tmp = nv
      nv = firstElementAddress[i]
      firstElementAddress[i] = tmp
    }
  }

  /// The number of elements the buffer stores.
  ///
  /// This property is obsolete. It's only used for the ArrayBufferProtocol and
  /// to keep backward compatibility.
  /// Use `immutableCount` or `mutableCount` instead.
  @inlinable
  internal var count: Int {
    get {
      return _storage.countAndCapacity.count
    }
    nonmutating set {
      _internalInvariant(newValue >= 0)

      _internalInvariant(
        newValue <= mutableCapacity,
        "Can't grow an array buffer past its capacity")

      mutableStorage.countAndCapacity.count = newValue
    }
  }
  
  /// The number of elements of the buffer.
  ///
  /// - Precondition: The buffer must be immutable.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal var immutableCount: Int {
    return immutableStorage.countAndCapacity.count
  }

  /// The number of elements of the buffer.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  internal var mutableCount: Int {
    @inline(__always)
    get {
      return mutableOrEmptyStorage.countAndCapacity.count
    }
    @inline(__always)
    nonmutating set {
      _internalInvariant(newValue >= 0)

      _internalInvariant(
        newValue <= mutableCapacity,
        "Can't grow an array buffer past its capacity")

      mutableStorage.countAndCapacity.count = newValue
    }
  }

  /// Traps unless the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  ///
  /// - Precondition: The buffer must be immutable.
  @inlinable
  @inline(__always)
  internal func _checkValidSubscript(_ index: Int) {
    _precondition(
      (index >= 0) && (index < immutableCount),
      "Index out of range"
    )
  }

  /// Traps unless the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal func _checkValidSubscriptMutating(_ index: Int) {
    _precondition(
      (index >= 0) && (index < mutableCount),
      "Index out of range"
    )
  }

  /// The number of elements the buffer can store without reallocation.
  ///
  /// This property is obsolete. It's only used for the ArrayBufferProtocol and
  /// to keep backward compatibility.
  /// Use `immutableCapacity` or `mutableCapacity` instead.
  @inlinable
  internal var capacity: Int {
    return _storage.countAndCapacity.capacity
  }

  /// The number of elements the buffer can store without reallocation.
  ///
  /// - Precondition: The buffer must be immutable.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal var immutableCapacity: Int {
    return immutableStorage.countAndCapacity.capacity
  }

  /// The number of elements the buffer can store without reallocation.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal var mutableCapacity: Int {
    return mutableOrEmptyStorage.countAndCapacity.capacity
  }

  /// Copy the elements in `bounds` from this buffer into uninitialized
  /// memory starting at `target`.  Return a pointer "past the end" of the
  /// just-initialized memory.
  @inlinable
  @discardableResult
  internal __consuming func _copyContents(
    subRange bounds: Range<Int>,
    initializing target: UnsafeMutablePointer<Element>
  ) -> UnsafeMutablePointer<Element> {
    _internalInvariant(bounds.lowerBound >= 0)
    _internalInvariant(bounds.upperBound >= bounds.lowerBound)
    _internalInvariant(bounds.upperBound <= count)

    let initializedCount = bounds.upperBound - bounds.lowerBound
    target.initialize(
      from: firstElementAddress + bounds.lowerBound, count: initializedCount)
    _fixLifetime(owner)
    return target + initializedCount
  }

  @inlinable
  internal __consuming func _copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) {
    guard buffer.count > 0 else { return (makeIterator(), 0) }
    let c = Swift.min(self.count, buffer.count)
    buffer.baseAddress!.initialize(
      from: firstElementAddress,
      count: c)
    _fixLifetime(owner)
    return (IndexingIterator(_elements: self, _position: c), c)
  }

  /// Returns a `_SliceBuffer` containing the given `bounds` of values
  /// from this buffer.
  @inlinable
  internal subscript(bounds: Range<Int>) -> _SliceBuffer<Element> {
    get {
      #if $Embedded
      let storage = Builtin.castToNativeObject(_storage)
      #else
      let storage = _storage
      #endif
      return _SliceBuffer(
        owner: storage,
        subscriptBaseAddress: firstElementAddress,
        indices: bounds,
        hasNativeBuffer: true)
    }
    set {
      fatalError("not implemented")
    }
  }

  /// Returns `true` if this buffer's storage is uniquely-referenced;
  /// otherwise, returns `false`.
  ///
  /// This function should only be used for internal soundness checks.
  /// To guard a buffer mutation, use `beginCOWMutation`.
  @inlinable
  internal mutating func isUniquelyReferenced() -> Bool {
    return _isUnique(&_storage)
  }

  /// Returns `true` and puts the buffer in a mutable state if the buffer's
  /// storage is uniquely-referenced; otherwise, performs no action and returns
  /// `false`.
  ///
  /// - Precondition: The buffer must be immutable.
  ///
  /// - Warning: It's a requirement to call `beginCOWMutation` before the buffer
  ///   is mutated.
  @_alwaysEmitIntoClient
  internal mutating func beginCOWMutation() -> Bool {
    if Bool(Builtin.beginCOWMutation(&_storage)) {
#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
      isImmutable = false
#endif
      return true
    }
    return false;
  }

  /// Puts the buffer in an immutable state.
  ///
  /// - Precondition: The buffer must be mutable or the empty array singleton.
  ///
  /// - Warning: After a call to `endCOWMutation` the buffer must not be mutated
  ///   until the next call of `beginCOWMutation`.
  @_alwaysEmitIntoClient
  @inline(__always)
  internal mutating func endCOWMutation() {
#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
    isImmutable = true
#endif
    Builtin.endCOWMutation(&_storage)
  }

  /// Creates and returns a new uniquely referenced buffer which is a copy of
  /// this buffer.
  ///
  /// This buffer is consumed, i.e. it's released.
  @_alwaysEmitIntoClient
  @inline(never)
  @_semantics("optimize.sil.specialize.owned2guarantee.never")
  internal __consuming func _consumeAndCreateNew() -> _ContiguousArrayBuffer {
    return _consumeAndCreateNew(bufferIsUnique: false,
                                minimumCapacity: count,
                                growForAppend: false)
  }

  /// Creates and returns a new uniquely referenced buffer which is a copy of
  /// this buffer.
  ///
  /// If `bufferIsUnique` is true, the buffer is assumed to be uniquely
  /// referenced and the elements are moved - instead of copied - to the new
  /// buffer.
  /// The `minimumCapacity` is the lower bound for the new capacity.
  /// If `growForAppend` is true, the new capacity is calculated using
  /// `_growArrayCapacity`, but at least kept at `minimumCapacity`.
  ///
  /// This buffer is consumed, i.e. it's released.
  @_alwaysEmitIntoClient
  @inline(never)
  @_semantics("optimize.sil.specialize.owned2guarantee.never")
  internal __consuming func _consumeAndCreateNew(
    bufferIsUnique: Bool, minimumCapacity: Int, growForAppend: Bool
  ) -> _ContiguousArrayBuffer {
    let newCapacity = _growArrayCapacity(oldCapacity: capacity,
                                         minimumCapacity: minimumCapacity,
                                         growForAppend: growForAppend)
    let c = count
    _internalInvariant(newCapacity >= c)
    
    let newBuffer = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: c, minimumCapacity: newCapacity)

    if bufferIsUnique {
      // As an optimization, if the original buffer is unique, we can just move
      // the elements instead of copying.
      let dest = newBuffer.mutableFirstElementAddress
      dest.moveInitialize(from: firstElementAddress,
                          count: c)
      mutableCount = 0
    } else {
      _copyContents(
        subRange: 0..<c,
        initializing: newBuffer.mutableFirstElementAddress)
    }
    return newBuffer
  }
  
  @_alwaysEmitIntoClient
  @inline(never)
  @_semantics("optimize.sil.specialize.owned2guarantee.never")
  internal __consuming func _consumeAndCreateNew(
    bufferIsUnique: Bool, 
    minimumCapacity: Int,
    growForAppend: Bool,
    rangeToNotCopy: Range<Int>
  ) -> _ContiguousArrayBuffer {
    let newCapacity = _growArrayCapacity(oldCapacity: capacity,
                                         minimumCapacity: minimumCapacity,
                                         growForAppend: growForAppend)
    let c = count
    _internalInvariant(newCapacity >= c)
    
    let newBuffer = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: c, minimumCapacity: newCapacity)
    let dest = newBuffer.mutableFirstElementAddress
    let offset = MemoryLayout<Element>.stride * rangeToNotCopy.upperBound

    if bufferIsUnique {
      // As an optimization, if the original buffer is unique, we can just move
      // the elements instead of copying.
      if rangeToNotCopy.lowerBound > 0 {
        dest.moveInitialize(from: firstElementAddress,
                            count: (0 ..< rangeToNotCopy.lowerBound).count)
      }
      if rangeToNotCopy.upperBound < c {
        dest.moveInitialize(from: firstElementAddress + offset,
                            count: (rangeToNotCopy.upperBound ..< c).count)
      }
      mutableCount = 0
    } else {
      if rangeToNotCopy.lowerBound > 0 {
        _copyContents(
          subRange: 0 ..< rangeToNotCopy.lowerBound,
          initializing: dest)
      }
      if rangeToNotCopy.upperBound < c {
        _copyContents(
          subRange: rangeToNotCopy.upperBound ..< c,
          initializing: dest + offset)
      }
    }
    return newBuffer
  }

#if _runtime(_ObjC)
  
  /// Convert to an NSArray.
  ///
  /// - Precondition: `Element` is bridged to Objective-C.
  ///
  /// - Complexity: O(1).
  @usableFromInline
  internal __consuming func _asCocoaArray() -> AnyObject {
    // _asCocoaArray was @inlinable in Swift 5.0 and 5.1, which means that there
    // are existing apps out there that effectively have the old implementation
    // Be careful with future changes to this function. Here be dragons!
    // The old implementation was
    // if count == 0 {
    //   return _emptyArrayStorage
    // }
    // if _isBridgedVerbatimToObjectiveC(Element.self) {
    //   return _storage
    // }
    // return __SwiftDeferredNSArray(_nativeStorage: _storage)
    
    _connectOrphanedFoundationSubclassesIfNeeded()
    if count == 0 {
      return _emptyArrayStorage
    }
    if _isBridgedVerbatimToObjectiveC(Element.self) {
      if #available(SwiftStdlib 5.7, *) {
        // We optimize _ContiguousArrayStorage<Element> where Element is any
        // class type to use _ContiguousArrayStorage<AnyObject> when we bridge
        // to objective-c we need to set the correct Element type so that when
        // we bridge back we can use O(1) bridging i.e we can adopt the storage.
        _ = _swift_setClassMetadata(_ContiguousArrayStorage<Element>.self,
                                    onObject: _storage)
      }
      return _storage
    }
    if _storage is __StaticArrayStorage {
      return __SwiftDeferredStaticNSArray<Element>(_nativeStorage: _storage)
    }
    return __SwiftDeferredNSArray(_nativeStorage: _storage)
  }
#endif

  #if $Embedded
  public typealias AnyObject = Builtin.NativeObject
  #endif

  /// An object that keeps the elements stored in this buffer alive.
  @inlinable
  internal var owner: AnyObject {
    #if !$Embedded
    return _storage
    #else
    return Builtin.castToNativeObject(_storage)
    #endif
  }

  /// An object that keeps the elements stored in this buffer alive.
  @inlinable
  internal var nativeOwner: AnyObject {
    return owner
  }

  /// A value that identifies the storage used by the buffer.
  ///
  /// Two buffers address the same elements when they have the same
  /// identity and count.
  @inlinable
  internal var identity: UnsafeRawPointer {
    return UnsafeRawPointer(firstElementAddress)
  }
  
  /// Returns `true` if we have storage for elements of the given
  /// `proposedElementType`.  If not, we'll be treated as immutable.
  @inlinable
  func canStoreElements(ofDynamicType proposedElementType: Any.Type) -> Bool {
    return _storage.canStoreElements(ofDynamicType: proposedElementType)
  }

  /// Returns `true` if the buffer stores only elements of type `U`.
  ///
  /// - Precondition: `U` is a class or `@objc` existential.
  ///
  /// - Complexity: O(*n*)
  @inlinable
  @_unavailableInEmbedded
  internal func storesOnlyElementsOfType<U>(
    _: U.Type
  ) -> Bool {
    _internalInvariant(_isClassOrObjCExistential(U.self))

    if _fastPath(_storage.staticElementType is U.Type) {
      // Done in O(1)
      return true
    }

    // Check the elements
    for x in self {
      if !(x is U) {
        return false
      }
    }
    return true
  }
}

@available(*, unavailable)
extension _ContiguousArrayBuffer: Sendable {}

/// Append the elements of `rhs` to `lhs`.
@inlinable
internal func += <Element, C: Collection>(
  lhs: inout _ContiguousArrayBuffer<Element>, rhs: __owned C
) where C.Element == Element {

  let oldCount = lhs.count
  let newCount = oldCount + rhs.count

  let buf: UnsafeMutableBufferPointer<Element>
  
  if _fastPath(newCount <= lhs.capacity) {
    buf = UnsafeMutableBufferPointer(
      start: lhs.firstElementAddress + oldCount,
      count: rhs.count)
    lhs.mutableCount = newCount
  }
  else {
    var newLHS = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: newCount,
      minimumCapacity: _growArrayCapacity(lhs.capacity))

    newLHS.firstElementAddress.moveInitialize(
      from: lhs.firstElementAddress, count: oldCount)
    lhs.mutableCount = 0
    (lhs, newLHS) = (newLHS, lhs)
    buf = UnsafeMutableBufferPointer(
      start: lhs.firstElementAddress + oldCount,
      count: rhs.count)
  }

  var (remainders,writtenUpTo) = buf.initialize(from: rhs)

  // ensure that exactly rhs.count elements were written
  _precondition(remainders.next() == nil, "rhs underreported its count")
  _precondition(writtenUpTo == buf.endIndex, "rhs overreported its count")    
}

extension _ContiguousArrayBuffer: RandomAccessCollection {
  /// The position of the first element in a non-empty collection.
  ///
  /// In an empty collection, `startIndex == endIndex`.
  @inlinable
  internal var startIndex: Int {
    return 0
  }
  /// The collection's "past the end" position.
  ///
  /// `endIndex` is not a valid argument to `subscript`, and is always
  /// reachable from `startIndex` by zero or more applications of
  /// `index(after:)`.
  @inlinable
  internal var endIndex: Int {
    return count
  }

  @usableFromInline
  internal typealias Indices = Range<Int>
}

extension Sequence {
  @inlinable
  public __consuming func _copyToContiguousArray() -> ContiguousArray<Element> {
    return _copySequenceToContiguousArray(self)
  }
}

@inlinable
internal func _copySequenceToContiguousArray<
  S: Sequence
>(_ source: S) -> ContiguousArray<S.Element> {
  let initialCapacity = source.underestimatedCount
  var builder =
    _UnsafePartiallyInitializedContiguousArrayBuffer<S.Element>(
      initialCapacity: initialCapacity)

  var iterator = source.makeIterator()

  // FIXME(performance): use _copyContents(initializing:).

  // Add elements up to the initial capacity without checking for regrowth.
  for _ in 0..<initialCapacity {
    builder.addWithExistingCapacity(iterator.next()!)
  }

  // Add remaining elements, if any.
  while let element = iterator.next() {
    builder.add(element)
  }

  return builder.finish()
}

extension Collection {
  @inlinable
  public __consuming func _copyToContiguousArray() -> ContiguousArray<Element> {
    return _copyCollectionToContiguousArray(self)
  }
}

extension _ContiguousArrayBuffer {
  @inlinable
  internal __consuming func _copyToContiguousArray() -> ContiguousArray<Element> {
    return ContiguousArray(_buffer: self)
  }
}

/// This is a fast implementation of _copyToContiguousArray() for collections.
///
/// It avoids the extra retain, release overhead from storing the
/// ContiguousArrayBuffer into
/// _UnsafePartiallyInitializedContiguousArrayBuffer. Since we do not support
/// ARC loops, the extra retain, release overhead cannot be eliminated which
/// makes assigning ranges very slow. Once this has been implemented, this code
/// should be changed to use _UnsafePartiallyInitializedContiguousArrayBuffer.
@inlinable
internal func _copyCollectionToContiguousArray<
  C: Collection
>(_ source: C) -> ContiguousArray<C.Element>
{
  let count = source.count
  if count == 0 {
    return ContiguousArray()
  }

  var result = _ContiguousArrayBuffer<C.Element>(
    _uninitializedCount: count,
    minimumCapacity: 0)

  let p = UnsafeMutableBufferPointer(
    start: result.firstElementAddress,
    count: count)
  var (itr, end) = source._copyContents(initializing: p)

  _debugPrecondition(itr.next() == nil,
    "invalid Collection: more than 'count' elements in collection")
  // We also have to check the evil shrink case in release builds, because
  // it can result in uninitialized array elements and therefore undefined
  // behavior.
  _precondition(end == p.endIndex,
    "invalid Collection: less than 'count' elements in collection")

  result.endCOWMutation()
  return ContiguousArray(_buffer: result)
}

/// A "builder" interface for initializing array buffers.
///
/// This presents a "builder" interface for initializing an array buffer
/// element-by-element. The type is unsafe because it cannot be deinitialized
/// until the buffer has been finalized by a call to `finish`.
@usableFromInline
@frozen
internal struct _UnsafePartiallyInitializedContiguousArrayBuffer<Element> {
  @usableFromInline
  internal var result: _ContiguousArrayBuffer<Element>
  @usableFromInline
  internal var p: UnsafeMutablePointer<Element>
  @usableFromInline
  internal var remainingCapacity: Int

  /// Initialize the buffer with an initial size of `initialCapacity`
  /// elements.
  @inlinable
  @inline(__always) // For performance reasons.
  internal init(initialCapacity: Int) {
    if initialCapacity == 0 {
      result = _ContiguousArrayBuffer()
    } else {
      result = _ContiguousArrayBuffer(
        _uninitializedCount: initialCapacity,
        minimumCapacity: 0)
    }

    p = result.firstElementAddress
    remainingCapacity = result.capacity
  }

  /// Add an element to the buffer, reallocating if necessary.
  @inlinable
  @inline(__always) // For performance reasons.
  internal mutating func add(_ element: Element) {
    if remainingCapacity == 0 {
      // Reallocate.
      let newCapacity = max(_growArrayCapacity(result.capacity), 1)
      var newResult = _ContiguousArrayBuffer<Element>(
        _uninitializedCount: newCapacity, minimumCapacity: 0)
      p = newResult.firstElementAddress + result.capacity
      remainingCapacity = newResult.capacity - result.capacity
      if !result.isEmpty {
        // This check prevents a data race writing to _swiftEmptyArrayStorage
        // Since count is always 0 there, this code does nothing anyway
        newResult.firstElementAddress.moveInitialize(
          from: result.firstElementAddress, count: result.capacity)
        result.mutableCount = 0
      }
      (result, newResult) = (newResult, result)
    }
    addWithExistingCapacity(element)
  }

  /// Add an element to the buffer, which must have remaining capacity.
  @inlinable
  @inline(__always) // For performance reasons.
  internal mutating func addWithExistingCapacity(_ element: Element) {
    _internalInvariant(remainingCapacity > 0,
      "_UnsafePartiallyInitializedContiguousArrayBuffer has no more capacity")
    remainingCapacity -= 1

    p.initialize(to: element)
    p += 1
  }

  /// Finish initializing the buffer, adjusting its count to the final
  /// number of elements.
  ///
  /// Returns the fully-initialized buffer. `self` is reset to contain an
  /// empty buffer and cannot be used afterward.
  @inlinable
  @inline(__always) // For performance reasons.
  internal mutating func finish() -> ContiguousArray<Element> {
    // Adjust the initialized count of the buffer.
    if (result.capacity != 0) {
      result.mutableCount = result.capacity - remainingCapacity
    } else {
      _internalInvariant(remainingCapacity == 0)
      _internalInvariant(result.count == 0)      
    }

    return finishWithOriginalCount()
  }

  /// Finish initializing the buffer, assuming that the number of elements
  /// exactly matches the `initialCount` for which the initialization was
  /// started.
  ///
  /// Returns the fully-initialized buffer. `self` is reset to contain an
  /// empty buffer and cannot be used afterward.
  @inlinable
  @inline(__always) // For performance reasons.
  internal mutating func finishWithOriginalCount() -> ContiguousArray<Element> {
    _internalInvariant(remainingCapacity == result.capacity - result.count,
      "_UnsafePartiallyInitializedContiguousArrayBuffer has incorrect count")
    var finalResult = _ContiguousArrayBuffer<Element>()
    (finalResult, result) = (result, finalResult)
    remainingCapacity = 0
    finalResult.endCOWMutation()
    return ContiguousArray(_buffer: finalResult)
  }
}

@available(*, unavailable)
extension _UnsafePartiallyInitializedContiguousArrayBuffer: Sendable {}
