//===--- TransferNonSendable.cpp ------------------------------------------===//
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

#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Type.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/ImmutablePointerSet.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/NodeDatastructures.h"
#include "swift/SIL/OperandDatastructures.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Test.h"
#include "swift/SILOptimizer/Analysis/RegionAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/PartitionUtils.h"
#include "swift/SILOptimizer/Utils/VariableNameUtils.h"
#include "swift/Sema/Concurrency.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace swift::PartitionPrimitives;
using namespace swift::PatternMatch;
using namespace swift::regionanalysisimpl;

namespace {

using TransferringOperandSetFactory = Partition::TransferringOperandSetFactory;
using Element = PartitionPrimitives::Element;
using Region = PartitionPrimitives::Region;

} // namespace

//===----------------------------------------------------------------------===//
//                              MARK: Utilities
//===----------------------------------------------------------------------===//

static std::optional<DiagnosticBehavior>
getDiagnosticBehaviorLimitForValue(SILValue value) {
  auto *nom = value->getType().getNominalOrBoundGenericNominal();
  if (!nom)
    return {};

  auto declRef = value->getFunction()->getDeclRef();
  if (!declRef)
    return {};

  auto *fromDC = declRef.getInnermostDeclContext();
  return getConcurrencyDiagnosticBehaviorLimit(nom, fromDC);
}

static std::optional<SILDeclRef> getDeclRefForCallee(SILInstruction *inst) {
  auto fas = FullApplySite::isa(inst);
  if (!fas)
    return {};

  SILValue calleeOrigin = fas.getCalleeOrigin();

  while (true) {
    // Intentionally don't lookup through dynamic_function_ref and
    // previous_dynamic_function_ref as the target of those functions is not
    // statically known.
    if (auto *fri = dyn_cast<FunctionRefInst>(calleeOrigin)) {
      if (auto *callee = fri->getReferencedFunctionOrNull()) {
        if (auto declRef = callee->getDeclRef())
          return declRef;
      }
    }

    if (auto *mi = dyn_cast<MethodInst>(calleeOrigin)) {
      return mi->getMember();
    }

    if (auto *pai = dyn_cast<PartialApplyInst>(calleeOrigin)) {
      calleeOrigin = pai->getCalleeOrigin();
      continue;
    }

    return {};
  }
}

static std::optional<std::pair<DescriptiveDeclKind, DeclName>>
getTransferringApplyCalleeInfo(SILInstruction *inst) {
  auto declRef = getDeclRefForCallee(inst);
  if (!declRef)
    return {};

  auto *decl = declRef->getDecl();
  if (!decl->hasName())
    return {};

  return {{decl->getDescriptiveKind(), decl->getName()}};
}

static Expr *inferArgumentExprFromApplyExpr(ApplyExpr *sourceApply,
                                            FullApplySite fai,
                                            const Operand *op) {

  Expr *foundExpr = nullptr;

  // If we have self, then infer it.
  if (fai.hasSelfArgument() && op == &fai.getSelfArgumentOperand()) {
    if (auto callExpr = dyn_cast<CallExpr>(sourceApply))
      if (auto calledExpr =
              dyn_cast<DotSyntaxCallExpr>(callExpr->getDirectCallee()))
        foundExpr = calledExpr->getBase();
  } else {
    // Otherwise, try to infer using the operand of the ApplyExpr.
    unsigned argNum = [&]() -> unsigned {
      if (fai.isCalleeOperand(*op))
        return op->getOperandNumber();
      return fai.getAppliedArgIndexWithoutIndirectResults(*op);
    }();

    // Something happened that we do not understand.
    if (argNum >= sourceApply->getArgs()->size()) {
      return nullptr;
    }

    foundExpr = sourceApply->getArgs()->getExpr(argNum);

    // If we have an erasure expression, lets use the original type. We do
    // this since we are not saying the specific parameter that is the
    // issue and we are using the type to explain it to the user.
    if (auto *erasureExpr = dyn_cast<ErasureExpr>(foundExpr))
      foundExpr = erasureExpr->getSubExpr();
  }

  return foundExpr;
}

//===----------------------------------------------------------------------===//
//                             MARK: Diagnostics
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(ASTContext &context, SourceLoc loc,
                                        Diag<T...> diag, U &&...args) {
  return std::move(context.Diags.diagnose(loc, diag, std::forward<U>(args)...)
                       .warnUntilSwiftVersion(6));
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(ASTContext &context, SILLocation loc,
                                        Diag<T...> diag, U &&...args) {
  return ::diagnoseError(context, loc.getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(const PartitionOp &op, Diag<T...> diag,
                                        U &&...args) {
  return ::diagnoseError(op.getSourceInst()->getFunction()->getASTContext(),
                         op.getSourceLoc().getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(const Operand *op, Diag<T...> diag,
                                        U &&...args) {
  return ::diagnoseError(op->getUser()->getFunction()->getASTContext(),
                         op->getUser()->getLoc().getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseError(const SILInstruction *inst,
                                        Diag<T...> diag, U &&...args) {
  return ::diagnoseError(inst->getFunction()->getASTContext(),
                         inst->getLoc().getSourceLoc(), diag,
                         std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(ASTContext &context, SourceLoc loc,
                                       Diag<T...> diag, U &&...args) {
  return context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(ASTContext &context, SILLocation loc,
                                       Diag<T...> diag, U &&...args) {
  return ::diagnoseNote(context, loc.getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(const PartitionOp &op, Diag<T...> diag,
                                       U &&...args) {
  return ::diagnoseNote(op.getSourceInst()->getFunction()->getASTContext(),
                        op.getSourceLoc().getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(const Operand *op, Diag<T...> diag,
                                       U &&...args) {
  return ::diagnoseNote(op->getUser()->getFunction()->getASTContext(),
                        op->getUser()->getLoc().getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

template <typename... T, typename... U>
static InFlightDiagnostic diagnoseNote(const SILInstruction *inst,
                                       Diag<T...> diag, U &&...args) {
  return ::diagnoseNote(inst->getFunction()->getASTContext(),
                        inst->getLoc().getSourceLoc(), diag,
                        std::forward<U>(args)...);
}

//===----------------------------------------------------------------------===//
//                           MARK: Require Liveness
//===----------------------------------------------------------------------===//

namespace {

class BlockLivenessInfo {
  // Generation counter so we do not need to reallocate.
  unsigned generation = 0;
  SILInstruction *firstRequireInst = nullptr;

  void resetIfNew(unsigned newGeneration) {
    if (generation == newGeneration)
      return;
    generation = newGeneration;
    firstRequireInst = nullptr;
  }

public:
  SILInstruction *getInst(unsigned callerGeneration) {
    resetIfNew(callerGeneration);
    return firstRequireInst;
  }

  void setInst(unsigned callerGeneration, SILInstruction *newValue) {
    resetIfNew(callerGeneration);
    firstRequireInst = newValue;
  }
};

/// We only want to emit errors for the first requires along a path from a
/// transfer instruction. We discover this by walking from user blocks to
struct RequireLiveness {
  unsigned generation;
  SILInstruction *transferInst;
  BasicBlockData<BlockLivenessInfo> &blockLivenessInfo;
  InstructionSet allRequires;
  InstructionSetWithSize finalRequires;

  /// If we have requires in the def block before our transfer, this is the
  /// first require.
  SILInstruction *firstRequireBeforeTransferInDefBlock = nullptr;

  RequireLiveness(unsigned generation, Operand *transferOp,
                  BasicBlockData<BlockLivenessInfo> &blockLivenessInfo)
      : generation(generation), transferInst(transferOp->getUser()),
        blockLivenessInfo(blockLivenessInfo),
        allRequires(transferOp->getParentFunction()),
        finalRequires(transferOp->getParentFunction()) {}

  template <typename Collection>
  void process(Collection collection);

  /// Attempt to process requireInst for our def block. Returns false if
  /// requireInst was before our def and we need to do interprocedural
  /// processing. Returns true if requireInst was after our transferInst and we
  /// were able to appropriately determine if we should emit it or not.
  void processDefBlock();

  /// Process all requires in block, updating blockLivenessInfo.
  void processNonDefBlock(SILBasicBlock *block);
};

} // namespace

void RequireLiveness::processDefBlock() {
  LLVM_DEBUG(llvm::dbgs() << "    Processing def block!\n");
  // First walk from the beginning of the block to the transfer instruction to
  // see if we have any requires before our def. Once we find one, we can skip
  // the traversal and jump straight to the transfer.
  for (auto ii = transferInst->getParent()->begin(),
            ie = transferInst->getIterator();
       ii != ie; ++ii) {
    if (allRequires.contains(&*ii) && !firstRequireBeforeTransferInDefBlock) {
      firstRequireBeforeTransferInDefBlock = &*ii;
      LLVM_DEBUG(llvm::dbgs() << "        Found transfer before def: "
                              << *firstRequireBeforeTransferInDefBlock);
      break;
    }
  }

  // Then walk from our transferInst to the end of the block looking for the
  // first require inst. Once we find it... return.
  //
  // NOTE: We start walking at the transferInst since the transferInst could use
  // the requireInst as well.
  for (auto ii = transferInst->getIterator(),
            ie = transferInst->getParent()->end();
       ii != ie; ++ii) {
    if (!allRequires.contains(&*ii))
      continue;

    finalRequires.insert(&*ii);
    LLVM_DEBUG(llvm::dbgs() << "        Found transfer after def: " << *ii);
    return;
  }
}

void RequireLiveness::processNonDefBlock(SILBasicBlock *block) {
  // Walk from the bottom to the top... assigning to our block state.
  auto blockState = blockLivenessInfo.get(block);
  for (auto &inst : llvm::make_range(block->rbegin(), block->rend())) {
    if (!finalRequires.contains(&inst))
      continue;
    blockState.get()->setInst(generation, &inst);
  }
}

template <typename Collection>
void RequireLiveness::process(Collection requireInstList) {
  LLVM_DEBUG(llvm::dbgs() << "==> Performing Require Liveness for: "
                          << *transferInst);

  // Then put all of our requires into our allRequires set.
  BasicBlockWorklist initializingWorklist(transferInst->getFunction());
  for (auto *require : requireInstList) {
    LLVM_DEBUG(llvm::dbgs() << "        Require Inst: " << *require);
    allRequires.insert(require);
    initializingWorklist.pushIfNotVisited(require->getParent());
  }

  // Then process our def block to see if we have any requires before and after
  // the transferInst...
  processDefBlock();

  // If we found /any/ requries after the transferInst, we can bail early since
  // that is guaranteed to dominate all further requires.
  if (!finalRequires.empty()) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "        Found transfer after def in def block! Exiting early!\n");
    return;
  }

  LLVM_DEBUG(llvm::dbgs() << "        Did not find transfer after def in def "
                             "block! Walking blocks!\n");

  // If we found a transfer in the def block before our def, add it to the block
  // state for the def.
  if (firstRequireBeforeTransferInDefBlock) {
    LLVM_DEBUG(
        llvm::dbgs()
        << "        Found a require before transfer! Adding to block state!\n");
    auto blockState = blockLivenessInfo.get(transferInst->getParent());
    blockState.get()->setInst(generation, firstRequireBeforeTransferInDefBlock);
  }

  // Then for each require block that isn't a def block transfer, find the
  // earliest transfer inst.
  while (auto *requireBlock = initializingWorklist.pop()) {
    auto blockState = blockLivenessInfo.get(requireBlock);
    for (auto &inst : *requireBlock) {
      if (!allRequires.contains(&inst))
        continue;
      LLVM_DEBUG(llvm::dbgs() << "        Mapping Block bb"
                              << requireBlock->getDebugID() << " to: " << inst);
      blockState.get()->setInst(generation, &inst);
      break;
    }
  }

  // Then walk from our def block looking for setInst blocks.
  auto *transferBlock = transferInst->getParent();
  BasicBlockWorklist worklist(transferInst->getFunction());
  for (auto *succBlock : transferBlock->getSuccessorBlocks())
    worklist.pushIfNotVisited(succBlock);

  while (auto *next = worklist.pop()) {
    // Check if we found an earliest requires... if so, add that to final
    // requires and continue. We don't want to visit successors.
    auto blockState = blockLivenessInfo.get(next);
    if (auto *inst = blockState.get()->getInst(generation)) {
      finalRequires.insert(inst);
      continue;
    }

    // Do not look at successors of the transfer block.
    if (next == transferBlock)
      continue;

    // Otherwise, we did not find a requires and need to search further
    // successors.
    for (auto *succBlock : next->getSuccessorBlocks())
      worklist.pushIfNotVisited(succBlock);
  }
}

//===----------------------------------------------------------------------===//
//            MARK: Forward Declaration Of TransferNonSendableImpl
//===----------------------------------------------------------------------===//

namespace {

struct TransferredNonTransferrableInfo {
  /// The use that actually caused the transfer.
  Operand *transferredOperand;

  /// The non-transferrable value that is in the same region as \p
  /// transferredOperand.get().
  llvm::PointerUnion<SILValue, SILInstruction *> nonTransferrable;

  /// The region info that describes the dynamic dataflow derived isolation
  /// region info for the non-transferrable value.
  ///
  /// This is equal to the merge of the IsolationRegionInfo from all elements in
  /// nonTransferrable's region when the error was diagnosed.
  SILDynamicMergedIsolationInfo isolationRegionInfo;

  TransferredNonTransferrableInfo(
      Operand *transferredOperand, SILValue nonTransferrableValue,
      SILDynamicMergedIsolationInfo isolationRegionInfo)
      : transferredOperand(transferredOperand),
        nonTransferrable(nonTransferrableValue),
        isolationRegionInfo(isolationRegionInfo) {}
  TransferredNonTransferrableInfo(
      Operand *transferredOperand, SILInstruction *nonTransferrableInst,
      SILDynamicMergedIsolationInfo isolationRegionInfo)
      : transferredOperand(transferredOperand),
        nonTransferrable(nonTransferrableInst),
        isolationRegionInfo(isolationRegionInfo) {}
};

class TransferNonSendableImpl {
  RegionAnalysisFunctionInfo *regionInfo;
  SmallFrozenMultiMap<Operand *, SILInstruction *, 8>
      transferOpToRequireInstMultiMap;
  SmallVector<TransferredNonTransferrableInfo, 8>
      transferredNonTransferrableInfoList;

public:
  TransferNonSendableImpl(RegionAnalysisFunctionInfo *regionInfo)
      : regionInfo(regionInfo) {}

  void emitDiagnostics();

private:
  void runDiagnosticEvaluator();

  void emitUseAfterTransferDiagnostics();
  void emitTransferredNonTransferrableDiagnostics();
};

} // namespace

//===----------------------------------------------------------------------===//
//                MARK: UseAfterTransfer Diagnostic Inference
//===----------------------------------------------------------------------===//

namespace {

class UseAfterTransferDiagnosticEmitter {
  Operand *transferOp;
  SmallVectorImpl<SILInstruction *> &requireInsts;
  bool emittedErrorDiagnostic = false;

public:
  UseAfterTransferDiagnosticEmitter(
      Operand *transferOp, SmallVectorImpl<SILInstruction *> &requireInsts)
      : transferOp(transferOp), requireInsts(requireInsts) {}

  ~UseAfterTransferDiagnosticEmitter() {
    // If we were supposed to emit a diagnostic and didn't emit an unknown
    // pattern error.
    if (!emittedErrorDiagnostic)
      emitUnknownPatternError();
  }

  std::optional<DiagnosticBehavior> getBehaviorLimit() const {
    return getDiagnosticBehaviorLimitForValue(transferOp->get());
  }

  /// If we can find a callee decl name, return that. None otherwise.
  std::optional<std::pair<DescriptiveDeclKind, DeclName>>
  getTransferringCalleeInfo() const {
    return getTransferringApplyCalleeInfo(transferOp->getUser());
  }

  void
  emitNamedIsolationCrossingError(SILLocation loc, Identifier name,
                                  SILIsolationInfo namedValuesIsolationInfo,
                                  ApplyIsolationCrossing isolationCrossing) {
    // Emit the short error.
    diagnoseError(loc, diag::regionbasedisolation_named_transfer_yields_race,
                  name)
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());

    // Then emit the note with greater context.
    SmallString<64> descriptiveKindStr;
    {
      if (!namedValuesIsolationInfo.isDisconnected()) {
        llvm::raw_svector_ostream os(descriptiveKindStr);
        namedValuesIsolationInfo.printForDiagnostics(os);
        os << ' ';
      }
    }

    if (auto calleeInfo = getTransferringCalleeInfo()) {
      diagnoseNote(
          loc,
          diag::regionbasedisolation_named_info_transfer_yields_race_callee,
          name, descriptiveKindStr, isolationCrossing.getCalleeIsolation(),
          calleeInfo->first, calleeInfo->second,
          isolationCrossing.getCallerIsolation());
    } else {
      diagnoseNote(
          loc, diag::regionbasedisolation_named_info_transfer_yields_race, name,
          descriptiveKindStr, isolationCrossing.getCalleeIsolation(),
          isolationCrossing.getCallerIsolation());
    }
    emitRequireInstDiagnostics();
  }

  void
  emitNamedIsolationCrossingError(SILLocation loc, Identifier name,
                                  SILIsolationInfo namedValuesIsolationInfo,
                                  ApplyIsolationCrossing isolationCrossing,
                                  DeclName calleeDeclName,
                                  DescriptiveDeclKind calleeDeclKind) {
    // Emit the short error.
    diagnoseError(loc, diag::regionbasedisolation_named_transfer_yields_race,
                  name)
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());

    // Then emit the note with greater context.
    SmallString<64> descriptiveKindStr;
    {
      if (!namedValuesIsolationInfo.isDisconnected()) {
        llvm::raw_svector_ostream os(descriptiveKindStr);
        namedValuesIsolationInfo.printForDiagnostics(os);
        os << ' ';
      }
    }

    diagnoseNote(
        loc, diag::regionbasedisolation_named_info_transfer_yields_race_callee,
        name, descriptiveKindStr, isolationCrossing.getCalleeIsolation(),
        calleeDeclKind, calleeDeclName, isolationCrossing.getCallerIsolation());
    emitRequireInstDiagnostics();
  }

  void emitTypedIsolationCrossing(SILLocation loc, Type inferredType,
                                  ApplyIsolationCrossing isolationCrossing) {
    diagnoseError(
        loc, diag::regionbasedisolation_transfer_yields_race_with_isolation,
        inferredType, isolationCrossing.getCallerIsolation(),
        isolationCrossing.getCalleeIsolation())
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
    emitRequireInstDiagnostics();
  }

  void emitNamedUseOfStronglyTransferredValue(SILLocation loc,
                                              Identifier name) {
    // Emit the short error.
    diagnoseError(loc, diag::regionbasedisolation_named_transfer_yields_race,
                  name)
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());

    // Then emit the note with greater context.
    diagnoseNote(
        loc, diag::regionbasedisolation_named_value_used_after_explicit_sending,
        name)
        .highlight(loc.getSourceRange());

    // Finally the require points.
    emitRequireInstDiagnostics();
  }

  void emitTypedUseOfStronglyTransferredValue(SILLocation loc,
                                              Type inferredType) {
    diagnoseError(
        loc,
        diag::
            regionbasedisolation_transfer_yields_race_stronglytransferred_binding,
        inferredType)
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
    emitRequireInstDiagnostics();
  }

  void emitTypedRaceWithUnknownIsolationCrossing(SILLocation loc,
                                                 Type inferredType) {
    diagnoseError(loc,
                  diag::regionbasedisolation_transfer_yields_race_no_isolation,
                  inferredType)
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
    emitRequireInstDiagnostics();
  }

  void emitNamedIsolationCrossingDueToCapture(
      SILLocation loc, Identifier name,
      SILIsolationInfo namedValuesIsolationInfo,
      ApplyIsolationCrossing isolationCrossing) {
    // Emit the short error.
    diagnoseError(loc, diag::regionbasedisolation_named_transfer_yields_race,
                  name)
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());

    SmallString<64> descriptiveKindStr;
    {
      if (!namedValuesIsolationInfo.isDisconnected()) {
        llvm::raw_svector_ostream os(descriptiveKindStr);
        namedValuesIsolationInfo.printForDiagnostics(os);
        os << ' ';
      }
    }

    diagnoseNote(
        loc, diag::regionbasedisolation_named_isolated_closure_yields_race,
        descriptiveKindStr, name, isolationCrossing.getCalleeIsolation(),
        isolationCrossing.getCallerIsolation())
        .highlight(loc.getSourceRange());
    emitRequireInstDiagnostics();
  }

  void emitTypedIsolationCrossingDueToCapture(
      SILLocation loc, Type inferredType,
      ApplyIsolationCrossing isolationCrossing) {
    diagnoseError(loc, diag::regionbasedisolation_isolated_capture_yields_race,
                  inferredType, isolationCrossing.getCalleeIsolation(),
                  isolationCrossing.getCallerIsolation())
        .highlight(loc.getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
    emitRequireInstDiagnostics();
  }

  void emitUnknownPatternError() {
    diagnoseError(transferOp->getUser(),
                  diag::regionbasedisolation_unknown_pattern)
        .limitBehaviorIf(getBehaviorLimit());
  }

private:
  ASTContext &getASTContext() const {
    return transferOp->getFunction()->getASTContext();
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseError(SourceLoc loc, Diag<T...> diag,
                                   U &&...args) {
    emittedErrorDiagnostic = true;
    return std::move(getASTContext()
                         .Diags.diagnose(loc, diag, std::forward<U>(args)...)
                         .warnUntilSwiftVersion(6));
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseError(SILLocation loc, Diag<T...> diag,
                                   U &&...args) {
    return diagnoseError(loc.getSourceLoc(), diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseError(SILInstruction *inst, Diag<T...> diag,
                                   U &&...args) {
    return diagnoseError(inst->getLoc(), diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseNote(SourceLoc loc, Diag<T...> diag, U &&...args) {
    return getASTContext().Diags.diagnose(loc, diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseNote(SILLocation loc, Diag<T...> diag,
                                  U &&...args) {
    return diagnoseNote(loc.getSourceLoc(), diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseNote(SILInstruction *inst, Diag<T...> diag,
                                  U &&...args) {
    return diagnoseNote(inst->getLoc(), diag, std::forward<U>(args)...);
  }

  void emitRequireInstDiagnostics() {
    // Now actually emit the require notes.
    while (!requireInsts.empty()) {
      auto *require = requireInsts.pop_back_val();
      diagnoseNote(require, diag::regionbasedisolation_maybe_race)
          .highlight(require->getLoc().getSourceRange());
    }
  }
};

class UseAfterTransferDiagnosticInferrer {
  Operand *transferOp;
  UseAfterTransferDiagnosticEmitter diagnosticEmitter;
  RegionAnalysisValueMap &valueMap;
  TransferringOperandToStateMap &transferringOpToStateMap;
  SILLocation baseLoc = SILLocation::invalid();
  Type baseInferredType;

  struct AutoClosureWalker;

public:
  UseAfterTransferDiagnosticInferrer(
      Operand *transferOp, SmallVectorImpl<SILInstruction *> &requireInsts,
      RegionAnalysisValueMap &valueMap,
      TransferringOperandToStateMap &transferringOpToStateMap)
      : transferOp(transferOp), diagnosticEmitter(transferOp, requireInsts),
        valueMap(valueMap), transferringOpToStateMap(transferringOpToStateMap),
        baseLoc(transferOp->getUser()->getLoc()),
        baseInferredType(transferOp->get()->getType().getASTType()) {}
  void infer();

  Operand *getTransferringOperand() const { return transferOp; }

private:
  bool initForIsolatedPartialApply(Operand *op, AbstractClosureExpr *ace);

  void initForApply(Operand *op, ApplyExpr *expr);
  void initForAutoclosure(Operand *op, AutoClosureExpr *expr);

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

bool UseAfterTransferDiagnosticInferrer::initForIsolatedPartialApply(
    Operand *op, AbstractClosureExpr *ace) {
  SmallVector<std::tuple<CapturedValue, unsigned, ApplyIsolationCrossing>, 8>
      foundCapturedIsolationCrossing;
  ace->getIsolationCrossing(foundCapturedIsolationCrossing);
  if (foundCapturedIsolationCrossing.empty())
    return false;

  unsigned opIndex = ApplySite(op->getUser()).getAppliedArgIndex(*op);
  bool emittedDiagnostic = false;
  for (auto &p : foundCapturedIsolationCrossing) {
    if (std::get<1>(p) != opIndex)
      continue;
    emittedDiagnostic = true;

    auto &state = transferringOpToStateMap.get(transferOp);
    if (auto rootValueAndName =
            VariableNameInferrer::inferNameAndRoot(transferOp->get())) {
      diagnosticEmitter.emitNamedIsolationCrossingDueToCapture(
          RegularLocation(std::get<0>(p).getLoc()), rootValueAndName->first,
          state.isolationInfo.getIsolationInfo(), std::get<2>(p));
      continue;
    }

    diagnosticEmitter.emitTypedIsolationCrossingDueToCapture(
        RegularLocation(std::get<0>(p).getLoc()), baseInferredType,
        std::get<2>(p));
  }

  return emittedDiagnostic;
}

void UseAfterTransferDiagnosticInferrer::initForApply(Operand *op,
                                                      ApplyExpr *sourceApply) {
  auto isolationCrossing = sourceApply->getIsolationCrossing().value();

  // Grab out full apply site and see if we can find a better expr.
  SILInstruction *i = const_cast<SILInstruction *>(op->getUser());
  auto fai = FullApplySite::isa(i);

  assert(!fai.getArgumentConvention(*op).isIndirectOutParameter() &&
         "An indirect out parameter is never transferred");
  auto *foundExpr = inferArgumentExprFromApplyExpr(sourceApply, fai, op);

  auto inferredArgType =
      foundExpr ? foundExpr->findOriginalType() : baseInferredType;
  diagnosticEmitter.emitTypedIsolationCrossing(baseLoc, inferredArgType,
                                               isolationCrossing);
}

/// This walker visits an AutoClosureExpr and looks for uses of a specific
/// captured value. We want to error on the uses in the autoclosure.
struct UseAfterTransferDiagnosticInferrer::AutoClosureWalker : ASTWalker {
  UseAfterTransferDiagnosticInferrer &foundTypeInfo;
  ValueDecl *targetDecl;
  SILIsolationInfo targetDeclIsolationInfo;
  SmallPtrSet<Expr *, 8> visitedCallExprDeclRefExprs;

  AutoClosureWalker(UseAfterTransferDiagnosticInferrer &foundTypeInfo,
                    ValueDecl *targetDecl,
                    SILIsolationInfo targetDeclIsolationInfo)
      : foundTypeInfo(foundTypeInfo), targetDecl(targetDecl),
        targetDeclIsolationInfo(targetDeclIsolationInfo) {}

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
          foundTypeInfo.diagnosticEmitter
              .emitTypedRaceWithUnknownIsolationCrossing(
                  foundTypeInfo.baseLoc, declRef->findOriginalType());
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

              // See if we can find a valueDecl/name for our callee so we can
              // emit a nicer error.
              ConcreteDeclRef concreteDecl =
                  callExpr->getDirectCallee()->getReferencedDecl();

              // If we do not find a direct one, see if we are calling a method
              // on a nominal type.
              if (!concreteDecl) {
                if (auto *dot = dyn_cast<DotSyntaxCallExpr>(
                        callExpr->getDirectCallee())) {
                  concreteDecl = dot->getSemanticFn()->getReferencedDecl();
                }
              }

              if (concreteDecl) {
                auto *valueDecl = concreteDecl.getDecl();
                assert(valueDecl &&
                       "Should be non-null if concreteDecl is valid");
                if (valueDecl->hasName()) {
                  foundTypeInfo.diagnosticEmitter
                      .emitNamedIsolationCrossingError(
                          foundTypeInfo.baseLoc,
                          targetDecl->getBaseIdentifier(),
                          targetDeclIsolationInfo, *isolationCrossing,
                          valueDecl->getName(),
                          valueDecl->getDescriptiveKind());
                  return Action::Continue(expr);
                }
              }

              // Otherwise default back to the "callee" error.
              foundTypeInfo.diagnosticEmitter.emitNamedIsolationCrossingError(
                  foundTypeInfo.baseLoc, targetDecl->getBaseIdentifier(),
                  targetDeclIsolationInfo, *isolationCrossing);
              return Action::Continue(expr);
            }
          }
        }
      }
    }

    return Action::Continue(expr);
  }
};

void UseAfterTransferDiagnosticInferrer::infer() {
  // Otherwise, see if our operand's instruction is a transferring parameter.
  if (auto fas = FullApplySite::isa(transferOp->getUser())) {
    assert(!fas.getArgumentConvention(*transferOp).isIndirectOutParameter() &&
           "We should never transfer an indirect out parameter");
    if (fas.getArgumentParameterInfo(*transferOp)
            .hasOption(SILParameterInfo::Sending)) {

      // First try to do the named diagnostic if we can find a name.
      if (auto rootValueAndName =
              VariableNameInferrer::inferNameAndRoot(transferOp->get())) {
        return diagnosticEmitter.emitNamedUseOfStronglyTransferredValue(
            baseLoc, rootValueAndName->first);
      }

      // Otherwise, emit the typed diagnostic.
      return diagnosticEmitter.emitTypedUseOfStronglyTransferredValue(
          baseLoc, baseInferredType);
    }
  }

  auto loc = transferOp->getUser()->getLoc();

  // If we have a partial_apply that is actor isolated, see if we found a
  // transfer error due to us transferring a value into it.
  if (auto *ace = loc.getAsASTNode<AbstractClosureExpr>()) {
    if (ace->getActorIsolation().isActorIsolated()) {
      if (initForIsolatedPartialApply(transferOp, ace)) {
        return;
      }
    }
  }

  if (auto *sourceApply = loc.getAsASTNode<ApplyExpr>()) {
    // Before we do anything further, see if we can find a name and emit a name
    // error.
    if (auto rootValueAndName =
            VariableNameInferrer::inferNameAndRoot(transferOp->get())) {
      auto &state = transferringOpToStateMap.get(transferOp);
      return diagnosticEmitter.emitNamedIsolationCrossingError(
          baseLoc, rootValueAndName->first,
          state.isolationInfo.getIsolationInfo(),
          *sourceApply->getIsolationCrossing());
    }

    // Otherwise, try to infer from the ApplyExpr.
    return initForApply(transferOp, sourceApply);
  }

  if (auto fas = FullApplySite::isa(transferOp->getUser())) {
    if (auto isolationCrossing = fas.getIsolationCrossing()) {
      return diagnosticEmitter.emitTypedIsolationCrossing(
          baseLoc, baseInferredType, *isolationCrossing);
    }
  }

  auto *autoClosureExpr = loc.getAsASTNode<AutoClosureExpr>();
  if (!autoClosureExpr) {
    return diagnosticEmitter.emitUnknownPatternError();
  }

  auto *i = transferOp->getUser();
  auto pai = ApplySite::isa(i);
  unsigned captureIndex = pai.getAppliedArgIndex(*transferOp);

  auto &state = transferringOpToStateMap.get(transferOp);
  auto captureInfo =
      autoClosureExpr->getCaptureInfo().getCaptures()[captureIndex];
  auto *captureDecl = captureInfo.getDecl();
  AutoClosureWalker walker(*this, captureDecl,
                           state.isolationInfo.getIsolationInfo());
  autoClosureExpr->walk(walker);
}

// Top level entrypoint for use after transfer diagnostics.
void TransferNonSendableImpl::emitUseAfterTransferDiagnostics() {
  auto *function = regionInfo->getFunction();
  BasicBlockData<BlockLivenessInfo> blockLivenessInfo(function);
  // We use a generation counter so we can lazily reset blockLivenessInfo
  // since we cannot clear it without iterating over it.
  unsigned blockLivenessInfoGeneration = 0;

  if (transferOpToRequireInstMultiMap.empty())
    return;

  LLVM_DEBUG(llvm::dbgs() << "Emitting use after transfer diagnostics.\n");

  for (auto [transferOp, requireInsts] :
       transferOpToRequireInstMultiMap.getRange()) {
    LLVM_DEBUG(llvm::dbgs()
               << "Transfer Op. Number: " << transferOp->getOperandNumber()
               << ". User: " << *transferOp->getUser());

    // Then look for our requires before we emit any error. We want to emit a
    // single we don't understand error if we do not find the require.
    bool didEmitRequireNote = false;
    InstructionSet requireInstsUnique(function);
    RequireLiveness liveness(blockLivenessInfoGeneration, transferOp,
                             blockLivenessInfo);
    ++blockLivenessInfoGeneration;
    liveness.process(requireInsts);

    SmallVector<SILInstruction *, 8> requireInstsForError;
    for (auto *require : requireInsts) {
      // We can have multiple of the same require insts if we had a require
      // and an assign from the same instruction. Our liveness checking
      // above doesn't care about that, but we still need to make sure we do
      // not emit twice.
      if (!requireInstsUnique.insert(require))
        continue;

      // If this was not a last require, do not emit an error.
      if (!liveness.finalRequires.contains(require))
        continue;

      requireInstsForError.push_back(require);
      didEmitRequireNote = true;
    }

    // If we did not emit a require, emit an "unknown pattern" error that
    // tells the user to file a bug. This importantly ensures that we can
    // guarantee that we always find the require if we successfully compile.
    if (!didEmitRequireNote) {
      diagnoseError(transferOp, diag::regionbasedisolation_unknown_pattern);
      continue;
    }

    UseAfterTransferDiagnosticInferrer diagnosticInferrer(
        transferOp, requireInstsForError, regionInfo->getValueMap(),
        regionInfo->getTransferringOpToStateMap());
    diagnosticInferrer.infer();
  }
}

//===----------------------------------------------------------------------===//
//            MARK: Transfer NonTransferrable Diagnostic Inference
//===----------------------------------------------------------------------===//

namespace {

class TransferNonTransferrableDiagnosticEmitter {
  TransferredNonTransferrableInfo info;
  bool emittedErrorDiagnostic = false;

public:
  TransferNonTransferrableDiagnosticEmitter(
      TransferredNonTransferrableInfo info)
      : info(info) {}

  ~TransferNonTransferrableDiagnosticEmitter() {
    if (!emittedErrorDiagnostic) {
      emitUnknownPatternError();
    }
  }

  Operand *getOperand() const { return info.transferredOperand; }

  SILValue getNonTransferrableValue() const {
    return info.nonTransferrable.dyn_cast<SILValue>();
  }

  SILInstruction *getNonTransferringActorIntroducingInst() const {
    return info.nonTransferrable.dyn_cast<SILInstruction *>();
  }

  std::optional<DiagnosticBehavior> getBehaviorLimit() const {
    return getDiagnosticBehaviorLimitForValue(info.transferredOperand->get());
  }

  /// If we can find a callee decl name, return that. None otherwise.
  std::optional<std::pair<DescriptiveDeclKind, DeclName>>
  getTransferringCalleeInfo() const {
    return getTransferringApplyCalleeInfo(info.transferredOperand->getUser());
  }

  /// Return the isolation region info for \p getNonTransferrableValue().
  SILDynamicMergedIsolationInfo getIsolationRegionInfo() const {
    return info.isolationRegionInfo;
  }

  void emitUnknownPatternError() {
    diagnoseError(getOperand()->getUser(),
                  diag::regionbasedisolation_unknown_pattern)
        .limitBehaviorIf(getBehaviorLimit());
  }

  void emitUnknownUse(SILLocation loc) {
    // TODO: This will eventually be an unknown pattern error.
    diagnoseError(loc,
                  diag::regionbasedisolation_task_or_actor_isolated_transferred)
        .limitBehaviorIf(getBehaviorLimit());
  }

  void emitFunctionArgumentApply(SILLocation loc, Type type,
                                 ApplyIsolationCrossing crossing) {
    SmallString<64> descriptiveKindStr;
    {
      llvm::raw_svector_ostream os(descriptiveKindStr);
      getIsolationRegionInfo().printForDiagnostics(os);
    }
    diagnoseError(loc, diag::regionbasedisolation_arg_transferred,
                  descriptiveKindStr, type, crossing.getCalleeIsolation())
        .highlight(getOperand()->getUser()->getLoc().getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
  }

  void emitNamedFunctionArgumentClosure(SILLocation loc, Identifier name,
                                        ApplyIsolationCrossing crossing) {
    emitNamedOnlyError(loc, name);
    SmallString<64> descriptiveKindStr;
    {
      if (!getIsolationRegionInfo().isDisconnected()) {
        llvm::raw_svector_ostream os(descriptiveKindStr);
        getIsolationRegionInfo().printForDiagnostics(os);
        os << ' ';
      }
    }
    diagnoseNote(loc,
                 diag::regionbasedisolation_named_isolated_closure_yields_race,
                 descriptiveKindStr, name, crossing.getCalleeIsolation(),
                 crossing.getCallerIsolation())
        .highlight(loc.getSourceRange());
  }

  void emitFunctionArgumentApplyStronglyTransferred(SILLocation loc,
                                                    Type type) {
    SmallString<64> descriptiveKindStr;
    {
      llvm::raw_svector_ostream os(descriptiveKindStr);
      getIsolationRegionInfo().printForDiagnostics(os);
    }
    auto diag =
        diag::regionbasedisolation_arg_passed_to_strongly_transferred_param;
    diagnoseError(loc, diag, descriptiveKindStr, type)
        .highlight(getOperand()->getUser()->getLoc().getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
  }

  void emitNamedOnlyError(SILLocation loc, Identifier name) {
    diagnoseError(loc, diag::regionbasedisolation_named_transfer_yields_race,
                  name)
        .highlight(getOperand()->getUser()->getLoc().getSourceRange())
        .limitBehaviorIf(getBehaviorLimit());
  }

  void emitNamedIsolation(SILLocation loc, Identifier name,
                          ApplyIsolationCrossing isolationCrossing) {
    emitNamedOnlyError(loc, name);
    SmallString<64> descriptiveKindStr;
    SmallString<64> descriptiveKindStrWithSpace;
    {
      if (!getIsolationRegionInfo().isDisconnected()) {
        {
          llvm::raw_svector_ostream os(descriptiveKindStr);
          getIsolationRegionInfo().printForDiagnostics(os);
        }
        descriptiveKindStrWithSpace = descriptiveKindStr;
        descriptiveKindStrWithSpace.push_back(' ');
      }
    }
    if (auto calleeInfo = getTransferringCalleeInfo()) {
      diagnoseNote(
          loc,
          diag::regionbasedisolation_named_transfer_non_transferrable_callee,
          name, descriptiveKindStrWithSpace,
          isolationCrossing.getCalleeIsolation(), calleeInfo->first,
          calleeInfo->second, descriptiveKindStr);
    } else {
      diagnoseNote(loc,
                   diag::regionbasedisolation_named_transfer_non_transferrable,
                   name, descriptiveKindStrWithSpace,
                   isolationCrossing.getCalleeIsolation(), descriptiveKindStr);
    }
  }

  void emitNamedFunctionArgumentApplyStronglyTransferred(SILLocation loc,
                                                         Identifier varName) {
    emitNamedOnlyError(loc, varName);
    SmallString<64> descriptiveKindStr;
    {
      if (!getIsolationRegionInfo().isDisconnected()) {
        llvm::raw_svector_ostream os(descriptiveKindStr);
        getIsolationRegionInfo().printForDiagnostics(os);
        os << ' ';
      }
    }
    auto diag = diag::regionbasedisolation_named_transfer_into_sending_param;
    diagnoseNote(loc, diag, descriptiveKindStr, varName);
  }

  void emitNamedTransferringReturn(SILLocation loc, Identifier varName) {
    emitNamedOnlyError(loc, varName);
    SmallString<64> descriptiveKindStr;
    SmallString<64> descriptiveKindStrWithSpace;
    {
      if (!getIsolationRegionInfo().isDisconnected()) {
        {
          llvm::raw_svector_ostream os(descriptiveKindStr);
          getIsolationRegionInfo().printForDiagnostics(os);
        }
        descriptiveKindStrWithSpace = descriptiveKindStr;
        descriptiveKindStrWithSpace.push_back(' ');
      }
    }
    auto diag =
        diag::regionbasedisolation_named_notransfer_transfer_into_result;
    diagnoseNote(loc, diag, descriptiveKindStrWithSpace, varName,
                 descriptiveKindStr);
  }

private:
  ASTContext &getASTContext() const {
    return getOperand()->getFunction()->getASTContext();
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseError(SourceLoc loc, Diag<T...> diag,
                                   U &&...args) {
    emittedErrorDiagnostic = true;
    return std::move(getASTContext()
                         .Diags.diagnose(loc, diag, std::forward<U>(args)...)
                         .warnUntilSwiftVersion(6));
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseError(SILLocation loc, Diag<T...> diag,
                                   U &&...args) {
    return diagnoseError(loc.getSourceLoc(), diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseError(SILInstruction *inst, Diag<T...> diag,
                                   U &&...args) {
    return diagnoseError(inst->getLoc(), diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseNote(SourceLoc loc, Diag<T...> diag, U &&...args) {
    return getASTContext().Diags.diagnose(loc, diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseNote(SILLocation loc, Diag<T...> diag,
                                  U &&...args) {
    return diagnoseNote(loc.getSourceLoc(), diag, std::forward<U>(args)...);
  }

  template <typename... T, typename... U>
  InFlightDiagnostic diagnoseNote(SILInstruction *inst, Diag<T...> diag,
                                  U &&...args) {
    return diagnoseNote(inst->getLoc(), diag, std::forward<U>(args)...);
  }
};

class TransferNonTransferrableDiagnosticInferrer {
  TransferNonTransferrableDiagnosticEmitter diagnosticEmitter;

public:
  TransferNonTransferrableDiagnosticInferrer(
      TransferredNonTransferrableInfo info)
      : diagnosticEmitter(info) {}

  /// Gathers diagnostics. Returns false if we emitted a "I don't understand
  /// error". If we emit such an error, we should bail without emitting any
  /// further diagnostics, since we may not have any diagnostics or be in an
  /// inconcistent state.
  bool run();

private:
  /// \p actualCallerIsolation is used to override the caller isolation we use
  /// when emitting the error if the closure would have the incorrect one.
  bool initForIsolatedPartialApply(
      Operand *op, AbstractClosureExpr *ace,
      std::optional<ActorIsolation> actualCallerIsolation = {});
};

} // namespace

bool TransferNonTransferrableDiagnosticInferrer::initForIsolatedPartialApply(
    Operand *op, AbstractClosureExpr *ace,
    std::optional<ActorIsolation> actualCallerIsolation) {
  SmallVector<std::tuple<CapturedValue, unsigned, ApplyIsolationCrossing>, 8>
      foundCapturedIsolationCrossing;
  ace->getIsolationCrossing(foundCapturedIsolationCrossing);
  if (foundCapturedIsolationCrossing.empty())
    return false;

  unsigned opIndex = ApplySite(op->getUser()).getAppliedArgIndex(*op);
  for (auto &p : foundCapturedIsolationCrossing) {
    if (std::get<1>(p) == opIndex) {
      auto loc = RegularLocation(std::get<0>(p).getLoc());
      auto crossing = std::get<2>(p);
      auto declIsolation = crossing.getCallerIsolation();
      auto closureIsolation = crossing.getCalleeIsolation();
      if (!bool(declIsolation) && actualCallerIsolation) {
        declIsolation = *actualCallerIsolation;
      }
      diagnosticEmitter.emitNamedFunctionArgumentClosure(
          loc, std::get<0>(p).getDecl()->getBaseIdentifier(),
          ApplyIsolationCrossing(declIsolation, closureIsolation));
      return true;
    }
  }

  return false;
}

bool TransferNonTransferrableDiagnosticInferrer::run() {
  // We need to find the isolation info.
  auto *op = diagnosticEmitter.getOperand();
  auto loc = op->getUser()->getLoc();

  if (auto *sourceApply = loc.getAsASTNode<ApplyExpr>()) {
    // First see if we have a transferring argument.
    if (auto fas = FullApplySite::isa(op->getUser())) {
      if (fas.getArgumentParameterInfo(*op).hasOption(
              SILParameterInfo::Sending)) {

        // See if we can infer a name from the value.
        SmallString<64> resultingName;
        if (auto varName = VariableNameInferrer::inferName(op->get())) {
          diagnosticEmitter.emitNamedFunctionArgumentApplyStronglyTransferred(
              loc, *varName);
          return true;
        }

        Type type = op->get()->getType().getASTType();
        if (auto *inferredArgExpr =
                inferArgumentExprFromApplyExpr(sourceApply, fas, op)) {
          type = inferredArgExpr->findOriginalType();
        }
        diagnosticEmitter.emitFunctionArgumentApplyStronglyTransferred(loc,
                                                                       type);
        return true;
      }
    }

    // First try to get the apply from the isolation crossing.
    auto isolation = sourceApply->getIsolationCrossing();

    // If we could not infer an isolation...
    if (!isolation) {
      // Otherwise, emit a "we don't know error" that tells the user to file a
      // bug.
      diagnosticEmitter.emitUnknownPatternError();
      return false;
    }
    assert(isolation && "Expected non-null");

    // Then if we are calling a closure expr. If so, we should use the loc of
    // the closure.
    if (auto *closureExpr =
            dyn_cast<AbstractClosureExpr>(sourceApply->getFn())) {
      initForIsolatedPartialApply(op, closureExpr,
                                  isolation->getCallerIsolation());
      return true;
    }

    // See if we can infer a name from the value.
    SmallString<64> resultingName;
    if (auto name = VariableNameInferrer::inferName(op->get())) {
      diagnosticEmitter.emitNamedIsolation(loc, *name, *isolation);
      return true;
    }

    // Attempt to find the specific sugared ASTType if we can to emit a better
    // diagnostic.
    Type type = op->get()->getType().getASTType();
    if (auto fas = FullApplySite::isa(op->getUser())) {
      if (auto *inferredArgExpr =
              inferArgumentExprFromApplyExpr(sourceApply, fas, op)) {
        type = inferredArgExpr->findOriginalType();
      }
    }

    diagnosticEmitter.emitFunctionArgumentApply(loc, type, *isolation);
    return true;
  }

  if (auto *ace = loc.getAsASTNode<AbstractClosureExpr>()) {
    if (ace->getActorIsolation().isActorIsolated()) {
      if (initForIsolatedPartialApply(op, ace)) {
        return true;
      }
    }
  }

  // See if we are in SIL and have an apply site specified isolation.
  if (auto fas = FullApplySite::isa(op->getUser())) {
    if (auto isolation = fas.getIsolationCrossing()) {
      diagnosticEmitter.emitFunctionArgumentApply(
          loc, op->get()->getType().getASTType(), *isolation);
      return true;
    }
  }

  if (auto *ri = dyn_cast<ReturnInst>(op->getUser())) {
    auto fType = ri->getFunction()->getLoweredFunctionType();
    if (fType->getNumResults() &&
        fType->getResults()[0].hasOption(SILResultInfo::IsSending)) {
      assert(llvm::all_of(fType->getResults(),
                          [](SILResultInfo resultInfo) {
                            return resultInfo.hasOption(
                                SILResultInfo::IsSending);
                          }) &&
             "All result info must be the same... if that changes... update "
             "this code!");
      SmallString<64> resultingName;
      if (auto name = VariableNameInferrer::inferName(op->get())) {
        diagnosticEmitter.emitNamedTransferringReturn(loc, *name);
        return true;
      }
    } else {
      assert(llvm::none_of(fType->getResults(),
                           [](SILResultInfo resultInfo) {
                             return resultInfo.hasOption(
                                 SILResultInfo::IsSending);
                           }) &&
             "All result info must be the same... if that changes... update "
             "this code!");
    }
  }

  diagnosticEmitter.emitUnknownUse(loc);
  return true;
}

// Top level emission for transfer non transferable diagnostic.
void TransferNonSendableImpl::emitTransferredNonTransferrableDiagnostics() {
  if (transferredNonTransferrableInfoList.empty())
    return;

  LLVM_DEBUG(
      llvm::dbgs() << "Emitting transfer non transferrable diagnostics.\n");

  for (auto info : transferredNonTransferrableInfoList) {
    TransferNonTransferrableDiagnosticInferrer diagnosticInferrer(info);
    diagnosticInferrer.run();
  }
}

//===----------------------------------------------------------------------===//
//                         MARK: Diagnostic Evaluator
//===----------------------------------------------------------------------===//

namespace {

struct DiagnosticEvaluator final
    : PartitionOpEvaluatorBaseImpl<DiagnosticEvaluator> {
  RegionAnalysisFunctionInfo *info;
  SmallFrozenMultiMap<Operand *, SILInstruction *, 8>
      &transferOpToRequireInstMultiMap;

  /// First value is the operand that was transferred... second value is the
  /// non-transferrable value in the same region as that value. The second value
  /// is what is non-transferrable.
  SmallVectorImpl<TransferredNonTransferrableInfo> &transferredNonTransferrable;

  DiagnosticEvaluator(Partition &workingPartition,
                      RegionAnalysisFunctionInfo *info,
                      SmallFrozenMultiMap<Operand *, SILInstruction *, 8>
                          &transferOpToRequireInstMultiMap,
                      SmallVectorImpl<TransferredNonTransferrableInfo>
                          &transferredNonTransferrable,
                      TransferringOperandToStateMap &operandToStateMap)
      : PartitionOpEvaluatorBaseImpl(
            workingPartition, info->getOperandSetFactory(), operandToStateMap),
        info(info),
        transferOpToRequireInstMultiMap(transferOpToRequireInstMultiMap),
        transferredNonTransferrable(transferredNonTransferrable) {}

  void handleLocalUseAfterTransfer(const PartitionOp &partitionOp,
                                   Element transferredVal,
                                   Operand *transferringOp) const {
    auto &operandState = operandToStateMap.get(transferringOp);
    // Ignore this if we have a gep like instruction that is returning a
    // sendable type and transferringOp was not set with closure
    // capture.
    if (auto *svi =
            dyn_cast<SingleValueInstruction>(partitionOp.getSourceInst())) {
      if (isa<TupleElementAddrInst, StructElementAddrInst>(svi) &&
          !SILIsolationInfo::isNonSendableType(svi->getType(),
                                               svi->getFunction())) {
        bool isCapture = operandState.isClosureCaptured;
        if (!isCapture) {
          return;
        }
      }
    }

    auto rep = info->getValueMap().getRepresentative(transferredVal);
    LLVM_DEBUG(llvm::dbgs()
               << "    Emitting Use After Transfer Error!\n"
               << "        Transferring Inst: " << *transferringOp->getUser()
               << "        Transferring Op Value: " << transferringOp->get()
               << "        Require Inst: " << *partitionOp.getSourceInst()
               << "        ID:  %%" << transferredVal << "\n"
               << "        Rep: " << *rep << "        Transferring Op Num: "
               << transferringOp->getOperandNumber() << '\n');
    transferOpToRequireInstMultiMap.insert(transferringOp,
                                           partitionOp.getSourceInst());
  }

  void handleTransferNonTransferrable(
      const PartitionOp &partitionOp, Element transferredVal,
      SILDynamicMergedIsolationInfo isolationRegionInfo) const {
    LLVM_DEBUG(llvm::dbgs()
                   << "    Emitting TransferNonTransferrable Error!\n"
                   << "        ID:  %%" << transferredVal << "\n"
                   << "        Rep: "
                   << *info->getValueMap().getRepresentative(transferredVal)
                   << "        Dynamic Isolation Region: ";
               isolationRegionInfo.printForDiagnostics(llvm::dbgs());
               llvm::dbgs() << '\n');
    auto *self = const_cast<DiagnosticEvaluator *>(this);
    auto nonTransferrableValue =
        info->getValueMap().getRepresentative(transferredVal);

    self->transferredNonTransferrable.emplace_back(
        partitionOp.getSourceOp(), nonTransferrableValue, isolationRegionInfo);
  }

  void handleTransferNonTransferrable(
      const PartitionOp &partitionOp, Element transferredVal,
      Element actualNonTransferrableValue,
      SILDynamicMergedIsolationInfo isolationRegionInfo) const {
    LLVM_DEBUG(llvm::dbgs()
                   << "    Emitting TransferNonTransferrable Error!\n"
                   << "        ID:  %%" << transferredVal << "\n"
                   << "        Rep: "
                   << *info->getValueMap().getRepresentative(transferredVal)
                   << "        Dynamic Isolation Region: ";
               isolationRegionInfo.printForDiagnostics(llvm::dbgs());
               llvm::dbgs() << '\n');

    auto *self = const_cast<DiagnosticEvaluator *>(this);
    // If we have a non-actor introducing fake representative value, just use
    // the value that actually introduced the actor isolation.
    if (auto nonTransferrableValue = info->getValueMap().maybeGetRepresentative(
            actualNonTransferrableValue)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "        ActualTransfer: " << nonTransferrableValue);
      self->transferredNonTransferrable.emplace_back(partitionOp.getSourceOp(),
                                                     nonTransferrableValue,
                                                     isolationRegionInfo);
    } else if (auto *nonTransferrableInst =
                   info->getValueMap().maybeGetActorIntroducingInst(
                       actualNonTransferrableValue)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "        ActualTransfer: " << *nonTransferrableInst);
      self->transferredNonTransferrable.emplace_back(
          partitionOp.getSourceOp(), nonTransferrableInst, isolationRegionInfo);
    } else {
      // Otherwise, just use the actual value.
      //
      // TODO: We are eventually going to want to be able to say that it is b/c
      // of the actor isolated parameter. Maybe we should put in the actual
      // region isolation info here.
      self->transferredNonTransferrable.emplace_back(
          partitionOp.getSourceOp(),
          info->getValueMap().getRepresentative(transferredVal),
          isolationRegionInfo);
    }
  }

  void handleUnknownCodePattern(const PartitionOp &op) const {
    diagnoseError(op.getSourceInst(),
                  diag::regionbasedisolation_unknown_pattern);
  }

  bool isActorDerived(Element element) const {
    return info->getValueMap().getIsolationRegion(element).isActorIsolated();
  }

  bool isTaskIsolatedDerived(Element element) const {
    return info->getValueMap().getIsolationRegion(element).isTaskIsolated();
  }

  SILIsolationInfo::Kind hasSpecialDerivation(Element element) const {
    return info->getValueMap().getIsolationRegion(element).getKind();
  }

  SILIsolationInfo getIsolationRegionInfo(Element element) const {
    return info->getValueMap().getIsolationRegion(element);
  }

  std::optional<Element> getElement(SILValue value) const {
    return info->getValueMap().getTrackableValue(value).getID();
  }

  SILValue getRepresentative(SILValue value) const {
    return info->getValueMap()
        .getTrackableValue(value)
        .getRepresentative()
        .maybeGetValue();
  }

  bool isClosureCaptured(Element element, Operand *op) const {
    auto value = info->getValueMap().maybeGetRepresentative(element);
    if (!value)
      return false;
    return info->isClosureCaptured(value, op);
  }
};

} // namespace

void TransferNonSendableImpl::runDiagnosticEvaluator() {
  // Then for each block...
  LLVM_DEBUG(llvm::dbgs() << "Walking blocks for diagnostics.\n");
  for (auto [block, blockState] : regionInfo->getRange()) {
    LLVM_DEBUG(llvm::dbgs() << "|--> Block bb" << block.getDebugID() << "\n");

    if (!blockState.getLiveness()) {
      LLVM_DEBUG(llvm::dbgs() << "Dead block... skipping!\n");
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "Entry Partition: ";
               blockState.getEntryPartition().print(llvm::dbgs()));

    // Grab its entry partition and setup an evaluator for the partition that
    // has callbacks that emit diagnsotics...
    Partition workingPartition = blockState.getEntryPartition();
    DiagnosticEvaluator eval(workingPartition, regionInfo,
                             transferOpToRequireInstMultiMap,
                             transferredNonTransferrableInfoList,
                             regionInfo->getTransferringOpToStateMap());

    // And then evaluate all of our partition ops on the entry partition.
    for (auto &partitionOp : blockState.getPartitionOps()) {
      eval.apply(partitionOp);
    }

    LLVM_DEBUG(llvm::dbgs() << "Exit Partition: ";
               workingPartition.print(llvm::dbgs()));
  }

  LLVM_DEBUG(llvm::dbgs() << "Finished walking blocks for diagnostics.\n");

  // Now that we have found all of our transferInsts/Requires emit errors.
  transferOpToRequireInstMultiMap.setFrozen();
}

//===----------------------------------------------------------------------===//
//                         MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

/// Once we have reached a fixpoint, this routine runs over all blocks again
/// reporting any failures by applying our ops to the converged dataflow
/// state.
void TransferNonSendableImpl::emitDiagnostics() {
  auto *function = regionInfo->getFunction();
  LLVM_DEBUG(llvm::dbgs() << "Emitting diagnostics for function "
                          << function->getName() << "\n");

  runDiagnosticEvaluator();
  emitTransferredNonTransferrableDiagnostics();
  emitUseAfterTransferDiagnostics();
}

namespace {

class TransferNonSendable : public SILFunctionTransform {
  void run() override {
    SILFunction *function = getFunction();

    auto *functionInfo = getAnalysis<RegionAnalysis>()->get(function);
    if (!functionInfo->isSupportedFunction()) {
      LLVM_DEBUG(llvm::dbgs() << "===> SKIPPING UNSUPPORTED FUNCTION: "
                              << function->getName() << '\n');

      return;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "===> PROCESSING: " << function->getName() << '\n');

    TransferNonSendableImpl impl(functionInfo);
    impl.emitDiagnostics();
  }
};

} // end anonymous namespace

SILTransform *swift::createTransferNonSendable() {
  return new TransferNonSendable();
}
