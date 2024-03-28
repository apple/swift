//===--- AccessEnforcementReleaseSinking.cpp - release sinking opt ---===//
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
///
/// This function pass sinks releases out of access scopes.
///
/// General case:
/// begin_access A
/// ...
/// strong_release / release_value / destroy
/// end_access
///
/// The release instruction can be sunk below the end_access instruction,
/// This extends the lifetime of the released value, but, might allow us to
/// Mark the access scope as no nested conflict.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "access-enforcement-release"

#include "swift/SIL/ApplySite.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

// Returns a bool: If this is a "sinkable" instruction type for this opt
static bool isSinkable(SILInstruction &inst) {
  switch (inst.getKind()) {
  default:
    return false;
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::ReleaseValueAddrInst:
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::UnmanagedReleaseValueInst:
    return true;
  }
}

// Returns a bool: If this is a "barrier" instruction for this opt
static bool isBarrier(SILInstruction *inst) {
  // Calls hide many dangers, from checking reference counts, to beginning
  // keypath access, to forcing memory to be live. Checking for these and other
  // possible barriers at ever call is certainly not worth it.
  if (FullApplySite::isa(inst) != FullApplySite())
    return true;

  // Don't extend lifetime past any sort of uniqueness check.
  if (mayCheckRefCount(inst))
    return true;

  // Don't extend object lifetime past deallocation.
  if (isa<DeallocationInst>(inst))
    return true;

  // Avoid introducing access conflicts.
  if (isa<BeginAccessInst>(inst) || isa<BeginUnpairedAccessInst>(inst))
    return true;

  if (auto *BI = dyn_cast<BuiltinInst>(inst)) {
    auto kind = BI->getBuiltinKind();
    if (!kind)
      return false; // LLVM intrinsics are not barriers.

    // Whitelist the safe builtin categories. Builtins should generally be
    // treated conservatively, because introducing a new builtin does not
    // require updating all passes to be aware of it.
    switch (kind.value()) {
    case BuiltinValueKind::None:
      llvm_unreachable("Builtin must has a non-empty kind.");

      // Unhandled categories don't generate a case. Instead, they result
      // in a build error: enumeration values not handled in switch.
#define BUILTIN(Id, Name, Attrs)

#define BUILTIN_NO_BARRIER(Id)                                                 \
  case BuiltinValueKind::Id:                                                   \
    return false;
#define BUILTIN_CAST_OPERATION(Id, Name, Attrs) BUILTIN_NO_BARRIER(Id)
#define BUILTIN_CAST_OR_BITCAST_OPERATION(Id, Name, Attrs)                     \
  BUILTIN_NO_BARRIER(Id)
#define BUILTIN_BINARY_OPERATION(Id, Name, Attrs) BUILTIN_NO_BARRIER(Id)
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(Id, Name, UncheckedID, Attrs,   \
                                               Overload)                       \
  BUILTIN_NO_BARRIER(Id)
#define BUILTIN_UNARY_OPERATION(Id, Name, Attrs, Overload)                     \
  BUILTIN_NO_BARRIER(Id)
#define BUILTIN_BINARY_PREDICATE(Id, Name, Attrs, Overload)                    \
  BUILTIN_NO_BARRIER(Id)
#define BUILTIN_SIL_OPERATION(Id, Name, Overload)                              \
  case BuiltinValueKind::Id:                                                   \
    llvm_unreachable("SIL operation must be lowered to instructions.");
#define BUILTIN_RUNTIME_CALL(Id, Name, Attrs)                                  \
  case BuiltinValueKind::Id:                                                   \
    return true; // A runtime call could be anything.

#define BUILTIN_SANITIZER_OPERATION(Id, Name, Attrs) BUILTIN_NO_BARRIER(Id)
#define BUILTIN_TYPE_CHECKER_OPERATION(Id, Name) BUILTIN_NO_BARRIER(Id)
#define BUILTIN_TYPE_TRAIT_OPERATION(Id, Name) BUILTIN_NO_BARRIER(Id)
#include "swift/AST/Builtins.def"

    // Handle BUILTIN_MISC_OPERATIONs individually.
    case BuiltinValueKind::Sizeof:
    case BuiltinValueKind::Strideof:
    case BuiltinValueKind::IsPOD:
    case BuiltinValueKind::IsConcrete:
    case BuiltinValueKind::IsBitwiseTakable:
    case BuiltinValueKind::IsSameMetatype:
    case BuiltinValueKind::Alignof:
    case BuiltinValueKind::OnFastPath:
    case BuiltinValueKind::ExtractElement:
    case BuiltinValueKind::InsertElement:
    case BuiltinValueKind::ShuffleVector:
    case BuiltinValueKind::StaticReport:
    case BuiltinValueKind::AssertConf:
    case BuiltinValueKind::StringObjectOr:
    case BuiltinValueKind::UToSCheckedTrunc:
    case BuiltinValueKind::SToUCheckedTrunc:
    case BuiltinValueKind::SToSCheckedTrunc:
    case BuiltinValueKind::UToUCheckedTrunc:
    case BuiltinValueKind::IntToFPWithOverflow:
    case BuiltinValueKind::BitWidth:
    case BuiltinValueKind::IsNegative:
    case BuiltinValueKind::WordAtIndex:
    case BuiltinValueKind::ZeroInitializer:
    case BuiltinValueKind::Once:
    case BuiltinValueKind::OnceWithContext:
    case BuiltinValueKind::GetObjCTypeEncoding:
    case BuiltinValueKind::WillThrow:
    case BuiltinValueKind::CondFailMessage:
    case BuiltinValueKind::PoundAssert:
    case BuiltinValueKind::TypePtrAuthDiscriminator:
    case BuiltinValueKind::TargetOSVersionAtLeast:
    case BuiltinValueKind::GlobalStringTablePointer:
    case BuiltinValueKind::COWBufferForReading:
    case BuiltinValueKind::GetCurrentAsyncTask:
    case BuiltinValueKind::GetCurrentExecutor:
    case BuiltinValueKind::AutoDiffCreateLinearMapContextWithType:
    case BuiltinValueKind::EndAsyncLet:
    case BuiltinValueKind::EndAsyncLetLifetime:
    case BuiltinValueKind::CreateTaskGroup:
    case BuiltinValueKind::CreateTaskGroupWithFlags:
    case BuiltinValueKind::DestroyTaskGroup:
    case BuiltinValueKind::StackAlloc:
    case BuiltinValueKind::UnprotectedStackAlloc:
    case BuiltinValueKind::StackDealloc:
    case BuiltinValueKind::AllocVector:
    case BuiltinValueKind::AssumeAlignment:
    case BuiltinValueKind::GetEnumTag:
    case BuiltinValueKind::InjectEnumTag:
    case BuiltinValueKind::ExtractFunctionIsolation:
    case BuiltinValueKind::AddressOfRawLayout:
      return false;

    // Handle some rare builtins that may be sensitive to object lifetime
    // or deinit side effects conservatively.
    case BuiltinValueKind::AllocRaw:
    case BuiltinValueKind::DeallocRaw:
    case BuiltinValueKind::Fence:
    case BuiltinValueKind::Ifdef:
    case BuiltinValueKind::AtomicLoad:
    case BuiltinValueKind::AtomicStore:
    case BuiltinValueKind::AtomicRMW:
    case BuiltinValueKind::Unreachable:
    case BuiltinValueKind::CmpXChg:
    case BuiltinValueKind::CondUnreachable:
    case BuiltinValueKind::DestroyArray:
    case BuiltinValueKind::CopyArray:
    case BuiltinValueKind::TakeArrayNoAlias:
    case BuiltinValueKind::TakeArrayFrontToBack:
    case BuiltinValueKind::TakeArrayBackToFront:
    case BuiltinValueKind::AssignCopyArrayNoAlias:
    case BuiltinValueKind::AssignCopyArrayFrontToBack:
    case BuiltinValueKind::AssignCopyArrayBackToFront:
    case BuiltinValueKind::AssignTakeArray:
    case BuiltinValueKind::Copy:
    case BuiltinValueKind::CancelAsyncTask:
    case BuiltinValueKind::StartAsyncLet:
    case BuiltinValueKind::CreateAsyncTask:
    case BuiltinValueKind::TaskRunInline:
    case BuiltinValueKind::StartAsyncLetWithLocalBuffer:
    case BuiltinValueKind::ConvertTaskToJob:
    case BuiltinValueKind::InitializeDefaultActor:
    case BuiltinValueKind::DestroyDefaultActor:
    case BuiltinValueKind::InitializeDistributedRemoteActor:
    case BuiltinValueKind::InitializeNonDefaultDistributedActor:
    case BuiltinValueKind::BuildOrdinaryTaskExecutorRef:
    case BuiltinValueKind::BuildOrdinarySerialExecutorRef:
    case BuiltinValueKind::BuildComplexEqualitySerialExecutorRef:
    case BuiltinValueKind::BuildDefaultActorExecutorRef:
    case BuiltinValueKind::BuildMainActorExecutorRef:
    case BuiltinValueKind::ResumeNonThrowingContinuationReturning:
    case BuiltinValueKind::ResumeThrowingContinuationReturning:
    case BuiltinValueKind::ResumeThrowingContinuationThrowing:
    case BuiltinValueKind::AutoDiffProjectTopLevelSubcontext:
    case BuiltinValueKind::AutoDiffAllocateSubcontextWithType:
    case BuiltinValueKind::AddressOfBorrowOpaque:
    case BuiltinValueKind::UnprotectedAddressOfBorrowOpaque:
    case BuiltinValueKind::DistributedActorAsAnyActor:
      return true;
    }
  }
  return false;
}

// Processes a block bottom-up, keeping a lookout for end_access instructions
// If we encounter a "barrier" we clear out the current end_access
// If we encounter a "release", and we have a current end_access, we sink it
static void processBlock(SILBasicBlock &block) {
  EndAccessInst *bottomEndAccessInst = nullptr;
  for (auto reverseIt = block.rbegin(); reverseIt != block.rend();
       ++reverseIt) {
    SILInstruction &currIns = *reverseIt;
    if (auto *currEAI = dyn_cast<EndAccessInst>(&currIns)) {
      if (!bottomEndAccessInst) {
        bottomEndAccessInst = currEAI;
      }
    } else if (isBarrier(&currIns)) {
      LLVM_DEBUG(llvm::dbgs() << "Found a barrier " << currIns
                              << ", clearing last seen end_access\n");
      bottomEndAccessInst = nullptr;
    } else if (isSinkable(currIns)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Found a sinkable instruction " << currIns << "\n");
      if (!bottomEndAccessInst) {
        LLVM_DEBUG(
            llvm::dbgs()
            << "Cannot be sunk: no open barrier-less end_access found\n");
        continue;
      }
      LLVM_DEBUG(llvm::dbgs() << "Moving sinkable instruction below "
                              << *bottomEndAccessInst << "\n");
      // We need to avoid iterator invalidation:
      // We know this is not the last instruction of the block:
      // 1) not a TermInst
      // 2) bottomEndAccessInst != nil
      assert(reverseIt != block.rbegin() &&
             "Did not expect a sinkable instruction at block's end");
      // Go back to previous iteration
      auto prevIt = reverseIt;
      --prevIt;
      // Move the instruction after the end_access
      currIns.moveAfter(bottomEndAccessInst);
      // make reverseIt into a valid iterator again
      reverseIt = prevIt;
    }
  }
}

namespace {
struct AccessEnforcementReleaseSinking : public SILFunctionTransform {
  void run() override {
    SILFunction *F = getFunction();
    if (F->empty())
      return;

    // FIXME: Support ownership.
    if (F->hasOwnership())
      return;

    LLVM_DEBUG(llvm::dbgs() << "Running AccessEnforcementReleaseSinking on "
                            << F->getName() << "\n");

    for (SILBasicBlock &currBB : *F) {
      processBlock(currBB);
    }
  }
};
} // namespace

SILTransform *swift::createAccessEnforcementReleaseSinking() {
  return new AccessEnforcementReleaseSinking();
}
