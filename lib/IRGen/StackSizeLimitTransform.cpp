//===--- StackSizeLimitTransform.cpp --------------------------------------===//
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
/// \file
///
/// This is a transform that ensures that swift allocations never blow out the
/// stack. We do this by limiting per function the amount of stack space
/// allocated and otherwise, heap allocate classes, values.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-irgen-stack-size-limit"

#include "ClassTypeInfo.h"
#include "IRGen.h"
#include "IRGenModule.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/Basic/BlotMapVector.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/IRGen/IRGenSILPasses.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILInstructionWorklist.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeInstruction.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/IR/Constants.h"

using namespace swift;
using namespace swift::irgen;

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

/// Given an alloc_stack \p asi and the next element to process, convert asi's
/// users to use a new heap allocated box without invalidating next.
static SILBasicBlock::iterator
convertStackBoxToHeapBox(AllocStackInst *asi, SILBasicBlock::iterator next) {
  SILBuilderWithScope builder(asi);
  auto *box =
      builder.createAllocBox(asi->getLoc(), asi->getType().getObjectType());
  auto *projectBox = builder.createProjectBox(asi->getLoc(), box, 0);
  while (!asi->use_empty()) {
    auto *use = *asi->use_begin();

    // Replace all of the alloc_stack's dealloc_stack with a destroy_value of
    // the box.
    if (auto *dsi = dyn_cast<DeallocStackInst>(use->getUser())) {
      SILBuilderWithScope destroyBuilder(dsi);
      destroyBuilder.emitDestroyValueOperation(dsi->getLoc(), box);
      if (next == dsi->getIterator())
        ++next;
      dsi->eraseFromParent();
      continue;
    }

    // Otherwise, make the alloc_stack user now use the project_box.
    use->set(projectBox);
  }

  // Then erase the alloc_stack, incrementing next as we need to.
  if (next == asi->getIterator())
    ++next;
  asi->eraseFromParent();
  return next;
}

//===----------------------------------------------------------------------===//
//                       Per Instruction Implementation
//===----------------------------------------------------------------------===//

namespace {

struct StackSizeLimitState {
  SmallVector<std::pair<AllocRefInst *, BuiltinInst *>, 32> builtinsToProcess;
  bool madeChange = false;
  int stackAllocSize;
  IRGenModule &igm;

  // Max stack promotion that we can perform in a single function.
  StackSizeLimitState(IRGenModule *igm)
      : stackAllocSize(igm->IRGen.Opts.StackPromotionSizeLimit), igm(*igm) {}
};

} // anonymous namespace

static SILBasicBlock::iterator
processAllocStackInst(StackSizeLimitState &state, AllocStackInst *asi,
                      SILBasicBlock::iterator next) {
  // First see if we have already hit our size limit. If we have, just
  // convert to a heap allocation.
  if (state.stackAllocSize < 0) {
    // Otherwise, convert the alloc_stack to an alloc_box and continue.
    next = convertStackBoxToHeapBox(asi, next);
    state.madeChange = true;
    return next;
  }

  // Otherwise, subtract this alloc_stack's size from our max
  // stack. Again, if we the stack limit and the alloc_stack does not
  // have a dynamic lifetime, convert it to a box.
  const irgen::TypeInfo &type = state.igm.getTypeInfo(asi->getElementType());
  if (llvm::Constant *sizeConst = type.getStaticSize(state.igm)) {
    auto *sizeInt = cast<llvm::ConstantInt>(sizeConst);
    // Should this be using Size(...?)
    state.stackAllocSize -= (int)sizeInt->getSExtValue();
    if (state.stackAllocSize < 0) {
      next = convertStackBoxToHeapBox(asi, next);
      state.madeChange = true;
      return next;
    }
  }

  return next;
}

static SILBasicBlock::iterator
processAllocRefInst(StackSizeLimitState &state, AllocRefInst *ari,
                    SILBasicBlock::iterator next) {
  // Skip alloc_ref that are not marked as can alloc stack or ones that
  // are marked as is objc.
  if (!ari->canAllocOnStack() || ari->isObjC()) {
    // We can not stack allocate objective-c allocations.
    state.madeChange = true;
    ari->setStackAllocatable(false);
    return next;
  }

  SILType selfType = ari->getType();
  auto &classTI = state.igm.getTypeInfo(selfType).as<irgen::ClassTypeInfo>();
  auto &classLayout = classTI.getClassLayout(state.igm, selfType,
                                             /*forBackwardDeployment=*/false);
  if (!classLayout.isFixedLayout()) {
    // We never stack allocate if we don't have a fixed layout.
    state.madeChange = true;
    ari->setStackAllocatable(false);
    return next;
  }

  // Calculate the total size needed.
  // The first part is the size of the class itself.
  Alignment classAlignment = classLayout.getAlignment();
  Size accumulatedSize = classLayout.getSize();

  auto tailTypeArray = ari->getTailAllocatedTypes();
  auto tailCountArray = ari->getTailAllocatedCounts();
  for (auto p : llvm::zip(tailTypeArray, tailCountArray)) {
    SILType eltType = std::get<0>(p);
    Operand &operand = std::get<1>(p);
    auto *count = dyn_cast<IntegerLiteralInst>(operand.get());
    if (!count) {
      state.madeChange = true;
      ari->setStackAllocatable(false);
      return next;
    }

    const TypeInfo &elemTI = state.igm.getTypeInfo(eltType);
    if (!elemTI.isFixedSize()) {
      state.madeChange = true;
      ari->setStackAllocatable(false);
      return next;
    }

    const FixedTypeInfo &elemFTI = elemTI.as<FixedTypeInfo>();
    Alignment elemAlign = elemFTI.getFixedAlignment();

    // This should not happen - just to be safe, mark the whole
    // allocation as on the heap.
    if (elemAlign > classAlignment) {
      state.madeChange = true;
      ari->setStackAllocatable(false);
      return next;
    }

    accumulatedSize = accumulatedSize.roundUpToAlignment(elemAlign);
    accumulatedSize +=
        count->getValue().getZExtValue() * elemFTI.getFixedStride();
  }

  if (accumulatedSize > Size(state.stackAllocSize)) {
    state.madeChange = true;
    ari->setStackAllocatable(false);
    return next;
  }

  // Otherwise, we can stack allocate.
  state.stackAllocSize -= accumulatedSize.getValue();
  return next;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class StackSizeLimitTransform : public SILFunctionTransform {
  void run() override {
    auto *f = getFunction();
    auto *igm = getIRGenModule();
    StackSizeLimitState state(igm);
    bool madeChange = false;

    // For now do a simple single pass, gathering builtins we want to expand and
    // deciding which on stack must really be on the heap.
    for (auto &block : *f) {
      for (auto next = block.begin(), ie = block.end(); next != ie;) {
        // If we see an alloc_stack...
        if (auto *asi = dyn_cast<AllocStackInst>(&*next)) {
          next = processAllocStackInst(state, asi, std::next(next));
          continue;
        }

        // If we have an alloc_ref, then check if this is an alloc_ref that fits
        // within our size limit.
        if (auto *ari = dyn_cast<AllocRefInst>(&*next)) {
          next = processAllocRefInst(state, ari, std::next(next));
          continue;
        }

        // Otherwise, see if we have our builtin and we can connect it to a
        // specific alloc_ref [stack]. In that case,
        auto *bi = dyn_cast<BuiltinInst>(&*next);
        ++next;

        if (!bi)
          continue;

        auto kind = bi->getBuiltinKind();
        if (!kind || *kind != BuiltinValueKind::IsOnStack)
          continue;

        // If we were passed an is class on stack or heap, check if:
        //
        // 1. We have an object.
        // 2. If this object (looking through casts) is an alloc_ref that is on
        //    the stack.
        SILValue arg = bi->getArguments()[0];
        assert(arg->getType().isObject() &&
               "IsClassOnStackOrHeap can only work on object types");
        arg = stripCasts(arg);
        auto *allocRef = dyn_cast<AllocRefInst>(arg);
        if (!allocRef) {
          // Delete the builtin and put in a trap. User error!
          //
          // TODO: Improve diagnostic.
          SILBuilderWithScope builder(bi);
          builder.createBuiltinTrap(bi->getLoc());
          if (next == bi->getIterator())
            ++next;
          bi->replaceAllUsesWithUndef();
          bi->eraseFromParent();
          madeChange = true;
          continue;
        }

        // Ok, in this case, we /could/ put this alloc_ref on the stack if we
        // aren't going to blow out the stack size.
        //
        // Stash our builtin for processing once we have finished
        // estimatingStackSize.
        state.builtinsToProcess.emplace_back(allocRef, bi);
      }

      // Then go over our (AllocRef, [BuiltinInst]) cleaning up as we go.
      while (!state.builtinsToProcess.empty()) {
        // Any ari left that can be allocated on the stack will actually be
        // allocated on the stack.
        AllocRefInst *ari;
        BuiltinInst *bi;
        std::tie(ari, bi) = state.builtinsToProcess.pop_back_val();
        SILBuilderWithScope builder(bi);
        auto *truthValue = builder.createIntegerLiteralBool(
            ari->getLoc(), bool(ari->canAllocOnStack()));
        bi->replaceAllUsesWith(truthValue);
        bi->eraseFromParent();
        madeChange = true;
      }

      if (madeChange)
        invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    }
  };
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

SILTransform *irgen::createStackSizeLimitTransform() {
  return new StackSizeLimitTransform();
}
