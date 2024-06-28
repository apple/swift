//=-- LexicalDestroyHoisting.cpp - Hoist destroy_values to deinit barriers. -=//
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
/// Hoist destroys of owned lexical values (owned arguments and the results of
/// move_value [lexical] instructions) up to deinit barriers.
//===----------------------------------------------------------------------===//

#include "swift/AST/Builtins.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
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
//                       MARK: LexicalDestroyHoisting
//===----------------------------------------------------------------------===//

namespace LexicalDestroyHoisting {

/// The environment within which to hoist.
struct Context final {
  /// The owned lexical value whose destroys are to be hoisted.
  SILValue const &value;

  /// value->getDefiningInstruction()
  SILInstruction *const definition;

  SILBasicBlock *defBlock;

  SILFunction &function;

  InstructionDeleter &deleter;

  BasicCalleeAnalysis *calleeAnalysis;

  Context(SILValue const &value, SILFunction &function,
          InstructionDeleter &deleter, BasicCalleeAnalysis *calleeAnalysis)
      : value(value), definition(value->getDefiningInstruction()),
        defBlock(value->getParentBlock()), function(function), deleter(deleter),
        calleeAnalysis(calleeAnalysis) {
    assert(value->isLexical());
    assert(value->getOwnershipKind() == OwnershipKind::Owned);
  }
  Context(Context const &) = delete;
  Context &operator=(Context const &) = delete;
};

/// How %value gets used.
struct Usage final {
  /// Instructions which are users of the simple (i.e. not reborrowed) value.
  SmallPtrSet<SILInstruction *, 16> users;
  // The instructions from which the hoisting starts, the destroy_values.
  llvm::SmallSetVector<SILInstruction *, 4> ends;

  Usage(){};
  Usage(Usage const &) = delete;
  Usage &operator=(Usage const &) = delete;
};

/// Identify users and destroy_values of %value.
///
/// returns true if all uses were found
///         false otherwise
bool findUsage(Context const &context, Usage &usage) {
  SmallVector<Operand *, 16> uses;
  if (!findUsesOfSimpleValue(context.value, &uses)) {
    // If the value escapes, don't hoist.
    return false;
  }
  for (auto *use : uses) {
    // Add the destroy_values to the collection of ends so we can seed the data
    // flow and determine whether any were reused.  They aren't uses over which
    // we can't hoist though.
    auto dv = dyn_cast<DestroyValueInst>(use->getUser());
    if (dv && dv->getOperand() == context.value) {
      usage.ends.insert(use->getUser());
    } else {
      usage.users.insert(use->getUser());
    }
  }
  return true;
}

/// How destroy_value hoisting is obstructed.
struct DeinitBarriers final {
  /// Instructions above which destroy_values cannot be hoisted.
  SmallVector<SILInstruction *, 4> instructions;

  /// Blocks one of whose phis is a barrier and consequently out of which
  /// destroy_values cannot be hoisted.
  SmallVector<SILBasicBlock *, 4> phis;

  SmallVector<SILBasicBlock *, 4> blocks;

  DeinitBarriers(Context &context) {}
  DeinitBarriers(DeinitBarriers const &) = delete;
  DeinitBarriers &operator=(DeinitBarriers const &) = delete;
};

class BarrierAccessScopeFinder;

/// Works backwards from the current location of destroy_values to the earliest
/// place they can be hoisted to.
///
/// Implements IterativeBackwardReachability::Effects
/// Implements IterativeBackwardReachability::bindBarriers::Visitor
/// Implements VisitBarrierAccessScopes::Effects
class Dataflow final {
  using Reachability = IterativeBackwardReachability<Dataflow>;
  using Effect = Reachability::Effect;
  Context const &context;
  Usage const &uses;
  DeinitBarriers &barriers;
  Reachability::Result result;
  Reachability reachability;
  SmallPtrSet<BeginAccessInst *, 8> barrierAccessScopes;

  enum class Classification { Barrier, Other };

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

  /// IterativeBackwardReachability::bindBarriers::Visitor

  void visitBarrierInstruction(SILInstruction *instruction) {
    barriers.instructions.push_back(instruction);
  }

  void visitBarrierPhi(SILBasicBlock *block) { barriers.phis.push_back(block); }

  void visitBarrierBlock(SILBasicBlock *block) {
    barriers.blocks.push_back(block);
  }

  void visitInitialBlock(SILBasicBlock *block) {
    barriers.blocks.push_back(block);
  }
};

Dataflow::Classification
Dataflow::classifyInstruction(SILInstruction *instruction) {
  if (instruction == context.definition) {
    return Classification::Barrier;
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
  case Classification::Other:
    return false;
  }
  llvm_unreachable("exhaustive switch not exhaustive?!");
}

Dataflow::Effect Dataflow::effectForInstruction(SILInstruction *instruction) {
  if (uses.ends.contains(instruction))
    return Effect::Gen();
  auto classification = classifyInstruction(instruction);
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
/// access scopes they contain barriers to hoisting.  Hoisting destroy_values
/// into such access scopes could introduce exclusivity violations.
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
  reachability.findBarriers(*this);
}

/// Hoist the destroy_values of %value.
class Rewriter final {
  Context &context;
  Usage const &uses;
  DeinitBarriers const &barriers;

  /// The destroy_value instructions for this owned lexical value that existed
  /// before LexicalDestroyHoisting ran and which were not modified.
  llvm::SmallPtrSet<SILInstruction *, 8> reusedDestroyValueInsts;

public:
  Rewriter(Context &context, Usage const &uses, DeinitBarriers const &barriers)
      : context(context), uses(uses), barriers(barriers) {}
  Rewriter(Rewriter const &) = delete;
  Rewriter &operator=(Rewriter const &) = delete;

  bool run();

private:
  bool createDestroyValue(SILInstruction *insertionPoint);
};

bool Rewriter::run() {
  bool madeChange = false;

  // Add destroy_values for phi barrier boundaries.
  //
  // A block is a phi barrier iff any of its predecessors' terminators get
  // classified as barriers.
  for (auto *block : barriers.phis) {
    madeChange |= createDestroyValue(&block->front());
  }

  // Add destroy_values for barrier boundaries.
  //
  // Insert destroy_values after every non-terminator barrier.
  //
  // For terminator barriers, add destroy_values at the beginning of the
  // successor blocks.  In order to reach a terminator and classify it as a
  // barrier, all of a block P's successors B had reachable beginnings.  If any
  // of them didn't, then BackwardReachability::meetOverSuccessors would never
  // have returned true for P, so none of its instructions would ever have been
  // classified (except for via checkReachablePhiBarrier, which doesn't record
  // terminator barriers).
  for (auto instruction : barriers.instructions) {
    if (auto *terminator = dyn_cast<TermInst>(instruction)) {
      auto successors = terminator->getParentBlock()->getSuccessorBlocks();
      for (auto *successor : successors) {
        madeChange |= createDestroyValue(&successor->front());
      }
    } else {
      auto *next = instruction->getNextInstruction();
      assert(next);
      madeChange |= createDestroyValue(next);
    }
  }

  // Add destroy_values for control-flow boundaries.
  //
  // Insert destroy_values at the beginning of blocks which were preceded by a
  // control flow branch (and which, thanks to the lack of critical edges,
  // don't have multiple predecessors) whose end was not reachable (because
  // reachability was not able to make it to the top of some other successor).
  //
  // In other words, a control flow boundary is the target edge from a block B
  // to its single predecessor P not all of whose successors S in succ(P) had
  // reachable beginnings.  We witness that fact about P's successors by way of
  // P not having a reachable end--see BackwardReachability::meetOverSuccessors.
  //
  // control-flow-boundary(B) := beginning-reachable(B) && !end-reachable(P)
  for (auto *block : barriers.blocks) {
    madeChange |= createDestroyValue(&block->front());
  }

  if (madeChange) {
    // Remove all the original destroy_values instructions.
    for (auto *end : uses.ends) {
      if (reusedDestroyValueInsts.contains(end)) {
        continue;
      }
      context.deleter.forceDelete(end);
    }
  }

  return madeChange;
}

bool Rewriter::createDestroyValue(SILInstruction *insertionPoint) {
  if (auto *ebi = dyn_cast<DestroyValueInst>(insertionPoint)) {
    if (llvm::find(uses.ends, insertionPoint) != uses.ends.end()) {
      reusedDestroyValueInsts.insert(insertionPoint);
      return false;
    }
  }
  auto builder = SILBuilderWithScope(insertionPoint);
  builder.createDestroyValue(
      RegularLocation::getAutoGeneratedLocation(insertionPoint->getLoc()),
      context.value);
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
} // end namespace LexicalDestroyHoisting

bool swift::hoistDestroysOfOwnedLexicalValue(
    SILValue const value, SILFunction &function, InstructionDeleter &deleter,
    BasicCalleeAnalysis *calleeAnalysis) {
  if (!value->isLexical())
    return false;
  if (value->getOwnershipKind() != OwnershipKind::Owned)
    return false;
  LexicalDestroyHoisting::Context context(value, function, deleter,
                                          calleeAnalysis);
  return LexicalDestroyHoisting::run(context);
}

namespace swift::test {
// Arguments:
// - bool: pruneDebug
// - bool: maximizeLifetimes
// - bool: "respectAccessScopes", whether to contract lifetimes to end within
//         access scopes which they previously enclosed but can't be hoisted
//         before
// - SILValue: value to canonicalize
// Dumps:
// - function after value canonicalization
static FunctionTest LexicalDestroyHoistingTest(
    "lexical_destroy_hoisting",
    [](auto &function, auto &arguments, auto &test) {
      auto *calleeAnalysis = test.template getAnalysis<BasicCalleeAnalysis>();
      InstructionDeleter deleter;
      auto value = arguments.takeValue();
      hoistDestroysOfOwnedLexicalValue(value, *value->getFunction(), deleter,
                                       calleeAnalysis);
      function.print(llvm::outs());
    });
} // end namespace swift::test
