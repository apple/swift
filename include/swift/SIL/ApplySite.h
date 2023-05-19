//===--- ApplySite.h -------------------------------------*- mode: c++ -*--===//
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
///
/// \file
///
/// This file defines utilities for working with "call-site like" SIL
/// instructions. We use the term "call-site" like since we handle partial
/// applications in our utilities.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_APPLYSITE_H
#define SWIFT_SIL_APPLYSITE_H

#include "swift/Basic/STLExtras.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/ADT/ArrayRef.h"

namespace swift {

class FullApplySite;

//===----------------------------------------------------------------------===//
//                                 ApplySite
//===----------------------------------------------------------------------===//

struct ApplySiteKind {
  enum innerty : std::underlying_type<SILInstructionKind>::type {
    ApplyInst = unsigned(SILInstructionKind::ApplyInst),
    PartialApplyInst = unsigned(SILInstructionKind::PartialApplyInst),
    TryApplyInst = unsigned(SILInstructionKind::TryApplyInst),
    BeginApplyInst = unsigned(SILInstructionKind::BeginApplyInst),
  } value;

  explicit ApplySiteKind(SILInstructionKind kind) {
    auto newValue = ApplySiteKind::fromNodeKindHelper(kind);
    assert(newValue && "Non apply site passed into ApplySiteKind");
    value = newValue.value();
  }

  ApplySiteKind(innerty value) : value(value) {}
  operator innerty() const { return value; }

  static Optional<ApplySiteKind> fromNodeKind(SILInstructionKind kind) {
    if (auto innerTyOpt = ApplySiteKind::fromNodeKindHelper(kind))
      return ApplySiteKind(*innerTyOpt);
    return None;
  }

private:
  static Optional<innerty> fromNodeKindHelper(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::ApplyInst:
      return ApplySiteKind::ApplyInst;
    case SILInstructionKind::PartialApplyInst:
      return ApplySiteKind::PartialApplyInst;
    case SILInstructionKind::TryApplyInst:
      return ApplySiteKind::TryApplyInst;
    case SILInstructionKind::BeginApplyInst:
      return ApplySiteKind::BeginApplyInst;
    default:
      return None;
    }
  }
};

/// An apply instruction.
class ApplySite {
  SILInstruction *Inst;

protected:
  explicit ApplySite(void *p) : Inst(static_cast<SILInstruction *>(p)) {}

public:
  ApplySite() : Inst(nullptr) {}
  explicit ApplySite(SILInstruction *inst)
      : Inst(static_cast<SILInstruction *>(inst)) {
    assert(classof(inst) && "not an apply instruction?");
  }
  ApplySite(ApplyInst *inst) : Inst(inst) {}
  ApplySite(PartialApplyInst *inst) : Inst(inst) {}
  ApplySite(TryApplyInst *inst) : Inst(inst) {}
  ApplySite(BeginApplyInst *inst) : Inst(inst) {}

  SILModule &getModule() const { return Inst->getModule(); }

  static ApplySite isa(SILInstruction *inst) {
    auto kind = ApplySiteKind::fromNodeKind(inst->getKind());
    if (!kind)
      return ApplySite();

    switch (kind.value()) {
    case ApplySiteKind::ApplyInst:
      return ApplySite(cast<ApplyInst>(inst));
    case ApplySiteKind::BeginApplyInst:
      return ApplySite(cast<BeginApplyInst>(inst));
    case ApplySiteKind::TryApplyInst:
      return ApplySite(cast<TryApplyInst>(inst));
    case ApplySiteKind::PartialApplyInst:
      return ApplySite(cast<PartialApplyInst>(inst));
    }
    llvm_unreachable("covered switch");
  }

  static ApplySite isa(SILValue value) {
    if (auto *inst = value->getDefiningInstruction())
      return ApplySite::isa(inst);
    return ApplySite();
  }

  ApplySiteKind getKind() const { return ApplySiteKind(Inst->getKind()); }

  SILInstruction *operator*() const { return Inst; }
  SILInstruction *operator->() const { return Inst; }

  explicit operator bool() const { return Inst != nullptr; }

  SILInstruction *getInstruction() const { return Inst; }
  SILLocation getLoc() const { return Inst->getLoc(); }
  const SILDebugScope *getDebugScope() const { return Inst->getDebugScope(); }
  SILFunction *getFunction() const { return Inst->getFunction(); }
  SILBasicBlock *getParent() const { return Inst->getParent(); }

#define FOREACH_IMPL_RETURN(OPERATION)                                         \
  do {                                                                         \
    switch (ApplySiteKind(Inst->getKind())) {                                  \
    case ApplySiteKind::ApplyInst:                                             \
      return cast<ApplyInst>(Inst)->OPERATION;                                 \
    case ApplySiteKind::BeginApplyInst:                                        \
      return cast<BeginApplyInst>(Inst)->OPERATION;                            \
    case ApplySiteKind::PartialApplyInst:                                      \
      return cast<PartialApplyInst>(Inst)->OPERATION;                          \
    case ApplySiteKind::TryApplyInst:                                          \
      return cast<TryApplyInst>(Inst)->OPERATION;                              \
    }                                                                          \
    llvm_unreachable("covered switch");                                        \
  } while (0)

  /// Return the callee operand as a value.
  SILValue getCallee() const { return getCalleeOperand()->get(); }

  /// Return the callee operand.
  Operand *getCalleeOperand() { FOREACH_IMPL_RETURN(getCalleeOperand()); }

  /// Return the callee operand.
  const Operand *getCalleeOperand() const {
    FOREACH_IMPL_RETURN(getCalleeOperand());
  }

  /// Return the callee value by looking through function conversions until we
  /// find a function_ref, partial_apply, or unrecognized callee value.
  SILValue getCalleeOrigin() const { FOREACH_IMPL_RETURN(getCalleeOrigin()); }

  /// Gets the referenced function by looking through partial apply,
  /// convert_function, and thin to thick function until we find a function_ref.
  SILFunction *getCalleeFunction() const {
    FOREACH_IMPL_RETURN(getCalleeFunction());
  }

  bool isCalleeDynamicallyReplaceable() const {
    FOREACH_IMPL_RETURN(isCalleeDynamicallyReplaceable());
  }

  /// Return the referenced function if the callee is a function_ref
  /// instruction.
  SILFunction *getReferencedFunctionOrNull() const {
    FOREACH_IMPL_RETURN(getReferencedFunctionOrNull());
  }

  /// Return the referenced function if the callee is a function_ref like
  /// instruction.
  ///
  /// WARNING: This not necessarily the function that will be called at runtime.
  /// If the callee is a (prev_)dynamic_function_ref the actual function called
  /// might be different because it could be dynamically replaced at runtime.
  ///
  /// If the client of this API wants to look at the content of the returned SIL
  /// function it should call getReferencedFunctionOrNull() instead.
  SILFunction *getInitiallyReferencedFunction() const {
    FOREACH_IMPL_RETURN(getInitiallyReferencedFunction());
  }

  /// Should we optimize this call.
  /// Calls to (previous_)dynamic_function_ref have a dynamic target function so
  /// we should not optimize them.
  bool canOptimize() const {
    return !swift::isa<DynamicFunctionRefInst>(getCallee()) &&
      !swift::isa<PreviousDynamicFunctionRefInst>(getCallee());
  }

  /// Return the type.
  SILType getType() const {
    return getSubstCalleeConv().getSILResultType(
        getFunction()->getTypeExpansionContext());
  }

  /// Get the type of the callee without the applied substitutions.
  CanSILFunctionType getOrigCalleeType() const {
    return getCallee()->getType().castTo<SILFunctionType>();
  }
  /// Get the conventions of the callee without the applied substitutions.
  SILFunctionConventions getOrigCalleeConv() const {
    return SILFunctionConventions(getOrigCalleeType(), getModule());
  }

  /// Get the type of the callee with the applied substitutions.
  CanSILFunctionType getSubstCalleeType() const {
    return getSubstCalleeSILType().castTo<SILFunctionType>();
  }
  SILType getSubstCalleeSILType() const {
    FOREACH_IMPL_RETURN(getSubstCalleeSILType());
  }
  void setSubstCalleeType(CanSILFunctionType t) {
    FOREACH_IMPL_RETURN(setSubstCalleeType(t));
  }

  /// Get the conventions of the callee with the applied substitutions.
  SILFunctionConventions getSubstCalleeConv() const {
    return SILFunctionConventions(getSubstCalleeType(), getModule());
  }

  bool isAsync() const {
    return getOrigCalleeType()->isAsync();
  }

  /// Returns true if the callee function is annotated with
  /// @_semantics("programtermination_point")
  bool isCalleeKnownProgramTerminationPoint() const {
    FOREACH_IMPL_RETURN(isCalleeKnownProgramTerminationPoint());
  }

  /// Returns true if the callee function is annotated with
  /// @_semantics("unavailable_code_reached")
  bool isCalleeUnavailableCodeReached() const {
    FOREACH_IMPL_RETURN(isCalleeUnavailableCodeReached());
  }

  /// Check if this is a call of a never-returning function.
  bool isCalleeNoReturn() const { FOREACH_IMPL_RETURN(isCalleeNoReturn()); }

  bool isCalleeThin() const {
    switch (getSubstCalleeType()->getRepresentation()) {
    case SILFunctionTypeRepresentation::CFunctionPointer:
    case SILFunctionTypeRepresentation::CXXMethod:
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::ObjCMethod:
    case SILFunctionTypeRepresentation::WitnessMethod:
    case SILFunctionTypeRepresentation::Closure:
      return true;
    case SILFunctionTypeRepresentation::Block:
    case SILFunctionTypeRepresentation::Thick:
      return false;
    }
  }

  /// True if this application has generic substitutions.
  bool hasSubstitutions() const { FOREACH_IMPL_RETURN(hasSubstitutions()); }

  /// The substitutions used to bind the generic arguments of this function.
  SubstitutionMap getSubstitutionMap() const {
    FOREACH_IMPL_RETURN(getSubstitutionMap());
  }

  /// Return the associated specialization information.
  const GenericSpecializationInformation *getSpecializationInfo() const {
    FOREACH_IMPL_RETURN(getSpecializationInfo());
  }

  /// Return an operand list corresponding to the applied arguments.
  MutableArrayRef<Operand> getArgumentOperands() const {
    FOREACH_IMPL_RETURN(getArgumentOperands());
  }

  /// Return a list of applied argument values.
  OperandValueArrayRef getArguments() const {
    FOREACH_IMPL_RETURN(getArguments());
  }

  /// Return the number of applied arguments.
  unsigned getNumArguments() const { FOREACH_IMPL_RETURN(getNumArguments()); }

  /// Return the apply operand for the given applied argument index.
  Operand &getArgumentRef(unsigned i) const { return getArgumentOperands()[i]; }

  /// Return the ith applied argument.
  SILValue getArgument(unsigned i) const { return getArguments()[i]; }

  /// Set the ith applied argument.
  void setArgument(unsigned i, SILValue V) const {
    getArgumentOperands()[i].set(V);
  }

  void setCallee(SILValue V) const {
    unsigned calleeIndex = getCalleeOperand()->getOperandNumber();
    getInstruction()->getAllOperands()[calleeIndex].set(V);
  }

  /// Return the operand index of the first applied argument.
  unsigned getOperandIndexOfFirstArgument() const {
    FOREACH_IMPL_RETURN(getArgumentOperandNumber());
  }
#undef FOREACH_IMPL_RETURN

  /// Returns true if \p oper is an argument operand and not the callee
  /// operand.
  bool isArgumentOperand(const Operand &oper) const {
    return oper.getOperandNumber() >= getOperandIndexOfFirstArgument() &&
      oper.getOperandNumber() < getOperandIndexOfFirstArgument() + getNumArguments();
  }

  /// Return the applied argument index for the given operand.
  unsigned getAppliedArgIndex(const Operand &oper) const {
    assert(oper.getUser() == Inst);
    assert(isArgumentOperand(oper));

    return oper.getOperandNumber() - getOperandIndexOfFirstArgument();
  }

  /// Return the callee's function argument index corresponding to the first
  /// applied argument: 0 for full applies; >= 0 for partial applies.
  unsigned getCalleeArgIndexOfFirstAppliedArg() const {
    switch (ApplySiteKind(Inst->getKind())) {
    case ApplySiteKind::ApplyInst:
    case ApplySiteKind::BeginApplyInst:
    case ApplySiteKind::TryApplyInst:
      return 0;
    case ApplySiteKind::PartialApplyInst:
      // The arguments to partial_apply are a suffix of the partial_apply's
      // callee. Note that getSubstCalleeConv is function type of the callee
      // argument passed to this apply, not necessarily the function type of
      // the underlying callee function (i.e. it is based on the `getCallee`
      // type, not the `getCalleeOrigin` type).
      //
      // pa1 = partial_apply f(c) : $(a, b, c)
      // pa2 = partial_apply pa1(b) : $(a, b)
      // apply pa2(a)
      return getSubstCalleeConv().getNumSILArguments() - getNumArguments();
    }
    llvm_unreachable("covered switch");
  }

  /// Return the callee's function argument index corresponding to the given
  /// apply operand. Each function argument index identifies a
  /// SILFunctionArgument in the callee and can be used as a
  /// SILFunctionConvention argument index.
  ///
  /// Note: Passing an applied argument index into SILFunctionConvention, as
  /// opposed to a function argument index, is incorrect.
  unsigned getCalleeArgIndex(const Operand &oper) const {
    return getCalleeArgIndexOfFirstAppliedArg() + getAppliedArgIndex(oper);
  }

  /// Return the SILArgumentConvention for the given applied argument operand.
  SILArgumentConvention getArgumentConvention(const Operand &oper) const {
    unsigned calleeArgIdx =
        getCalleeArgIndexOfFirstAppliedArg() + getAppliedArgIndex(oper);
    return getSubstCalleeConv().getSILArgumentConvention(calleeArgIdx);
  }

  /// Return the SILArgumentConvention for the given applied argument operand at
  /// the apply instruction.
  ///
  /// For full applies, this is equivalent to `getArgumentConvention`. But for
  /// a partial_apply, the argument ownership convention at the partial_apply
  /// instruction itself is different from the argument convention of the callee.
  /// For details see the partial_apply documentation in SIL.rst.
  SILArgumentConvention getArgumentOperandConvention(const Operand &oper) const {
    SILArgumentConvention conv = getArgumentConvention(oper);
    auto *pai = dyn_cast<PartialApplyInst>(Inst);
    if (!pai)
      return conv;
    switch (conv) {
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
    case SILArgumentConvention::Pack_Inout:
      return conv;
    case SILArgumentConvention::Direct_Owned:
    case SILArgumentConvention::Direct_Unowned:
    case SILArgumentConvention::Direct_Guaranteed:
      return pai->isOnStack() ? SILArgumentConvention::Direct_Guaranteed
                              : SILArgumentConvention::Direct_Owned;
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_In_Guaranteed:
      return pai->isOnStack() ? SILArgumentConvention::Indirect_In_Guaranteed
                              : SILArgumentConvention::Indirect_In;
    case SILArgumentConvention::Pack_Guaranteed:
    case SILArgumentConvention::Pack_Owned:
      return pai->isOnStack() ? SILArgumentConvention::Pack_Guaranteed
                              : SILArgumentConvention::Pack_Owned;
    case SILArgumentConvention::Indirect_Out:
    case SILArgumentConvention::Pack_Out:
      llvm_unreachable("partial_apply cannot have an @out operand");
    }
    llvm_unreachable("covered switch");
  }

  /// Return true if 'self' is an applied argument.
  bool hasSelfArgument() const {
    switch (ApplySiteKind(Inst->getKind())) {
    case ApplySiteKind::ApplyInst:
      return cast<ApplyInst>(Inst)->hasSelfArgument();
    case ApplySiteKind::BeginApplyInst:
      return cast<BeginApplyInst>(Inst)->hasSelfArgument();
    case ApplySiteKind::TryApplyInst:
      return cast<TryApplyInst>(Inst)->hasSelfArgument();
    case ApplySiteKind::PartialApplyInst:
      llvm_unreachable("unhandled case");
    }
    llvm_unreachable("covered switch");
  }

  /// Return the applied 'self' argument value.
  SILValue getSelfArgument() const {
    switch (ApplySiteKind(Inst->getKind())) {
    case ApplySiteKind::ApplyInst:
      return cast<ApplyInst>(Inst)->getSelfArgument();
    case ApplySiteKind::BeginApplyInst:
      return cast<BeginApplyInst>(Inst)->getSelfArgument();
    case ApplySiteKind::TryApplyInst:
      return cast<TryApplyInst>(Inst)->getSelfArgument();
    case ApplySiteKind::PartialApplyInst:
      llvm_unreachable("unhandled case");
    }
    llvm_unreachable("covered switch");
  }

  /// Return the 'self' apply operand.
  Operand &getSelfArgumentOperand() {
    switch (ApplySiteKind(Inst->getKind())) {
    case ApplySiteKind::ApplyInst:
      return cast<ApplyInst>(Inst)->getSelfArgumentOperand();
    case ApplySiteKind::BeginApplyInst:
      return cast<BeginApplyInst>(Inst)->getSelfArgumentOperand();
    case ApplySiteKind::TryApplyInst:
      return cast<TryApplyInst>(Inst)->getSelfArgumentOperand();
    case ApplySiteKind::PartialApplyInst:
      llvm_unreachable("Unhandled cast");
    }
    llvm_unreachable("covered switch");
  }

  /// Return a list of applied arguments without self.
  OperandValueArrayRef getArgumentsWithoutSelf() const {
    switch (ApplySiteKind(Inst->getKind())) {
    case ApplySiteKind::ApplyInst:
      return cast<ApplyInst>(Inst)->getArgumentsWithoutSelf();
    case ApplySiteKind::BeginApplyInst:
      return cast<BeginApplyInst>(Inst)->getArgumentsWithoutSelf();
    case ApplySiteKind::TryApplyInst:
      return cast<TryApplyInst>(Inst)->getArgumentsWithoutSelf();
    case ApplySiteKind::PartialApplyInst:
      llvm_unreachable("Unhandled case");
    }
    llvm_unreachable("covered switch");
  }

  /// Returns true if \p op is an operand that passes an indirect
  /// result argument to the apply site.
  bool isIndirectResultOperand(const Operand &op) const;

  ApplyOptions getApplyOptions() const {
    switch (ApplySiteKind(getInstruction()->getKind())) {
    case ApplySiteKind::ApplyInst:
      return cast<ApplyInst>(Inst)->getApplyOptions();
    case ApplySiteKind::BeginApplyInst:
      return cast<BeginApplyInst>(Inst)->getApplyOptions();
    case ApplySiteKind::TryApplyInst:
      return cast<TryApplyInst>(Inst)->getApplyOptions();
    case ApplySiteKind::PartialApplyInst:
      return ApplyOptions();
    }
    llvm_unreachable("covered switch");
  }

  /// If this is a terminator apply site, then pass a builder to insert at the
  /// first instruction of each successor to \p func. Otherwise, pass a builder
  /// to insert at std::next(Inst).
  ///
  /// The intention is that this abstraction will enable the compiler writer to
  /// ignore whether or not an apply site is a terminator when inserting
  /// instructions after an apply site. This results in eliminating unnecessary
  /// if-else code otherwise required to handle such situations.
  ///
  /// NOTE: We pass std::next() for begin_apply. If one wishes to insert code
  /// /after/ the end_apply/abort_apply, please use instead
  /// insertAfterApplication.
  void insertAfterInvocation(function_ref<void(SILBuilder &)> func) const;

  /// Pass a builder with insertion points that are guaranteed to be immediately
  /// after this apply site has been applied.
  ///
  /// For apply and try_apply, that means after the apply.  For partial_apply,
  /// that means after the partial_apply.  For begin_apply, that means after its
  /// end_apply and abort_apply instructions.
  ///
  /// This is just like insertAfterInvocation except that if the full apply site
  /// is a begin_apply, we pass the insertion points after the end_apply,
  /// abort_apply rather than an insertion point right after the
  /// begin_apply. For such functionality, please invoke insertAfterInvocation.
  void insertAfterApplication(function_ref<void(SILBuilder &)> func) const;

  /// Return whether the given apply is of a formally-throwing function
  /// which is statically known not to throw.
  bool isNonThrowing() const {
    return getApplyOptions().contains(ApplyFlags::DoesNotThrow);
  }

  /// Return whether the given apply is of a formally-async function
  /// which is statically known not to await.
  bool isNonAsync() const {
    return getApplyOptions().contains(ApplyFlags::DoesNotAwait);
  }

  static ApplySite getFromOpaqueValue(void *p) { return ApplySite(p); }

  friend bool operator==(ApplySite lhs, ApplySite rhs) {
    return lhs.getInstruction() == rhs.getInstruction();
  }
  friend bool operator!=(ApplySite lhs, ApplySite rhs) {
    return lhs.getInstruction() != rhs.getInstruction();
  }

  static bool classof(const SILInstruction *inst) {
    return bool(ApplySiteKind::fromNodeKind(inst->getKind()));
  }

  void dump() const LLVM_ATTRIBUTE_USED { getInstruction()->dump(); }

  /// Attempt to cast this apply site to a full apply site, returning None on
  /// failure.
  Optional<FullApplySite> asFullApplySite() const;
};

//===----------------------------------------------------------------------===//
//                               FullApplySite
//===----------------------------------------------------------------------===//

struct FullApplySiteKind {
  enum innerty : std::underlying_type<SILInstructionKind>::type {
    ApplyInst = unsigned(SILInstructionKind::ApplyInst),
    TryApplyInst = unsigned(SILInstructionKind::TryApplyInst),
    BeginApplyInst = unsigned(SILInstructionKind::BeginApplyInst),
  } value;

  explicit FullApplySiteKind(SILInstructionKind kind) {
    auto fullApplySiteKind = FullApplySiteKind::fromNodeKindHelper(kind);
    assert(fullApplySiteKind && "SILNodeKind is not a FullApplySiteKind?!");
    value = fullApplySiteKind.value();
  }

  FullApplySiteKind(innerty value) : value(value) {}
  operator innerty() const { return value; }

  static Optional<FullApplySiteKind> fromNodeKind(SILInstructionKind kind) {
    if (auto innerOpt = FullApplySiteKind::fromNodeKindHelper(kind))
      return FullApplySiteKind(*innerOpt);
    return None;
  }

private:
  static Optional<innerty> fromNodeKindHelper(SILInstructionKind kind) {
    switch (kind) {
    case SILInstructionKind::ApplyInst:
      return FullApplySiteKind::ApplyInst;
    case SILInstructionKind::TryApplyInst:
      return FullApplySiteKind::TryApplyInst;
    case SILInstructionKind::BeginApplyInst:
      return FullApplySiteKind::BeginApplyInst;
    default:
      return None;
    }
  }
};

/// A full function application.
class FullApplySite : public ApplySite {
  explicit FullApplySite(void *p) : ApplySite(p) {}

public:
  FullApplySite() : ApplySite() {}
  explicit FullApplySite(SILInstruction *inst) : ApplySite(inst) {
    assert(classof(inst) && "not an apply instruction?");
  }
  FullApplySite(ApplyInst *inst) : ApplySite(inst) {}
  FullApplySite(BeginApplyInst *inst) : ApplySite(inst) {}
  FullApplySite(TryApplyInst *inst) : ApplySite(inst) {}

  static FullApplySite isa(SILInstruction *inst) {
    auto kind = FullApplySiteKind::fromNodeKind(inst->getKind());
    if (!kind)
      return FullApplySite();
    switch (kind.value()) {
    case FullApplySiteKind::ApplyInst:
      return FullApplySite(cast<ApplyInst>(inst));
    case FullApplySiteKind::BeginApplyInst:
      return FullApplySite(cast<BeginApplyInst>(inst));
    case FullApplySiteKind::TryApplyInst:
      return FullApplySite(cast<TryApplyInst>(inst));
    }
    llvm_unreachable("covered switch");
  }

  static FullApplySite isa(SILValue value) {
    if (auto *inst = value->getDefiningInstruction())
      return FullApplySite::isa(inst);
    return FullApplySite();
  }

  FullApplySiteKind getKind() const {
    return FullApplySiteKind(getInstruction()->getKind());
  }

  bool hasIndirectSILResults() const {
    return getSubstCalleeConv().hasIndirectSILResults();
  }

  /// Get the SIL value that represents all of the given call's results. For a
  /// single direct result, returns the actual result. For multiple results,
  /// returns a pseudo-result tuple. The tuple has no storage of its own. The
  /// real results must be extracted from it.
  ///
  /// For ApplyInst, returns the single-value instruction itself.
  ///
  /// For TryApplyInst returns the continuation block argument.
  ///
  /// For BeginApplyInst, returns an invalid value. For coroutines, there is no
  /// single value representing all results. Yielded values are generally
  /// handled differently since they have the convention of incoming arguments.
  SILValue getResult() const {
    switch (getKind()) {
    case FullApplySiteKind::ApplyInst:
      return SILValue(cast<ApplyInst>(getInstruction()));
    case FullApplySiteKind::BeginApplyInst: {
      return SILValue();
    }
    case FullApplySiteKind::TryApplyInst: {
      auto *normalBlock = cast<TryApplyInst>(getInstruction())->getNormalBB();
      assert(normalBlock->getNumArguments() == 1 &&
             "Expected try apply to have a single result");
      return normalBlock->getArgument(0);
    }
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  unsigned getNumIndirectSILResults() const {
    return getSubstCalleeConv().getNumIndirectSILResults();
  }

  OperandValueArrayRef getIndirectSILResults() const {
    return getArguments().slice(0, getNumIndirectSILResults());
  }

  OperandValueArrayRef getArgumentsWithoutIndirectResults() const {
    return getArguments().slice(getNumIndirectSILResults());
  }

  InoutArgumentRange getInoutArguments() const {
    switch (getKind()) {
    case FullApplySiteKind::ApplyInst:
      return cast<ApplyInst>(getInstruction())->getInoutArguments();
    case FullApplySiteKind::TryApplyInst:
      return cast<TryApplyInst>(getInstruction())->getInoutArguments();
    case FullApplySiteKind::BeginApplyInst:
      return cast<BeginApplyInst>(getInstruction())->getInoutArguments();
    }
    llvm_unreachable("invalid apply kind");
  }

  /// Returns true if \p op is the callee operand of this apply site
  /// and not an argument operand.
  bool isCalleeOperand(const Operand &op) const {
    return op.getOperandNumber() < getOperandIndexOfFirstArgument();
  }

  /// Is this an ApplySite that begins the evaluation of a coroutine.
  bool beginsCoroutineEvaluation() const {
    switch (getKind()) {
    case FullApplySiteKind::ApplyInst:
    case FullApplySiteKind::TryApplyInst:
      return false;
    case FullApplySiteKind::BeginApplyInst:
      return true;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Returns true if \p op is an operand that passes an indirect
  /// result argument to the apply site.
  bool isIndirectResultOperand(const Operand &op) const {
    return isArgumentOperand(op)
      && (getCalleeArgIndex(op) < getNumIndirectSILResults());
  }

  static FullApplySite getFromOpaqueValue(void *p) { return FullApplySite(p); }

  static bool classof(const SILInstruction *inst) {
    return bool(FullApplySiteKind::fromNodeKind(inst->getKind()));
  }
};

} // namespace swift

namespace llvm {

template<>
struct PointerLikeTypeTraits<swift::ApplySite> {
public:
  static inline void *getAsVoidPointer(swift::ApplySite apply) {
    return (void*)apply.getInstruction();
  }
  static inline swift::ApplySite getFromVoidPointer(void *pointer) {
    return swift::ApplySite((swift::SILInstruction*)pointer);
  }
  enum { NumLowBitsAvailable =
         PointerLikeTypeTraits<swift::SILNode *>::NumLowBitsAvailable };
};

template<>
struct PointerLikeTypeTraits<swift::FullApplySite> {
public:
  static inline void *getAsVoidPointer(swift::FullApplySite apply) {
    return (void*)apply.getInstruction();
  }
  static inline swift::FullApplySite getFromVoidPointer(void *pointer) {
    return swift::FullApplySite((swift::SILInstruction*)pointer);
  }
  enum { NumLowBitsAvailable =
         PointerLikeTypeTraits<swift::SILNode *>::NumLowBitsAvailable };
};

// An ApplySite casts like a SILInstruction*.
template <> struct simplify_type<const ::swift::ApplySite> {
  using SimpleType = ::swift::SILInstruction *;
  static SimpleType getSimplifiedValue(const ::swift::ApplySite &Val) {
    return Val.getInstruction();
  }
};
template <>
struct simplify_type<::swift::ApplySite>
    : public simplify_type<const ::swift::ApplySite> {};
template <>
struct simplify_type<::swift::FullApplySite>
    : public simplify_type<const ::swift::ApplySite> {};
template <>
struct simplify_type<const ::swift::FullApplySite>
    : public simplify_type<const ::swift::ApplySite> {};

template <> struct DenseMapInfo<::swift::ApplySite> {
  static ::swift::ApplySite getEmptyKey() {
    return ::swift::ApplySite::getFromOpaqueValue(
        llvm::DenseMapInfo<void *>::getEmptyKey());
  }
  static ::swift::ApplySite getTombstoneKey() {
    return ::swift::ApplySite::getFromOpaqueValue(
        llvm::DenseMapInfo<void *>::getTombstoneKey());
  }
  static unsigned getHashValue(::swift::ApplySite AS) {
    auto *I = AS.getInstruction();
    return DenseMapInfo<::swift::SILInstruction *>::getHashValue(I);
  }
  static bool isEqual(::swift::ApplySite LHS, ::swift::ApplySite RHS) {
    return LHS == RHS;
  }
};

template <> struct DenseMapInfo<::swift::FullApplySite> {
  static ::swift::FullApplySite getEmptyKey() {
    return ::swift::FullApplySite::getFromOpaqueValue(
        llvm::DenseMapInfo<void *>::getEmptyKey());
  }
  static ::swift::FullApplySite getTombstoneKey() {
    return ::swift::FullApplySite::getFromOpaqueValue(
        llvm::DenseMapInfo<void *>::getTombstoneKey());
  }
  static unsigned getHashValue(::swift::FullApplySite AS) {
    auto *I = AS.getInstruction();
    return DenseMapInfo<::swift::SILInstruction *>::getHashValue(I);
  }
  static bool isEqual(::swift::FullApplySite LHS, ::swift::FullApplySite RHS) {
    return LHS == RHS;
  }
};

} // namespace llvm

//===----------------------------------------------------------------------===//
//           Inline Definitions to work around Forward Declaration
//===----------------------------------------------------------------------===//

namespace swift {

inline Optional<FullApplySite> ApplySite::asFullApplySite() const {
  return FullApplySite::isa(getInstruction());
}

inline bool ApplySite::isIndirectResultOperand(const Operand &op) const {
  auto fas = asFullApplySite();
  if (!fas)
    return false;
  return fas->isIndirectResultOperand(op);
}

} // namespace swift

#endif
