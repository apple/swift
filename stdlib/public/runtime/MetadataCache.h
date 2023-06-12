//===--- MetadataCache.h - Implements the metadata cache --------*- C++ -*-===//
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
#ifndef SWIFT_RUNTIME_METADATACACHE_H
#define SWIFT_RUNTIME_METADATACACHE_H

#include "swift/Runtime/AtomicWaitQueue.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Threading/Mutex.h"

#include "swift/shims/Visibility.h"

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"

#include <condition_variable>
#include <atomic>

#ifndef SWIFT_DEBUG_RUNTIME
#define SWIFT_DEBUG_RUNTIME 0
#endif

namespace swift {

RelativeWitnessTable *lookThroughOptionalConditionalWitnessTable(const RelativeWitnessTable *);

#if !SWIFT_STDLIB_PASSTHROUGH_METADATA_ALLOCATOR

class MetadataAllocator : public llvm::AllocatorBase<MetadataAllocator> {
private:
  uint16_t Tag;

public:
  constexpr MetadataAllocator(uint16_t tag) : Tag(tag) {}
  MetadataAllocator() = delete;

  void Reset() {}

  SWIFT_RETURNS_NONNULL SWIFT_NODISCARD
  void *Allocate(size_t size, size_t alignment);
  using AllocatorBase<MetadataAllocator>::Allocate;

  void Deallocate(const void *Ptr, size_t size, size_t Alignment);
  using AllocatorBase<MetadataAllocator>::Deallocate;

  void PrintStats() const {}
  
  MetadataAllocator withTag(uint16_t Tag) {
    MetadataAllocator Allocator = *this;
    Allocator.Tag = Tag;
    return Allocator;
  }
};

#else

class MetadataAllocator {
public:
  MetadataAllocator(uint16_t tag) {}
  SWIFT_RETURNS_NONNULL SWIFT_NODISCARD
  void *Allocate(size_t size, size_t alignment) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    void *ptr = nullptr;
    if (SWIFT_UNLIKELY(posix_memalign(&ptr, alignment, size) != 0 || !ptr)) {
      swift::crash("Could not allocate memory for type metadata.");
    }
    return ptr;
  }
  void Deallocate(const void *ptr, size_t size = 0, size_t Alignment = 0) {
    return free(const_cast<void *>(ptr));
  }
};

#endif

template <uint16_t StaticTag>
class TaggedMetadataAllocator : public MetadataAllocator {
public:
  constexpr TaggedMetadataAllocator() : MetadataAllocator(StaticTag) {}
};

using RawPrivateMetadataState = uint8_t;
enum class PrivateMetadataState : RawPrivateMetadataState {
  /// The metadata is being allocated.
  Allocating,

  /// The metadata has been allocated, but is not yet complete for
  /// external layout: that is, it does not have a size.
  Abstract,

  /// The metadata has a complete external layout, but may not have
  /// been fully initialized.
  LayoutComplete,

  /// The metadata has a complete external layout and has been fully
  /// initialized, but has not yet satisfied its transitive completeness
  /// requirements.
  NonTransitiveComplete,

  /// The metadata is fully complete.  There should no longer be waiters.
  Complete
};
inline bool operator<(PrivateMetadataState lhs, PrivateMetadataState rhs) {
  return RawPrivateMetadataState(lhs) < RawPrivateMetadataState(rhs);
}
inline bool operator<=(PrivateMetadataState lhs, PrivateMetadataState rhs) {
  return RawPrivateMetadataState(lhs) <= RawPrivateMetadataState(rhs);
}
inline bool operator>(PrivateMetadataState lhs, PrivateMetadataState rhs) {
  return RawPrivateMetadataState(lhs) > RawPrivateMetadataState(rhs);
}
inline bool operator>=(PrivateMetadataState lhs, PrivateMetadataState rhs) {
  return RawPrivateMetadataState(lhs) >= RawPrivateMetadataState(rhs);
}
inline bool satisfies(PrivateMetadataState state, MetadataState requirement) {
  switch (requirement) {
  case MetadataState::Abstract:
    return state >= PrivateMetadataState::Abstract;
  case MetadataState::LayoutComplete:
    return state >= PrivateMetadataState::LayoutComplete;
  case MetadataState::NonTransitiveComplete:
    return state >= PrivateMetadataState::NonTransitiveComplete;
  case MetadataState::Complete:
    return state >= PrivateMetadataState::Complete;
  }
  swift_unreachable("unsupported requirement kind");
}
inline MetadataState getAccomplishedRequestState(PrivateMetadataState state) {
  switch (state) {
  case PrivateMetadataState::Allocating:
    swift_unreachable("cannot call on allocating state");
  case PrivateMetadataState::Abstract:
    return MetadataState::Abstract;
  case PrivateMetadataState::LayoutComplete:
    return MetadataState::LayoutComplete;
  case PrivateMetadataState::NonTransitiveComplete:
    return MetadataState::NonTransitiveComplete;
  case PrivateMetadataState::Complete:
    return MetadataState::Complete;
  }
  swift_unreachable("bad state");
}

struct MetadataStateWithDependency {
  /// The current state of the metadata.
  PrivateMetadataState NewState;
  /// The known dependency that the metadata has, if any.
  MetadataDependency Dependency;
};

/// A typedef for simple global caches with stable addresses for the entries.
template <class EntryTy, uint16_t Tag>
using SimpleGlobalCache =
    StableAddressConcurrentReadableHashMap<EntryTy,
                                           TaggedMetadataAllocator<Tag>>;

struct ConcurrencyControl {
  using LockType = SmallMutex;
  LockType Lock;
};

template <class EntryType, uint16_t Tag>
class LockingConcurrentMapStorage {
  // This class must fit within
  // TargetGenericMetadataInstantiationCache::PrivateData. On 32-bit archs, that
  // space is not large enough to accommodate a Mutex along with everything
  // else. There, use a SmallMutex to squeeze into the available space.
  using MutexTy = std::conditional_t<sizeof(void *) == 8 && sizeof(Mutex) <= 56,
                                     Mutex, SmallMutex>;
  StableAddressConcurrentReadableHashMap<EntryType,
                                         TaggedMetadataAllocator<Tag>, MutexTy>
      Map;
  ConcurrencyControl Concurrency;

public:
  LockingConcurrentMapStorage() {}

  ConcurrencyControl &getConcurrency() { return Concurrency; }

  template <class KeyType, class... ArgTys>
  std::pair<EntryType*, bool>
  getOrInsert(KeyType key, ArgTys &&...args) {
    return Map.getOrInsert(key, args...);
  }

  template <class KeyType>
  EntryType *find(KeyType key) {
    return Map.find(key);
  }

  /// A default implementation for resolveEntry that assumes that the
  /// key type is a lookup key for the map.
  template <class KeyType>
  EntryType *resolveExistingEntry(KeyType key) {
    auto entry = Map.find(key);
    assert(entry && "entry doesn't already exist!");
    return entry;
  }
};

/// A map for which there is a phase of initialization that is guaranteed
/// to be performed exclusively.
///
/// In addition to the requirements of ConcurrentMap, the entry type must
/// provide the following members:
///
///   /// An encapsulation of the status of the entry.  The result type
///   /// of most operations.
///   using Status = ...;
///
///   /// Given that this is not the thread currently responsible for
///   /// initializing the entry, wait for the entry to complete.
///   Status await(ConcurrencyControl &concurrency, ArgTys...);
///
///   /// Perform allocation.  If this returns a status, initialization
///   /// is skipped.
///   Optional<Status>
///   beginAllocation(WaitQueue::Worker &worker, ArgTys...);
///
///   /// Attempt to initialize an entry.  This is called once for the entry,
///   /// immediately after construction, by the thread that successfully
///   /// constructed the entry.
///   Status beginInitialization(WaitQueue::Worker &worker, ArgTys...);
///
///   /// Perform a checkDependency operation.  This only needs to be
///   /// implemented if checkDependency is called on the map.
///   MetadataStateWithDependency
///   checkDependency(ConcurrencyControl &concurrency, ArgTys...);
template <class EntryType,
          class StorageType = LockingConcurrentMapStorage<EntryType, true>>
class LockingConcurrentMap {
  StorageType Storage;
  using Status = typename EntryType::Status;
  using WaitQueue = typename EntryType::WaitQueue;
  using Worker = typename WaitQueue::Worker;
  using Waiter = typename WaitQueue::Waiter;

public:
  LockingConcurrentMap() = default;

  template <class KeyType, class... ArgTys>
  std::pair<EntryType*, Status>
  getOrInsert(KeyType key, ArgTys &&...args) {
    Worker worker(Storage.getConcurrency().Lock);

    auto result = Storage.getOrInsert(key, worker, args...);
    auto entry = result.first;

    // If we are not inserting the entry, we need to potentially block on 
    // currently satisfies our conditions.
    if (!result.second) {
      auto status =
        entry->await(Storage.getConcurrency(), std::forward<ArgTys>(args)...);
      return { entry, status };
    }

    // Okay, we inserted.  We are responsible for allocating and
    // subsequently trying to initialize the entry.

    // Insertion should have called worker.createQueue(); tell the Worker
    // object that we published it.
    worker.flagCreatedQueueIsPublished();

    // Allocation.  This can fast-path and bypass initialization by returning
    // a status.
    if (auto status = entry->beginAllocation(worker, args...)) {
      return { entry, *status };
    }

    // Initialization.
    auto status = entry->beginInitialization(worker,
                                             std::forward<ArgTys>(args)...);
    return { entry, status };
  }

  template <class KeyType>
  EntryType *find(KeyType key) {
    return Storage.find(key);
  }

  /// Given that an entry already exists, await it.
  template <class KeyType, class... ArgTys>
  Status await(KeyType key, ArgTys &&...args) {
    EntryType *entry = Storage.resolveExistingEntry(key);
    return entry->await(Storage.getConcurrency(),
                        std::forward<ArgTys>(args)...);
  }

  /// If an entry already exists, await it; otherwise report failure.
  template <class KeyType, class... ArgTys>
  llvm::Optional<Status> tryAwaitExisting(KeyType key, ArgTys &&... args) {
    EntryType *entry = Storage.find(key);
    if (!entry) return None;
    return entry->await(Storage.getConcurrency(),
                        std::forward<ArgTys>(args)...);
  }

  /// Given that an entry already exists, check whether it has an active
  /// dependency.
  template <class KeyType, class... ArgTys>
  MetadataStateWithDependency
  checkDependency(KeyType key, ArgTys &&...args) {
    EntryType *entry = Storage.resolveExistingEntry(key);
    return entry->checkDependency(Storage.getConcurrency(),
                                  std::forward<ArgTys>(args)...);
  }
};

/// A base class for metadata cache entries which supports an unfailing
/// one-phase allocation strategy that should not be done by trial.
///
/// In addition to the requirements of ConcurrentMap, subclasses should
/// provide:
///
///   /// Allocate the cached entry.  This is not allowed to fail.
///   ValueType allocate(ArgTys...);
template <class Impl, class ValueType>
class SimpleLockingCacheEntryBase {
public:
  using WaitQueue = SimpleAtomicWaitQueue<ConcurrencyControl::LockType>;

private:
  static_assert(std::is_pointer<ValueType>::value,
                "value type must be a pointer type");

  static const uintptr_t IsWaitQueue = 1;
  static WaitQueue *getAsWaitQueue(uintptr_t value) {
    if (value & IsWaitQueue)
      return reinterpret_cast<WaitQueue*>(value & ~IsWaitQueue);
    return nullptr;
  }
  static ValueType castAsValue(uintptr_t value) {
    assert(!(value & IsWaitQueue));
    return reinterpret_cast<ValueType>(value);
  }

  std::atomic<uintptr_t> Value;

protected:
  Impl &asImpl() { return static_cast<Impl &>(*this); }
  const Impl &asImpl() const { return static_cast<const Impl &>(*this); }

  SimpleLockingCacheEntryBase(WaitQueue::Worker &worker)
    : Value(reinterpret_cast<uintptr_t>(worker.createQueue()) | IsWaitQueue) {}

public:
  using Status = ValueType;

  template <class... ArgTys>
  Status await(ConcurrencyControl &concurrency, ArgTys &&...args) {
    WaitQueue::Waiter waiter(concurrency.Lock);

    // Load the value.  If this is not a queue, we're done.
    auto value = Value.load(std::memory_order_acquire);
    if (getAsWaitQueue(value)) {
      bool waited = waiter.tryReloadAndWait([&] {
        // We can use a relaxed load because we're already ordered
        // by the lock.
        value = Value.load(std::memory_order_relaxed);
        return getAsWaitQueue(value);
      });

      if (waited) {
        // This load can be relaxed because we acquired the wait queue
        // lock, which was released by the worker thread after
        // initializing Value to the value.
        value = Value.load(std::memory_order_relaxed);
        assert(!getAsWaitQueue(value));
      }
    }
    return castAsValue(value);
  }

  template <class... ArgTys>
  llvm::Optional<Status> beginAllocation(WaitQueue::Worker &worker,
                                         ArgTys &&... args) {

    // Delegate to the implementation class.
    ValueType origValue =
      asImpl().allocate(std::forward<ArgTys>(args)...);

    auto value = reinterpret_cast<uintptr_t>(origValue);
    assert(!getAsWaitQueue(value) && "allocate returned an unaligned value");

    // Publish the value, which unpublishes the queue.
    worker.finishAndUnpublishQueue([&] {
      Value.store(value, std::memory_order_release);      
    });

    return origValue;
  }

  template <class... ArgTys>
  Status beginInitialization(WaitQueue::Worker &worker,
                             ArgTys &&...args) {
    swift_unreachable("beginAllocation always short-circuits");
  }
};

/// A summary of the information from a generic signature that's
/// sufficient to compare arguments.
template<typename Runtime>
struct GenericSignatureLayout {
  uint16_t NumKeyParameters = 0;
  uint16_t NumWitnessTables = 0;
  uint16_t NumPacks = 0;
  uint16_t NumShapeClasses = 0;
  const GenericPackShapeDescriptor *PackShapeDescriptors = nullptr;

  GenericSignatureLayout(const RuntimeGenericSignature<Runtime> &sig)
    : NumPacks(sig.getGenericPackShapeHeader().NumPacks),
      NumShapeClasses(sig.getGenericPackShapeHeader().NumShapeClasses),
      PackShapeDescriptors(sig.getGenericPackShapeDescriptors().data()) {

#ifndef NDEBUG
    unsigned packIdx = 0;
#endif

    for (const auto &gp : sig.getParams()) {
      if (gp.hasKeyArgument()) {
#ifndef NDEBUG
        if (gp.getKind() == GenericParamKind::TypePack) {
          assert(packIdx < NumPacks);
          assert(PackShapeDescriptors[packIdx].Kind
                 == GenericPackKind::Metadata);
          assert(PackShapeDescriptors[packIdx].Index
                 == NumShapeClasses + NumKeyParameters);
          assert(PackShapeDescriptors[packIdx].ShapeClass
                 < NumShapeClasses);
          ++packIdx;
        }
#endif

        ++NumKeyParameters;
      }
    }
    for (const auto &reqt : sig.getRequirements()) {
      if (reqt.Flags.hasKeyArgument() &&
          reqt.getKind() == GenericRequirementKind::Protocol) {
#ifndef NDEBUG
        if (reqt.getFlags().isPackRequirement()) {
          assert(packIdx < NumPacks);
          assert(PackShapeDescriptors[packIdx].Kind
                 == GenericPackKind::WitnessTable);
          assert(PackShapeDescriptors[packIdx].Index
                 == NumShapeClasses + NumKeyParameters + NumWitnessTables);
          assert(PackShapeDescriptors[packIdx].ShapeClass
                 < NumShapeClasses);
          ++packIdx;
        }
#endif

        ++NumWitnessTables;
      }
    }

#ifndef NDEBUG
    assert(packIdx == NumPacks);
#endif
  }

  size_t sizeInWords() const {
    return NumShapeClasses + NumKeyParameters + NumWitnessTables;
  }

  friend bool operator==(const GenericSignatureLayout<Runtime> &lhs,
                         const GenericSignatureLayout<Runtime> &rhs) {
    if (lhs.NumKeyParameters != rhs.NumKeyParameters ||
        lhs.NumWitnessTables != rhs.NumWitnessTables ||
        lhs.NumShapeClasses != rhs.NumShapeClasses ||
        lhs.NumPacks != rhs.NumPacks) {
      return false;
    }

    for (unsigned i = 0; i < lhs.NumPacks; ++i) {
      const auto &lhsElt = lhs.PackShapeDescriptors[i];
      const auto &rhsElt = rhs.PackShapeDescriptors[i];
      if (lhsElt.Kind != rhsElt.Kind ||
          lhsElt.Index != rhsElt.Index ||
          lhsElt.ShapeClass != rhsElt.ShapeClass)
        return false;
    }

    return true;
  }

  friend bool operator!=(const GenericSignatureLayout<Runtime> &lhs,
                         const GenericSignatureLayout<Runtime> &rhs) {
    return !(lhs == rhs);
  }
};

/// A key value as provided to the concurrent map.
class MetadataCacheKey {
  const void * const *Data;
  GenericSignatureLayout<InProcess> Layout;
  uint32_t Hash;

public:
  /// Compare two witness tables, which may involving checking the
  /// contents of their conformance descriptors.
  static bool areWitnessTablesEqual(const WitnessTable *awt,
                                    const WitnessTable *bwt) {
    if (awt == bwt)
      return true;
#if SWIFT_STDLIB_USE_RELATIVE_PROTOCOL_WITNESS_TABLES
    auto *aDescription = lookThroughOptionalConditionalWitnessTable(
      reinterpret_cast<const RelativeWitnessTable*>(awt))->getDescription();
    auto *bDescription = lookThroughOptionalConditionalWitnessTable(
      reinterpret_cast<const RelativeWitnessTable*>(bwt))->getDescription();
#else
    auto *aDescription = awt->getDescription();
    auto *bDescription = bwt->getDescription();
#endif
    return areConformanceDescriptorsEqual(aDescription, bDescription);
  }

  static void installGenericArguments(uint16_t numKeyArguments, uint16_t numPacks,
                                      const GenericPackShapeDescriptor *packShapeDescriptors,
                                      const void **dst, const void * const *src);

  /// Compare two conformance descriptors, checking their contents if necessary.
  static bool areConformanceDescriptorsEqual(
      const ProtocolConformanceDescriptor *aDescription,
      const ProtocolConformanceDescriptor *bDescription) {
    if (aDescription == bDescription)
      return true;

    if (!aDescription->isSynthesizedNonUnique() ||
        !bDescription->isSynthesizedNonUnique())
      return aDescription == bDescription;

    auto aType = aDescription->getCanonicalTypeMetadata();
    auto bType = bDescription->getCanonicalTypeMetadata();
    if (!aType || !bType)
      return aDescription == bDescription;

    return (aType == bType &&
            aDescription->getProtocol() == bDescription->getProtocol());
  }

private:
  static bool areMetadataPacksEqual(const void *lhsPtr,
                                    const void *rhsPtr,
                                    uintptr_t count) {
    MetadataPackPointer lhs(lhsPtr);
    MetadataPackPointer rhs(rhsPtr);

    // lhs is the user-supplied key, which might be on the stack.
    // rhs is the stored key in the cache.
    assert(rhs.getLifetime() == PackLifetime::OnHeap);

    auto *lhsElt = lhs.getElements();
    auto *rhsElt = rhs.getElements();

    for (uintptr_t i = 0; i < count; ++i) {
      if (lhsElt[i] != rhsElt[i])
        return false;
    }

    return true;
  }

  static bool areWitnessTablePacksEqual(const void *lhsPtr,
                                        const void *rhsPtr,
                                        uintptr_t count) {
    WitnessTablePackPointer lhs(lhsPtr);
    WitnessTablePackPointer rhs(rhsPtr);

    // lhs is the user-supplied key, which might be on the stack.
    // rhs is the stored key in the cache.
    assert(rhs.getLifetime() == PackLifetime::OnHeap);

    auto *lhsElt = lhs.getElements();
    auto *rhsElt = rhs.getElements();

    for (uintptr_t i = 0; i < count; ++i) {
      if (!areWitnessTablesEqual(lhsElt[i], rhsElt[i]))
        return false;
    }

    return true;
  }

public:
  MetadataCacheKey(const GenericSignatureLayout<InProcess> &layout,
                   const void *const *data)
      : Data(data), Layout(layout), Hash(computeHash()) {}

  MetadataCacheKey(const GenericSignatureLayout<InProcess> &layout,
                   const void *const *data, uint32_t hash)
      : Data(data), Layout(layout), Hash(hash) {}

  bool operator==(const MetadataCacheKey &rhs) const {
    // Compare the hashes.
    if (hash() != rhs.hash()) return false;

    // Compare the layouts.
    if (Layout != rhs.Layout) return false;

    // Compare the content.
    auto *adata = begin();
    auto *bdata = rhs.begin();
    const uintptr_t *packCounts = reinterpret_cast<const uintptr_t *>(adata);

    unsigned argIdx = 0;

    // Compare pack lengths for shape classes.
    for (unsigned i = 0; i != Layout.NumShapeClasses; ++i) {
      if (adata[argIdx] != bdata[argIdx])
        return false;

      ++argIdx;
    }

    auto *packs = Layout.PackShapeDescriptors;
    unsigned packIdx = 0;

    // Compare generic arguments for key parameters.
    for (unsigned i = 0; i != Layout.NumKeyParameters; ++i) {
      // Is this entry a metadata pack?
      if (packIdx < Layout.NumPacks &&
          packs[packIdx].Kind == GenericPackKind::Metadata &&
          argIdx == packs[packIdx].Index) {
        assert(packs[packIdx].ShapeClass < Layout.NumShapeClasses);
        uintptr_t count = packCounts[packs[packIdx].ShapeClass];

        if (!areMetadataPacksEqual(adata[argIdx], bdata[argIdx], count))
          return false;

        ++packIdx;
        ++argIdx;
        continue;
      }

      if (adata[argIdx] != bdata[argIdx])
        return false;

      ++argIdx;
    }

    // Compare witness tables.
    for (unsigned i = 0; i != Layout.NumWitnessTables; ++i) {
      // Is this entry a witness table pack?
      if (packIdx < Layout.NumPacks &&
          packs[packIdx].Kind == GenericPackKind::WitnessTable &&
          argIdx == packs[packIdx].Index) {
        assert(packs[packIdx].ShapeClass < Layout.NumShapeClasses);
        uintptr_t count = packCounts[packs[packIdx].ShapeClass];

        if (!areWitnessTablePacksEqual(adata[argIdx], bdata[argIdx], count))
          return false;

        ++packIdx;
        ++argIdx;
        continue;
      }

      if (!areWitnessTablesEqual((const WitnessTable *)adata[argIdx],
                                 (const WitnessTable *)bdata[argIdx]))
        return false;

      ++argIdx;
    }

    assert(packIdx == Layout.NumPacks && "Missed a pack");
    return true;
  }

  uint32_t hash() const {
    return Hash;
  }

  const GenericSignatureLayout<InProcess> &layout() const { return Layout; }

  friend llvm::hash_code hash_value(const MetadataCacheKey &key) {
    return key.Hash;
  }

  const void * const *begin() const { return Data; }
  const void * const *end() const { return Data + size(); }
  unsigned size() const { return Layout.sizeInWords(); }

  void installInto(const void **buffer) const {
    MetadataCacheKey::installGenericArguments(
        Layout.sizeInWords(),
        Layout.NumPacks,
        Layout.PackShapeDescriptors,
        buffer, Data);
  }

private:
  uint32_t computeHash() const {
    size_t H = 0x56ba80d1u * Layout.NumKeyParameters;

    auto *packs = Layout.PackShapeDescriptors;
    unsigned packIdx = 0;

    auto update = [&H](uintptr_t value) {
      H = (H >> 10) | (H << ((sizeof(uintptr_t) * 8) - 10));
      H ^= (value ^ (value >> 19));
    };

    // FIXME: The first NumShapeClasses entries are pack counts;
    // incorporate them into the hash
    for (unsigned i = Layout.NumShapeClasses,
                  e = Layout.NumShapeClasses + Layout.NumKeyParameters;
         i != e; ++i) {
      // Is this entry a metadata pack?
      if (packIdx < Layout.NumPacks &&
          packs[packIdx].Kind == GenericPackKind::Metadata &&
          i == packs[packIdx].Index) {
        assert(packs[packIdx].ShapeClass < Layout.NumShapeClasses);
        auto count = reinterpret_cast<uintptr_t>(Data[packs[packIdx].ShapeClass]);
        ++packIdx;

        MetadataPackPointer pack(Data[i]);
        for (unsigned j = 0; j < count; ++j)
          update(reinterpret_cast<uintptr_t>(pack.getElements()[j]));

        continue;
      }

      update(reinterpret_cast<uintptr_t>(Data[i]));
    }

    H *= 0x27d4eb2d;

    // Rotate right by 10 and then truncate to 32 bits.
    return uint32_t((H >> 10) | (H << ((sizeof(uintptr_t) * 8) - 10)));
  }
};

/// A helper class for ConcurrentMap entry types which allows trailing objects
/// objects and automatically implements the getExtraAllocationSize methods
/// in terms of numTrailingObjects calls.
///
/// For each trailing object type T, the subclass must provide:
///   size_t numTrailingObjects(OverloadToken<T>) const;
///   static size_t numTrailingObjects(OverloadToken<T>, ...) const;
/// where the arguments to the latter are the arguments to getOrInsert,
/// including the key.
template <class Impl, class... Objects>
struct ConcurrentMapTrailingObjectsEntry
    : swift::ABI::TrailingObjects<Impl, Objects...> {
protected:
  using TrailingObjects =
      swift::ABI::TrailingObjects<Impl, Objects...>;

  Impl &asImpl() { return static_cast<Impl &>(*this); }
  const Impl &asImpl() const { return static_cast<const Impl &>(*this); }

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

public:
  template <class KeyType, class... Args>
  static size_t getExtraAllocationSize(const KeyType &key,
                                       Args &&...args) {
    return TrailingObjects::template additionalSizeToAlloc<Objects...>(
        Impl::numTrailingObjects(OverloadToken<Objects>(), key, args...)...);
  }
  size_t getExtraAllocationSize() const {
    return TrailingObjects::template additionalSizeToAlloc<Objects...>(
        asImpl().numTrailingObjects(OverloadToken<Objects>())...);
  }
};

/// Reserve the runtime extra space to use for its own tracking.
struct PrivateMetadataCompletionContext {
  MetadataCompletionContext Public;
};

/// The alignment required for objects that will be stored in
/// PrivateMetadataTrackingInfo.
const size_t PrivateMetadataTrackingAlignment = 16;

/// The wait queue object that we create for metadata that are
/// being actively initialized right now.
struct alignas(PrivateMetadataTrackingAlignment) MetadataWaitQueue :
  public AtomicWaitQueue<MetadataWaitQueue, ConcurrencyControl::LockType> {

  /// A pointer to the completion context being used to complete this
  /// metadata.  This is only actually filled in if:
  ///
  /// - the initializing thread is unable to complete the metadata,
  ///   but its request doesn't need it to, and
  /// - the current completion context is non-zero.  (Completion contexts
  ///   are initially zeroed, so this only happens if the initialization
  ///   actually stores to the context, which is uncommon.)
  ///
  /// This should only be touched by the initializing thread, i.e. the
  /// thread that holds the lock embedded in this object.
  std::unique_ptr<PrivateMetadataCompletionContext> PersistentContext;

  /// The dependency that is currently blocking this initialization.
  /// This should only be touched while holding the global lock
  /// for this metadata cache.
  MetadataDependency BlockingDependency;

  class Worker : public AtomicWaitQueue::Worker {
    using super = AtomicWaitQueue::Worker;
    PrivateMetadataState State = PrivateMetadataState::Allocating;
  public:
    Worker(ConcurrencyControl::LockType &globalLock) : super(globalLock) {}

    void flagCreatedQueueIsPublished() {
      // This method is called after successfully inserting an entry into
      // the atomic storage, at a point that just assumes that a queue
      // was created.  However, we may not have created a queue if the
      // metadata was completed during construction.
      //
      // Testing CurrentQueue to see if we published a queue is generally
      // suspect because we might be looping and calling createQueue()
      // on each iteration.  However, the metadata cache system won't do
      // this, at least on the path leading to the call to this method,
      // so this works in this one case.
      if (CurrentQueue) {
        assert(State < PrivateMetadataState::Complete);
        super::flagCreatedQueueIsPublished();
      } else {
        assert(State == PrivateMetadataState::Complete);
      }
    }

    void setState(PrivateMetadataState newState) {
      // It would be nice to assert isWorkerThread() here, but we need
      // this to be callable before we've published the queue.
      State = newState;
    }
    PrivateMetadataState getState() const {
      assert(isWorkerThread() || State == PrivateMetadataState::Complete);
      return State;
    }
  };
};

/// A record used to store information about an attempt to
/// complete a metadata when there's no active worker thread.
struct alignas(PrivateMetadataTrackingAlignment) SuspendedMetadataCompletion {
  MetadataDependency BlockingDependency;
  std::unique_ptr<PrivateMetadataCompletionContext> PersistentContext;

  SuspendedMetadataCompletion(MetadataDependency blockingDependency,
                              PrivateMetadataCompletionContext *context)
    : BlockingDependency(blockingDependency),
      PersistentContext(context) {}
};

class PrivateMetadataTrackingInfo {
public:
  using RawType = uintptr_t;

private:
  enum : RawType {
    StateMask = 0x7,
    PointerIsWaitQueueMask = 0x8,
    AllBitsMask = StateMask | PointerIsWaitQueueMask,
    PointerMask = ~AllBitsMask,
  };

  static_assert(AllBitsMask < PrivateMetadataTrackingAlignment,
                "too many bits for alignment");

  RawType Data;

public:
  // Some std::atomic implementations require a default constructor
  // for no apparent reason.
  PrivateMetadataTrackingInfo() : Data(0) {}

  explicit PrivateMetadataTrackingInfo(PrivateMetadataState state)
    : Data(RawType(state)) {}

  explicit PrivateMetadataTrackingInfo(PrivateMetadataState state,
                                       MetadataWaitQueue *queue)
    : Data(RawType(state) | reinterpret_cast<RawType>(queue)
                          | PointerIsWaitQueueMask) {
    assert(queue);
    assert(!(reinterpret_cast<RawType>(queue) & AllBitsMask));
  }
  explicit PrivateMetadataTrackingInfo(PrivateMetadataState state,
                                       SuspendedMetadataCompletion *suspended)
    : Data(RawType(state) | reinterpret_cast<RawType>(suspended)) {
    assert(!(reinterpret_cast<RawType>(suspended) & AllBitsMask));
  }

  static PrivateMetadataTrackingInfo
  initial(MetadataWaitQueue::Worker &worker,
          PrivateMetadataState initialState) {
    worker.setState(initialState);
    if (initialState != PrivateMetadataState::Complete)
      return PrivateMetadataTrackingInfo(initialState, worker.createQueue());
    return PrivateMetadataTrackingInfo(initialState);
  }

  PrivateMetadataState getState() const {
    return PrivateMetadataState(Data & StateMask);
  }

  /// Does the state mean that we've allocated metadata?
  bool hasAllocatedMetadata() const {
    return getState() != PrivateMetadataState::Allocating;
  }

  bool isComplete() const {
    return getState() == PrivateMetadataState::Complete;
  }

  bool hasWaitQueue() const {
    return Data & PointerIsWaitQueueMask;
  }
  MetadataWaitQueue *getWaitQueue() const {
    if (hasWaitQueue())
      return reinterpret_cast<MetadataWaitQueue*>(Data & PointerMask);
    return nullptr;
  }

  SuspendedMetadataCompletion *getSuspendedCompletion() const {
    if (!hasWaitQueue())
      return reinterpret_cast<SuspendedMetadataCompletion*>(Data & PointerMask);
    return nullptr;
  }

  /// Return the blocking dependency for this metadata.  Should only
  /// be called while holding the global lock for the metadata cache.
  MetadataDependency getBlockingDependency_locked() const {
    if (auto queue = getWaitQueue())
      return queue->BlockingDependency;
    if (auto dependency = getSuspendedCompletion())
      return dependency->BlockingDependency;
    return MetadataDependency();
  }

  bool satisfies(MetadataState requirement) {
    return swift::satisfies(getState(), requirement);
  }

  enum CheckResult {
    /// The request is satisfied.
    Satisfied,

    /// The request is not satisfied, and the requesting thread
    /// should report that immediately.
    Unsatisfied,

    /// The request is not satisfied, and the requesting thread
    /// must wait for another thread to complete the initialization.
    Wait,

    /// The request is not satisfied, and the requesting thread
    /// should try to complete the initialization itself.
    Resume,
  };

  CheckResult check(MetadataRequest request) {
    switch (getState()) {
    // Always wait if the metadata is still allocating.  Non-blocking
    // requests still need to allocate abstract metadata that
    // downstream consumers can report a dependency on.
    case PrivateMetadataState::Allocating:
      return Wait;

    // We never need to do anything if we're complete.  This is the
    // most common result.
    case PrivateMetadataState::Complete:
      return Satisfied;

    case PrivateMetadataState::Abstract:
    case PrivateMetadataState::LayoutComplete:
    case PrivateMetadataState::NonTransitiveComplete:
      // If the request is satisfied, we don't need to do anything.
      if (satisfies(request.getState()))
        return Satisfied;

      // If there isn't an running thread, we should take over
      // initialization.
      if (!hasWaitQueue())
        return Resume;

      // If this is a blocking request, we should wait.
      if (request.isBlocking())
        return Wait;

      // Otherwise, we should return that the request is unsatisfied.
      return Unsatisfied;
    }
    swift_unreachable("bad state");
  }
};

/// Given that this is the initializing thread, and we've reached the
/// given state, should we block wait for further initialization?
inline bool shouldBlockInitialization(PrivateMetadataState currentState,
                                      MetadataRequest request) {
  switch (currentState) {
  case PrivateMetadataState::Allocating:
    swift_unreachable("initialization hasn't allocated?");
  case PrivateMetadataState::Complete:
    return false;
  case PrivateMetadataState::Abstract:
  case PrivateMetadataState::LayoutComplete:
  case PrivateMetadataState::NonTransitiveComplete:
    if (satisfies(currentState, request.getState()))
      return false;
    return request.isBlocking();
  }
  swift_unreachable("bad state");
}

/// Block until the dependency is satisfied.
void blockOnMetadataDependency(MetadataDependency request,
                               MetadataDependency dependency);

/// A cache entry class which provides the basic mechanisms for two-phase
/// metadata initialization.  Suitable for more heavyweight metadata kinds
/// such as generic types and tuples.  Does not provide the lookup-related
/// members.
///
/// The value type may be an arbitrary type, but it must be contextually
/// convertible to bool, and it must be default-constructible in a false
/// state.
///
/// In addition to the lookup members required by ConcurrentMap, concrete
/// implementations should provide:
///
///   /// A name describing the map; used in debugging diagnostics.
///   static const char *getName();
///
///   /// A constructor which should set up an entry.  Note that this phase
///   /// of initialization may race with other threads attempting to set up
///   /// the same entry; do not do anything during it which might block or
///   /// need to be reverted.
///   /// The extra arguments are those provided to getOrInsert.
///   Entry(MetadataCacheKey key, ExtraArgTys...);
///
///   /// Allocate the metadata.
///   AllocationResult allocate(ExtraArgTys...);
///
///   /// Try to initialize the metadata.
///   MetadataStateWithDependency tryInitialize(Metadata *metadata,
///                                     PrivateMetadataState state,
///                                     PrivateMetadataCompletionContext *ctxt);
template <class Impl, class... Objects>
class MetadataCacheEntryBase
       : public ConcurrentMapTrailingObjectsEntry<Impl, Objects...> {
  using super = ConcurrentMapTrailingObjectsEntry<Impl, Objects...>;
public:
  using ValueType = Metadata *;
  using Status = MetadataResponse;
  using WaitQueue = MetadataWaitQueue;

protected:
  using TrailingObjectsEntry = super;
  using super::asImpl;

private:
  /// The current state of this metadata cache entry.
  ///
  /// All modifications of this field are performed while holding
  /// the global lock associated with this metadata cache.  This is
  /// because these modifications all coincide with changes to the wait
  /// queue reference: either installing, removing, or replacing it.
  /// The proper reference-counting of the queue object requires the
  /// lock to be held during these operations.  However, this field
  /// can be read without holding the global lock, as part of the fast
  /// path of several operations on the entry, most importantly
  /// requesting the metadata.
  ///
  /// Acquiring and releasing the global lock provides a certain
  /// amount of memory ordering.  Thus:
  /// - Reads from the field performed in fast paths without holding
  ///   the lock must be acquires in order to properly order memory
  ///   with the initializing thread.
  /// - Reads from the field that are performed under the lock can
  ///   be relaxed because the lock will properly order them.
  /// - Modifications of the field can be stores rather than
  ///   compare-exchanges, although they must still use release
  ///   ordering to guarantee proper ordering with code in the
  ///   fast paths.
  std::atomic<PrivateMetadataTrackingInfo> TrackingInfo;

  static constexpr std::memory_order TrackingInfoIsLockedOrder =
    std::memory_order_relaxed;

public:
  MetadataCacheEntryBase(MetadataWaitQueue::Worker &worker,
                         PrivateMetadataState initialState =
                           PrivateMetadataState::Allocating)
      : TrackingInfo(PrivateMetadataTrackingInfo::initial(worker, initialState)) {
  }

  // Note that having an explicit destructor here is important to make this
  // a non-POD class and allow subclass fields to be allocated in our
  // tail-padding.
  ~MetadataCacheEntryBase() {
  }

  /// Given that this thread doesn't own the right to initialize the
  /// metadata, await the metadata being in the right state.
  template <class... Args>
  Status await(ConcurrencyControl &concurrency, MetadataRequest request,
               Args &&...extraArgs) {
    return awaitSatisfyingState(concurrency, request);
  }

  Status getStatusToReturn(PrivateMetadataState state) {
    assert(state != PrivateMetadataState::Allocating);
    return { asImpl().getValue(), getAccomplishedRequestState(state) };
  }

  /// The expected return type of allocate.
  struct AllocationResult {
    Metadata *Value;
    PrivateMetadataState State;
  };

  /// Perform the allocation operation.
  template <class... Args>
  llvm::Optional<Status> beginAllocation(MetadataWaitQueue::Worker &worker,
                                         MetadataRequest request,
                                         Args &&... args) {
    // Returning a non-None value here will preempt initialization, so we
    // should only do it if we're reached PrivateMetadataState::Complete.

    // We can skip allocation if we were allocated during construction.
    auto state = worker.getState();
    if (state != PrivateMetadataState::Allocating) {
#ifndef NDEBUG
      // We've already published the metadata as part of construction,
      // so we can verify that the mangled name round-trips.
      if (asImpl().allowMangledNameVerification(std::forward<Args>(args)...))
        verifyMangledNameRoundtrip(asImpl().getValue());
#endif

      // Skip initialization, too, if we're fully complete.
      if (state == PrivateMetadataState::Complete) {
        assert(!worker.isWorkerThread());
        return Status{asImpl().getValue(), MetadataState::Complete};
      }

      // Otherwise, go directly to the initialization phase.
      assert(worker.isWorkerThread());
      return None;
    }

    assert(worker.isWorkerThread());

    // Allocate the metadata.
    AllocationResult allocationResult =
      asImpl().allocate(std::forward<Args>(args)...);
    state = allocationResult.State;
    worker.setState(state);

    // Set the self-link before publishing the new status.
    auto value = const_cast<ValueType>(allocationResult.Value);
    asImpl().setValue(value);

    // If allocation gave us complete metadata, we can short-circuit
    // initialization; publish and report that we've finished.
    if (state == PrivateMetadataState::Complete) {
      finishAndPublishProgress(worker, MetadataDependency(), nullptr);

#ifndef NDEBUG
      // Now that we've published the allocated metadata, verify that
      // the mangled name round-trips.
      if (asImpl().allowMangledNameVerification(std::forward<Args>(args)...))
        verifyMangledNameRoundtrip(value);
#endif

      return Status{allocationResult.Value, MetadataState::Complete};
    }

    // Otherwise, we always try at least one round of initialization
    // even if the request is for abstract metadata, just to avoid
    // doing more unnecessary bookkeeping.  Publish the current
    // state so that e.g. recursive uses of this metadata are
    // satisfiable.
    notifyWaitingThreadsOfProgress(worker, MetadataDependency());

#ifndef NDEBUG
    // Now that we've published the allocated metadata, verify that
    // the mangled name round-trips.
    if (asImpl().allowMangledNameVerification(std::forward<Args>(args)...))
      verifyMangledNameRoundtrip(value);
#endif

    return None;
  }

  template <class... Args>
  static bool allowMangledNameVerification(Args &&...args) {
    // By default, always allow mangled name verification.
    return true;
  }

  /// Begin initialization immediately after allocation.
  template <class... Args>
  Status beginInitialization(WaitQueue::Worker &worker,
                             MetadataRequest request, Args &&...args) {
    // Note that we ignore the extra arguments; those are just for the
    // constructor and allocation.
    return doInitialization(worker, request);
  }

private:
  /// Try to complete the metadata.
  ///
  /// This is the initializing thread.  The lock is not held.
  Status doInitialization(WaitQueue::Worker &worker,
                          MetadataRequest request) {
    assert(worker.isWorkerThread());

    assert(worker.getState() > PrivateMetadataState::Allocating);
    auto value = asImpl().getValue();

    auto queue = worker.getPublishedQueue();

    // Figure out a completion context to use.
    static const constexpr PrivateMetadataCompletionContext zeroContext = {};
    PrivateMetadataCompletionContext scratchContext;
    PrivateMetadataCompletionContext *context;
    if (auto persistent = queue->PersistentContext.get()) {
      context = persistent;
    } else {
      // Initialize the scratch context to zero.
      scratchContext = zeroContext;
      context = &scratchContext;
    }

    // Try the complete the metadata.  This only loops if initialization
    // has a dependency, but the new dependency is resolved when we go to
    // add ourselves to its queue.
    while (true) {
      assert(worker.getState() < PrivateMetadataState::Complete);

      // Try a round of initialization.
      auto oldState = worker.getState();
      MetadataStateWithDependency MetadataStateWithDependency =
        asImpl().tryInitialize(value, oldState, context);
      auto newState = MetadataStateWithDependency.NewState;
      auto dependency = MetadataStateWithDependency.Dependency;
      worker.setState(newState);

      assert(oldState <= newState &&
             "initialization regressed to an earlier state");

      // If we don't have a dependency, we're finished.
      bool done, willWait;
      if (!dependency) {
        assert(newState == PrivateMetadataState::Complete &&
               "initialization didn't report a dependency but isn't complete");
        done = true;
        willWait = false;
      } else {
        assert(newState != PrivateMetadataState::Complete &&
               "initialization reported a dependency but is complete");
        done = false;
        willWait = shouldBlockInitialization(newState, request);
      }

      // If we're not going to wait, but we're not done, and the
      // completion context is no longer zero, copy the completion
      // context into the persistent state (if it isn't already there).
      if (!willWait && !done && !queue->PersistentContext) {
        if (memcmp(&scratchContext, &zeroContext, sizeof(zeroContext)) != 0)
          queue->PersistentContext.reset(
            new PrivateMetadataCompletionContext(scratchContext));
      }

      // If we're not going to wait, publish the new state and finish
      // execution.
      if (!willWait) {
        finishAndPublishProgress(worker, dependency,
                                 queue->PersistentContext.release());
        return getStatusToReturn(newState);
      }

      // We're going to wait.  If we've made progress, make sure we notify
      // any waiting threads about that progress; if they're satisfied
      // by that progress, they shouldn't be blocked.
      if (oldState < newState) {
        notifyWaitingThreadsOfProgress(worker, dependency);

        // This might change the queue pointer.
        queue = worker.getPublishedQueue();

        assert(!queue->PersistentContext ||
               queue->PersistentContext.get() == context);
      }

      // Block on the target dependency.
      blockOnDependency(worker, request, MetadataStateWithDependency.Dependency);

      // Go back and try initialization again.
    }
  }

  /// Publish a new metadata state.  Wake waiters if we had any.
  void finishAndPublishProgress(MetadataWaitQueue::Worker &worker,
                                MetadataDependency dependency,
                                PrivateMetadataCompletionContext *context) {
    auto newState = worker.getState();

    // Create a suspended completion if there's something to record there.
    // This will be deallocated when some other thread takes over
    // initialization.
    SuspendedMetadataCompletion *suspended = nullptr;
    if (dependency || context) {
      assert(newState != PrivateMetadataState::Complete);
      suspended = new SuspendedMetadataCompletion(dependency, context);
    }

    // We're done with this worker thread; replace the wait queue
    // with the dependency record.  We still want to do these stores
    // under the lock, though.
    worker.finishAndUnpublishQueue([&] { 
      auto newInfo = PrivateMetadataTrackingInfo(newState, suspended);
      assert(newInfo.hasAllocatedMetadata());

      // Set the new state and unpublish the reference to the queue.
      TrackingInfo.store(newInfo, std::memory_order_release);
    });
  }

  /// Notify any waiting threads that metadata has made progress.
  void notifyWaitingThreadsOfProgress(MetadataWaitQueue::Worker &worker,
                                      MetadataDependency dependency) {
    worker.maybeReplaceQueue([&] {
      MetadataWaitQueue *oldQueue = worker.getPublishedQueue();
      MetadataWaitQueue *newQueue;

      // If there aren't any other references to the existing queue,
      // we don't need to replace anything.
      if (oldQueue->isUniquelyReferenced_locked()) {
        newQueue = oldQueue;

      // Otherwise, make a new queue.  Cycling queues this way allows
      // waiting threads to unblock if they are satisfied with the given
      // progress.  If they aren't, they'll wait on the new queue.
      } else {
        newQueue = worker.createReplacementQueue();
        newQueue->PersistentContext = std::move(oldQueue->PersistentContext);
      }

      // Update the current blocking dependency.
      newQueue->BlockingDependency = dependency;

      // Only the worker thread modifies TrackingInfo, so we can do a
      // simple store instead of a compare-exchange.
      PrivateMetadataTrackingInfo newTrackingInfo =
        PrivateMetadataTrackingInfo(worker.getState(), newQueue);
      TrackingInfo.store(newTrackingInfo, std::memory_order_release);

      // We signal to maybeReplaceQueue that replacement is required by
      // returning a non-null queue.
      return (newQueue != oldQueue ? newQueue : nullptr);
    });
  }

  /// Given that the request is not satisfied by the current state of
  /// the metadata, wait for the request to be satisfied.
  ///
  /// If there's a thread that currently owns initialization for this
  /// metadata (i.e. it has published a wait queue into TrackingInfo),
  /// we simply wait on that thread.  Otherwise, we take over
  /// initialization on the current thread.
  ///
  /// If the request is non-blocking, we do not wait, but we may need
  /// to take over initialization.
  Status awaitSatisfyingState(ConcurrencyControl &concurrency,
                              MetadataRequest request) {
    // Try loading the current state before acquiring the lock.
    auto trackingInfo = TrackingInfo.load(std::memory_order_acquire);

    // Return if the current state says to do so.
    auto checkResult = trackingInfo.check(request);
    if (checkResult == PrivateMetadataTrackingInfo::Satisfied ||
        checkResult == PrivateMetadataTrackingInfo::Unsatisfied)
      return getStatusToReturn(trackingInfo.getState());

    MetadataWaitQueue::Worker worker(concurrency.Lock);

    std::unique_ptr<SuspendedMetadataCompletion> suspendedCompletionToDelete;
    worker.withLock([&](MetadataWaitQueue::Worker::Operation &op) {
      assert(!worker.isWorkerThread());

      // Reload the tracking info, since it might have been
      // changed by a concurrent worker thread.
      trackingInfo = TrackingInfo.load(TrackingInfoIsLockedOrder);
      checkResult = trackingInfo.check(request);

      switch (checkResult) {
      // Either the request is satisfied or we should tell the
      // requester immediately that it isn't.
      case PrivateMetadataTrackingInfo::Satisfied:
      case PrivateMetadataTrackingInfo::Unsatisfied:
        return;

      // There's currently an initializing thread for this metadata,
      // and either we've got a blocking request that isn't yet
      // satisfied or the metadata hasn't even been allocated yet.
      // Wait on the thread and then call this lambda again.
      case PrivateMetadataTrackingInfo::Wait:
        assert(trackingInfo.hasWaitQueue());
        return op.waitAndRepeat(trackingInfo.getWaitQueue());

      // There isn't a thread currently building the metadata,
      // and the request isn't satisfied.  Become the initializing
      // thread and try to build the metadata ourselves.
      case PrivateMetadataTrackingInfo::Resume: {
        assert(!trackingInfo.hasWaitQueue());

        // Create a queue and publish it, taking over execution.
        auto queue = op.createQueue();

        // Copy the information from the suspended completion, if any,
        // into the queue.
        if (auto suspendedCompletion =
              trackingInfo.getSuspendedCompletion()) {
          queue->BlockingDependency =
            suspendedCompletion->BlockingDependency;
          queue->PersistentContext =
            std::move(suspendedCompletion->PersistentContext);

          // Make sure we delete the suspended completion later.
          suspendedCompletionToDelete.reset(suspendedCompletion);
        }

        // Publish the wait queue we just made.
        auto newTrackingInfo =
          PrivateMetadataTrackingInfo(trackingInfo.getState(), queue);
        TrackingInfo.store(newTrackingInfo, std::memory_order_release);

        return op.flagQueueIsPublished(queue);
      }
      }
    });

    // If the check result wasn't Resume, it must have been Satisfied
    // or Unsatisfied, and we should return immediately.
    if (checkResult != PrivateMetadataTrackingInfo::Resume) {
      assert(checkResult == PrivateMetadataTrackingInfo::Satisfied ||
             checkResult == PrivateMetadataTrackingInfo::Unsatisfied);
      return getStatusToReturn(trackingInfo.getState());
    }

    // Otherwise, we published and are now the worker thread owning
    // this metadata's initialization.  Do the initialization.
    worker.setState(trackingInfo.getState());
    return doInitialization(worker, request);
  }

  /// Given that we are the active worker thread for this initialization,
  /// block until the given dependency is satisfied.
  void blockOnDependency(MetadataWaitQueue::Worker &worker,
                         MetadataRequest request,
                         MetadataDependency dependency) {
    assert(worker.isWorkerThread());
    assert(request.isBlocking());

    // Formulate the request for this metadata as a dependency.
    auto requestDependency = MetadataDependency(asImpl().getValue(),
                                                request.getState());

    // Block on the metadata dependency.
    blockOnMetadataDependency(requestDependency, dependency);
  }

public:
  /// Check whether this metadata has reached the given state and,
  /// if not, return a further metadata dependency if possible.
  ///
  /// It's possible for this to not return a dependency, but only if some
  /// other thread is currently still attempting to complete the first
  /// full round of attempted initialization.  It's also possible
  /// for the reported dependency to be out of date.
  MetadataStateWithDependency
  checkDependency(ConcurrencyControl &concurrency, MetadataState requirement) {
    // Do a quick check while not holding the lock.
    auto curInfo = TrackingInfo.load(std::memory_order_acquire);
    if (curInfo.satisfies(requirement))
      return { curInfo.getState(), MetadataDependency() };

    // Alright, try again while holding the lock, which is required
    // in order to safely read the blocking dependency.
    return concurrency.Lock.withLock([&]() -> MetadataStateWithDependency {
      curInfo = TrackingInfo.load(TrackingInfoIsLockedOrder);

      if (curInfo.satisfies(requirement))
        return { curInfo.getState(), MetadataDependency() };

      return { curInfo.getState(), curInfo.getBlockingDependency_locked() };
    });
  }
};

/// An convenient subclass of MetadataCacheEntryBase which provides
/// metadata lookup using a variadic key.
template <class Impl, class... Objects>
class VariadicMetadataCacheEntryBase :
         public MetadataCacheEntryBase<Impl, const void *, Objects...> {
  using super = MetadataCacheEntryBase<Impl, const void *, Objects...>;

protected:
  using super::asImpl;

  using ValueType = typename super::ValueType;

  using TrailingObjectsEntry = typename super::TrailingObjectsEntry;
  friend TrailingObjectsEntry;

  using TrailingObjects = typename super::TrailingObjects;
  friend TrailingObjects;

  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

  size_t numTrailingObjects(OverloadToken<const void *>) const {
    return Layout.sizeInWords();
  }

  template <class... Args>
  static size_t numTrailingObjects(OverloadToken<const void *>,
                                   const MetadataCacheKey &key,
                                   Args &&...extraArgs) {
    return key.size();
  }

private:
  /// These are set during construction and never changed.
  const GenericSignatureLayout<InProcess> Layout;
  const uint32_t Hash;

  /// Valid if TrackingInfo.getState() >= PrivateMetadataState::Abstract.
  ValueType Value;

  friend super;
  ValueType getValue() {
    return Value;
  }
  void setValue(ValueType value) {
    Value = value;
  }

public:
  VariadicMetadataCacheEntryBase(const MetadataCacheKey &key,
                                 MetadataWaitQueue::Worker &worker,
                                 PrivateMetadataState initialState,
                                 ValueType value)
      : super(worker, initialState),
        Layout(key.layout()),
        Hash(key.hash()),
        Value(value) {
    assert((value != nullptr) ==
           (initialState != PrivateMetadataState::Allocating));
    key.installInto(this->template getTrailingObjects<const void *>());
  }

  MetadataCacheKey getKey() const {
    return MetadataCacheKey(Layout,
                            this->template getTrailingObjects<const void*>(),
                            Hash);
  }

  intptr_t getKeyIntValueForDump() const {
    return Hash;
  }

  friend llvm::hash_code hash_value(const VariadicMetadataCacheEntryBase<Impl, Objects...> &value) {
    return hash_value(value.getKey());
  }

  bool matchesKey(const MetadataCacheKey &key) const {
    return key == getKey();
  }
};

template <class EntryType, uint16_t Tag>
class MetadataCache :
    public LockingConcurrentMap<EntryType,
             LockingConcurrentMapStorage<EntryType, Tag>> {
};

} // namespace swift

#endif // SWIFT_RUNTIME_METADATACACHE_H
