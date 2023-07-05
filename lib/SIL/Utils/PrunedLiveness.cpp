//===--- PrunedLiveness.cpp - Compute liveness from selected uses ---------===//
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

#include "swift/SIL/PrunedLiveness.h"
#include "swift/AST/TypeExpansionContext.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/ScopedAddressUtils.h"
#include "swift/SIL/Test.h"

using namespace swift;

void PrunedLiveBlocks::computeUseBlockLiveness(SILBasicBlock *userBB) {
  // If, we are visiting this block, then it is not already LiveOut. Mark it
  // LiveWithin to indicate a liveness boundary within the block.
  markBlockLive(userBB, LiveWithin);

  BasicBlockWorklist worklist(userBB->getFunction());
  worklist.push(userBB);

  while (auto *block = worklist.pop()) {
    // The popped `bb` is live; now mark all its predecessors LiveOut.
    //
    // Traversal terminates at any previously visited block, including the
    // blocks initialized as definition blocks.
    for (auto *predBlock : block->getPredecessorBlocks()) {
      switch (getBlockLiveness(predBlock)) {
      case Dead:
        worklist.pushIfNotVisited(predBlock);
        LLVM_FALLTHROUGH;
      case LiveWithin:
        markBlockLive(predBlock, LiveOut);
        break;
      case LiveOut:
        break;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                    PrunedLiveBlocks and PrunedLiveness
//===----------------------------------------------------------------------===//

llvm::StringRef PrunedLiveBlocks::getStringRef(IsLive isLive) const {
  switch (isLive) {
  case Dead:
    return "Dead";
  case LiveWithin:
    return "LiveWithin";
  case LiveOut:
    return "LiveOut";
  }
}

void PrunedLiveBlocks::print(llvm::raw_ostream &OS) const {
  if (!discoveredBlocks) {
    OS << "No deterministic live block list\n";
    return;
  }
  SmallVector<IsLive, 8> isLive;
  for (auto *block : *discoveredBlocks) {
    block->printAsOperand(OS);
    OS << ": " << getStringRef(this->getBlockLiveness(block)) << "\n";
  }
}

void PrunedLiveBlocks::dump() const {
  print(llvm::dbgs());
}

void PrunedLiveness::print(llvm::raw_ostream &OS) const {
  liveBlocks.print(OS);
  for (auto &userAndIsLifetimeEnding : users) {
    if (userAndIsLifetimeEnding.second)
      OS << "lifetime-ending user: ";
    else
      OS << "regular user: ";
    userAndIsLifetimeEnding.first->print(OS);
  }
}

void PrunedLiveness::dump() const {
  print(llvm::dbgs());
}

//===----------------------------------------------------------------------===//
//                           PrunedLivenessBoundary
//===----------------------------------------------------------------------===//

void PrunedLivenessBoundary::print(llvm::raw_ostream &OS) const {
  for (auto *user : lastUsers) {
    OS << "last user: " << *user;
  }
  for (auto *block : boundaryEdges) {
    OS << "boundary edge: ";
    block->printAsOperand(OS);
    OS << "\n";
  }
  if (!deadDefs.empty()) {
    for (auto *deadDef : deadDefs) {
      OS << "dead def: " << *deadDef;
    }
  }
}

void PrunedLivenessBoundary::dump() const {
  print(llvm::dbgs());
}

void PrunedLivenessBoundary::visitInsertionPoints(
    llvm::function_ref<void(SILBasicBlock::iterator insertPt)> visitor,
    DeadEndBlocks *deBlocks) {
  // Control flow merge blocks used as insertion points.
  SmallPtrSet<SILBasicBlock *, 4> mergeBlocks;

  for (SILInstruction *user : lastUsers) {
    if (!isa<TermInst>(user)) {
      visitor(std::next(user->getIterator()));
      continue;
    }
    auto *predBB = user->getParent();
    for (SILBasicBlock *succ : predBB->getSuccessors()) {
      if (!succ->getSinglePredecessorBlock()) {
        assert(predBB->getSingleSuccessorBlock() == succ);
        if (!mergeBlocks.insert(succ).second) {
          continue;
        }
      } else {
        assert(succ->getSinglePredecessorBlock() == predBB);
      }
      if (deBlocks && deBlocks->isDeadEnd(succ))
        continue;

      visitor(succ->begin());
    }
  }
  for (SILBasicBlock *edge : boundaryEdges) {
    if (deBlocks && deBlocks->isDeadEnd(edge))
      continue;

    visitor(edge->begin());
  }
  for (SILNode *deadDef : deadDefs) {
    if (auto *arg = dyn_cast<SILArgument>(deadDef))
      visitor(arg->getParent()->begin());
    else
      visitor(std::next(cast<SILInstruction>(deadDef)->getIterator()));
  }
}

namespace swift::test {
// Arguments:
// - variadic list of - instruction: a last user
// Dumps:
// - the insertion points
static FunctionTest
    PrunedLivenessBoundaryWithListOfLastUsersInsertionPointsTest(
        "pruned-liveness-boundary-with-list-of-last-users-insertion-points",
        [](auto &function, auto &arguments, auto &test) {
          PrunedLivenessBoundary boundary;
          while (arguments.hasUntaken()) {
            boundary.lastUsers.push_back(arguments.takeInstruction());
          }
          boundary.visitInsertionPoints(
              [](SILBasicBlock::iterator point) { point->dump(); });
        });
} // end namespace swift::test

//===----------------------------------------------------------------------===//
//                              PrunedLiveRange
//===----------------------------------------------------------------------===//

template <typename LivenessWithDefs>
void PrunedLiveRange<LivenessWithDefs>::updateForUse(SILInstruction *user,
                                                     bool lifetimeEnding) {
  liveBlocks.updateForUse(user, asImpl().isUserBeforeDef(user));

  // Note that a user may use the current value from multiple operands. If any
  // of the uses are non-lifetime-ending, then we must consider the user
  // itself non-lifetime-ending; it cannot be a final destroy point because
  // the value of the non-lifetime-ending operand must be kept alive until the
  // end of the user. Consider a call that takes the same value using
  // different conventions:
  //
  //   apply %f(%val, %val) : $(@guaranteed, @owned) -> ()
  //
  // This call is not considered the end of %val's lifetime. The @owned
  // argument must be copied.
  auto iterAndSuccess = users.insert({user, lifetimeEnding});
  if (!iterAndSuccess.second)
    iterAndSuccess.first->second &= lifetimeEnding;
}

template <typename LivenessWithDefs>
InnerBorrowKind
PrunedLiveRange<LivenessWithDefs>::updateForBorrowingOperand(Operand *operand) {
  assert(operand->getOperandOwnership() == OperandOwnership::Borrow);

  // A nested borrow scope is considered a use-point at each scope ending
  // instruction.
  //
  // Note: Ownership liveness should follow reborrows that are dominated by the
  // ownership definition.
  if (!BorrowingOperand(operand).visitScopeEndingUses([this](Operand *end) {
        if (end->getOperandOwnership() == OperandOwnership::Reborrow) {
          return false;
        }
        updateForUse(end->getUser(), /*lifetimeEnding*/ false);
        return true;
      })) {
    return InnerBorrowKind::Reborrowed;
  }
  return InnerBorrowKind::Contained;
}

template <typename LivenessWithDefs>
AddressUseKind PrunedLiveRange<LivenessWithDefs>::checkAndUpdateInteriorPointer(
    Operand *operand) {
  assert(operand->getOperandOwnership() == OperandOwnership::InteriorPointer);

  if (auto scopedAddress = ScopedAddressValue::forUse(operand)) {
    scopedAddress.visitScopeEndingUses([this](Operand *end) {
      updateForUse(end->getUser(), /*lifetimeEnding*/ false);
      return true;
    });
    return AddressUseKind::NonEscaping;
  }
  // FIXME: findTransitiveUses should be a visitor so we're not recursively
  // allocating use vectors and potentially merging the use points.
  SmallVector<Operand *, 8> uses;
  auto useKind = InteriorPointerOperand(operand).findTransitiveUses(&uses);
  for (auto *use : uses) {
    updateForUse(use->getUser(), /*lifetimeEnding*/ false);
  }
  if (uses.empty()) {
    // Handle a dead address
    updateForUse(operand->getUser(), /*lifetimeEnding*/ false);
  }
  return useKind;
}

template <typename LivenessWithDefs>
void PrunedLiveRange<LivenessWithDefs>::extendAcrossLiveness(
    PrunedLiveness &otherLiveness) {
  // update this liveness for all the interesting users in otherLiveness.
  for (std::pair<SILInstruction *, bool> userAndEnd :
       otherLiveness.getAllUsers()) {
    updateForUse(userAndEnd.first, userAndEnd.second);
  }
}

template <typename LivenessWithDefs>
LiveRangeSummary PrunedLiveRange<LivenessWithDefs>::updateForDef(SILValue def) {
  ValueSet visited(def->getFunction());
  return recursivelyUpdateForDef(def, visited, def);
}

template <typename LivenessWithDefs>
LiveRangeSummary PrunedLiveRange<LivenessWithDefs>::recursivelyUpdateForDef(
    SILValue initialDef, ValueSet &visited, SILValue value) {
  LiveRangeSummary summary;

  if (!visited.insert(value))
    return summary;

  // Note: Uses with OperandOwnership::NonUse cannot be considered normal uses
  // for liveness. Otherwise, liveness would need to separately track non-uses
  // everywhere. Non-uses cannot be treated like normal non-lifetime-ending uses
  // because they can occur on both applies, which need to extend liveness to
  // the return point, and on forwarding instructions, like
  // init_existential_ref, which need to consume their use even when
  // type-dependent operands exist.
  for (Operand *use : value->getUses()) {
    switch (use->getOperandOwnership()) {
    case OperandOwnership::NonUse:
      break;
    case OperandOwnership::Borrow:
      summary.meet(updateForBorrowingOperand(use));
      break;
    case OperandOwnership::PointerEscape:
      summary.meet(AddressUseKind::PointerEscape);
      break;
    case OperandOwnership::InteriorPointer:
      summary.meet(checkAndUpdateInteriorPointer(use));
      break;
    case OperandOwnership::GuaranteedForwarding: {
      updateForUse(use->getUser(), /*lifetimeEnding*/false);
      if (auto phiOper = PhiOperand(use)) {
        SILValue phi = phiOper.getValue();
        // If 'def' is any of the enclosing defs, then it must dominate the phi
        // and all phi uses should be handled recursively.
        if (!visitEnclosingDefs(phi, [initialDef](SILValue enclosingDef) {
          return enclosingDef != initialDef;
        })) {
          // At least one enclosing def was 'def'.
          summary.meet(recursivelyUpdateForDef(initialDef, visited, phi));
        }
        // Otherwise all enclosing defs are protected by separate reborrow
        // scopes, which are not included in "simple" liveness.
        break;
      }
      ForwardingOperand(use).visitForwardedValues([&](SILValue result) {
        // Do not include transitive uses with 'none' ownership
        if (result->getOwnershipKind() != OwnershipKind::None) {
          summary.meet(recursivelyUpdateForDef(initialDef, visited, result));
        }
        return true;
      });
      break;
    }
    case OperandOwnership::TrivialUse: {
      if (auto scopedAddress = ScopedAddressValue::forUse(use)) {
        scopedAddress.visitScopeEndingUses([this](Operand *end) {
          updateForUse(end->getUser(), /*lifetimeEnding*/false);
          return true;
        });
      }
      updateForUse(use->getUser(), /*lifetimeEnding*/false);
      break;
    }
    default:
      // Note: An outer reborrow ends the outer lifetime here.
      updateForUse(use->getUser(), use->isLifetimeEnding());
      break;
    }
  }
  return summary;
}

namespace swift::test {
// Arguments:
// - SILValue: value to a analyze
// Dumps:
// - the liveness result and boundary
static FunctionTest SSALivenessTest("ssa-liveness", [](auto &function,
                                                       auto &arguments,
                                                       auto &test) {
  auto value = arguments.takeValue();
  assert(!arguments.hasUntaken());
  llvm::outs() << "SSA lifetime analysis: " << value;

  SmallVector<SILBasicBlock *, 8> discoveredBlocks;
  SSAPrunedLiveness liveness(value->getFunction(), &discoveredBlocks);
  liveness.initializeDef(value);
  LiveRangeSummary summary = liveness.computeSimple();
  if (summary.innerBorrowKind == InnerBorrowKind::Reborrowed)
    llvm::outs() << "Incomplete liveness: Reborrowed inner scope\n";

  if (summary.addressUseKind == AddressUseKind::PointerEscape)
    llvm::outs() << "Incomplete liveness: Escaping address\n";
  else if (summary.addressUseKind == AddressUseKind::Unknown)
    llvm::outs() << "Incomplete liveness: Unknown address use\n";

  liveness.print(llvm::outs());

  PrunedLivenessBoundary boundary;
  liveness.computeBoundary(boundary);
  boundary.print(llvm::outs());
});
} // end namespace swift::test

template <typename LivenessWithDefs>
bool PrunedLiveRange<LivenessWithDefs>::isWithinBoundary(
    SILInstruction *inst) const {
  assert(asImpl().isInitialized());

  auto *block = inst->getParent();
  auto blockLiveness = getBlockLiveness(block);
  if (blockLiveness == PrunedLiveBlocks::Dead)
    return false;

  bool isLive = blockLiveness == PrunedLiveBlocks::LiveOut;

  if (isLive && !asImpl().isDefBlock(block))
    return true;

  // Check if instruction is between a last use and a definition
  for (SILInstruction &it : llvm::reverse(*block)) {
    // the def itself is not within the boundary, so cancel liveness before
    // matching 'inst'.
    if (asImpl().isDef(&it)) {
      isLive = false;
    }
    if (&it == inst) {
      return isLive;
    }
    if (!isLive && isInterestingUser(&it)) {
      isLive = true;
    }
  }
  llvm_unreachable("instruction must be in its parent block");
}

template <typename LivenessWithDefs>
bool PrunedLiveRange<LivenessWithDefs>::areUsesWithinBoundary(
    ArrayRef<Operand *> uses, DeadEndBlocks *deadEndBlocks) const {
  assert(asImpl().isInitialized());

  auto checkDeadEnd = [deadEndBlocks](SILInstruction *inst) {
    return deadEndBlocks && deadEndBlocks->isDeadEnd(inst->getParent());
  };
  for (auto *use : uses) {
    auto *user = use->getUser();
    if (!asImpl().isWithinBoundary(user) && !checkDeadEnd(user))
      return false;
  }
  return true;
}

template <typename LivenessWithDefs>
bool PrunedLiveRange<LivenessWithDefs>::areUsesOutsideBoundary(
    ArrayRef<Operand *> uses, DeadEndBlocks *deadEndBlocks) const {
  assert(asImpl().isInitialized());

  auto checkDeadEnd = [deadEndBlocks](SILInstruction *inst) {
    return deadEndBlocks && deadEndBlocks->isDeadEnd(inst->getParent());
  };
  for (auto *use : uses) {
    auto *user = use->getUser();
    if (asImpl().isWithinBoundary(user) || checkDeadEnd(user))
      return false;
  }
  return true;
}

template <typename LivenessWithDefs>
void PrunedLiveRange<LivenessWithDefs>::computeBoundary(
    PrunedLivenessBoundary &boundary) const {
  assert(asImpl().isInitialized());

  for (SILBasicBlock *block : getDiscoveredBlocks()) {
    // Process each block that has not been visited and is not LiveOut.
    switch (getBlockLiveness(block)) {
    case PrunedLiveBlocks::LiveOut:
      for (SILBasicBlock *succBB : block->getSuccessors()) {
        if (getBlockLiveness(succBB) == PrunedLiveBlocks::Dead) {
          boundary.boundaryEdges.push_back(succBB);
        }
      }
      asImpl().findBoundariesInBlock(block, /*isLiveOut*/ true, boundary);
      break;
    case PrunedLiveBlocks::LiveWithin: {
      asImpl().findBoundariesInBlock(block, /*isLiveOut*/ false, boundary);
      break;
    }
    case PrunedLiveBlocks::Dead:
      llvm_unreachable("All discovered blocks must be live");
    }
  }
}

template <typename LivenessWithDefs>
void PrunedLiveRange<LivenessWithDefs>::computeBoundary(
    PrunedLivenessBoundary &boundary,
    ArrayRef<SILBasicBlock *> postDomBlocks) const {
  assert(asImpl().isInitialized());

  if (postDomBlocks.empty())
    return; // all paths must be dead-ends or infinite loops

  BasicBlockWorklist blockWorklist(postDomBlocks[0]->getParent());

  // Visit each post-dominating block as the starting point for a
  // backward CFG traversal.
  for (auto *block : postDomBlocks) {
    blockWorklist.push(block);
  }
  while (auto *block = blockWorklist.pop()) {
    // Process each block that has not been visited and is not LiveOut.
    switch (getBlockLiveness(block)) {
    case PrunedLiveBlocks::LiveOut:
      asImpl().findBoundariesInBlock(block, /*isLiveOut*/ true, boundary);
      break;
    case PrunedLiveBlocks::LiveWithin: {
      asImpl().findBoundariesInBlock(block, /*isLiveOut*/ false, boundary);
      break;
    }
    case PrunedLiveBlocks::Dead:
      // Continue searching upward to find the pruned liveness boundary.
      for (auto *predBB : block->getPredecessorBlocks()) {
        if (getBlockLiveness(predBB) == PrunedLiveBlocks::LiveOut) {
          boundary.boundaryEdges.push_back(block);
        } else {
          blockWorklist.pushIfNotVisited(predBB);
        }
      }
      break;
    }
  }
}

namespace swift {
template class PrunedLiveRange<SSAPrunedLiveness>;
template class PrunedLiveRange<MultiDefPrunedLiveness>;
} // namespace swift

//===----------------------------------------------------------------------===//
//                             SSAPrunedLiveness
//===----------------------------------------------------------------------===//

/// Given live-within (non-live-out) \p block, find the last user.
void findBoundaryInNonDefBlock(SILBasicBlock *block,
                               PrunedLivenessBoundary &boundary,
                               const PrunedLiveness &liveness) {
  assert(liveness.getBlockLiveness(block) == PrunedLiveBlocks::LiveWithin);

  for (SILInstruction &inst : llvm::reverse(*block)) {
    if (liveness.isInterestingUser(&inst)) {
      boundary.lastUsers.push_back(&inst);
      return;
    }
  }
  llvm_unreachable("live-within block must contain an interesting use");
}

/// Given a live-within \p block that contains an SSA definition, and knowledge
/// that all live uses are dominated by that single definition, find either the
/// last user or a dead def.
///
/// A live range with a single definition cannot have any uses above that
/// definition in the same block. This even holds for unreachable self-loops.
void findBoundaryInSSADefBlock(SILNode *ssaDef,
                               PrunedLivenessBoundary &boundary,
                               const PrunedLiveness &liveness) {
  // defInst is null for argument defs.
  SILInstruction *defInst = dyn_cast<SILInstruction>(ssaDef);
  for (SILInstruction &inst : llvm::reverse(*ssaDef->getParentBlock())) {
    if (&inst == defInst) {
      boundary.deadDefs.push_back(cast<SILNode>(&inst));
      return;
    }
    if (liveness.isInterestingUser(&inst)) {
      boundary.lastUsers.push_back(&inst);
      return;
    }
  }
  auto *deadArg = dyn_cast<SILArgument>(ssaDef);
  assert(deadArg
         && "findBoundariesInBlock must be called on a live block");
  boundary.deadDefs.push_back(deadArg);
}

void SSAPrunedLiveness::findBoundariesInBlock(
    SILBasicBlock *block, bool isLiveOut,
    PrunedLivenessBoundary &boundary) const {
  assert(isInitialized());

  // For SSA, a live-out block cannot have a boundary.
  if (isLiveOut)
    return;

  // Handle live-within block
  if (!isDefBlock(block)) {
    findBoundaryInNonDefBlock(block, boundary, *this);
    return;
  }
  // Find either the last user or a dead def
  auto *defInst = def->getDefiningInstruction();
  SILNode *defNode = defInst ? cast<SILNode>(defInst) : cast<SILArgument>(def);
  findBoundaryInSSADefBlock(defNode, boundary, *this);
}

//===----------------------------------------------------------------------===//
//                           MultiDefPrunedLiveness
//===----------------------------------------------------------------------===//

bool MultiDefPrunedLiveness::isUserBeforeDef(SILInstruction *user) const {
  auto *block = user->getParent();
  if (!isDefBlock(block))
    return false;

  if (llvm::any_of(block->getArguments(), [this](SILArgument *arg) {
    return isDef(arg);
  })) {
    return false;
  }

  auto *current = user;
  while (true) {
    // If user is also a def, then the use is considered before the def.
    current = current->getPreviousInstruction();
    if (!current)
      return true;

    if (isDef(current))
      return false;
  }
}

namespace swift::test {
// Arguments:
// - the string "defs:"
// - list of live-range defining values or instructions
// - the string "uses:"
// - variadic list of live-range user instructions
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by considering only the
// specified uses. The actual uses of the def nodes are ignored.
//
// This is useful for testing non-ssa liveness, for example, of memory
// locations. In that case, the def nodes may be stores and the uses may be
// destroy_addrs.
static FunctionTest MultiDefUseLivenessTest(
    "multidefuse-liveness", [](auto &function, auto &arguments, auto &test) {
      SmallVector<SILBasicBlock *, 8> discoveredBlocks;
      MultiDefPrunedLiveness liveness(&function, &discoveredBlocks);

      llvm::outs() << "MultiDef lifetime analysis:\n";
      if (arguments.takeString() != "defs:") {
        llvm::report_fatal_error(
            "test specification expects the 'defs:' label\n");
      }
      while (true) {
        auto argument = arguments.takeArgument();
        if (isa<InstructionArgument>(argument)) {
          auto *instruction = cast<InstructionArgument>(argument).getValue();
          llvm::outs() << "  def instruction: " << *instruction;
          liveness.initializeDef(instruction);
          continue;
        }
        if (isa<ValueArgument>(argument)) {
          SILValue value = cast<ValueArgument>(argument).getValue();
          llvm::outs() << "  def value: " << value;
          liveness.initializeDef(value);
          continue;
        }
        if (cast<StringArgument>(argument).getValue() != "uses:") {
          llvm::report_fatal_error(
              "test specification expects the 'uses:' label\n");
        }
        break;
      }
      while (arguments.hasUntaken()) {
        auto *inst = arguments.takeInstruction();
        // lifetimeEnding has no effects on liveness, it's only a cache for the
        // caller.
        liveness.updateForUse(inst, /*lifetimeEnding*/ false);
      }
      liveness.print(llvm::outs());

      PrunedLivenessBoundary boundary;
      liveness.computeBoundary(boundary);
      boundary.print(llvm::outs());
    });
} // end namespace swift::test

void MultiDefPrunedLiveness::findBoundariesInBlock(
    SILBasicBlock *block, bool isLiveOut,
    PrunedLivenessBoundary &boundary) const {
  assert(isInitialized());

  if (!isDefBlock(block)) {
    // A live-out block with no defs cannot have a boundary.
    if (!isLiveOut) {
      findBoundaryInNonDefBlock(block, boundary, *this);
    }
    return;
  }
  // Handle def blocks...
  //
  // First, check for an SSA live range
  if (++defs.begin() == defs.end()) {
    // For SSA, a live-out block cannot have a boundary.
    if (!isLiveOut) {
      findBoundaryInSSADefBlock(*defs.begin(), boundary, *this);
    }
    return;
  }
  // Handle a live-out or live-within block with potentially multiple defs
  unsigned prevCount = boundary.deadDefs.size() + boundary.lastUsers.size();
  (void)prevCount;

  bool isLive = isLiveOut;
  for (auto &inst : llvm::reverse(*block)) {
    // Check if the instruction is a def before checking whether it is a
    // use. The same instruction can be both a dead def and boundary use.
    if (isDef(&inst)) {
      if (!isLive) {
        boundary.deadDefs.push_back(cast<SILNode>(&inst));
      }
      isLive = false;
    }
    // Note: the same instruction could potentially be both a dead def and last
    // user. The liveness boundary supports this, although it won't happen in
    // any context where we care about inserting code on the boundary.
    if (!isLive && isInterestingUser(&inst)) {
      boundary.lastUsers.push_back(&inst);
      isLive = true;
    }
  }
  if (!isLive) {
    for (SILArgument *deadArg : block->getArguments()) {
      if (defs.contains(deadArg)) {
        boundary.deadDefs.push_back(deadArg);
      }
    }
    if (auto *predBB = block->getSinglePredecessorBlock()) {
      if (getBlockLiveness(predBB) == PrunedLiveBlocks::LiveOut) {
        boundary.boundaryEdges.push_back(block);
      }
    }
  }
  // All live-within blocks must contain a boundary.
  assert(isLiveOut
         || (prevCount < boundary.deadDefs.size() + boundary.lastUsers.size())
         && "findBoundariesInBlock must be called on a live block");
}

LiveRangeSummary MultiDefPrunedLiveness::computeSimple() {
  assert(isInitialized() && "defs uninitialized");

  LiveRangeSummary summary;
  for (SILNode *defNode : defs) {
    if (auto *arg = dyn_cast<SILArgument>(defNode))
      summary.meet(updateForDef(arg));
    else {
      for (auto result : cast<SILInstruction>(defNode)->getResults()) {
        summary.meet(updateForDef(result));
      }
    }
  }
  return summary;
}

namespace swift::test {
// Arguments:
// - variadic list of live-range defining values or instructions
// Dumps:
// - the liveness result and boundary
//
// Computes liveness for the specified def nodes by finding all their direct SSA
// uses. If the def is an instruction, then all results are considered.
static FunctionTest MultiDefLivenessTest(
    "multidef-liveness", [](auto &function, auto &arguments, auto &test) {
      SmallVector<SILBasicBlock *, 8> discoveredBlocks;
      MultiDefPrunedLiveness liveness(&function, &discoveredBlocks);

      llvm::outs() << "MultiDef lifetime analysis:\n";
      while (arguments.hasUntaken()) {
        auto argument = arguments.takeArgument();
        if (isa<InstructionArgument>(argument)) {
          auto *instruction = cast<InstructionArgument>(argument).getValue();
          llvm::outs() << "  def instruction: " << instruction;
          liveness.initializeDef(instruction);
        } else {
          SILValue value = cast<ValueArgument>(argument).getValue();
          llvm::outs() << "  def value: " << value;
          liveness.initializeDef(value);
        }
      }
      liveness.computeSimple();
      liveness.print(llvm::outs());

      PrunedLivenessBoundary boundary;
      liveness.computeBoundary(boundary);
      boundary.print(llvm::outs());
    });
} // end namespace swift::test

//===----------------------------------------------------------------------===//
//                       DiagnosticPrunedLiveness
//===----------------------------------------------------------------------===//

// FIXME: This is wrong. Why is nonLifetimeEndingUsesInLiveOut inside
// PrunedLiveness, and what does it mean? Blocks may transition to LiveOut
// later. Or they may already be LiveOut from a previous use. After computing
// liveness, clients should check uses that are in PrunedLivenessBoundary.
void DiagnosticPrunedLiveness::
updateForUse(SILInstruction *user, bool lifetimeEnding) {
  SSAPrunedLiveness::updateForUse(user, 0);

  auto useBlockLive = getBlockLiveness(user->getParent());
  // Record all uses of blocks on the liveness boundary. For blocks marked
  // LiveWithin, the boundary is considered to be the last use in the block.
  if (!lifetimeEnding && useBlockLive == PrunedLiveBlocks::LiveOut) {
    if (nonLifetimeEndingUsesInLiveOut)
      nonLifetimeEndingUsesInLiveOut->insert(user);
    return;
  }
}
