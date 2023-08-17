//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

/// This file is copied from swift-collections and should not be modified here.
/// Rather all changes should be made to swift-collections and copied back.

import Swift

internal struct _UnsafeWrappedBuffer<Element> {
  internal let first: UnsafeBufferPointer<Element>

  internal let second: UnsafeBufferPointer<Element>?

  internal init(
    _ first: UnsafeBufferPointer<Element>,
    _ second: UnsafeBufferPointer<Element>? = nil
  ) {
    self.first = first
    self.second = second
    assert(first.count > 0 || second == nil)
  }

  internal init(
    start: UnsafePointer<Element>,
    count: Int
  ) {
    self.init(UnsafeBufferPointer(start: start, count: count))
  }

  internal init(
    first start1: UnsafePointer<Element>,
    count count1: Int,
    second start2: UnsafePointer<Element>,
    count count2: Int
  ) {
    self.init(UnsafeBufferPointer(start: start1, count: count1),
              UnsafeBufferPointer(start: start2, count: count2))
  }

  internal var count: Int { first.count + (second?.count ?? 0) }
}

internal struct _UnsafeMutableWrappedBuffer<Element> {
  internal let first: UnsafeMutableBufferPointer<Element>

  internal let second: UnsafeMutableBufferPointer<Element>?

  internal init(
    _ first: UnsafeMutableBufferPointer<Element>,
    _ second: UnsafeMutableBufferPointer<Element>? = nil
  ) {
    self.first = first
    self.second = second?.count == 0 ? nil : second
    assert(first.count > 0 || second == nil)
  }

  internal init(
    start: UnsafeMutablePointer<Element>,
    count: Int
  ) {
    self.init(UnsafeMutableBufferPointer(start: start, count: count))
  }

  internal init(
    first start1: UnsafeMutablePointer<Element>,
    count count1: Int,
    second start2: UnsafeMutablePointer<Element>,
    count count2: Int
  ) {
    self.init(UnsafeMutableBufferPointer(start: start1, count: count1),
              UnsafeMutableBufferPointer(start: start2, count: count2))
  }

  internal init(mutating buffer: _UnsafeWrappedBuffer<Element>) {
    self.init(.init(mutating: buffer.first),
              buffer.second.map { .init(mutating: $0) })
  }
}

extension _UnsafeMutableWrappedBuffer {
  internal var count: Int { first.count + (second?.count ?? 0) }

  internal func prefix(_ n: Int) -> Self {
    assert(n >= 0)
    if n >= self.count {
      return self
    }
    if n <= first.count {
      return Self(first.prefix(n)._rebased())
    }
    return Self(first, second!.prefix(n - first.count)._rebased())
  }

  internal func suffix(_ n: Int) -> Self {
    assert(n >= 0)
    if n >= self.count {
      return self
    }
    guard let second = second else {
      return Self(first.suffix(n)._rebased())
    }
    if n <= second.count {
      return Self(second.suffix(n)._rebased())
    }
    return Self(first.suffix(n - second.count)._rebased(), second)
  }
}

extension _UnsafeMutableWrappedBuffer {
  internal func deinitialize() {
    first._deinitializeAll()
    second?._deinitializeAll()
  }

  internal func initialize<I: IteratorProtocol>(
    fromPrefixOf iterator: inout I
  ) -> Int
  where I.Element == Element {
    var copied = 0
    var gap = first
    var wrapped = false
    while true {
      if copied == gap.count {
        guard !wrapped, let second = second, second.count > 0 else { break }
        gap = second
        copied = 0
        wrapped = true
      }
      guard let next = iterator.next() else { break }
      (gap.baseAddress! + copied).initialize(to: next)
      copied += 1
    }
    return wrapped ? first.count + copied : copied
  }

  internal func initialize<S: Sequence>(
    fromSequencePrefix elements: __owned S
  ) -> (iterator: S.Iterator, count: Int)
  where S.Element == Element {
    guard second == nil || first.count >= elements.underestimatedCount else {
      var it = elements.makeIterator()
      let copied = initialize(fromPrefixOf: &it)
      return (it, copied)
    }
    // Note: Array._copyContents traps when not given enough space, so we
    // need to check if we have enough contiguous space available above.
    //
    // FIXME: Add suppport for segmented (a.k.a. piecewise contiguous)
    // collections to the stdlib.
    var (it, copied) = elements._copyContents(initializing: first)
    if copied == first.count, let second = second {
      var i = 0
      while i < second.count {
        guard let next = it.next() else { break }
        (second.baseAddress! + i).initialize(to: next)
        i += 1
      }
      copied += i
    }
    return (it, copied)
  }

  internal func initialize<C: Collection>(
    from elements: __owned C
  ) where C.Element == Element {
    assert(self.count == elements.count)
    if let second = second {
      let wrap = elements.index(elements.startIndex, offsetBy: first.count)
      first._initialize(from: elements[..<wrap])
      second._initialize(from: elements[wrap...])
    } else {
      first._initialize(from: elements)
    }
  }

  internal func assign<C: Collection>(
    from elements: C
  ) where C.Element == Element {
    assert(elements.count == self.count)
    deinitialize()
    initialize(from: elements)
  }
}
