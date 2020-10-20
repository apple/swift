//===--- InstructionUtils.cpp - Utilities for SIL instructions ------------===//
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

#define DEBUG_TYPE "sil-inst-utils"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"

using namespace swift;

SILValue swift::stripOwnershipInsts(SILValue v) {
  while (true) {
    switch (v->getKind()) {
    default:
      return v;
    case ValueKind::CopyValueInst:
    case ValueKind::BeginBorrowInst:
      v = cast<SingleValueInstruction>(v)->getOperand(0);
    }
  }
}

/// Strip off casts/indexing insts/address projections from V until there is
/// nothing left to strip.
///
/// FIXME: Why don't we strip projections after stripping indexes?
SILValue swift::getUnderlyingObject(SILValue v) {
  while (true) {
    SILValue v2 = stripCasts(v);
    v2 = stripAddressProjections(v2);
    v2 = stripIndexingInsts(v2);
    v2 = stripOwnershipInsts(v2);
    if (v2 == v)
      return v2;
    v = v2;
  }
}

SILValue swift::getUnderlyingObjectStopAtMarkDependence(SILValue v) {
  while (true) {
    SILValue v2 = stripCastsWithoutMarkDependence(v);
    v2 = stripAddressProjections(v2);
    v2 = stripIndexingInsts(v2);
    v2 = stripOwnershipInsts(v2);
    if (v2 == v)
      return v2;
    v = v2;
  }
}

/// Return the underlying SILValue after stripping off identity SILArguments if
/// we belong to a BB with one predecessor.
SILValue swift::stripSinglePredecessorArgs(SILValue V) {
  while (true) {
    auto *A = dyn_cast<SILArgument>(V);
    if (!A)
      return V;
    
    SILBasicBlock *BB = A->getParent();
    
    // First try and grab the single predecessor of our parent BB. If we don't
    // have one, bail.
    SILBasicBlock *Pred = BB->getSinglePredecessorBlock();
    if (!Pred)
      return V;
    
    // Then grab the terminator of Pred...
    TermInst *PredTI = Pred->getTerminator();
    
    // And attempt to find our matching argument.
    //
    // *NOTE* We can only strip things here if we know that there is no semantic
    // change in terms of upcasts/downcasts/enum extraction since this is used
    // by other routines here. This means that we can only look through
    // cond_br/br.
    //
    // For instance, routines that use stripUpcasts() do not want to strip off a
    // downcast that results from checked_cast_br.
    if (auto *BI = dyn_cast<BranchInst>(PredTI)) {
      V = BI->getArg(A->getIndex());
      continue;
    }
    
    if (auto *CBI = dyn_cast<CondBranchInst>(PredTI)) {
      if (SILValue Arg = CBI->getArgForDestBB(BB, A)) {
        V = Arg;
        continue;
      }
    }
    
    return V;
  }
}

SILValue swift::stripCastsWithoutMarkDependence(SILValue v) {
  while (true) {
    v = stripSinglePredecessorArgs(v);
    if (auto *svi = dyn_cast<SingleValueInstruction>(v)) {
      if (isRCIdentityPreservingCast(svi)
          || isa<UncheckedTrivialBitCastInst>(v)
          || isa<BeginAccessInst>(v)
          || isa<EndCOWMutationInst>(v)) {
        v = svi->getOperand(0);
        continue;
      }
    }
    return v;
  }
}

SILValue swift::stripCasts(SILValue v) {
  while (true) {
    v = stripSinglePredecessorArgs(v);
    if (auto *svi = dyn_cast<SingleValueInstruction>(v)) {
      if (isRCIdentityPreservingCast(svi)
          || isa<UncheckedTrivialBitCastInst>(v)
          || isa<MarkDependenceInst>(v)
          || isa<BeginAccessInst>(v)) {
        v = cast<SingleValueInstruction>(v)->getOperand(0);
        continue;
      }
    }
    SILValue v2 = stripOwnershipInsts(v);
    if (v2 != v) {
      v = v2;
      continue;
    }
    return v;
  }
}

SILValue swift::stripUpCasts(SILValue v) {
  assert(v->getType().isClassOrClassMetatype() &&
         "Expected class or class metatype!");
  
  v = stripSinglePredecessorArgs(v);
  
  while (true) {
    if (auto *ui = dyn_cast<UpcastInst>(v)) {
      v = ui->getOperand();
      continue;
    }

    SILValue v2 = stripSinglePredecessorArgs(v);
    v2 = stripOwnershipInsts(v2);
    if (v2 == v) {
      return v2;
    }
    v = v2;
  }
}

SILValue swift::stripClassCasts(SILValue v) {
  while (true) {
    if (auto *ui = dyn_cast<UpcastInst>(v)) {
      v = ui->getOperand();
      continue;
    }
    
    if (auto *ucci = dyn_cast<UnconditionalCheckedCastInst>(v)) {
      v = ucci->getOperand();
      continue;
    }

    SILValue v2 = stripOwnershipInsts(v);
    if (v2 != v) {
      v = v2;
      continue;
    }

    return v;
  }
}

SILValue swift::stripAddressProjections(SILValue V) {
  while (true) {
    V = stripSinglePredecessorArgs(V);
    if (!Projection::isAddressProjection(V))
      return V;
    V = cast<SingleValueInstruction>(V)->getOperand(0);
  }
}

SILValue swift::stripValueProjections(SILValue V) {
  while (true) {
    V = stripSinglePredecessorArgs(V);
    if (!Projection::isObjectProjection(V))
      return V;
    V = cast<SingleValueInstruction>(V)->getOperand(0);
  }
}

SILValue swift::stripIndexingInsts(SILValue V) {
  while (true) {
    if (!isa<IndexingInst>(V))
      return V;
    V = cast<IndexingInst>(V)->getBase();
  }
}

SILValue swift::stripExpectIntrinsic(SILValue V) {
  auto *BI = dyn_cast<BuiltinInst>(V);
  if (!BI)
    return V;
  if (BI->getIntrinsicInfo().ID != llvm::Intrinsic::expect)
    return V;
  return BI->getArguments()[0];
}

SILValue swift::stripBorrow(SILValue V) {
  if (auto *BBI = dyn_cast<BeginBorrowInst>(V))
    return BBI->getOperand();
  return V;
}

// All instructions handled here must propagate their first operand into their
// single result.
//
// This is guaranteed to handle all function-type converstions: ThinToThick,
// ConvertFunction, and ConvertEscapeToNoEscapeInst.
SingleValueInstruction *swift::getSingleValueCopyOrCast(SILInstruction *I) {
  if (auto *convert = dyn_cast<ConversionInst>(I))
    return convert;

  switch (I->getKind()) {
  default:
    return nullptr;
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::CopyBlockInst:
  case SILInstructionKind::CopyBlockWithoutEscapingInst:
  case SILInstructionKind::BeginBorrowInst:
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::MarkDependenceInst:
    return cast<SingleValueInstruction>(I);
  }
}

// Does this instruction terminate a SIL-level scope?
bool swift::isEndOfScopeMarker(SILInstruction *user) {
  switch (user->getKind()) {
  default:
    return false;
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::EndBorrowInst:
  case SILInstructionKind::EndLifetimeInst:
    return true;
  }
}

bool swift::isIncidentalUse(SILInstruction *user) {
  return isEndOfScopeMarker(user) || user->isDebugInstruction() ||
         isa<FixLifetimeInst>(user);
}

bool swift::onlyAffectsRefCount(SILInstruction *user) {
  switch (user->getKind()) {
  default:
    return false;
  case SILInstructionKind::AutoreleaseValueInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::RetainValueInst:
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::UnmanagedAutoreleaseValueInst:
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::Name##RetainValueInst:                              \
  case SILInstructionKind::Name##ReleaseValueInst:                             \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  case SILInstructionKind::Name##RetainInst:                                   \
  case SILInstructionKind::Name##ReleaseInst:                                  \
  case SILInstructionKind::StrongRetain##Name##Inst:                           \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#include "swift/AST/ReferenceStorage.def"
    return true;
  }
}

bool swift::mayCheckRefCount(SILInstruction *User) {
  return isa<IsUniqueInst>(User) || isa<IsEscapingClosureInst>(User) ||
         isa<BeginCOWMutationInst>(User);
}

bool swift::isSanitizerInstrumentation(SILInstruction *Instruction) {
  auto *BI = dyn_cast<BuiltinInst>(Instruction);
  if (!BI)
    return false;

  Identifier Name = BI->getName();
  if (Name == BI->getModule().getASTContext().getIdentifier("tsanInoutAccess"))
    return true;

  return false;
}

// Instrumentation instructions should not affect the correctness of the
// program. That is, they should not affect the observable program state.
// The constant evaluator relies on this property to skip instructions.
bool swift::isInstrumentation(SILInstruction *Instruction) {
  if (isSanitizerInstrumentation(Instruction))
    return true;

  if (BuiltinInst *bi = dyn_cast<BuiltinInst>(Instruction)) {
    if (bi->getBuiltinKind() == BuiltinValueKind::IntInstrprofIncrement)
      return true;
  }

  return false;
}

SILValue swift::isPartialApplyOfReabstractionThunk(PartialApplyInst *PAI) {
  // A partial_apply of a reabstraction thunk either has a single capture
  // (a function) or two captures (function and dynamic Self type).
  if (PAI->getNumArguments() != 1 &&
      PAI->getNumArguments() != 2)
    return SILValue();

  auto *Fun = PAI->getReferencedFunctionOrNull();
  if (!Fun)
    return SILValue();

  // Make sure we have a reabstraction thunk.
  if (Fun->isThunk() != IsReabstractionThunk)
    return SILValue();

  // The argument should be a closure.
  auto Arg = PAI->getArgument(0);
  if (!Arg->getType().is<SILFunctionType>() ||
      (!Arg->getType().isReferenceCounted(PAI->getFunction()->getModule()) &&
       Arg->getType().getAs<SILFunctionType>()->getRepresentation() !=
           SILFunctionType::Representation::Thick))
    return SILValue();

  return Arg;
}

bool swift::onlyUsedByAssignByWrapper(PartialApplyInst *PAI) {
  bool usedByAssignByWrapper = false;
  for (Operand *Op : PAI->getUses()) {
    SILInstruction *User = Op->getUser();
    if (isa<AssignByWrapperInst>(User) && Op->getOperandNumber() >= 2) {
      usedByAssignByWrapper = true;
      continue;
    }
    if (isa<DestroyValueInst>(User))
      continue;
    return false;
  }
  return usedByAssignByWrapper;
}

/// Given a block used as a noescape function argument, attempt to find all
/// Swift closures that invoking the block will call. The StoredClosures may not
/// actually be partial_apply instructions. They may be copied, block arguments,
/// or conversions. The caller must continue searching up the use-def chain.
static SILValue findClosureStoredIntoBlock(SILValue V) {

  auto FnType = V->getType().castTo<SILFunctionType>();
  assert(FnType->getRepresentation() == SILFunctionTypeRepresentation::Block);
  (void)FnType;

  // Given a no escape block argument to a function,
  // pattern match to find the noescape closure that invoking the block
  // will call:
  //     %noescape_closure = ...
  //     %wae_Thunk = function_ref @$withoutActuallyEscapingThunk
  //     %sentinel =
  //       partial_apply [callee_guaranteed] %wae_thunk(%noescape_closure)
  //     %noescaped_wrapped = mark_dependence %sentinel on %noescape_closure
  //     %storage = alloc_stack
  //     %storage_address = project_block_storage %storage
  //     store %noescaped_wrapped to [init] %storage_address
  //     %block = init_block_storage_header %storage invoke %thunk
  //     %arg = copy_block %block

  InitBlockStorageHeaderInst *IBSHI = dyn_cast<InitBlockStorageHeaderInst>(V);
  if (!IBSHI)
    return nullptr;

  SILValue BlockStorage = IBSHI->getBlockStorage();
  auto *PBSI = BlockStorage->getSingleUserOfType<ProjectBlockStorageInst>();
  assert(PBSI && "Couldn't find block storage projection");

  auto *SI = PBSI->getSingleUserOfType<StoreInst>();
  assert(SI && "Couldn't find single store of function into block storage");

  auto *CV = dyn_cast<CopyValueInst>(SI->getSrc());
  if (!CV)
    return nullptr;
  auto *WrappedNoEscape = dyn_cast<MarkDependenceInst>(CV->getOperand());
  if (!WrappedNoEscape)
    return nullptr;
  auto Sentinel = dyn_cast<PartialApplyInst>(WrappedNoEscape->getValue());
  if (!Sentinel)
    return nullptr;
  auto NoEscapeClosure = isPartialApplyOfReabstractionThunk(Sentinel);
  if (WrappedNoEscape->getBase() != NoEscapeClosure)
    return nullptr;

  // This is the value of the closure to be invoked. To find the partial_apply
  // itself, the caller must search the use-def chain.
  return NoEscapeClosure;
}

/// Find all closures that may be propagated into the given function-type value.
///
/// Searches the use-def chain from the given value upward until a partial_apply
/// is reached. Populates `results` with the set of partial_apply instructions.
///
/// `funcVal` may be either a function type or an Optional function type. This
/// might be called on a directly applied value or on a call argument, which may
/// in turn be applied within the callee.
void swift::findClosuresForFunctionValue(
    SILValue funcVal, TinyPtrVector<PartialApplyInst *> &results) {

  SILType funcTy = funcVal->getType();
  // Handle `Optional<@convention(block) @noescape (_)->(_)>`
  if (auto optionalObjTy = funcTy.getOptionalObjectType())
    funcTy = optionalObjTy;
  assert(funcTy.is<SILFunctionType>());

  SmallVector<SILValue, 4> worklist;
  // Avoid exponential path exploration and prevent duplicate results.
  llvm::SmallDenseSet<SILValue, 8> visited;
  auto worklistInsert = [&](SILValue V) {
    if (visited.insert(V).second)
      worklist.push_back(V);
  };
  worklistInsert(funcVal);

  while (!worklist.empty()) {
    SILValue V = worklist.pop_back_val();

    if (auto *I = V->getDefiningInstruction()) {
      // Look through copies, borrows, and conversions.
      //
      // Handle copy_block and copy_block_without_actually_escaping before
      // calling findClosureStoredIntoBlock.
      if (SingleValueInstruction *SVI = getSingleValueCopyOrCast(I)) {
        worklistInsert(SVI->getOperand(0));
        continue;
      }
      // Look through `differentiable_function` operands, which are all
      // function-typed.
      if (auto *DFI = dyn_cast<DifferentiableFunctionInst>(I)) {
        for (auto &fn : DFI->getAllOperands())
          worklistInsert(fn.get());
        continue;
      }
    }
    // Look through Optionals.
    if (V->getType().getOptionalObjectType()) {
      auto *EI = dyn_cast<EnumInst>(V);
      if (EI && EI->hasOperand()) {
        worklistInsert(EI->getOperand());
      }
      // Ignore the .None case.
      continue;
    }
    // Look through Phis.
    //
    // This should be done before calling findClosureStoredIntoBlock.
    if (auto *arg = dyn_cast<SILPhiArgument>(V)) {
      SmallVector<std::pair<SILBasicBlock *, SILValue>, 2> blockArgs;
      arg->getIncomingPhiValues(blockArgs);
      for (auto &blockAndArg : blockArgs)
        worklistInsert(blockAndArg.second);

      continue;
    }
    // Look through ObjC closures.
    auto fnType = V->getType().getAs<SILFunctionType>();
    if (fnType
        && fnType->getRepresentation() == SILFunctionTypeRepresentation::Block) {
      if (SILValue storedClosure = findClosureStoredIntoBlock(V))
        worklistInsert(storedClosure);

      continue;
    }
    if (auto *PAI = dyn_cast<PartialApplyInst>(V)) {
      SILValue thunkArg = isPartialApplyOfReabstractionThunk(PAI);
      if (thunkArg) {
        // Handle reabstraction thunks recursively. This may reabstract over
        // @convention(block).
        worklistInsert(thunkArg);
        continue;
      }
      results.push_back(PAI);
      continue;
    }
    // Ignore other unrecognized values that feed this applied argument.
  }
}

bool PolymorphicBuiltinSpecializedOverloadInfo::init(
    SILFunction *fn, BuiltinValueKind builtinKind,
    ArrayRef<SILType> oldOperandTypes, SILType oldResultType) {
  assert(!isInitialized && "Expected uninitialized info");
  SWIFT_DEFER { isInitialized = true; };
  if (!isPolymorphicBuiltin(builtinKind))
    return false;

  // Ok, at this point we know that we have a true polymorphic builtin. See if
  // we have an overload for its current operand type.
  StringRef name = getBuiltinName(builtinKind);
  StringRef prefix = "generic_";
  assert(name.startswith(prefix) &&
         "Invalid polymorphic builtin name! Prefix should be Generic$OP?!");
  SmallString<32> staticOverloadName;
  staticOverloadName.append(name.drop_front(prefix.size()));

  // If our first argument is an address, we know we have an indirect @out
  // parameter by convention since all of these polymorphic builtins today never
  // take indirect parameters without an indirect out result parameter. We stash
  // this information and validate that if we have an out param, that our result
  // is equal to the empty tuple type.
  if (oldOperandTypes[0].isAddress()) {
    if (oldResultType != fn->getModule().Types.getEmptyTupleType())
      return false;

    hasOutParam = true;
    SILType firstType = oldOperandTypes.front();

    // We only handle polymorphic builtins with trivial types today.
    if (!firstType.is<BuiltinType>() || !firstType.isTrivial(*fn)) {
      return false;
    }

    resultType = firstType.getObjectType();
    oldOperandTypes = oldOperandTypes.drop_front();
  } else {
    resultType = oldResultType;
  }

  // Then go through all of our values and bail if any after substitution are
  // not concrete builtin types. Otherwise, stash each of them in the argTypes
  // array as objects. We will convert them as appropriate.
  for (SILType ty : oldOperandTypes) {
    // If after specialization, we do not have a trivial builtin type, bail.
    if (!ty.is<BuiltinType>() || !ty.isTrivial(*fn)) {
      return false;
    }

    // Otherwise, we have an object builtin type ready to go.
    argTypes.push_back(ty.getObjectType());
  }

  // Ok, we have all builtin types. Infer the underlying polymorphic builtin
  // name form our first argument.
  CanBuiltinType builtinType = argTypes.front().getAs<BuiltinType>();
  SmallString<32> builtinTypeNameStorage;
  StringRef typeName = builtinType->getTypeName(builtinTypeNameStorage, false);
  staticOverloadName.append("_");
  staticOverloadName.append(typeName);

  auto &ctx = fn->getASTContext();
  staticOverloadIdentifier = ctx.getIdentifier(staticOverloadName);

  // Ok, we have our overload identifier. Grab the builtin info from the
  // cache. If we did not actually found a valid builtin value kind for our
  // overload, then we do not have a static overload for the passed in types, so
  // return false.
  builtinInfo = &fn->getModule().getBuiltinInfo(staticOverloadIdentifier);
  return true;
}

bool PolymorphicBuiltinSpecializedOverloadInfo::init(BuiltinInst *bi) {
  assert(!isInitialized && "Can not init twice?!");
  SWIFT_DEFER { isInitialized = true; };

  // First quickly make sure we have a /real/ BuiltinValueKind, not an intrinsic
  // or None.
  auto kind = bi->getBuiltinKind();
  if (!kind)
    return false;

  SmallVector<SILType, 8> oldOperandTypes;
  copy(bi->getOperandTypes(), std::back_inserter(oldOperandTypes));
  assert(bi->getNumResults() == 1 &&
         "We expect a tuple here instead of real args");
  SILType oldResultType = bi->getResult(0)->getType();
  return init(bi->getFunction(), *kind, oldOperandTypes, oldResultType);
}

SILValue
swift::getStaticOverloadForSpecializedPolymorphicBuiltin(BuiltinInst *bi) {

  PolymorphicBuiltinSpecializedOverloadInfo info;
  if (!info.init(bi))
    return SILValue();

  SmallVector<SILValue, 8> rawArgsData;
  copy(bi->getOperandValues(), std::back_inserter(rawArgsData));

  SILValue result = bi->getResult(0);
  MutableArrayRef<SILValue> rawArgs = rawArgsData;

  if (info.hasOutParam) {
    result = rawArgs.front();
    rawArgs = rawArgs.drop_front();
  }

  assert(bi->getNumResults() == 1 &&
         "We assume that builtins have a single result today. If/when this "
         "changes, this code needs to be updated");

  SILBuilderWithScope builder(bi);

  // Ok, now we know that we can convert this to our specialized
  // builtin. Prepare the arguments for the specialized value, loading the
  // values if needed and storing the result into an out parameter if needed.
  //
  // NOTE: We only support polymorphic builtins with trivial types today, so we
  // use load/store trivial as a result.
  SmallVector<SILValue, 8> newArgs;
  for (SILValue arg : rawArgs) {
    if (arg->getType().isObject()) {
      newArgs.push_back(arg);
      continue;
    }

    SILValue load = builder.emitLoadValueOperation(
        bi->getLoc(), arg, LoadOwnershipQualifier::Trivial);
    newArgs.push_back(load);
  }

  BuiltinInst *newBI =
      builder.createBuiltin(bi->getLoc(), info.staticOverloadIdentifier,
                            info.resultType, {}, newArgs);

  // If we have an out parameter initialize it now.
  if (info.hasOutParam) {
    builder.emitStoreValueOperation(newBI->getLoc(), newBI->getResult(0),
                                    result, StoreOwnershipQualifier::Trivial);
  }

  return newBI;
}
