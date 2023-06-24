//===--- AddressLowering.cpp - Lower SIL address-only types. --------------===//
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
/// This pass removes "opaque SILValues" by translating them into addressable
/// memory locations such as a stack locations. This is mandatory for IRGen.
///
/// Lowering to LLVM IR requires each SILValue's type to be a valid "SIL storage
/// type". Opaque SILValues have address-only types. These require indirect
/// storage in LLVM, so their SIL storage type must be an address type.
///
/// This pass never creates copies except to replace explicit value copies
/// (copy_value, load [copy], store). For move-only values, this allows complete
/// diagnostics. And in general, this makes it impossible for SIL passes to
/// "accidentally" create copies.
///
/// This pass inserts moves (copy_addr [take] [initialize]) of owned values to
/// - compose aggregates
/// - resolve phi interference
///
/// For guaranteed values, this pass inserts neither copies nor moves. Opaque
/// values are potentially unmovable when borrowed. This means that guaranteed
/// address-only aggregates and phis are prohibited. This SIL invariant is
/// enforced by SILVerifier::checkOwnershipForwardingInst() and
/// SILVerifier::visitSILPhiArgument().
///
/// The simplest approach to address lowering is to map each opaque SILValue to
/// a separate alloc_stack. This pass avoids doing that in the following cases:
///
/// 1. Reused-storage: Some operations are guaranteed to reuse their operand's
/// storage. This includes extracting an enum payload and opening an existential
/// value. This is required to avoid introducing new copies or moves.
///
///   // %data's storage must reuse storage allocated for %enum
///   %data = unchecked_enum_data %enum : $Optional<T>, #Optional.some!enumelt
///
/// 2. Def-projection: Some operations are guaranteed to directly project out of
/// their operand's storage. This is also required to avoid introducing new
/// copies or moves. Unlike reused-storage, such projections are non-destructive
/// and repeatable.
///
///   // %field's storage is part of the storage allocated for %struct
///   %field = struct_extract %struct, #field
///
/// 3. Use-projection: Operations that compose aggregates may optionally allow
/// their operands to project into the storage allocated for their result. This
/// is only an optimization but is essential for reasonable code generation.
///
///  // %field's storage may be part of the storage allocated for %struct
///  %struct = struct(..., %field, ...)
///
/// 4. Phi-projection: Phi's may optionally allow their (branch) operands to
/// reuse the storage allocated for their result (block argument). This is only
/// an optimization, but is important to avoid many useless moves:
///
///   // %arg's storage may be part of the storage allocated for %phi
///   br bb(%arg)
///   bb(%phi : @owned $T)
///
/// The algorithm proceeds as follows:
///
/// ## Step #1: Map opaque values
///
/// Populate a map from each opaque SILValue to its ValueStorage in forward
/// order (RPO). Each opaque value is mapped to an ordinal ID representing the
/// storage. Storage locations can now be optimized by remapping the values.
///
/// Reused-storage operations are not mapped to ValueStorage.
///
/// ## Step #2: Allocate storage
///
/// In reverse order (PO), allocate the parent storage object for each opaque
/// value.
///
/// Handle def-projection: If the value is a subobject extraction
/// (struct_extract, tuple_extract, open_existential_value,
/// unchecked_enum_data), then mark the value's storage as a projection from the
/// def's storage.
///
/// Handle use-projection: If the value's use composes a parent object from this
/// value (struct, tuple, enum), and the use's storage dominates this value,
/// then mark the value's storage as a projection into the use's storage.
///
/// ValueStorage projections can be chained. A non-projection ValueStorage is
/// the root of a tree of projections.
///
/// When allocating storage, each ValueStorage root has its `storageAddress`
/// assigned to an `alloc_stack` or an argument. Opaque values that are storage
/// projections are not mapped to a `storageAddress` at this point. That happens
/// during rewriting.
///
/// Handle phi-projection: After allocating storage for all non-phi opaque
/// values, phi storage is allocated. (Phi values are block arguments in which
/// phi's arguments are branch operands). This is handled by a
/// PhiStorageOptimizer that checks for interference among the phi operands and
/// reuses storage allocated to other values.
///
/// ## Step #3. Rewrite opaque values
///
/// In forward order (RPO), rewrite each opaque value definition, and all its
/// uses. This generally involves creating a new `_addr` variant of the
/// instruction and obtaining the storage address from the `valueStorageMap`.
///
/// If this value's storage is a def-projection (the value is used to compose an
/// aggregate), then first generate instructions to materialize the
/// projection. This is a recursive process starting with the root of the
/// projection path.
///
/// A projection path will be materialized once for the leaf subobject. When
/// this happens, the `storageAddress` will be assigned for any intermediate
/// projection paths. When those values are rewritten, their `storageAddress`
/// will already be available.
///
//===----------------------------------------------------------------------===//
///
/// TODO: Much of the implementation complexity, including most of the general
/// helper routines, stems from handling calls with multiple return values as
/// tuples. Once those calls are properly represented as instructions with
/// multiple results, then the implementation complexity will fall away. See the
/// code tagged "TODO: Multi-Result".
///
/// TODO: Some complexity stems from the SILPhiArgument type/opcode being used
/// for terminator results rather than phis.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "address-lowering"

#include "PhiStorageOptimizer.h"
#include "swift/AST/Decl.h"
#include "swift/Basic/BlotSetVector.h"
#include "swift/Basic/Range.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using llvm::SmallSetVector;

/// Get a function's convention for Lowered SIL, even though the SIL stage is
/// still Canonical.
static SILFunctionConventions getLoweredFnConv(SILFunction *function) {
  return SILFunctionConventions(
      function->getLoweredFunctionType(),
      SILModuleConventions::getLoweredAddressConventions(
          function->getModule()));
}

/// Get a call's function convention for Lowered SIL even though the SIL stage
/// is still Canonical.
static SILFunctionConventions getLoweredCallConv(ApplySite call) {
  return SILFunctionConventions(
      call.getSubstCalleeType(),
      SILModuleConventions::getLoweredAddressConventions(call.getModule()));
}

//===----------------------------------------------------------------------===//
//                                Multi-Result
//
// TODO: These helpers all compensate for the legacy representation of return
// values as tuples. Once calls are properly represented as multi-value
// instructions, this complexity all goes away.
//
// Calls are currently SILValues, but when the result type is a tuple, the call
// value does not represent a real value with storage. This is a bad situation
// for address lowering because there's no way to tell from any given value
// whether it's legal to assign storage to that value. As a result, the
// implementation of call lowering doesn't fall out naturally from the algorithm
// that lowers values to storage.
//===----------------------------------------------------------------------===//

/// If \p pseudoResult represents multiple results and at least one result is
/// used, then return the destructure.
static DestructureTupleInst *getCallDestructure(FullApplySite apply) {
  if (apply.getKind() == FullApplySiteKind::BeginApplyInst)
    return nullptr;
  if (apply.getSubstCalleeConv().getNumDirectSILResults() == 1)
    return nullptr;

  SILValue pseudoResult = apply.getResult();
  assert(pseudoResult->getType().is<TupleType>());
  if (auto *use = pseudoResult->getSingleUse())
    return cast<DestructureTupleInst>(use->getUser());

  assert(pseudoResult->use_empty()
         && "pseudo result can only be used by a single destructure_tuple");
  return nullptr;
}

/// \p destructure is the pseudo result of a multi-result call.
/// Visit all real call results. Stop when the visitor returns `false`.
static bool visitCallMultiResults(
    DestructureTupleInst *destructure, SILFunctionConventions fnConv,
    llvm::function_ref<bool(SILValue, SILResultInfo)> visitor) {
  assert(fnConv.getNumDirectSILResults() == destructure->getNumResults());

  auto resultIter = destructure->getAllResults().begin();
  for (auto resultInfo : fnConv.getDirectSILResults()) {
    if (!visitor(*resultIter++, resultInfo))
      return false;
  }
  return true;
}

/// Visit all real call results. Stop when the visitor returns `false`.
static bool
visitCallResults(FullApplySite apply,
                 llvm::function_ref<bool(SILValue, SILResultInfo)> visitor) {
  auto fnConv = apply.getSubstCalleeConv();
  if (auto *destructure = getCallDestructure(apply)) {
    return visitCallMultiResults(destructure, fnConv, visitor);
  }
  return visitor(apply.getResult(), *fnConv.getDirectSILResults().begin());
}

/// Return true if the given value is either a "fake" tuple that represents all
/// of a call's results or an empty tuple of no results. This may return true
/// for either an apply instruction or a block argument.
static bool isPseudoCallResult(SILValue value) {
  if (auto *apply = dyn_cast<ApplyInst>(value))
    return ApplySite(apply).getSubstCalleeConv().getNumDirectSILResults() > 1;

  auto *bbArg = dyn_cast<SILPhiArgument>(value);
  if (!bbArg)
    return false;

  auto *term = bbArg->getTerminatorForResult();
  if (!term)
    return false;

  auto *tryApply = dyn_cast<TryApplyInst>(term);
  if (!tryApply)
    return false;

  return ApplySite(tryApply).getSubstCalleeConv().getNumDirectSILResults() > 1;
}

/// Return true if this is a pseudo-return value.
static bool isPseudoReturnValue(SILValue value) {
  if (value->getFunction()->getConventions().getNumDirectSILResults() < 2)
    return false;

  if (auto *tuple = dyn_cast<TupleInst>(value)) {
    Operand *singleUse = tuple->getSingleUse();
    return singleUse && isa<ReturnInst>(singleUse->getUser());
  }
  return false;
}

/// Return the value representing storage of an address-only or indirectly
/// returned tuple element. For real tuples, return the tuple value itself. If
/// the tuple is a pseudo-return value, return the indirect function argument
/// for the corresponding result after lowering.
///
///   bb0(..., %loweredIndirectResult : $*T, ...)
///     ....
///     %tuple = tuple(..., %operand, ...)
///     return %tuple
///
///   When called on %operand, return %loweredIndirectResult.
///
/// Precondition: \p operand's user is a TupleInst
///
/// Precondition: indirect function arguments have already been rewritten
///               (see insertIndirectReturnArgs()).
static SILValue getTupleStorageValue(Operand *operand) {
  auto *tuple = cast<TupleInst>(operand->getUser());
  if (!isPseudoReturnValue(tuple))
    return tuple;

  unsigned resultIdx = tuple->getElementIndex(operand);

  auto *function = tuple->getFunction();
  auto loweredFnConv = getLoweredFnConv(function);
  assert(loweredFnConv.getResults().size() == tuple->getElements().size());

  unsigned indirectResultIdx = 0;
  for (SILResultInfo result : loweredFnConv.getResults().slice(0, resultIdx)) {
    if (loweredFnConv.isSILIndirect(result))
      ++indirectResultIdx;
  }
  // Cannot call function->getIndirectSILResults here because that API uses the
  // function conventions before address lowering.
  return function->getArguments()[indirectResultIdx];
}

/// Return the value representing storage for a single return value.
///
///   bb0(..., %loweredIndirectResult : $*T, ...) // function entry
///     return %oper
///
///   For %oper, return %loweredIndirectResult
static SILValue getSingleReturnAddress(Operand *operand) {
  assert(!isPseudoReturnValue(operand->get()));

  auto *function = operand->getParentFunction();
  assert(getLoweredFnConv(function).getNumIndirectSILResults() == 1);

  // Cannot call getIndirectSILResults here because that API uses the
  // function conventions before address lowering.
  return function->getArguments()[0];
}

//===----------------------------------------------------------------------===//
//                              ValueStorageMap
//
//              Map Opaque SILValues to abstract storage units.
//===----------------------------------------------------------------------===//

/// Check if this is a copy->store pair. If so, the copy storage will be
/// projected from the source, and the copy semantics will be handled by
/// UseRewriter::visitStoreInst.
static bool isStoreCopy(SILValue value) {
  auto *copyInst = dyn_cast<CopyValueInst>(value);
  if (!copyInst)
    return false;

  if (!copyInst->hasOneUse())
    return false;

  auto *user = value->getSingleUse()->getUser();
  auto *storeInst = dyn_cast<StoreInst>(user);
  if (!storeInst)
    return false;

  SSAPrunedLiveness liveness(copyInst->getFunction());
  auto isStoreOutOfRange = [&liveness, storeInst](SILValue root) {
    liveness.initializeDef(root);
    auto summary = liveness.computeSimple();
    if (summary.addressUseKind != AddressUseKind::NonEscaping) {
      return true;
    }
    if (summary.innerBorrowKind != InnerBorrowKind::Contained) {
      return true;
    }
    if (!liveness.isWithinBoundary(storeInst)) {
      return true;
    }
    return false;
  };

  auto source = copyInst->getOperand();
  if (source->getOwnershipKind() == OwnershipKind::Guaranteed) {
    // [in_guaranteed_begin_apply_results] If any root of the source is a
    // begin_apply, we can't rely on projecting from the (rewritten) source:
    // The store may not be in the coroutine's range. The result would be
    // attempting to access invalid storage.
    SmallVector<SILValue, 4> roots;
    findGuaranteedReferenceRoots(source, /*lookThroughNestedBorrows=*/true,
                                 roots);
    // TODO: Rather than checking whether the store is out of range of any
    // guaranteed root's SSAPrunedLiveness, instead check whether it is out of
    // range of ExtendedLiveness of the borrow introducers:
    // - visit borrow introducers via visitBorrowIntroducers
    // - call ExtendedLiveness.compute on each borrow introducer
    if (llvm::any_of(roots, [&](SILValue root) {
          // Handle forwarding phis conservatively rather than recursing.
          if (SILArgument::asPhi(root) && !BorrowedValue(root))
            return true;

          if (isa<BeginApplyInst>(root->getDefiningInstruction())) {
            return true;
          }
          return isStoreOutOfRange(root);
        })) {
      return false;
    }
  } else if (source->getOwnershipKind() == OwnershipKind::Owned) {
    if (isStoreOutOfRange(source)) {
      return false;
    }
  }

  return true;
}

void ValueStorageMap::insertValue(SILValue value, SILValue storageAddress) {
  assert(!stableStorage && "cannot grow stable storage map");

  auto hashResult =
      valueHashMap.insert(std::make_pair(value, valueVector.size()));
  (void)hashResult;
  assert(hashResult.second && "SILValue already mapped");

  valueVector.emplace_back(value, ValueStorage(storageAddress));
}

void ValueStorageMap::replaceValue(SILValue oldValue, SILValue newValue) {
  auto pos = valueHashMap.find(oldValue);
  assert(pos != valueHashMap.end());
  unsigned ordinal = pos->second;
  valueHashMap.erase(pos);

  auto hashResult = valueHashMap.insert(std::make_pair(newValue, ordinal));
  (void)hashResult;
  assert(hashResult.second && "SILValue already mapped");

  valueVector[ordinal].value = newValue;
}

#ifndef NDEBUG
void ValueStorageMap::dump() {
  llvm::dbgs() << "ValueStorageMap:\n";
  for (unsigned ordinal : indices(valueVector)) {
    auto &valStoragePair = valueVector[ordinal];
    llvm::dbgs() << "value: ";
    valStoragePair.value->dump();
    auto &storage = valStoragePair.storage;
    if (storage.isUseProjection) {
      llvm::dbgs() << "  use projection: ";
      if (!storage.isRewritten)
        valueVector[storage.projectedStorageID].value->dump();
    } else if (storage.isDefProjection) {
      llvm::dbgs() << "  def projection: ";
      if (!storage.isRewritten)
        valueVector[storage.projectedStorageID].value->dump();
    }
    if (storage.storageAddress) {
      llvm::dbgs() << "  storage: ";
      storage.storageAddress->dump();
    }
  }
}
#endif

//===----------------------------------------------------------------------===//
//                            AddressLoweringState
//
//            Shared state for the pass's analysis and transforms.
//===----------------------------------------------------------------------===//

namespace {
class PhiRewriter;

struct AddressLoweringState {
  SILFunction *function;
  SILFunctionConventions loweredFnConv;

  // Dominators remain valid throughout this pass.
  DominanceInfo *domInfo;

  // Dead-end blocks remain valid through this pass.
  DeadEndBlocks *deBlocks;

  InstructionDeleter deleter;

  // All opaque values mapped to their associated storage.
  ValueStorageMap valueStorageMap;

  // All call sites with formally indirect SILArgument or SILResult conventions.
  //
  // Applies with indirect results are removed as they are rewritten. Applies
  // with only indirect arguments are rewritten in a post-pass, only after all
  // parameters are rewritten.
  SmallBlotSetVector<FullApplySite, 16> indirectApplies;

  // unconditional_checked_cast instructions of loadable type which need to be
  // rewritten.
  SmallVector<UnconditionalCheckedCastInst *, 2> nonopaqueResultUCCs;

  // checked_cast_br instructions to loadable type which need to be rewritten.
  SmallVector<CheckedCastBranchInst *, 2> nonopaqueResultCCBs;

  // All function-exiting terminators (return or throw instructions).
  SmallVector<TermInst *, 8> exitingInsts;

  // All instructions that yield values to callees.
  TinyPtrVector<YieldInst *> yieldInsts;

  // Handle moves from a phi's operand storage to the phi storage.
  std::unique_ptr<PhiRewriter> phiRewriter;

  AddressLoweringState(SILFunction *function, DominanceInfo *domInfo,
                       DeadEndBlocks *deBlocks)
      : function(function), loweredFnConv(getLoweredFnConv(function)),
        domInfo(domInfo), deBlocks(deBlocks) {
    for (auto &block : *function) {
      if (block.getTerminator()->isFunctionExiting())
        exitingInsts.push_back(block.getTerminator());
    }
  }

  SILModule *getModule() const { return &function->getModule(); }

  SILLocation genLoc() const {
    return RegularLocation::getAutoGeneratedLocation();
  }

  // Get a builder that uses function conventions for the Lowered SIL stage even
  // though the SIL stage hasn't explicitly changed yet.
  SILBuilder getBuilder(SILBasicBlock::iterator insertPt) const {
    return getBuilder(insertPt, &*insertPt);
  }
  SILBuilder getTermBuilder(TermInst *term) const {
    return getBuilder(term->getParent()->end(), term);
  }

  PhiRewriter &getPhiRewriter();

  SILValue getMaterializedAddress(SILValue origValue) const {
    return valueStorageMap.getStorage(origValue).getMaterializedAddress();
  }

protected:
  SILBuilder getBuilder(SILBasicBlock::iterator insertPt,
                        SILInstruction *originalInst) const {
    SILBuilder builder(originalInst->getParent(), insertPt);
    builder.setSILConventions(
        SILModuleConventions::getLoweredAddressConventions(
            builder.getModule()));
    builder.setCurrentDebugScope(originalInst->getDebugScope());
    return builder;
  }

  void prepareBuilder(SILBuilder &builder) {
    builder.setSILConventions(
      SILModuleConventions::getLoweredAddressConventions(
        builder.getModule()));
  };
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                             OpaqueValueVisitor
//
//                     Map opaque values to ValueStorage.
//===----------------------------------------------------------------------===//

/// Before populating the ValueStorageMap, replace each value-typed argument to
/// the current function with an address-typed argument by inserting a temporary
/// load instruction.
static void convertDirectToIndirectFunctionArgs(AddressLoweringState &pass) {
  // Insert temporary argument loads at the top of the function.
  SILBuilder argBuilder =
      pass.getBuilder(pass.function->getEntryBlock()->begin());

  auto fnConv = pass.function->getConventions();
  unsigned argIdx = fnConv.getSILArgIndexOfFirstParam();
  for (SILParameterInfo param :
       pass.function->getLoweredFunctionType()->getParameters()) {

    if (param.isFormalIndirect() && !fnConv.isSILIndirect(param)) {
      SILArgument *arg = pass.function->getArgument(argIdx);
      SILType addrType = arg->getType().getAddressType();
      auto loc = SILValue(arg).getLoc();
      SILValue undefAddress = SILUndef::get(addrType, *pass.function);
      SingleValueInstruction *load;
      if (addrType.isTrivial(*pass.function)) {
        load = argBuilder.createLoad(loc, undefAddress,
                                     LoadOwnershipQualifier::Trivial);
      } else if (param.isConsumed()) {
        load = argBuilder.createLoad(loc, undefAddress,
                                     LoadOwnershipQualifier::Take);
      } else {
        load = argBuilder.createLoadBorrow(loc, undefAddress);
        for (SILInstruction *termInst : pass.exitingInsts) {
          pass.getBuilder(termInst->getIterator())
              .createEndBorrow(pass.genLoc(), load);
        }
      }
      arg->replaceAllUsesWith(load);
      assert(!pass.valueStorageMap.contains(arg));

      arg = arg->getParent()->replaceFunctionArgument(
          arg->getIndex(), addrType, OwnershipKind::None, arg->getDecl());

      assert(isa<LoadInst>(load) || isa<LoadBorrowInst>(load));
      load->setOperand(0, arg);

      // Indirect calling convention may be used for loadable types. In that
      // case, generating the argument loads is sufficient.
      if (addrType.isAddressOnly(*pass.function)) {
        pass.valueStorageMap.insertValue(load, arg);
      }
    }
    ++argIdx;
  }
  assert(argIdx ==
         fnConv.getSILArgIndexOfFirstParam() + fnConv.getNumSILArguments());
}

/// Before populating the ValueStorageMap, insert function arguments for any
/// @out result type. Return the number of indirect result arguments added.
static unsigned insertIndirectReturnArgs(AddressLoweringState &pass) {
  auto &astCtx = pass.getModule()->getASTContext();
  auto typeCtx = pass.function->getTypeExpansionContext();
  auto *declCtx = pass.function->getDeclContext();
  if (!declCtx) {
    // Fall back to using the module as the decl context if the function
    // doesn't have one.  The can happen with default argument getters, for
    // example.
    declCtx = pass.function->getModule().getSwiftModule();
  }

  unsigned argIdx = 0;
  for (auto resultTy : pass.loweredFnConv.getIndirectSILResultTypes(typeCtx)) {
    auto bodyResultTy = pass.function->mapTypeIntoContext(resultTy);
    auto var = new (astCtx) ParamDecl(
        SourceLoc(), SourceLoc(), astCtx.getIdentifier("$return_value"),
        SourceLoc(), astCtx.getIdentifier("$return_value"), declCtx);
    var->setSpecifier(ParamSpecifier::InOut);

    SILFunctionArgument *funcArg =
        pass.function->begin()->insertFunctionArgument(
            argIdx, bodyResultTy.getAddressType(), OwnershipKind::None, var);
    // Insert function results into valueStorageMap so that the caller storage
    // can be projected onto values inside the function as use projections.
    //
    // This is the only case where a value defines its own storage.
    pass.valueStorageMap.insertValue(funcArg, funcArg);

    ++argIdx;
  }
  assert(argIdx == pass.loweredFnConv.getNumIndirectSILResults());
  return argIdx;
}

namespace {
/// Collect all opaque/resilient values, inserting them in `valueStorageMap` in
/// RPO order.
///
/// Collect all call arguments with formally indirect SIL argument convention in
/// `indirectOperands` and formally indirect SIL results in `indirectResults`.
///
/// TODO: Perform linear-scan style in-place stack slot coloring by keeping
/// track of each value's last use.
class OpaqueValueVisitor {
  AddressLoweringState &pass;
  PostOrderFunctionInfo postorderInfo;

public:
  explicit OpaqueValueVisitor(AddressLoweringState &pass)
      : pass(pass), postorderInfo(pass.function) {}

  void mapValueStorage();

protected:
  void checkForIndirectApply(FullApplySite applySite);
  void visitValue(SILValue value);
  void canonicalizeReturnValues();
};
} // end anonymous namespace

/// Top-level entry. Populates AddressLoweringState's `valueStorageMap`,
/// `indirectApplies`, and `exitingInsts`.
///
/// Find all Opaque/Resilient SILValues and add them
/// to valueStorageMap in RPO.
void OpaqueValueVisitor::mapValueStorage() {
  for (auto *block : postorderInfo.getReversePostOrder()) {
    // Opaque function arguments have already been replaced.
    if (block != pass.function->getEntryBlock()) {
      for (auto *arg : block->getArguments()) {
        if (isPseudoCallResult(arg))
          continue;

        visitValue(arg);
      }
    }
    for (auto &inst : *block) {
      if (auto apply = FullApplySite::isa(&inst))
        checkForIndirectApply(apply);

      if (auto *yieldInst = dyn_cast<YieldInst>(&inst)) {
        pass.yieldInsts.push_back(yieldInst);
      }

      for (auto result : inst.getResults()) {
        if (isPseudoCallResult(result) || isPseudoReturnValue(result))
          continue;

        visitValue(result);
      }
    }
  }
  canonicalizeReturnValues();
}

/// Populate `indirectApplies`.
void OpaqueValueVisitor::checkForIndirectApply(FullApplySite applySite) {
  auto calleeConv = applySite.getSubstCalleeConv();
  unsigned calleeArgIdx = applySite.getCalleeArgIndexOfFirstAppliedArg();
  for (Operand &operand : applySite.getArgumentOperands()) {
    if (operand.get()->getType().isObject()) {
      auto argConv = calleeConv.getSILArgumentConvention(calleeArgIdx);
      if (argConv.isIndirectConvention()) {
        pass.indirectApplies.insert(applySite);
        return;
      }
    }
    ++calleeArgIdx;
  }

  if (applySite.getSubstCalleeType()->hasIndirectFormalResults() ||
      applySite.getSubstCalleeType()->hasIndirectFormalYields()) {
    pass.indirectApplies.insert(applySite);
  }
}

/// If `value` is address-only, add it to the `valueStorageMap`.
void OpaqueValueVisitor::visitValue(SILValue value) {
  if (!value->getType().isObject())
    return;
  if (!value->getType().isAddressOnly(*pass.function)) {
    if (auto *ucci = dyn_cast<UnconditionalCheckedCastInst>(value)) {
      if (ucci->getSourceLoweredType().isAddressOnly(*pass.function))
        return;
      if (!canIRGenUseScalarCheckedCastInstructions(
              pass.function->getModule(), ucci->getSourceFormalType(),
              ucci->getTargetFormalType())) {
        pass.nonopaqueResultUCCs.push_back(ucci);
      }
    } else if (auto *arg = dyn_cast<SILArgument>(value)) {
      if (auto *ccbi = dyn_cast_or_null<CheckedCastBranchInst>(
              arg->getTerminatorForResult())) {
        if (ccbi->getSuccessBB() != arg->getParent())
          return;
        if (ccbi->getSourceLoweredType().isAddressOnly(*pass.function))
          return;
        if (!canIRGenUseScalarCheckedCastInstructions(
                pass.function->getModule(), ccbi->getSourceFormalType(),
                ccbi->getTargetFormalType())) {
          pass.nonopaqueResultCCBs.push_back(ccbi);
        }
      }
    }
    return;
  }
  if (pass.valueStorageMap.contains(value)) {
    // Function arguments are already mapped from loads.
    assert(isa<SILFunctionArgument>(
        pass.valueStorageMap.getStorage(value).storageAddress));
    return;
  }
  pass.valueStorageMap.insertValue(value, SILValue());
}

// Canonicalize returned values. For multiple direct results, the operand of the
// return instruction must be a tuple with no other uses.
//
// Given $() -> @out (T, T):
//   %t = def  : $(T, T)
//   use %t    : $(T, T)
//   return %t : $(T, T)
//
// Produce:
//   %t = def
//   use %t    : $(T, T)
//   (%e0, %e1) = destructure_tuple %t : $(T, T)
//   %r = tuple (%e0 : $T, %e1 : $T)
//   return %r : $(T, T)
//
// TODO: Multi-Result. This should be a standard OSSA canonicalization until
// returns are fixed to take multiple operands.
void OpaqueValueVisitor::canonicalizeReturnValues() {
  auto numResults = pass.function->getConventions().getNumDirectSILResults();
  if (numResults < 2)
    return;

  for (SILInstruction *termInst : pass.exitingInsts) {
    auto *returnInst = dyn_cast<ReturnInst>(termInst);
    if (!returnInst) {
      assert(isa<ThrowInst>(termInst));
      continue;
    }
    SILValue oldResult = returnInst->getOperand();
    if (oldResult->getOwnershipKind() != OwnershipKind::Owned)
      continue;

    assert(oldResult->getType().is<TupleType>());
    if (oldResult->hasOneUse()) {
      assert(isPseudoReturnValue(oldResult));
      continue;
    }
    // There is another nonconsuming use of the returned tuple.
    SILBuilderWithScope returnBuilder(returnInst);
    auto loc = pass.genLoc();
    auto *destructure = returnBuilder.createDestructureTuple(loc, oldResult);

    SmallVector<SILValue, 4> results;
    results.reserve(numResults);
    for (auto result : destructure->getResults()) {
      // Update the value storage map for new instructions. Since they are
      // created at function exits, they are naturally in RPO order.
      this->visitValue(result);
      results.push_back(result);
    }
    auto *newResult = returnBuilder.createTuple(
        pass.genLoc(), oldResult->getType(), results, OwnershipKind::Owned);
    returnInst->setOperand(newResult);

    assert(isPseudoReturnValue(newResult));
  }
}

/// Top-level entry point.
///
/// Prepare the SIL by rewriting function arguments and returns.
/// Initialize the ValueStorageMap with an entry for each opaque value in the
/// function.
static void prepareValueStorage(AddressLoweringState &pass) {
  // Fixup this function's argument types with temporary loads.
  convertDirectToIndirectFunctionArgs(pass);

  // Create a new function argument for each indirect result.
  insertIndirectReturnArgs(pass);

  // Populate valueStorageMap.
  OpaqueValueVisitor(pass).mapValueStorage();
}

//===----------------------------------------------------------------------===//
//                             Storage Projection
//
// These queries determine whether storage for a SILValue can be projected from
// its operands or into its uses.
// ===---------------------------------------------------------------------===//

/// Return the operand whose source is an aggregate value that is extracted
/// into the given subobject, \p value. Or return nullptr.
///
/// Def-projection oracle: The answer must be consistent across both
/// OpaqueStorageAllocation and AddressMaterialization.
///
/// Invariant:
///   `getProjectedDefOperand(value) != nullptr`
/// if-and-only-if
///   `pass.valueStorageMap.getStorage(value).isDefProjection`
///
/// Invariant: if \p value has guaranteed ownership, this must return a nonnull
/// value.
static Operand *getProjectedDefOperand(SILValue value) {
  switch (value->getKind()) {
  default:
    return nullptr;

  case ValueKind::BeginBorrowInst:
    return &cast<BeginBorrowInst>(value)->getOperandRef();

  case ValueKind::CopyValueInst:
    if (isStoreCopy(value))
      return &cast<CopyValueInst>(value)->getOperandRef();

    return nullptr;

  case ValueKind::MultipleValueInstructionResult: {
    SILInstruction *destructure =
        cast<MultipleValueInstructionResult>(value)->getParent();
    switch (destructure->getKind()) {
    default:
      return nullptr;
    case SILInstructionKind::DestructureStructInst:
      return &destructure->getOperandRef(0);
    case SILInstructionKind::DestructureTupleInst: {
      auto *oper = &destructure->getOperandRef(0);
      if (isPseudoCallResult(oper->get()))
        return nullptr;

      return oper;
    }
    }
  }
  case ValueKind::TupleExtractInst: {
    auto *TEI = cast<TupleExtractInst>(value);
    // TODO: Multi-Result: TupleExtract from an apply are handled specially
    // until we have multi-result calls. Force them to allocate storage.
    if (ApplySite::isa(TEI->getOperand()))
      return nullptr;

    LLVM_FALLTHROUGH;
  }
  case ValueKind::StructExtractInst:
  case ValueKind::OpenExistentialValueInst:
  case ValueKind::OpenExistentialBoxValueInst:
    assert(value->getOwnershipKind() == OwnershipKind::Guaranteed);
    return &cast<SingleValueInstruction>(value)->getAllOperands()[0];
  }
}

/// If \p value is a an existential or enum, then return the existential or enum
/// operand. These operations are always rewritten by the UseRewriter and always
/// reuse the same storage as their operand. Note that if the operation's result
/// is address-only, then the operand must be address-only and therefore must
/// mapped to ValueStorage.
///
/// If \p value is an unchecked_bitwise_cast, then return the cast operand.
///
/// open_existential_value must reuse storage because the boxed value is shared
/// with other instances of the existential. An explicit copy is needed to
/// obtain an owned value.
///
/// unchecked_enum_data and switch_enum must reuse storage because extracting
/// the payload destroys the enum value.
static Operand *getReusedStorageOperand(SILValue value) {
  switch (value->getKind()) {
  default:
    break;

  case ValueKind::OpenExistentialValueInst:
  case ValueKind::OpenExistentialBoxValueInst:
  case ValueKind::UncheckedEnumDataInst:
  case ValueKind::UncheckedBitwiseCastInst:
    return &cast<SingleValueInstruction>(value)->getOperandRef(0);

  case ValueKind::SILPhiArgument: {
    if (auto *term = cast<SILPhiArgument>(value)->getTerminatorForResult()) {
      if (auto *switchEnum = dyn_cast<SwitchEnumInst>(term)) {
        return &switchEnum->getAllOperands()[0];
      }
      if (auto *checkedCastBr = dyn_cast<CheckedCastBranchInst>(term)) {
        if (value->getParentBlock() == checkedCastBr->getFailureBB()) {
          return &checkedCastBr->getAllOperands()[0];
        }
      }
    }
    break;
  }
  }
  return nullptr;
}

/// If \p operand can project into its user, return the SILValue representing
/// user's storage. The user may compose an aggregate from its operands or
/// forwards its operands to arguments.
///
/// TODO: Handle SwitchValueInst
static SILValue getProjectedUseValue(Operand *operand) {
  auto *user = operand->getUser();
  switch (user->getKind()) {
  default:
    break;

  // structs an enums are straightforward compositions.
  case SILInstructionKind::StructInst:
  case SILInstructionKind::EnumInst:
    return cast<SingleValueInstruction>(user);

  // init_existential_value composes an existential value, but may depends on
  // opened archetypes. The caller will need to check that storage dominates
  // the opened types.
  case SILInstructionKind::InitExistentialValueInst:
    return cast<SingleValueInstruction>(user);

  // A tuple is either a composition or forwards its element through a return
  // through function argument storage. Either way, its element can be a
  // use projection.
  case SILInstructionKind::TupleInst:
    return getTupleStorageValue(operand);

  // Return instructions can project into the return value.
  case SILInstructionKind::ReturnInst:
    return getSingleReturnAddress(operand);
  }
  return SILValue();
}

static bool doesNotNeedStackAllocation(SILValue value) {
  auto *defInst = value->getDefiningInstruction();
  if (!defInst)
    return false;
  // [in_guaranteed_begin_apply_results] OSSA ensures that every use of a
  // guaranteed value resulting from a begin_apply will occur in the
  // coroutine's range (i.e. "before" the end_apply/abort apply).
  // AddressLowering takes advantage of this lack of uses outside of the
  // coroutine's range to directly use the storage that is yielded by the
  // coroutine rather than moving it to local storage.
  //
  // It is, however, valid in OSSA to have uses of an owned value produced by a
  // begin_apply outside of the coroutine range.  So in that case, it is
  // necessary to introduce new storage and move to it.
  if (isa<LoadBorrowInst>(defInst) ||
      (isa<BeginApplyInst>(defInst) &&
       value->getOwnershipKind() == OwnershipKind::Guaranteed))
    return true;

  return false;
}
//===----------------------------------------------------------------------===//
//                          OpaqueStorageAllocation
//
// For each ValueStorage, first determine whether it can project out of its
// definition's storage or into the storage of a use. If so, record the
// projection information. Otherwise emit an alloc_stack for this storage root.
// ===---------------------------------------------------------------------===//

// Record a storage projection from the source of the given operand into its
// use (e.g. struct_extract, tuple_extract, switch_enum).
void ValueStorageMap::recordDefProjection(Operand *oper,
                                          SILValue projectedValue) {
  auto &storage = getStorage(projectedValue);
  storage.projectedStorageID = getOrdinal(oper->get());
  storage.isDefProjection = true;
}

// Mark this operand as coalesced with \p useValue storage.
void ValueStorageMap::recordComposingUseProjection(Operand *oper,
                                                   SILValue userValue) {
  auto &storage = getStorage(oper->get());
  assert(!storage.isAllocated());
  storage.projectedStorageID = getOrdinal(userValue);

  storage.projectedOperandNum = oper->getOperandNumber();
  assert(storage.projectedOperandNum == oper->getOperandNumber() &&
         "operand overflow");

  storage.isUseProjection = true;

  if (userValue->getType().getEnumOrBoundGenericEnum()) {
    storage.initializesEnum = true;
  }
  assert(!storage.isPhiProjection());
}

// Mark this phi operand as coalesced with the phi storage.
void ValueStorageMap::recordPhiUseProjection(Operand *operand,
                                             SILPhiArgument *phi) {
  assert(isa<BranchInst>(operand->getUser()));

  auto &storage = getStorage(operand->get());
  assert(!storage.isAllocated());
  assert(storage.projectedOperandNum == ValueStorage::InvalidOper);

  storage.projectedStorageID = getOrdinal(phi);
  storage.isUseProjection = true;

  assert(storage.isPhiProjection());
}

bool ValueStorageMap::isComposingUseProjection(Operand *oper) const {
  auto hashPos = valueHashMap.find(oper->get());
  if (hashPos == valueHashMap.end())
    return false;

  auto &srcStorage = valueVector[hashPos->second].storage;
  if (!srcStorage.isUseProjection)
    return false;

  return srcStorage.projectedOperandNum == oper->getOperandNumber();
}

namespace {
/// Allocate storage on the stack for every opaque value defined in this
/// function in postorder. If the definition is an argument of this function,
/// simply replace the function argument with an address representing the
/// caller's storage.
class OpaqueStorageAllocation {
  AddressLoweringState &pass;
  /// The alloc_stacks that have been created which eventually need to have
  /// corresponding dealloc_stacks created.
  ///
  /// Supports erasure because created alloc_stacks may be erased when block
  /// arguments are coalesced.
  SmallBlotSetVector<AllocStackInst *, 16> allocs;
  /// The alloc_stacks that have been created which eventually need to be
  /// positioned appropriately.
  ///
  /// The subset of allocs which aren't for opened existentials.
  InstructionSet allocsToReposition;

public:
  explicit OpaqueStorageAllocation(AddressLoweringState &pass)
      : pass(pass), allocsToReposition(pass.function) {}

  void allocateOpaqueStorage();
  /// Position alloc_stacks according to uses and create dealloc_stacks that
  /// jointly postdominate.
  void finalizeOpaqueStorage();

protected:
  void allocateValue(SILValue value);
  bool findProjectionIntoUseImpl(SILValue value,
                                 ArrayRef<SILValue> incomingValues,
                                 bool intoPhi);

  bool findValueProjectionIntoUse(SILValue value) {
    return findProjectionIntoUseImpl(value, ArrayRef<SILValue>(value), false);
  }

  bool findPhiProjectionIntoUse(SILValue value,
                                ArrayRef<SILValue> incomingValues) {
    return findProjectionIntoUseImpl(value, incomingValues, true);
  }

  bool checkStorageDominates(AllocStackInst *allocInst,
                             ArrayRef<SILValue> incomingValues);

  void allocatePhi(PhiValue phi);

  void removeAllocation(SILValue value);

  AllocStackInst *createStackAllocation(SILValue value);

  SILBasicBlock *getLeastCommonAncestorOfUses(SILValue value);

  void createStackAllocationStorage(SILValue value) {
    pass.valueStorageMap.getStorage(value).storageAddress =
        createStackAllocation(value);
  }
};
} // end anonymous namespace

/// Top-level entry point: allocate storage for all opaque/resilient values.
void OpaqueStorageAllocation::allocateOpaqueStorage() {
  // Create an AllocStack for every opaque value defined in the function.  Visit
  // values in post-order to create storage for aggregates before subobjects.
  for (auto &valueStorageI : llvm::reverse(pass.valueStorageMap)) {
    SILValue value = valueStorageI.value;
    if (!PhiValue(value))
      allocateValue(value);
  }
  // Only allocate phis after all SSA values have been allocated. allocatedValue
  // assumes SSA form without checking interference. At that point, multiple
  // SILValues can share storage via projections, but the storage is still
  // singly defined. However, allocatePhi may coalesce multiple values, or even
  // a single value across multiple loop iterations. The burden for checking
  // interference is entirely on allocatePhi.
  for (auto &valueStorageI : llvm::reverse(pass.valueStorageMap)) {
    if (auto phi = PhiValue(valueStorageI.value)) {
      allocatePhi(phi);
    }
  }
}

/// Allocate storage for a single opaque/resilient value.
void OpaqueStorageAllocation::allocateValue(SILValue value) {
  // Phis must be deferred.
  assert(!PhiValue(value));

  // Pseudo call results have no storage.
  assert(!isPseudoCallResult(value));

  // Pseudo return values have no storage.
  assert(!isPseudoReturnValue(value));

  auto &storage = pass.valueStorageMap.getStorage(value);

  // Fake loads for incoming function arguments are already rewritten; so are
  // outgoing function arguments.
  if (storage.isRewritten)
    return;

  // Function arguments are preallocated to fake loads, so they aren't mapped to
  // storage, and indirect results are already rewritten.
  assert(!isa<SILFunctionArgument>(value));

  assert(!storage.isAllocated());

  if (getReusedStorageOperand(value))
    return;

  if (doesNotNeedStackAllocation(value))
    return;

  // Check for values that inherently project storage from their operand.
  if (auto *storageOper = getProjectedDefOperand(value)) {
    pass.valueStorageMap.recordDefProjection(storageOper, value);
    return;
  }

  if (value->getOwnershipKind() == OwnershipKind::Guaranteed) {
    value->dump();
    llvm::report_fatal_error("^^^ guaranteed values must reuse storage");
  }

  // Attempt to reuse a user's storage.
  if (findValueProjectionIntoUse(value))
    return;

  // Eagerly create stack allocation. This way any operands can check
  // alloc_stack dominance before their storage is coalesced with this
  // value. Unfortunately, this alloc_stack may be dead if we later coalesce
  // this value's storage with a branch use.
  createStackAllocationStorage(value);
}

/// Find a use of \p value that can provide the value's storage.
///
/// \p incomingValues is a Range of SILValues (e.g. ArrayRef<SILValue>),
/// that all need \p value's storage to be available in their scope.
bool OpaqueStorageAllocation::findProjectionIntoUseImpl(
    SILValue value, ArrayRef<SILValue> incomingValues, bool intoPhi) {
  // Def-projections take precedence.
  assert(!getProjectedDefOperand(value) && !getReusedStorageOperand(value));

  for (Operand *use : value->getUses()) {
    // Get the user's value, whose storage we will project into.
    SILValue userValue = getProjectedUseValue(use);
    if (!userValue)
      continue;

    assert(!getProjectedDefOperand(userValue)
           && "storage cannot project in two directions.");

    // Avoid handling preposterous types.
    if (use->getOperandNumber() > UINT16_MAX)
      continue;

    // Recurse through all storage projections to find the uniquely allocated
    // storage. Enum storage cannot be reused across multiple subobjects because
    // it must be initialized via a single init_enum_data_addr instruction.
    //
    // TODO: fix the memory verifier to consider the actual store instructions
    // to initialize an enum rather than the init_enum_data_addr to reuse enum
    // storage across multiple subobjects within the payload.
    auto *baseStorage = pass.valueStorageMap.getBaseStorage(
        userValue, /*allowInitEnum*/ !intoPhi);
    if (!baseStorage)
      continue;

    if (auto *stackInst =
            dyn_cast<AllocStackInst>(baseStorage->storageAddress)) {
      if (!checkStorageDominates(stackInst, incomingValues))
        continue;
    } else
      assert(isa<SILFunctionArgument>(baseStorage->storageAddress));

    LLVM_DEBUG(llvm::dbgs() << "  PROJECT "; value->dump();
               llvm::dbgs() << "  into use "; use->getUser()->dump());

    pass.valueStorageMap.recordComposingUseProjection(use, userValue);
    return true;
  }
  return false;
}

bool OpaqueStorageAllocation::checkStorageDominates(
    AllocStackInst *allocInst, ArrayRef<SILValue> incomingValues) {

  for (SILValue incomingValue : incomingValues) {
    if (auto *defInst = incomingValue->getDefiningInstruction()) {
      if (!pass.domInfo->properlyDominates(allocInst, defInst))
        return false;
      continue;
    }
    // Handle both phis and terminator results.
    auto *bbArg = cast<SILPhiArgument>(incomingValue);
    // The storage block must strictly dominate the argument block.
    if (!pass.domInfo->properlyDominates(allocInst->getParent(),
                                         bbArg->getParent())) {
      return false;
    }
  }
  return true;
}

void OpaqueStorageAllocation::allocatePhi(PhiValue phi) {
  // Coalesces phi operand storage with the phi storage. The algorithm processes
  // all incoming values at once, so it is run when visiting the block argument.
  //
  // The phi operand projections are computed first to give them priority. Then
  // we determine if the phi itself can share storage with one of its users.
  CoalescedPhi coalescedPhi;
  coalescedPhi.coalesce(phi, pass.valueStorageMap);

  SmallVector<SILValue, 4> coalescedValues;
  coalescedValues.reserve(coalescedPhi.getCoalescedOperands().size());
  for (SILValue value : coalescedPhi.getCoalescedValues())
    coalescedValues.push_back(value);

  if (!findPhiProjectionIntoUse(phi, coalescedValues))
    createStackAllocationStorage(phi);

  // Regardless of whether we projected into a user or allocated storage,
  // provide this storage to all the incoming values that can reuse it.
  for (Operand *phiOper : coalescedPhi.getCoalescedOperands()) {
    removeAllocation(phiOper->get());
    pass.valueStorageMap.recordPhiUseProjection(phiOper,
                                                PhiOperand(phiOper).getValue());
  }
}

// Unfortunately, we create alloc_stack instructions for SSA values before
// coalescing block arguments. This temporary storage now needs to be removed.
void OpaqueStorageAllocation::removeAllocation(SILValue value) {
  auto &storage = pass.valueStorageMap.getStorage(value);
  auto *allocInst = cast<AllocStackInst>(storage.storageAddress);
  storage.storageAddress = nullptr;

  // It's only use should be dealloc_stacks.
  SmallVector<Operand *, 4> uses(allocInst->getUses());
  for (Operand *use : uses) {
    pass.deleter.forceDelete(cast<DeallocStackInst>(use->getUser()));
  }
  allocs.erase(allocInst);
  pass.deleter.forceDelete(allocInst);
}

SILBasicBlock *
OpaqueStorageAllocation::getLeastCommonAncestorOfUses(SILValue value) {
  SILBasicBlock *lca = nullptr;
  for (auto *use : value->getUses()) {
    auto *block = use->getParentBlock();
    lca = lca ? pass.domInfo->findNearestCommonDominator(lca, block) : block;
  }
  return lca;
}

// Create alloc_stack that dominates an owned value \p value. Create
// jointly-postdominating dealloc_stack instructions.  Nesting will be fixed
// later.
//
// Any value that may be used by a return instruction must be deallocated
// immediately before the return. This allows the return to be rewritten by
// loading from storage.
AllocStackInst *OpaqueStorageAllocation::createStackAllocation(SILValue value) {
  assert(value->getOwnershipKind() != OwnershipKind::Guaranteed &&
         "creating storage for a guaranteed value implies a copy");
  // Instructions that produce an opened type never reach here because they
  // have guaranteed ownership--they project their storage. We reach this
  // point after the opened value has been copied.
  assert((!value->getDefiningInstruction() ||
          !value->getDefiningInstruction()->definesLocalArchetypes())
         && "owned open_existential is unsupported");

  SILType allocTy = value->getType();

  // For opened existential types, allocate stack space at the type
  // definition. Allocating as early as possible provides more opportunity for
  // creating use projections into value. But allocation must be no earlier
  // then the latest type definition.
  SILInstruction *latestOpeningInst = nullptr;
  allocTy.getASTType().visit([&](CanType type) {
    auto archetype = dyn_cast<ArchetypeType>(type);
    if (!archetype)
      return;

    if (auto openedTy = getOpenedArchetypeOf(archetype)) {
      auto openingVal =
          pass.getModule()->getRootLocalArchetypeDef(openedTy, pass.function);

      auto *openingInst = openingVal->getDefiningInstruction();
      assert(openingVal && "all opened archetypes should be resolved");
      if (latestOpeningInst) {
        if (pass.domInfo->dominates(openingInst, latestOpeningInst))
          return;

        assert(pass.domInfo->dominates(latestOpeningInst, openingInst) &&
               "opened archetypes must dominate their uses");
      }
      latestOpeningInst = openingInst;
    }
  });

  auto allocPt = latestOpeningInst
                     ? latestOpeningInst->getNextInstruction()->getIterator()
                     : pass.function->getEntryBlock()->front().getIterator();
  auto allocBuilder = pass.getBuilder(allocPt);
  AllocStackInst *alloc = allocBuilder.createAllocStack(pass.genLoc(), allocTy);
  allocs.insert(alloc);
  if (latestOpeningInst == nullptr) {
    allocsToReposition.insert(alloc);
  }
  return alloc;
}

void OpaqueStorageAllocation::finalizeOpaqueStorage() {
  SmallVector<SILBasicBlock *, 4> boundary;
  for (auto maybeAlloc : allocs) {
    // An allocation may be erased when coalescing block arguments.
    if (!maybeAlloc.has_value())
      continue;

    auto *alloc = maybeAlloc.value();

    if (allocsToReposition.contains(alloc)) {
      auto allocPt = &*getLeastCommonAncestorOfUses(alloc)->begin();
      alloc->moveBefore(allocPt);
    }

    // Deallocate at the predecessors of dominance frontier blocks that are
    // dominated by the alloc to ensure that allocation encloses not only the
    // uses of the current value, but also of any values reusing this storage as
    // a use projection.
    computeDominatedBoundaryBlocks(alloc->getParent(), pass.domInfo, boundary);
    for (SILBasicBlock *deallocBlock : boundary) {
      if (pass.deBlocks->isDeadEnd(deallocBlock))
        continue;
      auto deallocBuilder = pass.getBuilder(deallocBlock->back().getIterator());
      deallocBuilder.createDeallocStack(pass.genLoc(), alloc);
    }
    boundary.clear();
  }
}

//===----------------------------------------------------------------------===//
//                           AddressMaterialization
//
//            Materialize storage addresses, generate projections.
//===----------------------------------------------------------------------===//

namespace {
/// Materialize the address of a value's storage. For values that are directly
/// mapped to a storage location, return the mapped `AllocStackInst`.  For
/// subobjects emit any necessary `_addr` projections using the provided
/// `SILBuilder`.
///
/// This is a common utility for PhiRewriter, CallArgRewriter, ApplyRewriter,
/// ReturnRewriter, UseRewriter, and DefRewriter.
class AddressMaterialization {
  AddressLoweringState &pass;
  SILBuilder &B;

public:
  AddressMaterialization(AddressLoweringState &pass, SILBuilder &B)
      : pass(pass), B(B) {}

  /// Return the address of the storage for `origValue`. This may involve
  /// materializing projections. Record the materialized address as storage for
  /// origValue. Called once at the definition of \p origValue.
  SILValue materializeAddress(SILValue origValue) {
    ValueStorage &storage = pass.valueStorageMap.getStorage(origValue);
    if (storage.storageAddress)
      return storage.storageAddress;

    if (storage.isUseProjection) {
      recursivelyMaterializeStorage(storage, /*intoPhiOperand*/ false);
    } else {
      assert(storage.isDefProjection);
      storage.storageAddress = materializeDefProjection(origValue);
    }
    return storage.storageAddress;
  }

  void initializeComposingUse(Operand *operand);

  SILValue recursivelyMaterializeStorage(ValueStorage &storage,
                                         bool intoPhiOperand);

  SILValue materializeDefProjection(SILValue origValue);

protected:
  SILValue materializeStructExtract(SILInstruction *extractInst,
                                    SILValue elementValue, unsigned fieldIdx);

  SILValue materializeTupleExtract(SILInstruction *extractInst,
                                   SILValue elementValue, unsigned fieldIdx);

  SILValue materializeProjectionIntoUse(Operand *operand, bool intoPhiOperand);

  SILValue materializeComposingUser(SingleValueInstruction *user,
                                    bool intoPhiOperand) {
    return recursivelyMaterializeStorage(pass.valueStorageMap.getStorage(user),
                                         intoPhiOperand);
  }
};
} // anonymous namespace

/// Given the operand of an aggregate instruction (struct, tuple, enum), ensure
/// that the in-memory subobject is initialized. Generates an address
/// projection and copy if needed.
///
/// If the operand projects into its use, then the memory was already
/// initialized when visiting the use.
///
/// It's ok for the builder to reuse the user's SILLocation because
/// initializeComposingUse always inserts code immediately before the user.
void AddressMaterialization::initializeComposingUse(Operand *operand) {
  SILValue def = operand->get();
  if (def->getType().isAddressOnly(*pass.function)) {
    ValueStorage &storage = pass.valueStorageMap.getStorage(def);
    assert(storage.isRewritten && "Source value should be rewritten");

    if (storage.isUseProjection)
      return;

    auto destAddr =
        materializeProjectionIntoUse(operand, /*intoPhiOperand*/ false);
    B.createCopyAddr(operand->getUser()->getLoc(), storage.storageAddress,
                     destAddr, IsTake, IsInitialization);
    return;
  }
  SILValue destAddr = materializeProjectionIntoUse(operand,
                                                   /*intoPhiOperand*/ false);
  B.createTrivialStoreOr(operand->getUser()->getLoc(), operand->get(), destAddr,
                         StoreOwnershipQualifier::Init);
}

// Recursively materialize the address for storage at the point that an operand
// may project into it via either a composing-use (struct, tuple, enum) or phi
// projection.
//
// Precondition: \p storage is not a def-projection.
//
// If \p intoPhiOperand is true, this materializes the address in the path that
// reaches a phi operand, not the phi block itself. Do not map the storage onto
// the materialized address.
//
// If \p intoPhiOperand is false, then the materialized address is guaranteed to
// dominate the composing user. Map the user onto this address to avoid
// rematerialization.
//
// Note: This only materializes the address for the purpose of projection an
// operand into the storage. It does not materialize the final address of
// storage after materializing the result. In particular, it materializes
// init_enum_data_addr, but not inject_enum_addr.
//
SILValue AddressMaterialization::recursivelyMaterializeStorage(
    ValueStorage &storage, bool intoPhiOperand = false) {
  // If this storage is already materialized, then simply return its
  // address. This not only avoids redundant projections, but is necessary for
  // correctness when emitting init_enum_data_addr.
  if (!intoPhiOperand && storage.storageAddress)
    return storage.storageAddress;

  auto recordAddress = [&](SILValue address) {
    if (!intoPhiOperand)
      storage.storageAddress = address;
    return address;
  };
  if (storage.isComposingUseProjection()) {
    // Handle chains of composing users.
    auto &useStorage = pass.valueStorageMap.getProjectedStorage(storage);
    SILValue useVal = useStorage.value;
    if (auto *defInst = useVal->getDefiningInstruction()) {
      Operand *useOper =
          &defInst->getAllOperands()[storage.projectedOperandNum];
      return recordAddress(
          materializeProjectionIntoUse(useOper, intoPhiOperand));
    }
    // For indirect function results, projectedOperandNum is the index into
    // the tuple of opaque results, which isn't useful here.
    assert(isa<SILFunctionArgument>(useVal) && useStorage.storage.isRewritten);
    return recordAddress(useStorage.storage.storageAddress);
  }
  if (storage.isPhiProjection()) {
    return recordAddress(recursivelyMaterializeStorage(
        pass.valueStorageMap.getProjectedStorage(storage).storage,
        /*intoPhiOperand*/ true));
  }
  assert(!storage.isProjection() &&
         "a composing user may not also be a def projection");
  return storage.storageAddress;
}

/// Materialize the address of a subobject.
///
/// \param origValue is a value associated with the subobject storage. It is
/// either a SingleValueInstruction projection or a terminator result.
SILValue AddressMaterialization::materializeDefProjection(SILValue origValue) {
  switch (origValue->getKind()) {
  default:
    llvm_unreachable("Unexpected projection from def.");

  case ValueKind::CopyValueInst:
    assert(isStoreCopy(origValue));
    return pass.getMaterializedAddress(
        cast<CopyValueInst>(origValue)->getOperand());

  case ValueKind::MultipleValueInstructionResult: {
    auto *result = cast<MultipleValueInstructionResult>(origValue);
    SILInstruction *destructure = result->getParent();
    switch (destructure->getKind()) {
    default:
      llvm_unreachable("Unexpected projection from def.");

    case SILInstructionKind::DestructureStructInst: {
      return materializeStructExtract(destructure, origValue,
                                      result->getIndex());
      break;
    }
    case SILInstructionKind::DestructureTupleInst: {
      return materializeTupleExtract(destructure, origValue,
                                     result->getIndex());
      break;
    }
    }
  }
  case ValueKind::StructExtractInst: {
    auto *extractInst = cast<StructExtractInst>(origValue);
    return materializeStructExtract(extractInst, origValue,
                                    extractInst->getFieldIndex());
  }
  case ValueKind::TupleExtractInst: {
    auto *extractInst = cast<TupleExtractInst>(origValue);
    return materializeTupleExtract(extractInst, origValue,
                                   extractInst->getFieldIndex());
  }
  case ValueKind::SILPhiArgument: {
    // Handle this in the caller. unchecked_take_enum_data_addr is
    // destructive. It cannot be materialized on demand.
    llvm_unreachable("Unimplemented switch_enum optimization");
  }
  }
}

// \p structInst is a unary instruction whose first operand is a struct.
SILValue AddressMaterialization::materializeStructExtract(
    SILInstruction *extractInst, SILValue elementValue, unsigned fieldIdx) {
  auto structVal = extractInst->getOperand(0);
  SILValue srcAddr = pass.getMaterializedAddress(structVal);
  auto *structType = structVal->getType().getStructOrBoundGenericStruct();
  auto *varDecl = structType->getStoredProperties()[fieldIdx];
  return B.createStructElementAddr(pass.genLoc(), srcAddr, varDecl,
                                   elementValue->getType().getAddressType());
}

// \p tupleInst is a unary instruction whose first operand is a tuple.
SILValue AddressMaterialization::materializeTupleExtract(
    SILInstruction *extractInst, SILValue elementValue, unsigned fieldIdx) {
  SILValue srcAddr = pass.getMaterializedAddress(extractInst->getOperand(0));
  return B.createTupleElementAddr(pass.genLoc(), srcAddr, fieldIdx,
                                  elementValue->getType().getAddressType());
}

/// Recursively materialize the address of a subobject that is a member of the
/// operand's user. The operand's user must be an aggregate struct, tuple, enum,
/// init_existential_value.
SILValue
AddressMaterialization::materializeProjectionIntoUse(Operand *operand,
                                                     bool intoPhiOperand) {
  SILInstruction *user = operand->getUser();
  switch (user->getKind()) {
  default:
    LLVM_DEBUG(user->dump());
    llvm_unreachable("Unexpected projection from use.");
  case SILInstructionKind::EnumInst: {
    auto *enumInst = cast<EnumInst>(user);
    SILValue enumAddr = materializeComposingUser(enumInst, intoPhiOperand);
    return B.createInitEnumDataAddr(pass.genLoc(), enumAddr,
                                    enumInst->getElement(),
                                    operand->get()->getType().getAddressType());
  }
  case SILInstructionKind::InitExistentialValueInst: {
    auto *initExistentialValue = cast<InitExistentialValueInst>(user);
    SILValue containerAddr =
        materializeComposingUser(initExistentialValue, intoPhiOperand);
    auto canTy = initExistentialValue->getFormalConcreteType();
    auto opaque = Lowering::AbstractionPattern::getOpaque();
    auto &concreteTL = pass.function->getTypeLowering(opaque, canTy);
    return B.createInitExistentialAddr(pass.genLoc(), containerAddr, canTy,
                                       concreteTL.getLoweredType(),
                                       initExistentialValue->getConformances());
  }
  case SILInstructionKind::StructInst: {
    auto *structInst = cast<StructInst>(user);

    auto fieldIter = structInst->getStructDecl()->getStoredProperties().begin();
    std::advance(fieldIter, operand->getOperandNumber());

    SILValue structAddr = materializeComposingUser(structInst, intoPhiOperand);
    return B.createStructElementAddr(
        pass.genLoc(), structAddr, *fieldIter,
        operand->get()->getType().getAddressType());
  }
  case SILInstructionKind::TupleInst: {
    auto *tupleInst = cast<TupleInst>(user);
    if (isPseudoReturnValue(tupleInst)) {
      unsigned resultIdx = tupleInst->getElementIndex(operand);
      assert(resultIdx < pass.loweredFnConv.getNumIndirectSILResults());
      // Cannot call getIndirectSILResults here because that API uses the
      // original function type.
      return pass.function->getArguments()[resultIdx];
    }
    SILValue tupleAddr = materializeComposingUser(tupleInst, intoPhiOperand);
    return B.createTupleElementAddr(pass.genLoc(), tupleAddr,
                                    operand->getOperandNumber(),
                                    operand->get()->getType().getAddressType());
  }
  }
}

//===----------------------------------------------------------------------===//
//                              PhiRewriter
//
// Insert moves on CFG edges to break phi operand interferences.
//===----------------------------------------------------------------------===//

namespace {

// To materialize a phi operand in the corresponding phi predecessor block:
//
// 1. Materialize the phi address. If the phi projects into a use, this requires
// initialization of the user's storage in each predecessor.
//
// 2. If the phi operand is not coalesced, then move the operand into the
// materialized phi address.
//
// For blocks with multiple phis, all moves of phi operands semantically occur
// in parallel on the CFG edge from the predecessor to the phi block. As these
// moves are inserted into the predecessor's instruction list, maintain the
// illusion of parallel moves by resolving any interference between the phi
// moves. This is done by checking for anti-dependencies to or from other phi
// moves. If one phi move's source reads from another phi move's dest, then the
// read must occur before the write.
//
// Insert a second move to break an anti-dependence cycle when both the source
// and destination of the new phi interferes with other phis (the classic
// phi-swap problem).
//
// Input:
//     addr0 = alloc_stack // storage for val0
//     addr1 = alloc_stack // storage for val1
//   bb1:
//     br bb3(val0, val1)
//   bb2:
//     br bb3(val1, val0)
//   bb3(phi0, phi1):
//
// Output:
//
//   bb1:
//     br bb3(val0, val1)
//   bb2:
//     temp = alloc_stack
//     copy_addr [take] addr0 to [init] temp
//     copy_addr [take] addr1 to [init] addr0
//     copy_addr [take] temp to [init] addr1
//     dealloc_stack temp
//     br bb3(val1, val1)
//   bb3(phi0, phi1):
class PhiRewriter {
  AddressLoweringState &pass;

  // A set of moves from a phi operand storage to phi storage. These logically
  // occur on the CFG edge. Keep track of them to resolve anti-dependencies.
  SmallPtrSet<CopyAddrInst *, 16> phiMoves;

public:
  PhiRewriter(AddressLoweringState &pass) : pass(pass) {}

  void materializeOperand(PhiOperand phiOperand);

protected:
  PhiRewriter(const PhiRewriter &) = delete;
  PhiRewriter &operator=(const PhiRewriter &) = delete;

  CopyAddrInst *createPhiMove(SILBuilder &builder, SILValue from, SILValue to) {
    auto *move = builder.createCopyAddr(pass.genLoc(), from, to, IsTake,
                                        IsInitialization);
    phiMoves.insert(move);
    return move;
  }

  struct MovePosition {
    SILBasicBlock::iterator latestMovePos;
    bool foundAntiDependenceCycle = false;
  };
  MovePosition findPhiMovePosition(PhiOperand phiOper);
};
} // anonymous namespace

void PhiRewriter::materializeOperand(PhiOperand phiOper) {
  auto &operStorage =
      pass.valueStorageMap.getStorage(phiOper.getOperand()->get());
  if (operStorage.isPhiProjection()) {
    if (operStorage.projectedStorageID ==
        pass.valueStorageMap.getOrdinal(phiOper.getValue())) {
      // This operand was coalesced with this particular phi. No move needed.
      return;
    }
  }
  auto phiOperAddress = operStorage.getMaterializedAddress();

  auto movePos = findPhiMovePosition(phiOper);

  auto builder = pass.getBuilder(movePos.latestMovePos);
  AddressMaterialization addrMat(pass, builder);

  auto &phiStorage = pass.valueStorageMap.getStorage(phiOper.getValue());
  SILValue phiAddress =
      addrMat.recursivelyMaterializeStorage(phiStorage,
                                            /*intoPhiOperand*/ true);

  if (!movePos.foundAntiDependenceCycle) {
    createPhiMove(builder, phiOperAddress, phiAddress);
    return;
  }
  AllocStackInst *alloc =
      builder.createAllocStack(pass.genLoc(), phiOper.getValue()->getType());
  createPhiMove(builder, phiOperAddress, alloc);

  auto tempBuilder = pass.getBuilder(phiOper.getBranch()->getIterator());
  createPhiMove(tempBuilder, alloc, phiAddress);
  tempBuilder.createDeallocStack(pass.genLoc(), alloc);
}

PhiRewriter &AddressLoweringState::getPhiRewriter() {
  if (!this->phiRewriter) {
    this->phiRewriter = std::make_unique<PhiRewriter>(*this);
  }
  return *(this->phiRewriter.get());
}

// Return the latest position at which a move into this phi may be emitted
// without violating an anti-dependence on another phi move.
PhiRewriter::MovePosition PhiRewriter::findPhiMovePosition(PhiOperand phiOper) {
  auto phiBaseAddress =
      pass.valueStorageMap.getBaseStorage(phiOper.getValue()).storageAddress;

  auto operBaseAddress =
      pass.valueStorageMap.getBaseStorage(phiOper.getOperand()->get())
          .storageAddress;

  auto insertPt = phiOper.getBranch()->getIterator();
  bool foundEarliestInsertPoint = false;

  MovePosition movePos;
  movePos.latestMovePos = insertPt;

  // Continue scanning until all phi moves have been checked for interference.
  for (auto beginIter = phiOper.predBlock->begin(); insertPt != beginIter;) {
    --insertPt;

    auto *phiMove = dyn_cast<CopyAddrInst>(&*insertPt);
    if (!phiMove || !phiMoves.contains(phiMove))
      break;

    if (!foundEarliestInsertPoint &&
        getAccessBase(phiMove->getSrc()) == phiBaseAddress) {
      // Anti-dependence from the phi move to the phi value. Do not move into
      // the phi storage before this point.
      foundEarliestInsertPoint = true;
    }
    if (getAccessBase(phiMove->getDest()) == operBaseAddress) {
      // Anti-dependence from the phi operand to the phi move. Do not move out
      // of the operand storage after this point.
      movePos.latestMovePos = insertPt;
      // If the earliest and latest points conflict, allocate a temporary.
      if (foundEarliestInsertPoint) {
        movePos.foundAntiDependenceCycle = true;
      }
    }
  }
  return movePos;
}

//===----------------------------------------------------------------------===//
//                              CallArgRewriter
//
//  Rewrite call arguments for indirect parameters.
//===----------------------------------------------------------------------===//

namespace {
/// This rewrites one parameter at a time, replacing the incoming
/// object arguments with address-type arguments.
class CallArgRewriter {
  AddressLoweringState &pass;
  ApplySite apply;
  SILLocation callLoc;
  SILBuilder argBuilder;
  AddressMaterialization addrMat;

public:
  CallArgRewriter(ApplySite apply, AddressLoweringState &pass)
      : pass(pass), apply(apply), callLoc(apply.getLoc()),
        argBuilder(pass.getBuilder(apply.getInstruction()->getIterator())),
        addrMat(pass, argBuilder) {}

  bool rewriteArguments();

  void rewriteIndirectArgument(Operand *operand);
};
} // end anonymous namespace

/// Rewrite all incoming indirect arguments in place without modifying the call.
bool CallArgRewriter::rewriteArguments() {
  bool changed = false;

  auto origConv = apply.getSubstCalleeConv();
  assert(apply.getNumArguments() == origConv.getNumParameters() &&
         "results should not yet be rewritten");

  for (unsigned argIdx = apply.getCalleeArgIndexOfFirstAppliedArg(),
                endArgIdx = argIdx + apply.getNumArguments();
       argIdx < endArgIdx; ++argIdx) {

    Operand &operand = apply.getArgumentRef(argIdx);
    // Ignore arguments that have already been rewritten with an address.
    if (operand.get()->getType().isAddress())
      continue;

    auto argConv = apply.getSubstCalleeConv().getSILArgumentConvention(argIdx);
    if (argConv.isIndirectConvention()) {
      rewriteIndirectArgument(&operand);
      changed |= true;
    }
  }
  return changed;
}

/// Rewrite a formally indirect argument in place.
/// Update the operand to the incoming value's storage address.
/// After this, the SIL argument types no longer match SIL function conventions.
///
/// Temporary argument storage may be created for loadable values.
void CallArgRewriter::rewriteIndirectArgument(Operand *operand) {
  SILValue argValue = operand->get();

  if (argValue->getType().isAddressOnly(*pass.function)) {
    ValueStorage &storage = pass.valueStorageMap.getStorage(argValue);
    assert(storage.isRewritten && "arg source should be rewritten");
    operand->set(storage.storageAddress);
    return;
  }
  // Allocate temporary storage for a loadable operand.
  AllocStackInst *allocInst =
      argBuilder.createAllocStack(callLoc, argValue->getType());
  if (apply.getArgumentConvention(*operand).isOwnedConvention()) {
    argBuilder.createTrivialStoreOr(apply.getLoc(), argValue, allocInst,
                                    StoreOwnershipQualifier::Init);
    apply.insertAfterApplication([&](SILBuilder &callBuilder) {
      callBuilder.createDeallocStack(callLoc, allocInst);
    });
    operand->set(allocInst);
  } else {
    auto borrow = argBuilder.emitBeginBorrowOperation(callLoc, argValue);
    auto *store =
        argBuilder.emitStoreBorrowOperation(callLoc, borrow, allocInst);
    auto *storeBorrow = dyn_cast<StoreBorrowInst>(store);
    apply.insertAfterApplication([&](SILBuilder &callBuilder) {
      if (storeBorrow) {
        callBuilder.emitEndBorrowOperation(callLoc, storeBorrow);
      }
      if (borrow != argValue) {
        callBuilder.emitEndBorrowOperation(callLoc, borrow);
      }
      callBuilder.createDeallocStack(callLoc, allocInst);
    });
    if (storeBorrow) {
      operand->set(storeBorrow);
    } else {
      operand->set(allocInst);
    }
  }
}

//===----------------------------------------------------------------------===//
//                               ApplyRewriter
//
//                 Rewrite call sites with indirect results.
// ===---------------------------------------------------------------------===//

namespace {
/// Once any result needs to be rewritten, then the entire apply is
/// replaced. Creates new indirect result arguments for this function to
/// represent the caller's storage.
///
/// TODO: Multi-Result - this is complicated because calls are not properly
/// represented as multi-value instructions.
class ApplyRewriter {
  AddressLoweringState &pass;

  // This apply site mutates when the new apply instruction is generated.
  FullApplySite apply;
  SILLocation callLoc;

  // For building incoming arguments and materializing addresses.
  SILBuilder argBuilder;

  // For loading results.
  SILBuilder resultBuilder;

  AddressMaterialization addrMat;
  SILFunctionConventions opaqueCalleeConv;
  SILFunctionConventions loweredCalleeConv;

public:
  ApplyRewriter(FullApplySite oldCall, AddressLoweringState &pass)
      : pass(pass), apply(oldCall), callLoc(oldCall.getLoc()),
        argBuilder(pass.getBuilder(oldCall.getInstruction()->getIterator())),
        resultBuilder(pass.getBuilder(getCallResultInsertionPoint())),
        addrMat(pass, argBuilder),
        opaqueCalleeConv(oldCall.getSubstCalleeConv()),
        loweredCalleeConv(getLoweredCallConv(oldCall)) {}

  void convertApplyWithIndirectResults();
  void convertBeginApplyWithOpaqueYield();

protected:
  SILBasicBlock::iterator getCallResultInsertionPoint() {
    if (isa<ApplyInst>(apply) || isa<BeginApplyInst>(apply))
      return std::next(SILBasicBlock::iterator(apply.getInstruction()));

    auto *bb = cast<TryApplyInst>(apply)->getNormalBB();
    return bb->begin();
  }

  void makeIndirectArgs(MutableArrayRef<SILValue> newCallArgs);

  SILBasicBlock::iterator getResultInsertionPoint();

  SILValue materializeIndirectResultAddress(SILValue oldResult, SILType argTy);

  void rewriteApply(ArrayRef<SILValue> newCallArgs);

  void rewriteTryApply(ArrayRef<SILValue> newCallArgs);

  void replaceDirectResults(DestructureTupleInst *oldDestructure);
};
} // end anonymous namespace

/// Top-level entry: Allocate storage for formally indirect results at a call
/// site. Create a new apply instruction with indirect SIL arguments.  The
/// original apply instruction remains in place, unless it is a try_apply.
///
/// Input (T = address-only, L=Loadable):
///
///   %addr = alloc_stack $T                    // storage for %oldResult
///   ...
///   %oldResult = apply : $() -> @out T
///
/// Output:
///
///   %addr = alloc_stack $T                    // storage for %oldResult
///   ...
///   %newCall   = apply(%addr) : $() -> @out T // no uses
///   %oldResult = apply() : $() -> @out T      // original apply
///
/// Input:
///
///   %result = apply : $() -> @out L
///
/// Output:
///
///   %addr = alloc_stack $L                  // unmapped temp storage
///   %newCall = apply(%addr) : $() -> @out L // no uses
///   %oldCall = apply() : $() -> @out L      // original apply, no uses
///   %result = load %addr : $*L
///   dealloc_stack %addr
///
/// Input:
///
///   %addr0 = alloc_stack $T                 // storage for %result0
///   ...
///   %tuple = apply : $() -> (@out T, @out L, L)
///   (%r0, %r1, %r2) = destructure_tuple %tuple : $(T, T, T)
///
/// Output:
///
///   %addr0 = alloc_stack $T                 // storage for %r0
///   ...
///   %addr1   = alloc_stack                    // unmapped temp storage
///   %r2      = apply(%addr0, %addr1) : $() -> (@out T, @out L, L)
///   %oldCall = apply() : $() -> (@out T, @out L, L)
///   %r1      = load %addr1 : $*L
///   (%r0, %d1, %d2) = destructure_tuple %tuple : $(T, T, T)
///   // no uses of %d1, %d2
///
void ApplyRewriter::convertApplyWithIndirectResults() {
  // Gather information from the old apply before rewriting it and mutating
  // this->apply.

  // Avoid revisiting this apply.
  bool erased = pass.indirectApplies.erase(apply);
  assert(erased && "all results should be rewritten at the same time");
  (void)erased;

  // List of new call arguments.
  SmallVector<SILValue, 8> newCallArgs(loweredCalleeConv.getNumSILArguments());

  // Materialize and map the address of each opaque indirect result, possibly
  // creating alloc_stacks.
  //
  // Create a load for each loadable indirect result.
  //
  // Populate newCallArgs.
  makeIndirectArgs(newCallArgs);

  // Record the original result destructure before deleting a try_apply.
  auto *destructure = getCallDestructure(apply);

  switch (apply.getKind()) {
  case FullApplySiteKind::ApplyInst: {
    // this->apply will be updated with the new apply instruction.
    rewriteApply(newCallArgs);
    break;
  }
  case FullApplySiteKind::TryApplyInst: {
    // this->apply will be updated with the new try_apply instruction.
    rewriteTryApply(newCallArgs);
    break;
  }
  case FullApplySiteKind::BeginApplyInst:
    // BeginApply does not need to be rewritten. It's argument list is not
    // polluted with indirect results.
    break;
  };

  // Replace all results of the original call that remain direct. ApplyRewriter
  // is only used when at least one result is indirect. So any direct results
  // require a destructure.
  if (destructure) {
    replaceDirectResults(destructure);
  }
}

// Populate \p newCallArgs with the new call instruction's SIL argument list.
// Materialize temporary storage for loadable indirect results.
//
// Input (T = address-only, L=Loadable):
//
//   %addr = alloc_stack $T                    // storage for %oldResult
//   ...
//   %oldResult = apply : $() -> @out T
//
// Output (newCallArgs = [%addr]):
//
// Input:
//
//   %result = apply : $() -> @out L
//
// Output (newCallArgs = [%addr]):
//
//   %addr = alloc_stack $L                    // unmapped temp storage
//   %oldCall = apply() : $() -> @out L        // no uses
//   %result = load %addr : $*L
//   dealloc_stack %addr
//
// Input:
//
//   %addr0 = alloc_stack $T                   // storage for %r0
//   ...
//   %tuple  = apply : $() -> (@out T, @out L, L)
//   (%r0, %r1, %r2) = destructure_tuple %tuple : $(T, L, L)
//
// Output (newCallArgs = [%addr0, %addr1]):
//
//   %addr0 = alloc_stack $T                   // storage for %r0
//   ...
//   %addr1 = alloc_stack                      // unmapped temp storage
//   %tuple = apply() : $() -> (@out T, @out L, L)
//   %r1    = load %addr1 : $*L
//   dealloc_stack %addr1
//   (%r0, %d1, %r2) = destructure_tuple %tuple : $(T, L, L)
//   // no uses of %d1
//
void ApplyRewriter::makeIndirectArgs(MutableArrayRef<SILValue> newCallArgs) {

  auto typeCtx = pass.function->getTypeExpansionContext();

  // The index of the next indirect result argument.
  unsigned newResultArgIdx =
      loweredCalleeConv.getSILArgIndexOfFirstIndirectResult();

  auto visitCallResult = [&](SILValue result, SILResultInfo resultInfo) {
    assert(!opaqueCalleeConv.isSILIndirect(resultInfo) &&
           "canonical call results are always direct");

    if (loweredCalleeConv.isSILIndirect(resultInfo)) {
      SILValue indirectResultAddr = materializeIndirectResultAddress(
          result, loweredCalleeConv.getSILType(resultInfo, typeCtx));
      // Record the new indirect call argument.
      newCallArgs[newResultArgIdx++] = indirectResultAddr;
    }
    return true;
  };
  visitCallResults(apply, visitCallResult);

  // Append the existing call arguments to the SIL argument list. They were
  // already lowered to addresses by CallArgRewriter.
  assert(newResultArgIdx == loweredCalleeConv.getSILArgIndexOfFirstParam());
  unsigned origArgIdx = apply.getSubstCalleeConv().getSILArgIndexOfFirstParam();
  for (unsigned endIdx = newCallArgs.size(); newResultArgIdx < endIdx;
       ++newResultArgIdx, ++origArgIdx) {
    newCallArgs[newResultArgIdx] = apply.getArgument(origArgIdx);
  }
}

SILBasicBlock::iterator ApplyRewriter::getResultInsertionPoint() {
  switch (apply.getKind()) {
  case FullApplySiteKind::ApplyInst: {
    return std::next(apply.getInstruction()->getIterator());
  }
  case FullApplySiteKind::TryApplyInst: {
    auto *tryApply = cast<TryApplyInst>(apply.getInstruction());
    return tryApply->getNormalBB()->begin();
  }
  case FullApplySiteKind::BeginApplyInst: {
    llvm_unreachable("coroutines don't have indirect results");
  }
  }
}

/// Return the storage address for the indirect result corresponding to the
/// \p oldResult. Allocate temporary argument storage for an
/// indirect result that isn't mapped to storage because it is either loadable
/// or unused.
///
/// \p oldResult is invalid for an unused result.
SILValue ApplyRewriter::materializeIndirectResultAddress(SILValue oldResult,
                                                         SILType argTy) {
  if (oldResult && oldResult->getType().isAddressOnly(*pass.function)) {
    // Results that project into their uses have not yet been materialized.
    addrMat.materializeAddress(oldResult);

    auto &storage = pass.valueStorageMap.getStorage(oldResult);
    storage.markRewritten();
    return storage.storageAddress;
  }
  // Allocate temporary call-site storage for an unused or loadable result.
  auto *allocInst = argBuilder.createAllocStack(callLoc, argTy);

  // Instead of using resultBuilder, insert dealloc immediately after the call
  // for stack discipline across loadable indirect results.
  apply.insertAfterApplication([&](SILBuilder &callBuilder) {
    callBuilder.createDeallocStack(callLoc, allocInst);
  });

  if (oldResult && !oldResult->use_empty()) {
    // Insert reloads immediately after the call. Get the reload insertion
    // point after emitting dealloc to ensure the reload happens first.
    auto reloadBuilder = pass.getBuilder(getResultInsertionPoint());

    // This is a formally indirect argument, but is loadable.
    auto *loadInst = reloadBuilder.createTrivialLoadOr(
        callLoc, allocInst, LoadOwnershipQualifier::Take);
    oldResult->replaceAllUsesWith(loadInst);
  }
  return SILValue(allocInst);
}

void ApplyRewriter::rewriteApply(ArrayRef<SILValue> newCallArgs) {
  auto *oldCall = cast<ApplyInst>(apply.getInstruction());

  auto *newCall = argBuilder.createApply(
      callLoc, apply.getCallee(), apply.getSubstitutionMap(), newCallArgs,
      oldCall->getApplyOptions(), oldCall->getSpecializationInfo());

  this->apply = FullApplySite(newCall);

  // No need to delete this apply. It either has a single address-only result
  // and will be deleted at the end of the pass. Or it has multiple results and
  // will be deleted with its destructure_tuple.
}

/// Emit end_borrows for a an incomplete BorrowedValue with only nonlifetime
/// ending uses.
static void emitEndBorrows(SILValue value, AddressLoweringState &pass);

void ApplyRewriter::convertBeginApplyWithOpaqueYield() {
  // Avoid revisiting this apply.
  bool erased = pass.indirectApplies.erase(apply);
  assert(erased && "all begin_applies should be rewritten at the same time");
  (void)erased;

  auto *origCall = cast<BeginApplyInst>(apply.getInstruction());
  SmallVector<SILValue, 4> opValues;

  for (auto &oper : origCall->getArgumentOperands()) {
    opValues.push_back(oper.get());
  }

  // Recreate the begin_apply so that the instruction results have the right
  // ownership kind as per the lowered addresses convention.
  auto *newCall = argBuilder.createBeginApply(
      callLoc, apply.getCallee(), apply.getSubstitutionMap(), opValues,
      origCall->getApplyOptions(), origCall->getSpecializationInfo());
  this->apply = FullApplySite(newCall);

  // Replace uses of orig begin_apply with the new begin_apply
  auto oldResults = origCall->getAllResultsBuffer();
  auto newResults = newCall->getAllResultsBuffer();
  assert(oldResults.size() == newResults.size());
  for (auto i : indices(oldResults)) {
    auto &oldResult = oldResults[i];
    auto &newResult = newResults[i];
    if (oldResult.getType().isObject() && newResult.getType().isObject()) {
      // Handle direct conventions.
      oldResult.replaceAllUsesWith(&newResult);
      continue;
    }
    if (oldResult.getType().isAddress()) {
      // Handle inout convention.
      assert(newResult.getType().isAddress());
      oldResult.replaceAllUsesWith(&newResult);
      continue;
    }
    if (oldResult.getType().isAddressOnly(*pass.function)) {
      auto info = newCall->getSubstCalleeConv().getYieldInfoForOperandIndex(i);
      assert(info.isFormalIndirect());
      if (info.isConsumed()) {
        // Because it is legal to have uses of an owned value produced by a
        // begin_apply after a coroutine's range, AddressLowering must move the
        // value into local storage so that such out-of-coroutine-range uses can
        // be rewritten in terms of that address (instead of being rewritten in
        // terms of the yielded owned storage which is no longer valid beyond
        // the coroutine's range).
        auto &storage = pass.valueStorageMap.getStorage(&oldResult);
        auto destAddr = addrMat.materializeAddress(&oldResult);
        storage.storageAddress = destAddr;
        storage.markRewritten();
        resultBuilder.createCopyAddr(callLoc, &newResult, destAddr,
                                     info.isConsumed() ? IsTake : IsNotTake,
                                     IsInitialization);
      } else {
        // [in_guaranteed_begin_apply_results] Because OSSA ensures that all
        // uses of a guaranteed value produced by a begin_apply are used within
        // the coroutine's range, AddressLowering will not introduce uses of
        // invalid memory by rewriting the uses of a yielded guaranteed opaque
        // value as uses of yielded guaranteed storage.  However, it must
        // allocate storage for copies of [projections of] such values.
        pass.valueStorageMap.setStorageAddress(&oldResult, &newResult);
        pass.valueStorageMap.getStorage(&oldResult).markRewritten();
      }
      continue;
    }
    assert(oldResult.getType().isObject());
    assert(newResult.getType().isAddress());
    if (oldResult.getOwnershipKind() == OwnershipKind::Guaranteed) {
      SILValue load =
          resultBuilder.emitLoadBorrowOperation(callLoc, &newResult);
      oldResult.replaceAllUsesWith(load);
      emitEndBorrows(load, pass);
    } else {
      auto *load = resultBuilder.createTrivialLoadOr(
          callLoc, &newResult, LoadOwnershipQualifier::Take);
      oldResult.replaceAllUsesWith(load);
    }
  }
}

// Replace \p tryApply with a new try_apply using \p newCallArgs.
//
// If the old result was a single opaque value, then create and return a
// fake load that takes its place in the storage map. Otherwise, return an
// invalid SILValue.
//
// Update this->apply with the new call instruction.
//
// Input (T = address-only, L=Loadable):
//
//   %addr = alloc_stack $T                    // storage for %oldResult
//   ...
//   try_apply : $() -> @out T
//  bbNormal(%oldResult : $T):
//
// Output (return %oldResult - ApplyRewriter final)):
//
//   %addr = alloc_stack $T                    // storage for %oldResult
//   ...
//   try_apply(%addr) : $() -> @out T
//  bbNormal(%newResult : $()):
//   %oldResult = load undef
//
// Input:
//
//   %addr = alloc_stack $L                    // unmapped temp storage
//   try_apply() : $() -> @out L
//  bbNormal(%oldResult : $L):                 // no uses
//   %result = load %addr : $*L
//   dealloc_stack %addr
//
// Output (return invalid - ApplyRewriter final):
//
//   %addr = alloc_stack $L                    // unmapped temp storage
//   try_apply(%addr) : $() -> @out L
//  bbNormal(%oldResult : $()):                 // no uses
//   %result = load %addr : $*L
//   dealloc_stack %addr
//
// Input:
//
//   %addr0 = alloc_stack $T                   // storage for %result0
//   ...
//   %addr1 = alloc_stack                      // unmapped temp storage
//   try_apply() : $() -> (@out T, @out L, L)
//  bbNormal(%tuple : $(T, L, L)):
//   %r1 = load %addr1 : $*L
//   dealloc_stack %addr1
//   (%r0, %d1, %r2) = destructure_tuple %tuple : $(T, T, T)
//   // no uses of %d1
//
// Output (return invalid):
//
//   %addr0 = alloc_stack $T                   // storage for %result0
//   ...
//   %addr1 = alloc_stack                      // unmapped temp storage
//   try_apply(%addr0, %addr1) : $() -> (@out T, @out L, L)
//  bbNormal(%newResult : $L):                 // no uses yet
//   %r1 = load %addr1 : $*L
//   dealloc_stack %addr1
//   (%r0, %d1, %r2) = destructure_tuple undef : $(T, T, T)
//   // no uses of %d1
//
void ApplyRewriter::rewriteTryApply(ArrayRef<SILValue> newCallArgs) {
  auto typeCtx = pass.function->getTypeExpansionContext();
  auto *tryApply = cast<TryApplyInst>(apply.getInstruction());

  auto *newCallInst = argBuilder.createTryApply(
      callLoc, apply.getCallee(), apply.getSubstitutionMap(), newCallArgs,
      tryApply->getNormalBB(), tryApply->getErrorBB(),
      tryApply->getApplyOptions(), tryApply->getSpecializationInfo());

  auto *resultArg = cast<SILArgument>(apply.getResult());

  auto replaceTermResult = [&](SILValue newResultVal) {
    SILType resultTy = loweredCalleeConv.getSILResultType(typeCtx);
    auto ownership = resultTy.isTrivial(*pass.function) ? OwnershipKind::None
                                                        : OwnershipKind::Owned;

    resultArg->replaceAllUsesWith(newResultVal);
    assert(resultArg->getIndex() == 0);
    resultArg->getParent()->replacePhiArgument(0, resultTy, ownership,
                                               resultArg->getDecl());
  };
  // Immediately delete the old try_apply (old applies hang around until
  // dead code removal because they directly define values).
  pass.deleter.forceDelete(tryApply);
  this->apply = FullApplySite(newCallInst);

  // Handle a single opaque result value.
  if (pass.valueStorageMap.contains(resultArg)) {
    // Storage was materialized by materializeIndirectResultAddress.
    auto &origStorage = pass.valueStorageMap.getStorage(resultArg);
    assert(origStorage.isRewritten);
    (void)origStorage;

    // Rewriting try_apply with a new function type requires erasing the opaque
    // block argument.  Create a dummy load-copy until all uses have been
    // rewritten.
    LoadInst *loadArg = resultBuilder.createLoad(
        callLoc, origStorage.storageAddress, LoadOwnershipQualifier::Copy);

    pass.valueStorageMap.replaceValue(resultArg, loadArg);
    replaceTermResult(loadArg);
    return;
  }
  // Loadable results were loaded by materializeIndirectResultAddress.
  // Temporarily redirect all uses to Undef. They will be fixed in
  // replaceDirectResults().
  replaceTermResult(
      SILUndef::get(resultArg->getType().getAddressType(), *pass.function));
}

// Replace all formally direct results by rewriting the destructure_tuple.
//
// Input:
//
//   %addr0 = alloc_stack $T                   // storage for %r0
//   ...
//   %addr1 = alloc_stack                      // unmapped temp storage
//   %newPseudoResult = apply(%addr0, %addr1) : $() -> (@out T, @out L, L)
//   %tuple = apply() : $() -> (@out T, @out L, L)
//   %r1 = load %addr1 : $*L
//   dealloc_stack %addr1
//   (%r0, %d1, %r2) = destructure_tuple %tuple : $(T, T, T)
//   // no uses of %d1
//
// Output:
//
//   %addr0 = alloc_stack $T                   // storage for %r0
//   ...
//   %addr1 = alloc_stack                      // unmapped temp storage
//   %r2 = apply(%addr0, %addr1) : $() -> (@out T, @out L, L)
//   %tuple = apply() : $() -> (@out T, @out L, L)
//   %r1  = load %addr1 : $*L
//   dealloc_stack %addr1
//   (%r0, %d1, %d2) = destructure_tuple %tuple : $(T, T, T)
//   // no uses of %d1, %d2
//
void ApplyRewriter::replaceDirectResults(DestructureTupleInst *oldDestructure) {
  SILValue newPseudoResult = apply.getResult();

  DestructureTupleInst *newDestructure = nullptr;
  if (loweredCalleeConv.getNumDirectSILResults() > 1) {
    newDestructure =
        resultBuilder.createDestructureTuple(callLoc, newPseudoResult);
  }
  unsigned newDirectResultIdx = 0;

  auto visitOldCallResult = [&](SILValue result, SILResultInfo resultInfo) {
    assert(!opaqueCalleeConv.isSILIndirect(resultInfo) &&
           "canonical call results are always direct");

    if (loweredCalleeConv.isSILIndirect(resultInfo)) {
      if (result->getType().isAddressOnly(*pass.function)) {
        // Mark the extract as rewritten now so we don't attempt to convert the
        // call again.
        pass.valueStorageMap.getStorage(result).markRewritten();
        return true;
      }
      // This loadable indirect use should already be redirected to a load from
      // the argument storage and marked dead.
      assert(result->use_empty());
      return true;
    }
    auto newResult = newDestructure
                         ? newDestructure->getResult(newDirectResultIdx)
                         : newPseudoResult;
    ++newDirectResultIdx;
    result->replaceAllUsesWith(newResult);
    return true;
  };
  visitCallMultiResults(oldDestructure, opaqueCalleeConv, visitOldCallResult);
  assert(newDirectResultIdx == loweredCalleeConv.getNumDirectSILResults());

  // If the oldDestructure produces any address-only results, then it will still
  // have uses, those results are mapped to storage, and the destructure will be
  // force-deleted later during deleteRewrittenInstructions. But if there are no
  // address-only results, then all of the old destructure's uses will already
  // be replaced. It must be force deleted now to avoid deleting it later as
  // regular dead code and emitting a bad lifetime fixup for its owned operand.
  if (isInstructionTriviallyDead(oldDestructure)) {
    pass.deleter.forceDelete(oldDestructure);
  }
}

//===----------------------------------------------------------------------===//
//                               CheckedCastBrRewriter
//
//    Utilities for rewriting checked_cast_br with opaque source/target type
// ===---------------------------------------------------------------------===//
class CheckedCastBrRewriter {
  CheckedCastBranchInst *ccb;
  AddressLoweringState &pass;
  SILLocation castLoc;
  SILFunction *func;
  SILBasicBlock *successBB;
  SILBasicBlock *failureBB;
  SILArgument *origSuccessVal;
  SILArgument *origFailureVal;
  SILBuilder termBuilder;
  SILBuilder successBuilder;
  SILBuilder failureBuilder;

public:
  CheckedCastBrRewriter(CheckedCastBranchInst *ccb, AddressLoweringState &pass)
      : ccb(ccb), pass(pass), castLoc(ccb->getLoc()), func(ccb->getFunction()),
        successBB(ccb->getSuccessBB()), failureBB(ccb->getFailureBB()),
        origSuccessVal(successBB->getArgument(0)),
        origFailureVal(failureBB->getArgument(0)),
        termBuilder(pass.getTermBuilder(ccb)),
        successBuilder(pass.getBuilder(successBB->begin())),
        failureBuilder(pass.getBuilder(failureBB->begin())) {}

  /// Rewrite checked_cast_br with opaque source/target operands to
  /// checked_cast_addr_br
  void rewrite() {
    auto srcAddr =
        getAddressForCastEntity(ccb->getOperand(), /* needsInit */ true);
    auto destAddr =
        getAddressForCastEntity(origSuccessVal, /* needsInit */ false);

    // getReusedStorageOperand() ensured we do not allocate a separate address
    // for failure block arg. Set the storage address of the failure block arg
    // to be source address here.
    if (origFailureVal->getType().isAddressOnly(*func)) {
      pass.valueStorageMap.setStorageAddress(origFailureVal, srcAddr);
    }

    termBuilder.createCheckedCastAddrBranch(
        castLoc, CastConsumptionKind::TakeOnSuccess, srcAddr,
        ccb->getSourceFormalType(), destAddr, ccb->getTargetFormalType(),
        successBB, failureBB, ccb->getTrueBBCount(), ccb->getFalseBBCount());

    replaceBlockArg(origSuccessVal, destAddr);
    replaceBlockArg(origFailureVal, srcAddr);

    pass.deleter.forceDelete(ccb);
  }

private:
  /// Return the storageAddress if \p value is opaque, otherwise create and
  /// return a stack temporary.
  SILValue getAddressForCastEntity(SILValue value, bool needsInit) {
    if (value->getType().isAddressOnly(*func)) {
      auto builder = pass.getBuilder(ccb->getIterator());
      AddressMaterialization addrMat(pass, builder);
      return addrMat.materializeAddress(value);
    }

    // Create a stack temporary for a loadable value
    auto *addr = termBuilder.createAllocStack(castLoc, value->getType());
    if (needsInit) {
      termBuilder.createStore(castLoc, value, addr,
                              value->getType().isTrivial(*func)
                                  ? StoreOwnershipQualifier::Trivial
                                  : StoreOwnershipQualifier::Init);
    }
    successBuilder.createDeallocStack(castLoc, addr);
    failureBuilder.createDeallocStack(castLoc, addr);
    return addr;
  }

  void replaceBlockArg(SILArgument *blockArg, SILValue addr) {
    // Replace all uses of the opaque block arg with a load from its
    // storage address.
    auto load =
        pass.getBuilder(blockArg->getParent()->begin())
            .createTrivialLoadOr(castLoc, addr, LoadOwnershipQualifier::Take);
    blockArg->replaceAllUsesWith(load);

    blockArg->getParent()->eraseArgument(blockArg->getIndex());

    if (blockArg->getType().isAddressOnly(*func)) {
      // In case of opaque block arg, replace the block arg with the dummy load
      // in the valueStorageMap. DefRewriter::visitLoadInst will then rewrite
      // the dummy load to copy_addr.
      pass.valueStorageMap.replaceValue(blockArg, load);
    }
  }
};

//===----------------------------------------------------------------------===//
//                 unconditional_checked_cast rewriting
//
//      Rewrites an unconditional_checked_cast to an address instruction.
//===----------------------------------------------------------------------===//

static UnconditionalCheckedCastAddrInst *rewriteUnconditionalCheckedCastInst(
    UnconditionalCheckedCastInst *uncondCheckedCast, AddressLoweringState &pass,
    AddressMaterialization *addrMat) {
  auto srcVal = uncondCheckedCast->getOperand();
  auto destVal = SILValue(uncondCheckedCast);
  auto srcType = srcVal->getType();
  auto destType = destVal->getType();
  // There are four cases to handle:
  // - source address-only, target address-only
  // - source address-only, target loadable
  // - source loadable,     target address-only
  // - source loadable,     target loadable
  auto srcAddrOnly = srcType.isAddressOnly(*pass.function);
  auto destAddrOnly = destType.isAddressOnly(*pass.function);
  SILValue srcAddr;

  auto loc = uncondCheckedCast->getLoc();

  auto builder = pass.getBuilder(uncondCheckedCast->getIterator());
  if (srcAddrOnly) {
    srcAddr = pass.valueStorageMap.getStorage(srcVal).storageAddress;
  } else {
    srcAddr = builder.createAllocStack(loc, srcType);
    builder.createStore(loc, srcVal, srcAddr,
                        srcType.isTrivial(*pass.function)
                            ? StoreOwnershipQualifier::Trivial
                            : StoreOwnershipQualifier::Init);
  }
  assert(srcAddr);
  SILValue destAddr;
  if (destAddrOnly) {
    destAddr = addrMat->materializeAddress(destVal);
  } else {
    destAddr = builder.createAllocStack(loc, destType);
  }
  assert(destAddr);
  auto *uccai = builder.createUnconditionalCheckedCastAddr(
      uncondCheckedCast->getLoc(), srcAddr, srcAddr->getType().getASTType(),
      destAddr, destAddr->getType().getASTType());
  auto afterBuilder =
      pass.getBuilder(uncondCheckedCast->getNextInstruction()->getIterator());
  if (srcAddrOnly) {
    // No cleanup to do.
  } else {
    afterBuilder.createDeallocStack(loc, srcAddr);
  }
  if (destAddrOnly) {
    // No cleanup to do.
  } else {
    auto *load = afterBuilder.createLoad(loc, destAddr,
                                         destType.isTrivial(*pass.function)
                                             ? LoadOwnershipQualifier::Trivial
                                             : LoadOwnershipQualifier::Take);
    destVal->replaceAllUsesWith(load);
    afterBuilder.createDeallocStack(loc, destAddr);
  }
  if (!srcAddrOnly && !destAddrOnly) {
    pass.deleter.forceDelete(uncondCheckedCast);
  }
  return uccai;
}

//===----------------------------------------------------------------------===//
//                               ReturnRewriter
//
//             Rewrite return instructions for indirect results.
//===----------------------------------------------------------------------===//

class ReturnRewriter {
  AddressLoweringState &pass;
  SILFunctionConventions opaqueFnConv;

public:
  ReturnRewriter(AddressLoweringState &pass)
      : pass(pass), opaqueFnConv(pass.function->getConventions()) {}

  void rewriteReturns();

protected:
  void rewriteReturn(ReturnInst *returnInst);

  void rewriteElement(SILValue oldResult, SILArgument *newResultArg,
                      SILBuilder &returnBuilder);
};

void ReturnRewriter::rewriteReturns() {
  for (SILInstruction *termInst : pass.exitingInsts) {
    if (auto *returnInst = dyn_cast<ReturnInst>(termInst))
      rewriteReturn(returnInst);
    else
      assert(isa<ThrowInst>(termInst));
  }
}

void ReturnRewriter::rewriteReturn(ReturnInst *returnInst) {
  auto &astCtx = pass.getModule()->getASTContext();
  auto typeCtx = pass.function->getTypeExpansionContext();

  // Find the point before allocated storage has been deallocated.
  auto insertPt = SILBasicBlock::iterator(returnInst);
  for (auto bbStart = returnInst->getParent()->begin(); insertPt != bbStart;
       --insertPt) {
    if (!isa<DeallocStackInst>(*std::prev(insertPt)))
      break;
  }
  auto returnBuilder = pass.getBuilder(insertPt);

  // Gather direct function results.
  unsigned numOldResults = opaqueFnConv.getNumDirectSILResults();
  SmallVector<SILValue, 8> oldResults;
  TupleInst *pseudoReturnVal = nullptr;
  if (numOldResults == 1)
    oldResults.push_back(returnInst->getOperand());
  else {
    pseudoReturnVal = cast<TupleInst>(returnInst->getOperand());
    oldResults.append(pseudoReturnVal->getElements().begin(),
                      pseudoReturnVal->getElements().end());
    assert(oldResults.size() == numOldResults);
  }

  SmallVector<SILValue, 8> newDirectResults;
  unsigned newResultArgIdx =
      pass.loweredFnConv.getSILArgIndexOfFirstIndirectResult();

  // Initialize the indirect result arguments and populate newDirectResults.
  for_each(pass.function->getLoweredFunctionType()->getResults(), oldResults,
           [&](SILResultInfo resultInfo, SILValue oldResult) {
             // Assume that all original results are direct in SIL.
             assert(!opaqueFnConv.isSILIndirect(resultInfo));
             if (!pass.loweredFnConv.isSILIndirect(resultInfo)) {
               newDirectResults.push_back(oldResult);
               return;
             }
             SILArgument *newResultArg =
                 pass.function->getArgument(newResultArgIdx);
             rewriteElement(oldResult, newResultArg, returnBuilder);
             ++newResultArgIdx;
           });

  assert(newDirectResults.size() ==
         pass.loweredFnConv.getNumDirectSILResults());
  assert(newResultArgIdx == pass.loweredFnConv.getSILArgIndexOfFirstParam());

  // Generate a new return_inst for the new direct results.
  SILValue newReturnVal;
  if (newDirectResults.empty()) {
    SILType emptyTy = SILType::getPrimitiveObjectType(astCtx.TheEmptyTupleType);
    newReturnVal = returnBuilder.createTuple(pass.genLoc(), emptyTy, {});
  } else if (newDirectResults.size() == 1) {
    newReturnVal = newDirectResults[0];
  } else {
    newReturnVal = returnBuilder.createTuple(
        pass.genLoc(), pass.loweredFnConv.getSILResultType(typeCtx),
        newDirectResults);
  }
  // Rewrite the returned value.
  SILValue origFullResult = returnInst->getOperand();
  assert(isPseudoReturnValue(origFullResult) == (pseudoReturnVal != nullptr));

  returnInst->setOperand(newReturnVal);
  // A pseudo return value is not be deleted during deleteRewrittenInstructions
  // because it is not mapped ValueStorage. Delete it now since it's value are
  // all consumed by newReturnVal.
  if (pseudoReturnVal) {
    pass.deleter.forceDelete(pseudoReturnVal);
  }
}

void ReturnRewriter::rewriteElement(SILValue oldResult,
                                    SILArgument *newResultArg,
                                    SILBuilder &returnBuilder) {
  SILType resultTy = oldResult->getType();
  if (resultTy.isAddressOnly(*pass.function)) {
    ValueStorage &storage = pass.valueStorageMap.getStorage(oldResult);
    assert(storage.isRewritten);
    SILValue resultAddr = storage.storageAddress;
    if (resultAddr != newResultArg) {
      // Copy the result from local storage into the result argument.
      returnBuilder.createCopyAddr(pass.genLoc(), resultAddr, newResultArg,
                                   IsTake, IsInitialization);
    }
  } else {
    // Store the result into the result argument.
    returnBuilder.createTrivialStoreOr(pass.genLoc(), oldResult, newResultArg,
                                       StoreOwnershipQualifier::Init);
  }
}

//===----------------------------------------------------------------------===//
//                               YieldRewriter
//
//             Rewrite return instructions for indirect results.
//===----------------------------------------------------------------------===//

class YieldRewriter {
  AddressLoweringState &pass;
  SILFunctionConventions opaqueFnConv;

public:
  YieldRewriter(AddressLoweringState &pass)
      : pass(pass), opaqueFnConv(pass.function->getConventions()) {}

  void rewriteYields();

  void rewriteYield(YieldInst *yieldInst);

protected:
  void rewriteOperand(YieldInst *yieldInst, unsigned index);
};

void YieldRewriter::rewriteYields() {
  for (auto *yield : pass.yieldInsts) {
    rewriteYield(yield);
  }
}

void YieldRewriter::rewriteYield(YieldInst *yieldInst) {
  for (unsigned index = 0, count = yieldInst->getNumOperands(); index < count;
       ++index) {
    rewriteOperand(yieldInst, index);
  }
}
void YieldRewriter::rewriteOperand(YieldInst *yieldInst, unsigned index) {
  auto info = opaqueFnConv.getYieldInfoForOperandIndex(index);
  auto convention = info.getConvention();
  auto ty = pass.function->mapTypeIntoContext(
      opaqueFnConv.getSILType(info, pass.function->getTypeExpansionContext()));
  if (ty.isAddressOnly(*pass.function)) {
    assert(yieldInst->getOperand(index)->getType().isAddress() &&
           "rewriting yield of of address-only value after use rewriting!?");
    return;
  }

  OwnershipKind ownership = OwnershipKind::None;
  switch (convention) {
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Pack_Guaranteed:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Inout:
    return;
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    return;
  case ParameterConvention::Indirect_In:
    ownership = OwnershipKind::Owned;
    break;
  case ParameterConvention::Indirect_In_Guaranteed:
    ownership = OwnershipKind::Guaranteed;
  }

  if (ty.isTrivial(*pass.function))
    ownership = OwnershipKind::None;

  auto operand = yieldInst->getOperand(index);

  auto builder = pass.getBuilder(yieldInst->getIterator());

  auto *asi = builder.createAllocStack(yieldInst->getLoc(), ty);

  auto withSuccessorBuilders = [&](auto perform) {
    auto *resumeBB = &yieldInst->getResumeBB()->front();
    auto *unwindBB = &yieldInst->getUnwindBB()->front();
    auto resumeBuilder = pass.getBuilder(resumeBB->getIterator());
    auto unwindBuilder = pass.getBuilder(unwindBB->getIterator());
    perform(resumeBuilder, resumeBB->getLoc());
    perform(unwindBuilder, unwindBB->getLoc());
  };

  switch (ownership) {
  case OwnershipKind::Owned:
  case OwnershipKind::None:
    builder.createStore(yieldInst->getLoc(), operand, asi,
                        ownership == OwnershipKind::None
                            ? StoreOwnershipQualifier::Trivial
                            : StoreOwnershipQualifier::Init);
    yieldInst->setOperand(index, asi);
    withSuccessorBuilders(
        [&](auto builder, auto loc) { builder.createDeallocStack(loc, asi); });
    break;
  case OwnershipKind::Guaranteed: {
    BeginBorrowInst *bbi = nullptr;
    if (operand->getOwnershipKind() == OwnershipKind::Owned) {
      bbi = builder.createBeginBorrow(yieldInst->getLoc(), operand);
    }
    auto *storeBorrow = builder.createStoreBorrow(yieldInst->getLoc(),
                                                  bbi ? bbi : operand, asi);
    yieldInst->setOperand(index, storeBorrow);
    withSuccessorBuilders([&](auto builder, auto loc) {
      builder.createEndBorrow(loc, storeBorrow);
      if (bbi)
        builder.createEndBorrow(loc, bbi);
      builder.createDeallocStack(loc, asi);
    });
    break;
  }
  case OwnershipKind::Unowned:
  case OwnershipKind::Any:
    llvm_unreachable("unexpected ownership kind!?");
  }
}

//

//===----------------------------------------------------------------------===//
//                                UseRewriter
//
// Rewrite opaque value uses in forward order--uses are rewritten before defs.
//===----------------------------------------------------------------------===//

namespace {

class UseRewriter : SILInstructionVisitor<UseRewriter> {
  friend SILVisitorBase<UseRewriter>;
  friend SILInstructionVisitor<UseRewriter>;

  AddressLoweringState &pass;

  SILBuilder builder;
  AddressMaterialization addrMat;

  Operand *use = nullptr;

  explicit UseRewriter(AddressLoweringState &pass, Operand *use)
      : pass(pass), builder(pass.getBuilder(use->getUser()->getIterator())),
        addrMat(pass, builder), use(use) {}

public:
  static void rewriteUse(Operand *use, AddressLoweringState &pass) {
    // Special handling for the broken opened archetypes representation in which
    // a single result represents both a value of the opened type and the
    // metatype itself :/
    if (use->isTypeDependent())
      return;

    UseRewriter(pass, use).visit(use->getUser());
  }

protected:
  // If rewriting a use also rewrites the value defined by the user, then mark
  // the defined value as rewritten. The defined value will not be revisited by
  // DefRewriter.
  void markRewritten(SILValue oldValue, SILValue addr) {
    auto &storage = pass.valueStorageMap.getStorage(oldValue);
    // getReusedStorageOperand() ensures that oldValue does not already have
    // separate storage. So there's no need to delete its alloc_stack.
    assert(!storage.storageAddress || storage.storageAddress == addr);
    storage.storageAddress = addr;
    storage.markRewritten();
  }

  void beforeVisit(SILInstruction *inst) {
    LLVM_DEBUG(llvm::dbgs() << "REWRITE USE "; inst->dump());
  }

  void visitSILInstruction(SILInstruction *inst) {
    inst->dump();
    llvm::report_fatal_error("^^^ Unimplemented opaque value use.");
  }

  // Opaque call argument.
  void visitApplyInst(ApplyInst *applyInst) {
    CallArgRewriter(applyInst, pass).rewriteIndirectArgument(use);
  }

  void visitPartialApplyInst(PartialApplyInst *pai) {
    CallArgRewriter(pai, pass).rewriteIndirectArgument(use);
  }

  void visitBeginApplyInst(BeginApplyInst *bai) {
    CallArgRewriter(bai, pass).rewriteIndirectArgument(use);
  }

  void visitYieldInst(YieldInst *yield) {
    SILValue addr = addrMat.materializeAddress(use->get());
    yield->setOperand(use->getOperandNumber(), addr);
  }

  void visitValueMetatypeInst(ValueMetatypeInst *vmi) {
    SILValue opAddr = addrMat.materializeAddress(use->get());
    vmi->setOperand(opAddr);
  }

  void visitExistentialMetatypeInst(ExistentialMetatypeInst *emi) {
    SILValue opAddr = addrMat.materializeAddress(use->get());
    emi->setOperand(opAddr);
  }

  void visitAddressOfBorrowBuiltinInst(BuiltinInst *bi, bool stackProtected) {
    SILValue value = bi->getOperand(0);
    SILValue addr = pass.valueStorageMap.getStorage(value).storageAddress;
    auto &astCtx = pass.getModule()->getASTContext();
    SILType rawPointerType = SILType::getRawPointerType(astCtx);
    SILValue result = builder.createAddressToPointer(
        bi->getLoc(), addr, rawPointerType, stackProtected);
    bi->replaceAllUsesWith(result);
  }

  void visitBuiltinInst(BuiltinInst *bi) {
    switch (bi->getBuiltinKind().value_or(BuiltinValueKind::None)) {
    case BuiltinValueKind::ResumeNonThrowingContinuationReturning:
    case BuiltinValueKind::ResumeThrowingContinuationReturning: {
      SILValue opAddr = addrMat.materializeAddress(use->get());
      bi->setOperand(use->getOperandNumber(), opAddr);
      break;
    }
    case BuiltinValueKind::Copy: {
      SILValue opAddr = addrMat.materializeAddress(use->get());
      bi->setOperand(0, opAddr);
      break;
    }
    case BuiltinValueKind::AddressOfBorrowOpaque:
      visitAddressOfBorrowBuiltinInst(bi, /*stackProtected=*/true);
      break;
    case BuiltinValueKind::UnprotectedAddressOfBorrowOpaque:
      visitAddressOfBorrowBuiltinInst(bi, /*stackProtected=*/false);
      break;
    default:
      bi->dump();
      llvm::report_fatal_error("^^^ Unimplemented builtin opaque value use.");
    }
  }

  void visitBeginBorrowInst(BeginBorrowInst *borrow);

  void visitEndBorrowInst(EndBorrowInst *end) {}

  void visitFixLifetimeInst(FixLifetimeInst *fli) {
    SILValue value = fli->getOperand();
    SILValue address = pass.valueStorageMap.getStorage(value).storageAddress;
    builder.createFixLifetime(fli->getLoc(), address);
    pass.deleter.forceDelete(fli);
  }

  void visitBranchInst(BranchInst *) {
    pass.getPhiRewriter().materializeOperand(use);

    use->set(SILUndef::get(use->get()->getType(), *pass.function));
  }

  // Copy from an opaque source operand.
  void visitCopyValueInst(CopyValueInst *copyInst) {
    SILValue srcVal = copyInst->getOperand();
    SILValue srcAddr = pass.valueStorageMap.getStorage(srcVal).storageAddress;

    SILValue destAddr = addrMat.materializeAddress(copyInst);
    if (destAddr != srcAddr) {
      builder.createCopyAddr(copyInst->getLoc(), srcAddr, destAddr, IsNotTake,
                             IsInitialization);
    }
    markRewritten(copyInst, destAddr);
  }

  void visitDebugValueInst(DebugValueInst *debugInst) {
    SILValue srcVal = debugInst->getOperand();
    SILValue srcAddr = pass.valueStorageMap.getStorage(srcVal).storageAddress;
    builder.createDebugValueAddr(debugInst->getLoc(), srcAddr,
                                 *debugInst->getVarInfo());
    pass.deleter.forceDelete(debugInst);
  }

  void visitDeinitExistentialValueInst(
      DeinitExistentialValueInst *deinitExistential) {
    // FIXME: Unimplemented
    llvm::report_fatal_error("Unimplemented DeinitExistentialValue use.");
  }

  void visitDestroyValueInst(DestroyValueInst *destroy) {
    SILValue srcVal = destroy->getOperand();
    SILValue srcAddr = pass.valueStorageMap.getStorage(srcVal).storageAddress;
    builder.createDestroyAddr(destroy->getLoc(), srcAddr);
    pass.deleter.forceDelete(destroy);
  }

  void rewriteDestructure(SILInstruction *destructure);

  void visitDestructureStructInst(DestructureStructInst *destructure) {
    rewriteDestructure(destructure);
  }

  void visitDestructureTupleInst(DestructureTupleInst *destructure) {
    rewriteDestructure(destructure);
  }

  // Enums are rewritten on the def side to handle both address-only and
  // loadable payloads. An address-only payload implies an address-only Enum.
  void visitEnumInst(EnumInst *enumInst) {}

  // Handle InitExistentialValue on the def side because loadable values must
  // also be copied into existential storage.
  void
  visitInitExistentialValueInst(InitExistentialValueInst *initExistential) {}

  // Opening an opaque existential. Rewrite the opened existentials here on
  // the use-side because it may produce either loadable or address-only
  // types.
  void visitOpenExistentialValueInst(OpenExistentialValueInst *openExistential);

  void visitReturnInst(ReturnInst *returnInst) {
    // Returns are rewritten for any function with indirect results after
    // opaque value rewriting.
  }

  void visitStoreBorrowInst(StoreBorrowInst *sbi) {
    auto addr = addrMat.materializeAddress(use->get());
    SmallVector<Operand *, 4> uses(sbi->getUses());
    for (auto *use : uses) {
      if (auto *ebi = dyn_cast<EndBorrowInst>(use->getUser())) {
        pass.deleter.forceDelete(ebi);
      }
    }
    sbi->replaceAllUsesWith(addr);
    pass.deleter.forceDelete(sbi);
  }

  void rewriteStore(SILValue srcVal, SILValue destAddr,
                    IsInitialization_t isInit);

  void visitStoreInst(StoreInst *storeInst);

  void emitExtract(SingleValueInstruction *extractInst);

  void visitSelectEnumInst(SelectEnumInst *sei) {
    SmallVector<std::pair<EnumElementDecl *, SILValue>> caseValues;
    for (unsigned index = 0, count = sei->getNumCases(); index < count;
         ++index) {
      caseValues.push_back(sei->getCase(index));
    }

    SILValue opAddr = addrMat.materializeAddress(use->get());
    SILValue addr =
        builder.createSelectEnumAddr(sei->getLoc(), opAddr, sei->getType(),
                                     sei->getDefaultResult(), caseValues);
    sei->replaceAllUsesWith(addr);
    pass.deleter.forceDelete(sei);
  }

  // Extract from an opaque struct.
  void visitStructExtractInst(StructExtractInst *extractInst);

  // Structs are rewritten on the def-side, where both the address-only and
  // loadable elements that compose a struct can be handled. An address-only
  // member implies an address-only Struct.
  void visitStructInst(StructInst *structInst) {}

  // Opaque enum operand to a switch_enum.
  void visitSwitchEnumInst(SwitchEnumInst *SEI);

  // Opaque call argument.
  void visitTryApplyInst(TryApplyInst *tryApplyInst) {
    CallArgRewriter(tryApplyInst, pass).rewriteIndirectArgument(use);
  }

  // Tuples are rewritten on the def-side, where both the address-only and
  // loadable elements that compose a tuple can be handled. An address-only
  // element implies an address-only Tuple.
  void visitTupleInst(TupleInst *tupleInst) {}

  // Extract from an opaque tuple.
  void visitTupleExtractInst(TupleExtractInst *extractInst);

  void
  visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *uncheckedCastInst) {
    SILValue srcVal = uncheckedCastInst->getOperand();
    SILValue srcAddr = pass.valueStorageMap.getStorage(srcVal).storageAddress;

    auto destAddr = builder.createUncheckedAddrCast(
        uncheckedCastInst->getLoc(), srcAddr,
        uncheckedCastInst->getType().getAddressType());

    if (uncheckedCastInst->getType().isAddressOnly(*pass.function)) {
      markRewritten(uncheckedCastInst, destAddr);
      return;
    }

    // For loadable cast destination type, replace copies with load copies.
    if (Operand *use = uncheckedCastInst->getSingleUse()) {
      if (auto *cvi = dyn_cast<CopyValueInst>(use->getUser())) {
        auto *load = builder.createLoad(cvi->getLoc(), destAddr,
                                        LoadOwnershipQualifier::Copy);
        cvi->replaceAllUsesWith(load);
        pass.deleter.forceDelete(cvi);
        return;
      }
    }
    SILValue load =
        builder.emitLoadBorrowOperation(uncheckedCastInst->getLoc(), destAddr);
    uncheckedCastInst->replaceAllUsesWith(load);
    pass.deleter.forceDelete(uncheckedCastInst);
    emitEndBorrows(load, pass);
  }

  void visitUnconditionalCheckedCastInst(
      UnconditionalCheckedCastInst *uncondCheckedCast) {
    assert(uncondCheckedCast->getOperand()->getType().isAddressOnly(
        *pass.function));
    auto *uccai =
        rewriteUnconditionalCheckedCastInst(uncondCheckedCast, pass, &addrMat);
    if (uncondCheckedCast->getType().isAddressOnly(*pass.function)) {
      markRewritten(uncondCheckedCast, uccai->getDest());
    }
  }

  void visitCheckedCastBranchInst(CheckedCastBranchInst *checkedCastBranch);

  void visitUncheckedEnumDataInst(UncheckedEnumDataInst *enumDataInst);
};
} // end anonymous namespace

void UseRewriter::rewriteDestructure(SILInstruction *destructure) {
  for (auto result : destructure->getResults()) {
    SILValue extractAddr = addrMat.materializeDefProjection(result);
    if (result->getType().isAddressOnly(*pass.function)) {
      assert(use == getProjectedDefOperand(result));
      markRewritten(result, extractAddr);
    } else {
      assert(!pass.valueStorageMap.contains(result));
      auto guaranteed = !result->getType().isTrivial(*pass.function) &&
                        destructure->getOperand(0)->getOwnershipKind() ==
                            OwnershipKind::Guaranteed;
      SILValue loadElement;
      if (guaranteed) {
        loadElement =
            builder.emitLoadBorrowOperation(destructure->getLoc(), extractAddr);
      } else {
        loadElement = builder.createTrivialLoadOr(
            destructure->getLoc(), extractAddr, LoadOwnershipQualifier::Take);
      }
      result->replaceAllUsesWith(loadElement);
      if (guaranteed) {
        emitEndBorrows(loadElement, pass);
      }
    }
  }
}

void UseRewriter::visitBeginBorrowInst(BeginBorrowInst *borrow) {
  assert(use == getProjectedDefOperand(borrow));

  // Mark the value as rewritten and use the operand's storage.
  auto address = pass.valueStorageMap.getStorage(use->get()).storageAddress;
  markRewritten(borrow, address);

  // Borrows are irrelevant unless they are marked lexical.
  if (borrow->isLexical()) {
    if (auto base = getAccessBase(address)) {
      if (auto *allocStack = dyn_cast<AllocStackInst>(base)) {
        allocStack->setIsLexical();
        return;
      }
      // Function arguments are inherently lexical.
      if (isa<SILFunctionArgument>(base))
        return;
    }

    SWIFT_ASSERT_ONLY(address->dump());
    llvm_unreachable("^^^ unknown lexical address producer");
  }
}

// Opening an opaque existential. Rewrite the opened existentials here on
// the use-side because it may produce either loadable or address-only
// types.
void UseRewriter::visitOpenExistentialValueInst(
    OpenExistentialValueInst *openExistential) {
  assert(use == getReusedStorageOperand(openExistential));
  SILValue srcAddr = pass.valueStorageMap.getStorage(use->get()).storageAddress;

  // Replace the module's openedArchetypesDef
  pass.getModule()->willDeleteInstruction(openExistential);

  // Mutable access is always by address.
  auto *openAddr = builder.createOpenExistentialAddr(
      openExistential->getLoc(), srcAddr,
      openExistential->getType().getAddressType(),
      OpenedExistentialAccess::Immutable);

  openExistential->replaceAllTypeDependentUsesWith(openAddr);
  markRewritten(openExistential, openAddr);
}

void UseRewriter::rewriteStore(SILValue srcVal, SILValue destAddr,
                               IsInitialization_t isInit) {
  assert(use->get() == srcVal);
  auto *storeInst = use->getUser();
  auto loc = storeInst->getLoc();

  ValueStorage &storage = pass.valueStorageMap.getStorage(srcVal);
  SILValue srcAddr = storage.storageAddress;

  IsTake_t isTake = IsTake;
  if (auto *copy = dyn_cast<CopyValueInst>(srcVal)) {
    if (storage.isDefProjection) {
      SILValue copySrcAddr =
          pass.valueStorageMap.getStorage(copy->getOperand()).storageAddress;
      assert(srcAddr == copySrcAddr && "folded copy should borrow storage");
      (void)copySrcAddr;
      isTake = IsNotTake;
    }
  }
  SILBuilderWithScope::insertAfter(storeInst, [&](auto &builder) {
    builder.createCopyAddr(loc, srcAddr, destAddr, isTake, isInit);
  });
  pass.deleter.forceDelete(storeInst);
}

// If the source is a copy that projects storage from its def, then the copy
// semantics are handled here (by omitting the [take] flag from copy_addr).
void UseRewriter::visitStoreInst(StoreInst *storeInst) {
  IsInitialization_t isInit;
  auto qualifier = storeInst->getOwnershipQualifier();
  if (qualifier == StoreOwnershipQualifier::Init)
    isInit = IsInitialization;
  else {
    assert(qualifier == StoreOwnershipQualifier::Assign);
    isInit = IsNotInitialization;
  }
  rewriteStore(storeInst->getSrc(), storeInst->getDest(), isInit);
}

/// Emit end_borrows for a an incomplete BorrowedValue with only nonlifetime
/// ending uses. This function inserts end_borrows on the lifetime boundary.
static void emitEndBorrows(SILValue value, AddressLoweringState &pass) {
  assert(BorrowedValue(value));

  // Place end_borrows that cover the load_borrow uses. It is not necessary to
  // cover the outer borrow scope of the extract's operand. If a lexical
  // borrow scope exists for the outer value, which is now in memory, then
  // its alloc_stack will be marked lexical, and the in-memory values will be
  // kept alive until the end of the outer scope.
  SmallVector<Operand *, 4> usePoints;
  findInnerTransitiveGuaranteedUses(value, &usePoints);

  SmallVector<SILBasicBlock *, 4> discoveredBlocks;
  SSAPrunedLiveness liveness(value->getFunction(), &discoveredBlocks);
  liveness.initializeDef(value);
  for (auto *use : usePoints) {
    assert(!use->isLifetimeEnding() || isa<EndBorrowInst>(use->getUser()));
    liveness.updateForUse(use->getUser(), /*lifetimeEnding*/ false);
  }
  PrunedLivenessBoundary guaranteedBoundary;
  liveness.computeBoundary(guaranteedBoundary);
  guaranteedBoundary.visitInsertionPoints(
      [&](SILBasicBlock::iterator insertPt) {
        pass.getBuilder(insertPt).createEndBorrow(pass.genLoc(), value);
      });
}

// Extract from an opaque struct or tuple.
void UseRewriter::emitExtract(SingleValueInstruction *extractInst) {
  SILValue extractAddr = addrMat.materializeDefProjection(extractInst);

  if (extractInst->getType().isAddressOnly(*pass.function)) {
    assert(use == getProjectedDefOperand(extractInst));
    markRewritten(extractInst, extractAddr);
    return;
  }
  auto replaceUsesWithLoad = [&](SingleValueInstruction *oldInst,
                                 SILValue load) {
    oldInst->replaceAllUsesWith(load);
    pass.deleter.forceDelete(oldInst);
  };
  auto loc = extractInst->getLoc();
  if (extractInst->getType().isTrivial(*pass.function)) {
    auto *load =
        builder.createLoad(loc, extractAddr, LoadOwnershipQualifier::Trivial);
    replaceUsesWithLoad(extractInst, load);
    return;
  }
  if (Operand *use = extractInst->getSingleUse()) {
    if (auto *copy = dyn_cast<CopyValueInst>(use->getUser())) {
      auto *load =
          builder.createLoad(loc, extractAddr, LoadOwnershipQualifier::Copy);
      replaceUsesWithLoad(copy, load);
      return;
    }
  }
  SILValue loadElement =
      builder.emitLoadBorrowOperation(extractInst->getLoc(), extractAddr);
  replaceUsesWithLoad(extractInst, loadElement);
  emitEndBorrows(loadElement, pass);
}

void UseRewriter::visitStructExtractInst(StructExtractInst *extractInst) {
  emitExtract(extractInst);
}

// Extract from an opaque tuple.
void UseRewriter::visitTupleExtractInst(TupleExtractInst *extractInst) {
  emitExtract(extractInst);
}

// Rewrite switch_enum to switch_enum_addr. All associated block arguments are
// removed.
void UseRewriter::visitSwitchEnumInst(SwitchEnumInst * switchEnum) {
  SILValue enumVal = switchEnum->getOperand();
  assert(use->get() == enumVal);

  SILValue enumAddr = pass.getMaterializedAddress(enumVal);
  auto loc = switchEnum->getLoc();
  auto rewriteCase = [&](EnumElementDecl *caseDecl, SILBasicBlock *caseBB) {
    // Nothing to do for unused case payloads.
    if (caseBB->getArguments().size() == 0)
      return;

    assert(caseBB->getArguments().size() == 1);
    SILArgument *caseArg = caseBB->getArgument(0);

    assert(&switchEnum->getOperandRef() == getReusedStorageOperand(caseArg));
    assert(caseDecl->hasAssociatedValues() && "caseBB has a payload argument");

    SILBuilder caseBuilder = pass.getBuilder(caseBB->begin());
    auto *caseAddr =
        caseBuilder.createUncheckedTakeEnumDataAddr(loc, enumAddr, caseDecl);
    auto *caseLoad = caseBuilder.createTrivialLoadOr(
        loc, caseAddr, LoadOwnershipQualifier::Take);
    caseArg->replaceAllUsesWith(caseLoad);
    if (caseArg->getType().isAddressOnly(*pass.function)) {
      // Remap caseArg to the new dummy load which will be deleted during
      // deleteRewrittenInstructions.
      pass.valueStorageMap.replaceValue(caseArg, caseLoad);
      markRewritten(caseLoad, caseAddr);
    }
    caseBB->eraseArgument(0);
  };

  // TODO: The case list does not change. We should be able to avoid copying.
  SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 8> cases;
  SmallVector<ProfileCounter, 8> caseCounters;

  // Collect switch cases for rewriting and remove block arguments.
  for (unsigned caseIdx : range(switchEnum->getNumCases())) {
    auto caseDeclAndBB = switchEnum->getCase(caseIdx);
    EnumElementDecl *caseDecl = caseDeclAndBB.first;
    SILBasicBlock *caseBB = caseDeclAndBB.second;

    cases.push_back(caseDeclAndBB);
    caseCounters.push_back(switchEnum->getCaseCount(caseIdx));

    rewriteCase(caseDecl, caseBB);
  }
  SILBasicBlock *defaultBB = nullptr;
  auto defaultCounter = ProfileCounter();
  if (switchEnum->hasDefault()) {
    defaultBB = switchEnum->getDefaultBB();
    defaultCounter = switchEnum->getDefaultCount();
    if (auto defaultDecl = switchEnum->getUniqueCaseForDefault()) {
      rewriteCase(defaultDecl.get(), defaultBB);
    } else {
      assert(defaultBB->getArguments().size() == 1);
      SILArgument *arg = defaultBB->getArgument(0);
      assert(arg->getType().isAddressOnly(*pass.function));
      auto builder = pass.getBuilder(defaultBB->begin());
      auto addr = enumAddr;
      auto *load = builder.createTrivialLoadOr(switchEnum->getLoc(), addr,
                                               LoadOwnershipQualifier::Take);
      // Remap arg to the new dummy load which will be deleted during
      // deleteRewrittenInstructions.
      arg->replaceAllUsesWith(load);
      pass.valueStorageMap.replaceValue(arg, load);
      markRewritten(load, addr);
      defaultBB->eraseArgument(0);
    }
  }
  auto builder = pass.getTermBuilder(switchEnum);
  pass.deleter.forceDelete(switchEnum);
  builder.createSwitchEnumAddr(loc, enumAddr, defaultBB, cases,
                               ArrayRef<ProfileCounter>(caseCounters),
                               defaultCounter);
}

void UseRewriter::visitCheckedCastBranchInst(CheckedCastBranchInst *ccb) {
  CheckedCastBrRewriter(ccb, pass).rewrite();
}

void UseRewriter::visitUncheckedEnumDataInst(
    UncheckedEnumDataInst *enumDataInst) {
  assert(use == getReusedStorageOperand(enumDataInst));

  assert(enumDataInst->getOwnershipKind() != OwnershipKind::Guaranteed);

  // unchecked_enum_data could be a def-projection. It is handled as a
  // separate allocation to make it clear that it can't be
  // rematerialized. This means that
  auto srcAddr = pass.valueStorageMap.getStorage(use->get()).storageAddress;

  auto loc = enumDataInst->getLoc();
  auto elt = enumDataInst->getElement();
  auto destTy = enumDataInst->getType().getAddressType();
  auto *enumAddrInst =
      builder.createUncheckedTakeEnumDataAddr(loc, srcAddr, elt, destTy);

  markRewritten(enumDataInst, enumAddrInst);
}

//===----------------------------------------------------------------------===//
//                                DefRewriter
//
// Rewrite opaque value definitions in forward order--defs are after uses.
//===----------------------------------------------------------------------===//

namespace {
class DefRewriter : SILInstructionVisitor<DefRewriter> {
  friend SILVisitorBase<DefRewriter>;
  friend SILInstructionVisitor<DefRewriter>;

  AddressLoweringState &pass;

  SILBuilder builder;
  AddressMaterialization addrMat;

  ValueStorage &storage;

  explicit DefRewriter(AddressLoweringState &pass, SILValue value,
                       SILBasicBlock::iterator insertPt)
      : pass(pass), builder(pass.getBuilder(insertPt)), addrMat(pass, builder),
        storage(pass.valueStorageMap.getStorage(value)) {
    assert(!storage.isRewritten);
  }

public:
  static void rewriteValue(SILValue value, AddressLoweringState &pass) {
    if (auto *inst = value->getDefiningInstruction()) {
      DefRewriter(pass, value, inst->getIterator()).visit(inst);
    } else {
      // function args are already rewritten.
      auto *blockArg = cast<SILPhiArgument>(value);
      auto insertPt = blockArg->getParent()->begin();
      DefRewriter(pass, value, insertPt).rewriteArg(blockArg);
    }
  }

protected:
  // Set the storage address for an opaque block arg and mark it rewritten.
  void rewriteArg(SILPhiArgument *arg) {
    if (auto *tai =
            dyn_cast_or_null<TryApplyInst>(arg->getTerminatorForResult())) {
      CallArgRewriter(tai, pass).rewriteArguments();
      ApplyRewriter(tai, pass).convertApplyWithIndirectResults();
      return;
    } else if (auto *ccbi = dyn_cast_or_null<CheckedCastBranchInst>(
                   arg->getTerminatorForResult())) {
      CheckedCastBrRewriter(ccbi, pass).rewrite();
      return;
    }
    LLVM_DEBUG(llvm::dbgs() << "REWRITE ARG "; arg->dump());
    if (storage.storageAddress)
      LLVM_DEBUG(llvm::dbgs() << "  STORAGE "; storage.storageAddress->dump());
    storage.storageAddress = addrMat.materializeAddress(arg);
  }

  void beforeVisit(SILInstruction *inst) {
    LLVM_DEBUG(llvm::dbgs() << "REWRITE DEF "; inst->dump());
    if (storage.storageAddress)
      LLVM_DEBUG(llvm::dbgs() << "  STORAGE "; storage.storageAddress->dump());
  }

  void visitSILInstruction(SILInstruction *inst) {
    inst->dump();
    llvm::report_fatal_error("^^^ Unimplemented opaque value def.");
  }

  void visitApplyInst(ApplyInst *applyInst) {
    // Completely rewrite the apply instruction, handling any remaining
    // (loadable) indirect parameters, allocating memory for indirect
    // results, and generating a new apply instruction.
    CallArgRewriter(applyInst, pass).rewriteArguments();
    ApplyRewriter(applyInst, pass).convertApplyWithIndirectResults();
  }

  void visitBeginApplyInst(BeginApplyInst *bai) {
    CallArgRewriter(bai, pass).rewriteArguments();
    ApplyRewriter(bai, pass).convertBeginApplyWithOpaqueYield();
  }

  void visitBuiltinInst(BuiltinInst *bi) {
    switch (bi->getBuiltinKind().value_or(BuiltinValueKind::None)) {
    case BuiltinValueKind::Copy: {
      SILValue addr = addrMat.materializeAddress(bi);
      builder.createBuiltin(
          bi->getLoc(), bi->getName(),
          SILType::getEmptyTupleType(bi->getType().getASTContext()),
          bi->getSubstitutions(), {addr, bi->getOperand(0)});
      break;
    }
    default:
      bi->dump();
      llvm::report_fatal_error("^^^ Unimplemented builtin opaque value def.");
    }
  }

  // Rewrite the apply for an indirect result.
  void visitDestructureTupleInst(DestructureTupleInst *destructure) {
    SILValue srcVal = destructure->getOperand();
    assert(isPseudoCallResult(srcVal) && "destructure use should be rewritten");

    FullApplySite apply;
    if (auto *applyInst = dyn_cast<ApplyInst>(srcVal)) {
      apply = FullApplySite::isa(applyInst);
    } else {
      auto *termInst =
          SILArgument::isTerminatorResult(srcVal)->getTerminatorForResult();
      apply = FullApplySite::isa(termInst);
    }
    CallArgRewriter(apply, pass).rewriteArguments();
    ApplyRewriter(apply, pass).convertApplyWithIndirectResults();
  }

  // Define an opaque enum value.
  void visitEnumInst(EnumInst *enumInst) {
    if (enumInst->hasOperand()) {
      // Handle operands here because loadable operands must also be copied.
      addrMat.initializeComposingUse(&enumInst->getOperandRef());
    }
    SILValue enumAddr = addrMat.materializeAddress(enumInst);

    builder.createInjectEnumAddr(enumInst->getLoc(), enumAddr,
                                 enumInst->getElement());
  }

  // Define an existential.
  void visitInitExistentialValueInst(
      InitExistentialValueInst *initExistentialValue) {

    // Initialize memory for the operand which may be opaque or loadable.
    addrMat.initializeComposingUse(&initExistentialValue->getOperandRef());
  }

  void visitOpenExistentialBoxValueInst(
      OpenExistentialBoxValueInst *openExistentialBoxValue) {
    // Replace the module's openedArchetypesDef
    pass.getModule()->willDeleteInstruction(openExistentialBoxValue);

    auto *openAddr = builder.createOpenExistentialBox(
        openExistentialBoxValue->getLoc(),
        openExistentialBoxValue->getOperand(),
        openExistentialBoxValue->getType().getAddressType());

    openExistentialBoxValue->replaceAllTypeDependentUsesWith(openAddr);
    pass.valueStorageMap.setStorageAddress(openExistentialBoxValue, openAddr);
  }

  // Load an opaque value.
  void visitLoadInst(LoadInst *loadInst) {
    SILValue addr = addrMat.materializeAddress(loadInst);
    IsTake_t isTake;
    if (loadInst->getOwnershipQualifier() == LoadOwnershipQualifier::Take)
      isTake = IsTake;
    else {
      assert(loadInst->getOwnershipQualifier() == LoadOwnershipQualifier::Copy);
      isTake = IsNotTake;
    }
    // Dummy loads are already mapped to their storage address.
    if (addr != loadInst->getOperand()) {
      builder.createCopyAddr(loadInst->getLoc(), loadInst->getOperand(), addr,
                             isTake, IsInitialization);
    }
  }

  void visitLoadBorrowInst(LoadBorrowInst *lbi) {
    pass.valueStorageMap.setStorageAddress(lbi, lbi->getOperand());
  }

  // Define an opaque struct.
  void visitStructInst(StructInst *structInst) {
    // For each element, initialize the operand's memory. Some struct elements
    // may be loadable types.
    for (Operand &operand : structInst->getAllOperands())
      addrMat.initializeComposingUse(&operand);
  }

  // Define an opaque tuple.
  void visitTupleInst(TupleInst *tupleInst) {
    // For each element, initialize the operand's memory. Some tuple elements
    // may be loadable types.
    for (Operand &operand : tupleInst->getAllOperands())
      addrMat.initializeComposingUse(&operand);
  }

  void visitUnconditionalCheckedCastInst(
      UnconditionalCheckedCastInst *uncondCheckedCast) {
    assert(!uncondCheckedCast->getOperand()->getType().isAddressOnly(
        *pass.function));
    assert(uncondCheckedCast->getType().isAddressOnly(*pass.function));

    rewriteUnconditionalCheckedCastInst(uncondCheckedCast, pass, &addrMat);
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                           Rewrite Opaque Values
//===----------------------------------------------------------------------===//

// Rewrite applies with indirect parameters or results of loadable types which
// were not visited during opaque value rewriting.
static void rewriteIndirectApply(FullApplySite apply,
                                 AddressLoweringState &pass) {
  // If all indirect args were loadable, then they still need to be rewritten.
  CallArgRewriter(apply, pass).rewriteArguments();

  switch (apply.getKind()) {
  case FullApplySiteKind::ApplyInst:
  case FullApplySiteKind::TryApplyInst: {
    if (!apply.getSubstCalleeType()->hasIndirectFormalResults()) {
      return;
    }
    // If the call has indirect results and wasn't already rewritten, rewrite it
    // now. This handles try_apply, which is not rewritten when DefRewriter
    // visits block arguments. It also handles apply with loadable indirect
    // results.
    ApplyRewriter(apply, pass).convertApplyWithIndirectResults();
    break;
  }
  case FullApplySiteKind::BeginApplyInst: {
    if (!apply.getSubstCalleeType()->hasIndirectFormalYields()) {
      return;
    }
    ApplyRewriter(apply, pass).convertBeginApplyWithOpaqueYield();
    break;
  }
  }

  if (!apply.getInstruction()->isDeleted()) {
    assert(!getCallDestructure(apply)
           && "replaceDirectResults deletes the destructure");
    pass.deleter.forceDelete(apply.getInstruction());
  }
}

static void rewriteNonopaqueUnconditionalCheckedCast(
    UnconditionalCheckedCastInst *uncondCheckedCast,
    AddressLoweringState &pass) {
  assert(uncondCheckedCast->getOperand()->getType().isLoadable(*pass.function));
  assert(uncondCheckedCast->getType().isLoadable(*pass.function));

  rewriteUnconditionalCheckedCastInst(uncondCheckedCast, pass,
                                      /*addrMat=*/nullptr);
}

static void rewriteFunction(AddressLoweringState &pass) {
  // During rewriting, storage references are stable.
  pass.valueStorageMap.setStable();

  // For each opaque value in forward order, rewrite its users and its defining
  // instruction.
  for (auto &valueAndStorage : pass.valueStorageMap) {
    SILValue valueDef = valueAndStorage.value;
    // Rewrite a def that wasn't already rewritten when handling its operands.
    if (!valueAndStorage.storage.isRewritten) {
      DefRewriter::rewriteValue(valueDef, pass);
      valueAndStorage.storage.markRewritten();
    }
    // The def of interest may have been changed by rewriteValue.  Get that
    // redefinition back out of the ValueStoragePair.
    valueDef = valueAndStorage.value;
    // Rewrite a use of any non-address value mapped to storage (does not
    // include the already rewritten uses of indirect arguments).
    if (valueDef->getType().isAddress())
      continue;

    SmallPtrSet<Operand *, 8> originalUses;
    SmallVector<Operand *, 8> uses(valueDef->getUses());
    for (auto *oper : uses) {
      originalUses.insert(oper);
      UseRewriter::rewriteUse(oper, pass);
    }
    // Rewrite every new use that was added.
    uses.clear();
    for (auto *use : valueDef->getUses()) {
      if (originalUses.contains(use))
        continue;
      uses.push_back(use);
    }
    for (auto *oper : uses) {
      assert(oper->getUser() && isa<DebugValueInst>(oper->getUser()));
      UseRewriter::rewriteUse(oper, pass);
    }
  }
  // Rewrite any applies with indirect parameters now that all such parameters
  // are rewritten. If the apply had indirect results, it was already rewritten
  // by the defVisitor.
  for (auto optionalApply : pass.indirectApplies) {
    if (optionalApply) {
      rewriteIndirectApply(optionalApply.value(), pass);
    }
  }

  for (auto *ucci : pass.nonopaqueResultUCCs) {
    rewriteNonopaqueUnconditionalCheckedCast(ucci, pass);
  }

  for (auto *ccbi : pass.nonopaqueResultCCBs) {
    CheckedCastBrRewriter(ccbi, pass).rewrite();
  }

  // Rewrite this function's return value now that all opaque values within the
  // function are rewritten. This still depends on a valid ValueStorage
  // projection operands.
  if (pass.function->getLoweredFunctionType()->hasIndirectFormalResults())
    ReturnRewriter(pass).rewriteReturns();
  if (pass.function->getLoweredFunctionType()->hasIndirectFormalYields())
    YieldRewriter(pass).rewriteYields();
}

// Given an array of terminator operand values, produce an array of
// operands with those corresponding to deadArgIndices stripped out.
static void filterDeadArgs(OperandValueArrayRef origArgs,
                           ArrayRef<unsigned> deadArgIndices,
                           SmallVectorImpl<SILValue> &newArgs) {
  auto nextDeadArgI = deadArgIndices.begin();
  for (unsigned i : indices(origArgs)) {
    if (i == *nextDeadArgI && nextDeadArgI != deadArgIndices.end()) {
      ++nextDeadArgI;
      continue;
    }
    newArgs.push_back(origArgs[i]);
  }
  assert(nextDeadArgI == deadArgIndices.end());
}

// Rewrite a BranchInst omitting dead arguments.
static void removeBranchArgs(BranchInst *branch,
                             SmallVectorImpl<unsigned> &deadArgIndices,
                             AddressLoweringState &pass) {

  llvm::SmallVector<SILValue, 4> branchArgs;
  filterDeadArgs(branch->getArgs(), deadArgIndices, branchArgs);

  pass.getBuilder(branch->getIterator())
      .createBranch(branch->getLoc(), branch->getDestBB(), branchArgs);
  pass.deleter.forceDelete(branch);
}

// Remove opaque phis. Their inputs have already been substituted with Undef.
static void removeOpaquePhis(SILBasicBlock *bb, AddressLoweringState &pass) {
  if (bb->isEntry())
    return;

  SmallVector<unsigned, 16> deadArgIndices;
  for (auto *bbArg : bb->getArguments()) {
    if (bbArg->getType().isAddressOnly(*pass.function))
      deadArgIndices.push_back(bbArg->getIndex());
  }
  if (deadArgIndices.empty())
    return;

  // Iterate while modifying the predecessor's terminators.
  for (auto *predecessor : bb->getPredecessorBlocks()) {
    auto *branch = cast<BranchInst>(predecessor->getTerminator());
    removeBranchArgs(branch, deadArgIndices, pass);
  }
  // erase in reverse to avoid index invalidation.
  while (!deadArgIndices.empty()) {
    bb->eraseArgument(deadArgIndices.pop_back_val());
  }
}

// Instructions that use an opaque value without producing one are already
// deleted. The rest of the opaque definitions are now removed bottom-up
// by visiting valuestorageMap.
//
// Phis are removed here after all other instructions.
static void deleteRewrittenInstructions(AddressLoweringState &pass) {
  // Add the rest of the instructions to the dead list in post order.
  for (auto &valueAndStorage : llvm::reverse(pass.valueStorageMap)) {
    SILValue val = valueAndStorage.value;
    ValueStorage &storage = valueAndStorage.storage;

    assert(&pass.valueStorageMap.getStorage(val) == &valueAndStorage.storage
           && "invalid storage map");

    // Returned tuples and multi-result calls are not in the
    // valueStorageMap. Everything else must have been rewritten.
    assert(storage.isRewritten && "opaque value has not been rewritten");

    // If the storage was unused, e.g. because all uses were projected into
    // users, then delete the allocation.
    if (auto *allocInst = storage.storageAddress->getDefiningInstruction()) {
      pass.deleter.deleteIfDead(allocInst);
    }
    auto *deadInst = val->getDefiningInstruction();
    if (!deadInst || deadInst->isDeleted())
      continue;

    if (auto *destructure = dyn_cast<DestructureTupleInst>(deadInst)) {
      auto tupleVal = destructure->getOperand();
      if (auto *applyInst = dyn_cast<ApplyInst>(tupleVal)) {
        deadInst = applyInst;
      }
    }
    LLVM_DEBUG(llvm::dbgs() << "DEAD "; deadInst->dump());
    if (!isa<OpenExistentialValueInst>(deadInst) &&
        !isa<OpenExistentialBoxValueInst>(deadInst)) {
      pass.deleter.forceDeleteWithUsers(deadInst);
      continue;
    }
    // willDeleteInstruction was already called for open_existential_value to
    // update the registered type. Now fully erase the instruction, which will
    // harmlessly call willDeleteInstruction again.
    deadInst->getParent()->erase(deadInst);
  }

  pass.valueStorageMap.clear();

  // Remove block args after removing all instructions that may use them.
  for (auto &bb : *pass.function)
    removeOpaquePhis(&bb, pass);

  pass.deleter.cleanupDeadInstructions();
}

//===----------------------------------------------------------------------===//
//                        AddressLowering: Module Pass
//===----------------------------------------------------------------------===//

namespace {
// Note: the only reason this is not a FunctionTransform is to change the SIL
// stage for all functions at once.
class AddressLowering : public SILModuleTransform {
  /// The entry point to this module transformation.
  void run() override;

  void runOnFunction(SILFunction *F);
};
} // end anonymous namespace

void AddressLowering::runOnFunction(SILFunction *function) {
  if (!function->isDefinition())
    return;

  assert(function->hasOwnership() && "SIL opaque values requires OSSA");

  PrettyStackTraceSILFunction FuncScope("address-lowering", function);

  LLVM_DEBUG(llvm::dbgs() << "Address Lowering: " << function->getName()
                          << "\n");

  // Ensure that blocks can be processed in RPO order.
  removeUnreachableBlocks(*function);

  auto *dominance = PM->getAnalysis<DominanceAnalysis>();
  auto *deadEnds = PM->getAnalysis<DeadEndBlocksAnalysis>();

  AddressLoweringState pass(function, dominance->get(function),
                            deadEnds->get(function));

  // ## Step #1: Map opaque values
  //
  // First, rewrite this function's arguments and return values, then populate
  // pass.valueStorageMap with an entry for each opaque value.
  prepareValueStorage(pass);

  // ## Step #2: Allocate storage
  //
  // For each opaque value mapped in step #1, either create an
  // alloc_stack/dealloc_stack pair, or mark its ValueStorage entry as a
  // def-projection out of its operand's def or a use projection into its
  // composing use or into a phi (branch operand).
  OpaqueStorageAllocation allocator(pass);
  allocator.allocateOpaqueStorage();

  LLVM_DEBUG(llvm::dbgs() << "Finished allocating storage.\n"; function->dump();
             pass.valueStorageMap.dump());

  // ## Step #3. Rewrite opaque values
  //
  // Rewrite all instructions that either define or use an opaque value.
  // Creates new '_addr' variants of instructions, obtaining the storage
  // address from the 'valueStorageMap'. This materializes projections in
  // forward order, setting 'storageAddress' for each projection as it goes.
  rewriteFunction(pass);

  allocator.finalizeOpaqueStorage();

  deleteRewrittenInstructions(pass);

  StackNesting::fixNesting(function);

  // The CFG may change because of criticalEdge splitting during
  // createStackAllocation or StackNesting.
  invalidateAnalysis(function,
                     SILAnalysis::InvalidationKind::BranchesAndInstructions);
}

/// The entry point to this module transformation.
void AddressLowering::run() {
  if (getModule()->useLoweredAddresses())
    return;

  for (auto &F : *getModule()) {
    runOnFunction(&F);
  }
  // Update the SILModule before the PassManager has a chance to run
  // verification.
  getModule()->setLoweredAddresses(true);
}

SILTransform *swift::createAddressLowering() { return new AddressLowering(); }
