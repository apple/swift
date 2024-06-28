//===--- Context.cpp ------------------------------------------------------===//
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

#define DEBUG_TYPE "sil-semantic-arc-opts"

#include "Context.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/Projection.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::semanticarc;

static llvm::cl::opt<bool>
    VerifyAfterTransformOption("sil-semantic-arc-opts-verify-after-transform",
                               llvm::cl::Hidden, llvm::cl::init(false));

void Context::verify() const {
  if (VerifyAfterTransformOption)
    fn.verify();
}

//===----------------------------------------------------------------------===//
//                        Well Behaved Write Analysis
//===----------------------------------------------------------------------===//

/// Returns true if we were able to ascertain that either the initialValue has
/// no write uses or all of the write uses were writes that we could understand.
bool Context::constructCacheValue(
    SILValue initialValue,
    SmallVectorImpl<Operand *> &wellBehavedWriteAccumulator) {
  SmallVector<Operand *, 8> worklist(initialValue->getNonTypeDependentUses());

  while (!worklist.empty()) {
    auto *op = worklist.pop_back_val();
    assert(!op->isTypeDependent() &&
           "Uses that are type dependent should have been filtered before "
           "being inserted into the worklist");
    SILInstruction *user = op->getUser();

    if (Projection::isAddressProjection(user) ||
        isa<ProjectBlockStorageInst>(user)) {
      for (SILValue r : user->getResults()) {
        llvm::copy(r->getNonTypeDependentUses(),
                   std::back_inserter(worklist));
      }
      continue;
    }

    if (auto *oeai = dyn_cast<OpenExistentialAddrInst>(user)) {
      // Mutable access!
      if (oeai->getAccessKind() != OpenedExistentialAccess::Immutable) {
        wellBehavedWriteAccumulator.push_back(op);
      }

      //  Otherwise, look through it and continue.
      llvm::copy(oeai->getNonTypeDependentUses(),
                 std::back_inserter(worklist));
      continue;
    }

    if (auto *si = dyn_cast<StoreInst>(user)) {
      // We must be the dest since addresses can not be stored.
      assert(si->getDest() == op->get());
      wellBehavedWriteAccumulator.push_back(op);
      continue;
    }

    // Add any destroy_addrs to the resultAccumulator.
    if (isa<DestroyAddrInst>(user)) {
      wellBehavedWriteAccumulator.push_back(op);
      continue;
    }

    // load_borrow and incidental uses are fine as well.
    if (isa<LoadBorrowInst>(user) || isIncidentalUse(user)) {
      continue;
    }

    // Look through begin_access and mark them/their end_borrow as users.
    if (auto *bai = dyn_cast<BeginAccessInst>(user)) {
      // If we do not have a read, mark this as a write. Also, insert our
      // end_access as well.
      if (bai->getAccessKind() != SILAccessKind::Read) {
        wellBehavedWriteAccumulator.push_back(op);
        transform(bai->getUsersOfType<EndAccessInst>(),
                  std::back_inserter(wellBehavedWriteAccumulator),
                  [](EndAccessInst *eai) { return &eai->getAllOperands()[0]; });
      }

      // And then add the users to the worklist and continue.
      llvm::copy(bai->getNonTypeDependentUses(),
                 std::back_inserter(worklist));
      continue;
    }

    // If we have a load, we just need to mark the load [take] as a write.
    if (auto *li = dyn_cast<LoadInst>(user)) {
      if (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take) {
        wellBehavedWriteAccumulator.push_back(op);
      }
      continue;
    }

    // If we have a FullApplySite, we need to do per convention/inst logic.
    if (auto fas = FullApplySite::isa(user)) {
      // Begin by seeing if we have an in_guaranteed use. If we do, we are done.
      if (fas.getArgumentConvention(*op) ==
          SILArgumentConvention::Indirect_In_Guaranteed) {
        continue;
      }

      // Then see if we have an apply site that is not a coroutine apply
      // site. In such a case, without further analysis, we can treat it like an
      // instantaneous write and validate that it doesn't overlap with our load
      // [copy].
      if (!fas.beginsCoroutineEvaluation() &&
          fas.getArgumentConvention(*op).isInoutConvention()) {
        wellBehavedWriteAccumulator.push_back(op);
        continue;
      }

      // Otherwise, be conservative and return that we had a write that we did
      // not understand.
      LLVM_DEBUG(llvm::dbgs()
                 << "Function: " << user->getFunction()->getName() << "\n");
      LLVM_DEBUG(llvm::dbgs() << "Value: " << op->get());
      LLVM_DEBUG(llvm::dbgs() << "Unhandled apply site!: " << *user);

      return false;
    }

    // Copy addr that read are just loads.
    if (auto *cai = dyn_cast<CopyAddrInst>(user)) {
      // If our value is the destination, this is a write.
      if (cai->getDest() == op->get()) {
        wellBehavedWriteAccumulator.push_back(op);
        continue;
      }

      // Ok, so we are Src by process of elimination. Make sure we are not being
      // taken.
      if (cai->isTakeOfSrc()) {
        wellBehavedWriteAccumulator.push_back(op);
        continue;
      }

      // Otherwise, we are safe and can continue.
      continue;
    }

    // If we did not recognize the user, just return conservatively that it was
    // written to in a way we did not understand.
    LLVM_DEBUG(llvm::dbgs()
               << "Function: " << user->getFunction()->getName() << "\n");
    LLVM_DEBUG(llvm::dbgs() << "Value: " << op->get());
    LLVM_DEBUG(llvm::dbgs() << "Unknown instruction!: " << *user);
    return false;
  }

  // Ok, we finished our worklist and this address is not being written to.
  return true;
}
