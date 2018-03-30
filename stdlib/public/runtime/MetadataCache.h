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

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "swift/Runtime/Concurrent.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Mutex.h"
#include <condition_variable>
#include <thread>

#ifndef SWIFT_DEBUG_RUNTIME
#define SWIFT_DEBUG_RUNTIME 0
#endif

namespace swift {

class MetadataAllocator : public llvm::AllocatorBase<MetadataAllocator> {
public:
  void Reset() {}

  LLVM_ATTRIBUTE_RETURNS_NONNULL void *Allocate(size_t size, size_t alignment);
  using AllocatorBase<MetadataAllocator>::Allocate;

  void Deallocate(const void *Ptr, size_t size);
  using AllocatorBase<MetadataAllocator>::Deallocate;

  void PrintStats() const {}
};

/// A typedef for simple global caches.
template <class EntryTy>
using SimpleGlobalCache =
  ConcurrentMap<EntryTy, /*destructor*/ false, MetadataAllocator>;

template <class T, bool ProvideDestructor = true>
class StaticOwningPointer {
  T *Ptr;
public:
  StaticOwningPointer(T *ptr = nullptr) : Ptr(ptr) {}
  StaticOwningPointer(const StaticOwningPointer &) = delete;
  StaticOwningPointer &operator=(const StaticOwningPointer &) = delete;
  ~StaticOwningPointer() { delete Ptr; }

  T &operator*() const { return *Ptr; }
  T *operator->() const { return Ptr; }
};

template <class T>
class StaticOwningPointer<T, false> {
  T *Ptr;
public:
  StaticOwningPointer(T *ptr = nullptr) : Ptr(ptr) {}
  StaticOwningPointer(const StaticOwningPointer &) = delete;
  StaticOwningPointer &operator=(const StaticOwningPointer &) = delete;

  T &operator*() const { return *Ptr; }
  T *operator->() const { return Ptr; }
};

enum class ConcurrencyRequest {
  /// No special requests; proceed to calling finish.
  None,

  /// Acquire the lock and call the appropriate function.
  AcquireLockAndCallBack,

  /// Notify all waiters on the condition variable without acquiring the lock.
  NotifyAll,
};

struct ConcurrencyControl {
  Mutex Lock;
  ConditionVariable Queue;
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
///   beginAllocation(ConcurrencyControl &concurrency, ArgTys...);
///
///   /// Attempt to initialize an entry.  This is called once for the entry,
///   /// immediately after construction, by the thread that successfully
///   /// constructed the entry.
///   Status beginInitialization(ConcurrencyControl &concurrency, ArgTys...);
///
///   /// Attempt to resume initializing an entry.  Only one thread will be
///   /// trying this at once.  This only need to be implemented if
///   /// resumeInitialization is called on the map.
///   Status resumeInitialization(ConcurrencyControl &concurrency, ArgTys...);
///
///   /// Perform an enqueue operation.
///   /// This only needs to be implemented if enqueue is called on the map.
///   bool enqueueWithLock(ConcurrencyControl &concurrency, ArgTys...);
template <class EntryType, bool ProvideDestructor = true>
class LockingConcurrentMap {
  ConcurrentMap<EntryType, ProvideDestructor, MetadataAllocator> Map;

  using Status = typename EntryType::Status;

  StaticOwningPointer<ConcurrencyControl, ProvideDestructor> Concurrency;
public:
  LockingConcurrentMap() : Concurrency(new ConcurrencyControl()) {}

  MetadataAllocator &getAllocator() { return Map.getAllocator(); }

  template <class KeyType, class... ArgTys>
  std::pair<EntryType*, Status>
  getOrInsert(KeyType key, ArgTys &&...args) {
    auto result = Map.getOrInsert(key, args...);
    auto entry = result.first;

    // If we are not inserting the entry, we need to potentially block on 
    // currently satisfies our conditions.
    if (!result.second) {
      auto status =
        entry->await(*Concurrency, std::forward<ArgTys>(args)...);
      return { entry, status };
    }

    // Okay, we inserted.  We are responsible for allocating and
    // subsequently trying to initialize the entry.

    // Allocation.  This can fast-path and bypass initialization by returning
    // a status.
    if (auto status = entry->beginAllocation(*Concurrency, args...)) {
      return { entry, *status };
    }

    // Initialization.
    auto status = entry->beginInitialization(*Concurrency,
                                             std::forward<ArgTys>(args)...);
    return { entry, status };
  }

  template <class KeyType, class... ArgTys>
  std::pair<EntryType*, Status>
  resumeInitialization(KeyType key, ArgTys &&...args) {
    auto entry = Map.find(key);
    assert(entry && "entry doesn't already exist!");

    auto status =
      entry->resumeInitialization(*Concurrency, std::forward<ArgTys>(args)...);
    return { entry, status };
  }

  template <class KeyType, class... ArgTys>
  bool enqueue(KeyType key, ArgTys &&...args) {
    auto entry = Map.find(key);
    assert(entry && "entry doesn't already exist!");

    return entry->enqueue(*Concurrency, std::forward<ArgTys>(args)...);
  }

  /// Given that an entry already exists, await it.
  template <class KeyType, class... ArgTys>
  Status await(KeyType key, ArgTys &&...args) {
    auto entry = Map.find(key);
    assert(entry && "entry doesn't already exist!");

    return entry->await(*Concurrency, std::forward<ArgTys>(args)...);
  }
};

/// A base class for metadata cache entries which supports an unfailing
/// one-phase allocation strategy.
///
/// In addition to the requirements of ConcurrentMap, subclasses should
/// provide:
///
///   /// Allocate the cached entry.  This is not allowed to fail.
///   ValueType allocate(ArgTys...);
template <class Impl, class ValueType>
class SimpleLockingCacheEntryBase {
  static_assert(std::is_pointer<ValueType>::value,
                "value type must be a pointer type");

  static const uintptr_t Empty_NoWaiters = 0;
  static const uintptr_t Empty_HasWaiters = 1;
  static bool isSpecialValue(uintptr_t value) {
    return value <= Empty_HasWaiters;
  }

  std::atomic<uintptr_t> Value;

protected:
  Impl &asImpl() { return static_cast<Impl &>(*this); }
  const Impl &asImpl() const { return static_cast<const Impl &>(*this); }

  SimpleLockingCacheEntryBase() : Value(Empty_NoWaiters) {}

public:
  using Status = ValueType;

  template <class... ArgTys>
  Status await(ConcurrencyControl &concurrency, ArgTys &&...args) {
    // Load the value.  If this is not a special value, we're done.
    auto value = Value.load(std::memory_order_acquire);
    if (!isSpecialValue(value)) {
      return reinterpret_cast<ValueType>(value);
    }

    // The initializing thread will try to atomically swap in a valid value.
    // It can do that while we're holding the lock.  If it sees that there
    // aren't any waiters, it will not acquire the lock and will not try
    // to notify any waiters.  If it does see that there are waiters, it will
    // acquire the lock before notifying them in order to ensure that it
    // catches them all.  On the waiter side, we must set the has-waiters
    // flag while holding the lock.  This is because we otherwise can't be
    // sure that we'll have started waiting before the initializing thread
    // notifies the queue.
    //
    // We're adding a bit of complexity here for the advantage that, in the
    // absence of early contention, we never touch the lock at all.
    concurrency.Lock.withLockOrWait(concurrency.Queue, [&] {
      // Reload the current value.
      value = Value.load(std::memory_order_acquire);

      // If the value is still no-waiters, try to flag that
      // there's a waiter.  If that succeeds, we can go ahead and wait.
      if (value == Empty_NoWaiters &&
          Value.compare_exchange_strong(value, Empty_HasWaiters,
                                        std::memory_order_relaxed,
                                        std::memory_order_acquire))
        return false; // wait

      assert(value != Empty_NoWaiters);

      // If the value is already in the has-waiters state, we can go
      // ahead and wait.
      if (value == Empty_HasWaiters)
        return false; // wait

      // Otherwise, the initializing thread has finished, and we must not wait.
      return true;
    });

    return reinterpret_cast<ValueType>(value);
  }

  template <class... ArgTys>
  Optional<Status> beginAllocation(ConcurrencyControl &concurrency,
                                   ArgTys &&...args) {
    // Delegate to the implementation class.
    ValueType origValue = asImpl().allocate(std::forward<ArgTys>(args)...);

    auto value = reinterpret_cast<uintptr_t>(origValue);
    assert(!isSpecialValue(value) && "allocate returned a special value");

    // Publish the value.
    auto oldValue = Value.exchange(value, std::memory_order_release);
    assert(isSpecialValue(oldValue));

    // If there were any waiters, acquire the lock and notify the queue.
    if (oldValue != Empty_NoWaiters) {
      concurrency.Lock.withLockThenNotifyAll(concurrency.Queue, []{});
    }

    return origValue;
  }

  template <class... ArgTys>
  Status beginInitialization(ConcurrencyControl &concurrency,
                             ArgTys &&...args) {
    swift_runtime_unreachable("beginAllocation always short-circuits");
  }
};

// A wrapper around a pointer to a metadata cache entry that provides
// DenseMap semantics that compare values in the key vector for the metadata
// instance.
//
// This is stored as a pointer to the arguments buffer, so that we can save
// an offset while looking for the matching argument given a key.
class KeyDataRef {
  const void * const *Args;
  unsigned Length;

  KeyDataRef(const void * const *args, unsigned length)
    : Args(args), Length(length) {}

public:
  static KeyDataRef forArguments(const void * const *args,
                                 unsigned numArguments) {
    return KeyDataRef(args, numArguments);
  }

  bool operator==(KeyDataRef rhs) const {
    // Compare the sizes.
    unsigned asize = size(), bsize = rhs.size();
    if (asize != bsize) return false;

    // Compare the content.
    auto abegin = begin(), bbegin = rhs.begin();
    for (unsigned i = 0; i < asize; ++i)
      if (abegin[i] != bbegin[i]) return false;
    return true;
  }

  int compare(KeyDataRef rhs) const {
    // Compare the sizes.
    unsigned asize = size(), bsize = rhs.size();
    if (asize != bsize) {
      return (asize < bsize ? -1 : 1);
    }

    // Compare the content.
    auto abegin = begin(), bbegin = rhs.begin();
    for (unsigned i = 0; i < asize; ++i) {
      if (abegin[i] != bbegin[i])
        return (uintptr_t(abegin[i]) < uintptr_t(bbegin[i]) ? -1 : 1);
    }

    return 0;
  }

  size_t hash() {
    size_t H = 0x56ba80d1 * Length ;
    for (unsigned i = 0; i < Length; i++) {
      H = (H >> 10) | (H << ((sizeof(size_t) * 8) - 10));
      H ^= ((size_t)Args[i]) ^ ((size_t)Args[i] >> 19);
    }
    H *= 0x27d4eb2d;
    return (H >> 10) | (H << ((sizeof(size_t) * 8) - 10));
  }

  const void * const *begin() const { return Args; }
  const void * const *end() const { return Args + Length; }
  unsigned size() const { return Length; }
};

/// A key value as provided to the concurrent map.
struct MetadataCacheKey {
  size_t Hash;
  KeyDataRef KeyData;

  MetadataCacheKey(KeyDataRef data) : Hash(data.hash()), KeyData(data) {}
  MetadataCacheKey(const void *const *data, size_t size)
    : MetadataCacheKey(KeyDataRef::forArguments(data, size)) {}
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
  template <class... Args>
  static size_t getExtraAllocationSize(const MetadataCacheKey &key,
                                       Args &&...args) {
    return TrailingObjects::template additionalSizeToAlloc<Objects...>(
        Impl::numTrailingObjects(OverloadToken<Objects>(), key, args...)...);
  }
  size_t getExtraAllocationSize() const {
    return TrailingObjects::template additionalSizeToAlloc<Objects...>(
        asImpl().numTrailingObjects(OverloadToken<Objects>())...);
  }
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
  swift_runtime_unreachable("unsupported requirement kind");
}

class PrivateMetadataTrackingInfo {
public:
  using RawType = RawPrivateMetadataState;

private:
  enum : RawType {
    State_mask =      0x7,
    HasWaiters_mask = 0x8,
  };

  RawType Data;

public:
  explicit constexpr PrivateMetadataTrackingInfo(RawType data)
    : Data(data) {}
  explicit constexpr PrivateMetadataTrackingInfo(PrivateMetadataState state)
    : Data(RawType(state)) {}

  static constexpr PrivateMetadataTrackingInfo initial() {
    return PrivateMetadataTrackingInfo(PrivateMetadataState::Allocating);
  }

  PrivateMetadataState getState() const {
    return PrivateMetadataState(Data & State_mask);
  }

  /// Does the state mean that we've allocated metadata?
  bool hasAllocatedMetadata() const {
    return getState() != PrivateMetadataState::Allocating;
  }

  bool isComplete() const {
    return getState() == PrivateMetadataState::Complete;
  }

  bool hasWaiters() const { return Data & HasWaiters_mask; }
  PrivateMetadataTrackingInfo addWaiters() const {
    assert(!isComplete() && "adding waiters to completed state");
    return PrivateMetadataTrackingInfo(Data | HasWaiters_mask);
  }
  PrivateMetadataTrackingInfo removeWaiters() const {
    return PrivateMetadataTrackingInfo(Data & ~HasWaiters_mask);
  }

  MetadataState getAccomplishedRequestState() const {
    switch (getState()) {
    case PrivateMetadataState::Allocating:
      swift_runtime_unreachable("cannot call on allocating state");
    case PrivateMetadataState::Abstract:
      return MetadataState::Abstract;
    case PrivateMetadataState::LayoutComplete:
      return MetadataState::LayoutComplete;
    case PrivateMetadataState::NonTransitiveComplete:
      return MetadataState::NonTransitiveComplete;
    case PrivateMetadataState::Complete:
      return MetadataState::Complete;
    }
    swift_runtime_unreachable("bad state");
  }

  bool satisfies(MetadataState requirement) {
    return swift::satisfies(getState(), requirement);
  }

  bool shouldWait(MetadataRequest request) {
    switch (getState()) {
    // Always wait if we're allocating.  Non-blocking requests still need
    // to have an allocation that the downstream consumers can report
    // a dependency on.
    case PrivateMetadataState::Allocating:
      return true;

    // We never need to wait if we're complete.  This is the most common
    // result.
    case PrivateMetadataState::Complete:
      return false;

    case PrivateMetadataState::Abstract:
    case PrivateMetadataState::LayoutComplete:
    case PrivateMetadataState::NonTransitiveComplete:
      // Otherwise, if it's a non-blocking request, we do not need to block.
      return (request.isBlocking() && !satisfies(request.getState()));
    }
    swift_runtime_unreachable("bad state");
  }

  constexpr RawType getRawValue() const { return Data; }
  RawType &getRawValueRef() { return Data; }
};

/// Reserve the runtime extra space to use for its own tracking.
struct PrivateMetadataCompletionContext {
  MetadataCompletionContext Public;
};

struct MetadataCompletionQueueEntry {
  /// The metadata whose completion is blocked.
  Metadata * const Value;

  /// The next entry in the completion queue.
  std::unique_ptr<MetadataCompletionQueueEntry> Next;

  /// The saved state of the completion function.
  PrivateMetadataCompletionContext CompletionContext;

  Metadata *Dependency = nullptr;
  MetadataState DependencyRequirement = MetadataState::Abstract;

  MetadataCompletionQueueEntry(Metadata *value,
                               const PrivateMetadataCompletionContext &context)
    : Value(value), CompletionContext(context) {}
};

/// Add the given queue entry to the queue for the given metadata.
///
/// \return false if the entry was not added because the dependency
///   has already reached the desired requirement
bool addToMetadataQueue(std::unique_ptr<MetadataCompletionQueueEntry> &&queueEntry,
                        const Metadata *dependency,
                        MetadataState dependencyRequirement);


void resumeMetadataCompletion(
                    std::unique_ptr<MetadataCompletionQueueEntry> &&queueEntry);

/// A base class offerring a reasonable default implementation for entries
/// in a generic metadata cache.  Supports variably-sized keys.
///
/// The value type may be an arbitrary type, but it must be contextually
/// convertible to bool, and it must be default-constructible in a false
/// state.
///
/// Concrete implementations should provide:
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
///   TryInitializeResult tryInitialize(Metadata *metadata,
///                                     PrivateMetadataState state,
///                              PrivateMetadataCompletionContext *context,
///                                     ExtraArgTys...);
template <class Impl, class... Objects>
class MetadataCacheEntryBase
    : public ConcurrentMapTrailingObjectsEntry<Impl, const void *, Objects...> {
  using super =
             ConcurrentMapTrailingObjectsEntry<Impl, const void *, Objects...>;
  friend super;
  using TrailingObjects = typename super::TrailingObjects;
  friend TrailingObjects;

public:
  using ValueType = Metadata *;
  using Status = MetadataResponse;

protected:
  template<typename T>
  using OverloadToken = typename TrailingObjects::template OverloadToken<T>;

  size_t numTrailingObjects(OverloadToken<const void *>) const {
    return KeyLength;
  }

  template <class... Args>
  static size_t numTrailingObjects(OverloadToken<const void *>,
                                   const MetadataCacheKey &key,
                                   Args &&...extraArgs) {
    return key.KeyData.size();
  }

  using super::asImpl;

private:
  /// These are set during construction and never changed.
  const size_t Hash;
  const uint16_t KeyLength;

  /// What kind of data is stored in the LockedStorage field below?
  ///
  /// This is only ever modified under the lock.
  enum class LSK : uint8_t {
    AllocatingThread,
    CompletionQueue,
  };
  LSK LockedStorageKind;

  /// The current state of this metadata cache entry.
  ///
  /// This has to be stored as a PrivateMetadataTrackingInfo::RawType
  /// instead of a PrivateMetadataTrackingInfo because some of our
  /// targets don't support interesting structs as atomic types.
  std::atomic<PrivateMetadataTrackingInfo::RawType> TrackingInfo;

  /// Valid if TrackingInfo.getState() >= PrivateMetadataState::Abstract.
  ValueType Value;

  /// Additional storage that is only ever accessed under the lock.
  union LockedStorage_t {
    /// The thread that is allocating the entry.
    std::thread::id AllocatingThread;

    /// The completion queue.  This is only ever accessed under the lock.
    std::unique_ptr<MetadataCompletionQueueEntry> CompletionQueue;

    LockedStorage_t() {}
    ~LockedStorage_t() {}
  } LockedStorage;

public:
  MetadataCacheEntryBase(const MetadataCacheKey &key)
      : Hash(key.Hash), KeyLength(key.KeyData.size()),
        TrackingInfo(PrivateMetadataTrackingInfo::initial().getRawValue()) {
    LockedStorageKind = LSK::AllocatingThread;
    LockedStorage.AllocatingThread = std::this_thread::get_id();
    memcpy(this->template getTrailingObjects<const void*>(),
           key.KeyData.begin(),
           KeyLength * sizeof(void*));
  }

  ~MetadataCacheEntryBase() {
    if (LockedStorageKind == LSK::CompletionQueue)
      LockedStorage.CompletionQueue.~unique_ptr();
  }

  bool isBeingAllocatedByCurrentThread() const {
    return LockedStorageKind == LSK::AllocatingThread &&
           LockedStorage.AllocatingThread == std::this_thread::get_id();
  }

  KeyDataRef getKeyData() const {
    return KeyDataRef::forArguments(
                              this->template getTrailingObjects<const void*>(),
                                    KeyLength);
  }

  intptr_t getKeyIntValueForDump() const {
    return Hash;
  }

  int compareWithKey(const MetadataCacheKey &key) const {
    // Order by hash first, then by the actual key data.
    if (auto comparison = compareIntegers(key.Hash, Hash)) {
      return comparison;
    } else {
      return key.KeyData.compare(getKeyData());
    }
  }

  /// Given that this thread doesn't own the right to initialize the
  /// metadata, await the metadata being in the right state.
  template <class... Args>
  Status await(ConcurrencyControl &concurrency, MetadataRequest request,
               Args &&...extraArgs) {
    auto trackingInfo =
      PrivateMetadataTrackingInfo(TrackingInfo.load(std::memory_order_acquire));

    if (trackingInfo.shouldWait(request)) {
      awaitSatisfyingState(concurrency, request, trackingInfo);
    }

    assert(trackingInfo.hasAllocatedMetadata());
    return { Value, trackingInfo.getAccomplishedRequestState() };
  }

  /// The expected return type of allocate.
  struct AllocationResult {
    Metadata *Value;
    PrivateMetadataState State;
  };

  /// Perform the allocation operation.
  template <class... Args>
  Optional<Status>
  beginAllocation(ConcurrencyControl &concurrency, Args &&...args) {
    // Allocate the metadata.
    AllocationResult allocationResult =
      asImpl().allocate(std::forward<Args>(args)...);

    // Publish the value.
    Value = const_cast<ValueType>(allocationResult.Value);
    PrivateMetadataState newState = allocationResult.State;
    publishPrivateMetadataState(concurrency, newState);

    // If allocation gave us completed metadata, short-circuit initialization.
    if (allocationResult.State == PrivateMetadataState::Complete) {
      return Status{allocationResult.Value, MetadataState::Complete};
    }

    return None;
  }

  /// Begin initialization immediately after allocation.
  template <class... Args>
  Status beginInitialization(ConcurrencyControl &concurrency, Args &&...args) {
    return doInitialization(concurrency, nullptr, std::forward<Args>(args)...);
  }

  /// Resume initialization after a previous failure resulted in the
  /// metadata being enqueued on another metadata cache.
  ///
  /// We expect the first argument here to be of type
  /// std::unique_ptr<MetadataCompletionQueueEntry> &&.
  template <class... Args>
  Status resumeInitialization(ConcurrencyControl &concurrency, Args &&...args) {
    return doInitialization(concurrency, std::forward<Args>(args)...);
  }

protected:
  /// The expected return type of tryInitialize.
  struct TryInitializeResult {
    PrivateMetadataState NewState;
    MetadataState DependencyRequirement;
    const Metadata *Dependency;
  };

private:
  /// Try to complete the metadata.
  ///
  /// This is the initializing thread.  The lock is not held.
  template <class... Args>
  Status doInitialization(ConcurrencyControl &concurrency,
                     std::unique_ptr<MetadataCompletionQueueEntry> &&queueEntry,
                          MetadataRequest request,
                          Args &&...args) {
    // We should always have fully synchronized with any previous threads
    // that were processing the initialization, so a relaxed load is fine
    // here.  (This ordering is achieved by the locking which occurs as part
    // of queuing and dequeuing metadata.)
    auto curTrackingInfo =
      PrivateMetadataTrackingInfo(TrackingInfo.load(std::memory_order_relaxed));
    assert(curTrackingInfo.hasAllocatedMetadata());
    assert(!curTrackingInfo.isComplete());

    auto value = Value;

    // Figure out the completion context.
    PrivateMetadataCompletionContext scratchContext;
    PrivateMetadataCompletionContext *context;
    if (queueEntry) {
      context = &queueEntry->CompletionContext;
    } else {
      memset(&scratchContext, 0, sizeof(PrivateMetadataCompletionContext));
      context = &scratchContext;
    }

    // Try the complete the metadata.  This only loops if initialization
    // has a dependency, but the new dependency is resolved when we go to
    // add ourselves to its queue.
    bool hasProgress = false;
    while (true) {
      TryInitializeResult tryInitializeResult =
        asImpl().tryInitialize(value, curTrackingInfo.getState(), context,
                               args...);
      auto newState = tryInitializeResult.NewState;

      assert(curTrackingInfo.getState() <= newState &&
             "initialization regressed to an earlier state");

      // Publish the new state of the metadata (waking any waiting
      // threads immediately) if we've made any progress.  This seems prudent,
      // but it might mean acquiring the lock multiple times.
      if (curTrackingInfo.getState() < newState) {
        hasProgress = true;
        curTrackingInfo = PrivateMetadataTrackingInfo(newState);
        publishPrivateMetadataState(concurrency, newState);
      }

      // If we don't have a dependency, we're finished.
      if (!tryInitializeResult.Dependency) {
        assert(newState == PrivateMetadataState::Complete &&
               "initialization didn't report a dependency but isn't complete");
        hasProgress = true;
        break;
      }

      assert(newState != PrivateMetadataState::Complete &&
             "completed initialization reported a dependency");

      // Otherwise, we need to block this metadata on the dependency's queue.

      // Create a queue entry if necessary.  Start using its context
      // as the continuation context.
      if (!queueEntry) {
        queueEntry.reset(
          new MetadataCompletionQueueEntry(value, scratchContext));
        context = &queueEntry->CompletionContext;
      }

      // Try to block this metadata initialization on that queue.
      // If this succeeds, we can't consider ourselves the initializing
      // thread anymore.  The small amount of notification we do at the
      // end of this function is okay to race with another thread
      // potentially taking over initialization.
      if (addToMetadataQueue(std::move(queueEntry),
                             tryInitializeResult.Dependency,
                             tryInitializeResult.DependencyRequirement))
        break;

      // If that failed, we should still have ownership of the entry.
      assert(queueEntry);
    }

    // If we made progress, claim all the completion-queue entries that
    // are now satisfied and try to make progress on them.
    if (hasProgress) {
      auto queue = concurrency.Lock.withLock([&] {
        return claimSatisfiedQueueEntriesWithLock(curTrackingInfo);
      });

      // Immediately process all the entries we extracted.
      while (auto cur = std::move(queue)) {
        queue = std::move(cur->Next);
        resumeMetadataCompletion(std::move(cur));
      }
    }

    // If we're not actually satisfied by the current state, we might need
    // to block here.
    if (curTrackingInfo.shouldWait(request)) {
      awaitSatisfyingState(concurrency, request, curTrackingInfo);
    }

    return { value, curTrackingInfo.getAccomplishedRequestState() };
  }

  /// Claim all the satisfied completion queue entries, given that
  /// we're holding the lock.
  std::unique_ptr<MetadataCompletionQueueEntry>
  claimSatisfiedQueueEntriesWithLock(PrivateMetadataTrackingInfo newInfo) {
    // Collect anything in the metadata's queue whose target state has been
    // reached to the queue in result.  Note that we repurpose the Next field
    // in the collected entries.

    std::unique_ptr<MetadataCompletionQueueEntry> result;

    // If we're not even currently storing a completion queue,
    // there's nothing to do but wake waiting threads.
    if (LockedStorageKind != LSK::CompletionQueue) {
      return result;
    }

    auto nextToResume = &result;
    assert(!*nextToResume && "already items in queue?");

    // Walk the completion queue.
    auto *nextWaiter = &LockedStorage.CompletionQueue;
    while (auto waiter = nextWaiter->get()) {
      // If the new state of this entry doesn't satisfy the waiter's
      // requirements, skip over it.
      if (!newInfo.satisfies(waiter->DependencyRequirement)) {
        nextWaiter = &waiter->Next;
        continue;
      }

      // Add the waiter to the end of the next-to-resume queue, and update
      // the end to the waiter's Next field.
      *nextToResume = std::move(*nextWaiter); // owning pointer to waiter
      nextToResume = &waiter->Next;

      // Splice the waiter out of the completion queue.
      *nextWaiter = std::move(waiter->Next);

      assert(!*nextToResume);
    }

    return result;
  }

  /// Publish a new metadata state.  Wake waiters if we had any.
  void publishPrivateMetadataState(ConcurrencyControl &concurrency,
                                   PrivateMetadataState newState) {
    auto newInfo = PrivateMetadataTrackingInfo(newState);
    assert(newInfo.hasAllocatedMetadata());
    assert(!newInfo.hasWaiters());

    auto oldInfo = PrivateMetadataTrackingInfo(
      TrackingInfo.exchange(newInfo.getRawValue(), std::memory_order_release));
    assert(!oldInfo.isComplete());

    // If we have existing waiters, wake them now, since we no longer
    // remember in State that we have any.
    if (oldInfo.hasWaiters()) {
      // We need to acquire the lock.  There could be an arbitrary number
      // of threads simultaneously trying to set the has-waiters flag, and we
      // have to make sure they start waiting before we notify the queue.
      concurrency.Lock.withLockThenNotifyAll(concurrency.Queue, [] {});
    }
  }

  /// Wait for the request to be satisfied by the current state.
  void awaitSatisfyingState(ConcurrencyControl &concurrency,
                            MetadataRequest request,
                            PrivateMetadataTrackingInfo &trackingInfo) {
    concurrency.Lock.withLockOrWait(concurrency.Queue, [&] {
      // Re-load the state now that we have the lock.  If we don't
      // need to wait, we're done.  Otherwise, flag the existence of a
      // waiter; if that fails, start over with the freshly-loaded state.
      trackingInfo = PrivateMetadataTrackingInfo(
                                  TrackingInfo.load(std::memory_order_acquire));
      while (true) {
        if (!trackingInfo.shouldWait(request))
          return true;

        if (trackingInfo.hasWaiters())
          break;

        // Try to swap in the has-waiters bit.  If this succeeds, we can
        // ahead and wait.
        if (TrackingInfo.compare_exchange_weak(trackingInfo.getRawValueRef(),
                                        trackingInfo.addWaiters().getRawValue(),
                                        std::memory_order_relaxed,
                                        std::memory_order_acquire))
          break;
      }

      // As a QoI safe-guard against the simplest form of cyclic
      // dependency, check whether this thread is the one responsible
      // for allocating the metadata.
      if (isBeingAllocatedByCurrentThread()) {
        fprintf(stderr,
                "%s(%p): cyclic metadata dependency detected, aborting\n",
                Impl::getName(), static_cast<const void*>(this));
        abort();
      }

      return false;
    });
  }

public:
  /// Block a metadata initialization on the completion of this
  /// initialization.
  ///
  /// This is always called from the initializing thread.  The lock is not held.
  bool enqueue(ConcurrencyControl &concurrency,
               std::unique_ptr<MetadataCompletionQueueEntry> &&queueEntry) {
    assert(queueEntry);
    assert(!queueEntry->Next);

    return concurrency.Lock.withLock([&] {
      auto curInfo = PrivateMetadataTrackingInfo(
                                  TrackingInfo.load(std::memory_order_acquire));
      if (curInfo.satisfies(queueEntry->DependencyRequirement))
        return false;

      // Note that we don't set the waiters bit because we're not actually
      // blocking any threads.

      // Transition the locked storage to the completion queue.
      if (LockedStorageKind != LSK::CompletionQueue) {
        LockedStorageKind = LSK::CompletionQueue;
        new (&LockedStorage.CompletionQueue)
          std::unique_ptr<MetadataCompletionQueueEntry>();
      }

      queueEntry->Next = std::move(LockedStorage.CompletionQueue);
      LockedStorage.CompletionQueue = std::move(queueEntry);
      return true;
    });
  }
};

template <class EntryType, bool ProvideDestructor = true>
class MetadataCache :
    public LockingConcurrentMap<EntryType, ProvideDestructor> {
};

} // namespace swift

#endif // SWIFT_RUNTIME_METADATACACHE_H
