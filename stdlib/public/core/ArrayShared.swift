//===--- ArrayShared.swift ------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//

/// This type is used as a result of the `_checkSubscript` call to associate the
/// call with the array access call it guards.
///
/// In order for the optimizer see that a call to `_checkSubscript` is semantically
/// associated with an array access, a value of this type is returned and later passed
/// to the accessing function.  For example, a typical call to `_getElement` looks like
///   let token = _checkSubscript(index, ...)
///   return _getElement(index, ... , matchingSubscriptCheck: token)
@frozen
public struct _DependenceToken {
  @inlinable
  public init() {
  }
}

/// Returns an Array of `_count` uninitialized elements using the
/// given `storage`, and a pointer to uninitialized memory for the
/// first element.
///
/// This function is referenced by the compiler to allocate array literals.
///
/// - Precondition: `storage` is `_ContiguousArrayStorage`.
@inlinable // FIXME(inline-always)
@inline(__always)
@_semantics("array.uninitialized_intrinsic")
public // COMPILER_INTRINSIC
func _allocateUninitializedArray<Element>(_  builtinCount: Builtin.Word)
    -> (Array<Element>, Builtin.RawPointer) {
  let count = Int(builtinCount)
  if count > 0 {
    // Doing the actual buffer allocation outside of the array.uninitialized
    // semantics function enables stack propagation of the buffer.
    #if !$Embedded
    let bufferObject = Builtin.allocWithTailElems_1(
       getContiguousArrayStorageType(for: Element.self), builtinCount, Element.self)
    #else
    let bufferObject = Builtin.allocWithTailElems_1(
       _ContiguousArrayStorage<Element>.self, builtinCount, Element.self)
    #endif

    let (array, ptr) = Array<Element>._adoptStorage(bufferObject, count: count)
    return (array, ptr._rawValue)
  }
  // For an empty array no buffer allocation is needed.
  let (array, ptr) = Array<Element>._allocateUninitialized(count)
  return (array, ptr._rawValue)
}

// Referenced by the compiler to deallocate array literals on the
// error path.
@inlinable
@_semantics("array.dealloc_uninitialized")
public // COMPILER_INTRINSIC
func _deallocateUninitializedArray<Element>(
  _ array: __owned Array<Element>
) {
  var array = array
  array._deallocateUninitialized()
}

#if !INTERNAL_CHECKS_ENABLED
@_alwaysEmitIntoClient
@_semantics("array.finalize_intrinsic")
@_effects(readnone)
@_effects(escaping array.value** => return.value**)
@_effects(escaping array.value**.class*.value** => return.value**.class*.value**)
public // COMPILER_INTRINSIC
func _finalizeUninitializedArray<Element>(
  _ array: __owned Array<Element>
) -> Array<Element> {
  var mutableArray = array
  mutableArray._endMutation()
  return mutableArray
}
#else
// When asserts are enabled, _endCOWMutation writes to _native.isImmutable
// So we cannot have @_effects(readnone)
@_alwaysEmitIntoClient
@_semantics("array.finalize_intrinsic")
public // COMPILER_INTRINSIC
func _finalizeUninitializedArray<Element>(
  _ array: __owned Array<Element>
) -> Array<Element> {
  var mutableArray = array
  mutableArray._endMutation()
  return mutableArray
}
#endif

@_unavailableInEmbedded
extension Collection {  
  // Utility method for collections that wish to implement
  // CustomStringConvertible and CustomDebugStringConvertible using a bracketed
  // list of elements, like an array.
  internal func _makeCollectionDescription(
    withTypeName type: String? = nil
  ) -> String {
#if !SWIFT_STDLIB_STATIC_PRINT
    var result = ""
    if let type = type {
      result += "\(type)(["
    } else {
      result += "["
    }

    var first = true
    for item in self {
      if first {
        first = false
      } else {
        result += ", "
      }
      #if !$Embedded
      debugPrint(item, terminator: "", to: &result)
      #else
      "(cannot print value in embedded Swift)".write(to: &result)
      #endif
    }
    result += type != nil ? "])" : "]"
    return result
#else
    return "(collection printing not available)"
#endif
  }
}

extension _ArrayBufferProtocol {
  @inlinable // FIXME: @useableFromInline (https://github.com/apple/swift/issues/50130).
  @inline(never)
  internal mutating func _arrayOutOfPlaceReplace<C: Collection>(
    _ bounds: Range<Int>,
    with newValues: __owned C,
    count insertCount: Int
  ) where C.Element == Element {

    let growth = insertCount - bounds.count
    let newCount = self.count + growth
    var newBuffer = _forceCreateUniqueMutableBuffer(
      newCount: newCount, requiredCapacity: newCount)

    _arrayOutOfPlaceUpdate(
      &newBuffer, bounds.lowerBound - startIndex, insertCount,
      { rawMemory, count in
        var p = rawMemory
        var q = newValues.startIndex
        for _ in 0..<count {
          p.initialize(to: newValues[q])
          newValues.formIndex(after: &q)
          p += 1
        }
        _expectEnd(of: newValues, is: q)
      }
    )
  }
}

/// A _debugPrecondition check that `i` has exactly reached the end of
/// `s`.  This test is never used to ensure memory safety; that is
/// always guaranteed by measuring `s` once and re-using that value.
@inlinable
internal func _expectEnd<C: Collection>(of s: C, is i: C.Index) {
  _debugPrecondition(
    i == s.endIndex,
    "invalid Collection: count differed in successive traversals")
}

@inlinable
internal func _growArrayCapacity(_ capacity: Int) -> Int {
  /* We calculate this in an unusual way because we want to minimize the number
   of overflow checks, approximately match Cocoa's 1.6x growth factor, and round
   up to the nearest integer rather than down.
   */
  return (capacity * 2) &- (capacity >> 1)
}

@_alwaysEmitIntoClient
internal func _growArrayCapacity(
  oldCapacity: Int, minimumCapacity: Int, growForAppend: Bool
) -> Int {
  if growForAppend {
    if oldCapacity < minimumCapacity {
      // When appending to an array, grow exponentially.
      return Swift.max(minimumCapacity, _growArrayCapacity(oldCapacity))
    }
    return oldCapacity
  }
  // If not for append, just use the specified capacity, ignoring oldCapacity.
  // This means that we "shrink" the buffer in case minimumCapacity is less
  // than oldCapacity.
  return minimumCapacity
}

//===--- generic helpers --------------------------------------------------===//

extension _ArrayBufferProtocol {
  /// Create a unique mutable buffer that has enough capacity to hold 'newCount'
  /// elements and at least 'requiredCapacity' elements. Set the count of the new
  /// buffer to 'newCount'. The content of the buffer is uninitialized.
  /// The formula used to compute the new buffers capacity is:
  ///   max(requiredCapacity, source.capacity)  if newCount <= source.capacity
  ///   max(requiredCapacity, _growArrayCapacity(source.capacity)) otherwise
  @inline(never)
  @inlinable // @specializable
  internal func _forceCreateUniqueMutableBuffer(
    newCount: Int, requiredCapacity: Int
  ) -> _ContiguousArrayBuffer<Element> {
    return _forceCreateUniqueMutableBufferImpl(
      countForBuffer: newCount, minNewCapacity: newCount,
      requiredCapacity: requiredCapacity)
  }

  /// Create a unique mutable buffer that has enough capacity to hold
  /// 'minNewCapacity' elements and set the count of the new buffer to
  /// 'countForNewBuffer'. The content of the buffer uninitialized.
  /// The formula used to compute the new buffers capacity is:
  ///   max(minNewCapacity, source.capacity) if minNewCapacity <= source.capacity
  ///   max(minNewCapacity, _growArrayCapacity(source.capacity)) otherwise
  @inline(never)
  @inlinable // @specializable
  internal func _forceCreateUniqueMutableBuffer(
    countForNewBuffer: Int, minNewCapacity: Int
  ) -> _ContiguousArrayBuffer<Element> {
    return _forceCreateUniqueMutableBufferImpl(
      countForBuffer: countForNewBuffer, minNewCapacity: minNewCapacity,
      requiredCapacity: minNewCapacity)
  }

  /// Create a unique mutable buffer that has enough capacity to hold
  /// 'minNewCapacity' elements and at least 'requiredCapacity' elements and set
  /// the count of the new buffer to 'countForBuffer'. The content of the buffer
  /// uninitialized.
  /// The formula used to compute the new capacity is:
  ///  max(requiredCapacity, source.capacity) if minNewCapacity <= source.capacity
  ///  max(requiredCapacity, _growArrayCapacity(source.capacity))  otherwise
  @inlinable
  internal func _forceCreateUniqueMutableBufferImpl(
    countForBuffer: Int, minNewCapacity: Int,
    requiredCapacity: Int
  ) -> _ContiguousArrayBuffer<Element> {
    _internalInvariant(countForBuffer >= 0)
    _internalInvariant(requiredCapacity >= countForBuffer)
    _internalInvariant(minNewCapacity >= countForBuffer)

    let minimumCapacity = Swift.max(requiredCapacity,
      minNewCapacity > capacity
         ? _growArrayCapacity(capacity) : capacity)

    return _ContiguousArrayBuffer(
      _uninitializedCount: countForBuffer, minimumCapacity: minimumCapacity)
  }
}

extension _ArrayBufferProtocol {
  /// Initialize the elements of dest by copying the first headCount
  /// items from source, calling initializeNewElements on the next
  /// uninitialized element, and finally by copying the last N items
  /// from source into the N remaining uninitialized elements of dest.
  ///
  /// As an optimization, may move elements out of source rather than
  /// copying when it isUniquelyReferenced.
  @inline(never)
  @inlinable // @specializable
  internal mutating func _arrayOutOfPlaceUpdate(
    _ dest: inout _ContiguousArrayBuffer<Element>,
    _ headCount: Int, // Count of initial source elements to copy/move
    _ newCount: Int,  // Number of new elements to insert
    _ initializeNewElements: 
        ((UnsafeMutablePointer<Element>, _ count: Int) -> ()) = { ptr, count in
      _internalInvariant(count == 0)
    }
  ) {

    _internalInvariant(headCount >= 0)
    _internalInvariant(newCount >= 0)

    // Count of trailing source elements to copy/move
    let sourceCount = self.count
    let tailCount = dest.count - headCount - newCount
    _internalInvariant(headCount + tailCount <= sourceCount)

    let oldCount = sourceCount - headCount - tailCount
    let destStart = dest.firstElementAddress
    let newStart = destStart + headCount
    let newEnd = newStart + newCount

    // Check to see if we have storage we can move from
    if let backing = requestUniqueMutableBackingBuffer(
      minimumCapacity: sourceCount) {

      let sourceStart = firstElementAddress
      let oldStart = sourceStart + headCount

      // Destroy any items that may be lurking in a _SliceBuffer before
      // its real first element
      let backingStart = backing.firstElementAddress
      let sourceOffset = sourceStart - backingStart
      backingStart.deinitialize(count: sourceOffset)

      // Move the head items
      destStart.moveInitialize(from: sourceStart, count: headCount)

      // Destroy unused source items
      oldStart.deinitialize(count: oldCount)

      initializeNewElements(newStart, newCount)

      // Move the tail items
      newEnd.moveInitialize(from: oldStart + oldCount, count: tailCount)

      // Destroy any items that may be lurking in a _SliceBuffer after
      // its real last element
      let backingEnd = backingStart + backing.count
      let sourceEnd = sourceStart + sourceCount
      sourceEnd.deinitialize(count: backingEnd - sourceEnd)
      backing.count = 0
    }
    else {
      let headStart = startIndex
      let headEnd = headStart + headCount
      let newStart = _copyContents(
        subRange: headStart..<headEnd,
        initializing: destStart)
      initializeNewElements(newStart, newCount)
      let tailStart = headEnd + oldCount
      let tailEnd = endIndex
      _copyContents(subRange: tailStart..<tailEnd, initializing: newEnd)
    }
    self = Self(_buffer: dest, shiftedToStartIndex: startIndex)
  }
}

extension _ArrayBufferProtocol {
  @inline(never)
  @usableFromInline
  internal mutating func _outlinedMakeUniqueBuffer(bufferCount: Int) {

    if _fastPath(
        requestUniqueMutableBackingBuffer(minimumCapacity: bufferCount) != nil) {
      return
    }

    var newBuffer = _forceCreateUniqueMutableBuffer(
      newCount: bufferCount, requiredCapacity: bufferCount)
    _arrayOutOfPlaceUpdate(&newBuffer, bufferCount, 0)
  }

  /// Append items from `newItems` to a buffer.
  @inlinable
  internal mutating func _arrayAppendSequence<S: Sequence>(
    _ newItems: __owned S
  ) where S.Element == Element {
    
    // this function is only ever called from append(contentsOf:)
    // which should always have exhausted its capacity before calling
    _internalInvariant(count == capacity)
    var newCount = self.count

    // there might not be any elements to append remaining,
    // so check for nil element first, then increase capacity,
    // then inner-loop to fill that capacity with elements
    var stream = newItems.makeIterator()
    var nextItem = stream.next()
    while nextItem != nil {

      // grow capacity, first time around and when filled
      var newBuffer = _forceCreateUniqueMutableBuffer(
        countForNewBuffer: newCount, 
        // minNewCapacity handles the exponential growth, just
        // need to request 1 more than current count/capacity
        minNewCapacity: newCount + 1)

      _arrayOutOfPlaceUpdate(&newBuffer, newCount, 0)

      let currentCapacity = self.capacity
      let base = self.firstElementAddress

      // fill while there is another item and spare capacity
      while let next = nextItem, newCount < currentCapacity {
        (base + newCount).initialize(to: next)
        newCount += 1
        nextItem = stream.next()
      }
      self.count = newCount
    }
  }
}
