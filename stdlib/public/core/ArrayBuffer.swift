//===--- ArrayBuffer.swift - Dynamic storage for Swift Array --------------===//
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
//
//  This is the class that implements the storage and object management for
//  Swift Array.
//
//===----------------------------------------------------------------------===//

#if _runtime(_ObjC)
import SwiftShims

@usableFromInline
internal typealias _ArrayBridgeStorage
  = _BridgeStorage<__ContiguousArrayStorageBase>

@usableFromInline
@frozen
internal struct _ArrayBuffer<Element>: _ArrayBufferProtocol {
  @usableFromInline
  internal var _storage: _ArrayBridgeStorage

  @inlinable
  internal init(storage: _ArrayBridgeStorage) {
    _storage = storage
  }

  /// Create an empty buffer.
  @inlinable
  internal init() {
    _storage = _ArrayBridgeStorage(native: _emptyArrayStorage)
  }

  @inlinable
  internal init(nsArray: AnyObject) {
    _internalInvariant(_isClassOrObjCExistential(Element.self))
    _storage = _ArrayBridgeStorage(objC: nsArray)
  }

  /// Returns an `_ArrayBuffer<U>` containing the same elements.
  ///
  /// - Precondition: The elements actually have dynamic type `U`, and `U`
  ///   is a class or `@objc` existential.
  @inlinable
  __consuming internal func cast<U>(toBufferOf _: U.Type) -> _ArrayBuffer<U> {
    _internalInvariant(_isClassOrObjCExistential(Element.self))
    _internalInvariant(_isClassOrObjCExistential(U.self))
    return _ArrayBuffer<U>(storage: _storage)
  }

  /// Returns an `_ArrayBuffer<U>` containing the same elements,
  /// deferring checking each element's `U`-ness until it is accessed.
  ///
  /// - Precondition: `U` is a class or `@objc` existential derived from
  /// `Element`.
  @inlinable
  __consuming internal func downcast<U>(
    toBufferWithDeferredTypeCheckOf _: U.Type
  ) -> _ArrayBuffer<U> {
    _internalInvariant(_isClassOrObjCExistential(Element.self))
    _internalInvariant(_isClassOrObjCExistential(U.self))
    
    // FIXME: can't check that U is derived from Element pending
    // <rdar://problem/20028320> generic metatype casting doesn't work
    // _internalInvariant(U.self is Element.Type)

    return _ArrayBuffer<U>(
      storage: _ArrayBridgeStorage(native: _native._storage, isFlagged: true))
  }

  @inlinable
  internal var needsElementTypeCheck: Bool {
    // NSArray's need an element typecheck when the element type isn't AnyObject
    return !_isNativeTypeChecked && !(AnyObject.self is Element.Type)
  }
}

extension _ArrayBuffer {
  /// Adopt the storage of `source`.
  @inlinable
  internal init(_buffer source: NativeBuffer, shiftedToStartIndex: Int) {
    _internalInvariant(shiftedToStartIndex == 0, "shiftedToStartIndex must be 0")
    _storage = _ArrayBridgeStorage(native: source._storage)
  }

  /// `true`, if the array is native and does not need a deferred type check.
  @inlinable
  internal var arrayPropertyIsNativeTypeChecked: Bool {
    return _isNativeTypeChecked
  }

  /// Returns `true` if this buffer's storage is uniquely referenced;
  /// otherwise, returns `false`.
  ///
  /// This function should only be used for internal soundness checks.
  /// To guard a buffer mutation, use `beginCOWMutation`.
  @inlinable
  internal mutating func isUniquelyReferenced() -> Bool {
    if !_isClassOrObjCExistential(Element.self) {
      return _storage.isUniquelyReferencedUnflaggedNative()
    }
    return _storage.isUniquelyReferencedNative()
   }
  
  /// Returns `true` and puts the buffer in a mutable state if the buffer's
  /// storage is uniquely-referenced; otherwise performs no action and
  /// returns `false`.
  ///
  /// - Precondition: The buffer must be immutable.
  ///
  /// - Warning: It's a requirement to call `beginCOWMutation` before the buffer
  ///   is mutated.
  @_alwaysEmitIntoClient
  internal mutating func beginCOWMutation() -> Bool {
    let isUnique: Bool
    if !_isClassOrObjCExistential(Element.self) {
      isUnique = _storage.beginCOWMutationUnflaggedNative()
    } else if !_storage.beginCOWMutationNative() {
      return false
    } else {
      isUnique = _isNative
    }
#if INTERNAL_CHECKS_ENABLED && COW_CHECKS_ENABLED
    if isUnique {
      _native.isImmutable = false
    }
#endif
    return isUnique
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
    _native.isImmutable = true
#endif
    _storage.endCOWMutation()
  }

  /// Convert to an NSArray.
  ///
  /// O(1) if the element type is bridged verbatim, O(*n*) otherwise.
  @inlinable
  internal func _asCocoaArray() -> AnyObject {
    return _fastPath(_isNative) ? _native._asCocoaArray() : _nonNative.buffer
  }

  /// Creates and returns a new uniquely referenced buffer which is a copy of
  /// this buffer.
  ///
  /// This buffer is consumed, i.e. it's released.
  @_alwaysEmitIntoClient
  @inline(never)
  @_semantics("optimize.sil.specialize.owned2guarantee.never")
  internal __consuming func _consumeAndCreateNew() -> _ArrayBuffer {
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
  ) -> _ArrayBuffer {
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
      let dest = newBuffer.firstElementAddress
      dest.moveInitialize(from: mutableFirstElementAddress,
                          count: c)
      _native.mutableCount = 0
    } else {
      _copyContents(
        subRange: 0..<c,
        initializing: newBuffer.mutableFirstElementAddress)
    }
    return _ArrayBuffer(_buffer: newBuffer, shiftedToStartIndex: 0)
  }

  /// If this buffer is backed by a uniquely-referenced mutable
  /// `_ContiguousArrayBuffer` that can be grown in-place to allow the self
  /// buffer store minimumCapacity elements, returns that buffer.
  /// Otherwise, returns `nil`.
  @inlinable
  internal mutating func requestUniqueMutableBackingBuffer(minimumCapacity: Int)
  -> NativeBuffer? {
    if _fastPath(isUniquelyReferenced()) {
      let b = _native
      if _fastPath(b.mutableCapacity >= minimumCapacity) {
        return b
      }
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
  internal func requestNativeBuffer() -> NativeBuffer? {
    if !_isClassOrObjCExistential(Element.self) {
      return _native
    }
    return _fastPath(_storage.isNative) ? _native : nil
  }

  // We have two versions of type check: one that takes a range and the other
  // checks one element. The reason for this is that the ARC optimizer does not
  // handle loops atm. and so can get blocked by the presence of a loop (over
  // the range). This loop is not necessary for a single element access.
  @inline(never)
  @usableFromInline
  internal func _typeCheckSlowPath(_ index: Int) {
    if _fastPath(_isNative) {
      let element: AnyObject = cast(toBufferOf: AnyObject.self)._native[index]
      guard element is Element else {
        _assertionFailure(
          "Fatal error",
          """
          Down-casted Array element failed to match the target type
          Expected \(Element.self) but found \(type(of: element))
          """,
          flags: _fatalErrorFlags()
        )
      }
    }
    else {
      let element = _nonNative[index]
      guard element is Element else {
        _assertionFailure(
          "Fatal error",
          """
          NSArray element failed to match the Swift Array Element type
          Expected \(Element.self) but found \(type(of: element)).
          If these appear to be the same type, you likely have a duplicate ObjC
          class. Debug by setting OBJC_DEBUG_DUPLICATE_CLASSES and checking for
          log messages in STDERR.
          """,
          flags: _fatalErrorFlags()
        )
      }
    }
  }

  @inlinable
  internal func _typeCheck(_ subRange: Range<Int>) {
    if !_isClassOrObjCExistential(Element.self) {
      return
    }

    if _slowPath(needsElementTypeCheck) {
      // Could be sped up, e.g. by using
      // enumerateObjectsAtIndexes:options:usingBlock: in the
      // non-native case.
      for i in subRange.lowerBound ..< subRange.upperBound {
        _typeCheckSlowPath(i)
      }
    }
  }

  /// Copy the elements in `bounds` from this buffer into uninitialized
  /// memory starting at `target`.  Return a pointer "past the end" of the
  /// just-initialized memory.
  @inlinable
  @discardableResult
  __consuming internal func _copyContents(
    subRange bounds: Range<Int>,
    initializing target: UnsafeMutablePointer<Element>
  ) -> UnsafeMutablePointer<Element> {
    _typeCheck(bounds)
    if _fastPath(_isNative) {
      return _native._copyContents(subRange: bounds, initializing: target)
    }
    let buffer = UnsafeMutableRawPointer(target)
      .assumingMemoryBound(to: AnyObject.self)
    let result = _nonNative._copyContents(
      subRange: bounds,
      initializing: buffer)
    return UnsafeMutableRawPointer(result).assumingMemoryBound(to: Element.self)
  }

  @inlinable
  internal __consuming func _copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) {
    if _fastPath(_isNative) {
      let (_, c) = _native._copyContents(initializing: buffer)
      return (IndexingIterator(_elements: self, _position: c), c)
    }
    guard buffer.count > 0 else { return (makeIterator(), 0) }
    let ptr = UnsafeMutableRawPointer(buffer.baseAddress)?
      .assumingMemoryBound(to: AnyObject.self)
    let (_, c) = _nonNative._copyContents(
      initializing: UnsafeMutableBufferPointer(start: ptr, count: buffer.count))
    return (IndexingIterator(_elements: self, _position: c), c)
  }

  /// Returns a `_SliceBuffer` containing the given sub-range of elements in
  /// `bounds` from this buffer.
  @inlinable
  internal subscript(bounds: Range<Int>) -> _SliceBuffer<Element> {
    get {
      _typeCheck(bounds)
      if _fastPath(_isNative) {
        return _native[bounds]
      }
      return _nonNative[bounds].unsafeCastElements(to: Element.self)
    }
    set {
      fatalError("not implemented")
    }
  }

  /// A pointer to the first element.
  ///
  /// - Precondition: The elements are known to be stored contiguously.
  @inlinable
  internal var firstElementAddress: UnsafeMutablePointer<Element> {
    _internalInvariant(_isNative, "must be a native buffer")
    return _native.firstElementAddress
  }

  /// A mutable pointer to the first element.
  ///
  /// - Precondition: the buffer must be mutable.
  @_alwaysEmitIntoClient
  internal var mutableFirstElementAddress: UnsafeMutablePointer<Element> {
    _internalInvariant(_isNative, "must be a native buffer")
    return _native.mutableFirstElementAddress
  }

  @inlinable
  internal var firstElementAddressIfContiguous: UnsafeMutablePointer<Element>? {
    return _fastPath(_isNative) ? firstElementAddress : nil
  }

  /// The number of elements the buffer stores.
  ///
  /// This property is obsolete. It's only used for the ArrayBufferProtocol and
  /// to keep backward compatibility.
  /// Use `immutableCount` or `mutableCount` instead.
  @inlinable
  internal var count: Int {
    @inline(__always)
    get {
      return _fastPath(_isNative) ? _native.count : _nonNative.endIndex
    }
    set {
      _internalInvariant(_isNative, "attempting to update count of Cocoa array")
      _native.count = newValue
    }
  }
  
  /// The number of elements of the buffer.
  ///
  /// - Precondition: The buffer must be immutable.
  @_alwaysEmitIntoClient
  internal var immutableCount: Int {
    return _fastPath(_isNative) ? _native.immutableCount : _nonNative.endIndex
  }

  /// The number of elements of the buffer.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  internal var mutableCount: Int {
    @inline(__always)
    get {
      _internalInvariant(
        _isNative,
        "attempting to get mutating-count of non-native buffer")
      return _native.mutableCount
    }
    @inline(__always)
    set {
      _internalInvariant(_isNative, "attempting to update count of Cocoa array")
      _native.mutableCount = newValue
    }
  }

  /// Traps if an inout violation is detected or if the buffer is
  /// native and the subscript is out of range.
  ///
  /// wasNative == _isNative in the absence of inout violations.
  /// Because the optimizer can hoist the original check it might have
  /// been invalidated by illegal user code.
  ///
  /// This function is obsolete but must stay in the library for backward
  /// compatibility.
  @inlinable
  internal func _checkInoutAndNativeBounds(_ index: Int, wasNative: Bool) {
    _precondition(
      _isNative == wasNative,
      "inout rules were violated: the array was overwritten")

    if _fastPath(wasNative) {
      _native._checkValidSubscript(index)
    }
  }

  /// Traps if an inout violation is detected or if the buffer is
  /// native and typechecked and the subscript is out of range.
  ///
  /// wasNativeTypeChecked == _isNativeTypeChecked in the absence of
  /// inout violations.  Because the optimizer can hoist the original
  /// check it might have been invalidated by illegal user code.
  ///
  /// This function is obsolete but must stay in the library for backward
  /// compatibility.
  @inlinable
  internal func _checkInoutAndNativeTypeCheckedBounds(
    _ index: Int, wasNativeTypeChecked: Bool
  ) {
    _precondition(
      _isNativeTypeChecked == wasNativeTypeChecked,
      "inout rules were violated: the array was overwritten")

    if _fastPath(wasNativeTypeChecked) {
      _native._checkValidSubscript(index)
    }
  }

  /// Traps unless the given `index` is valid for subscripting, i.e.
  /// `0 ≤ index < count`.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  internal func _checkValidSubscriptMutating(_ index: Int) {
    _native._checkValidSubscriptMutating(index)
  }

  /// The number of elements the buffer can store without reallocation.
  ///
  /// This property is obsolete. It's only used for the ArrayBufferProtocol and
  /// to keep backward compatibility.
  /// Use `immutableCapacity` or `mutableCapacity` instead.
  @inlinable
  internal var capacity: Int {
    return _fastPath(_isNative) ? _native.capacity : _nonNative.endIndex
  }

  /// The number of elements the buffer can store without reallocation.
  ///
  /// - Precondition: The buffer must be immutable.
  @_alwaysEmitIntoClient
  internal var immutableCapacity: Int {
    return _fastPath(_isNative) ? _native.immutableCapacity : _nonNative.count
  }
  
  /// The number of elements the buffer can store without reallocation.
  ///
  /// - Precondition: The buffer must be mutable.
  @_alwaysEmitIntoClient
  internal var mutableCapacity: Int {
    _internalInvariant(_isNative, "attempting to get mutating-capacity of non-native buffer")
    return _native.mutableCapacity
  }

  @inlinable
  @inline(__always)
  internal func getElement(_ i: Int, wasNativeTypeChecked: Bool) -> Element {
    if _fastPath(wasNativeTypeChecked) {
      return _nativeTypeChecked[i]
    }
    return unsafeBitCast(_getElementSlowPath(i), to: Element.self)
  }

  @inline(never)
  @inlinable // @specializable
  @_effects(notEscaping self.value**)
  @_effects(escaping self.value**.class*.value** => return.value**)
  internal func _getElementSlowPath(_ i: Int) -> AnyObject {
    _internalInvariant(
      _isClassOrObjCExistential(Element.self),
      "Only single reference elements can be indexed here.")
    let element: AnyObject
    if _isNative {
      // _checkInoutAndNativeTypeCheckedBounds does no subscript
      // checking for the native un-typechecked case.  Therefore we
      // have to do it here.
      _native._checkValidSubscript(i)
      
      element = cast(toBufferOf: AnyObject.self)._native[i]
      guard element is Element else {
        _assertionFailure(
          "Fatal error",
          """
          Down-casted Array element failed to match the target type
          Expected \(Element.self) but found \(type(of: element))
          """,
          flags: _fatalErrorFlags()
        )
      }
    } else {
      // ObjC arrays do their own subscript checking.
      element = _nonNative[i]
      guard element is Element else {
        _assertionFailure(
          "Fatal error",
          """
          NSArray element failed to match the Swift Array Element type
          Expected \(Element.self) but found \(type(of: element))
          """,
          flags: _fatalErrorFlags()
        )
      }
    }
    return element
  }

  /// Get or set the value of the ith element.
  @inlinable
  internal subscript(i: Int) -> Element {
    get {
      return getElement(i, wasNativeTypeChecked: _isNativeTypeChecked)
    }
    
    nonmutating set {
      if _fastPath(_isNative) {
        _native[i] = newValue
      }
      else {
        var refCopy = self
        refCopy.replaceSubrange(
          i..<(i + 1),
          with: 1,
          elementsOf: CollectionOfOne(newValue))
      }
    }
  }

  /// Call `body(p)`, where `p` is an `UnsafeBufferPointer` over the
  /// underlying contiguous storage.  If no such storage exists, it is
  /// created on-demand.
  @inlinable
  internal func withUnsafeBufferPointer<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    if _fastPath(_isNative) {
      defer { _fixLifetime(self) }
      return try body(
        UnsafeBufferPointer(start: firstElementAddress, count: count))
    }
    return try ContiguousArray(self).withUnsafeBufferPointer(body)
  }
  
  /// Call `body(p)`, where `p` is an `UnsafeMutableBufferPointer`
  /// over the underlying contiguous storage.
  ///
  /// - Precondition: Such contiguous storage exists or the buffer is empty.
  @inlinable
  internal mutating func withUnsafeMutableBufferPointer<R>(
    _ body: (UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R {
    _internalInvariant(
      _isNative || count == 0,
      "Array is bridging an opaque NSArray; can't get a pointer to the elements"
    )
    defer { _fixLifetime(self) }
    return try body(UnsafeMutableBufferPointer(
      start: firstElementAddressIfContiguous, count: count))
  }
  
  /// An object that keeps the elements stored in this buffer alive.
  @inlinable
  internal var owner: AnyObject {
    return _fastPath(_isNative) ? _native._storage : _nonNative.buffer
  }
  
  /// An object that keeps the elements stored in this buffer alive.
  ///
  /// - Precondition: This buffer is backed by a `_ContiguousArrayBuffer`.
  @inlinable
  internal var nativeOwner: AnyObject {
    _internalInvariant(_isNative, "Expect a native array")
    return _native._storage
  }

  /// A value that identifies the storage used by the buffer.  Two
  /// buffers address the same elements when they have the same
  /// identity and count.
  @inlinable
  internal var identity: UnsafeRawPointer {
    if _isNative {
      return _native.identity
    }
    else {
      return UnsafeRawPointer(
        Unmanaged.passUnretained(_nonNative.buffer).toOpaque())
    }
  }
  
  //===--- Collection conformance -------------------------------------===//
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

  //===--- private --------------------------------------------------------===//
  internal typealias Storage = _ContiguousArrayStorage<Element>
  @usableFromInline
  internal typealias NativeBuffer = _ContiguousArrayBuffer<Element>

  @inlinable
  internal var _isNative: Bool {
    if !_isClassOrObjCExistential(Element.self) {
      return true
    } else {
      return _storage.isNative
    }
  }

  /// `true`, if the array is native and does not need a deferred type check.
  @inlinable
  internal var _isNativeTypeChecked: Bool {
    if !_isClassOrObjCExistential(Element.self) {
      return true
    } else {
      return _storage.isUnflaggedNative
    }
  }

  /// Our native representation.
  ///
  /// - Precondition: `_isNative`.
  @inlinable
  internal var _native: NativeBuffer {
    return NativeBuffer(
      _isClassOrObjCExistential(Element.self)
      ? _storage.nativeInstance : _storage.unflaggedNativeInstance)
  }

  /// Fast access to the native representation.
  ///
  /// - Precondition: `_isNativeTypeChecked`.
  @inlinable
  internal var _nativeTypeChecked: NativeBuffer {
    return NativeBuffer(_storage.unflaggedNativeInstance)
  }

  @inlinable
  internal var _nonNative: _CocoaArrayWrapper {
    get {
      _internalInvariant(_isClassOrObjCExistential(Element.self))
      return _CocoaArrayWrapper(_storage.objCInstance)
    }
  }
}

extension _ArrayBuffer: @unchecked Sendable
  where Element: Sendable { }
#endif
