//===--- StorageViewIterator.swift ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if hasFeature(NonescapableTypes)

extension StorageView where Element: ~Copyable /*& ~Escapable*/ {
  @frozen
  public struct Iterator: Copyable, ~Escapable {
    var curPointer: UnsafeRawPointer
    let endPointer: UnsafeRawPointer

    init<Owner: ~Escapable & ~Copyable>(
      startPointer: UnsafeRawPointer,
      endPointer: UnsafeRawPointer,
      owner: borrowing Owner
    ) -> _borrow(owner) Self {
      self.curPointer = startPointer
      self.endPointer = endPointer
      return self
    }

    init<Owner: ~Escapable & ~Copyable>(
      from start: StorageView.Index,
      to end: StorageView.Index,
      owner: borrowing Owner
    ) -> _borrow(owner) Self {
      assert(start._allocation == end._allocation)
      self.init(
        startPointer: start._rawValue, endPointer: end._rawValue, owner: owner
      )
      return self
    }
  }
}

extension StorageView.Iterator where Element: Copyable & Escapable {

  // This is the `IteratorProtocol` requirement, except that
  // StorageView.Iterator does not conform to `Escapable`
  public mutating func next() -> Element? {
    guard curPointer < endPointer else { return nil }
    defer {
      curPointer = curPointer.advanced(by: MemoryLayout<Element>.stride)
    }
    if _isPOD(Element.self) {
      return curPointer.loadUnaligned(as: Element.self)
    }
    return curPointer.load(as: Element.self)
  }
}

extension StorageView.Iterator where Element: _BitwiseCopyable {

  // This is the `IteratorProtocol` requirement, except that
  // StorageView.Iterator does not conform to `Escapable`
  public mutating func next() -> Element? {
    guard curPointer < endPointer else { return nil }
    defer {
      curPointer = curPointer.advanced(by: MemoryLayout<Element>.stride)
    }
    return curPointer.loadUnaligned(as: Element.self)
  }
}

#endif
