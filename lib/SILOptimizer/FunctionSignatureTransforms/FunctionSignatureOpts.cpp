//===--- FunctionSignatureOpts.cpp - Optimizes function signatures --------===//
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
/// This pass defines function signature related optimizations.
/// When a function signature optimization is performed, changes are made to
/// the original function and after all function signature optimizations are
/// finished, a new function is created and the old function is turned into
/// a thunk.
///
/// Another possibility is to implement these optimizations as separate passes,
/// but then we would send slightly different functions to the pass pipeline
/// multiple times through notifyPassManagerOfFunction. 
///
/// TODO: Optimize function with generic parameters.
///
/// TODO: Improve epilogue release matcher, i.e. do a data flow instead of
/// only finding releases in the return block. 
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-function-signature-opt"
#include "FunctionSignatureOpts.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/CallerAnalysis.h"
#include "swift/SILOptimizer/Analysis/EpilogueARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumFunctionSignaturesOptimized, "Total func sig optimized");
STATISTIC(NumDeadArgsEliminated, "Total dead args eliminated");
STATISTIC(NumOwnedConvertedToGuaranteed, "Total owned args -> guaranteed args");
STATISTIC(NumOwnedConvertedToNotOwnedResult, "Total owned result -> not owned result");
STATISTIC(NumSROAArguments, "Total SROA arguments optimized");

using SILParameterInfoList = llvm::SmallVector<SILParameterInfo, 8>;
using ArgumentIndexMap = llvm::SmallDenseMap<int, int>;

//===----------------------------------------------------------------------===//
//                           Optimization Heuristic
//===----------------------------------------------------------------------===//

/// Set to true to enable the support for partial specialization.
static llvm::cl::opt<bool>
    FSOEnableGenerics("sil-fso-enable-generics", llvm::cl::init(true),
                      llvm::cl::desc("Support function signature optimization "
                                     "of generic functions"));

static llvm::cl::opt<bool>
    FSOOptimizeIfNotCalled("sil-fso-optimize-if-not-called",
                           llvm::cl::init(false),
                           llvm::cl::desc("Optimize even if a function isn't "
                                          "called. For testing only!"));

static bool isSpecializableRepresentation(SILFunctionTypeRepresentation Rep,
                                          bool OptForPartialApply) {
  switch (Rep) {
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::Closure:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::CXXMethod:
    return true;
  case SILFunctionTypeRepresentation::WitnessMethod:
    return OptForPartialApply;
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::Block:
    return false;
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

/// Returns true if F is a function which the pass knows how to specialize
/// function signatures for.
static bool canSpecializeFunction(SILFunction *F,
                                  const CallerAnalysis::FunctionInfo *FuncInfo,
                                  bool OptForPartialApply) {
  // Do not specialize the signature of SILFunctions that are external
  // declarations since there is no body to optimize.
  if (F->isExternalDeclaration())
    return false;

  // For now ignore functions with indirect results.
  if (F->getConventions().hasIndirectSILResults())
    return false;

  // For now ignore coroutines.
  if (F->getLoweredFunctionType()->isCoroutine())
    return false;

  // Do not specialize the signature of always inline functions. We
  // will just inline them and specialize each one of the individual
  // functions that these sorts of functions are inlined into.
  // It is OK to specialize always inline functions if they are
  // used by partial_apply instructions.
  assert(!OptForPartialApply || FuncInfo);
  if (F->getInlineStrategy() == Inline_t::AlwaysInline &&
      (!OptForPartialApply || !FuncInfo->getMinPartialAppliedArgs()))
    return false;

  // For now ignore generic functions to keep things simple...
  if (!FSOEnableGenerics && F->getLoweredFunctionType()->isPolymorphic())
    return false;

  // Make sure F has a linkage that we can optimize.
  if (!isSpecializableRepresentation(F->getRepresentation(),
                                     OptForPartialApply))
    return false;

  return true;
}

//===----------------------------------------------------------------------===//
//                  Function Signature Transform Descriptor
//===----------------------------------------------------------------------===//

void FunctionSignatureTransformDescriptor::addThunkArgument(
    ArgumentDescriptor &AD, SILBuilder &Builder, SILBasicBlock *BB,
    llvm::SmallVectorImpl<SILValue> &NewArgs) {
  // Dead argument.
  if (AD.IsEntirelyDead) {
    return;
  }

  // Explode the argument.
  if (AD.Explode) {
    llvm::SmallVector<SILValue, 4> LeafValues;
    AD.ProjTree.createTreeFromValue(Builder, BB->getParent()->getLocation(),
                                    BB->getArgument(AD.Index), LeafValues);
    NewArgs.append(LeafValues.begin(), LeafValues.end());
    return;
  }

  // All other arguments get pushed as what they are.
  NewArgs.push_back(BB->getArgument(AD.Index));
}

std::string
FunctionSignatureTransformDescriptor::createOptimizedSILFunctionName() {
  SILFunction *F = OriginalFunction;

  auto P = Demangle::SpecializationPass::FunctionSignatureOpts;
  Mangle::FunctionSignatureSpecializationMangler Mangler(P, F->isSerialized(),
                                                         F);

  // Handle arguments' changes.
  for (unsigned i : indices(ArgumentDescList)) {
    const ArgumentDescriptor &Arg = ArgumentDescList[i];
    if (Arg.IsEntirelyDead) {
      Mangler.setArgumentDead(i);
      // No point setting other attribute if argument is dead.
      continue;
    }

    // If we have an @owned argument and found a callee release for it,
    // convert the argument to guaranteed.
    if (Arg.OwnedToGuaranteed) {
      Mangler.setArgumentOwnedToGuaranteed(i);
    }

    // If this argument is not dead and we can explode it, add 's' to the
    // mangling.
    if (Arg.Explode) {
      Mangler.setArgumentSROA(i);
    }
  }

  // Handle return value's change.
  // FIXME: handle multiple direct results here
  if (ResultDescList.size() == 1 && !ResultDescList[0].CalleeRetain.empty()) {
    Mangler.setReturnValueOwnedToUnowned();
  }

  return Mangler.mangle();
}

/// Collect all archetypes used by a function.
static bool usesGenerics(SILFunction *F,
                         ArrayRef<SILParameterInfo> InterfaceParams,
                         ArrayRef<SILResultInfo> InterfaceResults) {
  CanSILFunctionType FTy = F->getLoweredFunctionType();
  auto HasGenericSignature = FTy->getInvocationGenericSignature() != nullptr;
  if (!HasGenericSignature)
    return false;

  bool UsesGenerics = false;

  auto FindArchetypesAndGenericTypes = [FTy, &UsesGenerics](Type Ty) {
    if (Ty.findIf([FTy](Type Ty) -> bool {
          // Assume archetypes are always a problem.
          // TODO: This can ignore non-contextual archetypes.
          if (Ty->hasArchetype()) return true;

          // Assume type parameters are always a problem.  However, this
          // can ignore types that would substitute to concrete types.
          if (Ty->isTypeParameter()) {
            auto subs = FTy->getPatternSubstitutions();
            return (!subs || Ty.subst(subs)->isTypeParameter());
          }

          return false;
        }))
      UsesGenerics = true;
  };

  for (auto Param : InterfaceParams) {
    Param.getInterfaceType().visit(FindArchetypesAndGenericTypes);
  }

  for (auto Result : InterfaceResults) {
    Result.getInterfaceType().visit(FindArchetypesAndGenericTypes);
  }

  if (UsesGenerics)
    return UsesGenerics;

  for (auto &BB : *F) {
    for (auto &I : BB) {
      for (auto Arg : BB.getArguments()) {
        if (&BB != &*F->begin()) {
          // Scan types of all BB arguments. Ignore the entry BB, because
          // it is handled in a special way.
           Arg->getType().getASTType().visit(FindArchetypesAndGenericTypes);
           if (UsesGenerics)
             return UsesGenerics;
        }
      }
      // Scan types of all operands.
      for (auto &Op : I.getAllOperands()) {
        Op.get()->getType().getASTType().visit(FindArchetypesAndGenericTypes);
      }
      // Scan all substitutions of apply instructions.
      if (auto AI = ApplySite::isa(&I)) {
        auto Subs = AI.getSubstitutionMap();
        for (auto Replacement : Subs.getReplacementTypes()) {
          Replacement.visit(FindArchetypesAndGenericTypes);
        }
      }
      // Scan all substitutions of builtin instructions.
      if (auto *BI = dyn_cast<BuiltinInst>(&I)) {
        auto Subs = BI->getSubstitutions();
        for (auto Ty : Subs.getReplacementTypes()) {
          Ty.visit(FindArchetypesAndGenericTypes);
        }
      }

      // Scan the result type of the instruction.
      for (auto V : I.getResults()) {
        V->getType().getASTType().visit(FindArchetypesAndGenericTypes);
      }

      if (UsesGenerics)
        return UsesGenerics;
    }
  }
  return UsesGenerics;
}

// Map the parameter, result and error types out of context to get the interface
// type.
static void
mapInterfaceTypes(SILFunction *F,
                  MutableArrayRef<SILParameterInfo> InterfaceParams,
                  MutableArrayRef<SILResultInfo> InterfaceResults,
                  llvm::Optional<SILResultInfo> &InterfaceErrorResult) {

  for (auto &Param : InterfaceParams) {
    if (!Param.getInterfaceType()->hasArchetype())
      continue;
    Param = SILParameterInfo(
        Param.getInterfaceType()->mapTypeOutOfContext()->getCanonicalType(),
        Param.getConvention());
  }

  for (auto &Result : InterfaceResults) {
    if (!Result.getInterfaceType()->hasArchetype())
      continue;
    auto InterfaceResult = Result.getWithInterfaceType(
        Result.getInterfaceType()->mapTypeOutOfContext()->getCanonicalType());
    Result = InterfaceResult;
  }

  if (InterfaceErrorResult.has_value()) {
    if (InterfaceErrorResult.value().getInterfaceType()->hasArchetype()) {
      InterfaceErrorResult =
          SILResultInfo(InterfaceErrorResult.value()
                            .getInterfaceType()
                            ->mapTypeOutOfContext()
                            ->getCanonicalType(),
                        InterfaceErrorResult.value().getConvention());
    }
  }
}

CanSILFunctionType
FunctionSignatureTransformDescriptor::createOptimizedSILFunctionType() {
  SILFunction *F = OriginalFunction;
  CanSILFunctionType FTy = F->getLoweredFunctionType();
  auto ExpectedFTy = F->getLoweredType().castTo<SILFunctionType>();
  auto HasGenericSignature = FTy->getSubstGenericSignature() != nullptr;

  // The only way that we modify the arity of function parameters is here for
  // dead arguments. Doing anything else is unsafe since by definition non-dead
  // arguments will have SSA uses in the function. We would need to be smarter
  // in our moving to handle such cases.
  llvm::SmallVector<SILParameterInfo, 8> InterfaceParams;
  for (auto &ArgDesc : ArgumentDescList) {
    computeOptimizedArgInterface(ArgDesc, InterfaceParams);
  }

  // ResultDescs only covers the direct results; we currently can't ever
  // change an indirect result.  Piece the modified direct result information
  // back into the all-results list.
  llvm::SmallVector<SILResultInfo, 8> InterfaceResults;
  for (SILResultInfo InterfaceResult : FTy->getResults()) {
    if (InterfaceResult.isFormalDirect()) {
      auto &RV = ResultDescList[0];
      if (!RV.CalleeRetain.empty()) {
        ++NumOwnedConvertedToNotOwnedResult;
        InterfaceResults.push_back(SILResultInfo(InterfaceResult.getInterfaceType(),
                                                 ResultConvention::Unowned));
        continue;
      }
    }

    InterfaceResults.push_back(InterfaceResult);
  }

  llvm::SmallVector<SILYieldInfo, 8> InterfaceYields;
  for (SILYieldInfo InterfaceYield : FTy->getYields()) {
    // For now, don't touch the yield types.
    InterfaceYields.push_back(InterfaceYield);
  }

  bool UsesGenerics = false;
  if (HasGenericSignature) {
    // Not all of the generic type parameters are used by the function
    // parameters.
    // Check which of the generic type parameters are not used and check if they
    // are used anywhere in the function body. If this is not the case, we can
    // remove the unused generic type parameters from the generic signature.
    // This makes the code both smaller and faster, because no implicit
    // parameters for type metadata and conformances need to be passed to the
    // callee at the LLVM IR level.
    // TODO: Implement a more precise analysis, so that we can eliminate only
    // those generic parameters which are not used.
    UsesGenerics = usesGenerics(F, InterfaceParams, InterfaceResults);

    // The set of used archetypes is complete now.
    if (!UsesGenerics) {
      // None of the generic type parameters are used.
      LLVM_DEBUG(llvm::dbgs() << "None of generic parameters are used by "
                              << F->getName() << "\n";
                 llvm::dbgs() << "Interface params:\n";
                 for (auto Param : InterfaceParams) {
                   Param.getInterfaceType().dump(llvm::dbgs());
                 }

                 llvm::dbgs() << "Interface results:\n";
                 for (auto Result : InterfaceResults) {
                   Result.getInterfaceType().dump(llvm::dbgs());
                 });
    }
  }

  // Don't use a method representation if we modified self.
  auto ExtInfo = FTy->getExtInfo();
  auto witnessMethodConformance = FTy->getWitnessMethodConformanceOrInvalid();
  if (shouldModifySelfArgument) {
    ExtInfo = ExtInfo.withRepresentation(SILFunctionTypeRepresentation::Thin);
    witnessMethodConformance = ProtocolConformanceRef::forInvalid();
  }

  llvm::Optional<SILResultInfo> InterfaceErrorResult;
  if (ExpectedFTy->hasErrorResult()) {
    InterfaceErrorResult = ExpectedFTy->getErrorResult();
  }

  // Map the parameter, result and error types out of context to get the
  // proper interface type. This is required for generic functions.
  mapInterfaceTypes(F, InterfaceParams, InterfaceResults, InterfaceErrorResult);

  GenericSignature GenericSig =
      UsesGenerics ? FTy->getInvocationGenericSignature() : nullptr;

  return SILFunctionType::get(
      GenericSig, ExtInfo, FTy->getCoroutineKind(), FTy->getCalleeConvention(),
      InterfaceParams, InterfaceYields, InterfaceResults, InterfaceErrorResult,
      FTy->getPatternSubstitutions(), SubstitutionMap(),
      F->getModule().getASTContext(), witnessMethodConformance);
}

/// Compute what the function interface will look like based on the
/// optimization we are doing on the given argument descriptor. Default
/// implementation simply passes it through.
void FunctionSignatureTransformDescriptor::computeOptimizedArgInterface(
    ArgumentDescriptor &AD, SmallVectorImpl<SILParameterInfo> &Out) {
  // If this argument is live, but we cannot optimize it.
  if (!AD.canOptimizeLiveArg()) {
    if (AD.PInfo.has_value())
      Out.push_back(AD.PInfo.value());
    return;
  }

  // If we have a dead argument, bail.
  if (AD.IsEntirelyDead) {
    ++NumDeadArgsEliminated;
    return;
  }

  // Explode the argument or not ?
  if (AD.Explode) {
    ++NumSROAArguments;
    llvm::SmallVector<const ProjectionTreeNode *, 8> LeafNodes;
    AD.ProjTree.getLiveLeafNodes(LeafNodes);
    for (auto Node : LeafNodes) {
      SILType Ty = Node->getType();
      LLVM_DEBUG(llvm::dbgs() << "                " << Ty << "\n");
      // If Ty is trivial, just pass it directly.
      if (Ty.isTrivial(*AD.Arg->getFunction())) {
        SILParameterInfo NewInfo(Ty.getASTType(),
                                 ParameterConvention::Direct_Unowned);
        Out.push_back(NewInfo);
        continue;
      }

      // Ty is not trivial, pass it through as the original calling convention.
      auto ParameterConvention = AD.PInfo.value().getConvention();
      if (AD.OwnedToGuaranteed) {
        if (ParameterConvention == ParameterConvention::Direct_Owned)
          ParameterConvention = ParameterConvention::Direct_Guaranteed;
        else if (ParameterConvention == ParameterConvention::Indirect_In)
          ParameterConvention = ParameterConvention::Indirect_In_Guaranteed;
        else {
          llvm_unreachable("Unknown parameter convention transformation");
        }
      }
      SILParameterInfo NewInfo(Ty.getASTType(), ParameterConvention);
      Out.push_back(NewInfo);
    }
    return;
  }

  // If we cannot explode this value, handle callee release and return.
  // If we found releases in the callee in the last BB on an @owned
  // parameter, change the parameter to @guaranteed and continue...
  if (AD.OwnedToGuaranteed) {
    ++NumOwnedConvertedToGuaranteed;
    auto ParameterConvention = AD.PInfo.value().getConvention();
    if (ParameterConvention == ParameterConvention::Direct_Owned)
      ParameterConvention = ParameterConvention::Direct_Guaranteed;
    else if (ParameterConvention == ParameterConvention::Indirect_In)
      ParameterConvention = ParameterConvention::Indirect_In_Guaranteed;
    else {
      llvm_unreachable("Unknown parameter convention transformation");
    }

    SILParameterInfo NewInfo(AD.PInfo.value().getInterfaceType(),
                             ParameterConvention);
    Out.push_back(NewInfo);
    return;
  }

  // Otherwise just propagate through the parameter info.
  Out.push_back(AD.PInfo.value());
}

//===----------------------------------------------------------------------===//
//                        Function Signature Transform
//===----------------------------------------------------------------------===//

void FunctionSignatureTransform::createFunctionSignatureOptimizedFunction() {
  // Create the optimized function!
  SILFunction *F = TransformDescriptor.OriginalFunction;
  SILModule &M = F->getModule();
  std::string Name = TransformDescriptor.createOptimizedSILFunctionName();
  // The transformed function must not already exist. This would indicate
  // repeated application of FSO on the same function. That situation should be
  // detected earlier by avoiding reoptimization of FSO thunks.
  assert(!F->getModule().hasFunction(Name));

  SILLinkage linkage = getSpecializedLinkage(F, F->getLinkage());

  LLVM_DEBUG(llvm::dbgs() << "  -> create specialized function " << Name
                          << "\n");

  auto NewFTy = TransformDescriptor.createOptimizedSILFunctionType();
  GenericEnvironment *NewFGenericEnv;
  if (NewFTy->getInvocationGenericSignature()) {
    NewFGenericEnv = F->getGenericEnvironment();
  } else {
    NewFGenericEnv = nullptr;
  }

  // The specialized function is an internal detail, so we need to disconnect it
  // from a parent class, if one exists, thus the override of the
  // classSubclassScope.
  TransformDescriptor.OptimizedFunction = FunctionBuilder.createFunction(
      linkage, Name, NewFTy, NewFGenericEnv, F->getLocation(), F->isBare(),
      F->isTransparent(), F->isSerialized(), IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible, F->getEntryCount(), F->isThunk(),
      /*classSubclassScope=*/SubclassScope::NotApplicable,
      F->getInlineStrategy(), F->getEffectsKind(), nullptr, F->getDebugScope());
  SILFunction *NewF = TransformDescriptor.OptimizedFunction.get();
  if (!F->hasOwnership()) {
    NewF->setOwnershipEliminated();
  }

  if (F->isSpecialization()) {
    NewF->setSpecializationInfo(F->getSpecializationInfo());
  }

  // Then we transfer the body of F to NewF.
  NewF->moveAllBlocksFromOtherFunction(F);

  // Array semantic clients rely on the signature being as in the original
  // version.
  for (auto &Attr : F->getSemanticsAttrs()) {
    if (!StringRef(Attr).startswith("array."))
      NewF->addSemanticsAttr(Attr);
  }

  // Do the last bit of work to the newly created optimized function.
  DeadArgumentFinalizeOptimizedFunction();
  ArgumentExplosionFinalizeOptimizedFunction();

  // Update the ownership kinds of function entry BB arguments.
  for (auto Arg : NewF->begin()->getSILFunctionArguments()) {
    SILType MappedTy = Arg->getType();
    auto Ownershipkind =
        ValueOwnershipKind(*NewF, MappedTy, Arg->getArgumentConvention());
    Arg->setOwnershipKind(Ownershipkind);
  }

  // Create the thunk body !
  F->setThunk(IsSignatureOptimizedThunk);
  // The thunk now carries the information on how the signature is
  // optimized. If we inline the thunk, we will get the benefit of calling
  // the signature optimized function without additional setup on the
  // caller side.
  F->setInlineStrategy(AlwaysInline);
  SILBasicBlock *ThunkBody = F->createBasicBlock();
  for (auto &ArgDesc : TransformDescriptor.ArgumentDescList) {
    auto *NewArg =
        ThunkBody->createFunctionArgument(ArgDesc.Arg->getType(), ArgDesc.Decl);
    NewArg->copyFlags(ArgDesc.Arg);
  }

  SILLocation Loc = RegularLocation::getAutoGeneratedLocation();
  SILBuilder Builder(ThunkBody);
  Builder.setCurrentDebugScope(ThunkBody->getParent()->getDebugScope());

  FunctionRefInst *FRI = Builder.createFunctionRef(Loc, NewF);

  // Create the args for the thunk's apply, ignoring any dead arguments.
  llvm::SmallVector<SILValue, 8> ThunkArgs;
  for (auto &ArgDesc : TransformDescriptor.ArgumentDescList) {
    TransformDescriptor.addThunkArgument(ArgDesc, Builder, ThunkBody,
                                         ThunkArgs);
  }

  SILValue ReturnValue;
  SILType LoweredType = NewF->getLoweredType();
  SILType ResultType = NewF->getConventions().getSILResultType(
      Builder.getTypeExpansionContext());
  auto GenCalleeType = NewF->getLoweredFunctionType();
  auto SubstCalleeSILType = LoweredType;
  SubstitutionMap Subs;
  // Handle generic functions.
  if (GenCalleeType->isPolymorphic()) {
    // Produce a substitutions list and a set of substituted SIL types
    // required for creating a new SIL function.
    Subs = F->getForwardingSubstitutionMap();
    auto SubstCalleeType = GenCalleeType->substGenericArgs(
        M, Subs, Builder.getTypeExpansionContext());
    SubstCalleeSILType = SILType::getPrimitiveObjectType(SubstCalleeType);
    SILFunctionConventions Conv(SubstCalleeType, M);
    ResultType = Conv.getSILResultType(Builder.getTypeExpansionContext());
  }
  auto FunctionTy = LoweredType.castTo<SILFunctionType>();
  if (FunctionTy->hasErrorResult()) {
    // We need a try_apply to call a function with an error result.
    SILFunction *Thunk = ThunkBody->getParent();
    SILBasicBlock *NormalBlock = Thunk->createBasicBlock();
    ReturnValue =
        NormalBlock->createPhiArgument(ResultType, OwnershipKind::Owned);
    SILBasicBlock *ErrorBlock = Thunk->createBasicBlock();
    SILType Error =
        SILType::getPrimitiveObjectType(FunctionTy->getErrorResult().getInterfaceType());
    auto *ErrorArg = ErrorBlock->createPhiArgument(Error, OwnershipKind::Owned);
    Builder.createTryApply(Loc, FRI, Subs, ThunkArgs, NormalBlock, ErrorBlock);

    Builder.setInsertionPoint(ErrorBlock);
    Builder.createThrow(Loc, ErrorArg);
    Builder.setInsertionPoint(NormalBlock);
  } else {
    ReturnValue = Builder.createApply(Loc, FRI, Subs, ThunkArgs);
  }

  // Set up the return results.
  if (NewF->isNoReturnFunction(Builder.getTypeExpansionContext())) {
    Builder.createUnreachable(Loc);
  } else {
    Builder.createReturn(Loc, ReturnValue);
  }

  // Do the last bit work to finalize the thunk.
  OwnedToGuaranteedFinalizeThunkFunction(Builder, F);

  assert(F->getDebugScope()->Parent != NewF->getDebugScope()->Parent);
}

// Run the optimization.
bool FunctionSignatureTransform::run(bool hasCaller) {
  // We use a reference here on purpose so our transformations can know if we
  // are going to make a thunk and thus should just optimize.
  bool &Changed = TransformDescriptor.Changed;
  bool hasOnlyDirectInModuleCallers =
      TransformDescriptor.hasOnlyDirectInModuleCallers;
  SILFunction *F = TransformDescriptor.OriginalFunction;

  // If we are asked to assume a caller for testing purposes, set the flag.
  hasCaller |= FSOOptimizeIfNotCalled;

  if (!hasCaller && (F->getDynamicallyReplacedFunction() ||
                     F->getReferencedAdHocRequirementWitnessFunction() ||
                     canBeCalledIndirectly(F->getRepresentation()))) {
    LLVM_DEBUG(llvm::dbgs() << "  function has no caller -> abort\n");
    return false;
  }

  // Bail if we have a pseudo-generic function. We do not handle these today. If
  // we let it through here we crash when attempting to compute the optimized
  // function type.
  //
  // TODO: Add support for this.
  if (F->getLoweredFunctionType()->isPseudogeneric()) {
    LLVM_DEBUG(llvm::dbgs() << "  function is pseudo-generic -> abort\n");
    return false;
  }

  // Run OwnedToGuaranteed optimization.
  if (OwnedToGuaranteedAnalyze()) {
    Changed = true;
    LLVM_DEBUG(llvm::dbgs() << "  transform owned-to-guaranteed\n");
    OwnedToGuaranteedTransform();
  }

  // Run DeadArgument elimination transformation. We only specialize
  // if this function has a caller inside the current module or we have
  // already created a thunk.
  if ((hasCaller || Changed || hasOnlyDirectInModuleCallers) &&
      DeadArgumentAnalyzeParameters()) {
    Changed = true;
    LLVM_DEBUG(llvm::dbgs() << "  remove dead arguments\n");
    DeadArgumentTransformFunction();
  }

  // Run ArgumentExplosion transformation. We only specialize
  // if this function has a caller inside the current module or we have
  // already created a thunk.
  //
  // NOTE: we run argument explosion last because we've already initialized
  // the ArgumentDescList to have unexploded number of arguments. Exploding
  // it without changing the argument count is not going to help with
  // owned-to-guaranteed transformation.
  //
  // In order to not miss any opportunity, we send the optimized function
  // to the passmanager to optimize any opportunities exposed by argument
  // explosion.
  if ((hasCaller || Changed || hasOnlyDirectInModuleCallers) &&
      ArgumentExplosionAnalyzeParameters()) {
    Changed = true;
  }

  // Check if generic signature of the function could be changed by
  // removed some unused generic arguments.
  if (F->getLoweredFunctionType()->isPolymorphic() &&
      TransformDescriptor.createOptimizedSILFunctionType() !=
          F->getLoweredFunctionType()) {
    Changed = true;
  }

  // Create the specialized function and invalidate the old function.
  if (Changed) {
    createFunctionSignatureOptimizedFunction();
  }
  return Changed;
}

// Run dead argument elimination of partially applied functions.
//
// After this optimization CapturePropagation can replace the partial_apply by a
// direct reference to the specialized function.
bool FunctionSignatureTransform::removeDeadArgs(int minPartialAppliedArgs) {
  if (minPartialAppliedArgs < 1)
    return false;

  if (!DeadArgumentAnalyzeParameters())
    return false;

  SILFunction *F = TransformDescriptor.OriginalFunction;
  auto ArgumentDescList = TransformDescriptor.ArgumentDescList;

  // Check if at least the minimum number of partially applied arguments
  // are dead. Otherwise no partial_apply can be removed anyway.
  unsigned Size = ArgumentDescList.size();
  for (unsigned Idx : range(Size)) {
    if (Idx < Size - minPartialAppliedArgs) {
      // Don't remove arguments other than the partial applied ones, even if
      // they are dead.
      ArgumentDescList[Idx].IsEntirelyDead = false;
      continue;
    }

    // Is the partially applied argument dead?
    if (!ArgumentDescList[Idx].IsEntirelyDead)
      return false;

    // Currently we require that all dead parameters have trivial types.  The
    // reason is that it's very hard to find places where we can release those
    // parameters (as a replacement for the removed partial_apply).
    //
    // TODO: Maybe we can skip this restriction when we have semantic ARC.
    if (ArgumentDescList[Idx].Arg->getType().isTrivial(*F))
      continue;
    return false;
  }

  LLVM_DEBUG(llvm::dbgs() << "  remove dead arguments for partial_apply\n");
  DeadArgumentTransformFunction();
  createFunctionSignatureOptimizedFunction();
  return true;
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//

namespace {

class FunctionSignatureOpts : public SILFunctionTransform {
  
  /// If true, perform a special kind of dead argument elimination to enable
  /// removal of partial_apply instructions where all partially applied
  /// arguments are dead.
  bool OptForPartialApply;

public:

  FunctionSignatureOpts(bool OptForPartialApply) :
     OptForPartialApply(OptForPartialApply) { }

  void run() override {
    auto *F = getFunction();

    // Don't run function signature optimizations at -Os.
    if (F->optimizeForSize())
      return;

    // Don't optimize callees that should not be optimized.
    if (!F->shouldOptimize())
      return;

    if (F->isDynamicallyReplaceable())
      return;

    // This is the function to optimize.
    LLVM_DEBUG(llvm::dbgs() << "*** FSO on function: " << F->getName()
                            << " ***\n");

    // Check the signature of F to make sure that it is a function that we
    // can specialize. These are conditions independent of the call graph.
    // No need for CallerAnalysis if we are not optimizing for partial
    // applies.
    if (!OptForPartialApply &&
        !canSpecializeFunction(F, nullptr, OptForPartialApply)) {
      LLVM_DEBUG(llvm::dbgs() << "  cannot specialize function -> abort\n");
      return;
    }

    const CallerAnalysis *CA = PM->getAnalysis<CallerAnalysis>();
    const CallerAnalysis::FunctionInfo &FuncInfo = CA->getFunctionInfo(F);

    // Check the signature of F to make sure that it is a function that we
    // can specialize. These are conditions independent of the call graph.
    if (OptForPartialApply &&
        !canSpecializeFunction(F, &FuncInfo, OptForPartialApply)) {
      LLVM_DEBUG(llvm::dbgs() << "  cannot specialize function -> abort\n");
      return;
    }

    // Never repeat the same function signature optimization on the same
    // function. Multiple function signature optimizations are composed by
    // successively optimizing the newly created functions. Each optimization
    // creates a new level of thunk which are all ultimately inlined away.
    //
    // This happens, for example, when a reference to the original function is
    // discovered during devirtualization. That will cause the original function
    // (now an FSO thunk) to be pushed back on the function pass pipeline.
    if (F->isThunk() == IsSignatureOptimizedThunk) {
      LLVM_DEBUG(llvm::dbgs() << "  FSO already performed on this thunk\n");
      return;
    }

    // Ok, we think we can perform optimization. Now perform a quick check
    auto *RCIA = getAnalysis<RCIdentityAnalysis>();
    auto *EA = PM->getAnalysis<EpilogueARCAnalysis>();

    // As we optimize the function more and more, the name of the function is
    // going to change, make sure the mangler is aware of all the changes done
    // to the function.
    auto P = Demangle::SpecializationPass::FunctionSignatureOpts;
    Mangle::FunctionSignatureSpecializationMangler Mangler(P,
                                                           F->isSerialized(), F);

    /// Keep a map between the exploded argument index and the original argument
    /// index.
    llvm::SmallDenseMap<int, int> AIM;
    int asize = F->begin()->getArguments().size();
    for (unsigned i : range(asize)) {
      AIM[i] = i;
    }

    // Allocate the argument and result descriptors.
    llvm::SpecificBumpPtrAllocator<ProjectionTreeNode> Allocator;
    llvm::SmallVector<ArgumentDescriptor, 4> ArgumentDescList;
    llvm::SmallVector<ResultDescriptor, 4> ResultDescList;
    auto Args = F->begin()->getSILFunctionArguments();
    for (unsigned i : indices(Args)) {
      ArgumentDescList.emplace_back(Args[i], Allocator);
    }
    for (SILResultInfo IR : F->getLoweredFunctionType()->getResults()) {
      ResultDescList.emplace_back(IR);
    }

    SILOptFunctionBuilder FuncBuilder(*this);
    // Owned to guaranteed optimization.
    FunctionSignatureTransform FST(FuncBuilder, F, RCIA, EA, Mangler, AIM,
                                   ArgumentDescList, ResultDescList,
                                   FuncInfo.foundAllCallers());

    bool Changed = false;
    if (OptForPartialApply) {
      Changed = FST.removeDeadArgs(FuncInfo.getMinPartialAppliedArgs());
    } else {
      Changed = FST.run(FuncInfo.hasDirectCaller());
    }

    if (!Changed) {
      return;
    }

    ++NumFunctionSignaturesOptimized;
    // The old function must be a thunk now.
    assert(F->isThunk() && "Old function should have been turned into a thunk");

    invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);

    // Make sure the PM knows about this function. This will also help us
    // with self-recursion.
    addFunctionToPassManagerWorklist(FST.getOptimizedFunction(), F);

    if (!OptForPartialApply) {
      // We have to restart the pipeline for this thunk in order to run the
      // inliner (and other opts) again. This is important if the new
      // specialized function (which is called from this thunk) is
      // function-signature-optimized again and also becomes an
      // always-inline-thunk.
      restartPassPipeline();
    }
  }

};

} // end anonymous namespace

SILTransform *swift::createFunctionSignatureOpts() {
  return new FunctionSignatureOpts(/* OptForPartialApply */ false);
}

SILTransform *swift::createDeadArgSignatureOpt() {
  return new FunctionSignatureOpts(/* OptForPartialApply */ true);
}
