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
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILBasicBlock;
class SILInstruction;
class SILModule;
class SILValue;
class DeadEndBlocks;

/// Returns true if v is an address or trivial.
bool isValueAddressOrTrivial(SILValue v);

/// These operations forward both owned and guaranteed ownership.
bool isOwnershipForwardingValueKind(SILNodeKind kind);

/// Is this an instruction that can forward both owned and guaranteed ownership
/// kinds.
bool isOwnershipForwardingInst(SILInstruction *i);

/// Is this an instruction that can forward guaranteed ownership.
bool isGuaranteedForwardingInst(SILInstruction *i);

/// These operations forward guaranteed ownership, but don't necessarily forward
/// owned values.
bool isGuaranteedForwardingValueKind(SILNodeKind kind);

/// Is this a value that is the result of an operation that forwards owned
/// ownership.
bool isGuaranteedForwardingValue(SILValue value);

/// Is this a node kind that can forward owned ownership, but may not be able to
/// forward guaranteed ownership.
bool isOwnedForwardingValueKind(SILNodeKind kind);

/// Does this SILInstruction 'forward' owned ownership, but may not be able to
/// forward guaranteed ownership.
bool isOwnedForwardingInstruction(SILInstruction *inst);

/// Does this value 'forward' owned ownership, but may not be able to forward
/// guaranteed ownership.
///
/// This will be either a multiple value instruction resuilt, a single value
/// instruction that forwards or an argument that forwards the ownership from a
/// previous terminator.
bool isOwnedForwardingValue(SILValue value);

class BorrowingOperandKind {
public:
  enum Kind : uint8_t {
    BeginBorrow,
    BeginApply,
    Branch,
  };

private:
  Kind value;

public:
  BorrowingOperandKind(Kind newValue) : value(newValue) {}

  operator Kind() const { return value; }

  static Optional<BorrowingOperandKind> get(SILInstructionKind kind) {
    switch (kind) {
    default:
      return None;
    case SILInstructionKind::BeginBorrowInst:
      return BorrowingOperandKind(BeginBorrow);
    case SILInstructionKind::BeginApplyInst:
      return BorrowingOperandKind(BeginApply);
    case SILInstructionKind::BranchInst:
      return BorrowingOperandKind(Branch);
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
/// NOTE: We do not require that the guaranteed scope be represented by a
/// guaranteed value in the same function: see begin_apply. In such cases, we
/// require instead an end_* instruction to mark the end of the scope's region.
struct BorrowingOperand {
  BorrowingOperandKind kind;
  Operand *op;

  BorrowingOperand(Operand *op)
      : kind(*BorrowingOperandKind::get(op->getUser()->getKind())), op(op) {}
  BorrowingOperand(const BorrowingOperand &other)
      : kind(other.kind), op(other.op) {}
  BorrowingOperand &operator=(const BorrowingOperand &other) {
    kind = other.kind;
    op = other.op;
    return *this;
  }

  /// If value is a borrow introducer return it after doing some checks.
  static Optional<BorrowingOperand> get(Operand *op) {
    auto *user = op->getUser();
    auto kind = BorrowingOperandKind::get(user->getKind());
    if (!kind)
      return None;
    return BorrowingOperand(*kind, op);
  }

  void visitEndScopeInstructions(function_ref<void(Operand *)> func) const;

  /// Returns true if this borrow scope operand consumes guaranteed
  /// values and produces a new scope afterwards.
  bool consumesGuaranteedValues() const {
    switch (kind) {
    case BorrowingOperandKind::BeginBorrow:
    case BorrowingOperandKind::BeginApply:
      return false;
    case BorrowingOperandKind::Branch:
      return true;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Is this a borrow scope operand that can open new borrow scopes
  /// for owned values.
  bool canAcceptOwnedValues() const {
    switch (kind) {
    case BorrowingOperandKind::BeginBorrow:
    case BorrowingOperandKind::BeginApply:
      return true;
    case BorrowingOperandKind::Branch:
      return false;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Is the result of this instruction also a borrow introducer?
  ///
  /// TODO: This needs a better name.
  bool areAnyUserResultsBorrowIntroducers() const {
    // TODO: Can we derive this by running a borrow introducer check ourselves?
    switch (kind) {
    case BorrowingOperandKind::BeginBorrow:
    case BorrowingOperandKind::Branch:
      return true;
    case BorrowingOperandKind::BeginApply:
      return false;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Visit all of the results of the operand's user instruction that are
  /// consuming uses.
  void visitUserResultConsumingUses(function_ref<void(Operand *)> visitor) const;

  /// Visit all of the "results" of the user of this operand that are borrow
  /// scope introducers for the specific scope that this borrow scope operand
  /// summarizes.
  void
  visitBorrowIntroducingUserResults(function_ref<void(BorrowedValue)> visitor) const;

  /// Passes to visitor all of the consuming uses of this use's using
  /// instruction.
  ///
  /// This enables one to walk the def-use chain of guaranteed phis for a single
  /// guaranteed scope by using a worklist and checking if any of the operands
  /// are BorrowScopeOperands.
  void visitConsumingUsesOfBorrowIntroducingUserResults(
      function_ref<void(Operand *)> visitor) const;

  /// Compute the implicit uses that this borrowing operand "injects" into the
  /// set of its operands uses.
  ///
  /// E.x.: end_apply uses.
  ///
  /// \p errorFunction a callback that if non-null is passed an operand that
  /// triggers a mal-formed SIL error. This is just needed for the ownership
  /// verifier to emit good output.
  bool getImplicitUses(
      SmallVectorImpl<Operand *> &foundUses,
      std::function<void(Operand *)> *errorFunction = nullptr) const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  BorrowingOperand(BorrowingOperandKind kind, Operand *op)
      : kind(kind), op(op) {}
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const BorrowingOperand &operand);

class BorrowedValueKind {
public:
  /// Enum we use for exhaustive pattern matching over borrow scope introducers.
  enum Kind : uint8_t {
    LoadBorrow,
    BeginBorrow,
    SILFunctionArgument,
    Phi,
  };

private:
  Kind value;

public:
  static Optional<BorrowedValueKind> get(SILValue value) {
    if (value.getOwnershipKind() != ValueOwnershipKind::Guaranteed)
      return None;
    switch (value->getKind()) {
    default:
      return None;
    case ValueKind::LoadBorrowInst:
      return BorrowedValueKind(LoadBorrow);
    case ValueKind::BeginBorrowInst:
      return BorrowedValueKind(BeginBorrow);
    case ValueKind::SILFunctionArgument:
      return BorrowedValueKind(SILFunctionArgument);
    case ValueKind::SILPhiArgument: {
      if (llvm::any_of(value->getParentBlock()->getPredecessorBlocks(),
                       [](SILBasicBlock *block) {
                         return !isa<BranchInst>(block->getTerminator());
                       })) {
        return None;
      }
      return BorrowedValueKind(Phi);
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
struct BorrowedValue {
  BorrowedValueKind kind;
  SILValue value;

  /// If value is a borrow introducer return it after doing some checks.
  ///
  /// This is the only way to construct a BorrowScopeIntroducingValue. We make
  /// the primary constructor private for this reason.
  static Optional<BorrowedValue> get(SILValue value) {
    auto kind = BorrowedValueKind::get(value);
    if (!kind)
      return None;
    return BorrowedValue(*kind, value);
  }

  /// If this value is introducing a local scope, gather all local end scope
  /// instructions and append them to \p scopeEndingInsts. Asserts if this is
  /// called with a scope that is not local.
  ///
  /// NOTE: To determine if a scope is a local scope, call
  /// BorrowScopeIntoducingValue::isLocalScope().
  void getLocalScopeEndingInstructions(
      SmallVectorImpl<SILInstruction *> &scopeEndingInsts) const;

  /// If this value is introducing a local scope, gather all local end scope
  /// instructions and pass them individually to visitor. Asserts if this is
  /// called with a scope that is not local.
  ///
  /// The intention is that this method can be used instead of
  /// BorrowScopeIntroducingValue::getLocalScopeEndingUses() to avoid
  /// introducing an intermediate array when one needs to transform the
  /// instructions before storing them.
  ///
  /// NOTE: To determine if a scope is a local scope, call
  /// BorrowScopeIntoducingValue::isLocalScope().
  void visitLocalScopeEndingUses(function_ref<void(Operand *)> visitor) const;

  bool isLocalScope() const { return kind.isLocalScope(); }

  /// Returns true if the passed in set of uses is completely within
  /// the lifetime of this borrow introducer.
  ///
  /// NOTE: Scratch space is used internally to this method to store the end
  /// borrow scopes if needed.
  bool areUsesWithinScope(ArrayRef<Operand *> instructions,
                          SmallVectorImpl<Operand *> &scratchSpace,
                          SmallPtrSetImpl<SILBasicBlock *> &visitedBlocks,
                          DeadEndBlocks &deadEndBlocks) const;

  /// Given a local borrow scope introducer, visit all non-forwarding consuming
  /// users. This means that this looks through guaranteed block arguments.
  bool visitLocalScopeTransitiveEndingUses(
      function_ref<void(Operand *)> visitor) const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  /// Visit each of the interior pointer uses of this underlying borrow
  /// introduced value. These object -> address projections and any transitive
  /// address uses must be treated as liveness requiring uses of the guaranteed
  /// value and we can not shrink the scope beyond that point. Returns true if
  /// we were able to understand all uses and thus guarantee we found all
  /// interior pointer uses. Returns false otherwise.
  bool visitInteriorPointerOperands(
      function_ref<void(const InteriorPointerOperand &)> func) const;

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  BorrowedValue(BorrowedValueKind kind, SILValue value)
      : kind(kind), value(value) {}
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
Optional<BorrowedValue> getSingleBorrowIntroducingValue(SILValue inputValue);

/// Pass all uses of guaranteed value \p value to \p visitor looking through
/// guaranteed forwarding instructions.
///
/// \returns true if we were able to visit all uses, false if callback asked us
/// to end iterating early.
/// \arg visitor The visitor callback passed in by the caller. Return false to
/// stop visiting early.
bool visitGuaranteedValueUsesIgnoringForwarding(
    SILValue value, function_ref<bool(Operand *)> visitor);

class InteriorPointerOperandKind {
public:
  enum Kind : uint8_t {
    RefElementAddr,
    RefTailAddr,
    OpenExistentialBox,
  };

private:
  Kind value;

public:
  InteriorPointerOperandKind(Kind newValue) : value(newValue) {}

  operator Kind() const { return value; }

  static Optional<InteriorPointerOperandKind> get(Operand *use) {
    switch (use->getUser()->getKind()) {
    default:
      return None;
    case SILInstructionKind::RefElementAddrInst:
      return InteriorPointerOperandKind(RefElementAddr);
    case SILInstructionKind::RefTailAddrInst:
      return InteriorPointerOperandKind(RefTailAddr);
    case SILInstructionKind::OpenExistentialBoxInst:
      return InteriorPointerOperandKind(OpenExistentialBox);
    }
  }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP;
};

/// A mixed object->address projection that projects a memory location out of an
/// object with guaranteed ownership. All transitive address uses of the
/// interior pointer must be within the lifetime of the guaranteed lifetime. As
/// such, these must be treated as implicit uses of the parent guaranteed value.
struct InteriorPointerOperand {
  Operand *operand;
  InteriorPointerOperandKind kind;

  InteriorPointerOperand(Operand *op)
      : operand(op), kind(*InteriorPointerOperandKind::get(op)) {}

  /// If value is a borrow introducer return it after doing some checks.
  static Optional<InteriorPointerOperand> get(Operand *op) {
    auto kind = InteriorPointerOperandKind::get(op);
    if (!kind)
      return None;
    return InteriorPointerOperand(op, *kind);
  }

  /// Return the end scope of all borrow introducers of the parent value of this
  /// projection. Returns true if we were able to find all borrow introducing
  /// values.
  bool visitBaseValueScopeEndingUses(function_ref<void(Operand *)> func) const {
    SmallVector<BorrowedValue, 4> introducers;
    if (!getAllBorrowIntroducingValues(operand->get(), introducers))
      return false;
    for (const auto &introducer : introducers) {
      if (!introducer.isLocalScope())
        continue;
      introducer.visitLocalScopeEndingUses(func);
    }
    return true;
  }

  SILValue getProjectedAddress() const {
    switch (kind) {
    case InteriorPointerOperandKind::RefElementAddr:
      return cast<RefElementAddrInst>(operand->getUser());
    case InteriorPointerOperandKind::RefTailAddr:
      return cast<RefTailAddrInst>(operand->getUser());
    case InteriorPointerOperandKind::OpenExistentialBox:
      return cast<OpenExistentialBoxInst>(operand->getUser());
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Compute the list of implicit uses that this interior pointer operand puts
  /// on its parent guaranted value.
  ///
  /// Example: Uses of a ref_element_addr can not occur outside of the lifetime
  /// of the instruction's operand. The uses of that address act as liveness
  /// requirements to ensure that the underlying class is alive at all use
  /// points.
  bool getImplicitUses(SmallVectorImpl<Operand *> &foundUses,
                       std::function<void(Operand *)> *onError = nullptr);

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  InteriorPointerOperand(Operand *op, InteriorPointerOperandKind kind)
      : operand(op), kind(kind) {}
};

class OwnedValueIntroducerKind {
public:
  enum Kind : uint8_t {
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

    /// An owned value from the formataion of a new alloc_ref.
    AllocRefInit,
  };

private:
  Kind value;

public:
  static Optional<OwnedValueIntroducerKind> get(SILValue value) {
    if (value.getOwnershipKind() != ValueOwnershipKind::Owned)
      return None;

    switch (value->getKind()) {
    default:
      return None;
    case ValueKind::ApplyInst:
      return OwnedValueIntroducerKind(Apply);
    case ValueKind::BeginApplyResult:
      return OwnedValueIntroducerKind(BeginApply);
    case ValueKind::StructInst:
      return OwnedValueIntroducerKind(Struct);
    case ValueKind::TupleInst:
      return OwnedValueIntroducerKind(Tuple);
    case ValueKind::SILPhiArgument: {
      auto *phiArg = cast<SILPhiArgument>(value);
      if (dyn_cast_or_null<TryApplyInst>(phiArg->getSingleTerminator())) {
        return OwnedValueIntroducerKind(TryApply);
      }
      if (llvm::all_of(phiArg->getParent()->getPredecessorBlocks(),
                       [](SILBasicBlock *block) {
                         return isa<BranchInst>(block->getTerminator());
                       })) {
        return OwnedValueIntroducerKind(Phi);
      }
      return None;
    }
    case ValueKind::SILFunctionArgument:
      return OwnedValueIntroducerKind(FunctionArgument);
    case ValueKind::CopyValueInst:
      return OwnedValueIntroducerKind(Copy);
    case ValueKind::LoadInst: {
      auto qual = cast<LoadInst>(value)->getOwnershipQualifier();
      if (qual == LoadOwnershipQualifier::Take)
        return OwnedValueIntroducerKind(LoadTake);
      if (qual == LoadOwnershipQualifier::Copy)
        return OwnedValueIntroducerKind(LoadCopy);
      return None;
    }
    case ValueKind::PartialApplyInst:
      return OwnedValueIntroducerKind(PartialApplyInit);
    case ValueKind::AllocBoxInst:
      return OwnedValueIntroducerKind(AllocBoxInit);
    case ValueKind::AllocRefInst:
      return OwnedValueIntroducerKind(AllocRefInit);
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

  /// If a value is an owned value introducer we can recognize, return
  /// .some(OwnedValueIntroducer). Otherwise, return None.
  static Optional<OwnedValueIntroducer> get(SILValue value) {
    auto kind = OwnedValueIntroducerKind::get(value);
    if (!kind)
      return None;
    return OwnedValueIntroducer(value, *kind);
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
    case OwnedValueIntroducerKind::Copy:
    case OwnedValueIntroducerKind::LoadCopy:
      return true;
    case OwnedValueIntroducerKind::Apply:
    case OwnedValueIntroducerKind::BeginApply:
    case OwnedValueIntroducerKind::TryApply:
    case OwnedValueIntroducerKind::LoadTake:
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
    case OwnedValueIntroducerKind::FunctionArgument:
    case OwnedValueIntroducerKind::PartialApplyInit:
    case OwnedValueIntroducerKind::AllocBoxInit:
    case OwnedValueIntroducerKind::AllocRefInit:
      return false;
    }
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

Optional<OwnedValueIntroducer> getSingleOwnedValueIntroducer(SILValue value);

} // namespace swift

#endif
