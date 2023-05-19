//===--- SILBridging.h - header for the swift SILBridging module ----------===//
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

#ifndef SWIFT_SIL_SILBRIDGING_H
#define SWIFT_SIL_SILBRIDGING_H

#include "swift/Basic/BasicBridging.h"
#include "swift/Basic/BridgedSwiftObject.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Decl.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILFunctionConventions.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILWitnessTable.h"
#include "swift/SIL/SILDefaultWitnessTable.h"
#include "swift/SIL/SILVTable.h"
#include <stdbool.h>
#include <stddef.h>
#include <string>

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

struct BridgedInstruction;
struct OptionalBridgedInstruction;
struct OptionalBridgedOperand;
struct OptionalBridgedSuccessor;
struct BridgedBasicBlock;
struct BridgedSuccessorArray;
struct OptionalBridgedBasicBlock;

void registerBridgedClass(llvm::StringRef className, SwiftMetatype metatype);

struct BridgedValue {
  SwiftObject obj;

  enum class Kind {
    SingleValueInstruction,
    Argument,
    MultipleValueInstructionResult,
    Undef
  };

  // Unfortunately we need to take a detour over this enum.
  // Currently it's not possible to switch over `OwnershipKind::inntery`, because it's not a class enum.
  enum class Ownership {
    Unowned,
    Owned,
    Guaranteed,
    None
  };

  Kind getKind() const;

  swift::SILValue getSILValue() const { return static_cast<swift::ValueBase *>(obj); }

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedOperand getFirstUse() const;

  SWIFT_IMPORT_UNSAFE
  swift::SILType getType() const { return getSILValue()->getType(); }

  Ownership getOwnership() const {
    switch (getSILValue()->getOwnershipKind()) {
      case swift::OwnershipKind::Any:
        llvm_unreachable("Invalid ownership for value");
      case swift::OwnershipKind::Unowned:    return Ownership::Unowned;
      case swift::OwnershipKind::Owned:      return Ownership::Owned;
      case swift::OwnershipKind::Guaranteed: return Ownership::Guaranteed;
      case swift::OwnershipKind::None:       return Ownership::None;
    }
  }
};

struct OptionalBridgedValue {
  OptionalSwiftObject obj;
};

inline swift::ValueOwnershipKind castToOwnership(BridgedValue::Ownership ownership) {
  switch (ownership) {
    case BridgedValue::Ownership::Unowned:    return swift::OwnershipKind::Unowned;
    case BridgedValue::Ownership::Owned:      return swift::OwnershipKind::Owned;
    case BridgedValue::Ownership::Guaranteed: return swift::OwnershipKind::Guaranteed;
    case BridgedValue::Ownership::None:       return swift::OwnershipKind::None;
  }
}

// This is the layout of a class existential.
struct BridgeValueExistential {
  BridgedValue value;
  void * _Nonnull conformance;
};

struct BridgedValueArray {
  const BridgeValueExistential * _Nullable base;
  size_t count;

  llvm::ArrayRef<swift::SILValue> getValues(llvm::SmallVectorImpl<swift::SILValue> &storage);
};

struct BridgedOperand {
  swift::Operand * _Nonnull op;

  bool isTypeDependent() const { return op->isTypeDependent(); }

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedOperand getNextUse() const;

  SWIFT_IMPORT_UNSAFE
  BridgedValue getValue() const { return {op->get()}; }

  SWIFT_IMPORT_UNSAFE
  inline BridgedInstruction getUser() const;
};

struct OptionalBridgedOperand {
  swift::Operand * _Nullable op;

  // Assumes that `op` is not null.
  SWIFT_IMPORT_UNSAFE
  BridgedOperand advancedBy(SwiftInt index) const { return {op + index}; }

  // Assumes that `op` is not null.
  SwiftInt distanceTo(BridgedOperand element) const { return element.op - op; }
};

struct BridgedOperandArray {
  OptionalBridgedOperand base;
  SwiftInt count;
};

// Unfortunately we need to take a detour over this enum.
// Currently it's not possible to switch over `SILArgumentConvention::ConventionType`,
// because it's not a class enum.
enum class BridgedArgumentConvention {
  Indirect_In             = swift::SILArgumentConvention::Indirect_In,
  Indirect_In_Guaranteed  = swift::SILArgumentConvention::Indirect_In_Guaranteed,
  Indirect_Inout          = swift::SILArgumentConvention::Indirect_Inout,
  Indirect_InoutAliasable = swift::SILArgumentConvention::Indirect_InoutAliasable,
  Indirect_Out            = swift::SILArgumentConvention::Indirect_Out,
  Direct_Owned            = swift::SILArgumentConvention::Direct_Owned,
  Direct_Unowned          = swift::SILArgumentConvention::Direct_Unowned,
  Direct_Guaranteed       = swift::SILArgumentConvention::Direct_Guaranteed,
  Pack_Owned              = swift::SILArgumentConvention::Pack_Owned,
  Pack_Inout              = swift::SILArgumentConvention::Pack_Inout,
  Pack_Guaranteed         = swift::SILArgumentConvention::Pack_Guaranteed,
  Pack_Out                = swift::SILArgumentConvention::Pack_Out
};

inline BridgedArgumentConvention castToArgumentConvention(swift::SILArgumentConvention convention) {
  return static_cast<BridgedArgumentConvention>(convention.Value);
}

struct BridgedFunction {
  SwiftObject obj;

  SWIFT_IMPORT_UNSAFE
  swift::SILFunction * _Nonnull getFunction() const {
    return static_cast<swift::SILFunction *>(obj);
  }

  SWIFT_IMPORT_UNSAFE
  llvm::StringRef getName() const { return getFunction()->getName(); }

  std::string getDebugDescription() const;

  bool hasOwnership() const { return getFunction()->hasOwnership(); }

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedBasicBlock getFirstBlock() const;

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedBasicBlock getLastBlock() const;

  SwiftInt getNumIndirectFormalResults() const {
    return (SwiftInt)getFunction()->getLoweredFunctionType()->getNumIndirectFormalResults();
  }

  SwiftInt getNumParameters() const {
    return (SwiftInt)getFunction()->getLoweredFunctionType()->getNumParameters();
  }

  SwiftInt getSelfArgumentIndex() const {
    swift::SILFunctionConventions conv(getFunction()->getConventionsInContext());
    swift::CanSILFunctionType fTy = getFunction()->getLoweredFunctionType();
    if (!fTy->hasSelfParam())
      return -1;
    return conv.getNumParameters() + conv.getNumIndirectSILResults() - 1;
  }

  SwiftInt getNumSILArguments() const {
    return swift::SILFunctionConventions(getFunction()->getConventionsInContext()).getNumSILArguments();
  }

  swift::SILType getSILArgumentType(SwiftInt idx) const {
    swift::SILFunctionConventions conv(getFunction()->getConventionsInContext());
    return conv.getSILArgumentType(idx, getFunction()->getTypeExpansionContext());
  }

  BridgedArgumentConvention getSILArgumentConvention(SwiftInt idx) const {
    swift::SILFunctionConventions conv(getFunction()->getConventionsInContext());
    return castToArgumentConvention(swift::SILArgumentConvention(conv.getParamInfoForSILArg(idx).getConvention()));
  }

  swift::SILType getSILResultType() const {
    swift::SILFunctionConventions conv(getFunction()->getConventionsInContext());
    return conv.getSILResultType(getFunction()->getTypeExpansionContext());
  }

  bool isSwift51RuntimeAvailable() const {
    if (getFunction()->getResilienceExpansion() != swift::ResilienceExpansion::Maximal)
      return false;

    swift::ASTContext &ctxt = getFunction()->getModule().getASTContext();
    return swift::AvailabilityContext::forDeploymentTarget(ctxt).isContainedIn(ctxt.getSwift51Availability());
  }

  bool isPossiblyUsedExternally() const {
    return getFunction()->isPossiblyUsedExternally();
  }

  bool isAvailableExternally() const {
    return getFunction()->isAvailableExternally();
  }

  bool isTransparent() const {
    return getFunction()->isTransparent() == swift::IsTransparent;
  }

  bool isGlobalInitFunction() const {
    return getFunction()->isGlobalInit();
  }

  bool isGlobalInitOnceFunction() const {
    return getFunction()->isGlobalInitOnceFunction();
  }

  bool hasSemanticsAttr(llvm::StringRef attrName) const {
    return getFunction()->hasSemanticsAttr(attrName);
  }

  swift::EffectsKind getEffectAttribute() const {
    return getFunction()->getEffectsKind();
  }

  swift::PerformanceConstraints getPerformanceConstraints() const {
    return getFunction()->getPerfConstraints();
  }

  enum class InlineStrategy {
    InlineDefault = swift::InlineDefault,
    NoInline = swift::NoInline,
    AlwaysInline = swift::AlwaysInline
  };

  InlineStrategy getInlineStrategy() const {
    return (InlineStrategy)getFunction()->getInlineStrategy();
  }

  bool needsStackProtection() const {
    return getFunction()->needsStackProtection();
  }

  void setNeedStackProtection(bool needSP) const {
    getFunction()->setNeedStackProtection(needSP);
  }

  enum class ParseEffectsMode {
    argumentEffectsFromSource,
    argumentEffectsFromSIL,
    globalEffectsFromSIL,
    multipleEffectsFromSIL
  };

  struct ParsingError {
    const unsigned char * _Nullable message;
    SwiftInt position;
  };

  struct EffectInfo {
    SwiftInt argumentIndex;
    bool isDerived;
    bool isEmpty;
    bool isValid;
  };

  typedef void (* _Nonnull RegisterFn)(BridgedFunction f, void * _Nonnull data, SwiftInt size);
  typedef void (* _Nonnull WriteFn)(BridgedFunction, BridgedOStream, SwiftInt);
  typedef ParsingError (*_Nonnull ParseFn)(BridgedFunction,
                                           llvm::StringRef,
                                           ParseEffectsMode, SwiftInt,
                                           BridgedArrayRef);
  typedef SwiftInt (* _Nonnull CopyEffectsFn)(BridgedFunction, BridgedFunction);
  typedef EffectInfo (* _Nonnull GetEffectInfoFn)(BridgedFunction, SwiftInt);
  typedef swift::MemoryBehavior (* _Nonnull GetMemBehaviorFn)(BridgedFunction, bool);

  static void registerBridging(SwiftMetatype metatype,
              RegisterFn initFn, RegisterFn destroyFn,
              WriteFn writeFn, ParseFn parseFn,
              CopyEffectsFn copyEffectsFn,
              GetEffectInfoFn effectInfoFn,
              GetMemBehaviorFn memBehaviorFn);
};

struct OptionalBridgedFunction {
  OptionalSwiftObject obj;
};

struct BridgedGlobalVar {
  SwiftObject obj;

  SWIFT_IMPORT_UNSAFE
  swift::SILGlobalVariable * _Nonnull getGlobal() const {
    return static_cast<swift::SILGlobalVariable *>(obj);
  }

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  llvm::StringRef getName() const { return getGlobal()->getName(); }

  bool isLet() const { return getGlobal()->isLet(); }

  void setLet(bool value) const { getGlobal()->setLet(value); }

  bool isPossiblyUsedExternally() const {
    return getGlobal()->isPossiblyUsedExternally();
  }

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedInstruction getStaticInitializerValue() const;

  bool canBeInitializedStatically() const;

  static inline bool isValidStaticInitializer(BridgedInstruction inst);
};

struct OptionalBridgedGlobalVar {
  OptionalSwiftObject obj;
};

struct BridgedMultiValueResult {
  SwiftObject obj;

  swift::MultipleValueInstructionResult * _Nonnull getMVResult() const {
    return static_cast<swift::MultipleValueInstructionResult *>(obj);
  }

  SWIFT_IMPORT_UNSAFE
  inline BridgedInstruction getParent() const;

  SwiftInt getIndex() const {
    return (SwiftInt)getMVResult()->getIndex();
  }
};

struct OptionalBridgedInstruction {
  OptionalSwiftObject obj;

  OptionalBridgedInstruction() : obj(nullptr) {}

  OptionalBridgedInstruction(OptionalSwiftObject obj) : obj(obj) {}

  swift::SILInstruction * _Nullable getInst() const {
    if (!obj)
      return nullptr;
    return llvm::cast<swift::SILInstruction>(static_cast<swift::SILNode *>(obj)->castToInstruction());
  }
};

struct BridgedTypeArray {
  llvm::ArrayRef<swift::Type> typeArray;

  SWIFT_IMPORT_UNSAFE
  static BridgedTypeArray fromReplacementTypes(swift::SubstitutionMap substMap) {
    return {substMap.getReplacementTypes()};
  }

  SwiftInt getCount() const { return SwiftInt(typeArray.size()); }

  SWIFT_IMPORT_UNSAFE
  swift::SILType getAt(SwiftInt index) const {
    auto ty = swift::CanType(typeArray[index]);
    if (ty->isLegalSILType())
      return swift::SILType::getPrimitiveObjectType(ty);
    return swift::SILType();
  }
};

struct BridgedInstruction {
  SwiftObject obj;

  BridgedInstruction(SwiftObject obj) : obj(obj) {}

  template <class I> I *_Nonnull getAs() const {
    return llvm::cast<I>(static_cast<swift::SILNode *>(obj)->castToInstruction());
  }

  swift::SILInstruction * _Nonnull getInst() const { return getAs<swift::SILInstruction>(); }

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedInstruction getNext() const {
    auto iter = std::next(getInst()->getIterator());
    if (iter == getInst()->getParent()->end())
      return {nullptr};
    return {iter->asSILNode()};
  }

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedInstruction getPrevious() const {
    auto iter = std::next(getInst()->getReverseIterator());
    if (iter == getInst()->getParent()->rend())
      return {nullptr};
    return {iter->asSILNode()};
  }

  SWIFT_IMPORT_UNSAFE
  inline BridgedBasicBlock getParent() const;

  bool isDeleted() const {
    return getInst()->isDeleted();
  }

  SWIFT_IMPORT_UNSAFE
  BridgedOperandArray getOperands() const {
    auto operands = getInst()->getAllOperands();
    return {{operands.data()}, (SwiftInt)operands.size()};
  }

  void setOperand(SwiftInt index, BridgedValue value) const {
    getInst()->setOperand((unsigned)index, value.getSILValue());
  }

  SWIFT_IMPORT_UNSAFE
  swift::SILDebugLocation getLocation() const {
    return getInst()->getDebugLocation();
  }

  swift::MemoryBehavior getMemBehavior() const {
    return getInst()->getMemoryBehavior();
  }

  bool mayRelease() const {
    return getInst()->mayRelease();
  }

  bool mayHaveSideEffects() const {
    return getInst()->mayHaveSideEffects();
  }

  bool mayAccessPointer() const;
  bool mayLoadWeakOrUnowned() const;
  bool maySynchronizeNotConsideringSideEffects() const;
  bool mayBeDeinitBarrierNotConsideringSideEffects() const;

  SwiftInt MultipleValueInstruction_getNumResults() const {
    return getAs<swift::MultipleValueInstruction>()->getNumResults();
  }

  SWIFT_IMPORT_UNSAFE
  BridgedMultiValueResult MultipleValueInstruction_getResult(SwiftInt index) const {
    return {getAs<swift::MultipleValueInstruction>()->getResult(index)};
  }

  SWIFT_IMPORT_UNSAFE
  inline BridgedSuccessorArray TermInst_getSuccessors() const;

  SWIFT_IMPORT_UNSAFE
  llvm::StringRef CondFailInst_getMessage() const {
    return getAs<swift::CondFailInst>()->getMessage();
  }

  SwiftInt LoadInst_getLoadOwnership() const {
    return (SwiftInt)getAs<swift::LoadInst>()->getOwnershipQualifier();
  }

  swift::BuiltinValueKind BuiltinInst_getID() const {
    return getAs<swift::BuiltinInst>()->getBuiltinInfo().ID;
  }

  SWIFT_IMPORT_UNSAFE
  swift::SubstitutionMap BuiltinInst_getSubstitutionMap() const {
    return getAs<swift::BuiltinInst>()->getSubstitutions();
  }


  bool AddressToPointerInst_needsStackProtection() const {
    return getAs<swift::AddressToPointerInst>()->needsStackProtection();
  }

  bool IndexAddrInst_needsStackProtection() const {
    return getAs<swift::IndexAddrInst>()->needsStackProtection();
  }

  SWIFT_IMPORT_UNSAFE
  BridgedGlobalVar GlobalAccessInst_getGlobal() const {
    return {getAs<swift::GlobalAccessInst>()->getReferencedGlobal()};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedGlobalVar AllocGlobalInst_getGlobal() const {
    return {getAs<swift::AllocGlobalInst>()->getReferencedGlobal()};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedFunction FunctionRefBaseInst_getReferencedFunction() const {
    return {getAs<swift::FunctionRefBaseInst>()->getInitiallyReferencedFunction()};
  }

  SWIFT_IMPORT_UNSAFE
  llvm::APInt IntegerLiteralInst_getValue() const {
    return getAs<swift::IntegerLiteralInst>()->getValue();
  }

  SWIFT_IMPORT_UNSAFE
  llvm::StringRef StringLiteralInst_getValue() const {
    return getAs<swift::StringLiteralInst>()->getValue();
  }

  SwiftInt TupleExtractInst_fieldIndex() const {
    return getAs<swift::TupleExtractInst>()->getFieldIndex();
  }

  SwiftInt TupleElementAddrInst_fieldIndex() const {
    return getAs<swift::TupleElementAddrInst>()->getFieldIndex();
  }

  SwiftInt StructExtractInst_fieldIndex() const {
    return getAs<swift::StructExtractInst>()->getFieldIndex();
  }

  OptionalBridgedValue StructInst_getUniqueNonTrivialFieldValue() const {
    return {getAs<swift::StructInst>()->getUniqueNonTrivialFieldValue()};
  }

  SwiftInt StructElementAddrInst_fieldIndex() const {
    return getAs<swift::StructElementAddrInst>()->getFieldIndex();
  }

  SwiftInt ProjectBoxInst_fieldIndex() const {
    return getAs<swift::ProjectBoxInst>()->getFieldIndex();
  }

  SwiftInt EnumInst_caseIndex() const {
    return getAs<swift::EnumInst>()->getCaseIndex();
  }

  SwiftInt UncheckedEnumDataInst_caseIndex() const {
    return getAs<swift::UncheckedEnumDataInst>()->getCaseIndex();
  }

  SwiftInt InitEnumDataAddrInst_caseIndex() const {
    return getAs<swift::InitEnumDataAddrInst>()->getCaseIndex();
  }

  SwiftInt UncheckedTakeEnumDataAddrInst_caseIndex() const {
    return getAs<swift::UncheckedTakeEnumDataAddrInst>()->getCaseIndex();
  }

  SwiftInt InjectEnumAddrInst_caseIndex() const {
    return getAs<swift::InjectEnumAddrInst>()->getCaseIndex();
  }

  SwiftInt RefElementAddrInst_fieldIndex() const {
    return getAs<swift::RefElementAddrInst>()->getFieldIndex();
  }

  bool RefElementAddrInst_fieldIsLet() const {
    return getAs<swift::RefElementAddrInst>()->getField()->isLet();
  }

  SwiftInt PartialApplyInst_numArguments() const {
    return getAs<swift::PartialApplyInst>()->getNumArguments();
  }

  SwiftInt ApplyInst_numArguments() const {
    return getAs<swift::ApplyInst>()->getNumArguments();
  }

  bool ApplyInst_getNonThrowing() const {
    return getAs<swift::ApplyInst>()->isNonThrowing();
  }

  bool ApplyInst_getNonAsync() const {
    return getAs<swift::ApplyInst>()->isNonAsync();
  }

  const swift::GenericSpecializationInformation * _Nullable

  SWIFT_IMPORT_UNSAFE
  ApplyInst_getSpecializationInfo() const {
    return getAs<swift::ApplyInst>()->getSpecializationInfo();
  }

  SwiftInt PartialApply_getCalleeArgIndexOfFirstAppliedArg() const {
    return swift::ApplySite(getInst()).getCalleeArgIndexOfFirstAppliedArg();
  }

  bool PartialApplyInst_isOnStack() const {
    return getAs<swift::PartialApplyInst>()->isOnStack();
  }

  bool AllocStackInst_hasDynamicLifetime() const {
    return getAs<swift::AllocStackInst>()->hasDynamicLifetime();
  }

  bool AllocRefInstBase_isObjc() const {
    return getAs<swift::AllocRefInstBase>()->isObjC();
  }

  bool AllocRefInstBase_canAllocOnStack() const {
    return getAs<swift::AllocRefInstBase>()->canAllocOnStack();
  }

  SwiftInt BeginApplyInst_numArguments() const {
    return getAs<swift::BeginApplyInst>()->getNumArguments();
  }

  SwiftInt TryApplyInst_numArguments() const {
    return getAs<swift::TryApplyInst>()->getNumArguments();
  }

  SWIFT_IMPORT_UNSAFE
  inline BridgedBasicBlock BranchInst_getTargetBlock() const;

  SwiftInt SwitchEnumInst_getNumCases() const {
    return getAs<swift::SwitchEnumInst>()->getNumCases();
  }

  SwiftInt SwitchEnumInst_getCaseIndex(SwiftInt idx) const {
    auto *seInst = getAs<swift::SwitchEnumInst>();
    return seInst->getModule().getCaseIndex(seInst->getCase(idx).first);
  }

  SwiftInt StoreInst_getStoreOwnership() const {
    return (SwiftInt)getAs<swift::StoreInst>()->getOwnershipQualifier();
  }

  swift::SILAccessKind BeginAccessInst_getAccessKind() const {
    return getAs<swift::BeginAccessInst>()->getAccessKind();
  }

  bool BeginAccessInst_isStatic() const {
    return getAs<swift::BeginAccessInst>()->getEnforcement() == swift::SILAccessEnforcement::Static;
  }

  bool CopyAddrInst_isTakeOfSrc() const {
    return getAs<swift::CopyAddrInst>()->isTakeOfSrc();
  }

  bool CopyAddrInst_isInitializationOfDest() const {
    return getAs<swift::CopyAddrInst>()->isInitializationOfDest();
  }

  void RefCountingInst_setIsAtomic(bool isAtomic) const {
    getAs<swift::RefCountingInst>()->setAtomicity(
        isAtomic ? swift::RefCountingInst::Atomicity::Atomic
                 : swift::RefCountingInst::Atomicity::NonAtomic);
  }

  bool RefCountingInst_getIsAtomic() const {
    return getAs<swift::RefCountingInst>()->getAtomicity() == swift::RefCountingInst::Atomicity::Atomic;
  }

  SwiftInt CondBranchInst_getNumTrueArgs() const {
    return getAs<swift::CondBranchInst>()->getNumTrueArgs();
  }

  void AllocRefInstBase_setIsStackAllocatable() const {
    getAs<swift::AllocRefInstBase>()->setStackAllocatable();
  }

  inline void TermInst_replaceBranchTarget(BridgedBasicBlock from, BridgedBasicBlock to) const;

  SwiftInt KeyPathInst_getNumComponents() const {
    if (swift::KeyPathPattern *pattern = getAs<swift::KeyPathInst>()->getPattern()) {
      return (SwiftInt)pattern->getComponents().size();
    }
    return 0;
  }

  struct KeyPathFunctionResults {
    enum { maxFunctions = 5 };
    BridgedFunction functions[maxFunctions];
    SwiftInt numFunctions;
  };

  void KeyPathInst_getReferencedFunctions(SwiftInt componentIdx, KeyPathFunctionResults * _Nonnull results) const {
    swift::KeyPathPattern *pattern = getAs<swift::KeyPathInst>()->getPattern();
    const swift::KeyPathPatternComponent &comp = pattern->getComponents()[componentIdx];
    results->numFunctions = 0;

    comp.visitReferencedFunctionsAndMethods([results](swift::SILFunction *func) {
        assert(results->numFunctions < KeyPathFunctionResults::maxFunctions);
        results->functions[results->numFunctions++] = {func};
      }, [](swift::SILDeclRef) {});
  }

  SWIFT_IMPORT_UNSAFE
  swift::SubstitutionMap ApplySite_getSubstitutionMap() const {
    auto as = swift::ApplySite(getInst());
    return as.getSubstitutionMap();
  }

  BridgedArgumentConvention ApplySite_getArgumentConvention(SwiftInt calleeArgIdx) const {
    auto as = swift::ApplySite(getInst());
    auto conv = as.getSubstCalleeConv().getSILArgumentConvention(calleeArgIdx);
    return castToArgumentConvention(conv.Value);
  }

  SwiftInt ApplySite_getNumArguments() const {
    return swift::ApplySite(getInst()).getNumArguments();
  }

  SwiftInt FullApplySite_numIndirectResultArguments() const {
    auto fas = swift::FullApplySite(getInst());
    return fas.getNumIndirectSILResults();
  }
};

struct BridgedArgument {
  SwiftObject obj;

  swift::SILArgument * _Nonnull getArgument() const {
    return static_cast<swift::SILArgument *>(obj);
  }

  SWIFT_IMPORT_UNSAFE
  inline BridgedBasicBlock getParent() const;

  BridgedArgumentConvention getConvention() const {
    auto *fArg = llvm::cast<swift::SILFunctionArgument>(getArgument());
    return castToArgumentConvention(fArg->getArgumentConvention());
  }
};

struct OptionalBridgedArgument {
  OptionalSwiftObject obj;
};

struct OptionalBridgedBasicBlock {
  OptionalSwiftObject obj;

  swift::SILBasicBlock * _Nullable getBlock() const {
    return obj ? static_cast<swift::SILBasicBlock *>(obj) : nullptr;
  }
};

struct BridgedBasicBlock {
  SwiftObject obj;

  swift::SILBasicBlock * _Nonnull getBlock() const {
    return static_cast<swift::SILBasicBlock *>(obj);
  }

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedBasicBlock getNext() const {
    auto iter = std::next(getBlock()->getIterator());
    if (iter == getBlock()->getParent()->end())
      return {nullptr};
    return {&*iter};
  }

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedBasicBlock getPrevious() const {
    auto iter = std::next(getBlock()->getReverseIterator());
    if (iter == getBlock()->getParent()->rend())
      return {nullptr};
    return {&*iter};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedFunction getFunction() const {
    return {getBlock()->getParent()};
  }

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedInstruction getFirstInst() const {
    if (getBlock()->empty())
      return {nullptr};
    return {getBlock()->front().asSILNode()};
  }

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedInstruction getLastInst() const {
    if (getBlock()->empty())
      return {nullptr};
    return {getBlock()->back().asSILNode()};
  }

  SwiftInt getNumArguments() const {
    return getBlock()->getNumArguments();
  }

  SWIFT_IMPORT_UNSAFE
  BridgedArgument getArgument(SwiftInt index) const {
    return {getBlock()->getArgument(index)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedArgument addBlockArgument(swift::SILType type, BridgedValue::Ownership ownership) const {
    return {getBlock()->createPhiArgument(type, castToOwnership(ownership))};
  }

  void eraseArgument(SwiftInt index) const {
    getBlock()->eraseArgument(index);
  }

  void moveAllInstructionsToBegin(BridgedBasicBlock dest) const {
    dest.getBlock()->spliceAtBegin(getBlock());
  }

  void moveAllInstructionsToEnd(BridgedBasicBlock dest) const {
    dest.getBlock()->spliceAtEnd(getBlock());
  }

  void moveArgumentsTo(BridgedBasicBlock dest) const {
    dest.getBlock()->moveArgumentList(getBlock());
  }

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedSuccessor getFirstPred() const;
};

struct BridgedSuccessor {
  const swift::SILSuccessor * _Nonnull succ;

  SWIFT_IMPORT_UNSAFE
  inline OptionalBridgedSuccessor getNext() const;

  SWIFT_IMPORT_UNSAFE
  BridgedBasicBlock getTargetBlock() const {
    return {succ->getBB()};
  }

  SWIFT_IMPORT_UNSAFE
  inline BridgedInstruction getContainingInst() const;
};

struct OptionalBridgedSuccessor {
  const swift::SILSuccessor * _Nullable succ;

  // Assumes that `succ` is not null.
  SWIFT_IMPORT_UNSAFE
  BridgedSuccessor advancedBy(SwiftInt index) const { return {succ + index}; }
};

struct BridgedSuccessorArray {
  OptionalBridgedSuccessor base;
  SwiftInt count;
};

struct BridgedVTableEntry {
  const swift::SILVTableEntry * _Nonnull entry;

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  BridgedFunction getImplementation() const {
    return {entry->getImplementation()};
  }
};

struct BridgedVTableEntryArray {
  BridgedVTableEntry base;
  SwiftInt count;
};

struct BridgedVTable {
  const swift::SILVTable * _Nonnull vTable;

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  BridgedVTableEntryArray getEntries() const {
    auto entries = vTable->getEntries();
    return {{entries.data()}, (SwiftInt)entries.size()};
  }
};

struct BridgedWitnessTableEntry {
  const swift::SILWitnessTable::Entry * _Nonnull entry;

  SWIFT_IMPORT_UNSAFE
  std::string getDebugDescription() const;

  swift::SILWitnessTable::WitnessKind getKind() const {
    return entry->getKind();
  }

  SWIFT_IMPORT_UNSAFE
  OptionalBridgedFunction getMethodFunction() const {
    return {entry->getMethodWitness().Witness};
  }
};

struct BridgedWitnessTableEntryArray {
  BridgedWitnessTableEntry base;
  SwiftInt count;
};

struct BridgedWitnessTable {
  const swift::SILWitnessTable * _Nonnull table;

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  BridgedWitnessTableEntryArray getEntries() const {
    auto entries = table->getEntries();
    return {{entries.data()}, (SwiftInt)entries.size()};
  }
};

struct OptionalBridgedWitnessTable {
  const swift::SILWitnessTable * _Nullable table;
};

struct BridgedDefaultWitnessTable {
  const swift::SILDefaultWitnessTable * _Nonnull table;

  std::string getDebugDescription() const;

  SWIFT_IMPORT_UNSAFE
  BridgedWitnessTableEntryArray getEntries() const {
    auto entries = table->getEntries();
    return {{entries.data()}, (SwiftInt)entries.size()};
  }
};

struct OptionalBridgedDefaultWitnessTable {
  const swift::SILDefaultWitnessTable * _Nullable table;
};

struct BridgedBuilder{
  OptionalBridgedInstruction insertBefore;
  OptionalBridgedBasicBlock insertAtEnd;
  swift::SILDebugLocation loc;

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createBuiltinBinaryFunction(llvm::StringRef name,
                                                 swift::SILType operandType, swift::SILType resultType,
                                                 BridgedValueArray arguments) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    llvm::SmallVector<swift::SILValue, 16> argValues;
    return {builder.createBuiltinBinaryFunction(swift::RegularLocation(loc.getLocation()),
                                                name, operandType, resultType,
                                                arguments.getValues(argValues))};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createCondFail(BridgedValue condition, llvm::StringRef message) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createCondFail(swift::RegularLocation(loc.getLocation()),
                                   condition.getSILValue(), message)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createIntegerLiteral(swift::SILType type, SwiftInt value) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createIntegerLiteral(swift::RegularLocation(loc.getLocation()),
                                         type, value)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createAllocStack(swift::SILType type,
                                      bool hasDynamicLifetime, bool isLexical, bool wasMoved) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createAllocStack(swift::RegularLocation(loc.getLocation()),
                                     type, llvm::None, hasDynamicLifetime, isLexical, wasMoved)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createDeallocStack(BridgedValue operand) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createDeallocStack(swift::RegularLocation(loc.getLocation()),
                                       operand.getSILValue())};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createDeallocStackRef(BridgedValue operand) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createDeallocStackRef(swift::RegularLocation(loc.getLocation()),
                                          operand.getSILValue())};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createUncheckedRefCast(BridgedValue op, swift::SILType type) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createUncheckedRefCast(swift::RegularLocation(loc.getLocation()),
                                           op.getSILValue(), type)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createSetDeallocating(BridgedValue op, bool isAtomic) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createSetDeallocating(swift::RegularLocation(loc.getLocation()),
                                          op.getSILValue(),
                                          isAtomic ? swift::RefCountingInst::Atomicity::Atomic
                                                   : swift::RefCountingInst::Atomicity::NonAtomic)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createFunctionRef(BridgedFunction function) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createFunctionRef(swift::RegularLocation(loc.getLocation()),
                                      function.getFunction())};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createCopyValue(BridgedValue op) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createCopyValue(swift::RegularLocation(loc.getLocation()),
                                    op.getSILValue())};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createCopyAddr(BridgedValue from, BridgedValue to,
                                    bool takeSource, bool initializeDest) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createCopyAddr(swift::RegularLocation(loc.getLocation()),
                                   from.getSILValue(), to.getSILValue(),
                                   swift::IsTake_t(takeSource),
                                   swift::IsInitialization_t(initializeDest))};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createDestroyValue(BridgedValue op) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createDestroyValue(swift::RegularLocation(loc.getLocation()),
                                       op.getSILValue())};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createDebugStep() const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createDebugStep(swift::RegularLocation(loc.getLocation()))};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createApply(
            BridgedValue function, swift::SubstitutionMap subMap,
            BridgedValueArray arguments, bool isNonThrowing, bool isNonAsync,
            const swift::GenericSpecializationInformation * _Nullable specInfo) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    llvm::SmallVector<swift::SILValue, 16> argValues;
    swift::ApplyOptions applyOpts;
    if (isNonThrowing) { applyOpts |= swift::ApplyFlags::DoesNotThrow; }
    if (isNonAsync) { applyOpts |= swift::ApplyFlags::DoesNotAwait; }

    return {builder.createApply(swift::RegularLocation(loc.getLocation()),
                                function.getSILValue(), subMap,
                                arguments.getValues(argValues),
                                applyOpts, specInfo)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createSwitchEnumInst(
            BridgedValue enumVal, OptionalBridgedBasicBlock defaultBlock,
            const void * _Nullable enumCases, SwiftInt numEnumCases) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    using BridgedCase = const std::pair<SwiftInt, BridgedBasicBlock>;
    llvm::ArrayRef<BridgedCase> cases(static_cast<BridgedCase *>(enumCases),
                                (unsigned)numEnumCases);
    llvm::SmallDenseMap<SwiftInt, swift::EnumElementDecl *> mappedElements;
    swift::SILValue en = enumVal.getSILValue();
    swift::EnumDecl *enumDecl = en->getType().getEnumOrBoundGenericEnum();
    for (auto elemWithIndex : llvm::enumerate(enumDecl->getAllElements())) {
      mappedElements[elemWithIndex.index()] = elemWithIndex.value();
    }
    llvm::SmallVector<std::pair<swift::EnumElementDecl *, swift::SILBasicBlock *>, 16> convertedCases;
    for (auto c : cases) {
      assert(mappedElements.count(c.first) && "wrong enum element index");
      convertedCases.push_back({mappedElements[c.first], c.second.getBlock()});
    }
    return {builder.createSwitchEnum(swift::RegularLocation(loc.getLocation()),
                                     enumVal.getSILValue(),
                                     defaultBlock.getBlock(), convertedCases)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createUncheckedEnumData(BridgedValue enumVal, SwiftInt caseIdx,
                                             swift::SILType resultType) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    swift::SILValue en = enumVal.getSILValue();
    return {builder.createUncheckedEnumData(swift::RegularLocation(loc.getLocation()),
                                            enumVal.getSILValue(),
                                            en->getType().getEnumElement(caseIdx), resultType)};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createBranch(BridgedBasicBlock destBlock, BridgedValueArray arguments) const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    llvm::SmallVector<swift::SILValue, 16> argValues;
    return {builder.createBranch(swift::RegularLocation(loc.getLocation()),
                                 destBlock.getBlock(),
                                 arguments.getValues(argValues))};
  }

  SWIFT_IMPORT_UNSAFE
  BridgedInstruction createUnreachable() const {
    swift::SILBuilder builder(insertBefore.getInst(), insertAtEnd.getBlock(), loc.getScope());
    return {builder.createUnreachable(swift::RegularLocation(loc.getLocation()))};
  }
};

// AST bridging

struct BridgedNominalTypeDecl {
  swift::NominalTypeDecl * _Nonnull decl;
};

// Passmanager and Context

namespace swift {
  class SwiftPassInvocation;
}

struct BridgedChangeNotificationHandler {
  swift::SwiftPassInvocation * _Nonnull invocation;

  enum class Kind {
    instructionsChanged,
    callsChanged,
    branchesChanged,
    effectsChanged
  };

  void notifyChanges(Kind changeKind) const;
};

//===----------------------------------------------------------------------===//
//                             Inline functions
//===----------------------------------------------------------------------===//

OptionalBridgedOperand BridgedOperand::getNextUse() const {
  return {op->getNextUse()};
}

BridgedInstruction BridgedOperand::getUser() const {
  return {op->getUser()->asSILNode()};
}

OptionalBridgedOperand BridgedValue::getFirstUse() const {
  return {*getSILValue()->use_begin()};
}

OptionalBridgedSuccessor BridgedSuccessor::getNext() const {
  return {succ->getNext()};
}

BridgedInstruction BridgedSuccessor::getContainingInst() const {
  return {succ->getContainingInst()};
}

OptionalBridgedBasicBlock BridgedFunction::getFirstBlock() const {
  return {getFunction()->empty() ? nullptr : getFunction()->getEntryBlock()};
}

OptionalBridgedBasicBlock BridgedFunction::getLastBlock() const {
  return {getFunction()->empty() ? nullptr : &*getFunction()->rbegin()};
}

OptionalBridgedInstruction BridgedGlobalVar::getStaticInitializerValue() const {
  if (swift::SILInstruction *inst = getGlobal()->getStaticInitializerValue()) {
    return {inst->asSILNode()};
  }
  return {nullptr};
}

bool BridgedGlobalVar::isValidStaticInitializer(BridgedInstruction inst) {
  swift::SILInstruction *i = inst.getInst();
  return swift::SILGlobalVariable::isValidStaticInitializerInst(i, i->getModule());
}

BridgedInstruction BridgedMultiValueResult::getParent() const {
  return {getMVResult()->getParent()};
}

BridgedBasicBlock BridgedInstruction::getParent() const {
  assert(!getInst()->isStaticInitializerInst() &&
         "cannot get the parent of a static initializer instruction");
  return {getInst()->getParent()};
}

BridgedSuccessorArray BridgedInstruction::TermInst_getSuccessors() const {
  auto successors = getAs<swift::TermInst>()->getSuccessors();
  return {{successors.data()}, (SwiftInt)successors.size()};
}

BridgedBasicBlock BridgedInstruction::BranchInst_getTargetBlock() const {
  return {getAs<swift::BranchInst>()->getDestBB()};
}

void BridgedInstruction::TermInst_replaceBranchTarget(BridgedBasicBlock from, BridgedBasicBlock to) const {
  getAs<swift::TermInst>()->replaceBranchTarget(from.getBlock(), to.getBlock());
}

OptionalBridgedSuccessor BridgedBasicBlock::getFirstPred() const {
  return {getBlock()->pred_begin().getSuccessorRef()};
}

BridgedBasicBlock BridgedArgument::getParent() const {
  return {getArgument()->getParent()};
}


SWIFT_END_NULLABILITY_ANNOTATIONS

#endif
