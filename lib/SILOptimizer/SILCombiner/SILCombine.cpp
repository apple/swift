//===--- SILCombine.cpp ---------------------------------------------------===//
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
// A port of LLVM's InstCombine pass to SIL. Its main purpose is for performing
// small combining operations/peepholes at the SIL level. It additionally
// performs dead code elimination when it initially adds instructions to the
// work queue in order to reduce compile time by not visiting trivially dead
// instructions.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-combine"

#include "SILCombiner.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILBridgingUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalOSSALifetime.h"
#include "swift/SILOptimizer/Utils/CanonicalizeInstruction.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"

using namespace swift;

STATISTIC(NumCombined, "Number of instructions combined");
STATISTIC(NumDeadInst, "Number of dead insts eliminated");

static llvm::cl::opt<bool> EnableSinkingOwnedForwardingInstToUses(
    "silcombine-owned-code-sinking",
    llvm::cl::desc("Enable sinking of owened forwarding insts"),
    llvm::cl::init(true), llvm::cl::Hidden);

// Allow disabling general optimization for targetted unit tests.
static llvm::cl::opt<bool> EnableSILCombineCanonicalize(
    "sil-combine-canonicalize",
    llvm::cl::desc("Canonicalization during sil-combine"), llvm::cl::init(true),
    llvm::cl::Hidden);

//===----------------------------------------------------------------------===//
//                              Utility Methods
//===----------------------------------------------------------------------===//

/// addReachableCodeToWorklist - Walk the function in depth-first order, adding
/// all reachable code to the worklist.
///
/// This has a couple of tricks to make the code faster and more powerful.  In
/// particular, we DCE instructions as we go, to avoid adding them to the
/// worklist (this significantly speeds up SILCombine on code where many
/// instructions are dead or constant).
void SILCombiner::addReachableCodeToWorklist(SILBasicBlock *BB) {
  BasicBlockWorklist Worklist(BB);
  llvm::SmallVector<SILInstruction *, 128> InstrsForSILCombineWorklist;

  while (SILBasicBlock *BB = Worklist.pop()) {
    for (SILBasicBlock::iterator BBI = BB->begin(), E = BB->end(); BBI != E; ) {
      SILInstruction *Inst = &*BBI;
      ++BBI;

      // DCE instruction if trivially dead.
      if (isInstructionTriviallyDead(Inst)) {
        ++NumDeadInst;
        LLVM_DEBUG(llvm::dbgs() << "SC: DCE: " << *Inst << '\n');

        // We pass in false here since we need to signal to
        // eraseInstFromFunction to not add this instruction's operands to the
        // worklist since we have not initialized the worklist yet.
        //
        // The reason to just use a default argument here is that it allows us
        // to centralize all instruction removal in SILCombine into this one
        // function. This is important if we want to be able to update analyses
        // in a clean manner.
        eraseInstFromFunction(*Inst, BBI,
                              false /*Don't add operands to worklist*/);
        continue;
      }

      InstrsForSILCombineWorklist.push_back(Inst);
    }

    // Recursively visit successors.
    for (SILBasicBlock *Succ : BB->getSuccessors()) {
      Worklist.pushIfNotVisited(Succ);
    }
  }

  // Once we've found all of the instructions to add to the worklist, add them
  // in reverse order. This way SILCombine will visit from the top of the
  // function down. This jives well with the way that it adds all uses of
  // instructions to the worklist after doing a transformation, thus avoiding
  // some N^2 behavior in pathological cases.
  addInitialGroup(InstrsForSILCombineWorklist);
}

//===----------------------------------------------------------------------===//
//                               Implementation
//===----------------------------------------------------------------------===//

// Define a CanonicalizeInstruction subclass for use in SILCombine.
class SILCombineCanonicalize final : CanonicalizeInstruction {
  SmallSILInstructionWorklist<256> &Worklist;
  bool changed = false;

public:
  SILCombineCanonicalize(SmallSILInstructionWorklist<256> &Worklist,
                         DeadEndBlocks &deadEndBlocks)
      : CanonicalizeInstruction(DEBUG_TYPE, deadEndBlocks), Worklist(Worklist) {
  }

  void notifyNewInstruction(SILInstruction *inst) override {
    Worklist.add(inst);
    Worklist.addUsersOfAllResultsToWorklist(inst);
    changed = true;
  }

  // Just delete the given 'inst' and record its operands. The callback isn't
  // allowed to mutate any other instructions.
  void killInstruction(SILInstruction *inst) override {
    Worklist.eraseSingleInstFromFunction(*inst,
                                         /*AddOperandsToWorklist*/ true);
    changed = true;
  }

  void notifyHasNewUsers(SILValue value) override {
    if (Worklist.size() < 10000) {
      Worklist.addUsersToWorklist(value);
    }
    changed = true;
  }

  bool tryCanonicalize(SILInstruction *inst) {
    if (!EnableSILCombineCanonicalize)
      return false;

    changed = false;
    canonicalize(inst);
    return changed;
  }
};

bool SILCombiner::trySinkOwnedForwardingInst(SingleValueInstruction *svi) {
  if (auto *consumingUse = svi->getSingleConsumingUse()) {
    auto *consumingUser = consumingUse->getUser();

    // If our user is already in the same block, we don't move it further.
    if (svi->getParent() == consumingUser->getParent())
      return false;

    // Otherwise, make sure our instruction does not have any non-debug uses
    // that are non-lifetime ending. If so, we return.
    if (llvm::any_of(getNonDebugUses(svi),
                     [](Operand *use) { return !use->isLifetimeEnding(); }))
      return false;

    // Otherwise, delete all of the debug uses so we don't have to sink them as
    // well and then return true so we process svi in its new position.
    deleteAllDebugUses(svi, getInstModCallbacks());
    svi->moveBefore(consumingUser);
    MadeChange = true;

    // NOTE: We return false here so that our caller doesn't delete the
    // instruction and instead tries to simplify it.
    return false;
  }

  // If we have multiple consuming uses, then we know that our
  // forwarding inst must be live out of the current block and thus we
  // might be able to duplicate/sink.
  if (llvm::any_of(getNonDebugUses(svi),
                   [](Operand *use) { return !use->isLifetimeEnding(); }))
    return false;

  while (!svi->use_empty()) {
    auto *sviUse = *svi->use_begin();
    auto *sviUser = sviUse->getUser();

    if (auto *dvi = dyn_cast<DestroyValueInst>(sviUser)) {
      dvi->setOperand(svi->getOperand(0));
      Worklist.add(dvi);
      continue;
    }

    if (sviUser->isDebugInstruction()) {
      eraseInstFromFunction(*sviUser);
      continue;
    }

    auto *newSVI = svi->clone(sviUser);
    Worklist.add(newSVI);
    sviUse->set(newSVI);
  }

  eraseInstFromFunction(*svi);
  MadeChange = true;
  return true;
}

bool SILCombiner::doOneIteration(SILFunction &F, unsigned Iteration) {
  MadeChange = false;

  LLVM_DEBUG(llvm::dbgs() << "\n\nSILCOMBINE ITERATION #" << Iteration << " on "
                          << F.getName() << "\n");

  // Add reachable instructions to our worklist.
  addReachableCodeToWorklist(&*F.begin());

  SILCombineCanonicalize scCanonicalize(Worklist, deadEndBlocks);

  // Process until we run out of items in our worklist.
  while (!Worklist.isEmpty()) {
    SILInstruction *I = Worklist.pop_back_val();

    // When we erase an instruction, we use the map in the worklist to check if
    // the instruction is in the worklist. If it is, we replace it with null
    // instead of shifting all members of the worklist towards the front. This
    // check makes sure that if we run into any such residual null pointers, we
    // skip them.
    if (I == nullptr)
      continue;

    // Check to see if we can DCE the instruction.
    if (isInstructionTriviallyDead(I)) {
      LLVM_DEBUG(llvm::dbgs() << "SC: DCE: " << *I << '\n');
      eraseInstFromFunction(*I);
      ++NumDeadInst;
      MadeChange = true;
      continue;
    }

    // Canonicalize the instruction.
    if (scCanonicalize.tryCanonicalize(I)) {
      MadeChange = true;
      continue;
    }

    // If we have reached this point, all attempts to do simple simplifications
    // have failed. First if we have an owned forwarding value, we try to
    // sink. Otherwise, we perform the actual SILCombine operation.
    if (EnableSinkingOwnedForwardingInstToUses) {
      // If we have an ownership forwarding single value inst that forwards
      // through its first argument and it is trivially duplicatable, see if it
      // only has consuming uses. If so, we can duplicate the instruction into
      // the consuming use blocks and destroy any destroy_value uses of it that
      // we see. This makes it easier for SILCombine to fold instructions with
      // owned paramaters since chains of these values will be in the same
      // block.
      if (auto *svi = dyn_cast<SingleValueInstruction>(I)) {
        if ((isa<FirstArgOwnershipForwardingSingleValueInst>(svi) ||
             isa<OwnershipForwardingConversionInst>(svi)) &&
            SILValue(svi).getOwnershipKind() == OwnershipKind::Owned) {
          // Try to sink the value. If we sank the value and deleted it,
          // continue. If we didn't optimize or sank but we are still able to
          // optimize further, we fall through to SILCombine below.
          if (trySinkOwnedForwardingInst(svi)) {
            continue;
          }
        }
      }
    }

    // Then begin... SILCombine.
    Builder.setInsertionPoint(I);

#ifndef NDEBUG
    std::string OrigI;
#endif
    LLVM_DEBUG(llvm::raw_string_ostream SS(OrigI); I->print(SS);
               OrigI = SS.str(););
    LLVM_DEBUG(llvm::dbgs() << "SC: Visiting: " << OrigI << '\n');

    if (SILInstruction *Result = visit(I)) {
      ++NumCombined;
      // Should we replace the old instruction with a new one?
      Worklist.replaceInstructionWithInstruction(I, Result
#ifndef NDEBUG
          ,
          OrigI
#endif
      );
      MadeChange = true;
    }

    // Our tracking list has been accumulating instructions created by the
    // SILBuilder during this iteration. In order to finish this round of
    // SILCombine, go through the tracking list and add its contents to the
    // worklist and then clear said list in preparation for the next
    // iteration. We canonicalize any copies that we created in order to
    // eliminate unnecessary copies introduced by RAUWing when ownership is
    // enabled.
    //
    // NOTE: It is ok if copy propagation results in MadeChanges being set to
    // true. This is because we only add elements to the tracking list if we
    // actually made a change to the IR, so MadeChanges should already be true
    // at this point.
    auto &TrackingList = *Builder.getTrackingList();
    if (TrackingList.size() && Builder.hasOwnership()) {
      SmallSetVector<SILValue, 16> defsToCanonicalize;
      for (auto *trackedInst : TrackingList) {
        if (!trackedInst->isDeleted()) {
          if (auto *cvi = dyn_cast<CopyValueInst>(trackedInst)) {
            defsToCanonicalize.insert(
                CanonicalizeOSSALifetime::getCanonicalCopiedDef(cvi));
          }
        }
      }
      if (defsToCanonicalize.size()) {
        CanonicalizeOSSALifetime canonicalizer(
            false /*prune debug*/, false /*canonicalize borrows*/,
            false /*poison refs*/, NLABA, DA, getInstModCallbacks());
        auto analysisInvalidation = canonicalizeOSSALifetimes(
            canonicalizer, defsToCanonicalize.getArrayRef());
        if (bool(analysisInvalidation)) {
          NLABA->lockInvalidation();
          parentTransform->invalidateAnalysis(analysisInvalidation);
          NLABA->unlockInvalidation();
        }
      }
    }
    for (SILInstruction *I : TrackingList) {
      if (!I->isDeleted()) {
        LLVM_DEBUG(llvm::dbgs() << "SC: add " << *I
                                << " from tracking list to worklist\n");
        Worklist.add(I);
      }
    }
    TrackingList.clear();
  }

  Worklist.resetChecked();
  return MadeChange;
}

bool SILCombiner::runOnFunction(SILFunction &F) {
  clear();

  bool Changed = false;
  // Perform iterations until we do not make any changes.
  while (doOneIteration(F, Iteration)) {
    Changed = true;
    ++Iteration;
  }

  if (invalidatedStackNesting) {
    StackNesting::fixNesting(&F);
  }

  // Cleanup the builder and return whether or not we made any changes.
  return Changed;
}

void SILCombiner::eraseInstIncludingUsers(SILInstruction *inst) {
  for (SILValue result : inst->getResults()) {
    while (!result->use_empty()) {
      eraseInstIncludingUsers(result->use_begin()->getUser());
    }
  }
  eraseInstFromFunction(*inst);
}

/// Runs an instruction pass in libswift.
void SILCombiner::runSwiftInstructionPass(SILInstruction *inst,
                              void (*runFunction)(BridgedInstructionPassCtxt)) {
  Worklist.setLibswiftPassInvocation(&libswiftPassInvocation);
  runFunction({ {inst->asSILNode()}, {&libswiftPassInvocation} });
  Worklist.setLibswiftPassInvocation(nullptr);
  libswiftPassInvocation.finishedPassRun();
}

/// Registered briged instruction pass run functions.
static llvm::StringMap<BridgedInstructionPassRunFn> libswiftInstPasses;
static bool passesRegistered = false;

// Called from libswift's initializeLibSwift().
void SILCombine_registerInstructionPass(BridgedStringRef name,
                                        BridgedInstructionPassRunFn runFn) {
  libswiftInstPasses[getStringRef(name)] = runFn;
  passesRegistered = true;
}

#define SWIFT_INSTRUCTION_PASS_COMMON(INST, TAG, LEGACY_RUN) \
SILInstruction *SILCombiner::visit##INST(INST *inst) {                     \
  static BridgedInstructionPassRunFn runFunction = nullptr;                \
  static bool runFunctionSet = false;                                      \
  if (!runFunctionSet) {                                                   \
    runFunction = libswiftInstPasses[TAG];                                 \
    if (!runFunction && passesRegistered) {                                \
      llvm::errs() << "Swift pass " << TAG << " is not registered\n";      \
      abort();                                                             \
    }                                                                      \
    runFunctionSet = true;                                                 \
  }                                                                        \
  if (!runFunction) {                                                      \
    LEGACY_RUN;                                                            \
  }                                                                        \
  runSwiftInstructionPass(inst, runFunction);                              \
  return nullptr;                                                          \
}                                                                          \

#define PASS(ID, TAG, DESCRIPTION)

#define SWIFT_INSTRUCTION_PASS(INST, TAG) \
  SWIFT_INSTRUCTION_PASS_COMMON(INST, TAG, { return nullptr; })

#define SWIFT_INSTRUCTION_PASS_WITH_LEGACY(INST, TAG) \
  SWIFT_INSTRUCTION_PASS_COMMON(INST, TAG, { return legacyVisit##INST(inst); })

#include "swift/SILOptimizer/PassManager/Passes.def"

#undef SWIFT_INSTRUCTION_PASS_COMMON

//===----------------------------------------------------------------------===//
//                                Entry Points
//===----------------------------------------------------------------------===//

namespace {

class SILCombine : public SILFunctionTransform {

  llvm::SmallVector<SILInstruction *, 64> TrackingList;
  
  /// The entry point to the transformation.
  void run() override {
    auto *AA = PM->getAnalysis<AliasAnalysis>(getFunction());
    auto *DA = PM->getAnalysis<DominanceAnalysis>();
    auto *PCA = PM->getAnalysis<ProtocolConformanceAnalysis>();
    auto *CHA = PM->getAnalysis<ClassHierarchyAnalysis>();
    auto *NLABA = PM->getAnalysis<NonLocalAccessBlockAnalysis>();

    SILOptFunctionBuilder FuncBuilder(*this);
    // Create a SILBuilder with a tracking list for newly added
    // instructions, which we will periodically move to our worklist.
    SILBuilder B(*getFunction(), &TrackingList);
    SILCombiner Combiner(this, FuncBuilder, B, AA, DA, PCA, CHA, NLABA,
                         getOptions().RemoveRuntimeAsserts);
    bool Changed = Combiner.runOnFunction(*getFunction());
    assert(TrackingList.empty() &&
           "TrackingList should be fully processed by SILCombiner");

    if (Changed) {
      // Invalidate everything.
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createSILCombine() {
  return new SILCombine();
}

//===----------------------------------------------------------------------===//
//                          SwiftFunctionPassContext
//===----------------------------------------------------------------------===//

void LibswiftPassInvocation::eraseInstruction(SILInstruction *inst) {
  if (silCombiner) {
    silCombiner->eraseInstFromFunction(*inst);
  } else {
    inst->eraseFromParent();
  }
}
