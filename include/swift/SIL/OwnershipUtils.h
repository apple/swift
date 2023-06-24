//===--- OwnershipUtils.h ------------------------------------*- C++ -*----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_OWNERSHIPUTILS_H
#define SWIFT_SIL_OWNERSHIPUTILS_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/NoDiscard.h"
#include "swift/SIL/AddressWalker.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/StackList.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILBasicBlock;
class SILInstruction;
class SILModule;
class SILValue;
class DeadEndBlocks;
class MultiDefPrunedLiveness;
struct BorrowedValue;

//===----------------------------------------------------------------------===//
//                            Forwarding Utilities
//
// TODO: encapsulate in a ForwardingInstruction abstraction
//===----------------------------------------------------------------------===//

/// Is the opcode that produces \p value capable of forwarding inner guaranteed
/// values?
///
/// This may be true even if the current instance of the instruction is not a
/// GuaranteedForwarding. If true, then the operation may be trivially rewritten
/// with Guaranteed ownership.
bool canOpcodeForwardInnerGuaranteedValues(SILValue value);

/// Is the opcode that consumes \p use capable of forwarding inner guaranteed
/// values?
///
/// This may be true even if \p use is not a GuaranteedForwarding. If true, then
/// the operation may be trivially rewritten with Guaranteed ownership.
bool canOpcodeForwardInnerGuaranteedValues(Operand *use);

bool computeIsScoped(SILArgument *arg);

bool computeIsReborrow(SILArgument *arg);

// This is the use-def equivalent of use->getOperandOwnership() ==
// OperandOwnership::GuaranteedForwarding.
bool computeIsGuaranteedForwarding(SILValue value);

/// Is the opcode that produces \p value capable of forwarding owned values?
///
/// This may be true even if the current instance of the instruction is not a
/// ForwardingConsume. If true, then the operation may be trivially rewritten
/// with Owned ownership.
bool canOpcodeForwardOwnedValues(SILValue value);

/// Is this opcode that consumes \p use capable of forwarding owned values?
///
/// This may be true even if the current instance of the instruction is not a
/// ForwardingConsume. If true, then the operation may be trivially rewritten
/// with Owned ownership.
bool canOpcodeForwardOwnedValues(Operand *use);

// This is the use-def equivalent of use->getOperandOwnership() ==
// OperandOwnership::ForwardingConsume.
inline bool isForwardingConsume(SILValue value) {
  assert(value->getOwnershipKind() == OwnershipKind::Owned);
  return canOpcodeForwardOwnedValues(value);
}

//===----------------------------------------------------------------------===//
//                        Ownership Def-Use Utilities
//===----------------------------------------------------------------------===//

bool findPointerEscape(BorrowedValue value);

/// Whether the specified OSSA-lifetime introducer has a pointer escape.
///
/// precondition: \p value introduces an OSSA-lifetime, either a BorrowedValue
/// can be constructed from it or it's an owned value
bool findPointerEscape(SILValue value);

/// Find leaf "use points" of \p guaranteedValue that determine its lifetime
/// requirement. Return true if no PointerEscape use was found.
///
/// Precondition: \p guaranteedValue is not a BorrowedValue.
///
/// In general, if the client does not know whether \p guaranteed value
/// introduces a borrow scope or not, it should instead call
/// findTransitiveGuaranteedUses() which efficiently gathers use
/// points for arbitrary guaranteed values, including those that introduce a
/// borrow scope and may be reborrowed.
///
/// In valid OSSA, this should never be called on values that introduce a new
/// scope (doing so would be extremely inefficient). The lifetime of a borrow
/// introducing instruction is always determined by its direct EndBorrow uses
/// (see BorrowedValue::visitLocalScopeEndingUses). None of the non-scope-ending
/// uses are relevant, and there's no need to transitively follow forwarding
/// uses. However, this utility may be used on borrow-introducing values when
/// updating OSSA form to place EndBorrow uses after introducing new phis.
///
/// When this is called on a value that does not introduce a new scope, none of
/// the use points can be EndBorrows or Reborrows. Those uses are only allowed
/// on borrow-introducing values.
bool findInnerTransitiveGuaranteedUses(
    SILValue guaranteedValue, SmallVectorImpl<Operand *> *usePoints = nullptr);

/// Find all uses in the extended lifetime (i.e. including copies) of a simple
/// (i.e. not reborrowed) borrow scope and its transitive uses.
bool findExtendedUsesOfSimpleBorrowedValue(
    BorrowedValue borrowedValue,
    SmallVectorImpl<Operand *> *usePoints = nullptr);

/// Find leaf "use points" of a guaranteed value within its enclosing borrow
/// scope (without looking through reborrows). To find the use points of the
/// extended borrow scope, after looking through reborrows, use
/// findExtendedTransitiveGuaranteedUses() instead.
///
/// Accumulate results in \p usePoints. This avoids the need for separate
/// worklist and result vectors. Existing vector elements are ignored.
///
/// "Use points" are the relevant points for determining lifetime. They are
/// determined differently depending on each of these two cases:
///
/// 1. If \p guaranteedValue introduces a borrow scope (begin_borrow,
/// load_borrow, or phi), then its only use points are the scope-ending uses,
/// and this function returns true. This is, in fact, equivalent to calling
/// BorrowedValue::visitLocalScopeEndingUses(). Any scope-ending uses that are
/// reborrows are recorded as use points without following the reborrowed
/// uses. The \p visitReborrow callback can be used to transitively process
/// reborrows to discover the extended lifetime. Reborrows may be recursive, so
/// this will require checking membership in a working set. Nested borrow scope
/// are irrelevant to the parent scope's lifetime. They are not considered use
/// points, and reborrows within those nested scope are not visited by \p
/// visitReborrow.
///
/// 2. If \p guaranteedValue does not introduce a borrow scope (it is not a
/// valid BorrowedValue), then its uses are discovered transitively by looking
/// through forwarding operations. If any use is a PointerEscape, then this
/// returns false without adding more uses--the guaranteed value's lifetime is
/// indeterminate. If a use introduces a nested borrow scope, it creates use
/// points where the "extended" borrow scope ends. An extended borrow
/// scope is found by looking through any reborrows that end the nested
/// scope. Other uses within nested borrow scopes are ignored.
bool findTransitiveGuaranteedUses(SILValue guaranteedValue,
                                  SmallVectorImpl<Operand *> &usePoints,
                                  function_ref<void(Operand *)> visitReborrow);

/// Find all "use points" of guaranteed value across its extended borrow scope
/// (looking through reborrows). The "use points" are the relevant points for
/// determining lifetime.
///
/// Accumulate results in \p usePoints. This avoids the need for separate
/// worklist and result vectors. Existing vector elements are ignored.
///
/// "Use points" are the relevant points for determining lifetime. They are
/// determined differently depending on each of these two cases:
///
/// 1. If \p guaranteedValue introduces a borrow scope (begin_borrow,
/// load_borrow, or phi), then its only use points are the extended scope-ending
/// uses, and this function returns true. This is, in fact, equivalent to
/// calling BorrowedValue::visitExtendedScopeEndingUses().
///
/// 2. If \p guaranteedValue does not introduce a borrow scope (it is not a
/// valid BorrowedValue), then its uses are discovered transitively by looking
/// through forwarding operations. Only a BorrowedValue can have its lifetime
/// extended by a reborrow; therefore, in this case, the algorithm is equivalent
/// to findTransitiveGuaranteedUses(). See those comments for more detail.
bool findExtendedTransitiveGuaranteedUses(
  SILValue guaranteedValue,
  SmallVectorImpl<Operand *> &usePoints);

/// Find non-transitive uses of a simple (i.e. without looking through
/// reborrows) value.
///
/// The scope-ending use of borrows of the value are included.  If a borrow of
/// the value is reborrowed, returns false.
bool findUsesOfSimpleValue(SILValue value,
                           SmallVectorImpl<Operand *> *usePoints = nullptr);

/// Visit all GuaranteedForwardingPhis of \p value, not looking through
/// reborrows.
bool visitGuaranteedForwardingPhisForSSAValue(
    SILValue value, function_ref<bool(Operand *)> func);

//===----------------------------------------------------------------------===//
//                                Abstractions
//===----------------------------------------------------------------------===//

/// An operand that forwards ownership to one or more results.
class ForwardingOperand {
  Operand *use = nullptr;

public:
  explicit ForwardingOperand(Operand *use);

  OwnershipConstraint getOwnershipConstraint() const {
    // We use a force unwrap since a ForwardingOperand should always have an
    // ownership constraint.
    return use->getOwnershipConstraint();
  }

  bool preservesOwnership() const {
    auto &mixin = *ForwardingInstruction::get(use->getUser());
    return mixin.preservesOwnership();
  }

  ValueOwnershipKind getForwardingOwnershipKind() const;
  void setForwardingOwnershipKind(ValueOwnershipKind newKind) const;
  void replaceOwnershipKind(ValueOwnershipKind oldKind,
                            ValueOwnershipKind newKind) const;

  operator bool() const { return bool(use); }
  const Operand *operator->() const {
    assert(use);
    return use;
  }
  Operand *operator->() {
    assert(use);
    return use;
  }
  const Operand &operator*() const { return *use; }
  Operand &operator*() { return *use; }

  /// Call \p visitor with each value that contains the final forwarded
  /// ownership of. E.x.: result of a unchecked_ref_cast, phi arguments of a
  /// switch_enum.
  bool visitForwardedValues(function_ref<bool(SILValue)> visitor);

  /// If statically this forwarded operand has a single forwarded value that the
  /// operand forwards ownership into, return that value. Return false
  /// otherwise.
  SILValue getSingleForwardedValue() const;
};

class BorrowingOperandKind {
public:
  enum Kind : uint8_t {
    Invalid = 0,
    BeginBorrow,
    BeginApply,
    Branch,
    Apply,
    TryApply,
    Yield,
    PartialApplyStack,
    BeginAsyncLet,
  };

private:
  Kind value;

public:
  BorrowingOperandKind(Kind newValue) : value(newValue) {}

  operator Kind() const { return value; }

  static BorrowingOperandKind get(SILInstruction *i) {
    switch (i->getKind()) {
    default:
      return Kind::Invalid;
    case SILInstructionKind::BeginBorrowInst:
      return Kind::BeginBorrow;
    case SILInstructionKind::BeginApplyInst:
      return Kind::BeginApply;
    case SILInstructionKind::BranchInst:
      return Kind::Branch;
    case SILInstructionKind::ApplyInst:
      return Kind::Apply;
    case SILInstructionKind::TryApplyInst:
      return Kind::TryApply;
    case SILInstructionKind::YieldInst:
      return Kind::Yield;
    case SILInstructionKind::PartialApplyInst:
      return Kind::PartialApplyStack;
    case SILInstructionKind::BuiltinInst: {
      auto bi = cast<BuiltinInst>(i);
      if (bi->getBuiltinKind() == BuiltinValueKind::StartAsyncLetWithLocalBuffer) {
        return Kind::BeginAsyncLet;
      }
      return Kind::Invalid;
    }
    }
  }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, BorrowingOperandKind kind);

struct BorrowedValue;

/// An operand whose user instruction introduces a new borrow scope for the
/// operand's value. By executing the given user, the operand's value becomes
/// borrowed and thus the incoming value must implicitly be borrowed until the
/// user's corresponding end scope instruction.
///
/// Invariant: For a given operand, BorrowingOperand is valid iff
/// it has OperandOwnership::Borrow or OperandOwnership::Reborrow.
///
/// NOTE: We do not require that the guaranteed scope be represented by a
/// guaranteed value in the same function: see begin_apply. In such cases, we
/// require instead an end_* instruction to mark the end of the scope's region.
struct BorrowingOperand {
  Operand *op;
  BorrowingOperandKind kind;

  BorrowingOperand(Operand *op)
      : op(op), kind(BorrowingOperandKind::get(op->getUser())) {
    auto ownership = op->getOperandOwnership();
    if (ownership != OperandOwnership::Borrow
        && ownership != OperandOwnership::Reborrow) {
      // consuming applies and branch arguments are not borrowing operands.
      kind = BorrowingOperandKind::Invalid;
      return;
    }
    assert(kind != BorrowingOperandKind::Invalid && "missing case");
  }
  BorrowingOperand(const BorrowingOperand &other)
      : op(other.op), kind(other.kind) {}
  BorrowingOperand &operator=(const BorrowingOperand &other) {
    kind = other.kind;
    op = other.op;
    return *this;
  }

  // A set of operators so that a BorrowingOperand can be used like a normal
  // operand in a light weight way.
  const Operand *operator*() const { return op; }
  Operand *operator*() { return op; }
  const Operand *operator->() const { return op; }
  Operand *operator->() { return op; }

  operator bool() const { return kind != BorrowingOperandKind::Invalid; }

  /// If this borrowing operand results in the underlying value being borrowed
  /// over a region of code instead of just for a single instruction, visit
  /// those uses.
  ///
  /// Returns false and early exits if the visitor \p func returns false.
  ///
  /// For an instantaneous borrow, such as apply, this visits no uses. For
  /// begin_apply it visits the end_apply uses. For borrow introducers, it
  /// visits the end of the introduced borrow scope.
  bool visitScopeEndingUses(function_ref<bool(Operand *)> func) const;

  /// Returns true for borrows that create a local borrow scope but have no
  /// scope-ending uses (presumably all paths are dead-end blocks). This does
  /// not include instantaneous borrows, which don't require explicit scope
  /// ending uses.
  ///
  /// FIXME: Borrow scopes should have scope-ending uses on all paths, even to
  /// dead end blocks. When the verifier enforces this, remove this check.
  bool hasEmptyRequiredEndingUses() const;

  /// Visit the scope ending operands of the extended scope, after transitively
  /// searching through reborrows. These uses might not be dominated by this
  /// BorrowingOperand.
  ///
  /// Returns false and early exits if the visitor \p func returns false.
  bool visitExtendedScopeEndingUses(function_ref<bool(Operand *)> func) const;

  /// Returns true if this borrow scope operand consumes guaranteed
  /// values and produces a new scope afterwards.
  ///
  /// TODO: tuple, struct, destructure_tuple, destructure_struct.
  bool isReborrow() const {
    switch (kind) {
    case BorrowingOperandKind::Invalid:
      llvm_unreachable("Using invalid case?!");
    case BorrowingOperandKind::BeginBorrow:
    case BorrowingOperandKind::BeginApply:
    case BorrowingOperandKind::Apply:
    case BorrowingOperandKind::TryApply:
    case BorrowingOperandKind::Yield:
    case BorrowingOperandKind::PartialApplyStack:
    case BorrowingOperandKind::BeginAsyncLet:
      return false;
    case BorrowingOperandKind::Branch:
      return true;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Return true if the user instruction defines a borrowed value that
  /// introduces a borrow scope and therefore may be reborrowed. This is true
  /// for both reborrows and nested borrows.
  ///
  /// Note that begin_apply does create a borrow scope, and may define
  /// guaranteed value within that scope. The difference is that those yielded
  /// values do not themselves introduce a borrow scope. In other words, they
  /// cannot be reborrowed.
  ///
  /// If true, getBorrowIntroducingUserResult() can be called to acquire the
  /// BorrowedValue that introduces a new borrow scope.
  bool hasBorrowIntroducingUser() const {
    // TODO: Can we derive this by running a borrow introducer check ourselves?
    switch (kind) {
    case BorrowingOperandKind::Invalid:
      llvm_unreachable("Using invalid case?!");
    case BorrowingOperandKind::BeginBorrow:
    case BorrowingOperandKind::Branch:
      return true;
    case BorrowingOperandKind::BeginApply:
    case BorrowingOperandKind::Apply:
    case BorrowingOperandKind::TryApply:
    case BorrowingOperandKind::Yield:
    case BorrowingOperandKind::PartialApplyStack:
    case BorrowingOperandKind::BeginAsyncLet:
      return false;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// If this operand's user has a borrowed value result return a valid
  /// BorrowedValue instance.
  BorrowedValue getBorrowIntroducingUserResult() const;

  /// Return the borrowing operand's value.
  SILValue getScopeIntroducingUserResult();

  /// Compute the implicit uses that this borrowing operand "injects" into the
  /// set of its operands uses.
  ///
  /// E.x.: end_apply uses.
  void getImplicitUses(SmallVectorImpl<Operand *> &foundUses) const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  BorrowingOperand(Operand *op, BorrowingOperandKind kind)
      : op(op), kind(kind) {}
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const BorrowingOperand &operand);

class BorrowedValueKind {
public:
  /// Enum we use for exhaustive pattern matching over borrow scope introducers.
  enum Kind : uint8_t {
    Invalid = 0,
    LoadBorrow,
    BeginBorrow,
    SILFunctionArgument,
    Phi,
  };

private:
  Kind value;

public:
  static BorrowedValueKind get(SILValue value) {
    if (value->getOwnershipKind() != OwnershipKind::Guaranteed)
      return Kind::Invalid;
    switch (value->getKind()) {
    default:
      return Kind::Invalid;
    case ValueKind::LoadBorrowInst:
      return Kind::LoadBorrow;
    case ValueKind::BeginBorrowInst:
      return Kind::BeginBorrow;
    case ValueKind::SILFunctionArgument:
      return Kind::SILFunctionArgument;
    case ValueKind::SILPhiArgument: {
      if (llvm::any_of(value->getParentBlock()->getPredecessorBlocks(),
                       [](SILBasicBlock *block) {
                         return !isa<BranchInst>(block->getTerminator());
                       })) {
        return Kind::Invalid;
      }
      if (cast<SILArgument>(value)->isGuaranteedForwarding()) {
        return Kind::Invalid;
      }
      return Kind::Phi;
    }
    }
  }

  BorrowedValueKind(Kind newValue) : value(newValue) {}

  operator Kind() const { return value; }

  /// Is this a borrow scope that begins and ends within the same function and
  /// thus is guaranteed to have an "end_scope" instruction.
  ///
  /// In contrast, borrow scopes that are non-local (e.x. from
  /// SILFunctionArguments) rely a construct like a SILFunction as the begin/end
  /// of the scope.
  bool isLocalScope() const {
    switch (value) {
    case BorrowedValueKind::Invalid:
      llvm_unreachable("Using invalid case?!");
    case BorrowedValueKind::BeginBorrow:
    case BorrowedValueKind::LoadBorrow:
    case BorrowedValueKind::Phi:
      return true;
    case BorrowedValueKind::SILFunctionArgument:
      return false;
    }
    llvm_unreachable("Covered switch isnt covered?!");
  }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, BorrowedValueKind kind);

struct InteriorPointerOperand;

/// A higher level construct for working with values that act as a "borrow
/// introducer" for a new borrow scope.
///
/// DISCUSSION: A "borrow introducer" is a SILValue that represents the
/// beginning of a borrow scope that the ownership verifier validates. The idea
/// is this API allows one to work in a generic way with all of the various
/// introducers.
///
/// Some examples of borrow introducers: guaranteed SILFunctionArgument,
/// LoadBorrow, BeginBorrow, guaranteed BeginApply results.
///
/// NOTE: It is assumed that if a borrow introducer is a value of a
/// SILInstruction with multiple results, that the all of the SILInstruction's
/// guaranteed results are borrow introducers. In practice this means that
/// borrow introducers can not have guaranteed results that are not creating a
/// new borrow scope. No such instructions exist today.
///
/// This provides utilities for visiting the end of the borrow scope introduced
/// by this value. The scope ending uses are always dominated by this value and
/// jointly post-dominate this value (see visitLocalScopeEndingUses()). The
/// extended scope, including reborrows has end points that are not dominated by
/// this value but still jointly post-dominate (see
/// visitExtendedScopeEndingUses()).
struct BorrowedValue {
  SILValue value;
  BorrowedValueKind kind = BorrowedValueKind::Invalid;

  BorrowedValue() = default;

  /// If value is a borrow introducer, create a valid BorrowedValue.
  explicit BorrowedValue(SILValue value) {
    kind = BorrowedValueKind::get(value);
    if (kind)
      this->value = value;
  }

  operator bool() const { return kind != BorrowedValueKind::Invalid && value; }

  /// If this value is introducing a local scope, gather all local end scope
  /// instructions and append them to \p scopeEndingInsts. Asserts if this is
  /// called with a scope that is not local.
  ///
  /// NOTE: To determine if a scope is a local scope, call
  /// BorrowScopeIntroducingValue::isLocalScope().
  void getLocalScopeEndingInstructions(
      SmallVectorImpl<SILInstruction *> &scopeEndingInsts) const;

  /// If this value is introducing a local scope, gather all local end scope
  /// instructions and pass them individually to visitor. Asserts if this is
  /// called with a scope that is not local.
  ///
  /// Returns false and early exist if \p visitor returns false.
  ///
  /// The intention is that this method can be used instead of
  /// BorrowScopeIntroducingValue::getLocalScopeEndingUses() to avoid
  /// introducing an intermediate array when one needs to transform the
  /// instructions before storing them.
  ///
  /// NOTE: To determine if a scope is a local scope, call
  /// BorrowScopeIntroducingValue::isLocalScope().
  bool visitLocalScopeEndingUses(function_ref<bool(Operand *)> visitor) const;

  bool isLocalScope() const { return kind.isLocalScope(); }

  /// Add this scope's live blocks into the PrunedLiveness result. This
  /// includes reborrow scopes that are reachable from this borrow scope but not
  /// necessarilly dominated by the borrow scope.
  void computeTransitiveLiveness(MultiDefPrunedLiveness &liveness) const;

  /// Returns true if \p uses are completely within this borrow introducer's
  /// local scope.
  ///
  /// Precondition: \p uses are dominated by the local borrow introducer.
  ///
  /// This ignores reborrows. The assumption is that, since \p uses are
  /// dominated by this local scope, checking the extended borrow scope should
  /// not be necessary to determine they are within the scope.
  ///
  /// \p deadEndBlocks is optional during transition. It will be completely
  /// removed in an upcoming commit.
  bool areUsesWithinExtendedScope(ArrayRef<Operand *> uses,
                                  DeadEndBlocks *deadEndBlocks) const;

  /// Given a local borrow scope introducer, visit all non-forwarding consuming
  /// users. This means that this looks through guaranteed block arguments. \p
  /// visitor is *not* called on Reborrows, only on final scope ending uses.
  bool
  visitExtendedScopeEndingUses(function_ref<bool(Operand *)> visitor) const;

  /// Visit all lifetime ending operands of the entire borrow scope including
  /// reborrows
  bool
  visitTransitiveLifetimeEndingUses(function_ref<bool(Operand *)> func) const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  /// Visit each of the interior pointer uses of this underlying borrow
  /// introduced value without looking through nested borrows or reborrows.
  ///
  /// These object -> address projections and any transitive address uses must
  /// be treated as liveness requiring uses of the guaranteed value and we can
  /// not shrink the scope beyond that point. Returns true if we were able to
  /// understand all uses and thus guarantee we found all interior pointer
  /// uses. Returns false otherwise.
  bool visitInteriorPointerOperands(
      function_ref<void(InteriorPointerOperand)> func) const {
    return visitInteriorPointerOperandHelper(
        func, InteriorPointerOperandVisitorKind::NoNestedNoReborrows);
  }

  /// Visit each of the interior pointer uses of this underlying borrow
  /// introduced value looking through nested borrow scopes but not reborrows.
  bool visitNestedInteriorPointerOperands(
      function_ref<void(InteriorPointerOperand)> func) const {
    return visitInteriorPointerOperandHelper(
        func, InteriorPointerOperandVisitorKind::YesNestedNoReborrows);
  }

  /// Visit each of the interior pointer uses of this underlying borrow
  /// introduced value looking through nested borrow scopes and reborrows.
  bool visitExtendedInteriorPointerOperands(
      function_ref<void(InteriorPointerOperand)> func) const {
    return visitInteriorPointerOperandHelper(
        func, InteriorPointerOperandVisitorKind::YesNestedYesReborrows);
  }

  bool hasReborrow() const {
    for (auto *op : value->getUses()) {
      if (op->getOperandOwnership() == OperandOwnership::Reborrow)
        return true;
    }
    return false;
  }

  /// Visit all immediate uses of this borrowed value and if any of them are
  /// reborrows, place them in BorrowingOperand form into \p
  /// foundReborrows. Returns true if we appended any such reborrows to
  /// foundReborrows... false otherwise.
  bool gatherReborrows(SmallVectorImpl<std::pair<SILBasicBlock *, unsigned>>
                           &foundReborrows) const {
    bool foundAnyReborrows = false;
    for (auto *op : value->getUses()) {
      if (op->getOperandOwnership() == OperandOwnership::Reborrow) {
        foundReborrows.push_back(
            {value->getParentBlock(), op->getOperandNumber()});
        foundAnyReborrows = true;
      }
    }
    return foundAnyReborrows;
  }

  // Helpers to allow a BorrowedValue to easily be used as a SILValue
  // programatically.
  SILValue operator->() { return value; }
  SILValue operator->() const { return value; }
  SILValue operator*() { return value; }
  SILValue operator*() const { return value; }

private:
  enum class InteriorPointerOperandVisitorKind {
    NoNestedNoReborrows,
    YesNestedNoReborrows,
    YesNestedYesReborrows,
  };
  bool visitInteriorPointerOperandHelper(
      function_ref<void(InteriorPointerOperand)> func,
      InteriorPointerOperandVisitorKind kind) const;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const BorrowedValue &value);

/// Look up the def-use graph starting at use \p inputOperand, recording any
/// "borrow" introducing values that we find into \p out. If at any point, we
/// find a point in the chain we do not understand, we bail and return false. If
/// we are able to understand all of the def-use graph, we know that we have
/// found all of the borrow introducing values, we return true.
///
/// NOTE: This may return multiple borrow introducing values in cases where
/// there are phi-like nodes in the IR like any true phi block arguments or
/// aggregate literal instructions (struct, tuple, enum, etc.).
bool getAllBorrowIntroducingValues(SILValue value,
                                   SmallVectorImpl<BorrowedValue> &out);

/// Look up through the def-use chain of \p inputValue, looking for an initial
/// "borrow" introducing value. If at any point, we find two introducers or we
/// find a point in the chain we do not understand, we bail and return false. If
/// we are able to understand all of the def-use graph and only find a single
/// introducer, then we return a .some(BorrowScopeIntroducingValue).
BorrowedValue getSingleBorrowIntroducingValue(SILValue inputValue);

/// The algorithm that is used to determine what the verifier will consider to
/// be transitive uses of the given address. Used to implement \see
/// findTransitiveUses.
///
/// NOTE: Rather than return load_borrow as uses, this returns all of the
/// transitive uses of the load_borrow as uses. This is important when working
/// with this in OSSA. If one wishes to avoid this behavior, call find
/// transitive uses for address with ones own visitor.
inline AddressUseKind findTransitiveUsesForAddress(
    SILValue address, SmallVectorImpl<Operand *> *foundUses = nullptr,
    std::function<void(Operand *)> *onError = nullptr) {
  // This is a version of TransitiveUseVisitor that visits inner transitive
  // guaranteed uses to determine if a load_borrow is an escape in OSSA. This
  // is OSSA specific behavior and we should probably create a different API
  // for that. But for now, this lets this APIs users stay the same.
  struct BasicTransitiveAddressVisitor final : TransitiveAddressWalker {
    SmallVectorImpl<Operand *> *foundUses;
    std::function<void(Operand *)> *onErrorFunc;

    BasicTransitiveAddressVisitor(SmallVectorImpl<Operand *> *foundUses,
                                  std::function<void(Operand *)> *onErrorFunc)
        : foundUses(foundUses), onErrorFunc(onErrorFunc) {}

    bool visitUse(Operand *use) override {
      if (!foundUses)
        return true;

      if (auto *lbi = dyn_cast<LoadBorrowInst>(use->getUser())) {
        if (!findInnerTransitiveGuaranteedUses(lbi, foundUses)) {
          meet(AddressUseKind::PointerEscape);
        }
        return true;
      }

      // If we have a begin_apply, we want to use the token results if we have
      // any. If it doesn't have any token results, we just make our use the
      // begin_apply use itself below.
      if (auto *bai = dyn_cast<BeginApplyInst>(use->getUser())) {
        if (!bai->getTokenResult()->use_empty()) {
          for (auto *use : bai->getTokenResult()->getUses()) {
            foundUses->push_back(use);
          }
          return true;
        }
      }

      foundUses->push_back(use);
      return true;
    }

    void onError(Operand *use) override {
      if (onErrorFunc)
        (*onErrorFunc)(use);
    }
  };

  BasicTransitiveAddressVisitor visitor(foundUses, onError);
  return std::move(visitor).walk(address);
}

class InteriorPointerOperandKind {
public:
  enum Kind : uint8_t {
    Invalid = 0,
    RefElementAddr,
    RefTailAddr,
    OpenExistentialBox,
    ProjectBox,
    StoreBorrow,
  };

private:
  Kind value;

public:
  InteriorPointerOperandKind(Kind newValue) : value(newValue) {}

  operator Kind() const {
    return value;
  }

  explicit operator bool() const { return isValid(); }

  bool isValid() const { return value != Kind::Invalid; }

  static InteriorPointerOperandKind get(Operand *use) {
    switch (use->getUser()->getKind()) {
    default:
      return Kind::Invalid;
    case SILInstructionKind::RefElementAddrInst:
      return Kind::RefElementAddr;
    case SILInstructionKind::RefTailAddrInst:
      return Kind::RefTailAddr;
    case SILInstructionKind::OpenExistentialBoxInst:
      return Kind::OpenExistentialBox;
    case SILInstructionKind::ProjectBoxInst:
      return Kind::ProjectBox;
    case SILInstructionKind::StoreBorrowInst:
      return Kind::StoreBorrow;
    }
  }

  /// Given a \p value that is a result of an instruction with an interior
  /// pointer operand, return the interior pointer operand kind that would be
  /// appropriate for that operand.
  static InteriorPointerOperandKind inferFromResult(SILValue value) {
    switch (value->getKind()) {
    default:
      return Kind::Invalid;
    case ValueKind::RefElementAddrInst:
      return Kind::RefElementAddr;
    case ValueKind::RefTailAddrInst:
      return Kind::RefTailAddr;
    case ValueKind::OpenExistentialBoxInst:
      return Kind::OpenExistentialBox;
    case ValueKind::ProjectBoxInst:
      return Kind::ProjectBox;
    case ValueKind::StoreBorrowInst:
      return Kind::StoreBorrow;
    }
  }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP;
};

/// A mixed object->address projection that projects a memory location out of an
/// object with guaranteed ownership. All transitive address uses of the
/// interior pointer must be within the lifetime of the guaranteed lifetime. As
/// such, these must be treated as implicit uses of the parent guaranteed value.
///
/// FIXME: This should probably also handle project_box once we support
/// borrowing a box.
struct InteriorPointerOperand {
  Operand *operand;
  InteriorPointerOperandKind kind;

  InteriorPointerOperand()
      : operand(nullptr), kind(InteriorPointerOperandKind::Invalid) {}

  InteriorPointerOperand(Operand *op)
      : operand(op), kind(InteriorPointerOperandKind::get(op)) {
  }

  operator bool() const {
    return kind != InteriorPointerOperandKind::Invalid && operand;
  }

  SILInstruction *getUser() const { return operand->getUser(); }

  /// If \p op has a user that is an interior pointer, return a valid
  /// value. Otherwise, return None.
  static InteriorPointerOperand get(Operand *op) {
    auto kind = InteriorPointerOperandKind::get(op);
    if (!kind)
      return {};
    return InteriorPointerOperand(op, kind);
  }

  /// If \p val is a result of an instruction that is an interior pointer,
  /// return an interior pointer operand based off of the base value operand of
  /// the instruction.
  static InteriorPointerOperand inferFromResult(SILValue resultValue) {
    auto kind = InteriorPointerOperandKind::inferFromResult(resultValue);

    // NOTE: We use an exhaustive switch here to allow for further interior
    // pointer operand having instructions to make the interior pointer
    // operand's argument index arbitrary.
    switch (kind) {
    case InteriorPointerOperandKind::Invalid:
      // We do not have a valid instruction, so return None.
      return {};
    case InteriorPointerOperandKind::RefElementAddr:
    case InteriorPointerOperandKind::RefTailAddr:
    case InteriorPointerOperandKind::OpenExistentialBox:
    case InteriorPointerOperandKind::ProjectBox:
    case InteriorPointerOperandKind::StoreBorrow: {
      // Ok, we have a valid instruction. Return the relevant operand.
      auto *op =
          &cast<SingleValueInstruction>(resultValue)->getAllOperands()[0];
      return InteriorPointerOperand(op, kind);
    }
    }
    llvm_unreachable("covered switch");
  }

  /// Return the end scope of all borrow introducers of the parent value of this
  /// projection. Returns true if we were able to find all borrow introducing
  /// values.
  bool visitBaseValueScopeEndingUses(function_ref<bool(Operand *)> func) const {
    SmallVector<BorrowedValue, 4> introducers;
    if (!getAllBorrowIntroducingValues(operand->get(), introducers))
      return false;
    for (const auto &introducer : introducers) {
      if (!introducer.isLocalScope())
        continue;
      if (!introducer.visitLocalScopeEndingUses(func))
        return false;
    }
    return true;
  }

  /// Return the base BorrowedValue of the incoming value's operand.
  BorrowedValue getSingleBorrowedValue() const {
    return getSingleBorrowIntroducingValue(operand->get());
  }

  SILValue getProjectedAddress() const {
    switch (kind) {
    case InteriorPointerOperandKind::Invalid:
      llvm_unreachable("Calling method on invalid?!");
    case InteriorPointerOperandKind::RefElementAddr:
      return cast<RefElementAddrInst>(operand->getUser());
    case InteriorPointerOperandKind::RefTailAddr:
      return cast<RefTailAddrInst>(operand->getUser());
    case InteriorPointerOperandKind::OpenExistentialBox:
      return cast<OpenExistentialBoxInst>(operand->getUser());
    case InteriorPointerOperandKind::ProjectBox:
      return cast<ProjectBoxInst>(operand->getUser());
    case InteriorPointerOperandKind::StoreBorrow:
      return cast<StoreBorrowInst>(operand->getUser());
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Transitively compute the list of leaf uses that this interior pointer
  /// operand puts on its parent guaranteed value.
  ///
  /// If \p foundUses is nullptr, this simply returns true if no PointerEscapes
  /// were found.
  ///
  /// Example: Uses of a ref_element_addr can not occur outside of the lifetime
  /// of the instruction's operand. The uses of that address act as liveness
  /// requirements to ensure that the underlying class is alive at all use
  /// points.
  AddressUseKind
  findTransitiveUses(SmallVectorImpl<Operand *> *foundUses = nullptr,
                     std::function<void(Operand *)> *onError = nullptr) {
    return findTransitiveUsesForAddress(getProjectedAddress(), foundUses,
                                        onError);
  }

  Operand *operator->() { return operand; }
  const Operand *operator->() const { return operand; }
  Operand *operator*() { return operand; }
  const Operand *operator*() const { return operand; }

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  InteriorPointerOperand(Operand *op, InteriorPointerOperandKind kind)
      : operand(op), kind(kind) {}
};

/// Utility to check if an address may originate from an OSSA value. If so,
/// then uses of the address cannot be replaced without ensuring that they are
/// also within the same owned lifetime or borrow scope.
///
/// If hasOwnership() is false, then there is no enclosing lifetime or borrow
/// scope and interiorPointerOp is irrelevant.
///
/// If hasOwnership() is true, then interiorPointerOp refers to the operand that
/// converts a non-address value into the address from which the constructor's
/// address is derived. If the best-effort to find an InteriorPointerOperand
/// fails, then interiorPointerOp remains invalid, and clients must be
/// conservative.
///
/// TODO: Handle implicit borrow scopes once they are legal in SIL. The operand
/// of the base will be owned but mayBeBorrowed will remain true.
struct AddressOwnership {
  AccessBase base;

  AddressOwnership() = default;

  AddressOwnership(SILValue address) : base(AccessBase::compute(address)) {
    assert(address->getType().isAddress());
  }

  AddressOwnership(AccessBase base) : base(base) {}

  operator bool() const { return bool(base); }

  bool operator==(const AddressOwnership &other) const {
    return base.hasIdenticalAccessInfo(other.base);
  }

  bool operator!=(const AddressOwnership &other) const {
    return !(*this == other);
  }

  /// Return true if this address may be derived from a value with a local OSSA
  /// lifetime or borrow scope.
  bool hasLocalOwnershipLifetime() const {
    return base.hasLocalOwnershipLifetime();
  }

  /// Return the OSSA value from which this address is derived. This may be
  /// invalid even if hasOSSALifetime() is true in cases where the
  /// InteriorPointerOperand is unrecognized.
  SILValue getOwnershipReferenceRoot() const {
    if (base.isReference())
      return base.getOwnershipReferenceRoot();

    return SILValue();
  }

  /// Transitively compute uses of this base address.
  AddressUseKind findTransitiveUses(SmallVectorImpl<Operand *> &foundUses) {
    return findTransitiveUsesForAddress(base.getBaseAddress(), &foundUses);
  }

  /// Return true of all \p uses occur before the end of the address' lifetime
  /// or borrow scope.
  ///
  /// Precondition: all \p uses are dominated by the beginning of the address'
  /// lifetime or borrow scope.
  bool areUsesWithinLifetime(ArrayRef<Operand *> uses,
                             DeadEndBlocks &deadEndBlocks) const;
};

class OwnedValueIntroducerKind {
public:
  enum Kind : uint8_t {
    /// None value.
    Invalid = 0,

    /// An owned value that is a result of an Apply.
    Apply,

    /// An owned value returned as a result of applying a begin_apply.
    BeginApply,

    /// An owned value that is an argument that is in one of the successor
    /// blocks of a try_apply. This represents in a sense the try applies
    /// result.
    TryApply,

    /// An owned value produced as a result of performing a copy_value on some
    /// other value.
    Copy,

    /// An owned value produced as a result of performing a load [copy] on a
    /// memory location.
    LoadCopy,

    /// An owned value produced as a result of performing a load [take] from a
    /// memory location.
    LoadTake,

    /// An owned value produced by moving from another owned value.
    Move,

    /// An owned value that is a result of a true phi argument.
    ///
    /// A true phi argument here is defined as an SIL phi argument that only has
    /// branch predecessors.
    Phi,

    /// An owned value that is from a struct that has multiple operands that are
    /// owned.
    Struct,

    /// An owned value that is from a tuple that has multiple operands that are
    /// owned.
    Tuple,

    /// An owned value that is a function argument.
    FunctionArgument,

    /// An owned value that is a new partial_apply that has been formed.
    PartialApplyInit,

    /// An owned value from the formation of a new alloc_box.
    AllocBoxInit,

    /// An owned value from the formation of a new alloc_ref.
    AllocRefInit,
  };

private:
  Kind value;

public:
  static OwnedValueIntroducerKind get(SILValue value) {
    if (value->getOwnershipKind() != OwnershipKind::Owned)
      return Kind::Invalid;

    switch (value->getKind()) {
    default:
      return Kind::Invalid;
      ;
    case ValueKind::ApplyInst:
      return Kind::Apply;
    case ValueKind::MultipleValueInstructionResult:
      if (isaResultOf<BeginApplyInst>(value))
        return Kind::BeginApply;
      return Kind::Invalid;
    case ValueKind::StructInst:
      return Kind::Struct;
    case ValueKind::TupleInst:
      return Kind::Tuple;
    case ValueKind::SILPhiArgument: {
      auto *phiArg = cast<SILPhiArgument>(value);
      if (dyn_cast_or_null<TryApplyInst>(phiArg->getSingleTerminator())) {
        return Kind::TryApply;
      }
      if (llvm::all_of(phiArg->getParent()->getPredecessorBlocks(),
                       [](SILBasicBlock *block) {
                         return isa<BranchInst>(block->getTerminator());
                       })) {
        return Kind::Phi;
      }
      return Kind::Invalid;
    }
    case ValueKind::SILFunctionArgument:
      return Kind::FunctionArgument;
    case ValueKind::CopyValueInst:
      return Kind::Copy;
    case ValueKind::LoadInst: {
      auto qual = cast<LoadInst>(value)->getOwnershipQualifier();
      if (qual == LoadOwnershipQualifier::Take)
        return Kind::LoadTake;
      if (qual == LoadOwnershipQualifier::Copy)
        return Kind::LoadCopy;
      return Kind::Invalid;
    }
    case ValueKind::MoveValueInst:
      return Kind::Move;
    case ValueKind::PartialApplyInst:
      return Kind::PartialApplyInit;
    case ValueKind::AllocBoxInst:
      return Kind::AllocBoxInit;
    case ValueKind::AllocRefInst:
      return Kind::AllocRefInit;
    }
    llvm_unreachable("Default should have caught this");
  }

  OwnedValueIntroducerKind(Kind newValue) : value(newValue) {}

  operator Kind() const { return value; }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              OwnedValueIntroducerKind kind);

/// A higher level construct for working with values that introduce a new
/// "owned" value.
///
/// An owned "introducer" is a value that signals in a SIL program the begin of
/// a new semantic @owned ownership construct that is live without respect to
/// any other values in the function. This introducer value is then either used
/// directly, forwarded then used, and then finally destroyed.
///
/// NOTE: Previous incarnations of this concept used terms like "RC-identity".
struct OwnedValueIntroducer {
  /// The actual underlying value that introduces the new owned value.
  SILValue value;

  /// The kind of "introducer" that we use to classify any of various possible
  /// underlying introducing values.
  OwnedValueIntroducerKind kind;

  OwnedValueIntroducer()
      : value(nullptr), kind(OwnedValueIntroducerKind::Invalid) {}

  /// If a value is an owned value introducer we can recognize, return
  /// .some(OwnedValueIntroducer). Otherwise, return None.
  static OwnedValueIntroducer get(SILValue value) {
    auto kind = OwnedValueIntroducerKind::get(value);
    if (!kind)
      return {nullptr, kind};
    return {value, kind};
  }

  operator bool() const {
    return kind != OwnedValueIntroducerKind::Invalid && bool(value);
  }

  /// Returns true if this owned introducer is able to be converted into a
  /// guaranteed form if none of its direct uses are consuming uses (looking
  /// through forwarding uses).
  ///
  /// NOTE: Since the direct uses must be non-consuming, this means that any
  /// "ownership phis" (e.x. branch, struct) must return false here since we can
  /// not analyze them without analyzing their operands/incoming values.
  bool isConvertableToGuaranteed() const {
    switch (kind) {
    case OwnedValueIntroducerKind::Invalid:
      llvm_unreachable("Using invalid case?!");
    case OwnedValueIntroducerKind::Copy:
    case OwnedValueIntroducerKind::LoadCopy:
      return true;
    case OwnedValueIntroducerKind::Apply:
    case OwnedValueIntroducerKind::BeginApply:
    case OwnedValueIntroducerKind::TryApply:
    case OwnedValueIntroducerKind::LoadTake:
    case OwnedValueIntroducerKind::Move:
    case OwnedValueIntroducerKind::Phi:
    case OwnedValueIntroducerKind::Struct:
    case OwnedValueIntroducerKind::Tuple:
    case OwnedValueIntroducerKind::FunctionArgument:
    case OwnedValueIntroducerKind::PartialApplyInit:
    case OwnedValueIntroducerKind::AllocBoxInit:
    case OwnedValueIntroducerKind::AllocRefInit:
      return false;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Returns true if this introducer when converted to guaranteed is expected
  /// to have guaranteed operands that are consumed by the instruction.
  ///
  /// E.x.: phi, struct.
  bool hasConsumingGuaranteedOperands() const {
    switch (kind) {
    case OwnedValueIntroducerKind::Invalid:
      llvm_unreachable("Using invalid case?!");
    case OwnedValueIntroducerKind::Phi:
      return true;
    case OwnedValueIntroducerKind::Struct:
    case OwnedValueIntroducerKind::Tuple:
    case OwnedValueIntroducerKind::Copy:
    case OwnedValueIntroducerKind::LoadCopy:
    case OwnedValueIntroducerKind::Apply:
    case OwnedValueIntroducerKind::BeginApply:
    case OwnedValueIntroducerKind::TryApply:
    case OwnedValueIntroducerKind::LoadTake:
    case OwnedValueIntroducerKind::Move:
    case OwnedValueIntroducerKind::FunctionArgument:
    case OwnedValueIntroducerKind::PartialApplyInit:
    case OwnedValueIntroducerKind::AllocBoxInit:
    case OwnedValueIntroducerKind::AllocRefInit:
      return false;
    }
    llvm_unreachable("covered switch");
  }

  bool operator==(const OwnedValueIntroducer &other) const {
    return value == other.value;
  }

  bool operator!=(const OwnedValueIntroducer &other) const {
    return !(*this == other);
  }

  bool operator<(const OwnedValueIntroducer &other) const {
    return value < other.value;
  }

private:
  OwnedValueIntroducer(SILValue value, OwnedValueIntroducerKind kind)
      : value(value), kind(kind) {}
};

/// Look up the def-use graph starting at use \p inputOperand, recording any
/// values that act as "owned" introducers.
///
/// NOTE: This may return multiple owned introducers in cases where there are
/// phi-like nodes in the IR like any true phi block arguments or aggregate
/// literal instructions (struct, tuple, enum, etc.).
bool getAllOwnedValueIntroducers(SILValue value,
                                 SmallVectorImpl<OwnedValueIntroducer> &out);

OwnedValueIntroducer getSingleOwnedValueIntroducer(SILValue value);

using BaseValueSet = SmallPtrSet<SILValue, 8>;

/// Starting from \p borrowInst, find all reborrows along with their base
/// values, and run the visitor function \p visitReborrowPhiBaseValuePair on
/// them.
void visitExtendedReborrowPhiBaseValuePairs(
    BeginBorrowInst *borrowInst, function_ref<void(SILPhiArgument *, SILValue)>
                                     visitReborrowPhiBaseValuePair);

/// Starting from \p borrow, find all GuaranteedForwardingPhi uses along with
/// their base values, and run the visitor function \p
/// visitGuaranteedForwardingPhiBaseValuePair on them.
void visitExtendedGuaranteedForwardingPhiBaseValuePairs(
    BorrowedValue borrow, function_ref<void(SILPhiArgument *, SILValue)>
                              visitGuaranteedForwardingPhiBaseValuePair);

/// If \p value is a guaranteed non-phi value forwarded from it's instruction's
/// operands, visit each forwarded operand.
///
/// Returns true if \p visitOperand was called (for convenience).
///
/// Precondition: \p value is not a phi. The client must handle phis first by
/// checking if they are reborrows (using BorrowedValue). Reborrows have no
/// forwarded operands. Guaranteed forwarding need to be handled by recursing
/// through the phi operands.
bool visitForwardedGuaranteedOperands(
  SILValue value, function_ref<void(Operand *)> visitOperand);


/// Return true of the lifetime of \p innerPhiVal depends on \p outerPhiVal.
bool isInnerAdjacentPhi(SILArgument *innerPhiVal, SILArgument *outerPhiVal);

/// Visit the phis in the same block as \p phi whose lifetime depends on \p phi.
///
/// If the visitor returns false, stops visiting and returns false.  Otherwise,
/// returns true.
bool visitInnerAdjacentPhis(SILArgument *phi,
                            function_ref<bool(SILArgument *)> visitor);

/// Visit each definition of a scope that immediately encloses a guaranteed
/// value. The guaranteed value effectively keeps these scopes alive.
///
/// This means something different depepending on whether \p value is itself a
/// borrow introducer vs. a forwarded guaranteed value. If \p value is an
/// introducer, then this discovers the enclosing borrow scope and visits all
/// introducers of that scope. If \p value is a forwarded value, then this
/// visits the introducers of the current borrow scope.
bool visitEnclosingDefs(SILValue value, function_ref<bool(SILValue)> visitor);

/// Visit the values that introduce the borrow scopes that includes \p
/// value. If value is owned, or introduces a borrow scope, then this only
/// visits \p value.
///
/// Returns false if the visitor returned false and exited early.
bool visitBorrowIntroducers(SILValue value,
                            function_ref<bool(SILValue)> visitor);

/// Given a begin of a borrow scope, visit all end_borrow users of the borrow or
/// its reborrows.
void visitTransitiveEndBorrows(
    SILValue value,
    function_ref<void(EndBorrowInst *)> visitEndBorrow);

/// Whether the specified lexical begin_borrow instruction is nested.
///
/// A begin_borrow [lexical] is nested if the borrowed value's lifetime is
/// guaranteed by another lexical scope.  That happens if:
/// - the value is a guaranteed argument to the function
/// - the value is itself a begin_borrow [lexical]
bool isNestedLexicalBeginBorrow(BeginBorrowInst *bbi);

/// Whether specified move_value is redundant.
///
/// A move_value is redundant if it doesn't
/// - alter constraints (lexicality, ownership)
/// - enable optimizations (e.g, separate smaller scopes within which a value
///   escapes)
///
/// For example, if the lifetimes that a move_value separates both have the
/// same characteristics with respect to
/// - ownership
/// - lexicality
/// - escaping
/// then the move_value is redundant.
bool isRedundantMoveValue(MoveValueInst *mvi);

} // namespace swift

#endif
