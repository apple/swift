//===--- SpeculativeDevirtualizer.cpp - Speculatively devirtualize calls --===//
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
// Speculatively devirtualizes witness- and class-method calls into direct
// calls.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-speculative-devirtualizer"

#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/AST/ASTContext.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;

// This is the limit for the number of subclasses (jump targets) that the
// speculative devirtualizer will try to predict.
static const int MaxNumSpeculativeTargets = 6;

STATISTIC(NumTargetsPredicted, "Number of monomorphic functions predicted");

/// We want to form a second edge to the given block, but we know
/// that'll form a critical edge.  Return a basic block to which we can
/// create an edge essentially like the original edge.
static SILBasicBlock *cloneEdge(TermInst *TI, unsigned SuccIndex) {
#ifndef NDEBUG
  auto origDestBB = TI->getSuccessors()[SuccIndex].getBB();
#endif

  // Split the edge twice.  The first split will become our cloned
  // and temporarily-unused edge.  The second split will remain in place
  // as the original edge.
  auto clonedEdgeBB = splitEdge(TI, SuccIndex);
  auto replacementEdgeBB = splitEdge(TI, SuccIndex);

  // Extract the terminators.
  auto clonedEdgeBranch =
    cast<BranchInst>(clonedEdgeBB->getTerminator());
  auto replacementEdgeBranch =
    cast<BranchInst>(replacementEdgeBB->getTerminator());

  assert(TI->getSuccessors()[SuccIndex].getBB() == replacementEdgeBB);
  assert(replacementEdgeBranch->getDestBB() == clonedEdgeBB);
  assert(clonedEdgeBranch->getDestBB() == origDestBB);

  // Change the replacement branch to point to the original destination.
  // This will leave the cloned edge unused, which is how we wanted it.
  replacementEdgeBranch->getSuccessors()[0] = clonedEdgeBranch->getDestBB();
  assert(clonedEdgeBB->pred_empty());

  return clonedEdgeBB;
}

// A utility function for cloning the apply instruction.
static FullApplySite CloneApply(FullApplySite AI, SILValue SelfArg,
                                SILBuilder &Builder) {
  // Clone the Apply.
  Builder.setCurrentDebugScope(AI.getDebugScope());
  auto Args = AI.getArguments();
  SmallVector<SILValue, 8> Ret(Args.size());
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    if (i == e - 1 && SelfArg) {
      Ret[i] = SelfArg;
    } else {
      Ret[i] = Args[i];
    }
  }

  FullApplySite NAI;

  switch (AI.getInstruction()->getKind()) {
  case SILInstructionKind::ApplyInst:
    NAI = Builder.createApply(AI.getLoc(), AI.getCallee(),
                              AI.getSubstitutionMap(),
                              Ret, AI.getApplyOptions());
    break;
  case SILInstructionKind::TryApplyInst: {
    auto *TryApplyI = cast<TryApplyInst>(AI.getInstruction());
    auto NormalBB = cloneEdge(TryApplyI, TryApplyInst::NormalIdx);
    auto ErrorBB = cloneEdge(TryApplyI, TryApplyInst::ErrorIdx);
    NAI = Builder.createTryApply(AI.getLoc(), AI.getCallee(),
                                 AI.getSubstitutionMap(),
                                 Ret, NormalBB, ErrorBB,
                                 AI.getApplyOptions());
    break;
  }
  default:
    llvm_unreachable("Trying to clone an unsupported apply instruction");
  }

  return NAI;
}

/// Insert monomorphic inline caches for a specific class or metatype
/// type \p SubClassTy.
static FullApplySite speculateMonomorphicTarget(FullApplySite AI,
                                                CanType SubType,
                                                ClassDecl *CD,
                                                CheckedCastBranchInst *&CCBI) {
  if (SubType->hasDynamicSelfType())
    return FullApplySite();

  CCBI = nullptr;
  // Bail if this class_method cannot be devirtualized.
  if (!canDevirtualizeClassMethod(AI, CD))
    return FullApplySite();

  // Can't speculate begin_apply yet.
  if (isa<BeginApplyInst>(AI))
    return FullApplySite();

  // Create a diamond shaped control flow and a checked_cast_branch
  // instruction that checks the exact type of the object.
  // This cast selects between two paths: one that calls the slow dynamic
  // dispatch and one that calls the specific method.
  auto It = AI.getInstruction()->getIterator();
  SILFunction *F = AI.getFunction();
  SILBasicBlock *Entry = AI.getParent();

  ClassMethodInst *CMI = cast<ClassMethodInst>(AI.getCallee());

  // Iden is the basic block containing the direct call.
  SILBasicBlock *Iden = F->createBasicBlock();
  // Virt is the block containing the slow virtual call.
  SILBasicBlock *Virt = F->createBasicBlock();
  Iden->createPhiArgument(SILType::getPrimitiveObjectType(SubType),
                          CMI->getOperand()->getOwnershipKind());

  SILBasicBlock *Continue = Entry->split(It);

  SILBuilderWithScope Builder(Entry, AI.getInstruction());
  // Create the checked_cast_branch instruction that checks at runtime if the
  // class instance is identical to the SILType.

  CCBI = Builder.createCheckedCastBranch(AI.getLoc(), /*exact*/ true,
                                      CMI->getOperand(), 
                                      CMI->getOperand()->getType().getASTType(),
                                      SILType::getPrimitiveObjectType(SubType),
                                      SubType, Iden, Virt);
  It = CCBI->getIterator();

  SILBuilderWithScope VirtBuilder(Virt, AI.getInstruction());
  SILBuilderWithScope IdenBuilder(Iden, AI.getInstruction());
  // This is the class reference downcasted into subclass SubType.
  SILValue DownCastedClassInstance = Iden->getArgument(0);

  // Copy the two apply instructions into the two blocks.
  FullApplySite IdenAI = CloneApply(AI, DownCastedClassInstance, IdenBuilder);
  FullApplySite VirtAI = CloneApply(AI, SILValue(), VirtBuilder);

  // See if Continue has a release on self as the instruction right after the
  // apply. If it exists, move it into position in the diamond.
  SILBasicBlock::iterator next =
      next_or_end(Continue->begin(), Continue->end());
  auto *Release =
      (next == Continue->end()) ? nullptr : dyn_cast<StrongReleaseInst>(next);
  if (Release && Release->getOperand() == CMI->getOperand()) {
    VirtBuilder.createStrongRelease(Release->getLoc(), CMI->getOperand(),
                                    Release->getAtomicity());
    IdenBuilder.createStrongRelease(Release->getLoc(), DownCastedClassInstance,
                                    Release->getAtomicity());
    Release->eraseFromParent();
  }

  // Create a PHInode for returning the return value from both apply
  // instructions.
  SILArgument *Arg =
      Continue->createPhiArgument(AI.getType(), OwnershipKind::Owned);
  if (!isa<TryApplyInst>(AI)) {
    if (AI.getSubstCalleeType()->isNoReturnFunction(
            F->getModule(), AI.getFunction()->getTypeExpansionContext())) {
      IdenBuilder.createUnreachable(AI.getLoc());
      VirtBuilder.createUnreachable(AI.getLoc());
    } else {
      IdenBuilder.createBranch(AI.getLoc(), Continue,
                               { cast<ApplyInst>(IdenAI) });
      VirtBuilder.createBranch(AI.getLoc(), Continue,
                               { cast<ApplyInst>(VirtAI) });
    }
  }

  // Remove the old Apply instruction.
  assert(AI.getInstruction() == &Continue->front() &&
         "AI should be the first instruction in the split Continue block");
  if (isa<TryApplyInst>(AI)) {
    AI.getInstruction()->eraseFromParent();
    assert(Continue->empty() &&
           "There should not be an instruction after try_apply");
    Continue->eraseFromParent();
  } else {
    auto apply = cast<ApplyInst>(AI);
    apply->replaceAllUsesWith(Arg);
    apply->eraseFromParent();
    assert(!Continue->empty() &&
           "There should be at least a terminator after AI");
  }

  // Update the stats.
  ++NumTargetsPredicted;

  // Devirtualize the apply instruction on the identical path.
  auto NewInst =
      devirtualizeClassMethod(IdenAI, DownCastedClassInstance, CD, nullptr)
          .first;
  assert(NewInst && "Expected to be able to devirtualize apply!");
  (void)NewInst;

  deleteDevirtualizedApply(IdenAI);

  // Split critical edges resulting from VirtAI.
  if (auto *TAI = dyn_cast<TryApplyInst>(VirtAI)) {
    auto *ErrorBB = TAI->getFunction()->createBasicBlock();
    ErrorBB->createPhiArgument(TAI->getErrorBB()->getArgument(0)->getType(),
                               OwnershipKind::Owned);
    Builder.setInsertionPoint(ErrorBB);
    Builder.createBranch(TAI->getLoc(), TAI->getErrorBB(),
                         {ErrorBB->getArgument(0)});

    auto *NormalBB = TAI->getFunction()->createBasicBlock();
    NormalBB->createPhiArgument(TAI->getNormalBB()->getArgument(0)->getType(),
                                OwnershipKind::Owned);
    Builder.setInsertionPoint(NormalBB);
    Builder.createBranch(TAI->getLoc(), TAI->getNormalBB(),
                         {NormalBB->getArgument(0)});

    Builder.setInsertionPoint(VirtAI.getInstruction());
    SmallVector<SILValue, 4> Args;
    for (auto Arg : VirtAI.getArguments()) {
      Args.push_back(Arg);
    }
    FullApplySite NewVirtAI = Builder.createTryApply(
        VirtAI.getLoc(), VirtAI.getCallee(),
        VirtAI.getSubstitutionMap(),
        Args, NormalBB, ErrorBB,
        VirtAI.getApplyOptions());
    VirtAI.getInstruction()->eraseFromParent();
    VirtAI = NewVirtAI;
  }

  return VirtAI;
}

/// Returns true, if a method implementation to be called by the
/// default case handler of a speculative devirtualization is statically
/// known. This happens if it can be proven that generated
/// checked_cast_br instructions cover all other possible cases.
///
/// \p CHA class hierarchy analysis to be used
/// \p AI  invocation instruction
/// \p CD  static class of the instance whose method is being invoked
/// \p Subs set of direct subclasses of this class
static bool isDefaultCaseKnown(ClassHierarchyAnalysis *CHA,
                               FullApplySite AI,
                               ClassDecl *CD,
                               ClassHierarchyAnalysis::ClassList &Subs) {
  ClassMethodInst *CMI = cast<ClassMethodInst>(AI.getCallee());
  auto *Method = CMI->getMember().getAbstractFunctionDecl();
  assert(Method && "not a function");

  if (CD->isFinal())
    return true;

  // If the class has an @objc ancestry it can be dynamically subclassed and we
  // can't therefore statically know the default case.
  if (CD->checkAncestry(AncestryFlags::ObjC))
    return false;

  // Only handle classes defined within the SILModule's associated context.
  if (!CD->isChildContextOf(AI.getModule().getAssociatedContext()))
    return false;

  if (!CD->hasAccess())
    return false;

  // Only consider 'private' members, unless we are in whole-module compilation.
  switch (CD->getEffectiveAccess()) {
  case AccessLevel::Open:
    return false;
  case AccessLevel::Public:
  case AccessLevel::Package:
  case AccessLevel::Internal:
    if (!AI.getModule().isWholeModule())
      return false;
    break;
  case AccessLevel::FilePrivate:
  case AccessLevel::Private:
    break;
  }

  // This is a private or a module internal class.
  //
  // We can analyze the class hierarchy rooted at it and
  // eventually devirtualize a method call more efficiently.

  // First, analyze all direct subclasses.
  // We know that a dedicated checked_cast_br check is
  // generated for each direct subclass by tryToSpeculateTarget.
  for (auto S : Subs) {
    // Check if the subclass overrides a method
    auto *FD = S->findOverridingDecl(Method);
    if (!FD)
      continue;
    if (CHA->hasKnownDirectSubclasses(S)) {
      // This subclass has its own subclasses and
      // they will use this implementation or provide
      // their own. In either case it is not covered by
      // checked_cast_br instructions generated by
      // tryToSpeculateTarget. Therefore it increases
      // the number of remaining cases to be handled
      // by the default case handler.
      return false;
    }
  }

  // Then, analyze indirect subclasses.

  // Set of indirect subclasses for the class.
  auto &IndirectSubs = CHA->getIndirectSubClasses(CD);

  // Check if any indirect subclasses use an implementation
  // of the method different from the implementation in
  // the current class. If this is the case, then such
  // an indirect subclass would need a dedicated
  // checked_cast_br check to be devirtualized. But this is
  // not done by tryToSpeculateTarget yet and therefore
  // such a subclass should be handled by the "default"
  // case handler, which essentially means that "default"
  // case cannot be devirtualized since it covers more
  // then one alternative.
  for (auto S : IndirectSubs) {
    auto *ImplFD = S->findImplementingMethod(Method);
    if (ImplFD != Method) {
      // Different implementation is used by a subclass.
      // Therefore, the default case is not known.
      return false;
    }
  }

  return true;
}

/// Try to speculate the call target for the call \p AI. This function
/// returns true if a change was made.
static bool tryToSpeculateTarget(FullApplySite AI, ClassHierarchyAnalysis *CHA,
                                 OptRemark::Emitter &ORE) {
  ClassMethodInst *CMI = cast<ClassMethodInst>(AI.getCallee());

  // Strip any upcasts off of our 'self' value, potentially leaving us
  // with a value whose type is closer (in the class hierarchy) to the
  // actual dynamic type.
  auto SubTypeValue = stripUpCasts(CMI->getOperand());
  CanType SubType = SubTypeValue->getType().getASTType();

  // Bail if any generic types parameters of the class instance type are
  // unbound.
  // We cannot devirtualize unbound generic calls yet.
  if (SubType->hasArchetype())
    return false;

  auto *F = CMI->getFunction();
  auto &M = F->getModule();

  CheckedCastBranchInst *LastCCBI = nullptr;

  auto ClassType = getSelfInstanceType(SubType);
  ClassDecl *CD = ClassType.getClassOrBoundGenericClass();
  assert(CD && "Expected decl for class type!");

  if (!CHA->hasKnownDirectSubclasses(CD)) {
    // If there is only one possible alternative for this method,
    // try to devirtualize it completely.
    ClassHierarchyAnalysis::ClassList Subs;
    if (isDefaultCaseKnown(CHA, AI, CD, Subs)) {
      auto NewInst =
          tryDevirtualizeClassMethod(AI, SubTypeValue, CD, &ORE).first;
      if (NewInst)
        deleteDevirtualizedApply(AI);
      return bool(NewInst);
    }

    LLVM_DEBUG(llvm::dbgs() << "Inserting monomorphic speculative call for "
               "class " << CD->getName() << "\n");
    return !!speculateMonomorphicTarget(AI, SubType, CD, LastCCBI);
  }

  // True if any instructions were changed or generated.
  bool Changed = false;

  SmallVector<ClassDecl *, 8> Subs;
  getAllSubclasses(CHA, CD, ClassType, M, Subs);

  // Number of subclasses which cannot be handled by checked_cast_br checks.
  int NotHandledSubsNum = 0;
  if (Subs.size() > MaxNumSpeculativeTargets) {
    LLVM_DEBUG(llvm::dbgs() << "Class " << CD->getName() << " has too many ("
                            << Subs.size() << ") subclasses. Performing "
                              "speculative devirtualization only for the first "
                            << MaxNumSpeculativeTargets << " of them.\n");

    NotHandledSubsNum += (Subs.size() - MaxNumSpeculativeTargets);
    Subs.erase(&Subs[MaxNumSpeculativeTargets], Subs.end());
  }

  LLVM_DEBUG(llvm::dbgs() << "Class " << CD->getName() << " is a superclass. "
             "Inserting polymorphic speculative call.\n");

  // Try to devirtualize the static class of instance
  // if it is possible.
  if (auto F = getTargetClassMethod(M, CD, CMI)) {
    // Do not devirtualize if a method in the base class is marked
    // as non-optimizable. This way it is easy to disable the
    // devirtualization of this method in the base class and
    // any classes derived from it.
    if (!F->shouldOptimize())
      return false;
  }

  auto FirstAI = speculateMonomorphicTarget(AI, SubType, CD, LastCCBI);
  if (FirstAI) {
    Changed = true;
    AI = FirstAI;
  }

  // Perform a speculative devirtualization of a method invocation.
  // It replaces an indirect class_method-based call by a code to perform
  // a direct call of the method implementation based on the dynamic class
  // of the instance.
  //
  // The code is generated according to the following principles:
  //
  // - For each direct subclass, a dedicated checked_cast_br instruction
  // is generated to check if a dynamic class of the instance is exactly
  // this subclass.
  //
  // - If this check succeeds, then it jumps to the code which performs a
  // direct call of a method implementation specific to this subclass.
  //
  // - If this check fails, then a different subclass is checked by means of
  // checked_cast_br in a similar way.
  //
  // - Finally, if the instance does not exactly match any of the direct
  // subclasses, the "default" case code is generated, which should handle
  // all remaining alternatives, i.e. it should be able to dispatch to any
  // possible remaining method implementations. Typically this is achieved by
  // using a class_method instruction, which performs an indirect invocation.
  // But if it can be proven that only one specific implementation of
  // a method will be always invoked by this code, then a class_method-based
  // call can be devirtualized and replaced by a more efficient direct
  // invocation of this specific method implementation.
  //
  // Remark: With the current implementation of a speculative devirtualization,
  // if devirtualization of the "default" case is possible, then it would
  // by construction directly invoke the implementation of the method
  // corresponding to the static type of the instance. This may change
  // in the future, if we start using PGO for ordering of checked_cast_br
  // checks.

  // TODO: The ordering of checks may benefit from using a PGO, because
  // the most probable alternatives could be checked first.

  for (auto S : Subs) {
    LLVM_DEBUG(llvm::dbgs() << "Inserting a speculative call for class "
               << CD->getName() << " and subclass " << S->getName() << "\n");

    // FIXME: Add support for generic subclasses.
    if (S->isGenericContext()) {
      ++NotHandledSubsNum;
      continue;
    }

    CanType CanClassType = S->getDeclaredInterfaceType()->getCanonicalType();

    auto ClassOrMetatypeType = CanClassType;
    if (auto MT = dyn_cast<MetatypeType>(SubType)) {
      ClassOrMetatypeType = CanMetatypeType::get(CanClassType,
                                                 MT->getRepresentation());
    }

    // Pass the metatype of the subclass.
    auto NewAI = speculateMonomorphicTarget(AI, ClassOrMetatypeType, S, LastCCBI);
    if (!NewAI) {
      ++NotHandledSubsNum;
      continue;
    }
    AI = NewAI;
    Changed = true;
  }

  using namespace OptRemark;
  // Check if there is only a single statically known implementation
  // of the method which can be called by the default case handler.
  if (NotHandledSubsNum || !isDefaultCaseKnown(CHA, AI, CD, Subs)) {
    // Devirtualization of remaining cases is not possible,
    // because more than one implementation of the method
    // needs to be handled here. Thus, an indirect call through
    // the class_method cannot be eliminated completely.
    //
    if (Changed)
      ORE.emit([&]() {
        RemarkPassed R("PartialSpecDevirt", *AI.getInstruction());
        R << "Partially devirtualized call with run-time checks for "
          << NV("NumSubTypesChecked", Subs.size()) << " subclasses of "
          << NV("ClassType", ClassType);
        if (NotHandledSubsNum)
          R << ", number of subclasses not devirtualized: "
            << NV("NotHandledSubsNum", NotHandledSubsNum);
        if (!isDefaultCaseKnown(CHA, AI, CD, Subs))
          R << ", not all subclasses are known";
        return R;
      });
    return Changed;
  }

  auto RB = [&]() {
    return RemarkPassed("SpecDevirt", *AI.getInstruction())
           << "Devirtualized call with run-time checks for the derived classes "
              "of " << NV("ClassType", ClassType);
  };

  // At this point it is known that there is only one remaining method
  // implementation which is not covered by checked_cast_br checks yet.
  // So, it is safe to replace a class_method invocation by
  // a direct call of this remaining implementation.
  if (LastCCBI && SubTypeValue == LastCCBI->getOperand()) {
    // Remove last checked_cast_br, because it will always succeed.
    SILBuilderWithScope B(LastCCBI);
    auto CastedValue = B.createUncheckedReinterpretCast(
        LastCCBI->getLoc(), LastCCBI->getOperand(),
        LastCCBI->getTargetLoweredType());
    B.createBranch(LastCCBI->getLoc(), LastCCBI->getSuccessBB(), {CastedValue});
    LastCCBI->eraseFromParent();
    ORE.emit(RB);
    return true;
  }
  auto NewInst =
      tryDevirtualizeClassMethod(AI, SubTypeValue, CD, nullptr).first;
  if (NewInst) {
    ORE.emit(RB);
    deleteDevirtualizedApply(AI);
    return true;
  }

  if (Changed)
    ORE.emit(RB);
  return Changed;
}

namespace {
  /// Speculate the targets of virtual calls by assuming that the requested
  /// class is at the bottom of the class hierarchy.
  class SpeculativeDevirtualization : public SILFunctionTransform {
  public:
    ~SpeculativeDevirtualization() override {}

    void run() override {

      auto &CurFn = *getFunction();

      // Don't perform speculative devirtualization at -Os.
      if (CurFn.optimizeForSize())
        return;

      // Don't speculatively devirtualize calls inside thunks.
      if (CurFn.isThunk())
        return;

      ClassHierarchyAnalysis *CHA = PM->getAnalysis<ClassHierarchyAnalysis>();

      bool Changed = false;

      // Collect virtual calls that may be specialized.
      SmallVector<FullApplySite, 16> ToSpecialize;
      for (auto &BB : *getFunction()) {
        for (auto II = BB.begin(), IE = BB.end(); II != IE; ++II) {
          FullApplySite AI = FullApplySite::isa(&*II);
          if (AI && isa<ClassMethodInst>(AI.getCallee()))
            ToSpecialize.push_back(AI);
        }
      }

      OptRemark::Emitter ORE(DEBUG_TYPE, CurFn);
      // Go over the collected calls and try to insert speculative calls.
      for (auto AI : ToSpecialize)
        Changed |= tryToSpeculateTarget(AI, CHA, ORE);

      if (Changed) {
        CurFn.getModule().linkFunction(&CurFn, SILModule::LinkingMode::LinkAll);

        invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
      }
    }

  };

} // end anonymous namespace

SILTransform *swift::createSpeculativeDevirtualization() {
  return new SpeculativeDevirtualization();
}
