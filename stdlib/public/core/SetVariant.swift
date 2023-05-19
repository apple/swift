//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// This protocol is only used for compile-time checks that
/// every buffer type implements all required operations.
internal protocol _SetBuffer {
  associatedtype Element
  associatedtype Index

  var startIndex: Index { get }
  var endIndex: Index { get }
  func index(after i: Index) -> Index
  func index(for element: Element) -> Index?
  var count: Int { get }

  func contains(_ member: Element) -> Bool
  func element(at i: Index) -> Element
}

extension Set {
  @usableFromInline
  @frozen
  internal struct _Variant {
    @usableFromInline
    internal var object: _BridgeStorage<__RawSetStorage>

    @inlinable
    @inline(__always)
    init(dummy: ()) {
#if _pointerBitWidth(_64)
      self.object = _BridgeStorage(taggedPayload: 0)
#elseif _pointerBitWidth(_32)
      self.init(native: _NativeSet())
#else
#error("Unknown platform")
#endif
    }

    @inlinable
    @inline(__always)
    init(native: __owned _NativeSet<Element>) {
      self.object = _BridgeStorage(native: native._storage)
    }

#if _runtime(_ObjC)
    @inlinable
    @inline(__always)
    init(cocoa: __owned __CocoaSet) {
      self.object = _BridgeStorage(objC: cocoa.object)
    }
#endif
  }
}

extension Set._Variant {
#if _runtime(_ObjC)
  @usableFromInline
  @_transparent
  internal var guaranteedNative: Bool {
    return _canBeClass(Element.self) == 0
  }
#endif

  @inlinable
  internal mutating func isUniquelyReferenced() -> Bool {
    return object.isUniquelyReferencedUnflaggedNative()
  }

#if _runtime(_ObjC)
  @usableFromInline @_transparent
  internal var isNative: Bool {
    if guaranteedNative { return true }
    return object.isUnflaggedNative
  }
#endif

  @usableFromInline @_transparent
  internal var asNative: _NativeSet<Element> {
    get {
      return _NativeSet(object.unflaggedNativeInstance)
    }
    set {
      self = .init(native: newValue)
    }
    _modify {
      var native = _NativeSet<Element>(object.unflaggedNativeInstance)
      self = .init(dummy: ())
      defer {
        // This is in a defer block because yield might throw, and we need to
        // preserve Set's storage invariants when that happens.
        object = .init(native: native._storage)
      }
      yield &native
    }
  }

#if _runtime(_ObjC)
  @inlinable
  internal var asCocoa: __CocoaSet {
    return __CocoaSet(object.objCInstance)
  }
#endif

  @_alwaysEmitIntoClient
  internal var convertedToNative: _NativeSet<Element> {
#if _runtime(_ObjC)
    guard isNative else {  return _NativeSet<Element>(asCocoa) }
#endif
    return asNative
  }

  /// Reserves enough space for the specified number of elements to be stored
  /// without reallocating additional storage.
  internal mutating func reserveCapacity(_ capacity: Int) {
#if _runtime(_ObjC)
    guard isNative else {
      let cocoa = asCocoa
      let capacity = Swift.max(cocoa.count, capacity)
      self = .init(native: _NativeSet(cocoa, capacity: capacity))
      return
    }
#endif
    let isUnique = isUniquelyReferenced()
    asNative.reserveCapacity(capacity, isUnique: isUnique)
  }

  /// The number of elements that can be stored without expanding the current
  /// storage.
  ///
  /// For bridged storage, this is equal to the current count of the
  /// collection, since any addition will trigger a copy of the elements into
  /// newly allocated storage. For native storage, this is the element count
  /// at which adding any more elements will exceed the load factor.
  @inlinable
  internal var capacity: Int {
#if _runtime(_ObjC)
    guard isNative else {
      return asCocoa.count
    }
#endif
    return asNative.capacity
  }
}

extension Set._Variant: _SetBuffer {
  @usableFromInline
  internal typealias Index = Set<Element>.Index

  @inlinable
  internal var startIndex: Index {
#if _runtime(_ObjC)
    guard isNative else {
      return Index(_cocoa: asCocoa.startIndex)
    }
#endif
    return asNative.startIndex
  }

  @inlinable
  internal var endIndex: Index {
#if _runtime(_ObjC)
    guard isNative else {
      return Index(_cocoa: asCocoa.endIndex)
    }
#endif
    return asNative.endIndex
  }

  @inlinable
  internal func index(after index: Index) -> Index {
#if _runtime(_ObjC)
    guard isNative else {
      return Index(_cocoa: asCocoa.index(after: index._asCocoa))
    }
#endif
    return asNative.index(after: index)
  }

  @inlinable
  internal func formIndex(after index: inout Index) {
#if _runtime(_ObjC)
    guard isNative else {
      let isUnique = index._isUniquelyReferenced()
      asCocoa.formIndex(after: &index._asCocoa, isUnique: isUnique)
      return
    }
#endif
    index = asNative.index(after: index)
  }

  @inlinable
  @inline(__always)
  internal func index(for element: Element) -> Index? {
#if _runtime(_ObjC)
    guard isNative else {
      let cocoaElement = _bridgeAnythingToObjectiveC(element)
      guard let index = asCocoa.index(for: cocoaElement) else { return nil }
      return Index(_cocoa: index)
    }
#endif
    return asNative.index(for: element)
  }

  @inlinable
  internal var count: Int {
    @inline(__always)
    get {
#if _runtime(_ObjC)
      guard isNative else {
        return asCocoa.count
      }
#endif
      return asNative.count
    }
  }

  @inlinable
  @inline(__always)
  internal func contains(_ member: Element) -> Bool {
#if _runtime(_ObjC)
    guard isNative else {
      return asCocoa.contains(_bridgeAnythingToObjectiveC(member))
    }
#endif
    return asNative.contains(member)
  }

  @inlinable
  @inline(__always)
  internal func element(at index: Index) -> Element {
#if _runtime(_ObjC)
    guard isNative else {
      let cocoaMember = asCocoa.element(at: index._asCocoa)
      return _forceBridgeFromObjectiveC(cocoaMember, Element.self)
    }
#endif
    return asNative.element(at: index)
  }
}

extension Set._Variant {
  @inlinable
  internal mutating func update(with value: __owned Element) -> Element? {
#if _runtime(_ObjC)
    guard isNative else {
      // Make sure we have space for an extra element.
      var native = _NativeSet<Element>(asCocoa, capacity: asCocoa.count + 1)
      let old = native.update(with: value, isUnique: true)
      self = .init(native: native)
      return old
    }
#endif
    let isUnique = self.isUniquelyReferenced()
    return asNative.update(with: value, isUnique: isUnique)
  }

  @inlinable
  internal mutating func insert(
    _ element: __owned Element
  ) -> (inserted: Bool, memberAfterInsert: Element) {
#if _runtime(_ObjC)
    guard isNative else {
      // Make sure we have space for an extra element.
      let cocoaMember = _bridgeAnythingToObjectiveC(element)
      let cocoa = asCocoa
      if let m = cocoa.member(for: cocoaMember) {
        return (false, _forceBridgeFromObjectiveC(m, Element.self))
      }
      var native = _NativeSet<Element>(cocoa, capacity: cocoa.count + 1)
      native.insertNew(element, isUnique: true)
      self = .init(native: native)
      return (true, element)
    }
#endif
    let (bucket, found) = asNative.find(element)
    if found {
      return (false, asNative.uncheckedElement(at: bucket))
    }
    let isUnique = self.isUniquelyReferenced()
    asNative.insertNew(element, at: bucket, isUnique: isUnique)
    return (true, element)
  }

  @inlinable
  @discardableResult
  internal mutating func remove(at index: Index) -> Element {
#if _runtime(_ObjC)
    guard isNative else {
      // We have to migrate the data first.  But after we do so, the Cocoa
      // index becomes useless, so get the element first.
      let cocoa = asCocoa
      let cocoaMember = cocoa.member(for: index._asCocoa)
      let nativeMember = _forceBridgeFromObjectiveC(cocoaMember, Element.self)
      return _migrateToNative(cocoa, removing: nativeMember)
    }
#endif
    let isUnique = isUniquelyReferenced()
    let bucket = asNative.validatedBucket(for: index)
    return asNative.uncheckedRemove(at: bucket, isUnique: isUnique)
  }

  @inlinable
  @discardableResult
  internal mutating func remove(_ member: Element) -> Element? {
#if _runtime(_ObjC)
    guard isNative else {
      let cocoa = asCocoa
      let cocoaMember = _bridgeAnythingToObjectiveC(member)
      guard cocoa.contains(cocoaMember) else { return nil }
      return _migrateToNative(cocoa, removing: member)
    }
#endif
    let (bucket, found) = asNative.find(member)
    guard found else { return nil }
    let isUnique = isUniquelyReferenced()
    return asNative.uncheckedRemove(at: bucket, isUnique: isUnique)
  }

#if _runtime(_ObjC)
  @inlinable
  internal mutating func _migrateToNative(
    _ cocoa: __CocoaSet,
    removing member: Element
  ) -> Element {
    // FIXME(performance): fuse data migration and element deletion into one
    // operation.
    var native = _NativeSet<Element>(cocoa)
    let (bucket, found) = native.find(member)
    _precondition(found, "Bridging did not preserve equality")
    let old = native.uncheckedRemove(at: bucket, isUnique: true)
    _precondition(member == old, "Bridging did not preserve equality")
    self = .init(native: native)
    return old
  }
#endif

  @inlinable
  internal mutating func removeAll(keepingCapacity keepCapacity: Bool) {
    if !keepCapacity {
      self = .init(native: _NativeSet<Element>())
      return
    }
    guard count > 0 else { return }

#if _runtime(_ObjC)
    guard isNative else {
      self = .init(native: _NativeSet(capacity: asCocoa.count))
      return
    }
#endif
    let isUnique = isUniquelyReferenced()
    asNative.removeAll(isUnique: isUnique)
  }
}

extension Set._Variant {
  /// Returns an iterator over the elements.
  ///
  /// - Complexity: O(1).
  @inlinable
  @inline(__always)
  internal __consuming func makeIterator() -> Set<Element>.Iterator {
#if _runtime(_ObjC)
    guard isNative else {
      return Set.Iterator(_cocoa: asCocoa.makeIterator())
    }
#endif
    return Set.Iterator(_native: asNative.makeIterator())
  }
}

extension Set._Variant {
  @_alwaysEmitIntoClient
  internal __consuming func filter(
    _ isIncluded: (Element) throws -> Bool
  ) rethrows -> _NativeSet<Element> {
#if _runtime(_ObjC)
    guard isNative else {
      var result = _NativeSet<Element>()
      for cocoaElement in asCocoa {
        let nativeElement = _forceBridgeFromObjectiveC(
          cocoaElement, Element.self)
        if try isIncluded(nativeElement) {
          result.insertNew(nativeElement, isUnique: true)
        }
      }
      return result
    }
#endif
    return try asNative.filter(isIncluded)
  }

  @_alwaysEmitIntoClient
  internal __consuming func intersection(
    _ other: Set<Element>
  ) -> _NativeSet<Element> {
#if _runtime(_ObjC)
    switch (self.isNative, other._variant.isNative) {
    case (true, true):
      return asNative.intersection(other._variant.asNative)
    case (true, false):
      return asNative.genericIntersection(other)
    case (false, false):
      return _NativeSet(asCocoa).genericIntersection(other)
    case (false, true):
      // Note: It is tempting to implement this as `that.intersection(this)`,
      // but intersection isn't symmetric -- the result should only contain
      // elements from `self`.
      let that = other._variant.asNative
      var result = _NativeSet<Element>()
      for cocoaElement in asCocoa {
        let nativeElement = _forceBridgeFromObjectiveC(
          cocoaElement, Element.self)
        if that.contains(nativeElement) {
          result.insertNew(nativeElement, isUnique: true)
        }
      }
      return result
    }
#else
    return asNative.intersection(other._variant.asNative)
#endif
  }
}
