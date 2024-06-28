//===--- RegionAnalysis.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "transfer-non-sendable"

#include "swift/SILOptimizer/Analysis/RegionAnalysis.h"

#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/ImmutablePointerSet.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/NodeDatastructures.h"
#include "swift/SIL/OperandDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/PartitionUtils.h"
#include "swift/SILOptimizer/Utils/VariableNameUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::PartitionPrimitives;
using namespace swift::PatternMatch;
using namespace swift::regionanalysisimpl;

static llvm::cl::opt<bool> AbortOnUnknownPatternMatchError(
    "sil-region-isolation-assert-on-unknown-pattern",
    llvm::cl::desc("Abort if SIL region isolation detects an unknown pattern. "
                   "Intended only to be used when debugging the compiler!"),
    llvm::cl::init(false), llvm::cl::Hidden);

//===----------------------------------------------------------------------===//
//                              MARK: Utilities
//===----------------------------------------------------------------------===//

std::optional<ApplyIsolationCrossing>
regionanalysisimpl::getApplyIsolationCrossing(SILInstruction *inst) {
  if (ApplyExpr *apply = inst->getLoc().getAsASTNode<ApplyExpr>())
    if (auto crossing = apply->getIsolationCrossing())
      return crossing;

  if (auto fas = FullApplySite::isa(inst)) {
    if (auto crossing = fas.getIsolationCrossing())
      return crossing;
  }

  return {};
}

namespace {

struct UnderlyingTrackedValueInfo {
  SILValue value;

  /// Only used for addresses.
  std::optional<ActorIsolation> actorIsolation;

  explicit UnderlyingTrackedValueInfo(SILValue value) : value(value) {}

  UnderlyingTrackedValueInfo(SILValue value,
                             std::optional<ActorIsolation> actorIsolation)
      : value(value), actorIsolation(actorIsolation) {}
};

struct UseDefChainVisitor
    : public AccessUseDefChainVisitor<UseDefChainVisitor, SILValue> {
  bool isMerge = false;

  /// The actor isolation that we found while walking from use->def. Always set
  /// to the first one encountered.
  std::optional<ActorIsolation> actorIsolation;

  SILValue visitAll(SILValue sourceAddr) {
    SILValue result = visit(sourceAddr);
    if (!result)
      return sourceAddr;

    while (SILValue nextAddr = visit(result))
      result = nextAddr;

    return result;
  }

  SILValue visitBase(SILValue base, AccessStorage::Kind kind) {
    // If we are passed a project_box, we want to return the box itself. The
    // reason for this is that the project_box is considered to be non-aliasing
    // memory. We want to treat it as part of the box which is
    // aliasing... meaning that we need to merge.
    if (kind == AccessStorage::Box)
      return cast<ProjectBoxInst>(base)->getOperand();
    return SILValue();
  }

  SILValue visitNonAccess(SILValue) { return SILValue(); }

  SILValue visitPhi(SILPhiArgument *phi) {
    llvm_unreachable("Should never hit this");
  }

  // Override AccessUseDefChainVisitor to ignore access markers and find the
  // outer access base.
  SILValue visitNestedAccess(BeginAccessInst *access) {
    return visitAll(access->getSource());
  }

  SILValue visitStorageCast(SingleValueInstruction *cast, Operand *sourceAddr,
                            AccessStorageCast castType) {
    // If this is a type case, see if the result of the cast is sendable. In
    // such a case, we do not want to look through this cast.
    if (castType == AccessStorageCast::Type &&
        !SILIsolationInfo::isNonSendableType(cast->getType(),
                                             cast->getFunction()))
      return SILValue();

    // Do not look through begin_borrow [var_decl]. They are start new semantic
    // values.
    //
    // This only comes up if a codegen pattern occurs where the debug
    // information is place on a debug_value instead of the alloc_box.
    if (auto *bbi = dyn_cast<BeginBorrowInst>(cast)) {
      if (bbi->isFromVarDecl())
        return SILValue();
    }

    // If we do not have an identity cast, mark this as a merge.
    isMerge |= castType != AccessStorageCast::Identity;

    return sourceAddr->get();
  }

  SILValue visitAccessProjection(SingleValueInstruction *inst,
                                 Operand *sourceAddr) {
    // See if this access projection is into a single element value. If so, we
    // do not want to treat this as a merge.
    if (auto p = Projection(inst)) {
      switch (p.getKind()) {
      // Currently if we load and then project_box from a memory location,
      // we treat that as a projection. This follows the semantics/notes in
      // getAccessProjectionOperand.
      case ProjectionKind::Box:
        return cast<ProjectBoxInst>(inst)->getOperand();
      case ProjectionKind::Upcast:
      case ProjectionKind::RefCast:
      case ProjectionKind::BlockStorageCast:
      case ProjectionKind::BitwiseCast:
      case ProjectionKind::Class:
      case ProjectionKind::TailElems:
        llvm_unreachable("Shouldn't see this here");
      case ProjectionKind::Index:
        // Index is always a merge.
        isMerge = true;
        break;
      case ProjectionKind::Enum: {
        auto op = cast<UncheckedTakeEnumDataAddrInst>(inst)->getOperand();

        // See if our operand type is a sendable type. In such a case, we do not
        // want to look through our operand.
        if (!SILIsolationInfo::isNonSendableType(op->getType(),
                                                 op->getFunction()))
          return SILValue();

        break;
      }
      case ProjectionKind::Tuple: {
        // These are merges if we have multiple fields.
        auto op = cast<TupleElementAddrInst>(inst)->getOperand();

        if (!SILIsolationInfo::isNonSendableType(op->getType(),
                                                 op->getFunction()))
          return SILValue();

        isMerge |= op->getType().getNumTupleElements() > 1;
        break;
      }
      case ProjectionKind::Struct:
        auto op = cast<StructElementAddrInst>(inst)->getOperand();

        // See if our result type is a sendable type. In such a case, we do not
        // want to look through the struct_element_addr since we do not want to
        // identify the sendable type with the non-sendable operand. These we
        // are always going to ignore anyways since a sendable let/var field of
        // a struct can always be used.
        if (!SILIsolationInfo::isNonSendableType(op->getType(),
                                                 op->getFunction()))
          return SILValue();

        // These are merges if we have multiple fields.
        isMerge |= op->getType().getNumNominalFields() > 1;
        break;
      }
    }

    return sourceAddr->get();
  }
};

} // namespace

/// Classify an instructions as look through when we are looking through
/// values. We assert that all instructions that are CONSTANT_TRANSLATION
/// LookThrough to make sure they stay in sync.
static bool isStaticallyLookThroughInst(SILInstruction *inst) {
  switch (inst->getKind()) {
  default:
    return false;
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::BeginCOWMutationInst:
  case SILInstructionKind::BeginDeallocRefInst:
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperAddrInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::DestructureTupleInst:
  case SILInstructionKind::DifferentiableFunctionExtractInst:
  case SILInstructionKind::DropDeinitInst:
  case SILInstructionKind::EndCOWMutationInst:
  case SILInstructionKind::EndInitLetRefInst:
  case SILInstructionKind::ExplicitCopyValueInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::LinearFunctionExtractInst:
  case SILInstructionKind::MarkDependenceInst:
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::MarkUnresolvedNonCopyableValueInst:
  case SILInstructionKind::MarkUnresolvedReferenceBindingInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableAddrInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableBoxInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst:
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::ProjectBlockStorageInst:
  case SILInstructionKind::ProjectBoxInst:
  case SILInstructionKind::RefToBridgeObjectInst:
  case SILInstructionKind::RefToUnownedInst:
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::UnownedCopyValueInst:
  case SILInstructionKind::UnownedToRefInst:
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::ValueToBridgeObjectInst:
  case SILInstructionKind::WeakCopyValueInst:
  case SILInstructionKind::StrongCopyWeakValueInst:
  case SILInstructionKind::StrongCopyUnmanagedValueInst:
  case SILInstructionKind::RefToUnmanagedInst:
  case SILInstructionKind::UnmanagedToRefInst:
  case SILInstructionKind::InitExistentialValueInst:
    return true;
  case SILInstructionKind::MoveValueInst:
    // Look through if it isn't from a var decl.
    return !cast<MoveValueInst>(inst)->isFromVarDecl();
  case SILInstructionKind::BeginBorrowInst:
    // Look through if it isn't from a var decl.
    return !cast<BeginBorrowInst>(inst)->isFromVarDecl();
  case SILInstructionKind::UnconditionalCheckedCastInst: {
    auto cast = SILDynamicCastInst::getAs(inst);
    assert(cast);
    if (cast.isRCIdentityPreserving())
      return true;
    return false;
  }
  }
}

static bool isLookThroughIfResultNonSendable(SILInstruction *inst) {
  switch (inst->getKind()) {
  default:
    return false;
  case SILInstructionKind::RawPointerToRefInst:
    return true;
  }
}

static bool isLookThroughIfOperandNonSendable(SILInstruction *inst) {
  switch (inst->getKind()) {
  default:
    return false;
  case SILInstructionKind::RefToRawPointerInst:
    return true;
  }
}

static bool isLookThroughIfOperandAndResultNonSendable(SILInstruction *inst) {
  switch (inst->getKind()) {
  default:
    return false;
  case SILInstructionKind::UncheckedTrivialBitCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::UncheckedValueCastInst:
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
    return true;
  }
}

static SILValue getUnderlyingTrackedObjectValue(SILValue value) {
  auto *fn = value->getFunction();
  SILValue result = value;
  while (true) {
    SILValue temp = result;

    if (auto *svi = dyn_cast<SingleValueInstruction>(temp)) {
      if (isStaticallyLookThroughInst(svi)) {
        temp = svi->getOperand(0);
      }

      // If we have a cast and our operand and result are non-Sendable, treat it
      // as a look through.
      if (isLookThroughIfOperandAndResultNonSendable(svi)) {
        if (SILIsolationInfo::isNonSendableType(svi->getType(), fn) &&
            SILIsolationInfo::isNonSendableType(svi->getOperand(0)->getType(),
                                                fn)) {
          temp = svi->getOperand(0);
        }
      }

      if (isLookThroughIfResultNonSendable(svi)) {
        if (SILIsolationInfo::isNonSendableType(svi->getType(), fn)) {
          temp = svi->getOperand(0);
        }
      }

      if (isLookThroughIfOperandNonSendable(svi)) {
        // If our operand is a non-Sendable type, look through this instruction.
        if (SILIsolationInfo::isNonSendableType(svi->getOperand(0)->getType(),
                                                fn)) {
          temp = svi->getOperand(0);
        }
      }
    }

    if (auto *inst = temp->getDefiningInstruction()) {
      if (isStaticallyLookThroughInst(inst)) {
        temp = inst->getOperand(0);
      }
    }

    if (temp != result) {
      result = temp;
      continue;
    }

    return result;
  }
}

static UnderlyingTrackedValueInfo getUnderlyingTrackedValue(SILValue value) {
  // Look through a project_box, so that we process it like its operand object.
  if (auto *pbi = dyn_cast<ProjectBoxInst>(value)) {
    value = pbi->getOperand();
  }

  if (!value->getType().isAddress()) {
    SILValue underlyingValue = getUnderlyingTrackedObjectValue(value);

    if (!isa<LoadInst, LoadBorrowInst>(underlyingValue)) {
      return UnderlyingTrackedValueInfo(underlyingValue);
    }

    // If we got an address, lets see if we can do even better by looking at the
    // address.
    value = cast<SingleValueInstruction>(underlyingValue)->getOperand(0);
  }
  assert(value->getType().isAddress());

  UseDefChainVisitor visitor;
  SILValue base = visitor.visitAll(value);
  assert(base);
  if (base->getType().isObject())
    return {getUnderlyingTrackedValue(base).value, visitor.actorIsolation};

  return {base, visitor.actorIsolation};
}

SILValue RegionAnalysisFunctionInfo::getUnderlyingTrackedValue(SILValue value) {
  return ::getUnderlyingTrackedValue(value).value;
}

namespace {

struct TermArgSources {
  SmallFrozenMultiMap<SILValue, SILValue, 8> argSources;

  template <typename ValueRangeTy = ArrayRef<SILValue>>
  void addValues(ValueRangeTy valueRange, SILBasicBlock *destBlock) {
    for (auto pair : llvm::enumerate(valueRange))
      argSources.insert(destBlock->getArgument(pair.index()), pair.value());
  }

  TermArgSources() {}

  void init(SILInstruction *inst) {
    switch (cast<TermInst>(inst)->getTermKind()) {
    case TermKind::UnreachableInst:
    case TermKind::ReturnInst:
    case TermKind::ThrowInst:
    case TermKind::ThrowAddrInst:
    case TermKind::YieldInst:
    case TermKind::UnwindInst:
    case TermKind::TryApplyInst:
    case TermKind::SwitchValueInst:
    case TermKind::SwitchEnumInst:
    case TermKind::SwitchEnumAddrInst:
    case TermKind::AwaitAsyncContinuationInst:
    case TermKind::CheckedCastAddrBranchInst:
      llvm_unreachable("Unsupported?!");

    case TermKind::BranchInst:
      return init(cast<BranchInst>(inst));

    case TermKind::CondBranchInst:
      return init(cast<CondBranchInst>(inst));

    case TermKind::DynamicMethodBranchInst:
      return init(cast<DynamicMethodBranchInst>(inst));

    case TermKind::CheckedCastBranchInst:
      return init(cast<CheckedCastBranchInst>(inst));
    }

    llvm_unreachable("Covered switch isn't covered?!");
  }

private:
  void init(BranchInst *bi) { addValues(bi->getArgs(), bi->getDestBB()); }

  void init(CondBranchInst *cbi) {
    addValues(cbi->getTrueArgs(), cbi->getTrueBB());
    addValues(cbi->getFalseArgs(), cbi->getFalseBB());
  }

  void init(DynamicMethodBranchInst *dmBranchInst) {
    addValues({dmBranchInst->getOperand()}, dmBranchInst->getHasMethodBB());
  }

  void init(CheckedCastBranchInst *ccbi) {
    addValues({ccbi->getOperand()}, ccbi->getSuccessBB());
  }
};

} // namespace

static bool isProjectedFromAggregate(SILValue value) {
  assert(value->getType().isAddress());
  UseDefChainVisitor visitor;
  visitor.visitAll(value);
  return visitor.isMerge;
}

namespace {
using AsyncLetSourceValue =
    llvm::PointerUnion<PartialApplyInst *, ThinToThickFunctionInst *>;
} // namespace

static std::optional<AsyncLetSourceValue>
findAsyncLetPartialApplyFromStart(SILValue value) {
  // If our operand is Sendable then we want to return nullptr. We only want to
  // return a value if we are not
  auto fType = value->getType().castTo<SILFunctionType>();
  if (fType->isSendable())
    return {};

  SILValue temp = value;
  while (true) {
    if (isa<ConvertEscapeToNoEscapeInst>(temp) ||
        isa<ConvertFunctionInst>(temp)) {
      temp = cast<SingleValueInstruction>(temp)->getOperand(0);
    }
    if (temp == value)
      break;
    value = temp;
  }

  // We can also get a thin_to_thick_function here if we do not capture
  // anything. In such a case, we just do not process the partial apply get
  if (auto *ttfi = dyn_cast<ThinToThickFunctionInst>(value))
    return {{ttfi}};

  // Ok, we could still have a reabstraction thunk. In such a case, we want the
  // partial_apply that we process to be the original partial_apply (or
  // thin_to_thick)... so in that case process recursively.
  auto *pai = cast<PartialApplyInst>(value);
  if (auto *calleeFunction = pai->getCalleeFunction()) {
    if (calleeFunction->isThunk() == IsReabstractionThunk) {
      return findAsyncLetPartialApplyFromStart(pai->getArgument(0));
    }
  }

  // Otherwise, this is the right partial_apply... apply it!
  return {{pai}};
}

/// This recurses through reabstraction thunks.
static std::optional<AsyncLetSourceValue>
findAsyncLetPartialApplyFromStart(BuiltinInst *bi) {
  return findAsyncLetPartialApplyFromStart(bi->getOperand(1));
}

/// This recurses through reabstraction thunks.
static std::optional<AsyncLetSourceValue>
findAsyncLetPartialApplyFromGet(ApplyInst *ai) {
  auto *bi = cast<BuiltinInst>(FullApplySite(ai).getArgument(0));
  assert(*bi->getBuiltinKind() ==
         BuiltinValueKind::StartAsyncLetWithLocalBuffer);
  return findAsyncLetPartialApplyFromStart(bi);
}

static bool isAsyncLetBeginPartialApply(PartialApplyInst *pai) {
  if (auto *fas = pai->getCalleeFunction())
    if (fas->isThunk())
      return false;

  // Look through reabstraction thunks.
  SILValue result = pai;
  while (true) {
    SILValue iter = result;

    if (auto *use = iter->getSingleUse()) {
      if (auto *maybeThunk = dyn_cast<PartialApplyInst>(use->getUser())) {
        if (auto *fas = maybeThunk->getCalleeFunction()) {
          if (fas->isThunk()) {
            iter = maybeThunk;
          }
        }
      }
    }

    if (auto *cfi = iter->getSingleUserOfType<ConvertFunctionInst>())
      iter = cfi;
    if (auto *cvt = iter->getSingleUserOfType<ConvertEscapeToNoEscapeInst>())
      iter = cvt;

    if (iter == result)
      break;

    result = iter;
  }

  auto *bi = result->getSingleUserOfType<BuiltinInst>();
  if (!bi)
    return false;

  auto kind = bi->getBuiltinKind();
  if (!kind)
    return false;

  return *kind == BuiltinValueKind::StartAsyncLetWithLocalBuffer;
}

/// Returns true if this is a function argument that is able to be transferred
/// in the body of our function.
static bool isTransferrableFunctionArgument(SILFunctionArgument *arg) {
  // Indirect out parameters cannot be an input transferring parameter.
  if (arg->isIndirectResult() || arg->isIndirectErrorResult())
    return false;

  // If we have a function argument that is closure captured by a Sendable
  // closure, allow for the argument to be transferred.
  //
  // DISCUSSION: The reason that we do this is that in the case of us
  // having an actual Sendable closure there are two cases we can see:
  //
  // 1. If we have an actual Sendable closure, the AST will emit an
  // earlier error saying that we are capturing a non-Sendable value in a
  // Sendable closure. So we want to squelch the error that we would emit
  // otherwise. This only occurs when we are not in swift-6 mode since in
  // swift-6 mode we will error on the earlier error... but in the case of
  // us not being in swift 6 mode lets not emit extra errors.
  //
  // 2. If we have an async-let based Sendable closure, we want to allow
  // for the argument to be transferred in the async let's statement and
  // not emit an error.
  //
  // TODO: Once the async let refactoring change this will no longer be needed
  // since closure captures will have transferring parameters and be
  // non-Sendable.
  if (arg->isClosureCapture() &&
      arg->getFunction()->getLoweredFunctionType()->isSendable())
    return true;

  // Otherwise, we only allow for the argument to be transferred if it is
  // explicitly marked as a strong transferring parameter.
  return arg->isSending();
}

//===----------------------------------------------------------------------===//
//                            MARK: TrackableValue
//===----------------------------------------------------------------------===//

bool TrackableValue::isSendingParameter() const {
  // First get our alloc_stack.
  //
  // TODO: We should just put a flag on the alloc_stack, so we /know/ 100% that
  // it is from a consuming parameter. We don't have that so we pattern match.
  auto *asi =
      dyn_cast_or_null<AllocStackInst>(representativeValue.maybeGetValue());
  if (!asi)
    return false;

  if (asi->getParent() != asi->getFunction()->getEntryBlock())
    return false;

  // See if we are initialized from a transferring parameter and are the only
  // use of the parameter.
  OperandWorklist worklist(asi->getFunction());
  worklist.pushResultOperandsIfNotVisited(asi);

  while (auto *use = worklist.pop()) {
    auto *user = use->getUser();

    // Look through instructions that we don't care about.
    if (isa<MarkUnresolvedNonCopyableValueInst,
            MoveOnlyWrapperToCopyableAddrInst>(user)) {
      worklist.pushResultOperandsIfNotVisited(user);
    }

    if (auto *si = dyn_cast<StoreInst>(user)) {
      // Check if our store inst is from a function argument that is
      // transferring and for which the store is the only use of the function
      // argument.
      auto *fArg = dyn_cast<SILFunctionArgument>(si->getSrc());
      if (!fArg || !fArg->isSending())
        return false;
      return fArg->getSingleUse();
    }

    if (auto *copyAddr = dyn_cast<CopyAddrInst>(user)) {
      // Check if our copy_addr is from a function argument that is transferring
      // and for which the copy_addr is the only use of the function argument.
      auto *fArg = dyn_cast<SILFunctionArgument>(copyAddr->getSrc());
      if (!fArg || !fArg->isSending())
        return false;
      return fArg->getSingleUse();
    }
  }

  // Otherwise, this isn't a consuming parameter.
  return false;
}

//===----------------------------------------------------------------------===//
//                      MARK: Partial Apply Reachability
//===----------------------------------------------------------------------===//

namespace {

/// We need to be able to know if instructions that extract sendable fields from
/// non-sendable addresses are reachable from a partial_apply that captures the
/// non-sendable value or its underlying object by reference. In such a case, we
/// need to require the value to not be transferred when the extraction happens
/// since we could race on extracting the value.
///
/// The reason why we use a dataflow to do this is that:
///
/// 1. We do not want to recompute this for each individual instruction that
/// might be reachable from the partial apply.
///
/// 2. Just computing reachability early is a very easy way to do this.
struct PartialApplyReachabilityDataflow {
  PostOrderFunctionInfo *pofi;
  llvm::DenseMap<SILValue, unsigned> valueToBit;
  std::vector<std::pair<SILValue, SILInstruction *>> valueToGenInsts;

  struct BlockState {
    SmallBitVector entry;
    SmallBitVector exit;
    SmallBitVector gen;
    bool needsUpdate = true;
  };

  BasicBlockData<BlockState> blockData;
  bool propagatedReachability = false;

  PartialApplyReachabilityDataflow(SILFunction *fn, PostOrderFunctionInfo *pofi)
      : pofi(pofi), blockData(fn) {}

  /// Begin tracking an operand of a partial apply.
  void add(Operand *op);

  /// Once we have finished adding data to the data, propagate reachability.
  void propagateReachability();

  bool isReachable(SILValue value, SILInstruction *user) const;
  bool isReachable(Operand *op) const {
    return isReachable(op->get(), op->getUser());
  }

  bool isGenInstruction(SILValue value, SILInstruction *inst) const {
    assert(propagatedReachability && "Only valid once propagated reachability");
    auto iter =
        std::lower_bound(valueToGenInsts.begin(), valueToGenInsts.end(),
                         std::make_pair(value, nullptr),
                         [](const std::pair<SILValue, SILInstruction *> &p1,
                            const std::pair<SILValue, SILInstruction *> &p2) {
                           return p1 < p2;
                         });
    return iter != valueToGenInsts.end() && iter->first == value &&
           iter->second == inst;
  }

  void print(llvm::raw_ostream &os) const;

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  SILValue getRootValue(SILValue value) const {
    return getUnderlyingTrackedValue(value).value;
  }

  unsigned getBitForValue(SILValue value) const {
    unsigned size = valueToBit.size();
    auto &self = const_cast<PartialApplyReachabilityDataflow &>(*this);
    auto iter = self.valueToBit.try_emplace(value, size);
    return iter.first->second;
  }
};

} // namespace

void PartialApplyReachabilityDataflow::add(Operand *op) {
  assert(!propagatedReachability &&
         "Cannot add more operands once reachability is computed");
  SILValue underlyingValue = getRootValue(op->get());
  LLVM_DEBUG(llvm::dbgs() << "PartialApplyReachability::add.\nValue: "
                          << underlyingValue << "User: " << *op->getUser());

  unsigned bit = getBitForValue(underlyingValue);
  auto &state = blockData[op->getParentBlock()];
  state.gen.resize(bit + 1);
  state.gen.set(bit);
  valueToGenInsts.emplace_back(underlyingValue, op->getUser());
}

bool PartialApplyReachabilityDataflow::isReachable(SILValue value,
                                                   SILInstruction *user) const {
  assert(
      propagatedReachability &&
      "Can only check for reachability once reachability has been propagated");
  SILValue baseValue = getRootValue(value);
  auto iter = valueToBit.find(baseValue);
  // If we aren't tracking this value... just bail.
  if (iter == valueToBit.end())
    return false;
  unsigned bitNum = iter->second;
  auto &state = blockData[user->getParent()];

  // If we are reachable at entry, then we are done.
  if (state.entry.test(bitNum)) {
    return true;
  }

  // Otherwise, check if we are reachable at exit. If we are not, then we are
  // not reachable.
  if (!state.exit.test(bitNum)) {
    return false;
  }

  // We were not reachable at entry but are at our exit... walk the block and
  // see if our user is before a gen instruction.
  auto genStart = std::lower_bound(
      valueToGenInsts.begin(), valueToGenInsts.end(),
      std::make_pair(baseValue, nullptr),
      [](const std::pair<SILValue, SILInstruction *> &p1,
         const std::pair<SILValue, SILInstruction *> &p2) { return p1 < p2; });
  if (genStart == valueToGenInsts.end() || genStart->first != baseValue)
    return false;

  auto genEnd = genStart;
  while (genEnd != valueToGenInsts.end() && genEnd->first == baseValue)
    ++genEnd;

  // Walk forward from the beginning of the block to user. If we do not find a
  // gen instruction, then we know the gen occurs after the op.
  return llvm::any_of(
      user->getParent()->getRangeEndingAtInst(user), [&](SILInstruction &inst) {
        auto iter = std::lower_bound(
            genStart, genEnd, std::make_pair(baseValue, &inst),
            [](const std::pair<SILValue, SILInstruction *> &p1,
               const std::pair<SILValue, SILInstruction *> &p2) {
              return p1 < p2;
            });
        return iter != valueToGenInsts.end() && iter->first == baseValue &&
               iter->second == &inst;
      });
}

void PartialApplyReachabilityDataflow::propagateReachability() {
  assert(!propagatedReachability && "Cannot propagate reachability twice");
  propagatedReachability = true;

  // Now that we have finished initializing, resize all of our bitVectors to the
  // final number of bits.
  unsigned numBits = valueToBit.size();

  // If numBits is none, we have nothing to process.
  if (numBits == 0)
    return;

  for (auto iter : blockData) {
    iter.data.entry.resize(numBits);
    iter.data.exit.resize(numBits);
    iter.data.gen.resize(numBits);
    iter.data.needsUpdate = true;
  }

  // Freeze our value to gen insts map so we can perform in block checks.
  sortUnique(valueToGenInsts);

  // We perform a simple gen-kill dataflow with union. Since we are just
  // propagating reachability, there isn't any kill.
  bool anyNeedUpdate = true;
  SmallBitVector temp(numBits);
  blockData[&*blockData.getFunction()->begin()].needsUpdate = true;
  while (anyNeedUpdate) {
    anyNeedUpdate = false;

    for (auto *block : pofi->getReversePostOrder()) {
      auto &state = blockData[block];

      if (!state.needsUpdate) {
        continue;
      }

      state.needsUpdate = false;
      temp.reset();
      for (auto *predBlock : block->getPredecessorBlocks()) {
        auto &predState = blockData[predBlock];
        temp |= predState.exit;
      }

      state.entry = temp;

      temp |= state.gen;

      if (temp != state.exit) {
        state.exit = temp;
        for (auto *succBlock : block->getSuccessorBlocks()) {
          anyNeedUpdate = true;
          blockData[succBlock].needsUpdate = true;
        }
      }
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "Propagating Captures Result!\n";
             print(llvm::dbgs()));
}

void PartialApplyReachabilityDataflow::print(llvm::raw_ostream &os) const {
  // This is only invoked for debugging purposes, so make nicer output.
  std::vector<std::pair<unsigned, SILValue>> data;
  for (auto [value, bitNo] : valueToBit) {
    data.emplace_back(bitNo, value);
  }
  std::sort(data.begin(), data.end());

  os << "(BitNo, Value):\n";
  for (auto [bitNo, value] : data) {
    os << "    " << bitNo << ": " << value;
  }

  os << "(Block,GenBits):\n";
  for (auto [block, state] : blockData) {
    os << "    bb" << block.getDebugID() << ".\n"
       << "        Entry: " << state.entry << '\n'
       << "        Gen: " << state.gen << '\n'
       << "        Exit: " << state.exit << '\n';
  }
}

//===----------------------------------------------------------------------===//
//                 MARK: Expr/Type Inference for Diagnostics
//===----------------------------------------------------------------------===//

namespace {

struct InferredCallerArgumentTypeInfo {
  Type baseInferredType;
  SmallVector<std::pair<Type, std::optional<ApplyIsolationCrossing>>, 4>
      applyUses;

  void init(const Operand *op);

  /// Init for an apply that does not have an associated apply expr.
  ///
  /// This should only occur when writing SIL test cases today. In the future,
  /// we may represent all of the actor isolation information at the SIL level,
  /// but we are not there yet today.
  void initForApply(ApplyIsolationCrossing isolationCrossing);

  void initForApply(const Operand *op, ApplyExpr *expr);
  void initForAutoclosure(const Operand *op, AutoClosureExpr *expr);

  Expr *getFoundExprForSelf(ApplyExpr *sourceApply) {
    if (auto callExpr = dyn_cast<CallExpr>(sourceApply))
      if (auto calledExpr =
              dyn_cast<DotSyntaxCallExpr>(callExpr->getDirectCallee()))
        return calledExpr->getBase();
    return nullptr;
  }

  Expr *getFoundExprForParam(ApplyExpr *sourceApply, unsigned argNum) {
    auto *expr = sourceApply->getArgs()->getExpr(argNum);

    // If we have an erasure expression, lets use the original type. We do
    // this since we are not saying the specific parameter that is the
    // issue and we are using the type to explain it to the user.
    if (auto *erasureExpr = dyn_cast<ErasureExpr>(expr))
      expr = erasureExpr->getSubExpr();

    return expr;
  }
};

} // namespace

void InferredCallerArgumentTypeInfo::initForApply(
    ApplyIsolationCrossing isolationCrossing) {
  applyUses.emplace_back(baseInferredType, isolationCrossing);
}

void InferredCallerArgumentTypeInfo::initForApply(const Operand *op,
                                                  ApplyExpr *sourceApply) {
  auto isolationCrossing = sourceApply->getIsolationCrossing();
  assert(isolationCrossing && "Should have valid isolation crossing?!");

  // Grab out full apply site and see if we can find a better expr.
  SILInstruction *i = const_cast<SILInstruction *>(op->getUser());
  auto fai = FullApplySite::isa(i);

  Expr *foundExpr = nullptr;

  // If we have self, then infer it.
  if (fai.hasSelfArgument() && op == &fai.getSelfArgumentOperand()) {
    foundExpr = getFoundExprForSelf(sourceApply);
  } else {
    // Otherwise, try to infer using the operand of the ApplyExpr.
    unsigned argNum = [&]() -> unsigned {
      if (fai.isCalleeOperand(*op))
        return op->getOperandNumber();
      return fai.getAppliedArgIndexWithoutIndirectResults(*op);
    }();

    // If something funny happened and we get an arg num that is larger than our
    // num args... just return nullptr so we emit an error using our initial
    // foundExpr.
    //
    // TODO: We should emit a "I don't understand error" so this gets reported
    // to us.
    if (argNum < sourceApply->getArgs()->size()) {
      foundExpr = getFoundExprForParam(sourceApply, argNum);
    }
  }

  auto inferredArgType =
      foundExpr ? foundExpr->findOriginalType() : baseInferredType;
  applyUses.emplace_back(inferredArgType, isolationCrossing);
}

namespace {

struct Walker : ASTWalker {
  InferredCallerArgumentTypeInfo &foundTypeInfo;
  ValueDecl *targetDecl;
  SmallPtrSet<Expr *, 8> visitedCallExprDeclRefExprs;

  Walker(InferredCallerArgumentTypeInfo &foundTypeInfo, ValueDecl *targetDecl)
      : foundTypeInfo(foundTypeInfo), targetDecl(targetDecl) {}

  Expr *lookThroughExpr(Expr *expr) {
    while (true) {
      if (auto *memberRefExpr = dyn_cast<MemberRefExpr>(expr)) {
        expr = memberRefExpr->getBase();
        continue;
      }

      if (auto *cvt = dyn_cast<ImplicitConversionExpr>(expr)) {
        expr = cvt->getSubExpr();
        continue;
      }

      if (auto *e = dyn_cast<ForceValueExpr>(expr)) {
        expr = e->getSubExpr();
        continue;
      }

      if (auto *t = dyn_cast<TupleElementExpr>(expr)) {
        expr = t->getBase();
        continue;
      }

      return expr;
    }
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    if (auto *declRef = dyn_cast<DeclRefExpr>(expr)) {
      // If this decl ref expr was not visited as part of a callExpr, add it as
      // something without isolation crossing.
      if (!visitedCallExprDeclRefExprs.count(declRef)) {
        if (declRef->getDecl() == targetDecl) {
          visitedCallExprDeclRefExprs.insert(declRef);
          foundTypeInfo.applyUses.emplace_back(declRef->findOriginalType(),
                                               std::nullopt);
          return Action::Continue(expr);
        }
      }
    }

    if (auto *callExpr = dyn_cast<CallExpr>(expr)) {
      if (auto isolationCrossing = callExpr->getIsolationCrossing()) {
        // Search callExpr's arguments to see if we have our targetDecl.
        auto *argList = callExpr->getArgs();
        for (auto pair : llvm::enumerate(argList->getArgExprs())) {
          auto *arg = lookThroughExpr(pair.value());
          if (auto *declRef = dyn_cast<DeclRefExpr>(arg)) {
            if (declRef->getDecl() == targetDecl) {
              // Found our target!
              visitedCallExprDeclRefExprs.insert(declRef);
              foundTypeInfo.applyUses.emplace_back(declRef->findOriginalType(),
                                                   isolationCrossing);
              return Action::Continue(expr);
            }
          }
        }
      }
    }

    return Action::Continue(expr);
  }
};

} // namespace

void InferredCallerArgumentTypeInfo::init(const Operand *op) {
  baseInferredType = op->get()->getType().getASTType();
  auto *nonConstOp = const_cast<Operand *>(op);

  auto loc = op->getUser()->getLoc();
  if (auto *sourceApply = loc.getAsASTNode<ApplyExpr>()) {
    return initForApply(op, sourceApply);
  }

  if (auto fas = FullApplySite::isa(nonConstOp->getUser())) {
    if (auto isolationCrossing = fas.getIsolationCrossing()) {
      return initForApply(*isolationCrossing);
    }
  }

  auto *autoClosureExpr = loc.getAsASTNode<AutoClosureExpr>();
  if (!autoClosureExpr) {
    llvm::report_fatal_error("Unknown node");
  }

  auto *i = const_cast<SILInstruction *>(op->getUser());
  auto pai = ApplySite::isa(i);
  unsigned captureIndex = pai.getAppliedArgIndex(*op);

  auto captureInfo =
      autoClosureExpr->getCaptureInfo().getCaptures()[captureIndex];
  auto *captureDecl = captureInfo.getDecl();
  Walker walker(*this, captureDecl);
  autoClosureExpr->walk(walker);
}

//===----------------------------------------------------------------------===//
//                       MARK: Instruction Level Model
//===----------------------------------------------------------------------===//

namespace {

constexpr const char *SEP_STR = "╾──────────────────────────────╼\n";

} // namespace

//===----------------------------------------------------------------------===//
//                          MARK: PartitionOpBuilder
//===----------------------------------------------------------------------===//

namespace {

struct PartitionOpBuilder {
  /// Parent translator that contains state.
  PartitionOpTranslator *translator;

  /// Used to statefully track the instruction currently being translated, for
  /// insertion into generated PartitionOps.
  SILInstruction *currentInst = nullptr;

  /// List of partition ops mapped to the current instruction. Used when
  /// generating partition ops.
  SmallVector<PartitionOp, 8> currentInstPartitionOps;

  void reset(SILInstruction *inst) {
    currentInst = inst;
    currentInstPartitionOps.clear();
  }

  Element lookupValueID(SILValue value);
  bool valueHasID(SILValue value, bool dumpIfHasNoID = false);

  Element getActorIntroducingRepresentative(SILIsolationInfo actorIsolation);

  void addAssignFresh(SILValue value) {
    currentInstPartitionOps.emplace_back(
        PartitionOp::AssignFresh(lookupValueID(value), currentInst));
  }

  void addAssign(SILValue tgt, SILValue src) {
    assert(valueHasID(src, /*dumpIfHasNoID=*/true) &&
           "source value of assignment should already have been encountered");

    Element srcID = lookupValueID(src);
    if (lookupValueID(tgt) == srcID) {
      LLVM_DEBUG(llvm::dbgs() << "    Skipping assign since tgt and src have "
                                 "the same representative.\n");
      LLVM_DEBUG(llvm::dbgs() << "    Rep ID: %%" << srcID.num << ".\n");
      return;
    }

    currentInstPartitionOps.emplace_back(PartitionOp::Assign(
        lookupValueID(tgt), lookupValueID(src), currentInst));
  }

  void addTransfer(SILValue representative, Operand *op) {
    assert(valueHasID(representative) &&
           "transferred value should already have been encountered");

    currentInstPartitionOps.emplace_back(
        PartitionOp::Transfer(lookupValueID(representative), op));
  }

  void addUndoTransfer(SILValue representative,
                       SILInstruction *untransferringInst) {
    assert(valueHasID(representative) &&
           "transferred value should already have been encountered");

    currentInstPartitionOps.emplace_back(PartitionOp::UndoTransfer(
        lookupValueID(representative), untransferringInst));
  }

  void addMerge(SILValue fst, SILValue snd) {
    assert(valueHasID(fst, /*dumpIfHasNoID=*/true) &&
           valueHasID(snd, /*dumpIfHasNoID=*/true) &&
           "merged values should already have been encountered");

    if (lookupValueID(fst) == lookupValueID(snd))
      return;

    currentInstPartitionOps.emplace_back(PartitionOp::Merge(
        lookupValueID(fst), lookupValueID(snd), currentInst));
  }

  /// Mark \p value artifically as being part of an actor isolated region by
  /// introducing a new fake actor introducing representative and merging them.
  void addActorIntroducingInst(SILValue value,
                               SILIsolationInfo actorIsolation) {
    assert(valueHasID(value, /*dumpIfHasNoID=*/true) &&
           "merged values should already have been encountered");

    auto elt = getActorIntroducingRepresentative(actorIsolation);
    currentInstPartitionOps.emplace_back(
        PartitionOp::AssignFresh(elt, currentInst));
    currentInstPartitionOps.emplace_back(
        PartitionOp::Merge(lookupValueID(value), elt, currentInst));
  }

  void addRequire(SILValue value) {
    assert(valueHasID(value, /*dumpIfHasNoID=*/true) &&
           "required value should already have been encountered");
    currentInstPartitionOps.emplace_back(
        PartitionOp::Require(lookupValueID(value), currentInst));
  }

  void addUnknownPatternError(SILValue value) {
    if (AbortOnUnknownPatternMatchError) {
      llvm::report_fatal_error(
          "RegionIsolation: Aborting on unknown pattern match error");
    }
    currentInstPartitionOps.emplace_back(
        PartitionOp::UnknownPatternError(lookupValueID(value), currentInst));
  }

  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

  void print(llvm::raw_ostream &os) const;
};

} // namespace

//===----------------------------------------------------------------------===//
//                     MARK: Top Level Translator Struct
//===----------------------------------------------------------------------===//

namespace {

enum class TranslationSemantics {
  /// An instruction that does not affect region based state or if it does we
  /// would like to error on some other use. An example would be something
  /// like end_borrow, inject_enum_addr, or alloc_global. We do not produce
  /// any partition op.
  Ignored,

  /// An instruction whose result produces a completely new region. E.x.:
  /// alloc_box, alloc_pack, key_path. This results in the translator
  /// producing a partition op that introduces the new region.
  AssignFresh,

  /// An instruction that merges together all of its operands regions and
  /// assigns all of its results to be that new merged region. If the
  /// instruction does not have any non-Sendable operands, we produce a new
  /// singular region that all of the results are assigned to.
  ///
  /// From a partition op perspective, we emit require partition ops for each
  /// of the operands of the instruction, then merge the operand partition
  /// ops. If we do not have any non-Sendable operands, we then create an
  /// assign fresh op for the first result. If we do, we assign the first
  /// result to that merged region. Regardless of the case, we then assign all
  /// of the rest of the results to the region of the first operand.
  Assign,

  /// An instruction that getUnderlyingTrackedValue looks through and thus we
  /// look through from a region translation perspective. The result of this
  /// is we do not produce a new partition op and just assert that
  /// getUnderlyingTrackedValue can look through the instruction.
  LookThrough,

  /// Require that the region associated with a value not be consumed at this
  /// program point.
  Require,

  /// A "CopyLikeInstruction" with a Dest and Src operand value. If the store
  /// is considered to be to unaliased store (computed through a combination
  /// of AccessStorage's isUniquelyIdentified check and a custom search for
  /// captures by partial apply), then we treat this like an assignment of src
  /// to dest and emit assign partition ops. Otherwise, we emit merge
  /// partition ops so that we merge the old region of dest into the new src
  /// region. This ensures in the case of our value being captured by a
  /// closure, we do not lose that the closure could affect the memory
  /// location.
  Store,

  /// An instruction that is not handled in a standardized way. Examples:
  ///
  /// 1. Select insts.
  /// 2. Instructions without a constant way of being handled that change
  ///    their behavior depending on some state on the instruction itself.
  Special,

  /// An instruction that is a full apply site. Can cause transfering or
  /// untransferring of regions.
  Apply,

  /// A terminator instruction that acts like a phi in terms of its region.
  TerminatorPhi,

  /// An instruction that we should never see and if we do see, we should assert
  /// upon. This is generally used for non-Ownership SSA instructions and
  /// instructions that can only appear in Lowered SIL. Even if we should never
  /// see one of these instructions, we would still like to ensure that we
  /// handle every instruction to ensure we cover the IR.
  Asserting,

  /// An instruction that the checker thinks it can ignore as long as all of its
  /// operands are Sendable. If we see that such an instruction has a
  /// non-Sendable parameter, then someone added an instruction to the compiler
  /// without updating this code correctly. This is most likely driver error and
  /// should be caught in testing when we assert.
  AssertingIfNonSendable,

  /// Instructions that always unconditionally transfer all of their
  /// non-Sendable parameters and that do not have any results.
  TransferringNoResult,
};

} // namespace

namespace llvm {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              TranslationSemantics semantics) {
  switch (semantics) {
  case TranslationSemantics::Ignored:
    os << "ignored";
    return os;
  case TranslationSemantics::AssignFresh:
    os << "assign_fresh";
    return os;
  case TranslationSemantics::Assign:
    os << "assign";
    return os;
  case TranslationSemantics::LookThrough:
    os << "look_through";
    return os;
  case TranslationSemantics::Require:
    os << "require";
    return os;
  case TranslationSemantics::Store:
    os << "store";
    return os;
  case TranslationSemantics::Special:
    os << "special";
    return os;
  case TranslationSemantics::Apply:
    os << "apply";
    return os;
  case TranslationSemantics::TerminatorPhi:
    os << "terminator_phi";
    return os;
  case TranslationSemantics::Asserting:
    os << "asserting";
    return os;
  case TranslationSemantics::AssertingIfNonSendable:
    os << "asserting_if_nonsendable";
    return os;
  case TranslationSemantics::TransferringNoResult:
    os << "transferring_no_result";
    return os;
  }

  llvm_unreachable("Covered switch isn't covered?!");
}

} // namespace llvm

namespace swift {
namespace regionanalysisimpl {

/// PartitionOpTranslator is responsible for performing the translation from
/// SILInstructions to PartitionOps. Not all SILInstructions have an effect on
/// the region partition, and some have multiple effects - such as an
/// application pairwise merging its arguments - so the core functions like
/// translateSILBasicBlock map SILInstructions to std::vectors of PartitionOps.
/// No more than a single instance of PartitionOpTranslator should be used for
/// each SILFunction, as SILValues are assigned unique IDs through the
/// nodeIDMap. Some special correspondences between SIL values are also tracked
/// statefully by instances of this class, such as the "projection"
/// relationship: instructions like begin_borrow and begin_access create
/// effectively temporary values used for alternative access to base "projected"
/// values. These are tracked to implement "write-through" semantics for
/// assignments to projections when they're addresses.
///
/// TODO: when translating basic blocks, optimizations might be possible
///       that reduce lists of PartitionOps to smaller, equivalent lists
class PartitionOpTranslator {
  friend PartitionOpBuilder;

  SILFunction *function;

  /// A cache of argument IDs.
  std::optional<Partition> functionArgPartition;

  /// A builder struct that we use to convert individual instructions into lists
  /// of PartitionOps.
  PartitionOpBuilder builder;

  PartialApplyReachabilityDataflow partialApplyReachabilityDataflow;

  RegionAnalysisValueMap &valueMap;

  void gatherFlowInsensitiveInformationBeforeDataflow() {
    LLVM_DEBUG(llvm::dbgs() << ">>> Performing pre-dataflow scan to gather "
                               "flow insensitive information "
                            << function->getName() << ":\n");

    for (auto &block : *function) {
      for (auto &inst : block) {
        if (auto *pai = dyn_cast<PartialApplyInst>(&inst)) {
          ApplySite applySite(pai);
          for (Operand &op : applySite.getArgumentOperands()) {
            // See if this operand is inout_aliasable or is passed as a box. In
            // such a case, we are passing by reference so we need to add it to
            // the reachability.
            if (applySite.getArgumentConvention(op) ==
                    SILArgumentConvention::Indirect_InoutAliasable ||
                op.get()->getType().is<SILBoxType>())
              partialApplyReachabilityDataflow.add(&op);

            // See if this instruction is a partial apply whose non-sendable
            // address operands we need to mark as captured_uniquely identified.
            //
            // If we find an address or a box of a non-Sendable type that is
            // passed to a partial_apply, mark the value's representative as
            // being uniquely identified and captured.
            SILValue val = op.get();
            if (val->getType().isAddress() &&
                isNonSendableType(val->getType())) {
              auto trackVal = getTrackableValue(val, true);
              (void)trackVal;
              LLVM_DEBUG(trackVal.print(llvm::dbgs()));
              continue;
            }
            if (auto *pbi = dyn_cast<ProjectBoxInst>(val)) {
              if (isNonSendableType(pbi->getType())) {
                auto trackVal = getTrackableValue(val, true);
                (void)trackVal;
                continue;
              }
            }
          }
        }
      }
    }

    // Once we have finished processing all blocks, propagate reachability.
    partialApplyReachabilityDataflow.propagateReachability();
  }

public:
  PartitionOpTranslator(SILFunction *function, PostOrderFunctionInfo *pofi,
                        RegionAnalysisValueMap &valueMap,
                        IsolationHistory::Factory &historyFactory)
      : function(function), functionArgPartition(), builder(),
        partialApplyReachabilityDataflow(function, pofi), valueMap(valueMap) {
    builder.translator = this;
    gatherFlowInsensitiveInformationBeforeDataflow();

    LLVM_DEBUG(llvm::dbgs() << "Initializing Function Args:\n");
    auto functionArguments = function->getArguments();
    if (functionArguments.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "    None.\n");
      functionArgPartition = Partition::singleRegion(SILLocation::invalid(), {},
                                                     historyFactory.get());
      return;
    }

    llvm::SmallVector<Element, 8> nonSendableJoinedIndices;
    llvm::SmallVector<Element, 8> nonSendableSeparateIndices;
    for (SILArgument *arg : functionArguments) {
      // This will decide what the isolation region is.
      if (auto state = tryToTrackValue(arg)) {
        // If we can transfer our parameter, just add it to
        // nonSendableSeparateIndices.
        //
        // NOTE: We do not support today the ability to have multiple parameters
        // transfer together as part of the same region.
        if (isTransferrableFunctionArgument(cast<SILFunctionArgument>(arg))) {
          LLVM_DEBUG(llvm::dbgs() << "    %%" << state->getID()
                                  << " (transferring): " << *arg);
          nonSendableSeparateIndices.push_back(state->getID());
          continue;
        }

        // Otherwise, it is one of our merged parameters. Add it to the never
        // transfer list and to the region join list.
        LLVM_DEBUG(llvm::dbgs() << "    %%" << state->getID() << ": ";
                   state->print(llvm::dbgs()); llvm::dbgs() << *arg);
        nonSendableJoinedIndices.push_back(state->getID());
      } else {
        LLVM_DEBUG(llvm::dbgs() << "    Sendable: " << *arg);
      }
    }

    functionArgPartition = Partition::singleRegion(
        SILLocation::invalid(), nonSendableJoinedIndices, historyFactory.get());
    for (Element elt : nonSendableSeparateIndices) {
      functionArgPartition->trackNewElement(elt);
    }
  }

  bool isClosureCaptured(SILValue value, SILInstruction *inst) const {
    return partialApplyReachabilityDataflow.isReachable(value, inst);
  }

  std::optional<TrackableValue> getValueForId(Element id) const {
    return valueMap.getValueForId(id);
  }

  RegionAnalysisValueMap &getValueMap() const { return valueMap; }

private:
  /// Check if the passed in type is NonSendable.
  ///
  /// NOTE: We special case RawPointer and NativeObject to ensure they are
  /// treated as non-Sendable and strict checking is applied to it.
  bool isNonSendableType(SILType type) const {
    return SILIsolationInfo::isNonSendableType(type, function);
  }

  TrackableValue
  getTrackableValue(SILValue value,
                    bool isAddressCapturedByPartialApply = false) {
    return valueMap.getTrackableValue(value, isAddressCapturedByPartialApply);
  }

  std::optional<TrackableValue> tryToTrackValue(SILValue value) const {
    return valueMap.tryToTrackValue(value);
  }

  /// If \p value already has state associated with it, return that. Otherwise
  /// begin tracking \p value and initialize its isolation info to be \p.
  ///
  /// This is in contrast to tryToTrackValue which infers isolation
  /// information. This should only be used in exceptional cases when one 100%
  /// knows what the isolation is already. E.x.: a partial_apply.
  std::optional<std::pair<TrackableValue, bool>>
  initializeTrackedValue(SILValue value, SILIsolationInfo info) const {
    auto trackedValuePair = valueMap.initializeTrackableValue(value, info);

    // If we have a Sendable value return none.
    if (!trackedValuePair.first.isNonSendable())
      return {};

    return trackedValuePair;
  }

  TrackableValue
  getActorIntroducingRepresentative(SILInstruction *introducingInst,
                                    SILIsolationInfo actorIsolation) const {
    return valueMap.getActorIntroducingRepresentative(introducingInst,
                                                      actorIsolation);
  }

  bool valueHasID(SILValue value, bool dumpIfHasNoID = false) {
    return valueMap.valueHasID(value, dumpIfHasNoID);
  }

  Element lookupValueID(SILValue value) {
    return valueMap.lookupValueID(value);
  }

public:
  /// Return the partition consisting of all function arguments.
  ///
  /// Used to initialize the entry blocko of our analysis.
  const Partition &getEntryPartition() const { return *functionArgPartition; }

  /// Get the results of an apply instruction.
  ///
  /// This is the single result value for most apply instructions, but for try
  /// apply it is the two arguments to each succ block.
  void getApplyResults(const SILInstruction *inst,
                       SmallVectorImpl<SILValue> &foundResults) {
    if (isa<ApplyInst, BeginApplyInst, BuiltinInst, PartialApplyInst>(inst)) {
      copy(inst->getResults(), std::back_inserter(foundResults));
      return;
    }

    if (auto tryApplyInst = dyn_cast<TryApplyInst>(inst)) {
      foundResults.emplace_back(tryApplyInst->getNormalBB()->getArgument(0));
      if (tryApplyInst->getErrorBB()->getNumArguments() > 0) {
        foundResults.emplace_back(tryApplyInst->getErrorBB()->getArgument(0));
      }
      return;
    }

    llvm::report_fatal_error("all apply instructions should be covered");
  }

  /// Require all non-sendable sources, merge their regions, and assign the
  /// resulting region to all non-sendable targets, or assign non-sendable
  /// targets to a fresh region if there are no non-sendable sources.
  ///
  /// \arg isolationInfo An isolation info that can be specified as the true
  /// base isolation of results. Otherwise, results are assumed to have a
  /// element isolation of disconnected. NOTE: The results will still be in the
  /// region of the non-Sendable arguments so at the region level they will have
  /// the same value.
  template <typename TargetRange, typename SourceRange>
  void
  translateSILMultiAssign(const TargetRange &resultValues,
                          const SourceRange &sourceValues,
                          SILIsolationInfo resultIsolationInfoOverride = {},
                          bool requireSrcValues = true) {
    SmallVector<SILValue, 8> assignOperands;
    SmallVector<SILValue, 8> assignResults;

    // A helper we use to emit an unknown patten error if our merge is
    // invalid. This ensures we guarantee that if we find an actor merge error,
    // the compiler halts. Importantly this lets our users know 100% that if the
    // compiler exits successfully, actor merge errors could not have happened.
    std::optional<SILDynamicMergedIsolationInfo> mergedInfo;
    if (resultIsolationInfoOverride) {
      mergedInfo = resultIsolationInfoOverride;
    } else {
      mergedInfo = SILIsolationInfo::getDisconnected(false);
    }

    for (SILValue src : sourceValues) {
      if (auto value = tryToTrackValue(src)) {
        assignOperands.push_back(value->getRepresentative().getValue());
        auto originalMergedInfo = mergedInfo;
        (void)originalMergedInfo;
        if (mergedInfo)
          mergedInfo = mergedInfo->merge(value->getIsolationRegionInfo());

        // If we fail to merge, then we have an incompatibility in between some
        // of our arguments (consider isolated to different actors) or with the
        // isolationInfo we specified. Emit an unknown patten error.
        if (!mergedInfo) {
          LLVM_DEBUG(
              llvm::dbgs() << "Merge Failure!\n"
                           << "Original Info: ";
              if (originalMergedInfo)
                  originalMergedInfo->printForDiagnostics(llvm::dbgs());
              else llvm::dbgs() << "nil";
              llvm::dbgs() << "\nValue: "
                           << value->getRepresentative().getValue();
              llvm::dbgs() << "Value Info: ";
              value->getIsolationRegionInfo().printForDiagnostics(llvm::dbgs());
              llvm::dbgs() << "\n");
          builder.addUnknownPatternError(src);
          continue;
        }
      }
    }

    for (SILValue result : resultValues) {
      // If we had isolation info explicitly passed in... use our
      // resultIsolationInfoError. Otherwise, we want to infer.
      if (resultIsolationInfoOverride) {
        // We only get back result if it is non-Sendable.
        if (auto nonSendableValue =
                initializeTrackedValue(result, resultIsolationInfoOverride)) {
          // If we did not insert, emit an unknown patten error.
          if (!nonSendableValue->second) {
            builder.addUnknownPatternError(result);
          }
          assignResults.push_back(
              nonSendableValue->first.getRepresentative().getValue());
        }
      } else {
        if (auto value = tryToTrackValue(result)) {
          assignResults.push_back(value->getRepresentative().getValue());
        }
      }
    }

    // Require all srcs if we are supposed to. (By default we do).
    //
    // DISCUSSION: The reason that this may be useful is for special
    // instructions like store_borrow. On the one hand, we want store_borrow to
    // act like a store in the sense that we want to combine the regions of its
    // src and dest... but at the same time, we do not want to treat the store
    // itself as a use of its parent value. We want that to be any subsequent
    // uses of the store_borrow.
    if (requireSrcValues)
      for (auto src : assignOperands)
        builder.addRequire(src);

    // Merge all srcs.
    for (unsigned i = 1; i < assignOperands.size(); i++) {
      builder.addMerge(assignOperands[i - 1], assignOperands[i]);
    }

    // If we do not have any non sendable results, return early.
    if (assignResults.empty()) {
      // If we did not have any non-Sendable results and we did have
      // non-Sendable operands and we are supposed to mark value as actor
      // derived, introduce a fake element so we just propagate the actor
      // region.
      //
      // NOTE: Here we check if we have resultIsolationInfoOverride rather than
      // isolationInfo since we want to do this regardless of whether or not we
      // passed in a specific isolation info unlike earlier when processing
      // actual results.
      if (assignOperands.size() && resultIsolationInfoOverride) {
        builder.addActorIntroducingInst(assignOperands.back(),
                                        resultIsolationInfoOverride);
      }

      return;
    }

    auto assignResultsRef = llvm::ArrayRef(assignResults);
    SILValue front = assignResultsRef.front();
    assignResultsRef = assignResultsRef.drop_front();

    if (assignOperands.empty()) {
      // If no non-sendable srcs, non-sendable tgts get a fresh region.
      builder.addAssignFresh(front);
    } else {
      builder.addAssign(front, assignOperands.front());
    }

    // Assign all targets to the target region.
    while (assignResultsRef.size()) {
      SILValue next = assignResultsRef.front();
      assignResultsRef = assignResultsRef.drop_front();

      builder.addAssign(next, front);
    }
  }

  /// Transfer the parameters of our partial_apply.
  ///
  /// Handling async let has three-four parts:
  ///
  /// %partial_apply = partial_apply()
  /// %reabstraction = maybe reabstraction thunk of partial_apply
  /// builtin "async let start"(%reabstraction | %partial_apply)
  /// call %asyncLetGet()
  ///
  /// We transfer the captured parameters of %partial_apply at the async let
  /// start and then untransfer them at async let get.
  void translateAsyncLetStart(BuiltinInst *bi) {
    // Just track the result of the builtin inst as an assign fresh. We do this
    // so we properly track the partial_apply get. We already transferred the
    // parameters.
    builder.addAssignFresh(bi);
  }

  /// For discussion on how we handle async let, please see the comment on
  /// translateAsyncLetStart.
  void translateAsyncLetGet(ApplyInst *ai) {
    // This looks through reabstraction thunks.
    auto source = findAsyncLetPartialApplyFromGet(ai);
    assert(source.has_value());

    // If we didn't find a partial_apply, then we must have had a
    // thin_to_thick_function meaning we did not capture anything.
    if (source->is<ThinToThickFunctionInst *>())
      return;

    // If our partial_apply was Sendable, then Sema should have checked that
    // none of our captures were non-Sendable and we should have emitted an
    // error earlier.
    assert(bool(source.value()) &&
           "AsyncLet Get should always have a derivable partial_apply");
    auto *pai = source->get<PartialApplyInst *>();
    if (pai->getFunctionType()->isSendable())
      return;

    ApplySite applySite(pai);
    // For each of our partial apply operands...
    for (auto pair : llvm::enumerate(applySite.getArgumentOperands())) {
      Operand &op = pair.value();

      // If we are tracking the value...
      if (auto trackedArgValue = tryToTrackValue(op.get())) {

        // Gather the isolation info from the AST for this operand...
        InferredCallerArgumentTypeInfo typeInfo;
        typeInfo.init(&op);

        // Then see if /any/ of our uses are passed to over a isolation boundary
        // that is actor isolated... if we find one continue so we do not undo
        // the transfer for that element.
        if (llvm::any_of(
                typeInfo.applyUses,
                [](const std::pair<Type, std::optional<ApplyIsolationCrossing>>
                       &data) {
                  // If we do not have an apply isolation crossing, we just use
                  // undefined crossing since that is considered nonisolated.
                  ApplyIsolationCrossing crossing;
                  return data.second.value_or(crossing)
                      .CalleeIsolation.isActorIsolated();
                }))
          continue;

        builder.addUndoTransfer(trackedArgValue->getRepresentative().getValue(),
                                ai);
      }
    }
  }

  void translateSILPartialApplyAsyncLetBegin(PartialApplyInst *pai) {
    LLVM_DEBUG(llvm::dbgs() << "Translating Async Let Begin Partial Apply!\n");
    // Grab our partial apply and transfer all of its non-sendable
    // parameters. We do not merge the parameters since each individual capture
    // of the async let at the program level is viewed as still being in
    // separate regions. Otherwise, we would need to error on the following
    // code:
    //
    // let x = NonSendable(), x2 = NonSendable()
    // async let y = transferToActor(x) + transferToNonIsolated(x2)
    // _ = await y
    // useValue(x2)
    for (auto &op : ApplySite(pai).getArgumentOperands()) {
      if (auto trackedArgValue = tryToTrackValue(op.get())) {
        builder.addRequire(trackedArgValue->getRepresentative().getValue());
        builder.addTransfer(trackedArgValue->getRepresentative().getValue(),
                            &op);
      }
    }

    // Then mark our partial_apply result as being returned fresh.
    builder.addAssignFresh(pai);
  }

  /// Handles the semantics for SIL applies that cross isolation.
  ///
  /// Semantically this causes all arguments of the applysite to be transferred.
  void translateIsolatedPartialApply(PartialApplyInst *pai,
                                     SILIsolationInfo actorIsolation) {
    ApplySite applySite(pai);
    LLVM_DEBUG(llvm::dbgs() << "Translating Isolated Partial Apply!\n");

    // For each argument operand.
    for (auto &op : applySite.getArgumentOperands()) {
      // See if we tracked it.
      if (auto value = tryToTrackValue(op.get())) {
        // If we are tracking it, transfer it and if it is actor derived, mark
        // our partial apply as actor derived.
        builder.addRequire(value->getRepresentative().getValue());
        builder.addTransfer(value->getRepresentative().getValue(), &op);
      }
    }

    // Now that we have transferred everything into the partial_apply, perform
    // an assign fresh for the partial_apply if it is non-Sendable. If we use
    // any of the transferred values later, we will error, so it is safe to just
    // create a new value.
    if (pai->getFunctionType()->isSendable())
      return;

    auto trackedValue = initializeTrackedValue(pai, actorIsolation);
    assert(trackedValue && "Should not have happened already");
    assert(
        trackedValue->second &&
        "Insert should have happened since this could not happen before this");
    translateSILAssignFresh(trackedValue->first.getRepresentative().getValue());
  }

  void translateSILPartialApply(PartialApplyInst *pai) {
    // First check if our partial apply is Sendable and not global actor
    // isolated. In such a case, we will have emitted an earlier warning in Sema
    // and can return early. If we have a global actor isolated partial_apply,
    // we can be looser and can use region isolation since we know that the
    // Sendable closure will be executed serially due to the closure having to
    // run on the global actor queue meaning that we do not have to worry about
    // the Sendable closure being run concurrency.
    if (pai->getFunctionType()->isSendableType()) {
      auto isolationInfo = SILIsolationInfo::get(pai);
      if (!isolationInfo || !isolationInfo.hasActorIsolation() ||
          !isolationInfo.getActorIsolation().isGlobalActor())
        return;
    }

    // Then check if our partial_apply is fed into an async let begin. If so,
    // handle it especially.
    //
    // NOTE: If it is an async_let, then the closure itself will be Sendable. We
    // treat passing in a value into the async Sendable closure as transferring
    // it into the closure.
    if (isAsyncLetBeginPartialApply(pai)) {
      return translateSILPartialApplyAsyncLetBegin(pai);
    }

    // See if we have a reabstraction thunk. In such a case, just do an assign.
    if (auto *calleeFn = pai->getCalleeFunction()) {
      if (calleeFn->isThunk() == IsReabstractionThunk) {
        return translateSILAssign(pai);
      }
    }

    if (auto isolationRegionInfo = SILIsolationInfo::get(pai)) {
      return translateIsolatedPartialApply(pai, isolationRegionInfo);
    }

    SmallVector<SILValue, 8> applyResults;
    getApplyResults(pai, applyResults);
    translateSILMultiAssign(applyResults, pai->getOperandValues());
  }

  void translateCreateAsyncTask(BuiltinInst *bi) {
    if (auto value = tryToTrackValue(bi->getOperand(1))) {
      builder.addRequire(value->getRepresentative().getValue());
      builder.addTransfer(value->getRepresentative().getValue(),
                          &bi->getAllOperands()[1]);
    }
  }

  void translateSILBuiltin(BuiltinInst *bi) {
    if (auto kind = bi->getBuiltinKind()) {
      if (kind == BuiltinValueKind::StartAsyncLetWithLocalBuffer) {
        return translateAsyncLetStart(bi);
      }

      if (kind == BuiltinValueKind::CreateAsyncTask) {
        return translateCreateAsyncTask(bi);
      }
    }

    // If we do not have a special builtin, just do a multi-assign. Builtins do
    // not cross async boundaries.
    return translateSILMultiAssign(bi->getResults(), bi->getOperandValues());
  }

  void translateNonIsolationCrossingSILApply(FullApplySite fas) {
    // For non-self parameters, gather all of the transferring parameters and
    // gather our non-transferring parameters.
    SmallVector<SILValue, 8> nonTransferringParameters;
    if (fas.getNumArguments()) {
      // NOTE: We want to process indirect parameters as if they are
      // parameters... so we process them in nonTransferringParameters.
      for (auto &op : fas.getOperandsWithoutSelf()) {
        // If op is the callee operand, skip it.
        if (fas.isCalleeOperand(op))
          continue;

        if (!fas.getArgumentConvention(op).isIndirectOutParameter() &&
            fas.getArgumentParameterInfo(op).hasOption(
                SILParameterInfo::Sending)) {
          if (auto value = tryToTrackValue(op.get())) {
            builder.addRequire(value->getRepresentative().getValue());
            builder.addTransfer(value->getRepresentative().getValue(), &op);
          }
        } else {
          nonTransferringParameters.push_back(op.get());
        }
      }
    }

    // If our self parameter was transferring, transfer it. Otherwise, just
    // stick it in the non self operand values array and run multiassign on
    // it.
    if (fas.hasSelfArgument()) {
      auto &selfOperand = fas.getSelfArgumentOperand();
      if (fas.getArgumentParameterInfo(selfOperand)
              .hasOption(SILParameterInfo::Sending)) {
        if (auto value = tryToTrackValue(selfOperand.get())) {
          builder.addRequire(value->getRepresentative().getValue());
          builder.addTransfer(value->getRepresentative().getValue(),
                              &selfOperand);
        }
      } else {
        nonTransferringParameters.push_back(selfOperand.get());
      }
    }

    // Require our callee operand if it is non-Sendable.
    //
    // DISCUSSION: Even though we do not include our callee operand in the same
    // region as our operands/results, we still need to require that it is live
    // at the point of application. Otherwise, we will not emit errors if the
    // closure before this function application is already in the same region as
    // a transferred value. In such a case, the function application must error.
    if (auto value = tryToTrackValue(fas.getCallee())) {
      builder.addRequire(value->getRepresentative().getValue());
    }

    SmallVector<SILValue, 8> applyResults;
    getApplyResults(*fas, applyResults);

    auto type = fas.getSubstCalleeSILType().castTo<SILFunctionType>();
    auto isolationInfo = SILIsolationInfo::get(*fas);

    // If our result is not transferring, just do the normal multi-assign.
    if (!type->hasSendingResult()) {
      return translateSILMultiAssign(applyResults, nonTransferringParameters,
                                     isolationInfo);
    }

    // If our result is transferring, then pass in empty as our results, no
    // override isolation, then perform assign fresh.
    ArrayRef<SILValue> empty;
    translateSILMultiAssign(empty, nonTransferringParameters, {});
    for (SILValue result : applyResults) {
      if (auto value = tryToTrackValue(result)) {
        builder.addAssignFresh(value->getRepresentative().getValue());
      }
    }
  }

  void translateSILApply(SILInstruction *inst) {
    // Handles normal builtins and async let start.
    if (auto *bi = dyn_cast<BuiltinInst>(inst)) {
      return translateSILBuiltin(bi);
    }

    auto fas = FullApplySite::isa(inst);
    assert(bool(fas) && "Builtins should be handled above");

    // Handle async let get.
    if (auto *f = fas.getCalleeFunction()) {
      if (f->getName() == "swift_asyncLet_get") {
        return translateAsyncLetGet(cast<ApplyInst>(*fas));
      }
    }

    // If this apply does not cross isolation domains, it has normal
    // non-transferring multi-assignment semantics
    if (!getApplyIsolationCrossing(*fas))
      return translateNonIsolationCrossingSILApply(fas);

    if (auto cast = dyn_cast<ApplyInst>(inst))
      return translateIsolationCrossingSILApply(cast);
    if (auto cast = dyn_cast<BeginApplyInst>(inst))
      return translateIsolationCrossingSILApply(cast);
    if (auto cast = dyn_cast<TryApplyInst>(inst))
      return translateIsolationCrossingSILApply(cast);

    llvm_unreachable("Only ApplyInst, BeginApplyInst, and TryApplyInst should "
                     "cross isolation domains");
  }

  /// Handles the semantics for SIL applies that cross isolation.
  ///
  /// Semantically this causes all arguments of the applysite to be transferred.
  void translateIsolationCrossingSILApply(FullApplySite applySite) {
    // Require all operands first before we emit transferring.
    for (auto op : applySite.getArguments())
      if (auto value = tryToTrackValue(op))
        builder.addRequire(value->getRepresentative().getValue());

    auto handleSILOperands = [&](MutableArrayRef<Operand> operands) {
      for (auto &op : operands) {
        if (auto value = tryToTrackValue(op.get())) {
          builder.addTransfer(value->getRepresentative().getValue(), &op);
        }
      }
    };

    auto handleSILSelf = [&](Operand *self) {
      if (auto value = tryToTrackValue(self->get())) {
        builder.addTransfer(value->getRepresentative().getValue(), self);
      }
    };

    if (applySite.hasSelfArgument()) {
      handleSILOperands(applySite.getOperandsWithoutIndirectResultsOrSelf());
      handleSILSelf(&applySite.getSelfArgumentOperand());
    } else {
      handleSILOperands(applySite.getOperandsWithoutIndirectResults());
    }

    // non-sendable results can't be returned from cross-isolation calls without
    // a diagnostic emitted elsewhere. Here, give them a fresh value for better
    // diagnostics hereafter
    SmallVector<SILValue, 8> applyResults;
    getApplyResults(*applySite, applyResults);
    for (auto result : applyResults)
      if (auto value = tryToTrackValue(result))
        builder.addAssignFresh(value->getRepresentative().getValue());
  }

  template <typename DestValues>
  void translateSILLookThrough(DestValues destValues, SILValue src) {
    auto srcID = tryToTrackValue(src);

    for (SILValue dest : destValues) {
      auto destID = tryToTrackValue(dest);
      assert(((!destID || !srcID) || destID->getID() == srcID->getID()) &&
             "srcID and dstID are different?!");
    }
  }

  /// Add a look through operation. This asserts that dest and src map to the
  /// same ID. Should only be used on instructions that are always guaranteed to
  /// have this property due to getUnderlyingTrackedValue looking through them.
  ///
  /// DISCUSSION: We use this to ensure that the connection in between
  /// getUnderlyingTrackedValue and these instructions is enforced and
  /// explicit. Previously, we always called translateSILAssign and relied on
  /// the builder to recognize these cases and not create an assign
  /// PartitionOp. Doing such a thing obscures what is actually happening.
  template <>
  void translateSILLookThrough<SILValue>(SILValue dest, SILValue src) {
    auto srcID = tryToTrackValue(src);
    auto destID = tryToTrackValue(dest);
    assert(((!destID || !srcID) || destID->getID() == srcID->getID()) &&
           "srcID and dstID are different?!");
  }

  void translateSILLookThrough(SingleValueInstruction *svi) {
    assert(svi->getNumRealOperands() == 1);
    auto srcID = tryToTrackValue(svi->getOperand(0));
    auto destID = tryToTrackValue(svi);
    assert(((!destID || !srcID) || destID->getID() == srcID->getID()) &&
           "srcID and dstID are different?!");
  }

  template <typename Collection>
  void translateSILAssign(SILValue dest, Collection collection) {
    return translateSILMultiAssign(TinyPtrVector<SILValue>(dest), collection);
  }

  template <>
  void translateSILAssign<SILValue>(SILValue dest, SILValue src) {
    return translateSILAssign(dest, TinyPtrVector<SILValue>(src));
  }

  void translateSILAssign(SILInstruction *inst) {
    return translateSILMultiAssign(inst->getResults(),
                                   inst->getOperandValues());
  }

  /// If the passed SILValue is NonSendable, then create a fresh region for it,
  /// otherwise do nothing.
  ///
  /// By default this is initialized with disconnected isolation info unless \p
  /// isolationInfo is set.
  void translateSILAssignFresh(SILValue val) {
    return translateSILMultiAssign(TinyPtrVector<SILValue>(val),
                                   TinyPtrVector<SILValue>());
  }

  void translateSILAssignFresh(SILValue val, SILIsolationInfo info) {
    auto v = initializeTrackedValue(val, info);
    if (!v)
      return translateSILAssignFresh(val);

    if (!v->second)
      return translateUnknownPatternError(val);

    return translateSILAssignFresh(v->first.getRepresentative().getValue());
  }

  template <typename Collection>
  void translateSILMerge(SILValue dest, Collection collection,
                         bool requireOperands = true) {
    auto trackableDest = tryToTrackValue(dest);
    if (!trackableDest)
      return;
    for (SILValue elt : collection) {
      if (auto trackableSrc = tryToTrackValue(elt)) {
        if (requireOperands) {
          builder.addRequire(trackableSrc->getRepresentative().getValue());
          builder.addRequire(trackableDest->getRepresentative().getValue());
        }
        builder.addMerge(trackableDest->getRepresentative().getValue(),
                         trackableSrc->getRepresentative().getValue());
      }
    }
  }

  template <>
  void translateSILMerge<SILValue>(SILValue dest, SILValue src,
                                   bool requireOperands) {
    return translateSILMerge(dest, TinyPtrVector<SILValue>(src),
                             requireOperands);
  }

  void translateSILAssignmentToTransferringParameter(TrackableValue destRoot,
                                                     Operand *destOperand,
                                                     TrackableValue srcRoot,
                                                     Operand *srcOperand) {
    assert(isa<AllocStackInst>(destRoot.getRepresentative().getValue()) &&
           "Destination should always be an alloc_stack");

    // Transfer src. This ensures that we cannot use src again locally in this
    // function... which makes sense since its value is now in the transferring
    // parameter.
    builder.addRequire(srcRoot.getRepresentative().getValue());
    builder.addTransfer(srcRoot.getRepresentative().getValue(), srcOperand);

    // Then check if we are assigning into an aggregate projection. In such a
    // case, we want to ensure that we keep tracking the elements already in the
    // region of transferring. This is more conservative than we need to be
    // (since we could forget anything reachable from the aggregate
    // field)... but being more conservative is ok.
    if (isProjectedFromAggregate(destOperand->get()))
      return;

    // If we are assigning over the entire value though, we perform an assign
    // fresh since we are guaranteed that any value that could be referenced via
    // the old value is gone.
    builder.addAssignFresh(destRoot.getRepresentative().getValue());
  }

  /// If \p dest is known to be unaliased (computed through a combination of
  /// AccessStorage's inUniquelyIdenfitied check and a custom search for
  /// captures by applications), then these can be treated as assignments of \p
  /// dest to src. If the \p dest could be aliased, then we must instead treat
  /// them as merges, to ensure any aliases of \p dest are also updated.
  void translateSILStore(Operand *dest, Operand *src) {
    SILValue destValue = dest->get();
    SILValue srcValue = src->get();

    if (auto nonSendableDest = tryToTrackValue(destValue)) {
      // In the following situations, we can perform an assign:
      //
      // 1. A store to unaliased storage.
      // 2. A store that is to an entire value.
      //
      // DISCUSSION: If we have case 2, we need to merge the regions since we
      // are not overwriting the entire region of the value. This does mean that
      // we artificially include the previous region that was stored
      // specifically in this projection... but that is better than
      // miscompiling. For memory like this, we probably need to track it on a
      // per field basis to allow for us to assign.
      if (nonSendableDest.value().isNoAlias() &&
          !isProjectedFromAggregate(destValue))
        return translateSILAssign(destValue, srcValue);

      // Stores to possibly aliased storage must be treated as merges.
      return translateSILMerge(destValue, srcValue);
    }

    // Stores to storage of non-Sendable type can be ignored.
  }

  void translateSILTupleAddrConstructor(TupleAddrConstructorInst *inst) {
    SILValue dest = inst->getDest();
    if (auto nonSendableTgt = tryToTrackValue(dest)) {
      // In the following situations, we can perform an assign:
      //
      // 1. A store to unaliased storage.
      // 2. A store that is to an entire value.
      //
      // DISCUSSION: If we have case 2, we need to merge the regions since we
      // are not overwriting the entire region of the value. This does mean that
      // we artificially include the previous region that was stored
      // specifically in this projection... but that is better than
      // miscompiling. For memory like this, we probably need to track it on a
      // per field basis to allow for us to assign.
      if (nonSendableTgt.value().isNoAlias() && !isProjectedFromAggregate(dest))
        return translateSILAssign(dest, inst->getElements());

      // Stores to possibly aliased storage must be treated as merges.
      return translateSILMerge(dest, inst->getElements());
    }

    // Stores to storage of non-Sendable type can be ignored.
  }

  void translateSILRequire(SILValue val) {
    if (auto nonSendableVal = tryToTrackValue(val))
      return builder.addRequire(nonSendableVal->getRepresentative().getValue());
  }

  /// An enum select is just a multi assign.
  void translateSILSelectEnum(SelectEnumOperation selectEnumInst) {
    SmallVector<SILValue, 8> enumOperands;
    for (unsigned i = 0; i < selectEnumInst.getNumCases(); i++)
      enumOperands.push_back(selectEnumInst.getCase(i).second);
    if (selectEnumInst.hasDefault())
      enumOperands.push_back(selectEnumInst.getDefaultResult());
    return translateSILMultiAssign(
        TinyPtrVector<SILValue>(selectEnumInst->getResult(0)), enumOperands);
  }

  void translateSILSwitchEnum(SwitchEnumInst *switchEnumInst) {
    TermArgSources argSources;

    // accumulate each switch case that branches to a basic block with an arg
    for (unsigned i = 0; i < switchEnumInst->getNumCases(); i++) {
      SILBasicBlock *dest = switchEnumInst->getCase(i).second;
      if (dest->getNumArguments() > 0) {
        assert(dest->getNumArguments() == 1 &&
               "expected at most one bb arg in dest of enum switch");
        argSources.addValues({switchEnumInst->getOperand()}, dest);
      }
    }

    translateSILPhi(argSources);
  }

  // translate a SIL instruction corresponding to possible branches with args
  // to one or more basic blocks. This is the SIL equivalent of SSA Phi nodes.
  // each element of `branches` corresponds to the arguments passed to a bb,
  // and a pointer to the bb being branches to itself.
  // this is handled as assigning to each possible arg being branched to the
  // merge of all values that could be passed to it from this basic block.
  void translateSILPhi(TermArgSources &argSources) {
    argSources.argSources.setFrozen();
    for (auto pair : argSources.argSources.getRange()) {
      translateSILMultiAssign(TinyPtrVector<SILValue>(pair.first), pair.second);
    }
  }

  /// Instructions that transfer all of their non-Sendable parameters
  /// unconditionally and that do not have a result.
  void translateSILTransferringNoResult(MutableArrayRef<Operand> values) {
    for (auto &op : values) {
      if (auto ns = tryToTrackValue(op.get())) {
        builder.addRequire(ns->getRepresentative().getValue());
        builder.addTransfer(ns->getRepresentative().getValue(), &op);
      }
    }
  }

  /// Emit an unknown pattern error.
  void translateUnknownPatternError(SILValue value) {
    builder.addUnknownPatternError(value);
  }

  /// Translate the instruction's in \p basicBlock to a vector of PartitionOps
  /// that define the block's dataflow.
  void translateSILBasicBlock(SILBasicBlock *basicBlock,
                              std::vector<PartitionOp> &foundPartitionOps) {
    LLVM_DEBUG(llvm::dbgs() << SEP_STR << "Compiling basic block for function "
                            << basicBlock->getFunction()->getName() << ": ";
               basicBlock->dumpID(); llvm::dbgs() << SEP_STR;
               basicBlock->print(llvm::dbgs());
               llvm::dbgs() << SEP_STR << "Results:\n";);
    // Translate each SIL instruction to the PartitionOps that it represents if
    // any.
    for (auto &instruction : *basicBlock) {
      LLVM_DEBUG(llvm::dbgs() << "Visiting: " << instruction);
      translateSILInstruction(&instruction);
      copy(builder.currentInstPartitionOps,
           std::back_inserter(foundPartitionOps));
    }
  }

#define INST(INST, PARENT) TranslationSemantics visit##INST(INST *inst);
#include "swift/SIL/SILNodes.def"

  /// Top level switch that translates SIL instructions.
  void translateSILInstruction(SILInstruction *inst) {
    builder.reset(inst);
    SWIFT_DEFER { LLVM_DEBUG(builder.print(llvm::dbgs())); };

    auto computeOpKind = [&]() -> TranslationSemantics {
      switch (inst->getKind()) {
#define INST(ID, PARENT)                                                       \
  case SILInstructionKind::ID:                                                 \
    return visit##ID(cast<ID>(inst));
#include "swift/SIL/SILNodes.def"
      }
    };

    auto kind = computeOpKind();
    LLVM_DEBUG(llvm::dbgs() << "    Semantics: " << kind << '\n');
    switch (kind) {
    case TranslationSemantics::Ignored:
      return;

    case TranslationSemantics::AssignFresh:
      for (auto result : inst->getResults())
        translateSILAssignFresh(result);
      return;

    case TranslationSemantics::Assign:
      return translateSILMultiAssign(inst->getResults(),
                                     inst->getOperandValues());

    case TranslationSemantics::Require:
      for (auto op : inst->getOperandValues())
        translateSILRequire(op);
      return;

    case TranslationSemantics::LookThrough:
      assert(inst->getNumRealOperands() == 1);
      assert((isStaticallyLookThroughInst(inst) ||
              isLookThroughIfResultNonSendable(inst) ||
              isLookThroughIfOperandNonSendable(inst) ||
              isLookThroughIfOperandAndResultNonSendable(inst)) &&
             "Out of sync... should return true for one of these categories!");
      return translateSILLookThrough(inst->getResults(), inst->getOperand(0));

    case TranslationSemantics::Store:
      return translateSILStore(
          &inst->getAllOperands()[CopyLikeInstruction::Dest],
          &inst->getAllOperands()[CopyLikeInstruction::Src]);

    case TranslationSemantics::Special:
      return;

    case TranslationSemantics::Apply:
      return translateSILApply(inst);

    case TranslationSemantics::TerminatorPhi: {
      TermArgSources sources;
      sources.init(inst);
      return translateSILPhi(sources);
    }

    case TranslationSemantics::Asserting:
      llvm::errs() << "BannedInst: " << *inst;
      llvm::report_fatal_error(
          "transfer-non-sendable: Found banned instruction?!");
      return;

    case TranslationSemantics::AssertingIfNonSendable:
      // Do not error if all of our operands are sendable.
      if (llvm::none_of(inst->getOperandValues(), [&](SILValue value) {
            return ::SILIsolationInfo::isNonSendableType(value->getType(),
                                                         inst->getFunction());
          }))
        return;
      llvm::errs() << "BadInst: " << *inst;
      llvm::report_fatal_error(
          "transfer-non-sendable: Found instruction that is not allowed to "
          "have non-Sendable parameters with such parameters?!");
      return;
    case TranslationSemantics::TransferringNoResult:
      return translateSILTransferringNoResult(inst->getAllOperands());
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }
};

} // namespace regionanalysisimpl
} // namespace swift

Element PartitionOpBuilder::lookupValueID(SILValue value) {
  return translator->lookupValueID(value);
}

Element PartitionOpBuilder::getActorIntroducingRepresentative(
    SILIsolationInfo actorIsolation) {
  return translator
      ->getActorIntroducingRepresentative(currentInst, actorIsolation)
      .getID();
}

bool PartitionOpBuilder::valueHasID(SILValue value, bool dumpIfHasNoID) {
  return translator->valueHasID(value, dumpIfHasNoID);
}

void PartitionOpBuilder::print(llvm::raw_ostream &os) const {
#ifndef NDEBUG
  // If we do not have anything to dump, just return.
  if (currentInstPartitionOps.empty())
    return;

  // First line.
  llvm::dbgs() << " ┌─┬─╼";
  currentInst->print(llvm::dbgs());

  // Second line.
  llvm::dbgs() << " │ └─╼  ";
  currentInst->getLoc().getSourceLoc().printLineAndColumn(
      llvm::dbgs(), currentInst->getFunction()->getASTContext().SourceMgr);

  auto ops = llvm::ArrayRef(currentInstPartitionOps);

  // First op on its own line.
  llvm::dbgs() << "\n ├─────╼ ";
  ops.front().print(llvm::dbgs());

  // Rest of ops each on their own line.
  for (const PartitionOp &op : ops.drop_front()) {
    llvm::dbgs() << " │    └╼ ";
    op.print(llvm::dbgs());
  }

  // Now print out a translation from region to equivalence class value.
  llvm::dbgs() << " └─────╼ Used Values\n";
  llvm::SmallVector<Element, 8> opsToPrint;
  SWIFT_DEFER { opsToPrint.clear(); };
  for (const PartitionOp &op : ops) {
    // Now dump our the root value we map.
    for (unsigned opArg : op.getOpArgs()) {
      // If we didn't insert, skip this. We only emit this once.
      opsToPrint.push_back(Element(opArg));
    }
  }
  sortUnique(opsToPrint);
  for (Element opArg : opsToPrint) {
    llvm::dbgs() << "          └╼ ";
    auto trackableValue = translator->getValueForId(opArg);
    assert(trackableValue);
    llvm::dbgs() << "State: %%" << opArg << ". ";
    trackableValue->getValueState().print(llvm::dbgs());
    llvm::dbgs() << "\n             Rep Value: "
                 << trackableValue->getRepresentative();
    if (auto value = trackableValue->getRepresentative().maybeGetValue()) {
      llvm::dbgs() << "             Type: " << value->getType() << '\n';
    }
  }
#endif
}

//===----------------------------------------------------------------------===//
//                 MARK: Translator - Instruction To Op Kind
//===----------------------------------------------------------------------===//

#ifdef CONSTANT_TRANSLATION
#error "CONSTANT_TRANSLATION already defined?!"
#endif

#define CONSTANT_TRANSLATION(INST, Kind)                                       \
  TranslationSemantics PartitionOpTranslator::visit##INST(INST *inst) {        \
    assert((TranslationSemantics::Kind != TranslationSemantics::LookThrough || \
            isStaticallyLookThroughInst(inst)) &&                              \
           "Out of sync?!");                                                   \
    assert((TranslationSemantics::Kind == TranslationSemantics::LookThrough || \
            !isStaticallyLookThroughInst(inst)) &&                             \
           "Out of sync?!");                                                   \
    return TranslationSemantics::Kind;                                         \
  }

//===---
// Assign Fresh
//

CONSTANT_TRANSLATION(AllocBoxInst, AssignFresh)
CONSTANT_TRANSLATION(AllocPackInst, AssignFresh)
CONSTANT_TRANSLATION(AllocRefDynamicInst, AssignFresh)
CONSTANT_TRANSLATION(AllocRefInst, AssignFresh)
CONSTANT_TRANSLATION(AllocVectorInst, AssignFresh)
CONSTANT_TRANSLATION(KeyPathInst, AssignFresh)
CONSTANT_TRANSLATION(FunctionRefInst, AssignFresh)
CONSTANT_TRANSLATION(DynamicFunctionRefInst, AssignFresh)
CONSTANT_TRANSLATION(PreviousDynamicFunctionRefInst, AssignFresh)
CONSTANT_TRANSLATION(GlobalAddrInst, AssignFresh)
CONSTANT_TRANSLATION(GlobalValueInst, AssignFresh)
CONSTANT_TRANSLATION(HasSymbolInst, AssignFresh)
CONSTANT_TRANSLATION(ObjCProtocolInst, AssignFresh)
CONSTANT_TRANSLATION(WitnessMethodInst, AssignFresh)
// TODO: These should always be sendable.
CONSTANT_TRANSLATION(IntegerLiteralInst, AssignFresh)
CONSTANT_TRANSLATION(FloatLiteralInst, AssignFresh)
CONSTANT_TRANSLATION(StringLiteralInst, AssignFresh)
// Metatypes are Sendable, but AnyObject isn't
CONSTANT_TRANSLATION(ObjCMetatypeToObjectInst, AssignFresh)
CONSTANT_TRANSLATION(ObjCExistentialMetatypeToObjectInst, AssignFresh)

//===---
// Assign
//

// These are instructions that we treat as true assigns since we want to
// error semantically upon them if there is a use of one of these. For
// example, a cast would be inappropriate here. This is implemented by
// propagating the operand's region into the result's region and by
// requiring all operands.
CONSTANT_TRANSLATION(LoadWeakInst, Assign)
CONSTANT_TRANSLATION(StrongCopyUnownedValueInst, Assign)
CONSTANT_TRANSLATION(ClassMethodInst, Assign)
CONSTANT_TRANSLATION(ObjCMethodInst, Assign)
CONSTANT_TRANSLATION(SuperMethodInst, Assign)
CONSTANT_TRANSLATION(ObjCSuperMethodInst, Assign)
CONSTANT_TRANSLATION(LoadUnownedInst, Assign)

// These instructions are in between look through and a true assign. We should
// probably eventually treat them as look through but we haven't done the work
// yet of validating that everything fits together in terms of
// getUnderlyingTrackedObject.
CONSTANT_TRANSLATION(AddressToPointerInst, Assign)
CONSTANT_TRANSLATION(BaseAddrForOffsetInst, Assign)
CONSTANT_TRANSLATION(ConvertEscapeToNoEscapeInst, Assign)
CONSTANT_TRANSLATION(ConvertFunctionInst, Assign)
CONSTANT_TRANSLATION(CopyBlockInst, Assign)
CONSTANT_TRANSLATION(CopyBlockWithoutEscapingInst, Assign)
CONSTANT_TRANSLATION(IndexAddrInst, Assign)
CONSTANT_TRANSLATION(InitBlockStorageHeaderInst, Assign)
CONSTANT_TRANSLATION(InitExistentialAddrInst, Assign)
CONSTANT_TRANSLATION(InitExistentialRefInst, Assign)
CONSTANT_TRANSLATION(OpenExistentialBoxInst, Assign)
CONSTANT_TRANSLATION(OpenExistentialRefInst, Assign)
CONSTANT_TRANSLATION(TailAddrInst, Assign)
CONSTANT_TRANSLATION(ThickToObjCMetatypeInst, Assign)
CONSTANT_TRANSLATION(ThinToThickFunctionInst, Assign)
CONSTANT_TRANSLATION(UncheckedAddrCastInst, Assign)
CONSTANT_TRANSLATION(UncheckedEnumDataInst, Assign)
CONSTANT_TRANSLATION(UncheckedOwnershipConversionInst, Assign)
CONSTANT_TRANSLATION(IndexRawPointerInst, Assign)

// These are used by SIL to aggregate values together in a gep like way. We
// want to look at uses of structs, not the struct uses itself. So just
// propagate.
CONSTANT_TRANSLATION(ObjectInst, Assign)
CONSTANT_TRANSLATION(StructInst, Assign)
CONSTANT_TRANSLATION(TupleInst, Assign)

//===---
// Look Through
//

// Instructions that getUnderlyingTrackedValue is guaranteed to look through
// and whose operand and result are guaranteed to be mapped to the same
// underlying region.
CONSTANT_TRANSLATION(BeginAccessInst, LookThrough)
CONSTANT_TRANSLATION(BorrowedFromInst, LookThrough)
CONSTANT_TRANSLATION(BeginDeallocRefInst, LookThrough)
CONSTANT_TRANSLATION(BridgeObjectToRefInst, LookThrough)
CONSTANT_TRANSLATION(CopyValueInst, LookThrough)
CONSTANT_TRANSLATION(ExplicitCopyValueInst, LookThrough)
CONSTANT_TRANSLATION(EndCOWMutationInst, LookThrough)
CONSTANT_TRANSLATION(ProjectBoxInst, LookThrough)
CONSTANT_TRANSLATION(EndInitLetRefInst, LookThrough)
CONSTANT_TRANSLATION(InitEnumDataAddrInst, LookThrough)
CONSTANT_TRANSLATION(OpenExistentialAddrInst, LookThrough)
CONSTANT_TRANSLATION(UncheckedRefCastInst, LookThrough)
CONSTANT_TRANSLATION(UpcastInst, LookThrough)
CONSTANT_TRANSLATION(MarkUnresolvedNonCopyableValueInst, LookThrough)
CONSTANT_TRANSLATION(MarkUnresolvedReferenceBindingInst, LookThrough)
CONSTANT_TRANSLATION(CopyableToMoveOnlyWrapperValueInst, LookThrough)
CONSTANT_TRANSLATION(MoveOnlyWrapperToCopyableValueInst, LookThrough)
CONSTANT_TRANSLATION(MoveOnlyWrapperToCopyableBoxInst, LookThrough)
CONSTANT_TRANSLATION(MoveOnlyWrapperToCopyableAddrInst, LookThrough)
CONSTANT_TRANSLATION(CopyableToMoveOnlyWrapperAddrInst, LookThrough)
CONSTANT_TRANSLATION(MarkUninitializedInst, LookThrough)
// We identify destructured results with their operand's region.
CONSTANT_TRANSLATION(DestructureTupleInst, LookThrough)
CONSTANT_TRANSLATION(DestructureStructInst, LookThrough)
CONSTANT_TRANSLATION(ProjectBlockStorageInst, LookThrough)
CONSTANT_TRANSLATION(RefToUnownedInst, LookThrough)
CONSTANT_TRANSLATION(UnownedToRefInst, LookThrough)
CONSTANT_TRANSLATION(UnownedCopyValueInst, LookThrough)
CONSTANT_TRANSLATION(DropDeinitInst, LookThrough)
CONSTANT_TRANSLATION(ValueToBridgeObjectInst, LookThrough)
CONSTANT_TRANSLATION(BeginCOWMutationInst, LookThrough)
CONSTANT_TRANSLATION(OpenExistentialValueInst, LookThrough)
CONSTANT_TRANSLATION(WeakCopyValueInst, LookThrough)
CONSTANT_TRANSLATION(StrongCopyWeakValueInst, LookThrough)
CONSTANT_TRANSLATION(StrongCopyUnmanagedValueInst, LookThrough)
CONSTANT_TRANSLATION(RefToUnmanagedInst, LookThrough)
CONSTANT_TRANSLATION(UnmanagedToRefInst, LookThrough)
CONSTANT_TRANSLATION(InitExistentialValueInst, LookThrough)

//===---
// Store
//

// These are treated as stores - meaning that they could write values into
// memory. The beahvior of this depends on whether the tgt addr is aliased,
// but conservative behavior is to treat these as merges of the regions of
// the src value and tgt addr
CONSTANT_TRANSLATION(CopyAddrInst, Store)
CONSTANT_TRANSLATION(ExplicitCopyAddrInst, Store)
CONSTANT_TRANSLATION(StoreInst, Store)
CONSTANT_TRANSLATION(StoreWeakInst, Store)
CONSTANT_TRANSLATION(MarkUnresolvedMoveAddrInst, Store)
CONSTANT_TRANSLATION(UncheckedRefCastAddrInst, Store)
CONSTANT_TRANSLATION(UnconditionalCheckedCastAddrInst, Store)
CONSTANT_TRANSLATION(StoreUnownedInst, Store)

//===---
// Ignored
//

// These instructions are ignored because they cannot affect the region that a
// value is within or because even though they are technically a use we would
// rather emit an error on a better instruction.
CONSTANT_TRANSLATION(AllocGlobalInst, Ignored)
CONSTANT_TRANSLATION(AutoreleaseValueInst, Ignored)
CONSTANT_TRANSLATION(DeallocBoxInst, Ignored)
CONSTANT_TRANSLATION(DeallocPartialRefInst, Ignored)
CONSTANT_TRANSLATION(DeallocRefInst, Ignored)
CONSTANT_TRANSLATION(DeallocStackInst, Ignored)
CONSTANT_TRANSLATION(DeallocStackRefInst, Ignored)
CONSTANT_TRANSLATION(DebugValueInst, Ignored)
CONSTANT_TRANSLATION(DeinitExistentialAddrInst, Ignored)
CONSTANT_TRANSLATION(DeinitExistentialValueInst, Ignored)
CONSTANT_TRANSLATION(DestroyAddrInst, Ignored)
CONSTANT_TRANSLATION(DestroyValueInst, Ignored)
CONSTANT_TRANSLATION(EndAccessInst, Ignored)
CONSTANT_TRANSLATION(EndBorrowInst, Ignored)
CONSTANT_TRANSLATION(EndLifetimeInst, Ignored)
CONSTANT_TRANSLATION(ExtendLifetimeInst, Ignored)
CONSTANT_TRANSLATION(EndUnpairedAccessInst, Ignored)
CONSTANT_TRANSLATION(HopToExecutorInst, Ignored)
CONSTANT_TRANSLATION(InjectEnumAddrInst, Ignored)
CONSTANT_TRANSLATION(IsEscapingClosureInst, Ignored)
CONSTANT_TRANSLATION(MetatypeInst, Ignored)
CONSTANT_TRANSLATION(EndApplyInst, Ignored)
CONSTANT_TRANSLATION(AbortApplyInst, Ignored)
CONSTANT_TRANSLATION(DebugStepInst, Ignored)
CONSTANT_TRANSLATION(IncrementProfilerCounterInst, Ignored)
CONSTANT_TRANSLATION(SpecifyTestInst, Ignored)

//===---
// Require
//

// Instructions that only require that the region of the value be live:
CONSTANT_TRANSLATION(FixLifetimeInst, Require)
CONSTANT_TRANSLATION(ClassifyBridgeObjectInst, Require)
CONSTANT_TRANSLATION(BridgeObjectToWordInst, Require)
CONSTANT_TRANSLATION(IsUniqueInst, Require)
CONSTANT_TRANSLATION(MarkFunctionEscapeInst, Require)
CONSTANT_TRANSLATION(UnmanagedRetainValueInst, Require)
CONSTANT_TRANSLATION(UnmanagedReleaseValueInst, Require)
CONSTANT_TRANSLATION(UnmanagedAutoreleaseValueInst, Require)
CONSTANT_TRANSLATION(RebindMemoryInst, Require)
CONSTANT_TRANSLATION(BindMemoryInst, Require)
CONSTANT_TRANSLATION(BeginUnpairedAccessInst, Require)
// Require of the value we extract the metatype from.
CONSTANT_TRANSLATION(ValueMetatypeInst, Require)
// Require of the value we extract the metatype from.
CONSTANT_TRANSLATION(ExistentialMetatypeInst, Require)
// These can take a parameter. If it is non-Sendable, use a require.
CONSTANT_TRANSLATION(GetAsyncContinuationAddrInst, Require)

//===---
// Asserting If Non Sendable Parameter
//

// Takes metatypes as parameters and metatypes today are always sendable.
CONSTANT_TRANSLATION(InitExistentialMetatypeInst, AssertingIfNonSendable)
CONSTANT_TRANSLATION(OpenExistentialMetatypeInst, AssertingIfNonSendable)
CONSTANT_TRANSLATION(ObjCToThickMetatypeInst, AssertingIfNonSendable)

//===---
// Terminators
//

// Ignored terminators.
CONSTANT_TRANSLATION(CondFailInst, Ignored)
// Switch value inst is ignored since we only switch over integers and
// function_ref/class_method which are considered sendable.
CONSTANT_TRANSLATION(SwitchValueInst, Ignored)
CONSTANT_TRANSLATION(UnreachableInst, Ignored)
CONSTANT_TRANSLATION(UnwindInst, Ignored)
// Doesn't take a parameter.
CONSTANT_TRANSLATION(ThrowAddrInst, Ignored)

// Terminators that only need require.
CONSTANT_TRANSLATION(ThrowInst, Require)
CONSTANT_TRANSLATION(SwitchEnumAddrInst, Require)
CONSTANT_TRANSLATION(YieldInst, Require)

// Terminators that act as phis.
CONSTANT_TRANSLATION(BranchInst, TerminatorPhi)
CONSTANT_TRANSLATION(CondBranchInst, TerminatorPhi)
CONSTANT_TRANSLATION(CheckedCastBranchInst, TerminatorPhi)
CONSTANT_TRANSLATION(DynamicMethodBranchInst, TerminatorPhi)

// Today, await_async_continuation just takes Sendable values
// (UnsafeContinuation and UnsafeThrowingContinuation).
CONSTANT_TRANSLATION(AwaitAsyncContinuationInst, AssertingIfNonSendable)
CONSTANT_TRANSLATION(GetAsyncContinuationInst, AssertingIfNonSendable)
CONSTANT_TRANSLATION(ExtractExecutorInst, AssertingIfNonSendable)
CONSTANT_TRANSLATION(FunctionExtractIsolationInst, Require)

//===---
// Existential Box
//

// NOTE: Today these can only be used with Errors. Since Error is a sub-protocol
// of Sendable, we actually do not have any way to truly test them. These are
// just hypothetical assignments so we are complete.
CONSTANT_TRANSLATION(AllocExistentialBoxInst, AssignFresh)
CONSTANT_TRANSLATION(ProjectExistentialBoxInst, Assign)
CONSTANT_TRANSLATION(OpenExistentialBoxValueInst, Assign)
CONSTANT_TRANSLATION(DeallocExistentialBoxInst, Ignored)

//===---
// Differentiable
//

CONSTANT_TRANSLATION(DifferentiabilityWitnessFunctionInst, AssignFresh)
CONSTANT_TRANSLATION(DifferentiableFunctionExtractInst, LookThrough)
CONSTANT_TRANSLATION(LinearFunctionExtractInst, LookThrough)
CONSTANT_TRANSLATION(LinearFunctionInst, Assign)
CONSTANT_TRANSLATION(DifferentiableFunctionInst, Assign)

//===---
// Packs
//

CONSTANT_TRANSLATION(DeallocPackInst, Ignored)
CONSTANT_TRANSLATION(DynamicPackIndexInst, Ignored)
CONSTANT_TRANSLATION(OpenPackElementInst, Ignored)
CONSTANT_TRANSLATION(PackLengthInst, Ignored)
CONSTANT_TRANSLATION(PackPackIndexInst, Ignored)
CONSTANT_TRANSLATION(ScalarPackIndexInst, Ignored)

//===---
// Apply
//

CONSTANT_TRANSLATION(ApplyInst, Apply)
CONSTANT_TRANSLATION(BeginApplyInst, Apply)
CONSTANT_TRANSLATION(BuiltinInst, Apply)
CONSTANT_TRANSLATION(TryApplyInst, Apply)

//===---
// Asserting
//

// Non-OSSA instructions that we should never see since we bail on non-OSSA
// functions early.
CONSTANT_TRANSLATION(ReleaseValueAddrInst, Asserting)
CONSTANT_TRANSLATION(ReleaseValueInst, Asserting)
CONSTANT_TRANSLATION(RetainValueAddrInst, Asserting)
CONSTANT_TRANSLATION(RetainValueInst, Asserting)
CONSTANT_TRANSLATION(StrongReleaseInst, Asserting)
CONSTANT_TRANSLATION(StrongRetainInst, Asserting)
CONSTANT_TRANSLATION(StrongRetainUnownedInst, Asserting)
CONSTANT_TRANSLATION(UnownedReleaseInst, Asserting)
CONSTANT_TRANSLATION(UnownedRetainInst, Asserting)

// Instructions only valid in lowered SIL. Please only add instructions here
// after adding an assert into the SILVerifier that this property is true.
CONSTANT_TRANSLATION(AllocPackMetadataInst, Asserting)
CONSTANT_TRANSLATION(DeallocPackMetadataInst, Asserting)

// All of these instructions should be removed by DI which runs before us in the
// pass pipeline.
CONSTANT_TRANSLATION(AssignInst, Asserting)
CONSTANT_TRANSLATION(AssignByWrapperInst, Asserting)
CONSTANT_TRANSLATION(AssignOrInitInst, Asserting)

// We should never hit this since it can only appear as a final instruction in a
// global variable static initializer list.
CONSTANT_TRANSLATION(VectorInst, Asserting)

#undef CONSTANT_TRANSLATION

#define IGNORE_IF_SENDABLE_RESULT_ASSIGN_OTHERWISE(INST)                       \
  TranslationSemantics PartitionOpTranslator::visit##INST(INST *inst) {        \
    if (!isNonSendableType(inst->getType())) {                                 \
      return TranslationSemantics::Ignored;                                    \
    }                                                                          \
    return TranslationSemantics::Assign;                                       \
  }

IGNORE_IF_SENDABLE_RESULT_ASSIGN_OTHERWISE(TupleExtractInst)
IGNORE_IF_SENDABLE_RESULT_ASSIGN_OTHERWISE(StructExtractInst)

#undef IGNORE_IF_SENDABLE_RESULT_ASSIGN_OTHERWISE

#ifdef LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND
#error "LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND already defined"
#endif

#define LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(INST)                    \
                                                                               \
  TranslationSemantics PartitionOpTranslator::visit##INST(INST *cast) {        \
    assert(isLookThroughIfOperandAndResultNonSendable(cast) && "Out of sync"); \
    bool isOperandNonSendable =                                                \
        isNonSendableType(cast->getOperand()->getType());                      \
    bool isResultNonSendable = isNonSendableType(cast->getType());             \
                                                                               \
    if (isOperandNonSendable) {                                                \
      if (isResultNonSendable) {                                               \
        return TranslationSemantics::LookThrough;                              \
      }                                                                        \
                                                                               \
      return TranslationSemantics::Require;                                    \
    }                                                                          \
                                                                               \
    if (isResultNonSendable)                                                   \
      return TranslationSemantics::AssignFresh;                                \
                                                                               \
    return TranslationSemantics::Ignored;                                      \
  }

LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(UncheckedTrivialBitCastInst)
LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(UncheckedBitwiseCastInst)
LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(UncheckedValueCastInst)
LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(TupleElementAddrInst)
LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(StructElementAddrInst)
LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND(UncheckedTakeEnumDataAddrInst)

#undef LOOKTHROUGH_IF_NONSENDABLE_RESULT_AND_OPERAND

//===---
// Custom Handling
//

TranslationSemantics
PartitionOpTranslator::visitStoreBorrowInst(StoreBorrowInst *sbi) {
  // A store_borrow is an interesting instruction since we are essentially
  // temporarily binding an object value to an address... so really any uses of
  // the address, we want to consider to be uses of the parent object. So we
  // basically put source/dest into the same region, but do not consider the
  // store_borrow itself to be a require use. This prevents the store_borrow
  // from causing incorrect diagnostics.
  SILValue destValue = sbi->getDest();
  SILValue srcValue = sbi->getSrc();

  auto nonSendableDest = tryToTrackValue(destValue);
  if (!nonSendableDest)
    return TranslationSemantics::Ignored;

  // In the following situations, we can perform an assign:
  //
  // 1. A store to unaliased storage.
  // 2. A store that is to an entire value.
  //
  // DISCUSSION: If we have case 2, we need to merge the regions since we
  // are not overwriting the entire region of the value. This does mean that
  // we artificially include the previous region that was stored
  // specifically in this projection... but that is better than
  // miscompiling. For memory like this, we probably need to track it on a
  // per field basis to allow for us to assign.
  if (nonSendableDest.value().isNoAlias() &&
      !isProjectedFromAggregate(destValue)) {
    translateSILMultiAssign(sbi->getResults(), sbi->getOperandValues(),
                            SILIsolationInfo(), false /*require src*/);
  } else {
    // Stores to possibly aliased storage must be treated as merges.
    translateSILMerge(destValue, srcValue, false /*require src*/);
  }

  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitAllocStackInst(AllocStackInst *asi) {
  // Before we do anything, see if asi is Sendable or if it is non-Sendable,
  // that it is from a var decl. In both cases, we can just return assign fresh
  // and exit early.
  if (!SILIsolationInfo::isNonSendableType(asi) || asi->isFromVarDecl())
    return TranslationSemantics::AssignFresh;

  // Ok at this point we know that our value is a non-Sendable temporary.
  auto isolationInfo = SILIsolationInfo::get(asi);
  if (!bool(isolationInfo) || isolationInfo.isDisconnected()) {
    return TranslationSemantics::AssignFresh;
  }

  // Ok, we can handle this and have a valid isolation. Initialize the value.
  auto v = initializeTrackedValue(asi, isolationInfo);
  assert(v && "Only return none if we have a sendable value, but we checked "
              "that earlier!");

  // If we already had a value for this alloc_stack (which we shouldn't
  // ever)... emit an unknown pattern error.
  if (!v->second) {
    translateUnknownPatternError(asi);
    return TranslationSemantics::Special;
  }

  // NOTE: To prevent an additional reinitialization by the canned AssignFresh
  // code, we do our own assign fresh and return special.
  builder.addAssignFresh(v->first.getRepresentative().getValue());
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitMoveValueInst(MoveValueInst *mvi) {
  if (mvi->isFromVarDecl())
    return TranslationSemantics::Assign;
  return TranslationSemantics::LookThrough;
}

TranslationSemantics
PartitionOpTranslator::visitBeginBorrowInst(BeginBorrowInst *bbi) {
  if (bbi->isFromVarDecl())
    return TranslationSemantics::Assign;
  return TranslationSemantics::LookThrough;
}

/// LoadInst is technically a statically look through instruction, but we want
/// to handle it especially in the infrastructure, so we cannot mark it as
/// such. This makes marking it as a normal lookthrough instruction impossible
/// since the routine checks that invariant.
TranslationSemantics PartitionOpTranslator::visitLoadInst(LoadInst *limvi) {
  return TranslationSemantics::Special;
}

/// LoadBorrowInst is technically a statically look through instruction, but we
/// want to handle it especially in the infrastructure, so we cannot mark it as
/// such. This makes marking it as a normal lookthrough instruction impossible
/// since the routine checks that invariant.
TranslationSemantics
PartitionOpTranslator::visitLoadBorrowInst(LoadBorrowInst *lbi) {
  return TranslationSemantics::Special;
}

TranslationSemantics PartitionOpTranslator::visitReturnInst(ReturnInst *ri) {
  if (ri->getFunction()->getLoweredFunctionType()->hasSendingResult()) {
    return TranslationSemantics::TransferringNoResult;
  }
  return TranslationSemantics::Require;
}

TranslationSemantics
PartitionOpTranslator::visitRefToBridgeObjectInst(RefToBridgeObjectInst *r) {
  translateSILLookThrough(
      SILValue(r), r->getOperand(RefToBridgeObjectInst::ConvertedOperand));
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitPackElementGetInst(PackElementGetInst *r) {
  if (!isNonSendableType(r->getType()))
    return TranslationSemantics::Require;
  translateSILAssign(SILValue(r), r->getPack());
  return TranslationSemantics::Special;
}

TranslationSemantics PartitionOpTranslator::visitTuplePackElementAddrInst(
    TuplePackElementAddrInst *r) {
  if (!isNonSendableType(r->getType())) {
    translateSILRequire(r->getTuple());
  } else {
    translateSILAssign(SILValue(r), r->getTuple());
  }
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitTuplePackExtractInst(TuplePackExtractInst *r) {
  if (!isNonSendableType(r->getType())) {
    translateSILRequire(r->getTuple());
  } else {
    translateSILAssign(SILValue(r), r->getTuple());
  }
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitPackElementSetInst(PackElementSetInst *r) {
  // If the value we are storing is sendable, treat this as a require.
  if (!isNonSendableType(r->getValue()->getType())) {
    return TranslationSemantics::Require;
  }

  // Otherwise, this is a store.
  translateSILStore(r->getPackOperand(), r->getValueOperand());
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitRawPointerToRefInst(RawPointerToRefInst *r) {
  assert(isLookThroughIfResultNonSendable(r) && "Out of sync");
  // If our result is non sendable, perform a look through.
  if (isNonSendableType(r->getType()))
    return TranslationSemantics::LookThrough;

  // Otherwise to be conservative, we need to treat this as a require.
  return TranslationSemantics::Require;
}

TranslationSemantics
PartitionOpTranslator::visitRefToRawPointerInst(RefToRawPointerInst *r) {
  assert(isLookThroughIfOperandNonSendable(r) && "Out of sync");

  // If our source ref is non sendable, perform a look through.
  if (isNonSendableType(r->getOperand()->getType()))
    return TranslationSemantics::LookThrough;

  // Otherwise to be conservative, we need to treat the raw pointer as a fresh
  // sendable value.
  return TranslationSemantics::AssignFresh;
}

TranslationSemantics
PartitionOpTranslator::visitMarkDependenceInst(MarkDependenceInst *mdi) {
  assert(isStaticallyLookThroughInst(mdi) && "Out of sync");
  translateSILLookThrough(mdi->getResults(), mdi->getValue());
  translateSILRequire(mdi->getBase());
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitPointerToAddressInst(PointerToAddressInst *ptai) {
  if (!isNonSendableType(ptai->getType())) {
    return TranslationSemantics::Require;
  }
  return TranslationSemantics::Assign;
}

TranslationSemantics PartitionOpTranslator::visitUnconditionalCheckedCastInst(
    UnconditionalCheckedCastInst *ucci) {
  if (SILDynamicCastInst(ucci).isRCIdentityPreserving()) {
    assert(isStaticallyLookThroughInst(ucci) && "Out of sync");
    return TranslationSemantics::LookThrough;
  }

  assert(!isStaticallyLookThroughInst(ucci) && "Out of sync");
  return TranslationSemantics::Assign;
}

// RefElementAddrInst is not considered to be a lookThrough since we want to
// consider the address projected from the class to be a separate value that
// is in the same region as the parent operand. The reason that we want to
// do this is to ensure that if we assign into the ref_element_addr memory,
// we do not consider writes into the struct that contains the
// ref_element_addr to be merged into.
TranslationSemantics
PartitionOpTranslator::visitRefElementAddrInst(RefElementAddrInst *reai) {
  // If our field is a NonSendableType...
  if (!isNonSendableType(reai->getType())) {
    // And the field is a let... then ignore it. We know that we cannot race on
    // any writes to the field.
    if (reai->getField()->isLet()) {
      LLVM_DEBUG(llvm::dbgs() << "    Found a let! Not tracking!\n");
      return TranslationSemantics::Ignored;
    }

    // Otherwise, we need to treat the access to the field as a require since we
    // could have a race on assignment to the class.
    return TranslationSemantics::Require;
  }

  return TranslationSemantics::Assign;
}

TranslationSemantics
PartitionOpTranslator::visitRefTailAddrInst(RefTailAddrInst *reai) {
  // If our trailing type is Sendable...
  if (!isNonSendableType(reai->getType())) {
    // And our ref_tail_addr is immutable... we can ignore the access since we
    // cannot race against a write to any of these fields.
    if (reai->isImmutable()) {
      LLVM_DEBUG(
          llvm::dbgs()
          << "    Found an immutable Sendable ref_tail_addr! Not tracking!\n");
      return TranslationSemantics::Ignored;
    }

    // Otherwise, we need a require since the value maybe alive.
    return TranslationSemantics::Require;
  }

  // If we have a NonSendable type, treat the address as a separate Element from
  // our base value.
  return TranslationSemantics::Assign;
}

/// Enum inst is handled specially since if it does not have an argument,
/// we must assign fresh. Otherwise, we must propagate.
TranslationSemantics PartitionOpTranslator::visitEnumInst(EnumInst *ei) {
  if (ei->getNumOperands() == 0)
    return TranslationSemantics::AssignFresh;
  return TranslationSemantics::Assign;
}

TranslationSemantics
PartitionOpTranslator::visitSelectEnumAddrInst(SelectEnumAddrInst *inst) {
  translateSILSelectEnum(inst);
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitSelectEnumInst(SelectEnumInst *inst) {
  translateSILSelectEnum(inst);
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitSwitchEnumInst(SwitchEnumInst *inst) {
  translateSILSwitchEnum(inst);
  return TranslationSemantics::Special;
}

TranslationSemantics PartitionOpTranslator::visitTupleAddrConstructorInst(
    TupleAddrConstructorInst *inst) {
  translateSILTupleAddrConstructor(inst);
  return TranslationSemantics::Special;
}

TranslationSemantics
PartitionOpTranslator::visitPartialApplyInst(PartialApplyInst *pai) {
  translateSILPartialApply(pai);
  return TranslationSemantics::Special;
}

TranslationSemantics PartitionOpTranslator::visitCheckedCastAddrBranchInst(
    CheckedCastAddrBranchInst *ccabi) {
  assert(ccabi->getSuccessBB()->getNumArguments() <= 1);

  // checked_cast_addr_br does not have any arguments in its resulting
  // block. We should just use a multi-assign on its operands.
  //
  // TODO: We should be smarter and treat the success/fail branches
  // differently depending on what the result of checked_cast_addr_br
  // is. For now just keep the current behavior. It is more conservative,
  // but still correct.
  translateSILMultiAssign(ArrayRef<SILValue>(), ccabi->getOperandValues());
  return TranslationSemantics::Special;
}

//===----------------------------------------------------------------------===//
//                          MARK: Block Level Model
//===----------------------------------------------------------------------===//

BlockPartitionState::BlockPartitionState(
    SILBasicBlock *basicBlock, PartitionOpTranslator &translator,
    TransferringOperandSetFactory &ptrSetFactory,
    IsolationHistory::Factory &isolationHistoryFactory,
    TransferringOperandToStateMap &transferringOpToStateMap)
    : entryPartition(isolationHistoryFactory.get()),
      exitPartition(isolationHistoryFactory.get()), basicBlock(basicBlock),
      ptrSetFactory(ptrSetFactory),
      transferringOpToStateMap(transferringOpToStateMap) {
  translator.translateSILBasicBlock(basicBlock, blockPartitionOps);
}

bool BlockPartitionState::recomputeExitFromEntry(
    PartitionOpTranslator &translator) {
  Partition workingPartition = entryPartition;

  struct ComputeEvaluator final
      : PartitionOpEvaluatorBaseImpl<ComputeEvaluator> {
    PartitionOpTranslator &translator;

    ComputeEvaluator(Partition &workingPartition,
                     TransferringOperandSetFactory &ptrSetFactory,
                     PartitionOpTranslator &translator,
                     TransferringOperandToStateMap &transferringOpToStateMap)
        : PartitionOpEvaluatorBaseImpl(workingPartition, ptrSetFactory,
                                       transferringOpToStateMap),
          translator(translator) {}

    SILIsolationInfo getIsolationRegionInfo(Element elt) const {
      return translator.getValueMap().getIsolationRegion(elt);
    }

    std::optional<Element> getElement(SILValue value) const {
      return translator.getValueMap().getTrackableValue(value).getID();
    }

    SILValue getRepresentative(SILValue value) const {
      return translator.getValueMap()
          .getTrackableValue(value)
          .getRepresentative()
          .maybeGetValue();
    }

    bool isClosureCaptured(Element elt, Operand *op) const {
      auto iter = translator.getValueForId(elt);
      if (!iter)
        return false;
      auto value = iter->getRepresentative().maybeGetValue();
      if (!value)
        return false;
      return translator.isClosureCaptured(value, op->getUser());
    }
  };
  ComputeEvaluator eval(workingPartition, ptrSetFactory, translator,
                        transferringOpToStateMap);
  for (const auto &partitionOp : blockPartitionOps) {
    // By calling apply without providing any error handling callbacks, errors
    // will be surpressed.  will be suppressed
    eval.apply(partitionOp);
  }
  LLVM_DEBUG(llvm::dbgs() << "    Working Partition: ";
             workingPartition.print(llvm::dbgs()));
  bool exitUpdated = !Partition::equals(exitPartition, workingPartition);
  exitPartition = workingPartition;
  LLVM_DEBUG(llvm::dbgs() << "    Exit Partition: ";
             exitPartition.print(llvm::dbgs()));
  LLVM_DEBUG(llvm::dbgs() << "    Updated Partition: "
                          << (exitUpdated ? "yes\n" : "no\n"));
  return exitUpdated;
}

void BlockPartitionState::print(llvm::raw_ostream &os) const {
  os << SEP_STR << "BlockPartitionState[needsUpdate=" << needsUpdate
     << "]\nid: ";
#ifndef NDEBUG
  auto printID = [&](SILBasicBlock *block) { block->printID(os); };
#else
  auto printID = [&](SILBasicBlock *) { os << "NOASSERTS. "; };
#endif
  printID(basicBlock);
  os << "entry partition: ";
  entryPartition.print(os);
  os << "exit partition: ";
  exitPartition.print(os);
  os << "instructions:\n┌──────────╼\n";
  for (const auto &op : blockPartitionOps) {
    os << "│ ";
    op.print(os, true /*extra space*/);
  }
  os << "└──────────╼\nSuccs:\n";
  for (auto succ : basicBlock->getSuccessorBlocks()) {
    os << "→";
    printID(succ);
  }
  os << "Preds:\n";
  for (auto pred : basicBlock->getPredecessorBlocks()) {
    os << "←";
    printID(pred);
  }
  os << SEP_STR;
}

//===----------------------------------------------------------------------===//
//                         MARK: Dataflow Entrypoint
//===----------------------------------------------------------------------===//

static bool canComputeRegionsForFunction(SILFunction *fn) {
  if (!fn->getASTContext().LangOpts.hasFeature(Feature::RegionBasedIsolation))
    return false;

  assert(fn->getASTContext().LangOpts.StrictConcurrencyLevel ==
             StrictConcurrency::Complete &&
         "Need strict concurrency to be enabled for RegionBasedIsolation to be "
         "enabled as well");

  // If this function does not correspond to a syntactic declContext and it
  // doesn't have a parent module, don't check it since we cannot check if a
  // type is sendable.
  if (!fn->getDeclContext() && !fn->getParentModule()) {
    return false;
  }

  if (!fn->hasOwnership()) {
    LLVM_DEBUG(llvm::dbgs() << "Only runs on Ownership SSA, skipping!\n");
    return false;
  }

  // The sendable protocol should /always/ be available if TransferNonSendable
  // is enabled. If not, there is a major bug in the compiler and we should
  // fail loudly.
  if (!fn->getASTContext().getProtocol(KnownProtocolKind::Sendable))
    return false;

  return true;
}

RegionAnalysisFunctionInfo::RegionAnalysisFunctionInfo(
    SILFunction *fn, PostOrderFunctionInfo *pofi)
    : allocator(), fn(fn), valueMap(fn), translator(), ptrSetFactory(allocator),
      isolationHistoryFactory(allocator),
      transferringOpToStateMap(isolationHistoryFactory), blockStates(),
      pofi(pofi), solved(false), supportedFunction(true) {
  // Before we do anything, make sure that we support processing this function.
  //
  // NOTE: See documentation on supportedFunction for criteria.
  if (!canComputeRegionsForFunction(fn)) {
    supportedFunction = false;
    return;
  }

  translator = new (allocator)
      PartitionOpTranslator(fn, pofi, valueMap, isolationHistoryFactory);
  blockStates.emplace(fn, [this](SILBasicBlock *block) -> BlockPartitionState {
    return BlockPartitionState(block, *translator, ptrSetFactory,
                               isolationHistoryFactory,
                               transferringOpToStateMap);
  });
  // Mark all blocks as needing to be updated.
  for (auto &block : *fn) {
    (*blockStates)[&block].needsUpdate = true;
  }
  // Set our entry partition to have the "entry partition".
  (*blockStates)[fn->getEntryBlock()].entryPartition =
      translator->getEntryPartition();
  runDataflow();
}

RegionAnalysisFunctionInfo::~RegionAnalysisFunctionInfo() {
  // If we had a non-supported function, we didn't create a translator, so we do
  // not need to tear anything down.
  if (!supportedFunction)
    return;

  // Tear down translator before we tear down the allocator.
  translator->~PartitionOpTranslator();
}

bool RegionAnalysisFunctionInfo::isClosureCaptured(SILValue value,
                                                   Operand *op) {
  assert(supportedFunction && "Unsupported Function?!");
  return translator->isClosureCaptured(value, op->getUser());
}

void RegionAnalysisFunctionInfo::runDataflow() {
  assert(!solved && "solve should only be called once");
  solved = true;

  LLVM_DEBUG(llvm::dbgs() << SEP_STR << "Performing Dataflow!\n" << SEP_STR);
  LLVM_DEBUG(llvm::dbgs() << "Values!\n";
             translator->getValueMap().print(llvm::dbgs()));

  bool anyNeedUpdate = true;
  while (anyNeedUpdate) {
    anyNeedUpdate = false;

    for (auto *block : pofi->getReversePostOrder()) {
      auto &blockState = (*blockStates)[block];
      blockState.isLive = true;

      LLVM_DEBUG(llvm::dbgs() << "Block: bb" << block->getDebugID() << "\n");
      if (!blockState.needsUpdate) {
        LLVM_DEBUG(llvm::dbgs() << "    Doesn't need update! Skipping!\n");
        continue;
      }

      // Mark this block as no longer needing an update.
      blockState.needsUpdate = false;

      // Compute the new entry partition to this block.
      Partition newEntryPartition = blockState.entryPartition;

      LLVM_DEBUG(llvm::dbgs() << "    Visiting Preds!\n");

      // This loop computes the union of the exit partitions of all
      // predecessors of this block
      for (SILBasicBlock *predBlock : block->getPredecessorBlocks()) {
        BlockPartitionState &predState = (*blockStates)[predBlock];

        // Predecessors that have not been reached yet will have an empty pred
        // state... so just merge them all. Also our initial value of
        LLVM_DEBUG(llvm::dbgs()
                       << "    Pred. bb" << predBlock->getDebugID() << ": ";
                   predState.exitPartition.print(llvm::dbgs()));
        newEntryPartition =
            Partition::join(newEntryPartition, predState.exitPartition);
      }

      // Update the entry partition. We need to still try to
      // recomputeExitFromEntry before we know if we made a difference to the
      // exit partition after applying the instructions of the block.
      blockState.entryPartition = newEntryPartition;

      // recompute this block's exit partition from its (updated) entry
      // partition, and if this changed the exit partition notify all
      // successor blocks that they need to update as well
      if (blockState.recomputeExitFromEntry(*translator)) {
        for (SILBasicBlock *succBlock : block->getSuccessorBlocks()) {
          anyNeedUpdate = true;
          (*blockStates)[succBlock].needsUpdate = true;
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
//                              MARK: Value Map
//===----------------------------------------------------------------------===//

SILInstruction *RegionAnalysisValueMap::maybeGetActorIntroducingInst(
    Element trackableValueID) const {
  if (auto value = getValueForId(trackableValueID)) {
    auto rep = value->getRepresentative();
    if (rep.hasRegionIntroducingInst())
      return rep.getActorRegionIntroducingInst();
  }

  return nullptr;
}

std::optional<TrackableValue>
RegionAnalysisValueMap::getValueForId(Element id) const {
  auto iter = stateIndexToEquivalenceClass.find(id);
  if (iter == stateIndexToEquivalenceClass.end())
    return {};
  auto iter2 = equivalenceClassValuesToState.find(iter->second);
  if (iter2 == equivalenceClassValuesToState.end())
    return {};
  return {{iter2->first, iter2->second}};
}

SILValue
RegionAnalysisValueMap::getRepresentative(Element trackableValueID) const {
  return getValueForId(trackableValueID)->getRepresentative().getValue();
}

SILValue
RegionAnalysisValueMap::maybeGetRepresentative(Element trackableValueID) const {
  return getValueForId(trackableValueID)->getRepresentative().maybeGetValue();
}

SILIsolationInfo
RegionAnalysisValueMap::getIsolationRegion(Element trackableValueID) const {
  auto iter = getValueForId(trackableValueID);
  if (!iter)
    return {};
  return iter->getValueState().getIsolationRegionInfo();
}

SILIsolationInfo
RegionAnalysisValueMap::getIsolationRegion(SILValue value) const {
  auto iter = equivalenceClassValuesToState.find(RepresentativeValue(value));
  if (iter == equivalenceClassValuesToState.end())
    return {};
  return iter->getSecond().getIsolationRegionInfo();
}

std::pair<TrackableValue, bool>
RegionAnalysisValueMap::initializeTrackableValue(
    SILValue value, SILIsolationInfo newInfo) const {
  auto info = getUnderlyingTrackedValue(value);
  value = info.value;

  auto *self = const_cast<RegionAnalysisValueMap *>(this);
  auto iter = self->equivalenceClassValuesToState.try_emplace(
      value, TrackableValueState(equivalenceClassValuesToState.size()));

  // If we did not insert, just return the already stored value.
  if (!iter.second) {
    return {{iter.first->first, iter.first->second}, false};
  }

  // If we did not insert, just return the already stored value.
  self->stateIndexToEquivalenceClass[iter.first->second.getID()] = value;

  // Before we do anything, see if we have a Sendable value.
  if (!SILIsolationInfo::isNonSendableType(value->getType(), fn)) {
    iter.first->getSecond().addFlag(TrackableValueFlag::isSendable);
    return {{iter.first->first, iter.first->second}, true};
  }

  // Otherwise, we have a non-Sendable type... so wire up the isolation.
  iter.first->getSecond().setIsolationRegionInfo(newInfo);

  return {{iter.first->first, iter.first->second}, true};
}

/// If \p isAddressCapturedByPartialApply is set to true, then this value is
/// an address that is captured by a partial_apply and we want to treat it as
/// may alias.
TrackableValue RegionAnalysisValueMap::getTrackableValue(
    SILValue value, bool isAddressCapturedByPartialApply) const {
  auto info = getUnderlyingTrackedValue(value);
  value = info.value;

  auto *self = const_cast<RegionAnalysisValueMap *>(this);
  auto iter = self->equivalenceClassValuesToState.try_emplace(
      value, TrackableValueState(equivalenceClassValuesToState.size()));

  // If we did not insert, just return the already stored value.
  if (!iter.second) {
    return {iter.first->first, iter.first->second};
  }

  // If we did not insert, just return the already stored value.
  self->stateIndexToEquivalenceClass[iter.first->second.getID()] = value;

  // Otherwise, we need to compute our flags.

  // Treat function ref and class method as either actor isolated or
  // sendable. Formally they are non-Sendable, so we do the check before we
  // check the oracle.
  if (isa<FunctionRefInst, ClassMethodInst>(value)) {
    if (auto isolation = SILIsolationInfo::get(value)) {
      iter.first->getSecond().setIsolationRegionInfo(isolation);
      return {iter.first->first, iter.first->second};
    }

    iter.first->getSecond().addFlag(TrackableValueFlag::isSendable);
    return {iter.first->first, iter.first->second};
  }

  // Then check our oracle to see if the value is actually sendable. If we have
  // a Sendable value, just return early.
  if (!SILIsolationInfo::isNonSendableType(value->getType(), fn)) {
    iter.first->getSecond().addFlag(TrackableValueFlag::isSendable);
    return {iter.first->first, iter.first->second};
  }

  // Ok, at this point we have a non-Sendable value. First process addresses.
  if (value->getType().isAddress()) {
    // If we were able to find this was actor isolated from finding our
    // underlying object, use that. It is never wrong.
    if (info.actorIsolation) {
      SILIsolationInfo isolation;
      if (info.value->getType().isAnyActor()) {
        isolation = SILIsolationInfo::getActorInstanceIsolated(
            value, info.value, info.actorIsolation->getActor());
      } else if (info.actorIsolation->isGlobalActor()) {
        isolation = SILIsolationInfo::getGlobalActorIsolated(
            value, info.actorIsolation->getGlobalActor());
      }

      if (isolation) {
        iter.first->getSecond().setIsolationRegionInfo(isolation);
      }
    }

    auto storage = AccessStorageWithBase::compute(value);
    if (storage.storage) {
      // Check if we have a uniquely identified address that was not captured
      // by a partial apply... in such a case, we treat it as no-alias.
      if (storage.storage.isUniquelyIdentified() &&
          !isAddressCapturedByPartialApply) {
        iter.first->getSecond().removeFlag(TrackableValueFlag::isMayAlias);
      }

      if (auto isolation = SILIsolationInfo::get(storage.base)) {
        iter.first->getSecond().setIsolationRegionInfo(isolation);
      }
    }
  }

  // Check if we have a load or load_borrow from an address. In that case, we
  // want to look through the load and find a better root from the address we
  // loaded from.
  if (isa<LoadInst, LoadBorrowInst>(iter.first->first.getValue())) {
    auto *svi = cast<SingleValueInstruction>(iter.first->first.getValue());

    // See if we can use get underlying tracked value to find if it is actor
    // isolated.
    //
    // TODO: Instead of using AccessStorageBase, just use our own visitor
    // everywhere. Just haven't done it due to possible perturbations.
    auto parentAddrInfo = getUnderlyingTrackedValue(svi);
    if (parentAddrInfo.actorIsolation) {
      iter.first->getSecond().setIsolationRegionInfo(
          SILIsolationInfo::getActorInstanceIsolated(
              svi, parentAddrInfo.value,
              parentAddrInfo.actorIsolation->getActor()));
    }

    auto storage = AccessStorageWithBase::compute(svi->getOperand(0));
    if (storage.storage) {
      if (auto isolation = SILIsolationInfo::get(storage.base)) {
        iter.first->getSecond().setIsolationRegionInfo(isolation);
      }
    }

    return {iter.first->first, iter.first->second};
  }

  // Ok, we have a non-Sendable type, see if we do not have any isolation
  // yet. If we don't, attempt to infer its isolation.
  if (!iter.first->getSecond().hasIsolationRegionInfo()) {
    if (auto isolation = SILIsolationInfo::get(iter.first->first.getValue())) {
      iter.first->getSecond().setIsolationRegionInfo(isolation);
      return {iter.first->first, iter.first->second};
    }
  }

  return {iter.first->first, iter.first->second};
}

std::optional<TrackableValue>
RegionAnalysisValueMap::getTrackableValueForActorIntroducingInst(
    SILInstruction *inst) const {
  auto *self = const_cast<RegionAnalysisValueMap *>(this);
  auto iter = self->equivalenceClassValuesToState.find(inst);
  if (iter == self->equivalenceClassValuesToState.end())
    return {};

  // Otherwise, we need to compute our flags.
  return {{iter->first, iter->second}};
}

std::optional<TrackableValue>
RegionAnalysisValueMap::tryToTrackValue(SILValue value) const {
  auto state = getTrackableValue(value);
  if (state.isNonSendable())
    return state;
  return {};
}

TrackableValue RegionAnalysisValueMap::getActorIntroducingRepresentative(
    SILInstruction *introducingInst, SILIsolationInfo actorIsolation) const {
  auto *self = const_cast<RegionAnalysisValueMap *>(this);
  auto iter = self->equivalenceClassValuesToState.try_emplace(
      introducingInst,
      TrackableValueState(equivalenceClassValuesToState.size()));

  // If we did not insert, just return the already stored value.
  if (!iter.second) {
    return {iter.first->first, iter.first->second};
  }

  // Otherwise, wire up the value.
  self->stateIndexToEquivalenceClass[iter.first->second.getID()] =
      introducingInst;
  iter.first->getSecond().setIsolationRegionInfo(actorIsolation);
  return {iter.first->first, iter.first->second};
}

bool RegionAnalysisValueMap::valueHasID(SILValue value, bool dumpIfHasNoID) {
  assert(getTrackableValue(value).isNonSendable() &&
         "Can only accept non-Sendable values");
  bool hasID = equivalenceClassValuesToState.count(value);
  if (!hasID && dumpIfHasNoID) {
    llvm::errs() << "FAILURE: valueHasID of ";
    value->print(llvm::errs());
    llvm::report_fatal_error("standard compiler error");
  }
  return hasID;
}

Element RegionAnalysisValueMap::lookupValueID(SILValue value) {
  auto state = getTrackableValue(value);
  assert(state.isNonSendable() &&
         "only non-Sendable values should be entered in the map");
  return state.getID();
}

void RegionAnalysisValueMap::print(llvm::raw_ostream &os) const {
#ifndef NDEBUG
  // Since this is just used for debug output, be inefficient to make nicer
  // output.
  std::vector<std::pair<unsigned, RepresentativeValue>> temp;
  for (auto p : stateIndexToEquivalenceClass) {
    temp.emplace_back(p.first, p.second);
  }
  std::sort(temp.begin(), temp.end());
  for (auto p : temp) {
    os << "%%" << p.first << ": ";
    auto value = getValueForId(Element(p.first));
    value->print(os);
  }
#endif
}

//===----------------------------------------------------------------------===//
//                              Main Entry Point
//===----------------------------------------------------------------------===//

void RegionAnalysis::initialize(SILPassManager *pm) {
  poa = pm->getAnalysis<PostOrderAnalysis>();
}

SILAnalysis *swift::createRegionAnalysis(SILModule *) {
  return new RegionAnalysis();
}
