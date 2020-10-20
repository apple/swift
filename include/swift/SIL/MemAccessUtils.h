//===--- MemAccessUtils.h - Utilities for SIL memory access. ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// These utilities model the storage locations of memory access. See
/// ProgrammersGuide.md for high-level design.
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
/// but it is not guaranteed to because some accesses contain storage casts. For
/// non-formal access, it returns a best-effort address corresponding to the
/// base of an access.
///
/// 2. getAccessScope(): If the memory operation is part of a formal access,
/// then this is guaranteed to return the begin_access marker. Otherwise, it
/// returns the best-effort address or pointer corresponding to the base of an
/// access. Useful to find the scope of a formal access.
///
/// 3. getAccessBase(): Find the ultimate base of any address corresponding to
/// the accessed object, regardless of whether the address is nested within
/// access scopes, and regardless of any storage casts. This returns either an
/// address type, pointer type, or box type, but never a reference type.
/// Each object's property or its tail storage is separately accessed.
///
/// For better identification an access base, use
/// AccessedStorage::compute(). It returns an AccessedStorage value
/// that identifies the storage location of a memory access. It provides APIs
/// for inspecting type of accessed storage and allows for disambiguation
/// between different types of storage and different properties within a class.
///
/// AccessedStorage::compute() follows the same logic as getAccessBase(), but if
/// the base is not recognized as a valid access, it returns invalid
/// AccessedStorage. It also performs further analysis to determine the root
/// reference of an object access.
///
/// AccessedStorage::compute() returns the outermost AccessedStorage for any
/// memory address. It can be called on the address of a memory operation, the
/// address of a begin_access, or any other address value. If the address is
/// from an enforced begin_access or from any memory operation that is part of a
/// formal access, then it returns a valid AccessedStorage value. If the memory
/// operation is not part of a formal access, then it still identifies the
/// accessed location as a best effort, but the result may be invalid storage.
///
///    An active goal is to require compute() to always return a
///    valid AccessedStorage value even for operations that aren't part of a
///    formal access.
///
/// The AccessEnforcementWMO pass is an example of an optimistic optimization
/// that relies on this requirement for correctness. If
/// AccessedStorage::compute() simply bailed out on an unrecognized memory
/// address by returning an invalid AccessedStorage, then the optimization could
/// make incorrect assumptions about the absence of access to globals or class
/// properties.
///
/// identifyFormalAccess() is similar to AccessedStorage::compute(), but returns
/// the formally accessed storage of a begin_access instruction. This must
/// return a valid AccessedStorage value unless the access has "Unsafe"
/// enforcement. The formal access location may be nested within an outer
/// begin_access. For the purpose of exclusivity, nested accesses are considered
/// distinct formal accesses so they return distinct AccessedStorage values even
/// though they may access the same memory.
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
/// The returned base address is guaranteed to match the unique AccessedStorage
/// value for the same \p address. That is, if two calls to getAccessBase()
/// return the same base address, then they must also have the same storage.
SILValue getAccessBase(SILValue address);

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

} // end namespace swift

//===----------------------------------------------------------------------===//
//                            MARK: AccessedStorage
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

/// Represents the identity of a storage object being accessed.
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
/// Memory operations on "uniquely identified" storage cannot overlap with any
/// other memory operation on distinct "uniquely identified" storage.
///
/// AccessedStorage may be one of several kinds of "identified" storage
/// objects. Storage is "identified" when the base of the formal access is
/// recognized and the kind of storage precisely identified. The base is usually
/// represented by the SILValue that the memory address is derived from. For
/// global variable access, the base is the global's declaration instead.
///
/// Unidentified *valid* storage is also associated with a SILValue that
/// produces the accessed address but that value has not been determined to be
/// the base of a formal access. It may be from a ref_tail_addr, undef, or some
/// recognized memory initialization pattern. Unidentified valid storage cannot
/// represent any arbitrary base address--it must at least been proven not to
/// correspond to any class or global variable access, unless it's nested within
/// another access to the same object. So, Unidentified can overlap with
/// Class/Global access, but it cannot be the only formal access to that memory.
///
/// An *invalid* AccessedStorage object is Unidentified and associated with an
/// invalid SILValue. This signals that analysis has failed to recognize an
/// expected address producer pattern.
///
///     An active goal is to enforce that every memory operation's
///     AccessedStorage is either valid or explicitly guarded by an "unsafe"
///     begin_access.
///
/// Note that the SILValue that represents a storage object is not
/// necessarilly an address type. It may instead be a SILBoxType. So, even
/// though address phis are not allowed, finding the base of an access may
/// require traversing phis.
///
/// Support for integer IDs and bitsets. An AccessedStorage value has enough
/// extra bits to store a unique index for each identified access in a
/// function. An AccessedStorage (without an ID) can be cheaply formed
/// on-the-fly for any memory operation then used as a hash key to lookup its
/// unique integer index which is stored directly in the hashed value but not
/// used as part of the hash key.
class AccessedStorage {
public:
  /// Enumerate over all valid begin_access bases. Clients can use a covered
  /// switch to warn if AccessedStorage ever adds a case.
  enum Kind : uint8_t {
    Box,
    Stack,
    Global,
    Class,
    Tail,
    Argument,
    Yield,
    Nested,
    Unidentified,
    NumKindBits = countBitsUsed(static_cast<unsigned>(Unidentified))
  };

  static const char *getKindName(Kind k);

  // Give object tail storage a fake large property index for convenience.
  static constexpr unsigned TailIndex = std::numeric_limits<int>::max();

  /// Directly create an AccessedStorage for class or tail property access.
  static AccessedStorage forClass(SILValue object, unsigned propertyIndex) {
    AccessedStorage storage;
    if (propertyIndex == TailIndex)
      storage.initKind(Tail);
    else
      storage.initKind(Class, propertyIndex);
    storage.value = object;
    return storage;
  }

  /// Return an AccessedStorage value that best identifies a formally accessed
  /// variable pointed to by \p sourceAddress, looking through any nested
  /// formal accesses to find the underlying storage.
  ///
  /// \p sourceAddress may be an address, pointer, or box type.
  ///
  /// If \p sourceAddress is within a formal access scope, which does not have
  /// "Unsafe" enforcement, then this always returns valid storage.
  ///
  /// If \p sourceAddress is not within a formal access scope, or within an
  /// "Unsafe" scope, then this finds the formal storage if possible, otherwise
  /// returning invalid storage.
  static AccessedStorage compute(SILValue sourceAddress);

  /// Return an AccessedStorage object that identifies formal access scope that
  /// immediately encloses \p sourceAddress.
  ///
  /// \p sourceAddress may be an address, pointer, or box type.
  ///
  /// If \p sourceAddress is within a formal access scope, this always returns a
  /// valid "Nested" storage value.
  ///
  /// If \p sourceAddress is not within a formal access scope, then this finds
  /// the formal storage if possible, otherwise returning invalid storage.
  static AccessedStorage computeInScope(SILValue sourceAddress);

protected:
  // Checking the storage kind is far more common than other fields. Make sure
  // it can be byte load with no shift.
  static const int ReservedKindBits = 7;
  static_assert(ReservedKindBits >= NumKindBits, "Too many storage kinds.");

  static const unsigned InvalidElementIndex =
      (1 << (32 - (ReservedKindBits + 1))) - 1;

  // Form a bitfield that is effectively a union over any pass-specific data
  // with the fields used within this class as a common prefix.
  //
  // This allows passes to embed analysis flags, and reserves enough space to
  // embed a unique index.
  //
  // AccessedStorageAnalysis defines an StorageAccessInfo object that maps each
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
    SWIFT_INLINE_BITFIELD_BASE(AccessedStorage, 32,
                               kind : ReservedKindBits,
                               isLet : 1,
                               elementIndex : 32 - (ReservedKindBits + 1));

    // Define bits for use in AccessedStorageAnalysis. Each identified storage
    // object is mapped to one instance of this subclass.
    SWIFT_INLINE_BITFIELD_FULL(StorageAccessInfo, AccessedStorage,
                               64 - NumAccessedStorageBits,
                               accessKind : NumSILAccessKindBits,
                               noNestedConflict : 1,
                               storageIndex : 64 - (NumAccessedStorageBits
                                                    + NumSILAccessKindBits
                                                    + 1));

    // Define bits for use in the AccessEnforcementOpts pass. Each begin_access
    // in the function is mapped to one instance of this subclass.  Reserve a
    // bit for a seenNestedConflict flag, which is the per-begin-access result
    // of pass-specific analysis. The remaning bits are sufficient to index all
    // begin_[unpaired_]access instructions.
    //
    // `AccessedStorage` refers to the AccessedStorageBitfield defined above,
    // setting aside enough bits for common data.
    SWIFT_INLINE_BITFIELD_FULL(AccessEnforcementOptsInfo, AccessedStorage,
                               64 - NumAccessedStorageBits,
                               seenNestedConflict : 1,
                               seenIdenticalStorage : 1,
                               beginAccessIndex : 62 - NumAccessedStorageBits);

    // Define data flow bits for use in the AccessEnforcementDom pass. Each
    // begin_access in the function is mapped to one instance of this subclass.
    SWIFT_INLINE_BITFIELD(DomAccessedStorage, AccessedStorage, 1 + 1,
                          isInner : 1, containsRead : 1);
  } Bits;

private:
  union {
    // For non-class storage, 'value' is the access base. For class storage
    // 'value' is the object base, where the access base is the class' stored
    // property. For tail storage 'value' is the object base and there is no
    // value for the access base.
    SILValue value;
    SILGlobalVariable *global;
  };

  void initKind(Kind k, unsigned elementIndex = InvalidElementIndex) {
    Bits.opaqueBits = 0;
    Bits.AccessedStorage.kind = k;
    Bits.AccessedStorage.elementIndex = elementIndex;
  }

  unsigned getElementIndex() const { return Bits.AccessedStorage.elementIndex; }
  void setElementIndex(unsigned elementIndex) {
    Bits.AccessedStorage.elementIndex = elementIndex;
  }

public:
  AccessedStorage() : value() { initKind(Unidentified); }

  AccessedStorage(SILValue base, Kind kind);

  // Return true if this is a valid storage object.
  operator bool() const { return getKind() != Unidentified || value; }

  Kind getKind() const { return static_cast<Kind>(Bits.AccessedStorage.kind); }

  // Clear any bits reserved for subclass data. Useful for up-casting back to
  // the base class.
  void resetSubclassData() {
    initKind(getKind(), Bits.AccessedStorage.elementIndex);
  }

  SILValue getValue() const {
    assert(getKind() != Global && getKind() != Class && getKind() != Tail);
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

  SILGlobalVariable *getGlobal() const {
    assert(getKind() == Global);
    return global;
  }

  bool isReference() const { return getKind() == Class || getKind() == Tail; }

  SILValue getObject() const {
    assert(isReference());
    return value;
  }
  unsigned getPropertyIndex() const {
    assert(getKind() == Class);
    return getElementIndex();
  }

  /// Return the address or reference root that the storage was based
  /// on. Returns an invalid SILValue for globals or invalid storage.
  SILValue getRoot() const {
    switch (getKind()) {
    case AccessedStorage::Box:
    case AccessedStorage::Stack:
    case AccessedStorage::Nested:
    case AccessedStorage::Argument:
    case AccessedStorage::Yield:
    case AccessedStorage::Unidentified:
      return getValue(); // Can be invalid for Unidentified storage.
    case AccessedStorage::Global:
      return SILValue();
    case AccessedStorage::Class:
    case AccessedStorage::Tail:
      return getObject();
    }
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
  /// This compares only the AccessedStorage base class bits, ignoring the
  /// subclass bits. It is used for hash lookup equality, so it should not
  /// perform any additional lookups or dereference memory outside itself.
  bool hasIdenticalBase(const AccessedStorage &other) const {
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

  /// Return true if the given access is guaranteed to be within a heap object.
  bool isObjectAccess() const {
    return getKind() == Class || getKind() == Tail;
  }

  /// Return true if the given access is on a 'let' lvalue.
  bool isLetAccess() const { return Bits.AccessedStorage.isLet; }

  /// If this is a uniquely identified formal access, then it cannot
  /// alias with any other uniquely identified access to different storage.
  bool isUniquelyIdentified() const {
    switch (getKind()) {
    case Box:
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
  bool isDistinctFrom(const AccessedStorage &other) const {
    return isDistinctFrom<&AccessedStorage::isUniquelyIdentified>(other);
  }

  /// Return true if this identifies the base of a formal access location.
  ///
  /// Most formal access bases are uniquely identified, but class access
  /// may alias other references to the same object.
  bool isFormalAccessBase() const {
    if (isUniquelyIdentified())
      return true;

    return getKind() == Class;
  }

  /// Returns the ValueDecl for the underlying storage, if it can be
  /// determined. Otherwise returns null.
  ///
  /// If \p base is provided, then it must be the accessed base for this
  /// storage, as passed to the AccessedStorage constructor. What \p base is
  /// provided, this is guaranteed to return a valid decl for class properties;
  /// otherwise it is only a best effort based on the type of the object root
  /// *before* the object is cast to the final accessed reference type.
  const ValueDecl *getDecl(SILValue base = SILValue()) const;

  /// Get all leaf uses of all address, pointer, or box values that have a this
  /// AccessedStorage in common. Return true if all uses were found before
  /// reaching the limit.
  ///
  /// The caller of 'collectUses' can determine the use type (exact, inner, or
  /// overlapping) from the resulting \p uses list by checking 'accessPath ==
  /// usePath', accessPath.contains(usePath)', and
  /// 'accessPath.mayOverlap(usePath)'. Alternatively, the client may call
  /// 'visitAccessedStorageUses' with its own AccessUseVisitor subclass to
  /// sort the use types.
  bool
  collectUses(SmallVectorImpl<Operand *> &uses, AccessUseType useTy,
              SILFunction *function,
              unsigned useLimit = std::numeric_limits<unsigned>::max()) const;

  void print(raw_ostream &os) const;
  void dump() const;

private:
  // Disable direct comparison because we allow subclassing with bitfields.
  // Currently, we use DenseMapInfo to unique storage, which defines key
  // equalilty only in terms of the base AccessedStorage class bits.
  bool operator==(const AccessedStorage &) const = delete;
  bool operator!=(const AccessedStorage &) const = delete;

  template <bool (AccessedStorage::*IsUniqueFn)() const>
  bool isDistinctFrom(const AccessedStorage &other) const {
    if ((this->*IsUniqueFn)()) {
      if ((other.*IsUniqueFn)() && !hasIdenticalBase(other))
        return true;

      if (other.isObjectAccess())
        return true;

      // We currently assume that Unidentified storage may overlap with
      // Box/Stack storage.
      return false;
    }
    if ((other.*IsUniqueFn)())
      return other.isDistinctFrom<IsUniqueFn>(*this);

    // Neither storage is uniquely identified.
    if (isObjectAccess()) {
      if (other.isObjectAccess()) {
        // Property access cannot overlap with Tail access.
        if (getKind() != other.getKind())
          return true;

        // We could also check if the object types are distinct, but that only
        // helps if we know the relationships between class types.
        return getKind() == Class
               && getPropertyIndex() != other.getPropertyIndex();
      }
      // Any type of nested/argument address may be within the same object.
      //
      // We also currently assume Unidentified access may be within an object
      // purely to handle KeyPath accesses. The deriviation of the KeyPath
      // address must separately appear to be a Class access so that all Class
      // accesses are accounted for.
      return false;
    }
    if (other.isObjectAccess())
      return other.isDistinctFrom<IsUniqueFn>(*this);

    // Neither storage is from a class or tail.
    //
    // Unidentified values may alias with each other or with any kind of
    // nested/argument access.
    return false;
  }

  void setLetAccess(SILValue base);
};

} // end namespace swift

namespace llvm {

/// Enable using AccessedStorage as a key in DenseMap.
/// Do *not* include any extra pass data in key equality.
///
/// AccessedStorage hashing and comparison is used to determine when two
/// 'begin_access' instructions access the same or disjoint underlying objects.
///
/// `DenseMapInfo::isEqual()` guarantees that two AccessStorage values refer to
/// the same memory if both values are valid.
///
/// `!DenseMapInfo::isEqual()` does not guarantee that two identified
/// AccessStorage values are distinct. Inequality does, however, guarantee that
/// two *uniquely* identified AccessStorage values are distinct.
template <> struct DenseMapInfo<swift::AccessedStorage> {
  static swift::AccessedStorage getEmptyKey() {
    return swift::AccessedStorage(swift::SILValue::getFromOpaqueValue(
                               llvm::DenseMapInfo<void *>::getEmptyKey()),
                           swift::AccessedStorage::Unidentified);
  }

  static swift::AccessedStorage getTombstoneKey() {
    return swift::AccessedStorage(swift::SILValue::getFromOpaqueValue(
                               llvm::DenseMapInfo<void *>::getTombstoneKey()),
                           swift::AccessedStorage::Unidentified);
  }

  static unsigned getHashValue(swift::AccessedStorage storage) {
    switch (storage.getKind()) {
    case swift::AccessedStorage::Box:
    case swift::AccessedStorage::Stack:
    case swift::AccessedStorage::Nested:
    case swift::AccessedStorage::Yield:
    case swift::AccessedStorage::Unidentified:
      return DenseMapInfo<swift::SILValue>::getHashValue(storage.getValue());
    case swift::AccessedStorage::Argument:
      return storage.getParamIndex();
    case swift::AccessedStorage::Global:
      return DenseMapInfo<void *>::getHashValue(storage.getGlobal());
    case swift::AccessedStorage::Class:
      return llvm::hash_combine(storage.getObject(),
                                storage.getPropertyIndex());
    case swift::AccessedStorage::Tail:
      return DenseMapInfo<swift::SILValue>::getHashValue(storage.getObject());
    }
    llvm_unreachable("Unhandled AccessedStorageKind");
  }

  static bool isEqual(swift::AccessedStorage LHS, swift::AccessedStorage RHS) {
    return LHS.hasIdenticalBase(RHS);
  }
};

} // namespace llvm

namespace swift {

/// For convenience, encapsulate and AccessedStorage value along with its
/// accessed base address.
struct AccessedStorageWithBase {
  AccessedStorage storage;
  // The base of the formal access. For class storage, it is the
  // ref_element_addr. For global storage it is the global_addr or initializer
  // apply. For other storage, it is the same as accessPath.getRoot().
  //
  // Base may be invalid for global_addr -> address_to_pointer -> phi patterns.
  // FIXME: add a structural requirement to SIL so base is always valid in OSSA.
  SILValue base;

  AccessedStorageWithBase(AccessedStorage storage, SILValue base)
      : storage(storage), base(base) {}

  /// Identical to AccessedStorage::compute but preserves the access base.
  static AccessedStorageWithBase compute(SILValue sourceAddress);

  /// Identical to AccessedStorage::computeInScope but preserves the base.
  static AccessedStorageWithBase computeInScope(SILValue sourceAddress);
};

/// Return an AccessedStorage value that identifies formally accessed storage
/// for \p beginAccess, considering any outer access scope as having distinct
/// storage from this access scope. This is useful for exclusivity checking
/// to distinguish between nested access vs. conflicting access on the same
/// storage.
///
/// May return an invalid storage for either:
/// - A \p beginAccess with Unsafe enforcement
/// - Non-OSSA form in which address-type block args are allowed
inline AccessedStorage identifyFormalAccess(BeginAccessInst *beginAccess) {
  return AccessedStorage::computeInScope(beginAccess->getSource());
}

inline AccessedStorage
identifyFormalAccess(BeginUnpairedAccessInst *beginAccess) {
  return AccessedStorage::computeInScope(beginAccess->getSource());
}

} // end namespace swift

//===----------------------------------------------------------------------===//
//                              MARK: AccessPath
//===----------------------------------------------------------------------===//

namespace swift {

/// Identify an addressable location based the AccessedStorage and projection
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
/// A well-formed path has at most one offset component at the begining of the
/// path (chained index_addrs are merged into one offset). In other words,
/// taking an offset from a subobject projection is not well-formed access
/// path. However, it is possible (however undesirable) for programmers to
/// convert a subobject address into a pointer (for example, via implicit
/// conversion), then advance that pointer. Since we can't absolutely prevent
/// this, we instead consider it an invalid AccessPath. This is the only case in
/// which AccessPath::storage can differ from AccessedStorage::compute().
///
/// Storing an AccessPath ammortizes to constant space. To cache identification
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
  /// returning the outermost AccessedStorage.
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

  // Encode a dynamic index_addr as an UnknownOffset.
  static constexpr int UnknownOffset = std::numeric_limits<int>::min() >> 1;

  struct PathNode;

  // An access path index.
  //
  // Note:
  // - IndexTrieNode::RootIndex   = INT_MIN      = 0x80000000
  // - AccessedStorage::TailIndex = INT_MAX      = 0x7FFFFFFF
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

    bool operator==(PathNode other) const { return node == other.node; }
    bool operator!=(PathNode other) const { return node != other.node; }
  };

private:
  AccessedStorage storage;
  PathNode pathNode;
  // store the single offset index independent from the PathNode to simplify
  // checking for path overlap.
  int offset = 0;

public:
  // AccessPaths are built by AccessPath::compute(address).
  //
  // AccessedStorage is only used to identify the storage location; AccessPath
  // ignores its subclass bits.
  AccessPath(AccessedStorage storage, PathNode pathNode, int offset)
      : storage(storage), pathNode(pathNode), offset(offset) {
    assert(storage.getKind() != AccessedStorage::Nested);
    assert(pathNode.isValid() || !storage && "Access path requires a pathNode");
  }

  AccessPath() = default;

  bool operator==(AccessPath other) const {
    return
      storage.hasIdenticalBase(other.storage) && pathNode == other.pathNode;
  }
  bool operator!=(AccessPath other) const { return !(*this == other); }

  bool isValid() const { return pathNode.isValid(); }

  AccessedStorage getStorage() const { return storage; }

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
  /// AccessedStorage in common. Return true if all uses were found before
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

  void printPath(raw_ostream &os) const;
  void print(raw_ostream &os) const;
  void dump() const;
};

// Encapsulate the result of computing an AccessPath. AccessPath does not store
// the base address of the formal access because it does not always uniquely
// indentify the access, but AccessPath users may use the base address to to
// recover the def-use chain for a specific global_addr or ref_element_addr.
struct AccessPathWithBase {
  AccessPath accessPath;
  // The address-type value that is the base of the formal access. For
  // class storage, it is the ref_element_addr. For global storage it is the
  // global_addr or initializer apply. For other storage, it is the same as
  // accessPath.getRoot().
  //
  // Note: base may be invalid for global_addr -> address_to_pointer -> phi
  // patterns, while the accessPath is still valid.
  //
  // FIXME: add a structural requirement to SIL so base is always valid in OSSA.
  SILValue base;

  /// Compute the access path at \p address, and record the access base. This
  /// ignores begin_access markers, returning the outermost AccessedStorage.
  static AccessPathWithBase compute(SILValue address);

  /// Compute the access path at \p address, and record the access base. If \p
  /// address is within a formal access, then AccessStorage will have a nested
  /// type and base will be a begin_access marker.
  static AccessPathWithBase computeInScope(SILValue address);

  AccessPathWithBase(AccessPath accessPath, SILValue base)
      : accessPath(accessPath), base(base) {}

  bool operator==(AccessPathWithBase other) const {
    return accessPath == other.accessPath && base == other.base;
  }
  bool operator!=(AccessPathWithBase other) const { return !(*this == other); }

  void print(raw_ostream &os) const;
  void dump() const;
};

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
        DenseMapInfo<swift::AccessedStorage>::getEmptyKey(),
        swift::AccessPath::PathNode(
          DenseMapInfo<swift::IndexTrieNode *>::getEmptyKey()), 0);
  }
  static inline swift::AccessPath getTombstoneKey() {
    return swift::AccessPath(
        DenseMapInfo<swift::AccessedStorage>::getTombstoneKey(),
        swift::AccessPath::PathNode(
          DenseMapInfo<swift::IndexTrieNode *>::getTombstoneKey()), 0);
  }
  static inline unsigned getHashValue(const swift::AccessPath &val) {
    return llvm::hash_combine(
        DenseMapInfo<swift::AccessedStorage>::getHashValue(val.getStorage()),
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
bool visitAccessedStorageUses(AccessUseVisitor &visitor,
                              AccessedStorage storage,
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

} // end namespace swift

//===----------------------------------------------------------------------===//
//             MARK: Helper API for specific formal access patterns
//===----------------------------------------------------------------------===//

namespace swift {

/// Return true if the given address operand is used by a memory operation that
/// initializes the memory at that address, implying that the previous value is
/// uninitialized.
bool memInstMustInitialize(Operand *memOper);

/// Is this an alloc_stack instruction that is:
///
/// 1. Only initialized once in its own def block.
/// 2. Never written to again except by destroy_addr.
///
/// On return, destroyingUsers contains the list of users that destroy the
/// alloc_stack. If the alloc_stack is destroyed in pieces, we do not guarantee
/// that the list of destroying users is a minimal jointly post-dominating set.
bool isSingleInitAllocStack(AllocStackInst *asi,
                            SmallVectorImpl<Operand *> &destroyingUses);

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
/// `storage` must be a valid, non-nested AccessedStorage object.
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
/// This is not a member of AccessedStorage because it only makes sense to use
/// in SILGen before access markers are emitted, or when verifying access
/// markers.
bool isPossibleFormalAccessBase(const AccessedStorage &storage,
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

/// Return true if \p value is a cast that preserves the identity and
/// reference-counting equivalence of the reference at operand zero.
bool isRCIdentityPreservingCast(SingleValueInstruction *svi);

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
  // Check getAccessProjectionOperand() before isAccessedStorageCast() because
  // it will consider any project_box to be a storage cast.
  case SILInstructionKind::ProjectBoxInst:
    if (auto *load = dyn_cast<LoadInst>(svi->getOperand(0)))
      return &load->getOperandRef();

    return nullptr;
  };
}

/// An address, pointer, or box cast that occurs outside of the formal
/// access. These convert the base of accessed storage without affecting the
/// AccessPath. Useful for both use-def and def-use traversal. The source
/// address must be at operand(0).
///
/// Some of these casts, such as address_to_pointer, may also occur inside of a
/// formal access. TODO: Add stricter structural guarantee such that these never
/// occur within an access. It's important to be able to get the accessed
/// address without looking though type casts or pointer_to_address [strict],
/// which we can't do if those operations are behind access projections.
inline bool isAccessedStorageCast(SingleValueInstruction *svi) {
  switch (svi->getKind()) {
  default:
    return false;

  // Simply pass-thru the incoming address.
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::MarkDependenceInst:
  // Look through a project_box to identify the underlying alloc_box as the
  // accesed object. It must be possible to reach either the alloc_box or the
  // containing enum in this loop, only looking through simple value
  // propagation such as copy_value and begin_borrow.
  case SILInstructionKind::ProjectBoxInst:
  case SILInstructionKind::ProjectBlockStorageInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::BeginBorrowInst:
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
  // AccessedStorage for 'Unsafe' access.
  case SILInstructionKind::PointerToAddressInst:
    return true;
  }
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
    return asImpl().visitBase(field, AccessedStorage::Class);
  }
  Result visitTailAccess(RefTailAddrInst *tail) {
    return asImpl().visitBase(tail, AccessedStorage::Tail);
  }
  Result visitArgumentAccess(SILFunctionArgument *arg) {
    return asImpl().visitBase(arg, AccessedStorage::Argument);
  }
  Result visitBoxAccess(AllocBoxInst *box) {
    return asImpl().visitBase(box, AccessedStorage::Box);
  }
  /// \p global may be either a GlobalAddrInst or the ApplyInst for a global
  /// accessor function.
  Result visitGlobalAccess(SILValue global) {
    return asImpl().visitBase(global, AccessedStorage::Global);
  }
  Result visitYieldAccess(BeginApplyResult *yield) {
    return asImpl().visitBase(yield, AccessedStorage::Yield);
  }
  Result visitStackAccess(AllocStackInst *stack) {
    return asImpl().visitBase(stack, AccessedStorage::Stack);
  }
  Result visitNestedAccess(BeginAccessInst *access) {
    return asImpl().visitBase(access, AccessedStorage::Nested);
  }
  Result visitUnidentified(SILValue base) {
    return asImpl().visitBase(base, AccessedStorage::Unidentified);
  }

  // Subclasses must provide implementations for:
  //
  // Result visitBase(SILValue base, AccessedStorage::Kind kind);
  // Result visitNonAccess(SILValue base);
  // Result visitPhi(SILPhiArgument *phi);
  // Result visitStorageCast(SingleValueInstruction *cast, Operand *sourceOper);
  // Result visitAccessProjection(SingleValueInstruction *cast,
  //                              Operand *sourceOper);

  Result visit(SILValue sourceAddr);
};

template<typename Impl, typename Result>
Result AccessUseDefChainVisitor<Impl, Result>::visit(SILValue sourceAddr) {
  if (auto *svi = dyn_cast<SingleValueInstruction>(sourceAddr)) {
    if (auto *projOper = getAccessProjectionOperand(svi))
      return asImpl().visitAccessProjection(svi, projOper);

    if (isAccessedStorageCast(svi))
      return asImpl().visitStorageCast(svi, &svi->getAllOperands()[0]);
  }
  switch (sourceAddr->getKind()) {
  default:
    break;

  // MARK: Handle immediately-identifiable instructions.

  // An AllocBox is a fully identified memory location.
  case ValueKind::AllocBoxInst:
    return asImpl().visitBoxAccess(cast<AllocBoxInst>(sourceAddr));

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
  case ValueKind::BeginApplyResult:
    return asImpl().visitYieldAccess(cast<BeginApplyResult>(sourceAddr));

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
    if (phiArg->isPhiArgument()) {
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
