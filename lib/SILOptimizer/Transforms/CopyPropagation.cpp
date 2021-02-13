//===--------- CopyPropagation.cpp - Remove redundant SSA copies. ---------===//
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
///
/// SSA Copy propagation pass to remove unnecessary copy_value and destroy_value
/// instructions.
///
/// Because this algorithm rewrites copies and destroys without attempting to
/// balance the retain count, it is only sound when SIL is in ownership-SSA
/// form. The pass itself is mostly for testing the underlying functionality
/// which can also be invoked as a utility for any owned value.
///
/// TODO: Cleanup the resulting SIL by deleting instructions that produce dead
/// values (after removing its copies).
///
/// ===----------------------------------------------------------------------===

#define DEBUG_TYPE "copy-propagation"

#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

//!!!
#include "swift/Basic/Defer.h"

using namespace swift;

//===----------------------------------------------------------------------===//
// CopyPropagation: Top-Level Function Transform.
//===----------------------------------------------------------------------===//

namespace {
class CopyPropagation : public SILFunctionTransform {
  /// True if debug_value instructions should be pruned.
  bool pruneDebug;

public:
  CopyPropagation(bool pruneDebug): pruneDebug(pruneDebug) {}

  /// The entry point to this function transformation.
  void run() override;
};
} // end anonymous namespace

static bool isCopyDead(CopyValueInst *copy, bool pruneDebug) {
  for (Operand *use : copy->getUses()) {
    auto *user = use->getUser();
    if (isa<DestroyValueInst>(user)) {
      continue;
    }
    if (pruneDebug && isa<DebugValueInst>(user)) {
      continue;
    }
    return false;
  }
  return true;
}

/// Top-level pass driver.
void CopyPropagation::run() {
  auto *f = getFunction();
  auto *accessBlockAnalysis = getAnalysis<NonLocalAccessBlockAnalysis>();
  auto *dominanceAnalysis = getAnalysis<DominanceAnalysis>();
  auto *deBlocksAnalysis = getAnalysis<DeadEndBlocksAnalysis>();

  //!!!
  if (f->hasName("$ss18_StringBreadcrumbsCyABSScfc")) {
    llvm::DebugFlag = true;
    llvm::setCurrentDebugType("copy-propagation");
  }
  SWIFT_DEFER { llvm::DebugFlag = false; };

  // Debug label for unit testing.
  LLVM_DEBUG(llvm::dbgs() << "*** CopyPropagation: " << f->getName() << "\n");

  // This algorithm fundamentally assumes ownership.
  if (!f->hasOwnership())
    return;

  // Driver: Find all copied defs.
  llvm::SmallSetVector<SILValue, 16> copiedDefs;
  for (auto &bb : *f) {
    for (auto &i : bb) {
      if (auto *copy = dyn_cast<CopyValueInst>(&i))
        copiedDefs.insert(
            CanonicalizeOSSALifetime::getCanonicalCopiedDef(copy));
    }
  }
  // Push copy_value instructions above their struct_extract operands by
  // inserting destructures.
  //
  // copiedDefs be be modified, but it never shrinks
  for (unsigned idx = 0; idx < copiedDefs.size(); ++idx) {
    SILValue def = copiedDefs[idx];
    auto *copy = dyn_cast<CopyValueInst>(def);
    if (!copy)
      continue;

    auto *extract = dyn_cast<StructExtractInst>(copy->getOperand());
    if (!extract
        || SILValue(extract).getOwnershipKind() != OwnershipKind::Guaranteed)
      continue;

    if (SILValue destructuredResult = convertExtractToDestructure(extract)) {
      // Remove to-be-deleted instructions from copiedDeds. The extract cannot
      // be in the copiedDefs set since getCanonicalCopiedDef does not allow a
      // guaranteed projection to be a canonical def.
      copiedDefs.remove(copy);
      --idx; // point back to the current element, which was erased.

      // TODO: unfortunately SetVector has no element replacement.
      copiedDefs.insert(destructuredResult);

      auto *destructure = cast<DestructureStructInst>(
          destructuredResult.getDefiningInstruction());
      auto *newCopy = cast<CopyValueInst>(destructure->getOperand());
      copiedDefs.insert(
          CanonicalizeOSSALifetime::getCanonicalCopiedDef(newCopy));

      LLVM_DEBUG(llvm::dbgs() << "Destructure Conversion:\n"
                              << *extract << "  to " << *destructure);
      // Delete both the copy and the extract.
      InstructionDeleter().recursivelyDeleteUsersIfDead(extract);
    }
  }
  // Perform copy propgation for each copied value.
  CanonicalizeOSSALifetime canonicalizer(pruneDebug, accessBlockAnalysis,
                                         dominanceAnalysis,
                                         deBlocksAnalysis->get(f));
  // Cleanup dead copies. If getCanonicalCopiedDef returns a copy (because the
  // copy's source operand is unrecgonized), then thecan copy is itself treated
  // like a def and may be dead after canonicalization.
  llvm::SmallVector<CopyValueInst *, 4> deadCopies;
  for (auto &def : copiedDefs) {
    // Canonicalized this def.
    canonicalizer.canonicalizeValueLifetime(def);

    if (auto *copy = dyn_cast<CopyValueInst>(def)) {
      if (isCopyDead(copy, pruneDebug)) {
        deadCopies.push_back(copy);
      }
    }
    // Canonicalize any new outer copy.
    if (SILValue outerCopy = canonicalizer.createdOuterCopy()) {
      SILValue outerDef = canonicalizer.getCanonicalCopiedDef(outerCopy);
      canonicalizer.canonicalizeValueLifetime(outerDef);
    }
    // TODO: also canonicalize any lifetime.persistentCopies like separate owned
    // live ranges.
  }
  if (canonicalizer.hasChanged() || !deadCopies.empty()) {
    InstructionDeleter deleter;
    for (auto *copy : deadCopies) {
      deleter.recursivelyDeleteUsersIfDead(copy);
    }
    // Preserves NonLocalAccessBlockAnalysis.
    accessBlockAnalysis->lockInvalidation();
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
    accessBlockAnalysis->unlockInvalidation();
    f->verifyOwnership(deBlocksAnalysis->get(f));
  }
}

SILTransform *swift::createCopyPropagation() {
  return new CopyPropagation(/*pruneDebug*/ true);
}

SILTransform *swift::createMandatoryCopyPropagation() {
  return new CopyPropagation(/*pruneDebug*/ false);
}
