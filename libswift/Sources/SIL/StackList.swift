//===--- StackList.swift - defines the StackList data structure -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SILBridging

/// A very efficient implementation of a stack, which can also be iterated over.
///
/// A StackList is the best choice for things like worklists, etc., if no random
/// access is needed.
/// Compared to Array, it does not require any memory allocations, because it
/// uses the bump pointer allocator of the SILModule.
/// All operations have (almost) zero cost.
///
/// Ideally this would be a move-only type. Until then, only pass StackLists as
/// inout!
/// Note: it is required to manually remove all elements - either by pop() or
///       removeAll().
public struct StackList<Element> : Sequence, CustomReflectable {

  private let context: BridgedPassContext
  private var firstSlab = BridgedSlab(data: nil)
  private var lastSlab = BridgedSlab(data: nil)
  private var endIndex: Int = slabCapacity

  private static var slabCapacity: Int {
    BridgedSlabCapacity / MemoryLayout<Element>.size
  }

  private static func bind(_ slab: BridgedSlab) -> UnsafeMutablePointer<Element> {
    return slab.data!.bindMemory(to: Element.self, capacity: StackList.slabCapacity)
  }

  public struct Iterator : IteratorProtocol {
    var slab: BridgedSlab
    var index: Int
    let lastSlab: BridgedSlab
    let endIndex: Int
    
    public mutating func next() -> Element? {
      let end = (slab.data == lastSlab.data ? endIndex : slabCapacity)
      if index < end {
        let elem = StackList.bind(slab)[index]
        index += 1

        if index >= end && slab.data != lastSlab.data {
          slab = PassContext_getNextSlab(slab)
          index = 0
        }
        return elem
      }
      return nil
    }
  }
  
  public init(context: BridgedPassContext) { self.context = context }

  public func makeIterator() -> Iterator {
    return Iterator(slab: firstSlab, index: 0, lastSlab: lastSlab, endIndex: endIndex)
  }

  public var first: Element? {
    if isEmpty {
      return nil
    }
    return StackList.bind(firstSlab)[0]
  }

  public var last: Element? {
    if isEmpty {
      return nil
    }
    return StackList.bind(lastSlab)[endIndex - 1]
  }

  public mutating func push(_ element: Element) {
    if endIndex >= StackList.slabCapacity {
      lastSlab = PassContext_allocSlab(context, lastSlab)
      if firstSlab.data == nil {
        firstSlab = lastSlab
      }
      endIndex = 0
    }
    (StackList.bind(lastSlab) + endIndex).initialize(to: element)
    endIndex += 1
  }
  
  public var isEmpty: Bool { return firstSlab.data == nil }
  
  public mutating func pop() -> Element? {
    if isEmpty {
      return nil
    }
    assert(endIndex > 0)
    endIndex -= 1
    let elem = (StackList.bind(lastSlab) + endIndex).move()
    
    if endIndex == 0 {
      if lastSlab.data == firstSlab.data {
        _ = PassContext_freeSlab(context, lastSlab)
        firstSlab.data = nil
        lastSlab.data = nil
      } else {
        lastSlab = PassContext_freeSlab(context, lastSlab)
      }
      endIndex = StackList.slabCapacity
    }

    return elem
  }
  
  public mutating func removeAll() {
    if isEmpty {
      return
    }
    while lastSlab.data != firstSlab.data {
      lastSlab = PassContext_freeSlab(context, lastSlab)
    }
    _ = PassContext_freeSlab(context, lastSlab)
    firstSlab.data = nil
    lastSlab.data = nil
    endIndex = StackList.slabCapacity
  }
  
  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0) }
    return Mirror(self, children: c)
  }
}
