//===--- ReleaseDevirtualizer.cpp - Devirtualizes release-instructions ----===//
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

#define DEBUG_TYPE "release-devirtualizer"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/SubstitutionMap.h"
#include "llvm/ADT/Statistic.h"

STATISTIC(NumReleasesDevirtualized, "Number of devirtualized releases");

using namespace swift;

namespace {

/// Devirtualizes release instructions which are known to destruct the object.
///
/// This means, it replaces a sequence of
///    %x = alloc_ref [stack] $X
///      ...
///    strong_release %x
///    dealloc_ref [stack] %x
/// with
///    %x = alloc_ref [stack] $X
///      ...
///    set_deallocating %x
///    %d = function_ref @dealloc_of_X
///    %a = apply %d(%x)
///    dealloc_ref [stack] %x
///
/// The optimization is only done for stack promoted objects because they are
/// known to have no associated objects (which are not explicitly released
/// in the deinit method).
class ReleaseDevirtualizer : public SILFunctionTransform {

public:
  ReleaseDevirtualizer() {}

private:
  /// The entry point to the transformation.
  void run() override;

  /// Devirtualize releases of array buffers.
  bool devirtualizeReleaseOfObject(SILInstruction *ReleaseInst,
                                   DeallocRefInst *DeallocInst);

  /// Replace the release-instruction \p ReleaseInst with an explicit call to
  /// the deallocating destructor of \p AllocType for \p object.
  bool createDeallocCall(SILType AllocType, SILInstruction *ReleaseInst,
                         SILValue object);

  RCIdentityFunctionInfo *RCIA = nullptr;
};

void ReleaseDevirtualizer::run() {
  LLVM_DEBUG(llvm::dbgs() << "** ReleaseDevirtualizer **\n");

  SILFunction *F = getFunction();
  RCIA = PM->getAnalysis<RCIdentityAnalysis>()->get(F);

  SmallPtrSet<SILValue, 8> devirtualizedDestroys;  
  bool Changed = false;
  for (SILBasicBlock &BB : *F) {

    // The last release_value or strong_release instruction before the
    // deallocation.
    SILInstruction *LastRelease = nullptr;

    for (SILInstruction &I : BB) {
      if (LastRelease) {
        if (auto *DRI = dyn_cast<DeallocRefInst>(&I)) {
          if (devirtualizeReleaseOfObject(LastRelease, DRI)) {
            devirtualizedDestroys.insert(DRI->getOperand());
            Changed = true;
          }
          LastRelease = nullptr;
          continue;
        }
      }

      if (isa<ReleaseValueInst>(&I) ||
          isa<StrongReleaseInst>(&I)) {
        LastRelease = &I;
      } else if (I.mayReleaseOrReadRefCount()) {
        LastRelease = nullptr;
      }
    }
  }
  
  for (auto alloc : devirtualizedDestroys) {
    unsigned destroys = 0;
    unsigned deallocs = 0;
    SmallVector<SILInstruction *, 8> toRemove;
    for (auto *use : alloc->getUses()) {
      auto user = use->getUser();
      if (isa<SetDeallocatingInst>(user))
        destroys++;
      else if (isa<DeallocRefInst>(user))
        deallocs++;
      if (isa<ReleaseValueInst>(user) || isa<StrongReleaseInst>(user) ||
          isa<RetainValueInst>(user) || isa<StrongRetainInst>(user))
        toRemove.push_back(user);
    }
    if (destroys == deallocs) {
      for (auto *refCountInst : toRemove)
        refCountInst->eraseFromParent();
    }
  }
  
  if (Changed) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
  }
}

bool ReleaseDevirtualizer::
devirtualizeReleaseOfObject(SILInstruction *ReleaseInst,
                            DeallocRefInst *DeallocInst) {

  LLVM_DEBUG(llvm::dbgs() << "  try to devirtualize " << *ReleaseInst);

  // We only do the optimization for stack promoted object, because for these
  // we know that they don't have associated objects, which are _not_ released
  // by the deinit method.
  // This restriction is no problem because only stack promotion result in this
  // alloc-release-dealloc pattern.
  if (!DeallocInst->canAllocOnStack())
    return false;

  // Is the dealloc_ref paired with an alloc_ref?
  auto *ARI = dyn_cast<AllocRefInst>(DeallocInst->getOperand());
  if (!ARI)
    return false;

  // Does the last release really release the allocated object?
  SILValue rcRoot = RCIA->getRCIdentityRoot(ReleaseInst->getOperand(0));
  if (rcRoot != ARI)
    return false;

  SILType AllocType = ARI->getType();
  return createDeallocCall(AllocType, ReleaseInst, ARI);
}

bool ReleaseDevirtualizer::createDeallocCall(SILType AllocType,
                                            SILInstruction *ReleaseInst,
                                            SILValue object) {
  LLVM_DEBUG(llvm::dbgs() << "  create dealloc call\n");

  ClassDecl *Cl = AllocType.getClassOrBoundGenericClass();
  assert(Cl && "no class type allocated with alloc_ref");

  // Find the destructor of the type.
  DestructorDecl *Destructor = Cl->getDestructor();
  SILDeclRef DeallocRef(Destructor, SILDeclRef::Kind::Deallocator);
  SILModule &M = ReleaseInst->getFunction()->getModule();
  SILFunction *Dealloc = M.lookUpFunction(DeallocRef);
  if (!Dealloc)
    return false;
  TypeExpansionContext context(*ReleaseInst->getFunction());
  CanSILFunctionType DeallocType =
      Dealloc->getLoweredFunctionTypeInContext(context);
  auto *NTD = AllocType.getASTType()->getAnyNominal();
  auto AllocSubMap = AllocType.getASTType()
    ->getContextSubstitutionMap(M.getSwiftModule(), NTD);

  DeallocType = DeallocType->substGenericArgs(M, AllocSubMap, context);

  SILBuilder B(ReleaseInst);
  if (object->getType() != AllocType)
    object = B.createUncheckedRefCast(ReleaseInst->getLoc(), object, AllocType);

  // Do what a release would do before calling the deallocator: set the object
  // in deallocating state, which means set the RC_DEALLOCATING_FLAG flag.
  B.createSetDeallocating(ReleaseInst->getLoc(), object,
                          cast<RefCountingInst>(ReleaseInst)->getAtomicity());

  // Create the call to the destructor with the allocated object as self
  // argument.
  auto *MI = B.createFunctionRef(ReleaseInst->getLoc(), Dealloc);

  B.createApply(ReleaseInst->getLoc(), MI, AllocSubMap, {object});

  ++NumReleasesDevirtualized;
  ReleaseInst->eraseFromParent();
  return true;
}

} // end anonymous namespace

SILTransform *swift::createReleaseDevirtualizer() {
  return new ReleaseDevirtualizer();
}
