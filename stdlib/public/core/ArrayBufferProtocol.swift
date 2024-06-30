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
    elementsOf newValues: __owned C,
    holeAlreadyPunched: Bool
  ) where C: Collection, C.Element == Element {
    _internalInvariant(startIndex == 0, "_SliceBuffer should override this function.")
    let elements = self.firstElementAddress

    // erase all the elements we're replacing to create a hole
    let holeStart = elements + subrange.lowerBound
    let holeEnd = holeStart + newCount
    let eraseCount = subrange.count
    
    if !holeAlreadyPunched {
      holeStart.deinitialize(count: eraseCount)
    }
    
    let growth = newCount - eraseCount

    if growth != 0 {
      let tailStart = elements + subrange.upperBound
      let tailCount = self.count - subrange.upperBound
      holeEnd.moveInitialize(from: tailStart, count: tailCount)
      self.count += growth
    }

    // don't use UnsafeMutableBufferPointer.initialize(fromContentsOf:)
    // since it behaves differently on collections that misreport count,
    // and breaks validation tests for those usecases / potentially
    // breaks ABI guarantees.
    if newCount > 0 {
      let done: Void? = newValues.withContiguousStorageIfAvailable {
        _precondition(
          $0.count == newCount,
          "invalid Collection: count differed in successive traversals"
        )
        holeStart.initialize(from: $0.baseAddress!, count: newCount)
      }
      if done == nil {
        var place = holeStart
        var i = newValues.startIndex
        while place < holeEnd {
          place.initialize(to: newValues[i])
          place += 1
          newValues.formIndex(after: &i)
        }
        _expectEnd(of: newValues, is: i)
      }
    }
  }

  @inlinable
  internal mutating func replaceSubrange<C>(
    _ subrange: Range<Int>,
    with newCount: Int,
    elementsOf newValues: __owned C
  ) where C: Collection, C.Element == Element {
    replaceSubrange(
      subrange,
      with: newCount,
      elementsOf: newValues,
      holeAlreadyPunched: false)
  }
}
