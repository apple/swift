//===--- ArrayBufferProtocol.swift ----------------------------------------===//
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

/// The underlying buffer for an ArrayType conforms to
/// `_ArrayBufferProtocol`.  This buffer does not provide value semantics.
@usableFromInline
internal protocol _ArrayBufferProtocol
  : MutableCollection, RandomAccessCollection 
where Indices == Range<Int> {

  /// Create an empty buffer.
  init()

  /// Adopt the entire buffer, presenting it at the provided `startIndex`.
  init(_buffer: _ContiguousArrayBuffer<Element>, shiftedToStartIndex: Int)

  init(copying buffer: Self)

  /// Copy the elements in `bounds` from this buffer into uninitialized
  /// memory starting at `target`.  Return a pointer "past the end" of the
  /// just-initialized memory.
  @discardableResult
  __consuming func _copyContents(
    subRange bounds: Range<Int>,
    initializing target: UnsafeMutablePointer<Element>
  ) -> UnsafeMutablePointer<Element>

  /// If this buffer is backed by a uniquely-referenced mutable
  /// `_ContiguousArrayBuffer` that can be grown in-place to allow the `self`
  /// buffer store `minimumCapacity` elements, returns that buffer.
  /// Otherwise, returns `nil`.
  ///
  /// - Note: The result's firstElementAddress may not match ours, if we are a
  ///   _SliceBuffer.
  ///
  /// - Note: This function must remain mutating; otherwise the buffer
  ///   may acquire spurious extra references, which will cause
  ///   unnecessary reallocation.
  mutating func requestUniqueMutableBackingBuffer(
    minimumCapacity: Int
  ) -> _ContiguousArrayBuffer<Element>?

  /// Returns `true` if this buffer is backed by a uniquely-referenced mutable
  /// _ContiguousArrayBuffer; otherwise, returns `false`.
  ///
  /// - Note: This function must remain mutating; otherwise the buffer
  ///   may acquire spurious extra references, which will cause
  ///   unnecessary reallocation.
  mutating func isMutableAndUniquelyReferenced() -> Bool

  /// If this buffer is backed by a `_ContiguousArrayBuffer`
  /// containing the same number of elements as `self`, return it.
  /// Otherwise, return `nil`.
  func requestNativeBuffer() -> _ContiguousArrayBuffer<Element>?

  /// Replace the given `subRange` with the first `newCount` elements of
  /// the given collection.
  ///
  /// - Precondition: This buffer is backed by a uniquely-referenced
  /// `_ContiguousArrayBuffer`.
  mutating func replaceSubrange<C>(
    _ subrange: Range<Int>,
    with newCount: Int,
    elementsOf newValues: __owned C
  ) where C: Collection, C.Element == Element

  /// Returns a `_SliceBuffer` containing the elements in `bounds`.
  subscript(bounds: Range<Int>) -> _SliceBuffer<Element> { get }

  /// Call `body(p)`, where `p` is an `UnsafeBufferPointer` over the
  /// underlying contiguous storage.  If no such storage exists, it is
  /// created on-demand.
  func withUnsafeBufferPointer<R>(
    _ body: (UnsafeBufferPointer<Element>) throws -> R
  ) rethrows -> R

  /// Call `body(p)`, where `p` is an `UnsafeMutableBufferPointer`
  /// over the underlying contiguous storage.
  ///
  /// - Precondition: Such contiguous storage exists or the buffer is empty.
  mutating func withUnsafeMutableBufferPointer<R>(
    _ body: (UnsafeMutableBufferPointer<Element>) throws -> R
  ) rethrows -> R

  /// The number of elements the buffer stores.
  override var count: Int { get set }

  /// The number of elements the buffer can store without reallocation.
  var capacity: Int { get }

  #if $Embedded
  typealias AnyObject = Builtin.NativeObject
  #endif

  /// An object that keeps the elements stored in this buffer alive.
  var owner: AnyObject { get }

  /// A pointer to the first element.
  ///
  /// - Precondition: The elements are known to be stored contiguously.
  var firstElementAddress: UnsafeMutablePointer<Element> { get }

  /// If the elements are stored contiguously, a pointer to the first
  /// element. Otherwise, `nil`.
  var firstElementAddressIfContiguous: UnsafeMutablePointer<Element>? { get }

  /// Returns a base address to which you can add an index `i` to get the
  /// address of the corresponding element at `i`.
  var subscriptBaseAddress: UnsafeMutablePointer<Element> { get }

  /// A value that identifies the storage used by the buffer.  Two
  /// buffers address the same elements when they have the same
  /// identity and count.
  var identity: UnsafeRawPointer { get }
}

extension _ArrayBufferProtocol {
  @inlinable
  internal var subscriptBaseAddress: UnsafeMutablePointer<Element> {
    return firstElementAddress
  }

  // Make sure the compiler does not inline _copyBuffer to reduce code size.
  @inline(never)
  @inlinable // This code should be specializable such that copying an array is
             // fast and does not end up in an unspecialized entry point.
  internal init(copying buffer: Self) {
    let newBuffer = _ContiguousArrayBuffer<Element>(
      _uninitializedCount: buffer.count, minimumCapacity: buffer.count)
    buffer._copyContents(
      subRange: buffer.indices,
      initializing: newBuffer.firstElementAddress)
    self = Self( _buffer: newBuffer, shiftedToStartIndex: buffer.startIndex)
  }

  @inlinable
  internal mutating func replaceSubrange<C>(
    _ subrange: Range<Int>,
    with newCount: Int,
    @_eagerMove elementsOf newValues: __owned C
  ) where C: Collection, C.Element == Element {
    _internalInvariant(startIndex == 0, "_SliceBuffer should override this function.")
    let oldCount = self.count
    let eraseCount = subrange.count

    let growth = newCount - eraseCount
    // This check will prevent storing a 0 count to the empty array singleton.
    if growth != 0 {
      self.count = oldCount + growth
    }

    let elements = self.subscriptBaseAddress
    let oldTailIndex = subrange.upperBound
    let oldTailStart = elements + oldTailIndex
    let newTailIndex = oldTailIndex + growth
    let newTailStart = oldTailStart + growth
    let tailCount = oldCount - subrange.upperBound

    if growth > 0 {
      // Slide the tail part of the buffer forwards, in reverse order
      // so as not to self-clobber.
      newTailStart.moveInitialize(from: oldTailStart, count: tailCount)

      // Update the original subrange
      var i = newValues.startIndex
      for j in subrange {
        elements[j] = newValues[i]
        newValues.formIndex(after: &i)
      }
      // Initialize the hole left by sliding the tail forward
      for j in oldTailIndex..<newTailIndex {
        (elements + j).initialize(to: newValues[i])
        newValues.formIndex(after: &i)
      }
      _expectEnd(of: newValues, is: i)
    }
    else { // We're not growing the buffer
      // Assign all the new elements into the start of the subrange
      var i = subrange.lowerBound
      @_eagerMove var j = newValues.startIndex
      for _ in 0..<newCount {
        elements[i] = newValues[j]
        i += 1
        newValues.formIndex(after: &j)
      }
      _expectEnd(of: newValues, is: j)

      // If the size didn't change, we're done.
      if growth == 0 {
        return
      }

      // Move the tail backward to cover the shrinkage.
      let shrinkage = -growth
      if tailCount > shrinkage {   // If the tail length exceeds the shrinkage

        // Update the rest of the replaced range with the first
        // part of the tail.
        newTailStart.moveUpdate(from: oldTailStart, count: shrinkage)

        // Slide the rest of the tail back
        oldTailStart.moveInitialize(
          from: oldTailStart + shrinkage, count: tailCount - shrinkage)
      }
      else {                      // Tail fits within erased elements
        // Update the start of the replaced range with the tail
        newTailStart.moveUpdate(from: oldTailStart, count: tailCount)

        // Destroy elements remaining after the tail in subrange
        (newTailStart + tailCount).deinitialize(
          count: shrinkage - tailCount)
      }
    }
  }
}
