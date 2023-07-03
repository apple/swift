//===--- SILArgument.h - SIL BasicBlock Argument Representation -*- C++ -*-===//
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

#ifndef SWIFT_SIL_SILARGUMENT_H
#define SWIFT_SIL_SILARGUMENT_H

#include "swift/AST/LifetimeAnnotation.h"
#include "swift/Basic/Compiler.h"
#include "swift/SIL/Lifetime.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILFunctionConventions.h"
#include "swift/SIL/SILValue.h"

namespace swift {

class SILBasicBlock;
class SILModule;
class SILPhiArgument;
class SILUndef;
class TermInst;

struct SILArgumentKind {
  enum innerty : std::underlying_type<ValueKind>::type {
#define ARGUMENT(ID, PARENT) ID = unsigned(SILNodeKind::ID),
#define ARGUMENT_RANGE(ID, FIRST, LAST) First_##ID = FIRST, Last_##ID = LAST,
#include "swift/SIL/SILNodes.def"
  } value;

  explicit SILArgumentKind(ValueKind kind)
      : value(*SILArgumentKind::fromValueKind(kind)) {}
  SILArgumentKind(innerty value) : value(value) {}
  operator innerty() const { return value; }

  static llvm::Optional<SILArgumentKind> fromValueKind(ValueKind kind) {
    switch (kind) {
#define ARGUMENT(ID, PARENT)                                                   \
  case ValueKind::ID:                                                          \
    return SILArgumentKind(ID);
#include "swift/SIL/SILNodes.def"
    default:
      return llvm::None;
    }
  }
};

class SILArgument : public ValueBase {
  friend class SILBasicBlock;

  SILBasicBlock *parentBlock;
  const ValueDecl *decl;
  USE_SHARED_UINT8;

protected:
  SILArgument(ValueKind subClassKind, SILBasicBlock *inputParentBlock,
              SILType type, ValueOwnershipKind ownershipKind,
              const ValueDecl *inputDecl = nullptr, bool reborrow = false,
              bool pointerEscape = false);

  // A special constructor, only intended for use in
  // SILBasicBlock::replacePHIArg and replaceFunctionArg.
  explicit SILArgument(ValueKind subClassKind, SILType type,
                       ValueOwnershipKind ownershipKind,
                       const ValueDecl *inputDecl = nullptr,
                       bool reborrow = false, bool pointerEscape = false)
      : ValueBase(subClassKind, type), parentBlock(nullptr), decl(inputDecl) {
    sharedUInt8().SILArgument.valueOwnershipKind = uint8_t(ownershipKind);
    // When the optimizer creates reborrows, reborrow flag needs to be set by
    // calling setReborrow.
    sharedUInt8().SILArgument.reborrow = false;
    sharedUInt8().SILArgument.pointerEscape = false;
  }

public:
  void operator=(const SILArgument &) = delete;
  void operator delete(void *, size_t) = delete;

  ValueOwnershipKind getOwnershipKind() const {
    return ValueOwnershipKind(sharedUInt8().SILArgument.valueOwnershipKind);
  }

  bool isScoped() const {
    auto ownershipKind = getOwnershipKind();
    if (ownershipKind == OwnershipKind::Owned) {
      return true;
    }
    if (ownershipKind != OwnershipKind::Guaranteed) {
      return false;
    }
    return isReborrow();
  }

  bool isReborrow() const {
    return ValueOwnershipKind(sharedUInt8().SILArgument.reborrow);
  }

  bool isGuaranteedForwarding() const {
    return getOwnershipKind() == OwnershipKind::Guaranteed && !isReborrow();
  }

  bool hasPointerEscape() const {
    return ValueOwnershipKind(sharedUInt8().SILArgument.pointerEscape);
  }

  void setOwnershipKind(ValueOwnershipKind newKind) {
    sharedUInt8().SILArgument.valueOwnershipKind = uint8_t(newKind);
  }

  void setReborrow(bool isReborrow) {
    sharedUInt8().SILArgument.reborrow = isReborrow;
  }

  void setHasPointerEscape(bool hasPointerEscape) {
    sharedUInt8().SILArgument.pointerEscape = hasPointerEscape;
  }

  SILBasicBlock *getParent() const { return parentBlock; }

  /// Returns true if this argument is erased from a basic block.
  ///
  /// Note that SILArguments which are erased from a SILBasicBlock are not
  /// destroyed and freed, but are kept in memory. So it's safe to keep a
  /// pointer to an erased argument and then at a later time check if its
  /// erased.
  bool isErased() const { return !parentBlock; }

  SILFunction *getFunction();
  const SILFunction *getFunction() const;

  SILModule &getModule() const;

  const ValueDecl *getDecl() const { return decl; }

  static bool classof(const SILInstruction *) = delete;
  static bool classof(const SILUndef *) = delete;
  static bool classof(SILNodePointer node) {
    return node->getKind() >= SILNodeKind::First_SILArgument &&
           node->getKind() <= SILNodeKind::Last_SILArgument;
  }

  unsigned getIndex() const;

  /// Return non-null if \p value is a phi.
  static SILPhiArgument *asPhi(SILValue value);

  /// Return non-null if \p value is a terminator result.
  static SILPhiArgument *isTerminatorResult(SILValue value);

  /// Return true if this block argument is a phi as opposed to a terminator
  /// result.
  bool isPhi() const;

  /// Return true if this block argument is a terminator result.
  bool isTerminatorResult() const;

  /// If this argument is a phi, return the incoming phi value for the given
  /// predecessor BB. If this argument is not a phi, return an invalid SILValue.
  SILValue getIncomingPhiValue(SILBasicBlock *predBlock) const;

  /// If this argument is a phi, populate `OutArray` with the incoming phi
  /// values for each predecessor BB. If this argument is not a phi, return
  /// false.
  ///
  /// If this block has no predecessors, returnedPhiValues will be empty.
  bool getIncomingPhiValues(SmallVectorImpl<SILValue> &returnedPhiValues) const;

  /// If this argument is a phi, populate `OutArray` with each predecessor block
  /// and its incoming phi value. If this argument is not a phi, return false.
  ///
  /// If this block has no predecessors, returnedPredAndPhiValuePairs will be
  /// empty.
  bool
  getIncomingPhiValues(SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
                           &returnedPredAndPhiValuePairs) const;

  /// If this argument is a true phi, populate `OutArray` with the operand in
  /// each predecessor block associated with an incoming value.
  bool
  getIncomingPhiOperands(SmallVectorImpl<Operand *> &returnedPhiOperands) const;

  /// If this argument is a true phi, for each operand in each predecessor block
  /// associated with an incoming value, call visitor(op). Visitor must return
  /// true for iteration to continue. False to stop it.
  ///
  /// Returns false if this is not a true phi or that a visitor signaled error
  /// by returning false.
  bool visitIncomingPhiOperands(function_ref<bool(Operand *)> visitor) const;

  /// Returns true if we were able to find a single terminator operand value for
  /// each predecessor of this arguments basic block. The found values are
  /// stored in OutArray.
  ///
  /// Note: this peeks through any projections or cast implied by the
  /// terminator. e.g. the incoming value for a switch_enum payload argument is
  /// the enum itself (the operand of the switch_enum).
  [[nodiscard]] bool getSingleTerminatorOperands(
      SmallVectorImpl<SILValue> &returnedSingleTermOperands) const;

  /// Returns true if we were able to find single terminator operand values for
  /// each predecessor of this arguments basic block. The found values are
  /// stored in OutArray alongside their predecessor block.
  ///
  /// Note: this peeks through any projections or cast implied by the
  /// terminator. e.g. the incoming value for a switch_enum payload argument is
  /// the enum itself (the operand of the switch_enum).
  [[nodiscard]] bool getSingleTerminatorOperands(
      SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
          &returnedSingleTermOperands) const;

  /// If this SILArgument's parent block has a single predecessor whose
  /// terminator has a single operand, return that terminator.
  TermInst *getSingleTerminator() const;

  /// Return the terminator instruction for which this argument is a result,
  /// otherwise return nullptr.
  TermInst *getTerminatorForResult() const;

  /// If this terminator result forwards an operand, then return it.
  ///
  /// Precondition: this->isTerminatorResult()
  ///
  /// TODO: Move this and other APIs into a TerminatorResult abstraction.
  Operand *forwardedTerminatorResultOperand() const;

  /// Return the SILArgumentKind of this argument.
  SILArgumentKind getKind() const {
    return SILArgumentKind(ValueBase::getKind());
  }

protected:
  void setParent(SILBasicBlock *newParentBlock) {
    parentBlock = newParentBlock;
  }
};

class SILPhiArgument : public SILArgument {
  friend class SILBasicBlock;

  SILPhiArgument(SILBasicBlock *parentBlock, SILType type,
                 ValueOwnershipKind ownershipKind,
                 const ValueDecl *decl = nullptr, bool isReborrow = false,
                 bool hasPointerEscape = false)
      : SILArgument(ValueKind::SILPhiArgument, parentBlock, type, ownershipKind,
                    decl, isReborrow, hasPointerEscape) {}

  // A special constructor, only intended for use in
  // SILBasicBlock::replacePHIArg.
  explicit SILPhiArgument(SILType type, ValueOwnershipKind ownershipKind,
                          const ValueDecl *decl = nullptr,
                          bool isReborrow = false,
                          bool hasPointerEscape = false)
      : SILArgument(ValueKind::SILPhiArgument, type, ownershipKind, decl,
                    isReborrow, hasPointerEscape) {}

public:
  /// Return true if this is block argument is a phi, as opposed to a terminator
  /// result.
  bool isPhi() const;

  /// Whether any of the values incoming to this phi are lexical.
  bool isLexical() const;

  /// Return true if this block argument is a terminator result.
  bool isTerminatorResult() const { return !isPhi(); }

  /// If this argument is a phi, return the incoming phi value for the given
  /// predecessor BB. If this argument is not a phi, return an invalid SILValue.
  ///
  /// FIXME: Once SILPhiArgument actually implies that it is a phi argument,
  /// this will be guaranteed to return a valid SILValue.
  SILValue getIncomingPhiValue(SILBasicBlock *predBlock) const;

  /// If this argument is a true phi, return the operand in the \p predBLock
  /// associated with an incoming value.
  ///
  /// \returns the operand or nullptr if this is not a true phi.
  Operand *getIncomingPhiOperand(SILBasicBlock *predBlock) const;

  /// If this argument is a phi, populate `OutArray` with the incoming phi
  /// values for each predecessor BB. If this argument is not a phi, return
  /// false.
  ///
  /// FIXME: Once SILPhiArgument actually implies that it is a phi argument,
  /// this will always succeed.
  bool getIncomingPhiValues(SmallVectorImpl<SILValue> &returnedPhiValues) const;

  /// If this argument is a phi, populate `OutArray` with each predecessor block
  /// and its incoming phi value. If this argument is not a phi, return false.
  ///
  /// FIXME: Once SILPhiArgument actually implies that it is a phi argument,
  /// this will always succeed.
  bool
  getIncomingPhiValues(SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
                           &returnedPredAndPhiValuePairs) const;

  /// If this argument is a true phi, populate `OutArray` with the operand in
  /// each predecessor block associated with an incoming value.
  bool
  getIncomingPhiOperands(SmallVectorImpl<Operand *> &returnedPhiOperands) const;

  /// If this argument is a phi, call visitor for each passing the operand for
  /// each incoming phi values for each predecessor BB. If this argument is not
  /// a phi, return false.
  ///
  /// If visitor returns false, iteration is stopped and we return false.
  bool visitIncomingPhiOperands(function_ref<bool(Operand *)> visitor) const;

  /// Visit incoming phi operands and the argument into which they are incoming;
  /// if an operand's value is itself a phi, visit that phi's operands.
  ///
  /// Returns false when called on a non-phi and when the visitor returns false.
  bool visitTransitiveIncomingPhiOperands(
      function_ref<bool(SILPhiArgument *, Operand *)> visitor) const;

  /// Returns true if we were able to find a single terminator operand value for
  /// each predecessor of this arguments basic block. The found values are
  /// stored in OutArray.
  ///
  /// Note: this peeks through any projections or cast implied by the
  /// terminator. e.g. the incoming value for a switch_enum payload argument is
  /// the enum itself (the operand of the switch_enum).
  [[nodiscard]] bool getSingleTerminatorOperands(
      SmallVectorImpl<SILValue> &returnedSingleTermOperands) const;

  /// Returns true if we were able to find single terminator operand values for
  /// each predecessor of this arguments basic block. The found values are
  /// stored in OutArray alongside their predecessor block.
  ///
  /// Note: this peeks through any projections or cast implied by the
  /// terminator. e.g. the incoming value for a switch_enum payload argument is
  /// the enum itself (the operand of the switch_enum).
  [[nodiscard]] bool getSingleTerminatorOperands(
      SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
          &returnedSingleTermOperands) const;

  /// If this SILArgument's parent block has a single predecessor whose
  /// terminator has a single operand, return that terminator.
  TermInst *getSingleTerminator() const;

  /// Return the terminator instruction for which this argument is a result,
  /// otherwise return nullptr.
  TermInst *getTerminatorForResult() const;

  static bool classof(const SILInstruction *) = delete;
  static bool classof(const SILUndef *) = delete;
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::SILPhiArgument;
  }
};

class SILFunctionArgument : public SILArgument {
  friend class SILBasicBlock;

  USE_SHARED_UINT32;

  SILFunctionArgument(
      SILBasicBlock *parentBlock, SILType type,
      ValueOwnershipKind ownershipKind, const ValueDecl *decl = nullptr,
      bool isNoImplicitCopy = false,
      LifetimeAnnotation lifetimeAnnotation = LifetimeAnnotation::None,
      bool isCapture = false,
      bool isParameterPack = false)
      : SILArgument(ValueKind::SILFunctionArgument, parentBlock, type,
                    ownershipKind, decl) {
    sharedUInt32().SILFunctionArgument.noImplicitCopy = isNoImplicitCopy;
    sharedUInt32().SILFunctionArgument.lifetimeAnnotation = lifetimeAnnotation;
    sharedUInt32().SILFunctionArgument.closureCapture = isCapture;
    sharedUInt32().SILFunctionArgument.parameterPack = isParameterPack;
  }

  // A special constructor, only intended for use in
  // SILBasicBlock::replaceFunctionArg.
  explicit SILFunctionArgument(SILType type, ValueOwnershipKind ownershipKind,
                               const ValueDecl *decl = nullptr)
      : SILArgument(ValueKind::SILFunctionArgument, type, ownershipKind, decl) {
  }

public:
  bool isNoImplicitCopy() const {
    return sharedUInt32().SILFunctionArgument.noImplicitCopy;
  }

  void setNoImplicitCopy(bool newValue) {
    sharedUInt32().SILFunctionArgument.noImplicitCopy = newValue;
  }

  bool isClosureCapture() const {
    return sharedUInt32().SILFunctionArgument.closureCapture;
  }
  void setClosureCapture(bool newValue) {
    sharedUInt32().SILFunctionArgument.closureCapture = newValue;
  }

  /// Is this parameter a pack that corresponds to multiple
  /// formal parameters?  (This could mean multiple ParamDecl*s,
  /// or it could mean a ParamDecl* that's a pack expansion.)  Note
  /// that not all lowered parameters of pack type are parameter packs:
  /// they can be part of a single formal parameter of tuple type.
  /// This flag indicates that the lowered parameter has a one-to-many
  /// relationship with formal parameters.
  ///
  /// TODO: preserve the parameter pack references in SIL in a side table
  /// instead of using a single bit.
  bool isFormalParameterPack() const {
    return sharedUInt32().SILFunctionArgument.parameterPack;
  }
  void setFormalParameterPack(bool isPack) {
    sharedUInt32().SILFunctionArgument.parameterPack = isPack;
  }

  LifetimeAnnotation getLifetimeAnnotation() const {
    return LifetimeAnnotation::Case(
        sharedUInt32().SILFunctionArgument.lifetimeAnnotation);
  }

  void setLifetimeAnnotation(LifetimeAnnotation newValue) {
    sharedUInt32().SILFunctionArgument.lifetimeAnnotation = newValue;
  }

  Lifetime getLifetime() const {
    return getType()
        .getLifetime(*getFunction())
        .getLifetimeForAnnotatedValue(getLifetimeAnnotation());
  }

  bool isIndirectResult() const;

  SILArgumentConvention getArgumentConvention() const;

  /// Given that this is an entry block argument, and given that it does
  /// not correspond to an indirect result, return the corresponding
  /// SILParameterInfo.
  SILParameterInfo getKnownParameterInfo() const;

  /// Returns true if this SILArgument is the self argument of its
  /// function. This means that this will return false always for SILArguments
  /// of SILFunctions that do not have self argument and for non-function
  /// argument SILArguments.
  bool isSelf() const;

  /// Returns true if this SILArgument is passed via the given convention.
  bool hasConvention(SILArgumentConvention convention) const {
    return getArgumentConvention() == convention;
  }

  /// Copy all flags stored in this->sharedUInt32() into arg.
  ///
  /// By using this API, cloners can be sure they are updated for the addition
  /// of further flags.
  void copyFlags(SILFunctionArgument *arg) {
    setNoImplicitCopy(arg->isNoImplicitCopy());
    setLifetimeAnnotation(arg->getLifetimeAnnotation());
    setClosureCapture(arg->isClosureCapture());
    setFormalParameterPack(arg->isFormalParameterPack());
  }

  static bool classof(const SILInstruction *) = delete;
  static bool classof(const SILUndef *) = delete;
  static bool classof(SILNodePointer node) {
    return node->getKind() == SILNodeKind::SILFunctionArgument;
  }
};

//===----------------------------------------------------------------------===//
// Out of line Definitions for SILArgument to avoid Forward Decl issues
//===----------------------------------------------------------------------===//

/// Return non-null if \p value is a real phi argument.
inline SILPhiArgument *SILArgument::asPhi(SILValue value) {
  if (auto *arg = dyn_cast<SILPhiArgument>(value)) {
    if (arg->isPhi())
      return arg;
  }
  return nullptr;
}

inline bool SILArgument::isPhi() const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->isPhi();
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

/// Return non-null if \p value is a terminator result.
inline SILPhiArgument *SILArgument::isTerminatorResult(SILValue value) {
  if (auto *arg = dyn_cast<SILPhiArgument>(value)) {
    if (arg->isTerminatorResult())
      return arg;
  }
  return nullptr;
}

inline bool SILArgument::isTerminatorResult() const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->isTerminatorResult();
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline SILValue
SILArgument::getIncomingPhiValue(SILBasicBlock *predBlock) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getIncomingPhiValue(predBlock);
  case SILArgumentKind::SILFunctionArgument:
    return SILValue();
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline bool SILArgument::getIncomingPhiValues(
    SmallVectorImpl<SILValue> &returnedPhiValues) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getIncomingPhiValues(returnedPhiValues);
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline bool SILArgument::getIncomingPhiValues(
    SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
        &returnedPredAndPhiValuePairs) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getIncomingPhiValues(
        returnedPredAndPhiValuePairs);
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline bool SILArgument::getSingleTerminatorOperands(
    SmallVectorImpl<SILValue> &returnedSingleTermOperands) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getSingleTerminatorOperands(
        returnedSingleTermOperands);
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline bool SILArgument::getSingleTerminatorOperands(
    SmallVectorImpl<std::pair<SILBasicBlock *, SILValue>>
        &returnedSingleTermOperands) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getSingleTerminatorOperands(
        returnedSingleTermOperands);
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline TermInst *SILArgument::getSingleTerminator() const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getSingleTerminator();
  case SILArgumentKind::SILFunctionArgument:
    return nullptr;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline TermInst *SILArgument::getTerminatorForResult() const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getTerminatorForResult();
  case SILArgumentKind::SILFunctionArgument:
    return nullptr;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline bool SILArgument::getIncomingPhiOperands(
    SmallVectorImpl<Operand *> &returnedPhiOperands) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->getIncomingPhiOperands(
        returnedPhiOperands);
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

inline bool SILArgument::visitIncomingPhiOperands(
    function_ref<bool(Operand *)> visitor) const {
  switch (getKind()) {
  case SILArgumentKind::SILPhiArgument:
    return cast<SILPhiArgument>(this)->visitIncomingPhiOperands(visitor);
  case SILArgumentKind::SILFunctionArgument:
    return false;
  }
  llvm_unreachable("Covered switch is not covered?!");
}

} // end swift namespace

#endif
