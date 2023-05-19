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

// Having @objc stuff in an extension creates an ObjC category, which we don't
// want.
#if _runtime(_ObjC)

internal protocol _AbstractStringStorage: _NSCopying {
  var asString: String { get }
  var count: Int { get }
  var isASCII: Bool { get }
  var start: UnsafePointer<UInt8> { get }
  var UTF16Length: Int { get }
}

#else

internal protocol _AbstractStringStorage {
  var asString: String { get }
  var count: Int { get }
  var isASCII: Bool { get }
  var start: UnsafePointer<UInt8> { get }
}

#endif

private typealias _CountAndFlags = _StringObject.CountAndFlags

/*

_CapacityAndFlags has the following layout. It is stored directly in
___StringStorage on 64-bit systems. On 32-bit systems, a 32-bit count and 16-bit
_flags are stored separately (for more efficient layout), and this is
_materialized as needed.

┌────────────────┬────────┬───────┐
│ b63            │ b62:48 │ b47:0 │
├────────────────┼────────┼───────┤
│ hasBreadcrumbs │ TBD    │ count │
└────────────────┴────────┴───────┘

*/
fileprivate struct _CapacityAndFlags {
  // Stores the "real capacity" (capacity + 1 for null terminator)
  // in the bottom 48 bits, and flags in the top 16.
  fileprivate var _storage: UInt64

#if _pointerBitWidth(_32)
  fileprivate init(realCapacity: Int, flags: UInt16) {
    let realCapUInt = UInt64(UInt(bitPattern: realCapacity))
    _internalInvariant(realCapUInt == realCapUInt & _CountAndFlags.countMask)
    self._storage = (UInt64(flags) &<< 48) | realCapUInt

    _internalInvariant(self.flags == flags)
  }

  fileprivate var flags: UInt16 {
    return UInt16(
      truncatingIfNeeded: (self._storage & _CountAndFlags.flagsMask) &>> 48)
  }
#endif

  internal init(hasBreadcrumbs crumbs: Bool, realCapacity: Int) {
    let realCapUInt = UInt64(UInt(bitPattern: realCapacity))
    _internalInvariant(realCapUInt == realCapUInt & _CountAndFlags.countMask)

    let crumbsFlag = crumbs ? _CapacityAndFlags.hasBreadcrumbsMask : 0
    self._storage = crumbsFlag | realCapUInt

    _internalInvariant(
      crumbs == self.hasBreadcrumbs && realCapacity == self._realCapacity)
  }

  // The capacity of our allocation. Note that this includes the nul-terminator,
  // which is not available for overriding.
  internal var _realCapacity: Int {
    Int(truncatingIfNeeded: self._storage & _CountAndFlags.countMask)
  }

  private static var hasBreadcrumbsMask: UInt64 { 0x8000_0000_0000_0000 }

  // Code unit capacity (excluding null terminator)
  fileprivate var capacity: Int { _realCapacity &- 1 }

  fileprivate var hasBreadcrumbs: Bool {
    (self._storage & _CapacityAndFlags.hasBreadcrumbsMask) != 0
  }
}

/*

 String's storage class has a header, which includes the isa pointer, reference
 count, and stored properties (count and capacity). After the header is a tail
 allocation for the UTF-8 code units, a null terminator, and some spare capacity
 (if available). After that, it optionally contains another tail allocation for
 the breadcrumbs pointer.

 If the requested code unit capacity is less than the breadcrumbs stride, no
 pointer is allocated. This has the effect of either allowing us to save space
 with a smaller allocation, or claim additional excess capacity, depending on
 which half of the malloc bucket the requested capacity lies within.


 Class Header: Below is the 64-bit and then 32-bit class header for
 __StringStorage. `B` denotes the byte number (starting at 0)

 0                                                                            32
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Class Header (64 bit)                                                       │
 ├─────────────┬─────────────────┬────────────────────┬────────────────────────┤
 │ B0 ..< B8   │ B8 ..< B16      │ B16 ..< B24        │ B24 ..< B32            │
 ├─────────────┼─────────────────┼────────────────────┼────────────────────────┤
 │ isa pointer │ reference count │ capacity and flags │ count and flags        │
 └─────────────┴─────────────────┴────────────────────┴────────────────────────┘

 0                                                                            20
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Class Header (32 bit)                                                       │
 ├─────────┬───────────┬────────────┬─────────────┬─────────────┬──────────────┤
 │ B0..<B4 │ B4 ..< B8 │ B8 ..< B12 │ B12 ..< B16 │ B16 ..< B18 │ B18 ..< B20  │
 ├─────────┼───────────┼────────────┼─────────────┼─────────────┼──────────────┤
 │ isa     │ ref count │ capacity   │ count       │ countFlags  │ capacityFlags│
 └─────────┴───────────┴────────────┴─────────────┴─────────────┴──────────────┘

 Tail Allocations:

 `__StringStorage.create` takes a requested minimum code unit capacity and a
 count (which it uses to guarantee null-termination invariant). This will
 allocate the class header and a tail allocation for the requested capacity plus
 one (for the null-terminator), plus any excess `malloc` bucket space. If
 breadcrumbs need be present, they appear at the very end of the allocation.

 For small allocations, `__StringStorage.create` will round up the total
 allocation to a malloc bucket estimate (16 bytes) and claim any excess capacity
 as additional code unit capacity. For large allocations, `malloc_size` is
 consulted and any excess capacity is likewise claimed as additional code unit
 capacity.

 H                                                                             n
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Tail allocation, no breadcrumbs pointer                                     │
 ├───────────────────┬────────┬────────────────────────────────────────────────┤
 │ B<H> ..< B<H+c>   │ B<H+c> │ B<H+c+1> ..< B<n>                              │
 ├───────────────────┼────────┼────────────────────────────────────────────────┤
 │ code unit count   │ null   │ spare code unit capacity                       │
 └───────────────────┴────────┴────────────────────────────────────────────────┘

 H                                                                             n
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Tail allocations, with breadcrumbs pointer                                  │
 ├───────────────────┬────────┬──────────────────────────┬─────────────────────┤
 │ B<H> ..< B<H+c>   │ B<H+c> │ B<H+c+1> ..< B<n-P>      │ B<n-P> ..< B<n>     │
 ├───────────────────┼────────┼──────────────────────────┼─────────────────────┤
 │ code unit count   │ null   │ spare code unit capacity │ breadcrumbs pointer │
 └───────────────────┴────────┴──────────────────────────┴─────────────────────┘

 where:
  * `H` is the class header size (32/20 on 64/32 bit, constant)
  * `n` is the total allocation size (estimate or `malloc_size`)
  * `c` is the given count
  * `P` is the size of the breadcrumbs pointer (8/4 on 64/32 bit, constant)

*/

// TODO: Migrate this to somewhere it can be shared with Array
import SwiftShims
fileprivate func _allocate<T: AnyObject>(
  numHeaderBytes: Int,        // The size of the class header
  numTailBytes: Int,          // The desired number of tail bytes
  growthFactor: Float? = nil, // Exponential growth factor for large allocs
  tailAllocator: (_ numTailBytes: Int) -> T // Do the actual tail allocation
) -> (T, realNumTailBytes: Int) {
  _internalInvariant(getSwiftClassInstanceExtents(T.self).1 == numHeaderBytes)

  func roundUp(_ x: Int) -> Int { (x + 15) & ~15 }

  let numBytes = numHeaderBytes + numTailBytes

  let linearBucketThreshold = 128
  if _fastPath(numBytes < linearBucketThreshold) {
    // Allocate up to the nearest bucket of 16
    let realNumBytes = roundUp(numBytes)
    let realNumTailBytes = realNumBytes - numHeaderBytes
    _internalInvariant(realNumTailBytes >= numTailBytes)
    let object = tailAllocator(realNumTailBytes)
    return (object, realNumTailBytes)
  }

  let growTailBytes: Int
  if let growth = growthFactor {
    growTailBytes = Swift.max(numTailBytes, Int(Float(numTailBytes) * growth))
  } else {
    growTailBytes = numTailBytes
  }

  let total = roundUp(numHeaderBytes + growTailBytes)
  let totalTailBytes = total - numHeaderBytes

  let object = tailAllocator(totalTailBytes)
  if let allocSize = _mallocSize(ofAllocation:
    UnsafeRawPointer(Builtin.bridgeToRawPointer(object))) {
    _internalInvariant(allocSize % MemoryLayout<Int>.stride == 0)

    let realNumTailBytes = allocSize - numHeaderBytes
    _internalInvariant(realNumTailBytes >= numTailBytes)

    return (object, realNumTailBytes)
  } else {
    return (object, totalTailBytes)
  }
}

fileprivate func _allocateStringStorage(
  codeUnitCapacity capacity: Int
) -> (__StringStorage, _CapacityAndFlags) {
  let pointerSize = MemoryLayout<Int>.stride
  let headerSize = Int(_StringObject.nativeBias)
  let codeUnitSize = capacity + 1 /* code units and null */
  let needBreadcrumbs = capacity >= _StringBreadcrumbs.breadcrumbStride
  let breadcrumbSize = needBreadcrumbs ? pointerSize : 0

  let (storage, numTailBytes) = _allocate(
    numHeaderBytes: headerSize,
    numTailBytes: codeUnitSize + breadcrumbSize
  ) { tailBytes in
      Builtin.allocWithTailElems_1(
        __StringStorage.self, tailBytes._builtinWordValue, UInt8.self)
  }

  let capAndFlags = _CapacityAndFlags(
    hasBreadcrumbs: needBreadcrumbs,
    realCapacity: numTailBytes - breadcrumbSize)

  _internalInvariant(numTailBytes >= codeUnitSize + breadcrumbSize)
  return (storage, capAndFlags)
}

// NOTE: older runtimes called this class _StringStorage. The two
// must coexist without conflicting ObjC class names, so it was
// renamed. The old name must not be used in the new runtime.
final internal class __StringStorage
  : __SwiftNativeNSString, _AbstractStringStorage {
#if _pointerBitWidth(_64)
  fileprivate var _capacityAndFlags: _CapacityAndFlags
  internal var _countAndFlags: _StringObject.CountAndFlags

  @inline(__always)
  internal var count: Int { _countAndFlags.count }
#elseif _pointerBitWidth(_32)
  // The total allocated storage capacity. Note that this includes the required
  // nul-terminator.
  private var _realCapacity: Int
  private var _count: Int
  private var _countFlags: UInt16
  private var _capacityFlags: UInt16

  @inline(__always)
  internal var count: Int { _count }

  @inline(__always)
  internal var _countAndFlags: _StringObject.CountAndFlags {
    _CountAndFlags(count: _count, flags: _countFlags)
  }

  @inline(__always)
  fileprivate var _capacityAndFlags: _CapacityAndFlags {
    _CapacityAndFlags(realCapacity: _realCapacity, flags: _capacityFlags)
  }
#else
#error("Unknown platform")
#endif

  @inline(__always)
  final internal var isASCII: Bool { _countAndFlags.isASCII }

  final internal var asString: String {
    @_effects(readonly) @inline(__always)
    get { String(_StringGuts(self)) }
  }


  private init(_doNotCallMe: ()) {
    _internalInvariantFailure("Use the create method")
  }

  deinit {
    if hasBreadcrumbs {
      _breadcrumbsAddress.deinitialize(count: 1)
    }
  }
}

// Creation
extension __StringStorage {
  @_effects(releasenone)
  private static func create(
    codeUnitCapacity capacity: Int, countAndFlags: _CountAndFlags
  ) -> __StringStorage {
    _internalInvariant(capacity >= countAndFlags.count)
    _internalInvariant(
      countAndFlags.isNativelyStored && countAndFlags.isTailAllocated)

    let (storage, capAndFlags) = _allocateStringStorage(
      codeUnitCapacity: capacity)

    _internalInvariant(capAndFlags.capacity >= capacity)

#if _pointerBitWidth(_64)
    storage._capacityAndFlags = capAndFlags
    storage._countAndFlags = countAndFlags
#elseif _pointerBitWidth(_32)
    storage._realCapacity = capAndFlags._realCapacity
    storage._count = countAndFlags.count
    storage._countFlags = countAndFlags.flags
    storage._capacityFlags = capAndFlags.flags
#else
#error("Unknown platform")
#endif

    _internalInvariant(
      storage._countAndFlags._storage == countAndFlags._storage)
    _internalInvariant(
      storage._capacityAndFlags._storage == capAndFlags._storage)
    _internalInvariant(
      storage.unusedCapacity == capAndFlags.capacity - countAndFlags.count)

    if storage.hasBreadcrumbs {
      storage._breadcrumbsAddress.initialize(to: nil)
    }

    storage.terminator.pointee = 0 // nul-terminated

    // We can check layout invariants, but our code units have not yet been
    // initialized so we can't verify e.g. ASCII-ness
    storage._invariantCheck(initialized: false)
    return storage
  }

  // The caller is expected to check UTF8 validity and ASCII-ness and update
  // the resulting StringStorage accordingly
  internal static func create(
    uninitializedCodeUnitCapacity capacity: Int,
    initializingUncheckedUTF8With initializer: (
      _ buffer: UnsafeMutableBufferPointer<UInt8>
    ) throws -> Int
  ) rethrows -> __StringStorage {
    let storage = __StringStorage.create(
      codeUnitCapacity: capacity,
      countAndFlags: _CountAndFlags(mortalCount: 0, isASCII: false)
    )
    let buffer = UnsafeMutableBufferPointer(start: storage.mutableStart,
                                            count: capacity)
    let count = try initializer(buffer)

    let countAndFlags = _CountAndFlags(mortalCount: count, isASCII: false)
    #if _pointerBitWidth(_64)
    storage._countAndFlags = countAndFlags
    #elseif _pointerBitWidth(_32)
    storage._count = countAndFlags.count
    storage._countFlags = countAndFlags.flags
    #else
    #error("Unknown platform")
    #endif

    storage.terminator.pointee = 0 // nul-terminated
    return storage
  }

  @_effects(releasenone)
  internal static func create(
    initializingFrom bufPtr: UnsafeBufferPointer<UInt8>,
    codeUnitCapacity capacity: Int,
    isASCII: Bool
  ) -> __StringStorage {
    let countAndFlags = _CountAndFlags(
      mortalCount: bufPtr.count, isASCII: isASCII)
    _internalInvariant(capacity >= bufPtr.count)
    let storage = __StringStorage.create(
      codeUnitCapacity: capacity, countAndFlags: countAndFlags)
    let addr = bufPtr.baseAddress._unsafelyUnwrappedUnchecked
    storage.mutableStart.initialize(from: addr, count: bufPtr.count)
    storage._invariantCheck()
    return storage
  }

  @_effects(releasenone)
  internal static func create(
    initializingFrom bufPtr: UnsafeBufferPointer<UInt8>, isASCII: Bool
  ) -> __StringStorage {
    __StringStorage.create(
      initializingFrom: bufPtr,
      codeUnitCapacity: bufPtr.count,
      isASCII: isASCII)
  }
}

// Usage
extension __StringStorage {
  internal var hasBreadcrumbs: Bool { _capacityAndFlags.hasBreadcrumbs }

  @inline(__always)
  internal var mutableStart: UnsafeMutablePointer<UInt8> {
    UnsafeMutablePointer(Builtin.projectTailElems(self, UInt8.self))
  }
  @inline(__always)
  private var mutableEnd: UnsafeMutablePointer<UInt8> {
     mutableStart + count
  }

  @inline(__always)
  internal var start: UnsafePointer<UInt8> {
     UnsafePointer(mutableStart)
  }

  @inline(__always)
  private final var end: UnsafePointer<UInt8> {
    UnsafePointer(mutableEnd)
  }

  // Point to the nul-terminator.
  @inline(__always)
  internal final var terminator: UnsafeMutablePointer<UInt8> {
    mutableEnd
  }

  @inline(__always)
  internal var codeUnits: UnsafeBufferPointer<UInt8> {
    UnsafeBufferPointer(start: start, count: count)
  }

  // The address after the last bytes of capacity
  //
  // If breadcrumbs are present, this will point to them, otherwise it will
  // point to the end of the allocation (as far as Swift is concerned).
  fileprivate var _realCapacityEnd: Builtin.RawPointer {
    Builtin.getTailAddr_Word(
      start._rawValue,
      _capacityAndFlags._realCapacity._builtinWordValue,
      UInt8.self,
      Optional<_StringBreadcrumbs>.self)
  }

  // @opaque
  fileprivate var _breadcrumbsAddress: UnsafeMutablePointer<_StringBreadcrumbs?> {
    _precondition(
      hasBreadcrumbs, "Internal error: string breadcrumbs not present")
    return UnsafeMutablePointer(_realCapacityEnd)
  }

  // The total capacity available for code units. Note that this excludes the
  // required nul-terminator.
  internal var capacity: Int { _capacityAndFlags.capacity }

  // The unused capacity available for appending. Note that this excludes the
  // required nul-terminator.
  //
  // NOTE: Callers who wish to mutate this storage should enforce nul-termination
  //
  // TODO: Refactoring or removing. Excluding the last byte is awkward.
  @inline(__always)
  private var unusedStorage: UnsafeMutableBufferPointer<UInt8> {
    UnsafeMutableBufferPointer(
      start: mutableEnd, count: unusedCapacity)
  }

  // The capacity available for appending. Note that this excludes the required
  // nul-terminator.
  internal var unusedCapacity: Int { capacity &- count }

  #if !INTERNAL_CHECKS_ENABLED
  @inline(__always) internal func _invariantCheck(initialized: Bool = true) {}
  #else
  internal func _invariantCheck(initialized: Bool = true) {
    let rawSelf = UnsafeRawPointer(Builtin.bridgeToRawPointer(self))
    let rawStart = UnsafeRawPointer(start)
    _internalInvariant(unusedCapacity >= 0)
    _internalInvariant(count <= capacity)
    _internalInvariant(rawSelf + Int(_StringObject.nativeBias) == rawStart)
    _internalInvariant(
      self._capacityAndFlags._realCapacity > self.count,
      "no room for nul-terminator")
    _internalInvariant(self.terminator.pointee == 0, "not nul terminated")
    let str = asString
    _internalInvariant(str._guts._object.isPreferredRepresentation)

    _countAndFlags._invariantCheck()
    if isASCII && initialized {
      _internalInvariant(_allASCII(self.codeUnits))
    }
    if hasBreadcrumbs, let crumbs = _breadcrumbsAddress.pointee {
      crumbs._invariantCheck(for: self.asString)
    }
    _internalInvariant(_countAndFlags.isNativelyStored)
    _internalInvariant(_countAndFlags.isTailAllocated)

    // Check that capacity end matches our notion of unused storage, and also
    // checks that breadcrumbs were dutifully aligned.
    _internalInvariant(UnsafeMutablePointer<UInt8>(_realCapacityEnd)
      == unusedStorage.baseAddress! + (unusedStorage.count + 1))
  }
  #endif // INTERNAL_CHECKS_ENABLED
}

// Appending
extension __StringStorage {
  // Perform common post-RRC adjustments and invariant enforcement.
  @_effects(releasenone)
  internal func _updateCountAndFlags(newCount: Int, newIsASCII: Bool) {
    let countAndFlags = _CountAndFlags(
      mortalCount: newCount, isASCII: newIsASCII)
#if _pointerBitWidth(_64)
    self._countAndFlags = countAndFlags
#elseif _pointerBitWidth(_32)
    self._count = countAndFlags.count
    self._countFlags = countAndFlags.flags
#else
#error("Unknown platform")
#endif
    self.terminator.pointee = 0

    // TODO(String performance): Consider updating breadcrumbs when feasible.
    if hasBreadcrumbs {
      self._breadcrumbsAddress.pointee = nil
    }
    _invariantCheck()
  }

  // Perform common post-append adjustments and invariant enforcement.
  @_effects(releasenone)
  private func _postAppendAdjust(
    appendedCount: Int, appendedIsASCII isASCII: Bool
  ) {
    let oldTerminator = self.terminator
    _updateCountAndFlags(
      newCount: self.count + appendedCount, newIsASCII: self.isASCII && isASCII)
    _internalInvariant(oldTerminator + appendedCount == self.terminator)
  }

  @_effects(releasenone)
  internal func appendInPlace(
    _ other: UnsafeBufferPointer<UInt8>, isASCII: Bool
  ) {
    _internalInvariant(self.capacity >= other.count)
    let srcAddr = other.baseAddress._unsafelyUnwrappedUnchecked
    let srcCount = other.count
    self.mutableEnd.initialize(from: srcAddr, count: srcCount)
    _postAppendAdjust(appendedCount: srcCount, appendedIsASCII: isASCII)
  }

  @_effects(releasenone)
  internal func appendInPlace<Iter: IteratorProtocol>(
    _ other: inout Iter, isASCII: Bool
  ) where Iter.Element == UInt8 {
    var srcCount = 0
    while let cu = other.next() {
      _internalInvariant(self.unusedCapacity >= 1)
      unusedStorage[srcCount] = cu
      srcCount += 1
    }
    _postAppendAdjust(appendedCount: srcCount, appendedIsASCII: isASCII)
  }

  internal func clear() {
    _updateCountAndFlags(newCount: 0, newIsASCII: true)
  }
}

// Removing
extension __StringStorage {
  @_effects(releasenone)
  internal func remove(from lower: Int, to upper: Int) {
    _internalInvariant(lower <= upper)

    let lowerPtr = mutableStart + lower
    let upperPtr = mutableStart + upper
    let tailCount = mutableEnd - upperPtr
    lowerPtr.moveInitialize(from: upperPtr, count: tailCount)

    _updateCountAndFlags(
      newCount: self.count &- (upper &- lower), newIsASCII: self.isASCII)
  }

  // Reposition a tail of this storage from src to dst. Returns the length of
  // the tail.
  @_effects(releasenone)
  internal func _slideTail(
    src: UnsafeMutablePointer<UInt8>,
    dst: UnsafeMutablePointer<UInt8>
  ) -> Int {
    _internalInvariant(dst >= mutableStart && src <= mutableEnd)
    let tailCount = mutableEnd - src
    dst.moveInitialize(from: src, count: tailCount)
    return tailCount
  }

  @_effects(releasenone)
  internal func replace(
    from lower: Int, to upper: Int, with replacement: UnsafeBufferPointer<UInt8>
  ) {
    _internalInvariant(lower <= upper)
    let replCount = replacement.count
    _internalInvariant(replCount - (upper - lower) <= unusedCapacity)

    // Position the tail.
    let lowerPtr = mutableStart + lower
    let tailCount = _slideTail(
      src: mutableStart + upper, dst: lowerPtr + replCount)

    // Copy in the contents.
    lowerPtr.moveInitialize(
      from: UnsafeMutablePointer(
        mutating: replacement.baseAddress._unsafelyUnwrappedUnchecked),
      count: replCount)

    let isASCII = self.isASCII && _allASCII(replacement)
    _updateCountAndFlags(newCount: lower + replCount + tailCount, newIsASCII: isASCII)
  }


  @_effects(releasenone)
  internal func replace<C: Collection>(
    from lower: Int,
    to upper: Int,
    with replacement: C,
    replacementCount replCount: Int
  ) where C.Element == UInt8 {
    _internalInvariant(lower <= upper)
    _internalInvariant(replCount - (upper - lower) <= unusedCapacity)

    // Position the tail.
    let lowerPtr = mutableStart + lower
    let tailCount = _slideTail(
      src: mutableStart + upper, dst: lowerPtr + replCount)

    // Copy in the contents.
    var isASCII = self.isASCII
    var srcCount = 0
    for cu in replacement {
      if cu >= 0x80 { isASCII = false }
      lowerPtr[srcCount] = cu
      srcCount += 1
    }
    _internalInvariant(srcCount == replCount)

    _updateCountAndFlags(
      newCount: lower + replCount + tailCount, newIsASCII: isASCII)
  }
}

// For shared storage and bridging literals
// NOTE: older runtimes called this class _SharedStringStorage. The two
// must coexist without conflicting ObjC class names, so it was
// renamed. The old name must not be used in the new runtime.
final internal class __SharedStringStorage
  : __SwiftNativeNSString, _AbstractStringStorage {
  internal var _owner: AnyObject?
  internal var start: UnsafePointer<UInt8>

#if _pointerBitWidth(_64)
  internal var _countAndFlags: _StringObject.CountAndFlags
#elseif _pointerBitWidth(_32)
  internal var _count: Int
  internal var _countFlags: UInt16

  @inline(__always)
  internal var _countAndFlags: _StringObject.CountAndFlags {
    _CountAndFlags(count: _count, flags: _countFlags)
  }
#else
#error("Unknown platform")
#endif

  internal var _breadcrumbs: _StringBreadcrumbs? = nil

  internal var count: Int { _countAndFlags.count }

  internal init(
    immortal ptr: UnsafePointer<UInt8>,
    countAndFlags: _StringObject.CountAndFlags
  ) {
    self._owner = nil
    self.start = ptr
#if _pointerBitWidth(_64)
    self._countAndFlags = countAndFlags
#elseif _pointerBitWidth(_32)
    self._count = countAndFlags.count
    self._countFlags = countAndFlags.flags
#else
#error("Unknown platform")
#endif
    super.init()
    self._invariantCheck()
  }

  @inline(__always)
  final internal var isASCII: Bool { return _countAndFlags.isASCII }

  final internal var asString: String {
    @_effects(readonly) @inline(__always) get {
      return String(_StringGuts(self))
    }
  }
}

extension __SharedStringStorage {
#if !INTERNAL_CHECKS_ENABLED
  @inline(__always)
  internal func _invariantCheck() {}
#else
  internal func _invariantCheck() {
    if let crumbs = _breadcrumbs {
      crumbs._invariantCheck(for: self.asString)
    }
    _countAndFlags._invariantCheck()
    _internalInvariant(!_countAndFlags.isNativelyStored)
    _internalInvariant(!_countAndFlags.isTailAllocated)
    let str = asString
    _internalInvariant(!str._guts._object.isPreferredRepresentation)
  }
#endif // INTERNAL_CHECKS_ENABLED
}

// Get and populate breadcrumbs
extension _StringGuts {
  /// Atomically load and return breadcrumbs, populating them if necessary.
  ///
  /// This emits aquire/release barriers to avoid access reordering trouble.
  ///
  /// This returns an unmanaged +0 reference to allow accessing breadcrumbs
  /// without incurring retain/release operations.
  @_effects(releasenone)
  internal func loadUnmanagedBreadcrumbs() -> Unmanaged<_StringBreadcrumbs> {
    // FIXME: Returning Unmanaged emulates the original implementation (that
    // used to return a nonatomic pointer), but it may be an unnecessary
    // complication. (UTF-16 transcoding seems expensive enough not to worry
    // about retain overhead.)
    _internalInvariant(hasBreadcrumbs)

    let mutPtr: UnsafeMutablePointer<_StringBreadcrumbs?>
    if hasNativeStorage {
      mutPtr = _object.withNativeStorage { $0._breadcrumbsAddress }
    } else {
      mutPtr = _object.withSharedStorage {
        UnsafeMutablePointer(Builtin.addressof(&$0._breadcrumbs))
      }
    }

    if let breadcrumbs = _stdlib_atomicAcquiringLoadARCRef(object: mutPtr) {
      return breadcrumbs
    }
    let desired = _StringBreadcrumbs(String(self))
    return _stdlib_atomicAcquiringInitializeARCRef(
      object: mutPtr, desired: desired)
  }
}

