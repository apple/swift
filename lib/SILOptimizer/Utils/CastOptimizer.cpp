//===--- CastOptimizer.cpp ------------------------------------------------===//
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
///
/// \file
///
/// This file contains local cast optimizations and simplifications.
///
//===----------------------------------------------------------------------===//

#include "swift/SILOptimizer/Utils/CastOptimizer.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include <deque>

using namespace swift;

//===----------------------------------------------------------------------===//
//                  ObjC -> Swift Bridging Cast Optimization
//===----------------------------------------------------------------------===//

static SILFunction *
getObjCToSwiftBridgingFunction(SILOptFunctionBuilder &funcBuilder,
                               SILDynamicCastInst dynamicCast) {
  // inline constructor.
  auto *bridgeFuncDecl = [&]() -> FuncDecl * {
    auto &astContext = dynamicCast.getModule().getASTContext();
    if (dynamicCast.isConditional()) {
      return astContext.getConditionallyBridgeFromObjectiveCBridgeable();
    }
    return astContext.getForceBridgeFromObjectiveCBridgeable();
  }();

  assert(bridgeFuncDecl && "Bridging function doesn't exist?!");

  SILDeclRef funcDeclRef(bridgeFuncDecl, SILDeclRef::Kind::Func);

  // Lookup a function from the stdlib.
  return funcBuilder.getOrCreateFunction(dynamicCast.getLocation(), funcDeclRef,
                                         ForDefinition_t::NotForDefinition);
}

static SubstitutionMap lookupBridgeToObjCProtocolSubs(SILModule &mod,
                                                      CanType target) {
  auto bridgedProto =
      mod.getASTContext().getProtocol(KnownProtocolKind::ObjectiveCBridgeable);
  auto conf = mod.getSwiftModule()->lookupConformance(target, bridgedProto);
  return SubstitutionMap::getProtocolSubstitutions(conf.getRequirement(),
                                                   target, conf);
}

/// Given that our insertion point is at the cast that we are trying to
/// optimize, convert our incoming value to something that can be passed to the
/// bridge call.
static std::pair<SILValue, SILInstruction *>
convertObjectToLoadableBridgeableType(SILBuilderWithScope &builder,
                                      SILDynamicCastInst dynamicCast,
                                      SILValue src) {
  auto *f = dynamicCast.getFunction();
  auto loc = dynamicCast.getLocation();
  bool isConditional = dynamicCast.isConditional();

  SILValue load =
      builder.emitLoadValueOperation(loc, src, LoadOwnershipQualifier::Take);

  SILType silBridgedTy = *dynamicCast.getLoweredBridgedTargetObjectType();

  // If we are not conditional...
  if (!isConditional) {
    // and our loaded type is our bridged type, just return the load as our
    // SILValue and signal to our caller that we did not create a new cast
    // instruction by returning nullptr as second.
    if (load->getType() == silBridgedTy) {
      return {load, nullptr};
    }

    // Otherwise, just perform an unconditional checked cast to the sil bridged
    // ty. We return the cast as our value and as our new cast instruction.
    auto *cast =
        builder.createUnconditionalCheckedCast(loc, load, silBridgedTy,
                                               dynamicCast.getBridgedTargetType());
    return {cast, cast};
  }

  SILBasicBlock *castSuccessBB =
      f->createBasicBlockAfter(dynamicCast.getInstruction()->getParent());
  castSuccessBB->createPhiArgument(silBridgedTy, OwnershipKind::Owned);

  // If we /are/ conditional and we do not need to bridge the load to the sil,
  // then we just create our cast success block and branch from the end of the
  // cast instruction block to the cast success block. We leave our insertion
  // point in the cast success block since when we return, we are going to
  // insert the bridge call/switch there. We return the argument of the cast
  // success block as the value to be passed to the bridging function.
  if (load->getType() == silBridgedTy) {
    f->moveBlockAfter(castSuccessBB, dynamicCast.getInstruction()->getParent());
    builder.createBranch(loc, castSuccessBB, load);
    builder.setInsertionPoint(castSuccessBB);
    return {castSuccessBB->getArgument(0), nullptr};
  }

  auto *castFailBB = ([&]() -> SILBasicBlock * {
    auto *failureBB = dynamicCast.getFailureBlock();
    SILBuilderWithScope failureBBBuilder(&(*failureBB->begin()), builder);
    return splitBasicBlockAndBranch(failureBBBuilder, &(*failureBB->begin()),
                                    nullptr, nullptr);
  }());

  // Now that we have created the failure bb, move our cast success block right
  // after the checked_cast_br bb.
  f->moveBlockAfter(castSuccessBB, dynamicCast.getInstruction()->getParent());

  // Ok, we need to perform the full cast optimization. This means that we are
  // going to replace the cast terminator in inst_block with a checked_cast_br.
  auto *ccbi = builder.createCheckedCastBranch(loc, false, load, silBridgedTy,
                                               dynamicCast.getBridgedTargetType(),
                                               castSuccessBB, castFailBB);
  splitEdge(ccbi, /* EdgeIdx to CastFailBB */ 1);

  // Now that we have split the edge to cast fail bb, add the default argument
  // for the checked_cast_br. Then we need to handle our error conditions,
  // namely we destroy on take_always and otherwise store the value back into
  // the memory location that we took it out of.
  {
    auto *newFailureBlock = ccbi->getFailureBB();
    SILValue defaultArg;
    if (builder.hasOwnership()) {
      defaultArg = newFailureBlock->createPhiArgument(load->getType(),
                                                      OwnershipKind::Owned);
    } else {
      defaultArg = ccbi->getOperand();
    }

    // This block should be properly terminated already due to our method of
    // splitting the failure block, so we can use begin() safely.
    SILBuilderWithScope failureBuilder(newFailureBlock->begin());

    switch (dynamicCast.getBridgedConsumptionKind()) {
    case CastConsumptionKind::TakeAlways:
      failureBuilder.emitDestroyValueOperation(loc, defaultArg);
      break;
    case CastConsumptionKind::TakeOnSuccess:
    case CastConsumptionKind::CopyOnSuccess:
      // Without ownership, we do not need to consume the taken value.
      if (failureBuilder.hasOwnership()) {
        failureBuilder.emitStoreValueOperation(loc, defaultArg, src,
                                               StoreOwnershipQualifier::Init);
      }
      break;
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("this should never occur here");
    }
  }

  builder.setInsertionPoint(castSuccessBB);
  return {castSuccessBB->getArgument(0), ccbi};
}

/// Create a call of _forceBridgeFromObjectiveC_bridgeable or
/// _conditionallyBridgeFromObjectiveC_bridgeable which converts an ObjC
/// instance into a corresponding Swift type, conforming to
/// _ObjectiveCBridgeable.
///
/// Control Flow Modification Model
/// ===============================
///
/// NOTE: In the following we assume that our src type is not address only. We
/// do not support optimizing such source types today.
///
/// Unconditional Casts
/// -------------------
///
/// In the case of unconditional casts, we do not touch the CFG at all. We
/// perform the following optimizations:
///
/// 1. If the bridged type and the src type equal, we replace the cast with the
///    apply.
///
/// 2. If src is an address and bridged type has the matching object type to
///    src, just load the value and again replace the cast with the apply.
///
/// 3. If src is an address and after loading still doesn't match bridged type,
///    insert an unconditional_checked_cast before calling the apply.
///
/// Conditional Casts
/// -----------------
///
/// In the case of a conditional const (i.e. checked_cast_addr_br), we transform
/// the following CFG:
///
/// ```
///    InstBlock (checked_cast_addr_br) -> FailureBB -> FailureSucc
///        \
///         \----------------------------> SuccessBB -> SuccessSucc
/// ```
///
/// to a CFG of the following form:
///
/// ```
///   InstBlock (checked_cast_br) -> CastFailBB -> FailureBB -> FailureSucc
///        |                                          ^
///        \-> CastSuccessBB (bridge call + switch) --|
///                 |
///                 \-> BridgeSuccessBB -> SuccessBB -> SuccessSucc
/// ```
///
/// NOTE: That if the underlying src type matches the type of the underlying
/// bridge source object, we can omit the initial checked_cast_br and just load
/// the value + branch to the CastSuccessBB. This results instead in the
/// following CFG:
///
/// ```
///   InstBlock (br)                             FailureBB -> FailureSucc
///        |                                          ^
///        \-> CastSuccessBB (bridge call + switch) --|
///                 |
///                 \-> BridgeSuccessBB -> SuccessBB -> SuccessSucc
/// ```
///
SILInstruction *
CastOptimizer::optimizeBridgedObjCToSwiftCast(SILDynamicCastInst dynamicCast) {
  auto kind = dynamicCast.getKind();
  (void)kind;
  assert(((kind == SILDynamicCastKind::CheckedCastAddrBranchInst) ||
          (kind == SILDynamicCastKind::UnconditionalCheckedCastAddrInst)) &&
         "Unsupported dynamic cast kind");

  CanType target = dynamicCast.getTargetFormalType();
  auto &mod = dynamicCast.getModule();

  // AnyHashable is a special case that we do not handle since we only handle
  // objc targets in this function. Bailout early.
  if (target->isAnyHashable()) {
    return nullptr;
  }

  SILValue src = dynamicCast.getSource();

  SILInstruction *Inst = dynamicCast.getInstruction();
  auto *F = Inst->getFunction();

  // Check if we have a source type that is address only. We do not support that
  // today.
  if (src->getType().isAddressOnly(*F)) {
    return nullptr;
  }

  bool isConditional = dynamicCast.isConditional();
  SILValue Dest = dynamicCast.getDest();
  SILBasicBlock *SuccessBB = dynamicCast.getSuccessBlock();
  SILBasicBlock *FailureBB = dynamicCast.getFailureBlock();
  auto Loc = Inst->getLoc();

  // The conformance to _BridgedToObjectiveC is statically known.
  // Retrieve the bridging operation to be used if a static conformance
  // to _BridgedToObjectiveC can be proven.
  SILFunction *bridgingFunc =
      getObjCToSwiftBridgingFunction(functionBuilder, dynamicCast);
  if (!bridgingFunc)
    return nullptr;

  auto paramTypes = bridgingFunc->getLoweredFunctionType()->getParameters();
  (void)paramTypes;
  assert(paramTypes[0].getConvention() ==
             ParameterConvention::Direct_Guaranteed &&
         "Parameter should be @guaranteed");

  SILBuilderWithScope Builder(Inst, builderContext);

  // Generate a load for the source argument since as part of our optimization
  // we are going to promote the cast to work with objects instead of
  // addresses. Additionally, if we have an objc object that is not bridgeable,
  // but that could be converted to something that is bridgeable, we try to
  // convert to the bridgeable type.
  SILValue srcOp;
  SILInstruction *newI;
  std::tie(srcOp, newI) =
      convertObjectToLoadableBridgeableType(Builder, dynamicCast, src);

  // Now emit the a cast from the casted ObjC object into a target type.
  // This is done by means of calling _forceBridgeFromObjectiveC or
  // _conditionallyBridgeFromObjectiveC_bridgeable from the Target type.
  auto *funcRef = Builder.createFunctionRefFor(Loc, bridgingFunc);
  SubstitutionMap subMap = lookupBridgeToObjCProtocolSubs(mod, target);

  auto MetaTy = MetatypeType::get(target, MetatypeRepresentation::Thick);
  auto SILMetaTy = F->getTypeLowering(MetaTy).getLoweredType();
  auto *MetaTyVal = Builder.createMetatype(Loc, SILMetaTy);

  // Temporary to hold the intermediate result.
  AllocStackInst *Tmp = nullptr;
  CanType OptionalTy;
  SILValue outOptionalParam;
  if (isConditional) {
    // Create a temporary
    OptionalTy = OptionalType::get(Dest->getType().getASTType())
                     ->getImplementationType()
                     ->getCanonicalType();
    Tmp = Builder.createAllocStack(Loc,
                                   SILType::getPrimitiveObjectType(OptionalTy));
    outOptionalParam = Tmp;
  } else {
    outOptionalParam = Dest;
  }

  // Emit a retain.
  SILValue srcArg = Builder.emitCopyValueOperation(Loc, srcOp);

  SmallVector<SILValue, 1> Args;
  Args.push_back(outOptionalParam);
  Args.push_back(srcArg);
  Args.push_back(MetaTyVal);

  auto *AI = Builder.createApply(Loc, funcRef, subMap, Args);

  // If we have guaranteed normal arguments, insert the destroy.
  //
  // TODO: Is it safe to just eliminate the initial retain?
  Builder.emitDestroyOperation(Loc, srcArg);

  // If we have an unconditional_checked_cast_addr, return early. We do not need
  // to handle any conditional code.
  if (isa<UnconditionalCheckedCastAddrInst>(Inst)) {
    // Destroy the source value as unconditional_checked_cast_addr would.
    Builder.emitDestroyOperation(Loc, srcOp);
    eraseInstAction(Inst);
    return (newI) ? newI : AI;
  }

  auto *CCABI = cast<CheckedCastAddrBranchInst>(Inst);
  switch (CCABI->getConsumptionKind()) {
  case CastConsumptionKind::TakeAlways:
    Builder.emitDestroyOperation(Loc, srcOp);
    break;
  case CastConsumptionKind::TakeOnSuccess: {
    {
      // Insert a release in the success BB.
      SILBuilderWithScope successBuilder(SuccessBB->begin());
      successBuilder.emitDestroyOperation(Loc, srcOp);
    }
    {
      // And a store in the failure BB.
      if (Builder.hasOwnership()) {
        SILBuilderWithScope failureBuilder(FailureBB->begin());
        SILValue writeback = srcOp;
        SILType srcType = src->getType().getObjectType();
        if (writeback->getType() != srcType) {
          writeback =
              failureBuilder.createUncheckedRefCast(Loc, writeback, srcType);
        }
        failureBuilder.emitStoreValueOperation(Loc, writeback, src,
                                               StoreOwnershipQualifier::Init);
      }
    }
    break;
  }
  case CastConsumptionKind::BorrowAlways:
    llvm_unreachable("checked_cast_addr_br never has BorrowAlways");
  case CastConsumptionKind::CopyOnSuccess:
    // If we are performing copy_on_success, store the value back into memory
    // here since we loaded it. We may need to cast back to the actual
    // underlying type.
    if (Builder.hasOwnership()) {
      SILValue writeback = srcOp;
      SILType srcType = src->getType().getObjectType();
      if (writeback->getType() != srcType) {
        writeback = Builder.createUncheckedRefCast(Loc, writeback, srcType);
      }
      Builder.emitStoreValueOperation(Loc, writeback, src,
                                      StoreOwnershipQualifier::Init);
    }
    break;
  }

  // Results should be checked in case we process a conditional
  // case. E.g. casts from NSArray into [SwiftType] may fail, i.e. return .None.
  if (isConditional) {
    // Copy the temporary into Dest.
    // Load from the optional.
    auto *SomeDecl = Builder.getASTContext().getOptionalSomeDecl();

    auto *BridgeSuccessBB =
        Inst->getFunction()->createBasicBlockAfter(Builder.getInsertionBB());
    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 2> CaseBBs;
    CaseBBs.emplace_back(SomeDecl, BridgeSuccessBB);
    CaseBBs.emplace_back(mod.getASTContext().getOptionalNoneDecl(), FailureBB);

    Builder.createSwitchEnumAddr(Loc, outOptionalParam, nullptr, CaseBBs);

    Builder.setInsertionPoint(FailureBB->begin());
    Builder.createDeallocStack(Loc, Tmp);

    Builder.setInsertionPoint(BridgeSuccessBB);
    auto Addr = Builder.createUncheckedTakeEnumDataAddr(Loc, outOptionalParam,
                                                        SomeDecl);

    Builder.createCopyAddr(Loc, Addr, Dest, IsTake, IsInitialization);

    Builder.createDeallocStack(Loc, Tmp);
    SmallVector<SILValue, 1> SuccessBBArgs;
    Builder.createBranch(Loc, SuccessBB, SuccessBBArgs);
  }

  eraseInstAction(Inst);
  return (newI) ? newI : AI;
}

//===----------------------------------------------------------------------===//
//                  Swift -> ObjC Bridging Cast Optimization
//===----------------------------------------------------------------------===//

static bool canOptimizeCast(const swift::Type &BridgedTargetTy,
                            swift::SILModule &M,
                            swift::SILFunctionConventions &substConv,
                            TypeExpansionContext context) {
  // DestTy is the type which we want to convert to
  SILType DestTy =
      SILType::getPrimitiveObjectType(BridgedTargetTy->getCanonicalType());
  // ConvTy  is the return type of the _bridgeToObjectiveCImpl()
  auto ConvTy = substConv.getSILResultType(context).getObjectType();
  if (ConvTy == DestTy) {
    // Destination is the same type
    return true;
  }
  // Check if a superclass/subclass of the source operand
  if (DestTy.isExactSuperclassOf(ConvTy)) {
    return true;
  }
  if (ConvTy.isExactSuperclassOf(DestTy)) {
    return true;
  }
  // check if it is a bridgeable CF type
  if (ConvTy.getASTType() ==
      getNSBridgedClassOfCFClass(M.getSwiftModule(),
                                 DestTy.getASTType())) {
    return true;
  }
  if (DestTy.getASTType() ==
      getNSBridgedClassOfCFClass(M.getSwiftModule(),
                                 ConvTy.getASTType())) {
    return true;
  }
  // All else failed - can't optimize this case
  return false;
}

static Optional<std::pair<SILFunction *, SubstitutionMap>>
findBridgeToObjCFunc(SILOptFunctionBuilder &functionBuilder,
                     SILDynamicCastInst dynamicCast) {
  CanType sourceFormalType = dynamicCast.getSourceFormalType();
  auto loc = dynamicCast.getLocation();
  auto &mod = dynamicCast.getModule();
  auto bridgedProto =
      mod.getASTContext().getProtocol(KnownProtocolKind::ObjectiveCBridgeable);

  auto conf = mod.getSwiftModule()->lookupConformance(
    sourceFormalType, bridgedProto);
  assert(conf && "_ObjectiveCBridgeable conformance should exist");
  (void)conf;

  // Generate code to invoke _bridgeToObjectiveC
  ModuleDecl *modDecl =
      mod.getASTContext().getLoadedModule(mod.getASTContext().Id_Foundation);
  if (!modDecl)
    return None;
  SmallVector<ValueDecl *, 2> results;
  modDecl->lookupMember(results,
                        sourceFormalType.getNominalOrBoundGenericNominal(),
                        mod.getASTContext().Id_bridgeToObjectiveC,
                        Identifier());
  ArrayRef<ValueDecl *> resultsRef(results);
  if (resultsRef.empty()) {
    mod.getSwiftModule()->lookupMember(
        results, sourceFormalType.getNominalOrBoundGenericNominal(),
        mod.getASTContext().Id_bridgeToObjectiveC, Identifier());
    resultsRef = results;
  }
  if (resultsRef.size() != 1)
    return None;

  auto *resultDecl = results.front();
  auto memberDeclRef = SILDeclRef(resultDecl);
  auto *bridgedFunc = functionBuilder.getOrCreateFunction(
      loc, memberDeclRef, ForDefinition_t::NotForDefinition);

  // Get substitutions, if source is a bound generic type.
  auto subMap = sourceFormalType->getContextSubstitutionMap(
      mod.getSwiftModule(), resultDecl->getDeclContext());

  // Implementation of _bridgeToObjectiveC could not be found.
  if (!bridgedFunc)
    return None;

  if (dynamicCast.getFunction()->isSerialized() &&
      !bridgedFunc->hasValidLinkageForFragileRef())
    return None;

  if (bridgedFunc->getLoweredFunctionType()
          ->getSingleResult()
          .isFormalIndirect())
    return None;
  return std::make_pair(bridgedFunc, subMap);
}

static SILValue computeFinalCastedValue(SILBuilderWithScope &builder,
                                        SILDynamicCastInst dynamicCast,
                                        ApplyInst *newAI) {
  auto loc = dynamicCast.getLocation();
  auto convTy = newAI->getType();
  bool isConditional = dynamicCast.isConditional();
  auto destLoweredTy = dynamicCast.getTargetLoweredType().getObjectType();
  auto destFormalTy = dynamicCast.getTargetFormalType();
  assert(destLoweredTy == dynamicCast.getLoweredBridgedTargetObjectType() &&
         "Expected Dest Type to be the same as BridgedTargetTy");

  auto &m = dynamicCast.getModule();
  if (convTy == destLoweredTy) {
    return newAI;
  }

  if (destLoweredTy.isExactSuperclassOf(convTy)) {
    return builder.createUpcast(loc, newAI, destLoweredTy);
  }

  if (convTy.isExactSuperclassOf(destLoweredTy)) {
    // If we are not conditional, we are ok with the downcast via checked cast
    // fails since we will trap.
    if (!isConditional) {
      return builder.createUnconditionalCheckedCast(loc, newAI,
                                                    destLoweredTy, destFormalTy);
    }

    // Otherwise if we /are/ emitting a conditional cast, make sure that we
    // handle the failure gracefully.
    //
    // Since we are being returned the value at +1, we need to destroy the
    // newAI on failure.
    auto *failureBB = dynamicCast.getFailureBlock();
    {
      SILBuilderWithScope innerBuilder(&*failureBB->begin(), builder);
      auto valueToDestroy = ([&]() -> SILValue {
        if (!innerBuilder.hasOwnership())
          return newAI;
        return failureBB->createPhiArgument(newAI->getType(),
                                            OwnershipKind::Owned);
      }());
      innerBuilder.emitDestroyOperation(loc, valueToDestroy);
    }

    auto *condBrSuccessBB =
        newAI->getFunction()->createBasicBlockAfter(newAI->getParent());
    condBrSuccessBB->createPhiArgument(destLoweredTy, OwnershipKind::Owned);
    builder.createCheckedCastBranch(loc, /* isExact*/ false, newAI,
                                    destLoweredTy, destFormalTy,
                                    condBrSuccessBB, failureBB);
    builder.setInsertionPoint(condBrSuccessBB, condBrSuccessBB->begin());
    return condBrSuccessBB->getArgument(0);
  }

  if (convTy.getASTType() ==
          getNSBridgedClassOfCFClass(m.getSwiftModule(), destLoweredTy.getASTType()) ||
      destLoweredTy.getASTType() ==
          getNSBridgedClassOfCFClass(m.getSwiftModule(), convTy.getASTType())) {
    // Handle NS <-> CF toll-free bridging here.
    return SILValue(builder.createUncheckedRefCast(loc, newAI, destLoweredTy));
  }

  llvm_unreachable(
      "optimizeBridgedSwiftToObjCCast: should never reach this condition: if "
      "the Destination does not have the same type, is not a bridgeable CF "
      "type and isn't a superclass/subclass of the source operand we should "
      "have bailed earlier.");
}

/// Create a call of _bridgeToObjectiveC which converts an _ObjectiveCBridgeable
/// instance into a bridged ObjC type.
SILInstruction *
CastOptimizer::optimizeBridgedSwiftToObjCCast(SILDynamicCastInst dynamicCast) {
  SILInstruction *Inst = dynamicCast.getInstruction();
  const SILFunction *F = Inst->getFunction();
  CastConsumptionKind ConsumptionKind = dynamicCast.getBridgedConsumptionKind();
  bool isConditional = dynamicCast.isConditional();
  SILValue Src = dynamicCast.getSource();
  SILValue Dest = dynamicCast.getDest();
  CanType BridgedTargetTy = dynamicCast.getBridgedTargetType();
  SILBasicBlock *SuccessBB = dynamicCast.getSuccessBlock();
  SILBasicBlock *FailureBB = dynamicCast.getFailureBlock();
  auto &M = Inst->getModule();
  auto Loc = Inst->getLoc();

  bool AddressOnlyType = false;
  if (!Src->getType().isLoadable(*F) || !Dest->getType().isLoadable(*F)) {
    AddressOnlyType = true;
  }

  // Find the _BridgedToObjectiveC protocol.
  SILFunction *bridgedFunc = nullptr;
  SubstitutionMap subMap;
  {
    auto result = findBridgeToObjCFunc(functionBuilder, dynamicCast);
    if (!result)
      return nullptr;
    std::tie(bridgedFunc, subMap) = result.value();
  }

  SILType SubstFnTy = bridgedFunc->getLoweredType().substGenericArgs(
      M, subMap, TypeExpansionContext(*F));
  SILFunctionConventions substConv(SubstFnTy.castTo<SILFunctionType>(), M);

  // Check that this is a case that the authors of this code thought it could
  // handle.
  if (!canOptimizeCast(BridgedTargetTy, M, substConv,
                       F->getTypeExpansionContext())) {
    return nullptr;
  }

  SILBuilderWithScope Builder(Inst, builderContext);
  auto FnRef = Builder.createFunctionRefFor(Loc, bridgedFunc);
  auto ParamTypes = SubstFnTy.castTo<SILFunctionType>()->getParameters();
  SILValue oldSrc;
  if (Src->getType().isAddress() && !substConv.isSILIndirect(ParamTypes[0])) {
    // Create load
    oldSrc = Src;
    Src =
        Builder.emitLoadValueOperation(Loc, Src, LoadOwnershipQualifier::Take);
  }

  // Compensate different owning conventions of the replaced cast instruction
  // and the inserted conversion function.
  bool needReleaseAfterCall = false;
  bool needReleaseInSuccess = false;
  switch (ParamTypes[0].getConvention()) {
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Indirect_In_Guaranteed:
    switch (ConsumptionKind) {
    case CastConsumptionKind::TakeAlways:
      needReleaseAfterCall = true;
      break;
    case CastConsumptionKind::TakeOnSuccess:
      needReleaseInSuccess = true;
      break;
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("Should never hit this");
    case CastConsumptionKind::CopyOnSuccess:
      // We assume that our caller is correct and will treat our argument as
      // being immutable, so we do not need to do anything here.
      break;
    }
    break;
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Indirect_In:
    // Currently this
    // cannot appear, because the _bridgeToObjectiveC protocol witness method
    // always receives the this pointer (= the source) as guaranteed.
    // If it became possible (perhaps with the advent of ownership and
    // explicit +1 annotations), the implementation should look something
    // like this:
    /*
    switch (ConsumptionKind) {
      case CastConsumptionKind::TakeAlways:
        break;
      case CastConsumptionKind::TakeOnSuccess:
        needRetainBeforeCall = true;
        needReleaseInSuccess = true;
        break;
      case CastConsumptionKind::CopyOnSuccess:
        needRetainBeforeCall = true;
        break;
    }
    break;
     */
    llvm_unreachable("this should never happen so is currently untestable");
  case ParameterConvention::Direct_Unowned:
    assert(!AddressOnlyType &&
           "AddressOnlyType with Direct_Unowned is not supported");
    break;
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    // TODO handle remaining indirect argument types
    return nullptr;
  }

  // Generate a code to invoke the bridging function.
  auto *NewAI = Builder.createApply(Loc, FnRef, subMap, Src);

  // First if we are going to destroy the value unconditionally, just insert the
  // destroy right after the call. This handles some of the conditional cases
  // and /all/ of the consuming unconditional cases.
  if (needReleaseAfterCall) {
    Builder.emitDestroyOperation(Loc, Src);
  } else {
    if (SuccessBB) {
      SILBuilderWithScope succBuilder(&*SuccessBB->begin(), Builder);
      if (needReleaseInSuccess) {
        succBuilder.emitDestroyOperation(Loc, Src);
      } else {
        if (oldSrc) {
          succBuilder.emitStoreValueOperation(Loc, Src, oldSrc,
                                              StoreOwnershipQualifier::Init);
        }
      }
      SILBuilderWithScope failBuilder(&*FailureBB->begin(), Builder);
      if (oldSrc) {
        failBuilder.emitStoreValueOperation(Loc, Src, oldSrc,
                                            StoreOwnershipQualifier::Init);
      }
    } else {
      if (oldSrc) {
        Builder.emitStoreValueOperation(Loc, Src, oldSrc,
                                        StoreOwnershipQualifier::Init);
      }
    }
  }

  if (!Dest)
    return NewAI;

  // If it is addr cast then store the result into the dest.
  //
  // NOTE: We assume that dest was uninitialized when passed to us.
  SILValue castedValue = computeFinalCastedValue(Builder, dynamicCast, NewAI);
  auto qual = Builder.hasOwnership() ? StoreOwnershipQualifier::Init
                                     : StoreOwnershipQualifier::Unqualified;
  SILInstruction *NewI = Builder.createStore(Loc, castedValue, Dest, qual);
  if (isConditional && NewI->getParent() != NewAI->getParent()) {
    Builder.createBranch(Loc, SuccessBB);
  }

  eraseInstAction(Inst);
  return NewI;
}

//===----------------------------------------------------------------------===//
//               Top Level Bridge Cast Optimization Entrypoint
//===----------------------------------------------------------------------===//

/// Make use of the fact that some of these casts cannot fail.  For
/// example, if the ObjC type is exactly the expected _ObjectiveCType
/// type, then it would always succeed for NSString, NSNumber, etc.
/// Casts from NSArray, NSDictionary and NSSet may fail.
///
/// If ObjC class is not exactly _ObjectiveCType, then its conversion
/// to a required _ObjectiveCType may fail.
SILInstruction *
CastOptimizer::optimizeBridgedCasts(SILDynamicCastInst dynamicCast) {
  CanType source = dynamicCast.getSourceFormalType();
  CanType target = dynamicCast.getTargetFormalType();
  auto &M = dynamicCast.getModule();

  // To apply the bridged optimizations, we should ensure that types are not
  // existential (and keep in mind that generic parameters can be existentials),
  // and that one of the types is a class and another one is a struct.
  if (source.isAnyExistentialType() || target.isAnyExistentialType() ||
      source->is<ArchetypeType>() || target->is<ArchetypeType>() ||
      (source.getClassOrBoundGenericClass() &&
       !target.getStructOrBoundGenericStruct()) ||
      (target.getClassOrBoundGenericClass() &&
       !source.getStructOrBoundGenericStruct()))
    return nullptr;

  // Casts involving non-bound generic types cannot be optimized.
  if (source->hasArchetype() || target->hasArchetype())
    return nullptr;

  CanType CanBridgedSourceTy = dynamicCast.getBridgedSourceType();
  CanType CanBridgedTargetTy = dynamicCast.getBridgedTargetType();

  // If we were unable to bridge either of our source/target types, return
  // nullptr.
  if (!CanBridgedSourceTy || !CanBridgedTargetTy)
    return nullptr;

  if (CanBridgedSourceTy == source && CanBridgedTargetTy == target) {
    // Both source and target type are ObjC types.
    return nullptr;
  }

  if (CanBridgedSourceTy != source && CanBridgedTargetTy != target) {
    // Both source and target type are Swift types.
    return nullptr;
  }

  if ((CanBridgedSourceTy && CanBridgedSourceTy->getAnyNominal() ==
                                 M.getASTContext().getNSErrorDecl()) ||
      (CanBridgedTargetTy && CanBridgedTargetTy->getAnyNominal() ==
                                 M.getASTContext().getNSErrorDecl())) {
    // FIXME: Can't optimize bridging with NSError.
    return nullptr;
  }

  // Check what kind of conversion it is? ObjC->Swift or Swift-ObjC?
  if (CanBridgedTargetTy != target) {
    // This is an ObjC to Swift cast.
    return optimizeBridgedObjCToSwiftCast(dynamicCast);
  } else {
    // This is a Swift to ObjC cast
    return optimizeBridgedSwiftToObjCCast(dynamicCast);
  }

  llvm_unreachable("Unknown kind of bridging");
}

//===----------------------------------------------------------------------===//
//                         Cast Optimizer Public API
//===----------------------------------------------------------------------===//

SILInstruction *CastOptimizer::simplifyCheckedCastAddrBranchInst(
    CheckedCastAddrBranchInst *Inst) {
  if (auto *I = optimizeCheckedCastAddrBranchInst(Inst))
    Inst = dyn_cast<CheckedCastAddrBranchInst>(I);

  if (!Inst)
    return nullptr;

  SILDynamicCastInst dynamicCast(Inst);
  auto Loc = dynamicCast.getLocation();
  auto Src = dynamicCast.getSource();
  auto Dest = dynamicCast.getDest();
  auto *SuccessBB = dynamicCast.getSuccessBlock();
  auto *FailureBB = dynamicCast.getFailureBlock();

  SILBuilderWithScope Builder(Inst, builderContext);

  // Check if we can statically predict the outcome of the cast.
  auto Feasibility =
      dynamicCast.classifyFeasibility(true /*allow whole module*/);

  if (Feasibility == DynamicCastFeasibility::WillFail) {
    if (shouldDestroyOnFailure(Inst->getConsumptionKind())) {
      auto &srcTL = Builder.getTypeLowering(Src->getType());
      srcTL.emitDestroyAddress(Builder, Loc, Src);
    }
    auto NewI = Builder.createBranch(Loc, FailureBB);
    eraseInstAction(Inst);
    willFailAction();
    return NewI;
  }

  bool ResultNotUsed = isa<AllocStackInst>(Dest);
  if (ResultNotUsed) {
    for (auto Use : Dest->getUses()) {
      auto *User = Use->getUser();
      if (isa<DeallocStackInst>(User) || isa<DestroyAddrInst>(User) ||
          User == Inst)
        continue;
      ResultNotUsed = false;
      break;
    }
  }

  auto *BB = Inst->getParent();

  SILInstruction *BridgedI = nullptr;

  // To apply the bridged optimizations, we should
  // ensure that types are not existential,
  // and that not both types are classes.
  BridgedI = optimizeBridgedCasts(dynamicCast);

  if (!BridgedI) {
    // If the cast may succeed or fail, and it can't be optimized into a
    // bridging operation, then let it be.
    if (Feasibility == DynamicCastFeasibility::MaySucceed) {
      return nullptr;
    }

    assert(Feasibility == DynamicCastFeasibility::WillSucceed);

    // Replace by unconditional_addr_cast, followed by a branch.
    // The unconditional_addr_cast can be skipped, if the result of a cast
    // is not used afterwards.
    if (ResultNotUsed) {
      if (shouldTakeOnSuccess(Inst->getConsumptionKind())) {
        auto &srcTL = Builder.getTypeLowering(Src->getType());
        srcTL.emitDestroyAddress(Builder, Loc, Src);
      }
      for (auto iter = Dest->use_begin(); iter != Dest->use_end();) {
        SILInstruction *user = (*iter++)->getUser();
        if (isa<DestroyAddrInst>(user))
          eraseInstAction(user);
      }
      eraseInstAction(Inst);
      Builder.setInsertionPoint(BB);
      auto *NewI = Builder.createBranch(Loc, SuccessBB);
      willSucceedAction();
      return NewI;
    }

    // Since it is an addr cast, only address types are handled here.
    if (!Src->getType().isAddress() || !Dest->getType().isAddress()) {
      return nullptr;
    }

    // Both TakeOnSuccess and TakeAlways can be reduced to an
    // UnconditionalCheckedCast, since the failure path is irrelevant.
    AllocStackInst *copiedSrc = nullptr;
    switch (Inst->getConsumptionKind()) {
    case CastConsumptionKind::BorrowAlways:
      llvm_unreachable("checked_cast_addr_br never has BorrowAlways");
    case CastConsumptionKind::CopyOnSuccess:
      if (!Src->getType().isTrivial(*BB->getParent())) {
        copiedSrc = Builder.createAllocStack(Loc, Src->getType());
        Builder.createCopyAddr(Loc, Src, copiedSrc, IsNotTake, IsInitialization);
        Src = copiedSrc;
      }
      break;
    case CastConsumptionKind::TakeAlways:
    case CastConsumptionKind::TakeOnSuccess:
      break;
    }

    bool result = emitSuccessfulIndirectUnconditionalCast(
      Builder, Builder.getModule().getSwiftModule(), Loc, Src,
      Inst->getSourceFormalType(), Dest, Inst->getTargetFormalType(), Inst);
    (void)result;
    assert(result && "emit cannot fail for an checked_cast_addr_br");

    if (copiedSrc)
      Builder.createDeallocStack(Loc, copiedSrc);
    eraseInstAction(Inst);
  }
  SILInstruction *NewI = &BB->back();
  if (!isa<TermInst>(NewI)) {
    Builder.setInsertionPoint(BB);
    NewI = Builder.createBranch(Loc, SuccessBB);
  }
  willSucceedAction();
  return NewI;
}

SILInstruction *
CastOptimizer::simplifyCheckedCastBranchInst(CheckedCastBranchInst *Inst) {
  if (Inst->isExact()) {
    SILDynamicCastInst dynamicCast(Inst);
    auto *ARI = dyn_cast<AllocRefInst>(stripUpCasts(dynamicCast.getSource()));
    if (!ARI)
      return nullptr;

    // We know the dynamic type of the operand.
    SILBuilderWithScope Builder(Inst, builderContext);
    auto Loc = dynamicCast.getLocation();

    if (ARI->getType() == dynamicCast.getTargetLoweredType()) {
      // This exact cast will succeed.
      SmallVector<SILValue, 1> Args;
      Args.push_back(ARI);
      auto *NewI =
          Builder.createBranch(Loc, dynamicCast.getSuccessBlock(), Args);
      eraseInstAction(Inst);
      willSucceedAction();
      return NewI;
    }

    // This exact cast will fail. With ownership enabled, we pass a copy of the
    // original casts value to the failure block.
    TinyPtrVector<SILValue> Args;
    if (Builder.hasOwnership())
      Args.push_back(dynamicCast.getSource());
    auto *NewI = Builder.createBranch(Loc, dynamicCast.getFailureBlock(), Args);
    eraseInstAction(Inst);
    willFailAction();
    return NewI;
  }

  if (auto *I = optimizeCheckedCastBranchInst(Inst))
    Inst = dyn_cast<CheckedCastBranchInst>(I);

  if (!Inst)
    return nullptr;

  SILDynamicCastInst dynamicCast(Inst);
  auto TargetLoweredType = dynamicCast.getTargetLoweredType();
  auto TargetFormalType = dynamicCast.getTargetFormalType();
  auto Loc = dynamicCast.getLocation();
  auto *SuccessBB = dynamicCast.getSuccessBlock();
  auto *FailureBB = dynamicCast.getFailureBlock();
  auto Op = dynamicCast.getSource();
  auto *F = dynamicCast.getFunction();

  // Check if we can statically predict the outcome of the cast.
  auto Feasibility =
      dynamicCast.classifyFeasibility(false /*allow whole module*/);
  if (Feasibility == DynamicCastFeasibility::MaySucceed) {
    return nullptr;
  }

  SILBuilderWithScope Builder(Inst, builderContext);
  if (Feasibility == DynamicCastFeasibility::WillFail) {
    auto *NewI = Builder.createBranch(Loc, FailureBB);
    if (Builder.hasOwnership()) {
      FailureBB->getArgument(0)->replaceAllUsesWith(Op);
      FailureBB->eraseArgument(0);
      SuccessBB->getArgument(0)->replaceAllUsesWithUndef();
      SuccessBB->eraseArgument(0);
    }
    eraseInstAction(Inst);
    willFailAction();
    return NewI;
  }

  assert(Feasibility == DynamicCastFeasibility::WillSucceed);

  bool ResultNotUsed = SuccessBB->getArgument(0)->use_empty();
  SILValue CastedValue;
  if (Op->getType() != TargetLoweredType) {
    // Apply the bridged cast optimizations.
    //
    // TODO: Bridged casts cannot be expressed by checked_cast_br yet.
    // Should we ever support it, please review this code.
    auto BridgedI = optimizeBridgedCasts(dynamicCast);

    if (BridgedI) {
      llvm_unreachable(
          "Bridged casts cannot be expressed by checked_cast_br yet");
    } else {
      // Replace by unconditional_cast, followed by a branch.
      // The unconditional_cast can be skipped, if the result of a cast
      // is not used afterwards.
      if (!ResultNotUsed) {
        if (!dynamicCast.canSILUseScalarCheckedCastInstructions())
          return nullptr;

        CastedValue =
            emitSuccessfulScalarUnconditionalCast(Builder, Loc, dynamicCast);
      } else {
        CastedValue = SILUndef::get(TargetLoweredType, *F);
      }
      if (!CastedValue)
        CastedValue =
            Builder.createUnconditionalCheckedCast(
              Loc, Op, TargetLoweredType, TargetFormalType);
    }

  } else {
    // No need to cast.
    CastedValue = Op;
  }

  BranchInst *NewI = nullptr;

  if (Builder.hasOwnership()) {
    NewI = Builder.createBranch(Loc, SuccessBB);
    SuccessBB->getArgument(0)->replaceAllUsesWith(CastedValue);
    SuccessBB->eraseArgument(0);
    FailureBB->getArgument(0)->replaceAllUsesWithUndef();
    FailureBB->eraseArgument(0);
  }
  else {
    NewI = Builder.createBranch(Loc, SuccessBB, CastedValue);
  }

  eraseInstAction(Inst);
  willSucceedAction();
  return NewI;
}

SILInstruction *CastOptimizer::optimizeCheckedCastAddrBranchInst(
    CheckedCastAddrBranchInst *Inst) {
  auto Loc = Inst->getLoc();
  auto Src = Inst->getSrc();
  auto Dest = Inst->getDest();
  auto *SuccessBB = Inst->getSuccessBB();
  auto *FailureBB = Inst->getFailureBB();

  // If there is an unbound generic type involved in the cast, bail.
  if (Src->getType().hasArchetype() || Dest->getType().hasArchetype())
    return nullptr;

  // %1 = metatype $A.Type
  // [%2 = init_existential_metatype %1 ...]
  // %3 = alloc_stack
  // store %1 to %3 or store %2 to %3
  // checked_cast_addr_br %3 to ...
  // ->
  // %1 = metatype $A.Type
  // %c = checked_cast_br %1 to ...
  // store %c to %3 (if successful)
  if (auto *ASI = dyn_cast<AllocStackInst>(Src)) {
    // Check if the value of this alloc_stack is set only once by a store
    // instruction, used only by CCABI and then deallocated.
    bool isLegal = true;
    StoreInst *Store = nullptr;
    for (auto Use : ASI->getUses()) {
      auto *User = Use->getUser();
      if (isa<DeallocStackInst>(User) || User == Inst)
        continue;
      if (auto *SI = dyn_cast<StoreInst>(User)) {
        if (SI->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
          // We do not handle [assign]
          isLegal = false;
          break;
        }
        if (!Store) {
          Store = SI;
          continue;
        }
      }
      isLegal = false;
      break;
    }

    if (isLegal && Store) {
      // Check what was the value stored in the allocated stack slot.
      auto Src = Store->getSrc();
      MetatypeInst *MI = nullptr;
      if (auto *IEMI = dyn_cast<InitExistentialMetatypeInst>(Src)) {
        MI = dyn_cast<MetatypeInst>(IEMI->getOperand());
      }

      if (!MI)
        MI = dyn_cast<MetatypeInst>(Src);

      if (MI) {
        if (SuccessBB->getSinglePredecessorBlock() &&
            canSILUseScalarCheckedCastInstructions(
                Inst->getModule(), MI->getType().getASTType(),
                Inst->getTargetFormalType())) {
          SILBuilderWithScope B(Inst, builderContext);
          auto NewI = B.createCheckedCastBranch(
              Loc, false /*isExact*/, MI,
              Inst->getTargetLoweredType().getObjectType(),
              Inst->getTargetFormalType(),
              SuccessBB, FailureBB, Inst->getTrueBBCount(),
              Inst->getFalseBBCount());
          SuccessBB->createPhiArgument(Dest->getType().getObjectType(),
                                       OwnershipKind::Owned);
          B.setInsertionPoint(SuccessBB->begin());
          // Store the result
          B.emitStoreValueOperation(Loc, SuccessBB->getArgument(0), Dest,
                                    StoreOwnershipQualifier::Trivial);
          if (B.hasOwnership())
            FailureBB->createPhiArgument(MI->getType(), OwnershipKind::None);
          eraseInstAction(Inst);
          return NewI;
        }
      }
    }
  }
  return nullptr;
}

SILInstruction *
CastOptimizer::optimizeCheckedCastBranchInst(CheckedCastBranchInst *Inst) {
  if (Inst->isExact())
    return nullptr;

  // InstOptUtils.helper we use to simplify replacing a checked_cast_branch with
  // an optimized checked cast branch.
  auto replaceCastHelper = [](SILBuilderWithScope &B,
                              SILDynamicCastInst dynamicCast,
                              MetatypeInst *mi) -> SILInstruction * {
    // Make sure that the failure block has the new metatype type for
    // its default argument as required when we are in ossa
    // mode. Without ossa, failure blocks do not have args, so we do
    // not need to do anything.
    auto *fBlock = dynamicCast.getFailureBlock();
    if (B.hasOwnership()) {
      fBlock->replacePhiArgumentAndReplaceAllUses(0, mi->getType(),
                                                  OwnershipKind::None);
    }
    return B.createCheckedCastBranch(
        dynamicCast.getLocation(), false /*isExact*/, mi,
        dynamicCast.getTargetLoweredType(),
        dynamicCast.getTargetFormalType(),
        dynamicCast.getSuccessBlock(),
        fBlock, *dynamicCast.getSuccessBlockCount(),
        *dynamicCast.getFailureBlockCount());
  };

  SILDynamicCastInst dynamicCast(Inst);

  auto Op = dynamicCast.getSource();

  // Try to simplify checked_cond_br instructions using existential
  // metatypes by propagating a concrete type whenever it can be
  // determined statically.

  // %0 = metatype $A.Type
  // %1 = init_existential_metatype ..., %0: $A
  // checked_cast_br %1, ....
  // ->
  // %0 = metatype $A.Type
  // checked_cast_br %0 to ...
  if (auto *IEMI = dyn_cast<InitExistentialMetatypeInst>(Op)) {
    if (auto *MI = dyn_cast<MetatypeInst>(IEMI->getOperand())) {
      SILBuilderWithScope B(Inst, builderContext);
      auto *NewI = replaceCastHelper(B, dynamicCast, MI);
      eraseInstAction(Inst);
      return NewI;
    }
  }

  if (auto *EMI = dyn_cast<ExistentialMetatypeInst>(Op)) {
    // Operand of the existential_metatype instruction.
    auto Op = EMI->getOperand();
    auto EmiTy = EMI->getType();

    // %0 = alloc_stack $T
    // %1 = init_existential_addr %0: $*T, $A
    // %2 = existential_metatype $T.Type, %0: $*T
    // checked_cast_br %2 to ...
    // ->
    // %1 = metatype $A.Type
    // checked_cast_br %1 to ...

    if (auto *ASI = dyn_cast<AllocStackInst>(Op)) {
      // Should be in the same BB.
      if (ASI->getParent() != EMI->getParent())
        return nullptr;
      // Check if this alloc_stack is only initialized once by means of
      // single init_existential_addr.
      bool isLegal = true;
      // init_existential instruction used to initialize this alloc_stack.
      InitExistentialAddrInst *FoundIEI = nullptr;
      for (auto Use : getNonDebugUses(ASI)) {
        auto *User = Use->getUser();
        if (isa<ExistentialMetatypeInst>(User) || isa<DestroyAddrInst>(User) ||
            isa<DeallocStackInst>(User))
          continue;
        if (auto *IEI = dyn_cast<InitExistentialAddrInst>(User)) {
          if (!FoundIEI) {
            FoundIEI = IEI;
            continue;
          }
        }
        isLegal = false;
        break;
      }

      if (isLegal && FoundIEI) {
        // Should be in the same BB.
        if (FoundIEI->getParent() != EMI->getParent())
          return nullptr;
        // Get the type used to initialize the existential.
        auto LoweredConcreteTy = FoundIEI->getLoweredConcreteType();
        // We don't know enough at compile time about existential
        // and generic type parameters.
        if (LoweredConcreteTy.isAnyExistentialType() ||
            LoweredConcreteTy.is<ArchetypeType>())
          return nullptr;
        // Get the metatype of this type.
        auto EMT = EmiTy.castTo<AnyMetatypeType>();
        auto *MetaTy = MetatypeType::get(LoweredConcreteTy.getASTType(),
                                         EMT->getRepresentation());
        auto CanMetaTy = CanTypeWrapper<MetatypeType>(MetaTy);
        auto SILMetaTy = SILType::getPrimitiveObjectType(CanMetaTy);
        SILBuilderWithScope B(Inst, builderContext);
        auto *MI = B.createMetatype(FoundIEI->getLoc(), SILMetaTy);
        auto *NewI = replaceCastHelper(B, dynamicCast, MI);
        eraseInstAction(Inst);
        return NewI;
      }
    }

    // %0 = alloc_ref $A
    // %1 = init_existential_ref %0: $A, $...
    // %2 = existential_metatype ..., %1 :  ...
    // checked_cast_br %2, ....
    // ->
    // %1 = metatype $A.Type
    // checked_cast_br %1, ....
    if (auto *FoundIERI = dyn_cast<InitExistentialRefInst>(Op)) {
      auto *ASRI = dyn_cast<AllocRefInst>(FoundIERI->getOperand());
      if (!ASRI)
        return nullptr;
      // Should be in the same BB.
      if (ASRI->getParent() != EMI->getParent())
        return nullptr;
      // Check if this alloc_stack is only initialized once by means of
      // a single init_existential_ref.
      bool isLegal = true;
      for (auto Use : getNonDebugUses(ASRI)) {
        auto *User = Use->getUser();
        if (isa<ExistentialMetatypeInst>(User) || isa<StrongReleaseInst>(User))
          continue;
        if (auto *IERI = dyn_cast<InitExistentialRefInst>(User)) {
          if (IERI == FoundIERI) {
            continue;
          }
        }
        isLegal = false;
        break;
      }

      if (isLegal && FoundIERI) {
        // Should be in the same BB.
        if (FoundIERI->getParent() != EMI->getParent())
          return nullptr;
        // Get the type used to initialize the existential.
        auto ConcreteTy = FoundIERI->getFormalConcreteType();
        // We don't know enough at compile time about existential
        // and generic type parameters.
        if (ConcreteTy.isAnyExistentialType() ||
            ConcreteTy->is<ArchetypeType>())
          return nullptr;
        // Get the SIL metatype of this type.
        auto EMT = EMI->getType().castTo<AnyMetatypeType>();
        auto *MetaTy = MetatypeType::get(ConcreteTy, EMT->getRepresentation());
        auto CanMetaTy = CanTypeWrapper<MetatypeType>(MetaTy);
        auto SILMetaTy = SILType::getPrimitiveObjectType(CanMetaTy);
        SILBuilderWithScope B(Inst, builderContext);
        auto *MI = B.createMetatype(FoundIERI->getLoc(), SILMetaTy);
        auto *NewI = replaceCastHelper(B, dynamicCast, MI);
        eraseInstAction(Inst);
        return NewI;
      }
    }
  }

  return nullptr;
}

ValueBase *CastOptimizer::optimizeUnconditionalCheckedCastInst(
    UnconditionalCheckedCastInst *Inst) {
  SILDynamicCastInst dynamicCast(Inst);
  auto Loc = dynamicCast.getLocation();

  // Check if we can statically predict the outcome of the cast.
  auto Feasibility =
      dynamicCast.classifyFeasibility(false /*allowWholeModule*/);

  if (Feasibility == DynamicCastFeasibility::WillFail) {
    // Remove the cast and insert a trap, followed by an
    // unreachable instruction.
    SILBuilderWithScope Builder(Inst, builderContext);
    auto *Trap = Builder.createUnconditionalFail(Loc, "failed cast");
    Inst->replaceAllUsesWithUndef();
    eraseInstAction(Inst);
    Builder.setInsertionPoint(std::next(SILBasicBlock::iterator(Trap)));
    auto *UnreachableInst =
        Builder.createUnreachable(ArtificialUnreachableLocation());

    // Delete everything after the unreachable except for dealloc_stack which we
    // move before the trap.
    deleteInstructionsAfterUnreachable(UnreachableInst, Trap);

    willFailAction();
    return nullptr;
  }

  if (Feasibility == DynamicCastFeasibility::WillSucceed) {

    if (Inst->use_empty()) {
      eraseInstAction(Inst);
      willSucceedAction();
      return nullptr;
    }
  }

  SILBuilderWithScope Builder(Inst, builderContext);

  // Try to apply the bridged casts optimizations
  auto NewI = optimizeBridgedCasts(dynamicCast);
  if (NewI) {
    // FIXME: I'm not sure why this is true!
    auto newValue = cast<SingleValueInstruction>(NewI);
    replaceInstUsesAction(Inst, newValue);
    eraseInstAction(Inst);
    willSucceedAction();
    return newValue;
  }

  // If the cast may succeed or fail and can't be optimized into a bridging
  // call, let it be.
  if (Feasibility == DynamicCastFeasibility::MaySucceed) {
    return nullptr;
  }

  assert(Feasibility == DynamicCastFeasibility::WillSucceed);

  if (dynamicCast.isBridgingCast())
    return nullptr;

  auto Result =
      emitSuccessfulScalarUnconditionalCast(Builder, Loc, dynamicCast);

  if (!Result) {
    // No optimization was possible.
    return nullptr;
  }

  replaceInstUsesAction(Inst, Result);
  eraseInstAction(Inst);
  willSucceedAction();
  return Result;
}

/// Deletes all instructions after \p UnreachableInst except dealloc_stack
/// instructions are moved before \p TrapInst.
void CastOptimizer::deleteInstructionsAfterUnreachable(
    SILInstruction *UnreachableInst, SILInstruction *TrapInst) {
  auto UnreachableInstIt = std::next(SILBasicBlock::iterator(UnreachableInst));
  auto *Block = TrapInst->getParent();
  while (UnreachableInstIt != Block->end()) {
    SILInstruction *CurInst = &*UnreachableInstIt;
    ++UnreachableInstIt;
    CurInst->replaceAllUsesOfAllResultsWithUndef();
    eraseInstAction(CurInst);
  }
}

/// TODO: Move to emitSuccessfulIndirectUnconditionalCast?
///
/// Peephole to avoid runtime calls:
/// unconditional_checked_cast_addr T in %0 : $*T to P in %1 : $*P
/// ->
/// %addr = init_existential_addr %1 : $*P, T
/// copy_addr %0 to %addr
///
/// where T is a type statically known to conform to P.
///
/// In caase P is a class existential type, it generates:
/// %val = load %0 : $*T
/// %existential = init_existential_ref %val : $T, $T, P
/// store %existential to %1 : $*P
///
/// Returns true if the optimization was possible and false otherwise.
static bool optimizeStaticallyKnownProtocolConformance(
    UnconditionalCheckedCastAddrInst *Inst) {
  auto Loc = Inst->getLoc();
  auto Src = Inst->getSrc();
  auto Dest = Inst->getDest();
  auto SourceType = Inst->getSourceFormalType();
  auto TargetType = Inst->getTargetFormalType();
  auto &Mod = Inst->getModule();

  if (TargetType->isAnyExistentialType() &&
      !SourceType->isAnyExistentialType()) {
    auto &Ctx = Mod.getASTContext();
    auto *SM = Mod.getSwiftModule();

    auto *Proto = dyn_cast_or_null<ProtocolDecl>(TargetType->getAnyNominal());
    if (!Proto)
      return false;

    // SourceType is a non-existential type with a non-conditional
    // conformance to a protocol represented by the TargetType.
    //
    // TypeChecker::conformsToProtocol checks any conditional conformances. If
    // they depend on information not known until runtime, the conformance
    // will not be returned. For instance, if `X: P` where `T == Int` in `func
    // foo<T>(_: T) { ... X<T>() as? P ... }`, the cast will succeed for
    // `foo(0)` but not for `foo("string")`. There are many cases where
    // everything is completely static (`X<Int>() as? P`), in which case a
    // valid conformance will be returned.
    auto Conformance = SM->conformsToProtocol(SourceType, Proto);
    if (Conformance.isInvalid())
      return false;

    SILBuilderWithScope B(Inst);
    SmallVector<ProtocolConformanceRef, 1> NewConformances;
    NewConformances.push_back(Conformance);
    ArrayRef<ProtocolConformanceRef> Conformances =
        Ctx.AllocateCopy(NewConformances);

    auto ExistentialRepr =
        Dest->getType().getPreferredExistentialRepresentation(SourceType);

    switch (ExistentialRepr) {
    default:
      return false;
    case ExistentialRepresentation::Opaque: {
      auto ExistentialAddr = B.createInitExistentialAddr(
          Loc, Dest, SourceType, Src->getType().getObjectType(), Conformances);
      B.createCopyAddr(Loc, Src, ExistentialAddr, IsTake_t::IsTake,
                       IsInitialization_t::IsInitialization);
      break;
    }
    case ExistentialRepresentation::Class: {
      auto Value =
          B.emitLoadValueOperation(Loc, Src, LoadOwnershipQualifier::Take);
      auto Existential =
          B.createInitExistentialRef(Loc, Dest->getType().getObjectType(),
                                     SourceType, Value, Conformances);
      B.emitStoreValueOperation(Loc, Existential, Dest,
                                StoreOwnershipQualifier::Init);
      break;
    }
    case ExistentialRepresentation::Boxed: {
      auto AllocBox = B.createAllocExistentialBox(Loc, Dest->getType(),
                                                  SourceType, Conformances);
      auto Projection =
          B.createProjectExistentialBox(Loc, Src->getType(), AllocBox);
      // This needs to be a copy_addr (for now) because we must handle
      // address-only types.
      B.createCopyAddr(Loc, Src, Projection, IsTake, IsInitialization);
      B.emitStoreValueOperation(Loc, AllocBox, Dest,
                                StoreOwnershipQualifier::Init);
      break;
    }
    };
    return true;
  }
  // Not a concrete -> existential cast.
  return false;
}

SILInstruction *CastOptimizer::optimizeUnconditionalCheckedCastAddrInst(
    UnconditionalCheckedCastAddrInst *Inst) {
  SILDynamicCastInst dynamicCast(Inst);
  auto Loc = dynamicCast.getLocation();

  // Check if we can statically predict the outcome of the cast.
  auto Feasibility =
      dynamicCast.classifyFeasibility(false /*allow whole module*/);

  if (Feasibility == DynamicCastFeasibility::MaySucceed) {
    // Forced bridged casts can be still simplified here.
    // If they fail, they fail inside the conversion function.
    if (!dynamicCast.isBridgingCast())
      return nullptr;
  }

  if (Feasibility == DynamicCastFeasibility::WillFail) {
    // Remove the cast and insert a trap, followed by an
    // unreachable instruction.
    SILBuilderWithScope Builder(Inst, builderContext);
    auto *TrapI = Builder.createUnconditionalFail(Loc, "failed cast");
    eraseInstAction(Inst);
    Builder.setInsertionPoint(std::next(TrapI->getIterator()));
    auto *UnreachableInst =
        Builder.createUnreachable(ArtificialUnreachableLocation());

    // Delete everything after the unreachable except for dealloc_stack which we
    // move before the trap.
    deleteInstructionsAfterUnreachable(UnreachableInst, TrapI);

    willFailAction();
  }

  if (Feasibility == DynamicCastFeasibility::WillSucceed ||
      Feasibility == DynamicCastFeasibility::MaySucceed) {

    // Check if a result of a cast is unused. If this is the case, the cast can
    // be removed even if the cast may fail at runtime.
    // Swift optimizer does not claim to be crash-preserving.
    SILValue dest = dynamicCast.getDest();
    bool ResultNotUsed = isa<AllocStackInst>(dest);
    DestroyAddrInst *DestroyDestInst = nullptr;
    if (ResultNotUsed) {
      for (auto Use : dest->getUses()) {
        auto *User = Use->getUser();
        if (isa<DeallocStackInst>(User) || User == Inst)
          continue;
        if (isa<DestroyAddrInst>(User) && !DestroyDestInst) {
          DestroyDestInst = cast<DestroyAddrInst>(User);
          continue;
        }
        ResultNotUsed = false;
        DestroyDestInst = nullptr;
        break;
      }
    }

    if (ResultNotUsed) {
      SILBuilderWithScope B(Inst, builderContext);
      B.createDestroyAddr(Loc, dynamicCast.getSource());
      if (DestroyDestInst)
        eraseInstAction(DestroyDestInst);
      eraseInstAction(Inst);
      willSucceedAction();
      return nullptr;
    }

    // Try to apply the bridged casts optimizations.
    auto NewI = optimizeBridgedCasts(dynamicCast);
    if (NewI) {
      willSucceedAction();
      return nullptr;
    }

    if (Feasibility == DynamicCastFeasibility::MaySucceed)
      return nullptr;

    assert(Feasibility == DynamicCastFeasibility::WillSucceed);

    if (optimizeStaticallyKnownProtocolConformance(Inst)) {
      eraseInstAction(Inst);
      willSucceedAction();
      return nullptr;
    }

    if (dynamicCast.isBridgingCast())
      return nullptr;

    SILBuilderWithScope Builder(Inst, builderContext);
    if (!emitSuccessfulIndirectUnconditionalCast(Builder, Loc, dynamicCast)) {
      // No optimization was possible.
      return nullptr;
    }

    eraseInstAction(Inst);
    willSucceedAction();
  }

  return nullptr;
}

/// Simplify conversions between thick and objc metatypes.
SILValue CastOptimizer::optimizeMetatypeConversion(
    ConversionOperation mci, MetatypeRepresentation representation) {
  SILValue op = mci->getOperand(0);
  // Instruction has a proper target type already.
  SILType ty = mci->getType();
  auto metatypeTy = op->getType().getAs<AnyMetatypeType>();

  if (metatypeTy->getRepresentation() != representation)
    return SILValue();

  auto loc = mci->getLoc();

  // Rematerialize the incoming metatype instruction with the outgoing type.
  auto replaceCast = [&](SILValue newValue) -> SILValue {
    assert(ty.getAs<AnyMetatypeType>()->getRepresentation() ==
           newValue->getType().getAs<AnyMetatypeType>()->getRepresentation());
    replaceValueUsesAction(*mci, newValue);
    eraseInstAction(*mci);
    return newValue;
  };

  if (auto *mi = dyn_cast<MetatypeInst>(op)) {
    return replaceCast(
        SILBuilderWithScope(*mci, builderContext).createMetatype(loc, ty));
  }

  // For metatype instructions that require an operand, generate the new
  // metatype at the same position as the original to avoid extending the
  // lifetime of `op` past its destroy.
  if (auto *vmi = dyn_cast<ValueMetatypeInst>(op)) {
    return replaceCast(SILBuilderWithScope(vmi, builderContext)
                           .createValueMetatype(loc, ty, vmi->getOperand()));
  }

  if (auto *emi = dyn_cast<ExistentialMetatypeInst>(op)) {
    return replaceCast(
        SILBuilderWithScope(emi, builderContext)
            .createExistentialMetatype(loc, ty, emi->getOperand()));
  }

  return SILValue();
}
