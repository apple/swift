//===--- PriorityQueue.h - Merging sorted linked lists ----------*- C++ -*-===//
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
//
// This file defines a class that helps with maintaining and merging a
// sorted linked list.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_PRIORITYQUEUE_H
#define SWIFT_BASIC_PRIORITYQUEUE_H

#include <cassert>

namespace swift {

/// A class for priority FIFO queue with a fixed number of priorities.
///
/// The `Node` type parameter represents a reference to a list node.
/// Conceptually, a `Node` value is either null or a reference to an
/// object with an abstract sort value and a `next` reference
/// (another `Node` value).
///
/// A null reference can be created by explicitly default-constructing
/// the `Node` type, e.g. with `Node()`.  Converting a `Node` value
/// contextually to `bool` tests whether the node is a null reference.
/// `Node` values can be compared with the `==` and `!=` operators,
/// and equality with `Node()` is equivalent to a `bool` conversion.
/// These conditions are designed to allow pointer types to be used
/// directly, but they also permit other types.  `ListMerger` is not
/// currently written to support smart pointer types efficiently,
/// however.
///
/// The sort value and `next` reference are not accessed directly;
/// instead, they are accessed with `static` functions on the
/// `NodeTraits` type parameter:
///
/// ```
///   /// Return the current value of the next reference.
///   static Node getNext(Node n);
///
///   /// Set the current value of the next reference.
///   static void setNext(Node n, Node next);
///
///   /// Total number of priority buckets.
///   enum { prioritiesCount = ... };
///
///   /// Returns priority of the Node as value between 0 and `prioritiesCount-1`.
///   /// Smaller indices have higher priority.
///   static int getPriorityIndex(Node);
/// ```
///
/// All nodes are stored in a single linked list, sorted by priority.
/// Within the same priority jobs are sorted in the FIFO order.
///
template <class Node, class NodeTraits>
class PriorityQueue {
private:
  /// Head of the linked list.
  Node head;
  /// Last node of the corresponding priority, or null if no nodes of that
  /// priority exist.
  Node tails[NodeTraits::prioritiesCount];

public:
  PriorityQueue() : head(), tails{} {}

  /// Add a single node to this queue.
  ///
  /// The next reference of the node will be overwritten and does not
  /// need to be meaningful.
  ///
  /// The relative order of the existing nodes in the queue will not change,
  /// and if there are nodes in the current list which compare equal
  /// to the new node, it will be inserted after them.
  void enqueue(Node newNode) {
    assert(newNode && "inserting a null node");
    int priorityIndex = NodeTraits::getPriorityIndex(newNode);
    enqueueRun(priorityIndex, newNode, newNode);
  }

  /// Add a chain of nodes of mixed priorities to this queue.
  void enqueueContentsOf(Node otherHead) {
    if (!otherHead) return;
    Node runHead = otherHead;
    int priorityIndex = NodeTraits::getPriorityIndex(runHead);
    do {
      // Find run of jobs of the same priority
      Node runTail = runHead;
      Node next = NodeTraits::getNext(runTail);
      int nextRunPriorityIndex = badIndex;
      while (next) {
        nextRunPriorityIndex = NodeTraits::getPriorityIndex(next);
        if (nextRunPriorityIndex != priorityIndex) break;
        runTail = next;
        next = NodeTraits::getNext(runTail);
      }

      enqueueRun(priorityIndex, runHead, runTail);
      runHead = next;
      priorityIndex = nextRunPriorityIndex;
    } while(runHead);
  }

  Node dequeue() {
    if (!head) {
      return head;
    }
    auto result = head;
    int resultIndex = NodeTraits::getPriorityIndex(result);
    head = NodeTraits::getNext(result);
    if (!head || resultIndex != NodeTraits::getPriorityIndex(head)) {
      tails[resultIndex] = Node();
    }
    return result;
  }

  Node peek() const { return head; }
  bool empty() const { return !head; }
private:
  // Use large negative value to increase chance of causing segfault if this
  // value ends up being used for indexing. Using -1 would cause accessing
  // `head`, which is less noticeable.
  static const int badIndex = std::numeric_limits<int>::min();

  void enqueueRun(int priorityIndex, Node runHead, Node runTail) {
    for (int i = priorityIndex;; i--) {
      if (i < 0) {
        NodeTraits::setNext(runTail, head);
        head = runHead;
        break;
      }
      if (tails[i]) {
        NodeTraits::setNext(runTail, NodeTraits::getNext(tails[i]));
        NodeTraits::setNext(tails[i], runHead);
        break;
      }
    }
    tails[priorityIndex] = runTail;
  }
};

} // end namespace swift

#endif
