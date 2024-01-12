//===--- ListMerger.h - Merging sorted linked lists -------------*- C++ -*-===//
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

#ifndef SWIFT_BASIC_LISTMERGER_H
#define SWIFT_BASIC_LISTMERGER_H

#include <assert.h>
#include <tuple>

namespace swift {

/// A class for building and merging sorted linked lists.
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
///   /// Compare the sort value of this node with that of another
///   /// node, returning negative (<), zero (==), or positive (>).
///   /// A node must compare equal to itself.  A sorted list obeys
///   /// the condition that each node in the list compares <= the next.
///   static int compare(Node lhs, Node rhs);
/// ```
///
/// The merger holds a current list of nodes.  The sort value and
/// next references of nodes must not be accessed after being added
/// to the merger and before being released except by the merger.
template <class Node, class NodeTraits>
class ListMerger {
public:
  class LastInsertionPoint {
    friend class ListMerger;
    Node node = Node();
    bool isKnownLastOfEquals = false;

  public:
    LastInsertionPoint() {}

    void nodeWasRemoved(Node removedNode) {
      if (node == removedNode) {
        *this = LastInsertionPoint();
      }
    }
  };

private:
  Node root;
  LastInsertionPoint lastInsertionPoint;

public:
  /// Construct a merger with the given sorted list as its current list.
  ListMerger(Node initialList = Node(),
             LastInsertionPoint insertionPoint = LastInsertionPoint())
      : root(initialList), lastInsertionPoint(insertionPoint) {}

  /// Add a single node to this merger's current list.
  ///
  /// The next reference of the node will be overwritten and does not
  /// need to be meaningful.
  ///
  /// The relative order of nodes in the current list will not change,
  /// and if there are nodes in the current list which compare equal
  /// to the new node, it will be inserted after them.
  void insert(Node newNode) {
    assert(newNode && "inserting a null node");

    Node prev = Node();
    Node cur = root;
    Node stopper = Node();

    // If we have a previous insertion point, compare against it.
    if (Node lastIP = lastInsertionPoint.node) {
      int comparison = NodeTraits::compare(lastIP, newNode);

      // If it compares equal, put the new node immediately after the
      // last in the sequence of equals that contains it.  This is a
      // common fast path when we're adding many nodes that compare equal.
      if (comparison == 0) {
        lastIP = findLastOfEqualsFromLastIP(lastIP);
        NodeTraits::setNext(newNode, NodeTraits::getNext(lastIP));
        NodeTraits::setNext(lastIP, newNode);
        setLastInsertionPoint(newNode, /*known last of equals*/ true);
        return;

      // If the new node must follow the last insertion node, we can
      // at least start the search there.
      } else if (comparison < 0) {
        lastIP = findLastOfEqualsFromLastIP(lastIP);
        prev = lastIP;
        cur = NodeTraits::getNext(lastIP);

      // Otherwise, we can at least end the search at the last inserted
      // node.
      } else {
        stopper = lastIP;
      }
    }

    // Invariants:
    //   root     == [ ..., prev, cur, ... ]
    //   prev <= newRoot

    // Scan forward looking for either `end` or a node that strictly
    // follows the new node.
    while (cur != stopper && NodeTraits::compare(cur, newNode) <= 0) {
      prev = cur;
      cur = NodeTraits::getNext(cur);
    }

    NodeTraits::setNext(newNode, cur);
    if (prev) {
      NodeTraits::setNext(prev, newNode);
    } else {
      root = newNode;
    }
    setLastInsertionPoint(newNode, /*known last of equals*/ true);
  }

  /// Add a single node to this merger's current list.
  ///
  /// The next reference of the node will be overwritten and does not
  /// need to be meaningful.
  ///
  /// The relative order of nodes in the current list will not change,
  /// and if there are nodes in the current list which compare equal
  /// to the new node, it will be inserted *before* them.
  ///
  /// This is useful for the pattern where nodes are naturally encountered
  /// in the opposite of their desired order in the final list and
  /// need to be reversed.  It generally doesn't make any sense to mix
  /// this with calls to insert or merge on the same merger.
  void insertAtFront(Node newNode) {
    assert(newNode && "inserting a null node");

    auto insertBetween = [newNode, this](Node prev, Node next) {
      if (prev) {
        assert(NodeTraits::getNext(prev) == next);
        assert(NodeTraits::compare(prev, newNode) < 0);
        NodeTraits::setNext(prev, newNode);
      } else {
        assert(root == next);
        root = newNode;
      }

      assert(!next || NodeTraits::compare(newNode, next) <= 0);
      NodeTraits::setNext(newNode, next);
      setLastInsertionPoint(prev, /*known last of equals*/ true);
    };

    Node prev = Node();
    Node cur = root;

    // If we have a previous insertion point, check for the presumed-common
    // case that we're inserting something that should immediately follow it.
    if (auto lastIP = lastInsertionPoint.node) {
      lastIP = findLastOfEqualsFromLastIP(lastIP);

      // Compare against the next node after lastIP, if it exists.
      if (Node nextAfterLastIP = NodeTraits::getNext(lastIP)) {
        int comparison = NodeTraits::compare(nextAfterLastIP, newNode);

        // If the new node compares equal to the next node, insert here.
        if (comparison == 0) {
          insertBetween(lastIP, nextAfterLastIP);
          return;
        }

        // If the new node should follow the next node, start scanning
        // after it.
        if (comparison < 0) {
          prev = nextAfterLastIP;
          cur = NodeTraits::getNext(nextAfterLastIP);
        }

        // Otherwise, we'll need to scan from the beginning.

      // If there is no next node, compare against the previous.
      } else {
        int comparison = NodeTraits::compare(lastIP, newNode);

        // If the new node should follow the last node, we can
        // insert here.
        if (comparison < 0) {
          insertBetween(lastIP, Node());
          return;
        }

        // Otherwise, we'll need to scan from the beginning.
      }
    }

    assert(!prev || NodeTraits::compare(prev, newNode) < 0);

    // Scan forward, looking for a node which the new node must be
    // inserted prior to.
    // Invariant: prev < newNode, if prev exists
    while (cur) {
      // Compare the new node against the current IP.
      int comparison = NodeTraits::compare(cur, newNode);

      // If the new node isn't strictly greater than cur, insert here.
      if (comparison >= 0) break;

      // Otherwise, continue.
      prev = cur;
      cur = NodeTraits::getNext(prev);
    }

    insertBetween(prev, cur);
  }

  /// Add a sorted list of nodes to this merger's current list.
  /// The list must be well-formed (i.e. appropriately terminated).
  ///
  /// The relative order of nodes in both the current and the new list
  /// will not change.  If there are nodes in the current list which
  /// compare equal to nodes in the new list, they will appear before
  /// the new nodes.
  ///
  /// For example, if the current list is `[1@A, 1@B, 2@C]`, and the new
  /// list is `[0@D, 1@E, 2@F]`, the current list after the merge will
  /// be `[0@D, 1@A, 1@B, 1@E, 2@C, 2@F]`.
  void merge(Node rootOfNewList) {
    if (!rootOfNewList) return;

    Node prev = Node();
    Node cur = root;
    Node stopper = Node();

    // If we have a previous insertion point, compare the new root
    // against it.
    if (Node lastIP = lastInsertionPoint.node) {
      int comparison = NodeTraits::compare(lastIP, rootOfNewList);

      // If it compares equal, we've got an insertion point where
      // we can place rootOfNewList: the end of the sequence of
      // equals that includes lastIP.  This is a common fast path
      // when we have many nodes that compare equal.
      if (comparison == 0) {
        lastIP = findLastOfEqualsFromLastIP(lastIP);
        prev = lastIP;
        cur = NodeTraits::getNext(lastIP);
        goto foundInsertionPoint; // seems to be the best option

      // If the new node must follow the last insertion point, we can
      // at least start the search there.
      } else if (comparison < 0) {
        lastIP = findLastOfEqualsFromLastIP(lastIP);
        prev = lastIP;
        cur = NodeTraits::getNext(lastIP);

      // Otherwise, we can end the initial search at that position.
      } else {
        stopper = lastIP;
      }
    }

    while (rootOfNewList) {
      // Invariants:
      //   root == [ ..., prev, cur, ... ]
      //   prev <= rootOfNewList

      // Check if the position between prev and cur is where we should
      // insert the root of the new list.
      if (cur != stopper && NodeTraits::compare(cur, rootOfNewList) <= 0) {
        prev = cur;
        cur = NodeTraits::getNext(cur);
        continue;
      }

      // Place rootOfNewList at this position.  Note that this might not be
      // a proper splice because there may be nodes following prev that
      // are now no longer reflected in the existing list.
      if (!prev) {
        root = rootOfNewList;
      } else {
      foundInsertionPoint:
        NodeTraits::setNext(prev, rootOfNewList);
      }

      // If we've run out of nodes in the existing list, it *is*
      // a proper splice, and we're done.
      if (!cur) {
        assert(!stopper);
        setLastInsertionPoint(rootOfNewList, /*known end of equals*/ false);
        return;
      }

      // If not, scan forward in the new list looking for a node that
      // cur should precede.
      Node prevInNewList = rootOfNewList;
      Node curInNewList = NodeTraits::getNext(rootOfNewList);
      while (curInNewList && NodeTraits::compare(cur, curInNewList) > 0) {
        prevInNewList = curInNewList;
        curInNewList = NodeTraits::getNext(curInNewList);
      }

      // prevInNewList < cur <= curInNewList (if it exists)

      // Turn this:
      //   root           == [ ..., prev, cur, ... ]
      //   rootOfNewList  == [ ..., prevInNewList, curInNewList, ... ]
      // into:
      //   root           == [ ..., prev, rootOfNewList, ..., prevInNewList,
      //                       cur, ... ]
      //   rootOfNewList' == [ curInNewList, ... ]
      //
      // Note that the next insertion point we'll check is *after* cur,
      // since we know that cur <= curInNewList.

      NodeTraits::setNext(prevInNewList, cur);
      rootOfNewList = curInNewList;
      prev = cur;
      cur = NodeTraits::getNext(cur);

      setLastInsertionPoint(prevInNewList, /*known end of equals*/ true);

      // Any stopper we have was only known to exceed the original root
      // node of the new list, which we've now inserted.  From now on,
      // we'll need to scan to the end of the list.
      stopper = Node();
    }
  }

  /// Get the current list that's been built up, and clear the internal
  /// state of this merger.
  std::tuple<Node, LastInsertionPoint> release() {
    auto result = std::make_tuple(root, lastInsertionPoint);
    root = Node();
    lastInsertionPoint = LastInsertionPoint();
    return result;
  }

private:
  /// Set the last point at which we inserted a node, and specify
  /// whether we know it was the last in its sequence of equals.
  void setLastInsertionPoint(Node lastIP, bool knownEndOfEquals) {
    lastInsertionPoint.node = lastIP;
    lastInsertionPoint.isKnownLastOfEquals = knownEndOfEquals;
  }

  /// Given the value of lastInsertionPoint (passed in to avoid
  /// reloading it), find the last node in the sequence of equals that
  /// contains it.
  Node findLastOfEqualsFromLastIP(Node lastIP) const {
    assert(lastIP == lastInsertionPoint.node);
    if (!lastInsertionPoint.isKnownLastOfEquals)
      return findLastOfEquals(lastIP);
    return lastIP;
  }

  /// Find the last node in the sequence of equals that contains `node`.
  static Node findLastOfEquals(Node node) {
    while (Node next = NodeTraits::getNext(node)) {
      int comparison = NodeTraits::compare(node, next);
      assert(comparison <= 0 && "list is out of order");
      if (comparison < 0) break;
      node = next;
    }
    return node;
  }
};

} // end namespace swift

#endif
