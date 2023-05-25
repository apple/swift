//===--- Worklist.swift ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// A utility for processing entities in a worklist.
///
/// A `Worklist` is basically a combination of a stack and a set.
/// It can be used for typical worklist-processing algorithms.
///
/// This type should be a move-only type, but unfortunately we don't have move-only
/// types yet. Therefore it's needed to call `deinitialize()` explicitly to
/// destruct this data structure, e.g. in a `defer {}` block.
struct Worklist<Set: IntrusiveSet> : CustomStringConvertible, NoReflectionChildren {
  typealias Element = Set.Element
  private var worklist: Stack<Element>
  private var pushedElements: Set

  init(_ context: some Context) {
    self.worklist = Stack(context)
    self.pushedElements = Set(context)
  }
  
  mutating func pop() -> Element? { return worklist.pop() }

  /// Pop and allow the popped element to be pushed again to the worklist.
  mutating func popAndForget() -> Element? {
    if let element = worklist.pop() {
      pushedElements.erase(element)
      return element
    }
    return nil
  }

  mutating func pushIfNotVisited(_ element: Element) {
    if pushedElements.insert(element) {
      worklist.append(element)
    }
  }

  mutating func pushIfNotVisited<S: Sequence>(contentsOf other: S) where S.Element == Element {
    for element in other {
      pushIfNotVisited(element)
    }
  }

  /// Returns true if \p element was pushed to the worklist, regardless if it's already popped or not.
  func hasBeenPushed(_ element: Element) -> Bool { pushedElements.contains(element) }

  var isEmpty: Bool { worklist.isEmpty }

  var description: String {
    """
    worklist: \(worklist)
    pushed:   \(pushedElements)
    """
  }

  /// TODO: once we have move-only types, make this a real deinit.
  mutating func deinitialize() {
    pushedElements.deinitialize()
    worklist.deinitialize()
  }
}

typealias BasicBlockWorklist = Worklist<BasicBlockSet>
typealias InstructionWorklist = Worklist<InstructionSet>
typealias ValueWorklist = Worklist<ValueSet>
