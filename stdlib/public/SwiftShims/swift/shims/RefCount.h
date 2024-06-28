//===--- RefCount.h ---------------------------------------------*- C++ -*-===//
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
#ifndef SWIFT_STDLIB_SHIMS_REFCOUNT_H
#define SWIFT_STDLIB_SHIMS_REFCOUNT_H

#include "Visibility.h"
#include "SwiftStdint.h"

// This definition is a placeholder for importing into Swift.
// It provides size and alignment but cannot be manipulated safely there.
typedef struct {
  __swift_uintptr_t refCounts;
} InlineRefCountsPlaceholder;

#if defined(__swift__)

typedef InlineRefCountsPlaceholder InlineRefCounts;

#else

#include <type_traits>
#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#include "HeapObject.h"
#include "swift/Basic/type_traits.h"
#include "swift/Runtime/Atomic.h"
#include "swift/Runtime/Config.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/Heap.h"

/*
  An object conceptually has three refcounts. These refcounts
  are stored either "inline" in the field following the isa
  or in a "side table entry" pointed to by the field following the isa.

  The strong RC counts strong references to the object. When the strong RC
  reaches zero the object is deinited, unowned reference reads become errors,
  and weak reference reads become nil. The strong RC is stored as an extra
  count: when the physical field is 0 the logical value is 1.

  The unowned RC counts unowned references to the object. The unowned RC
  also has an extra +1 on behalf of the strong references; this +1 is
  decremented after deinit completes. When the unowned RC reaches zero
  the object's allocation is freed.

  The weak RC counts weak references to the object. The weak RC also has an
  extra +1 on behalf of the unowned references; this +1 is decremented
  after the object's allocation is freed. When the weak RC reaches zero
  the object's side table entry is freed.

  Objects initially start with no side table. They can gain a side table when:
  * a weak reference is formed
  and pending future implementation:
  * strong RC or unowned RC overflows (inline RCs will be small on 32-bit)
  * associated object storage is needed on an object
  * etc
  Gaining a side table entry is a one-way operation; an object with a side
  table entry never loses it. This prevents some thread races.

  Strong and unowned variables point at the object.
  Weak variables point at the object's side table.


  Storage layout:

  HeapObject {
    isa
    InlineRefCounts {
      atomic<InlineRefCountBits> {
        strong RC + unowned RC + flags
        OR
        HeapObjectSideTableEntry*
      }
    }
  }

  HeapObjectSideTableEntry {
    SideTableRefCounts {
      object pointer
      atomic<SideTableRefCountBits> {
        strong RC + unowned RC + weak RC + flags
      }
    }
  }

  InlineRefCounts and SideTableRefCounts share some implementation
  via RefCounts<T>.

  InlineRefCountBits and SideTableRefCountBits share some implementation
  via RefCountBitsT<bool>.

  In general: The InlineRefCounts implementation tries to perform the
  operation inline. If the object has a side table it calls the
  HeapObjectSideTableEntry implementation which in turn calls the
  SideTableRefCounts implementation.
  Downside: this code is a bit twisted.
  Upside: this code has less duplication than it might otherwise


  Object lifecycle state machine:

  LIVE without side table
  The object is alive.
  Object's refcounts are initialized as 1 strong, 1 unowned, 1 weak.
  No side table. No weak RC storage.
  Strong variable operations work normally.
  Unowned variable operations work normally.
  Weak variable load can't happen.
  Weak variable store adds the side table, becoming LIVE with side table.
  When the strong RC reaches zero deinit() is called and the object
    becomes DEINITING.

  LIVE with side table
  Weak variable operations work normally.
  Everything else is the same as LIVE.

  DEINITING without side table
  deinit() is in progress on the object.
  Strong variable operations have no effect.
  Unowned variable load halts in swift_abortRetainUnowned().
  Unowned variable store works normally.
  Weak variable load can't happen.
  Weak variable store stores nil.
  When deinit() completes, the generated code calls swift_deallocObject.
    swift_deallocObject calls canBeFreedNow() checking for the fast path
    of no weak or unowned references.
    If canBeFreedNow() the object is freed and it becomes DEAD.
    Otherwise, it decrements the unowned RC and the object becomes DEINITED.

  DEINITING with side table
  Weak variable load returns nil.
  Weak variable store stores nil.
  canBeFreedNow() is always false, so it never transitions directly to DEAD.
  Everything else is the same as DEINITING.

  DEINITED without side table
  deinit() has completed but there are unowned references outstanding.
  Strong variable operations can't happen.
  Unowned variable store can't happen.
  Unowned variable load halts in swift_abortRetainUnowned().
  Weak variable operations can't happen.
  When the unowned RC reaches zero, the object is freed and it becomes DEAD.

  DEINITED with side table
  Weak variable load returns nil.
  Weak variable store can't happen.
  When the unowned RC reaches zero, the object is freed, the weak RC is
    decremented, and the object becomes FREED.
  Everything else is the same as DEINITED.

  FREED without side table
  This state never happens.

  FREED with side table
  The object is freed but there are weak refs to the side table outstanding.
  Strong variable operations can't happen.
  Unowned variable operations can't happen.
  Weak variable load returns nil.
  Weak variable store can't happen.
  When the weak RC reaches zero, the side table entry is freed and
    the object becomes DEAD.

  DEAD
  The object and its side table are gone.
*/

namespace swift {
  struct HeapObject;
  class HeapObjectSideTableEntry;
}

// FIXME: HACK: copied from HeapObject.cpp
extern "C" SWIFT_LIBRARY_VISIBILITY SWIFT_NOINLINE SWIFT_USED void
_swift_release_dealloc(swift::HeapObject *object);

namespace swift {

// RefCountIsInline: refcount stored in an object
// RefCountNotInline: refcount stored in an object's side table entry
enum RefCountInlinedness { RefCountNotInline = false, RefCountIsInline = true };

enum PerformDeinit { DontPerformDeinit = false, DoPerformDeinit = true };


// Raw storage of refcount bits, depending on pointer size and inlinedness.
// 32-bit inline refcount is 32-bits. All others are 64-bits.

template <RefCountInlinedness refcountIsInline, size_t sizeofPointer>
struct RefCountBitsInt;

// 64-bit inline
// 64-bit out of line
template <RefCountInlinedness refcountIsInline>
struct RefCountBitsInt<refcountIsInline, 8> {
  typedef uint64_t Type;
  typedef int64_t SignedType;
};

// 32-bit out of line
template <>
struct RefCountBitsInt<RefCountNotInline, 4> {
  typedef uint64_t Type;
  typedef int64_t SignedType;
};

// 32-bit inline
template <>
struct RefCountBitsInt<RefCountIsInline, 4> {
  typedef uint32_t Type;
  typedef int32_t SignedType;  
};


// Layout of refcount bits.
// field value = (bits & mask) >> shift
// FIXME: redo this abstraction more cleanly
  
# define maskForField(name) (((uint64_t(1)<<name##BitCount)-1) << name##Shift)
# define shiftAfterField(name) (name##Shift + name##BitCount)

template <size_t sizeofPointer>
struct RefCountBitOffsets;

// 64-bit inline
// 64-bit out of line
// 32-bit out of line
template <>
struct RefCountBitOffsets<8> {
  /*
   The bottom 32 bits (on 64 bit architectures, fewer on 32 bit) of the refcount
   field are effectively a union of two different configurations:
   
   ---Normal case---
   Bit 0: Does this object need to call out to the ObjC runtime for deallocation
   Bits 1-31: Unowned refcount
   
   ---Immortal case---
   All bits set, the object does not deallocate or have a refcount
   */
  static const size_t PureSwiftDeallocShift = 0;
  static const size_t PureSwiftDeallocBitCount = 1;
  static const uint64_t PureSwiftDeallocMask = maskForField(PureSwiftDealloc);

  static const size_t UnownedRefCountShift = shiftAfterField(PureSwiftDealloc);
  static const size_t UnownedRefCountBitCount = 31;
  static const uint64_t UnownedRefCountMask = maskForField(UnownedRefCount);

  static const size_t IsImmortalShift = 0; // overlaps PureSwiftDealloc and UnownedRefCount
  static const size_t IsImmortalBitCount = 32;
  static const uint64_t IsImmortalMask = maskForField(IsImmortal);

  static const size_t IsDeinitingShift = shiftAfterField(UnownedRefCount);
  static const size_t IsDeinitingBitCount = 1;
  static const uint64_t IsDeinitingMask = maskForField(IsDeiniting);

  static const size_t StrongExtraRefCountShift = shiftAfterField(IsDeiniting);
  static const size_t StrongExtraRefCountBitCount = 30;
  static const uint64_t StrongExtraRefCountMask = maskForField(StrongExtraRefCount);
  
  static const size_t UseSlowRCShift = shiftAfterField(StrongExtraRefCount);
  static const size_t UseSlowRCBitCount = 1;
  static const uint64_t UseSlowRCMask = maskForField(UseSlowRC);

  static const size_t SideTableShift = 0;
  static const size_t SideTableBitCount = 62;
  static const uint64_t SideTableMask = maskForField(SideTable);
  static const size_t SideTableUnusedLowBits = 3;

  static const size_t SideTableMarkShift = SideTableBitCount;
  static const size_t SideTableMarkBitCount = 1;
  static const uint64_t SideTableMarkMask = maskForField(SideTableMark);
};

// 32-bit inline
template <>
struct RefCountBitOffsets<4> {
  static const size_t PureSwiftDeallocShift = 0;
  static const size_t PureSwiftDeallocBitCount = 1;
  static const uint32_t PureSwiftDeallocMask = maskForField(PureSwiftDealloc);
  
  static const size_t UnownedRefCountShift = shiftAfterField(PureSwiftDealloc);
  static const size_t UnownedRefCountBitCount = 7;
  static const uint32_t UnownedRefCountMask = maskForField(UnownedRefCount);

  static const size_t IsImmortalShift = 0; // overlaps PureSwiftDealloc and UnownedRefCount
  static const size_t IsImmortalBitCount = 8;
  static const uint32_t IsImmortalMask = maskForField(IsImmortal);

  static const size_t IsDeinitingShift = shiftAfterField(UnownedRefCount);
  static const size_t IsDeinitingBitCount = 1;
  static const uint32_t IsDeinitingMask = maskForField(IsDeiniting);
  
  static const size_t StrongExtraRefCountShift = shiftAfterField(IsDeiniting);
  static const size_t StrongExtraRefCountBitCount = 22;
  static const uint32_t StrongExtraRefCountMask = maskForField(StrongExtraRefCount);
  
  static const size_t UseSlowRCShift = shiftAfterField(StrongExtraRefCount);
  static const size_t UseSlowRCBitCount = 1;
  static const uint32_t UseSlowRCMask = maskForField(UseSlowRC);

  static const size_t SideTableShift = 0;
  static const size_t SideTableBitCount = 30;
  static const uint32_t SideTableMask = maskForField(SideTable);
  static const size_t SideTableUnusedLowBits = 2;

  static const size_t SideTableMarkShift = SideTableBitCount;
  static const size_t SideTableMarkBitCount = 1;
  static const uint32_t SideTableMarkMask = maskForField(SideTableMark);
};


// FIXME: reinstate these assertions
#if 0
  static_assert(StrongExtraRefCountShift == IsDeinitingShift + 1,
                "IsDeiniting must be LSB-wards of StrongExtraRefCount");
  static_assert(UseSlowRCShift + UseSlowRCBitCount == sizeof(bits)*8,
                "UseSlowRC must be MSB");
  static_assert(SideTableBitCount + SideTableMarkBitCount +
                UseSlowRCBitCount == sizeof(bits)*8,
               "wrong bit count for RefCountBits side table encoding");
  static_assert(UnownedRefCountBitCount +
                IsDeinitingBitCount + StrongExtraRefCountBitCount +
                UseSlowRCBitCount == sizeof(bits)*8,
                "wrong bit count for RefCountBits refcount encoding");
#endif


// Basic encoding of refcount and flag data into the object's header.
template <RefCountInlinedness refcountIsInline>
class RefCountBitsT {

  friend class RefCountBitsT<RefCountIsInline>;
  friend class RefCountBitsT<RefCountNotInline>;
  
  static const RefCountInlinedness Inlinedness = refcountIsInline;

  typedef typename RefCountBitsInt<refcountIsInline, sizeof(void*)>::Type
    BitsType;
  typedef typename RefCountBitsInt<refcountIsInline, sizeof(void*)>::SignedType
    SignedBitsType;
  typedef RefCountBitOffsets<sizeof(BitsType)>
    Offsets;

  BitsType bits;

  // "Bitfield" accessors.

# define getFieldIn(bits, offsets, name)                                \
    ((bits & offsets::name##Mask) >> offsets::name##Shift)
# define setFieldIn(bits, offsets, name, val)                           \
    bits = ((bits & ~offsets::name##Mask) |                             \
            (((BitsType(val) << offsets::name##Shift) & offsets::name##Mask)))
  
# define getField(name) getFieldIn(bits, Offsets, name)
# define setField(name, val) setFieldIn(bits, Offsets, name, val)  
# define copyFieldFrom(src, name)                                       \
    setFieldIn(bits, Offsets, name,                                     \
               getFieldIn(src.bits, decltype(src)::Offsets, name))

  // RefCountBits uses always_inline everywhere
  // to improve performance of debug builds.
  
  private:
    SWIFT_ALWAYS_INLINE
    bool getUseSlowRC() const { return bool(getField(UseSlowRC)); }

    SWIFT_ALWAYS_INLINE
    void setUseSlowRC(bool value) { setField(UseSlowRC, value); }

  public:
  
  enum Immortal_t { Immortal };

  SWIFT_ALWAYS_INLINE
  bool isImmortal(bool checkSlowRCBit) const {
    if (checkSlowRCBit) {
      return (getField(IsImmortal) == Offsets::IsImmortalMask) &&
           bool(getField(UseSlowRC));
    } else {
      return (getField(IsImmortal) == Offsets::IsImmortalMask);
    }
  }

  SWIFT_ALWAYS_INLINE
  bool isOverflowingUnownedRefCount(uint32_t oldValue, uint32_t inc) const {
    auto newValue = getUnownedRefCount();
    return newValue != oldValue + inc ||
      newValue == Offsets::UnownedRefCountMask >> Offsets::UnownedRefCountShift;
  }

  SWIFT_ALWAYS_INLINE
  void setIsImmortal(bool value) {
    assert(value);
    setField(IsImmortal, Offsets::IsImmortalMask);
    setField(UseSlowRC, value);
  }

  SWIFT_ALWAYS_INLINE
  bool pureSwiftDeallocation() const {
    return bool(getField(PureSwiftDealloc)) && !bool(getField(UseSlowRC));
  }

  SWIFT_ALWAYS_INLINE
  void setPureSwiftDeallocation(bool value) {
    setField(PureSwiftDealloc, value);
  }

  SWIFT_ALWAYS_INLINE
  static constexpr BitsType immortalBits() {
    return (BitsType(2) << Offsets::StrongExtraRefCountShift) |
           (BitsType(Offsets::IsImmortalMask)) |
           (BitsType(1) << Offsets::UseSlowRCShift);
  }

  SWIFT_ALWAYS_INLINE
  RefCountBitsT() = default;

  SWIFT_ALWAYS_INLINE
  constexpr
  RefCountBitsT(uint32_t strongExtraCount, uint32_t unownedCount)
    : bits((BitsType(strongExtraCount) << Offsets::StrongExtraRefCountShift) |
           (BitsType(1)                << Offsets::PureSwiftDeallocShift) |
           (BitsType(unownedCount)     << Offsets::UnownedRefCountShift))
  { }

  SWIFT_ALWAYS_INLINE
  constexpr
  RefCountBitsT(Immortal_t immortal)
  : bits(immortalBits())
  { }

  SWIFT_ALWAYS_INLINE
  RefCountBitsT(HeapObjectSideTableEntry* side)
    : bits((reinterpret_cast<BitsType>(side) >> Offsets::SideTableUnusedLowBits)
           | (BitsType(1) << Offsets::UseSlowRCShift)
           | (BitsType(1) << Offsets::SideTableMarkShift))
  {
    assert(refcountIsInline);
  }

  SWIFT_ALWAYS_INLINE
  RefCountBitsT(const RefCountBitsT<RefCountIsInline> *newbitsPtr) {
    bits = 0;

    assert(newbitsPtr && "expected non null newbits");
    RefCountBitsT<RefCountIsInline> newbits = *newbitsPtr;
    
    if (refcountIsInline || sizeof(newbits) == sizeof(*this)) {
      // this and newbits are both inline
      // OR this is out-of-line but the same layout as inline.
      // (FIXME: use something cleaner than sizeof for same-layout test)
      // Copy the bits directly.
      bits = newbits.bits;
    }
    else {
      // this is out-of-line and not the same layout as inline newbits.
      // Copy field-by-field. If it's immortal, just set that.
      if (newbits.isImmortal(false)) {
        setField(IsImmortal, Offsets::IsImmortalMask);
      } else {
        copyFieldFrom(newbits, PureSwiftDealloc);
        copyFieldFrom(newbits, UnownedRefCount);
        copyFieldFrom(newbits, IsDeiniting);
        copyFieldFrom(newbits, StrongExtraRefCount);
        copyFieldFrom(newbits, UseSlowRC);
      }
    }
  }

  SWIFT_ALWAYS_INLINE
  bool hasSideTable() const {
    bool hasSide = getUseSlowRC() && !isImmortal(false);

    // Side table refcount must not point to another side table.
    assert((refcountIsInline || !hasSide)  &&
           "side table refcount must not have a side table entry of its own");
    
    return hasSide;
  }

  SWIFT_ALWAYS_INLINE
  HeapObjectSideTableEntry *getSideTable() const {
    assert(hasSideTable());

    // Stored value is a shifted pointer.
    return reinterpret_cast<HeapObjectSideTableEntry *>
      (uintptr_t(getField(SideTable)) << Offsets::SideTableUnusedLowBits);
  }

  SWIFT_ALWAYS_INLINE
  uint32_t getUnownedRefCount() const {
    assert(!hasSideTable());
    return uint32_t(getField(UnownedRefCount));
  }

  SWIFT_ALWAYS_INLINE
  bool getIsDeiniting() const {
    assert(!hasSideTable());
    return bool(getField(IsDeiniting));
  }

  SWIFT_ALWAYS_INLINE
  uint32_t getStrongExtraRefCount() const {
    assert(!hasSideTable());
    return uint32_t(getField(StrongExtraRefCount));
  }

  SWIFT_ALWAYS_INLINE
  void setHasSideTable(bool value) {
    bits = 0;
    setUseSlowRC(value);
  }

  SWIFT_ALWAYS_INLINE
  void setSideTable(HeapObjectSideTableEntry *side) {
    assert(hasSideTable());
    // Stored value is a shifted pointer.
    uintptr_t value = reinterpret_cast<uintptr_t>(side);
    uintptr_t storedValue = value >> Offsets::SideTableUnusedLowBits;
    assert(storedValue << Offsets::SideTableUnusedLowBits == value);
    setField(SideTable, storedValue);
    setField(SideTableMark, 1);
  }

  SWIFT_ALWAYS_INLINE
  void setUnownedRefCount(uint32_t value) {
    assert(!hasSideTable());
    setField(UnownedRefCount, value);
  }

  SWIFT_ALWAYS_INLINE
  void setIsDeiniting(bool value) {
    assert(!hasSideTable());
    setField(IsDeiniting, value);
  }

  SWIFT_ALWAYS_INLINE
  void setStrongExtraRefCount(uint32_t value) {
    assert(!hasSideTable());
    setField(StrongExtraRefCount, value);
  }
  

  // Returns true if the increment is a fast-path result.
  // Returns false if the increment should fall back to some slow path
  // (for example, because UseSlowRC is set or because the refcount overflowed).
  SWIFT_NODISCARD SWIFT_ALWAYS_INLINE bool
  incrementStrongExtraRefCount(uint32_t inc) {
    // This deliberately overflows into the UseSlowRC field.
    bits += BitsType(inc) << Offsets::StrongExtraRefCountShift;
    return (SignedBitsType(bits) >= 0);
  }

  // Returns true if the decrement is a fast-path result.
  // Returns false if the decrement should fall back to some slow path
  // (for example, because UseSlowRC is set
  // or because the refcount is now zero and should deinit).
  SWIFT_NODISCARD SWIFT_ALWAYS_INLINE bool
  decrementStrongExtraRefCount(uint32_t dec) {
#ifndef NDEBUG
    if (!hasSideTable() && !isImmortal(false)) {
      // Can't check these assertions with side table present.

      if (getIsDeiniting())
        assert(getStrongExtraRefCount() >= dec  &&
               "releasing reference whose refcount is already zero");
      else
        assert(getStrongExtraRefCount() + 1 >= dec  &&
               "releasing reference whose refcount is already zero");
    }
#endif

    // If we're decrementing by more than 1, then this underflow might end up
    // subtracting away an existing 1 in UseSlowRC. Check that separately. This
    // check should be constant folded away for the swift_release case.
    if (dec > 1 && getUseSlowRC())
      return false;

    // This deliberately underflows by borrowing from the UseSlowRC field.
    bits -= BitsType(dec) << Offsets::StrongExtraRefCountShift;
    return (SignedBitsType(bits) >= 0);
  }

  // Returns the old reference count before the increment.
  SWIFT_ALWAYS_INLINE
  uint32_t incrementUnownedRefCount(uint32_t inc) {
    uint32_t old = getUnownedRefCount();
    setUnownedRefCount(old + inc);
    return old;
  }

  SWIFT_ALWAYS_INLINE
  void decrementUnownedRefCount(uint32_t dec) {
    setUnownedRefCount(getUnownedRefCount() - dec);
  }

  SWIFT_ALWAYS_INLINE
  bool isUniquelyReferenced() {
    static_assert(Offsets::UnownedRefCountBitCount +
                  Offsets::IsDeinitingBitCount +
                  Offsets::StrongExtraRefCountBitCount +
                  Offsets::PureSwiftDeallocBitCount +
                  Offsets::UseSlowRCBitCount == sizeof(bits)*8,
                  "inspect isUniquelyReferenced after adding fields");

    // Unowned: don't care (FIXME: should care and redo initForNotFreeing)
    // IsDeiniting: false
    // StrongExtra: 0
    // UseSlowRC: false

    // Compiler is clever enough to optimize this.
    return
      !getUseSlowRC() && !getIsDeiniting() && getStrongExtraRefCount() == 0;
  }

  SWIFT_ALWAYS_INLINE
  BitsType getBitsValue() {
    return bits;
  }

# undef getFieldIn
# undef setFieldIn
# undef getField
# undef setField
# undef copyFieldFrom
};

# undef maskForField
# undef shiftAfterField

typedef RefCountBitsT<RefCountIsInline> InlineRefCountBits;

class alignas(sizeof(void*) * 2) SideTableRefCountBits : public RefCountBitsT<RefCountNotInline>
{
  uint32_t weakBits;

  public:
    SWIFT_ALWAYS_INLINE
    SideTableRefCountBits() = default;

    SWIFT_ALWAYS_INLINE
    constexpr SideTableRefCountBits(uint32_t strongExtraCount,
                                    uint32_t unownedCount)
        : RefCountBitsT<RefCountNotInline>(strongExtraCount, unownedCount)
          // weak refcount starts at 1 on behalf of the unowned count
          ,
          weakBits(1) {}

    SWIFT_ALWAYS_INLINE
    SideTableRefCountBits(HeapObjectSideTableEntry *side) = delete;

    SWIFT_ALWAYS_INLINE
    SideTableRefCountBits(InlineRefCountBits newbits)
        : RefCountBitsT<RefCountNotInline>(&newbits), weakBits(1) {}

    SWIFT_ALWAYS_INLINE
    void incrementWeakRefCount() { weakBits++; }

    SWIFT_ALWAYS_INLINE
    bool decrementWeakRefCount() {
      assert(weakBits > 0);
      weakBits--;
      return weakBits == 0;
  }

  SWIFT_ALWAYS_INLINE
  uint32_t getWeakRefCount() {
    return weakBits;
  }

  // Side table ref count never has a side table of its own.
  SWIFT_ALWAYS_INLINE
  bool hasSideTable() {
    return false;
  }
};


// Barriers
//
// Strong refcount increment is unordered with respect to other memory locations
//
// Strong refcount decrement is a release operation with respect to other
// memory locations. When an object's reference count becomes zero,
// an acquire fence is performed before beginning Swift deinit or ObjC
// -dealloc code. This ensures that the deinit code sees all modifications
// of the object's contents that were made before the object was released.
//
// Unowned and weak increment and decrement are all unordered.
// There is no deinit equivalent for these counts so no fence is needed.
//
// Accessing the side table requires that refCounts be accessed with
// a load-consume. Only code that is guaranteed not to try dereferencing
// the side table may perform a load-relaxed of refCounts.
// Similarly, storing the new side table pointer into refCounts is a
// store-release, but most other stores into refCounts are store-relaxed.

template <typename RefCountBits>
class RefCounts {
  std::atomic<RefCountBits> refCounts;

  // Out-of-line slow paths.

  SWIFT_NOINLINE
  HeapObject *incrementSlow(RefCountBits oldbits, uint32_t inc);

  SWIFT_NOINLINE
  void incrementNonAtomicSlow(RefCountBits oldbits, uint32_t inc);

  SWIFT_NOINLINE
  bool tryIncrementSlow(RefCountBits oldbits);

  SWIFT_NOINLINE
  bool tryIncrementNonAtomicSlow(RefCountBits oldbits);

  SWIFT_NOINLINE
  void incrementUnownedSlow(uint32_t inc);

  public:
  enum Initialized_t { Initialized };
  enum Immortal_t { Immortal };

  // RefCounts must be trivially constructible to avoid ObjC++
  // destruction overhead at runtime. Use RefCounts(Initialized)
  // to produce an initialized instance.
  RefCounts() = default;
  
  // Refcount of a new object is 1.
  constexpr RefCounts(Initialized_t)
    : refCounts(RefCountBits(0, 1)) {}

  // Refcount of an immortal object has top and bottom bits set
  constexpr RefCounts(Immortal_t)
  : refCounts(RefCountBits(RefCountBits::Immortal)) {}
  
  void init() {
    refCounts.store(RefCountBits(0, 1), std::memory_order_relaxed);
  }

  // Initialize for a stack promoted object. This prevents that the final
  // release frees the memory of the object.
  // FIXME: need to mark these and assert they never get a side table,
  // because the extra unowned ref will keep the side table alive forever
  void initForNotFreeing() {
    refCounts.store(RefCountBits(0, 2), std::memory_order_relaxed);
  }
  
  // Initialize for an object which will never deallocate.
  void initImmortal() {
    refCounts.store(RefCountBits(RefCountBits::Immortal), std::memory_order_relaxed);
  }
  
  void setIsImmortal(bool immortal) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (oldbits.isImmortal(true)) {
      return;
    }
    RefCountBits newbits;
    do {
      newbits = oldbits;
      newbits.setIsImmortal(immortal);
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
  }
  
  void setPureSwiftDeallocation(bool nonobjc) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);

    // Having a side table precludes using bits this way, but also precludes
    // doing the pure Swift deallocation path. So trying to turn this off
    // on something that has a side table is a noop
    if (!nonobjc && oldbits.hasSideTable()) {
      return;
    }
    // Immortal and no objc complications share a bit, so don't let setting
    // the complications one clear the immortal one
    if (oldbits.isImmortal(true) || oldbits.pureSwiftDeallocation() == nonobjc){
      assert(!oldbits.hasSideTable());
      return;
    }
    RefCountBits newbits;
    do {
      newbits = oldbits;
      newbits.setPureSwiftDeallocation(nonobjc);
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
  }
  
  bool getPureSwiftDeallocation() {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    return bits.pureSwiftDeallocation();
  }
  
  // Initialize from another refcount bits.
  // Only inline -> out-of-line is allowed (used for new side table entries).
  void init(InlineRefCountBits newBits) {
    refCounts.store(newBits, std::memory_order_relaxed);
  }

  // Increment the reference count.
  //
  // This returns the enclosing HeapObject so that it the result of this call
  // can be directly returned from swift_retain. This makes the call to
  // incrementSlow() a tail call.
  SWIFT_ALWAYS_INLINE
  HeapObject *increment(uint32_t inc = 1) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    
    // constant propagation will remove this in swift_retain, it should only
    // be present in swift_retain_n
    if (inc != 1 && oldbits.isImmortal(true)) {
      return getHeapObject();
    }
    
    RefCountBits newbits;
    do {
      newbits = oldbits;
      bool fast = newbits.incrementStrongExtraRefCount(inc);
      if (SWIFT_UNLIKELY(!fast)) {
        if (oldbits.isImmortal(false))
          return getHeapObject();
        return incrementSlow(oldbits, inc);
      }
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
    return getHeapObject();
  }

  SWIFT_ALWAYS_INLINE
  void incrementNonAtomic(uint32_t inc = 1) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    
    // constant propagation will remove this in swift_retain, it should only
    // be present in swift_retain_n
    if (inc != 1 && oldbits.isImmortal(true)) {
      return;
    }
    
    auto newbits = oldbits;
    bool fast = newbits.incrementStrongExtraRefCount(inc);
    if (SWIFT_UNLIKELY(!fast)) {
      if (oldbits.isImmortal(false))
        return;
      return incrementNonAtomicSlow(oldbits, inc);
    }
    refCounts.store(newbits, std::memory_order_relaxed);
 }

  // Increment the reference count, unless the object is deiniting.
  SWIFT_ALWAYS_INLINE
  bool tryIncrement() {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    RefCountBits newbits;
    do {
      if (!oldbits.hasSideTable() && oldbits.getIsDeiniting())
        return false;

      newbits = oldbits;
      bool fast = newbits.incrementStrongExtraRefCount(1);
      if (SWIFT_UNLIKELY(!fast)) {
        if (oldbits.isImmortal(false))
          return true;
        return tryIncrementSlow(oldbits);
      }
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
    return true;
  }

  SWIFT_ALWAYS_INLINE
  bool tryIncrementNonAtomic() {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (!oldbits.hasSideTable() && oldbits.getIsDeiniting())
      return false;

    auto newbits = oldbits;
    bool fast = newbits.incrementStrongExtraRefCount(1);
    if (SWIFT_UNLIKELY(!fast)) {
      if (oldbits.isImmortal(false))
        return true;
      return tryIncrementNonAtomicSlow(oldbits);
    }
    refCounts.store(newbits, std::memory_order_relaxed);
    return true;
  }

  // Decrement the reference count.
  // Return true if the caller should now deinit the object.
  SWIFT_ALWAYS_INLINE
  bool decrementShouldDeinit(uint32_t dec) {
    return doDecrement<DontPerformDeinit>(dec);
  }

  SWIFT_ALWAYS_INLINE
  bool decrementShouldDeinitNonAtomic(uint32_t dec) {
    return doDecrementNonAtomic<DontPerformDeinit>(dec);
  }

  SWIFT_ALWAYS_INLINE
  void decrementAndMaybeDeinit(uint32_t dec) {
    doDecrement<DoPerformDeinit>(dec);
  }

  SWIFT_ALWAYS_INLINE
  void decrementAndMaybeDeinitNonAtomic(uint32_t dec) {
    doDecrementNonAtomic<DoPerformDeinit>(dec);
  }

  // Non-atomically release the last strong reference and mark the
  // object as deiniting.
  //
  // Precondition: the reference count must be 1
  SWIFT_ALWAYS_INLINE
  void decrementFromOneNonAtomic() {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (bits.isImmortal(true)) {
      return;
    }
    if (bits.hasSideTable())
      return bits.getSideTable()->decrementFromOneNonAtomic();
    
    assert(!bits.getIsDeiniting());
    assert(bits.getStrongExtraRefCount() == 0 && "Expect a refcount of 1");
    bits.setStrongExtraRefCount(0);
    bits.setIsDeiniting(true);
    refCounts.store(bits, std::memory_order_relaxed);
  }

  // Return the reference count.
  // Once deinit begins the reference count is undefined.
  uint32_t getCount() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (bits.hasSideTable())
      return bits.getSideTable()->getCount();
    
    return bits.getStrongExtraRefCount() + 1;
  }

  // Return whether the reference count is exactly 1.
  // Once deinit begins the reference count is undefined.
  bool isUniquelyReferenced() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (bits.hasSideTable())
      return bits.getSideTable()->isUniquelyReferenced();
    
    assert(!bits.getIsDeiniting());
    return bits.isUniquelyReferenced();
  }

  // Return true if the object has started deiniting.
  bool isDeiniting() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (bits.hasSideTable())
      return bits.getSideTable()->isDeiniting();
    else
      return bits.getIsDeiniting();
  }

  bool hasSideTable() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    return bits.hasSideTable();
  }

  void *getSideTable() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (!bits.hasSideTable())
      return nullptr;
    return bits.getSideTable();
  }

  /// Return true if the object can be freed directly right now.
  /// (transition DEINITING -> DEAD)
  /// This is used in swift_deallocObject().
  /// Can be freed now means:  
  ///   no side table
  ///   unowned reference count is 1
  /// The object is assumed to be deiniting with no strong references already.
  bool canBeFreedNow() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    return (!bits.hasSideTable() &&
            bits.getIsDeiniting() &&
            bits.getStrongExtraRefCount() == 0 &&
            bits.getUnownedRefCount() == 1);
  }

  private:

  // Second slow path of doDecrement, where the
  // object may have a side table entry.
  template <PerformDeinit performDeinit>
  bool doDecrementSideTable(RefCountBits oldbits, uint32_t dec);

  // Second slow path of doDecrementNonAtomic, where the
  // object may have a side table entry.
  template <PerformDeinit performDeinit>
  bool doDecrementNonAtomicSideTable(RefCountBits oldbits, uint32_t dec);

  // First slow path of doDecrement, where the object may need to be deinited.
  // Side table is handled in the second slow path, doDecrementSideTable().
  template <PerformDeinit performDeinit>
  bool doDecrementSlow(RefCountBits oldbits, uint32_t dec) {
    RefCountBits newbits;
    
    // Constant propagation will remove this in swift_release, it should only
    // be present in swift_release_n.
    if (dec != 1 && oldbits.isImmortal(true)) {
      return false;
    }
    
    bool deinitNow;
    do {
      newbits = oldbits;
      
      bool fast =
        newbits.decrementStrongExtraRefCount(dec);
      if (fast) {
        // Decrement completed normally. New refcount is not zero.
        deinitNow = false;
      }
      else if (oldbits.isImmortal(false)) {
        return false;
      } else if (oldbits.hasSideTable()) {
        // Decrement failed because we're on some other slow path.
        return doDecrementSideTable<performDeinit>(oldbits, dec);
      }
      else {
        // Decrement underflowed. Begin deinit.
        // LIVE -> DEINITING
        deinitNow = true;
        assert(!oldbits.getIsDeiniting());  // FIXME: make this an error?
        newbits = oldbits;  // Undo failed decrement of newbits.
        newbits.setStrongExtraRefCount(0);
        newbits.setIsDeiniting(true);
      }
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
    if (performDeinit && deinitNow) {
      std::atomic_thread_fence(std::memory_order_acquire);
      _swift_release_dealloc(getHeapObject());
    }

    return deinitNow;
  }

  // First slow path of doDecrementNonAtomic, where the object may need to be deinited.
  // Side table is handled in the second slow path, doDecrementNonAtomicSideTable().
  template <PerformDeinit performDeinit>
  bool doDecrementNonAtomicSlow(RefCountBits oldbits, uint32_t dec) {
    bool deinitNow;
    auto newbits = oldbits;
    
    // constant propagation will remove this in swift_release, it should only
    // be present in swift_release_n
    if (dec != 1 && oldbits.isImmortal(true)) {
      return false;
    }

    bool fast =
      newbits.decrementStrongExtraRefCount(dec);
    if (fast) {
      // Decrement completed normally. New refcount is not zero.
      deinitNow = false;
    }
    else if (oldbits.isImmortal(false)) {
      return false;
    }
    else if (oldbits.hasSideTable()) {
      // Decrement failed because we're on some other slow path.
      return doDecrementNonAtomicSideTable<performDeinit>(oldbits, dec);
    }
    else {
      // Decrement underflowed. Begin deinit.
      // LIVE -> DEINITING
      deinitNow = true;
      assert(!oldbits.getIsDeiniting());  // FIXME: make this an error?
      newbits = oldbits;  // Undo failed decrement of newbits.
      newbits.setStrongExtraRefCount(0);
      newbits.setIsDeiniting(true);
    }
    refCounts.store(newbits, std::memory_order_relaxed);
    if (performDeinit && deinitNow) {
      _swift_release_dealloc(getHeapObject());
    }

    return deinitNow;
  }
  
  public:  // FIXME: access control hack

  // Fast path of atomic strong decrement.
  // 
  // Deinit is optionally handled directly instead of always deferring to 
  // the caller because the compiler can optimize this arrangement better.
  template <PerformDeinit performDeinit>
  SWIFT_ALWAYS_INLINE
  bool doDecrement(uint32_t dec) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    RefCountBits newbits;
    
    // constant propagation will remove this in swift_release, it should only
    // be present in swift_release_n
    if (dec != 1 && oldbits.isImmortal(true)) {
      return false;
    }
    
    do {
      newbits = oldbits;
      bool fast =
        newbits.decrementStrongExtraRefCount(dec);
      if (SWIFT_UNLIKELY(!fast)) {
        if (oldbits.isImmortal(false)) {
            return false;
        }
        // Slow paths include side table; deinit; underflow
        return doDecrementSlow<performDeinit>(oldbits, dec);
      }
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));

    return false;  // don't deinit
  }

  // This is independently specialized below for inline and out-of-line use.
  template <PerformDeinit performDeinit>
  bool doDecrementNonAtomic(uint32_t dec);


  // UNOWNED
  
  public:
  // Increment the unowned reference count.
  void incrementUnowned(uint32_t inc) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (oldbits.isImmortal(true))
      return;
    RefCountBits newbits;
    do {
      if (oldbits.hasSideTable())
        return oldbits.getSideTable()->incrementUnowned(inc);

      newbits = oldbits;
      assert(newbits.getUnownedRefCount() != 0);
      uint32_t oldValue = newbits.incrementUnownedRefCount(inc);

      // Check overflow and use the side table on overflow.
      if (newbits.isOverflowingUnownedRefCount(oldValue, inc))
        return incrementUnownedSlow(inc);

    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
  }

  void incrementUnownedNonAtomic(uint32_t inc) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (oldbits.isImmortal(true))
      return;
    if (oldbits.hasSideTable())
      return oldbits.getSideTable()->incrementUnownedNonAtomic(inc);

    auto newbits = oldbits;
    assert(newbits.getUnownedRefCount() != 0);
    uint32_t oldValue = newbits.incrementUnownedRefCount(inc);

    // Check overflow and use the side table on overflow.
    if (newbits.isOverflowingUnownedRefCount(oldValue, inc))
      return incrementUnownedSlow(inc);

    refCounts.store(newbits, std::memory_order_relaxed);
  }

  // Decrement the unowned reference count.
  // Return true if the caller should free the object.
  bool decrementUnownedShouldFree(uint32_t dec) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (oldbits.isImmortal(true))
      return false;
    RefCountBits newbits;
    
    bool performFree;
    do {
      if (oldbits.hasSideTable())
        return oldbits.getSideTable()->decrementUnownedShouldFree(dec);

      newbits = oldbits;
      newbits.decrementUnownedRefCount(dec);
      if (newbits.getUnownedRefCount() == 0) {
        // DEINITED -> FREED, or DEINITED -> DEAD
        // Caller will free the object. Weak decrement is handled by
        // HeapObjectSideTableEntry::decrementUnownedShouldFree.
        assert(newbits.getIsDeiniting());
        performFree = true;
      } else {
        performFree = false;
      }
      // FIXME: underflow check?
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
    return performFree;
  }

  bool decrementUnownedShouldFreeNonAtomic(uint32_t dec) {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (oldbits.isImmortal(true))
      return false;
    if (oldbits.hasSideTable())
      return oldbits.getSideTable()->decrementUnownedShouldFreeNonAtomic(dec);

    bool performFree;
    auto newbits = oldbits;
    newbits.decrementUnownedRefCount(dec);
    if (newbits.getUnownedRefCount() == 0) {
      // DEINITED -> FREED, or DEINITED -> DEAD
      // Caller will free the object. Weak decrement is handled by
      // HeapObjectSideTableEntry::decrementUnownedShouldFreeNonAtomic.
      assert(newbits.getIsDeiniting());
      performFree = true;
    } else {
      performFree = false;
    }
    // FIXME: underflow check?
    refCounts.store(newbits, std::memory_order_relaxed);
    return performFree;
  }

  // Return unowned reference count.
  // Note that this is not equal to the number of outstanding unowned pointers.
  uint32_t getUnownedCount() const {
    auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    if (bits.hasSideTable())
      return bits.getSideTable()->getUnownedCount();
    else 
      return bits.getUnownedRefCount();
  }


  // WEAK
  
  public:
  // Returns the object's side table entry (creating it if necessary) with
  // its weak ref count incremented.
  // Returns nullptr if the object is already deiniting.
  // Use this when creating a new weak reference to an object.
  HeapObjectSideTableEntry* formWeakReference();

  // Increment the weak reference count.
  void incrementWeak() {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    RefCountBits newbits;
    do {
      newbits = oldbits;
      assert(newbits.getWeakRefCount() != 0);
      newbits.incrementWeakRefCount();
      
      if (newbits.getWeakRefCount() < oldbits.getWeakRefCount())
        swift_abortWeakRetainOverflow();
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));
  }
  
  bool decrementWeakShouldCleanUp() {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
    RefCountBits newbits;

    bool performFree;
    do {
      newbits = oldbits;
      performFree = newbits.decrementWeakRefCount();
    } while (!refCounts.compare_exchange_weak(oldbits, newbits,
                                              std::memory_order_relaxed));

    return performFree;
  }

  bool decrementWeakShouldCleanUpNonAtomic() {
    auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);

    auto newbits = oldbits;
    auto performFree = newbits.decrementWeakRefCount();
    refCounts.store(newbits, std::memory_order_relaxed);

    return performFree;
  }
  
  // Return weak reference count.
  // Note that this is not equal to the number of outstanding weak pointers.
  uint32_t getWeakCount() const;

#ifndef NDEBUG
  bool isImmutableCOWBuffer();
  bool setIsImmutableCOWBuffer(bool immutable);
#endif

  // DO NOT TOUCH.
  // This exists for the benefits of the Refcounting.cpp tests. Do not use it
  // elsewhere.
  auto getBitsValue()
    -> decltype(auto) {
    return refCounts.load(std::memory_order_relaxed).getBitsValue();
  }

  void dump() const;

  private:
  HeapObject *getHeapObject();
  
  HeapObjectSideTableEntry* allocateSideTable(bool failIfDeiniting);
};

typedef RefCounts<InlineRefCountBits> InlineRefCounts;
typedef RefCounts<SideTableRefCountBits> SideTableRefCounts;

static_assert(std::is_trivially_destructible<InlineRefCounts>::value,
              "InlineRefCounts must be trivially destructible");

template <>
inline uint32_t RefCounts<InlineRefCountBits>::getWeakCount() const;
template <>
inline uint32_t RefCounts<SideTableRefCountBits>::getWeakCount() const;

class HeapObjectSideTableEntry {
  // FIXME: does object need to be atomic?
  std::atomic<HeapObject*> object;
  SideTableRefCounts refCounts;

#ifndef NDEBUG
  // Used for runtime consistency checking of COW buffers.
  bool immutableCOWBuffer = false;
#endif

  public:
  HeapObjectSideTableEntry(HeapObject *newObject)
    : object(newObject), 
#if __arm__ || __powerpc__ // https://github.com/apple/swift/issues/48416
   refCounts(SideTableRefCounts::Initialized)
#else
   refCounts()
#endif
  { }

  void *operator new(size_t) = delete;
  void operator delete(void *) = delete;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
  static ptrdiff_t refCountsOffset() {
    return offsetof(HeapObjectSideTableEntry, refCounts);
  }
#pragma clang diagnostic pop

  HeapObject* tryRetain() {
    if (refCounts.tryIncrement())
      return object.load(std::memory_order_relaxed);
    else
      return nullptr;
  }

  void initRefCounts(InlineRefCountBits newbits) {
    refCounts.init(newbits);
  }

  HeapObject *unsafeGetObject() const {
    return object.load(std::memory_order_relaxed);
  }

  // STRONG
  
  void incrementStrong(uint32_t inc) {
    refCounts.increment(inc);
  }

  template <PerformDeinit performDeinit>
  bool decrementStrong(uint32_t dec) {
    return refCounts.doDecrement<performDeinit>(dec);
  }

  template <PerformDeinit performDeinit>
  bool decrementNonAtomicStrong(uint32_t dec) {
    return refCounts.doDecrementNonAtomic<performDeinit>(dec);
  }

  void decrementFromOneNonAtomic() {
    decrementNonAtomicStrong<DontPerformDeinit>(1);
  }
  
  bool isDeiniting() const {
    return refCounts.isDeiniting();
  }

  bool tryIncrement() {
    return refCounts.tryIncrement();
  }

  bool tryIncrementNonAtomic() {
    return refCounts.tryIncrementNonAtomic();
  }

  // Return weak reference count.
  // Note that this is not equal to the number of outstanding weak pointers.
  uint32_t getCount() const {
    return refCounts.getCount();
  }

  bool isUniquelyReferenced() const {
    return refCounts.isUniquelyReferenced();
  }

  // UNOWNED

  void incrementUnowned(uint32_t inc) {
    return refCounts.incrementUnowned(inc);
  }

  void incrementUnownedNonAtomic(uint32_t inc) {
    return refCounts.incrementUnownedNonAtomic(inc);
  }

  bool decrementUnownedShouldFree(uint32_t dec) {
    bool shouldFree = refCounts.decrementUnownedShouldFree(dec);
    if (shouldFree) {
      // DEINITED -> FREED
      // Caller will free the object.
      decrementWeak();
    }

    return shouldFree;
  }

  bool decrementUnownedShouldFreeNonAtomic(uint32_t dec) {
    bool shouldFree = refCounts.decrementUnownedShouldFreeNonAtomic(dec);
    if (shouldFree) {
      // DEINITED -> FREED
      // Caller will free the object.
      decrementWeakNonAtomic();
    }

    return shouldFree;
  }

  uint32_t getUnownedCount() const {
    return refCounts.getUnownedCount();
  }

  
  // WEAK

  SWIFT_NODISCARD
  HeapObjectSideTableEntry* incrementWeak() {
    // incrementWeak need not be atomic w.r.t. concurrent deinit initiation.
    // The client can't actually get a reference to the object without
    // going through tryRetain(). tryRetain is the one that needs to be
    // atomic w.r.t. concurrent deinit initiation.
    // The check here is merely an optimization.
    if (refCounts.isDeiniting())
      return nullptr;
    refCounts.incrementWeak();
    return this;
  }

  void decrementWeak() {
    // FIXME: assertions
    // FIXME: optimize barriers
    bool cleanup = refCounts.decrementWeakShouldCleanUp();
    if (!cleanup)
      return;

    // Weak ref count is now zero. Delete the side table entry.
    // FREED -> DEAD
    assert(refCounts.getUnownedCount() == 0);
    swift_cxx_deleteObject(this);
  }

  void decrementWeakNonAtomic() {
    // FIXME: assertions
    // FIXME: optimize barriers
    bool cleanup = refCounts.decrementWeakShouldCleanUpNonAtomic();
    if (!cleanup)
      return;

    // Weak ref count is now zero. Delete the side table entry.
    // FREED -> DEAD
    assert(refCounts.getUnownedCount() == 0);
    swift_cxx_deleteObject(this);
  }

  uint32_t getWeakCount() const {
    return refCounts.getWeakCount();
  }
  
  void *getSideTable() {
    return refCounts.getSideTable();
  }
  
#ifndef NDEBUG
  bool isImmutableCOWBuffer() const {
    return immutableCOWBuffer;
  }
  
  void setIsImmutableCOWBuffer(bool immutable) {
    immutableCOWBuffer = immutable;
  }
#endif

  void dumpRefCounts() {
    refCounts.dump();
  }
};


// Inline version of non-atomic strong decrement.
// This version can actually be non-atomic.
template <>
template <PerformDeinit performDeinit>
SWIFT_ALWAYS_INLINE inline bool
RefCounts<InlineRefCountBits>::doDecrementNonAtomic(uint32_t dec) {

  // We can get away without atomicity here.
  // The caller claims that there are no other threads with strong references 
  // to this object.
  // We can non-atomically check that there are no outstanding unowned or
  // weak references, and if nobody else has a strong reference then
  // nobody else can form a new unowned or weak reference.
  // Therefore there is no other thread that can be concurrently
  // manipulating this object's retain counts.

  auto oldbits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);

  // Use slow path if we can't guarantee atomicity.
  if (oldbits.hasSideTable() || oldbits.getUnownedRefCount() != 1)
    return doDecrementNonAtomicSlow<performDeinit>(oldbits, dec);
    
  if (oldbits.isImmortal(true)) {
    return false;
  }

  auto newbits = oldbits;
  bool fast = newbits.decrementStrongExtraRefCount(dec);
  if (!fast) {
    return doDecrementNonAtomicSlow<performDeinit>(oldbits, dec);
  }

  refCounts.store(newbits, std::memory_order_relaxed);
  return false;  // don't deinit
}

// Out-of-line version of non-atomic strong decrement.
// This version needs to be atomic because of the 
// threat of concurrent read of a weak reference.
template <>
template <PerformDeinit performDeinit>
inline bool RefCounts<SideTableRefCountBits>::
doDecrementNonAtomic(uint32_t dec) {
  return doDecrement<performDeinit>(dec);
}


template <>
template <PerformDeinit performDeinit>
inline bool RefCounts<InlineRefCountBits>::
doDecrementSideTable(InlineRefCountBits oldbits, uint32_t dec) {
  auto side = oldbits.getSideTable();
  return side->decrementStrong<performDeinit>(dec);
}

template <>
template <PerformDeinit performDeinit>
inline bool RefCounts<InlineRefCountBits>::
doDecrementNonAtomicSideTable(InlineRefCountBits oldbits, uint32_t dec) {
  auto side = oldbits.getSideTable();
  return side->decrementNonAtomicStrong<performDeinit>(dec);
}

template <>
template <PerformDeinit performDeinit>
inline bool RefCounts<SideTableRefCountBits>::
doDecrementSideTable(SideTableRefCountBits oldbits, uint32_t dec) {
  swift::crash("side table refcount must not have "
               "a side table entry of its own");
}

template <>
template <PerformDeinit performDeinit>
inline bool RefCounts<SideTableRefCountBits>::
doDecrementNonAtomicSideTable(SideTableRefCountBits oldbits, uint32_t dec) {
  swift::crash("side table refcount must not have "
               "a side table entry of its own");
}

template <>
inline uint32_t RefCounts<InlineRefCountBits>::getWeakCount() const {
  auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
  if (bits.hasSideTable()) {
    return bits.getSideTable()->getWeakCount();
  } else {
    // No weak refcount storage. Return only the weak increment held
    // on behalf of the unowned count.
    return bits.getUnownedRefCount() ? 1 : 0;
  }
}

template <>
inline uint32_t RefCounts<SideTableRefCountBits>::getWeakCount() const {
  auto bits = refCounts.load(SWIFT_MEMORY_ORDER_CONSUME);
  return bits.getWeakRefCount();
}

template <> inline
HeapObject* RefCounts<InlineRefCountBits>::getHeapObject() {
  auto offset = sizeof(void *);
  auto prefix = ((char *)this - offset);
  return (HeapObject *)prefix;
}

template <> inline
HeapObject* RefCounts<SideTableRefCountBits>::getHeapObject() {
  auto offset = HeapObjectSideTableEntry::refCountsOffset();
  auto prefix = ((char *)this - offset);
  return *(HeapObject **)prefix;
}


// namespace swift
}

// for use by SWIFT_HEAPOBJECT_NON_OBJC_MEMBERS
typedef swift::InlineRefCounts InlineRefCounts;

// These assertions apply to both the C and the C++ declarations.
static_assert(sizeof(InlineRefCounts) == sizeof(InlineRefCountsPlaceholder),
              "InlineRefCounts and InlineRefCountsPlaceholder must match");
static_assert(sizeof(InlineRefCounts) == sizeof(__swift_uintptr_t),
              "InlineRefCounts must be pointer-sized");
static_assert(__alignof(InlineRefCounts) == __alignof(__swift_uintptr_t),
              "InlineRefCounts must be pointer-aligned");

#endif // !defined(__swift__)

#endif
