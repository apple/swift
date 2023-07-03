//===--- MemAccessUtils.h - Utilities for SIL memory access. ----*- C++ -*-===//
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
///
/// These utilities model the storage locations of memory access. See
/// SILMemoryAccess.md for high-level design.
///
/// Terminology: In the examples below, 'address' is the address of the memory
/// operation, 'access' is the address of the formal access scope, 'base' is the
/// address of the formally accessed memory, and 'root' is the reference root of
/// the object that the formally accessed memory is a member of.
///
///  struct S {
///    var field: Int64
///   }
///   class C {
///     var prop: S
///   }
///
///   %root    = alloc_ref $C
///   %base    = ref_element_addr %root : $C, #C.prop
///   %access  = begin_access [read] [static] %base : $*S
///   %address = struct_element_addr %access : $*S, #.field
///   %value   = load [trivial] %address : $*Int64
///   end_access %access : $*S
/// 
/// OR
/// 
///   %root    = alloc_box $S
///   %base    = project_box %root : ${ var S }
///   %access  = begin_access [read] [static] %base : $*S
///   %address = struct_element_addr %access : $*S, #.field
///   %value   = load [trivial] %address : $*Int64
///   end_access %access : $*S
///
/// All memory operations that are part of a formal access, as defined by
/// exclusivity rules, are marked by begin_access and end_access instructions.
///
///     Currently, access markers are stripped early in the pipeline. An active
///     goal is to require access markers in OSSA form, and to enable access
///     marker verification.
///
/// To verify access markers, SIL checks that all memory operations either have
/// an address that originates in begin_access, or originates from a pattern
/// that is recognized as a non-formal-access. This implies that every SIL
/// memory operation has a recognizable address source. Given the address of a
/// memory operation, there are three levels of APIs that inspect the origin of
/// that address:
///
/// 1. getTypedAccessAddress(): Find the originating address as close as
/// possible to the address of the formal access *without* looking past any
/// storage casts. This is useful when the type of the returned access address
/// must be consistent with the memory operation's type (the same type or a
/// parent type). For a formal access, this typically returns the begin_access,
/// but it is not guaranteed to because some accesses contain storage casts
/// (TODO: make storage casts within access scopes illegal). For non-formal
/// access, it returns a best-effort address corresponding to the base of an
/// access.
///
/// 2. getAccessScope(): If the memory operation is part of a formal access,
/// then this is guaranteed to return the begin_access marker. Otherwise, it
/// returns the best-effort address or pointer corresponding to the base of an
/// access. Useful to find the scope of a formal access.
///
/// 3. getAccessBase(): Find the ultimate base of any address corresponding to
/// the accessed object, regardless of whether the address is nested within
/// access scopes, and regardless of any storage casts. This returns either an
/// address or pointer type, but never a reference or box type.
/// Each object's property or its tail storage is separately accessed.
///
/// In addition to the free-standing functions, the AccessBase and
/// AccessStorage classes encapsulate the identity of an access. They can be
/// used to:
/// - directly compare and hash access identities
/// - exhaustively switch over the kinds of accesses
/// - cache access lookups for repeated queries
///
/// AccessBase::compute() follows the same logic as getAccessBase(), but if the
/// base is not recognized as a valid access, it returns invalid
/// AccessBase. AccessStorage::compute() extends this API by using
/// findReferenceRoot() to more precisely identify the storage object.
///
/// AccessBase::compute() and AccessStorage::compute() can be called on the
/// address of any memory operation, the address of a begin_access, or any other
/// address value. If the address is the operand of any enforced begin_access or
/// any memory operation that corresponds to formal access, then compute()
/// must return a valid AccessBase or AccessStorage value. If the memory
/// operation is *not* part of a formal access, then it still identifies the
/// accessed storage as a best effort, but the result may be invalid storage.
///
///    An active goal is to require compute() to always return a
///    valid AccessStorage value even for operations that aren't part of a
///    formal access.
///
/// The AccessEnforcementWMO pass is an example of an optimistic optimization
/// that relies on this requirement for correctness. If
/// AccessStorage::compute() simply bailed out on an unrecognized memory
/// address by returning an invalid AccessStorage, then the optimization could
/// make incorrect assumptions about the absence of access to globals or class
/// properties.
///
/// computeInScope() returns an AccessBase or AccessStorage value for the
/// immediately enclosing access scope. Within a formal access, it always
/// returns a Nested storage kind, which provides the begin_access marker.
///
/// identifyFormalAccess() works like AccessStorage::computeInScope(), but it
/// should only be passed finds an address that is the operand of a begin_access
/// marker, rather than any arbitrary address. This must return a valid
/// AccessStorage value unless the access has "Unsafe" enforcement. The given
/// begin_access marker may be nested within another, outer access scope. For
/// the purpose of exclusivity, nested accesses are considered distinct formal
/// accesses so they return distinct AccessStorage values even though they may
/// access the same memory. This way, nested accesses do not appear to conflict.
///
/// AccessPath identifies both the accessed storage and the path to a specific
/// storage location within that storage object. See SILMemoryAccess.md and the
/// class comments below for details. AccessPath::compute() and
/// AccessPath::computeInScope() mirror the AccessStorage API.
/// AccessPath::contains() and AccessPath::mayOverlap() provide efficient
/// comparison of access paths.
///
/// AccessPath::collectUses() gathers all reachable uses of the accessed
/// storage, allowing the selection of Exact, Inner, or Overlapping uses.
/// visitAccessStorageUses() and visitAccessPathUses() generalize
/// handling of all reachable uses for a given storage location.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_MEMACCESSUTILS_H
#define SWIFT_SIL_MEMACCESSUTILS_H

#include "swift/Basic/IndexTrie.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/ADT/DenseMap.h"

//===----------------------------------------------------------------------===//
//                            MARK: Standalone API
//===----------------------------------------------------------------------===//

namespace swift {

/// Get the source address of a formal access by stripping access markers.
///
/// Postcondition: If \p v is an address, then the returned value is also an
/// address (pointer-to-address is not stripped).
inline SILValue stripAccessMarkers(SILValue v) {
  while (auto *bai = dyn_cast<BeginAccessInst>(v)) {
    v = bai->getOperand();
  }
  return v;
}

/// Return the source address after stripping as many access projections as
/// possible without losing the address type.
///
/// For formal accesses, this typically returns the begin_access, but may fail
/// for accesses that call into an addressor, which performs pointer
/// conversion.
///
/// If there is no access marker, then this returns the "best-effort" address
/// corresponding to the accessed variable. This never looks through
/// pointer_to_address or other conversions that may change the address type
/// other than via type-safe (TBAA-compatible) projection.
SILValue getTypedAccessAddress(SILValue address);

/// Return the source address or pointer after stripping all access projections
/// and storage casts.
///
/// If this is a formal access, then it is guaranteed to return the immediately
/// enclosing begin_access and may "see through" storage casts to do so.
///
/// If there is no access marker, then it returns a "best effort" address
/// corresponding to the accessed variable. In this case, the returned value
/// could be a non-address pointer type.
SILValue getAccessScope(SILValue address);

/// Return the source address or pointer after stripping access projections,
/// access markers, and storage casts.
///
/// The returned base address is guaranteed to match the unique AccessStorage
/// value for the same \p address. That is, if two calls to getAccessBase()
/// return the same base address, then they must also have the same storage.
SILValue getAccessBase(SILValue address);

/// Find the root of a reference, which may be a non-trivial type, box type, or
/// BridgeObject. This is guaranteed to be consistent with
/// AccessStorage::getRoot() and AccessPath::getRoot().
SILValue findReferenceRoot(SILValue ref);

/// Find the first owned root of the reference.
SILValue findOwnershipReferenceRoot(SILValue ref);

/// Look through all ownership forwarding instructions to find the values which
/// were originally borrowed.
///
/// Note: This treats guaranteed forwarding phis like roots even though they do
/// not introduce the borrow scope. This ensures that all roots dominate \p
/// reference Value. But the client will need to handle forwarding phis.
void findGuaranteedReferenceRoots(SILValue referenceValue,
                                  bool lookThroughNestedBorrows,
                                  SmallVectorImpl<SILValue> &roots);

/// Find the aggregate containing the first owned root of the
/// reference. Identical to findOwnershipReferenceRoot, but looks through
/// struct_extract, tuple_extract, etc.
SILValue findOwnershipReferenceAggregate(SILValue ref);

/// Return true if \p address points to a let-variable.
///
/// let-variables are only written during let-variable initialization, which is
/// assumed to store directly to the same, unaliased access base.
///
/// The address of a let-variable must be the base of a formal access, not an
/// access projection. A 'let' member of a struct is *not* a let-variable,
/// because it's memory may be written when formally modifying the outer
/// struct. A let-variable is either an entire local variable, global variable,
/// or class property (these are all formal access base addresses).
bool isLetAddress(SILValue address);

/// Return true if two accesses to the same storage may conflict given the kind
/// of each access.
inline bool accessKindMayConflict(SILAccessKind a, SILAccessKind b) {
  return !(a == SILAccessKind::Read && b == SILAccessKind::Read);
}

/// Whether \p instruction accesses storage whose representation is either (1)
/// unidentified such as by reading a pointer or (2) global.
bool mayAccessPointer(SILInstruction *instruction);

/// Whether this instruction loads or copies a value whose storage does not
/// increment the stored value's reference count.
bool mayLoadWeakOrUnowned(SILInstruction* instruction);

/// Conservatively, whether this instruction could involve a synchronization
/// point like a memory barrier, lock or syscall.
bool maySynchronizeNotConsideringSideEffects(SILInstruction* instruction);

/// Conservatively, whether this instruction could be a barrier to hoisting
/// destroys.
///
/// Does not consider function so effects, so every apply is treated as a
/// barrier.
bool mayBeDeinitBarrierNotConsideringSideEffects(SILInstruction *instruction);

} // end namespace swift

//===----------------------------------------------------------------------===//
//                            MARK: AccessStorage
//===----------------------------------------------------------------------===//

namespace swift {

/// Control def-use traversals, allowing them to remain with an access scope or
/// consider operations across scope boundaries.
enum class NestedAccessType { StopAtAccessBegin, IgnoreAccessBegin };

/// Exact uses only include uses whose AccessPath is identical to this one.
/// Inner uses have an AccessPath the same as or contained by this one.
/// Overlapping uses may contain, be contained by, or have an unknown
/// relationship with this one. An unknown relationship typically results from
/// a dynamic index_addr offset.
///
/// The enum values are ordered. Each successive use type is a superset of the
/// previous.
enum class AccessUseType { Exact, Inner, Overlapping };

/// When walking from a value to its storage, casts may be encountered.  The
/// cases describe variety of encountered casts, categorized by the kind of
/// transformation that the casts perform.
///
/// The enum values are ordered.  Each successive cast kind is more
/// transformative than the last.
///
/// TODO: Distinguish between LayoutEquivalent and LayoutCompatible.
enum class AccessStorageCast { Identity, Type };

/// The physical representation used to identify access information and common
/// API used by both AccessBase and AccessStorage.
///
/// May be one of several kinds of "identified" storage objects. Storage is
/// "identified" when the base of the formal access is recognized and the kind
/// of storage precisely identified.
///
/// Unidentified access may have a valid AccessRepresentation. This is the case
/// for a SILValue that produces the address but that value has not been
/// determined to be the *base* of a formal access. It may be from a
/// ref_tail_addr, undef, or some recognized memory initialization pattern. A
/// valid Unidentified address cannot represent any arbitrary address--it must
/// at least be proven not to correspond to any class or global variable
/// access. The Unidentified address can be nested within another access to the
/// same object such that the Unidentified value is derived from the identified
/// Class/Global access. But the Unidentified access can never be the *only*
/// formal access to Class/Global memory. This would break assumptions that all
/// Class/Global accesses are discoverable.
///
/// An *invalid* AccessStorage object is Unidentified and associated with an
/// invalid SILValue. This signals that analysis has failed to recognize an
/// expected address producer pattern.
///
///     An active goal is to enforce that every memory operation's
///     AccessStorage is either valid or explicitly guarded by an "unsafe"
///     begin_access.
///
/// Some identified storage is also known to be "uniquely identified". Memory
/// operations on "uniquely identified" storage cannot overlap with any other
/// memory operation on distinct "uniquely identified" storage.
class AccessRepresentation {
public:
  /// Enumerate over all valid begin_access bases. Clients can use a covered
  /// switch to warn if AccessStorage ever adds a case.
  enum Kind : uint8_t {
    Box,
    Stack,
    Global,
    Class,
    Tail,
    Argument, // Address or RawPointer argument
    Yield,
    Nested,
    Unidentified,
    NumKindBits = countBitsUsed(static_cast<unsigned>(Unidentified))
  };

  static const char *getKindName(Kind k);

  // Checking the storage kind is far more common than other fields. Make sure
  // it can be byte load with no shift.
  static const int ReservedKindBits = 7;
  static_assert(ReservedKindBits >= NumKindBits, "Too many storage kinds.");

  static const unsigned InvalidElementIndex =
      (1 << (32 - (ReservedKindBits + 1))) - 1;
  // Give object tail storage a fake large property index for convenience.

  static constexpr unsigned TailIndex = std::numeric_limits<int>::max();

protected:
  // Form a bitfield that is effectively a union over any pass-specific data
  // with the fields used within this class as a common prefix.
  //
  // This allows passes to embed analysis flags, and reserves enough space to
  // embed a unique index.
  //
  // AccessStorageAnalysis defines an StorageAccessInfo object that maps each
  // storage object within a function to its unique storage index and summary
  // information of that storage object.
  //
  // AccessEnforcementOpts defines an AccessEnforcementOptsInfo object that maps
  // each begin_access to its storage object, unique access index, and summary
  // info for that access.
  union {
    uint64_t opaqueBits;
    // elementIndex can overflow while gracefully degrading analysis. For now,
    // reserve an absurd number of bits at a nice alignment boundary, but this
    // can be reduced.
    SWIFT_INLINE_BITFIELD_BASE(AccessRepresentation, 32,
                               kind : ReservedKindBits,
                               isLet : 1,
                               elementIndex : 32 - (ReservedKindBits + 1));

    // Define bits for use in AccessStorageAnalysis. Each identified storage
    // object is mapped to one instance of this subclass.
    SWIFT_INLINE_BITFIELD_FULL(StorageAccessInfo, AccessRepresentation,
                               64 - NumAccessRepresentationBits,
                               accessKind : NumSILAccessKindBits,
                               noNestedConflict : 1,
                               storageIndex : 64 - (NumAccessRepresentationBits
                                                    + NumSILAccessKindBits
                                                    + 1));

    // Define bits for use in the AccessEnforcementOpts pass. Each begin_access
    // in the function is mapped to one instance of this subclass.  Reserve a
    // bit for a seenNestedConflict flag, which is the per-begin-access result
    // of pass-specific analysis. The remaining bits are sufficient to index all
    // begin_[unpaired_]access instructions.
    //
    // `AccessRepresentation` refers to the AccessRepresentationBitfield defined
    // above, setting aside enough bits for common data.
    SWIFT_INLINE_BITFIELD_FULL(AccessEnforcementOptsInfo,
                               AccessRepresentation,
                               64 - NumAccessRepresentationBits,
                               seenNestedConflict : 1,
                               seenIdenticalStorage : 1,
                               beginAccessIndex :
                                 62 - NumAccessRepresentationBits);

    // Define data flow bits for use in the AccessEnforcementDom pass. Each
    // begin_access in the function is mapped to one instance of this subclass.
    SWIFT_INLINE_BITFIELD(DomAccessStorage, AccessRepresentation, 1 + 1,
                          isInner : 1, containsRead : 1);
  } Bits;

  // 'value' or 'global' and 'isLet' are initialized in the subclass.
  union {
    // For AccessBase, 'value' always contains the base address.
    //
    // For AccessStorage:
    // - For Global, 'global' refers to the global variable
    // - For Class and Tail, 'value' contains the object root
    // - For other access kinds, 'value' contains the base address
    SILValue value;
    SILGlobalVariable *global;
  };

  void setLetAccess(bool isLet) {
    Bits.AccessRepresentation.isLet = isLet;
  }

public:
  AccessRepresentation() : value() {
    Bits.opaqueBits = 0;
    initKind(Unidentified, InvalidElementIndex);
  }

  AccessRepresentation(SILValue base, Kind kind);

  Kind getKind() const {
    return static_cast<Kind>(Bits.AccessRepresentation.kind);
  }

  void initKind(Kind k, unsigned elementIndex) {
    Bits.opaqueBits = 0;
    Bits.AccessRepresentation.kind = k;
    Bits.AccessRepresentation.elementIndex = elementIndex;
  }

  unsigned getElementIndex() const {
    return Bits.AccessRepresentation.elementIndex;
  }

  void setElementIndex(unsigned elementIndex) {
    Bits.AccessRepresentation.elementIndex = elementIndex;
  }

  // Return true if this is a valid access representation.
  operator bool() const { return getKind() != Unidentified || value; }

  // Clear any bits reserved for subclass data. Useful for up-casting back to
  // the base class.
  void resetSubclassData() {
    initKind(getKind(), Bits.AccessRepresentation.elementIndex);
  }

  SILValue getValue() const {
    assert(getKind() != Global && getKind() != Class && getKind() != Tail);
    assert(value && "Invalid storage has an invalid value");
    return value;
  }

  unsigned getParamIndex() const {
    assert(getKind() == Argument);
    return getElementIndex();
  }

  SILFunctionArgument *getArgument() const {
    assert(getKind() == Argument);
    return cast<SILFunctionArgument>(value);
  }

  /// Return true if this access is based on a reference-counted object.
  bool isReference() const {
    return getKind() == Box || getKind() == Class || getKind() == Tail;
  }

  /// Return true if the given access is guaranteed to be within an object of
  /// class type.
  bool isObjectAccess() const {
    return getKind() == Class || getKind() == Tail;
  }

  unsigned getPropertyIndex() const {
    assert(getKind() == Class);
    return getElementIndex();
  }

  /// Return true if the given storage objects have identical access information
  ///
  /// This compares only the AccessStorage base class bits, ignoring the
  /// subclass bits. It is used for hash lookup equality, so it should not
  /// perform any additional lookups or dereference memory outside itself.
  bool hasIdenticalAccessInfo(const AccessRepresentation &other) const {
    if (getKind() != other.getKind())
      return false;

    switch (getKind()) {
    case Box:
    case Stack:
    case Tail:
    case Argument:
    case Yield:
    case Nested:
    case Unidentified:
      return value == other.value;
    case Global:
      return global == other.global;
    case Class:
      return value == other.value
             && getElementIndex() == other.getElementIndex();
    }
    llvm_unreachable("covered switch");
  }
  /// Return true if the storage is guaranteed local.
  bool isLocal() const {
    switch (getKind()) {
    case Box:
      return isa<AllocBoxInst>(value);
    case Stack:
      return true;
    case Global:
    case Class:
    case Tail:
    case Argument:
    case Yield:
    case Nested:
    case Unidentified:
      return false;
    }
    llvm_unreachable("unhandled kind");
  }

  /// If this is a uniquely identified formal access, then it cannot
  /// alias with any other uniquely identified access to different storage.
  bool isUniquelyIdentified() const {
    switch (getKind()) {
    case Box:
      return isa<AllocBoxInst>(value);
    case Stack:
    case Global:
      return true;
    case Argument:
      return
        getArgument()->getArgumentConvention().isExclusiveIndirectParameter();
    case Class:
    case Tail:
    case Yield:
    case Nested:
    case Unidentified:
      return false;
    }
    llvm_unreachable("unhandled kind");
  }

  /// Return true if this storage is guaranteed not to overlap with \p other's
  /// storage.
  bool isDistinctFrom(const AccessRepresentation &other) const;

  /// Return true if this identifies the base of a formal access location.
  ///
  /// Most formal access bases are uniquely identified, but class access
  /// may alias other references to the same object.
  bool isFormalAccessBase() const {
    if (isUniquelyIdentified())
      return true;

    return getKind() == Class;
  }

  /// Return true if the given access is on a 'let' lvalue.
  bool isLetAccess() const { return Bits.AccessRepresentation.isLet; }

  void print(raw_ostream &os) const;

private:
  // Disable direct comparison because we allow subclassing with bitfields.
  // Currently, we use DenseMapInfo to unique storage, which defines key
  // equality only in terms of the base AccessStorage class bits.
  bool operator==(const AccessRepresentation &) const = delete;
  bool operator!=(const AccessRepresentation &) const = delete;
};

/// The base of a formal access.
///
/// Note that the SILValue that represents a storage object is not
/// necessarily an address type. It may instead be a SILBoxType. So, even
/// though address phis are not allowed, finding the base of an access may
/// require traversing phis.
class AccessBase : public AccessRepresentation {
public:
  /// Return an AccessBase for the formally accessed variable pointed to by \p
  /// sourceAddress.
  ///
  /// \p sourceAddress may be an address type or Builtin.RawPointer.
  ///
  /// If \p sourceAddress is within a formal access scope, which does not have
  /// "Unsafe" enforcement, then this always returns the valid base.
  ///
  /// If \p sourceAddress is not within a formal access scope, or within an
  /// "Unsafe" scope, then this finds the formal base if possible,
  /// otherwise returning an invalid base.
  static AccessBase compute(SILValue sourceAddress);

  // Do not add any members to this class. AccessBase can be copied as
  // AccessRepresentation.

public:
  AccessBase() = default;

  AccessBase(SILValue base, Kind kind);

  /// Return the base address of this access.
  ///
  /// Precondition: this is a valid AccessedBase.
  ///
  /// Postcondition: the returned value has address or RawPointer type.
  SILValue getBaseAddress() const {
    assert(value && "An invalid base value implies invalid storage");
    assert(value->getType().isAddress()
           || isa<BuiltinRawPointerType>(value->getType().getASTType()));
    return value;
  }

  /// Return the immediate reference for the box or class object being accessed.
  ///
  /// Use findReferenceRoot() or findOwnershipRoot() on this result to precisely
  /// identify the storage object.
  ///
  /// Precondition: isReference() is true.
  SILValue getReference() const;

  /// Return the OSSA root of the reference being accessed.
  ///
  /// Precondition: isReference() is true.
  SILValue getOwnershipReferenceRoot() const {
    return findOwnershipReferenceRoot(getReference());
  }

  /// Return the storage root of the reference being accessed.
  ///
  /// Precondition: isReference() is true.
  SILValue getStorageReferenceRoot() const {
    return findReferenceRoot(getReference());
  }

  /// Return the global variable being accessed. Always valid.
  ///
  /// Precondition: getKind() == Global
  SILGlobalVariable *getGlobal() const;

  /// Returns the ValueDecl for the formal access, if it can be
  /// determined. Otherwise returns null.
  const ValueDecl *getDecl() const;

  /// Return true if this base address may be derived from a reference that is
  /// only valid within a locally scoped OSSA lifetime. This is not true for
  /// scoped storage such as alloc_stack and @in argument. It can be
  /// independently assumed that addresses are only used within the scope of the
  /// storage object.
  ///
  /// Useful to determine whether addresses with the same AccessStorage are in
  /// fact substitutable without fixing OSSA lifetime.
  bool hasLocalOwnershipLifetime() const;

  void print(raw_ostream &os) const;
  void dump() const;
};

/// Represents the identity of a storage object being accessed.
///
/// Combines AccessBase with the reference root for Class and Tail access to
/// more precisely identify storage. For efficiency of the physical
/// representation, this does not preserve the base address. For convenient
/// access to both the address base and storage use AccessStorageWithBase.
///
/// Requirements:
///
///     A bitwise comparable encoding and hash key to identify each location
///     being formally accessed. Any two accesses of "uniquely identified"
///     storage must have the same key if they access the same storage and
///     distinct keys if they access distinct storage. For more efficient
///     analysis, accesses to non-uniquely identified storage should have the
///     same key if they may point to the same storage.
///
///     Complete identification of all class or global accesses. Failing to
///     identify a class or global access will introduce undefined program
///     behavior which can't be tested.
///
/// Support for integer IDs and bitsets. An AccessStorage value has enough
/// extra bits to store a unique index for each identified access in a
/// function. An AccessStorage (without an ID) can be cheaply formed
/// on-the-fly for any memory operation then used as a hash key to lookup its
/// unique integer index which is stored directly in the hashed value but not
/// used as part of the hash key.
class AccessStorage : public AccessRepresentation {
public:
  /// Return an AccessStorage value that best identifies a formally accessed
  /// variable pointed to by \p sourceAddress, looking through any nested
  /// formal accesses to find the underlying storage.
  ///
  /// \p sourceAddress may be an address type or Builtin.RawPointer.
  ///
  /// If \p sourceAddress is within a formal access scope, which does not have
  /// "Unsafe" enforcement, then this always returns valid storage.
  ///
  /// If \p sourceAddress is not within a formal access scope, or within an
  /// "Unsafe" scope, then this finds the formal storage if possible, otherwise
  /// returning invalid storage.
  static AccessStorage compute(SILValue sourceAddress);

  /// Return an AccessStorage object that identifies formal access scope that
  /// immediately encloses \p sourceAddress.
  ///
  /// \p sourceAddress may be an address type or Builtin.RawPointer.
  ///
  /// If \p sourceAddress is within a formal access scope, this always returns a
  /// valid "Nested" storage value.
  ///
  /// If \p sourceAddress is not within a formal access scope, then this finds
  /// the formal storage if possible, otherwise returning invalid storage.
  static AccessStorage computeInScope(SILValue sourceAddress);

  /// Create storage for the tail elements of \p object.
  static AccessStorage forBase(AccessBase base) {
    return AccessStorage(base.getBaseAddress(), base.getKind());
  }

  /// Create storage for the tail elements of \p object.
  static AccessStorage forObjectTail(SILValue object);

  // Do not add any members to this class. AccessBase can be copied as
  // AccessRepresentation.

public:
  AccessStorage() = default;

  AccessStorage(SILValue base, Kind kind);

  /// Return a new AccessStorage for Class/Tail/Box access based on
  /// existing storage and a new object.
  AccessStorage transformReference(SILValue object) const {
    AccessStorage storage;
    storage.initKind(getKind(), getElementIndex());
    storage.value = findReferenceRoot(object);
    return storage;
  }

  SILGlobalVariable *getGlobal() const {
    assert(getKind() == Global);
    return global;
  }

  SILValue getObject() const {
    assert(isReference());
    return value;
  }

  /// Return the address or reference root that the storage was based
  /// on. Returns an invalid SILValue for globals or invalid storage.
  SILValue getRoot() const {
    switch (getKind()) {
    case AccessStorage::Box:
    case AccessStorage::Stack:
    case AccessStorage::Nested:
    case AccessStorage::Argument:
    case AccessStorage::Yield:
    case AccessStorage::Unidentified:
      return getValue();
    case AccessStorage::Global:
      return SILValue();
    case AccessStorage::Class:
    case AccessStorage::Tail:
      return getObject();
    }
    llvm_unreachable("covered switch");
  }

  /// Visit all access roots. If any roots are visited then the original memory
  /// operation access must be reachable from one of those roots. Unidentified
  /// storage might not have any root. Identified storage always has at least
  /// one root. Identified non-global storage always has a single root. For
  /// Global storage, this visits all global_addr instructions in the function
  /// that reference the same SILGlobalVariable.
  ///
  /// \p function must be non-null for Global storage (global_addr cannot
  /// occur in a static initializer).
  void
  visitRoots(SILFunction *function,
             llvm::function_ref<bool(SILValue)> visitor) const;

  /// Return true if the given storage objects have identical storage locations.
  ///
  /// This compares only the AccessStorage base class bits, ignoring the
  /// subclass bits. It is used for hash lookup equality, so it should not
  /// perform any additional lookups or dereference memory outside itself.
  bool hasIdenticalStorage(const AccessStorage &other) const {
    return hasIdenticalAccessInfo(other);
  }

  /// Returns the ValueDecl for the underlying storage, if it can be
  /// determined. Otherwise returns null.
  const ValueDecl *getDecl() const;

  /// Get all leaf uses of all address, pointer, or box values that have a this
  /// AccessStorage in common. Return true if all uses were found before
  /// reaching the limit.
  ///
  /// The caller of 'collectUses' can determine the use type (exact, inner, or
  /// overlapping) from the resulting \p uses list by checking 'accessPath ==
  /// usePath', accessPath.contains(usePath)', and
  /// 'accessPath.mayOverlap(usePath)'. Alternatively, the client may call
  /// 'visitAccessStorageUses' with its own AccessUseVisitor subclass to
  /// sort the use types.
  bool
  collectUses(SmallVectorImpl<Operand *> &uses, AccessUseType useTy,
              SILFunction *function,
              unsigned useLimit = std::numeric_limits<unsigned>::max()) const;

  void print(raw_ostream &os) const;
  void dump() const;
};

} // end namespace swift

namespace llvm {

/// Enable using AccessStorage as a key in DenseMap.
/// Do *not* include any extra pass data in key equality.
///
/// AccessStorage hashing and comparison is used to determine when two
/// 'begin_access' instructions access the same or disjoint underlying objects.
///
/// `DenseMapInfo::isEqual()` guarantees that two AccessStorage values refer to
/// the same memory if both values are valid.
///
/// `!DenseMapInfo::isEqual()` does not guarantee that two identified
/// AccessStorage values are distinct. Inequality does, however, guarantee that
/// two *uniquely* identified AccessStorage values are distinct.
template <> struct DenseMapInfo<swift::AccessStorage> {
  static swift::AccessStorage getEmptyKey() {
    return swift::AccessStorage(swift::SILValue::getFromOpaqueValue(
                               llvm::DenseMapInfo<void *>::getEmptyKey()),
                           swift::AccessStorage::Unidentified);
  }

  static swift::AccessStorage getTombstoneKey() {
    return swift::AccessStorage(swift::SILValue::getFromOpaqueValue(
                               llvm::DenseMapInfo<void *>::getTombstoneKey()),
                           swift::AccessStorage::Unidentified);
  }

  static unsigned getHashValue(swift::AccessStorage storage) {
    switch (storage.getKind()) {
    case swift::AccessStorage::Unidentified:
      if (!storage)
        return DenseMapInfo<swift::SILValue>::getHashValue(swift::SILValue());
      LLVM_FALLTHROUGH;
    case swift::AccessStorage::Box:
    case swift::AccessStorage::Stack:
    case swift::AccessStorage::Nested:
    case swift::AccessStorage::Yield:
      return DenseMapInfo<swift::SILValue>::getHashValue(storage.getValue());
    case swift::AccessStorage::Argument:
      return storage.getParamIndex();
    case swift::AccessStorage::Global:
      return DenseMapInfo<void *>::getHashValue(storage.getGlobal());
    case swift::AccessStorage::Class:
      return llvm::hash_combine(storage.getObject(),
                                storage.getPropertyIndex());
    case swift::AccessStorage::Tail:
      return DenseMapInfo<swift::SILValue>::getHashValue(storage.getObject());
    }
    llvm_unreachable("Unhandled AccessStorageKind");
  }

  static bool isEqual(swift::AccessStorage LHS, swift::AccessStorage RHS) {
    return LHS.hasIdenticalStorage(RHS);
  }
};

} // namespace llvm

namespace swift {

/// For convenience, encapsulate and AccessStorage value along with its
/// accessed base address.
struct AccessStorageWithBase {
  /// Identical to AccessStorage::compute but preserves the access base.
  static AccessStorageWithBase compute(SILValue sourceAddress);

  /// Identical to AccessStorage::computeInScope but preserves the base.
  static AccessStorageWithBase computeInScope(SILValue sourceAddress);

  AccessStorage storage;
  // The base of the formal access. For class storage, it is the
  // ref_element_addr. For global storage it is the global_addr or initializer
  // apply. For other storage, it is the same as accessPath.getRoot().
  //
  // Base may be invalid for global_addr -> address_to_pointer -> phi patterns.
  // FIXME: add a structural requirement to SIL so base is always valid in OSSA.
  SILValue base;

  AccessStorageWithBase(AccessStorage storage, SILValue base)
      : storage(storage), base(base) {}

  AccessBase getAccessBase() const {
    return AccessBase(base, storage.getKind());
  }

  /// Returns the ValueDecl for the underlying storage, if it can be
  /// determined. Otherwise returns null. This is more complete than either
  /// AccessBase::getDecl() or AccessStorage::getDecl().
  const ValueDecl *getDecl() const;

  bool operator==(const AccessStorageWithBase &other) const {
    return storage.hasIdenticalStorage(other.storage) && base == other.base;
  }

  bool operator!=(const AccessStorageWithBase &other) const {
    return !(*this == other);
  }

  void print(raw_ostream &os) const;
  void dump() const;
};

/// Extends AccessStorageWithBase by adding information that was obtained while
/// visiting from a particular address, to which an instance of this is
/// relative.
struct RelativeAccessStorageWithBase {

  /// Identical to AccessStorageWithBase::compute but preserves information
  /// specific to the walk from address;
  static RelativeAccessStorageWithBase compute(SILValue address);

  /// Identical to AccessStorageWithBase::computeInScope but preserves
  /// information specific to the walk from address;
  static RelativeAccessStorageWithBase computeInScope(SILValue address);

  /// The address to which this RelativeAccessStorageWithBase is relative.
  SILValue address;
  /// The underlying access storage and base.
  AccessStorageWithBase storageWithBase;
  /// The most transformative cast that was seen between when walking from
  /// address to storage.base;
  llvm::Optional<AccessStorageCast> cast;

  AccessStorage getStorage() const { return storageWithBase.storage; }
};

/// Return an AccessStorage value that identifies formally accessed storage
/// for \p beginAccess, considering any outer access scope as having distinct
/// storage from this access scope. This is useful for exclusivity checking
/// to distinguish between nested access vs. conflicting access on the same
/// storage.
///
/// May return an invalid storage for either:
/// - A \p beginAccess with Unsafe enforcement
/// - Non-OSSA form in which address-type block args are allowed
inline AccessStorage identifyFormalAccess(BeginAccessInst *beginAccess) {
  return AccessStorage::computeInScope(beginAccess->getSource());
}

inline AccessStorage
identifyFormalAccess(BeginUnpairedAccessInst *beginAccess) {
  return AccessStorage::computeInScope(beginAccess->getSource());
}

} // end namespace swift

//===----------------------------------------------------------------------===//
//                              MARK: AccessPath
//===----------------------------------------------------------------------===//

namespace swift {

/// Identify an addressable location based the AccessStorage and projection
/// path.
///
/// Each unique path from a base address implies a unique memory location within
/// that object. A path prefix identifies memory that contains all paths with
/// the same prefix. The AccessPath returned by AccessPath::compute(address)
/// identifies the object seen by any memory operation that *directly* operates
/// on 'address'. The computed path is a prefix of the paths of any contained
/// subobjects.
///
/// Path indices, encoded by AccessPath::Index, may be either subobject
/// projections or offset indices. We print subobject indices as '#n' and offset
/// indices as '@n'.
///
/// Example Def->Use: (Path indices)
///   struct_element_addr #1: (#1)
///   ref_tail_addr -> struct_element_addr #2: (#2)
///   ref_tail_addr -> index_addr #1 -> struct_element_addr #2: (@1, #2)
///   pointer_to_address -> struct_element_addr #2: (#2)
///   pointer_to_address -> index_addr #1 -> struct_element_addr #2: (@1, #2)
///
/// The index of ref_element_addr is part of the storage identity and does
/// not contribute to the access path indices.
///
/// A well-formed path has at most one offset component at the beginning of the
/// path (chained index_addrs are merged into one offset). In other words,
/// taking an offset from a subobject projection is not well-formed access
/// path. However, it is possible (however undesirable) for programmers to
/// convert a subobject address into a pointer (for example, via implicit
/// conversion), then advance that pointer. Since we can't absolutely prevent
/// this, we instead consider it an invalid AccessPath. This is the only case in
/// which AccessPath::storage can differ from AccessStorage::compute().
///
/// Storing an AccessPath amortizes to constant space. To cache identification
/// of address locations, AccessPath should be used rather than the
/// ProjectionPath which requires quadratic space in the number of address
/// values and quadratic time when comparing addresses.
///
/// Type-cast operations such as address_to_pointer may appear on the access
/// path. It is illegal to use these operations to cast to a non-layout
/// compatible type. TODO: add enforcement for this rule.
class AccessPath {
public:
  /// Compute the access path at \p address. This ignores begin_access markers,
  /// returning the outermost AccessStorage.
  ///
  /// The computed access path corresponds to the subobject for a memory
  /// operation that directly operates on \p address; so, for an indexable
  /// address, this implies an operation at index zero.
  static AccessPath compute(SILValue address);

  /// Compute the access path at \p address. If \p address is within a formal
  /// access, then AccessStorage will have a nested type and base will be a
  /// begin_access marker.
  ///
  /// This is primarily useful for recovering the access scope. The original
  /// storage kind will only be discovered when \p address is part of a formal
  /// access, thus not within an access scope.
  static AccessPath computeInScope(SILValue address);

  /// Creates an AccessPass, which identifies the first tail-element of the
  /// object \p rootReference.
  static AccessPath forTailStorage(SILValue rootReference);

  // Encode a dynamic index_addr as an UnknownOffset.
  static constexpr int UnknownOffset = std::numeric_limits<int>::min() >> 1;

  struct PathNode;

  // An access path index.
  //
  // Note:
  // - IndexTrieNode::RootIndex   = INT_MIN      = 0x80000000
  // - AccessStorage::TailIndex = INT_MAX      = 0x7FFFFFFF
  // - AccessPath::UnknownOffset  = (INT_MIN>>1) = 0xC0000000
  // - An offset index is never zero
  class Index {
  public:
    friend struct PathNode;

    // Use the sign bit to identify offset indices. Subobject projections are
    // always positive.
    constexpr static unsigned IndexFlag = unsigned(1) << 31;
    static int encodeOffset(int indexValue) {
      assert(indexValue != 0 && "an offset index cannot be zero");
      // Must be able to sign-extended the 31-bit value.
      assert(((indexValue << 1) >> 1) == indexValue);
      return indexValue | IndexFlag;
    }

    // Encode a positive field index, property index, or TailIndex.
    static Index forSubObjectProjection(unsigned projIdx) {
      assert(Index(projIdx).isSubObjectProjection());
      return Index(projIdx);
    }

    static Index forOffset(unsigned projIdx) {
      return Index(encodeOffset(projIdx));
    }

  private:
    int indexEncoding;
    Index(int indexEncoding) : indexEncoding(indexEncoding) {}

  public:
    bool isSubObjectProjection() const { return indexEncoding >= 0; }

    int getSubObjectIndex() const {
      assert(isSubObjectProjection());
      return indexEncoding;
    }

    // Sign-extend the 31-bit value.
    int getOffset() const {
      assert(!isSubObjectProjection());
      return ((indexEncoding << 1) >> 1);
    }

    bool isUnknownOffset() const {
      return indexEncoding == AccessPath::UnknownOffset;
    }

    int getEncoding() const { return indexEncoding; }
    
    void print(raw_ostream &os) const;
    
    void dump() const;
  };

  // A component of the AccessPath.
  //
  // Transient wrapper around the underlying IndexTrieNode that encodes either a
  // subobject projection or an offset index.
  struct PathNode {
    IndexTrieNode *node = nullptr;

    constexpr PathNode() = default;

    PathNode(IndexTrieNode *node) : node(node) {}

    bool isValid() const { return node != nullptr; }

    bool isRoot() const { return node->isRoot(); }

    bool isLeaf() const { return node->isLeaf(); }

    Index getIndex() const { return Index(node->getIndex()); }

    PathNode getParent() const { return node->getParent(); }

    // Return the PathNode from \p subNode's path one level deeper than \p
    // prefixNode.
    //
    // Precondition: this != subNode
    PathNode findPrefix(PathNode subNode) const;

    bool isPrefixOf(PathNode other) { return node->isPrefixOf(other.node); }

    bool operator==(PathNode other) const { return node == other.node; }
    bool operator!=(PathNode other) const { return node != other.node; }
  };

private:
  AccessStorage storage;
  PathNode pathNode;
  // store the single offset index independent from the PathNode to simplify
  // checking for path overlap.
  int offset = 0;

public:
  // AccessPaths are built by AccessPath::compute(address).
  //
  // AccessStorage is only used to identify the storage location; AccessPath
  // ignores its subclass bits.
  AccessPath(AccessStorage storage, PathNode pathNode, int offset)
      : storage(storage), pathNode(pathNode), offset(offset) {
    assert(pathNode.isValid() || !storage && "Access path requires a pathNode");
  }

  AccessPath() = default;

  bool operator==(AccessPath other) const {
    return
      storage.hasIdenticalStorage(other.storage)
      && pathNode == other.pathNode
      && offset == other.offset;
  }
  bool operator!=(AccessPath other) const { return !(*this == other); }

  bool isValid() const { return pathNode.isValid(); }

  AccessStorage getStorage() const { return storage; }

  PathNode getPathNode() const { return pathNode; }

  int getOffset() const { return offset; }

  bool hasUnknownOffset() const { return offset == UnknownOffset; }

  /// Return true if this path contains \p subPath.
  ///
  /// Identical AccessPath's contain each other.
  ///
  /// Returns false if either path is invalid.
  bool contains(AccessPath subPath) const;

  /// Return true if this path may overlap with \p otherPath.
  ///
  /// Returns true if either path is invalid.
  bool mayOverlap(AccessPath otherPath) const;

  /// Return the address root that the access path was based on. Returns
  /// an invalid SILValue for globals or invalid storage.
  SILValue getRoot() const { return storage.getRoot(); }

  /// Get all leaf uses of all address, pointer, or box values that have a this
  /// AccessStorage in common. Return true if all uses were found before
  /// reaching the limit.
  ///
  /// The caller of 'collectUses' can determine the use type (exact, inner, or
  /// overlapping) from the resulting \p uses list by checking 'accessPath ==
  /// usePath', accessPath.contains(usePath)', and
  /// 'accessPath.mayOverlap(usePath)'. Alternatively, the client may call
  /// 'visitAccessPathUses' with its own AccessUseVisitor subclass to
  /// sort the use types.
  bool
  collectUses(SmallVectorImpl<Operand *> &uses, AccessUseType useTy,
              SILFunction *function,
              unsigned useLimit = std::numeric_limits<unsigned>::max()) const;

  /// Returns a new AccessPass, identical to this AccessPath, except that the
  /// offset is replaced with \p newOffset.
  AccessPath withOffset(int newOffset) const {
    return AccessPath(storage, pathNode, newOffset);
  }

  void printPath(raw_ostream &os) const;
  void print(raw_ostream &os) const;
  void dump() const;
};

// Encapsulate the result of computing an AccessPath. AccessPath does not store
// the base address of the formal access because it does not always uniquely
// identify the access, but AccessPath users may use the base address to to
// recover the def-use chain for a specific global_addr or ref_element_addr.
struct AccessPathWithBase {
  AccessPath accessPath;
  // The address-type value that is the base of the formal access. For class
  // storage, it is the ref_element_addr; for box storage, the project_box; for
  // global storage the global_addr or initializer apply. For other
  // storage, it is the same as accessPath.getRoot().
  //
  // Note: base may be invalid for phi patterns, even though the accessPath is
  // valid because we don't currently keep track of multiple bases. Multiple
  // bases for the same storage can happen with global_addr, ref_element_addr,
  // ref_tail_addr, and project_box.
  //
  // FIXME: add a structural requirement to SIL/OSSA so valid storage has
  // a single base. For most cases, it is as simple by sinking the
  // projection. For index_addr, it may require hoisting ref_tail_addr.
  SILValue base;

  /// Compute the access path at \p address, and record the access base. This
  /// ignores begin_access markers, returning the outermost AccessStorage.
  static AccessPathWithBase compute(SILValue address);

  /// Compute the access path at \p address, and record the access base. If \p
  /// address is within a formal access, then AccessStorage will have a nested
  /// type and base will be a begin_access marker.
  static AccessPathWithBase computeInScope(SILValue address);

  AccessPathWithBase(AccessPath accessPath, SILValue base)
      : accessPath(accessPath), base(base) {}

  AccessBase getAccessBase() const {
    return AccessBase(base, accessPath.getStorage().getKind());
  }

  bool isValid() const { return base && accessPath.isValid(); }

  bool operator==(AccessPathWithBase other) const {
    return accessPath == other.accessPath && base == other.base;
  }
  bool operator!=(AccessPathWithBase other) const { return !(*this == other); }

  void print(raw_ostream &os) const;
  void dump() const;
};

// Visits all the "product leaves" of the type tree of the specified value and
// invokes provided visitor, identifying the leaf by its path node and
// providing its type.
//
// The "product leaves" are the leaves obtained by only looking through type
// products (structs and tuples) and NOT type sums (enums).
void visitProductLeafAccessPathNodes(
    SILValue address, TypeExpansionContext tec, SILModule &module,
    std::function<void(AccessPath::PathNode, SILType)> visitor);

inline AccessPath AccessPath::compute(SILValue address) {
  return AccessPathWithBase::compute(address).accessPath;
}

inline AccessPath AccessPath::computeInScope(SILValue address) {
  return AccessPathWithBase::compute(address).accessPath;
}

} // end namespace swift

namespace llvm {

/// Allow AccessPath to be used in DenseMap.
template <> struct DenseMapInfo<swift::AccessPath> {
  static inline swift::AccessPath getEmptyKey() {
    return swift::AccessPath(
        DenseMapInfo<swift::AccessStorage>::getEmptyKey(),
        swift::AccessPath::PathNode(
          DenseMapInfo<swift::IndexTrieNode *>::getEmptyKey()), 0);
  }
  static inline swift::AccessPath getTombstoneKey() {
    return swift::AccessPath(
        DenseMapInfo<swift::AccessStorage>::getTombstoneKey(),
        swift::AccessPath::PathNode(
          DenseMapInfo<swift::IndexTrieNode *>::getTombstoneKey()), 0);
  }
  static inline unsigned getHashValue(const swift::AccessPath &val) {
    return llvm::hash_combine(
        DenseMapInfo<swift::AccessStorage>::getHashValue(val.getStorage()),
        val.getPathNode().node);
  }
  static bool isEqual(const swift::AccessPath &lhs,
                      const swift::AccessPath &rhs) {
    return lhs == rhs;
  }
};
template <> struct DenseMapInfo<swift::AccessPathWithBase> {
  static inline swift::AccessPathWithBase getEmptyKey() {
    return swift::AccessPathWithBase(
        DenseMapInfo<swift::AccessPath>::getEmptyKey(),
        DenseMapInfo<swift::SILValue>::getEmptyKey());
  }
  static inline swift::AccessPathWithBase getTombstoneKey() {
    return swift::AccessPathWithBase(
        DenseMapInfo<swift::AccessPath>::getTombstoneKey(),
        DenseMapInfo<swift::SILValue>::getTombstoneKey());
  }
  static inline unsigned getHashValue(const swift::AccessPathWithBase &val) {
    return llvm::hash_combine(
        DenseMapInfo<swift::AccessPath>::getHashValue(val.accessPath),
        DenseMapInfo<swift::SILValue>::getHashValue(val.base));
  }
  static bool isEqual(const swift::AccessPathWithBase &lhs,
                      const swift::AccessPathWithBase &rhs) {
    return lhs == rhs;
  }
};

// Allow AccessPath::PathNode to be used as a pointer-like template argument.
template<>
struct PointerLikeTypeTraits<swift::AccessPath::PathNode> {
  static inline void *getAsVoidPointer(swift::AccessPath::PathNode node) {
    return (void *)node.node;
  }
  static inline swift::AccessPath::PathNode getFromVoidPointer(void *pointer) {
    return swift::AccessPath::PathNode((swift::IndexTrieNode *)pointer);
  }
  enum { NumLowBitsAvailable =
         PointerLikeTypeTraits<swift::IndexTrieNode *>::NumLowBitsAvailable };
};
} // end namespace llvm

//===----------------------------------------------------------------------===//
//                        MARK: Use visitors
//===----------------------------------------------------------------------===//

namespace swift {

/// Interface to the customizable use visitor.
struct AccessUseVisitor {
  AccessUseType useTy;
  NestedAccessType nestedAccessTy;

  AccessUseVisitor(AccessUseType useTy, NestedAccessType nestedTy)
    : useTy(useTy), nestedAccessTy(nestedTy) {}

  virtual ~AccessUseVisitor() {}

  bool findInnerUses() const { return useTy >= AccessUseType::Inner; }
  bool findOverlappingUses() const {
    return useTy == AccessUseType::Overlapping;
  }

  bool visitExactUse(Operand *use) {
    return visitUse(use, AccessUseType::Exact);
  }
  bool visitInnerUse(Operand *use) {
    return findInnerUses() ? visitUse(use, AccessUseType::Inner) : true;
  }
  bool visitOverlappingUse(Operand *use) {
    return
      findOverlappingUses() ? visitUse(use, AccessUseType::Overlapping) : true;
  }

  virtual bool visitUse(Operand *use, AccessUseType useTy) = 0;
};

/// Visit all uses of \p storage.
///
/// Return true if all uses were collected. This is always true as long the \p
/// visitor's visitUse method returns true.
bool visitAccessStorageUses(AccessUseVisitor &visitor, AccessStorage storage,
                            SILFunction *function);

/// Visit the uses of \p accessPath.
///
/// If the storage kind is Global, then function must be non-null (global_addr
/// only occurs inside SILFunction).
///
/// Return true if all uses were collected. This is always true as long the \p
/// visitor's visitUse method returns true.
bool visitAccessPathUses(AccessUseVisitor &visitor, AccessPath accessPath,
                         SILFunction *function);

/// Similar to visitAccessPathUses, but the visitor is restricted to a specific
/// access base, such as a particular ref_element_addr.
bool visitAccessPathBaseUses(AccessUseVisitor &visitor,
                             AccessPathWithBase accessPathWithBase,
                             SILFunction *function);

} // end namespace swift

//===----------------------------------------------------------------------===//
//                      MARK: UniqueAddressUses
//===----------------------------------------------------------------------===//

namespace swift {

/// Analyze and classify the leaf uses of unique storage.
///
/// Storage that has a unique set of roots within this function includes
/// alloc_stack, alloc_box, exclusive argument, and global variables. All access
/// to the storage within this function is derived from these roots.
///
/// Gather the kinds of uses that are typically relevant to algorithms:
/// - accesses    (specifically, begin_access insts)
/// - loads       (including copies out of, not including inout args)
/// - stores      (including copies into and inout args)
/// - destroys    (of the entire aggregate)
/// - debugUses   (only populated when preserveDebugInfo == false)
/// - unknownUses (e.g. address_to_pointer, box escape)
struct UniqueStorageUseVisitor {
  static bool findUses(UniqueStorageUseVisitor &visitor);

  SILFunction *function;
  AccessStorage storage;

  UniqueStorageUseVisitor(AccessStorage storage, SILFunction *function)
      : function(function), storage(storage) {}

  virtual ~UniqueStorageUseVisitor() = default;

  virtual bool visitBeginAccess(Operand *use) = 0;
  virtual bool visitLoad(Operand *use) = 0;
  virtual bool visitStore(Operand *use) = 0;
  virtual bool visitDestroy(Operand *use) = 0;
  virtual bool visitDealloc(Operand *use) = 0;
  virtual bool visitDebugUse(Operand *use) = 0;
  virtual bool visitUnknownUse(Operand *use) = 0;
};

} // namespace swift

//===----------------------------------------------------------------------===//
//             MARK: Helper API for specific formal access patterns
//===----------------------------------------------------------------------===//

namespace swift {

/// Return true if the given address operand is used by a memory operation that
/// initializes the memory at that address, implying that the previous value is
/// uninitialized.
bool memInstMustInitialize(Operand *memOper);

/// Is this an alloc_stack instruction that we can prove is:
///
/// 1. Only initialized once in its own def block.
/// 2. Never written to again except by destroy_addr.
///
/// Then we return the single initializing use and if \p destroyingUsers was
/// non-null, On return, if non-null, \p destroyingUsers contains the list of
/// users that destroy the alloc_stack. If the alloc_stack is destroyed in
/// pieces, we do not guarantee that the list of destroying users is a minimal
/// jointly post-dominating set.
Operand *getSingleInitAllocStackUse(
    AllocStackInst *asi, SmallVectorImpl<Operand *> *destroyingUses = nullptr);

/// Same as getSingleInitAllocStack except we throw away the single use and just
/// provide a bool.
inline bool isSingleInitAllocStack(AllocStackInst *asi,
                                   SmallVectorImpl<Operand *> &destroyingUses) {
  return getSingleInitAllocStackUse(asi, &destroyingUses);
}

/// Return true if the given address value is produced by a special address
/// producer that is only used for local initialization, not formal access.
bool isAddressForLocalInitOnly(SILValue sourceAddr);

/// Return true if the given apply invokes a global addressor defined in another
/// module.
bool isExternalGlobalAddressor(ApplyInst *AI);

/// Return true if the given StructExtractInst extracts the RawPointer from
/// Unsafe[Mutable]Pointer.
bool isUnsafePointerExtraction(StructExtractInst *SEI);

/// Given a block argument address base, check if it is actually a box projected
/// from a switch_enum. This is a valid pattern at any SIL stage resulting in a
/// block-type phi. In later SIL stages, the optimizer may form address-type
/// phis, causing this assert if called on those cases.
void checkSwitchEnumBlockArg(SILPhiArgument *arg);

/// Return true if the given address producer may be the source of a formal
/// access (a read or write of a potentially aliased, user visible variable).
///
/// `storage` must be a valid, non-nested AccessStorage object.
///
/// If this returns false, then the address can be safely accessed without
/// a begin_access marker. To determine whether to emit begin_access:
///   storage = identifyFormalAccess(address)
///   needsAccessMarker = storage && isPossibleFormalAccessBase(storage)
///
/// Warning: This is only valid for SIL with well-formed accesses. For example,
/// it will not handle address-type phis. Optimization passes after
/// DiagnoseStaticExclusivity may violate these assumptions.
///
/// This is not a member of AccessStorage because it only makes sense to use
/// in SILGen before access markers are emitted, or when verifying access
/// markers.
bool isPossibleFormalAccessStorage(const AccessStorage &storage,
                                   SILFunction *F);

/// Perform a RAUW operation on begin_access with it's own source operand.
/// Then erase the begin_access and all associated end_access instructions.
/// Return an iterator to the following instruction.
///
/// The caller should use this iterator rather than assuming that the
/// instruction following this begin_access was not also erased.
SILBasicBlock::iterator removeBeginAccess(BeginAccessInst *beginAccess);

} // end namespace swift

//===----------------------------------------------------------------------===//
//                        MARK: AccessUseDefChainVisitor
//===----------------------------------------------------------------------===//

namespace swift {

/// Return true if \p svi is a cast that preserves the identity equivalence of
/// the reference at operand zero.
bool isIdentityPreservingRefCast(SingleValueInstruction *svi);

/// Return true if \p svi is a cast that preserves the identity equivalence and
/// reference-counting equivalence of the reference at operand zero.
bool isIdentityAndOwnershipPreservingRefCast(SingleValueInstruction *svi);

/// If \p svi is an access projection, return an address-type operand for the
/// incoming address.
///
/// An access projection is on the inside of a formal access. It includes
/// struct_element_addr and tuple_element_addr, but not ref_element_addr.
///
/// The returned address may point to any compatible type, which may alias with
/// the projected address. Arbitrary address casts are not allowed.
inline Operand *getAccessProjectionOperand(SingleValueInstruction *svi) {
  switch (svi->getKind()) {
  default:
    return nullptr;

  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::IndexAddrInst:
  case SILInstructionKind::TailAddrInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  // open_existential_addr and unchecked_take_enum_data_addr are problematic
  // because they both modify memory and are access projections. Ideally, they
  // would not be casts, but will likely be eliminated with opaque values.
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
    return &svi->getAllOperands()[0];

  // Special-case this indirect enum pattern:
  //   unchecked_take_enum_data_addr -> load -> project_box
  // (the individual load and project_box are not access projections)
  //
  // FIXME: Make sure this case goes away with OSSA and opaque values.  If not,
  // then create a special instruction for this pattern. That way we have an
  // invariant that all access projections are single-value address-to-address
  // conversions. Then reuse this helper for both use-def an def-use traversals.
  //
  // Check getAccessProjectionOperand() before isAccessStorageCast() because
  // it will consider any project_box to be a storage cast.
  case SILInstructionKind::ProjectBoxInst:
    if (auto *load = dyn_cast<LoadInst>(svi->getOperand(0)))
      return &load->getOperandRef();

    return nullptr;
  };
}

/// A cast for the purposes of AccessStorage which may change the
/// underlying type but doesn't affect the AccessPath.  See isAccessStorageCast.
inline bool isAccessStorageTypeCast(SingleValueInstruction *svi) {
  switch (svi->getKind()) {
  default:
    return false;
  // Simply pass-thru the incoming address.  But change its type!
  case SILInstructionKind::MoveOnlyWrapperToCopyableAddrInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperAddrInst:
  // Simply pass-thru the incoming address.  But change its type!
  case SILInstructionKind::UncheckedAddrCastInst:
  // Casting to RawPointer does not affect the AccessPath. When converting
  // between address types, they must be layout compatible (with truncation).
  case SILInstructionKind::AddressToPointerInst:
  // Access to a Builtin.RawPointer. It may be important to continue looking
  // through this because some RawPointers originate from identified
  // locations. See the special case for global addressors, which return
  // RawPointer, above.
  //
  // If the inductive search does not find a valid addressor, it will
  // eventually reach the default case that returns in invalid location. This
  // is correct for RawPointer because, although accessing a RawPointer is
  // legal SIL, there is no way to guarantee that it doesn't access class or
  // global storage, so returning a valid unidentified storage object would be
  // incorrect. It is the caller's responsibility to know that formal access
  // to such a location can be safely ignored.
  //
  // For example:
  //
  // - KeyPath Builtins access RawPointer. However, the caller can check
  // that the access `isFromBuilin` and ignore the storage.
  //
  // - lldb generates RawPointer access for debugger variables, but SILGen
  // marks debug VarDecl access as 'Unsafe' and SIL passes don't need the
  // AccessStorage for 'Unsafe' access.
  case SILInstructionKind::PointerToAddressInst:
    return true;
  }
}

/// A cast for the purposes of AccessStorage which doesn't change the
/// underlying type and doesn't affect the AccessPath.  See isAccessStorageCast.
inline bool isAccessStorageIdentityCast(SingleValueInstruction *svi) {
  switch (svi->getKind()) {
  default:
    return false;

  // Simply pass-thru the incoming address.
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::MarkMustCheckInst:
  case SILInstructionKind::DropDeinitInst:
  case SILInstructionKind::MarkUnresolvedReferenceBindingInst:
  case SILInstructionKind::MarkDependenceInst:
  case SILInstructionKind::CopyValueInst:
    return true;
  }
}

/// An address, pointer, or box cast that occurs outside of the formal
/// access. These convert the base of accessed storage without affecting the
/// AccessPath. Useful for both use-def and def-use traversal. The source
/// address must be at operand(0).
///
/// Some of these casts, such as address_to_pointer, may also occur inside of a
/// formal access.
///
/// TODO: Add stricter structural guarantee such that these never
/// occur within an access. It's important to be able to get the accessed
/// address without looking though type casts or pointer_to_address [strict],
/// which we can't do if those operations are behind access projections.
inline bool isAccessStorageCast(SingleValueInstruction *svi) {
  return isAccessStorageTypeCast(svi) || isAccessStorageIdentityCast(svi);
}

/// Abstract CRTP class for a visiting instructions that are part of the use-def
/// chain from an accessed address up to the storage base.
///
/// Given the address of a memory operation begin visiting at
/// getAccessedAddress(address).
template <typename Impl, typename Result = void>
class AccessUseDefChainVisitor {
protected:
  Impl &asImpl() { return static_cast<Impl &>(*this); }

public:
  Result visitClassAccess(RefElementAddrInst *field) {
    return asImpl().visitBase(field, AccessStorage::Class);
  }
  Result visitTailAccess(RefTailAddrInst *tail) {
    return asImpl().visitBase(tail, AccessStorage::Tail);
  }
  Result visitArgumentAccess(SILFunctionArgument *arg) {
    return asImpl().visitBase(arg, AccessStorage::Argument);
  }
  Result visitBoxAccess(ProjectBoxInst *box) {
    return asImpl().visitBase(box, AccessStorage::Box);
  }
  /// \p global may be either a GlobalAddrInst or the ApplyInst for a global
  /// accessor function.
  Result visitGlobalAccess(SILValue global) {
    return asImpl().visitBase(global, AccessStorage::Global);
  }
  Result visitYieldAccess(MultipleValueInstructionResult *yield) {
    return asImpl().visitBase(yield, AccessStorage::Yield);
  }
  Result visitStackAccess(AllocStackInst *stack) {
    return asImpl().visitBase(stack, AccessStorage::Stack);
  }
  Result visitNestedAccess(BeginAccessInst *access) {
    return asImpl().visitBase(access, AccessStorage::Nested);
  }
  Result visitUnidentified(SILValue base) {
    return asImpl().visitBase(base, AccessStorage::Unidentified);
  }

  // Subclasses must provide implementations for:
  //
  // Result visitBase(SILValue base, AccessStorage::Kind kind);
  // Result visitNonAccess(SILValue base);
  // Result visitPhi(SILPhiArgument *phi);
  // Result visitStorageCast(SingleValueInstruction *cast, Operand *sourceOper,
  // AccessStorageCast cast); Result
  // visitAccessProjection(SingleValueInstruction *projectedAddr,
  //                              Operand *sourceOper);

  Result visit(SILValue sourceAddr);
};

template<typename Impl, typename Result>
Result AccessUseDefChainVisitor<Impl, Result>::visit(SILValue sourceAddr) {
  if (auto *svi = dyn_cast<SingleValueInstruction>(sourceAddr)) {
    if (auto *projOper = getAccessProjectionOperand(svi))
      return asImpl().visitAccessProjection(svi, projOper);

    if (isAccessStorageTypeCast(svi))
      return asImpl().visitStorageCast(svi, &svi->getAllOperands()[0],
                                       AccessStorageCast::Type);
    if (isAccessStorageIdentityCast(svi))
      return asImpl().visitStorageCast(svi, &svi->getAllOperands()[0],
                                       AccessStorageCast::Identity);
    if (auto *sbi = dyn_cast<StoreBorrowInst>(svi))
      return asImpl().visitStorageCast(
          svi, &sbi->getAllOperands()[CopyLikeInstruction::Dest],
          AccessStorageCast::Identity);
  }
  switch (sourceAddr->getKind()) {
  default:
    break;

  // MARK: Handle immediately-identifiable instructions.

  case ValueKind::ProjectBoxInst:
    return asImpl().visitBoxAccess(cast<ProjectBoxInst>(sourceAddr));

  // An AllocStack is a fully identified memory location, which may occur
  // after inlining code already subjected to stack promotion.
  case ValueKind::AllocStackInst:
    return asImpl().visitStackAccess(cast<AllocStackInst>(sourceAddr));

  case ValueKind::GlobalAddrInst:
    return asImpl().visitGlobalAccess(sourceAddr);

  case ValueKind::ApplyInst: {
    FullApplySite apply(cast<ApplyInst>(sourceAddr));
    if (auto *funcRef = apply.getReferencedFunctionOrNull()) {
      if (getVariableOfGlobalInit(funcRef)) {
        return asImpl().visitGlobalAccess(sourceAddr);
      }
    }
    if (isExternalGlobalAddressor(cast<ApplyInst>(sourceAddr)))
      return asImpl().visitUnidentified(sourceAddr);

    // Don't currently allow any other calls to return an accessed address.
    return asImpl().visitNonAccess(sourceAddr);
  }
  case ValueKind::RefElementAddrInst:
    return asImpl().visitClassAccess(cast<RefElementAddrInst>(sourceAddr));

  // ref_tail_addr project an address from a reference.
  // This is a valid address producer for nested @inout argument
  // access, but it is never used for formal access of identified objects.
  case ValueKind::RefTailAddrInst:
    return asImpl().visitTailAccess(cast<RefTailAddrInst>(sourceAddr));

  // A yield is effectively a nested access, enforced independently in
  // the caller and callee.
  case ValueKind::MultipleValueInstructionResult:
    if (auto *baResult = isaResultOf<BeginApplyInst>(sourceAddr))
      return asImpl().visitYieldAccess(baResult);
    break;

  // A function argument is effectively a nested access, enforced
  // independently in the caller and callee.
  case ValueKind::SILFunctionArgument:
    return asImpl().visitArgumentAccess(
      cast<SILFunctionArgument>(sourceAddr));

  // View the outer begin_access as a separate location because nested
  // accesses do not conflict with each other.
  case ValueKind::BeginAccessInst:
    return asImpl().visitNestedAccess(cast<BeginAccessInst>(sourceAddr));

  // Static index_addr is handled by getAccessProjectionOperand. Dynamic
  // index_addr is currently unidentified because we can't form an AccessPath
  // including them.
  case ValueKind::SILUndef:
    return asImpl().visitUnidentified(sourceAddr);

  // MARK: The sourceAddr producer cannot immediately be classified,
  // follow the use-def chain.

  case ValueKind::StructExtractInst:
    // Handle nested access to a KeyPath projection. The projection itself
    // uses a Builtin. However, the returned UnsafeMutablePointer may be
    // converted to an address and accessed via an inout argument.
    if (isUnsafePointerExtraction(cast<StructExtractInst>(sourceAddr)))
      return asImpl().visitUnidentified(sourceAddr);
    return asImpl().visitNonAccess(sourceAddr);

  case ValueKind::SILPhiArgument: {
    auto *phiArg = cast<SILPhiArgument>(sourceAddr);
    if (phiArg->isPhi()) {
      return asImpl().visitPhi(phiArg);
    }

    // A non-phi block argument may be a box value projected out of
    // switch_enum. Address-type block arguments are not allowed.
    if (sourceAddr->getType().isAddress())
      return asImpl().visitNonAccess(sourceAddr);

    checkSwitchEnumBlockArg(cast<SILPhiArgument>(sourceAddr));
    return asImpl().visitUnidentified(sourceAddr);
  }
  } // end switch
  if (isAddressForLocalInitOnly(sourceAddr))
    return asImpl().visitUnidentified(sourceAddr);

  return asImpl().visitNonAccess(sourceAddr);
}

} // end namespace swift

//===----------------------------------------------------------------------===//
//                          AccessUseDefChainCloner
//===----------------------------------------------------------------------===//

namespace swift {

/// Clone all projections and casts on the access use-def chain until the
/// checkBase predicate returns a valid base.
///
/// See comments on the cloneUseDefChain() API.
template <typename CheckBase>
class AccessUseDefChainCloner
    : public AccessUseDefChainVisitor<AccessUseDefChainCloner<CheckBase>,
                                      SILValue> {
  CheckBase checkBase;
  SILInstruction *insertionPoint;
  SILValue placeHolder = SILValue();

public:
  AccessUseDefChainCloner(CheckBase checkBase, SILInstruction *insertionPoint)
      : checkBase(checkBase), insertionPoint(insertionPoint) {}

  // Main entry point
  SILValue cloneUseDefChain(SILValue addr) {
    placeHolder = SILValue();
    return cloneRecursive(addr);
  }

  // Secondary entry point to check that cloning will succeed.
  bool canCloneUseDefChain(SILValue addr) {
    // Use any valid address as a placeholder. It is inaccessible.
    placeHolder = addr;
    return cloneRecursive(addr);
  }

  SILValue cloneRecursive(SILValue addr) {
    if (SILValue base = checkBase(addr))
      return base;

    return this->visit(addr);
  }

  // Recursively clone an address on the use-def chain.
  //
  // Helper for cloneUseDefChain.
  SILValue cloneProjection(SingleValueInstruction *projectedAddr,
                           Operand *sourceOper) {
    SILValue projectedSource = cloneRecursive(sourceOper->get());
    if (!projectedSource)
      return nullptr;

    if (placeHolder)
      return placeHolder;

    SILInstruction *clone = projectedAddr->clone(insertionPoint);
    clone->setOperand(sourceOper->getOperandNumber(), projectedSource);
    return cast<SingleValueInstruction>(clone);
  }

  // MARK: Visitor implementation

  SILValue visitBase(SILValue base, AccessStorage::Kind kind) {
    assert(false && "access base cannot be cloned");
    return SILValue();
  }

  SILValue visitNonAccess(SILValue base) {
    assert(false && "unknown address root cannot be cloned");
    return SILValue();
  }

  SILValue visitPhi(SILPhiArgument *phi) {
    assert(false && "unexpected phi on access path");
    return SILValue();
  }

  SILValue visitStorageCast(SingleValueInstruction *cast, Operand *sourceOper,
                            AccessStorageCast) {
    // The cloner does not currently know how to create compensating
    // end_borrows or fix mark_dependence operands.
    if (isa<BeginBorrowInst>(cast) || isa<MarkDependenceInst>(cast))
      return SILValue();

    return cloneProjection(cast, sourceOper);
  }

  SILValue visitNestedAccess(BeginAccessInst *access) {
    // The cloner does not currently know how to handle begin_access
    return SILValue();
  }

  SILValue visitAccessProjection(SingleValueInstruction *projectedAddr,
                                 Operand *sourceOper) {
    return cloneProjection(projectedAddr, sourceOper);
  }
};

/// Clone all projections and casts on the access use-def chain until the
/// checkBase predicate returns a valid base.
///
/// This will not clone ref_element_addr or ref_tail_addr because those aren't
/// part of the access chain.
///
/// CheckBase is a unary predicate that takes the next source address and either
/// returns a valid SILValue to use as the base of the cloned access path, or an
/// invalid SILValue to continue cloning.
///
/// CheckBase must return a valid SILValue either before attempting to clone the
/// access base. The most basic valid predicate is:
///
///    auto checkBase = [&](SILValue srcAddr) {
///      return (srcAddr == accessBase) ? srcAddr : SILValue();
///    }
template <typename CheckBase>
SILValue cloneUseDefChain(SILValue addr, SILInstruction *insertionPoint,
                          CheckBase checkBase) {
  return AccessUseDefChainCloner<CheckBase>(checkBase, insertionPoint)
      .cloneUseDefChain(addr);
}

/// Analog to cloneUseDefChain to check validity. begin_borrow and
/// mark_dependence currently cannot be cloned.
template <typename CheckBase>
bool canCloneUseDefChain(SILValue addr, CheckBase checkBase) {
  return AccessUseDefChainCloner<CheckBase>(checkBase, nullptr)
      .canCloneUseDefChain(addr);
}

} // end namespace swift

//===----------------------------------------------------------------------===//
//                              MARK: Verification
//===----------------------------------------------------------------------===//

namespace swift {

/// Visit each address accessed by the given memory operation.
///
/// This only visits instructions that modify memory in some user-visible way,
/// which could be considered part of a formal access.
void visitAccessedAddress(SILInstruction *I,
                          llvm::function_ref<void(Operand *)> visitor);

} // end namespace swift

#endif
