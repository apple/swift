//===--- FunctionSignatureOpts.cpp - Optimizes function signatures --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
/// TODO: there are more refactoring that can be done for this optimization.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-function-signature-opt"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/CallerAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/FunctionSignatureOptUtils.h"
#include "swift/SILOptimizer/Utils/Local.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Mangle.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumFunctionSignaturesOptimized, "Total func sig optimized");
STATISTIC(NumDeadArgsEliminated, "Total dead args eliminated");
STATISTIC(NumOwnedConvertedToGuaranteed, "Total owned args -> guaranteed args");
STATISTIC(NumOwnedConvertedToNotOwnedResult, "Total owned result -> not owned result");
STATISTIC(NumSROAArguments, "Total SROA arguments optimized");

using SILParameterInfoList = llvm::SmallVector<SILParameterInfo, 8>;
using SILResultInfoList =  llvm::SmallVector<SILResultInfo, 8>;
using FSSM = FunctionSignatureSpecializationMangler;

//===----------------------------------------------------------------------===//
//                           Utilties 
//===----------------------------------------------------------------------===//
/// Return the single apply found in this function.
static SILInstruction *findOnlyApply(SILFunction *F) {
  SILInstruction *OnlyApply = nullptr;
  for (auto &B : *F) {
    for (auto &X : B) {
      if (!isa<ApplyInst>(X))
        continue;
      assert(!OnlyApply && "There are more than 1 function calls");
      OnlyApply = &X;
    }
  }
  assert(OnlyApply && "There is no function calls");
  return OnlyApply;
}

/// Creates a decrement on \p Ptr at insertion point \p InsertPt that creates a
/// strong_release if \p Ptr has reference semantics itself or a release_value
/// if \p Ptr is a non-trivial value without reference-semantics.
static SILInstruction *
createDecrement(SILValue Ptr, SILInstruction *InsertPt) {
  // Setup the builder we will use to insert at our insertion point.
  SILBuilder B(InsertPt);
  auto Loc = RegularLocation(SourceLoc());

  // If Ptr has reference semantics itself, create a strong_release.
  if (Ptr->getType().isReferenceCounted(B.getModule()))
    return B.createStrongRelease(Loc, Ptr, Atomicity::Atomic);

  // Otherwise create a release value.
  return B.createReleaseValue(Loc, Ptr, Atomicity::Atomic);
}

static void collapseThunkChain(SILFunction *Caller, SILPassManager *PM) {
  // Keep walking down the chain of thunks and inline every one of them.
  while (Caller->isThunk()) {
    FullApplySite AI = FullApplySite(findOnlyApply(Caller)); 
    SILFunction *Callee = AI.getCalleeFunction();

    // Reached the end of the chain of functions.
    if (!Callee->isThunk())
      break;

    SmallVector<SILValue, 8> Args;
    for (const auto &Arg : AI.getArguments())
      Args.push_back(Arg);

    TypeSubstitutionMap ContextSubs;
    SILInliner Inliner(*Caller, *Callee,
                        SILInliner::InlineKind::PerformanceInline, ContextSubs,
                        AI.getSubstitutions());

    auto Success = Inliner.inlineFunction(AI, Args);
    assert(Success && "Failed to inline thunks");
    recursivelyDeleteTriviallyDeadInstructions(AI.getInstruction(), true);

    // Delete the temporary thunk.
    PM->invalidateAnalysis(Callee, SILAnalysis::InvalidationKind::Everything);
  }
}

//===----------------------------------------------------------------------===//
//                     Function Signature Transformation 
//===----------------------------------------------------------------------===//

/// FunctionSignatureTransform - This is the base class for all function
/// signature transformations. All other transformations inherit from this.
class FunctionSignatureTransform {
protected:
  /// The actual function to analyze and transform.
  SILFunction *F;

  /// Optimized function.
  SILFunction *FO;

  /// The allocator we are using.
  llvm::BumpPtrAllocator &Allocator;

  /// The alias analysis we are using.
  AliasAnalysis *AA;

  /// The RC identity analysis we are using.
  RCIdentityAnalysis *RCIA;

  /// Keep a "view" of precompiled information on argumentis that we will use
  /// during our optimization.
  llvm::SmallVector<ArgumentDescriptor, 4> ArgDescList;

  /// Keep a "view" of precompiled information on the direct results that we
  /// will use during our optimization.
  llvm::SmallVector<ResultDescriptor, 4> ResultDescList;

  /// Return a function name based on ArgDescList and ResultDescList.
  std::string createOptimizedSILFunctionName();

  /// Return a function type based on ArgDescList and ResultDescList.
  CanSILFunctionType createOptimizedSILFunctionType();

  /// Take ArgDescList and ResultDescList and create an optimized function
  /// based on the current function we are analyzing. This also has
  /// the side effect of turning the current function into a thunk.
  /// If function specialization is successful, the optimized function is
  /// returned, otherwise nullptr is returned.
  SILFunction *createOptimizedSILFunction();

  /// ----------------------------------------------------------///
  /// Function to implemented for specific FSO transformations. ///
  /// ----------------------------------------------------------///

  /// Compute what the function name will be based on the given result decriptor.
  /// Default implementation simply passes it through.
  virtual void computeOptimizedFunctionName(ResultDescriptor &RV, FSSM &M) {}

  /// Compute what the function name will be based on the given result decriptor.
  /// Default implementation simply passes it through.
  virtual void computeOptimizedFunctionName(ArgumentDescriptor &AD, FSSM &M) {}

  /// Compute what the function interface will look like based on the
  /// optimization we are doing on the given result descriptor. Default
  /// implemenation simply passes it through.
  virtual void
  computeOptimizedInterface(ResultDescriptor &RV, SILResultInfoList &Out){
    Out.push_back(RV.ResultInfo);
  }

  /// Compute what the function interface will look like based on the
  /// optimization we are doing on the given argument descriptor. Default
  /// implemenation simply passes it through.
  virtual void
  computeOptimizedInterface(ArgumentDescriptor &AD, SILParameterInfoList &Out){
    Out.push_back(AD.Arg->getKnownParameterInfo());
  }

  /// Setup the thunk arguments based on the given argument and result
  /// descriptor info. Every transformation must defines this interface. Default
  /// implementation simply passes it through.
  virtual void
  addThunkArgument(const ArgumentDescriptor &AD, SILBuilder &Builder, SILBasicBlock *BB, 
               llvm::SmallVectorImpl<SILValue> &NewArgs) {
    NewArgs.push_back(BB->getBBArg(AD.Index));
  } 

  /// Set up epilogue work for the thunk arguments based in the given argument.
  /// Default implementation simply passes it through.
  virtual void
  completeThunkArgument(const ArgumentDescriptor &AD, SILBuilder &Builder, SILFunction *F) {}

  virtual void
  completeThunkResult(const ResultDescriptor &RD, SILBuilder &Builder, SILFunction *F) {}

  /// Analyze the function and decide whether to optimize based on the function
  /// signature.
  virtual bool analyze() = 0;

  /// Do the actual transformations and return the transformed function, not the
  /// thunk.
  virtual SILFunction *transform() = 0;
public:
  /// Constructor.
  FunctionSignatureTransform(SILFunction *F, llvm::BumpPtrAllocator &BPA,
                             AliasAnalysis *AA, RCIdentityAnalysis *RCIA)
    : F(F), FO(nullptr), Allocator(BPA), AA(AA), RCIA(RCIA) {}

  /// virtual destructor.
  virtual ~FunctionSignatureTransform() {}

  ArrayRef<ArgumentDescriptor> getArgDescList() { return ArgDescList; }

  ArrayRef<ResultDescriptor> getResultDescList() { return ResultDescList; }
};

std::string
FunctionSignatureTransform::createOptimizedSILFunctionName() {
  Mangle::Mangler M;
  auto P = SpecializationPass::FunctionSignatureOpts;
  FSSM FM(P, M, F->isFragile(), F);

  // Compute the argument name.
  for (auto &ArgDesc : ArgDescList) {
    computeOptimizedFunctionName(ArgDesc, FM);
  }

  // Compute the result name.
  for (auto &ResultDesc : ResultDescList) {
    computeOptimizedFunctionName(ResultDesc, FM);
  }

  FM.mangle();
  return M.finalize();
}

CanSILFunctionType
FunctionSignatureTransform::createOptimizedSILFunctionType() {
  // Compute the argument interface parameters.
  SILParameterInfoList InterfaceParams;
  for (auto &ArgDesc : ArgDescList) {
    computeOptimizedInterface(ArgDesc, InterfaceParams);
  }

  // Compute the result interface parameters.
  SILResultInfoList InterfaceResults;
  for (auto &ResultDesc : ResultDescList) {
    computeOptimizedInterface(ResultDesc, InterfaceResults);
  }

  CanSILFunctionType FTy = F->getLoweredFunctionType();
  return SILFunctionType::get(FTy->getGenericSignature(), FTy->getExtInfo(),
                              FTy->getCalleeConvention(), InterfaceParams,
                              InterfaceResults, FTy->getOptionalErrorResult(),
                              F->getModule().getASTContext());
}

SILFunction *FunctionSignatureTransform::createOptimizedSILFunction() {
  // Create the optimized function !
  //
  // Create the name of the optimized function.
  std::string NewFName = createOptimizedSILFunctionName();
  // We are extremely unlucky to have a collision on the function name.
  if (F->getModule().lookUpFunction(NewFName))
    return nullptr;
 
  // Create the type of the optimized function.
  SILModule &M = F->getModule();
  CanSILFunctionType NewFTy = createOptimizedSILFunctionType();
 
  // Create the optimized function.
  auto *NewF = M.getOrCreateFunction(
      F->getLinkage(), NewFName, NewFTy, nullptr, F->getLocation(), F->isBare(),
      F->isTransparent(), F->isFragile(), F->isThunk(), F->getClassVisibility(),
      F->getInlineStrategy(), F->getEffectsKind(), 0, F->getDebugScope(),
      F->getDeclContext());

  NewF->setDeclCtx(F->getDeclContext());

  // Array semantic clients rely on the signature being as in the original
  // version.
  for (auto &Attr : F->getSemanticsAttrs())
    if (!StringRef(Attr).startswith("array."))
      NewF->addSemanticsAttr(Attr);

  // Then we transfer the body of F to NewF. At this point, the arguments of the
  // first BB will not match.
  NewF->spliceBody(F);

  // Create the thunk body !
  SILBasicBlock *ThunkBody = F->createBasicBlock();
  for (auto &ArgDesc : ArgDescList) {
    ThunkBody->createBBArg(ArgDesc.Arg->getType(), ArgDesc.Decl);
  }

  SILLocation Loc = ThunkBody->getParent()->getLocation();
  SILBuilder Builder(ThunkBody);
  Builder.setCurrentDebugScope(ThunkBody->getParent()->getDebugScope());

  FunctionRefInst *FRI = Builder.createFunctionRef(Loc, NewF);

  // Create the args for the thunk's apply, ignoring any dead arguments.
  llvm::SmallVector<SILValue, 8> ThunkArgs;
  for (auto &ArgDesc : getArgDescList()) {
    addThunkArgument(ArgDesc, Builder, ThunkBody, ThunkArgs);
  }

  // We are ignoring generic functions and functions with out parameters for
  // now.
  SILType LoweredType = NewF->getLoweredType();
  SILType ResultType = LoweredType.getFunctionInterfaceResultType();
  SILValue ReturnValue = Builder.createApply(Loc, FRI, LoweredType, ResultType,
                                             ArrayRef<Substitution>(),
                                             ThunkArgs, false);
  for (auto &ArgDesc : getArgDescList()) {
    completeThunkArgument(ArgDesc, Builder, F);
  }
  for (auto &ResDesc : getResultDescList()) {
    completeThunkResult(ResDesc, Builder, F);
  }

  // Function that are marked as @NoReturn must be followed by an 'unreachable'
  // instruction.
  if (NewF->getLoweredFunctionType()->isNoReturn()) {
    Builder.createUnreachable(Loc);
    return NewF;
  }

  Builder.createReturn(Loc, ReturnValue);

  F->setThunk(IsThunk);
  assert(F->getDebugScope()->Parent != NewF->getDebugScope()->Parent);
  return NewF;
}

//===----------------------------------------------------------------------===//
//                      Owned to Guaranteed Optimization 
//===----------------------------------------------------------------------===//

/// OwnedToGuaranteedTransform - Owned to Guanranteed optimization.
class OwnedToGuaranteedTransform : public FunctionSignatureTransform {

  /// Analyze the function and decide whether to optimize based on the function
  /// signature.
  bool analyzeParameters();
  bool analyzeResult();

  /// Transform the parameters and result of the function.
  void transformParameters();
  void transformResult();

public:
   OwnedToGuaranteedTransform(SILFunction *F, llvm::BumpPtrAllocator &BPA,
                              AliasAnalysis *AA, RCIdentityAnalysis *RCIA)
     : FunctionSignatureTransform(F, BPA, AA, RCIA) {}
   /// virtual destructor.
   virtual ~OwnedToGuaranteedTransform() {}

   /// Find any owned to guaranteed opportunities.
   bool analyze() {
     bool Params = analyzeParameters();
     bool Result = analyzeResult();
     return Params || Result;
   }

  /// Do the actual transformations.
  SILFunction *transform() {
    // Create the new function.
    FO = createOptimizedSILFunction();
    if (!FO)
      return F;
    // Optimize the new function.
    transformResult();
    transformParameters();
    return FO;
  }

  virtual void completeThunkArgument(const ArgumentDescriptor &AD,
                                     SILBuilder &Builder, SILFunction *F) {
    // If we have any arguments that were consumed but are now guaranteed,
    // insert a release_value.
    if (!AD.OwnedToGuaranteed)
      return;
    Builder.createReleaseValue(RegularLocation(SourceLoc()),
                               F->getArguments()[AD.Index],
                               Atomicity::Atomic);
  }

  virtual void completeThunkResult(const ResultDescriptor &RD,
                                   SILBuilder &Builder, SILFunction *F) {
    if (!RD.OwnedToGuaranteed)
      return;
    Builder.createRetainValue(RegularLocation(SourceLoc()), findOnlyApply(F),
                              Atomicity::Atomic);
  }

  /// Compute what the function name will be based on the given result decriptor.
  void computeOptimizedFunctionName(ResultDescriptor &RV, FSSM &M) {
    if (!RV.OwnedToGuaranteed)
      return;
    M.setReturnValueOwnedToUnowned();
  }

  /// Compute what the function name will be based on the given result decriptor.
  void computeOptimizedFunctionName(ArgumentDescriptor &AD, FSSM &M) {
    if (!AD.OwnedToGuaranteed)
      return;
    M.setArgumentOwnedToGuaranteed(AD.Index);
  }

  void computeOptimizedInterface(ResultDescriptor &R, SILResultInfoList &Out);
  void computeOptimizedInterface(ArgumentDescriptor &A, SILParameterInfoList &Out);

};

bool OwnedToGuaranteedTransform::analyzeParameters() {
  ArrayRef<SILArgument *> Args = F->begin()->getBBArgs();
  // A map from consumed SILArguments to the release associated with an
  // argument.
  // TODO: The return block and throw block should really be abstracted away.
  ConsumedArgToEpilogueReleaseMatcher ArgToReturnReleaseMap(RCIA->get(F), F);
  ConsumedArgToEpilogueReleaseMatcher ArgToThrowReleaseMap(
      RCIA->get(F), F, ConsumedArgToEpilogueReleaseMatcher::ExitKind::Throw);

  // Did we decide we should optimize any parameter?
  bool SignatureOptimize = false;

  // Analyze the argument information.
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgumentDescriptor A(Allocator, Args[i]);

    // See if we can find a ref count equivalent strong_release or release_value
    // at the end of this function if our argument is an @owned parameter.
    if (A.hasConvention(SILArgumentConvention::Direct_Owned)) {
      auto Releases = ArgToReturnReleaseMap.getReleasesForArgument(A.Arg);
      if (!Releases.empty()) {
        // If the function has a throw block we must also find a matching
        // release in the throw block.
        auto ReleasesInThrow = ArgToThrowReleaseMap.getReleasesForArgument(A.Arg);
        if (!ArgToThrowReleaseMap.hasBlock() || !ReleasesInThrow.empty()) {
          A.CalleeRelease = Releases;
          A.CalleeReleaseInThrowBlock = ReleasesInThrow;
          // We can convert this parameter to a @guaranteed.
          A.OwnedToGuaranteed = true;
          SignatureOptimize = true;
        }
      }
    }
    // Add the argument to our list.
    ArgDescList.push_back(std::move(A));
  }
  return SignatureOptimize;
}

bool OwnedToGuaranteedTransform::analyzeResult() {
  // Did we decide we should optimize any parameter?
  auto FTy = F->getLoweredFunctionType();
  for (SILResultInfo InterfaceResult : FTy->getAllResults()) {
    ResultDescList.emplace_back(InterfaceResult);
  }

  // Analyze return result information.
  if (FTy->getIndirectResults().size())
    return false;

  // For now, only do anything if there's a single direct result.
  if (FTy->getDirectResults().size() != 1)
    return false; 

  bool SignatureOptimize = false;
  if (ResultDescList[0].hasConvention(ResultConvention::Owned)) {
    auto &RI = ResultDescList[0];
    // We have an @owned return value, find the epilogue retains now.
    ConsumedResultToEpilogueRetainMatcher RVToReturnRetainMap(RCIA->get(F), AA, F);
    auto Retains = RVToReturnRetainMap.getEpilogueRetains();
    // We do not need to worry about the throw block, as the return value is only
    // going to be used in the return block/normal block of the try_apply instruction.
    if (!Retains.empty()) {
      RI.CalleeRetain = Retains;
      SignatureOptimize = true;
      RI.OwnedToGuaranteed = true;
    }
  }
  return SignatureOptimize;
}

void OwnedToGuaranteedTransform::transformParameters() {
  // And remove all Callee releases that we found and made redundant via owned
  // to guaranteed conversion.
  for (const ArgumentDescriptor &AD : ArgDescList) {
    if (!AD.OwnedToGuaranteed)
      continue;
    ++ NumOwnedConvertedToGuaranteed;
    for (auto &X : AD.CalleeRelease) 
      X->eraseFromParent();
    for (auto &X : AD.CalleeReleaseInThrowBlock) 
      X->eraseFromParent();
  }
}

void OwnedToGuaranteedTransform::transformResult() {
  // And remove all callee retains that we found and made redundant via owned
  // to unowned conversion.
  for (const ResultDescriptor &RD : ResultDescList) {
    if (!RD.OwnedToGuaranteed)
      continue;
    ++NumOwnedConvertedToNotOwnedResult; 
    for (auto &X : RD.CalleeRetain) {
      if (isa<StrongRetainInst>(X) || isa<RetainValueInst>(X)) {
        X->eraseFromParent();
        continue;
      }
      // Create a release to balance it out.
      assert(isa<ApplyInst>(X) && "Unknown epilogue retain");
      createDecrement(X, dyn_cast<ApplyInst>(X)->getParent()->getTerminator());
    }
  }
}

void
OwnedToGuaranteedTransform::
computeOptimizedInterface(ArgumentDescriptor &AD, SILParameterInfoList &Out) {
  auto ParameterInfo = AD.Arg->getKnownParameterInfo();
  // If this argument is live, but we cannot optimize it.
  if (!AD.canOptimizeLiveArg()) {
    Out.push_back(ParameterInfo);
    return;
  }

  // If we cannot explode this value, handle callee release and return.
  // If we found releases in the callee in the last BB on an @owned
  // parameter, change the parameter to @guaranteed and continue...
  if (AD.OwnedToGuaranteed) {
    assert(ParameterInfo.getConvention() == ParameterConvention::Direct_Owned &&
           "Can only transform @owned => @guaranteed in this code path");
    SILParameterInfo NewInfo(ParameterInfo.getType(),
                             ParameterConvention::Direct_Guaranteed);
    Out.push_back(NewInfo);
    ++NumOwnedConvertedToGuaranteed;
    return;
  }

  // Otherwise just propagate through the parameter info.
  Out.push_back(ParameterInfo);
}

void
OwnedToGuaranteedTransform::
computeOptimizedInterface(ResultDescriptor &RV, SILResultInfoList &Out) {
  // ResultDescs only covers the direct results; we currently can't ever
  // change an indirect result.  Piece the modified direct result information
  // back into the all-results list.
  if (RV.OwnedToGuaranteed) {
    Out.push_back(SILResultInfo(RV.ResultInfo.getType(), ResultConvention::Unowned));
    ++NumOwnedConvertedToNotOwnedResult;
    return;
  }

  Out.push_back(RV.ResultInfo);
}

//===----------------------------------------------------------------------===//
//                        Dead Argument Optimization 
//===----------------------------------------------------------------------===//

/// DeadArgumentTransform - Owned to Guanranteed optimization.
class DeadArgumentTransform : public FunctionSignatureTransform {
  /// Does any call inside the given function may bind dynamic 'Self' to a
  /// generic argument of the callee.
  bool MayBindDynamicSelf;

  /// Analyze the function and decide whether to optimize based on the function
  /// signature.
  bool analyzeParameters();

  /// Transform the parameters and result of the function.
  void transformParameters();

  /// Return true if this argument is used in a non-trivial way.
  bool hasNonTrivialNonDebugUse(SILArgument *Arg); 

public:
   DeadArgumentTransform(SILFunction *F, llvm::BumpPtrAllocator &BPA,
                           AliasAnalysis *AA, RCIdentityAnalysis *RCIA)
     : FunctionSignatureTransform(F, BPA, AA, RCIA),
       MayBindDynamicSelf(computeMayBindDynamicSelf(F)) {}

   /// virtual destructor.
   virtual ~DeadArgumentTransform() {}

   /// Find any owned to guaranteed opportunities.
   bool analyze() {
     // pass through the result.
     auto FTy = F->getLoweredFunctionType();
     for (SILResultInfo InterfaceResult : FTy->getAllResults()) {
       ResultDescList.emplace_back(InterfaceResult);
     }
     return analyzeParameters();
   }

  /// Do the actual transformations.
  SILFunction *transform() {
    // Create the new function.
    FO = createOptimizedSILFunction();
    if (!FO)
      return F;
    // Optimize the new function.
    transformParameters();
    return FO;
  }

  bool isArgumentABIRequired(SILArgument *Arg) {
    // This implicitly asserts that a function binding dynamic self has a self
   // metadata argument or object from which self metadata can be obtained.
   return MayBindDynamicSelf && (F->getSelfMetadataArgument() == Arg);
  }

  /// Simply add the function argument.
  void addThunkArgument(const ArgumentDescriptor &AD, SILBuilder &Builder,
                    SILBasicBlock *BB, llvm::SmallVectorImpl<SILValue> &NewArgs) {
    if (AD.IsEntirelyDead) {
      ++NumDeadArgsEliminated;
      return;
    }
    NewArgs.push_back(BB->getBBArg(AD.Index));
  }

  void computeOptimizedInterface(ArgumentDescriptor &A, SILParameterInfoList &Out);

  /// Compute what the function name will be based on the given result decriptor.
  void computeOptimizedFunctionName(ArgumentDescriptor &AD, FSSM &M) {
    if (!AD.IsEntirelyDead)
      return;
    M.setArgumentDead(AD.Index);
  }
};

bool DeadArgumentTransform::hasNonTrivialNonDebugUse(SILArgument *Arg) {
  llvm::SmallVector<SILInstruction *, 8> Worklist;
  llvm::SmallPtrSet<SILInstruction *, 8> SeenInsts;

  for (Operand *I : getNonDebugUses(SILValue(Arg)))
    Worklist.push_back(I->getUser());

  while (!Worklist.empty()) {
    SILInstruction *U = Worklist.pop_back_val();
    if (!SeenInsts.insert(U).second)
      continue;

    // If U is a terminator inst, return false.
    if (isa<TermInst>(U))
      return true;

    // If U has side effects...
    if (U->mayHaveSideEffects()) 
      return true;

    // Otherwise add all non-debug uses of I to the worklist.
    for (Operand *I : getNonDebugUses(SILValue(U)))
      Worklist.push_back(I->getUser());
  }
  return false;
}

bool DeadArgumentTransform::analyzeParameters() {
  // Did we decide we should optimize any parameter?
  bool SignatureOptimize = false;
  ArrayRef<SILArgument *> Args = F->begin()->getBBArgs();

  // Analyze the argument information.
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgumentDescriptor A(Allocator, Args[i]);

    // Check whether argument is dead.
    A.IsEntirelyDead = true;
    A.IsEntirelyDead &= !Args[i]->isSelf();
    A.IsEntirelyDead &= !isArgumentABIRequired(Args[i]);
    A.IsEntirelyDead &= !hasNonTrivialNonDebugUse(Args[i]); 
    SignatureOptimize |= A.IsEntirelyDead;

    // Add the argument to our list.
    ArgDescList.push_back(std::move(A));
  }
  return SignatureOptimize;
}

void DeadArgumentTransform::transformParameters() {
  SILBasicBlock *BB = &*FO->begin();
  // Remove any dead argument starting from the last argument to the first.
  for (const ArgumentDescriptor &AD : reverse(ArgDescList)) {
    if (!AD.IsEntirelyDead)
      continue;
    SILArgument *Arg = BB->getBBArg(AD.Index);
    eraseUsesOfValue(Arg);
    BB->eraseBBArg(AD.Index);
  }
}

void DeadArgumentTransform::
computeOptimizedInterface(ArgumentDescriptor &AD, SILParameterInfoList &Out) {
  // If this argument is live, but we cannot optimize it.
  // If we cannot explode this value, handle callee release and return.
  // If we found releases in the callee in the last BB on an @owned
  // parameter, change the parameter to @guaranteed and continue...
  if (AD.IsEntirelyDead)
    return;

  // Otherwise just propagate through the parameter info.
  auto ParameterInfo = AD.Arg->getKnownParameterInfo();
  Out.push_back(ParameterInfo);
}

//===----------------------------------------------------------------------===//
//                        Dead Argument Optimization 
//===----------------------------------------------------------------------===//

/// ArgumentExplosionTransform - Owned to Guanranteed optimization.
class ArgumentExplosionTransform : public FunctionSignatureTransform {
  /// Does any call inside the given function may bind dynamic 'Self' to a
  /// generic argument of the callee.
  bool MayBindDynamicSelf;

  /// Analyze the function and decide whether to optimize based on the function
  /// signature.
  bool analyzeParameters();

  /// Transform the parameters and result of the function.
  void transformParameters();

  /// Return true if this argument is used in a non-trivial way.
  bool hasNonTrivialNonDebugUse(SILArgument *Arg); 

public:
   ArgumentExplosionTransform(SILFunction *F, llvm::BumpPtrAllocator &BPA,
                           AliasAnalysis *AA, RCIdentityAnalysis *RCIA)
     : FunctionSignatureTransform(F, BPA, AA, RCIA),
       MayBindDynamicSelf(computeMayBindDynamicSelf(F)) {}

   /// virtual destructor.
   virtual ~ArgumentExplosionTransform() {}

   /// Find any owned to guaranteed opportunities.
   bool analyze() {
     // Pass through the results.
     auto FTy = F->getLoweredFunctionType();
     for (SILResultInfo InterfaceResult : FTy->getAllResults()) {
       ResultDescList.emplace_back(InterfaceResult);
     }
     return analyzeParameters();
   }

  /// Do the actual transformations.
  SILFunction *transform() {
    // Create the new function.
    FO = createOptimizedSILFunction();
    if (!FO)
      return F;
    // Optimize the new function.
    transformParameters();
    return FO;
  }

  bool isArgumentABIRequired(SILArgument *Arg) {
    // This implicitly asserts that a function binding dynamic self has a self
   // metadata argument or object from which self metadata can be obtained.
   return MayBindDynamicSelf && (F->getSelfMetadataArgument() == Arg);
  }

  /// Simply add the function argument.
  void addThunkArgument(const ArgumentDescriptor &AD, SILBuilder &Builder,
                    SILBasicBlock *BB, llvm::SmallVectorImpl<SILValue> &NewArgs) {
    if (!AD.Explode) {
       NewArgs.push_back(BB->getBBArg(AD.Index));
       return;
     }

    ++NumSROAArguments;
    AD.ProjTree.createTreeFromValue(Builder, BB->getParent()->getLocation(),
                                    BB->getBBArg(AD.Index), NewArgs);
  }

  void computeOptimizedInterface(ArgumentDescriptor &A, SILParameterInfoList &Out);


  /// Compute what the function name will be based on the given result decriptor.
  void computeOptimizedFunctionName(ArgumentDescriptor &AD, FSSM &M) {
    if (!AD.Explode)
      return;
    M.setArgumentSROA(AD.Index);
  }
};

bool ArgumentExplosionTransform::analyzeParameters() {
  // Did we decide we should optimize any parameter?
  bool SignatureOptimize = false;
  ArrayRef<SILArgument *> Args = F->begin()->getBBArgs();

  // Analyze the argument information.
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgumentDescriptor A(Allocator, Args[i]);

    A.ProjTree.computeUsesAndLiveness(A.Arg);
    // Has non-trivial uses.
    A.Explode = A.shouldExplode();

    // Add the argument to our list.
    ArgDescList.push_back(std::move(A));

    SignatureOptimize |= A.Explode;
  }
  return SignatureOptimize;
}

void ArgumentExplosionTransform::transformParameters() {
  SILBasicBlock *BB = &*FO->begin();
  SILBuilder Builder(BB->begin());
  Builder.setCurrentDebugScope(BB->getParent()->getDebugScope());
  for (const ArgumentDescriptor &AD : reverse(ArgDescList)) {
    if (!AD.Explode)
      continue;

  // OK, we need to explode this argument.
  unsigned ArgOffset = AD.Index + 1;
  llvm::SmallVector<SILValue, 8> LeafValues;

  // We do this in the same order as leaf types since ProjTree expects that the
  // order of leaf values matches the order of leaf types.
  {
    llvm::SmallVector<const ProjectionTreeNode*, 8> LeafNodes;
    AD.ProjTree.getLeafNodes(LeafNodes);
    for (auto Node : LeafNodes) {
      LeafValues.push_back(BB->insertBBArg(
          ArgOffset++, Node->getType(), BB->getBBArg(AD.Index)->getDecl()));
    }
  }

  // We have built a projection tree and filled it with liveness information.
  //
  // Use this as a base to replace values in current function with their leaf
  // values.
  //
  // NOTE: this also allows us to NOT modify the results of an analysis pass.
  llvm::BumpPtrAllocator Allocator;
  ProjectionTree PT(BB->getModule(), Allocator);
  PT.initializeWithExistingTree(AD.ProjTree);

  // Then go through the projection tree constructing aggregates and replacing
  // uses.
  //
  // TODO: What is the right location to use here?
  PT.replaceValueUsesWithLeafUses(Builder, BB->getParent()->getLocation(),
                                  LeafValues);

  // We ignored debugvalue uses when we constructed the new arguments, in order
  // to preserve as much information as possible, we construct a new value for
  // OrigArg from the leaf values and use that in place of the OrigArg.
  SILValue NewOrigArgValue = PT.computeExplodedArgumentValue(Builder,
                                           BB->getParent()->getLocation(),
                                           LeafValues);

  // Replace all uses of the original arg with the new value.
  SILArgument *OrigArg = BB->getBBArg(AD.Index);
  OrigArg->replaceAllUsesWith(NewOrigArgValue);

  // Now erase the old argument since it does not have any uses. We also
  // decrement ArgOffset since we have one less argument now.
  BB->eraseBBArg(AD.Index);
  }
}

void ArgumentExplosionTransform::
computeOptimizedInterface(ArgumentDescriptor &AD, SILParameterInfoList &Out) {
  auto PInfo = AD.Arg->getKnownParameterInfo();
  // We are not exploding the argument.
  if (!AD.Explode) {
    Out.push_back(PInfo);
    return;
  }

  llvm::SmallVector<const ProjectionTreeNode*, 8> LeafNodes;
  AD.ProjTree.getLeafNodes(LeafNodes);
  for (auto Node : LeafNodes) {
    // Node type.
    SILType Ty = Node->getType();
    DEBUG(llvm::dbgs() << "                " << Ty << "\n");
    // If Ty is trivial, just pass it directly.
    if (Ty.isTrivial(AD.Arg->getModule())) {
      SILParameterInfo NewInfo(Ty.getSwiftRValueType(),
                               ParameterConvention::Direct_Unowned);
      Out.push_back(NewInfo);
      continue;
    }

    // Ty is not trivial, pass it through as the original calling convention.
    SILParameterInfo NewInfo(Ty.getSwiftRValueType(), PInfo.getConvention());
    Out.push_back(NewInfo);
  }
}

//===----------------------------------------------------------------------===//
//                           Top Level Entry Point
//===----------------------------------------------------------------------===//
namespace {
class FunctionSignatureOpts : public SILFunctionTransform {
  /// This is the function to analyze and optimize.
  SILFunction *NewF;
  SILFunction *getOptFunction() { return NewF; }
  void setOptFunction(SILFunction *F) { NewF = F; }
public:
  /// constructor.
  FunctionSignatureOpts() : NewF(nullptr) {}
  void run() override {
    auto *F = getFunction();
    auto *RCIA = getAnalysis<RCIdentityAnalysis>();
    auto *AA = PM->getAnalysis<AliasAnalysis>();
    auto *CA = PM->getAnalysis<CallerAnalysis>();
    llvm::BumpPtrAllocator Allocator;
    DEBUG(llvm::dbgs() << "*** FSO on function: " << F->getName() << " ***\n");

    if (F->isThunk())
      return;

    // Don't optimize callees that should not be optimized.
    if (!F->shouldOptimize())
      return;

    // If this function does not have a caller in the current module.
    if (!CA->hasCaller(F))
      return;

    // Check the signature of F to make sure that it is a function that we
    // can specialize. These are conditions independent of the call graph.
    if (!canSpecializeFunction(F))
      return;

    bool Changed = false;
    // Set the function to optimize.
    setOptFunction(getFunction());

    // We run function signature optimization in the following sequence.
    // OwnedToGuaranteed enables dead argument elimination, and dead argument
    // elimination gives opportunities for argument explosion.
    //
    // Owned to Guaranteed optimization.
    OwnedToGuaranteedTransform OG(getOptFunction(), Allocator, AA, RCIA);
    if (OG.analyze()) {
      Changed = true;
      setOptFunction(OG.transform());
    }

    DeadArgumentTransform DA(getOptFunction(), Allocator, AA, RCIA);
    if (DA.analyze()) {
      Changed = true;
      setOptFunction(DA.transform());
    }

    ArgumentExplosionTransform AE(getOptFunction(), Allocator, AA, RCIA);
    if (AE.analyze()) {
      Changed = true;
      setOptFunction(AE.transform());
    }

    OwnedToGuaranteedTransform OG2(getOptFunction(), Allocator, AA, RCIA);
    if (OG2.analyze()) {
      Changed = true;
      setOptFunction(OG2.transform());
    }

    DeadArgumentTransform DA2(getOptFunction(), Allocator, AA, RCIA);
    if (DA2.analyze()) {
      Changed = true;
      setOptFunction(DA2.transform());
    }

    ArgumentExplosionTransform AE2(getOptFunction(), Allocator, AA, RCIA);
    if (AE2.analyze()) {
      Changed = true;
      setOptFunction(AE2.transform());
    }

    if (Changed) { 
      // Collapse the chain of thunks.
      collapseThunkChain(F, PM);

      // The thunk now carries the information on how the signature is
      // optimized. If we inline the thunk, we will get the benefit of calling
      // the signature optimized function without additional setup on the
      // caller side.
      F->setInlineStrategy(AlwaysInline);

      // Make sure the PM knows about this function. This will also help us
      // with self-recursion.
      notifyPassManagerOfFunction(NewF);
      invalidateAnalysis(SILAnalysis::InvalidationKind::Everything);
      F->verify();
      NewF->verify();
      ++ NumFunctionSignaturesOptimized;
    }
  }

  StringRef getName() override { return "Function Signature Optimization"; }
};

} // end anonymous namespace

SILTransform *swift::createFunctionSignatureOpts() {
  return new FunctionSignatureOpts();
}
