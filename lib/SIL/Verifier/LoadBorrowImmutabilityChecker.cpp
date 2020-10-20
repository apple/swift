//===--- LoadBorrowImmutabilityChecker.cpp --------------------------------===//
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
/// \file
///
/// This file defines a verifier that exhaustively validates that there aren't
/// any load_borrows in a SIL module that have in-scope writes to their
/// underlying storage.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-load-borrow-immutability-checker"
#include "VerifierPrivate.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/MultiMapCache.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/LinearLifetimeChecker.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;
using namespace swift::silverifier;

//===----------------------------------------------------------------------===//
//                               Write Gatherer
//===----------------------------------------------------------------------===//

namespace {

// Visitor for visitAccessPathUses().
class GatherWritesVisitor : public AccessUseVisitor {
  // Result: writes to the AccessPath being visited.
  SmallVectorImpl<Operand *> &writeAccumulator;

public:
  GatherWritesVisitor(SmallVectorImpl<Operand *> &writes)
      : AccessUseVisitor(AccessUseType::Overlapping,
                         NestedAccessType::StopAtAccessBegin),
        writeAccumulator(writes) {}

  bool visitUse(Operand *op, AccessUseType useTy);
};

// Fuctor for MultiMapCache construction.
struct GatherWrites {
  const SILFunction *function;
  GatherWrites(const SILFunction *function) : function(function) {}

  bool operator()(const AccessPath &accessPath,
                  SmallVectorImpl<Operand *> &writeAccumulator) {
    GatherWritesVisitor visitor(writeAccumulator);
    return visitAccessPathUses(visitor, accessPath,
                               const_cast<SILFunction *>(function));
  }
};

} // end anonymous namespace

// Filter out recognized uses that do not write to memory.
//
// TODO: Ensure that all of the conditional-write logic below is encapsulated in
// mayWriteToMemory and just call that instead. Possibly add additional
// verification that visitAccessPathUses recognizes all instructions that may
// propagate pointers (even though they don't write).
bool GatherWritesVisitor::visitUse(Operand *op, AccessUseType useTy) {
  // If this operand is for a dependent type, then it does not actually access
  // the operand's address value. It only uses the metatype defined by the
  // operation (e.g. open_existential).
  if (op->isTypeDependent()) {
    return true;
  }
  SILInstruction *user = op->getUser();
  if (isIncidentalUse(user)) {
    return true;
  }
  switch (user->getKind()) {

  // Known reads...
  case SILInstructionKind::LoadBorrowInst:
  case SILInstructionKind::SelectEnumAddrInst:
  case SILInstructionKind::SwitchEnumAddrInst:
  case SILInstructionKind::DeallocStackInst:
  case SILInstructionKind::DeallocBoxInst:
  case SILInstructionKind::WitnessMethodInst:
  case SILInstructionKind::ExistentialMetatypeInst:
    return true;

  // Known writes...
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::InjectEnumAddrInst:
  case SILInstructionKind::StoreInst:
  case SILInstructionKind::AssignInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::MarkFunctionEscapeInst:
    writeAccumulator.push_back(op);
    return true;

  // Load/Store variations...
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, NAME)      \
  case SILInstructionKind::Load##Name##Inst:                                   \
    if (cast<Load##Name##Inst>(user)->isTake() == IsTake) {                    \
      writeAccumulator.push_back(op);                                          \
    }                                                                          \
    return true;                                                               \
                                                                               \
  case SILInstructionKind::Store##Name##Inst:                                  \
    writeAccumulator.push_back(op);                                            \
    return true;
#include "swift/AST/ReferenceStorage.def"

  // Ignored pointer uses...

  // Allow store_borrow within the load_borrow scope.
  // FIXME: explain why.
  case SILInstructionKind::StoreBorrowInst:
  // Returns are never in scope.
  case SILInstructionKind::ReturnInst:
    return true;

  // Reads that may perform a "take"...

  case SILInstructionKind::LoadInst:
    if (cast<LoadInst>(user)->getOwnershipQualifier()
        == LoadOwnershipQualifier::Take) {
      writeAccumulator.push_back(op);
    }
    return true;

  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
    return true;

  case SILInstructionKind::CheckedCastAddrBranchInst: {
    auto *ccbi = cast<CheckedCastAddrBranchInst>(user);
    if (ccbi->getConsumptionKind() != CastConsumptionKind::CopyOnSuccess) {
      writeAccumulator.push_back(op);
    }
    return true;
  }

  // Conditional writes...

  case SILInstructionKind::CopyAddrInst:
    if (cast<CopyAddrInst>(user)->getDest() == op->get()) {
      writeAccumulator.push_back(op);
      return true;
    }
    // This operand is the copy source. Check if it is taken.
    if (cast<CopyAddrInst>(user)->isTakeOfSrc()) {
      writeAccumulator.push_back(op);
    }
    return true;

  // If this value is dependent on another, conservatively consider it a write.
  //
  // FIXME: explain why a mark_dependence effectively writes to storage.
  case SILInstructionKind::MarkDependenceInst:
    if (cast<MarkDependenceInst>(user)->getValue() == op->get()) {
      writeAccumulator.push_back(op);
    }
    return true;

  // Check for mutable existentials.
  case SILInstructionKind::OpenExistentialAddrInst:
    if (cast<OpenExistentialAddrInst>(user)->getAccessKind()
        != OpenedExistentialAccess::Immutable) {
      writeAccumulator.push_back(op);
    }
    return true;

  case SILInstructionKind::BeginAccessInst:
    if (cast<BeginAccessInst>(user)->getAccessKind() != SILAccessKind::Read) {
      writeAccumulator.push_back(op);
    }
    return true;

  case SILInstructionKind::BuiltinInst:
    if (!cast<BuiltinInst>(user)->mayWriteToMemory()) {
      return true;
    }
    writeAccumulator.push_back(op);
    return true;

  case SILInstructionKind::YieldInst: {
    SILYieldInfo info = cast<YieldInst>(user)->getYieldInfoForOperand(*op);
    if (info.isIndirectInGuaranteed()) {
      return true;
    }
    if (info.isIndirectMutating() || info.isConsumed()) {
      writeAccumulator.push_back(op);
      return true;
    }
    break; // unknown yield convention
  }

  default:
    break;
  } // end switch(user->getKind())

  // If we have a FullApplySite, see if we use the value as an
  // indirect_guaranteed parameter. If we use it as inout, we need
  // interprocedural analysis that we do not perform here.
  if (auto fas = FullApplySite::isa(user)) {
    if (fas.isIndirectResultOperand(*op)) {
      writeAccumulator.push_back(op);
      return true;
    }
    auto argConv = fas.getArgumentConvention(*op);

    // A box or pointer value may be passed directly. Consider that a write.
    if (!argConv.isIndirectConvention()) {
      writeAccumulator.push_back(op);
      return true;
    }
    if (argConv == SILArgumentConvention::Indirect_In_Guaranteed) {
      return true;
    }
    if (argConv.isInoutConvention()) {
      writeAccumulator.push_back(op);
      return true;
    }
    if (argConv.isOwnedConvention()) {
      writeAccumulator.push_back(op);
      return true;
    }
    // Otherwise, be conservative and return that we had a write that we did
    // not understand.
    llvm::errs() << "Full apply site not understood: " << *user;
    return false;
  }

  // Handle a capture-by-address like a write.
  if (auto as = ApplySite::isa(user)) {
    writeAccumulator.push_back(op);
    return true;
  }
  // We don't have an inclusive list of all use patterns for non-address
  // values. References and pointers can be passed to almost anything that takes
  // a value. We assume that visitAccessPathUses has already looked past
  // operations that can propagate a reference or pointer, and simply check that
  // the leaf use that it returned cannot itself write to memory.
  if (!op->get()->getType().isAddress() && !user->mayWriteToMemory()) {
    return true;
  }
  // If we did not recognize the user, just return conservatively that it was
  // written to in a way we did not understand.
  llvm::errs() << "Function: " << user->getFunction()->getName() << "\n";
  llvm::errs() << "Value: " << op->get();
  llvm::errs() << "Unknown instruction: " << *user;
  llvm::report_fatal_error("Unexpected instruction using borrowed address?!");
  return false;
}

//===----------------------------------------------------------------------===//
//                   Load Borrow Immutability Analysis
//===----------------------------------------------------------------------===//

LoadBorrowImmutabilityAnalysis::LoadBorrowImmutabilityAnalysis(
    DeadEndBlocks &deadEndBlocks, const SILFunction *f)
    : cache(GatherWrites(f)), deadEndBlocks(deadEndBlocks) {}

// \p address may be an address, pointer, or box type.
bool LoadBorrowImmutabilityAnalysis::isImmutableInScope(
    LoadBorrowInst *lbi, ArrayRef<Operand *> endBorrowUses,
    AccessPath accessPath) {

  SmallPtrSet<SILBasicBlock *, 8> visitedBlocks;
  LinearLifetimeChecker checker(visitedBlocks, deadEndBlocks);
  auto writes = cache.get(accessPath);

  // Treat None as a write.
  if (!writes) {
    llvm::errs() << "Failed to find cached writes for: ";
    accessPath.getStorage().print(llvm::errs());
    return false;
  }
  // Then for each write...
  for (auto *op : *writes) {
    visitedBlocks.clear();

    // First see if the write is a dead end block. In such a case, just skip it.
    if (deadEndBlocks.isDeadEnd(op->getUser()->getParent())) {
      continue;
    }
    // See if the write is within the load borrow's lifetime. If it isn't, we
    // don't have to worry about it.
    if (!checker.validateLifetime(lbi, endBorrowUses, op)) {
      continue;
    }
    // Ok, we found a write that overlaps with our load_borrow. We now need to
    // prove that the write is to an address that can not trivially alias our
    // load_borrow.
    //
    // If the load_borrow's is within an access scope, then the write must also
    // be within the same access scope. In that case, use
    // AccessPath::computeForAccessScope to ensure that writeAccessPath storage
    // matches accessPath storage.
    AccessPath writePath =
        (accessPath.getStorage().getKind() == AccessedStorage::Nested)
            ? AccessPath::computeInScope(op->get())
            : AccessPath::compute(op->get());

    if (!writePath.isValid()) {
      llvm::errs() << "Couldn't find path root for write: " << *op->getUser();
      return false;
    }
    if (accessPath.mayOverlap(writePath)) {
      continue;
    }
    llvm::errs() << "Write: " << *op->getUser();
    return false;
  }
  // Ok, we are good.
  return true;
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

bool LoadBorrowImmutabilityAnalysis::isImmutable(LoadBorrowInst *lbi) {
  AccessPath accessPath = AccessPath::computeInScope(lbi->getOperand());
  // Bail on an invalid AccessPath. AccessPath completeness is verified
  // independently--it may be invalid in extraordinary situations. When
  // AccessPath is valid, we know all its uses are recognizable.
  if (!accessPath.isValid()) {
    return true;
  }
  // If we have a let address, then we are already done.
  if (accessPath.getStorage().isLetAccess()) {
    return true;
  }
  // At this point, we know that we /may/ have writes. Now we go through various
  // cases to try and exhaustively identify if those writes overlap with our
  // load_borrow.
  SmallVector<Operand *, 8> endBorrowUses;
  transform(lbi->getUsersOfType<EndBorrowInst>(),
            std::back_inserter(endBorrowUses),
            [](EndBorrowInst *ebi) { return &ebi->getAllOperands()[0]; });

  switch (accessPath.getStorage().getKind()) {
  case AccessedStorage::Nested: {
    // If we have a begin_access and...
    auto *bai = cast<BeginAccessInst>(accessPath.getStorage().getValue());
    // We do not have a modify, assume we are correct.
    if (bai->getAccessKind() != SILAccessKind::Modify) {
      return true;
    }
    // Otherwise, validate that any writes to our begin_access is not when the
    // load_borrow's result is live.
    //
    // TODO: As a separate analysis, verify that the load_borrow scope is always
    // nested within the begin_access scope (to ensure no aliasing access).
    return isImmutableInScope(lbi, endBorrowUses, accessPath);
  }
  case AccessedStorage::Argument: {
    auto *arg =
        cast<SILFunctionArgument>(accessPath.getStorage().getArgument());
    if (arg->hasConvention(SILArgumentConvention::Indirect_In_Guaranteed)) {
      return true;
    }
    return isImmutableInScope(lbi, endBorrowUses, accessPath);
  }
  // FIXME: A yielded address could overlap with another in this function.
  case AccessedStorage::Yield:
  case AccessedStorage::Stack:
  case AccessedStorage::Box:
  case AccessedStorage::Class:
  case AccessedStorage::Tail:
  case AccessedStorage::Global:
  case AccessedStorage::Unidentified:
    return isImmutableInScope(lbi, endBorrowUses, accessPath);
  }
  llvm_unreachable("Covered switch isn't covered?!");
}
