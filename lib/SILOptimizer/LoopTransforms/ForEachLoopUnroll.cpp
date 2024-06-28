//===--- ForEachLoopUnrolling.cpp - Unroll loops over array literals ----- ===//
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

// This pass unrolls Sequence.forEach calls invoked on an array created
// from an array literal. See below for an overview of the algorithm. This is
// a function-level transformation pass that operates on ownership SIL and
// so must be applied before ownership is stripped.
//
// Algorithm overview:
//
// 1. Iterate over the body of the function and analyze calls to the array
//    initializer annotated "array.uninitialized_intrinsic". This is done in
//    `run` method.
//
// 2. For every "array.uninitialized_intrinsic" initializer call, try
//    extracting the elements with which the array is initialized, and classify
//    the uses of the array into incidental uses, forEach calls and uses that
//    can write into the array. If any of the following conditions hold, give
//    up and look for the next initializer call:
//         - The elements stored into the array cannot be extracted.
//         - The array can be modified after initialization.
//         - There are no forEach calls on the array.
//         - The array is too large to unroll forEach calls. This check uses
//            SILModule::UnrollThreshold parameter.
//    If none of the above conditions hold, procede to unrolling every forEach
//    call on the array (Step 3). Step 2 is implemented in the function:
//    `tryUnrollForEachCallsOverArrayLiteral`.
//
// 3. Given a forEach call, unroll it by applying the "body closure" passed to
//    the call to every element of the array. There are three important
//    details to this unrolling. Firstly, the SILValue of the element stored
//    into the array through the store instruction (identified in step 2) may
//    not be valid at the point where the unrolling must happen (i.e., where the
//    forEach is called). Therefore, we need to copy_value the elements at the
//    point where they are stored and ensure that the copies are alive until
//    they are used by the unrolled code. The implementation actually
//    makes the copies valid for the entire lifetime of the array.
//
//    Secondly, the body closure uses @in_guaranteed convention for the
//    parameter. Therefore, an alloc_stack is created before the unrolled code
//    begins to hold the elements, and is destroyed once the unrolled code ends.
//
//    Thirdly, the body closure throws. Hence, it has to be try_applied. This
//    means that we need to chain the try_applies in such a way that when the
//    try_apply on the element e_i completes normally it jumps to the try_apply
//    of e_i+1. When the try_apply (of any element) throws, it must go to the
//    error block of the original forEach call.
//
//    All of this is implemented by the function `unrollForEach`.
//
// 4. Delete the forEach calls that were unrolled and clean up dead code
//    resulting due to that.
//
// This transformation is illustrated below on a small example:
//
// Input:
//      %initResult = apply %arrayUninitialized(...)
//      (%array, %storage_ptr) = destructure_tuple %initResult
//      store %elem1 at array index 1
//      store %elem2 at array index 2
//      ..
//      try_apply %forEach(%body, %array) normal bb1, error bb2
//    bb1(%res : $()):
//      ..
//    bb2(%error : @owned $Error):
//      ...
//    bb3:
//      destroy_value %array
//
// Output:
//
//      %initResult = apply %arrayUninitialized(...)
//      (%array, %storage_ptr) = destructure_tuple %initResult
//      %elem1copy = copy_value %elem1  <--
//      store %elem1 at array index 1
//      %elem2copy = copy_value %elem2  <--
//      store %elem2 at array index 2
//      ..
//      alloc_stack %stack
//      %elem1borrow = begin_borrow %elem1copy
//      store_borrow %elem1borrow to %stack
//      try_apply %body(%stack) normal normalbb1, error errbb1
//
//    errbb1(%error : @owned $Error):
//      end_borrow %elem1borrow
//      br bb2(%error)
//
//    normalbb1(%res : $()):
//      end_borrow %elem1borrow
//      %elem2borrow = begin_borrow %elem2copy
//      store_borrow %elem2borrow to %stack
//      try_apply %body(%stack) normal bb1, error errbb2
//
//    errbb2(%error : @owned $Error):
//      end_borrow %elem2borrow
//      br bb2(%error)
//
//    bb1(%res : $()):
//      end_borrow %elem2borrow
//      dealloc_stack %stack
//      ...
//    bb2(%error : @owned $Error):
//      ...
//      dealloc_stack %stack
//    bb3:
//      destroy_value %elem1copy
//      destroy_value %elem2copy
//      destroy_value %array

#include "swift/AST/ASTContext.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILConstants.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;

namespace {

/// A class for processing and storing information about an array
/// such as the values with which it is initialized and the kind of
/// users it has.
class ArrayInfo {
  /// The array value returned by the _allocateUninitialized call.
  SILValue arrayValue;

  /// A map from an array index to the store instruction that initializes that
  /// index.
  llvm::DenseMap<uint64_t, StoreInst *> elementStoreMap;

  /// List of Sequence.forEach calls invoked on the array.
  llvm::SmallSetVector<TryApplyInst *, 4> forEachCalls;

  /// Indicates whether the array could be modified after initialization. Note
  /// that this not include modifications to the elements of the array. When
  /// set, this will prevent the forEach loops from getting unrolled.
  bool mayBeWritten = false;

  /// Instructions that destroy the array. These must be destroy_value
  /// instructions either of the \c arrayValue or the copy_values of that.
  SmallVector<SILInstruction *, 4> destroys;

  /// Classify uses of the array into forEach uses, read-only uses etc. and set
  /// the fields of this instance appropriately. This function will recursively
  /// classify the uses of borrows and copy-values of the array as well.
  void classifyUsesOfArray(SILValue arrayValue);

public:
  ArrayInfo() {}

  /// Given an apply instruction \c apply, try to initialize this ArrayInfo
  /// with it. This would succeed iff the apply instruction starts an
  /// initialization pattern that is auto-generated by the compiler for array
  /// literals. Return true on success and false on failure.
  bool tryInitialize(ApplyInst *apply);

  /// Return the SILValue of the array that is initialized.
  SILValue getArrayValue() {
    assert(arrayValue);
    return arrayValue;
  }

  /// Return true iff the array could be modified after initialization.
  bool mayBeModified() {
    assert(arrayValue);
    return mayBeWritten;
  }

  /// Return the forEach uses identified for the array.
  ArrayRef<TryApplyInst *> getForEachUses() {
    assert(arrayValue);
    return ArrayRef<TryApplyInst *>(forEachCalls.begin(), forEachCalls.end());
  }

  /// Return the number of elements in the array.
  uint64_t getNumElements() {
    assert(arrayValue);
    return elementStoreMap.size();
  }

  /// Return the store instruction that initializes the given \c index
  /// of the array.
  StoreInst *getElementStore(uint64_t index) {
    assert(arrayValue);
    return elementStoreMap[index];
  }

  /// Return the SIL type of the elements stored in the array.
  /// \pre the array must be non-empty.
  SILType getElementSILType() {
    assert(getNumElements() > 0 && "cannot call this on empty arrays");
    return elementStoreMap[0]->getSrc()->getType();
  }

  /// Add the destroy_value instructions that represents the last use of the
  /// array to the parameter \c lastDestroys. The \c lastDestroys
  /// added by this function are guaranteed to come after all uses of the
  /// \c arrayValue and copy_values of the \c arrayValue (along all
  /// "non-dead-end" blocks).
  void getLastDestroys(SmallVectorImpl<DestroyValueInst *> &lastDestroys);
};

/// Given a Array-typed SIL value \c array and an instruction \c user that uses
/// the array, check whether this use actually represents a call to _fixLifetime
/// function. This would be the case if \c user is a store_borrow instruction
/// that stores into an alloc_stack which is passed to a fixLifetime
/// instruction. That is, if the following pattern holds:
///
///            %stack = alloc_stack
///      user: store_borrow %array to %stack
///            fixLifetime %stack
/// \returns the fixLifetime instruction if this is a fixLifetime use of the
/// array, nullptr otherwise.
static FixLifetimeInst *isFixLifetimeUseOfArray(SILInstruction *user,
                                                SILValue array) {
  StoreBorrowInst *storeUser = dyn_cast<StoreBorrowInst>(user);
  if (!storeUser || storeUser->getSrc() != array)
    return nullptr;
  AllocStackInst *alloc = dyn_cast<AllocStackInst>(storeUser->getDest());
  if (!alloc)
    return nullptr;
  auto fixLifetimeUsers = storeUser->getUsersOfType<FixLifetimeInst>();
  if (fixLifetimeUsers.empty())
    return nullptr;
  auto firstUser = fixLifetimeUsers.begin();
  FixLifetimeInst *result = *firstUser;
  // We need to have a unique result.
  if (++firstUser != fixLifetimeUsers.end())
    return nullptr;
  return result;
}

/// Given an array-typed SIL value \c array and an instruction \c user that uses
/// the array, check whether this use actually represents a call to the
/// Sequence.forEach function. This would be case if \c user is a store_borrow
/// instruction that stores into an alloc_stack which is passed to a try-apply
/// of the Sequence.forEach function. That is, if the following pattern holds:
///
///            %stack = alloc_stack
///      user: store_borrow %array to %stack
///            try_apply %forEachCall(%closure, %array)
/// \returns the try-apply instruction invoking the forEach function if this is
/// a forEach use of the array, nullptr otherwise.
static TryApplyInst *isForEachUseOfArray(SILInstruction *user, SILValue array) {
  StoreBorrowInst *storeUser = dyn_cast<StoreBorrowInst>(user);
  if (!storeUser || storeUser->getSrc() != array)
    return nullptr;
  AllocStackInst *alloc = dyn_cast<AllocStackInst>(storeUser->getDest());
  if (!alloc)
    return nullptr;
  auto applyUsers = storeUser->getUsersOfType<TryApplyInst>();
  if (applyUsers.empty())
    return nullptr;
  auto firstUser = applyUsers.begin();
  TryApplyInst *apply = *firstUser;
  // We need to have a unique result.
  if (++firstUser != applyUsers.end())
    return nullptr;
  SILFunction *callee = apply->getCalleeFunction();
  if (!callee || !callee->hasSemanticsAttr(semantics::SEQUENCE_FOR_EACH))
    return nullptr;
  return apply;
}

void ArrayInfo::classifyUsesOfArray(SILValue arrayValue) {
  for (Operand *operand : arrayValue->getUses()) {
    auto *user = operand->getUser();
    if (isIncidentalUse(user))
      continue;
    // Ignore this user if it is a call to _fixLifetime. Note that this use
    // will not be subsumed by InstructionUtils::isIncidentalUse check made
    // above as the array would be passed indirectly.
    if (isFixLifetimeUseOfArray(user, arrayValue))
      continue;
    if (auto *MDI = dyn_cast<MarkDependenceInst>(user)) {
      if (MDI->getBase() == arrayValue) {
        continue;
      }
    }
    // Check if this is a forEach call on the array.
    if (TryApplyInst *forEachCall = isForEachUseOfArray(user, arrayValue)) {
      forEachCalls.insert(forEachCall);
      continue;
    }
    // Recursively classify begin_borrow, copy_value, and move_value uses.
    if (BeginBorrowInst *beginBorrow = dyn_cast<BeginBorrowInst>(user)) {
      classifyUsesOfArray(beginBorrow);
      continue;
    }
    if (CopyValueInst *copyValue = dyn_cast<CopyValueInst>(user)) {
      classifyUsesOfArray(copyValue);
      continue;
    }
    if (MoveValueInst *moveValue = dyn_cast<MoveValueInst>(user)) {
      classifyUsesOfArray(moveValue);
      continue;
    }
    if (DestroyValueInst *destroyValue = dyn_cast<DestroyValueInst>(user)) {
      destroys.push_back(destroyValue);
      continue;
    }
    // Set mayBeWritten to true if the user could potentially modify the array.
    // Note that the array elements are allowed to be modified as long
    // as the array itself is not modified (which is possible with reference
    // types).
    ArraySemanticsCall arrayOp(user);
    if (arrayOp.doesNotChangeArray())
      continue;
    
    if (arrayOp.getKind() == swift::ArrayCallKind::kArrayFinalizeIntrinsic) {
      classifyUsesOfArray((ApplyInst *)arrayOp);
      continue;
    }
    
    mayBeWritten = true;
  }
}

bool ArrayInfo::tryInitialize(ApplyInst *apply) {
  ArraySemanticsCall arrayAllocateUninitCall(
      apply, semantics::ARRAY_UNINITIALIZED_INTRINSIC);
  if (!arrayAllocateUninitCall)
    return false;
  arrayValue = arrayAllocateUninitCall.getArrayValue();
  if (!arrayValue)
    return false;
  if (!arrayAllocateUninitCall.mapInitializationStores(elementStoreMap))
    return false;
  // Collect information about uses of the array value.
  classifyUsesOfArray(arrayValue);
  return true;
}

void ArrayInfo::getLastDestroys(
    SmallVectorImpl<DestroyValueInst *> &lastDestroys) {
  assert(arrayValue);
  // Collect the frontier instructions of the field \c destroys, which stores
  // destroy_value instructions of the \c arrayValue as well as of its
  // copy_values, using ValueLifetimeAnalysis. The frontier is a list of
  // instructions that mark the exits of the flow of control from the
  // \c destroys.
  ValueLifetimeAnalysis lifetimeAnalysis(arrayValue->getDefiningInstruction(),
                                         destroys);
  ValueLifetimeAnalysis::Frontier frontier;
  lifetimeAnalysis.computeFrontier(frontier,
                                   ValueLifetimeAnalysis::DontModifyCFG);
  for (SILInstruction *frontierInst : frontier) {
    // Skip frontier instructions at the start of a basic block as they do not
    // follow a destroy_value of the array. Note that the goal is to collect
    // the last destroys, which must always immediately precede a frontier
    // instruction as it marks the end of the use of the array.
    if (frontierInst == &frontierInst->getParent()->front())
      continue;
    SILInstruction *inst = &*(--frontierInst->getIterator());
    // This must be a destroy instruction. Moreover it must also belong to \c
    // destroys.
    DestroyValueInst *lastDestroy = dyn_cast<DestroyValueInst>(inst);
    assert(lastDestroy);
    lastDestroys.push_back(cast<DestroyValueInst>(lastDestroy));
  }
}

/// Delete the forEach calls from the SIL that contains it. This function will
/// not clean up any resulting dead instructions.
static void removeForEachCall(TryApplyInst *forEachCall,
                              InstructionDeleter &deleter) {
  auto *sbi = cast<StoreBorrowInst>(forEachCall->getArgument(1));
  auto *asi = cast<AllocStackInst>(sbi->getDest());
  // The allocStack will be used in the forEach call and also in a store
  // instruction and a dealloc_stack instruction. Force delete all of them.
  deleter.recursivelyForceDeleteUsersAndFixLifetimes(asi);
}

/// Unroll the \c forEachCall on an array, using the information in
/// \c ArrayInfo. Once unrolled, \c forEachCall will be deleted
/// from the containing function.  This function implements the following
/// transformation.
///  - If the array stores non-trivial elements, for every element stored
///   into the array, insert a copy_value of the element just before it
///   is stored. This is necessary so that an @owned SILValue for the
///   element is available at the point where the forEach is used.
///
///  - Create an alloc_stack A at the point of the forEach call. This is
///   necessary to indirectly pass the elements of the array.
///
///  For every element e_i at the index i of the array, do the following:
///    - create a new basic block b_i if i > 0. Let b_0 denote the basic block
///      that contains the forEach call.
///    - store_borrow the element e_i into the alloc_stack A. Note that we
///      can use the owned copy of e_i created in the previous step.
///    - try_apply the forEach's body closure on the alloc_stack A.
///      If i is not the last index, jump to b_i+1 in the normal case of the
///      try_apply. If i is the last index of the array jump to the normal
///      target of the forEach call. Jump to a new error block: err_i in the
///      error case. Make err_i jump to the error target of the original
///      forEach call.
///
///  - Dealloc the alloc_stack along the normal and error targets of the
///   forEach calls.
///
///  - Destroy all the copies of the elements (if it is non-trivial) just
///   before the array's lifetime ends.
static void unrollForEach(ArrayInfo &arrayInfo, TryApplyInst *forEachCall,
                          InstructionDeleter &deleter) {
  if (arrayInfo.getNumElements() == 0) {
    // If this is an empty array, delete the forEach entirely.
    removeForEachCall(forEachCall, deleter);
    return;
  }

  SILFunction *fun = forEachCall->getFunction();
  SILLocation forEachLoc = forEachCall->getLoc();
  SILValue forEachBodyClosure = forEachCall->getArgument(0);
  SILType arrayElementType = arrayInfo.getElementSILType();

  SILFunctionType *bodyClosureType =
    forEachBodyClosure->getType().getAs<SILFunctionType>();
  SILParameterInfo bodyParameterInfo = bodyClosureType->getParameters()[0];
  // The forEachBodyClosure must use @in_guaranteed convention for passing
  // arguments.
  assert(bodyParameterInfo.getConvention() ==
         ParameterConvention::Indirect_In_Guaranteed &&
         "forEach body closure is expected to take @in_guaranteed argument");

  // Copy the elements stored into the array. This is necessary as we need to
  // extend the lifetime of the stored elements at least until the forEach call,
  // which will now be unrolled. The following code inserts a copy_value of the
  // elements just before the point where they are stored into the array, and
  // a corresponding destroy_value at the end of the lifetime of the array. In
  // other words, the lifetime of the element copies are made to match the
  // lifetime of the array. Even though copies can be destroyed sooner after
  // they are used by the unrolled code, doing so may introduce more destroys
  // than needed as the unrolled code have many branches (due to try applies)
  // all of which joins later into a single path eventually.
  SmallVector<SILValue, 4> elementCopies;
  for (uint64_t i = 0; i < arrayInfo.getNumElements(); ++i) {
    StoreInst *elementStore = arrayInfo.getElementStore(i);
    // Insert the copy just before the store of the element into the array.
    SILValue copy = SILBuilderWithScope(elementStore)
                        .emitCopyValueOperation(elementStore->getLoc(),
                                                elementStore->getSrc());
    elementCopies.push_back(copy);
  }

  // Destroy the copy_value of the elements before the last destroys of the
  // array. It is important that the copied elements are destroyed before the
  // array is destroyed. This enables other optimizations.
  SmallVector<DestroyValueInst *, 4> lastDestroys;
  arrayInfo.getLastDestroys(lastDestroys);
  for (DestroyValueInst *destroy : lastDestroys) {
    SILBuilderWithScope destroyBuilder(destroy);
    for (SILValue element : elementCopies) {
      destroyBuilder.emitDestroyValueOperation(destroy->getLoc(), element);
    }
  }

  // Create alloc_stack for passing the array elements indirectly.
  SILValue allocStack = SILBuilderWithScope(forEachCall)
                            .createAllocStack(forEachLoc, arrayElementType);

  // Extract the Error and normal targets of the forEach call. Both these
  // targets must be taking a phi argument.
  SILBasicBlock *normalBB = forEachCall->getNormalBB();
  SILBasicBlock *errorBB = forEachCall->getErrorBB();
  assert(normalBB->getSILPhiArguments().size() == 1);
  SILPhiArgument *normalArgument = normalBB->getSILPhiArguments()[0];
  SILPhiArgument *errorArgument = nullptr;
  if (errorBB->getSILPhiArguments().size() == 1)
    errorArgument = errorBB->getSILPhiArguments()[0];

  // A generator for creating a basic block for use as the target of the
  // "normal" branch of a try_apply.
  auto normalTargetGenerator = [&](SILBasicBlock *insertionBlock) {
    SILBasicBlock *newBB = fun->createBasicBlockBefore(insertionBlock);
    newBB->createPhiArgument(normalArgument->getType(),
                             normalArgument->getOwnershipKind());
    return newBB;
  };

  // A generator for creating a basic block for use as the target of the
  // "error" branch of a try_apply. The error block created here will always
  // jump to the error target of the original forEach.
  auto errorTargetGenerator = [&](SILBasicBlock *insertionBlock,
                                  SILValue borrowedElem, SILValue storeBorrow) {
    SILBasicBlock *newErrorBB = fun->createBasicBlockBefore(insertionBlock);
    SILValue argument;
    if (errorArgument) {
      argument = newErrorBB->createPhiArgument(
        errorArgument->getType(), errorArgument->getOwnershipKind());
    }

    // Make the errorBB jump to the error target of the original forEach.
    SILBuilderWithScope builder(newErrorBB, forEachCall);
    if (storeBorrow) {
      builder.createEndBorrow(forEachLoc, storeBorrow);
    }
    if (borrowedElem) {
      builder.createEndBorrow(forEachLoc, borrowedElem);
    }

    if (argument)
      builder.createBranch(forEachLoc, errorBB, argument);
    else
      builder.createBranch(forEachLoc, errorBB);
    return newErrorBB;
  };

  // The basic block to jump to in the normal case of the try_apply in each
  // unrolling.
  SILBasicBlock *nextNormalBB = normalBB;

  // Iterate through the array indices in the reverse order and do the
  // following:
  //   - create a new basic block b_i if i > 0. Let b_0 denote the basic block
  //     that contains the forEach call.
  //   - store_borrow the owned copy of the element e_i into the `allocStack`.
  //   - try_apply the forEach's body closure on the `allocStack`. The normal
  //     target of the try_apply is b_i+1 if i is not the last index, otherwise
  //     it is `normalBB`. (The normal target is captured by `nextNormalBB`.)
  //     Jump to a new error block: err_i in the error case. Note that all
  //     error blocks jump to the error target of the original forEach call.
  for (uint64_t num = arrayInfo.getNumElements(); num > 0; --num) {
    SILValue elementCopy = elementCopies[num - 1];
    // Creating the next normal block ends the borrow scope for borrowedElem
    // from the previous iteration.
    SILBasicBlock *currentBB = num > 1 ? normalTargetGenerator(nextNormalBB)
                                       : forEachCall->getParentBlock();
    SILBuilderWithScope unrollBuilder(currentBB, forEachCall);
    SILBuilderWithScope normalBuilder(&nextNormalBB->front(), forEachCall);
    SILValue borrowedElem;
    SILValue addr;

    if (arrayElementType.isTrivial(*fun)) {
      unrollBuilder.createStore(forEachLoc, elementCopy, allocStack,
                                StoreOwnershipQualifier::Trivial);
      addr = allocStack;
    } else {
      // Borrow the elementCopy and store it in the allocStack. Note that the
      // element's copy is guaranteed to be alive until the array is alive.
      // Therefore it is okay to use a borrow here.
      borrowedElem = unrollBuilder.createBeginBorrow(forEachLoc, elementCopy);
      addr =
          unrollBuilder.createStoreBorrow(forEachLoc, borrowedElem, allocStack);
      normalBuilder.createEndBorrow(forEachLoc, addr);
      normalBuilder.createEndBorrow(forEachLoc, borrowedElem);
    }

    SILBasicBlock *errorTarget =
        errorTargetGenerator(nextNormalBB, borrowedElem,
                             isa<StoreBorrowInst>(addr) ? addr : SILValue());
    // Note that the substitution map must be empty as we are not supporting
    // elements of address-only type. All elements in the array are guaranteed
    // to be loadable. TODO: generalize this to address-only types.
    unrollBuilder.createTryApply(forEachLoc, forEachBodyClosure,
                                 SubstitutionMap(), addr, nextNormalBB,
                                 errorTarget);

    if (nextNormalBB == normalBB) {
      // Dealloc the stack in the normalBB and also in errorBB. Note that every
      // try_apply created during the unrolling must pass through these blocks.
      normalBuilder.createDeallocStack(forEachLoc, allocStack);
    }
    nextNormalBB = currentBB;
  }
  // Dealloc the stack in the normalBB and also in errorBB. Note that every
  // try_apply created during the unrolling must pass through these blocks.
  SILBuilderWithScope(&errorBB->front())
      .createDeallocStack(forEachLoc, allocStack);
  // Remove the forEach call as it has now been unrolled.
  removeForEachCall(forEachCall, deleter);
}

/// Determine whether the cost of unrolling the forEach is within the
/// \c unrollThreshold.
static bool canUnrollForEachOfArray(ArrayInfo arrayInfo, SILModule &module) {
  const uint64_t unrollThreshold = module.getOptions().UnrollThreshold;
  // The cost of unrolling a forEach loop is mostly just two instructions per
  // array element: one to store the element into an alloc_stack and another to
  // invoke the forEach body closure with the element. Note that the copy_value
  // of the element and the basic blocks created for the try-applies of the
  // body closure are not counted, as these should likely get optimized away.
  const uint64_t cost = 2;
  return arrayInfo.getNumElements() * cost <= unrollThreshold;
}

static bool tryUnrollForEachCallsOverArrayLiteral(ApplyInst *apply,
                                                  InstructionDeleter &deleter) {
  ArrayInfo arrayInfo;
  if (!arrayInfo.tryInitialize(apply))
    return false;
  // Bail out, if the array could be modified after initialization.
  if (arrayInfo.mayBeModified())
    return false;
  // If there are no forEach loops to unroll, return.
  ArrayRef<TryApplyInst *> forEachCalls = arrayInfo.getForEachUses();
  if (forEachCalls.empty())
    return false;
  // If the array is too large to unroll, bail out.
  if (!canUnrollForEachOfArray(arrayInfo, apply->getParent()->getModule()))
    return false;
  for (TryApplyInst *forEachCall : forEachCalls)
    unrollForEach(arrayInfo, forEachCall, deleter);
  return true;
}

class ForEachLoopUnroller : public SILFunctionTransform {

  ~ForEachLoopUnroller() override {}

  void run() override {
    SILFunction &fun = *getFunction();
    bool changed = false;

    if (!fun.hasOwnership())
      return;

    InstructionDeleter deleter;
    for (SILBasicBlock &bb : fun) {
      for (auto instIter = bb.begin(); instIter != bb.end();) {
        SILInstruction *inst = &*instIter;
        ApplyInst *apply = dyn_cast<ApplyInst>(inst);
        if (!apply) {
          ++instIter;
          continue;
        }
        // Note that the following operation may delete a forEach call but
        // would not delete this apply instruction, which is an array
        // initializer. Therefore, the iterator should be valid here.
        changed |= tryUnrollForEachCallsOverArrayLiteral(apply, deleter);
        ++instIter;
      }
    }

    if (changed) {
      deleter.cleanupDeadInstructions();
      PM->invalidateAnalysis(&fun,
                  SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createForEachLoopUnroll() {
  return new ForEachLoopUnroller();
}
