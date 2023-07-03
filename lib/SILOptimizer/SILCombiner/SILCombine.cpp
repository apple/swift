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
#include "swift/Basic/BridgingUtils.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/Analysis/SimplifyInstruction.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/SILOptimizer/Utils/CanonicalizeInstruction.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
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
    llvm::cl::desc("Enable sinking of owned forwarding insts"),
    llvm::cl::init(true), llvm::cl::Hidden);

// Allow disabling general optimization for targeted unit tests.
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

SILCombiner::SILCombiner(SILFunctionTransform *trans,
                         bool removeCondFails, bool enableCopyPropagation) :
  parentTransform(trans),
  AA(trans->getPassManager()->getAnalysis<AliasAnalysis>(trans->getFunction())),
  CA(trans->getPassManager()->getAnalysis<BasicCalleeAnalysis>()),
  DA(trans->getPassManager()->getAnalysis<DominanceAnalysis>()),
  PCA(trans->getPassManager()->getAnalysis<ProtocolConformanceAnalysis>()),
  CHA(trans->getPassManager()->getAnalysis<ClassHierarchyAnalysis>()),
  NLABA(trans->getPassManager()->getAnalysis<NonLocalAccessBlockAnalysis>()),
  Worklist("SC"),
  deleter(InstModCallbacks()
              .onDelete([&](SILInstruction *instToDelete) {
                // We allow for users in SILCombine to perform 2 stage
                // deletion, so we need to split the erasing of
                // instructions from adding operands to the worklist.
                eraseInstFromFunction(*instToDelete,
                                      false /* don't add operands */);
              })
              .onNotifyWillBeDeleted(
                  [&](SILInstruction *instThatWillBeDeleted) {
                    Worklist.addOperandsToWorklist(
                      *instThatWillBeDeleted);
                  })
              .onCreateNewInst([&](SILInstruction *newlyCreatedInst) {
                Worklist.add(newlyCreatedInst);
              })
              .onSetUseValue([&](Operand *use, SILValue newValue) {
                use->set(newValue);
                Worklist.add(use->getUser());
              })),
  deadEndBlocks(trans->getFunction()), MadeChange(false),
  RemoveCondFails(removeCondFails),
  enableCopyPropagation(enableCopyPropagation), Iteration(0),
  Builder(*trans->getFunction(), &TrackingList),
  FuncBuilder(*trans),
  CastOpt(
      FuncBuilder, nullptr /*SILBuilderContext*/,
      /* ReplaceValueUsesAction */
      [&](SILValue Original, SILValue Replacement) {
        replaceValueUsesWith(Original, Replacement);
      },
      /* ReplaceInstUsesAction */
      [&](SingleValueInstruction *I, ValueBase *V) {
        replaceInstUsesWith(*I, V);
      },
      /* EraseAction */
      [&](SILInstruction *I) { eraseInstFromFunction(*I); }),
  deBlocks(trans->getFunction()),
  ownershipFixupContext(getInstModCallbacks(), deBlocks),
  swiftPassInvocation(trans->getPassManager(),
                      trans->getFunction(), this) {}

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

    LLVM_DEBUG(llvm::dbgs() << "Sink forwarding: " << *svi << '\n');

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

    LLVM_DEBUG(llvm::dbgs()
               << "Sink forwarding: " << *svi << " to " << *newSVI << '\n');

    Worklist.add(newSVI);
    sviUse->set(newSVI);
  }

  eraseInstFromFunction(*svi);
  MadeChange = true;
  return true;
}

/// Canonicalize each extended OSSA lifetime that contains an instruction newly
/// created during this SILCombine iteration.
///
/// \p currentInst is null if the current instruction was deleted during its
/// SILCombine.
///
/// Avoid endless worklist iteration as follows:
///
/// - Canonicalization only runs on the canonical definition of the visited
///   instruction if it was itself a copy or any new copies were inserted
///   as a result of optimization.
///
/// - Instructions are only added back to the SILCombine worklist when
///   canonicalization deletes an instruction. Only the canonical def being
///   processed and its uses are added rather than arbitrary operands of the
///   deleted instruction. This ensures that an instruction is only added back
///   to the worklist after SILCombine either directly optimized it or created a
///   new copy_value for which it is the canonical def or its use.
void SILCombiner::canonicalizeOSSALifetimes(SILInstruction *currentInst) {
  if (!enableCopyPropagation || !Builder.hasOwnership())
    return;

  SmallSetVector<SILValue, 16> defsToCanonicalize;

  // copyInst was either optimized by a SILCombine visitor or is a copy_value
  // produced by the visitor. Find the canonical def.
  auto recordCopiedDef = [&defsToCanonicalize](CopyValueInst *copyInst) {
    SILValue def = CanonicalizeOSSALifetime::getCanonicalCopiedDef(copyInst);

    // getCanonicalCopiedDef returns a copy whenever that the copy's source is
    // guaranteed. In that case, find the root of the borrowed lifetime. If it
    // is a function argument, then a simple guaranteed canonicalization can be
    // performed. Canonicalizing other borrow scopes is not handled by
    // SILCombine because it's not a single-lifetime canonicalization.  Instead,
    // SILCombine treats a copy that uses a borrowed value as a separate owned
    // live range. Handling the compensation code across the borrow scope
    // boundary requires post processing in a particular order.  The copy
    // propagation pass knows how to handle that. To avoid complexity and ensure
    // fast convergence, rewriting borrow scopes should not be combined with
    // other unrelated transformations.
    if (auto *copyDef = dyn_cast<CopyValueInst>(def)) {
      if (SILValue borrowDef = CanonicalizeBorrowScope::getCanonicalBorrowedDef(
              copyDef->getOperand())) {
        if (isa<SILFunctionArgument>(borrowDef)) {
          def = borrowDef;
        }
      }
    }
    defsToCanonicalize.insert(def);
  };

  if (auto *copyInst = dyn_cast_or_null<CopyValueInst>(currentInst))
    recordCopiedDef(copyInst);

  for (auto *trackedInst : *Builder.getTrackingList()) {
    if (trackedInst->isDeleted())
      continue;
    if (auto *copyInst = dyn_cast<CopyValueInst>(trackedInst))
      recordCopiedDef(copyInst);
  }
  if (defsToCanonicalize.empty())
    return;

  // Remove instructions deleted during canonicalization from SILCombine's
  // worklist. CanonicalizeOSSALifetime invalidates operands before invoking
  // the deletion callback.
  auto canonicalizeCallbacks =
      InstModCallbacks().onDelete([this](SILInstruction *instToDelete) {
        eraseInstFromFunction(*instToDelete,
                              false /*do not add operands to the worklist*/);
      });
  InstructionDeleter deleter(std::move(canonicalizeCallbacks));

  DominanceInfo *domTree = DA->get(&Builder.getFunction());
  CanonicalizeOSSALifetime canonicalizer(
      false /*prune debug*/,
      !parentTransform->getFunction()->shouldOptimize() /*maximize lifetime*/,
      parentTransform->getFunction(), NLABA, domTree, CA, deleter);
  CanonicalizeBorrowScope borrowCanonicalizer(parentTransform->getFunction(),
                                              deleter);

  while (!defsToCanonicalize.empty()) {
    SILValue def = defsToCanonicalize.pop_back_val();

    deleter.getCallbacks().resetHadCallbackInvocation();

    auto canonicalized = [&]() {
      if (!deleter.getCallbacks().hadCallbackInvocation())
        return;

      if (auto *inst = def->getDefiningInstruction()) {
        Worklist.add(inst);
      }
      for (auto *use : def->getUses()) {
        Worklist.add(use->getUser());
      }
    };

    if (def->getOwnershipKind() == OwnershipKind::Guaranteed) {
      if (auto functionArg = dyn_cast<SILFunctionArgument>(def)) {
        if (borrowCanonicalizer.canonicalizeFunctionArgument(functionArg))
          canonicalized();
      }
      continue;
    }
    if (canonicalizer.canonicalizeValueLifetime(def)) {
      canonicalized();
    }
  }
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

    if (!parentTransform->continueWithNextSubpassRun(I))
      return false;

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
      // owned parameters since chains of these values will be in the same
      // block.
      if (auto *svi = dyn_cast<SingleValueInstruction>(I)) {
        if (auto fwdOp = ForwardingOperation(svi)) {
          if (fwdOp.canForwardFirstOperandOnly() &&
              SILValue(svi)->getOwnershipKind() == OwnershipKind::Owned) {
            // Try to sink the value. If we sank the value and deleted it,
            // continue. If we didn't optimize or sank but we are still able to
            // optimize further, we fall through to SILCombine below.
            if (trySinkOwnedForwardingInst(svi)) {
              continue;
            }
          }
        }
      }
    }

    // Then begin... SILCombine.
    Builder.setInsertionPoint(I);

#ifndef NDEBUG
    std::string OrigIStr;
#endif
    LLVM_DEBUG(llvm::raw_string_ostream SS(OrigIStr); I->print(SS);
               OrigIStr = SS.str(););
    LLVM_DEBUG(llvm::dbgs() << "SC: Visiting: " << OrigIStr << '\n');

    SILInstruction *currentInst = I;
    if (SILInstruction *Result = visit(I)) {
      ++NumCombined;
      // Should we replace the old instruction with a new one?
      Worklist.replaceInstructionWithInstruction(I, Result
#ifndef NDEBUG
                                                 ,
                                                 OrigIStr
#endif
      );
      currentInst = Result;
      MadeChange = true;
    }

    // Eliminate copies created that this SILCombine iteration may have
    // introduced during OSSA-RAUW.
    canonicalizeOSSALifetimes(currentInst->isDeleted() ? nullptr : currentInst);

    // Builder's tracking list has been accumulating instructions created by the
    // during this SILCombine iteration. To finish this iteration, go through
    // the tracking list and add its contents to the worklist and then clear
    // said list in preparation for the next iteration.
    for (SILInstruction *I : *Builder.getTrackingList()) {
      if (!I->isDeleted()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "SC: add " << *I << " from tracking list to worklist\n");
        Worklist.add(I);
      }
    }
    Builder.getTrackingList()->clear();
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

  assert(TrackingList.empty() && "TrackingList should be fully processed");
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

/// Runs a Swift instruction pass.
void SILCombiner::runSwiftInstructionPass(SILInstruction *inst,
                              void (*runFunction)(BridgedInstructionPassCtxt)) {
  swiftPassInvocation.startInstructionPassRun(inst);
  runFunction({ {inst->asSILNode()}, {&swiftPassInvocation} });
  swiftPassInvocation.finishedInstructionPassRun();
}

/// Registered briged instruction pass run functions.
static llvm::StringMap<BridgedInstructionPassRunFn> swiftInstPasses;
static bool passesRegistered = false;

// Called from initializeSwiftModules().
void SILCombine_registerInstructionPass(llvm::StringRef instClassName,
                                        BridgedInstructionPassRunFn runFn) {
  swiftInstPasses[instClassName] = runFn;
  passesRegistered = true;
}

#define SWIFT_SILCOMBINE_PASS(INST) \
SILInstruction *SILCombiner::visit##INST(INST *inst) {                     \
  static BridgedInstructionPassRunFn runFunction = nullptr;                \
  static bool passDisabled = false;                                        \
  if (!runFunction) {                                                      \
    runFunction = swiftInstPasses[#INST];                                  \
    if (!runFunction) {                                                    \
      if (passesRegistered) {                                              \
        llvm::errs() << "Swift pass " << #INST << " is not registered\n";  \
        abort();                                                           \
      } else {                                                             \
        return nullptr;                                                    \
      }                                                                    \
    }                                                                      \
    StringRef instName = getSILInstructionName(SILInstructionKind::INST);  \
    passDisabled = SILPassManager::isInstructionPassDisabled(instName);    \
  }                                                                        \
  if (passDisabled &&                                                      \
      SILPassManager::disablePassesForFunction(inst->getFunction())) {     \
    return nullptr;                                                        \
  }                                                                        \
  runSwiftInstructionPass(inst, runFunction);                              \
  return nullptr;                                                          \
}                                                                          \

#define PASS(ID, TAG, DESCRIPTION)

#include "swift/SILOptimizer/PassManager/Passes.def"

//===----------------------------------------------------------------------===//
//                                Entry Points
//===----------------------------------------------------------------------===//

namespace {

class SILCombine : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    bool enableCopyPropagation =
        getOptions().CopyPropagation == CopyPropagationOption::On;
    if (getOptions().EnableOSSAModules) {
      enableCopyPropagation =
          getOptions().CopyPropagation != CopyPropagationOption::Off;
    }

    SILCombiner Combiner(this, getOptions().RemoveRuntimeAsserts,
                         enableCopyPropagation);
    bool Changed = Combiner.runOnFunction(*getFunction());

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

void SwiftPassInvocation::eraseInstruction(SILInstruction *inst) {
  if (silCombiner) {
    silCombiner->eraseInstFromFunction(*inst);
  } else {
    inst->eraseFromParent();
  }
}
