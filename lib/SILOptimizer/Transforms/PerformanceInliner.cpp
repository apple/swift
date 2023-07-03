//===--- PerformanceInliner.cpp - Basic cost based performance inlining ---===//
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

#define DEBUG_TYPE "sil-inliner"
#include "swift/AST/Module.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/SILOptimizer/Utils/PerformanceInlinerUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumFunctionsInlined, "Number of functions inlined");

llvm::cl::opt<bool> PrintShortestPathInfo(
    "print-shortest-path-info", llvm::cl::init(false),
    llvm::cl::desc("Print shortest-path information for inlining"));

llvm::cl::opt<bool> EnableSILInliningOfGenerics(
  "sil-inline-generics", llvm::cl::init(false),
  llvm::cl::desc("Enable inlining of generics"));

llvm::cl::opt<bool>
    EnableSILAggressiveInlining("sil-aggressive-inline", llvm::cl::init(false),
                               llvm::cl::desc("Enable aggressive inlining"));

llvm::cl::opt<bool> EnableVerifyAfterInlining(
    "sil-inline-verify-after-inline", llvm::cl::init(false),
    llvm::cl::desc("Run sil verification after inlining all found callee apply "
                   "sites into a caller."));

llvm::cl::opt<bool> SILPrintInliningCallee(
    "sil-print-inlining-callee", llvm::cl::init(false),
    llvm::cl::desc("Print functions that are inlined into other functions."));

llvm::cl::opt<bool> SILPrintInliningCallerBefore(
    "sil-print-inlining-caller-before", llvm::cl::init(false),
    llvm::cl::desc(
        "Print functions into which another function is about to be inlined."));

llvm::cl::opt<bool> SILPrintInliningCallerAfter(
    "sil-print-inlining-caller-after", llvm::cl::init(false),
    llvm::cl::desc(
        "Print functions into which another function has been inlined."));

llvm::cl::opt<bool> EnableVerifyAfterEachInlining(
    "sil-inline-verify-after-each-inline", llvm::cl::init(false),
    llvm::cl::desc(
        "Run sil verification after inlining each found callee apply "
        "site into a caller."));

//===----------------------------------------------------------------------===//
//                           Printing Helpers
//===----------------------------------------------------------------------===//

extern void printInliningDetailsCallee(StringRef passName, SILFunction *caller,
                                       SILFunction *callee);

extern void printInliningDetailsCallerBefore(StringRef passName,
                                             SILFunction *caller,
                                             SILFunction *callee);

extern void printInliningDetailsCallerAfter(StringRef passName,
                                            SILFunction *caller,
                                            SILFunction *callee);

//===----------------------------------------------------------------------===//
//                           Performance Inliner
//===----------------------------------------------------------------------===//

namespace {

using Weight = ShortestPathAnalysis::Weight;

class SILPerformanceInliner {
  StringRef PassName;
  SILOptFunctionBuilder &FuncBuilder;

  /// Specifies which functions not to inline, based on @_semantics and
  /// global_init attributes.
  InlineSelection WhatToInline;

  SILPassManager *pm;
  DominanceAnalysis *DA;
  SILLoopAnalysis *LA;
  BasicCalleeAnalysis *BCA;

  // For keys of SILFunction and SILLoop.
  llvm::DenseMap<SILFunction *, ShortestPathAnalysis *> SPAs;
  llvm::SpecificBumpPtrAllocator<ShortestPathAnalysis> SPAAllocator;

  ColdBlockInfo CBI;

  OptRemark::Emitter &ORE;

  /// The following constants define the cost model for inlining. Some constants
  /// are also defined in ShortestPathAnalysis.
  enum {
    /// The base value for every call: it represents the benefit of removing the
    /// call overhead itself.
    RemovedCallBenefit = 20,

    /// The benefit if the operand of an apply gets constant, e.g. if a closure
    /// is passed to an apply instruction in the callee.
    RemovedClosureBenefit = RemovedCallBenefit + 50,

    /// The benefit if a load can (probably) eliminated because it loads from
    /// a stack location in the caller.
    RemovedLoadBenefit = RemovedCallBenefit + 5,

    /// The benefit if a store can (probably) eliminated because it stores to
    /// a stack location in the caller.
    RemovedStoreBenefit = RemovedCallBenefit + 10,

    /// The benefit if the condition of a terminator instruction gets constant
    /// due to inlining.
    RemovedTerminatorBenefit = RemovedCallBenefit + 10,

    /// The benefit if a retain/release can (probably) be eliminated after
    /// inlining.
    RefCountBenefit = RemovedCallBenefit + 20,

    /// The benefit of a onFastPath builtin.
    FastPathBuiltinBenefit = RemovedCallBenefit + 40,

    /// The benefit of being able to devirtualize a call.
    DevirtualizedCallBenefit = RemovedCallBenefit + 300,

    /// The benefit of being able to produce a generic
    /// specialization for a call.
    GenericSpecializationBenefit = RemovedCallBenefit + 300,

    /// The benefit of inlining an exclusivity-containing callee.
    /// The exclusivity needs to be: dynamic,
    /// has no nested conflict and addresses known storage
    ExclusivityBenefit = RemovedCallBenefit + 10,

    /// The benefit of inlining class methods with -Osize.
    /// We only inline very small class methods with -Osize.
    OSizeClassMethodBenefit = 5,

    /// Approximately up to this cost level a function can be inlined without
    /// increasing the code size.
    TrivialFunctionThreshold = 18,

    /// Configuration for the "soft" caller block limit. When changing, make
    /// sure you update BlockLimitMaxIntNumerator.
    BlockLimitDenominator = 3000,

    /// Computations with BlockLimitDenominator will overflow with numerators
    /// >= this value. This equals cbrt(INT_MAX) * cbrt(BlockLimitDenominator);
    /// we hardcode its value because std::cbrt() is not constexpr.
    BlockLimitMaxIntNumerator = 18608,

    /// No inlining is done if the caller has more than this number of blocks.
    OverallCallerBlockLimit = 400,

    /// The assumed execution length of a function call.
    DefaultApplyLength = 10
  };

  OptimizationMode OptMode;

#ifndef NDEBUG
  SILFunction *LastPrintedCaller = nullptr;
  void dumpCaller(SILFunction *Caller) {
    if (Caller != LastPrintedCaller) {
      llvm::dbgs() << "\nInline into caller: " << Caller->getName() << '\n';
      LastPrintedCaller = Caller;
    }
  }
#endif

  ShortestPathAnalysis *getSPA(SILFunction *F, SILLoopInfo *LI) {
    ShortestPathAnalysis *&SPA = SPAs[F];
    if (!SPA) {
      SPA = new (SPAAllocator.Allocate()) ShortestPathAnalysis(F, LI);
    }
    return SPA;
  }

  bool profileBasedDecision(
      const FullApplySite &AI, int Benefit, SILFunction *Callee, int CalleeCost,
      int &NumCallerBlocks,
      const llvm::DenseMapIterator<
          swift::SILBasicBlock *, uint64_t,
          llvm::DenseMapInfo<swift::SILBasicBlock *>,
          llvm::detail::DenseMapPair<swift::SILBasicBlock *, uint64_t>, true>
          &bbIt);

  bool isProfitableToInline(
      FullApplySite AI, Weight CallerWeight, ConstantTracker &callerTracker,
      int &NumCallerBlocks,
      const llvm::DenseMap<SILBasicBlock *, uint64_t> &BBToWeightMap);

  bool decideInWarmBlock(
      FullApplySite AI, Weight CallerWeight, ConstantTracker &callerTracker,
      int &NumCallerBlocks,
      const llvm::DenseMap<SILBasicBlock *, uint64_t> &BBToWeightMap);

  bool decideInColdBlock(FullApplySite AI, SILFunction *Callee, int numCallerBlocks);

  void visitColdBlocks(SmallVectorImpl<FullApplySite> &AppliesToInline,
                       SILBasicBlock *root, DominanceInfo *DT, int numCallerBlocks);

  void collectAppliesToInline(SILFunction *Caller,
                              SmallVectorImpl<FullApplySite> &Applies);

public:
  SILPerformanceInliner(StringRef PassName, SILOptFunctionBuilder &FuncBuilder,
                        InlineSelection WhatToInline,
                        SILPassManager *pm, DominanceAnalysis *DA,
                        SILLoopAnalysis *LA, BasicCalleeAnalysis *BCA,
                        OptimizationMode OptMode, OptRemark::Emitter &ORE)
      : PassName(PassName), FuncBuilder(FuncBuilder),
        WhatToInline(WhatToInline), pm(pm), DA(DA), LA(LA), BCA(BCA), CBI(DA), ORE(ORE),
        OptMode(OptMode) {}

  bool inlineCallsIntoFunction(SILFunction *F);
};

} // end anonymous namespace

// Returns true if it is possible to perform a generic
// specialization for a given call.
static bool canSpecializeGeneric(ApplySite AI, SILFunction *F,
                                 SubstitutionMap Subs) {
  return ReabstractionInfo::canBeSpecialized(AI, F, Subs);
}

bool SILPerformanceInliner::profileBasedDecision(
    const FullApplySite &AI, int Benefit, SILFunction *Callee, int CalleeCost,
    int &NumCallerBlocks,
    const llvm::DenseMapIterator<
        swift::SILBasicBlock *, uint64_t,
        llvm::DenseMapInfo<swift::SILBasicBlock *>,
        llvm::detail::DenseMapPair<swift::SILBasicBlock *, uint64_t>, true>
        &bbIt) {
  if (CalleeCost < TrivialFunctionThreshold) {
    // We do not increase code size below this threshold
    return true;
  }
  auto callerCount = bbIt->getSecond();
  if (callerCount < 1) {
    // Never called - do not inline
    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "profiled decision: NO, "
                               "reason= Never Called.\n");
    return false;
  }
  auto calleeCount = Callee->getEntryCount();
  if (calleeCount) {
    // If we have Callee count - use SI heuristic:
    auto calleCountVal = calleeCount.getValue();
    auto percent = (long double)callerCount / (long double)calleCountVal;
    if (percent < 0.8) {
      LLVM_DEBUG(dumpCaller(AI.getFunction());
                 llvm::dbgs() << "profiled decision: NO, reason=SI "
                              << std::to_string(percent) << "%\n");
      return false;
    }
    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "profiled decision: YES, reason=SI "
                            << std::to_string(percent) << "%\n");
  } else {
    // No callee count - use a "modified" aggressive IHF for now
    if (CalleeCost > Benefit && callerCount < 100) {
      LLVM_DEBUG(dumpCaller(AI.getFunction());
                 llvm::dbgs() << "profiled decision: NO, reason=IHF "
                              << callerCount << '\n');
      return false;
    }
    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "profiled decision: YES, reason=IHF "
                            << callerCount << '\n');
  }
  // We're gonna inline!
  NumCallerBlocks += Callee->size();
  return true;
}

bool SILPerformanceInliner::isProfitableToInline(
    FullApplySite AI, Weight CallerWeight, ConstantTracker &callerTracker,
    int &NumCallerBlocks,
    const llvm::DenseMap<SILBasicBlock *, uint64_t> &BBToWeightMap) {
  SILFunction *Callee = AI.getReferencedFunctionOrNull();
  assert(Callee);
  bool IsGeneric = AI.hasSubstitutions();

  // Start with a base benefit.
  int BaseBenefit = RemovedCallBenefit;

  // Osize heuristic.
  //
  // As a hack, don't apply this at all to coroutine inlining; avoiding
  // coroutine allocation overheads is extremely valuable.  There might be
  // more principled ways of getting this effect.
  bool isClassMethodAtOsize = false;
  if (OptMode == OptimizationMode::ForSize && !isa<BeginApplyInst>(AI)) {
    // Don't inline into thunks.
    if (AI.getFunction()->isThunk())
      return false;

    // Don't inline class methods.
    if (Callee->hasSelfParam()) {
      auto SelfTy = Callee->getLoweredFunctionType()->getSelfInstanceType(
          FuncBuilder.getModule(), AI.getFunction()->getTypeExpansionContext());
      if (SelfTy->mayHaveSuperclass() &&
          Callee->getRepresentation() == SILFunctionTypeRepresentation::Method)
        isClassMethodAtOsize = true;
    }
    // Use command line option to control inlining in Osize mode.
    const uint64_t CallerBaseBenefitReductionFactor = AI.getFunction()->getModule().getOptions().CallerBaseBenefitReductionFactor;
    BaseBenefit = BaseBenefit / CallerBaseBenefitReductionFactor;
  }

  // It is always OK to inline a simple call.
  // TODO: May be consider also the size of the callee?
  if (isPureCall(AI, BCA)) {
    OptRemark::Emitter::emitOrDebug(DEBUG_TYPE, &ORE, [&]() {
      using namespace OptRemark;
      return RemarkPassed("Inline", *AI.getInstruction())
             << "Pure call. Always profitable to inline "
             << NV("Callee", Callee);
    });

    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "    pure-call decision " << Callee->getName()
                            << '\n');
    return true;
  }

  // Bail out if this generic call can be optimized by means of
  // the generic specialization, because we prefer generic specialization
  // to inlining of generics.
  if (IsGeneric && canSpecializeGeneric(AI, Callee, AI.getSubstitutionMap())) {
    return false;
  }

  // Bail out if this is a generic call of a `@_specialize(exported:)` function
  // and we are in the early inliner. We want to give the generic specializer
  // the opportunity to see specialized call sites.
  if (IsGeneric && WhatToInline == InlineSelection::NoSemanticsAndEffects  &&
      Callee->hasPrespecialization()) {
    return false;
  }

  SILLoopInfo *LI = LA->get(Callee);
  ShortestPathAnalysis *SPA = getSPA(Callee, LI);
  assert(SPA->isValid());

  ConstantTracker constTracker(Callee, &callerTracker, AI);
  DominanceInfo *DT = DA->get(Callee);
  SILBasicBlock *CalleeEntry = &Callee->front();
  DominanceOrder domOrder(CalleeEntry, DT, Callee->size());

  // We don't want to blow up code-size
  // We will only inline if *ALL* dynamic accesses are
  // known and have no nested conflict
  bool AllAccessesBeneficialToInline = true;
  bool returnsAllocation = false;

  // Calculate the inlining cost of the callee.
  int CalleeCost = 0;
  int Benefit = 0;
  // We don’t know if we want to update the benefit with
  // the exclusivity heuristic or not. We can *only* do that
  // if AllAccessesBeneficialToInline is true
  int ExclusivityBenefitWeight = 0;
  int ExclusivityBenefitBase = ExclusivityBenefit;
  if (EnableSILAggressiveInlining) {
    ExclusivityBenefitBase += 500;
  }

  SubstitutionMap CalleeSubstMap = AI.getSubstitutionMap();

  CallerWeight.updateBenefit(Benefit, BaseBenefit);

  // Go through all blocks of the function, accumulate the cost and find
  // benefits.
  while (SILBasicBlock *block = domOrder.getNext()) {
    constTracker.beginBlock();
    Weight BlockW = SPA->getWeight(block, CallerWeight);

    for (SILInstruction &I : *block) {
      constTracker.trackInst(&I);

      CalleeCost += (int)instructionInlineCost(I);

      if (FullApplySite FAI = FullApplySite::isa(&I)) {
        // Check if the callee is passed as an argument. If so, increase the
        // threshold, because inlining will (probably) eliminate the closure.
        SILInstruction *def = constTracker.getDefInCaller(FAI.getCallee());
        if (def && (isa<FunctionRefInst>(def) || isa<PartialApplyInst>(def)))
          BlockW.updateBenefit(Benefit, RemovedClosureBenefit);
        // Check if inlining the callee would allow for further
        // optimizations like devirtualization or generic specialization. 
        if (!def)
          def = dyn_cast_or_null<SingleValueInstruction>(FAI.getCallee());

        if (!def)
          continue;

        auto Subs = FAI.getSubstitutionMap();

        // Bail if it is not a generic call or inlining of generics is forbidden.
        if (!EnableSILInliningOfGenerics || !Subs.hasAnySubstitutableParams())
          continue;

        if (!isa<FunctionRefInst>(def) && !isa<ClassMethodInst>(def) &&
            !isa<WitnessMethodInst>(def))
          continue;

        // It is a generic call inside the callee. Check if after inlining
        // it will be possible to perform a generic specialization or
        // devirtualization of this call.

        // Create the list of substitutions as they will be after
        // inlining.
        auto SubMap = Subs.subst(CalleeSubstMap);

        // Check if the call can be devirtualized.
        if (isa<ClassMethodInst>(def) || isa<WitnessMethodInst>(def) ||
            isa<SuperMethodInst>(def)) {
          // TODO: Take AI.getSubstitutions() into account.
          if (canDevirtualizeApply(FAI, nullptr)) {
            LLVM_DEBUG(llvm::dbgs() << "Devirtualization will be possible "
                                       "after inlining for the call:\n";
                       FAI.getInstruction()->dumpInContext());
            BlockW.updateBenefit(Benefit, DevirtualizedCallBenefit);
          }
        }

        // Check if a generic specialization would be possible.
        if (isa<FunctionRefInst>(def)) {
          auto CalleeF = FAI.getCalleeFunction();
          if (!canSpecializeGeneric(FAI, CalleeF, SubMap))
            continue;
          LLVM_DEBUG(llvm::dbgs() << "Generic specialization will be possible "
                                     "after inlining for the call:\n";
                     FAI.getInstruction()->dumpInContext());
          BlockW.updateBenefit(Benefit, GenericSpecializationBenefit);
        }
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        // Check if it's a load from a stack location in the caller. Such a load
        // might be optimized away if inlined.
        if (constTracker.isStackAddrInCaller(LI->getOperand()))
          BlockW.updateBenefit(Benefit, RemovedLoadBenefit);
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        // Check if it's a store to a stack location in the caller. Such a load
        // might be optimized away if inlined.
        if (constTracker.isStackAddrInCaller(SI->getDest()))
          BlockW.updateBenefit(Benefit, RemovedStoreBenefit);
      } else if (isa<StrongReleaseInst>(&I) || isa<ReleaseValueInst>(&I)) {
        SILValue Op = stripCasts(I.getOperand(0));
        if (auto *Arg = dyn_cast<SILFunctionArgument>(Op)) {
          if (Arg->getArgumentConvention() ==
              SILArgumentConvention::Direct_Guaranteed) {
            BlockW.updateBenefit(Benefit, RefCountBenefit);
          }
        }
      } else if (auto *BI = dyn_cast<BuiltinInst>(&I)) {
        if (BI->getBuiltinInfo().ID == BuiltinValueKind::OnFastPath)
          BlockW.updateBenefit(Benefit, FastPathBuiltinBenefit);
      } else if (auto *BAI = dyn_cast<BeginAccessInst>(&I)) {
        if (BAI->getEnforcement() == SILAccessEnforcement::Dynamic) {
          // The access is dynamic and has no nested conflict
          // See if the storage location is considered by
          // access enforcement optimizations
          auto storage = AccessStorage::compute(BAI->getSource());
          if (BAI->hasNoNestedConflict() && (storage.isFormalAccessBase())) {
            BlockW.updateBenefit(ExclusivityBenefitWeight,
                                 ExclusivityBenefitBase);
          } else {
            AllAccessesBeneficialToInline = false;
          }
        }
      } else if (auto ri = dyn_cast<ReturnInst>(&I)) {
        SILValue retVal = ri->getOperand();
        if (auto *uci = dyn_cast<UpcastInst>(retVal))
          retVal = uci->getOperand();

        // Inlining functions which return an allocated object or partial_apply
        // most likely has a benefit in the caller, because e.g. it can enable
        // de-virtualization.
        if (isa<AllocationInst>(retVal) || isa<PartialApplyInst>(retVal)) {
          BlockW.updateBenefit(Benefit, RemovedCallBenefit + 10);
          returnsAllocation = true;
        }
      }
    }
    // Don't count costs in blocks which are dead after inlining.
    SILBasicBlock *takenBlock = constTracker.getTakenBlock(block->getTerminator());
    if (takenBlock) {
      BlockW.updateBenefit(Benefit, RemovedTerminatorBenefit);
      domOrder.pushChildrenIf(block, [=](SILBasicBlock *child) {
        return child->getSinglePredecessorBlock() != block ||
               child == takenBlock;
      });
    } else {
      domOrder.pushChildren(block);
    }
  }

  if (AllAccessesBeneficialToInline) {
    Benefit = std::max(Benefit, ExclusivityBenefitWeight);
  }

  if (AI.getFunction()->isThunk()) {
    // Only inline trivial functions into thunks (which will not increase the
    // code size).
    if (CalleeCost > TrivialFunctionThreshold) {
      return false;
    }

    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "    decision {" << CalleeCost << " into thunk} "
                            << Callee->getName() << '\n');
    return true;
  }

  // We reduce the benefit if the caller is too large. For this we use a
  // cubic function on the number of caller blocks. This starts to prevent
  // inlining at about 800 - 1000 caller blocks.
  if (NumCallerBlocks < BlockLimitMaxIntNumerator)
    Benefit -= 
      (NumCallerBlocks * NumCallerBlocks) / BlockLimitDenominator *
                          NumCallerBlocks / BlockLimitDenominator;
  else
    // The calculation in the if branch would overflow if we performed it.
    Benefit = 0;

  // If we have profile info - use it for final inlining decision.
  auto *bb = AI.getInstruction()->getParent();
  auto bbIt = BBToWeightMap.find(bb);
  if (bbIt != BBToWeightMap.end()) {
    if (profileBasedDecision(AI, Benefit, Callee, CalleeCost, NumCallerBlocks,
                             bbIt)) {
      OptRemark::Emitter::emitOrDebug(DEBUG_TYPE, &ORE, [&]() {
        using namespace OptRemark;
        return RemarkPassed("Inline", *AI.getInstruction())
               << "Profitable due to provided profile";
      });
      return true;
    }

    OptRemark::Emitter::emitOrDebug(DEBUG_TYPE, &ORE, [&]() {
      using namespace OptRemark;
      return RemarkMissed("Inline", *AI.getInstruction())
             << "Not profitable due to provided profile";
    });
    return false;
  }

  if (isClassMethodAtOsize && Benefit > OSizeClassMethodBenefit) {
    Benefit = OSizeClassMethodBenefit;
    if (returnsAllocation)
      Benefit += 10;
  }

  // This is the final inlining decision.
  if (CalleeCost > Benefit) {
    OptRemark::Emitter::emitOrDebug(DEBUG_TYPE, &ORE, [&]() {
      using namespace OptRemark;
      return RemarkMissed("Inline", *AI.getInstruction())
             << "Not profitable to inline function " << NV("Callee", Callee)
             << " (cost = " << NV("Cost", CalleeCost)
             << ", benefit = " << NV("Benefit", Benefit) << ")";
    });
    return false;
  }

  NumCallerBlocks += Callee->size();

  LLVM_DEBUG(dumpCaller(AI.getFunction());
             llvm::dbgs() << "    decision {c=" << CalleeCost
                          << ", b=" << Benefit
                          << ", l=" << SPA->getScopeLength(CalleeEntry, 0)
                          << ", c-w=" << CallerWeight
                          << ", bb=" << Callee->size()
                          << ", c-bb=" << NumCallerBlocks
                          << "} " << Callee->getName() << '\n');
  OptRemark::Emitter::emitOrDebug(DEBUG_TYPE, &ORE, [&]() {
    using namespace OptRemark;
    return RemarkPassed("Inlined", *AI.getInstruction())
           << NV("Callee", Callee) << " inlined into "
           << NV("Caller", AI.getFunction())
           << " (cost = " << NV("Cost", CalleeCost)
           << ", benefit = " << NV("Benefit", Benefit) << ")";
  });

  return true;
}

static bool returnsClosure(SILFunction *F) {
  for (SILBasicBlock &BB : *F) {
    if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
      return isa<PartialApplyInst>(RI->getOperand());
    }
  }
  return false;
}

static bool hasMaxNumberOfBasicBlocks(SILFunction *f, int limit) {
  for (SILBasicBlock &block : *f) {
    (void)block;
    if (limit == 0)
      return false;
    limit--;
  }
  return true;
}

static bool isInlineAlwaysCallSite(SILFunction *Callee, int numCallerBlocks) {
  if (Callee->isTransparent())
    return true;
  if (Callee->getInlineStrategy() == AlwaysInline &&
      !Callee->getModule().getOptions().IgnoreAlwaysInline &&

      // Protect against misuse of @inline(__always).
      // Inline-always should only be used on relatively small functions.
      // It must not be used on recursive functions. This check prevents that
      // the compiler blows up if @inline(__always) is put on a recursive function.
      (numCallerBlocks < 64 || hasMaxNumberOfBasicBlocks(Callee, 64))) {
    return true;
  }
  return false;
}

/// Checks if a given generic apply should be inlined unconditionally, i.e.
/// without any complex analysis using e.g. a cost model.
/// It returns true if a function should be inlined.
/// It returns false if a function should not be inlined.
/// It returns None if the decision cannot be made without a more complex
/// analysis.
static llvm::Optional<bool> shouldInlineGeneric(FullApplySite AI,
                                                int numCallerBlocks) {
  assert(AI.hasSubstitutions() &&
         "Expected a generic apply");

  SILFunction *Callee = AI.getReferencedFunctionOrNull();

  // Do not inline @_semantics functions when compiling the stdlib,
  // because they need to be preserved, so that the optimizer
  // can properly optimize a user code later.
  ModuleDecl *SwiftModule = Callee->getModule().getSwiftModule();
  if (Callee->hasSemanticsAttrThatStartsWith("array.") &&
      (SwiftModule->isStdlibModule() || SwiftModule->isOnoneSupportModule()))
    return false;

  // Do not inline into thunks.
  if (AI.getFunction()->isThunk())
    return false;

  // Always inline generic functions which are marked as
  // AlwaysInline or transparent.
  if (isInlineAlwaysCallSite(Callee, numCallerBlocks))
    return true;

  // If all substitutions are concrete, then there is no need to perform the
  // generic inlining. Let the generic specializer create a specialized
  // function and then decide if it is beneficial to inline it.
  if (!AI.getSubstitutionMap().hasArchetypes())
    return false;

  if (Callee->getLoweredFunctionType()->getCoroutineKind() !=
      SILCoroutineKind::None) {
    // Co-routines are so expensive (e.g. Array.subscript.read) that we always
    // enable inlining them in a generic context. Though the final inlining
    // decision is done by the usual heuristics. Therefore we return None and
    // not true.
    return llvm::None;
  }

  // The returned partial_apply of a thunk is most likely being optimized away
  // if inlined. Because some thunks cannot be specialized (e.g. if an opened
  // existential is in the substitution list), we inline such thunks also in case
  // they are generic.
  if (Callee->isThunk() && returnsClosure(Callee))
    return true;

  // All other generic functions should not be inlined if this kind of inlining
  // is disabled.
  if (!EnableSILInliningOfGenerics)
    return false;

  // It is not clear yet if this function should be decided or not.
  return llvm::None;
}

bool SILPerformanceInliner::decideInWarmBlock(
    FullApplySite AI, Weight CallerWeight, ConstantTracker &callerTracker,
    int &NumCallerBlocks,
    const llvm::DenseMap<SILBasicBlock *, uint64_t> &BBToWeightMap) {
  if (AI.hasSubstitutions()) {
    // Only inline generics if definitively clear that it should be done.
    auto ShouldInlineGeneric = shouldInlineGeneric(AI, NumCallerBlocks);
    if (ShouldInlineGeneric.has_value())
      return ShouldInlineGeneric.value();
  }

  SILFunction *Callee = AI.getReferencedFunctionOrNull();

  if (isInlineAlwaysCallSite(Callee, NumCallerBlocks)) {
    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "    always-inline decision "
                            << Callee->getName() << '\n');
    return true;
  }

  return isProfitableToInline(AI, CallerWeight, callerTracker, NumCallerBlocks,
                              BBToWeightMap);
}

/// Return true if inlining this call site into a cold block is profitable.
bool SILPerformanceInliner::decideInColdBlock(FullApplySite AI,
                                              SILFunction *Callee, int numCallerBlocks) {
  if (AI.hasSubstitutions()) {
    // Only inline generics if definitively clear that it should be done.
    auto ShouldInlineGeneric = shouldInlineGeneric(AI, numCallerBlocks);
    if (ShouldInlineGeneric.has_value())
      return ShouldInlineGeneric.value();

    return false;
  }

  if (isInlineAlwaysCallSite(Callee, numCallerBlocks)) {
    LLVM_DEBUG(dumpCaller(AI.getFunction());
               llvm::dbgs() << "    always-inline decision "
                            << Callee->getName() << '\n');
    return true;
  }

  int CalleeCost = 0;

  for (SILBasicBlock &Block : *Callee) {
    for (SILInstruction &I : Block) {
      CalleeCost += int(instructionInlineCost(I));
      if (CalleeCost > TrivialFunctionThreshold)
        return false;
    }
  }
  LLVM_DEBUG(dumpCaller(AI.getFunction());
             llvm::dbgs() << "    cold decision {" << CalleeCost << "} "
                          << Callee->getName() << '\n');
  return true;
}

/// Record additional weight increases.
///
/// Why can't we just add the weight when we call isProfitableToInline? Because
/// the additional weight is for _another_ function than the current handled
/// callee.
static void addWeightCorrection(FullApplySite FAS,
                        llvm::DenseMap<FullApplySite, int> &WeightCorrections) {
  SILFunction *Callee = FAS.getReferencedFunctionOrNull();
  if (Callee && Callee->hasSemanticsAttr(semantics::ARRAY_UNINITIALIZED)) {
    // We want to inline the argument to an array.uninitialized call, because
    // this argument is most likely a call to a function which contains the
    // buffer allocation for the array. It is essential to inline it for stack
    // promotion of the array buffer.
    SILValue BufferArg = FAS.getArgument(0);
    SILValue Base = stripValueProjections(stripCasts(BufferArg));
    if (auto BaseApply = FullApplySite::isa(Base))
      WeightCorrections[BaseApply] += 6;
  }
}

static bool containsWeight(TermInst *inst) {
  for (auto &succ : inst->getSuccessors()) {
    if (succ.getCount()) {
      return true;
    }
  }
  return false;
}

static void
addToBBCounts(llvm::DenseMap<SILBasicBlock *, uint64_t> &BBToWeightMap,
              uint64_t numToAdd, swift::TermInst *termInst) {
  for (auto &succ : termInst->getSuccessors()) {
    auto *currBB = succ.getBB();
    assert(BBToWeightMap.find(currBB) != BBToWeightMap.end() &&
           "Expected to find block in map");
    BBToWeightMap[currBB] += numToAdd;
  }
}

static void
calculateBBWeights(SILFunction *Caller, DominanceInfo *DT,
                   llvm::DenseMap<SILBasicBlock *, uint64_t> &BBToWeightMap) {
  auto entryCount = Caller->getEntryCount();
  if (!entryCount) {
    // No profile for function - return
    return;
  }
  // Add all blocks to BBToWeightMap without count 0
  for (auto &block : *Caller) {
    BBToWeightMap[&block] = 0;
  }
  BBToWeightMap[Caller->getEntryBlock()] = entryCount.getValue();
  DominanceOrder domOrder(&Caller->front(), DT, Caller->size());
  while (SILBasicBlock *block = domOrder.getNext()) {
    auto bbIt = BBToWeightMap.find(block);
    assert(bbIt != BBToWeightMap.end() && "Expected to find block in map");
    auto bbCount = bbIt->getSecond();
    auto *termInst = block->getTerminator();
    if (containsWeight(termInst)) {
      // Instruction already contains accurate counters - use them as-is
      uint64_t countSum = 0;
      uint64_t blocksWithoutCount = 0;
      for (auto &succ : termInst->getSuccessors()) {
        auto *currBB = succ.getBB();
        assert(BBToWeightMap.find(currBB) != BBToWeightMap.end() &&
               "Expected to find block in map");
        auto currCount = succ.getCount();
        if (!currCount) {
          ++blocksWithoutCount;
          continue;
        }
        auto currCountVal = currCount.getValue();
        countSum += currCountVal;
        BBToWeightMap[currBB] += currCountVal;
      }
      if (countSum < bbCount) {
        // inaccurate profile - fill in the gaps for BBs without a count:
        if (blocksWithoutCount > 0) {
          auto numToAdd = (bbCount - countSum) / blocksWithoutCount;
          for (auto &succ : termInst->getSuccessors()) {
            auto *currBB = succ.getBB();
            auto currCount = succ.getCount();
            if (!currCount) {
              BBToWeightMap[currBB] += numToAdd;
            }
          }
        }
      } else {
        auto numOfSucc = termInst->getSuccessors().size();
        assert(numOfSucc > 0 && "Expected successors > 0");
        auto numToAdd = (countSum - bbCount) / numOfSucc;
        addToBBCounts(BBToWeightMap, numToAdd, termInst);
      }
    } else {
      // Fill counters speculatively
      auto numOfSucc = termInst->getSuccessors().size();
      if (numOfSucc == 0) {
        // No successors to fill
        continue;
      }
      auto numToAdd = bbCount / numOfSucc;
      addToBBCounts(BBToWeightMap, numToAdd, termInst);
    }
    domOrder.pushChildrenIf(block, [&](SILBasicBlock *child) { return true; });
  }
}

void SILPerformanceInliner::collectAppliesToInline(
    SILFunction *Caller, SmallVectorImpl<FullApplySite> &Applies) {
  DominanceInfo *DT = DA->get(Caller);
  SILLoopInfo *LI = LA->get(Caller);

  llvm::DenseMap<FullApplySite, int> WeightCorrections;

  // Compute the shortest-path analysis for the caller.
  ShortestPathAnalysis *SPA = getSPA(Caller, LI);
  SPA->analyze(CBI, [&](FullApplySite FAS) -> int {
  
    // This closure returns the length of a called function.

    // At this occasion we record additional weight increases.
    addWeightCorrection(FAS, WeightCorrections);

    if (SILFunction *Callee = getEligibleFunction(FAS, WhatToInline)) {
      // Compute the shortest-path analysis for the callee.
      SILLoopInfo *CalleeLI = LA->get(Callee);
      ShortestPathAnalysis *CalleeSPA = getSPA(Callee, CalleeLI);
      if (!CalleeSPA->isValid()) {
        CalleeSPA->analyze(CBI, [](FullApplySite FAS) {
          // We don't compute SPA for another call-level. Functions called from
          // the callee are assumed to have DefaultApplyLength.
          return DefaultApplyLength;
        });
      }
      int CalleeLength = CalleeSPA->getScopeLength(&Callee->front(), 0);
      // Just in case the callee is a noreturn function.
      if (CalleeLength >= ShortestPathAnalysis::InitialDist)
        return DefaultApplyLength;
      return CalleeLength;
    }
    // Some unknown function.
    return DefaultApplyLength;
  });

#ifndef NDEBUG
  if (PrintShortestPathInfo) {
    SPA->dump();
  }
#endif

  ConstantTracker constTracker(Caller);
  DominanceOrder domOrder(&Caller->front(), DT, Caller->size());
  int NumCallerBlocks = (int)Caller->size();

  llvm::DenseMap<SILBasicBlock *, uint64_t> BBToWeightMap;
  calculateBBWeights(Caller, DT, BBToWeightMap);

  // Go through all instructions and find candidates for inlining.
  // We do this in dominance order for the constTracker.
  SmallVector<FullApplySite, 8> InitialCandidates;
  while (SILBasicBlock *block = domOrder.getNext()) {
    constTracker.beginBlock();
    Weight BlockWeight;

    for (auto I = block->begin(), E = block->end(); I != E; ++I) {
      constTracker.trackInst(&*I);

      if (!FullApplySite::isa(&*I))
        continue;

      pm->setDependingOnCalleeBodies();

      FullApplySite AI = FullApplySite(&*I);

      auto *Callee = getEligibleFunction(AI, WhatToInline);
      if (Callee) {
        // Check if we have an always_inline or transparent function. If we do,
        // just add it to our final Applies list and continue.
        if (isInlineAlwaysCallSite(Callee, NumCallerBlocks)) {
          NumCallerBlocks += Callee->size();
          Applies.push_back(AI);
          continue;
        }

        // Next make sure that we do not have more blocks than our overall
        // caller block limit at this point. In such a case, we continue. This
        // will ensure that any further non inline always functions are skipped,
        // but we /do/ inline any inline_always functions remaining.
        if (NumCallerBlocks > OverallCallerBlockLimit)
          continue;

        // Otherwise, calculate our block weights and determine if we want to
        // inline this.
        if (!BlockWeight.isValid())
          BlockWeight = SPA->getWeight(block, Weight(0, 0));

        // The actual weight including a possible weight correction.
        Weight W(BlockWeight, WeightCorrections.lookup(AI));

        if (decideInWarmBlock(AI, W, constTracker, NumCallerBlocks,
                              BBToWeightMap))
          InitialCandidates.push_back(AI);
      }
    }

    domOrder.pushChildrenIf(block, [&] (SILBasicBlock *child) {
      if (CBI.isSlowPath(block, child)) {
        // Handle cold blocks separately.
        visitColdBlocks(InitialCandidates, child, DT, NumCallerBlocks);
        return false;
      }
      return true;
    });
  }

  // Calculate how many times a callee is called from this caller.
  llvm::DenseMap<SILFunction *, unsigned> CalleeCount;
  for (auto AI : InitialCandidates) {
    SILFunction *Callee = AI.getReferencedFunctionOrNull();
    assert(Callee && "apply_inst does not have a direct callee anymore");
    ++CalleeCount[Callee];
  }

  // Now copy each candidate callee that has a small enough number of
  // call sites into the final set of call sites.
  for (auto AI : InitialCandidates) {
    SILFunction *Callee = AI.getReferencedFunctionOrNull();
    assert(Callee && "apply_inst does not have a direct callee anymore");

    const unsigned CallsToCalleeThreshold = 1024;
    if (CalleeCount[Callee] <= CallsToCalleeThreshold) {
      Applies.push_back(AI);
    }
  }
}

/// Attempt to inline all calls smaller than our threshold.
/// returns True if a function was inlined.
bool SILPerformanceInliner::inlineCallsIntoFunction(SILFunction *Caller) {
  // Don't optimize functions that are marked with the opt.never attribute.
  if (!Caller->shouldOptimize())
    return false;

  // First step: collect all the functions we want to inline.  We
  // don't change anything yet so that the dominator information
  // remains valid.
  SmallVector<FullApplySite, 8> AppliesToInline;
  collectAppliesToInline(Caller, AppliesToInline);
  bool invalidatedStackNesting = false;

  if (AppliesToInline.empty())
    return false;

  InstructionDeleter deleter;

  // Second step: do the actual inlining.
  // We inline in reverse order, because for very large blocks with many applies
  // to inline, splitting the block at every apply would be quadratic.
  for (auto AI : llvm::reverse(AppliesToInline)) {
    SILFunction *Callee = AI.getReferencedFunctionOrNull();
    assert(Callee && "apply_inst does not have a direct callee anymore");

    if (!Callee->shouldOptimize()) {
      continue;
    }

    // If we have a callee that doesn't have ownership, but the caller does have
    // ownership... do not inline. The two modes are incompatible, so skip this
    // apply site for now.
    if (!Callee->hasOwnership() && Caller->hasOwnership()) {
      continue;
    }

    LLVM_DEBUG(dumpCaller(Caller); llvm::dbgs()
                                   << "    inline [" << Callee->size() << "->"
                                   << Caller->size() << "] "
                                   << Callee->getName() << "\n");

    // Note that this must happen before inlining as the apply instruction
    // will be deleted after inlining.
    invalidatedStackNesting |= SILInliner::invalidatesStackNesting(AI);

    if (SILPrintInliningCallee) {
      printInliningDetailsCallee(PassName, Caller, Callee);
    }
    if (SILPrintInliningCallerBefore) {
      printInliningDetailsCallerBefore(PassName, Caller, Callee);
    }
    // We've already determined we should be able to inline this, so
    // unconditionally inline the function.
    //
    // If for whatever reason we can not inline this function, inlineFullApply
    // will assert, so we are safe making this assumption.
    SILInliner::inlineFullApply(AI, SILInliner::InlineKind::PerformanceInline,
                                FuncBuilder, deleter);
    // When inlining an OSSA function into a non-OSSA function, ownership of
    // nonescaping closures is lowered.  At that point, they are recognized as
    // stack users.  Since they weren't recognized as such before, they may not
    // satisfy stack discipline.  Fix that up now.
    invalidatedStackNesting |=
        Callee->hasOwnership() && !Caller->hasOwnership();
    ++NumFunctionsInlined;
    if (SILPrintInliningCallerAfter) {
      printInliningDetailsCallerAfter(PassName, Caller, Callee);
    }
    if (EnableVerifyAfterEachInlining) {
      deleter.cleanupDeadInstructions();

      // The inliner splits blocks at call sites. Re-merge trivial branches to
      // reestablish a canonical CFG.
      mergeBasicBlocks(Caller);

      if (invalidatedStackNesting) {
        StackNesting::fixNesting(Caller);
        invalidatedStackNesting = false;
      }

      Caller->verify();
    }
  }
  deleter.cleanupDeadInstructions();
  
  // The inliner splits blocks at call sites. Re-merge trivial branches to
  // reestablish a canonical CFG.
  mergeBasicBlocks(Caller);

  if (invalidatedStackNesting) {
    StackNesting::fixNesting(Caller);
  }

  // If we were asked to verify our caller after inlining all callees we could
  // find into it, do so now. This makes it easier to catch verification bugs in
  // the inliner without running the entire inliner.
  if (EnableVerifyAfterInlining) {
    Caller->verify();
  }

  return true;
}

// Find functions in cold blocks which are forced to be inlined.
// All other functions are not inlined in cold blocks.
void SILPerformanceInliner::visitColdBlocks(
    SmallVectorImpl<FullApplySite> &AppliesToInline, SILBasicBlock *Root,
    DominanceInfo *DT, int numCallerBlocks) {
  DominanceOrder domOrder(Root, DT);
  while (SILBasicBlock *block = domOrder.getNext()) {
    for (SILInstruction &I : *block) {
      auto *AI = dyn_cast<ApplyInst>(&I);
      if (!AI)
        continue;

      auto *Callee = getEligibleFunction(AI, WhatToInline);
      if (Callee && decideInColdBlock(AI, Callee, numCallerBlocks)) {
        AppliesToInline.push_back(AI);
      }
    }
    domOrder.pushChildren(block);
  }
}


//===----------------------------------------------------------------------===//
//                          Performance Inliner Pass
//===----------------------------------------------------------------------===//

namespace {
class SILPerformanceInlinerPass : public SILFunctionTransform {
  /// Specifies which functions not to inline, based on @_semantics and
  /// global_init attributes.
  InlineSelection WhatToInline;
  std::string PassName;

public:
  SILPerformanceInlinerPass(InlineSelection WhatToInline, StringRef LevelName):
    WhatToInline(WhatToInline), PassName(LevelName) {}

  void run() override {
    DominanceAnalysis *DA = PM->getAnalysis<DominanceAnalysis>();
    SILLoopAnalysis *LA = PM->getAnalysis<SILLoopAnalysis>();
    BasicCalleeAnalysis *BCA = PM->getAnalysis<BasicCalleeAnalysis>();
    OptRemark::Emitter ORE(DEBUG_TYPE, *getFunction());

    if (getOptions().InlineThreshold == 0) {
      return;
    }

    auto OptMode = getFunction()->getEffectiveOptimizationMode();

    SILOptFunctionBuilder FuncBuilder(*this);

    SILPerformanceInliner Inliner(getID(), FuncBuilder, WhatToInline,
                                  getPassManager(), DA, LA, BCA, OptMode, ORE);

    assert(getFunction()->isDefinition() &&
           "Expected only functions with bodies!");

    // Inline things into this function, and if we do so invalidate
    // analyses for this function and restart the pipeline so that we
    // can further optimize this function before attempting to inline
    // in it again.
    if (Inliner.inlineCallsIntoFunction(getFunction())) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
      restartPassPipeline();
    }
  }

};
} // end anonymous namespace

SILTransform *swift::createAlwaysInlineInliner() {
  return new SILPerformanceInlinerPass(InlineSelection::OnlyInlineAlways,
                                       "InlineAlways Performance Inliner");
}

/// Create an inliner pass that does not inline functions that are marked with
/// the @_semantics or @_effects attributes.
SILTransform *swift::createEarlyPerfInliner() {
  return new SILPerformanceInlinerPass(
    InlineSelection::NoSemanticsAndEffects, "Early Performance Inliner");
}

/// Create an inliner pass that inlines all functions that are marked with
/// the @_semantics, @_effects or global_init attributes.
SILTransform *swift::createPerfInliner() {
  return new SILPerformanceInlinerPass(
    InlineSelection::Everything, "Performance Inliner");
}
