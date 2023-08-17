//===--- SILValue.h - Value base class for SIL ------------------*- C++ -*-===//
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
// This file defines the SILValue class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILVALUE_H
#define SWIFT_SIL_SILVALUE_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/ArrayRefView.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILNode.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

namespace swift {

class DominanceInfo;
class PostOrderFunctionInfo;
class ReversePostOrderInfo;
class Operand;
class SILInstruction;
class SILArgument;
class SILLocation;
class DeadEndBlocks;
class ValueBaseUseIterator;
class ConsumingUseIterator;
class NonConsumingUseIterator;
class TypeDependentUseIterator;
class NonTypeDependentUseIterator;
class SILValue;
class SILModuleConventions;

/// An enumeration which contains values for all the concrete ValueBase
/// subclasses.
enum class ValueKind : std::underlying_type<SILNodeKind>::type {
#define VALUE(ID, PARENT) \
  ID = unsigned(SILNodeKind::ID),
#define VALUE_RANGE(ID, FIRST, LAST) \
  First_##ID = unsigned(SILNodeKind::First_##ID), \
  Last_##ID = unsigned(SILNodeKind::Last_##ID),
#include "swift/SIL/SILNodes.def"
};

/// ValueKind hashes to its underlying integer representation.
static inline llvm::hash_code hash_value(ValueKind K) {
  return llvm::hash_value(size_t(K));
}

/// What constraint does the given use of an SSA value put on the lifetime of
/// the given SSA value.
///
/// There are two possible constraints: NonLifetimeEnding and
/// LifetimeEnding. NonLifetimeEnding means that the SSA value must be
/// able to be used in a valid way at the given use
/// point. LifetimeEnding means that the value has been invalidated at
/// the given use point and any uses reachable from that point are
/// invalid in SIL causing a SIL verifier error.
enum class UseLifetimeConstraint {
  /// This use requires the SSA value to be live after the given instruction's
  /// execution.
  NonLifetimeEnding,

  /// This use invalidates the given SSA value.
  ///
  /// This means that the given SSA value can not have any uses that are
  /// reachable from this instruction. When a value has owned semantics this
  /// means the SSA value is destroyed at this point. When a value has
  /// guaranteed (i.e. shared borrow) semantics this means that the program
  /// has left the scope of the borrowed SSA value and said value can not be
  /// used.
  LifetimeEnding,
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              UseLifetimeConstraint constraint);

/// A lattice that we use to classify ownership at the SIL level. None is top
/// and Any is bottom and all of the other ownership kinds are mid level nodes
/// in the lattice. Graphically the lattice looks as follows:
///             +----+
///     +-------|None|---------+
///     |       +----+         |
///     |          |           |
///     v          v           v
/// +-------+   +-----+   +----------+
/// |Unowned|   |Owned|   |Guaranteed|
/// +-------+   +-----+   +----------+
///     |          |           |
///     |          v           |
///     |        +---+         |
///     +------->|Any|<--------+
///              +---+
///
/// One moves up the lattice by performing a join operation and one moves down
/// the lattice by performing a meet operation.
///
/// This type is used in two different composition types:
///
/// * ValueOwnershipKind: This represents the ownership kind that a value can
///   take. Since our ownership system is strict, we require that all values
///   have a non-Any ownership since Any represents a type of ownership unknown
///   statically. Thus we treat Any as representing an invalid
///   value. ValueOwnershipKinds can only perform a meet operation to determine
///   if two ownership kinds are compatible with a merge of Any showing the
///   merge is impossible since values can not have any ownership. Values with
///   ownership None are statically proven to be trivial values, often because
///   they are trivially typed, but sometimes because of path-sensitive
///   information like knowledge of an enum case. Trivial values have no
///   ownership semantics.
///
/// * OwnershipConstraint: This represents a constraint on the values that can
///   be used by a specific operand. Here Any is valid and is used for operands
///   that don't care about the ownership kind (lack ownership constraints). In
///   contrast, a constraint of None is the most restrictive. It requires a
///   trivial value. An Unowned, Owned, or Guaranteed constraint requires either
///   a value with the named ownership, or a trivial value.
struct OwnershipKind {
  enum innerty : uint8_t {
    /// An ownership kind that models an ownership that is unknown statically at
    /// compile time. It is invalid when applied to values because we have
    /// strict ownership rules for values. But it is an expected/normal state
    /// when constraining ownership kinds.
    Any = 0,

    /// A SILValue with `Unowned` ownership kind is an independent value that
    /// has a lifetime that is only guaranteed to last until the next program
    /// visible side-effect. To maintain the lifetime of an unowned value, it
    /// must be converted to an owned representation via a copy_value.
    ///
    /// Unowned ownership kind occurs mainly along method/function boundaries in
    /// between Swift and Objective-C code.
    Unowned,

    /// A SILValue with `Owned` ownership kind is an independent value that has
    /// an ownership independent of any other ownership imbued within it. The
    /// SILValue must be paired with a consuming operation that ends the SSA
    /// value's lifetime exactly once along all paths through the program.
    Owned,

    /// A SILValue with `Guaranteed` ownership kind is an independent value that
    /// is guaranteed to be live over a specific region of the program. This
    /// region can come in several forms:
    ///
    /// 1. @guaranteed function argument. This guarantees that a value will
    /// outlive a function.
    ///
    /// 2. A shared borrow region. This is a region denoted by a
    /// begin_borrow/load_borrow instruction and an end_borrow instruction. The
    /// SSA value must not be destroyed or taken inside the borrowed region.
    ///
    /// Any value with guaranteed ownership must be paired with an end_borrow
    /// instruction exactly once along any path through the program.
    Guaranteed,

    /// A SILValue with None ownership kind is an independent value outside of
    /// the ownership system. It is used to model values that are statically
    /// determined to be trivial. This includes trivially typed values as well
    /// as trivial cases of non-trivial enums. Naturally None can be merged with
    /// any ValueOwnershipKind allowing us to naturally model merge and branch
    /// points in the SSA graph, where more information about the value is
    /// statically available on some control flow paths.
    None,

    LastValueOwnershipKind = None,
  } value;

  using UnderlyingType = std::underlying_type<innerty>::type;
  static constexpr unsigned NumBits = SILNode::NumVOKindBits;
  static constexpr UnderlyingType MaxValue = (UnderlyingType(1) << NumBits);
  static constexpr uint64_t Mask = MaxValue - 1;
  static_assert(unsigned(OwnershipKind::LastValueOwnershipKind) < MaxValue,
                "LastValueOwnershipKind is larger than max representable "
                "ownership value?!");

  OwnershipKind(OwnershipKind::innerty other) : value(other) {}
  OwnershipKind(const OwnershipKind &other) : value(other.value) {}

  OwnershipKind &operator=(const OwnershipKind &other) {
    value = other.value;
    return *this;
  }

  OwnershipKind &operator=(OwnershipKind::innerty other) {
    value = other;
    return *this;
  }

  operator OwnershipKind::innerty() const { return value; }

  /// Move down the lattice.
  OwnershipKind meet(OwnershipKind other) const {
    // None merges with anything.
    if (*this == OwnershipKind::None)
      return other;
    if (other == OwnershipKind::None)
      return *this;

    // At this point, if the two ownership kinds don't line up, the merge
    // fails. Return any to show that we have lost information and now have a
    // value kind that is invalid on values.
    if (*this != other)
      return OwnershipKind::Any;

    // Otherwise, we are good, return *this.
    return *this;
  }

  /// Move up the lattice.
  OwnershipKind join(OwnershipKind other) const {
    if (*this == OwnershipKind::Any)
      return other;
    if (other == OwnershipKind::Any)
      return *this;
    if (*this != other)
      return OwnershipKind::None;
    return *this;
  }

  /// Convert this ownership kind to a StringRef.
  StringRef asString() const;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const OwnershipKind &kind);

struct OperandOwnership;

/// A value representing the specific ownership semantics that a SILValue may
/// have.
struct ValueOwnershipKind {
  using innerty = OwnershipKind::innerty;

  OwnershipKind value;

  ValueOwnershipKind(innerty newValue) : value(newValue) {}
  ValueOwnershipKind(OwnershipKind newValue) : value(newValue) {}
  explicit ValueOwnershipKind(unsigned newValue) : value(innerty(newValue)) {}
  ValueOwnershipKind(const SILFunction &f, SILType type,
                     SILArgumentConvention convention);
  ValueOwnershipKind(const SILFunction &f, SILType type,
                     SILArgumentConvention convention,
                     SILModuleConventions moduleConventions);

  /// Parse Value into a ValueOwnershipKind.
  ///
  /// *NOTE* Emits an unreachable if an invalid value is passed in.
  explicit ValueOwnershipKind(StringRef value);

  operator OwnershipKind() const { return value; }
  explicit operator unsigned() const { return value; }
  operator innerty() const { return value; }

  explicit operator bool() const { return value != OwnershipKind::Any; }

#ifndef __cpp_impl_three_way_comparison
  // C++20 (more precisely P1185) introduced more overload candidates for
  // comparison operator calls. With that in place the following definitions are
  // redundant and actually cause compilation errors because of ambiguity.
  // P1630 explains the rationale behind introducing this backward
  // incompatibility.
  //
  // References:
  // https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1185r2.html
  // https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2019/p1630r1.html

  bool operator==(ValueOwnershipKind other) const {
    return value == other.value;
  }
  bool operator!=(ValueOwnershipKind other) const {
    return !(value == other.value);
  }

  bool operator==(innerty other) const { return value == other; }
  bool operator!=(innerty other) const { return !(value == other); }
#endif

  /// We merge by moving down the lattice.
  ValueOwnershipKind merge(ValueOwnershipKind rhs) const {
    return value.meet(rhs.value);
  }

  /// Given that there is an aggregate value (like a struct or enum) with this
  /// ownership kind, and a subobject of type Proj is being projected from the
  /// aggregate, return Trivial if Proj has trivial type and the aggregate's
  /// ownership kind otherwise.
  ValueOwnershipKind getProjectedOwnershipKind(const SILFunction &func,
                                               SILType projType) const;

  /// Return the lifetime constraint semantics for this
  /// ValueOwnershipKind when forwarding ownership.
  ///
  /// This is MustBeInvalidated for Owned and MustBeLive for all other ownership
  /// kinds.
  UseLifetimeConstraint getForwardingLifetimeConstraint() const {
    switch (value) {
    case OwnershipKind::Any:
    case OwnershipKind::None:
    case OwnershipKind::Guaranteed:
    case OwnershipKind::Unowned:
      return UseLifetimeConstraint::NonLifetimeEnding;
    case OwnershipKind::Owned:
      return UseLifetimeConstraint::LifetimeEnding;
    }
    llvm_unreachable("covered switch");
  }

  /// Return the OperandOwnership for a forwarded operand when the forwarded
  /// result has this ValueOwnershipKind. \p allowUnowned is true for a subset
  /// of forwarding operations that are allowed to propagate Unowned values.
  OperandOwnership getForwardingOperandOwnership(bool allowUnowned) const;

  /// Returns true if \p Other can be merged successfully with this, implying
  /// that the two ownership kinds are "compatible".
  ///
  /// The reason why we do not compare directy is to allow for
  /// OwnershipKind::None to merge into other forms of ValueOwnershipKind.
  bool isCompatibleWith(ValueOwnershipKind other) const {
    return bool(merge(other));
  }

  /// Returns isCompatibleWith(other->getOwnershipKind()).
  ///
  /// Definition is inline after SILValue is defined to work around circular
  /// dependencies.
  bool isCompatibleWith(SILValue other) const;

  template <typename RangeTy> static ValueOwnershipKind merge(RangeTy &&r) {
    auto initial = OwnershipKind::None;
    return accumulate(std::forward<RangeTy>(r), initial,
                      [](ValueOwnershipKind acc, ValueOwnershipKind x) {
                        if (!acc)
                          return acc;
                        return acc.merge(x);
                      });
  }

  StringRef asString() const;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, ValueOwnershipKind Kind);

/// This is the base class of the SIL value hierarchy, which represents a
/// runtime computed value. Some examples of ValueBase are SILArgument and
/// SingleValueInstruction.
class ValueBase : public SILNode, public SILAllocated<ValueBase> {
  friend class Operand;

  SILType Type;
  Operand *FirstUse = nullptr;

  ValueBase(const ValueBase &) = delete;
  ValueBase &operator=(const ValueBase &) = delete;

protected:
  ValueBase(ValueKind kind, SILType type)
      : SILNode(SILNodeKind(kind)), Type(type) {}

public:
  ~ValueBase() {
    assert(use_empty() && "Cannot destroy a value that still has uses!");
  }

  LLVM_ATTRIBUTE_ALWAYS_INLINE
  ValueKind getKind() const { return ValueKind(SILNode::getKind()); }

  SILType getType() const {
    return Type;
  }

  /// Replace every use of a result of this instruction with the corresponding
  /// result from RHS.
  ///
  /// The method assumes that both instructions have the same number of
  /// results. To replace just one result use SILValue::replaceAllUsesWith.
  void replaceAllUsesWith(ValueBase *RHS);

  /// Replace all uses of this instruction with an undef value of the
  /// same type as the result of this instruction.
  void replaceAllUsesWithUndef();

  void replaceAllTypeDependentUsesWith(ValueBase *RHS);

  /// Is this value a direct result of the given instruction?
  bool isResultOf(SILInstruction *I) const;

  /// Returns true if this value has no uses.
  /// To ignore debug-info instructions use swift::onlyHaveDebugUses instead
  /// (see comment in DebugUtils.h).
  bool use_empty() const { return FirstUse == nullptr; }

  using use_iterator = ValueBaseUseIterator;
  using use_range = iterator_range<use_iterator>;
  using consuming_use_iterator = ConsumingUseIterator;
  using consuming_use_range = iterator_range<consuming_use_iterator>;
  using non_consuming_use_iterator = NonConsumingUseIterator;
  using non_consuming_use_range = iterator_range<non_consuming_use_iterator>;
  using typedependent_use_iterator = TypeDependentUseIterator;
  using typedependent_use_range = iterator_range<typedependent_use_iterator>;
  using non_typedependent_use_iterator = NonTypeDependentUseIterator;
  using non_typedependent_use_range =
      iterator_range<non_typedependent_use_iterator>;

  inline use_iterator use_begin() const;
  inline use_iterator use_end() const;

  inline consuming_use_iterator consuming_use_begin() const;
  inline consuming_use_iterator consuming_use_end() const;

  inline non_consuming_use_iterator non_consuming_use_begin() const;
  inline non_consuming_use_iterator non_consuming_use_end() const;

  inline typedependent_use_iterator typedependent_use_begin() const;
  inline typedependent_use_iterator typedependent_use_end() const;

  inline non_typedependent_use_iterator non_typedependent_use_begin() const;
  inline non_typedependent_use_iterator non_typedependent_use_end() const;

  /// Returns a range of all uses, which is useful for iterating over all uses.
  /// To ignore debug-info instructions use swift::getNonDebugUses instead
  /// (see comment in DebugUtils.h).
  inline use_range getUses() const;

  /// Returns true if this value has exactly one use.
  /// To ignore debug-info instructions use swift::hasOneNonDebugUse instead
  /// (see comment in DebugUtils.h).
  inline bool hasOneUse() const;

  /// Returns .some(single user) if this value has a single user. Returns .none
  /// otherwise.
  inline Operand *getSingleUse() const;

  /// Returns .some(single user) if this value is non-trivial, we are in ossa,
  /// and it has a single consuming user. Returns .none otherwise.
  inline Operand *getSingleConsumingUse() const;

  /// Returns a range of all consuming uses
  inline consuming_use_range getConsumingUses() const;

  /// Returns a range of all non consuming uses
  inline non_consuming_use_range getNonConsumingUses() const;

  /// Returns a range of uses that are classified as a type dependent
  /// operand of the user.
  inline typedependent_use_range getTypeDependentUses() const;

  /// Returns a range of uses that are not classified as a type dependent
  /// operand of the user.
  inline non_typedependent_use_range getNonTypeDependentUses() const;

  template <class T>
  inline T *getSingleUserOfType() const;

  template <class T> inline T *getSingleConsumingUserOfType() const;

  /// Returns true if this operand has exactly two uses.
  ///
  /// This is useful if one has found a predefined set of 2 unique users and
  /// wants to check if there are any other users without iterating over the
  /// entire use list.
  inline bool hasTwoUses() const;

  /// Helper struct for DowncastUserFilterRange and UserRange
  struct UseToUser;

  using UserRange =
      llvm::iterator_range<llvm::mapped_iterator<swift::ValueBaseUseIterator,
                                                 swift::ValueBase::UseToUser,
                                                 swift::SILInstruction *>>;
  inline UserRange getUsers() const;

  template <typename Subclass>
  using DowncastUserFilterRange =
      DowncastFilterRange<Subclass,
                          iterator_range<llvm::mapped_iterator<
                              use_iterator, UseToUser, SILInstruction *>>>;

  /// Iterate over the use list of this ValueBase visiting all users that are of
  /// class T.
  ///
  /// Example:
  ///
  ///   ValueBase *v = ...;
  ///   for (CopyValueInst *cvi : v->getUsersOfType<CopyValueInst>()) { ... }
  ///
  /// NOTE: Uses llvm::dyn_cast internally.
  template <typename T>
  inline DowncastUserFilterRange<T> getUsersOfType() const;

  /// Return the instruction that defines this value, or null if it is
  /// not defined by an instruction.
  const SILInstruction *getDefiningInstruction() const {
    return const_cast<ValueBase*>(this)->getDefiningInstruction();
  }
  SILInstruction *getDefiningInstruction();

  /// Return the instruction that defines this value, terminator instruction
  /// that produces this result, or null if it is not defined by an instruction.
  const SILInstruction *getDefiningInstructionOrTerminator() const {
    return const_cast<ValueBase*>(this)->getDefiningInstructionOrTerminator();
  }
  SILInstruction *getDefiningInstructionOrTerminator();

  /// Return the SIL instruction that can be used to describe the first time
  /// this value is available.
  ///
  /// For instruction results, this returns getDefiningInstruction(). For
  /// arguments, this returns SILBasicBlock::begin() for the argument's parent
  /// block. Returns nullptr for SILUndef.
  ///
  /// FIXME: remove this redundant API from SILValue.
  SILInstruction *getDefiningInsertionPoint();

  // Const version of \see getDefiningInsertionPoint.
  const SILInstruction *getDefiningInsertionPoint() const {
    return const_cast<ValueBase *>(this)->getDefiningInsertionPoint();
  }

  /// Return the next SIL instruction to execute /after/ this value is
  /// available.
  ///
  /// Operationally this means that:
  ///
  /// * For SILArguments, this returns the first instruction in the block.
  ///
  /// * For SILInstructions, this returns std::next. This is the main divergence
  ///   from getDefiningInsertionPoint() (see discussion below).
  ///
  /// * For SILUndef, this returns nullptr.
  ///
  /// DISCUSSION: The reason that this exists is that when one wants a "next"
  /// instruction pointer, one often times wants to write:
  ///
  ///   if (auto *insertPt = value->getDefiningInsertionPoint())
  ///     return std::next(insertPt);
  ///
  /// This is incorrect for SILArguments since after processing a SILArgument,
  /// we need to process the actual first instruction in the block. With this
  /// API, one can simply do:
  ///
  ///   if (auto *inst = value->getNextInstruction())
  ///     return inst;
  ///
  /// And get the correct answer every time.
  SILInstruction *getNextInstruction();

  // Const version of \see getDefiningInsertionPoint.
  const SILInstruction *getNextInstruction() const {
    return const_cast<ValueBase *>(this)->getNextInstruction();
  }

  struct DefiningInstructionResult {
    SILInstruction *Instruction;
    size_t ResultIndex;
  };

  /// Return the instruction that defines this value and the appropriate
  /// result index, or None if it is not defined by an instruction.
  llvm::Optional<DefiningInstructionResult> getDefiningInstructionResult();

  /// Returns the ValueOwnershipKind that describes this SILValue's ownership
  /// semantics if the SILValue has ownership semantics. Returns is a value
  /// without any Ownership Semantics.
  ///
  /// An example of a SILValue without ownership semantics is a
  /// struct_element_addr.
  ///
  /// NOTE: This is implemented in ValueOwnership.cpp not SILValue.cpp.
  ValueOwnershipKind getOwnershipKind() const;

  bool isLexical() const;

  bool isGuaranteedForwarding() const;

  /// Unsafely eliminate moveonly from this value's type. Returns true if the
  /// value's underlying type was move only and thus was changed. Returns false
  /// otherwise.
  ///
  /// NOTE: Please do not use this directly! It is only meant to be used by the
  /// optimizer pass: SILMoveOnlyWrappedTypeEliminator.
  bool unsafelyEliminateMoveOnlyWrapper(const SILFunction *fn) {
    if (!Type.isMoveOnlyWrapped() && !Type.isBoxedMoveOnlyWrappedType(fn))
      return false;
    if (Type.isMoveOnlyWrapped()) {
      Type = Type.removingMoveOnlyWrapper();
    } else {
      assert(Type.isBoxedMoveOnlyWrappedType(fn));
      Type = Type.removingMoveOnlyWrapperToBoxedType(fn);
    }
    return true;
  }

  /// Returns true if this value should be traced for optimization debugging
  /// (it has a debug_value [trace] user).
  bool hasDebugTrace() const;

  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_ValueBase &&
           node->getKind() <= SILNodeKind::Last_ValueBase;
  }
  static bool classof(const ValueBase *V) { return true; }

  /// This is supportable but usually suggests a logic mistake.
  static bool classof(const SILInstruction *) = delete;
};

} // end namespace swift

namespace llvm {

/// ValueBase * is always at least eight-byte aligned; make the three tag bits
/// available through PointerLikeTypeTraits.
template<>
struct PointerLikeTypeTraits<swift::ValueBase *> {
public:
  static inline void *getAsVoidPointer(swift::ValueBase *I) {
    return (void*)I;
  }
  static inline swift::ValueBase *getFromVoidPointer(void *P) {
    return (swift::ValueBase *)P;
  }
  enum { NumLowBitsAvailable = 3 };
};

} // end namespace llvm

namespace swift {

/// SILValue - A SILValue is a wrapper around a ValueBase pointer.
class SILValue {
  ValueBase *Value;

public:
  SILValue(const ValueBase *V = nullptr)
    : Value(const_cast<ValueBase *>(V)) { }

  ValueBase *operator->() const { return Value; }
  ValueBase &operator*() const { return *Value; }
  operator ValueBase *() const { return Value; }

  // Comparison.
  bool operator==(SILValue RHS) const { return Value == RHS.Value; }
  bool operator==(ValueBase *RHS) const { return Value == RHS; }
  bool operator!=(SILValue RHS) const { return !(*this == RHS); }
  bool operator!=(ValueBase *RHS) const { return Value != RHS; }

  /// Return true if underlying ValueBase of this SILValue is non-null. Return
  /// false otherwise.
  explicit operator bool() const { return Value != nullptr; }

  /// Get a location for this value.
  SILLocation getLoc() const;

  /// Convert this SILValue into an opaque pointer like type. For use with
  /// PointerLikeTypeTraits.
  void *getOpaqueValue() const {
    return (void *)Value;
  }

  /// Convert the given opaque pointer into a SILValue. For use with
  /// PointerLikeTypeTraits.
  static SILValue getFromOpaqueValue(void *p) {
    return SILValue((ValueBase *)p);
  }

  enum {
    NumLowBitsAvailable =
    llvm::PointerLikeTypeTraits<ValueBase *>::
          NumLowBitsAvailable
  };

  /// If this SILValue is a result of an instruction, return its
  /// defining instruction. Returns nullptr otherwise.
  SILInstruction *getDefiningInstruction() {
    return Value->getDefiningInstruction();
  }

  /// If this SILValue is a result of an instruction, return its
  /// defining instruction. Returns nullptr otherwise.
  const SILInstruction *getDefiningInstruction() const {
    return Value->getDefiningInstruction();
  }

  /// Verify that this SILValue and its uses respects ownership invariants.
  ///
  /// \p DEBlocks is nullptr when OSSA lifetimes are complete.
  void verifyOwnership(DeadEndBlocks *DEBlocks) const;

  SWIFT_DEBUG_DUMP;
};

inline SILNodePointer::SILNodePointer(SILValue value) : node(value) { }

inline bool ValueOwnershipKind::isCompatibleWith(SILValue other) const {
  return isCompatibleWith(other->getOwnershipKind());
}

/// Constraints on the ownership of an operand value.
///
/// The ownershipKind component constrains the operand's value ownership to be
/// the same or "above" the constraint in the lattice, such that
/// join(constraint, valueOwnership) == valueOwnership. In other words, applying
/// the constraint does not change the value's ownership. For example, a value
/// with None ownership is accepted by any OwnershipConstraint, and an
/// OwnershipConstraint with 'Any' ownership kind can accept any value. Note
/// that operands commonly allow either Owned or Guaranteed operands. These
/// operands have an Any ownership constraint to allow either. However,
/// enforcement of Unowned value is more strict. This requires separate logic in
/// canAcceptUnownedValue() to avoid complicating the OwnershipKind lattice.
class OwnershipConstraint {
  OwnershipKind ownershipKind;
  UseLifetimeConstraint lifetimeConstraint;

public:
  OwnershipConstraint(OwnershipKind inputOwnershipKind,
                      UseLifetimeConstraint inputLifetimeConstraint)
      : ownershipKind(inputOwnershipKind),
        lifetimeConstraint(inputLifetimeConstraint) {
    assert((ownershipKind != OwnershipKind::None ||
            lifetimeConstraint == UseLifetimeConstraint::NonLifetimeEnding) &&
           "ValueOwnershipKind::None can never have their lifetime ended");
  }

  OwnershipKind getPreferredKind() const {
    return ownershipKind;
  }

  bool isLifetimeEnding() const {
    return lifetimeConstraint == UseLifetimeConstraint::LifetimeEnding;
  }

  UseLifetimeConstraint getLifetimeConstraint() const {
    return lifetimeConstraint;
  }

  bool isConsuming() const {
    return ownershipKind == OwnershipKind::Owned
      && lifetimeConstraint == UseLifetimeConstraint::LifetimeEnding;
  }

  bool satisfiedBy(const Operand *use) const;

  bool satisfiesConstraint(ValueOwnershipKind testKind) const {
    return ownershipKind.join(testKind) == testKind;
  }

  bool operator==(const OwnershipConstraint &other) const {
    return ownershipKind == other.ownershipKind &&
           isLifetimeEnding() == other.isLifetimeEnding();
  }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              OwnershipConstraint constraint);

/// Categorize all uses in terms of their ownership effect.
///
/// Used to verify completeness of the ownership use model and exhaustively
/// switch over any category of ownership use. Implies ownership constraints and
/// lifetime constraints.
///
/// OperandOwnership may be statically determined by the user's opcode alone, or
/// by the opcode and operand type. Or it may be dynamically determined by an
/// ownership kind variable in the user's state. However, it may never be
/// inferred from the ownership of the incoming value. This way, the logic for
/// determining which ValueOwnershipKind an operand may accept is reliable.
///
/// Any use that takes an Owned or Guaranteed value may also take a trivial
/// value (ownership None), because the ownership semantics are irrelevant.
struct OperandOwnership {
  enum innerty : uint8_t {
    /// Operands that do not use the value. They only represent a dependence
    /// on a dominating definition and do not require liveness.
    /// (type-dependent operands)
    NonUse,

    /// Uses that can only handle trivial values. The operand value must have
    /// None ownership. These uses require liveness but are otherwise
    /// unverified.
    TrivialUse,

    /// Use the value only for the duration of the operation, which may have
    /// side effects. Requires an owned or guaranteed value.
    /// (single-instruction apply with @guaranteed argument)
    InstantaneousUse,

    /// MARK: Uses of Any ownership values:

    /// Use a value without requiring or propagating ownership. The operation
    /// may not have side-effects that could affect ownership. This is limited
    /// to a small number of operations that are allowed to take Unowned values.
    /// (copy_value, single-instruction apply with @unowned argument))
    UnownedInstantaneousUse,

    /// Forwarding instruction with an Unowned result. Its operands may have any
    /// ownership.
    ForwardingUnowned,

    // Escape a pointer into a value which cannot be tracked or verified.
    //
    // TODO: Eliminate the PointerEscape category. All pointer escapes should be
    // InteriorPointer, guarded by a borrow scope, and verified.
    PointerEscape,

    /// Bitwise escape. Escapes the nontrivial contents of the value.
    /// OSSA does not enforce the lifetime of the escaping bits.
    /// The programmer must explicitly force lifetime extension.
    /// (ref_to_unowned, unchecked_trivial_bitcast)
    BitwiseEscape,

    /// Borrow. Propagates the owned or guaranteed value within a scope, without
    /// ending its lifetime.
    /// (begin_borrow, begin_apply with @guaranteed argument)
    Borrow,

    /// MARK: Uses of Owned (or None) values:

    /// Destroying Consume. Destroys the owned value immediately.
    /// (store, destroy, @owned destructure).
    DestroyingConsume,
    /// Forwarding Consume. Consumes the owned value indirectly via a move.
    /// (br, destructure, tuple, struct, cast, switch).
    ForwardingConsume,

    /// MARK: Uses of Guaranteed (or None) values:

    /// Interior Pointer. Propagates a trivial value (e.g. address, pointer, or
    /// no-escape closure) that depends on the guaranteed value within the
    /// base's borrow scope. The verifier checks that all uses of the trivial
    /// value are in scope.
    /// (ref_element_addr, open_existential_box)
    InteriorPointer,
    /// Forwarded Borrow. Propagates the guaranteed value within the base's
    /// borrow scope.
    /// (tuple_extract, struct_extract, cast, switch)
    GuaranteedForwarding,
    /// End Borrow. End the borrow scope opened directly by the operand.
    /// The operand must be a begin_borrow, begin_apply, or function argument.
    /// (end_borrow, end_apply)
    EndBorrow,
    // Reborrow. Ends the borrow scope opened directly by the operand and begins
    // one or multiple disjoint borrow scopes. If a forwarded value is
    // reborrowed, then its base must also be reborrowed at the same point.
    // (br, FIXME: should also include destructure, tuple, struct)
    Reborrow
  } value;

  OperandOwnership(innerty newValue) : value(newValue) {}
  OperandOwnership(const OperandOwnership &other): value(other.value) {}

  OperandOwnership &operator=(const OperandOwnership &other) {
    value = other.value;
    return *this;
  }

  OperandOwnership &operator=(OperandOwnership::innerty other) {
    value = other;
    return *this;
  }

  operator innerty() const { return value; }

  StringRef asString() const;

  /// Return the OwnershipConstraint corresponding to this OperandOwnership.
  OwnershipConstraint getOwnershipConstraint();
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const OperandOwnership &operandOwnership);

/// Map OperandOwnership to the OwnershipConstraint used in OSSA validation.
///
/// Each OperandOwnership kind maps directly to a fixed OwnershipConstraint. Any
/// value that can be legally passed to this operand must have an ownership kind
/// permitted by this constraint. A constraint permits an ownership kind if,
/// when it is applied to that ownership kind via a lattice join, it returns the
/// same ownership kind, indicating that no restriction exists.
///
/// Consequently, OperandOwnership kinds that are allowed to take either Owned
/// or Guaranteed values map to an OwnershipKind::Any constraint.
///
/// Unowned values are more restricted than Owned or Guaranteed values in
/// terms of their valid uses, which helps limit the situations where the
/// implementation needs to consider this special case. This additional
/// restriction is validated by `canAcceptUnownedValue`.
///
/// Forwarding instructions that produce Owned or Guaranteed values always
/// forward an operand of the same ownership kind. Each case has a distinct
/// OperandOwnership (ForwardingConsume and GuaranteedForwarding), which
/// enforces a specific constraint on the operand's ownership. Forwarding
/// instructions that produce an Unowned value, however, may forward an operand
/// of any ownership. Therefore, ForwardingUnowned is mapped to
/// OwnershipKind::Any.
///
/// This design yields the following advantages:
///
/// 1. Keeping the verification of Unowned in a separate utility avoids
///    the need to add an extra OwnedOrGuaranteed state to the OwnershipKind
///    lattice. That state would be meaningless as a representation of value
///    ownership, would serve no purpose as a data flow state, and would make
///    the basic definition of ownership less approachable to developers.
///
/// 2. Owned or Guaranteed values can be passed to instructions that want to
///    produce an unowned result from a parent operand. This simplifies the IR
///    and makes RAUWing Unowned values with Owned or Guaranteed values much
///    easier since it does not need to introduce operations that convert those
///    values to Unowned. This significantly simplifies the implementation of
///    OSSA utilities.
///
/// Defined inline so the switch is eliminated for constant OperandOwnership.
inline OwnershipConstraint OperandOwnership::getOwnershipConstraint() {
  switch (value) {
  case OperandOwnership::TrivialUse:
    return {OwnershipKind::None, UseLifetimeConstraint::NonLifetimeEnding};
  case OperandOwnership::NonUse:
  case OperandOwnership::InstantaneousUse:
  case OperandOwnership::UnownedInstantaneousUse:
  case OperandOwnership::ForwardingUnowned:
  case OperandOwnership::PointerEscape:
  case OperandOwnership::BitwiseEscape:
  case OperandOwnership::Borrow:
    return {OwnershipKind::Any, UseLifetimeConstraint::NonLifetimeEnding};
  case OperandOwnership::DestroyingConsume:
  case OperandOwnership::ForwardingConsume:
    return {OwnershipKind::Owned, UseLifetimeConstraint::LifetimeEnding};
  case OperandOwnership::InteriorPointer:
  case OperandOwnership::GuaranteedForwarding:
    return {OwnershipKind::Guaranteed,
            UseLifetimeConstraint::NonLifetimeEnding};
  case OperandOwnership::EndBorrow:
  case OperandOwnership::Reborrow:
    return {OwnershipKind::Guaranteed, UseLifetimeConstraint::LifetimeEnding};
  }
  llvm_unreachable("covered switch");
}

/// Return true if this use can accept Unowned values.
///
/// This extra restriction is applied on top of the OwnershipConstraint to limit
/// the spread of Unowned values.
inline bool canAcceptUnownedValue(OperandOwnership operandOwnership) {
  switch (operandOwnership) {
  case OperandOwnership::NonUse:
  case OperandOwnership::UnownedInstantaneousUse:
  case OperandOwnership::ForwardingUnowned:
  case OperandOwnership::PointerEscape:
  case OperandOwnership::BitwiseEscape:
    return true;
  case OperandOwnership::TrivialUse:
  case OperandOwnership::InstantaneousUse:
  case OperandOwnership::Borrow:
  case OperandOwnership::DestroyingConsume:
  case OperandOwnership::ForwardingConsume:
  case OperandOwnership::InteriorPointer:
  case OperandOwnership::GuaranteedForwarding:
  case OperandOwnership::EndBorrow:
  case OperandOwnership::Reborrow:
    return false;
  }
  llvm_unreachable("covered switch");
}

/// Return true if all OperandOwnership invariants hold.
bool checkOperandOwnershipInvariants(const Operand *operand,
                                     SILModuleConventions *silConv = nullptr);

/// Return the OperandOwnership for a forwarded operand when the forwarding
/// operation has this "forwarding ownership" (as returned by
/// getForwardingOwnershipKind()). \p allowUnowned is true for a subset of
/// forwarding operations that are allowed to propagate Unowned values.
///
/// Forwarding ownership is determined by the forwarding instruction's constant
/// ownership attribute. If forwarding ownership is owned, then the instruction
/// moves owned operand to its result, ending its lifetime. If forwarding
/// ownership is guaranteed, then the instruction propagates the lifetime of its
/// borrows operand through its result.
///
/// The resulting forwarded value typically has forwarding ownership, but may
/// differ when the result is trivial type. e.g. an owned or guaranteed value
/// can be cast to a trivial type using owned or guaranteed forwarding.
inline OperandOwnership
ValueOwnershipKind::getForwardingOperandOwnership(bool allowUnowned) const {
  switch (value) {
  case OwnershipKind::Any:
    llvm_unreachable("invalid value ownership");
  case OwnershipKind::Unowned:
    if (allowUnowned) {
      return OperandOwnership::ForwardingUnowned;
    }
    llvm_unreachable("invalid value ownership");
  case OwnershipKind::None:
    return OperandOwnership::TrivialUse;
  case OwnershipKind::Guaranteed:
    return OperandOwnership::GuaranteedForwarding;
  case OwnershipKind::Owned:
    return OperandOwnership::ForwardingConsume;
  }
}

/// A formal SIL reference to a value, suitable for use as a stored
/// operand.
class Operand {
  /// The value used as this operand.
  SILValue TheValue;

  /// The next operand in the use-chain.  Note that the chain holds
  /// every use of the current ValueBase, not just those of the
  /// designated result.
  Operand *NextUse = nullptr;

  /// A back-pointer in the use-chain, required for fast patching
  /// of use-chains.
  Operand **Back = nullptr;

  /// The owner of this operand.
  /// FIXME: this could be space-compressed.
  SILInstruction *Owner;

public:
  Operand(SILInstruction *owner) : Owner(owner) {}
  Operand(SILInstruction *owner, SILValue theValue)
      : TheValue(theValue), Owner(owner) {
    insertIntoCurrent();
  }

  /// Operands are not copyable.
  Operand(const Operand &use) = delete;
  Operand &operator=(const Operand &use) = delete;

  Operand(Operand &&) = default;
  Operand &operator=(Operand &&) = default;

  /// Return the current value being used by this operand.
  SILValue get() const { return TheValue; }

  /// Set the current value being used by this operand.
  void set(SILValue newValue) {
    // It's probably not worth optimizing for the case of switching
    // operands on a single value.
    removeFromCurrent();
    TheValue = newValue;
    insertIntoCurrent();
  }

  /// Swap the given operand with the current one.
  void swap(Operand &Op) {
    SILValue OtherV = Op.get();
    Op.set(get());
    set(OtherV);
  }

  /// Remove this use of the operand.
  void drop() {
    removeFromCurrent();
    TheValue = SILValue();
    NextUse = nullptr;
    Back = nullptr;
    Owner = nullptr;
  }

  ~Operand() {
    removeFromCurrent();
  }

  /// Return the user that owns this use.
  SILInstruction *getUser() { return Owner; }
  const SILInstruction *getUser() const { return Owner; }

  Operand *getNextUse() const { return NextUse; }

  /// Return true if this operand is a type dependent operand.
  ///
  /// Implemented in SILInstruction.h
  bool isTypeDependent() const;

  /// Return which operand this is in the operand list of the using instruction.
  unsigned getOperandNumber() const;

  /// Return the use ownership of this operand.
  ///
  /// NOTE: This is implemented in OperandOwnership.cpp.
  OperandOwnership
  getOperandOwnership(SILModuleConventions *silConv = nullptr) const;

  /// Return the ownership constraint that restricts what types of values this
  /// Operand can contain.
  OwnershipConstraint
  getOwnershipConstraint(SILModuleConventions *silConv = nullptr) const {
    return getOperandOwnership(silConv).getOwnershipConstraint();
  }

  /// Returns true if changing the operand to use a value with the given
  /// ownership kind, without rewriting the instruction, would not cause the
  /// operand to violate the operand's ownership constraints.
  bool canAcceptKind(ValueOwnershipKind kind,
                     SILModuleConventions *silConv = nullptr) const;

  /// Returns true if this operand and its value satisfy the operand's
  /// operand constraint.
  bool satisfiesConstraints(SILModuleConventions *silConv = nullptr) const;

  /// Returns true if this operand acts as a use that ends the lifetime its
  /// associated value, either by consuming the owned value or ending the
  /// guaranteed scope.
  bool isLifetimeEnding() const;

  /// Returns true if this ends the lifetime of an owned operand.
  bool isConsuming() const;

  bool endsLocalBorrowScope() const {
    auto ownership = getOperandOwnership();
    return ownership == OperandOwnership::EndBorrow
           || ownership == OperandOwnership::Reborrow;
  }

  SILBasicBlock *getParentBlock() const;
  SILFunction *getParentFunction() const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP;

private:
  void removeFromCurrent() {
    if (!Back)
      return;
    *Back = NextUse;
    if (NextUse)
      NextUse->Back = Back;
  }

  void insertIntoCurrent() {
    Back = &TheValue->FirstUse;
    NextUse = TheValue->FirstUse;
    if (NextUse)
      NextUse->Back = &NextUse;
    TheValue->FirstUse = this;
  }

  friend class ValueBase;
  friend class ValueBaseUseIterator;
  friend class ConsumingUseIterator;
  friend class NonConsumingUseIterator;
  friend class TypeDependentUseIterator;
  friend class NonTypeDependentUseIterator;
  template <unsigned N> friend class FixedOperandList;
  friend class TrailingOperandsList;
};

/// A class which adapts an array of Operands into an array of Values.
///
/// The intent is that this should basically act exactly like
/// ArrayRef except projecting away the Operand-ness.
inline SILValue getSILValueType(const Operand &op) {
  return op.get();
}
using OperandValueArrayRef = ArrayRefView<Operand, SILValue, getSILValueType>;

/// An iterator over all uses of a ValueBase.
class ValueBaseUseIterator {
protected:
  Operand *Cur;
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = Operand*;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;    

  ValueBaseUseIterator() = default;
  explicit ValueBaseUseIterator(Operand *cur) : Cur(cur) {}
  Operand *operator->() const { return Cur; }
  Operand *operator*() const { return Cur; }

  SILInstruction *getUser() const {
    return Cur->getUser();
  }

  ValueBaseUseIterator &operator++() {
    assert(Cur && "incrementing past end()!");
    Cur = Cur->NextUse;
    return *this;
  }

  ValueBaseUseIterator operator++(int unused) {
    ValueBaseUseIterator copy = *this;
    ++*this;
    return copy;
  }

  friend bool operator==(ValueBaseUseIterator lhs,
                         ValueBaseUseIterator rhs) {
    return lhs.Cur == rhs.Cur;
  }
  friend bool operator!=(ValueBaseUseIterator lhs,
                         ValueBaseUseIterator rhs) {
    return !(lhs == rhs);
  }
};
inline ValueBase::use_iterator ValueBase::use_begin() const {
  return ValueBase::use_iterator(FirstUse);
}
inline ValueBase::use_iterator ValueBase::use_end() const {
  return ValueBase::use_iterator(nullptr);
}
inline iterator_range<ValueBase::use_iterator> ValueBase::getUses() const {
  return { use_begin(), use_end() };
}

class ConsumingUseIterator : public ValueBaseUseIterator {
public:
  explicit ConsumingUseIterator(Operand *cur) : ValueBaseUseIterator(cur) {}
  ConsumingUseIterator &operator++() {
    assert(Cur && "incrementing past end()!");
    assert(Cur->isLifetimeEnding());
    while ((Cur = Cur->NextUse)) {
      if (Cur->isLifetimeEnding())
        break;
    }
    return *this;
  }

  ConsumingUseIterator operator++(int unused) {
    ConsumingUseIterator copy = *this;
    ++*this;
    return copy;
  }
};

inline ValueBase::consuming_use_iterator
ValueBase::consuming_use_begin() const {
  auto cur = FirstUse;
  while (cur && !cur->isLifetimeEnding()) {
    cur = cur->NextUse;
  }
  return ValueBase::consuming_use_iterator(cur);
}

inline ValueBase::consuming_use_iterator ValueBase::consuming_use_end() const {
  return ValueBase::consuming_use_iterator(nullptr);
}

class NonConsumingUseIterator : public ValueBaseUseIterator {
public:
  explicit NonConsumingUseIterator(Operand *cur) : ValueBaseUseIterator(cur) {}
  NonConsumingUseIterator &operator++() {
    assert(Cur && "incrementing past end()!");
    assert(!Cur->isLifetimeEnding());
    while ((Cur = Cur->NextUse)) {
      if (!Cur->isLifetimeEnding())
        break;
    }
    return *this;
  }

  NonConsumingUseIterator operator++(int unused) {
    NonConsumingUseIterator copy = *this;
    ++*this;
    return copy;
  }
};

inline ValueBase::non_consuming_use_iterator
ValueBase::non_consuming_use_begin() const {
  auto cur = FirstUse;
  while (cur && cur->isLifetimeEnding()) {
    cur = cur->NextUse;
  }
  return ValueBase::non_consuming_use_iterator(cur);
}

inline ValueBase::non_consuming_use_iterator
ValueBase::non_consuming_use_end() const {
  return ValueBase::non_consuming_use_iterator(nullptr);
}

class TypeDependentUseIterator : public ValueBaseUseIterator {
public:
  explicit TypeDependentUseIterator(Operand *cur) : ValueBaseUseIterator(cur) {}
  TypeDependentUseIterator &operator++() {
    assert(Cur && "incrementing past end()!");
    while ((Cur = Cur->NextUse)) {
      if (Cur->isTypeDependent())
        break;
    }
    return *this;
  }

  TypeDependentUseIterator operator++(int unused) {
    TypeDependentUseIterator copy = *this;
    ++*this;
    return copy;
  }
};

inline ValueBase::typedependent_use_iterator
ValueBase::typedependent_use_begin() const {
  auto cur = FirstUse;
  while (cur && !cur->isTypeDependent()) {
    cur = cur->NextUse;
  }
  return ValueBase::typedependent_use_iterator(cur);
}

inline ValueBase::typedependent_use_iterator
ValueBase::typedependent_use_end() const {
  return ValueBase::typedependent_use_iterator(nullptr);
}

class NonTypeDependentUseIterator : public ValueBaseUseIterator {
public:
  explicit NonTypeDependentUseIterator(Operand *cur)
      : ValueBaseUseIterator(cur) {}
  NonTypeDependentUseIterator &operator++() {
    assert(Cur && "incrementing past end()!");
    assert(!Cur->isTypeDependent());
    while ((Cur = Cur->NextUse)) {
      if (!Cur->isTypeDependent())
        break;
    }
    return *this;
  }

  NonTypeDependentUseIterator operator++(int unused) {
    NonTypeDependentUseIterator copy = *this;
    ++*this;
    return copy;
  }
};

inline ValueBase::non_typedependent_use_iterator
ValueBase::non_typedependent_use_begin() const {
  auto cur = FirstUse;
  while (cur && cur->isTypeDependent()) {
    cur = cur->NextUse;
  }
  return ValueBase::non_typedependent_use_iterator(cur);
}

inline ValueBase::non_typedependent_use_iterator
ValueBase::non_typedependent_use_end() const {
  return ValueBase::non_typedependent_use_iterator(nullptr);
}

inline bool ValueBase::hasOneUse() const {
  auto I = use_begin(), E = use_end();
  if (I == E) return false;
  return ++I == E;
}
inline Operand *ValueBase::getSingleUse() const {
  auto I = use_begin(), E = use_end();

  // If we have no elements, return nullptr.
  if (I == E) return nullptr;

  // Otherwise, grab the first element and then increment.
  Operand *Op = *I;
  ++I;

  // If the next element is not the end list, then return nullptr. We do not
  // have one user.
  if (I != E) return nullptr;

  // Otherwise, the element that we accessed.
  return Op;
}

inline Operand *ValueBase::getSingleConsumingUse() const {
  Operand *result = nullptr;
  for (auto *op : getUses()) {
    if (op->isLifetimeEnding()) {
      if (result) {
        return nullptr;
      }
      result = op;
    }
  }
  return result;
}

inline ValueBase::consuming_use_range ValueBase::getConsumingUses() const {
  return {consuming_use_begin(), consuming_use_end()};
}

inline ValueBase::non_consuming_use_range
ValueBase::getNonConsumingUses() const {
  return {non_consuming_use_begin(), non_consuming_use_end()};
}

inline ValueBase::typedependent_use_range
ValueBase::getTypeDependentUses() const {
  return {typedependent_use_begin(), typedependent_use_end()};
}

inline ValueBase::non_typedependent_use_range
ValueBase::getNonTypeDependentUses() const {
  return {non_typedependent_use_begin(), non_typedependent_use_end()};
}

inline bool ValueBase::hasTwoUses() const {
  auto iter = use_begin(), end = use_end();
  for (unsigned i = 0; i < 2; ++i) {
    if (iter == end)
      return false;
    ++iter;
  }
  return iter == end;
}

template <class T>
inline T *ValueBase::getSingleUserOfType() const {
  T *result = nullptr;
  for (auto *op : getUses()) {
    if (auto *tmp = dyn_cast<T>(op->getUser())) {
      if (result)
        return nullptr;
      result = tmp;
    }
  }
  return result;
}

template <class T> inline T *ValueBase::getSingleConsumingUserOfType() const {
  auto *op = getSingleConsumingUse();
  if (!op)
    return nullptr;

  return dyn_cast<T>(op->getUser());
}

struct ValueBase::UseToUser {
  SILInstruction *operator()(const Operand *use) const {
    return const_cast<SILInstruction *>(use->getUser());
  }
  SILInstruction *operator()(const Operand &use) const {
    return const_cast<SILInstruction *>(use.getUser());
  }
  SILInstruction *operator()(Operand *use) { return use->getUser(); }
  SILInstruction *operator()(Operand &use) { return use.getUser(); }
};

inline ValueBase::UserRange ValueBase::getUsers() const {
  return llvm::map_range(getUses(), ValueBase::UseToUser());
}

template <typename T>
inline ValueBase::DowncastUserFilterRange<T> ValueBase::getUsersOfType() const {
  auto begin = llvm::map_iterator(use_begin(), UseToUser());
  auto end = llvm::map_iterator(use_end(), UseToUser());
  auto transformRange = llvm::make_range(begin, end);
  return makeDowncastFilterRange<T>(transformRange);
}

/// A constant-size list of the operands of an instruction.
template <unsigned N> class FixedOperandList {
  Operand Buffer[N];

  FixedOperandList(const FixedOperandList &) = delete;
  FixedOperandList &operator=(const FixedOperandList &) = delete;

public:
  template <class... T> FixedOperandList(SILInstruction *user, T&&...args)
      : Buffer{ { user, std::forward<T>(args) }... } {
    static_assert(sizeof...(args) == N, "wrong number of initializers");
  }

  /// Returns the full list of operands.
  MutableArrayRef<Operand> asArray() {
    return MutableArrayRef<Operand>(Buffer, N);
  }
  ArrayRef<Operand> asArray() const {
    return ArrayRef<Operand>(Buffer, N);
  }

  /// Returns the full list of operand values.
  OperandValueArrayRef asValueArray() const {
    return OperandValueArrayRef(asArray());
  }

  /// Indexes into the full list of operands.
  Operand &operator[](unsigned i) { return asArray()[i]; }
  const Operand &operator[](unsigned i) const { return asArray()[i]; }
};

/// A helper class for initializing the list of trailing operands.
class TrailingOperandsList {
public:
  static void InitOperandsList(Operand *p, SILInstruction *user,
                               SILValue operand, ArrayRef<SILValue> operands) {
    assert(p && "Trying to initialize operands using a nullptr");
    new (p++) Operand(user, operand);
    for (auto op : operands) {
      new (p++) Operand(user, op);
    }
  }
  static void InitOperandsList(Operand *p, SILInstruction *user,
                               SILValue operand0, SILValue operand1,
                               ArrayRef<SILValue> operands) {
    assert(p && "Trying to initialize operands using a nullptr");
    new (p++) Operand(user, operand0);
    new (p++) Operand(user, operand1);
    for (auto op : operands) {
      new (p++) Operand(user, op);
    }
  }

  static void InitOperandsList(Operand *p, SILInstruction *user,
                               ArrayRef<SILValue> operands) {
    assert(p && "Trying to initialize operands using a nullptr");
    for (auto op : operands) {
      new (p++) Operand(user, op);
    }
  }
};

/// SILValue hashes just like a pointer.
static inline llvm::hash_code hash_value(SILValue V) {
  return llvm::hash_value((ValueBase *)V);
}

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SILValue V) {
  V->print(OS);
  return OS;
}

/// Used internally in e.g. the SIL parser and deserializer to handle forward-
/// referenced values.
/// A PlaceholderValue must not appear in valid SIL.
class PlaceholderValue : public ValueBase {
  static int numPlaceholderValuesAlive;

public:
  PlaceholderValue(SILType type);
  ~PlaceholderValue();

  static int getNumPlaceholderValuesAlive() { return numPlaceholderValuesAlive; }

  static bool classof(const SILArgument *) = delete;
  static bool classof(const SILInstruction *) = delete;
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::PlaceholderValue;
  }
};

} // end namespace swift


namespace llvm {
  /// A SILValue casts like a ValueBase *.
  template<> struct simplify_type<const ::swift::SILValue> {
    using SimpleType = ::swift::ValueBase *;
    static SimpleType getSimplifiedValue(::swift::SILValue Val) {
      return Val;
    }
  };
  template<> struct simplify_type< ::swift::SILValue>
    : public simplify_type<const ::swift::SILValue> {};

  // Values hash just like pointers.
  template<> struct DenseMapInfo<swift::SILValue> {
    static swift::SILValue getEmptyKey() {
      return swift::SILValue::getFromOpaqueValue(
                                      llvm::DenseMapInfo<void*>::getEmptyKey());
    }
    static swift::SILValue getTombstoneKey() {
      return swift::SILValue::getFromOpaqueValue(
                                  llvm::DenseMapInfo<void*>::getTombstoneKey());
    }
    static unsigned getHashValue(swift::SILValue V) {
      return DenseMapInfo<swift::ValueBase *>::getHashValue(V);
    }
    static bool isEqual(swift::SILValue LHS, swift::SILValue RHS) {
      return LHS == RHS;
    }
  };

  /// SILValue is a PointerLikeType.
  template<> struct PointerLikeTypeTraits<::swift::SILValue> {
    using SILValue = ::swift::SILValue;
  public:
    static void *getAsVoidPointer(SILValue v) {
      return v.getOpaqueValue();
    }
    static SILValue getFromVoidPointer(void *p) {
      return SILValue::getFromOpaqueValue(p);
    }

    enum { NumLowBitsAvailable = swift::SILValue::NumLowBitsAvailable };
  };

} // end namespace llvm

#endif
