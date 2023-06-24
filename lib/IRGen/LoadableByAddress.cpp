//===--- LoadableByAddress.cpp - Lower SIL address-only types. ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
// This pass lowers loadable SILTypes. On completion, the SILType of every
// function argument is an address instead of the type itself.
// This reduces the code size.
// Consequently, this pass is required for IRGen.
// It is a mandatory IRGen preparation pass (not a diagnostic pass).
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "loadable-address"
#include "FixedTypeInfo.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "NativeConventionSchema.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/IRGen/IRGenSILPasses.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::irgen;

static GenericEnvironment *getSubstGenericEnvironment(CanSILFunctionType fnTy) {
  return fnTy->getSubstGenericSignature().getGenericEnvironment();
}

static GenericEnvironment *getSubstGenericEnvironment(SILFunction *F) {
  if (F->getLoweredFunctionType()->isPolymorphic())
    return F->getGenericEnvironment();
  return getSubstGenericEnvironment(F->getLoweredFunctionType());
}

class LargeSILTypeMapper {
public:
  LargeSILTypeMapper() {}

public:
  SILType getNewSILType(GenericEnvironment *GenericEnv, SILType storageType,
                        irgen::IRGenModule &Mod);
  bool shouldTransformResults(GenericEnvironment *env,
                              CanSILFunctionType fnType,
                              irgen::IRGenModule &IGM);
  bool shouldTransformFunctionType(GenericEnvironment *env,
                                   CanSILFunctionType fnType,
                                   irgen::IRGenModule &IGM);
  SILParameterInfo getNewParameter(GenericEnvironment *env,
                                   SILParameterInfo param,
                                   irgen::IRGenModule &IGM);
  bool shouldTransformParameter(GenericEnvironment *env, SILParameterInfo param,
                                irgen::IRGenModule &IGM);
  SmallVector<SILParameterInfo, 4> getNewParameters(GenericEnvironment *env,
                                                    CanSILFunctionType fnType,
                                                    irgen::IRGenModule &IGM);
  SmallVector<SILYieldInfo, 2> getNewYields(GenericEnvironment *env,
                                            CanSILFunctionType fnType,
                                            irgen::IRGenModule &IGM);
  SmallVector<SILResultInfo, 2> getNewResults(GenericEnvironment *GenericEnv,
                                              CanSILFunctionType fnType,
                                              irgen::IRGenModule &Mod);
  CanSILFunctionType getNewSILFunctionType(GenericEnvironment *env,
                                           CanSILFunctionType fnType,
                                           irgen::IRGenModule &IGM);
  SILType getNewOptionalFunctionType(GenericEnvironment *GenericEnv,
                                     SILType storageType,
                                     irgen::IRGenModule &Mod);
  SILType getNewTupleType(GenericEnvironment *GenericEnv,
                          irgen::IRGenModule &Mod,
                          const SILType &nonOptionalType,
                          const SILType &storageType);
  bool newResultsDiffer(GenericEnvironment *GenericEnv,
                        ArrayRef<SILResultInfo> origResults,
                        irgen::IRGenModule &Mod);
  bool shouldConvertBBArg(SILArgument *arg, irgen::IRGenModule &Mod);
  bool containsDifferentFunctionSignature(GenericEnvironment *genEnv,
                                          irgen::IRGenModule &Mod,
                                          SILType storageType,
                                          SILType newSILType);

private:
  // Cache of already computed type transforms
  llvm::MapVector<std::pair<GenericEnvironment *, SILType>, SILType>
      oldToNewTypeMap;
};

/// Utility to determine if this is a large loadable type
static bool isLargeLoadableType(GenericEnvironment *GenericEnv, SILType t,
                                irgen::IRGenModule &Mod) {
  if (t.isAddress() || t.isClassOrClassMetatype()) {
    return false;
  }

  auto canType = t.getASTType();
  if (canType->hasTypeParameter()) {
    assert(GenericEnv && "Expected a GenericEnv");
    canType = GenericEnv->mapTypeIntoContext(canType)->getCanonicalType();
  }

  if (canType.getAnyGeneric()) {
    assert(t.isObject() && "Expected only two categories: address and object");
    assert(!canType->hasTypeParameter());
    const TypeInfo &TI = Mod.getTypeInfoForLowered(canType);
    auto &nativeSchemaOrigParam = TI.nativeParameterValueSchema(Mod);
    return nativeSchemaOrigParam.requiresIndirect();
  }
  return false;
}

static bool modifiableFunction(CanSILFunctionType funcType) {
  if (funcType->getLanguage() == SILFunctionLanguage::C) {
    // C functions should use the old ABI
    return false;
  }
  return true;
}

bool LargeSILTypeMapper::shouldTransformParameter(GenericEnvironment *env,
                                                  SILParameterInfo param,
                                                  irgen::IRGenModule &IGM) {
  auto newParam = getNewParameter(env, param, IGM);
  return (param != newParam);
}

static bool isFuncOrOptionalFuncType(SILType Ty) {
  SILType nonOptionalType = Ty;
  if (auto optType = Ty.getOptionalObjectType()) {
    nonOptionalType = optType;
  }
  return nonOptionalType.is<SILFunctionType>();
}

bool LargeSILTypeMapper::shouldTransformFunctionType(GenericEnvironment *env,
                                                     CanSILFunctionType fnType,
                                                     irgen::IRGenModule &IGM) {
  // Map substituted function types according to their substituted generic
  // signature.
  if (fnType->getPatternSubstitutions()) {
    env = getSubstGenericEnvironment(fnType);
  }

  if (shouldTransformResults(env, fnType, IGM))
    return true;

  for (auto param : fnType->getParameters()) {
    if (shouldTransformParameter(env, param, IGM))
      return true;
  }

  for (auto yield : fnType->getYields()) {
    if (shouldTransformParameter(env, yield, IGM))
      return true;
  }

  return false;
}

// Get the function type or the optional function type
static CanSILFunctionType getInnerFunctionType(SILType storageType) {
  if (auto currSILFunctionType = storageType.getAs<SILFunctionType>()) {
    return currSILFunctionType;
  }
  if (auto optionalType = storageType.getOptionalObjectType()) {
    if (auto currSILFunctionType = optionalType.getAs<SILFunctionType>()) {
      return currSILFunctionType;
    }
  }
  return CanSILFunctionType();
}

static SILType getNonOptionalType(SILType t) {
  SILType nonOptionalType = t;
  if (auto optType = t.getOptionalObjectType()) {
    nonOptionalType = optType;
  }
  return nonOptionalType;
}

bool LargeSILTypeMapper::containsDifferentFunctionSignature(
    GenericEnvironment *genEnv, irgen::IRGenModule &Mod, SILType storageType,
    SILType newSILType) {
  if (storageType == newSILType) {
    return false;
  }
  if (getInnerFunctionType(storageType)) {
    return true;
  }
  SILType nonOptionalType = getNonOptionalType(storageType);
  if (nonOptionalType.getAs<TupleType>()) {
    auto origType = nonOptionalType.getAs<TupleType>();
    for (TupleTypeElt canElem : origType->getElements()) {
      auto origCanType = CanType(canElem.getType());
      auto elem = SILType::getPrimitiveObjectType(origCanType);
      auto newElem = getNewSILType(genEnv, elem, Mod);
      if (containsDifferentFunctionSignature(genEnv, Mod, elem, newElem)) {
        return true;
      }
    }
  }
  return false;
}

bool LargeSILTypeMapper::newResultsDiffer(GenericEnvironment *GenericEnv,
                                          ArrayRef<SILResultInfo> origResults,
                                          irgen::IRGenModule &Mod) {
  SmallVector<SILResultInfo, 2> newResults;
  for (auto result : origResults) {
    SILType currResultTy = result.getSILStorageInterfaceType();
    SILType newSILType = getNewSILType(GenericEnv, currResultTy, Mod);
    // We (currently) only care about function signatures
    if (containsDifferentFunctionSignature(GenericEnv, Mod, currResultTy,
                                           newSILType)) {
      return true;
    }
  }
  return false;
}

static bool modNonFuncTypeResultType(GenericEnvironment *genEnv,
                                     CanSILFunctionType loweredTy,
                                     irgen::IRGenModule &Mod) {
  if (!modifiableFunction(loweredTy)) {
    return false;
  }
  if (loweredTy->getNumResults() != 1) {
    return false;
  }
  auto singleResult = loweredTy->getSingleResult();
  auto resultStorageType = singleResult.getSILStorageInterfaceType();
  if (isLargeLoadableType(genEnv, resultStorageType, Mod)) {
    return true;
  }
  return false;
}

SmallVector<SILResultInfo, 2>
LargeSILTypeMapper::getNewResults(GenericEnvironment *GenericEnv,
                                  CanSILFunctionType fnType,
                                  irgen::IRGenModule &Mod) {
  // Get new SIL Function results - same as old results UNLESS:
  // 1) Function type results might have a different signature
  // 2) Large loadables are replaced by @out version
  auto origResults = fnType->getResults();
  SmallVector<SILResultInfo, 2> newResults;
  for (auto result : origResults) {
    SILType currResultTy = result.getSILStorageInterfaceType();
    SILType newSILType = getNewSILType(GenericEnv, currResultTy, Mod);
    if (modNonFuncTypeResultType(GenericEnv, fnType, Mod)) {
      // Case (2) Above
      SILResultInfo newSILResultInfo(newSILType.getASTType(),
                                     ResultConvention::Indirect);
      newResults.push_back(newSILResultInfo);
    } else if (containsDifferentFunctionSignature(GenericEnv, Mod, currResultTy,
                                                  newSILType)) {
      // Case (1) Above
      SILResultInfo newResult(newSILType.getASTType(), result.getConvention());
      newResults.push_back(newResult);
    } else {
      newResults.push_back(result);
    }
  }
  return newResults;
}

CanSILFunctionType
LargeSILTypeMapper::getNewSILFunctionType(GenericEnvironment *env,
                                          CanSILFunctionType fnType,
                                          irgen::IRGenModule &IGM) {
  if (!modifiableFunction(fnType)) {
    return fnType;
  }
  
  // Map substituted function types according to their substituted generic
  // signature.
  if (fnType->getPatternSubstitutions()) {
    env = getSubstGenericEnvironment(fnType);
  }
  
  auto newParams = getNewParameters(env, fnType, IGM);
  auto newYields = getNewYields(env, fnType, IGM);
  auto newResults = getNewResults(env, fnType, IGM);
  auto newFnType = SILFunctionType::get(
      fnType->getInvocationGenericSignature(),
      fnType->getExtInfo(),
      fnType->getCoroutineKind(),
      fnType->getCalleeConvention(),
      newParams,
      newYields,
      newResults,
      fnType->getOptionalErrorResult(),
      fnType->getPatternSubstitutions(),
      fnType->getInvocationSubstitutions(),
      fnType->getASTContext(),
      fnType->getWitnessMethodConformanceOrInvalid());
  return newFnType;
}

SILType
LargeSILTypeMapper::getNewOptionalFunctionType(GenericEnvironment *GenericEnv,
                                               SILType storageType,
                                               irgen::IRGenModule &Mod) {
  SILType newSILType = storageType;
  if (auto objectType = storageType.getOptionalObjectType()) {
    if (auto fnType = objectType.getAs<SILFunctionType>()) {
      if (shouldTransformFunctionType(GenericEnv, fnType, Mod)) {
        auto newFnType = getNewSILFunctionType(GenericEnv, fnType, Mod);        
        newSILType =
          SILType::getPrimitiveType(newFnType, storageType.getCategory());
        newSILType = SILType::getOptionalType(newSILType);
      }
    }
  }
  return newSILType;
}

bool LargeSILTypeMapper::shouldTransformResults(GenericEnvironment *genEnv,
                                                CanSILFunctionType loweredTy,
                                                irgen::IRGenModule &Mod) {
  if (!modifiableFunction(loweredTy)) {
    return false;
  }

  if (loweredTy->getNumResults() != 1) {
    auto resultType = loweredTy->getAllResultsInterfaceType();
    auto newResultType = getNewSILType(genEnv, resultType, Mod);
    return resultType != newResultType;
  }

  auto singleResult = loweredTy->getSingleResult();
  auto resultStorageType = singleResult.getSILStorageInterfaceType();
  auto newResultStorageType = getNewSILType(genEnv, resultStorageType, Mod);
  if (resultStorageType != newResultStorageType) {
    return true;
  }
  return modNonFuncTypeResultType(genEnv, loweredTy, Mod);
}

static bool modResultType(SILFunction *F, irgen::IRGenModule &Mod,
                          LargeSILTypeMapper &Mapper) {
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  auto loweredTy = F->getLoweredFunctionType();

  return Mapper.shouldTransformResults(genEnv, loweredTy, Mod);
}

static bool shouldTransformYields(GenericEnvironment *genEnv,
                                  CanSILFunctionType loweredTy,
                                  irgen::IRGenModule &Mod,
                                  LargeSILTypeMapper &Mapper) {
  if (!modifiableFunction(loweredTy)) {
    return false;
  }
  for (auto &yield : loweredTy->getYields()) {
    auto yieldStorageType = yield.getSILStorageInterfaceType();
    auto newYieldStorageType =
        Mapper.getNewSILType(genEnv, yieldStorageType, Mod);
    if (yieldStorageType != newYieldStorageType)
      return true;
  }
  return false;
}

static bool modYieldType(SILFunction *F, irgen::IRGenModule &Mod,
                         LargeSILTypeMapper &Mapper) {
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  auto loweredTy = F->getLoweredFunctionType();

  return shouldTransformYields(genEnv, loweredTy, Mod, Mapper);
}

SILParameterInfo LargeSILTypeMapper::getNewParameter(GenericEnvironment *env,
                                                     SILParameterInfo param,
                                                     irgen::IRGenModule &IGM) {
  SILType storageType = param.getSILStorageInterfaceType();
  SILType newOptFuncType =
      getNewOptionalFunctionType(env, storageType, IGM);
  if (newOptFuncType != storageType) {
    return param.getWithInterfaceType(newOptFuncType.getASTType());
  }

  if (auto paramFnType = storageType.getAs<SILFunctionType>()) {
    if (shouldTransformFunctionType(env, paramFnType, IGM)) {
      auto newFnType = getNewSILFunctionType(env, paramFnType, IGM);
      return param.getWithInterfaceType(newFnType);
    } else {
      return param;
    }
  } else if (isLargeLoadableType(env, storageType, IGM)) {
    if (param.getConvention() == ParameterConvention::Direct_Guaranteed)
      return SILParameterInfo(storageType.getASTType(),
                              ParameterConvention::Indirect_In_Guaranteed,
                              param.getDifferentiability());
    else
      return SILParameterInfo(storageType.getASTType(),
                              ParameterConvention::Indirect_In,
                              param.getDifferentiability());
  } else {
    auto newType = getNewSILType(env, storageType, IGM);
    return SILParameterInfo(newType.getASTType(),
                            param.getConvention(),
                            param.getDifferentiability());
  }
}

SmallVector<SILParameterInfo, 4>
LargeSILTypeMapper::getNewParameters(GenericEnvironment *env,
                                     CanSILFunctionType fnType,
                                     irgen::IRGenModule &IGM) {
  SmallVector<SILParameterInfo, 4> newParams;
  for (SILParameterInfo param : fnType->getParameters()) {
    auto newParam = getNewParameter(env, param, IGM);
    newParams.push_back(newParam);
  }
  return newParams;
}

SmallVector<SILYieldInfo, 2>
LargeSILTypeMapper::getNewYields(GenericEnvironment *env,
                                 CanSILFunctionType fnType,
                                 irgen::IRGenModule &IGM) {
  SmallVector<SILYieldInfo, 2> newYields;
  for (auto oldYield : fnType->getYields()) {
    auto newYieldAsParam = getNewParameter(env, oldYield, IGM);
    newYields.push_back(SILYieldInfo(newYieldAsParam.getInterfaceType(),
                                     newYieldAsParam.getConvention()));
  }
  return newYields;
}

SILType LargeSILTypeMapper::getNewTupleType(GenericEnvironment *GenericEnv,
                                            irgen::IRGenModule &Mod,
                                            const SILType &nonOptionalType,
                                            const SILType &storageType) {
  auto origType = nonOptionalType.getAs<TupleType>();
  assert(origType && "Expected a tuple type");
  SmallVector<TupleTypeElt, 2> newElems;
  for (TupleTypeElt canElem : origType->getElements()) {
    auto origCanType = CanType(canElem.getType());
    auto elem = SILType::getPrimitiveObjectType(origCanType);
    auto newElem = getNewSILType(GenericEnv, elem, Mod);
    newElems.emplace_back(newElem.getASTType(), canElem.getName());
  }
  auto type = TupleType::get(newElems, nonOptionalType.getASTContext());
  auto canType = CanType(type);
  SILType newSILType = SILType::getPrimitiveObjectType(canType);
  if (nonOptionalType.isAddress()) {
    newSILType = newSILType.getAddressType();
  }
  if (nonOptionalType != storageType) {
    newSILType = SILType::getOptionalType(newSILType);
  }
  if (storageType.isAddress()) {
    newSILType = newSILType.getAddressType();
  }
  return newSILType;
}

SILType LargeSILTypeMapper::getNewSILType(GenericEnvironment *GenericEnv,
                                          SILType storageType,
                                          irgen::IRGenModule &Mod) {
  // See if the type is already in the cache:
  auto typePair = std::make_pair(GenericEnv, storageType);
  if (oldToNewTypeMap.find(typePair) != oldToNewTypeMap.end()) {
    return oldToNewTypeMap[typePair];
  }

  SILType nonOptionalType = storageType;
  if (auto optType = storageType.getOptionalObjectType()) {
    nonOptionalType = optType;
  }
  if (nonOptionalType.getAs<TupleType>()) {
    SILType newSILType =
        getNewTupleType(GenericEnv, Mod, nonOptionalType, storageType);
    auto typeToRet = isLargeLoadableType(GenericEnv, newSILType, Mod)
                         ? newSILType.getAddressType()
                         : newSILType;
    oldToNewTypeMap[typePair] = typeToRet;
    return typeToRet;
  }
  SILType newSILType = getNewOptionalFunctionType(GenericEnv, storageType, Mod);
  if (newSILType != storageType) {
    oldToNewTypeMap[typePair] = newSILType;
    return newSILType;
  }
  if (auto fnType = storageType.getAs<SILFunctionType>()) {
    if (shouldTransformFunctionType(GenericEnv, fnType, Mod)) {
      auto newFnType = getNewSILFunctionType(GenericEnv, fnType, Mod);
      newSILType = SILType::getPrimitiveType(newFnType,
                                             storageType.getCategory());
    }
  } else if (isLargeLoadableType(GenericEnv, storageType, Mod)) {
    newSILType = storageType.getAddressType();
  }
  oldToNewTypeMap[typePair] = newSILType;
  return newSILType;
}

//===----------------------------------------------------------------------===//
// StructLoweringState: shared state for the pass's analysis and transforms.
//===----------------------------------------------------------------------===//

namespace {
struct StructLoweringState {
  SILFunction *F;
  irgen::IRGenModule &Mod;
  LargeSILTypeMapper &Mapper;

  // All large loadable function arguments that we modified
  SmallVector<SILValue, 16> largeLoadableArgs;
  // All modified function signature function arguments
  SmallVector<SILValue, 16> funcSigArgs;
  // All args for which we did a load
  llvm::MapVector<SILValue, SILValue> argsToLoadedValueMap;
  // All applies for which we did an alloc
  llvm::MapVector<SILInstruction *, SILValue> applyRetToAllocMap;
  // reverse map of the one above
  llvm::MapVector<SILInstruction *, SILInstruction *> allocToApplyRetMap;
  // All call sites with SILArgument that needs to be re-written
  // Calls are removed from the set when rewritten.
  SmallVector<SILInstruction *, 16> applies;
  // All MethodInst that use the large struct
  SmallVector<MethodInst *, 16> methodInstsToMod;
  // Large loadable store instrs should call the outlined copy
  SmallVector<StoreInst *, 16> storeInstsToMod;
  // All switch_enum instrs that should be converted to switch_enum_addr
  SmallVector<SwitchEnumInst *, 16> switchEnumInstsToMod;
  // All struct_extract instrs that should be converted to struct_element_addr
  SmallVector<StructExtractInst *, 16> structExtractInstsToMod;
  // All tuple instructions for which the return type is a function type
  SmallVector<SingleValueInstruction *, 8> tupleInstsToMod;
  // All allock stack instructions to modify
  SmallVector<AllocStackInst *, 8> allocStackInstsToMod;
  // All pointer to address instructions to modify
  SmallVector<PointerToAddressInst *, 8> pointerToAddrkInstsToMod;
  // All Retain and release instrs should be replaced with _addr version
  SmallVector<RetainValueInst *, 16> retainInstsToMod;
  SmallVector<ReleaseValueInst *, 16> releaseInstsToMod;
  // All result types instrs for which we need to convert the ResultTy
  llvm::SetVector<SingleValueInstruction *> resultTyInstsToMod;
  // All instructions that use the large struct that are not covered above
  SmallVector<SILInstruction *, 16> instsToMod;
  // All function-exiting terminators (return or throw instructions).
  SmallVector<TermInst *, 8> returnInsts;
  // All (large type) return instructions that are modified
  SmallVector<ReturnInst *, 8> modReturnInsts;
  // All (large type) yield instructions that are modified
  SmallVector<YieldInst *, 8> modYieldInsts;
  // All destroy_value instrs should be replaced with _addr version
  SmallVector<SILInstruction *, 16> destroyValueInstsToMod;
  // All debug instructions.
  // to be modified *only if* the operands are used in "real" instructions
  SmallVector<DebugValueInst *, 16> debugInstsToMod;

  StructLoweringState(SILFunction *F, irgen::IRGenModule &Mod,
                      LargeSILTypeMapper &Mapper)
      : F(F), Mod(Mod), Mapper(Mapper) {}

  bool isLargeLoadableType(CanSILFunctionType fnTy, SILType type) {
    return ::isLargeLoadableType(getSubstGenericEnvironment(fnTy), type, Mod);
  }

  SILType getNewSILType(CanSILFunctionType fnTy, SILType type) {
    return Mapper.getNewSILType(getSubstGenericEnvironment(fnTy), type, Mod);
  }

  bool containsDifferentFunctionSignature(CanSILFunctionType fnTy,
                                          SILType type) {
    return Mapper.containsDifferentFunctionSignature(
        getSubstGenericEnvironment(fnTy), Mod, type, getNewSILType(fnTy, type));
  }

  bool hasLargeLoadableYields() {
    auto fnType = F->getLoweredFunctionType();
    if (!fnType->isCoroutine()) return false;

    auto env = getSubstGenericEnvironment(fnType);
    for (auto yield : fnType->getYields()) {
      if (Mapper.shouldTransformParameter(env, yield, Mod))
        return true;
    }
    return false;
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// LargeValueVisitor: Map large loadable values to ValueStorage.
//===----------------------------------------------------------------------===//

namespace {
class LargeValueVisitor {
  StructLoweringState &pass;
  PostOrderFunctionInfo postorderInfo;

public:
  explicit LargeValueVisitor(StructLoweringState &pass)
      : pass(pass), postorderInfo(pass.F) {}

  void mapReturnInstrs();
  void mapValueStorage();

protected:
  void visitApply(ApplySite applySite);
  void visitMethodInst(MethodInst *instr);
  void visitStoreInst(StoreInst *instr);
  void visitSwitchEnumInst(SwitchEnumInst *instr);
  void visitStructExtractInst(StructExtractInst *instr);
  void visitRetainInst(RetainValueInst *instr);
  void visitReleaseInst(ReleaseValueInst *instr);
  void visitResultTyInst(SingleValueInstruction *instr);
  void visitDebugValueInst(DebugValueInst *instr);
  void visitDestroyValueInst(DestroyValueInst *instr);
  void visitTupleInst(SingleValueInstruction *instr);
  void visitAllocStackInst(AllocStackInst *instr);
  void visitPointerToAddressInst(PointerToAddressInst *instr);
  void visitReturnInst(ReturnInst *instr);
  void visitYieldInst(YieldInst *instr);
  void visitDeallocInst(DeallocStackInst *instr);
  void visitInstr(SILInstruction *instr);
};
} // end anonymous namespace

void LargeValueVisitor::mapReturnInstrs() {
  for (auto *BB : postorderInfo.getReversePostOrder()) {
    if (BB->getTerminator()->isFunctionExiting())
      pass.returnInsts.push_back(BB->getTerminator());
  }
}

void LargeValueVisitor::mapValueStorage() {
  for (auto *BB : postorderInfo.getReversePostOrder()) {
    for (auto &II : *BB) {
      SILInstruction *currIns = &II;
      switch (currIns->getKind()) {
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst:
      case SILInstructionKind::BeginApplyInst:
      case SILInstructionKind::PartialApplyInst: {
        visitApply(ApplySite(currIns));
        break;
      }
      case SILInstructionKind::ClassMethodInst:
      case SILInstructionKind::SuperMethodInst:
      case SILInstructionKind::ObjCMethodInst:
      case SILInstructionKind::ObjCSuperMethodInst:
      case SILInstructionKind::WitnessMethodInst: {
        // TODO Any more instructions to add here?
        auto *MI = cast<MethodInst>(currIns);
        visitMethodInst(MI);
        break;
      }
      case SILInstructionKind::StructExtractInst:
      case SILInstructionKind::StructElementAddrInst:
      case SILInstructionKind::RefTailAddrInst:
      case SILInstructionKind::RefElementAddrInst:
      case SILInstructionKind::BeginAccessInst:
      case SILInstructionKind::EnumInst: {
        // TODO Any more instructions to add here?
        visitResultTyInst(cast<SingleValueInstruction>(currIns));
        break;
      }
      case SILInstructionKind::StoreInst: {
        auto *SI = cast<StoreInst>(currIns);
        visitStoreInst(SI);
        break;
      }
      case SILInstructionKind::RetainValueInst: {
        auto *RETI = cast<RetainValueInst>(currIns);
        visitRetainInst(RETI);
        break;
      }
      case SILInstructionKind::ReleaseValueInst: {
        auto *RELI = cast<ReleaseValueInst>(currIns);
        visitReleaseInst(RELI);
        break;
      }
      case SILInstructionKind::DebugValueInst: {
        auto *DI = cast<DebugValueInst>(currIns);
        visitDebugValueInst(DI);
        break;
      }
      case SILInstructionKind::DestroyValueInst: {
        auto *DI = cast<DestroyValueInst>(currIns);
        visitDestroyValueInst(DI);
        break;
      }
      case SILInstructionKind::SwitchEnumInst: {
        auto *SEI = cast<SwitchEnumInst>(currIns);
        visitSwitchEnumInst(SEI);
        break;
      }
      case SILInstructionKind::TupleElementAddrInst:
      case SILInstructionKind::TupleExtractInst: {
        visitTupleInst(cast<SingleValueInstruction>(currIns));
        break;
      }
      case SILInstructionKind::AllocStackInst: {
        auto *ASI = cast<AllocStackInst>(currIns);
        visitAllocStackInst(ASI);
        break;
      }
      case SILInstructionKind::PointerToAddressInst: {
        auto *PTA = cast<PointerToAddressInst>(currIns);
        visitPointerToAddressInst(PTA);
        break;
      }
      case SILInstructionKind::ReturnInst: {
        auto *RI = cast<ReturnInst>(currIns);
        visitReturnInst(RI);
        break;
      }
      case SILInstructionKind::YieldInst: {
        auto *YI = cast<YieldInst>(currIns);
        visitYieldInst(YI);
        break;
      }
      case SILInstructionKind::DeallocStackInst: {
        auto *DI = cast<DeallocStackInst>(currIns);
        visitDeallocInst(DI);
        break;
      }
      default: {
        assert(!ApplySite::isa(currIns) && "Did not expect an ApplySite");
        assert(!isa<MethodInst>(currIns) && "Unhandled Method Inst");
        visitInstr(currIns);
        break;
      }
      }
    }
  }
}

static bool modifiableApply(ApplySite applySite, irgen::IRGenModule &Mod) {
  // If the callee is a method then use the old ABI
  if (applySite.getSubstCalleeType()->getLanguage() == SILFunctionLanguage::C) {
    return false;
  }
  SILValue callee = applySite.getCallee();
  if (auto site = ApplySite::isa(callee)) {
    return modifiableApply(site, Mod);
  }
  return true;
}

void LargeValueVisitor::visitApply(ApplySite applySite) {
  if (!modifiableApply(applySite, pass.Mod)) {
    return visitInstr(applySite.getInstruction());
  }
  for (Operand &operand : applySite.getArgumentOperands()) {
    SILValue currOperand = operand.get();
    SILType silType = currOperand->getType();
    SILType newSilType = pass.getNewSILType(applySite.getSubstCalleeType(),
                                            silType);
    if (silType != newSilType ||
        std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                  currOperand) != pass.largeLoadableArgs.end() ||
        std::find(pass.funcSigArgs.begin(), pass.funcSigArgs.end(),
                  currOperand) != pass.funcSigArgs.end()) {
      pass.applies.push_back(applySite.getInstruction());
      return;
    }
  }

  // For coroutines, we need to consider the yields, not the direct result
  // (which should always be void).
  if (auto beginApply = dyn_cast<BeginApplyInst>(applySite)) {
    for (auto yield : beginApply->getYieldedValues()) {
      auto oldYieldType = yield->getType();
      auto newYieldType = pass.getNewSILType(beginApply->getSubstCalleeType(),
                                             oldYieldType);
      if (oldYieldType != newYieldType) {
        pass.applies.push_back(applySite.getInstruction());
        return;
      }
    }
    return;
  }

  SILType currType = applySite.getType();
  SILType newType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                       currType);
  // We only care about function type results
  if (!pass.isLargeLoadableType(pass.F->getLoweredFunctionType(),
                                currType) &&
      (currType != newType)) {
    pass.applies.push_back(applySite.getInstruction());
    return;
  }
  // Check callee - need new generic env:
  CanSILFunctionType origSILFunctionType = applySite.getSubstCalleeType();
  GenericEnvironment *genEnvCallee = nullptr;
  auto newSILFunctionType = pass.Mapper.getNewSILFunctionType(
      genEnvCallee, origSILFunctionType, pass.Mod);
  if (origSILFunctionType != newSILFunctionType) {
    pass.applies.push_back(applySite.getInstruction());
  }
}

static bool isMethodInstUnmodifiable(MethodInst *instr) {
  for (auto *user : instr->getUses()) {
    if (ApplySite::isa(user->getUser())) {
      ApplySite applySite = ApplySite(user->getUser());
      if (applySite.getSubstCalleeType()->getLanguage() ==
          SILFunctionLanguage::C) {
        return true;
      }
    }
  }
  return false;
}

void LargeValueVisitor::visitMethodInst(MethodInst *instr) {
  if (isMethodInstUnmodifiable(instr)) {
    // Do not change the method!
    visitInstr(instr);
    return;
  }
  SILType currSILType = instr->getType();
  auto fnType = currSILType.castTo<SILFunctionType>();

  GenericEnvironment *genEnv = nullptr;
  if (fnType->isPolymorphic()) {
    genEnv = getSubstGenericEnvironment(fnType);
  }
  if (pass.Mapper.shouldTransformFunctionType(genEnv, fnType, pass.Mod)) {
    pass.methodInstsToMod.push_back(instr);
    return;
  }
  if (pass.Mapper.newResultsDiffer(genEnv, fnType->getResults(), pass.Mod)) {
    pass.methodInstsToMod.push_back(instr);
  }
}

void LargeValueVisitor::visitStoreInst(StoreInst *instr) {
  SILValue src = instr->getSrc();
  if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                src) != pass.largeLoadableArgs.end()) {
    pass.storeInstsToMod.push_back(instr);
  }
}

bool LargeSILTypeMapper::shouldConvertBBArg(SILArgument *arg,
                                            irgen::IRGenModule &Mod) {
  auto *F = arg->getFunction();
  SILType storageType = arg->getType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  auto currCanType = storageType.getASTType();
  if (auto funcType = dyn_cast<SILFunctionType>(currCanType)) {
    if (funcType->isPolymorphic()) {
      genEnv = getSubstGenericEnvironment(funcType);
    }
  }
  SILType newSILType = getNewSILType(genEnv, storageType, Mod);
  // We (currently) only care about function signatures
  if (containsDifferentFunctionSignature(genEnv, Mod, storageType,
                                         newSILType)) {
    return true;
  }
  return false;
}

void LargeValueVisitor::visitSwitchEnumInst(SwitchEnumInst *instr) {
  SILValue operand = instr->getOperand();
  if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                operand) != pass.largeLoadableArgs.end()) {
    pass.switchEnumInstsToMod.push_back(instr);
    return;
  }
  // In case we converted the target BB type of this enum,
  // to an address based one - need to modify
  unsigned numOfCases = instr->getNumCases();
  SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 16> caseBBs;
  for (unsigned i = 0; i < numOfCases; ++i) {
    auto currCase = instr->getCase(i);
    auto *currBB = currCase.second;
    for (SILArgument *arg : currBB->getArguments()) {
      if (pass.Mapper.shouldConvertBBArg(arg, pass.Mod)) {
        SILType storageType = arg->getType();
        SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                                storageType);
        if (newSILType.isAddress()) {
          pass.switchEnumInstsToMod.push_back(instr);
          return;
        }
      }
    }
  }
}

void LargeValueVisitor::visitStructExtractInst(StructExtractInst *instr) {
  SILValue operand = instr->getOperand();
  if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                operand) != pass.largeLoadableArgs.end()) {
    pass.structExtractInstsToMod.push_back(instr);
  }
}

void LargeValueVisitor::visitRetainInst(RetainValueInst *instr) {
  for (Operand &operand : instr->getAllOperands()) {
    if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                  operand.get()) != pass.largeLoadableArgs.end()) {
      pass.retainInstsToMod.push_back(instr);
      return;
    }
  }
}

void LargeValueVisitor::visitReleaseInst(ReleaseValueInst *instr) {
  for (Operand &operand : instr->getAllOperands()) {
    if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                  operand.get()) != pass.largeLoadableArgs.end()) {
      pass.releaseInstsToMod.push_back(instr);
      return;
    }
  }
}

void LargeValueVisitor::visitDebugValueInst(DebugValueInst *instr) {
  for (Operand &operand : instr->getAllOperands()) {
    if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                  operand.get()) != pass.largeLoadableArgs.end()) {
      pass.debugInstsToMod.push_back(instr);
    }
  }
}

void LargeValueVisitor::visitDestroyValueInst(DestroyValueInst *instr) {
  for (Operand &operand : instr->getAllOperands()) {
    if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                  operand.get()) != pass.largeLoadableArgs.end()) {
      pass.destroyValueInstsToMod.push_back(instr);
    }
  }
}

void LargeValueVisitor::visitResultTyInst(SingleValueInstruction *instr) {
  SILType currSILType = instr->getType().getObjectType();
  SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                          currSILType);
  if (currSILType != newSILType) {
    pass.resultTyInstsToMod.insert(instr);
  }
  auto *SEI = dyn_cast<StructExtractInst>(instr);
  if (SEI) {
    visitStructExtractInst(SEI);
  } else {
    visitInstr(instr);
  }
}

void LargeValueVisitor::visitTupleInst(SingleValueInstruction *instr) {
  SILType currSILType = instr->getType().getObjectType();
  if (auto funcType = getInnerFunctionType(currSILType)) {
    GenericEnvironment *genEnv = getSubstGenericEnvironment(instr->getFunction());
    if (!genEnv && funcType->isPolymorphic()) {
      genEnv = getSubstGenericEnvironment(funcType);
    }
    auto newSILFunctionType =
        pass.Mapper.getNewSILFunctionType(genEnv, funcType, pass.Mod);
    if (funcType != newSILFunctionType) {
      pass.tupleInstsToMod.push_back(instr);
    }
  }
  visitInstr(instr);
}

void LargeValueVisitor::visitAllocStackInst(AllocStackInst *instr) {
  SILType currSILType = instr->getType().getObjectType();
  if (pass.containsDifferentFunctionSignature(pass.F->getLoweredFunctionType(),
                                              currSILType)) {
    pass.allocStackInstsToMod.push_back(instr);
  }
}

void LargeValueVisitor::visitPointerToAddressInst(PointerToAddressInst *instr) {
  SILType currSILType = instr->getType().getObjectType();
  if (getInnerFunctionType(currSILType)) {
    pass.pointerToAddrkInstsToMod.push_back(instr);
  }
}

static bool modNonFuncTypeResultType(SILFunction *F, irgen::IRGenModule &Mod) {
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  auto loweredTy = F->getLoweredFunctionType();
  return modNonFuncTypeResultType(genEnv, loweredTy, Mod);
}

void LargeValueVisitor::visitReturnInst(ReturnInst *instr) {
  if (!modResultType(pass.F, pass.Mod, pass.Mapper)) {
    visitInstr(instr);
  } else if (modNonFuncTypeResultType(pass.F, pass.Mod)) {
    pass.modReturnInsts.push_back(instr);
  } // else: function signature return instructions remain as-is
}

void LargeValueVisitor::visitYieldInst(YieldInst *instr) {
  if (!modYieldType(pass.F, pass.Mod, pass.Mapper)) {
    visitInstr(instr);
  } else {
    pass.modYieldInsts.push_back(instr);
  } // else: function signature return instructions remain as-is
}

void LargeValueVisitor::visitDeallocInst(DeallocStackInst *instr) {
  auto opInstr = instr->getOperand();
  if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                opInstr) != pass.largeLoadableArgs.end()) {
    auto *opAsInstr = dyn_cast<AllocStackInst>(opInstr);
    assert(opAsInstr && "Expected an alloc stack instruction");
    assert(pass.allocToApplyRetMap.find(opAsInstr) !=
               pass.allocToApplyRetMap.end() &&
           "Unexpected dealloc instr!");
    (void)opAsInstr;
  }
}

void LargeValueVisitor::visitInstr(SILInstruction *instr) {
  for (Operand &operand : instr->getAllOperands()) {
    if (std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                  operand.get()) != pass.largeLoadableArgs.end()) {
      pass.instsToMod.push_back(instr);
      // will be replaced later by the load / alloc_stack:
      pass.argsToLoadedValueMap[operand.get()] = operand.get();
    }
  }
}

//===----------------------------------------------------------------------===//
// LoadableStorageAllocation: Generate alloc_stack and address projections
// for all loadable types we pass around.
//===----------------------------------------------------------------------===//

namespace {
class LoadableStorageAllocation {
public:
  StructLoweringState &pass;

  explicit LoadableStorageAllocation(StructLoweringState &pass) : pass(pass) {}

  void allocateLoadableStorage();
  void replaceLoad(LoadInst *unoptimizableLoad);

protected:
  void replaceLoadWithCopyAddr(LoadInst *optimizableLoad);
  void replaceLoadWithCopyAddrForModifiable(LoadInst *unoptimizableLoad);
  void convertIndirectFunctionArgs();
  void insertIndirectReturnArgs();
  void convertIndirectFunctionPointerArgsForUnmodifiable();
  void convertIndirectBasicBlockArgs();
  void convertApplyResults();
  void allocateForArg(SILValue value);
  AllocStackInst *allocateForApply(SILInstruction *apply, SILType type);
  SILArgument *replaceArgType(SILBuilder &argBuilder, SILArgument *arg,
                              SILType newSILType);
};
} // end anonymous namespace

static AllocStackInst *allocate(StructLoweringState &pass, SILType type) {
  assert(type.isObject());

  // Insert an alloc_stack at the beginning of the function.
  SILBuilderWithScope allocBuilder(&*pass.F->begin());
  // Don't put any variable debug info into the alloc_stack, there will be a
  // debug_value inserted later. TODO: It may be more elegant to insert
  // the variable info into the alloc_stack instead of additionally generating a
  // debug_value.
  AllocStackInst *alloc = allocBuilder.createAllocStack(
      RegularLocation::getAutoGeneratedLocation(), type);

  // Insert dealloc_stack at the end(s) of the function.
  for (TermInst *termInst : pass.returnInsts) {
    SILBuilderWithScope deallocBuilder(termInst);
    deallocBuilder.createDeallocStack(
        RegularLocation::getAutoGeneratedLocation(), alloc);
  }

  return alloc;
}

static StoreOwnershipQualifier
getStoreInitOwnership(StructLoweringState &pass, SILType type) {
  if (!pass.F->hasOwnership()) {
    return StoreOwnershipQualifier::Unqualified;
  } else if (type.isTrivial(*pass.F)) {
    return StoreOwnershipQualifier::Trivial;
  } else {
    return StoreOwnershipQualifier::Init;
  }
}

static StoreInst *createStoreInit(StructLoweringState &pass,
                                  SILBasicBlock::iterator where,
                                  SILLocation loc,
                                  SILValue value, SILValue address) {
  SILBuilderWithScope storeBuilder(where);
  return storeBuilder.createStore(loc, value, address,
                                 getStoreInitOwnership(pass, value->getType()));
}

static SILInstruction *createOutlinedCopyCall(SILBuilder &copyBuilder,
                                              SILValue src, SILValue tgt,
                                              StructLoweringState &pass,
                                              SILLocation *loc = nullptr) {
  SILLocation locToUse = loc ? *loc : copyBuilder.getInsertionPoint()->getLoc();
  auto *copy =
      copyBuilder.createCopyAddr(locToUse, src, tgt, IsTake, IsInitialization);
  return copy;
}

void LoadableStorageAllocation::replaceLoadWithCopyAddr(
    LoadInst *optimizableLoad) {
  SILValue value = optimizableLoad->getOperand();

  auto allocInstr = allocate(pass, value->getType().getObjectType());

  SILBuilderWithScope outlinedBuilder(optimizableLoad);
  createOutlinedCopyCall(outlinedBuilder, value, allocInstr, pass);

  for (auto *user : optimizableLoad->getUses()) {
    SILInstruction *userIns = user->getUser();
    switch (userIns->getKind()) {
    case SILInstructionKind::CopyAddrInst:
    case SILInstructionKind::DeallocStackInst:
      break;
    case SILInstructionKind::ApplyInst:
    case SILInstructionKind::TryApplyInst:
    case SILInstructionKind::BeginApplyInst:
    case SILInstructionKind::PartialApplyInst: {
      if (std::find(pass.applies.begin(), pass.applies.end(), userIns) ==
          pass.applies.end()) {
        pass.applies.push_back(userIns);
      }
      break;
    }
    case SILInstructionKind::YieldInst:
      // The rewrite is enough.
      break;
    case SILInstructionKind::RetainValueInst: {
      auto *insToInsert = cast<RetainValueInst>(userIns);
      pass.retainInstsToMod.push_back(insToInsert);
      break;
    }
    case SILInstructionKind::ReleaseValueInst: {
      auto *insToInsert = cast<ReleaseValueInst>(userIns);
      pass.releaseInstsToMod.push_back(insToInsert);
      break;
    }
    case SILInstructionKind::StoreInst: {
      auto *insToInsert = cast<StoreInst>(userIns);
      pass.storeInstsToMod.push_back(insToInsert);
      break;
    }
    case SILInstructionKind::DebugValueInst: {
      auto *insToInsert = cast<DebugValueInst>(userIns);
      pass.debugInstsToMod.push_back(insToInsert);
      break;
    }
    case SILInstructionKind::DestroyValueInst: {
      auto *insToInsert = cast<DestroyValueInst>(userIns);
      pass.destroyValueInstsToMod.push_back(insToInsert);
      break;
    }
    case SILInstructionKind::StructExtractInst: {
      auto *instToInsert = cast<StructExtractInst>(userIns);
      if (std::find(pass.structExtractInstsToMod.begin(),
                    pass.structExtractInstsToMod.end(),
                    instToInsert) == pass.structExtractInstsToMod.end()) {
        pass.structExtractInstsToMod.push_back(instToInsert);
      }
      break;
    }
    case SILInstructionKind::SwitchEnumInst: {
      auto *instToInsert = cast<SwitchEnumInst>(userIns);
      if (std::find(pass.switchEnumInstsToMod.begin(),
                    pass.switchEnumInstsToMod.end(),
                    instToInsert) == pass.switchEnumInstsToMod.end()) {
        pass.switchEnumInstsToMod.push_back(instToInsert);
      }
      break;
    }
    default:
      llvm_unreachable("Unexpected instruction");
    }
  }

  optimizableLoad->replaceAllUsesWith(allocInstr);
  optimizableLoad->getParent()->erase(optimizableLoad);
}

static bool isYieldUseRewritable(StructLoweringState &pass,
                                  YieldInst *inst, Operand *operand) {
  assert(inst == operand->getUser());
  return pass.isLargeLoadableType(pass.F->getLoweredFunctionType(),
                                  operand->get()->getType());
}

/// Does the value's uses contain instructions that *must* be rewrites?
static bool hasMandatoryRewriteUse(StructLoweringState &pass,
                                   SILValue value) {
  for (auto *user : value->getUses()) {
    SILInstruction *userIns = user->getUser();
    switch (userIns->getKind()) {
    case SILInstructionKind::ApplyInst:
    case SILInstructionKind::TryApplyInst:
    case SILInstructionKind::BeginApplyInst:
    case SILInstructionKind::PartialApplyInst: {
      ApplySite site(userIns);
      SILValue callee = site.getCallee();
      if (callee == value) {
        break;
      }
      SILType currType = value->getType().getObjectType();
      SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                              currType);
      if (currType == newSILType) {
        break;
      }
      return true;
    }
    case SILInstructionKind::YieldInst:
      if (isYieldUseRewritable(pass, cast<YieldInst>(userIns), user))
        return true;
      break;
    default:
      break;
    }
  }
  return false;
}

void LoadableStorageAllocation::replaceLoadWithCopyAddrForModifiable(
    LoadInst *unoptimizableLoad) {
  if (!hasMandatoryRewriteUse(pass, unoptimizableLoad)) {
    return;
  }
  SILValue value = unoptimizableLoad->getOperand();

  AllocStackInst *alloc = allocate(pass, value->getType().getObjectType());

  SILBuilderWithScope outlinedBuilder(unoptimizableLoad);
  createOutlinedCopyCall(outlinedBuilder, value, alloc, pass);

  SmallVector<Operand *, 8> usesToMod;
  for (auto *use : unoptimizableLoad->getUses()) {
    SILInstruction *userIns = use->getUser();
    switch (userIns->getKind()) {
    case SILInstructionKind::CopyAddrInst:
    case SILInstructionKind::DeallocStackInst:
      break;
    case SILInstructionKind::ApplyInst:
    case SILInstructionKind::TryApplyInst:
    case SILInstructionKind::BeginApplyInst:
    case SILInstructionKind::PartialApplyInst: {
      ApplySite site(userIns);
      if (!modifiableApply(site, pass.Mod)) {
        break;
      }
      SILValue callee = site.getCallee();
      if (callee == unoptimizableLoad) {
        break;
      }
      SILType currType = unoptimizableLoad->getType().getObjectType();
      SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                              currType);
      if (currType == newSILType) {
        break;
      }
      if (std::find(pass.applies.begin(), pass.applies.end(), userIns) ==
          pass.applies.end()) {
        pass.applies.push_back(userIns);
      }
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::YieldInst: {
      if (isYieldUseRewritable(pass, cast<YieldInst>(userIns), use))
        usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::RetainValueInst: {
      auto *insToInsert = cast<RetainValueInst>(userIns);
      pass.retainInstsToMod.push_back(insToInsert);
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::ReleaseValueInst: {
      auto *insToInsert = cast<ReleaseValueInst>(userIns);
      pass.releaseInstsToMod.push_back(insToInsert);
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::StoreInst: {
      auto *insToInsert = cast<StoreInst>(userIns);
      pass.storeInstsToMod.push_back(insToInsert);
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::DebugValueInst: {
      auto *insToInsert = cast<DebugValueInst>(userIns);
      pass.debugInstsToMod.push_back(insToInsert);
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::DestroyValueInst: {
      auto *insToInsert = cast<DestroyValueInst>(userIns);
      pass.destroyValueInstsToMod.push_back(insToInsert);
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::StructExtractInst: {
      auto *instToInsert = cast<StructExtractInst>(userIns);
      pass.structExtractInstsToMod.push_back(instToInsert);
      usesToMod.push_back(use);
      break;
    }
    case SILInstructionKind::SwitchEnumInst: {
      auto *instToInsert = cast<SwitchEnumInst>(userIns);
      pass.switchEnumInstsToMod.push_back(instToInsert);
      usesToMod.push_back(use);
      break;
    }
    default:
      break;
    }
  }
  while (!usesToMod.empty()) {
    auto *use = usesToMod.pop_back_val();
    use->set(alloc);
  }
}

void LoadableStorageAllocation::allocateLoadableStorage() {
  // We need to map all functions exits
  // required for Apply result's allocations
  // Else we might get the following error:
  // "stack dealloc does not match most recent stack alloc"
  // When we dealloc later
  LargeValueVisitor(pass).mapReturnInstrs();
  if (modifiableFunction(pass.F->getLoweredFunctionType())) {
    // Turn by-value function args to by-address ones
    convertIndirectFunctionArgs();
  } else {
    convertIndirectFunctionPointerArgsForUnmodifiable();
  }
  convertApplyResults();

  // Populate the pass' data structs
  LargeValueVisitor(pass).mapValueStorage();

  // Turn by-value BB args to by-address ones
  convertIndirectBasicBlockArgs();

  // Create an AllocStack for every used large loadable type in the function.
  for (auto &argToAlloc : pass.argsToLoadedValueMap) {
    assert(argToAlloc.first == argToAlloc.second);
    allocateForArg(argToAlloc.first);
  }
}

SILArgument *LoadableStorageAllocation::replaceArgType(SILBuilder &argBuilder,
                                                       SILArgument *arg,
                                                       SILType newSILType) {
  SILValue undef = SILUndef::get(newSILType, *pass.F);
  SmallVector<Operand *, 8> useList(arg->use_begin(), arg->use_end());
  for (auto *use : useList) {
    use->set(undef);
  }

  // Make sure that this is an argument we want to replace.
  assert(std::find(pass.largeLoadableArgs.begin(), pass.largeLoadableArgs.end(),
                   arg) == pass.largeLoadableArgs.end());

  arg = arg->getParent()->replaceFunctionArgument(
      arg->getIndex(), newSILType, OwnershipKind::None, arg->getDecl());

  for (auto *use : useList) {
    use->set(arg);
  }

  return arg;
}

void LoadableStorageAllocation::insertIndirectReturnArgs() {
  GenericEnvironment *genEnv = getSubstGenericEnvironment(pass.F);
  auto loweredTy = pass.F->getLoweredFunctionType();
  SILType resultStorageType = loweredTy->getAllResultsSubstType(pass.F->getModule(),
                                                                pass.F->getTypeExpansionContext());
  auto canType = resultStorageType.getASTType();
  if (canType->hasTypeParameter()) {
    assert(genEnv && "Expected a GenericEnv");
    canType = genEnv->mapTypeIntoContext(canType)->getCanonicalType();
  }
  resultStorageType = SILType::getPrimitiveObjectType(canType);
  auto newResultStorageType =
      pass.F->getLoweredType(pass.getNewSILType(loweredTy, resultStorageType));

  auto &ctx = pass.F->getModule().getASTContext();
  auto var = new (ctx) ParamDecl(
      SourceLoc(), SourceLoc(),
      ctx.getIdentifier("$return_value"), SourceLoc(),
      ctx.getIdentifier("$return_value"),
      pass.F->getDeclContext());
  var->setSpecifier(ParamSpecifier::InOut);
  pass.F->begin()->insertFunctionArgument(
      0, newResultStorageType.getAddressType(), OwnershipKind::None, var);
}

void LoadableStorageAllocation::convertIndirectFunctionArgs() {
  SILBasicBlock *entry = pass.F->getEntryBlock();
  SILBuilderWithScope argBuilder(entry->begin());

  for (SILArgument *arg : entry->getArguments()) {
    SILType storageType = arg->getType();
    SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                            storageType);
    if (newSILType != storageType) {
      ValueOwnershipKind ownership = arg->getOwnershipKind();
      arg = replaceArgType(argBuilder, arg, newSILType);
      if (pass.isLargeLoadableType(pass.F->getLoweredFunctionType(),
                                   storageType)) {
        // Add to largeLoadableArgs if and only if it wasn't a modified function
        // signature arg
        pass.largeLoadableArgs.push_back(arg);
      } else {
        arg->setOwnershipKind(ownership);
        pass.funcSigArgs.push_back(arg);
      }
    }
  }

  // Convert the result type to indirect if necessary:
  if (modNonFuncTypeResultType(pass.F, pass.Mod)) {
    insertIndirectReturnArgs();
  }
}

static void convertBBArgType(SILBuilder &argBuilder, SILType newSILType,
                             SILArgument *arg) {
  SILValue undef = SILUndef::get(newSILType, argBuilder.getFunction());
  SmallVector<Operand *, 8> useList(arg->use_begin(), arg->use_end());
  for (auto *use : useList) {
    use->set(undef);
  }

  arg = arg->getParent()->replacePhiArgument(arg->getIndex(), newSILType,
                                             arg->getOwnershipKind());
  for (auto *use : useList) {
    use->set(arg);
  }
}

static bool containsFunctionType(CanType ty) {
  if (auto tuple = dyn_cast<TupleType>(ty)) {
    for (auto elt : tuple.getElementTypes()) {
      if (containsFunctionType(elt))
        return true;
    }
    return false;
  }
  if (auto optionalType = ty.getOptionalObjectType()) {
    return containsFunctionType(optionalType);
  }
  return isa<SILFunctionType>(ty);
}

void LoadableStorageAllocation::convertApplyResults() {
  for (auto &BB : *pass.F) {
    for (auto &II : BB) {
      auto *currIns = &II;
      auto applySite = FullApplySite::isa(currIns);
      if (!applySite) {
        continue;
      }
      if (!modifiableApply(applySite, pass.Mod)) {
        continue;
      }

      CanSILFunctionType origSILFunctionType = applySite.getSubstCalleeType();
      GenericEnvironment *genEnv = getSubstGenericEnvironment(origSILFunctionType);
      if (!pass.Mapper.shouldTransformResults(genEnv, origSILFunctionType,
                                              pass.Mod)) {
        continue;
      }
      auto resultStorageType = origSILFunctionType->getAllResultsInterfaceType();
      if (!pass.isLargeLoadableType(origSILFunctionType, resultStorageType)) {
        // Make sure it contains a function type
        auto numFuncTy = llvm::count_if(origSILFunctionType->getResults(),
            [](const SILResultInfo &origResult) {
              auto resultStorageTy = origResult.getSILStorageInterfaceType();
              return containsFunctionType(resultStorageTy.getASTType());
            });
        assert(numFuncTy != 0 &&
               "Expected a SILFunctionType inside the result Type");
        (void)numFuncTy;
        continue;
      }
      auto resultContextTy = origSILFunctionType->substInterfaceType(
          pass.F->getModule(), resultStorageType,
          pass.F->getTypeExpansionContext());
      auto newSILType = pass.getNewSILType(origSILFunctionType,
                                           resultContextTy);
      auto *newVal = allocateForApply(currIns, newSILType.getObjectType());
      if (auto apply = dyn_cast<ApplyInst>(currIns)) {
        apply->replaceAllUsesWith(newVal);
      } else {
        auto tryApplyIns = cast<TryApplyInst>(currIns);
        auto *normalBB = tryApplyIns->getNormalBB();
        SILBuilderWithScope argBuilder(normalBB->begin());
        assert(normalBB->getNumArguments() == 1 &&
               "Expected only one arg for try_apply normal BB");
        auto arg = normalBB->getArgument(0);
        arg->replaceAllUsesWith(newVal);
        auto emptyTy = SILType::getPrimitiveObjectType(
            TupleType::getEmpty(argBuilder.getModule().getASTContext()));
        convertBBArgType(argBuilder, emptyTy, arg);
      }
    }
  }
}

void LoadableStorageAllocation::
    convertIndirectFunctionPointerArgsForUnmodifiable() {
  SILBasicBlock *entry = pass.F->getEntryBlock();
  SILBuilderWithScope argBuilder(entry->begin());

  for (SILArgument *arg : entry->getArguments()) {
    SILType storageType = arg->getType();
    SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                            storageType);
    if (pass.containsDifferentFunctionSignature(pass.F->getLoweredFunctionType(),
                                                storageType)) {
      auto *castInstr = argBuilder.createUncheckedReinterpretCast(
          RegularLocation(const_cast<ValueDecl *>(arg->getDecl())), arg,
          newSILType);
      arg->replaceAllUsesWith(castInstr);
      castInstr->setOperand(0, arg);
    }
  }
}

void LoadableStorageAllocation::convertIndirectBasicBlockArgs() {
  SILBasicBlock *entry = pass.F->getEntryBlock();
  for (SILBasicBlock &BB : *pass.F) {
    if (&BB == entry) {
      // Already took care of function args
      continue;
    }
    SILBuilderWithScope argBuilder(BB.begin());
    for (SILArgument *arg : BB.getArguments()) {
      if (!pass.Mapper.shouldConvertBBArg(arg, pass.Mod)) {
        continue;
      }
      SILType storageType = arg->getType();
      SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                              storageType);
      // We don't change the type from object to address for function args:
      // a tuple with both a large type and a function arg should remain
      // as an object type for now
      if (storageType.isObject()) {
        newSILType = newSILType.getObjectType();
      }
      convertBBArgType(argBuilder, newSILType, arg);
    }
  }
}

void LoadableStorageAllocation::allocateForArg(SILValue value) {
  if (auto *allocInstr = dyn_cast<AllocStackInst>(value)) {
    // Special case: the value was already an Alloc
    // This happens in case of values from apply results (for example)
    // we *should* add a load for the current uses.
    // Said load should happen before the first use
    // As such add it right after the apply()
    LoadInst *load = nullptr;
    assert(pass.allocToApplyRetMap.find(allocInstr) !=
               pass.allocToApplyRetMap.end() &&
           "Alloc is not for apply results");
    auto *applyInst = pass.allocToApplyRetMap[allocInstr];
    assert(applyInst && "Value is not an apply");
    auto II = applyInst->getIterator();
    SILBuilderWithScope loadBuilder(II);
    if (auto *tryApply = dyn_cast<TryApplyInst>(applyInst)) {
      auto *tgtBB = tryApply->getNormalBB();
      assert(tgtBB && "Could not find try apply's target BB");
      loadBuilder.setInsertionPoint(tgtBB->begin());
    } else {
      ++II;
      loadBuilder.setInsertionPoint(II);
    }
    if (!pass.F->hasOwnership()) {
      load = loadBuilder.createLoad(applyInst->getLoc(), value,
                                    LoadOwnershipQualifier::Unqualified);
    } else {
      load = loadBuilder.createLoad(applyInst->getLoc(), value,
                                    LoadOwnershipQualifier::Take);
    }
    pass.argsToLoadedValueMap[value] = load;
    return;
  }

  assert(!ApplySite::isa(value) && "Unexpected instruction");

  // Find the first non-AllocStackInst and use its scope when creating
  // the new SILBuilder. An AllocStackInst does not directly cause any
  // code to be generated. The location of an AllocStackInst carries information
  // about the source variable; it doesn't matter where in the instruction
  // stream the AllocStackInst is located.
  auto BBIter = pass.F->begin()->begin();
  SILInstruction *FirstNonAllocStack = &*BBIter;
  while (isa<AllocStackInst>(FirstNonAllocStack) &&
         BBIter != pass.F->begin()->end()) {
    ++BBIter;
    FirstNonAllocStack = &*BBIter;
  }
  SILBuilderWithScope allocBuilder(&*pass.F->begin()->begin(),
                                   FirstNonAllocStack);

  AllocStackInst *allocInstr = allocBuilder.createAllocStack(
      RegularLocation::getAutoGeneratedLocation(), value->getType());

  LoadInst *loadCopy = nullptr;
  auto *applyOutlinedCopy =
      createOutlinedCopyCall(allocBuilder, value, allocInstr, pass);

  if (!pass.F->hasOwnership()) {
    loadCopy = allocBuilder.createLoad(applyOutlinedCopy->getLoc(), allocInstr,
                                       LoadOwnershipQualifier::Unqualified);
  } else {
    loadCopy = allocBuilder.createLoad(applyOutlinedCopy->getLoc(), allocInstr,
                                       LoadOwnershipQualifier::Take);
  }
  pass.argsToLoadedValueMap[value] = loadCopy;

  // Insert stack deallocations.
  for (TermInst *termInst : pass.returnInsts) {
    SILBuilderWithScope deallocBuilder(termInst);
    deallocBuilder.createDeallocStack(allocInstr->getLoc(), allocInstr);
  }
}

AllocStackInst *
LoadableStorageAllocation::allocateForApply(SILInstruction *apply,
                                            SILType type) {
  SILBuilderWithScope allocBuilder(&*pass.F->begin());
  SILLocation Loc = apply->getLoc();
  if (dyn_cast_or_null<VarDecl>(Loc.getAsASTNode<Decl>()))
    // FIXME: Remove this. This is likely indicative of a bug earlier in the
    // pipeline. An apply instruction should not have a VarDecl as location.
    Loc = RegularLocation::getAutoGeneratedLocation();
  auto *allocInstr = allocBuilder.createAllocStack(Loc, type);

  pass.largeLoadableArgs.push_back(allocInstr);
  pass.allocToApplyRetMap[allocInstr] = apply;
  pass.applyRetToAllocMap[apply] = allocInstr;

  for (TermInst *termInst : pass.returnInsts) {
    SILBuilderWithScope deallocBuilder(termInst);
    deallocBuilder.createDeallocStack(allocInstr->getLoc(), allocInstr);
  }

  return allocInstr;
}

//===----------------------------------------------------------------------===//
// LoadableByAddress: Top-Level Function Transform.
//===----------------------------------------------------------------------===//

namespace {
class LoadableByAddress : public SILModuleTransform {
  /// The entry point to this function transformation.
  void run() override;

  void runOnFunction(SILFunction *F);

private:
  void updateLoweredTypes(SILFunction *F);
  void recreateSingleApply(SILInstruction *applyInst,
                           SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateApply(SILInstruction &I,
                     SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateTupleInstr(SILInstruction &I,
                          SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateConvInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateBuiltinInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateLoadInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateUncheckedEnumDataInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete);
  bool recreateUncheckedTakeEnumDataAddrInst(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete);
  bool fixStoreToBlockStorageInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete);

  bool recreateDifferentiabilityWitnessFunction(
      SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete);

  bool shouldTransformGlobal(SILGlobalVariable *global);

  bool shouldTransformInitExprOfGlobal(SILGlobalVariable *global);

private:
  llvm::SetVector<SILFunction *> modFuncs;
  llvm::SetVector<SingleValueInstruction *> conversionInstrs;
  llvm::SetVector<BuiltinInst *> builtinInstrs;
  llvm::SetVector<LoadInst *> loadInstrsOfFunc;
  llvm::SetVector<UncheckedEnumDataInst *> uncheckedEnumDataOfFunc;
  llvm::SetVector<UncheckedTakeEnumDataAddrInst *>
      uncheckedTakeEnumDataAddrOfFunc;
  llvm::SetVector<StoreInst *> storeToBlockStorageInstrs;
  llvm::SetVector<SILInstruction *> modApplies;
  llvm::MapVector<SILInstruction *, SILValue> allApplyRetToAllocMap;

public:
  LargeSILTypeMapper MapperCache;
};
} // end anonymous namespace

/// Given that we've allocated space to hold a scalar value, try to rewrite
/// the uses of the scalar to be uses of the address.
static void rewriteUsesOfSscalar(StructLoweringState &pass,
                                 SILValue address, SILValue scalar,
                                 StoreInst *storeToAddress) {
  // Copy the uses, since we're going to edit them.
  SmallVector<Operand *, 8> uses(scalar->getUses());
  for (Operand *scalarUse : uses) {
    SILInstruction *user = scalarUse->getUser();

    if (ApplySite::isa(user)) {
      ApplySite site(user);
      if (modifiableApply(site, pass.Mod)) {
        // Just rewrite the operand in-place.  This will produce a temporary
        // type error, but we should fix that up when we rewrite the apply's
        // function type.
        scalarUse->set(address);
      }
    } else if (isa<YieldInst>(user)) {
      // The rules for the yield are changing anyway, so we can just rewrite
      // it in-place.
      scalarUse->set(address);
    } else if (auto *storeUser = dyn_cast<StoreInst>(user)) {
      // Don't rewrite the store to the allocation.
      if (storeUser == storeToAddress)
        continue;

      // Optimization: replace with copy_addr to reduce code size
      assert(std::find(pass.storeInstsToMod.begin(), pass.storeInstsToMod.end(),
                       storeUser) == pass.storeInstsToMod.end() &&
             "Did not expect this instr in storeInstsToMod");
      SILBuilderWithScope copyBuilder(storeUser);
      SILValue dest = storeUser->getDest();
      createOutlinedCopyCall(copyBuilder, address, dest, pass);
      storeUser->eraseFromParent();
    } else if (auto *dbgInst = dyn_cast<DebugValueInst>(user)) {
      SILBuilderWithScope dbgBuilder(dbgInst);
      // Rewrite the debug_value to point to the variable in the alloca.
      dbgBuilder.createDebugValueAddr(dbgInst->getLoc(), address,
                                      *dbgInst->getVarInfo());
      dbgInst->eraseFromParent();
    }
  }
}

static void allocateAndSetForInstResult(StructLoweringState &pass,
                                        SILValue instResult,
                                        SILInstruction *inst) {
  auto alloc = allocate(pass, instResult->getType());

  auto II = inst->getIterator();
  ++II;
  auto store = createStoreInit(pass, II, inst->getLoc(), instResult, alloc);

  // Traverse all the uses of instResult - see if we can replace
  rewriteUsesOfSscalar(pass, alloc, instResult, store);
}

static void allocateAndSetForArgument(StructLoweringState &pass,
                                      SILArgument *value,
                                      SILInstruction *user) {
  AllocStackInst *alloc = allocate(pass, value->getType());

  SILLocation loc = user->getLoc();
  loc.markAutoGenerated();

  // Store the value into the allocation.
  auto II = value->getParent()->begin();
  if (II == alloc->getParent()->begin()) {
    // Store should happen *after* the allocation.
    ++II;
  }
  auto store = createStoreInit(pass, II, loc, value, alloc);

  // Traverse all the uses of value - see if we can replace
  rewriteUsesOfSscalar(pass, alloc, value, store);
}

static bool allUsesAreReplaceable(StructLoweringState &pass,
                                  SingleValueInstruction *instr) {
  for (auto *user : instr->getUses()) {
    SILInstruction *userIns = user->getUser();
    switch (userIns->getKind()) {
    case SILInstructionKind::RetainValueInst:
    case SILInstructionKind::ReleaseValueInst:
    case SILInstructionKind::StoreInst:
    case SILInstructionKind::DebugValueInst:
    case SILInstructionKind::DestroyValueInst:
      break;
    case SILInstructionKind::ApplyInst:
    case SILInstructionKind::TryApplyInst:
    case SILInstructionKind::BeginApplyInst:
    case SILInstructionKind::PartialApplyInst: {
      // Replaceable only if it is not the function pointer
      ApplySite site(userIns);
      if (!modifiableApply(site, pass.Mod)) {
        return false;
      }
      SILValue callee = site.getCallee();
      if (callee == instr) {
        return false;
      }
      SILType currType = instr->getType().getObjectType();
      SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                              currType);
      if (currType == newSILType) {
        return false;
      }
      break;
    }
    case SILInstructionKind::YieldInst:
      if (!isYieldUseRewritable(pass, cast<YieldInst>(userIns), user))
        return false;
      break;
    case SILInstructionKind::StructExtractInst:
    case SILInstructionKind::SwitchEnumInst:
      break;
    default:
      return false;
    }
  }
  return true;
}

void LoadableStorageAllocation::replaceLoad(LoadInst *load) {
  if (allUsesAreReplaceable(pass, load)) {
    replaceLoadWithCopyAddr(load);
  } else {
    replaceLoadWithCopyAddrForModifiable(load);
  }
}

static void allocateAndSet(StructLoweringState &pass,
                           LoadableStorageAllocation &allocator,
                           SILValue operand, SILInstruction *user) {
  auto inst = operand->getDefiningInstruction();
  if (!inst) {
    allocateAndSetForArgument(pass, cast<SILArgument>(operand), user);
    return;
  }

  if (auto *load = dyn_cast<LoadInst>(operand)) {
    allocator.replaceLoad(load);
  } else {
    // TODO: peephole: special handling of known cases:
    // ApplyInst, TupleExtractInst
    allocateAndSetForInstResult(pass, operand, inst);
  }
}

/// Rewrite all of the large-loadable operands in the given list.
static void allocateAndSetAll(StructLoweringState &pass,
                              LoadableStorageAllocation &allocator,
                              SILInstruction *user,
                              MutableArrayRef<Operand> operands) {
  for (Operand &operand : operands) {
    SILValue value = operand.get();
    SILType silType = value->getType();
    if (pass.isLargeLoadableType(pass.F->getLoweredFunctionType(),
                                 silType)) {
      allocateAndSet(pass, allocator, value, user);
    }
  }
}

static void retypeTupleInstr(SingleValueInstruction *instr, IRGenModule &Mod,
                             LargeSILTypeMapper &Mapper) {
  SILType currSILType = instr->getType();
  auto funcType = getInnerFunctionType(currSILType);
  assert(funcType && "Expected a function Type");
  GenericEnvironment *genEnv = getSubstGenericEnvironment(instr->getFunction());
  if (!genEnv && funcType->getSubstGenericSignature()) {
    genEnv = getSubstGenericEnvironment(funcType);
  }
  SILType newSILType = Mapper.getNewSILType(genEnv, currSILType, Mod);
  if (currSILType == newSILType) {
    return;
  }

  auto II = instr->getIterator();
  ++II;
  SILBuilderWithScope Builder(II);
  SingleValueInstruction *newInstr = nullptr;
  switch (instr->getKind()) {
  // Add cast to the new sil function type:
  case SILInstructionKind::TupleExtractInst: {
    auto extractInst = cast<TupleExtractInst>(instr);
    newInstr = Builder.createTupleExtract(
      extractInst->getLoc(), extractInst->getOperand(),
      extractInst->getFieldIndex(),
      newSILType.getObjectType());
    break;
  }
  case SILInstructionKind::TupleElementAddrInst: {
    auto elementAddrInst = cast<TupleElementAddrInst>(instr);
    newInstr = Builder.createTupleElementAddr(
      elementAddrInst->getLoc(), elementAddrInst->getOperand(),
      elementAddrInst->getFieldIndex(),
      newSILType.getAddressType());
    break;
  }
  default:
    llvm_unreachable("Unexpected instruction inside tupleInstsToMod");
  }
  instr->replaceAllUsesWith(newInstr);
  instr->eraseFromParent();
}

static SILValue createCopyOfEnum(StructLoweringState &pass,
                                 SwitchEnumInst *orig) {
  auto value = orig->getOperand();
  auto type = value->getType();
  if (type.isObject()) {
    // support for non-address operands / enums
    auto *alloc = allocate(pass, type);
    createStoreInit(pass, orig->getIterator(), orig->getLoc(), value, alloc);
    return alloc;
  }

  auto alloc = allocate(pass, type.getObjectType());

  SILBuilderWithScope copyBuilder(orig);
  createOutlinedCopyCall(copyBuilder, value, alloc, pass);

  return alloc;
}

static void createResultTyInstrAndLoad(LoadableStorageAllocation &allocator,
                                       SingleValueInstruction *instr,
                                       StructLoweringState &pass) {
  bool updateResultTy = pass.resultTyInstsToMod.count(instr) != 0;
  if (updateResultTy) {
    pass.resultTyInstsToMod.remove(instr);
  }
  SILBuilderWithScope builder(instr);
  auto *currStructExtractInst = dyn_cast<StructExtractInst>(instr);
  assert(currStructExtractInst && "Expected StructExtractInst");
  SingleValueInstruction *newInstr = builder.createStructElementAddr(
      currStructExtractInst->getLoc(), currStructExtractInst->getOperand(),
      currStructExtractInst->getField(),
      currStructExtractInst->getType().getAddressType());
  // Load the struct element then see if we can get rid of the load:
  LoadInst *loadArg = nullptr;
  if (!pass.F->hasOwnership()) {
    loadArg = builder.createLoad(newInstr->getLoc(), newInstr,
                                 LoadOwnershipQualifier::Unqualified);
  } else {
    loadArg = builder.createLoad(newInstr->getLoc(), newInstr,
                                 LoadOwnershipQualifier::Take);
  }
  instr->replaceAllUsesWith(loadArg);
  instr->getParent()->erase(instr);

  // If the load is of a function type - do not replace it.
  if (isFuncOrOptionalFuncType(loadArg->getType())) {
    return;
  }

  allocator.replaceLoad(loadArg);

  if (updateResultTy) {
    pass.resultTyInstsToMod.insert(newInstr);
  }
}

static void rewriteFunction(StructLoweringState &pass,
                            LoadableStorageAllocation &allocator) {

  bool repeat = false;
  llvm::SetVector<SILInstruction *> currentModApplies;
  do {
    while (!pass.switchEnumInstsToMod.empty()) {
      auto *instr = pass.switchEnumInstsToMod.pop_back_val();
      /* unchecked_take_enum_data_addr can be destructive.
       * work on a copy instead of the original enum */
      auto copiedValue = createCopyOfEnum(pass, instr);
      SILBuilderWithScope enumBuilder(instr);
      unsigned numOfCases = instr->getNumCases();
      SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 16> caseBBs;
      for (unsigned i = 0; i < numOfCases; ++i) {
        auto currCase = instr->getCase(i);
        auto *currBB = currCase.second;
        SILBuilderWithScope argBuilder(currBB->begin());
        assert(currBB->getNumArguments() <= 1 && "Unhandled BB Type");
        EnumElementDecl *decl = currCase.first;
        for (SILArgument *arg : currBB->getArguments()) {
          SILType storageType = arg->getType();
          SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                                  storageType);
          if (storageType == newSILType) {
            newSILType = newSILType.getAddressType();
          }

          auto *newArg = argBuilder.createUncheckedTakeEnumDataAddr(
              instr->getLoc(), copiedValue, decl, newSILType.getAddressType());
          arg->replaceAllUsesWith(newArg);
          currBB->eraseArgument(0);

          // Load the enum addr then see if we can get rid of the load:
          LoadInst *loadArg = nullptr;
          if (!pass.F->hasOwnership()) {
            loadArg = argBuilder.createLoad(
                newArg->getLoc(), newArg, LoadOwnershipQualifier::Unqualified);
          } else {
            loadArg = argBuilder.createLoad(newArg->getLoc(), newArg,
                                            LoadOwnershipQualifier::Take);
          }
          newArg->replaceAllUsesWith(loadArg);
          loadArg->setOperand(newArg);

          // If the load is of a function type - do not replace it.
          if (isFuncOrOptionalFuncType(loadArg->getType())) {
            continue;
          }

          allocator.replaceLoad(loadArg);
        }
        caseBBs.push_back(std::make_pair(decl, currBB));
      }
      SILBasicBlock *defaultBB =
          instr->hasDefault() ? instr->getDefaultBB() : nullptr;
      enumBuilder.createSwitchEnumAddr(
          instr->getLoc(), copiedValue, defaultBB, caseBBs);
      instr->getParent()->erase(instr);
    }

    while (!pass.structExtractInstsToMod.empty()) {
      auto *instr = pass.structExtractInstsToMod.pop_back_val();
      createResultTyInstrAndLoad(allocator, instr, pass);
    }

    while (!pass.applies.empty()) {
      auto *applyInst = pass.applies.pop_back_val();
      if (currentModApplies.count(applyInst) == 0) {
        currentModApplies.insert(applyInst);
      }
      ApplySite applySite = ApplySite(applyInst);
      allocateAndSetAll(pass, allocator, applyInst,
                        applySite.getArgumentOperands());
    }

    while (!pass.modYieldInsts.empty()) {
      YieldInst *inst = pass.modYieldInsts.pop_back_val();
      allocateAndSetAll(pass, allocator, inst, inst->getAllOperands());
    }

    repeat = !pass.switchEnumInstsToMod.empty() ||
             !pass.structExtractInstsToMod.empty();
    assert(pass.applies.empty());
    pass.applies.append(currentModApplies.begin(), currentModApplies.end());
  } while (repeat);

  for (SILInstruction *instr : pass.instsToMod) {
    for (Operand &operand : instr->getAllOperands()) {
      auto currOperand = operand.get();
      if (std::find(pass.largeLoadableArgs.begin(),
                    pass.largeLoadableArgs.end(),
                    currOperand) != pass.largeLoadableArgs.end()) {
        SILValue newOperand = pass.argsToLoadedValueMap[currOperand];
        assert(newOperand != currOperand &&
               "Did not allocate storage and convert operand");
        operand.set(newOperand);
      }
    }
  }

  for (SingleValueInstruction *instr : pass.tupleInstsToMod) {
    retypeTupleInstr(instr, pass.Mod, pass.Mapper);
  }

  while (!pass.allocStackInstsToMod.empty()) {
    auto *instr = pass.allocStackInstsToMod.pop_back_val();
    SILBuilderWithScope allocBuilder(instr);
    SILType currSILType = instr->getType();
    SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                            currSILType);
    auto *newInstr = allocBuilder.createAllocStack(instr->getLoc(), newSILType,
                                                   instr->getVarInfo());
    instr->replaceAllUsesWith(newInstr);
    instr->getParent()->erase(instr);
  }

  while (!pass.pointerToAddrkInstsToMod.empty()) {
    auto *instr = pass.pointerToAddrkInstsToMod.pop_back_val();
    SILBuilderWithScope pointerBuilder(instr);
    SILType currSILType = instr->getType();
    SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                            currSILType);
    auto *newInstr = pointerBuilder.createPointerToAddress(
        instr->getLoc(), instr->getOperand(), newSILType.getAddressType(),
        instr->isStrict(), instr->isInvariant(), instr->alignment());
    instr->replaceAllUsesWith(newInstr);
    instr->getParent()->erase(instr);
  }

  for (DebugValueInst *instr : pass.debugInstsToMod) {
    assert(instr->getAllOperands().size() == 1 &&
           "Debug instructions have one operand");
    for (Operand &operand : instr->getAllOperands()) {
      auto currOperand = operand.get();
      if (pass.argsToLoadedValueMap.find(currOperand) !=
          pass.argsToLoadedValueMap.end()) {
        SILValue newOperand = pass.argsToLoadedValueMap[currOperand];
        assert(newOperand != currOperand &&
               "Did not allocate storage and convert operand");
        operand.set(newOperand);
      } else {
        assert(currOperand->getType().isAddress() &&
               "Expected an address type");
        SILBuilderWithScope debugBuilder(instr);
        // SILBuilderWithScope skips over metainstructions.
        debugBuilder.setCurrentDebugScope(instr->getDebugScope());
        debugBuilder.createDebugValueAddr(instr->getLoc(), currOperand,
                                          *instr->getVarInfo());
        instr->getParent()->erase(instr);
      }
    }
  }

  for (SILInstruction *instr : pass.destroyValueInstsToMod) {
    assert(instr->getAllOperands().size() == 1 &&
           "destroy_value instructions have one operand");
    for (Operand &operand : instr->getAllOperands()) {
      auto currOperand = operand.get();
      assert(currOperand->getType().isAddress() && "Expected an address type");
      SILBuilderWithScope destroyBuilder(instr);
      destroyBuilder.createDestroyAddr(instr->getLoc(), currOperand);
      instr->getParent()->erase(instr);
    }
  }

  for (StoreInst *instr : pass.storeInstsToMod) {
    SILValue src = instr->getSrc();
    SILValue tgt = instr->getDest();
    SILType srcType = src->getType();
    SILType tgtType = tgt->getType();
    assert(srcType && "Expected an address-type source");
    assert(tgtType.isAddress() && "Expected an address-type target");
    assert(srcType == tgtType && "Source and target type do not match");
    (void)srcType;
    (void)tgtType;

    SILBuilderWithScope copyBuilder(instr);
    createOutlinedCopyCall(copyBuilder, src, tgt, pass);
    instr->getParent()->erase(instr);
  }

  for (RetainValueInst *instr : pass.retainInstsToMod) {
    SILBuilderWithScope retainBuilder(instr);
    retainBuilder.createRetainValueAddr(
        instr->getLoc(), instr->getOperand(), instr->getAtomicity());
    instr->getParent()->erase(instr);
  }

  for (ReleaseValueInst *instr : pass.releaseInstsToMod) {
    SILBuilderWithScope releaseBuilder(instr);
    releaseBuilder.createReleaseValueAddr(
        instr->getLoc(), instr->getOperand(), instr->getAtomicity());
    instr->getParent()->erase(instr);
  }

  for (SingleValueInstruction *instr : pass.resultTyInstsToMod) {
    // Update the return type of these instrs
    // Note: The operand was already updated!
    SILType currSILType = instr->getType().getObjectType();
    SILType newSILType = pass.getNewSILType(pass.F->getLoweredFunctionType(),
                                            currSILType);
    SILBuilderWithScope resultTyBuilder(instr);
    SILLocation Loc = instr->getLoc();
    SingleValueInstruction *newInstr = nullptr;
    switch (instr->getKind()) {
    case SILInstructionKind::StructExtractInst: {
      auto *convInstr = cast<StructExtractInst>(instr);
      newInstr = resultTyBuilder.createStructExtract(
          Loc, convInstr->getOperand(), convInstr->getField(),
          newSILType.getObjectType());
      break;
    }
    case SILInstructionKind::StructElementAddrInst: {
      auto *convInstr = cast<StructElementAddrInst>(instr);
      newInstr = resultTyBuilder.createStructElementAddr(
          Loc, convInstr->getOperand(), convInstr->getField(),
          newSILType.getAddressType());
      break;
    }
    case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
      auto *convInstr = cast<UncheckedTakeEnumDataAddrInst>(instr);
      newInstr = resultTyBuilder.createUncheckedTakeEnumDataAddr(
          Loc, convInstr->getOperand(), convInstr->getElement(),
          newSILType.getAddressType());
      break;
    }
    case SILInstructionKind::RefTailAddrInst: {
      auto *convInstr = cast<RefTailAddrInst>(instr);
      newInstr = resultTyBuilder.createRefTailAddr(Loc, convInstr->getOperand(),
                                                   newSILType.getAddressType());
      break;
    }
    case SILInstructionKind::RefElementAddrInst: {
      auto *convInstr = cast<RefElementAddrInst>(instr);
      newInstr = resultTyBuilder.createRefElementAddr(
          Loc, convInstr->getOperand(), convInstr->getField(),
          newSILType.getAddressType());
      break;
    }
    case SILInstructionKind::BeginAccessInst: {
      auto *convInstr = cast<BeginAccessInst>(instr);
      newInstr = resultTyBuilder.createBeginAccess(
          Loc, convInstr->getOperand(), convInstr->getAccessKind(),
          convInstr->getEnforcement(), convInstr->hasNoNestedConflict(),
          convInstr->isFromBuiltin());
      break;
    }
    case SILInstructionKind::EnumInst: {
      auto *convInstr = cast<EnumInst>(instr);
      SILValue operand =
          convInstr->hasOperand() ? convInstr->getOperand() : SILValue();
      newInstr = resultTyBuilder.createEnum(
          Loc, operand, convInstr->getElement(), newSILType.getObjectType());
      break;
    }
    default:
      llvm_unreachable("Unhandled aggrTy instr");
    }
    instr->replaceAllUsesWith(newInstr);
    instr->eraseFromParent();
  }

  for (MethodInst *instr : pass.methodInstsToMod) {
    SILType currSILType = instr->getType();
    auto currSILFunctionType = currSILType.castTo<SILFunctionType>();
    GenericEnvironment *genEnvForMethod = nullptr;
    if (currSILFunctionType->isPolymorphic()) {
      genEnvForMethod = getSubstGenericEnvironment(currSILFunctionType);
    }
    SILType newSILType =
        SILType::getPrimitiveObjectType(pass.Mapper.getNewSILFunctionType(
            genEnvForMethod, currSILFunctionType, pass.Mod));
    auto member = instr->getMember();
    auto loc = instr->getLoc();
    SILBuilderWithScope methodBuilder(instr);
    MethodInst *newInstr = nullptr;

    switch (instr->getKind()) {
    case SILInstructionKind::ClassMethodInst: {
      SILValue selfValue = instr->getOperand(0);
      newInstr = methodBuilder.createClassMethod(loc, selfValue, member,
                                                 newSILType);
      break;
    }
    case SILInstructionKind::SuperMethodInst: {
      SILValue selfValue = instr->getOperand(0);
      newInstr = methodBuilder.createSuperMethod(loc, selfValue, member,
                                                 newSILType);
      break;
    }
    case SILInstructionKind::WitnessMethodInst: {
      auto *WMI = cast<WitnessMethodInst>(instr);
      newInstr = methodBuilder.createWitnessMethod(
          loc, WMI->getLookupType(), WMI->getConformance(), member, newSILType);
      break;
    }
    default:
      llvm_unreachable("Expected known MethodInst ValueKind");
    }

    instr->replaceAllUsesWith(newInstr);
    instr->getParent()->erase(instr);
  }

  while (!pass.modReturnInsts.empty()) {
    auto *instr = pass.modReturnInsts.pop_back_val();
    auto loc = instr->getLoc(); // SILLocation::RegularKind
    auto regLoc = RegularLocation(loc);
    SILBuilderWithScope retBuilder(instr);
    assert(modNonFuncTypeResultType(pass.F, pass.Mod) &&
           "Expected a regular type");
    // Before we return an empty tuple, init return arg:
    auto *entry = pass.F->getEntryBlock();
    auto *retArg = entry->getArgument(0);
    auto retOp = instr->getOperand();
    auto storageType = retOp->getType();
    if (storageType.isAddress()) {
      // There *might* be a dealloc_stack that already released this value
      // we should create the copy *before* the epilogue's deallocations
      auto IIR = instr->getReverseIterator();
      for (++IIR; IIR != instr->getParent()->rend(); ++IIR) {
        auto *currIIInstr = &(*IIR);
        if (currIIInstr->getKind() != SILInstructionKind::DeallocStackInst) {
          // got the right location - stop.
          --IIR;
          break;
        }
      }
      auto II = (IIR != instr->getParent()->rend())
                    ? IIR->getIterator()
                    : instr->getParent()->begin();
      SILBuilderWithScope retCopyBuilder(II);
      createOutlinedCopyCall(retCopyBuilder, retOp, retArg, pass, &regLoc);
    } else {
      retBuilder.createStore(regLoc, retOp, retArg,
                             getStoreInitOwnership(pass, retOp->getType()));
    }
    auto emptyTy = SILType::getPrimitiveObjectType(
        retBuilder.getModule().getASTContext().TheEmptyTupleType);
    auto newRetTuple = retBuilder.createTuple(regLoc, emptyTy, {});
    retBuilder.createReturn(newRetTuple->getLoc(), newRetTuple);
    instr->eraseFromParent();
  }
}

// Rewrite function return argument if it is a "function pointer"
// If it is a large type also return true - will be re-written later
// Returns true if the return argument needed re-writing
static bool rewriteFunctionReturn(StructLoweringState &pass) {
  auto loweredTy = pass.F->getLoweredFunctionType();
  SILFunction *F = pass.F;
  SILType resultTy = loweredTy->getAllResultsInterfaceType();
  SILType newSILType = pass.getNewSILType(loweredTy, resultTy);
  // We (currently) only care about function signatures
  if (pass.isLargeLoadableType(loweredTy, resultTy)) {
    return true;
  } else if (pass.containsDifferentFunctionSignature(loweredTy, resultTy)) {
    llvm::SmallVector<SILResultInfo, 2> newSILResultInfo;
    if (auto tupleType = newSILType.getAs<TupleType>()) {
      auto originalResults = loweredTy->getResults();
      for (unsigned int i = 0; i < originalResults.size(); ++i) {
        auto origResultInfo = originalResults[i];
        auto canElem = tupleType.getElementType(i);
        SILType objectType = SILType::getPrimitiveObjectType(canElem);
        auto newResult = SILResultInfo(objectType.getASTType(), origResultInfo.getConvention());
        newSILResultInfo.push_back(newResult);
      }
    } else {
      assert(loweredTy->getNumResults() == 1 && "Expected a single result");
      auto origResultInfo = loweredTy->getSingleResult();
      auto newResult = SILResultInfo(newSILType.getASTType(), origResultInfo.getConvention());
      newSILResultInfo.push_back(newResult);
    }

    auto NewTy = SILFunctionType::get(
        loweredTy->getInvocationGenericSignature(),
        loweredTy->getExtInfo(),
        loweredTy->getCoroutineKind(),
        loweredTy->getCalleeConvention(),
        loweredTy->getParameters(),
        loweredTy->getYields(),
        newSILResultInfo, loweredTy->getOptionalErrorResult(),
        loweredTy->getPatternSubstitutions(),
        loweredTy->getInvocationSubstitutions(),
        F->getModule().getASTContext(),
        loweredTy->getWitnessMethodConformanceOrInvalid());
    F->rewriteLoweredTypeUnsafe(NewTy);
    return true;
  }
  return false;
}

void LoadableByAddress::runOnFunction(SILFunction *F) {
  CanSILFunctionType funcType = F->getLoweredFunctionType();
  IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(F);

  if (F->isExternalDeclaration()) {
    if (!modifiableFunction(funcType)) {
      return;
    }
    // External function - re-write external declaration - this is ABI!
    GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
    auto loweredTy = F->getLoweredFunctionType();
    if (!genEnv && loweredTy->getSubstGenericSignature()) {
      genEnv = getSubstGenericEnvironment(loweredTy);
    }
    if (MapperCache.shouldTransformFunctionType(genEnv, loweredTy,
                                                *currIRMod)) {
      modFuncs.insert(F);
    }
    return;
  }
  
  StructLoweringState pass(F, *currIRMod, MapperCache);

  // Rewrite function args and insert allocs.
  LoadableStorageAllocation allocator(pass);
  allocator.allocateLoadableStorage();

  bool rewrittenReturn = false;
  if (modifiableFunction(funcType)) {
    rewrittenReturn = rewriteFunctionReturn(pass);
  }

  LLVM_DEBUG(llvm::dbgs() << "\nREWRITING: " << F->getName();
             F->print(llvm::dbgs()));

  // Rewrite instructions relating to the loadable struct.
  rewriteFunction(pass, allocator);

  invalidateAnalysis(F, SILAnalysis::InvalidationKind::Instructions);

  // If we modified the function arguments - add to list of functions to clone
  if (modifiableFunction(funcType) &&
      (rewrittenReturn ||
       !pass.largeLoadableArgs.empty() ||
       !pass.funcSigArgs.empty() ||
       pass.hasLargeLoadableYields())) {
    modFuncs.insert(F);
  }
  // If we modified any applies - add them to the global list for recreation
  if (!pass.applies.empty()) {
    modApplies.insert(pass.applies.begin(), pass.applies.end());
  }
  if (!pass.applyRetToAllocMap.empty()) {
    for (auto elm : pass.applyRetToAllocMap) {
      allApplyRetToAllocMap.insert(elm);
    }
  }
}

static SILValue
getOperandTypeWithCastIfNecessary(SILInstruction *containingInstr, SILValue op,
                                  IRGenModule &Mod, SILBuilder &builder,
                                  LargeSILTypeMapper &Mapper) {
  SILType currSILType = op->getType();
  SILType nonOptionalType = currSILType;
  if (auto optType = currSILType.getOptionalObjectType()) {
    nonOptionalType = optType;
  }
  if (auto funcType = nonOptionalType.getAs<SILFunctionType>()) {
    GenericEnvironment *genEnv =
      getSubstGenericEnvironment(containingInstr->getFunction());
    if (!genEnv && funcType->isPolymorphic()) {
      genEnv = getSubstGenericEnvironment(funcType);
    }
    auto newFnType = Mapper.getNewSILFunctionType(genEnv, funcType, Mod);
    SILType newSILType = SILType::getPrimitiveObjectType(newFnType);
    if (nonOptionalType.isAddress()) {
      newSILType = newSILType.getAddressType();
    }
    if (nonOptionalType != currSILType) {
      newSILType = SILType::getOptionalType(newSILType);
    }
    if (currSILType.isAddress()) {
      newSILType = newSILType.getAddressType();
    }
    if (currSILType.isAddress()) {
      if (newSILType != currSILType) {
        auto castInstr = builder.createUncheckedAddrCast(
            containingInstr->getLoc(), op, newSILType);
        return castInstr;
      }
      return op;
    }
    assert(currSILType.isObject() && "Expected an object type");
    if (newSILType != currSILType) {
      auto castInstr = builder.createUncheckedReinterpretCast(
          containingInstr->getLoc(), op, newSILType);
      return castInstr;
    }
  }
  return op;
}

void LoadableByAddress::recreateSingleApply(
    SILInstruction *applyInst, SmallVectorImpl<SILInstruction *> &Delete) {
  auto *F = applyInst->getFunction();
  IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(F);
  // Collect common info
  ApplySite applySite = ApplySite(applyInst);
  SILValue callee = applySite.getCallee();
  if (auto site = ApplySite::isa(callee)) {
    // We need to re-create the callee's apply before recreating this one
    // else verification will fail with wrong SubstCalleeType
    auto calleInstr = site.getInstruction();
    if (modApplies.remove(calleInstr)) {
      recreateSingleApply(calleInstr, Delete);
      callee = applySite.getCallee();
    }
  }
  CanSILFunctionType origSILFunctionType = applySite.getSubstCalleeType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(origSILFunctionType);
  CanSILFunctionType newSILFunctionType = MapperCache.getNewSILFunctionType(
      genEnv, origSILFunctionType, *currIRMod);
  SILFunctionConventions newSILFunctionConventions(newSILFunctionType,
                                                   *getModule());
  SmallVector<SILValue, 8> callArgs;
  SILBuilderWithScope applyBuilder(applyInst);
  // If we turned a direct result into an indirect parameter
  // Find the new alloc we created earlier.
  // and pass it as first parameter:
  if ((isa<ApplyInst>(applyInst) || isa<TryApplyInst>(applyInst)) &&
      modNonFuncTypeResultType(genEnv, origSILFunctionType, *currIRMod) &&
      modifiableApply(applySite, *getIRGenModule())) {
    assert(allApplyRetToAllocMap.find(applyInst) !=
           allApplyRetToAllocMap.end());
    auto newAlloc = allApplyRetToAllocMap.find(applyInst)->second;
    callArgs.push_back(newAlloc);
  }

  // Collect arg operands
  for (Operand &operand : applySite.getArgumentOperands()) {
    SILValue currOperand = operand.get();
    currOperand = getOperandTypeWithCastIfNecessary(
        applyInst, currOperand, *currIRMod, applyBuilder, MapperCache);
    callArgs.push_back(currOperand);
  }
  // Recreate apply with new operands due to substitution-type cache
  switch (applyInst->getKind()) {
  case SILInstructionKind::ApplyInst: {
    auto *castedApply = cast<ApplyInst>(applyInst);
    SILValue newApply =
      applyBuilder.createApply(castedApply->getLoc(), callee,
                               applySite.getSubstitutionMap(),
                               callArgs,
                               castedApply->getApplyOptions());
    castedApply->replaceAllUsesWith(newApply);
    break;
  }
  case SILInstructionKind::TryApplyInst: {
    auto *castedApply = cast<TryApplyInst>(applyInst);
    applyBuilder.createTryApply(
        castedApply->getLoc(), callee,
        applySite.getSubstitutionMap(), callArgs,
        castedApply->getNormalBB(), castedApply->getErrorBB(),
        castedApply->getApplyOptions());
    break;
  }
  case SILInstructionKind::BeginApplyInst: {
    auto oldApply = cast<BeginApplyInst>(applyInst);
    auto newApply =
      applyBuilder.createBeginApply(oldApply->getLoc(), callee,
                                    applySite.getSubstitutionMap(), callArgs,
                                    oldApply->getApplyOptions());

    // Use the new token result.
    oldApply->getTokenResult()->replaceAllUsesWith(newApply->getTokenResult());

    // Rewrite all the yields.
    auto oldYields = oldApply->getOrigCalleeType()->getYields();
    auto oldYieldedValues = oldApply->getYieldedValues();
    auto newYields = newApply->getOrigCalleeType()->getYields();
    auto newYieldedValues = newApply->getYieldedValues();
    assert(oldYields.size() == newYields.size() &&
           oldYields.size() == oldYieldedValues.size() &&
           newYields.size() == newYieldedValues.size());
    (void)newYields;
    for (auto i : indices(oldYields)) {
      SILValue oldValue = oldYieldedValues[i];
      SILValue newValue = newYieldedValues[i];

      // For now, just replace the value with an immediate load if the old value
      // was direct.
      if (oldValue->getType() != newValue->getType() &&
          !oldValue->getType().isAddress()) {
        LoadOwnershipQualifier ownership;
        if (!F->hasOwnership()) {
          ownership = LoadOwnershipQualifier::Unqualified;
        } else if (newValue->getType().isTrivial(*F)) {
          ownership = LoadOwnershipQualifier::Trivial;
        } else {
          assert(oldYields[i].isConsumed() &&
                 "borrowed yields not yet supported here");
          ownership = LoadOwnershipQualifier::Take;
        }
        newValue = applyBuilder.createLoad(applyInst->getLoc(), newValue,
                                           ownership);
      }
      oldValue->replaceAllUsesWith(newValue);
    }
    break;
  }
  case SILInstructionKind::PartialApplyInst: {
    auto *castedApply = cast<PartialApplyInst>(applyInst);
    // Change the type of the Closure
    auto partialApplyConvention = castedApply->getType()
                                      .getAs<SILFunctionType>()
                                      ->getCalleeConvention();

    auto newApply = applyBuilder.createPartialApply(
        castedApply->getLoc(), callee, applySite.getSubstitutionMap(), callArgs,
        partialApplyConvention, castedApply->isOnStack());
    castedApply->replaceAllUsesWith(newApply);
    break;
  }
  default:
    llvm_unreachable("Unexpected instr: unknown apply type");
  }
  Delete.push_back(applyInst);
}

bool LoadableByAddress::recreateApply(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  if (!modApplies.count(&I))
    return false;
  recreateSingleApply(&I, Delete);
  modApplies.remove(&I);
  return true;
}

bool LoadableByAddress::recreateLoadInstr(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  auto *loadInstr = dyn_cast<LoadInst>(&I);
  if (!loadInstr || !loadInstrsOfFunc.count(loadInstr))
    return false;

  SILBuilderWithScope loadBuilder(loadInstr);
  // If this is a load of a function for which we changed the return type:
  // add UncheckedBitCast before the load
  auto loadOp = loadInstr->getOperand();
  loadOp = getOperandTypeWithCastIfNecessary(
      loadInstr, loadOp, *getIRGenModule(), loadBuilder, MapperCache);
  auto *newInstr = loadBuilder.createLoad(loadInstr->getLoc(), loadOp,
                                          loadInstr->getOwnershipQualifier());
  loadInstr->replaceAllUsesWith(newInstr);
  Delete.push_back(loadInstr);
  return true;
}

bool LoadableByAddress::recreateUncheckedEnumDataInstr(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  auto enumInstr = dyn_cast<UncheckedEnumDataInst>(&I);
  if (!enumInstr || !uncheckedEnumDataOfFunc.count(enumInstr))
    return false;
  SILBuilderWithScope enumBuilder(enumInstr);
  SILFunction *F = enumInstr->getFunction();
  IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(F);
  SILType origType = enumInstr->getType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  SILType newType = MapperCache.getNewSILType(genEnv, origType, *currIRMod);
  auto caseTy = enumInstr->getOperand()->getType().getEnumElementType(
      enumInstr->getElement(), F->getModule(), TypeExpansionContext(*F));
  SingleValueInstruction *newInstr = nullptr;
  if (newType.isAddress()) {
    newType = newType.getObjectType();
  }
  if (caseTy != newType) {
    auto *takeEnum = enumBuilder.createUncheckedEnumData(
        enumInstr->getLoc(), enumInstr->getOperand(), enumInstr->getElement(),
        caseTy);
    newInstr = enumBuilder.createUncheckedReinterpretCast(enumInstr->getLoc(),
                                                          takeEnum, newType);
  } else {
    newInstr = enumBuilder.createUncheckedEnumData(
        enumInstr->getLoc(), enumInstr->getOperand(), enumInstr->getElement(),
        newType);
  }
  enumInstr->replaceAllUsesWith(newInstr);
  Delete.push_back(enumInstr);
  return false;
}

bool LoadableByAddress::recreateUncheckedTakeEnumDataAddrInst(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  auto *enumInstr = dyn_cast<UncheckedTakeEnumDataAddrInst>(&I);
  if (!enumInstr || !uncheckedTakeEnumDataAddrOfFunc.count(enumInstr))
    return false;
  SILBuilderWithScope enumBuilder(enumInstr);
  SILFunction *F = enumInstr->getFunction();
  IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(F);
  SILType origType = enumInstr->getType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  SILType newType = MapperCache.getNewSILType(genEnv, origType, *currIRMod);
  auto caseTy = enumInstr->getOperand()->getType().getEnumElementType(
      enumInstr->getElement(), F->getModule(), TypeExpansionContext(*F));
  SingleValueInstruction *newInstr = nullptr;
  if (caseTy != origType.getObjectType()) {
    auto *takeEnum = enumBuilder.createUncheckedTakeEnumDataAddr(
        enumInstr->getLoc(), enumInstr->getOperand(), enumInstr->getElement(),
        caseTy.getAddressType());
    newInstr = enumBuilder.createUncheckedAddrCast(
        enumInstr->getLoc(), takeEnum, newType.getAddressType());
  } else {
    newInstr = enumBuilder.createUncheckedTakeEnumDataAddr(
        enumInstr->getLoc(), enumInstr->getOperand(), enumInstr->getElement(),
        newType.getAddressType());
  }
  enumInstr->replaceAllUsesWith(newInstr);
  Delete.push_back(enumInstr);
  return true;
}

bool LoadableByAddress::fixStoreToBlockStorageInstr(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  auto *instr = dyn_cast<StoreInst>(&I);
  if (!instr || !storeToBlockStorageInstrs.count(instr))
    return false;
  auto dest = instr->getDest();
  auto destBlock = cast<ProjectBlockStorageInst>(dest);
  SILType destType = destBlock->getType();
  auto src = instr->getSrc();
  SILType srcType = src->getType();
  if (destType.getObjectType() != srcType) {
    // Add cast to destType
    SILBuilderWithScope castBuilder(instr);
    auto *castInstr = castBuilder.createUncheckedReinterpretCast(
        instr->getLoc(), src, destType.getObjectType());
    instr->setOperand(StoreInst::Src, castInstr);
  }
  return true;
}

bool LoadableByAddress::recreateDifferentiabilityWitnessFunction(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  auto *instr = dyn_cast<DifferentiabilityWitnessFunctionInst>(&I);
  if (!instr)
    return false;

  // Check if we need to recreate the instruction.
  auto *currIRMod = getIRGenModule()->IRGen.getGenModule(instr->getFunction());
  auto resultFnTy = instr->getType().castTo<SILFunctionType>();
  auto genSig = resultFnTy->getSubstGenericSignature();
  auto *genEnv = genSig.getGenericEnvironment();
  auto newResultFnTy =
      MapperCache.getNewSILFunctionType(genEnv, resultFnTy, *currIRMod);
  if (resultFnTy == newResultFnTy)
    return true;

  SILBuilderWithScope builder(instr);
  auto *newInstr = builder.createDifferentiabilityWitnessFunction(
      instr->getLoc(), instr->getWitnessKind(), instr->getWitness(),
      SILType::getPrimitiveObjectType(newResultFnTy));
  instr->replaceAllUsesWith(newInstr);
  Delete.push_back(instr);
  return true;
}

bool LoadableByAddress::recreateTupleInstr(
    SILInstruction &I, SmallVectorImpl<SILInstruction *> &Delete) {
  auto *tupleInstr = dyn_cast<TupleInst>(&I);
  if (!tupleInstr)
    return false;

  // Check if we need to recreate the tuple:
  auto *F = tupleInstr->getFunction();
  auto *currIRMod = getIRGenModule()->IRGen.getGenModule(F);
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  auto resultTy = tupleInstr->getType();
  auto newResultTy = MapperCache.getNewSILType(genEnv, resultTy, *currIRMod);
  if (resultTy == newResultTy)
    return true;

  // The tuple type have changed based on its members.
  // For example if one or more of them are ‘large’ loadable types
  SILBuilderWithScope tupleBuilder(tupleInstr);
  SmallVector<SILValue, 8> elems;
  for (auto elem : tupleInstr->getElements()) {
    elems.push_back(elem);
  }
  auto *newTuple = tupleBuilder.createTuple(tupleInstr->getLoc(), newResultTy,
                                            elems);
  tupleInstr->replaceAllUsesWith(newTuple);
  Delete.push_back(tupleInstr);
  return true;
}

bool LoadableByAddress::recreateConvInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete) {
  auto *convInstr = dyn_cast<SingleValueInstruction>(&I);
  if (!convInstr || !conversionInstrs.count(convInstr))
    return false;
  IRGenModule *currIRMod =
      getIRGenModule()->IRGen.getGenModule(convInstr->getFunction());
  SILType currSILType = convInstr->getType();
  auto currSILFunctionType = currSILType.castTo<SILFunctionType>();
  GenericEnvironment *genEnv =
    getSubstGenericEnvironment(convInstr->getFunction());
  // Differentiable function conversion instructions can happen while the
  // function is still generic. In that case, we must calculate the new type
  // using the converted function's generic environment rather than the
  // converting function's generic environment.
  //
  // This happens in witness thunks for default implementations of derivative
  // requirements.
  if (convInstr->getKind() == SILInstructionKind::DifferentiableFunctionInst ||
      convInstr->getKind() == SILInstructionKind::DifferentiableFunctionExtractInst ||
      convInstr->getKind() == SILInstructionKind::LinearFunctionInst ||
      convInstr->getKind() == SILInstructionKind::LinearFunctionExtractInst) {
    genEnv = currSILFunctionType->getSubstGenericSignature().getGenericEnvironment();
  }
  CanSILFunctionType newFnType = MapperCache.getNewSILFunctionType(
      genEnv, currSILFunctionType, *currIRMod);
  SILType newType = SILType::getPrimitiveObjectType(newFnType);
  SILBuilderWithScope convBuilder(convInstr);
  SingleValueInstruction *newInstr = nullptr;
  switch (convInstr->getKind()) {
  case SILInstructionKind::ThinToThickFunctionInst: {
    auto instr = cast<ThinToThickFunctionInst>(convInstr);
    newInstr = convBuilder.createThinToThickFunction(
        instr->getLoc(), instr->getOperand(), newType);
    break;
  }
  case SILInstructionKind::ConvertFunctionInst: {
    auto instr = cast<ConvertFunctionInst>(convInstr);
    newInstr = convBuilder.createConvertFunction(
        instr->getLoc(), instr->getOperand(), newType,
        instr->withoutActuallyEscaping());
    break;
  }
  case SILInstructionKind::ConvertEscapeToNoEscapeInst: {
    auto instr = cast<ConvertEscapeToNoEscapeInst>(convInstr);
    newInstr = convBuilder.createConvertEscapeToNoEscape(
        instr->getLoc(), instr->getOperand(), newType,
        instr->isLifetimeGuaranteed());
    break;
  }
  case SILInstructionKind::MarkDependenceInst: {
    auto instr = cast<MarkDependenceInst>(convInstr);
    newInstr = convBuilder.createMarkDependence(
        instr->getLoc(), instr->getValue(), instr->getBase());
    break;
  }
  case SILInstructionKind::DifferentiableFunctionInst: {
    auto instr = cast<DifferentiableFunctionInst>(convInstr);
    newInstr = convBuilder.createDifferentiableFunction(
        instr->getLoc(), instr->getParameterIndices(),
        instr->getResultIndices(), instr->getOriginalFunction(),
        instr->getOptionalDerivativeFunctionPair());
    break;
  }
  case SILInstructionKind::DifferentiableFunctionExtractInst: {
    auto instr = cast<DifferentiableFunctionExtractInst>(convInstr);
    // Rewrite `differentiable_function_extract` with explicit extractee type.
    newInstr = convBuilder.createDifferentiableFunctionExtract(
        instr->getLoc(), instr->getExtractee(), instr->getOperand(), newType);
    break;
  }
  case SILInstructionKind::LinearFunctionInst: {
    auto instr = cast<LinearFunctionInst>(convInstr);
    newInstr = convBuilder.createLinearFunction(
        instr->getLoc(), instr->getParameterIndices(),
        instr->getOriginalFunction(), instr->getOptionalTransposeFunction());
    break;
  }
  case SILInstructionKind::LinearFunctionExtractInst: {
    auto instr = cast<LinearFunctionExtractInst>(convInstr);
    newInstr = convBuilder.createLinearFunctionExtract(
        instr->getLoc(), instr->getExtractee(), instr->getOperand());
    break;
  }
  default:
    llvm_unreachable("Unexpected conversion instruction");
  }
  convInstr->replaceAllUsesWith(newInstr);
  Delete.push_back(convInstr);
  return true;
}

bool LoadableByAddress::recreateBuiltinInstr(SILInstruction &I,
                         SmallVectorImpl<SILInstruction *> &Delete) {
	auto builtinInstr = dyn_cast<BuiltinInst>(&I);
  if (!builtinInstr || !builtinInstrs.count(builtinInstr))
    return false;
  auto *currIRMod =
      getIRGenModule()->IRGen.getGenModule(builtinInstr->getFunction());
  auto *F = builtinInstr->getFunction();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  auto resultTy = builtinInstr->getType();
  auto newResultTy = MapperCache.getNewSILType(genEnv, resultTy, *currIRMod);

  llvm::SmallVector<SILValue, 5> newArgs;
  for (auto oldArg : builtinInstr->getArguments()) {
    newArgs.push_back(oldArg);
  }

  SILBuilderWithScope builtinBuilder(builtinInstr);
  auto *newInstr = builtinBuilder.createBuiltin(
      builtinInstr->getLoc(), builtinInstr->getName(), newResultTy,
      builtinInstr->getSubstitutions(), newArgs);
  builtinInstr->replaceAllUsesWith(newInstr);
  Delete.push_back(builtinInstr);
  return true;
}

void LoadableByAddress::updateLoweredTypes(SILFunction *F) {
  IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(F);
  CanSILFunctionType funcType = F->getLoweredFunctionType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(F);
  if (!genEnv && funcType->getSubstGenericSignature()) {
    genEnv = getSubstGenericEnvironment(funcType);
  }
  auto newFuncTy =
      MapperCache.getNewSILFunctionType(genEnv, funcType, *currIRMod);
  F->rewriteLoweredTypeUnsafe(newFuncTy);
}

void IRGenModule::lowerSILFunction(SILFunction *f) {
  CanSILFunctionType funcType = f->getLoweredFunctionType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(f);
  if (!genEnv && funcType->getSubstGenericSignature()) {
    genEnv = getSubstGenericEnvironment(funcType);
  }
  LargeSILTypeMapper MapperCache;
  auto newFuncTy = MapperCache.getNewSILFunctionType(genEnv, funcType, *this);
  f->rewriteLoweredTypeUnsafe(newFuncTy);
}

bool LoadableByAddress::shouldTransformGlobal(SILGlobalVariable *global) {
  SILInstruction *init = global->getStaticInitializerValue();
  if (!init)
    return false;
  auto silTy = global->getLoweredType();
  if (!isa<SILFunctionType>(silTy.getASTType()))
    return false;

  auto *decl = global->getDecl();
  IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(
      decl ? decl->getDeclContext() : nullptr);
  auto silFnTy = global->getLoweredFunctionType();
  GenericEnvironment *genEnv = getSubstGenericEnvironment(silFnTy);
  if (MapperCache.shouldTransformFunctionType(genEnv, silFnTy, *currIRMod))
    return true;
  return false;
}

bool LoadableByAddress::shouldTransformInitExprOfGlobal(SILGlobalVariable *global) {
  for (const SILInstruction &initInst : *global) {
    if (auto *fri = dyn_cast<FunctionRefBaseInst>(&initInst)) {
      SILFunction *refF = fri->getInitiallyReferencedFunction();
      if (modFuncs.count(refF) != 0)
        return true;
    }
  }
  return false;
}

namespace {
  class GlobalInitCloner : public SILCloner<GlobalInitCloner> {
    LoadableByAddress *pass;
    IRGenModule *irgenModule;
  public:
    GlobalInitCloner(SILGlobalVariable *global, LoadableByAddress *pass,
                     IRGenModule *irgenModule)
      : SILCloner<GlobalInitCloner>(global), pass(pass), irgenModule(irgenModule) {
    }

    SILType remapType(SILType ty) {
      if (auto fnType = ty.getAs<SILFunctionType>()) {
        GenericEnvironment *genEnv = getSubstGenericEnvironment(fnType);
        return SILType::getPrimitiveObjectType(
            pass->MapperCache.getNewSILFunctionType(genEnv, fnType, *irgenModule));
      }
      return ty;
    }

    void clone(SILInstruction *inst) {
      visit(inst);
    }
  };
}

/// The entry point to this function transformation.
void LoadableByAddress::run() {  
  // Set the SIL state before the PassManager has a chance to run
  // verification.
  getModule()->setStage(SILStage::Lowered);

  for (auto &F : *getModule())
    runOnFunction(&F);

  if (modFuncs.empty() && modApplies.empty()) {
    return;
  }

  // Scan the module for all references of the modified functions:
  llvm::SetVector<FunctionRefBaseInst *> funcRefs;
  llvm::SetVector<SILInstruction *> globalRefs;
  for (SILFunction &CurrF : *getModule()) {
    for (SILBasicBlock &BB : CurrF) {
      for (SILInstruction &I : BB) {
        if (auto *allocGlobal = dyn_cast<AllocGlobalInst>(&I)) {
          auto *global = allocGlobal->getReferencedGlobal();
          if (shouldTransformGlobal(global))
            globalRefs.insert(allocGlobal);
        } else if (auto *globalAddr = dyn_cast<GlobalAddrInst>(&I)) {
          auto *global = globalAddr->getReferencedGlobal();
          if (shouldTransformGlobal(global))
            globalRefs.insert(globalAddr);
        } else if (auto *globalVal = dyn_cast<GlobalValueInst>(&I)) {
          auto *global = globalVal->getReferencedGlobal();
          if (shouldTransformGlobal(global))
            globalRefs.insert(globalVal);
        } else if (auto *FRI = dyn_cast<FunctionRefBaseInst>(&I)) {
          SILFunction *RefF = FRI->getInitiallyReferencedFunction();
          if (modFuncs.count(RefF) != 0) {
            // Go over the uses and add them to lists to modify
            //
            // FIXME: Why aren't function_ref uses processed transitively?  And
            // why is it necessary to visit uses at all if they will be visited
            // later in this loop?
            for (auto *user : FRI->getUses()) {
              SILInstruction *currInstr = user->getUser();
              switch (currInstr->getKind()) {
              case SILInstructionKind::ApplyInst:
              case SILInstructionKind::TryApplyInst:
              case SILInstructionKind::BeginApplyInst:
              case SILInstructionKind::PartialApplyInst: {
                if (modApplies.count(currInstr) == 0) {
                  modApplies.insert(currInstr);
                }
                break;
              }
              case SILInstructionKind::ConvertFunctionInst:
              case SILInstructionKind::ConvertEscapeToNoEscapeInst:
              case SILInstructionKind::MarkDependenceInst:
              case SILInstructionKind::ThinToThickFunctionInst:
              case SILInstructionKind::DifferentiableFunctionInst:
              case SILInstructionKind::LinearFunctionInst:
              case SILInstructionKind::LinearFunctionExtractInst:
              case SILInstructionKind::DifferentiableFunctionExtractInst: {
                conversionInstrs.insert(
                    cast<SingleValueInstruction>(currInstr));
                break;
              }
              case SILInstructionKind::BuiltinInst: {
                auto *instr = cast<BuiltinInst>(currInstr);
                builtinInstrs.insert(instr);
                break;
              }
              case SILInstructionKind::DebugValueInst:
                break;
              default:
                llvm_unreachable("Unhandled use of FunctionRefInst");
              }
            }
            funcRefs.insert(FRI);
          }
        } else if (auto *Cvt = dyn_cast<MarkDependenceInst>(&I)) {
          SILValue val = Cvt->getValue();
          SILType currType = val->getType();
          if (auto fType = currType.getAs<SILFunctionType>()) {
            if (modifiableFunction(fType)) {
              conversionInstrs.insert(Cvt);
            }
          }
        } else if (auto *Cvt = dyn_cast<ConvertEscapeToNoEscapeInst>(&I)) {
          SILValue val = Cvt->getOperand();
          SILType currType = val->getType();
          auto fType = currType.getAs<SILFunctionType>();
          assert(fType && "Expected SILFunctionType");
          if (modifiableFunction(fType)) {
            conversionInstrs.insert(Cvt);
          }
        } else if (auto *CFI = dyn_cast<ConvertFunctionInst>(&I)) {
          SILValue val = CFI->getOperand();
          SILType currType = val->getType();
          auto fType = currType.getAs<SILFunctionType>();
          assert(fType && "Expected SILFunctionType");
          if (modifiableFunction(fType)) {
            conversionInstrs.insert(CFI);
          }
        } else if (auto *TTI = dyn_cast<ThinToThickFunctionInst>(&I)) {

          auto canType = TTI->getCallee()->getType();
          auto fType = canType.castTo<SILFunctionType>();

          if (modifiableFunction(fType))
            conversionInstrs.insert(TTI);

        } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
          loadInstrsOfFunc.insert(LI);
        } else if (auto *UED = dyn_cast<UncheckedEnumDataInst>(&I)) {
          uncheckedEnumDataOfFunc.insert(UED);
        } else if (auto *UED = dyn_cast<UncheckedTakeEnumDataAddrInst>(&I)) {
          uncheckedTakeEnumDataAddrOfFunc.insert(UED);
        } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
          auto dest = SI->getDest();
          if (isa<ProjectBlockStorageInst>(dest)) {
            storeToBlockStorageInstrs.insert(SI);
          }
        } else if (auto *PAI = dyn_cast<PartialApplyInst>(&I)) {
          if (modApplies.count(PAI) == 0) {
            modApplies.insert(PAI);
          }
        } else if (isa<DifferentiableFunctionInst>(&I) ||
                   isa<LinearFunctionInst>(&I) ||
                   isa<DifferentiableFunctionExtractInst>(&I) ||
                   isa<LinearFunctionExtractInst>(&I)) {
          conversionInstrs.insert(cast<SingleValueInstruction>(&I));
        }
      }
    }
  }

  for (auto *F : modFuncs) {
    // Update the lowered type of the Function
    updateLoweredTypes(F);
  }

  // Update globals' initializer.
  SmallVector<SILGlobalVariable *, 16> deadGlobals;
  for (SILGlobalVariable &global : getModule()->getSILGlobals()) {
    if (shouldTransformInitExprOfGlobal(&global)) {
      auto *decl = global.getDecl();
      IRGenModule *currIRMod = getIRGenModule()->IRGen.getGenModule(
          decl ? decl->getDeclContext() : nullptr);

      auto silTy = global.getLoweredType();
      if (isa<SILFunctionType>(silTy.getASTType())) {
        auto silFnTy = global.getLoweredFunctionType();
        GenericEnvironment *genEnv = getSubstGenericEnvironment(silFnTy);
        if (MapperCache.shouldTransformFunctionType(genEnv, silFnTy, *currIRMod)) {
            auto newSILFnType =
              MapperCache.getNewSILFunctionType(genEnv, silFnTy, *currIRMod);
            global.unsafeSetLoweredType(SILType::getPrimitiveObjectType(newSILFnType));
        }
      }

      // Rewrite the init basic block...
      SmallVector<SILInstruction *, 8> initBlockInsts;
      for (auto it = global.begin(), end = global.end(); it != end; ++it) {
        initBlockInsts.push_back(const_cast<SILInstruction *>(&*it));
      }
      GlobalInitCloner cloner(&global, this, currIRMod);
      for (auto *oldInst : initBlockInsts) {
        cloner.clone(oldInst);
        global.unsafeRemove(oldInst, *getModule());
      }
    }
  }

  // Rewrite global variable users.
  for (auto *inst : globalRefs) {
    if (auto *allocGlobal = dyn_cast<AllocGlobalInst>(inst)) {
      // alloc_global produces no results.
      SILBuilderWithScope builder(inst);
      builder.createAllocGlobal(allocGlobal->getLoc(),
                                allocGlobal->getReferencedGlobal());
      allocGlobal->eraseFromParent();
    } else if (auto *globalAddr = dyn_cast<GlobalAddrInst>(inst)) {
      SILBuilderWithScope builder(inst);
      auto *newInst = builder.createGlobalAddr(
          globalAddr->getLoc(), globalAddr->getReferencedGlobal());
      globalAddr->replaceAllUsesWith(newInst);
      globalAddr->eraseFromParent();
    } else if (auto *globalVal = dyn_cast<GlobalValueInst>(inst)) {
      SILBuilderWithScope builder(inst);
      auto *newInst = builder.createGlobalValue(
          globalVal->getLoc(), globalVal->getReferencedGlobal());
      globalVal->replaceAllUsesWith(newInst);
      globalVal->eraseFromParent();
    }
  }

  // Update all references:
  // Note: We don't need to update the witness tables and vtables
  // They just contain a pointer to the function
  // The pointer does not change
  for (auto *instr : funcRefs) {
    SILFunction *F = instr->getInitiallyReferencedFunction();
    SILBuilderWithScope refBuilder(instr);
    SingleValueInstruction *newInstr =
        refBuilder.createFunctionRef(instr->getLoc(), F, instr->getKind());
    instr->replaceAllUsesWith(newInstr);
    instr->getParent()->erase(instr);
  }

  // Recreate the instructions in topological order. Some instructions inherit
  // their result type from their operand.
  for (SILFunction &CurrF : *getModule()) {
    SmallVector<SILInstruction *, 32> Delete;
    for (SILBasicBlock &BB : CurrF) {
      for (SILInstruction &I : BB) {
        if (recreateTupleInstr(I, Delete))
          continue;
        else if (recreateConvInstr(I, Delete))
          continue;
        else if (recreateBuiltinInstr(I, Delete))
          continue;
        else if (recreateUncheckedEnumDataInstr(I, Delete))
          continue;
        else if (recreateUncheckedTakeEnumDataAddrInst(I, Delete))
          continue;
        else if (recreateLoadInstr(I, Delete))
          continue;
        else if (recreateApply(I, Delete))
          continue;
        else if (recreateDifferentiabilityWitnessFunction(I, Delete))
          continue;
        else
          fixStoreToBlockStorageInstr(I, Delete);
      }
    }
    for (auto *Inst : Delete)
      Inst->eraseFromParent();
  }

  // Clean up the data structs:
  modFuncs.clear();
  conversionInstrs.clear();
  loadInstrsOfFunc.clear();
  uncheckedEnumDataOfFunc.clear();
  modApplies.clear();
  storeToBlockStorageInstrs.clear();
}

SILTransform *irgen::createLoadableByAddress() {
  return new LoadableByAddress();
}
