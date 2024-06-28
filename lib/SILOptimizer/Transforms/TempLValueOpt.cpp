//===--- TempLValueOpt.cpp - Optimize temporary l-values ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This pass optimizes copies from a temporary (an "l-value") to a destination.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cow-opts"

#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

/// Optimizes copies from a temporary (an "l-value") to a destination.
///
/// \code
///   %temp = alloc_stack $Ty
///   instructions_which_store_to %temp
///   copy_addr [take] %temp to %destination
///   dealloc_stack %temp
/// \endcode
///
/// is optimized to
///
/// \code
///   destroy_addr %destination
///   instructions_which_store_to %destination
/// \endcode
///
/// The name TempLValueOpt refers to the TempRValueOpt pass, which performs
/// a related transformation, just with the temporary on the "right" side.
///
/// Note that TempLValueOpt is similar to CopyForwarding::backwardPropagateCopy.
/// It's more restricted (e.g. the copy-source must be an alloc_stack).
/// That enables other patterns to be optimized, which backwardPropagateCopy
/// cannot handle.
///
/// The pass also performs a peephole optimization on copy_addr - destroy
/// sequences.
/// It replaces
///
/// \code
///   copy_addr %source to %destination
///   destroy_addr %source
/// \endcode
///
/// with
///
/// \code
///   copy_addr [take] %source to %destination
/// \endcode
///
/// This peephole optimization is also done by the DestroyHoisting pass. But
/// DestroyHoisting currently only runs on OSSA.
/// TODO: when DestroyHoisting can run later in the pipeline, check if we still
///       need this peephole here.
class TempLValueOptPass : public SILFunctionTransform {
public:
  TempLValueOptPass() {}

  void run() override;

private:
  void tempLValueOpt(CopyAddrInst *copyInst);
  void combineCopyAndDestroy(CopyAddrInst *copyInst);
};

void TempLValueOptPass::run() {
  SILFunction *F = getFunction();
  if (!F->shouldOptimize())
    return;

  LLVM_DEBUG(llvm::dbgs() << "*** TempLValueOptPass on function: "
                          << F->getName() << " ***\n");

  for (SILBasicBlock &block : *F) {
    // First collect all copy_addr instructions upfront to avoid iterator
    // invalidation problems (the optimizations might delete the copy_addr
    // itself or any following instruction).
    llvm::SmallVector<CopyAddrInst *, 32> copyInsts;
    for (SILInstruction &inst : block) {
      if (auto *copyInst = dyn_cast<CopyAddrInst>(&inst))
        copyInsts.push_back(copyInst);
    }
    // Do the optimizations.
    for (CopyAddrInst *copyInst : copyInsts) {
      combineCopyAndDestroy(copyInst);
      tempLValueOpt(copyInst);
    }
  }
}

static SingleValueInstruction *isMovableProjection(SILValue addr) {
  if (auto *enumData = dyn_cast<InitEnumDataAddrInst>(addr))
    return enumData;
  if (auto *existentialAddr = dyn_cast<InitExistentialAddrInst>(addr))
    return existentialAddr;
    
  if (SingleValueInstruction *svi = Projection::isAddressProjection(addr)) {
    if (svi->getNumOperands() == 1)
      return svi;
  }
  return nullptr;
}

void TempLValueOptPass::tempLValueOpt(CopyAddrInst *copyInst) {

  // An overview of the algorithm:
  //
  //   alloc_stack %temp
  //   ...
  //   first_use_of %temp // beginOfLiverange
  //   ... // no reads or writes from/to %destination
  //   copy_addr [take] %temp to %destination // copyInst
  //   ... // no further uses of %temp (copyInst is the end of %temp liverange)
  //   dealloc_stack %temp
  //
  // All projections to %destination are hoisted above the first use of %temp.
  // Then all uses of %temp are replaced by %destination.
  // In case the copyInst is not initializing %destination, a
  // 'destroy_addr %destination' is inserted before the first use of %temp.

  if (!copyInst->isTakeOfSrc())
    return;

  auto *temporary = dyn_cast<AllocStackInst>(copyInst->getSrc());
  if (!temporary)
    return;

  // Collect all users of the temporary into a set. Also, for simplicity,
  // require that all users are within a single basic block.
  InstructionSet users(getFunction());
  SILBasicBlock *block = temporary->getParent();
  for (Operand *use : temporary->getUses()) {
    SILInstruction *user = use->getUser();
    if (user->getParent() != block)
      return;
    users.insert(user);
  }

  // Collect all address projections of the destination - in case we need to
  // hoist them.
  InstructionSet projections(getFunction());
  SILValue destination = copyInst->getDest();
  SILValue destRootAddr = destination;
  while (SingleValueInstruction *projInst = isMovableProjection(destRootAddr)) {
    // In theory, users of the temporary and address projections of the
    // destination should be completely distinct. Otherwise the copyInst would
    // be an identity copy (source == destination).
    // Just to be on the safe side, bail if it's not the case (instead of an
    // assert).
    if (users.contains(projInst))
      return;
    projections.insert(projInst);
    destRootAddr = projInst->getOperand(0);
  }
  // The root address of the destination. It's null if it's not an instruction,
  // but a block argument.
  SILInstruction *destRootInst = destRootAddr->getDefiningInstruction();

  bool needDestroyEarly = false;
  BasicCalleeAnalysis *bca = nullptr;
  if (!copyInst->isInitializationOfDest()) {
    needDestroyEarly = true;
    bca = PM->getAnalysis<BasicCalleeAnalysis>();
  }

  // Iterate over the liverange of the temporary and make some validity checks.
  AliasAnalysis *AA = nullptr;
  SILInstruction *beginOfLiverange = nullptr;
  bool endOfLiverangeReached = false;
  for (auto iter = temporary->getIterator(); iter != block->end(); ++iter) {
    SILInstruction *inst = &*iter;
    // The dealloc_stack is the last user of the temporary.
    if (isa<DeallocStackInst>(inst) && inst->getOperand(0) == temporary)
      break;

    if (users.contains(inst) != 0) {
      // Check if the copyInst is the last user of the temporary (beside the
      // dealloc_stack).
      if (endOfLiverangeReached)
        return;

      // Find the first user of the temporary to get a more precise liverange.
      // It would be too conservative to treat the alloc_stack itself as the
      // begin of the liverange.
      if (!beginOfLiverange)
        beginOfLiverange = inst;

      if (inst == copyInst)
        endOfLiverangeReached = true;
    }
    if (beginOfLiverange && !endOfLiverangeReached) {
      // If the root address of the destination is within the liverange of the
      // temporary, we cannot replace all uses of the temporary with the
      // destination (it would break the def-use dominance rule).
      if (inst == destRootInst)
        return;
        
      if (!AA)
        AA = PM->getAnalysis<AliasAnalysis>(getFunction());

      // Check if the destination is not accessed within the liverange of
      // the temporary.
      // This is unlikely, because the destination is initialized at the
      // copyInst. But still, the destination could contain an initialized value
      // which is destroyed before the copyInst.
      if (AA->mayReadOrWriteMemory(inst, destination) &&
          // Needed to treat init_existential_addr as not-writing projection.
          projections.contains(inst) == 0)
        return;

      if (needDestroyEarly && isDeinitBarrier(inst, bca))
        return;
    }
  }
  assert(endOfLiverangeReached);

  // Move all projections of the destination address before the liverange of
  // the temporary.
  for (auto iter = beginOfLiverange->getIterator();
       iter != copyInst->getIterator();) {
    SILInstruction *inst = &*iter++;
    if (projections.contains(inst))
      inst->moveBefore(beginOfLiverange);
  }

  if (!copyInst->isInitializationOfDest()) {
    // Make sure the destination is uninitialized before the liverange of
    // the temporary.
    SILBuilderWithScope builder(beginOfLiverange);
    builder.createDestroyAddr(copyInst->getLoc(), destination);
  }

  // Replace all uses of the temporary with the destination address.
  while (!temporary->use_empty()) {
    Operand *use = *temporary->use_begin();
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
      case SILInstructionKind::DeallocStackInst:
        user->eraseFromParent();
        break;
      default:
        use->set(destination);
    }
  }
  temporary->eraseFromParent();
  copyInst->eraseFromParent();
  invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
}

void TempLValueOptPass::combineCopyAndDestroy(CopyAddrInst *copyInst) {
  if (copyInst->isTakeOfSrc())
    return;

  // Find a destroy_addr of the copy's source address.
  DestroyAddrInst *destroy = nullptr;
  for (Operand *srcUse : copyInst->getSrc()->getUses()) {
    if ((destroy = dyn_cast<DestroyAddrInst>(srcUse->getUser())))
      break;
  }
  SILBasicBlock *block = copyInst->getParent();
  if (!destroy || destroy->getParent() != block)
    return;
  assert(destroy->getOperand() == copyInst->getSrc());

  // Check if the destroy_addr is after the copy_addr and if there are no
  // memory accesses between them.
  SmallVector<SILInstruction *, 4> debugInsts;
  for (auto iter = std::next(copyInst->getIterator());
       iter != block->end(); ++iter) {
    SILInstruction *inst = &*iter;
    if (inst == destroy) {
      copyInst->setIsTakeOfSrc(IsTake);
      destroy->eraseFromParent();
      // Cleanup all debug_value of the copy src btw the copy and destroy
      for (auto debugInst : debugInsts) {
        debugInst->eraseFromParent();
      }
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
      return;
    }
    if(auto *debugInst = DebugValueInst::hasAddrVal(inst)) {
      if (debugInst->getOperand() == copyInst->getSrc())
        debugInsts.push_back(debugInst);
    }
    if (inst->mayReadOrWriteMemory())
      return;
  }
}

} // end anonymous namespace

SILTransform *swift::createTempLValueOpt() {
  return new TempLValueOptPass();
}

