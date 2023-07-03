//===--- SuccessorMap.h - Find the first mapped successor -------*- C++ -*-===//
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
// A data structure which maps from a discrete ordered domain (e.g.
// 'unsigned') to an arbitrary value type.  It provides two core
// operations:
//
//   - setting a value for an unmapped key
//   - find the value for the smallest mapped key that is larger than a
//     given unmapped key
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_SUCCESSORMAP_H
#define SWIFT_BASIC_SUCCESSORMAP_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

namespace swift {

/// Traits for a key type.  The default implementation is suitable for
/// a fundamental discrete type like 'unsigned'.
template <class T> struct SuccessorMapTraits {
  static bool equals(const T &lhs, const T &rhs) { return lhs == rhs; }
  static bool precedes(const T &lhs, const T &rhs) { return lhs < rhs; }
  static T getSuccessor(const T &value) { return value + 1; }
};

/// A successor map.  Not a STL-style map.
template <class K, class V, class Traits = SuccessorMapTraits<K> >
class SuccessorMap {
  struct Node {
    Node(K &&key, V &&value)
      : Begin(std::move(key)),
        End(Traits::getSuccessor(Begin)),
        Value(std::move(value)) {}

    Node(const K &key, const V &value)
      : Begin(key),
        End(Traits::getSuccessor(Begin)),
        Value(value) {}

    Node *Left = nullptr;
    Node *Right = nullptr;

    /// A half-open range.
    K Begin, End;
    V Value;

    SWIFT_DEBUG_DUMP {
      dumpNode(this);
    }
  };

  // The entire tree is uniquely owned by the map object.
  Node *Root = nullptr;

public:
  SuccessorMap() {}

  SuccessorMap(SuccessorMap &&other) : Root(other.Root) {
    other.Root = nullptr;
  }
  SuccessorMap &operator=(SuccessorMap &&other) {
    clear();
    Root = other.Root;
    other.Root = nullptr;
    return *this;
  }

  SuccessorMap(const SuccessorMap &other) : Root(copyTree(other.Root)) {}
  SuccessorMap &operator=(const SuccessorMap &other) {
    // TODO: this is clearly optimizable to re-use nodes.
    deleteTree(Root);
    Root = copyTree(other.Root);
    return *this;
  }

  ~SuccessorMap() {
    deleteTree(Root);
  }

  void clear() {
    deleteTree(Root);
    Root = nullptr;
  }

  template <class KeyTy, class ValueTy>
  void insert(KeyTy &&key, ValueTy &&value) {
    // Splay to find the greatest lower and least upper bounds.
    bool haveUpperBound = splay(key);
    Node *upperBound = (haveUpperBound ? Root : nullptr);
    Node *lowerBound = (haveUpperBound ? Root->Left : Root);
    assert(haveUpperBound == (upperBound != nullptr));
    assert(!lowerBound || !lowerBound->Right);

    // Try to add this key to the end of the lower bound.
    assert(!upperBound || Traits::precedes(key, upperBound->Begin));
    assert(!lowerBound || !Traits::precedes(key, lowerBound->End));

    // If the key is the end of the left child, append to it,
    // dropping the inserted value on the floor.
    if (lowerBound && Traits::equals(lowerBound->End, key)) {
      lowerBound->End = Traits::getSuccessor(lowerBound->End);

      // If the end of the lower bound is now the same as the
      // beginning of the upper bound, combine the nodes.
      if (upperBound && Traits::equals(lowerBound->End, upperBound->Begin)) {
        lowerBound->End = std::move(upperBound->End);
        lowerBound->Right = upperBound->Right;
        assert(upperBound->Left == lowerBound);
        Root = lowerBound;
        delete upperBound;
      }
      return;
    }

    // Otherwise, if the key immediately precedes the beginning of the
    // upper bound, prepend to it.
    auto keySuccessor = Traits::getSuccessor(key);
    if (upperBound && Traits::equals(keySuccessor, upperBound->Begin)) {
      upperBound->Begin = std::move(keySuccessor);
      upperBound->Value = std::forward<ValueTy>(value);
      return;
    }

    // Otherwise, create a new node.
    Root = new Node(std::forward<KeyTy>(key), std::forward<ValueTy>(value));
    Root->Left = lowerBound;
    Root->Right = upperBound;
    if (upperBound) upperBound->Left = nullptr;
  }

  /// Find the address of the stored value corresponding to the
  /// smallest key larger than the given one, or return a null pointer
  /// if the key is larger than anything in the map.
  V *findLeastUpperBound(const K &key) {
    if (splay(key)) {
      return &Root->Value;
    } else {
      return nullptr;
    }
  }

  /// Validate the well-formedness of this data structure.
  void validate() const {
#ifndef NDEBUG
    if (Root)
      validateNode(Root, llvm::None, llvm::None);
#endif
  }

  SWIFT_DEBUG_DUMP {
    if (Root) dumpNode(Root);
    else llvm::errs() << "(empty)\n";
  }

private:
  /// Perform a top-down splay operation, attempting to set things up
  /// so that Root is the least upper bound and its left child is the
  /// greatest lower bound.  The only time that's not satisfiable is
  /// if the key is larger than anything in the map.
  ///
  /// We assume that the key is not mapped.
  ///
  /// \return true if the root is now the least upper bound and its
  ///   left child (if present) is the greatest lower bound
  bool splay(const K &key) {
    if (!Root) return false;

    // The root of the current subtree.
    Node *cur = Root;

    // The root of the tree of nodes that are larger than the current
    // subtree, and the address of the empty slot on its far left arm.
    Node *upperTree = nullptr;
    Node **upperLeftmost = &upperTree;

    // The root of the tree of nodes that are smaller than the current
    // subtrees, and the address of the empty slot on its far right arm.
    // As an invariant, this tree is always either empty or has no right
    // subtree.
    Node *lowerTree = nullptr;
    Node **lowerRightmost = &lowerTree;

    // Rotate a node in to become the new root of the lower tree.
    // Its right child must be clear.
    auto rotateAsLowerRoot = [&](Node *node) {
      assert(*lowerRightmost == nullptr);
      // The left child goes in the rightmost position of the old lower tree.
      // The right child gets dropped, and its position is the new rightmost
      // position.
      *lowerRightmost = node->Left;
      lowerRightmost = &node->Right;
      node->Left = lowerTree;
      node->Right = nullptr;
      lowerTree = node;
    };

    // Put a node in the leftmost position of the upper tree.
    auto placeInUpperLeftmost = [&](Node *node) {
      assert(*upperLeftmost == nullptr);
      assert(node->Left == nullptr);
      *upperLeftmost = node;
      upperLeftmost = &node->Left;
    };

    // A helper function to re-assemble the root node.  Tail-called on
    // all exit paths from splay().
    auto reassemble = [&](bool foundUpperBound) {
      assert(*lowerRightmost == nullptr);
      assert(*upperLeftmost == nullptr);
      *lowerRightmost = cur->Left;
      cur->Left = lowerTree;
      *upperLeftmost = cur->Right;
      cur->Right = upperTree;
      Root = cur;
      assert(!foundUpperBound ||
             Root->Left == nullptr ||
             Root->Left->Right == nullptr);
      return foundUpperBound;
    };

    // A helper to finish the operation, given that 'cur' is an upper bound.
    auto finishWithUpperBound = [&] {
      assert(cur->Left == nullptr);
      return reassemble(true);
    };

    // A helper to finish the operation, given that there is no upper
    // bound in the 'cur' subtree.
    auto finishWithoutUpperBound = [&] {
      assert(cur->Right == nullptr);

      // If the upper tree is empty, we really don't have an upper bound.
      if (!upperTree) return reassemble(false);

      // Otherwise, pull the leftmost leaf off the upper tree to
      // become the new root.
      Node **leafPosition = &upperTree;
      while ((*leafPosition)->Left) {
        leafPosition = &(*leafPosition)->Left;
      }
      Node *newRoot = *leafPosition;
      *leafPosition = newRoot->Right;
      newRoot->Right = nullptr;

      // Adjust upperLeftmost.
      while (*leafPosition) leafPosition = &(*leafPosition)->Left;
      upperLeftmost = leafPosition;
      rotateAsLowerRoot(cur);
      cur = newRoot;
      return finishWithUpperBound();
    };

    while (true) {
      assert(cur);
      assert(lowerTree != nullptr || lowerRightmost == &lowerTree);
      assert(lowerTree == nullptr || lowerRightmost == &lowerTree->Right);
      assert(*lowerRightmost == nullptr);
      assert(*upperLeftmost == nullptr);

      // Check if we should recurse into the left subtree.
      if (Traits::precedes(key, cur->Begin)) {
        // We should.  If the left subtree is empty, then 'cur' is our
        // least upper bound.
        auto left = cur->Left;
        if (!left) return finishWithUpperBound();

        // Otherwise, check if we should recurse into the left-left subtree.
        if (Traits::precedes(key, left->Begin)) {
          // We should.  If the left-left subtree is empty, then 'left'
          // is our least upper bound.  Zig left.
          auto leftLeft = left->Left;
          if (!leftLeft) {
            cur->Left = nullptr;
            placeInUpperLeftmost(cur);
            cur = left;
            return finishWithUpperBound();
          }

          // Otherwise, zig-zig left.
          cur->Left = left->Right;
          left->Right = cur;
          left->Left = nullptr;
          placeInUpperLeftmost(left);
          cur = leftLeft;
          continue;
        }
        assert(!Traits::precedes(key, left->End) && "key already mapped!");

        // We should recurse into the left-right subtree.  In either
        // case, break off 'left' as the new root of the lower-bound tree.
        auto leftRight = left->Right;
        rotateAsLowerRoot(left);
        cur->Left = nullptr;

        // If the left-right subtree is empty, then 'cur' is our least
        // upper bound.
        if (!leftRight) return finishWithUpperBound();

        // Otherwise, complete the zig-zag left and continue.
        placeInUpperLeftmost(cur);
        cur = leftRight;
        continue;
      }
      assert(!Traits::precedes(key, cur->End) && "key already mapped!");

      // We should recurse into the right subtree. If that's empty,
      // we're done, and the subtree has no upper bound for the key.
      auto right = cur->Right;
      if (!right) return finishWithoutUpperBound();

      // Check whether we should recurse into the right-left subtree.
      if (Traits::precedes(key, right->Begin)) {
        // We should.  In either case, we need to rotate 'cur' to
        // become the new root of the lower tree.
        rotateAsLowerRoot(cur);

        // If the right-left subtree is empty, then 'right' is the
        // least upper bound.  Zig right.
        auto rightLeft = right->Left;
        if (!rightLeft) {
          cur = right;
          return finishWithUpperBound();
        }

        // Otherwise, complete the zig-zag right and continue.
        right->Left = nullptr;
        placeInUpperLeftmost(right);
        cur = rightLeft;
        continue;
      }
      assert(!Traits::precedes(key, right->End) && "key already mapped!");

      // We should recurse into the right-right subtree.  If that's
      // empty, we're done, and the subtree has no upper bound for the
      // key.  Zig right.
      auto rightRight = right->Right;
      if (!rightRight) {
        rotateAsLowerRoot(cur);
        cur = right;
        return finishWithoutUpperBound();
      }

      // Otherwise, zig-zig right and continue.
      cur->Right = right->Left;
      right->Left = cur;
      rotateAsLowerRoot(right);
      cur = rightRight;
      continue;
    }
  }

#ifndef NDEBUG
  /// Validate that the node is well-formed and that all of its keys
  /// (and those of its children) fall (non-inclusively) between
  /// lowerBound and upperBound-1.
  static void validateNode(Node *node, llvm::Optional<K> lowerBound,
                           llvm::Optional<K> upperBound) {
    // The node cannot have an empty key range.
    assert(Traits::precedes(node->Begin, node->End));

    // The first key must be strictly higher than the lower bound.
    if (lowerBound.has_value())
      assert(Traits::precedes(lowerBound.value(), node->Begin));

    // The last key (i.e. End-1) must be strictly lower than
    // upperBound-1, or in other words, End must precede upperBound.
    if (upperBound.has_value())
      assert(Traits::precedes(node->End, upperBound.value()));

    // The keys in the left sub-tree must all be strictly less than
    // Begin-1, because if any key equals Begin-1, that node should
    // have been merged into this one.
    if (node->Left)
      validateNode(node->Left, lowerBound, node->Begin);

    // The keys in the right sub-tree must all be strictly greater
    // than End, because if any key equals End, that node should have
    // been merged into this one.
    if (node->Right)
      validateNode(node->Right, node->End, upperBound);
  }
#endif

  static void dumpNode(const Node *node) {
    dumpNode(node, 0);
  }

  static void dumpNode(const Node *node, unsigned indent) {
    llvm::errs().indent(indent);
    if (!node) {
      llvm::errs() << "(null)\n";
      return;
    }

    llvm::errs() << node->Begin << ".." << node->End
                 << ": " << node->Value << "\n";
    dumpNode(node->Left, indent + 2);
    dumpNode(node->Right, indent + 2);
  }

  /// Delete all the nodes in the given sub-tree.
  static void deleteTree(Node *root) {
    llvm::SmallVector<Node*, 16> queue; // actually a stack
    auto enqueue = [&](Node *n) {
      if (n) queue.push_back(n);
    };

    enqueue(root);
    while (!queue.empty()) {
      auto cur = queue.pop_back_val();
      enqueue(cur->Left);
      enqueue(cur->Right);
      delete cur;
    }
  }

  static Node *copyTree(Node *oldRoot) {
    // A list of nodes which have been cloned, but whose children
    // haven't yet been cloned.
    llvm::SmallVector<Node*, 16> worklist;

    auto cloneAtPosition = [&](Node *&position) {
      Node *oldNode = position;
      if (!oldNode) return;
      Node *newNode = new Node(*oldNode);
      position = newNode;
      worklist.push_back(newNode);
    };

    Node *newRoot = oldRoot;
    cloneAtPosition(newRoot);
    while (!worklist.empty()) {
      auto node = worklist.pop_back_val();
      cloneAtPosition(node->Left);
      cloneAtPosition(node->Right);
    }

    return newRoot;
  }
};

} // end namespace swift

#endif // SWIFT_BASIC_SUCCESSORMAP_H
