//=====-- ShrinkBorrowScope.cpp - Hoist end_borrows to deinit barriers. -=====//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
/// Shrink borrow scopes by hoisting end_borrows up to deinit barriers.  After
/// this is done, CanonicalizeOSSALifetime is free to hoist the destroys of the
/// owned value up to the end_borrow.  In this way, the lexical lifetime of
/// guaranteed values is preserved.
//===----------------------------------------------------------------------===//

#include "swift/AST/Builtins.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/Reachability.h"
#include "swift/SILOptimizer/Analysis/VisitBarrierAccessScopes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeBorrowScope.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/STLExtras.h"

#define DEBUG_TYPE "copy-propagation"

using namespace swift;

//===----------------------------------------------------------------------===//
//                       MARK: ShrinkBorrowScope
//===----------------------------------------------------------------------===//

namespace ShrinkBorrowScope {

/// The environment within which to hoist.
struct Context final {
  /// The instruction that begins the borrow scope.
  BeginBorrowInst const &introducer;

  /// BorrowedValue(introducer)
  BorrowedValue const borrowedValue;

  /// The value whose lifetime is guaranteed by the lexical borrow scope.
  ///
  /// introducer->getOperand()
  SILValue const borrowee;

  SILBasicBlock *defBlock;

  SILFunction &function;

  /// The copy_value instructions that the utility creates or changes.
  ///
  /// Clients provide this so that they can update worklists in response.
  SmallVectorImpl<CopyValueInst *> &modifiedCopyValueInsts;

  InstructionDeleter &deleter;

  BasicCalleeAnalysis *calleeAnalysis;

  Context(BeginBorrowInst const &introducer,
          SmallVectorImpl<CopyValueInst *> &modifiedCopyValueInsts,
          InstructionDeleter &deleter, BasicCalleeAnalysis *calleeAnalysis)
      : introducer(introducer), borrowedValue(BorrowedValue(&introducer)),
        borrowee(introducer.getOperand()), defBlock(introducer.getParent()),
        function(*introducer.getFunction()),
        modifiedCopyValueInsts(modifiedCopyValueInsts), deleter(deleter),
        calleeAnalysis(calleeAnalysis) {}
  Context(Context const &) = delete;
  Context &operator=(Context const &) = delete;
};

/// How %lifetime gets used.
struct Usage final {
  /// Instructions which are users of the simple (i.e. not reborrowed) extended
  /// i.e. copied lifetime of the introducer.
  SmallPtrSet<SILInstruction *, 16> users;
  // The instructions from which the shrinking starts, the scope ending
  // instructions.
  llvm::SmallSetVector<SILInstruction *, 4> ends;

  Usage(){};
  Usage(Usage const &) = delete;
  Usage &operator=(Usage const &) = delete;
};

/// Identify scope ending uses and extended users of %lifetime.
///
/// returns true if all uses were found
///         false otherwise
bool findUsage(Context const &context, Usage &usage) {
  llvm::SmallVector<SILInstruction *, 16> scopeEndingInsts;
  context.borrowedValue.getLocalScopeEndingInstructions(scopeEndingInsts);

  // Add all the end_borrows to the collection of ends.
  for (auto *instruction : scopeEndingInsts) {
    // If a scope ending instruction is not an end_borrow, bail out.
    if (!isa<EndBorrowInst>(instruction))
      return false;
    usage.ends.insert(instruction);
  }

  SmallVector<Operand *, 16> uses;
  if (!findExtendedUsesOfSimpleBorrowedValue(context.borrowedValue, &uses)) {
    // If the value produced by begin_borrow escapes, don't shrink the borrow
    // scope.
    return false;
  }
  for (auto *use : uses) {
    usage.users.insert(use->getUser());
  }
  return true;
}

/// How end_borrow hoisting is obstructed.
struct DeinitBarriers final {
  /// Copies to be rewritten as copies of %borrowee.
  SmallVector<CopyValueInst *, 4> copies;

  /// Instructions above which end_borrows cannot be hoisted.
  SmallVector<SILInstruction *, 4> instructions;

  /// Blocks one of whose phis is a barrier and consequently out of which
  /// end_borrows cannot be hoisted.
  SmallVector<SILBasicBlock *, 4> phis;

  /// Blocks whose single predecessors has another successor to the top of which
  /// end_borrows cannot be hoisted.
  SmallVector<SILBasicBlock *, 4> blocks;

  DeinitBarriers(Context &context) {}
  DeinitBarriers(DeinitBarriers const &) = delete;
  DeinitBarriers &operator=(DeinitBarriers const &) = delete;
};

class BarrierAccessScopeFinder;

/// Works backwards from the current location of end_borrows to the earliest
/// place they can be hoisted to.
///
/// Implements IterativeBackwardReachability::Effects.
/// Implements IterativeBackwardReachability::findBarrier::Visitor.
/// Implements VisitBarrierAccessScopes::Effects
class Dataflow final {
public:
  using Reachability = IterativeBackwardReachability<Dataflow>;
  using Effect = Reachability::Effect;

private:
  Context const &context;
  Usage const &uses;
  DeinitBarriers &barriers;
  Reachability::Result result;
  Reachability reachability;
  SmallPtrSet<BeginAccessInst *, 8> barrierAccessScopes;
  bool recordCopies = false;

  enum class Classification { Barrier, Copy, Other };

public:
  Dataflow(Context const &context, Usage const &uses, DeinitBarriers &barriers)
      : context(context), uses(uses), barriers(barriers),
        result(&context.function),
        reachability(Reachability::untilInitialBlock(
            &context.function, context.defBlock, *this, result)) {}
  Dataflow(Dataflow const &) = delete;
  Dataflow &operator=(Dataflow const &) = delete;

  void run();

private:
  friend Reachability;
  friend class BarrierAccessScopeFinder;
  friend class VisitBarrierAccessScopes<Dataflow, BarrierAccessScopeFinder>;

  Classification classifyInstruction(SILInstruction *);

  bool classificationIsBarrier(Classification);

  /// IterativeBackwardReachability::Effects
  /// VisitBarrierAccessScopes::Effects

  auto gens() { return uses.ends; }

  Effect effectForInstruction(SILInstruction *);

  Effect effectForPhi(SILBasicBlock *);

  /// VisitBarrierAccessScopes::Effects

  auto localGens() { return result.localGens; }

  bool isLocalGen(SILInstruction *instruction) {
    return result.localGens.contains(instruction);
  }

  /// IterativeBackwardReachability::findBarrier::Visitor.

  void visitBarrierInstruction(SILInstruction *instruction) {
    barriers.instructions.push_back(instruction);
  }

  void visitBarrierPhi(SILBasicBlock *block) { barriers.phis.push_back(block); }

  void visitBarrierBlock(SILBasicBlock *block) {
    barriers.blocks.push_back(block);
  }
};

/// Whether the specified value is %lifetime or its iterated copy_value.
///
/// In other words, it has to be a simple extended def of %lifetime.
bool isSimpleExtendedIntroducerDef(Context const &context, SILValue value) {
  while (true) {
    auto *instruction = value.getDefiningInstruction();
    if (!instruction)
      return false;
    if (instruction == &context.introducer)
      return true;
    if (auto *cvi = dyn_cast<CopyValueInst>(instruction)) {
      value = cvi->getOperand();
      continue;
    }
    return false;
  }
}

Dataflow::Classification
Dataflow::classifyInstruction(SILInstruction *instruction) {
  if (instruction == &context.introducer) {
    return Classification::Barrier;
  }
  if (auto *cvi = dyn_cast<CopyValueInst>(instruction)) {
    if (isSimpleExtendedIntroducerDef(context, cvi->getOperand())) {
      return Classification::Copy;
    }
  }
  if (uses.users.contains(instruction)) {
    return Classification::Barrier;
  }
  if (auto *eai = dyn_cast<EndAccessInst>(instruction)) {
    return barrierAccessScopes.contains(eai->getBeginAccess())
               ? Classification::Barrier
               : Classification::Other;
  }
  if (isDeinitBarrier(instruction, context.calleeAnalysis)) {
    return Classification::Barrier;
  }
  return Classification::Other;
}

bool Dataflow::classificationIsBarrier(Classification classification) {
  switch (classification) {
  case Classification::Barrier:
    return true;
  case Classification::Copy:
  case Classification::Other:
    return false;
  }
  llvm_unreachable("exhaustive switch not exhaustive?!");
}

Dataflow::Effect Dataflow::effectForInstruction(SILInstruction *instruction) {
  if (uses.ends.contains(instruction))
    return Effect::Gen();
  auto classification = classifyInstruction(instruction);
  if (recordCopies && classification == Classification::Copy)
    barriers.copies.push_back(cast<CopyValueInst>(instruction));
  return classificationIsBarrier(classification) ? Effect::Kill()
                                                 : Effect::NoEffect();
}

Dataflow::Effect Dataflow::effectForPhi(SILBasicBlock *block) {
  assert(llvm::all_of(block->getArguments(),
                      [&](auto argument) { return PhiValue(argument); }));

  bool isBarrier =
      llvm::any_of(block->getPredecessorBlocks(), [&](auto *predecessor) {
        return classificationIsBarrier(
            classifyInstruction(predecessor->getTerminator()));
      });
  return isBarrier ? Effect::Kill() : Effect::NoEffect();
}

/// Finds end_access instructions which are barriers to hoisting because the
/// access scopes they contain barriers to hoisting.  Hoisting end_borrows into
/// such access scopes could introduce exclusivity violations.
///
/// Implements BarrierAccessScopeFinder::Visitor
class BarrierAccessScopeFinder final {
  using Impl = VisitBarrierAccessScopes<Dataflow, BarrierAccessScopeFinder>;
  Impl impl;
  Dataflow &dataflow;

public:
  BarrierAccessScopeFinder(Context const &context, Dataflow &dataflow)
      : impl(&context.function, dataflow, *this), dataflow(dataflow) {}

  void find() { impl.visit(); }

private:
  friend Impl;

  bool isInRegion(SILBasicBlock *block) {
    return dataflow.result.discoveredBlocks.contains(block);
  }

  void visitBarrierAccessScope(BeginAccessInst *bai) {
    dataflow.barrierAccessScopes.insert(bai);
    for (auto *eai : bai->getEndAccesses()) {
      dataflow.reachability.addKill(eai);
    }
  }
};

void Dataflow::run() {
  reachability.initialize();
  BarrierAccessScopeFinder finder(context, *this);
  finder.find();
  reachability.solve();
  recordCopies = true;
  reachability.findBarriers(*this);
}

/// Hoist the scope ends of %lifetime, rewriting copies and borrows along the
/// way.
class Rewriter final {
  Context &context;
  Usage const &uses;
  DeinitBarriers const &barriers;

  // The end _borrow instructions for this borrow scope that existed before
  // ShrinkBorrowScope ran and which were not modified.
  llvm::SmallPtrSet<SILInstruction *, 8> reusedEndBorrowInsts;

public:
  Rewriter(Context &context, Usage const &uses, DeinitBarriers const &barriers)
      : context(context), uses(uses), barriers(barriers) {}
  Rewriter(Rewriter const &) = delete;
  Rewriter &operator=(Rewriter const &) = delete;

  bool run();

private:
  EndBorrowInst *findPreexistingEndBorrow(SILInstruction *instruction);
  bool createEndBorrow(SILInstruction *insertionPoint);
};

bool Rewriter::run() {
  bool madeChange = false;

  for (auto *cvi : barriers.copies) {
    cvi->setOperand(context.borrowee);
    context.modifiedCopyValueInsts.push_back(cvi);
    madeChange = true;
  }

  // Add end_borrows for phi barrier boundaries.
  //
  // A block is a phi barrier iff any of its predecessors' terminators get
  // classified as barriers.  That happens when a copy of %lifetime is passed
  // to a phi.
  for (auto *block : barriers.phis) {
    madeChange |= createEndBorrow(&block->front());
  }

  // Add end_borrows for barrier boundaries.
  //
  // Insert end_borrows after every non-terminator barrier.
  //
  // For terminator barriers, add end_borrows at the beginning of the successor
  // blocks.  In order to reach a terminator and classify it as a barrier, all
  // of a block P's successors B had reachable beginnings.  If any of them
  // didn't, then BackwardReachability::meetOverSuccessors would never have
  // returned true for P, so none of its instructions would ever have been
  // classified (except for via effectForPhi, which doesn't record terminator
  // barriers).
  for (auto instruction : barriers.instructions) {
    if (auto *terminator = dyn_cast<TermInst>(instruction)) {
      auto successors = terminator->getParentBlock()->getSuccessorBlocks();
      for (auto *successor : successors) {
        // If a terminator is a barrier, it must not branch to a merge point.
        // Doing so would require one of the following:
        // - the terminator was passed a phi--which is handled by barriers.phis
        // - the terminator had a result--which can't happen thanks to the lack
        //   of critical edges
        // - the terminator was a BranchInst which was passed no arguments but
        //   which was nonetheless identified as a barrier--which is illegal
        assert(successor->getSinglePredecessorBlock() ==
               terminator->getParentBlock());
        madeChange |= createEndBorrow(&successor->front());
      }
    } else {
      auto *next = instruction->getNextInstruction();
      assert(next);
      madeChange |= createEndBorrow(next);
    }
  }

  // Add end_borrows for control-flow boundaries.
  //
  // Insert end_borrows at the beginning of blocks which were preceded by a
  // control flow branch (and which, thanks to the lack of critical edges,
  // don't have multiple predecessors) whose end was not reachable (because
  // reachability was not able to make it to the top of some other successor).
  //
  // In other words, a control flow boundary is the target block of the edge
  // to a block B from its single predecessor P not all of whose successors S
  // in succ(P) had reachable beginnings.  We witness that fact about P's
  // successors by way of P not having a reachable end--see
  // IterativeBackwardReachability::meetOverSuccessors.
  //
  // control-flow-boundary(B) := beginning-reachable(B) && !end-reachable(P)
  for (auto *block : barriers.blocks) {
    madeChange |= createEndBorrow(&block->front());
  }

  if (madeChange) {
    // Remove all the original end_borrow instructions.
    for (auto *end : uses.ends) {
      if (reusedEndBorrowInsts.contains(end)) {
        continue;
      }
      context.deleter.forceDelete(end);
    }
  }

  return madeChange;
}

EndBorrowInst *
Rewriter::findPreexistingEndBorrow(SILInstruction *insertionPoint) {
  for (auto *instruction = insertionPoint; instruction;
       instruction = instruction->getNextInstruction()) {
    if (auto *ebi = dyn_cast<EndBorrowInst>(instruction)) {
      if (llvm::find(uses.ends, ebi) != uses.ends.end())
        return ebi;
    }
    if (auto *cvi = dyn_cast<CopyValueInst>(instruction)) {
      if (llvm::is_contained(barriers.copies, cvi)) {
        continue;
      }
    }
    /// Otherwise, this is an "interesting" instruction.  We want to record that
    /// we were able to hoist the end of the borrow scope over it, so we stop
    /// looking for the preexisting end_borrow.
    return nullptr;
  }
  return nullptr;
}

bool Rewriter::createEndBorrow(SILInstruction *insertionPoint) {
  if (auto *ebi = findPreexistingEndBorrow(insertionPoint)) {
    reusedEndBorrowInsts.insert(ebi);
    return false;
  }
  auto builder = SILBuilderWithScope(insertionPoint);
  builder.createEndBorrow(
      RegularLocation::getAutoGeneratedLocation(insertionPoint->getLoc()),
      &context.introducer);
  return true;
}

bool run(Context &context) {
  Usage usage;
  if (!findUsage(context, usage))
    return false;

  DeinitBarriers barriers(context);
  Dataflow flow(context, usage, barriers);
  flow.run();

  Rewriter rewriter(context, usage, barriers);

  return rewriter.run();
}
} // end namespace ShrinkBorrowScope

bool swift::shrinkBorrowScope(
    BeginBorrowInst const &bbi, InstructionDeleter &deleter,
    BasicCalleeAnalysis *calleeAnalysis,
    SmallVectorImpl<CopyValueInst *> &modifiedCopyValueInsts) {
  ShrinkBorrowScope::Context context(bbi, modifiedCopyValueInsts, deleter,
                                     calleeAnalysis);
  return ShrinkBorrowScope::run(context);
}

namespace swift::test {
// Arguments:
// - BeginBorrowInst - the introducer for the scope to shrink
// - bool - the expected return value of shrinkBorrowScope
// - variadic list of values consisting of the copies expected to be rewritten
// Dumps:
// - DELETED:  <<instruction deleted>>
static FunctionTest ShrinkBorrowScopeTest(
    "shrink-borrow-scope", [](auto &function, auto &arguments, auto &test) {
      auto instruction = arguments.takeValue();
      auto expected = arguments.takeBool();
      auto *bbi = cast<BeginBorrowInst>(instruction);
      auto *analysis = test.template getAnalysis<BasicCalleeAnalysis>();
      SmallVector<CopyValueInst *, 4> modifiedCopyValueInsts;
      InstructionDeleter deleter(
          InstModCallbacks().onDelete([&](auto *instruction) {
            llvm::errs() << "DELETED:\n";
            instruction->dump();
          }));
      auto shrunk =
          shrinkBorrowScope(*bbi, deleter, analysis, modifiedCopyValueInsts);
      unsigned index = 0;
      for (auto *cvi : modifiedCopyValueInsts) {
        auto expectedCopy = arguments.takeValue();
        llvm::errs() << "rewritten copy " << index << ":\n";
        llvm::errs() << "expected:\n";
        expectedCopy->print(llvm::errs());
        llvm::errs() << "got:\n";
        cvi->dump();
        assert(cvi == expectedCopy);
        ++index;
      }
      assert(expected == shrunk && "didn't shrink expectedly!?");
    });
} // namespace swift::test
