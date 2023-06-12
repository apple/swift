//===--- SILLowerAggregateInstrs.cpp - Aggregate insts to Scalar insts  ---===//
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
///
/// \file
///
/// Simplify aggregate instructions into scalar instructions using simple
/// peephole transformations.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-lower-aggregate-instrs"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::Lowering;

STATISTIC(NumExpand, "Number of instructions expanded");

static llvm::cl::opt<bool> EnableExpandAll("sil-lower-agg-instrs-expand-all",
                                           llvm::cl::init(false));

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

/// We only expand if we are not in ownership and shouldExpand is true. The
/// reason why is that this was originally done to help the low level ARC
/// optimizer. To the high level ARC optimizer, this is just noise and
/// unnecessary IR. At the same time for testing purposes, we want to provide a
/// way even with ownership enabled to expand so we can check correctness.
static bool shouldExpandShim(SILFunction *fn, SILType type) {
  // shouldExpand returns false for struct-with-deinit types, so bypassing it is
  // incorrect for move-only types
  if (EnableExpandAll) {
    assert(!type.isPureMoveOnly()
           && "sil-lower-agg-instrs-expand-all is incompatible with move-only "
              "types");
    return true;
  }
  return !fn->hasOwnership() && shouldExpand(fn->getModule(), type);
}

//===----------------------------------------------------------------------===//
//                      Higher Level Operation Expansion
//===----------------------------------------------------------------------===//

/// Lower copy_addr into loads/stores/retain/release if we have a
/// non-address only type. We do this here so we can process the resulting
/// loads/stores.
///
/// This peephole implements the following optimizations with the ossa version
/// of the optimization first.
///
/// copy_addr %0 to %1 : $*T
/// ->
///     %new = load [copy] %0 : $*T
///     store %new to [assign] %1 : $*T
/// ->
///     %new = load %0 : $*T        // Load the new value from the source
///     strong_retain %new : $T     // Retain the new value
///     %old = load %1 : $*T        // Load the old value from the destination
///     strong_release %old : $T    // Release the old
///     store %new to %1 : $*T      // Store the new value to the destination
///
/// copy_addr [take] %0 to %1 : $*T
/// ->
///     // load [take], not load [copy]!
///     %new = load [take] %0 : $*T
///     store %new to [assign] %1 : $*T
/// ->
///     %new = load %0 : $*T
///     // no retain of %new!
///     %old = load %1 : $*T
///     strong_release %old : $T
///     store %new to %1 : $*T
///
/// copy_addr %0 to [init] %1 : $*T
/// ->
///     %new = load [copy] %0 : $*T
///     store %new to [init] %1 : $*T
/// ->
///     %new = load %0 : $*T
///     strong_retain %new : $T
///     // no load/release of %old!
///     store %new to %1 : $*T
///
/// copy_addr [take] %0 to [init] %1 : $*T
/// ->
///     %new = load [take] %0 : $*T
///     store %new to [init] %1 : $*T
/// ->
///     %new = load %0 : $*T
///     // no retain of %new!
///     // no load/release of %old!
///     store %new to %1 : $*T
static bool expandCopyAddr(CopyAddrInst *cai) {
  SILFunction *fn = cai->getFunction();
  SILValue source = cai->getSrc();

  // If we have an address only type don't do anything.
  SILType srcType = source->getType();
  if (srcType.isAddressOnly(*fn))
    return false;

  bool expand = shouldExpandShim(fn, srcType.getObjectType());
  using TypeExpansionKind = Lowering::TypeLowering::TypeExpansionKind;
  auto expansionKind = expand ? TypeExpansionKind::MostDerivedDescendents
                              : TypeExpansionKind::None;

  SILBuilderWithScope builder(cai);

  // If our object type is not trivial, we may need to destroy the old value and
  // copy the new one. Handle the trivial case quickly and return.
  if (srcType.isTrivial(*fn)) {
    SILValue newValue = builder.emitLoadValueOperation(
        cai->getLoc(), source, LoadOwnershipQualifier::Trivial);
    SILValue destAddr = cai->getDest();
    // Create the store.
    builder.emitStoreValueOperation(cai->getLoc(), newValue, destAddr,
                                    StoreOwnershipQualifier::Trivial);
    ++NumExpand;
    return true;
  }

  // %new = load [copy|take] %0 : $*T
  auto loadQual = [&]() -> LoadOwnershipQualifier {
    if (IsTake_t::IsTake == cai->isTakeOfSrc())
      return LoadOwnershipQualifier::Take;
    return LoadOwnershipQualifier::Copy;
  }();
  SILValue newValue = builder.emitLoweredLoadValueOperation(
      cai->getLoc(), source, loadQual, expansionKind);
  SILValue destAddr = cai->getDest();

  // Create the store in the guaranteed uninitialized memory.
  //
  // store %new to [init|assign] %1
  //
  // If we are not initializing the destination, we need to destroy what is
  // currently there before we re-initialize the memory.
  auto storeQualifier = [&]() -> StoreOwnershipQualifier {
    if (IsInitialization_t::IsInitialization != cai->isInitializationOfDest())
      return StoreOwnershipQualifier::Assign;
    return StoreOwnershipQualifier::Init;
  }();
  builder.emitLoweredStoreValueOperation(cai->getLoc(), newValue, destAddr,
                                         storeQualifier, expansionKind);

  ++NumExpand;
  return true;
}

static bool expandDestroyAddr(DestroyAddrInst *dai) {
  SILFunction *fn = dai->getFunction();
  SILBuilderWithScope builder(dai);

  // Strength reduce destroy_addr inst into release/store if
  // we have a non-address only type.
  SILValue addr = dai->getOperand();

  // If we have an address only type, do nothing.
  SILType type = addr->getType();
  if (type.isAddressOnly(*fn))
    return false;

  // We only expand if ownership is not enabled and we do not have a large
  // type. This was something that was only really beneficial for the low level
  // ARC optimizer which runs without ownership enabled.
  bool expand = shouldExpandShim(fn, type.getObjectType());

  // If we have a non-trivial type...
  if (!type.isTrivial(*fn)) {
    // If we have a type with reference semantics, emit a load/destroy.
    SILValue li = builder.emitLoadValueOperation(dai->getLoc(), addr,
                                                 LoadOwnershipQualifier::Take);
    auto &typeLowering = fn->getTypeLowering(type);
    using TypeExpansionKind = Lowering::TypeLowering::TypeExpansionKind;
    auto expansionKind = expand ? TypeExpansionKind::MostDerivedDescendents
                                : TypeExpansionKind::None;
    typeLowering.emitLoweredDestroyValue(builder, dai->getLoc(), li,
                                         expansionKind);
  }

  ++NumExpand;
  return true;
}

static bool expandReleaseValue(ReleaseValueInst *rvi) {
  SILFunction *fn = rvi->getFunction();
  SILBuilderWithScope builder(rvi);

  // Strength reduce destroy_addr inst into release/store if
  // we have a non-address only type.
  SILValue value = rvi->getOperand();

  // If we have an address only type, do nothing.
  SILType type = value->getType();
  assert(!SILModuleConventions(fn->getModule()).useLoweredAddresses() ||
         type.isLoadable(*fn) &&
             "release_value should never be called on a non-loadable type.");

  if (!shouldExpandShim(fn, type.getObjectType()))
    return false;

  auto &TL = fn->getTypeLowering(type);
  TL.emitLoweredDestroyValueMostDerivedDescendents(builder, rvi->getLoc(),
                                                   value);

  LLVM_DEBUG(llvm::dbgs() << "    Expanding: " << *rvi);

  ++NumExpand;
  return true;
}

static bool expandRetainValue(RetainValueInst *rvi) {
  SILFunction *fn = rvi->getFunction();
  SILBuilderWithScope builder(rvi);

  // Strength reduce destroy_addr inst into release/store if
  // we have a non-address only type.
  SILValue value = rvi->getOperand();

  // If we have an address only type, do nothing.
  SILType type = value->getType();
  assert(!SILModuleConventions(fn->getModule()).useLoweredAddresses() ||
         type.isLoadable(*fn) &&
             "Copy Value can only be called on loadable types.");

  if (!shouldExpandShim(fn, type.getObjectType()))
    return false;

  auto &typeLowering = fn->getTypeLowering(type);
  typeLowering.emitLoweredCopyValueMostDerivedDescendents(builder,
                                                          rvi->getLoc(), value);

  LLVM_DEBUG(llvm::dbgs() << "    Expanding: " << *rvi);

  ++NumExpand;
  return true;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

static bool processFunction(SILFunction &fn) {
  bool changed = false;
  for (auto &block : fn) {
    auto ii = block.begin(), ie = block.end();
    while (ii != ie) {
      SILInstruction *inst = &*ii;

      LLVM_DEBUG(llvm::dbgs() << "Visiting: " << *inst);

      if (auto *cai = dyn_cast<CopyAddrInst>(inst))
        if (expandCopyAddr(cai)) {
          ++ii;
          cai->eraseFromParent();
          changed = true;
          continue;
        }

      if (auto *dai = dyn_cast<DestroyAddrInst>(inst))
        if (expandDestroyAddr(dai)) {
          ++ii;
          dai->eraseFromParent();
          changed = true;
          continue;
        }

      if (auto *rvi = dyn_cast<RetainValueInst>(inst))
        if (expandRetainValue(rvi)) {
          ++ii;
          rvi->eraseFromParent();
          changed = true;
          continue;
        }

      if (auto *rvi = dyn_cast<ReleaseValueInst>(inst))
        if (expandReleaseValue(rvi)) {
          ++ii;
          rvi->eraseFromParent();
          changed = true;
          continue;
        }

      ++ii;
    }
  }
  return changed;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class SILLowerAggregate : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() override {
    SILFunction *f = getFunction();
    LLVM_DEBUG(llvm::dbgs() << "***** LowerAggregate on function: "
                            << f->getName() << " *****\n");
    bool changed = processFunction(*f);
    if (changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createLowerAggregateInstrs() {
  return new SILLowerAggregate();
}
