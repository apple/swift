//===--- Concurrent.h - Concurrent Data Structures  -------------*- C++ -*-===//
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
#ifndef SWIFT_RUNTIME_CONCURRENTUTILS_H
#define SWIFT_RUNTIME_CONCURRENTUTILS_H
#include <iterator>
#include <algorithm>
#include <atomic>
#include <functional>
#include <stdint.h>
#include <vector>
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/Allocator.h"
#include "Atomic.h"
#include "Debug.h"
#include "Mutex.h"

#if defined(__FreeBSD__) || defined(__CYGWIN__) || defined(__HAIKU__)
#include <stdio.h>
#endif

#if defined(__APPLE__) && defined(__MACH__)
#include <malloc/malloc.h>
#endif

namespace swift {

/// This is a node in a concurrent linked list.
template <class ElemTy> struct ConcurrentListNode {
  ConcurrentListNode(ElemTy Elem) : Payload(Elem), Next(nullptr) {}
  ConcurrentListNode(const ConcurrentListNode &) = delete;
  ConcurrentListNode &operator=(const ConcurrentListNode &) = delete;

  /// The element.
  ElemTy Payload;
  /// Points to the next link in the chain.
  ConcurrentListNode<ElemTy> *Next;
};

/// This is a concurrent linked list. It supports insertion at the beginning
/// of the list and traversal using iterators.
/// This is a very simple implementation of a concurrent linked list
/// using atomic operations. The 'push_front' method allocates a new link
/// and attempts to compare and swap the old head pointer with pointer to
/// the new link. This operation may fail many times if there are other
/// contending threads, but eventually the head pointer is set to the new
/// link that already points to the old head value. Notice that the more
/// difficult feature of removing links is not supported.
/// See 'push_front' for more details.
template <class ElemTy> struct ConcurrentList {
  ConcurrentList() : First(nullptr) {}
  ~ConcurrentList() {
    clear();
  }

  /// Remove all of the links in the chain. This method leaves
  /// the list at a usable state and new links can be added.
  /// Notice that this operation is non-concurrent because
  /// we have no way of ensuring that no one is currently
  /// traversing the list.
  void clear() {
    // Iterate over the list and delete all the nodes.
    auto Ptr = First.load(std::memory_order_acquire);
    First.store(nullptr, std:: memory_order_release);

    while (Ptr) {
      auto N = Ptr->Next;
      delete Ptr;
      Ptr = N;
    }
  }

  ConcurrentList(const ConcurrentList &) = delete;
  ConcurrentList &operator=(const ConcurrentList &) = delete;

  /// A list iterator.
  struct ConcurrentListIterator :
      public std::iterator<std::forward_iterator_tag, ElemTy> {

    /// Points to the current link.
    ConcurrentListNode<ElemTy> *Ptr;
    /// C'tor.
    ConcurrentListIterator(ConcurrentListNode<ElemTy> *P) : Ptr(P) {}
    /// Move to the next element.
    ConcurrentListIterator &operator++() {
      Ptr = Ptr->Next;
      return *this;
    }
    /// Access the element.
    ElemTy &operator*() { return Ptr->Payload; }
    /// Same?
    bool operator==(const ConcurrentListIterator &o) const {
      return o.Ptr == Ptr;
    }
    /// Not the same?
    bool operator!=(const ConcurrentListIterator &o) const {
      return o.Ptr != Ptr;
    }
  };

  /// Iterator entry point.
  typedef ConcurrentListIterator iterator;
  /// Marks the beginning of the list.
  iterator begin() const {
    return ConcurrentListIterator(First.load(std::memory_order_acquire));
  }
  /// Marks the end of the list.
  iterator end() const { return ConcurrentListIterator(nullptr); }

  /// Add a new item to the list.
  void push_front(ElemTy Elem) {
    /// Allocate a new node.
    ConcurrentListNode<ElemTy> *N = new ConcurrentListNode<ElemTy>(Elem);
    // Point to the first element in the list.
    N->Next = First.load(std::memory_order_acquire);
    auto OldFirst = N->Next;
    // Try to replace the current First with the new node.
    while (!std::atomic_compare_exchange_weak_explicit(&First, &OldFirst, N,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
      // If we fail, update the new node to point to the new head and try to
      // insert before the new
      // first element.
      N->Next = OldFirst;
    }
  }

  /// Points to the first link in the list.
  std::atomic<ConcurrentListNode<ElemTy> *> First;
};

/// A utility function for ordering two integers, which is useful
/// for implementing compareWithKey.
template <class T>
static inline int compareIntegers(T left, T right) {
  return (left == right ? 0 : left < right ? -1 : 1);
}

/// A utility function for ordering two pointers, which is useful
/// for implementing compareWithKey.
template <class T>
static inline int comparePointers(const T *left, const T *right) {
  return (left == right ? 0 : std::less<const T *>()(left, right) ? -1 : 1);
}

template <class EntryTy, bool ProvideDestructor, class Allocator>
class ConcurrentMapBase;

/// The partial specialization of ConcurrentMapBase whose destructor is
/// trivial.  The other implementation inherits from this, so this is a
/// base for all ConcurrentMaps.
template <class EntryTy, class Allocator>
class ConcurrentMapBase<EntryTy, false, Allocator> : protected Allocator {
protected:
  struct Node {
    std::atomic<Node*> Left;
    std::atomic<Node*> Right;
    EntryTy Payload;

    template <class... Args>
    Node(Args &&... args)
      : Left(nullptr), Right(nullptr), Payload(std::forward<Args>(args)...) {}

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

  #ifndef NDEBUG
    void dump() const {
      auto L = Left.load(std::memory_order_acquire);
      auto R = Right.load(std::memory_order_acquire);
      printf("\"%p\" [ label = \" {<f0> %08lx | {<f1> | <f2>}}\" "
             "style=\"rounded\" shape=\"record\"];\n",
             this, (long) Payload.getKeyValueForDump());

      if (L) {
        L->dump();
        printf("\"%p\":f1 -> \"%p\":f0;\n", this, L);
      }
      if (R) {
        R->dump();
        printf("\"%p\":f2 -> \"%p\":f0;\n", this, R);
      }
    }
  #endif
  };

  std::atomic<Node*> Root;

  constexpr ConcurrentMapBase() : Root(nullptr) {}

  // Implicitly trivial destructor.
  ~ConcurrentMapBase() = default;

  void destroyNode(Node *node) {
    assert(node && "destroying null node");
    auto allocSize = sizeof(Node) + node->Payload.getExtraAllocationSize();

    // Destroy the node's payload.
    node->~Node();

    // Deallocate the node.  The static_cast here is required
    // because LLVM's allocator API is insane.
    this->Deallocate(static_cast<void*>(node), allocSize, alignof(Node));
  }
};

/// The partial specialization of ConcurrentMapBase which provides a
/// non-trivial destructor.
template <class EntryTy, class Allocator>
class ConcurrentMapBase<EntryTy, true, Allocator>
    : protected ConcurrentMapBase<EntryTy, false, Allocator> {
protected:
  using super = ConcurrentMapBase<EntryTy, false, Allocator>;
  using Node = typename super::Node;

  constexpr ConcurrentMapBase() {}

  ~ConcurrentMapBase() {
    destroyTree(this->Root);
  }

private:
  void destroyTree(const std::atomic<Node*> &edge) {
    // This can be a relaxed load because destruction is not allowed to race
    // with other operations.
    auto node = edge.load(std::memory_order_relaxed);
    if (!node) return;

    // Destroy the node's children.
    destroyTree(node->Left);
    destroyTree(node->Right);

    // Destroy the node itself.
    this->destroyNode(node);
  }
};

/// A concurrent map that is implemented using a binary tree. It supports
/// concurrent insertions but does not support removals or rebalancing of
/// the tree.
///
/// The entry type must provide the following operations:
///
///   /// For debugging purposes only. Summarize this key as an integer value.
///   intptr_t getKeyIntValueForDump() const;
///
///   /// A ternary comparison.  KeyTy is the type of the key provided
///   /// to find or getOrInsert.
///   int compareWithKey(KeyTy key) const;
///
///   /// Return the amount of extra trailing space required by an entry,
///   /// where KeyTy is the type of the first argument to getOrInsert and
///   /// ArgTys is the type of the remaining arguments.
///   static size_t getExtraAllocationSize(KeyTy key, ArgTys...)
///
///   /// Return the amount of extra trailing space that was requested for
///   /// this entry.  This method is only used to compute the size of the
///   /// object during node deallocation; it does not need to return a
///   /// correct value so long as the allocator's Deallocate implementation
///   /// ignores this argument.
///   size_t getExtraAllocationSize() const;
///
/// If ProvideDestructor is false, the destructor will be trivial.  This
/// can be appropriate when the object is declared at global scope.
template <class EntryTy, bool ProvideDestructor = true,
          class Allocator = llvm::MallocAllocator>
class ConcurrentMap
      : private ConcurrentMapBase<EntryTy, ProvideDestructor, Allocator> {
  using super = ConcurrentMapBase<EntryTy, ProvideDestructor, Allocator>;

  using Node = typename super::Node;

  /// Inherited from base class:
  ///   std::atomic<Node*> Root;
  using super::Root;

  /// This member stores the address of the last node that was found by the
  /// search procedure. We cache the last search to accelerate code that
  /// searches the same value in a loop.
  std::atomic<Node*> LastSearch;

public:
  constexpr ConcurrentMap() : LastSearch(nullptr) {}

  ConcurrentMap(const ConcurrentMap &) = delete;
  ConcurrentMap &operator=(const ConcurrentMap &) = delete;

  // ConcurrentMap<T, false> must have a trivial destructor.
  ~ConcurrentMap() = default;

public:

  Allocator &getAllocator() {
    return *this;
  }

#ifndef NDEBUG
  void dump() const {
    auto R = Root.load(std::memory_order_acquire);
    printf("digraph g {\n"
           "graph [ rankdir = \"TB\"];\n"
           "node  [ fontsize = \"16\" ];\n"
           "edge  [ ];\n");
    if (R) {
      R->dump();
    }
    printf("\n}\n");
  }
#endif

  /// Search for a value by key \p Key.
  /// \returns a pointer to the value or null if the value is not in the map.
  template <class KeyTy>
  EntryTy *find(const KeyTy &key) {
    // Check if we are looking for the same key that we looked for in the last
    // time we called this function.
    if (Node *last = LastSearch.load(std::memory_order_acquire)) {
      if (last->Payload.compareWithKey(key) == 0)
        return &last->Payload;
    }

    // Search the tree, starting from the root.
    Node *node = Root.load(std::memory_order_acquire);
    while (node) {
      int comparisonResult = node->Payload.compareWithKey(key);
      if (comparisonResult == 0) {
        LastSearch.store(node, std::memory_order_release);
        return &node->Payload;
      } else if (comparisonResult < 0) {
        node = node->Left.load(std::memory_order_acquire);
      } else {
        node = node->Right.load(std::memory_order_acquire);
      }
    }

    return nullptr;
  }

  /// Get or create an entry in the map.
  ///
  /// \returns the entry in the map and whether a new node was added (true)
  ///   or already existed (false)
  template <class KeyTy, class... ArgTys>
  std::pair<EntryTy*, bool> getOrInsert(KeyTy key, ArgTys &&... args) {
    // Check if we are looking for the same key that we looked for the
    // last time we called this function.
    if (Node *last = LastSearch.load(std::memory_order_acquire)) {
      if (last && last->Payload.compareWithKey(key) == 0)
        return { &last->Payload, false };
    }

    // The node we allocated.
    Node *newNode = nullptr;

    // Start from the root.
    auto edge = &Root;

    while (true) {
      // Load the edge.
      Node *node = edge->load(std::memory_order_acquire);

      // If there's a node there, it's either a match or we're going to
      // one of its children.
      if (node) {
      searchFromNode:

        // Compare our key against the node's key.
        int comparisonResult = node->Payload.compareWithKey(key);

        // If it's equal, we can use this node.
        if (comparisonResult == 0) {
          // Destroy the node we allocated before if we're carrying one around.
          if (newNode) this->destroyNode(newNode);

          // Cache and report that we found an existing node.
          LastSearch.store(node, std::memory_order_release);
          return { &node->Payload, false };
        }

        // Otherwise, select the appropriate child edge and descend.
        edge = (comparisonResult < 0 ? &node->Left : &node->Right);
        continue;
      }

      // Create a new node.
      if (!newNode) {
        size_t allocSize =
          sizeof(Node) + EntryTy::getExtraAllocationSize(key, args...);
        void *memory = this->Allocate(allocSize, alignof(Node));
        newNode = ::new (memory) Node(key, std::forward<ArgTys>(args)...);
      }

      // Try to set the edge to the new node.
      if (std::atomic_compare_exchange_strong_explicit(edge, &node, newNode,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
        // If that succeeded, cache and report that we created a new node.
        LastSearch.store(newNode, std::memory_order_release);
        return { &newNode->Payload, true };
      }

      // Otherwise, we lost the race because some other thread initialized
      // the edge before us.  node will be set to the current value;
      // repeat the search from there.
      assert(node && "spurious failure from compare_exchange_strong?");
      goto searchFromNode;
    }
  }
};


/// An append-only array that can be read without taking locks. Writes
/// are still locked and serialized, but only with respect to other
/// writes.
template <class ElemTy> struct ConcurrentReadableArray {
private:
  /// The struct used for the array's storage. The `Elem` member is
  /// considered to be the first element of a variable-length array,
  /// whose size is determined by the allocation. The `Capacity` member
  /// from `ConcurrentReadableArray` indicates how large it can be.
  struct Storage {
    std::atomic<size_t> Count;
    typename std::aligned_storage<sizeof(ElemTy), alignof(ElemTy)>::type Elem;

    static Storage *allocate(size_t capacity) {
      auto size = sizeof(Storage) + (capacity - 1) * sizeof(Storage().Elem);
      auto *ptr = reinterpret_cast<Storage *>(malloc(size));
      if (!ptr) swift::crash("Could not allocate memory.");
      ptr->Count.store(0, std::memory_order_relaxed);
      return ptr;
    }

    void deallocate() {
      for (size_t i = 0; i < Count; ++i) {
        data()[i].~ElemTy();
      }
      free(this);
    }

    ElemTy *data() {
      return reinterpret_cast<ElemTy *>(&Elem);
    }
  };
  
  size_t Capacity;
  std::atomic<size_t> ReaderCount;
  std::atomic<Storage *> Elements;
  Mutex WriterLock;
  std::vector<Storage *> FreeList;
  
  void incrementReaders() {
    ReaderCount.fetch_add(1, std::memory_order_acquire);
  }
  
  void decrementReaders() {
    ReaderCount.fetch_sub(1, std::memory_order_release);
  }
  
  void deallocateFreeList() {
    for (Storage *storage : FreeList)
      storage->deallocate();
    FreeList.clear();
    FreeList.shrink_to_fit();
  }
  
public:
  struct Snapshot {
    ConcurrentReadableArray *Array;
    const ElemTy *Start;
    size_t Count;
    
    Snapshot(ConcurrentReadableArray *array, const ElemTy *start, size_t count)
      : Array(array), Start(start), Count(count) {}
    
    Snapshot(const Snapshot &other)
      : Array(other.Array), Start(other.Start), Count(other.Count) {
      Array->incrementReaders();
    }
    
    ~Snapshot() {
      Array->decrementReaders();
    }
    
    const ElemTy *begin() { return Start; }
    const ElemTy *end() { return Start + Count; }
    size_t count() { return Count; }
  };

  // This type cannot be safely copied or moved.
  ConcurrentReadableArray(const ConcurrentReadableArray &) = delete;
  ConcurrentReadableArray(ConcurrentReadableArray &&) = delete;
  ConcurrentReadableArray &operator=(const ConcurrentReadableArray &) = delete;
  
  ConcurrentReadableArray() : Capacity(0), ReaderCount(0), Elements(nullptr) {}
  
  ~ConcurrentReadableArray() {
    assert(ReaderCount.load(std::memory_order_acquire) == 0 &&
           "deallocating ConcurrentReadableArray with outstanding snapshots");
    deallocateFreeList();
  }
  
  void push_back(const ElemTy &elem) {
    ScopedLock guard(WriterLock);
    
    auto *storage = Elements.load(std::memory_order_relaxed);
    auto count = storage ? storage->Count.load(std::memory_order_relaxed) : 0;
    if (count >= Capacity) {
      auto newCapacity = std::max((size_t)16, count * 2);
      auto *newStorage = Storage::allocate(newCapacity);
      if (storage) {
        std::copy(storage->data(), storage->data() + count, newStorage->data());
        newStorage->Count.store(count, std::memory_order_release);
        FreeList.push_back(storage);
      }
      
      storage = newStorage;
      Capacity = newCapacity;
      Elements.store(storage, std::memory_order_release);
    }
    
    new(&storage->data()[count]) ElemTy(elem);
    storage->Count.store(count + 1, std::memory_order_release);
    
    if (ReaderCount.load(std::memory_order_acquire) == 0)
      deallocateFreeList();
  }

  Snapshot snapshot() {
    incrementReaders();
    auto *storage = Elements.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (storage == nullptr) {
      return Snapshot(this, nullptr, 0);
    }
    
    auto count = storage->Count.load(std::memory_order_acquire);
    const auto *ptr = storage->data();
    return Snapshot(this, ptr, count);
  }
};

using llvm::hash_value;

/// A hash table that can be queried without taking any locks. Writes are still
/// locked and serialized, but only with respect to other locks. Writers can add
/// elements and clear the table, but they cannot remove individual elements.
/// Readers work by taking a snapshot of the table and then querying that
/// snapshot.
///
/// The basic structure of the table consists of two arrays. Elements are stored
/// in a contiguous array, with new elements appended to the end. The second
/// array is the actual hash table, and it contains indices into the elements
/// array. This scheme cuts down on wasted space when the elements are larger
/// than a few bytes: instead of wasting `(1 - loadFactor) * sizeof(element)`
/// bytes on unused space in the hash table, we only waste `(1 - loadFactor) *
/// sizeof(index)`. This scheme also avoids readers seeing partially constructed
/// elements.
///
/// Reader/writer synchronization for new elements is handled by keeping an
/// element count which is only incremented when the element has been fully
/// constructed. A reader which sees an index beyond its view of the current
/// count will ignore it and treat that as if there was no entry.
///
/// Reader/writer synchronization for resizing the arrays is handled by tracking
/// the current number of active readers. When resizing, the new array is
/// allocated, the data copied, and then the old array is placed in a free list.
/// The free list is only deallocated if there are no readers, otherwise freeing
/// is deferred.
///
/// Reader/writer synchronization for clearing the table is a combination of the
/// above. By keeping the old arrays around until all readers are finished, we
/// ensure that readers which started before the clear see valid (pre-clear)
/// data. Readers which see any array as empty will produce no results, thus
/// providing valid post-clear data.
template <class ElemTy> struct ConcurrentReadableHashMap {
  // We use memcpy and don't call destructors. Make sure the elements will put
  // up with this.
  static_assert(std::is_trivially_copyable<ElemTy>::value,
                "Elements must be trivially copyable.");
  static_assert(std::is_trivially_destructible<ElemTy>::value,
                "Elements must not have destructors (they won't be called).");

private:
  /// The type of the elements of the indices array. TODO: use one or two byte
  /// indices for smaller tables to save more memory.
  using Index = unsigned;

  /// The reciprocal of the load factor at which we expand the table. A value of
  /// 4 means that we resize at 1/4 = 75% load factor.
  static const size_t ResizeProportion = 4;

  /// Get the "good size" for a given allocation size. When available, this
  /// rounds up to the next allocation quantum by calling `malloc_good_size`.
  /// Otherwise, just return the passed-in size, which is always valid even if
  /// not necessarily optimal.
  size_t goodSize(size_t size) {
#if defined(__APPLE__) && defined(__MACH__) && \
        defined(SWIFT_STDLIB_HAS_DARWIN_LIBMALLOC)
    return malloc_good_size(size);
#else
    return size;
#endif
  }

  /// A private class representing the storage of the indices. In order to
  /// ensure that readers can get a consistent view of the indices with a single
  /// atomic read, we store the size of the indices array inline, as the first
  /// element in the array.
  ///
  /// We want the number of indices to be a power of two so that we can use a
  /// bitwise AND to convert a hash code to an index. We want the entire array
  /// to be a power of two in size to be friendly to the allocator, but the size
  /// is stored inline. We work around this contradiction by considering the
  /// first index to always be occupied with a value that never matches any key.
  struct IndexStorage {
    std::atomic<Index> Mask;

    static IndexStorage *allocate(size_t capacity) {
      assert((capacity & (capacity - 1)) == 0 &&
             "Capacity must be a power of 2");
      auto *ptr =
          reinterpret_cast<IndexStorage *>(calloc(capacity, sizeof(Mask)));
      if (!ptr)
        swift::crash("Could not allocate memory.");
      ptr->Mask.store(capacity - 1, std::memory_order_relaxed);
      return ptr;
    }

    std::atomic<Index> &at(size_t i) { return (&Mask)[i]; }
  };

  /// A simple linked list representing pointers that need to be freed.
  struct FreeListNode {
    FreeListNode *Next;
    void *Ptr;

    static void add(FreeListNode **head, void *ptr) {
      auto *newNode = new FreeListNode{*head, ptr};
      *head = newNode;
    }

    static void freeAll(FreeListNode **head) {
      auto *node = *head;
      while (node) {
        auto *next = node->Next;
        free(node->Ptr);
        delete node;
        node = next;
      }
      *head = nullptr;
    }
  };

  /// The number of readers currently active, equal to the number of snapshot
  /// objects currently alive.
  std::atomic<uint32_t> ReaderCount{0};

  /// The number of elements in the elements array.
  std::atomic<uint32_t> ElementCount{0};

  /// The array of elements.
  std::atomic<ElemTy *> Elements{nullptr};

  /// The array of indices.
  std::atomic<IndexStorage *> Indices{nullptr};

  /// The writer lock, which must be taken before any mutation of the table.
  Mutex WriterLock;

  /// The maximum number of elements that the current elements array can hold.
  uint32_t ElementCapacity{0};

  /// The list of pointers to be freed once no readers are active.
  FreeListNode *FreeList{nullptr};

  void incrementReaders() {
    ReaderCount.fetch_add(1, std::memory_order_acquire);
  }

  void decrementReaders() {
    ReaderCount.fetch_sub(1, std::memory_order_release);
  }

  /// Free all the arrays in the free lists if there are no active readers. If
  /// there are active readers, do nothing.
  void deallocateFreeListIfSafe() {
    if (ReaderCount.load(std::memory_order_acquire) == 0)
      FreeListNode::freeAll(&FreeList);
  }

  /// Grow the elements array, adding the old array to the free list and
  /// returning the new array with all existing elements copied into it.
  ElemTy *resize(ElemTy *elements, size_t elementCount) {
    // Grow capacity by 25%, making sure we grow by at least 1.
    size_t newCapacity =
        std::max(elementCount + (elementCount >> 2), elementCount + 1);
    size_t newSize = newCapacity * sizeof(ElemTy);

    newSize = goodSize(newSize);
    newCapacity = newSize / sizeof(ElemTy);

    ElemTy *newElements = static_cast<ElemTy *>(malloc(newSize));
    if (elements) {
      memcpy(newElements, elements, elementCount * sizeof(ElemTy));
      FreeListNode::add(&FreeList, elements);
    }

    ElementCapacity = newCapacity;
    Elements.store(newElements, std::memory_order_release);
    return newElements;
  }

  /// Grow the indices array, adding the old array to the free list and
  /// returning the new array with all existing indices copied into it. This
  /// operation performs a rehash, so that the indices are in the correct
  /// location in the new array.
  IndexStorage *resize(IndexStorage *indices, Index indicesMask,
                       ElemTy *elements) {
    // Mask is size - 1. Double the size. Start with 4 (fits into 16-byte malloc
    // bucket).
    size_t newCount = indices ? 2 * (indicesMask + 1) : 4;
    size_t newMask = newCount - 1;

    IndexStorage *newIndices = IndexStorage::allocate(newCount);

    for (size_t i = 1; i <= indicesMask; i++) {
      Index index = indices->at(i).load(std::memory_order_relaxed);
      if (index == 0)
        continue;

      auto *element = &elements[index - 1];
      auto hash = hash_value(*element);

      size_t newI = hash & newMask;
      while (newIndices->at(newI) != 0)
        newI = (newI + 1) & newMask;
      newIndices->at(newI).store(index, std::memory_order_relaxed);
    }

    Indices.store(newIndices, std::memory_order_release);

    FreeListNode::add(&FreeList, indices);

    return newIndices;
  }

  /// Search for the given key within the given indices and elements arrays. If
  /// an entry already exists for that key, return a pointer to the element. If
  /// no entry exists, return a pointer to the location in the indices array
  /// where the index of the new element would be stored.
  template <class KeyTy>
  static std::pair<ElemTy *, std::atomic<Index> *>
  find(const KeyTy &key, IndexStorage *indices, size_t elementCount,
       ElemTy *elements) {
    if (!indices)
      return {nullptr, nullptr};
    auto hash = hash_value(key);
    auto indicesMask = indices->Mask.load(std::memory_order_relaxed);

    auto i = hash & indicesMask;
    while (true) {
      // Index 0 is used for the mask and is not actually an index.
      if (i == 0)
        i++;

      auto *indexPtr = &indices->at(i);
      auto index = indexPtr->load(std::memory_order_acquire);
      // Element indices are 1-based, 0 means no entry.
      if (index == 0)
        return {nullptr, indexPtr};
      if (index - 1 < elementCount) {
        auto *candidate = &elements[index - 1];
        if (candidate->matchesKey(key))
          return {candidate, nullptr};
      }

      i = (i + 1) & indicesMask;
    }
  }

public:
  // This type cannot be safely copied or moved.
  ConcurrentReadableHashMap(const ConcurrentReadableHashMap &) = delete;
  ConcurrentReadableHashMap(ConcurrentReadableHashMap &&) = delete;
  ConcurrentReadableHashMap &
  operator=(const ConcurrentReadableHashMap &) = delete;

  ConcurrentReadableHashMap()
      : ReaderCount(0), ElementCount(0), Elements(nullptr), Indices(nullptr),
        ElementCapacity(0) {}

  ~ConcurrentReadableHashMap() {
    assert(ReaderCount.load(std::memory_order_acquire) == 0 &&
           "deallocating ConcurrentReadableHashMap with outstanding snapshots");
    FreeListNode::freeAll(&FreeList);
  }

  /// Readers take a snapshot of the hash map, then work with the snapshot.
  class Snapshot {
    ConcurrentReadableHashMap *Map;
    IndexStorage *Indices;
    ElemTy *Elements;
    size_t ElementCount;

  public:
    Snapshot(ConcurrentReadableHashMap *map, IndexStorage *indices,
             ElemTy *elements, size_t elementCount)
        : Map(map), Indices(indices), Elements(elements),
          ElementCount(elementCount) {}

    Snapshot(const Snapshot &other)
        : Map(other.Map), Indices(other.Indices), Elements(other.Elements),
          ElementCount(other.ElementCount) {
      Map->incrementReaders();
    }

    ~Snapshot() { Map->decrementReaders(); }

    /// Search for an element matching the given key. Returns a pointer to the
    /// found element, or nullptr if no matching element exists.
    template <class KeyTy> const ElemTy *find(const KeyTy &key) {
      if (!Indices || !ElementCount || !Elements)
        return nullptr;
      return ConcurrentReadableHashMap::find(key, Indices, ElementCount,
                                             Elements)
          .first;
    }
  };

  /// Take a snapshot of the current state of the hash map.
  Snapshot snapshot() {
    incrementReaders();

    // Carefully loading the indices, element count, and elements pointer in
    // order ensures a consistent view of the table with respect to concurrent
    // inserts. However, this is not sufficient to avoid an inconsistent view
    // with respect to concurrent clears. The danger scenario is:
    //
    // 1. Read indices and elementCount from a table with N entries.
    // 2. Another thread clears the table.
    // 3. Another thread inserts M entries, where M < N.
    // 4. The reader thread reads elements.
    // 5. The reader thread performs a find. The key's hash leads us to an index
    //    I, where > M.
    // 6. The reader thread reads from element I, which is off the end of the
    //    elements array.
    //
    // To avoid this, read the elements pointer twice, at the beginning and end.
    // If the values are not the same then there may have been a clear in the
    // middle, so we retry. This will have false positives: a new element
    // pointer can just mean a concurrent insert that triggered a resize of the
    // elements array. This is harmless aside from a small performance hit, and
    // should not happen often.
    IndexStorage *indices;
    size_t elementCount;
    ElemTy *elements;
    ElemTy *elements2;
    do {
      elements = Elements.load(std::memory_order_acquire);
      indices = Indices.load(std::memory_order_acquire);
      elementCount = ElementCount.load(std::memory_order_acquire);
      elements2 = Elements.load(std::memory_order_acquire);
    } while (elements != elements2);

    return Snapshot(this, indices, elements, elementCount);
  }

  /// Get an element by key, or insert a new element for that key if one is not
  /// already present. Invoke `call` with the pointer to the element. BEWARE:
  /// `call` is invoked with the internal writer lock held, keep work to a
  /// minimum.
  ///
  /// `call` is passed the following parameters:
  ///   - `element`: the pointer to the element corresponding to `key`
  ///   - `created`: true if the element is newly created, false if it already
  ///                exists
  /// `call` returns a `bool`. When `created` is `true`, the return values mean:
  ///   - `true` the new entry is to be kept
  ///   - `false` indicates that the new entry is discarded
  /// If the new entry is kept, then the new element MUST be initialized, and
  /// have a hash value that matches the hash value of `key`.
  ///
  /// The return value is ignored when `created` is `false`.
  template <class KeyTy, typename Call>
  void getOrInsert(KeyTy key, const Call &call) {
    ScopedLock guard(WriterLock);

    auto *indices = Indices.load(std::memory_order_relaxed);
    if (!indices)
      indices = resize(indices, 0, nullptr);

    auto indicesMask = indices->Mask.load(std::memory_order_relaxed);
    auto elementCount = ElementCount.load(std::memory_order_relaxed);
    auto *elements = Elements.load(std::memory_order_relaxed);

    auto found = find(key, indices, elementCount, elements);
    if (found.first) {
      call(found.first, false);
      deallocateFreeListIfSafe();
      return;
    }

    // The actual capacity is indicesMask + 1. The number of slots in use is
    // elementCount + 1, since the mask also takes a slot.
    auto emptyCount = (indicesMask + 1) - (elementCount + 1);
    auto proportion = (indicesMask + 1) / emptyCount;
    if (proportion >= ResizeProportion) {
      indices = resize(indices, indicesMask, elements);
      found = find(key, indices, elementCount, elements);
      assert(!found.first && "Shouldn't suddenly find the key after rehashing");
    }

    if (elementCount >= ElementCapacity) {
      elements = resize(elements, elementCount);
    }
    auto *element = &elements[elementCount];

    // Order matters: fill out the element, then update the count,
    // then update the index.
    bool keep = call(element, true);
    if (keep) {
      assert(hash_value(key) == hash_value(*element) &&
             "Element must have the same hash code as its key.");
      ElementCount.store(elementCount + 1, std::memory_order_release);
      found.second->store(elementCount + 1, std::memory_order_release);
    }

    deallocateFreeListIfSafe();
  }

  /// Clear the hash table, freeing (when safe) all memory currently used for
  /// indices and elements.
  void clear() {
    ScopedLock guard(WriterLock);

    auto *indices = Indices.load(std::memory_order_relaxed);
    auto *elements = Elements.load(std::memory_order_relaxed);

    // Order doesn't matter here, snapshots will gracefully handle any field
    // being NULL/0 while the others are not.
    Indices.store(nullptr, std::memory_order_relaxed);
    ElementCount.store(0, std::memory_order_relaxed);
    Elements.store(nullptr, std::memory_order_relaxed);
    ElementCapacity = 0;

    FreeListNode::add(&FreeList, indices);
    FreeListNode::add(&FreeList, elements);

    deallocateFreeListIfSafe();
  }
};

} // end namespace swift

#endif // SWIFT_RUNTIME_CONCURRENTUTILS_H
