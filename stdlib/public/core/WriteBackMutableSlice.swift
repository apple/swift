//===--- WriteBackMutableSlice.swift --------------------------------------===//
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

@inlinable
internal func _writeBackMutableSlice<C, Slice_>(
  _ self_: inout C, bounds: Range<C.Index>, slice: Slice_
) where
  C: MutableCollection,
  Slice_: Collection,
  C.Element == Slice_.Element,
  C.Index == Slice_.Index {

  self_._failEarlyRangeCheck(bounds, bounds: self_.startIndex..<self_.endIndex)

  // FIXME(performance): can we use
  // _withUnsafeMutableBufferPointerIfSupported?  Would that create inout
  // aliasing violations if the newValue points to the same buffer?

  // Allow the underlying collection to have a say about what indices can be
  // slice boundaries. This is crucial for cases where the same index type is
  // used in multiple collection types of varying granularity (similar to
  // `String` views). Letting the base collection do the actual slicing allows
  // it to properly validate indices, and to optionally handle unaligned slices
  // by rounding their boundaries down to the nearest valid index.
  let orig = self_[bounds]
  var selfElementIndex = orig.startIndex
  let selfElementsEndIndex = orig.endIndex

  var newElementIndex = slice.startIndex
  let newElementsEndIndex = slice.endIndex

  while selfElementIndex != selfElementsEndIndex &&
    newElementIndex != newElementsEndIndex {

    self_[selfElementIndex] = slice[newElementIndex]
    self_.formIndex(after: &selfElementIndex)
    slice.formIndex(after: &newElementIndex)
  }

  _precondition(
    selfElementIndex == selfElementsEndIndex,
    "Cannot replace a slice of a MutableCollection with a slice of a smaller size")
  _precondition(
    newElementIndex == newElementsEndIndex,
    "Cannot replace a slice of a MutableCollection with a slice of a larger size")
}

