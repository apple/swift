//===--- ConsumeOperatorCopyableAddressChecker.cpp ------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// NOTE: This pass is assumed to run before all memory optimizations that
/// manipulate the lifetimes of values in memory such as any of the -Onone
/// predictable memory optimizations. allocbox to stack is ok since it doesn't
/// effect the actual memory itself, just whether the memory is boxed or not.
///
/// In this file, we implement a checker that for memory objects in SIL checks
/// that after a call to _move, one can no longer use a var or a let with an
/// address only type. If you use it after that point, but before a known
/// destroy point, you get an error. Example:
///
///   var x = Class()
///   let y = _move(x)
///   _use(x) // error!
///   x = Class()
///   _use(x) // Ok, we reinitialized the memory.
///
/// Below, I describe in detail the algorithm. NOTE: The design below will be
/// updated to support inits once we actually support vars. Currently, we only
/// support lets.
///
/// # Design
///
/// ## Introduction
///
/// At its heart this checker is a dataflow checker that first understands the
/// lifetime of a specific address and then optimizes based off of that
/// information. It uses AccessUseVisitor to reliably get all users of an
/// address. Then it partitions those uses into three sets:
///
///  * A set of mark_unresolved_move_addr. A mark_unresolved_move_addr is an
///    instruction that marks an invocation by _move on a value. Since we have
///    not yet proven using dataflow that it can be a move, semantically this
///    instruction is actually a copy_addr [init] so we maintain a valid IR if
///    we have uses later that we want to error upon. This is where we always
///    begin tracking dataflow.
///
///  * A set of destroy_value operations. These are points where we stop
///    tracking dataflow.
///
///  * A set of misc uses that just require liveness, We call the last category
///    "livenessUses". These are the uses that can not exist in between the move
///    and the destroy_addr operation and if we see any, we emit an error to the
///    user.
///
/// ## Gathering information to prepare for the Dataflow
///
/// We perform several different dataflow operations:
///
/// 1. First mark_unresolved_move_addr are propagated downwards to determine if
///    they propagate downwards out of blocks. When this is done, we perform the
///    single basic block form of this diagnostic. If we emit a diagnostic while
///    doing that, we exit. Otherwise, we know that there must not be any uses
///    or consumes in the given block, so it propagates move out.
///
/// 2. Then mark_unresolved_move_addr and all liveness uses are propagated up to
///    determine if a block propagates liveness up. We always use the earliest
///    user in the block. Once we find that user, we insert it into a
///    DenseMap<SILBasicBlock *, SILInstruction *>. This is so we can emit a
///    nice diagnostic.
///
/// 3. Then we propagate destroy_addr upwards stopping if we see an init. If we
///    do not see an init, then we know that we propagated that destroy upward
///    out of the block. We then insert that into a DenseMap<SILBasicBlock *,
///    DestroyAddrInst *> we are maintaining. NOTE: We do not need to check for
///    moves here since any move in our block would have either resulted in the
///    destroy_addr being eliminated earlier by the single block form of the
///    diagnostic and us exiting early or us emitting an error diagnostic and
///    exiting early.
///
/// NOTE: The reason why we do not track init information on a per block is that
/// in our dataflow, we are treating inits as normal uses and if we see an init
/// without seeing a destroy_addr, we will error on the use and bail. Once we
/// decide to support vars, this will change.
///
/// NOTE: The reason why we do not track all consuming operations, just destroy
/// operations is that instead we are treating a consuming operation as a
/// liveness use. Since we are always going to just exit on the first error for
/// any specific move (all moves will be checked individually of course), we
/// know that we will stop processing blocks at that point.
///
/// ## Performing the Global Dataflow
///
/// Finally using this information we gathered above, for each markMoveAddrOut
/// block individually, we walk the CFG downwards starting with said block's
/// successors looking for liveness uses and destroy_addr blocks.
///
/// 1. If we visit any "liveness block", immediately emit an error diagnostic as
///    the user requested and return. We can not convert the
///    mark_unresolved_move_addr into a move safely.
///
/// 2. If we visit a DestroyAddr block instead, we mark the destroy_addr as
///    being a destroy_addr that is associated with a move. This is done a per
///    address basis.
///
/// Once we have finished visiting mark_unresolved_move_addr, if we found /any/
/// mark_unresolved_move_addr that were safe to convert to a take (and that we
/// did convert to a take), we need to then cleanup destroys.
///
/// ## Cleaning up Destroys
///
/// We do this simultaneously for all of the mark_unresolved_move_addr applied
/// to a single address. Specifically, we place into a worklist all of the
/// predecessor blocks of all destroy_addr blocks that we found while performing
/// global dataflow. Then for each block b until we run out of blocks:
///
/// 1. If b is a block associated with one of our converted
///    mark_unresolved_move_addr, continue. Along that path, we are shortening
///    the lifetime of the address as requested by the user.
///
/// 2. Then we check if b is a block that was not visited when processing the
///    new moves. In such a case, we have found the dominance frontier of one or
///    many of the moves and insert a destroy_addr at the end of the b and
///    continue.
///
/// 3. Finally, if b is not on our dominance frontier and isn't a stopping
///    point, we add its predecessors to the worklist and continue.
///
/// Once these steps have been completed, we delete all of the old destroy_addr
/// since the lifetime of the address has now been handled appropriately along
/// all paths through the program.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-consume-operator-copyable-addresses-checker"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Types.h"
#include "swift/Basic/BlotSetVector.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/PointerSumType.h"

using namespace swift;

static llvm::cl::opt<bool> DisableUnhandledConsumeOperator(
    "sil-consume-operator-disable-unknown-moveaddr-diagnostic");

//===----------------------------------------------------------------------===//
//                                 Utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

static SourceLoc getSourceLocFromValue(SILValue value) {
  if (auto *defInst = value->getDefiningInstruction())
    return defInst->getLoc().getSourceLoc();
  if (auto *arg = dyn_cast<SILFunctionArgument>(value))
    return arg->getDecl()->getLoc();
  llvm_unreachable("Do not know how to get source loc for value?!");
}

#ifndef NDEBUG
static void dumpBitVector(llvm::raw_ostream &os, const SmallBitVector &bv) {
  for (unsigned i = 0; i < bv.size(); ++i) {
    os << (bv[i] ? '1' : '0');
  }
}
#endif

/// Returns true if a value has one or zero debug uses.
static bool hasMoreThanOneDebugUse(SILValue v) {
  auto Range = getDebugUses(v);
  auto i = Range.begin(), e = Range.end();
  if (i == e)
    return false;
  ++i;
  return i != e;
}

//===----------------------------------------------------------------------===//
//                            Forward Declarations
//===----------------------------------------------------------------------===//

namespace {

enum class DownwardScanResult {
  Invalid,
  Destroy,
  Reinit,
  // NOTE: We use UseForDiagnostic both for defer uses and normal uses.
  UseForDiagnostic,
  MoveOut,
  ClosureConsume,
  ClosureUse,
};

struct ClosureOperandState {
  /// This is the downward scan result that visiting a full applysite of this
  /// closure will effect on the address being analyzed.
  DownwardScanResult result = DownwardScanResult::Invalid;

  /// Instructions that act as consumes in the closure callee. This is the set
  /// of earliest post dominating consumes that should be eliminated in the
  /// cloned callee. Only set if state is upwards consume.
  TinyPtrVector<SILInstruction *> pairedConsumingInsts;

  /// The set of instructions in the callee that are uses that require the move
  /// to be alive. Only set if state is upwards use.
  TinyPtrVector<SILInstruction *> pairedUseInsts;

  /// The single debug value in the closure callee that we sink to the reinit
  /// points.
  DebugValueInst *singleDebugValue = nullptr;

  bool isUpwardsUse() const { return result == DownwardScanResult::ClosureUse; }

  bool isUpwardsConsume() const {
    return result == DownwardScanResult::ClosureConsume;
  }
};

} // namespace

/// Is this a reinit instruction that we know how to convert into its init form.
static bool isReinitToInitConvertibleInst(Operand *memUse) {
  auto *memInst = memUse->getUser();
  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *cai = cast<CopyAddrInst>(memInst);
    return !cai->isInitializationOfDest();
  }
  case SILInstructionKind::StoreInst: {
    auto *si = cast<StoreInst>(memInst);
    return si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign;
  }
  }
}

static void convertMemoryReinitToInitForm(SILInstruction *memInst) {
  switch (memInst->getKind()) {
  default:
    llvm_unreachable("unsupported?!");

  case SILInstructionKind::CopyAddrInst: {
    auto *cai = cast<CopyAddrInst>(memInst);
    cai->setIsInitializationOfDest(IsInitialization_t::IsInitialization);
    return;
  }
  case SILInstructionKind::StoreInst: {
    auto *si = cast<StoreInst>(memInst);
    si->setOwnershipQualifier(StoreOwnershipQualifier::Init);
    return;
  }
  }
}

//===----------------------------------------------------------------------===//
//                               Use State
//===----------------------------------------------------------------------===//

namespace {

struct UseState {
  SILValue address;
  SmallVector<MarkUnresolvedMoveAddrInst *, 1> markMoves;
  SmallPtrSet<SILInstruction *, 1> seenMarkMoves;
  SmallSetVector<SILInstruction *, 2> inits;
  SmallSetVector<SILInstruction *, 4> livenessUses;
  SmallBlotSetVector<DestroyAddrInst *, 4> destroys;
  llvm::SmallDenseMap<SILInstruction *, unsigned, 4> destroyToIndexMap;
  SmallBlotSetVector<SILInstruction *, 4> reinits;
  llvm::SmallDenseMap<SILInstruction *, unsigned, 4> reinitToIndexMap;
  llvm::SmallMapVector<Operand *, ClosureOperandState, 1> closureUses;
  llvm::SmallDenseMap<Operand *, unsigned, 1> closureOperandToIndexMap;

  void insertMarkUnresolvedMoveAddr(MarkUnresolvedMoveAddrInst *inst) {
    if (!seenMarkMoves.insert(inst).second)
      return;
    markMoves.emplace_back(inst);
  }

  void insertDestroy(DestroyAddrInst *dai) {
    destroyToIndexMap[dai] = destroys.size();
    destroys.insert(dai);
  }

  void insertReinit(SILInstruction *inst) {
    reinitToIndexMap[inst] = reinits.size();
    reinits.insert(inst);
  }

  void insertClosureOperand(Operand *op) {
    closureOperandToIndexMap[op] = closureUses.size();
    closureUses[op] = {};
  }

  void clear() {
    address = SILValue();
    markMoves.clear();
    seenMarkMoves.clear();
    inits.clear();
    livenessUses.clear();
    destroys.clear();
    destroyToIndexMap.clear();
    reinits.clear();
    reinitToIndexMap.clear();
    closureUses.clear();
    closureOperandToIndexMap.clear();
  }

  SILFunction *getFunction() const { return address->getFunction(); }
};

} // namespace

//===----------------------------------------------------------------------===//
//                                  Dataflow
//===----------------------------------------------------------------------===//

/// Returns true if we are move out, false otherwise. If we find an interesting
/// inst, we return it in foundInst. If no inst is returned, one must continue.
static DownwardScanResult
downwardScanForMoveOut(MarkUnresolvedMoveAddrInst *mvi, UseState &useState,
                       SILInstruction **foundInst, Operand **foundOperand,
                       TinyPtrVector<SILInstruction *> &foundClosureInsts) {
  // Forward scan looking for uses or reinits.
  for (auto &next : llvm::make_range(std::next(mvi->getIterator()),
                                     mvi->getParent()->end())) {
    LLVM_DEBUG(llvm::dbgs() << "DownwardScan. Checking: " << next);

    // If we hit a non-destroy_addr, then we immediately know that we found an
    // error. Return the special result with the next stashed within it.
    if (useState.livenessUses.count(&next) ||
        useState.seenMarkMoves.count(&next) || useState.inits.count(&next)) {
      // Emit a diagnostic error and process the next mark_unresolved_move_addr.
      LLVM_DEBUG(llvm::dbgs() << "SingleBlock liveness user: " << next);
      *foundInst = &next;
      return DownwardScanResult::UseForDiagnostic;
    }

    {
      auto iter = useState.reinitToIndexMap.find(&next);
      if (iter != useState.reinitToIndexMap.end()) {
        LLVM_DEBUG(llvm::dbgs() << "DownwardScan: reinit: " << next);
        *foundInst = &next;
        return DownwardScanResult::Reinit;
      }
    }

    // If we see a destroy_addr, then stop processing since it pairs directly
    // with our move.
    {
      auto iter = useState.destroyToIndexMap.find(&next);
      if (iter != useState.destroyToIndexMap.end()) {
        auto *dai = cast<DestroyAddrInst>(iter->first);
        LLVM_DEBUG(llvm::dbgs() << "DownwardScan: Destroy: " << *dai);
        *foundInst = dai;
        return DownwardScanResult::Destroy;
      }
    }

    // Finally check if we have a closure user that we were able to handle.
    if (auto fas = FullApplySite::isa(&next)) {
      LLVM_DEBUG(llvm::dbgs() << "DownwardScan: ClosureCheck: " << **fas);
      for (auto &op : fas.getArgumentOperands()) {
        auto iter = useState.closureUses.find(&op);
        if (iter == useState.closureUses.end()) {
          continue;
        }

        LLVM_DEBUG(llvm::dbgs()
                   << "DownwardScan: ClosureCheck: Matching Operand: "
                   << fas.getAppliedArgIndex(op));
        *foundInst = &next;
        *foundOperand = &op;
        switch (iter->second.result) {
        case DownwardScanResult::Invalid:
        case DownwardScanResult::Destroy:
        case DownwardScanResult::Reinit:
        case DownwardScanResult::UseForDiagnostic:
        case DownwardScanResult::MoveOut:
          llvm_unreachable("unhandled");
        case DownwardScanResult::ClosureConsume:
          LLVM_DEBUG(llvm::dbgs() << ". ClosureConsume.\n");
          llvm::copy(iter->second.pairedConsumingInsts,
                     std::back_inserter(foundClosureInsts));
          break;
        case DownwardScanResult::ClosureUse:
          LLVM_DEBUG(llvm::dbgs() << ". ClosureUse.\n");
          llvm::copy(iter->second.pairedUseInsts,
                     std::back_inserter(foundClosureInsts));
          break;
        }
        return iter->second.result;
      }
    }
  }

  // We are move out!
  LLVM_DEBUG(llvm::dbgs() << "DownwardScan. We are move out!\n");
  return DownwardScanResult::MoveOut;
}

/// Scan backwards from \p inst to the beginning of its parent block looking for
/// uses. We return true if \p inst is the first use that we are tracking for
/// the given block. This means it propagates liveness upwards through the CFG.
///
/// This works only for an instruction expected to be a normal use.
static bool upwardScanForUseOut(SILInstruction *inst, UseState &useState) {
  // We scan backwards from the instruction before \p inst to the beginning of
  // the block.
  for (auto &iter : llvm::make_range(std::next(inst->getReverseIterator()),
                                     inst->getParent()->rend())) {
    // If we hit another liveness use, then this isn't the first use in the
    // block. We want to store only the first use in the block. In such a case,
    // we bail since when we visit that earlier instruction, we will do the
    // appropriate check.
    if (useState.livenessUses.contains(&iter))
      // If we are not tracking a destroy, we stop at liveness uses. If we have
      // a destroy_addr, we use the destroy blocks to ignore the liveness uses
      // since we use the destroy_addr to signal we should stop tracking when we
      // use dataflow and to pair/delete with a move.
      return false;
    if (useState.destroyToIndexMap.count(&iter))
      return false;
    if (auto *mmai = dyn_cast<MarkUnresolvedMoveAddrInst>(&iter))
      if (useState.seenMarkMoves.count(mmai))
        return false;
    if (useState.inits.contains(&iter))
      return false;
    if (useState.reinitToIndexMap.count(&iter))
      return false;
    if (auto fas = FullApplySite::isa(&iter)) {
      for (auto &op : fas.getArgumentOperands()) {
        if (useState.closureUses.find(&op) != useState.closureUses.end())
          return false;
      }
    }
  }
  return true;
}

/// Scan backwards from \p inst to the beginning of its parent block looking for
/// uses. We return true if \p inst is the first use that we are tracking for
/// the given block. This means it propagates liveness upwards through the CFG.
static bool upwardScanForDestroys(SILInstruction *inst, UseState &useState) {
  // We scan backwards from the instruction before \p inst to the beginning of
  // the block.
  for (auto &iter : llvm::make_range(std::next(inst->getReverseIterator()),
                                     inst->getParent()->rend())) {
    // If we find a destroy_addr earlier in the block, do not mark this block as
    // being associated with this destroy. We always want to associate the move
    // with the earliest destroy_addr.
    if (useState.destroyToIndexMap.count(&iter))
      return false;
    if (useState.reinitToIndexMap.count(&iter))
      return false;
    // If we see an init, then we return found other use to not track this
    // destroy_addr up since it is balanced by the init.
    if (useState.inits.contains(&iter))
      return false;
    if (auto fas = FullApplySite::isa(&iter)) {
      for (auto &op : fas.getArgumentOperands()) {
        if (useState.closureUses.find(&op) != useState.closureUses.end())
          return false;
      }
    }

    // Otherwise, we have a normal use, just ignore it.
  }

  // Ok, this instruction is the first use in the block of our value. So return
  // true so we track it as such.
  return true;
}

/// Search for the first init in the block.
static bool upwardScanForInit(SILInstruction *inst, UseState &useState) {
  // We scan backwards from the instruction before \p inst to the beginning of
  // the block.
  for (auto &iter : llvm::make_range(std::next(inst->getReverseIterator()),
                                     inst->getParent()->rend())) {
    if (useState.inits.contains(&iter))
      return false;
    if (auto fas = FullApplySite::isa(&iter)) {
      for (auto &op : fas.getArgumentOperands()) {
        if (useState.closureUses.find(&op) != useState.closureUses.end())
          return false;
      }
    }
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                      Closure Argument Global Dataflow
//===----------------------------------------------------------------------===//

namespace {

/// A utility class that analyzes a closure that captures a moved value. It is
/// used to perform move checking within the closure as well as to determine a
/// set of reinit/destroys that we will need to convert to init and or eliminate
/// while cloning the closure.
///
/// NOTE: We do not need to consider if the closure reinitializes the memory
/// since there must be some sort of use for the closure to even reference it
/// and the compiler emits assigns when it reinitializes vars this early in the
/// pipeline.
struct ClosureArgDataflowState {
  SmallVector<SILInstruction *, 32> livenessWorklist;
  SmallVector<SILInstruction *, 32> consumingWorklist;
  MultiDefPrunedLiveness livenessForConsumes;
  UseState &useState;

public:
  ClosureArgDataflowState(SILFunction *function, UseState &useState)
      : livenessForConsumes(function), useState(useState) {}

  bool process(
      SILArgument *arg, ClosureOperandState &state,
      SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers);

private:
  /// Perform our liveness dataflow. Returns true if we found any liveness uses
  /// at all. These we will need to error upon.
  bool performLivenessDataflow(const BasicBlockSet &initBlocks,
                               const BasicBlockSet &livenessBlocks,
                               const BasicBlockSet &consumingBlocks);

  /// Perform our consuming dataflow. Returns true if we found an earliest set
  /// of consuming uses that we can handle that post-dominate the argument.
  /// Returns false otherwise.
  bool performConsumingDataflow(const BasicBlockSet &initBlocks,
                                const BasicBlockSet &consumingBlocks);

  void classifyUses(BasicBlockSet &initBlocks, BasicBlockSet &livenessBlocks,
                    BasicBlockSet &consumingBlocks);

  bool handleSingleBlockCase(SILArgument *address, ClosureOperandState &state);
};

} // namespace

bool ClosureArgDataflowState::handleSingleBlockCase(
    SILArgument *address, ClosureOperandState &state) {
  // Walk the instructions from the beginning of the block to the end.
  for (auto &inst : *address->getParent()) {
    assert(!useState.inits.count(&inst) &&
           "Shouldn't see an init before a destroy or reinit");

    // If we see a destroy, then we know we are upwards consume... stash it so
    // that we can destroy it
    if (auto *dvi = dyn_cast<DestroyAddrInst>(&inst)) {
      if (useState.destroyToIndexMap.count(dvi)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "ClosureArgDataflow: Found Consume: " << *dvi);

        if (hasMoreThanOneDebugUse(address))
          return false;

        state.pairedConsumingInsts.push_back(dvi);
        state.result = DownwardScanResult::ClosureConsume;
        return true;
      }
    }

    // Same for reinits.
    if (useState.reinits.count(&inst)) {
      LLVM_DEBUG(llvm::dbgs() << "ClosureArgDataflow: Found Reinit: " << inst);

      if (hasMoreThanOneDebugUse(address))
        return false;

      state.pairedConsumingInsts.push_back(&inst);
      state.result = DownwardScanResult::ClosureConsume;
      return true;
    }

    // Finally, if we have a liveness use, report it for a diagnostic.
    if (useState.livenessUses.count(&inst)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "ClosureArgDataflow: Found liveness use: " << inst);
      state.pairedUseInsts.push_back(&inst);
      state.result = DownwardScanResult::ClosureUse;
      return true;
    }
  }

  LLVM_DEBUG(
      llvm::dbgs() << "ClosureArgDataflow: Did not find interesting uses.\n");
  return false;
}

bool ClosureArgDataflowState::performLivenessDataflow(
    const BasicBlockSet &initBlocks, const BasicBlockSet &livenessBlocks,
    const BasicBlockSet &consumingBlocks) {
  LLVM_DEBUG(llvm::dbgs() << "ClosureArgLivenessDataflow. Start!\n");
  bool foundSingleLivenessUse = false;
  auto *fn = useState.getFunction();
  auto *frontBlock = &*fn->begin();
  BasicBlockWorklist worklist(fn);

  for (unsigned i : indices(livenessWorklist)) {
    auto *&user = livenessWorklist[i];

    // If our use is in the first block, then we are done with this user. Set
    // the found single liveness use flag and continue!
    if (frontBlock == user->getParent()) {
      foundSingleLivenessUse = true;
      continue;
    }

    bool success = false;
    for (auto *predBlock : user->getParent()->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
    while (auto *next = worklist.pop()) {
      if (livenessBlocks.contains(next) || initBlocks.contains(next) ||
          consumingBlocks.contains(next)) {
        continue;
      }

      if (frontBlock == next) {
        success = true;
        foundSingleLivenessUse = true;
        break;
      }

      for (auto *predBlock : next->getPredecessorBlocks()) {
        worklist.pushIfNotVisited(predBlock);
      }
    }
    if (!success) {
      user = nullptr;
    }
  }
  return foundSingleLivenessUse;
}

bool ClosureArgDataflowState::performConsumingDataflow(
    const BasicBlockSet &initBlocks, const BasicBlockSet &consumingBlocks) {
  auto *fn = useState.getFunction();
  auto *frontBlock = &*fn->begin();

  bool foundSingleConsumingUse = false;
  BasicBlockWorklist worklist(fn);
  for (unsigned i : indices(consumingWorklist)) {
    auto *&user = consumingWorklist[i];

    if (frontBlock == user->getParent())
      continue;

    bool success = false;
    for (auto *predBlock : user->getParent()->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
    while (auto *next = worklist.pop()) {
      if (initBlocks.contains(next) || consumingBlocks.contains(next)) {
        continue;
      }

      if (frontBlock == next) {
        success = true;
        foundSingleConsumingUse = true;
        break;
      }

      for (auto *predBlock : next->getPredecessorBlocks()) {
        worklist.pushIfNotVisited(predBlock);
      }
    }
    if (!success) {
      user = nullptr;
    }
  }
  return foundSingleConsumingUse;
}

void ClosureArgDataflowState::classifyUses(BasicBlockSet &initBlocks,
                                           BasicBlockSet &livenessBlocks,
                                           BasicBlockSet &consumingBlocks) {

  for (auto *user : useState.inits) {
    if (upwardScanForInit(user, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found init block at: " << *user);
      livenessForConsumes.initializeDef(user);
      initBlocks.insert(user->getParent());
    }
  }

  for (auto *user : useState.livenessUses) {
    if (upwardScanForUseOut(user, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found use block at: " << *user);
      livenessBlocks.insert(user->getParent());
      livenessWorklist.push_back(user);
    }
  }

  for (auto destroyOpt : useState.destroys) {
    assert(destroyOpt);

    auto *destroy = *destroyOpt;

    auto iter = useState.destroyToIndexMap.find(destroy);
    assert(iter != useState.destroyToIndexMap.end());

    if (upwardScanForDestroys(destroy, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found destroy block at: " << *destroy);
      consumingBlocks.insert(destroy->getParent());
      consumingWorklist.push_back(destroy);
    }
  }

  for (auto reinitOpt : useState.reinits) {
    assert(reinitOpt);

    auto *reinit = *reinitOpt;
    auto iter = useState.reinitToIndexMap.find(reinit);
    assert(iter != useState.reinitToIndexMap.end());

    if (upwardScanForDestroys(reinit, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found reinit block at: " << *reinit);
      consumingBlocks.insert(reinit->getParent());
      consumingWorklist.push_back(reinit);
    }
  }
}

bool ClosureArgDataflowState::process(
    SILArgument *address, ClosureOperandState &state,
    SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers) {
  SILFunction *fn = address->getFunction();
  assert(fn);

  // First see if our function only has a single block. In such a case,
  // summarize using the single processing routine.
  if (address->getParent()->getTerminator()->isFunctionExiting()) {
    LLVM_DEBUG(llvm::dbgs() << "ClosureArgDataflow: Single Block Case.\n");
    return handleSingleBlockCase(address, state);
  }

  LLVM_DEBUG(llvm::dbgs() << "ClosureArgDataflow: Multiple Block Case.\n");

  // At this point, we begin by classifying the uses of our address into init
  // blocks, liveness blocks, consuming blocks. We also seed the worklist for
  // our two dataflows.
  SWIFT_DEFER {
    livenessWorklist.clear();
    consumingWorklist.clear();
  };
  BasicBlockSet initBlocks(fn);
  BasicBlockSet livenessBlocks(fn);
  BasicBlockSet consumingBlocks(fn);
  classifyUses(initBlocks, livenessBlocks, consumingBlocks);

  // Liveness Dataflow:
  //
  // The way that we do this is that for each such instruction:
  //
  // 1. If the instruction is in the entrance block, then it is our only answer.
  //
  // 2. If the user is not in the entrance block, visit recursively its
  //    predecessor blocks until one either hits the entrance block (in which
  //    case this is the result) /or/ one hits a block in one of our basic block
  //    sets which means there is an earlier use. Consuming blocks only stop for
  //    consuming blocks and init blocks. Liveness blocks stop for all other
  //    blocks.
  //
  // The result is what remains in our set. Thus we start by processing
  // liveness.
  if (performLivenessDataflow(initBlocks, livenessBlocks, consumingBlocks)) {
    for (unsigned i : indices(livenessWorklist)) {
      if (auto *ptr = livenessWorklist[i]) {
        LLVM_DEBUG(llvm::dbgs()
                   << "ClosureArgLivenessDataflow. Final: Liveness User: "
                   << *ptr);
        state.pairedUseInsts.push_back(ptr);
      }
    }
    state.result = DownwardScanResult::ClosureUse;
    return true;
  }

  // Then perform the consuming use dataflow. In this case, we think we may have
  // found a set of post-dominating consuming uses for our inout_aliasable
  // parameter. We are going to change it to be an out parameter and eliminate
  // these when we clone the closure.
  if (performConsumingDataflow(initBlocks, consumingBlocks)) {
    // Before we do anything, make sure our argument has at least one single
    // debug_value user. If we have many we can't handle it since something in
    // SILGen is emitting weird code. Our tests will ensure that SILGen does not
    // diverge by mistake. So we are really just being careful.
    if (hasMoreThanOneDebugUse(address)) {
      // Failing b/c more than one debug use!
      return false;
    }

    //!!! FIXME: Why?
    // auto *frontBlock = &*fn->begin();
    // livenessForConsumes.initializeDefBlock(frontBlock);

    for (unsigned i : indices(livenessWorklist)) {
      if (auto *ptr = livenessWorklist[i]) {
        state.pairedConsumingInsts.push_back(ptr);
        livenessForConsumes.updateForUse(ptr, true /*is lifetime ending*/);
      }
    }

    // If our consumes do not have a linear lifetime, bail. We will error on the
    // move being unknown.
    for (auto *ptr : state.pairedConsumingInsts) {
      if (livenessForConsumes.isWithinBoundary(ptr))
        return false;
      postDominatingConsumingUsers.insert(ptr);
    }
    state.result = DownwardScanResult::ClosureConsume;
    return true;
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                            Closure Use Gatherer
//===----------------------------------------------------------------------===//

namespace {

/// Visit all of the uses of a closure argument, initializing useState as we go.
struct GatherClosureUseVisitor : public AccessUseVisitor {
  UseState &useState;

  GatherClosureUseVisitor(UseState &useState)
      : AccessUseVisitor(AccessUseType::Overlapping,
                         NestedAccessType::IgnoreAccessBegin),
        useState(useState) {}

  bool visitUse(Operand *op, AccessUseType useTy) override;
  void reset(SILValue address) { useState.address = address; }
  void clear() { useState.clear(); }
};

} // end anonymous namespace

// Filter out recognized uses that do not write to memory.
//
// TODO: Ensure that all of the conditional-write logic below is encapsulated in
// mayWriteToMemory and just call that instead. Possibly add additional
// verification that visitAccessPathUses recognizes all instructions that may
// propagate pointers (even though they don't write).
bool GatherClosureUseVisitor::visitUse(Operand *op, AccessUseType useTy) {
  // If this operand is for a dependent type, then it does not actually access
  // the operand's address value. It only uses the metatype defined by the
  // operation (e.g. open_existential).
  if (op->isTypeDependent()) {
    return true;
  }

  // Ignore debug_values. We should leave them on the argument so that later in
  // the function the user can still access the out parameter once it is
  // updated.
  if (isa<DebugValueInst>(op->getUser()))
    return true;

  // Ignore end_access. For our purposes, they are irrelevant and we do not want
  // to treat them like liveness uses.
  if (isa<EndAccessInst>(op->getUser()))
    return true;

  if (memInstMustInitialize(op)) {
    if (stripAccessMarkers(op->get()) != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found init use not on base address: "
                 << *op->getUser());
      return false;
    }

    LLVM_DEBUG(llvm::dbgs() << "ClosureUse: Found init: " << *op->getUser());
    useState.inits.insert(op->getUser());
    return true;
  }

  if (isReinitToInitConvertibleInst(op)) {
    if (stripAccessMarkers(op->get()) != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found reinit use not on base address: "
                 << *op->getUser());
      return false;
    }

    LLVM_DEBUG(llvm::dbgs() << "ClosureUse: Found reinit: " << *op->getUser());
    useState.insertReinit(op->getUser());
    return true;
  }

  if (auto *dvi = dyn_cast<DestroyAddrInst>(op->getUser())) {
    // If we see a destroy_addr not on our base address, bail! Just error and
    // say that we do not understand the code.
    if (dvi->getOperand() != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found destroy_addr no on base address: "
                 << *dvi);
      return false;
    }
    LLVM_DEBUG(llvm::dbgs() << "ClosureUse: Found destroy_addr: " << *dvi);
    useState.insertDestroy(dvi);
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "ClosureUse: Found liveness use: "
                          << *op->getUser());
  useState.livenessUses.insert(op->getUser());

  return true;
}

//===----------------------------------------------------------------------===//
//                          Closure Argument Cloner
//===----------------------------------------------------------------------===//

namespace {

struct ClosureArgumentInOutToOutCloner
    : SILClonerWithScopes<ClosureArgumentInOutToOutCloner> {
  friend class SILInstructionVisitor<ClosureArgumentInOutToOutCloner>;
  friend class SILCloner<ClosureArgumentInOutToOutCloner>;

  SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers;
  SILFunction *orig;
  const SmallBitVector &argsToConvertIndices;
  SmallPtrSet<SILValue, 8> oldArgSet;

  // Map from clonedArg -> oldArg.
  llvm::SmallMapVector<SILValue, SILValue, 4> clonedArgToOldArgMap;

public:
  ClosureArgumentInOutToOutCloner(
      SILOptFunctionBuilder &funcBuilder, SILFunction *orig,
      IsSerialized_t isSerialized,
      SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers,
      const SmallBitVector &argsToConvertIndices, StringRef name);

  void populateCloned();

  SILFunction *getCloned() { return &getBuilder().getFunction(); }

  void visitDebugValueInst(DebugValueInst *inst) {
    // Do not clone if our inst argument is one of our cloned arguments. In such
    // a case, we are going to handle the debug_value when we visit a post
    // dominating consuming reinit.
    if (oldArgSet.count(inst->getOperand())) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    Visiting debug value that is in the old arg set!\n");
      return;
    }
    LLVM_DEBUG(llvm::dbgs()
               << "    Visiting debug value that we will clone!\n");
    SILCloner<ClosureArgumentInOutToOutCloner>::visitDebugValueInst(inst);
  }

  void visitDestroyValueInst(DestroyValueInst *inst) {
    if (!postDominatingConsumingUsers.count(inst)) {
      SILCloner<ClosureArgumentInOutToOutCloner>::visitDestroyValueInst(inst);
    }

    // Don't do anything if we have a destroy.
  }

  void visitCopyAddrInst(CopyAddrInst *inst) {
    if (!postDominatingConsumingUsers.count(inst)) {
      return SILCloner<ClosureArgumentInOutToOutCloner>::visitCopyAddrInst(
          inst);
    }

    // If this copy_addr is one of the copies that we need to fixup, convert it
    // to an init from a reinit. We also insert a debug_value
    assert(!inst->isInitializationOfDest() && "Should be a reinit");
    getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
    recordClonedInstruction(
        inst, getBuilder().createCopyAddr(
                  getOpLocation(inst->getLoc()), getOpValue(inst->getSrc()),
                  getOpValue(inst->getDest()), inst->isTakeOfSrc(),
                  IsInitialization_t::IsInitialization));

    // Then if in our caller we had a debug_value on our dest, add it here.
    auto base = AccessPathWithBase::compute(inst->getDest()).base;
    if (oldArgSet.count(base)) {
      if (auto *op = getSingleDebugUse(base)) {
        if (auto *dvi = dyn_cast<DebugValueInst>(op->getUser())) {
          SILCloner<ClosureArgumentInOutToOutCloner>::visitDebugValueInst(dvi);
        }
      }
    }
  }

  void visitStoreInst(StoreInst *inst) {
    if (!postDominatingConsumingUsers.count(inst)) {
      return SILCloner<ClosureArgumentInOutToOutCloner>::visitStoreInst(inst);
    }

    // If this store is one of the copies that we need to fixup, convert it
    // to an init from being an assign.
    assert(inst->getOwnershipQualifier() == StoreOwnershipQualifier::Assign);
    getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
    recordClonedInstruction(
        inst, getBuilder().createStore(
                  getOpLocation(inst->getLoc()), getOpValue(inst->getSrc()),
                  getOpValue(inst->getDest()), StoreOwnershipQualifier::Init));

    auto base = AccessPathWithBase::compute(inst->getDest()).base;
    if (oldArgSet.count(base)) {
      if (auto *op = getSingleDebugUse(base)) {
        if (auto *dvi = dyn_cast<DebugValueInst>(op->getUser())) {
          SILCloner<ClosureArgumentInOutToOutCloner>::visitDebugValueInst(dvi);
        }
      }
    }
  }

private:
  static SILFunction *initCloned(
      SILOptFunctionBuilder &funcBuilder, SILFunction *orig,
      IsSerialized_t isSerialized,
      SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers,
      const SmallBitVector &argsToConvertIndices, StringRef cloneName);
};

} // namespace

static std::string getClonedName(SILFunction *func, IsSerialized_t serialized,
                                 const SmallBitVector &argsToConvertIndices) {
  auto kind = Demangle::SpecializationPass::MoveDiagnosticInOutToOut;
  Mangle::FunctionSignatureSpecializationMangler Mangler(kind, serialized,
                                                         func);
  for (int i = argsToConvertIndices.find_first(); i != -1;
       i = argsToConvertIndices.find_next(i)) {
    Mangler.setArgumentInOutToOut(i);
  }
  return Mangler.mangle();
}

ClosureArgumentInOutToOutCloner::ClosureArgumentInOutToOutCloner(
    SILOptFunctionBuilder &funcBuilder, SILFunction *orig,
    IsSerialized_t isSerialized,
    SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers,
    const SmallBitVector &argsToConvertIndices, StringRef name)
    : SILClonerWithScopes<ClosureArgumentInOutToOutCloner>(*initCloned(
          funcBuilder, orig, isSerialized, postDominatingConsumingUsers,
          argsToConvertIndices, name)),
      postDominatingConsumingUsers(postDominatingConsumingUsers), orig(orig),
      argsToConvertIndices(argsToConvertIndices) {
  assert(orig->getDebugScope()->getParentFunction() !=
         getCloned()->getDebugScope()->getParentFunction());
}

/// Create the function corresponding to the clone of the
/// original closure with the signature modified to reflect promoted
/// parameters (which are specified by PromotedArgIndices).
SILFunction *ClosureArgumentInOutToOutCloner::initCloned(
    SILOptFunctionBuilder &funcBuilder, SILFunction *orig,
    IsSerialized_t serialized,
    SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers,
    const SmallBitVector &argsToConvertIndices, StringRef clonedName) {
  SILModule &mod = orig->getModule();
  SmallVector<SILParameterInfo, 4> clonedInterfaceArgTys;
  SmallVector<SILResultInfo, 4> clonedResultInfos;
  SILFunctionType *origFTI = orig->getLoweredFunctionType();

  // First initialized cloned result infos with the old results.
  for (auto result : origFTI->getResults())
    clonedResultInfos.push_back(result);

  // Generate a new parameter list with deleted parameters removed...
  unsigned initArgIndex = orig->getConventions().getSILArgIndexOfFirstParam();
  LLVM_DEBUG(llvm::dbgs() << "CLONER: initArgIndex: " << initArgIndex << '\n');
  for (auto state :
       llvm::enumerate(origFTI->getParameters().drop_front(initArgIndex))) {
    unsigned index = state.index();
    auto paramInfo = state.value();

    // If we are supposed to convert this, add the parameter to the result list.
    if (argsToConvertIndices.test(index)) {
      LLVM_DEBUG(llvm::dbgs() << "CLONER: Converting: " << index << "\n");
      clonedResultInfos.emplace_back(paramInfo.getInterfaceType(),
                                     ResultConvention::Indirect);
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "CLONER: Letting through: " << index << "\n");
    // Otherwise, just let it through.
    clonedInterfaceArgTys.push_back(paramInfo);
    ++index;
  }

  // Create the new function type for the cloned function with some of
  // the parameters moved to be results.
  auto clonedTy = SILFunctionType::get(
      origFTI->getInvocationGenericSignature(), origFTI->getExtInfo(),
      origFTI->getCoroutineKind(), origFTI->getCalleeConvention(),
      clonedInterfaceArgTys, origFTI->getYields(), clonedResultInfos,
      origFTI->getOptionalErrorResult(), origFTI->getPatternSubstitutions(),
      origFTI->getInvocationSubstitutions(), mod.getASTContext(),
      origFTI->getWitnessMethodConformanceOrInvalid());
  LLVM_DEBUG(llvm::dbgs() << "CLONER: clonedTy: " << clonedTy << "\n");
  assert((orig->isTransparent() || orig->isBare() || orig->getLocation()) &&
         "SILFunction missing location");
  assert((orig->isTransparent() || orig->isBare() || orig->getDebugScope()) &&
         "SILFunction missing DebugScope");
  assert(!orig->isGlobalInit() && "Global initializer cannot be cloned");
  auto *Fn = funcBuilder.createFunction(
      swift::getSpecializedLinkage(orig, orig->getLinkage()), clonedName,
      clonedTy, orig->getGenericEnvironment(), orig->getLocation(),
      orig->isBare(), orig->isTransparent(), serialized, IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible, orig->getEntryCount(),
      orig->isThunk(), orig->getClassSubclassScope(), orig->getInlineStrategy(),
      orig->getEffectsKind(), orig, orig->getDebugScope());
  for (auto &Attr : orig->getSemanticsAttrs()) {
    Fn->addSemanticsAttr(Attr);
  }

  return Fn;
}

/// Populate the body of the cloned closure, modifying instructions as
/// necessary to take into consideration the removed parameters.
void ClosureArgumentInOutToOutCloner::populateCloned() {
  SILFunction *cloned = getCloned();

  // Create arguments for the entry block
  SILBasicBlock *origEntryBlock = &*orig->begin();
  SILBasicBlock *clonedEntryBlock = cloned->createBasicBlock();

  SmallVector<SILValue, 4> entryArgs;
  entryArgs.reserve(origEntryBlock->getArguments().size());

  // First process all of the indirect results and add our new results after
  // them.
  auto oldArgs = origEntryBlock->getArguments();
  auto origConventions = orig->getConventions();
  for (unsigned i : range(origConventions.getSILArgIndexOfFirstIndirectResult(),
                          origConventions.getSILArgIndexOfFirstParam())) {
    LLVM_DEBUG(llvm::dbgs() << "Have indirect result\n");
    auto *arg = oldArgs[i];
    // Create a new argument which copies the original argument.
    auto *newArg = clonedEntryBlock->createFunctionArgument(arg->getType(),
                                                            arg->getDecl());
    clonedArgToOldArgMap[newArg] = arg;
    entryArgs.push_back(newArg);
  }

  // To avoid needing to mess with types, just go through our original arguments
  // in the entry block to get the right types.
  for (auto state : llvm::enumerate(origEntryBlock->getArguments())) {
    unsigned argNo = state.index();
    LLVM_DEBUG(llvm::dbgs() << "Testing Old Arg Number: " << argNo << "\n");
    if (!argsToConvertIndices.test(argNo))
      continue;

    auto *arg = state.value();
    auto *newArg = clonedEntryBlock->createFunctionArgument(arg->getType(),
                                                            arg->getDecl());
    clonedArgToOldArgMap[newArg] = arg;
    oldArgSet.insert(arg);
    entryArgs.push_back(newArg);
    LLVM_DEBUG(llvm::dbgs() << "Mapping From: " << *arg);
    LLVM_DEBUG(llvm::dbgs()
               << "   of function: " << arg->getFunction()->getName() << '\n');
    LLVM_DEBUG(llvm::dbgs() << "Mapping To: " << *newArg);
    LLVM_DEBUG(llvm::dbgs() << "   of function: "
                            << newArg->getFunction()->getName() << '\n');
  }

  // Finally, recreate the rest of the arguments which we did not specialize.
  for (auto state : llvm::enumerate(origEntryBlock->getArguments())) {
    unsigned argNo = state.index();
    if (argsToConvertIndices.test(argNo))
      continue;

    auto *arg = state.value();
    auto *newArg = clonedEntryBlock->createFunctionArgument(arg->getType(),
                                                            arg->getDecl());

    clonedArgToOldArgMap[newArg] = arg;
    entryArgs.push_back(newArg);
  }

  // Visit original BBs in depth-first preorder, starting with the
  // entry block, cloning all instructions and terminators.
  cloneFunctionBody(
      orig, clonedEntryBlock, entryArgs, [&](SILValue clonedArg) -> SILValue {
        LLVM_DEBUG(llvm::dbgs() << "Searching for: " << *clonedArg);
        auto iter = clonedArgToOldArgMap.find(clonedArg);
        assert(iter != clonedArgToOldArgMap.end() &&
               "Should map all cloned args to an old arg");
        LLVM_DEBUG(llvm::dbgs() << "Found it! Mapping to : " << *iter->second);
        return iter->second;
      });
}

/////////////////////////////////////
// Caller Lexical Lifetime Visitor //
/////////////////////////////////////

namespace {

/// Visit all of the uses of a lexical lifetime, initializing useState as we go.
struct GatherLexicalLifetimeUseVisitor : public AccessUseVisitor {
  UseState &useState;

  GatherLexicalLifetimeUseVisitor(UseState &useState)
      : AccessUseVisitor(AccessUseType::Overlapping,
                         NestedAccessType::IgnoreAccessBegin),
        useState(useState) {}

  bool visitUse(Operand *op, AccessUseType useTy) override;
  void reset(SILValue address) { useState.address = address; }
  void clear() { useState.clear(); }
};

} // end anonymous namespace

// Filter out recognized uses that do not write to memory.
//
// TODO: Ensure that all of the conditional-write logic below is encapsulated in
// mayWriteToMemory and just call that instead. Possibly add additional
// verification that visitAccessPathUses recognizes all instructions that may
// propagate pointers (even though they don't write).
bool GatherLexicalLifetimeUseVisitor::visitUse(Operand *op,
                                               AccessUseType useTy) {
  // If this operand is for a dependent type, then it does not actually access
  // the operand's address value. It only uses the metatype defined by the
  // operation (e.g. open_existential).
  if (op->isTypeDependent()) {
    return true;
  }

  // If we have a move from src, this is a mark_move we want to visit.
  if (auto *move = dyn_cast<MarkUnresolvedMoveAddrInst>(op->getUser())) {
    if (move->getSrc() == op->get()) {
      LLVM_DEBUG(llvm::dbgs() << "Found move: " << *move);
      useState.insertMarkUnresolvedMoveAddr(move);
      return true;
    }
  }

  if (memInstMustInitialize(op)) {
    if (stripAccessMarkers(op->get()) != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found init use not on base address: "
                 << *op->getUser());
      return false;
    }

    LLVM_DEBUG(llvm::dbgs() << "Found init: " << *op->getUser());
    useState.inits.insert(op->getUser());
    return true;
  }

  if (isReinitToInitConvertibleInst(op)) {
    if (stripAccessMarkers(op->get()) != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found reinit use not on base address: "
                 << *op->getUser());
      return false;
    }

    LLVM_DEBUG(llvm::dbgs() << "Found reinit: " << *op->getUser());
    useState.insertReinit(op->getUser());
    return true;
  }

  if (auto *dvi = dyn_cast<DestroyAddrInst>(op->getUser())) {
    // If we see a destroy_addr not on our base address, bail! Just error and
    // say that we do not understand the code.
    if (dvi->getOperand() != useState.address) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!!! Error! Found destroy_addr no on base address: "
                 << *dvi);
      return false;
    }
    LLVM_DEBUG(llvm::dbgs() << "Found destroy_addr: " << *dvi);
    useState.insertDestroy(dvi);
    return true;
  }

  // Then see if we have a inout_aliasable full apply site use. In that case, we
  // are going to try and extend move checking into the partial apply using
  // cloning to eliminate destroys or reinits.
  if (auto fas = FullApplySite::isa(op->getUser())) {
    if (stripAccessMarkers(op->get()) != useState.address) {
      LLVM_DEBUG(
          llvm::dbgs()
          << "!!! Error! Found consuming closure use not on base address: "
          << *op->getUser());
      return false;
    }

    if (fas.getCaptureConvention(*op) ==
        SILArgumentConvention::Indirect_InoutAliasable) {
      // If we don't find the function, we can't handle this, so bail.
      auto *func = fas.getCalleeFunction();
      if (!func || !func->isDefer())
        return false;
      LLVM_DEBUG(llvm::dbgs() << "Found closure use: " << *op->getUser());
      useState.insertClosureOperand(op);
      return true;
    }
  }

  // Ignore dealloc_stack.
  if (isa<DeallocStackInst>(op->getUser()))
    return true;

  // Ignore end_access.
  if (isa<EndAccessInst>(op->getUser()))
    return true;

  LLVM_DEBUG(llvm::dbgs() << "Found liveness use: " << *op->getUser());
  useState.livenessUses.insert(op->getUser());

  return true;
}

//===----------------------------------------------------------------------===//
//                              Global Dataflow
//===----------------------------------------------------------------------===//

namespace {

struct DataflowState {
  llvm::DenseMap<SILBasicBlock *, SILInstruction *> useBlocks;
  llvm::DenseSet<SILBasicBlock *> initBlocks;
  llvm::DenseMap<SILBasicBlock *, SILInstruction *> destroyBlocks;
  llvm::DenseMap<SILBasicBlock *, SILInstruction *> reinitBlocks;
  llvm::DenseMap<SILBasicBlock *, Operand *> closureConsumeBlocks;
  llvm::DenseMap<SILBasicBlock *, ClosureOperandState *> closureUseBlocks;
  SmallVector<MarkUnresolvedMoveAddrInst *, 8> markMovesThatPropagateDownwards;

  SILOptFunctionBuilder &funcBuilder;
  UseState &useState;
  llvm::SmallMapVector<FullApplySite, SmallBitVector, 8>
      &applySiteToPromotedArgIndices;
  SmallBlotSetVector<SILInstruction *, 8> &closureConsumes;

  DataflowState(SILOptFunctionBuilder &funcBuilder, UseState &useState,
                llvm::SmallMapVector<FullApplySite, SmallBitVector, 8>
                    &applySiteToPromotedArgIndices,
                SmallBlotSetVector<SILInstruction *, 8> &closureConsumes)
      : funcBuilder(funcBuilder), useState(useState),
        applySiteToPromotedArgIndices(applySiteToPromotedArgIndices),
        closureConsumes(closureConsumes) {}
  void init();
  bool process(
      SILValue address, DebugVarCarryingInst addressDebugInst,
      SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers);
  bool handleSingleBlockClosure(SILArgument *address,
                                ClosureOperandState &state);
  bool cleanupAllDestroyAddr(
      SILValue address, DebugVarCarryingInst addressDebugInst, SILFunction *fn,
      SmallBitVector &destroyIndices, SmallBitVector &reinitIndices,
      SmallBitVector &consumingClosureIndices,
      BasicBlockSet &blocksVisitedWhenProcessingNewTakes,
      BasicBlockSet &blocksWithMovesThatAreNowTakes,
      SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers);
  void clear() {
    useBlocks.clear();
    initBlocks.clear();
    destroyBlocks.clear();
    reinitBlocks.clear();
    markMovesThatPropagateDownwards.clear();
    closureConsumeBlocks.clear();
    closureUseBlocks.clear();
  }
};

} // namespace

bool DataflowState::cleanupAllDestroyAddr(
    SILValue address, DebugVarCarryingInst addressDebugInst, SILFunction *fn,
    SmallBitVector &destroyIndices, SmallBitVector &reinitIndices,
    SmallBitVector &consumingClosureIndices,
    BasicBlockSet &blocksVisitedWhenProcessingNewTakes,
    BasicBlockSet &blocksWithMovesThatAreNowTakes,
    SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers) {
  bool madeChange = false;
  BasicBlockWorklist worklist(fn);

  LLVM_DEBUG(llvm::dbgs() << "Cleanup up destroy addr!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Visiting destroys!\n");
  LLVM_DEBUG(llvm::dbgs() << "    Destroy Indices: " << destroyIndices << "\n");
  for (int index = destroyIndices.find_first(); index != -1;
       index = destroyIndices.find_next(index)) {
    LLVM_DEBUG(llvm::dbgs() << "    Index: " << index << "\n");
    auto dai = useState.destroys[index];
    if (!dai)
      continue;
    LLVM_DEBUG(llvm::dbgs() << "    Destroy: " << *dai);
    for (auto *predBlock : (*dai)->getParent()->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "    Visiting reinit!\n");
  for (int index = reinitIndices.find_first(); index != -1;
       index = reinitIndices.find_next(index)) {
    auto reinit = useState.reinits[index];
    if (!reinit)
      continue;
    LLVM_DEBUG(llvm::dbgs() << "  Reinit: " << **reinit);
    for (auto *predBlock : (*reinit)->getParent()->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "    Visiting consuming closures!\n");
  for (int index = consumingClosureIndices.find_first(); index != -1;
       index = consumingClosureIndices.find_next(index)) {
    auto &pair = *std::next(useState.closureUses.begin(), index);
    auto *op = pair.first;
    LLVM_DEBUG(llvm::dbgs() << "  Consuming closure: " << *op->getUser());
    for (auto *predBlock : op->getUser()->getParent()->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "Processing worklist!\n");
  while (auto *next = worklist.pop()) {
    LLVM_DEBUG(llvm::dbgs()
               << "Looking at block: bb" << next->getDebugID() << "\n");

    // Any blocks that contained processed moves are stop points.
    if (blocksWithMovesThatAreNowTakes.contains(next)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    Block contained a move that is now a true take.\n");
      continue;
    }

    // Then if we find that we have a block that was never visited when we
    // walked along successor edges from the move, then we know that we need to
    // insert a destroy_addr.
    //
    // This is safe to do since this block lives along the dominance frontier
    // and we do not allow for critical edges, so as we walk along predecessors,
    // given that any such block must also have a successor that was reachable
    // from our move, we know that this unprocessed block must only have one
    // successor, a block reachable from our move and thus must not have any
    // unhandled uses.
    if (!blocksVisitedWhenProcessingNewTakes.contains(next)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found a block that was not visited when "
                                 "we processed takes of the given move.\n");
      // Insert a destroy_addr here since the block isn't reachable from any of
      // our moves.
      SILBasicBlock::iterator iter;
      if (!isa<TermInst>(next->front())) {
        iter = std::prev(next->getTerminator()->getIterator());
      } else {
        iter = next->begin();
      }
      SILBuilderWithScope builder(iter);
      auto *dvi = builder.createDestroyAddr(
          RegularLocation::getAutoGeneratedLocation(), address);
      // Create a debug_value undef if we have debug info to stop the async dbg
      // info propagation from creating debug info for an already destroyed
      // value. We use a separate builder since we need to control the debug
      // scope/location to get llvm to do the right thing.
      if (addressDebugInst) {
        if (auto varInfo = addressDebugInst.getVarInfo()) {
          // We need to always insert /after/ the reinit since the value will
          // not be defined before the value.
          SILBuilderWithScope dbgValueInsertBuilder(dvi);
          dbgValueInsertBuilder.setCurrentDebugScope(
              addressDebugInst->getDebugScope());
          dbgValueInsertBuilder.createDebugValue(
              (*addressDebugInst)->getLoc(),
              SILUndef::get(address->getType(), dvi->getModule()), *varInfo,
              false,
              /*was moved*/ true);
        }
      }
      useState.destroys.insert(dvi);
      continue;
    }

    // Otherwise, this block is reachable from one of our move blocks, visit
    // further predecessors.
    for (auto *predBlock : next->getPredecessorBlocks()) {
      worklist.pushIfNotVisited(predBlock);
    }
  }

  for (int index = destroyIndices.find_first(); index != -1;
       index = destroyIndices.find_next(index)) {
    auto destroy = useState.destroys[index];
    if (!destroy)
      continue;
    LLVM_DEBUG(llvm::dbgs() << "Erasing destroy_addr: " << *destroy);
    (*destroy)->eraseFromParent();
    madeChange = true;
  }

  for (int index = reinitIndices.find_first(); index != -1;
       index = reinitIndices.find_next(index)) {
    auto reinit = useState.reinits[index];
    if (!reinit)
      continue;
    LLVM_DEBUG(llvm::dbgs() << "Converting reinit to init: " << *reinit);
    convertMemoryReinitToInitForm(*reinit);

    // Make sure to create a new debug_value for the reinit value.
    if (addressDebugInst) {
      if (auto varInfo = addressDebugInst.getVarInfo()) {
        // We need to always insert /after/ the reinit since the value will not
        // be defined before the value.
        SILBuilderWithScope reinitBuilder((*reinit)->getNextInstruction());
        reinitBuilder.setCurrentDebugScope(addressDebugInst->getDebugScope());
        reinitBuilder.createDebugValue((*addressDebugInst)->getLoc(), address,
                                       *varInfo, false,
                                       /*was moved*/ true);
      }
    }
    madeChange = true;
  }

  // Check for consuming closures. If we find such a consuming closure, track
  // that this full apply site needs to have some parameters converted when we
  // are done processing.
  //
  // NOTE: We do this late to ensure that we only clone a defer exactly once
  // rather than multiple times for multiple vars.
  for (int index = consumingClosureIndices.find_first(); index != -1;
       index = consumingClosureIndices.find_next(index)) {
    auto &pair = *std::next(useState.closureUses.begin(), index);
    auto *closureUse = pair.first;
    if (!closureUse)
      continue;

    // This is correct today due to us only supporting defer. When we handle
    // partial apply, we will need to do more work ehre.
    FullApplySite fas(closureUse->getUser());
    assert(fas);
    unsigned appliedArgIndex = fas.getAppliedArgIndex(*closureUse);
    LLVM_DEBUG(llvm::dbgs() << "Processing closure use: " << **fas);
    LLVM_DEBUG(llvm::dbgs() << "AppliedArgIndex: " << appliedArgIndex << '\n');
    auto &bitVector = applySiteToPromotedArgIndices[fas];
    auto conventions = fas.getSubstCalleeConv();
    unsigned numNonResultArgs = conventions.getNumSILArguments();
    if (bitVector.size() < numNonResultArgs)
      bitVector.resize(numNonResultArgs);
    bitVector.set(appliedArgIndex);
    for (auto *user : pair.second.pairedConsumingInsts) {
      closureConsumes.insert(user);
    }
  }

  return madeChange;
}

bool DataflowState::process(
    SILValue address, DebugVarCarryingInst addressDebugInst,
    SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers) {
  SILFunction *fn = address->getFunction();
  assert(fn);

  bool madeChange = false;
  SmallBitVector indicesOfPairedDestroys;
  auto getIndicesOfPairedDestroys = [&]() -> SmallBitVector & {
    if (indicesOfPairedDestroys.size() != useState.destroys.size())
      indicesOfPairedDestroys.resize(useState.destroys.size());
    return indicesOfPairedDestroys;
  };
  SmallBitVector indicesOfPairedReinits;
  auto getIndicesOfPairedReinits = [&]() -> SmallBitVector & {
    if (indicesOfPairedReinits.size() != useState.reinits.size())
      indicesOfPairedReinits.resize(useState.reinits.size());
    return indicesOfPairedReinits;
  };
  SmallBitVector indicesOfPairedConsumingClosureUses;
  auto getIndicesOfPairedConsumingClosureUses = [&]() -> SmallBitVector & {
    if (indicesOfPairedConsumingClosureUses.size() !=
        useState.closureUses.size())
      indicesOfPairedConsumingClosureUses.resize(useState.closureUses.size());
    return indicesOfPairedConsumingClosureUses;
  };

  BasicBlockSet blocksVisitedWhenProcessingNewTakes(fn);
  BasicBlockSet blocksWithMovesThatAreNowTakes(fn);
  bool convertedMarkMoveToTake = false;

  for (auto *mvi : markMovesThatPropagateDownwards) {
    bool emittedSingleDiagnostic = false;

    LLVM_DEBUG(llvm::dbgs() << "Checking Multi Block Dataflow for: " << *mvi);
    LLVM_DEBUG(llvm::dbgs() << "    Parent Block: bb"
                            << mvi->getParent()->getDebugID() << "\n");

    BasicBlockWorklist worklist(fn);
    BasicBlockSetVector visitedBlocks(fn);
    for (auto *succBlock : mvi->getParent()->getSuccessorBlocks()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "    SuccBlocks: bb" << succBlock->getDebugID() << "\n");
      worklist.pushIfNotVisited(succBlock);
      visitedBlocks.insert(succBlock);
    }

    while (auto *next = worklist.pop()) {
      LLVM_DEBUG(llvm::dbgs() << "Visiting: bb" << next->getDebugID() << "\n");

      // Before we check if we are supposed to stop processing here, check if we
      // have a use block. In such a case, emit an error.
      //
      // NOTE: We do this before since we could have a use before a destroy_addr
      // in this block. We would like to error in such a case.
      auto iter = useBlocks.find(next);
      if (iter != useBlocks.end()) {
        LLVM_DEBUG(llvm::dbgs() << "    Is Use Block! Emitting Error!\n");
        // We found one! Emit the diagnostic and continue and see if we can get
        // more diagnostics.
        auto &astContext = fn->getASTContext();
        {
          auto diag =
              diag::sil_movechecking_value_used_after_consume;
          StringRef name = getDebugVarName(address);
          diagnose(astContext, getSourceLocFromValue(address), diag, name);
        }

        {
          auto diag = diag::sil_movechecking_consuming_use_here;
          diagnose(astContext, mvi->getLoc().getSourceLoc(), diag);
        }

        {
          auto diag = diag::sil_movechecking_nonconsuming_use_here;
          diagnose(astContext, iter->second->getLoc().getSourceLoc(), diag);
        }

        emittedSingleDiagnostic = true;
        break;
      }

      // Now see if we have a closure use.
      {
        auto iter = closureUseBlocks.find(next);
        if (iter != closureUseBlocks.end()) {
          LLVM_DEBUG(llvm::dbgs()
                     << "    Is Use Block From Closure! Emitting Error!\n");
          // We found one! Emit the diagnostic and continue and see if we can
          // get more diagnostics.
          auto &astContext = fn->getASTContext();
          {
            auto diag =
                diag::sil_movechecking_value_used_after_consume;
            StringRef name = getDebugVarName(address);
            diagnose(astContext, getSourceLocFromValue(address), diag, name);
          }

          {
            auto diag = diag::sil_movechecking_consuming_use_here;
            diagnose(astContext, mvi->getLoc().getSourceLoc(), diag);
          }

          {
            auto diag = diag::sil_movechecking_nonconsuming_use_here;
            for (auto *user : iter->second->pairedUseInsts) {
              diagnose(astContext, user->getLoc().getSourceLoc(), diag);
            }
          }

          emittedSingleDiagnostic = true;
          break;
        }
      }

      // Then see if this is a destroy block. If so, do not add successors and
      // continue. This is because we stop processing at destroy_addr. This
      // destroy_addr is paired with the mark_unresolved_move_addr.
      {
        auto iter = destroyBlocks.find(next);
        if (iter != destroyBlocks.end()) {
          LLVM_DEBUG(llvm::dbgs() << "    Is Destroy Block! Setting up for "
                                     "later deletion if possible!\n");
          auto indexIter = useState.destroyToIndexMap.find(iter->second);
          assert(indexIter != useState.destroyToIndexMap.end());
          getIndicesOfPairedDestroys().set(indexIter->second);
          continue;
        }
      }

      {
        auto iter = reinitBlocks.find(next);
        if (iter != reinitBlocks.end()) {
          LLVM_DEBUG(llvm::dbgs() << "    Is reinit Block! Setting up for "
                                     "later deletion if possible!\n");
          auto indexIter = useState.reinitToIndexMap.find(iter->second);
          assert(indexIter != useState.reinitToIndexMap.end());
          getIndicesOfPairedReinits().set(indexIter->second);
          continue;
        }
      }

      {
        auto iter = closureConsumeBlocks.find(next);
        if (iter != closureConsumeBlocks.end()) {
          LLVM_DEBUG(llvm::dbgs() << "    Is reinit Block! Setting up for "
                                     "later deletion if possible!\n");
          auto indexIter = useState.closureOperandToIndexMap.find(iter->second);
          assert(indexIter != useState.closureOperandToIndexMap.end());
          getIndicesOfPairedConsumingClosureUses().set(indexIter->second);
          continue;
        }
      }

      // Then see if this is an init block. If so, do not add successors and
      // continue. We already checked that we are not destroy up in this block
      // by the check a few lines up. So we know that we are in one of the
      // following situations:
      //
      // 1. We are the only use in the block. In this case, we must have
      //    consumed the value with a non-destroy_addr earlier (e.x.: apply). In
      //    such a case, we need to just stop processing since we are re-initing
      //    memory for a var.
      //
      // 2. There is a consuming use that is treated as a consuming use before
      //    us. In that case, we will have already errored upon it.
      if (initBlocks.count(next)) {
        LLVM_DEBUG(llvm::dbgs() << "    Is Init Block!\n");
        continue;
      }

      LLVM_DEBUG(
          llvm::dbgs()
          << "    No match! Pushing unvisited successors onto the worklist!\n");
      // Otherwise, add successors if we haven't visited them to the worklist.
      for (auto *succBlock : next->getSuccessorBlocks()) {
        worklist.pushIfNotVisited(succBlock);
        visitedBlocks.insert(succBlock);
      }
    }

    // At this point, we know that if we emitted a diagnostic, we need to
    // convert the move to a copy_addr [init] since we found a use that violates
    // the move. We just want to emit correct IR without the
    // mark_unresolved_move_addr within it.
    blocksWithMovesThatAreNowTakes.insert(mvi->getParent());
    SILBuilderWithScope builder(mvi);
    if (emittedSingleDiagnostic) {
      builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(),
                             IsNotTake, IsInitialization);
    } else {
      builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(),
                             IsTake, IsInitialization);

      // Now that we have processed all of our mark_moves, eliminate all of the
      // destroy_addr and set our debug value as being moved.
      if (addressDebugInst) {
        addressDebugInst.markAsMoved();
        if (auto varInfo = addressDebugInst.getVarInfo()) {
          SILBuilderWithScope undefBuilder(builder);
          undefBuilder.setCurrentDebugScope(addressDebugInst->getDebugScope());
          undefBuilder.createDebugValue(
              addressDebugInst->getLoc(),
              SILUndef::get(address->getType(), builder.getModule()), *varInfo,
              false /*poison*/, true /*was moved*/);
        }
      }

      // Flush our SetVector into the visitedByNewMove.
      for (auto *block : visitedBlocks) {
        blocksVisitedWhenProcessingNewTakes.insert(block);
      }
      convertedMarkMoveToTake = true;
    }

    mvi->eraseFromParent();
    madeChange = true;
  }

  if (!convertedMarkMoveToTake)
    return madeChange;

  // Now that we have processed all of our mark_moves, eliminate all of the
  // destroy_addr.
  madeChange |= cleanupAllDestroyAddr(
      address, addressDebugInst, fn, getIndicesOfPairedDestroys(),
      getIndicesOfPairedReinits(), getIndicesOfPairedConsumingClosureUses(),
      blocksVisitedWhenProcessingNewTakes, blocksWithMovesThatAreNowTakes,
      postDominatingConsumingUsers);

  return madeChange;
}

void DataflowState::init() {
  // Go through all init uses and if we don't see any other of our uses, then
  // mark this as an "init block".
  for (auto *init : useState.inits) {
    if (upwardScanForInit(init, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found use block at: " << *init);
      initBlocks.insert(init->getParent());
    }
  }

  // Then go through all normal uses and do upwardScanForUseOut.
  for (auto *user : useState.livenessUses) {
    if (upwardScanForUseOut(user, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found liveness block at: " << *user);
      useBlocks[user->getParent()] = user;
    }
  }

  for (auto destroyOpt : useState.destroys) {
    // Any destroys we eliminated when processing single basic blocks will be
    // nullptr. Skip them!
    if (!destroyOpt)
      continue;

    auto *destroy = *destroyOpt;

    auto iter = useState.destroyToIndexMap.find(destroy);
    assert(iter != useState.destroyToIndexMap.end());

    if (upwardScanForDestroys(destroy, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found destroy block at: " << *destroy);
      destroyBlocks[destroy->getParent()] = destroy;
    }
  }

  for (auto reinitOpt : useState.reinits) {
    // Any destroys we eliminated when processing single basic blocks will be
    // nullptr. Skip them!
    if (!reinitOpt)
      continue;

    auto *reinit = *reinitOpt;
    auto iter = useState.reinitToIndexMap.find(reinit);
    assert(iter != useState.reinitToIndexMap.end());

    if (upwardScanForDestroys(reinit, useState)) {
      LLVM_DEBUG(llvm::dbgs() << "    Found reinit block at: " << *reinit);
      reinitBlocks[reinit->getParent()] = reinit;
    }
  }

  for (auto &closureUse : useState.closureUses) {
    auto *use = closureUse.first;
    auto &state = closureUse.second;
    auto *user = use->getUser();

    switch (state.result) {
    case DownwardScanResult::Invalid:
    case DownwardScanResult::Destroy:
    case DownwardScanResult::Reinit:
    case DownwardScanResult::UseForDiagnostic:
    case DownwardScanResult::MoveOut:
      llvm_unreachable("unhandled");
    case DownwardScanResult::ClosureUse:
      if (upwardScanForUseOut(user, useState)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "    Found closure liveness block at: " << *user);
        closureUseBlocks[user->getParent()] = &state;
      }
      break;
    case DownwardScanResult::ClosureConsume:
      if (upwardScanForDestroys(user, useState)) {
        LLVM_DEBUG(llvm::dbgs()
                   << "    Found closure consuming block at: " << *user);
        closureConsumeBlocks[user->getParent()] = use;
      }
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
//                              Address Checker
//===----------------------------------------------------------------------===//

namespace {

struct ConsumeOperatorCopyableAddressesChecker {
  SILFunction *fn;
  UseState useState;
  DataflowState dataflowState;
  UseState closureUseState;
  SILOptFunctionBuilder &funcBuilder;
  llvm::SmallMapVector<FullApplySite, SmallBitVector, 8>
      applySiteToPromotedArgIndices;
  SmallBlotSetVector<SILInstruction *, 8> closureConsumes;

  ConsumeOperatorCopyableAddressesChecker(SILFunction *fn,
                                          SILOptFunctionBuilder &funcBuilder)
      : fn(fn), useState(),
        dataflowState(funcBuilder, useState, applySiteToPromotedArgIndices,
                      closureConsumes),
        closureUseState(), funcBuilder(funcBuilder) {}

  void cloneDeferCalleeAndRewriteUses(
      SmallVectorImpl<SILValue> &temporaryStorage,
      const SmallBitVector &bitVector, FullApplySite oldApplySite,
      SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers);

  bool check(SILValue address);
  bool performClosureDataflow(Operand *callerOperand,
                              ClosureOperandState &calleeOperandState);

  void emitDiagnosticForMove(SILValue borrowedValue,
                             StringRef borrowedValueName, MoveValueInst *mvi);
  bool performSingleBasicBlockAnalysis(SILValue address,
                                       DebugVarCarryingInst addressDebugInst,
                                       MarkUnresolvedMoveAddrInst *mvi);

  ASTContext &getASTContext() const { return fn->getASTContext(); }
};

} // namespace

void ConsumeOperatorCopyableAddressesChecker::cloneDeferCalleeAndRewriteUses(
    SmallVectorImpl<SILValue> &newArgs, const SmallBitVector &bitVector,
    FullApplySite oldApplySite,
    SmallBlotSetVector<SILInstruction *, 8> &postDominatingConsumingUsers) {
  auto *origCallee = oldApplySite.getReferencedFunctionOrNull();
  assert(origCallee);

  auto name = getClonedName(origCallee, origCallee->isSerialized(), bitVector);

  SILFunction *newCallee = nullptr;
  if (auto *fn = origCallee->getModule().lookUpFunction(name)) {
    newCallee = fn;
  } else {
    ClosureArgumentInOutToOutCloner cloner(
        funcBuilder, origCallee, origCallee->isSerialized(),
        postDominatingConsumingUsers, bitVector, name);
    cloner.populateCloned();
    newCallee = cloner.getCloned();
  }
  assert(newCallee);

  // Ok, we now have populated our new callee. We need to create a new full
  // apply site that calls the new function appropriately.
  SWIFT_DEFER { newArgs.clear(); };

  // First add all of our old results to newArgs.
  auto oldConv = oldApplySite.getSubstCalleeConv();
  for (unsigned i : range(oldConv.getSILArgIndexOfFirstIndirectResult(),
                          oldConv.getSILArgIndexOfFirstParam())) {
    newArgs.push_back(oldApplySite->getOperand(i));
  }

  // Now add all of our new out params.
  for (int i = bitVector.find_first(); i != -1; i = bitVector.find_next(i)) {
    unsigned appliedArgIndex =
        oldApplySite.getOperandIndexOfFirstArgument() + i;
    newArgs.push_back(oldApplySite->getOperand(appliedArgIndex));
  }

  // Finally, add all of the rest of our arguments, skipping our new out
  // parameters.
  for (unsigned i : range(oldConv.getSILArgIndexOfFirstParam(),
                          oldConv.getNumSILArguments())) {
    if (bitVector.test(i))
      continue;
    unsigned appliedArgIndex =
        oldApplySite.getOperandIndexOfFirstArgument() + i;
    newArgs.push_back(oldApplySite->getOperand(appliedArgIndex));
  }

  // Then create our new apply.
  SILBuilderWithScope builder(*oldApplySite);
  auto *newCalleeRef =
      builder.createFunctionRef(oldApplySite->getLoc(), newCallee);
  auto *newApply =
      builder.createApply(oldApplySite->getLoc(), newCalleeRef,
                          oldApplySite.getSubstitutionMap(), newArgs);
  oldApplySite->replaceAllUsesPairwiseWith(newApply);
  oldApplySite->eraseFromParent();
}

bool ConsumeOperatorCopyableAddressesChecker::performClosureDataflow(
    Operand *callerOperand, ClosureOperandState &calleeOperandState) {
  auto fas = FullApplySite::isa(callerOperand->getUser());
  auto *func = fas.getCalleeFunction();
  auto *address =
      func->begin()->getArgument(fas.getCalleeArgIndex(*callerOperand));

  LLVM_DEBUG(llvm::dbgs() << "Performing closure dataflow on caller use: "
                          << *callerOperand->getUser());
  LLVM_DEBUG(llvm::dbgs() << "    Callee: " << func->getName() << '\n');
  LLVM_DEBUG(llvm::dbgs() << "    Callee Argument: " << *address);
  // We emit an end closure dataflow to make it easier when reading debug output
  // to make it easy to see when we have returned to analyzing the caller.
  SWIFT_DEFER {
    LLVM_DEBUG(llvm::dbgs()
                   << "Finished performing closure dataflow on Callee: "
                   << func->getName() << '\n';);
  };
  auto accessPathWithBase = AccessPathWithBase::compute(address);
  auto accessPath = accessPathWithBase.accessPath;

  // Bail on an invalid AccessPath.
  //
  // AccessPath completeness is verified independently--it may be invalid in
  // extraordinary situations. When AccessPath is valid, we know all its uses
  // are recognizable.
  //
  // NOTE: If due to an invalid access path we fail here, we will just error
  // on the _move since the _move would not have been handled.
  if (!accessPath.isValid())
    return false;

  // TODO: Hoist this useState into an ivar that we can reuse in between closure
  // operands?
  GatherClosureUseVisitor visitor(closureUseState);
  SWIFT_DEFER { visitor.clear(); };
  visitor.reset(address);
  if (!visitAccessPathUses(visitor, accessPath, fn))
    return false;

  ClosureArgDataflowState closureUseDataflowState(fn, closureUseState);
  return closureUseDataflowState.process(address, calleeOperandState,
                                         closureConsumes);
}

// Returns true if we emitted a diagnostic and handled the single block
// case. Returns false if we visited all of the uses and seeded the UseState
// struct with the information needed to perform our interprocedural dataflow.
bool ConsumeOperatorCopyableAddressesChecker::performSingleBasicBlockAnalysis(
    SILValue address, DebugVarCarryingInst addressDebugInst,
    MarkUnresolvedMoveAddrInst *mvi) {
  // First scan downwards to make sure we are move out of this block.
  auto &useState = dataflowState.useState;
  auto &applySiteToPromotedArgIndices =
      dataflowState.applySiteToPromotedArgIndices;
  auto &closureConsumes = dataflowState.closureConsumes;

  SILInstruction *interestingUser = nullptr;
  Operand *interestingUse = nullptr;
  TinyPtrVector<SILInstruction *> interestingClosureUsers;
  switch (downwardScanForMoveOut(mvi, useState, &interestingUser,
                                 &interestingUse, interestingClosureUsers)) {
  case DownwardScanResult::Invalid:
    llvm_unreachable("invalid");
  case DownwardScanResult::Destroy: {
    assert(!interestingUse);
    assert(interestingUser);

    // If we found a destroy, then we found a single block case that we can
    // handle. Remove the destroy and convert the mark_unresolved_move_addr
    // into a true move.
    auto *dvi = cast<DestroyAddrInst>(interestingUser);
    SILBuilderWithScope builder(mvi);
    builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(), IsTake,
                           IsInitialization);
    // Also, mark the alloc_stack as being moved at some point.
    if (addressDebugInst) {
      if (auto varInfo = addressDebugInst.getVarInfo()) {
        SILBuilderWithScope undefBuilder(builder);
        undefBuilder.setCurrentDebugScope(addressDebugInst->getDebugScope());
        undefBuilder.createDebugValue(
            addressDebugInst->getLoc(),
            SILUndef::get(address->getType(), builder.getModule()), *varInfo,
            false,
            /*was moved*/ true);
      }
      addressDebugInst.markAsMoved();
    }

    useState.destroys.erase(dvi);
    mvi->eraseFromParent();
    dvi->eraseFromParent();
    return false;
  }
  case DownwardScanResult::ClosureUse: {
    assert(interestingUse);
    assert(interestingUser);

    // Then check if we found a user that violated our dataflow rules. In such
    // a case, emit an error, cleanup our mark_unresolved_move_addr, and
    // finally continue.
    auto &astCtx = mvi->getFunction()->getASTContext();
    {
      auto diag =
          diag::sil_movechecking_value_used_after_consume;
      StringRef name = getDebugVarName(address);
      diagnose(astCtx, getSourceLocFromValue(address), diag, name);
    }

    auto diag = diag::sil_movechecking_consuming_use_here;
    diagnose(astCtx, mvi->getLoc().getSourceLoc(), diag);

    {
      auto diag = diag::sil_movechecking_nonconsuming_use_here;
      for (auto *user : interestingClosureUsers) {
        diagnose(astCtx, user->getLoc().getSourceLoc(), diag);
      }
    }

    // We purposely continue to see if at least in simple cases, we can flag
    // mistakes from other moves. Since we are setting emittedDiagnostic to
    // true, we will not perform the actual dataflow due to a check after
    // the loop.
    //
    // We also clean up mvi by converting it to a copy_addr init so we do not
    // emit fail errors later.
    //
    // TODO: Can we handle multiple errors in the same block for a single
    // move?
    SILBuilderWithScope builder(mvi);
    builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(),
                           IsNotTake, IsInitialization);
    mvi->eraseFromParent();
    return true;
  }
  case DownwardScanResult::UseForDiagnostic: {
    assert(!interestingUse);
    assert(interestingUser);

    // Then check if we found a user that violated our dataflow rules. In such
    // a case, emit an error, cleanup our mark_unresolved_move_addr, and
    // finally continue.
    auto &astCtx = mvi->getFunction()->getASTContext();
    {
      auto diag =
          diag::sil_movechecking_value_used_after_consume;
      StringRef name = getDebugVarName(address);
      diagnose(astCtx, getSourceLocFromValue(address), diag, name);
    }

    {
      auto diag = diag::sil_movechecking_consuming_use_here;
      diagnose(astCtx, mvi->getLoc().getSourceLoc(), diag);
    }

    {
      auto diag = diag::sil_movechecking_nonconsuming_use_here;
      diagnose(astCtx, interestingUser->getLoc().getSourceLoc(), diag);
    }

    // We purposely continue to see if at least in simple cases, we can flag
    // mistakes from other moves. Since we are setting emittedDiagnostic to
    // true, we will not perform the actual dataflow due to a check after
    // the loop.
    //
    // We also clean up mvi by converting it to a copy_addr init so we do not
    // emit fail errors later.
    //
    // TODO: Can we handle multiple errors in the same block for a single
    // move?
    SILBuilderWithScope builder(mvi);
    builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(),
                           IsNotTake, IsInitialization);
    mvi->eraseFromParent();
    return true;
  }
  case DownwardScanResult::Reinit: {
    assert(!interestingUse);
    assert(interestingUser);

    // If we have a reinit, then we have a successful move.
    convertMemoryReinitToInitForm(interestingUser);
    useState.reinits.erase(interestingUser);
    SILBuilderWithScope builder(mvi);
    builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(), IsTake,
                           IsInitialization);
    if (addressDebugInst) {
      if (auto varInfo = addressDebugInst.getVarInfo()) {
        {
          SILBuilderWithScope undefBuilder(builder);
          undefBuilder.setCurrentDebugScope(addressDebugInst->getDebugScope());
          undefBuilder.createDebugValue(
              addressDebugInst->getLoc(),
              SILUndef::get(address->getType(), builder.getModule()), *varInfo,
              false,
              /*was moved*/ true);
        }
        {
          // Make sure at the reinit point to create a new debug value after the
          // reinit instruction so we reshow the variable.
          auto *next = interestingUser->getNextInstruction();
          SILBuilderWithScope reinitBuilder(next);
          reinitBuilder.setCurrentDebugScope(addressDebugInst->getDebugScope());
          reinitBuilder.createDebugValue(addressDebugInst->getLoc(), address,
                                         *varInfo, false,
                                         /*was moved*/ true);
        }
      }
      addressDebugInst.markAsMoved();
    }
    mvi->eraseFromParent();
    return false;
  }
  case DownwardScanResult::ClosureConsume: {
    assert(interestingUse);
    assert(interestingUser);

    // If we found a closure consume, then we found a single block case that we
    // can handle. Remove the destroys/reinit, register the specific.
    SILBuilderWithScope builder(mvi);
    builder.createCopyAddr(mvi->getLoc(), mvi->getSrc(), mvi->getDest(), IsTake,
                           IsInitialization);

    // This is correct today due to us only supporting defer. When we handle
    // partial apply, we will need to do more work ehre.
    FullApplySite fas(interestingUser);
    assert(fas);
    auto &bitVector = applySiteToPromotedArgIndices[fas];
    auto conventions = fas.getSubstCalleeConv();
    unsigned numNonResultArgs = conventions.getNumSILArguments();
    if (bitVector.size() < numNonResultArgs)
      bitVector.resize(numNonResultArgs);
    bitVector.set(fas.getAppliedArgIndex(*interestingUse));
    for (auto *user : interestingClosureUsers) {
      closureConsumes.insert(user);
    }
    LLVM_DEBUG(llvm::dbgs() << "Found apply site to clone: " << **fas);
    LLVM_DEBUG(llvm::dbgs() << "BitVector: ";
               dumpBitVector(llvm::dbgs(), bitVector); llvm::dbgs() << '\n');
    if (addressDebugInst) {
      if (auto varInfo = addressDebugInst.getVarInfo()) {
        SILBuilderWithScope undefBuilder(builder);
        undefBuilder.setCurrentDebugScope(addressDebugInst->getDebugScope());
        undefBuilder.createDebugValue(
            addressDebugInst->getLoc(),
            SILUndef::get(address->getType(), builder.getModule()), *varInfo,
            false,
            /*was moved*/ true);
      }
      addressDebugInst.markAsMoved();
    }
    mvi->eraseFromParent();
    return false;
  }
  case DownwardScanResult::MoveOut:
    assert(!interestingUse);
    assert(!interestingUser);
    break;
  }

  // If we did not found any uses later in the block that was an interesting
  // use, we need to perform dataflow.
  LLVM_DEBUG(llvm::dbgs() << "Our move is live out, so we need to process "
                             "it with the dataflow.\n");
  dataflowState.markMovesThatPropagateDownwards.emplace_back(mvi);

  // Now scan up to see if mvi is also a use to seed the dataflow. This could
  // happen if we have an earlier move.
  if (upwardScanForUseOut(mvi, dataflowState.useState)) {
    LLVM_DEBUG(llvm::dbgs() << "MVI projects a use up");
    dataflowState.useBlocks[mvi->getParent()] = mvi;
  }
  return false;
}

bool ConsumeOperatorCopyableAddressesChecker::check(SILValue address) {
  auto accessPathWithBase = AccessPathWithBase::compute(address);
  auto accessPath = accessPathWithBase.accessPath;

  // Bail on an invalid AccessPath.
  //
  // AccessPath completeness is verified independently--it may be invalid in
  // extraordinary situations. When AccessPath is valid, we know all its uses
  // are recognizable.
  //
  // NOTE: If due to an invalid access path we fail here, we will just error
  // on the _move since the _move would not have been handled.
  if (!accessPath.isValid())
    return false;

  GatherLexicalLifetimeUseVisitor visitor(useState);
  SWIFT_DEFER { visitor.clear(); };
  visitor.reset(address);
  if (!visitAccessPathUses(visitor, accessPath, fn))
    return false;

  // See if our base address is an inout. If we found any moves, add as a
  // liveness use all function terminators.
  if (auto *fArg = dyn_cast<SILFunctionArgument>(address)) {
    if (fArg->hasConvention(SILArgumentConvention::Indirect_Inout)) {
      if (visitor.useState.markMoves.size()) {
        SmallVector<SILBasicBlock *, 2> exitingBlocks;
        fn->findExitingBlocks(exitingBlocks);
        for (auto *block : exitingBlocks) {
          visitor.useState.livenessUses.insert(block->getTerminator());
        }
      }
    }
  }

  // Now initialize our data structures.
  SWIFT_DEFER { dataflowState.clear(); };

  // First go through and perform dataflow on each of the closures our address
  // depends on. We do not have to worry about other unrelated addresses from
  // being passed to the defer in our argument slot since address phis are
  // banned in canonical SIL.
  //
  // This summary will let us treat the whole closure's effect on the closure
  // operand as if it was a single instruction.
  for (auto &pair : useState.closureUses) {
    auto *operand = pair.first;
    auto &closureState = pair.second;

    if (!performClosureDataflow(operand, closureState)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "!! Early exit due to failing to analyze closure operand: "
                 << *operand->getUser());
      return false;
    }
  }

  // Perform the single basic block analysis emitting a diagnostic/pairing
  // mark_unresolved_move_addr and destroys if needed. If we discover a
  // mark_move that propagates its state out of the current block, this
  // routine also prepares the pass for running the multi-basic block
  // diagnostic.
  bool emittedSingleBBDiagnostic = false;

  // Before we process any moves, gather the debug inst associated with our
  // address.
  //
  // NOTE: The reason why we do this early is that we rely on our address
  // initially having a single DebugValueCarryingInst (either an alloc_stack
  // itself or a debug_value associated with an argument). If we do this while
  // processing, as we insert additional debug info we will cause this condition
  // to begin failing.
  auto addressDebugInst = DebugVarCarryingInst::getFromValue(address);

  for (auto *mvi : useState.markMoves) {
    LLVM_DEBUG(llvm::dbgs() << "Performing single block analysis on: " << *mvi);
    emittedSingleBBDiagnostic |=
        performSingleBasicBlockAnalysis(address, addressDebugInst, mvi);
  }

  if (emittedSingleBBDiagnostic) {
    LLVM_DEBUG(llvm::dbgs()
               << "Performed single block analysis and found error!\n");
    return true;
  }

  // Then check if we do not need to propagate down any mark moves. In that
  // case, since we did not emit an error but we did not have any
  if (dataflowState.markMovesThatPropagateDownwards.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "Single block analysis handled all cases "
                               "without finding an error!\n");
    return true;
  }

  // Ok, we need to perform global dataflow for one of our moves. Initialize our
  // dataflow state engine and then run the dataflow itself.
  dataflowState.init();
  bool result =
      dataflowState.process(address, addressDebugInst, closureConsumes);
  return result;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class ConsumeOperatorCopyableAddressesCheckerPass
    : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();
    auto &astContext = fn->getASTContext();

    // Only run this pass if the move only language feature is enabled.
    if (!astContext.supportsMoveOnlyTypes())
      return;

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    SmallSetVector<SILValue, 32> addressesToCheck;

    for (auto *arg : fn->front().getSILFunctionArguments()) {
      if (arg->getType().isAddress() &&
          (arg->hasConvention(SILArgumentConvention::Indirect_In) ||
           arg->hasConvention(SILArgumentConvention::Indirect_Inout)))
        addressesToCheck.insert(arg);
    }

    for (auto &block : *fn) {
      for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
        auto *inst = &*ii;
        ++ii;

        if (auto *asi = dyn_cast<AllocStackInst>(inst)) {
          // Only check lexical alloc_stack that were not emitted as vars.
          if (asi->isLexical()) {
            LLVM_DEBUG(llvm::dbgs() << "Found lexical alloc_stack: " << *asi);
            addressesToCheck.insert(asi);
            continue;
          }
        }
      }
    }

    LLVM_DEBUG(llvm::dbgs() << "Visiting Function: " << fn->getName() << "\n");
    auto addressToProcess =
        llvm::makeArrayRef(addressesToCheck.begin(), addressesToCheck.end());

    SILOptFunctionBuilder funcBuilder(*this);

    ConsumeOperatorCopyableAddressesChecker checker(getFunction(), funcBuilder);

    bool madeChange = false;
    while (!addressToProcess.empty()) {
      auto address = addressToProcess.front();
      addressToProcess = addressToProcess.drop_front(1);
      LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *address);
      madeChange |= checker.check(address);
    }

    if (madeChange) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }

    // Now go through and clone any apply sites that we need to clone.
    SmallVector<SILValue, 8> newArgs;
    bool rewroteCallee = false;
    for (auto &pair : checker.applySiteToPromotedArgIndices) {
      SWIFT_DEFER { newArgs.clear(); };
      auto fas = pair.first;
      auto &bitVector = pair.second;
      LLVM_DEBUG(llvm::dbgs() << "CLONING APPLYSITE: " << **fas);
      LLVM_DEBUG(llvm::dbgs() << "BitVector: ";
                 dumpBitVector(llvm::dbgs(), bitVector); llvm::dbgs() << '\n');
      checker.cloneDeferCalleeAndRewriteUses(newArgs, bitVector, fas,
                                             checker.closureConsumes);
      rewroteCallee = true;
    }
    if (rewroteCallee)
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);

    // Now search through our function one last time and any move_value
    // [allows_diagnostics] that remain are ones that we did not know how to
    // check so emit a diagnostic so the user doesn't assume that they have
    // guarantees. This gives us the guarantee that any moves written by the
    // user must have been properly resolved and thus maintain that all move
    // uses have been resolved appropriately.
    //
    // TODO: Emit specific diagnostics here (e.x.: _move of global).
    if (DisableUnhandledConsumeOperator)
      return;

    bool lateMadeChange = false;
    for (auto &block : *fn) {
      for (auto ii = block.begin(), ie = block.end(); ii != ie;) {
        auto *inst = &*ii;
        ++ii;

        if (auto *mai = dyn_cast<MarkUnresolvedMoveAddrInst>(inst)) {
          auto diag =
              diag::sil_movekillscopyablevalue_move_applied_to_unsupported_move;
          diagnose(astContext, mai->getLoc().getSourceLoc(), diag);

          // Now that we have emitted the error, replace the move_addr with a
          // copy_addr so that future passes never see it. We mark it as a
          // copy_addr [init].
          SILBuilderWithScope builder(mai);
          builder.createCopyAddr(mai->getLoc(), mai->getSrc(), mai->getDest(),
                                 IsNotTake, IsInitialization);
          mai->eraseFromParent();
          lateMadeChange = true;
        }
      }
    }
    if (lateMadeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // anonymous namespace

SILTransform *swift::createConsumeOperatorCopyableAddressesChecker() {
  return new ConsumeOperatorCopyableAddressesCheckerPass();
}
