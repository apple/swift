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

import SwiftShims

/// An instance of this class has all `Set` data tail-allocated.
/// Enough bytes are allocated to hold the bitmap for marking valid entries,
/// keys, and values. The data layout starts with the bitmap, followed by the
/// keys, followed by the values.
// NOTE: older runtimes called this class _RawSetStorage. The two
// must coexist without a conflicting ObjC class name, so it was
// renamed. The old name must not be used in the new runtime.
@_fixed_layout
@usableFromInline
@_objc_non_lazy_realization
internal class __RawSetStorage: __SwiftNativeNSSet {
  // NOTE: The precise layout of this type is relied on in the runtime to
  // provide a statically allocated empty singleton.  See
  // stdlib/public/stubs/GlobalObjects.cpp for details.

  /// The current number of occupied entries in this set.
  @usableFromInline
  @nonobjc
  internal final var _count: Int

  /// The maximum number of elements that can be inserted into this set without
  /// exceeding the hash table's maximum load factor.
  @usableFromInline
  @nonobjc
  internal final var _capacity: Int

  /// The scale of this set. The number of buckets is 2 raised to the
  /// power of `scale`.
  @usableFromInline
  @nonobjc
  internal final var _scale: Int8

  /// The scale corresponding to the highest `reserveCapacity(_:)` call so far,
  /// or 0 if there were none. This may be used later to allow removals to
  /// resize storage.
  ///
  /// FIXME: <rdar://problem/18114559> Shrink storage on deletion
  @usableFromInline
  @nonobjc
  internal final var _reservedScale: Int8

  // Currently unused, set to zero.
  @nonobjc
  internal final var _extra: Int16

  /// A mutation count, enabling stricter index validation.
  @usableFromInline
  @nonobjc
  internal final var _age: Int32

  /// The hash seed used to hash elements in this set instance.
  @usableFromInline
  internal final var _seed: Int

  /// A raw pointer to the start of the tail-allocated hash buffer holding set
  /// members.
  @usableFromInline
  @nonobjc
  internal final var _rawElements: UnsafeMutableRawPointer

  // This type is made with allocWithTailElems, so no init is ever called.
  // But we still need to have an init to satisfy the compiler.
  @nonobjc
  internal init(_doNotCallMe: ()) {
    _internalInvariantFailure("This class cannot be directly initialized")
  }

  @inlinable
  @nonobjc
  internal final var _bucketCount: Int {
    @inline(__always) get { return 1 &<< _scale }
  }

  @inlinable
  @nonobjc
  internal final var _metadata: UnsafeMutablePointer<_HashTable.Word> {
    @inline(__always) get {
      let address = Builtin.projectTailElems(self, _HashTable.Word.self)
      return UnsafeMutablePointer(address)
    }
  }

  // The _HashTable struct contains pointers into tail-allocated storage, so
  // this is unsafe and needs `_fixLifetime` calls in the caller.
  @inlinable
  @nonobjc
  internal final var _hashTable: _HashTable {
    @inline(__always) get {
      return _HashTable(words: _metadata, bucketCount: _bucketCount)
    }
  }
}

/// The storage class for the singleton empty set.
/// The single instance of this class is created by the runtime.
// NOTE: older runtimes called this class _EmptySetSingleton. The two
// must coexist without conflicting ObjC class names, so it was renamed.
// The old names must not be used in the new runtime.
@_fixed_layout
@usableFromInline
@_objc_non_lazy_realization
internal class __EmptySetSingleton: __RawSetStorage {
  @nonobjc
  override internal init(_doNotCallMe: ()) {
    _internalInvariantFailure("This class cannot be directly initialized")
  }

#if _runtime(_ObjC)
  @objc
  internal required init(objects: UnsafePointer<AnyObject?>, count: Int) {
    _internalInvariantFailure("This class cannot be directly initialized")
  }
#endif
}

#if $Embedded
// In embedded Swift, the stdlib is a .swiftmodule only without any .o/.a files,
// to allow consuming it by clients with different LLVM codegen setting (-mcpu
// flags, etc.), which means we cannot declare the singleton in a C/C++ file.
//
// TODO: We should figure out how to make this a constant so that it's placed in
// non-writable memory (can't be a let, Builtin.addressof below requires a var).
public var _swiftEmptySetSingleton: (Int, Int, Int, Int, UInt8, UInt8, UInt16, UInt32, Int, Int, Int) =
    (
      /*isa*/0, /*refcount*/-1, // HeapObject header
      /*count*/0, 
      /*capacity*/0, 
      /*scale*/0, 
      /*reservedScale*/0, 
      /*extra*/0, 
      /*age*/0, 
      /*seed*/0, 
      /*rawElements*/1, 
      /*metadata*/~1
    )
#endif

extension __RawSetStorage {
  /// The empty singleton that is used for every single Set that is created
  /// without any elements. The contents of the storage must never be mutated.
  @inlinable
  @nonobjc
  internal static var empty: __EmptySetSingleton {
    Builtin.bridgeFromRawPointer(_swift_stdlib_getEmptySetSingleton()._rawValue)
  }
}

extension __EmptySetSingleton: _NSSetCore {
#if _runtime(_ObjC)
  //
  // NSSet implementation, assuming Self is the empty singleton
  //
  @objc(copyWithZone:)
  internal func copy(with zone: _SwiftNSZone?) -> AnyObject {
    return self
  }

  @objc
  internal var count: Int {
    return 0
  }

  @objc(member:)
  internal func member(_ object: AnyObject) -> AnyObject? {
    return nil
  }

  @objc
  internal func objectEnumerator() -> _NSEnumerator {
    return __SwiftEmptyNSEnumerator()
  }

  @objc(countByEnumeratingWithState:objects:count:)
  internal func countByEnumerating(
    with state: UnsafeMutablePointer<_SwiftNSFastEnumerationState>,
    objects: UnsafeMutablePointer<AnyObject>?, count: Int
  ) -> Int {
    // Even though we never do anything in here, we need to update the
    // state so that callers know we actually ran.
    var theState = state.pointee
    if theState.state == 0 {
      theState.state = 1 // Arbitrary non-zero value.
      theState.itemsPtr = AutoreleasingUnsafeMutablePointer(objects)
      theState.mutationsPtr = _fastEnumerationStorageMutationsPtr
    }
    state.pointee = theState
    return 0
  }
#endif
}

@usableFromInline
final internal class _SetStorage<Element: Hashable>
  : __RawSetStorage, _NSSetCore {
  // This type is made with allocWithTailElems, so no init is ever called.
  // But we still need to have an init to satisfy the compiler.
  @nonobjc
  override internal init(_doNotCallMe: ()) {
    _internalInvariantFailure("This class cannot be directly initialized")
  }

  deinit {
    guard _count > 0 else { return }
    if !_isPOD(Element.self) {
      let elements = _elements
      for bucket in _hashTable {
        (elements + bucket.offset).deinitialize(count: 1)
      }
    }
    _fixLifetime(self)
  }

  @inlinable
  final internal var _elements: UnsafeMutablePointer<Element> {
    @inline(__always)
    get {
      return self._rawElements.assumingMemoryBound(to: Element.self)
    }
  }

  internal var asNative: _NativeSet<Element> {
    return _NativeSet(self)
  }

#if _runtime(_ObjC)
  @objc
  internal required init(objects: UnsafePointer<AnyObject?>, count: Int) {
    _internalInvariantFailure("don't call this designated initializer")
  }

  @objc(copyWithZone:)
  internal func copy(with zone: _SwiftNSZone?) -> AnyObject {
    return self
  }

  @objc
  internal var count: Int {
    return _count
  }

  @objc
  internal func objectEnumerator() -> _NSEnumerator {
    return _SwiftSetNSEnumerator<Element>(asNative)
  }

  @objc(countByEnumeratingWithState:objects:count:)
  internal func countByEnumerating(
    with state: UnsafeMutablePointer<_SwiftNSFastEnumerationState>,
    objects: UnsafeMutablePointer<AnyObject>?, count: Int
  ) -> Int {
    defer { _fixLifetime(self) }
    let hashTable = _hashTable
    var theState = state.pointee
    if theState.state == 0 {
      theState.state = 1 // Arbitrary non-zero value.
      theState.itemsPtr = AutoreleasingUnsafeMutablePointer(objects)
      theState.mutationsPtr = _fastEnumerationStorageMutationsPtr
      theState.extra.0 = CUnsignedLong(hashTable.startBucket.offset)
    }

    // Test 'objects' rather than 'count' because (a) this is very rare anyway,
    // and (b) the optimizer should then be able to optimize away the
    // unwrapping check below.
    if _slowPath(objects == nil) {
      return 0
    }

    let unmanagedObjects = _UnmanagedAnyObjectArray(objects!)
    var bucket = _HashTable.Bucket(offset: Int(theState.extra.0))
    let endBucket = hashTable.endBucket
    _precondition(bucket == endBucket || hashTable.isOccupied(bucket),
      "Invalid fast enumeration state")
    var stored = 0
    for i in 0..<count {
      if bucket == endBucket { break }
      let element = _elements[bucket.offset]
      unmanagedObjects[i] = _bridgeAnythingToObjectiveC(element)
      stored += 1
      bucket = hashTable.occupiedBucket(after: bucket)
    }
    theState.extra.0 = CUnsignedLong(bucket.offset)
    state.pointee = theState
    return stored
  }

  @objc(member:)
  internal func member(_ object: AnyObject) -> AnyObject? {
    guard let native = _conditionallyBridgeFromObjectiveC(object, Element.self)
    else { return nil }

    let (bucket, found) = asNative.find(native)
    guard found else { return nil }
    return _bridgeAnythingToObjectiveC(_elements[bucket.offset])
  }
#endif
}

extension _SetStorage {
  @usableFromInline
  @_effects(releasenone)
  internal static func copy(original: __RawSetStorage) -> _SetStorage {
    return .allocate(
      scale: original._scale,
      age: original._age,
      seed: original._seed)
  }

  @usableFromInline
  @_effects(releasenone)
  static internal func resize(
    original: __RawSetStorage,
    capacity: Int,
    move: Bool
  ) -> _SetStorage {
    let scale = _HashTable.scale(forCapacity: capacity)
    return allocate(scale: scale, age: nil, seed: nil)
  }

  @usableFromInline
  @_effects(releasenone)
  static internal func allocate(capacity: Int) -> _SetStorage {
    let scale = _HashTable.scale(forCapacity: capacity)
    return allocate(scale: scale, age: nil, seed: nil)
  }

#if _runtime(_ObjC)
  @usableFromInline
  @_effects(releasenone)
  static internal func convert(
    _ cocoa: __CocoaSet,
    capacity: Int
  ) -> _SetStorage {
    let scale = _HashTable.scale(forCapacity: capacity)
    let age = _HashTable.age(for: cocoa.object)
    return allocate(scale: scale, age: age, seed: nil)
  }
#endif

  static internal func allocate(
    scale: Int8,
    age: Int32?,
    seed: Int?
  ) -> _SetStorage {
    // The entry count must be representable by an Int value; hence the scale's
    // peculiar upper bound.
    _internalInvariant(scale >= 0 && scale < Int.bitWidth - 1)

    let bucketCount = (1 as Int) &<< scale
    let wordCount = _UnsafeBitset.wordCount(forCapacity: bucketCount)
    let storage = Builtin.allocWithTailElems_2(
      _SetStorage<Element>.self,
      wordCount._builtinWordValue, _HashTable.Word.self,
      bucketCount._builtinWordValue, Element.self)

    let metadataAddr = Builtin.projectTailElems(storage, _HashTable.Word.self)
    let elementsAddr = Builtin.getTailAddr_Word(
      metadataAddr, wordCount._builtinWordValue, _HashTable.Word.self,
      Element.self)
    storage._count = 0
    storage._capacity = _HashTable.capacity(forScale: scale)
    storage._scale = scale
    storage._reservedScale = 0
    storage._extra = 0

    if let age = age {
      storage._age = age
    } else {
      // The default mutation count is simply a scrambled version of the storage
      // address.
      storage._age = Int32(
        truncatingIfNeeded: ObjectIdentifier(storage).hashValue)
    }

    storage._seed = seed ?? _HashTable.hashSeed(for: Builtin.castToNativeObject(storage), scale: scale)
    storage._rawElements = UnsafeMutableRawPointer(elementsAddr)

    // Initialize hash table metadata.
    storage._hashTable.clear()
    return storage
  }
}
