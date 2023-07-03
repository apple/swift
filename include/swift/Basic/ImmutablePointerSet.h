//===--- ImmutablePointerSet.h ----------------------------------*- C++ -*-===//
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
///
/// \file
///
/// This file contains an implementation of a bump ptr allocated immutable
/// pointer set.
///
/// The target of this data structure are sets of pointers (with N < 100) that
/// are propagated through many basic blocks. These pointer sets will be merged
/// and copied far more than being created from an array.
///
/// Thus we assume the following constraints:
///
/// 1. Our set operations are purely additive. Given a set, one can only add
/// elements to it. One can not remove elements to it. This means we only
/// support construction of sets from arrays and concatenation of pointer sets.
///
/// 2. Our sets must always be ordered and be able to be iterated over
/// efficiently in that order.
///
/// 3. An O(log(n)) set contains method.
///
/// Beyond these constraints, we would like for our data structure to have the
/// following properties for performance reasons:
///
/// 1. Its memory should be bump ptr allocated. We like fast allocation.
///
/// 2. No destructors need to be called when the bump ptr allocator is being
/// destroyed. We like fast destruction and do not want to have to iterate over
/// potentially many of these sets and invoke destructors.
///
/// Thus our design is to represent our sets as bump ptr allocated arrays whose
/// elements are sorted and uniqued. The actual uniquing of the arrays
/// themselves is performed via folding set node.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_IMMUTABLEPOINTERSET_H
#define SWIFT_BASIC_IMMUTABLEPOINTERSET_H

#include "swift/Basic/STLExtras.h"
#include "swift/Basic/NullablePtr.h"
#include "llvm/Support/Allocator.h"
#include "llvm/ADT/FoldingSet.h"
#include <algorithm>
#include <type_traits>

namespace swift {

template <typename PtrTy> class ImmutablePointerSetFactory;

/// An immutable set of pointers. It is backed by a tail allocated sorted array
/// ref.
template <typename T> class ImmutablePointerSet : public llvm::FoldingSetNode {
  using PtrTy = typename std::add_pointer<T>::type;
  friend class ImmutablePointerSetFactory<T>;

  NullablePtr<ImmutablePointerSetFactory<T>> ParentFactory;
  llvm::ArrayRef<PtrTy> Data;

  ImmutablePointerSet(ImmutablePointerSetFactory<T> *ParentFactory,
                      llvm::ArrayRef<PtrTy> NewData)
      : ParentFactory(ParentFactory), Data(NewData) {}

public:
  ~ImmutablePointerSet() = default;
  ImmutablePointerSet(const ImmutablePointerSet &) = default;
  ImmutablePointerSet(ImmutablePointerSet &&) = default;

  ImmutablePointerSet &operator=(const ImmutablePointerSet &) = default;
  ImmutablePointerSet &operator=(ImmutablePointerSet &&) = default;

  bool operator==(const ImmutablePointerSet<T> &P) const {
    // If this and P have different sizes, we can not be equivalent.
    if (size() != P.size())
      return false;

    // Ok, we now know that both have the same size. If one is empty, the other
    // must be as well, implying equality.
    if (empty())
      return true;

    // Ok, both sets are not empty and the same number of elements. Compare
    // element wise.
    return std::equal(begin(), end(), P.begin());
  }

  bool operator!=(const ImmutablePointerSet<T> &P) const {
    return !(*this == P);
  }

  unsigned count(PtrTy Ptr) const {
    // This returns the first element >= Ptr. Since we know that our array is
    // sorted and uniqued, Ptr must be that element.
    auto LowerBound = std::lower_bound(begin(), end(), Ptr);

    // If Ptr is > than everything in the array, then we obviously have 0.
    if (LowerBound == end())
      return 0;

    // Then check if Ptr is > or Ptr is ==. We only have Ptr if we have == to.
    return *LowerBound == Ptr;
  }

  using iterator = typename llvm::ArrayRef<PtrTy>::iterator;
  iterator begin() const { return Data.begin(); }
  iterator end() const { return Data.end(); }

  unsigned size() const { return Data.size(); }
  bool empty() const { return Data.empty(); }

  void Profile(llvm::FoldingSetNodeID &ID) const {
    assert(!Data.empty() && "Should not profile empty ImmutablePointerSet");
    for (PtrTy P : Data) {
      ID.AddPointer(P);
    }
  }

  ImmutablePointerSet<T> *merge(ImmutablePointerSet<T> *Other) {
    if (empty())
      return Other;
    if (Other->empty())
      return this;
    assert(Other->ParentFactory.get() == ParentFactory.get());
    return ParentFactory.get()->merge(this, Other);
  }

  bool hasEmptyIntersection(const ImmutablePointerSet<T> *Other) const {
    // If we are empty or Other is empty, then there are automatically no
    // elements that could be in the intersection. Return true.
    if (empty() || Other->empty())
      return true;

    // Ok, at this point we know that both self and Other are non-empty. They
    // can only have a non-empty intersection if they have elements in common.
    // Sadly it seems the STL does not have such a predicate that is
    // non-constructive in the algorithm library, so we implement it ourselves.
    auto LHSI = begin();
    auto LHSE = end();
    auto RHSI = Other->begin();
    auto RHSE = Other->end();

    // Our implementation is to perform a sorted merge like traversal of both
    // lists, always advancing the iterator with a smaller value. If we ever hit
    // an equality in between our iterators, we have a non-empty intersection.
    //
    // Until either of our iterators hits the end of our target arrays...
    while (LHSI != LHSE && RHSI != RHSE) {
      // If LHSI is equivalent to RHSI, then we have a non-empty intersection...
      // Early exit.
      if (*LHSI == *RHSI)
        return false;

      // Otherwise, if *LHSI is less than *RHSI, advance LHSI.
      if (*LHSI < *RHSI) {
        ++LHSI;
        continue;
      }

      // Otherwise, We know that *RHSI < *LHSI. Advance RHSI.
      ++RHSI;
    }

    // We did not have any overlapping intersection.
    return true;
  }
};

template <typename T> class ImmutablePointerSetFactory {
  using PtrTy = typename std::add_pointer<T>::type;

  using PtrSet = ImmutablePointerSet<T>;
  // This is computed out-of-line so that ImmutablePointerSetFactory is
  // treated as a complete type.
  static const unsigned AllocAlignment;

  llvm::BumpPtrAllocator &Allocator;
  llvm::FoldingSetVector<PtrSet> Set;
  static PtrSet EmptyPtrSet;

public:
  ImmutablePointerSetFactory(llvm::BumpPtrAllocator &A) : Allocator(A), Set() {}
  ImmutablePointerSetFactory(const ImmutablePointerSetFactory &) = delete;
  ImmutablePointerSetFactory(ImmutablePointerSetFactory &&) = delete;
  ImmutablePointerSetFactory &
  operator=(const ImmutablePointerSetFactory &) = delete;
  ImmutablePointerSetFactory &operator=(ImmutablePointerSetFactory &&) = delete;

  // We use a sentinel value here so that we can create an empty value
  // statically.
  static PtrSet *getEmptySet() { return &EmptyPtrSet; }

  void clear() { Set.clear(); }

  /// Given a sorted and uniqued list \p Array, return the ImmutablePointerSet
  /// containing Array. Asserts if \p Array is not sorted and uniqued.
  PtrSet *get(llvm::ArrayRef<PtrTy> Array) {
    if (Array.empty())
      return ImmutablePointerSetFactory::getEmptySet();

    // We expect our users to sort/unique the input array. This is because doing
    // it here would either require us to allocate more memory than we need or
    // write into the input Array, which we don't want.
    assert(is_sorted_and_uniqued(Array));

    llvm::FoldingSetNodeID ID;
    for (PtrTy Ptr : Array) {
      ID.AddPointer(Ptr);
    }

    void *InsertPt;
    if (auto *PSet = Set.FindNodeOrInsertPos(ID, InsertPt)) {
      return PSet;
    }

    size_t NumElts = Array.size();
    size_t MemSize = sizeof(PtrSet) + sizeof(PtrTy) * NumElts;

    // Allocate the memory.
    auto *Mem =
        reinterpret_cast<PtrSet *>(Allocator.Allocate(MemSize, AllocAlignment));

    // Copy in the pointers into the tail allocated memory. We do not need to do
    // any sorting/uniquing ourselves since we assume that our users perform
    // this task for us.
    llvm::MutableArrayRef<PtrTy> DataMem(reinterpret_cast<PtrTy *>(&Mem[1]),
                                         NumElts);
    std::copy(Array.begin(), Array.end(), DataMem.begin());

    // Allocate the new node and insert it into the Set.
    auto *NewNode = new (Mem) PtrSet(this, DataMem);
    Set.InsertNode(NewNode, InsertPt);
    return NewNode;
  }

  PtrSet *merge(PtrSet *S1, llvm::ArrayRef<PtrTy> S2) {
    if (S1->empty())
      return get(S2);

    if (S2.empty())
      return S1;

    // We assume that S2 is sorted and uniqued.
    assert(is_sorted_and_uniqued(S2));

    // If S1 and S2 have the same size, do a quick check to see if they
    // equal. If so, we can bail early and just return S1.
    if (S1->size() == S2.size() &&
        std::equal(S1->begin(), S1->end(), S2.begin()))
      return S1;

    llvm::FoldingSetNodeID ID;

    // We know that both of our pointer sets are sorted, so we can essentially
    // perform a sorted set merging algorithm to create the ID. We also count
    // the number of unique elements for allocation purposes.
    unsigned NumElts = 0;
    set_union_for_each(*S1, S2, [&ID, &NumElts](const PtrTy Ptr) -> void {
      ID.AddPointer(Ptr);
      NumElts++;
    });

    // If we find our ID then continue.
    void *InsertPt;
    if (auto *PSet = Set.FindNodeOrInsertPos(ID, InsertPt)) {
      return PSet;
    }

    unsigned MemSize = sizeof(PtrSet) + sizeof(PtrTy) * NumElts;

    // Allocate the memory.
    auto *Mem =
        reinterpret_cast<PtrSet *>(Allocator.Allocate(MemSize, AllocAlignment));

    // Copy in the union of the two pointer sets into the tail allocated
    // memory. Since we know that our sorted arrays are uniqued, we can use
    // set_union to get the uniqued sorted array that we want.
    llvm::MutableArrayRef<PtrTy> DataMem(reinterpret_cast<PtrTy *>(&Mem[1]),
                                         NumElts);
    std::set_union(S1->begin(), S1->end(), S2.begin(), S2.end(),
                   DataMem.begin());

    // Allocate the new node, insert it into the Set, and return it.
    auto *NewNode = new (Mem) PtrSet(this, DataMem);
    Set.InsertNode(NewNode, InsertPt);
    return NewNode;
  }

  PtrSet *merge(PtrSet *S1, PtrSet *S2) {
    // If either S1 or S2 are the empty PtrSet, just return S2 or S1.
    if (S1->empty())
      return S2;
    if (S2->empty())
      return S1;

    // We know that all of our PtrSets are uniqued. So if S1 and S2 are the same
    // set, their pointers must also be the same set. In such a case, we return
    // early returning S1 without any loss of generality.
    if (S1 == S2)
      return S1;

    llvm::FoldingSetNodeID ID;

    // We know that both of our pointer sets are sorted, so we can essentially
    // perform a sorted set merging algorithm to create the ID. We also count
    // the number of unique elements for allocation purposes.
    unsigned NumElts = 0;
    set_union_for_each(*S1, *S2, [&ID, &NumElts](const PtrTy Ptr) -> void {
      ID.AddPointer(Ptr);
      NumElts++;
    });

    // If we find our ID then continue.
    void *InsertPt;
    if (auto *PSet = Set.FindNodeOrInsertPos(ID, InsertPt)) {
      return PSet;
    }

    unsigned MemSize = sizeof(PtrSet) + sizeof(PtrTy) * NumElts;

    // Allocate the memory.
    auto *Mem =
        reinterpret_cast<PtrSet *>(Allocator.Allocate(MemSize, AllocAlignment));

    // Copy in the union of the two pointer sets into the tail allocated
    // memory. Since we know that our sorted arrays are uniqued, we can use
    // set_union to get the uniqued sorted array that we want.
    llvm::MutableArrayRef<PtrTy> DataMem(reinterpret_cast<PtrTy *>(&Mem[1]),
                                         NumElts);
    std::set_union(S1->begin(), S1->end(), S2->begin(), S2->end(),
                   DataMem.begin());

    // Allocate the new node, insert it into the Set, and return it.
    auto *NewNode = new (Mem) PtrSet(this, DataMem);
    Set.InsertNode(NewNode, InsertPt);
    return NewNode;
  }
};

template <typename T>
ImmutablePointerSet<T> ImmutablePointerSetFactory<T>::EmptyPtrSet =
    ImmutablePointerSet<T>(nullptr, {});

template <typename T>
#if !defined(_MSC_VER) || defined(__clang__)
constexpr
#endif
const unsigned ImmutablePointerSetFactory<T>::AllocAlignment =
    (alignof(PtrSet) > alignof(PtrTy)) ? alignof(PtrSet) : alignof(PtrTy);

} // end swift namespace

#endif // SWIFT_BASIC_IMMUTABLEPOINTERSET_H
