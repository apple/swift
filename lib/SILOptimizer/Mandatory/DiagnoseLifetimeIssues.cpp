//==-------- DiagnoseLifetimeIssues.cpp - Diagnose lifetime issues ---------==//
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
//
// This file implements a diagnostic pass that prints a warning if an object is
// stored to a weak property (or is weakly captured) and destroyed before the
// property (or captured reference) is ever used again.
// This can happen if the programmer relies on the lexical scope to keep an
// object alive, but copy-propagation can shrink the object's lifetime to its
// last use.
// For example:
//
// func test() {
//   let k = Klass()
//   // k is deallocated immediatly after the closure capture (a store_weak).
//   functionWithClosure({ [weak k] in
//                         // crash!
//                         k!.foo()
//                       })
// }
//
// Pass dependency: currently runs before MandatoryCopyPropagation, which
// creates additional @sil_unowned values. This probably won't affect the
// diagnostics though.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "diagnose-lifetime-issues"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/PrunedLiveness.h"
#include "swift/Demangling/Demangler.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

/// Performs the analysis and prints the warnings.
class DiagnoseLifetimeIssues {
  PrunedLiveness liveness;

  /// Reuse a general worklist for def-use traversal.
  SmallSetVector<SILValue, 8> defUseWorklist;

  bool computeCanonicalLiveness(SILValue def);

  bool isDeadStore(StoreWeakInst *store);

public:
  DiagnoseLifetimeIssues() {}

  void diagnose(SILFunction *function);
};

/// Returns true if def is an owned value resulting from an object allocation.
static bool isAllocation(SILValue def) {
  if (def.getOwnershipKind() != OwnershipKind::Owned)
    return false;

  if (isa<AllocRefInst>(def))
    return true;
  
  // Check if it's a call to an allocating initializer.
  if (auto *applyInst = dyn_cast<ApplyInst>(def)) {
    SILFunction *callee = applyInst->getReferencedFunctionOrNull();
    if (!callee)
      return false;
    
    Demangle::StackAllocatedDemangler<1024> demangler;
    Demangle::Node *root = demangler.demangleSymbol(callee->getName());
    return root && root->getKind() == Demangle::Node::Kind::Global &&
           root->getFirstChild()->getKind() == Demangle::Node::Kind::Allocator;
  }
    
  return false;
}

/// Computes the canoncial lifetime of \p def, like the copy-propagation pass
/// would do.
/// The only difference is that we are treating enum instructions (taking a
/// payload) like copies. Enums are important because the operand of a
/// store_weak is always an Optional.
bool DiagnoseLifetimeIssues::computeCanonicalLiveness(SILValue def) {
  defUseWorklist.clear();
  defUseWorklist.insert(def);
  while (!defUseWorklist.empty()) {
    SILValue value = defUseWorklist.pop_back_val();
    for (Operand *use : value->getUses()) {
      auto *user = use->getUser();

      // Recurse through copies and enums.
      if (isa<CopyValueInst>(user) || isa<EnumInst>(user)) {
        defUseWorklist.insert(cast<SingleValueInstruction>(user));
        continue;
      }
      switch (use->getOperandOwnership()) {
      case OperandOwnership::NonUse:
        break;
      case OperandOwnership::TrivialUse:
        llvm_unreachable("this operand cannot handle ownership");

      // Conservatively treat a conversion to an unowned value as a pointer
      // escape. Is it legal to canonicalize ForwardingUnowned?
      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        return false;
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::BitwiseEscape:
        liveness.updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::ForwardingConsume:
        // TODO: handle forwarding instructions, e.g. casts.
        return false;
      case OperandOwnership::DestroyingConsume:
        // destroy_value does not force pruned liveness (but store etc. does).
        if (!isa<DestroyValueInst>(user))
          return false;
        break;
      case OperandOwnership::Borrow:
        if (!liveness.updateForBorrowingOperand(use))
          return false;
        break;
      case OperandOwnership::InteriorPointer:
      case OperandOwnership::ForwardingBorrow:
      case OperandOwnership::EndBorrow:
      case OperandOwnership::Reborrow:
        llvm_unreachable("operand kind cannot take an owned value");
      }
    }
  }
  return true;
}

/// Gets the underlying definition of \p val, looking through copy_value and
/// enum instructions.
static SILValue getCanonicalDef(SILValue val) {
  while (true) {
    if (auto *copyInst = dyn_cast<CopyValueInst>(val)) {
      SILValue copySrc = copyInst->getOperand();
      if (copySrc.getOwnershipKind() != OwnershipKind::Owned)
        return val;
      val = copySrc;
      continue;
    }
    if (auto *enumInst = dyn_cast<EnumInst>(val)) {
      if (enumInst->hasOperand()) {
        val = enumInst->getOperand();
        continue;
      }
    }
    return val;
  }
}

/// Returns true if the stored object of \p store is never loaded within the
/// lifetime of the stored object.
bool DiagnoseLifetimeIssues::isDeadStore(StoreWeakInst *store) {
  SILValue storedDef = getCanonicalDef(store->getSrc());
  
  // Only for allocations we know that a destroy will actually deallocate the
  // object. Otherwise the object could be kept alive by other references and
  // we would issue a false alarm.
  if (!isAllocation(storedDef))
    return false;

  liveness.initializeDefBlock(storedDef->getParentBlock());
  if (!computeCanonicalLiveness(storedDef))
    return false;


  // Check if the lifetime of the stored object ends at the store_weak.
  //
  // A more sophisticated analysis would be to check if there are no
  // (potential) loads from the store's destination address after the store,
  // but within the object's liferange. But without a good alias analysis (and
  // we don't want to use AliasAnalysis in a mandatory pass) it's practially
  // impossible that a use of the object is not a potential load. So we would
  // always see a potential load if the lifetime of the object goes beyond the
  // store_weak.

  SILBasicBlock *storeBlock = store->getParent();
  if (liveness.getBlockLiveness(storeBlock) != PrunedLiveBlocks::LiveWithin)
    return false;

  // If there are any uses after the store_weak, it means that the liferange of
  // the object goes beyond the store_weak.
  for (SILInstruction &inst : make_range(std::next(store->getIterator()),
                                         storeBlock->end())) {
    switch (liveness.isInterestingUser(&inst)) {
    case PrunedLiveness::NonUser:
      break;
    case PrunedLiveness::NonLifetimeEndingUse:
    case PrunedLiveness::LifetimeEndingUse:
      return false;
    }
  }
  return true;
}

/// Prints warnings for dead weak stores in \p function.
void DiagnoseLifetimeIssues::diagnose(SILFunction *function) {
  for (SILBasicBlock &block : *function) {
    for (SILInstruction &inst : block) {
      if (auto *stWeak = dyn_cast<StoreWeakInst>(&inst)) {
        if (isDeadStore(stWeak)) {
          function->getModule().getASTContext().Diags.diagnose(
              stWeak->getLoc().getSourceLoc(), diag::warn_dead_weak_store);
        }
        liveness.clear();
        continue;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                            The function pass
//===----------------------------------------------------------------------===//

class DiagnoseLifetimeIssuesPass : public SILFunctionTransform {
public:
  DiagnoseLifetimeIssuesPass() {}

private:
  void run() override {
    SILFunction *function = getFunction();
    // Don't rerun diagnostics on deserialized functions.
    if (function->wasDeserializedCanonical())
      return;

    if (!function->hasOwnership())
      return;

    DiagnoseLifetimeIssues diagnoser;
    diagnoser.diagnose(function);
  }
};

} // end anonymous namespace

SILTransform *swift::createDiagnoseLifetimeIssues() {
  return new DiagnoseLifetimeIssuesPass();
}
