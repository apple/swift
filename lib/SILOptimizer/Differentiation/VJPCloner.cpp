//===--- VJPCloner.cpp - VJP function generation --------------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines a helper class for generating VJP functions for automatic
// differentiation.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/VJPCloner.h"
#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Differentiation/ADContext.h"
#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"
#include "swift/SILOptimizer/Differentiation/LinearMapInfo.h"
#include "swift/SILOptimizer/Differentiation/PullbackCloner.h"
#include "swift/SILOptimizer/Differentiation/Thunk.h"

#include "swift/SIL/TerminatorUtils.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/PrettyStackTrace.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/DifferentiationMangler.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {
namespace autodiff {

class VJPCloner::Implementation final
    : public TypeSubstCloner<VJPCloner::Implementation, SILOptFunctionBuilder> {
  friend class VJPCloner;
  friend class PullbackCloner;

  /// The parent VJP cloner.
  VJPCloner &cloner;

  /// The global context.
  ADContext &context;

  /// The original function.
  SILFunction *const original;

  /// The differentiability witness.
  SILDifferentiabilityWitness *const witness;

  /// The VJP function.
  SILFunction *const vjp;

  /// The pullback function.
  SILFunction *pullback;

  /// The differentiation invoker.
  DifferentiationInvoker invoker;

  /// Info from activity analysis on the original function.
  const DifferentiableActivityInfo &activityInfo;

  /// The loop info.
  SILLoopInfo *loopInfo;

  /// The linear map info.
  LinearMapInfo pullbackInfo;

  /// Caches basic blocks whose phi arguments have been remapped (adding a
  /// predecessor enum argument).
  SmallPtrSet<SILBasicBlock *, 4> remappedBasicBlocks;

  /// The `AutoDiffLinearMapContext` object. If null, no explicit context is
  /// needed (no loops).
  SILValue pullbackContextValue;
  /// The unique, borrowed context object. This is valid until the exit block.
  SILValue borrowedPullbackContextValue;

  /// The generic signature of the `Builtin.autoDiffAllocateSubcontext(_:_:)`
  /// declaration. It is used for creating a builtin call.
  GenericSignature builtinAutoDiffAllocateSubcontextGenericSignature;

  bool errorOccurred = false;

  /// Mapping from original blocks to pullback values. Used to build pullback
  /// struct instances.
  llvm::DenseMap<SILBasicBlock *, SmallVector<SILValue, 8>> pullbackValues;

  ASTContext &getASTContext() const { return vjp->getASTContext(); }
  SILModule &getModule() const { return vjp->getModule(); }
  const AutoDiffConfig &getConfig() const {
    return witness->getConfig();
  }

  Implementation(VJPCloner &parent, ADContext &context,
                 SILDifferentiabilityWitness *witness, SILFunction *vjp,
                 DifferentiationInvoker invoker);

  /// Creates an empty pullback function, to be filled in by `PullbackCloner`.
  SILFunction *createEmptyPullback();

  /// Run VJP generation. Returns true on error.
  bool run();

  /// Initializes a context object if needed.
  void emitLinearMapContextInitializationIfNeeded() {
    if (!pullbackInfo.hasHeapAllocatedContext())
      return;
 
    // Get linear map struct size.
    auto *returnBB = &*original->findReturnBB();
    auto pullbackTupleType =
      remapASTType(pullbackInfo.getLinearMapTupleType(returnBB)->getCanonicalType());
    Builder.setInsertionPoint(vjp->getEntryBlock());
    auto topLevelSubcontextSize = emitMemoryLayoutSize(
        Builder, original->getLocation(), pullbackTupleType);
    // Create an context.
    pullbackContextValue = Builder.createBuiltin(
        original->getLocation(),
        getASTContext().getIdentifier(
            getBuiltinName(BuiltinValueKind::AutoDiffCreateLinearMapContext)),
        SILType::getNativeObjectType(getASTContext()),
        SubstitutionMap(), {topLevelSubcontextSize});
    borrowedPullbackContextValue = Builder.createBeginBorrow(
        original->getLocation(), pullbackContextValue);
    LLVM_DEBUG(getADDebugStream()
               << "Context object initialized because there are loops\n"
               << *vjp->getEntryBlock() << '\n'
               << "pullback tuple type: " << pullbackTupleType << '\n');
  }

  /// Get the lowered SIL type of the given AST type.
  SILType getLoweredType(Type type) {
    auto vjpGenSig = vjp->getLoweredFunctionType()->getSubstGenericSignature();
    Lowering::AbstractionPattern pattern(vjpGenSig,
                                         type->getReducedType(vjpGenSig));
    return vjp->getLoweredType(pattern, type);
  }

  GenericSignature getBuiltinAutoDiffAllocateSubcontextDecl() {
    if (builtinAutoDiffAllocateSubcontextGenericSignature)
      return builtinAutoDiffAllocateSubcontextGenericSignature;
    auto &ctx = getASTContext();
    auto *decl = cast<FuncDecl>(getBuiltinValueDecl(
        ctx, ctx.getIdentifier(
            getBuiltinName(BuiltinValueKind::AutoDiffAllocateSubcontext))));
    builtinAutoDiffAllocateSubcontextGenericSignature =
        decl->getGenericSignature();
    assert(builtinAutoDiffAllocateSubcontextGenericSignature);
    return builtinAutoDiffAllocateSubcontextGenericSignature;
  }

  // Creates a trampoline block for given original terminator instruction, the
  // pullback struct value for its parent block, and a successor basic block.
  //
  // The trampoline block has the same arguments as and branches to the remapped
  // successor block, but drops the last predecessor enum argument.
  //
  // Used for cloning branching terminator instructions with specific
  // requirements on successor block arguments, where an additional predecessor
  // enum argument is not acceptable.
  SILBasicBlock *createTrampolineBasicBlock(TermInst *termInst,
                                            TupleInst *pbTupleVal,
                                            SILBasicBlock *succBB);

  /// Build a pullback tuple value for the given original terminator
  /// instruction.
  TupleInst *buildPullbackValueTupleValue(TermInst *termInst);
  llvm::SmallVector<SILValue, 8> getPullbackValues(SILBasicBlock *origBB);

  /// Build a predecessor enum instance using the given builder for the given
  /// original predecessor/successor blocks and pullback struct value.
  EnumInst *buildPredecessorEnumValue(SILBuilder &builder,
                                      SILBasicBlock *predBB,
                                      SILBasicBlock *succBB,
                                      SILValue pbTupleVal);

public:
  /// Remap original basic blocks, adding predecessor enum arguments.
  SILBasicBlock *remapBasicBlock(SILBasicBlock *bb) {
    auto *vjpBB = BBMap[bb];
    // If error has occurred, or if block has already been remapped, return
    // remapped, return remapped block.
    if (errorOccurred || remappedBasicBlocks.count(bb))
      return vjpBB;
    // Add predecessor enum argument to the remapped block.
    auto *predEnum = pullbackInfo.getBranchingTraceDecl(bb);
    auto enumTy =
        getOpASTType(predEnum->getDeclaredInterfaceType()->getCanonicalType());
    auto enumLoweredTy = context.getTypeConverter().getLoweredType(
        enumTy, TypeExpansionContext::minimal());
    vjpBB->createPhiArgument(enumLoweredTy, OwnershipKind::Owned);
    remappedBasicBlocks.insert(bb);
    return vjpBB;
  }

  /// General visitor for all instructions. If any error is emitted by previous
  /// visits, bail out.
  void visit(SILInstruction *inst) {
    if (errorOccurred)
      return;
    TypeSubstCloner::visit(inst);
  }

  void visitSILInstruction(SILInstruction *inst) {
    context.emitNondifferentiabilityError(
        inst, invoker, diag::autodiff_expression_not_differentiable_note);
    errorOccurred = true;
  }

  void postProcess(SILInstruction *orig, SILInstruction *cloned) {
    if (errorOccurred)
      return;
    SILClonerWithScopes::postProcess(orig, cloned);
  }

  void visitReturnInst(ReturnInst *ri) {
    Builder.setCurrentDebugScope(getOpScope(ri->getDebugScope()));
    auto loc = ri->getOperand().getLoc();
    // Build pullback tuple value for original block.
    auto *origExit = ri->getParent();

    // Get the value in the VJP corresponding to the original result.
    auto *origRetInst = cast<ReturnInst>(origExit->getTerminator());
    auto origResult = getOpValue(origRetInst->getOperand());
    SmallVector<SILValue, 8> origResults;
    extractAllElements(origResult, Builder, origResults);

    // Get and partially apply the pullback.
    auto vjpSubstMap = vjp->getForwardingSubstitutionMap();
    auto *pullbackRef = Builder.createFunctionRef(loc, pullback);

    // Prepare partial application arguments.
    SILValue partialApplyArg;
    PartialApplyInst *pullbackPartialApply;
    if (borrowedPullbackContextValue) {
      auto *pbTupleVal = buildPullbackValueTupleValue(ri);
      // Initialize the top-level subcontext buffer with the top-level pullback
      // tuple.
      auto addr = emitProjectTopLevelSubcontext(
          Builder, loc, borrowedPullbackContextValue, pbTupleVal->getType());
      Builder.createStore(
          loc, pbTupleVal, addr,
          pbTupleVal->getType().isTrivial(*pullback) ?
              StoreOwnershipQualifier::Trivial : StoreOwnershipQualifier::Init);

      Builder.createEndBorrow(loc, borrowedPullbackContextValue);
      pullbackPartialApply = Builder.createPartialApply(
        loc, pullbackRef, vjpSubstMap, {pullbackContextValue},
        ParameterConvention::Direct_Guaranteed);
    } else {
      pullbackPartialApply = Builder.createPartialApply(
        loc, pullbackRef, vjpSubstMap, getPullbackValues(origExit),
        ParameterConvention::Direct_Guaranteed);
    }

    auto pullbackType = vjp->mapTypeIntoContext(
        vjp->getConventions().getSILType(
            vjp->getLoweredFunctionType()->getResults().back(),
            vjp->getTypeExpansionContext()));
    auto pullbackFnType = pullbackType.castTo<SILFunctionType>();
    auto pullbackSubstType =
        pullbackPartialApply->getType().castTo<SILFunctionType>();

    // If necessary, convert the pullback value to the returned pullback
    // function type.
    SILValue pullbackValue;
    if (pullbackSubstType == pullbackFnType) {
      pullbackValue = pullbackPartialApply;
    } else if (pullbackSubstType->isABICompatibleWith(pullbackFnType, *vjp)
                   .isCompatible()) {
      pullbackValue =
          Builder.createConvertFunction(loc, pullbackPartialApply, pullbackType,
                                        /*withoutActuallyEscaping*/ false);
    } else {
      llvm::report_fatal_error("Pullback value type is not ABI-compatible "
                               "with the returned pullback type");
    }

    // Return a tuple of the original result and pullback.
    SmallVector<SILValue, 8> directResults;
    directResults.append(origResults.begin(), origResults.end());
    directResults.push_back(pullbackValue);
    Builder.createReturn(ri->getLoc(),
                         joinElements(directResults, Builder, loc));
  }

  void visitBranchInst(BranchInst *bi) {
    Builder.setCurrentDebugScope(getOpScope(bi->getDebugScope()));
    // Build pullback struct value for original block.
    // Build predecessor enum value for destination block.
    auto *origBB = bi->getParent();
    auto *pbTupleVal = buildPullbackValueTupleValue(bi);
    auto *enumVal = buildPredecessorEnumValue(getBuilder(), origBB,
                                              bi->getDestBB(), pbTupleVal);

    // Remap arguments, appending the new enum values.
    SmallVector<SILValue, 8> args;
    for (auto origArg : bi->getArgs())
      args.push_back(getOpValue(origArg));
    args.push_back(enumVal);

    // Create a new `br` instruction.
    getBuilder().createBranch(bi->getLoc(), getOpBasicBlock(bi->getDestBB()),
                              args);
  }

  void visitCondBranchInst(CondBranchInst *cbi) {
    Builder.setCurrentDebugScope(getOpScope(cbi->getDebugScope()));
    // Build pullback struct value for original block.
    auto *pbTupleVal = buildPullbackValueTupleValue(cbi);
    // Create a new `cond_br` instruction.
    getBuilder().createCondBranch(
        cbi->getLoc(), getOpValue(cbi->getCondition()),
        createTrampolineBasicBlock(cbi, pbTupleVal, cbi->getTrueBB()),
        createTrampolineBasicBlock(cbi, pbTupleVal, cbi->getFalseBB()));
  }

  void visitSwitchEnumTermInst(SwitchEnumTermInst inst) {
    Builder.setCurrentDebugScope(getOpScope(inst->getDebugScope()));
    // Build pullback tuple value for original block.
    auto *pbTupleVal = buildPullbackValueTupleValue(*inst);

    // Create trampoline successor basic blocks.
    SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 4> caseBBs;
    for (unsigned i : range(inst.getNumCases())) {
      auto caseBB = inst.getCase(i);
      auto *trampolineBB =
          createTrampolineBasicBlock(inst, pbTupleVal, caseBB.second);
      caseBBs.push_back({caseBB.first, trampolineBB});
    }
    // Create trampoline default basic block.
    SILBasicBlock *newDefaultBB = nullptr;
    if (auto *defaultBB = inst.getDefaultBBOrNull().getPtrOrNull())
      newDefaultBB = createTrampolineBasicBlock(inst, pbTupleVal, defaultBB);

    // Create a new `switch_enum` instruction.
    switch (inst->getKind()) {
    case SILInstructionKind::SwitchEnumInst:
      getBuilder().createSwitchEnum(
          inst->getLoc(), getOpValue(inst.getOperand()), newDefaultBB, caseBBs);
      break;
    case SILInstructionKind::SwitchEnumAddrInst:
      getBuilder().createSwitchEnumAddr(
          inst->getLoc(), getOpValue(inst.getOperand()), newDefaultBB, caseBBs);
      break;
    default:
      llvm_unreachable("Expected `switch_enum` or `switch_enum_addr`");
    }
  }

  void visitSwitchEnumInst(SwitchEnumInst *sei) {
    visitSwitchEnumTermInst(sei);
  }

  void visitSwitchEnumAddrInst(SwitchEnumAddrInst *seai) {
    visitSwitchEnumTermInst(seai);
  }

  void visitCheckedCastBranchInst(CheckedCastBranchInst *ccbi) {
    Builder.setCurrentDebugScope(getOpScope(ccbi->getDebugScope()));
    // Build pullback struct value for original block.
    auto *pbTupleVal = buildPullbackValueTupleValue(ccbi);
    // Create a new `checked_cast_branch` instruction.
    getBuilder().createCheckedCastBranch(
        ccbi->getLoc(), ccbi->isExact(), getOpValue(ccbi->getOperand()),
        getOpASTType(ccbi->getSourceFormalType()),
        getOpType(ccbi->getTargetLoweredType()),
        getOpASTType(ccbi->getTargetFormalType()),
        createTrampolineBasicBlock(ccbi, pbTupleVal, ccbi->getSuccessBB()),
        createTrampolineBasicBlock(ccbi, pbTupleVal, ccbi->getFailureBB()),
        ccbi->getTrueBBCount(), ccbi->getFalseBBCount());
  }

  void visitCheckedCastAddrBranchInst(CheckedCastAddrBranchInst *ccabi) {
    Builder.setCurrentDebugScope(getOpScope(ccabi->getDebugScope()));
    // Build pullback struct value for original block.
    auto *pbTupleVal = buildPullbackValueTupleValue(ccabi);
    // Create a new `checked_cast_addr_branch` instruction.
    getBuilder().createCheckedCastAddrBranch(
        ccabi->getLoc(), ccabi->getConsumptionKind(),
        getOpValue(ccabi->getSrc()), getOpASTType(ccabi->getSourceFormalType()),
        getOpValue(ccabi->getDest()),
        getOpASTType(ccabi->getTargetFormalType()),
        createTrampolineBasicBlock(ccabi, pbTupleVal, ccabi->getSuccessBB()),
        createTrampolineBasicBlock(ccabi, pbTupleVal, ccabi->getFailureBB()),
        ccabi->getTrueBBCount(), ccabi->getFalseBBCount());
  }

  // If an `apply` has active results or active inout arguments, replace it
  // with an `apply` of its VJP.
  void visitApplyInst(ApplyInst *ai) {
    // If callee should not be differentiated, do standard cloning.
    if (!pullbackInfo.shouldDifferentiateApplySite(ai)) {
      LLVM_DEBUG(getADDebugStream() << "No active results:\n" << *ai << '\n');
      TypeSubstCloner::visitApplyInst(ai);
      return;
    }
    // If callee is `array.uninitialized_intrinsic`, do standard cloning.
    // `array.uninitialized_intrinsic` differentiation is handled separately.
    if (ArraySemanticsCall(ai, semantics::ARRAY_UNINITIALIZED_INTRINSIC)) {
      LLVM_DEBUG(getADDebugStream()
                 << "Cloning `array.uninitialized_intrinsic` `apply`:\n"
                 << *ai << '\n');
      TypeSubstCloner::visitApplyInst(ai);
      return;
    }
    // If callee is `array.finalize_intrinsic`, do standard cloning.
    // `array.finalize_intrinsic` has special-case pullback generation.
    if (ArraySemanticsCall(ai, semantics::ARRAY_FINALIZE_INTRINSIC)) {
      LLVM_DEBUG(getADDebugStream()
                 << "Cloning `array.finalize_intrinsic` `apply`:\n"
                 << *ai << '\n');
      TypeSubstCloner::visitApplyInst(ai);
      return;
    }
    // If the original function is a semantic member accessor, do standard
    // cloning. Semantic member accessors have special pullback generation
    // logic, so all `apply` instructions can be directly cloned to the VJP.
    if (isSemanticMemberAccessor(original)) {
      LLVM_DEBUG(getADDebugStream()
                 << "Cloning `apply` in semantic member accessor:\n"
                 << *ai << '\n');
      TypeSubstCloner::visitApplyInst(ai);
      return;
    }

    Builder.setCurrentDebugScope(getOpScope(ai->getDebugScope()));
    auto loc = ai->getLoc();
    auto &builder = getBuilder();
    auto origCallee = getOpValue(ai->getCallee());
    auto originalFnTy = origCallee->getType().castTo<SILFunctionType>();

    LLVM_DEBUG(getADDebugStream() << "VJP-transforming:\n" << *ai << '\n');

    // Get the minimal parameter and result indices required for differentiating
    // this `apply`.
    SmallVector<SILValue, 4> allResults;
    SmallVector<unsigned, 8> activeParamIndices;
    SmallVector<unsigned, 8> activeResultIndices;
    collectMinimalIndicesForFunctionCall(ai, getConfig(), activityInfo,
                                         allResults, activeParamIndices,
                                         activeResultIndices);
    assert(!activeParamIndices.empty() && "Parameter indices cannot be empty");
    assert(!activeResultIndices.empty() && "Result indices cannot be empty");
    LLVM_DEBUG(auto &s = getADDebugStream() << "Active indices: params=(";
               llvm::interleave(
                   activeParamIndices.begin(), activeParamIndices.end(),
                   [&s](unsigned i) { s << i; }, [&s] { s << ", "; });
               s << "), results=("; llvm::interleave(
                   activeResultIndices.begin(), activeResultIndices.end(),
                   [&s](unsigned i) { s << i; }, [&s] { s << ", "; });
               s << ")\n";);

    // Form expected indices.
    AutoDiffConfig config(
        IndexSubset::get(getASTContext(),
                         ai->getArgumentsWithoutIndirectResults().size(),
                         activeParamIndices),
        IndexSubset::get(getASTContext(),
                         ai->getSubstCalleeType()->getNumAutoDiffSemanticResults(),
                         activeResultIndices));

    // Emit the VJP.
    SILValue vjpValue;
    // If functionSource is a `@differentiable` function, just extract it.
    if (originalFnTy->isDifferentiable()) {
      auto paramIndices = originalFnTy->getDifferentiabilityParameterIndices();
      for (auto i : config.parameterIndices->getIndices()) {
        if (!paramIndices->contains(i)) {
          context.emitNondifferentiabilityError(
              origCallee, invoker,
              diag::
                  autodiff_function_noderivative_parameter_not_differentiable);
          errorOccurred = true;
          return;
        }
      }
      builder.emitScopedBorrowOperation(
          loc, origCallee, [&](SILValue borrowedDiffFunc) {
            auto origFnType = origCallee->getType().castTo<SILFunctionType>();
            auto origFnUnsubstType =
                origFnType->getUnsubstitutedType(getModule());
            if (origFnType != origFnUnsubstType) {
              borrowedDiffFunc = builder.createConvertFunction(
                  loc, borrowedDiffFunc,
                  SILType::getPrimitiveObjectType(origFnUnsubstType),
                  /*withoutActuallyEscaping*/ false);
            }
            vjpValue = builder.createDifferentiableFunctionExtract(
                loc, NormalDifferentiableFunctionTypeComponent::VJP,
                borrowedDiffFunc);
            vjpValue = builder.emitCopyValueOperation(loc, vjpValue);
          });
      auto vjpFnType = vjpValue->getType().castTo<SILFunctionType>();
      auto vjpFnUnsubstType = vjpFnType->getUnsubstitutedType(getModule());
      if (vjpFnType != vjpFnUnsubstType) {
        vjpValue = builder.createConvertFunction(
            loc, vjpValue, SILType::getPrimitiveObjectType(vjpFnUnsubstType),
            /*withoutActuallyEscaping*/ false);
      }
    }

    // Check and diagnose non-differentiable original function type.
    auto diagnoseNondifferentiableOriginalFunctionType =
        [&](CanSILFunctionType origFnTy) {
          // Check and diagnose non-differentiable arguments.
          for (auto paramIndex : config.parameterIndices->getIndices()) {
            if (!originalFnTy->getParameters()[paramIndex]
                     .getSILStorageInterfaceType()
                     .isDifferentiable(getModule())) {
              auto arg = ai->getArgumentsWithoutIndirectResults()[paramIndex];
              // FIXME: This shouldn't be necessary and might indicate a bug in
              // the transformation.
              RegularLocation nonAutoGenLoc(arg.getLoc());
              nonAutoGenLoc.markNonAutoGenerated();
              auto startLoc = nonAutoGenLoc.getStartSourceLoc();
              auto endLoc = nonAutoGenLoc.getEndSourceLoc();
              context
                  .emitNondifferentiabilityError(
                      arg, invoker, diag::autodiff_nondifferentiable_argument)
                  .fixItInsert(startLoc, "withoutDerivative(at: ")
                  .fixItInsertAfter(endLoc, ")");
              errorOccurred = true;
              return true;
            }
          }
          // Check and diagnose non-differentiable results.
          for (auto resultIndex : config.resultIndices->getIndices()) {
            SILType remappedResultType;
            if (resultIndex >= originalFnTy->getNumResults()) {
              auto semanticResultArgIdx = resultIndex - originalFnTy->getNumResults();
              auto semanticResultArg =
                  *std::next(ai->getAutoDiffSemanticResultArguments().begin(),
                             semanticResultArgIdx);
              remappedResultType = semanticResultArg->getType();
            } else {
              remappedResultType = originalFnTy->getResults()[resultIndex]
                                       .getSILStorageInterfaceType();
            }
            if (!remappedResultType.isDifferentiable(getModule())) {
              auto startLoc = ai->getLoc().getStartSourceLoc();
              auto endLoc = ai->getLoc().getEndSourceLoc();
              context
                  .emitNondifferentiabilityError(
                      origCallee, invoker,
                      diag::autodiff_nondifferentiable_result)
                  .fixItInsert(startLoc, "withoutDerivative(at: ")
                  .fixItInsertAfter(endLoc, ")");
              errorOccurred = true;
              return true;
            }
          }
          return false;
        };
    if (diagnoseNondifferentiableOriginalFunctionType(originalFnTy))
      return;

    // If VJP has not yet been found, emit an `differentiable_function`
    // instruction on the remapped original function operand and
    // an `differentiable_function_extract` instruction to get the VJP.
    // The `differentiable_function` instruction will be canonicalized during
    // the transform main loop.
    if (!vjpValue) {
      // FIXME: Handle indirect differentiation invokers. This may require some
      // redesign: currently, each original function + witness pair is mapped
      // only to one invoker.
      /*
      DifferentiationInvoker indirect(ai, attr);
      auto insertion =
          context.getInvokers().try_emplace({original, attr}, indirect);
      auto &invoker = insertion.first->getSecond();
      invoker = indirect;
      */

      // If the original `apply` instruction has a substitution map, then the
      // applied function is specialized.
      // In the VJP, specialization is also necessary for parity. The original
      // function operand is specialized with a remapped version of same
      // substitution map using an argument-less `partial_apply`.
      if (ai->getSubstitutionMap().empty()) {
        origCallee = builder.emitCopyValueOperation(loc, origCallee);
      } else {
        auto substMap = getOpSubstitutionMap(ai->getSubstitutionMap());
        auto vjpPartialApply = getBuilder().createPartialApply(
            ai->getLoc(), origCallee, substMap, {},
            ParameterConvention::Direct_Guaranteed);
        origCallee = vjpPartialApply;
        originalFnTy = origCallee->getType().castTo<SILFunctionType>();
        // Diagnose if new original function type is non-differentiable.
        if (diagnoseNondifferentiableOriginalFunctionType(originalFnTy))
          return;
      }

      auto *diffFuncInst = context.createDifferentiableFunction(
          getBuilder(), loc, config.parameterIndices, config.resultIndices,
          origCallee);

      // Record the `differentiable_function` instruction.
      context.getDifferentiableFunctionInstWorklist().push_back(diffFuncInst);

      builder.emitScopedBorrowOperation(
          loc, diffFuncInst, [&](SILValue borrowedADFunc) {
            auto extractedVJP =
                getBuilder().createDifferentiableFunctionExtract(
                    loc, NormalDifferentiableFunctionTypeComponent::VJP,
                    borrowedADFunc);
            vjpValue = builder.emitCopyValueOperation(loc, extractedVJP);
          });
      builder.emitDestroyValueOperation(loc, diffFuncInst);
    }

    // Record desired/actual VJP indices.
    // Temporarily set original pullback type to `None`.
    NestedApplyInfo info{config, /*originalPullbackType*/ llvm::None};
    auto insertion = context.getNestedApplyInfo().try_emplace(ai, info);
    auto &nestedApplyInfo = insertion.first->getSecond();
    nestedApplyInfo = info;

    // Call the VJP using the original parameters.
    SmallVector<SILValue, 8> vjpArgs;
    auto vjpFnTy = getOpType(vjpValue->getType()).castTo<SILFunctionType>();
    auto numVJPArgs =
        vjpFnTy->getNumParameters() + vjpFnTy->getNumIndirectFormalResults();
    vjpArgs.reserve(numVJPArgs);
    // Collect substituted arguments.
    for (auto origArg : ai->getArguments())
      vjpArgs.push_back(getOpValue(origArg));
    assert(vjpArgs.size() == numVJPArgs);
    // Apply the VJP.
    // The VJP should be specialized, so no substitution map is necessary.
    auto *vjpCall = getBuilder().createApply(loc, vjpValue, SubstitutionMap(),
                                             vjpArgs, ai->getApplyOptions());
    LLVM_DEBUG(getADDebugStream() << "Applied vjp function\n" << *vjpCall);
    builder.emitDestroyValueOperation(loc, vjpValue);

    // Get the VJP results (original results and pullback).
    SmallVector<SILValue, 8> vjpDirectResults;
    extractAllElements(vjpCall, getBuilder(), vjpDirectResults);
    ArrayRef<SILValue> originalDirectResults =
        ArrayRef<SILValue>(vjpDirectResults).drop_back(1);
    SILValue originalDirectResult =
        joinElements(originalDirectResults, getBuilder(), vjpCall->getLoc());
    SILValue pullback = vjpDirectResults.back();
    {
      auto pullbackFnType = pullback->getType().castTo<SILFunctionType>();
      auto pullbackUnsubstFnType =
          pullbackFnType->getUnsubstitutedType(getModule());
      if (pullbackFnType != pullbackUnsubstFnType) {
        pullback = builder.createConvertFunction(
            loc, pullback,
            SILType::getPrimitiveObjectType(pullbackUnsubstFnType),
            /*withoutActuallyEscaping*/ false);
      }
    }

    // Store the original result to the value map.
    mapValue(ai, originalDirectResult);

    // Checkpoint the pullback.
    auto pullbackType = pullbackInfo.lookUpLinearMapType(ai);

    // If actual pullback type does not match lowered pullback type, reabstract
    // the pullback using a thunk.
    auto actualPullbackType =
        getOpType(pullback->getType()).getAs<SILFunctionType>();
    auto loweredPullbackType =
        getOpType(getLoweredType(pullbackType)).castTo<SILFunctionType>();
    if (!loweredPullbackType->isEqual(actualPullbackType)) {
      // Set non-reabstracted original pullback type in nested apply info.
      nestedApplyInfo.originalPullbackType = actualPullbackType;
      SILOptFunctionBuilder fb(context.getTransform());
      pullback = reabstractFunction(
          getBuilder(), fb, ai->getLoc(), pullback, loweredPullbackType,
          [this](SubstitutionMap subs) -> SubstitutionMap {
            return this->getOpSubstitutionMap(subs);
          });
    }
    pullbackValues[ai->getParent()].push_back(pullback);

    // Some instructions that produce the callee may have been cloned.
    // If the original callee did not have any users beyond this `apply`,
    // recursively kill the cloned callee.
    if (auto *origCallee = cast_or_null<SingleValueInstruction>(
            ai->getCallee()->getDefiningInstruction()))
      if (origCallee->hasOneUse())
        recursivelyDeleteTriviallyDeadInstructions(
            getOpValue(origCallee)->getDefiningInstruction());
  }

  void visitTryApplyInst(TryApplyInst *tai) {
    Builder.setCurrentDebugScope(getOpScope(tai->getDebugScope()));
    // Build pullback struct value for original block.
    auto *pbTupleVal = buildPullbackValueTupleValue(tai);
    // Create a new `try_apply` instruction.
    auto args = getOpValueArray<8>(tai->getArguments());
    getBuilder().createTryApply(
        tai->getLoc(), getOpValue(tai->getCallee()),
        getOpSubstitutionMap(tai->getSubstitutionMap()), args,
        createTrampolineBasicBlock(tai, pbTupleVal, tai->getNormalBB()),
        createTrampolineBasicBlock(tai, pbTupleVal, tai->getErrorBB()),
        tai->getApplyOptions());
  }

  void visitDifferentiableFunctionInst(DifferentiableFunctionInst *dfi) {
    // Clone `differentiable_function` from original to VJP, then add the cloned
    // instruction to the `differentiable_function` worklist.
    TypeSubstCloner::visitDifferentiableFunctionInst(dfi);
    auto *newDFI = cast<DifferentiableFunctionInst>(getOpValue(dfi));
    context.getDifferentiableFunctionInstWorklist().push_back(newDFI);
  }

  void visitLinearFunctionInst(LinearFunctionInst *lfi) {
    // Clone `linear_function` from original to VJP, then add the cloned
    // instruction to the `linear_function` worklist.
    TypeSubstCloner::visitLinearFunctionInst(lfi);
    auto *newLFI = cast<LinearFunctionInst>(getOpValue(lfi));
    context.getLinearFunctionInstWorklist().push_back(newLFI);
  }
};

/// Initialization helper function.
///
/// Returns the substitution map used for type remapping.
static SubstitutionMap getSubstitutionMap(SILFunction *original,
                                          SILFunction *vjp) {
  auto substMap = original->getForwardingSubstitutionMap();
  if (auto *vjpGenEnv = vjp->getGenericEnvironment()) {
    auto vjpSubstMap = vjpGenEnv->getForwardingSubstitutionMap();
    substMap = SubstitutionMap::get(
        vjpGenEnv->getGenericSignature(), QuerySubstitutionMap{vjpSubstMap},
        LookUpConformanceInSubstitutionMap(vjpSubstMap));
  }
  return substMap;
}

/// Initialization helper function.
///
/// Returns the activity info for the given original function, autodiff indices,
/// and VJP generic signature.
static const DifferentiableActivityInfo &
getActivityInfoHelper(ADContext &context, SILFunction *original,
                      const AutoDiffConfig &config, SILFunction *vjp) {
  // Get activity info of the original function.
  auto &passManager = context.getPassManager();
  auto *activityAnalysis =
      passManager.getAnalysis<DifferentiableActivityAnalysis>();
  auto &activityCollection = *activityAnalysis->get(original);
  auto &activityInfo = activityCollection.getActivityInfo(
      vjp->getLoweredFunctionType()->getSubstGenericSignature(),
      AutoDiffDerivativeFunctionKind::VJP);
  LLVM_DEBUG(activityInfo.dump(config, getADDebugStream()));
  return activityInfo;
}

VJPCloner::Implementation::Implementation(VJPCloner &cloner, ADContext &context,
                                          SILDifferentiabilityWitness *witness,
                                          SILFunction *vjp,
                                          DifferentiationInvoker invoker)
    : TypeSubstCloner(*vjp, *witness->getOriginalFunction(),
                      getSubstitutionMap(witness->getOriginalFunction(), vjp)),
      cloner(cloner), context(context),
      original(witness->getOriginalFunction()), witness(witness),
      vjp(vjp), invoker(invoker),
      activityInfo(getActivityInfoHelper(
          context, original, witness->getConfig(), vjp)),
      loopInfo(context.getPassManager().getAnalysis<SILLoopAnalysis>()
                   ->get(original)),
      pullbackInfo(context, AutoDiffLinearMapKind::Pullback, original, vjp,
                   witness->getConfig(), activityInfo, loopInfo) {
  // Create empty pullback function.
  pullback = createEmptyPullback();
  context.recordGeneratedFunction(pullback);
}

VJPCloner::VJPCloner(ADContext &context,
                     SILDifferentiabilityWitness *witness, SILFunction *vjp,
                     DifferentiationInvoker invoker)
    : impl(*new Implementation(*this, context, witness, vjp, invoker)) {}

VJPCloner::~VJPCloner() { delete &impl; }

ADContext &VJPCloner::getContext() const { return impl.context; }
SILModule &VJPCloner::getModule() const { return impl.getModule(); }
SILFunction &VJPCloner::getOriginal() const { return *impl.original; }
SILFunction &VJPCloner::getVJP() const { return *impl.vjp; }
SILFunction &VJPCloner::getPullback() const { return *impl.pullback; }
SILDifferentiabilityWitness *VJPCloner::getWitness() const {
  return impl.witness;
}
const AutoDiffConfig &VJPCloner::getConfig() const {
  return impl.getConfig();
}
DifferentiationInvoker VJPCloner::getInvoker() const { return impl.invoker; }
LinearMapInfo &VJPCloner::getPullbackInfo() const { return impl.pullbackInfo; }
SILLoopInfo *VJPCloner::getLoopInfo() const { return impl.loopInfo; }
const DifferentiableActivityInfo &VJPCloner::getActivityInfo() const {
  return impl.activityInfo;
}

SILFunction *VJPCloner::Implementation::createEmptyPullback() {
  auto &module = context.getModule();
  auto origTy = original->getLoweredFunctionType();
  // Get witness generic signature for remapping types.
  // Witness generic signature may have more requirements than VJP generic
  // signature: when witness generic signature has same-type requirements
  // binding all generic parameters to concrete types, VJP function type uses
  // all the concrete types and VJP generic signature is null.
  auto witnessCanGenSig = witness->getDerivativeGenericSignature().getCanonicalSignature();
  auto lookupConformance = LookUpConformanceInModule(module.getSwiftModule());

  // Given a type, returns its formal SIL parameter info.
  auto getTangentParameterInfoForOriginalResult =
      [&](CanType tanType, ResultConvention origResConv) -> SILParameterInfo {
    tanType = tanType->getReducedType(witnessCanGenSig);
    Lowering::AbstractionPattern pattern(witnessCanGenSig, tanType);
    auto &tl = context.getTypeConverter().getTypeLowering(
        pattern, tanType, TypeExpansionContext::minimal());
    ParameterConvention conv;
    switch (origResConv) {
    case ResultConvention::Unowned:
    case ResultConvention::UnownedInnerPointer:
    case ResultConvention::Owned:
    case ResultConvention::Autoreleased:
      if (tl.isAddressOnly()) {
        conv = ParameterConvention::Indirect_In_Guaranteed;
      } else {
        conv = tl.isTrivial() ? ParameterConvention::Direct_Unowned
                              : ParameterConvention::Direct_Guaranteed;
      }
      break;
    case ResultConvention::Indirect:
      conv = ParameterConvention::Indirect_In_Guaranteed;
      break;
    case ResultConvention::Pack:
      conv = ParameterConvention::Pack_Guaranteed;
      break;
    }
    return {tanType, conv};
  };

  // Given a type, returns its formal SIL result info.
  auto getTangentResultInfoForOriginalParameter =
      [&](CanType tanType, ParameterConvention origParamConv) -> SILResultInfo {
    tanType = tanType->getReducedType(witnessCanGenSig);
    Lowering::AbstractionPattern pattern(witnessCanGenSig, tanType);
    auto &tl = context.getTypeConverter().getTypeLowering(
        pattern, tanType, TypeExpansionContext::minimal());
    ResultConvention conv;
    switch (origParamConv) {
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Unowned:
      if (tl.isAddressOnly()) {
        conv = ResultConvention::Indirect;
      } else {
        conv = tl.isTrivial() ? ResultConvention::Unowned
                              : ResultConvention::Owned;
      }
      break;
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_In_Guaranteed:
    case ParameterConvention::Indirect_InoutAliasable:
      conv = ResultConvention::Indirect;
      break;
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Inout:
      conv = ResultConvention::Pack;
      break;
    }
    return {tanType, conv};
  };

  // Parameters of the pullback are:
  // - the tangent vectors of the original results, and
  // - a pullback struct.
  // Results of the pullback are in the tangent space of the original
  // parameters.
  SmallVector<SILParameterInfo, 8> pbParams;
  SmallVector<SILResultInfo, 8> adjResults;
  auto origParams = origTy->getParameters();
  auto config = witness->getConfig();

  // Add pullback parameters based on original result indices.
  SmallVector<unsigned, 4> semanticResultParamIndices;
  for (auto i : range(origTy->getNumParameters())) {
    auto origParam = origParams[i];
    if (!origParam.isAutoDiffSemanticResult())
      continue;
    semanticResultParamIndices.push_back(i);
  }

  for (auto resultIndex : config.resultIndices->getIndices()) {
    // Handle formal result.
    if (resultIndex < origTy->getNumResults()) {
      auto origResult = origTy->getResults()[resultIndex];
      origResult = origResult.getWithInterfaceType(
          origResult.getInterfaceType()->getReducedType(witnessCanGenSig));
      auto paramInfo = getTangentParameterInfoForOriginalResult(
          origResult.getInterfaceType()
              ->getAutoDiffTangentSpace(lookupConformance)
              ->getType()
              ->getReducedType(witnessCanGenSig),
          origResult.getConvention());
      pbParams.push_back(paramInfo);
      continue;
    }

    // Handle semantic result parameter.
    unsigned paramIndex = 0;
    unsigned resultParamIndex = 0;
    for (auto i : range(origTy->getNumParameters())) {
      auto origParam = origTy->getParameters()[i];
      if (!origParam.isAutoDiffSemanticResult()) {
        ++paramIndex;
        continue;
      }
      if (resultParamIndex == resultIndex - origTy->getNumResults())
        break;
      ++paramIndex;
      ++resultParamIndex;
    }
    auto resultParam = origParams[paramIndex];
    auto origResult = resultParam.getWithInterfaceType(
      resultParam.getInterfaceType()->getReducedType(witnessCanGenSig));

    auto resultParamTanConvention = resultParam.getConvention();
    if (!config.isWrtParameter(paramIndex))
      resultParamTanConvention = ParameterConvention::Indirect_In_Guaranteed;

    pbParams.emplace_back(origResult.getInterfaceType()
                          ->getAutoDiffTangentSpace(lookupConformance)
                          ->getType()
                          ->getReducedType(witnessCanGenSig),
                          resultParamTanConvention);
  }

  if (pullbackInfo.hasHeapAllocatedContext()) {
    // Accept a `AutoDiffLinarMapContext` heap object if there are loops.
    pbParams.push_back({
      getASTContext().TheNativeObjectType,
      ParameterConvention::Direct_Guaranteed
    });
  } else {
    // Accept a pullback struct in the pullback parameter list. This is the
    // returned pullback's closure context.
    auto *origExit = &*original->findReturnBB();
    auto pbTupleType =
      pullbackInfo.getLinearMapTupleLoweredType(origExit).getAs<TupleType>();
    for (Type eltTy : pbTupleType->getElementTypes())
      pbParams.emplace_back(CanType(eltTy), ParameterConvention::Direct_Owned);
  }

  // Add pullback results for the requested wrt parameters.
  for (auto i : config.parameterIndices->getIndices()) {
    auto origParam = origParams[i];
    if (origParam.isAutoDiffSemanticResult())
      continue;
    origParam = origParam.getWithInterfaceType(
        origParam.getInterfaceType()->getReducedType(witnessCanGenSig));
    adjResults.push_back(getTangentResultInfoForOriginalParameter(
        origParam.getInterfaceType()
            ->getAutoDiffTangentSpace(lookupConformance)
            ->getType()
            ->getReducedType(witnessCanGenSig),
        origParam.getConvention()));
  }

  Mangle::DifferentiationMangler mangler;
  auto pbName = mangler.mangleLinearMap(
      original->getName(), AutoDiffLinearMapKind::Pullback, config);
  // Set pullback generic signature equal to VJP generic signature.
  // Do not use witness generic signature, which may have same-type requirements
  // binding all generic parameters to concrete types.
  auto pbGenericSig = vjp->getLoweredFunctionType()->getSubstGenericSignature();
  auto *pbGenericEnv = pbGenericSig.getGenericEnvironment();
  auto pbType = SILFunctionType::get(
      pbGenericSig, SILExtInfo::getThin(), origTy->getCoroutineKind(),
      origTy->getCalleeConvention(), pbParams, {}, adjResults, llvm::None,
      origTy->getPatternSubstitutions(), origTy->getInvocationSubstitutions(),
      original->getASTContext());

  SILOptFunctionBuilder fb(context.getTransform());
  auto linkage = vjp->isSerialized() ? SILLinkage::Public : SILLinkage::Private;
  auto *pullback = fb.createFunction(
      linkage, context.getASTContext().getIdentifier(pbName).str(), pbType,
      pbGenericEnv, original->getLocation(), original->isBare(),
      IsNotTransparent, vjp->isSerialized(),
      original->isDynamicallyReplaceable(), original->isDistributed(),
      original->isRuntimeAccessible());
  pullback->setDebugScope(new (module)
                              SILDebugScope(original->getLocation(), pullback));

  return pullback;
}

SILBasicBlock *VJPCloner::Implementation::createTrampolineBasicBlock(
    TermInst *termInst, TupleInst *pbTupleVal, SILBasicBlock *succBB) {
  assert(llvm::find(termInst->getSuccessorBlocks(), succBB) !=
             termInst->getSuccessorBlocks().end() &&
         "Basic block is not a successor of terminator instruction");
  // Create the trampoline block.
  auto *vjpSuccBB = getOpBasicBlock(succBB);
  auto *trampolineBB = vjp->createBasicBlockBefore(vjpSuccBB);
  for (auto *arg : vjpSuccBB->getArguments().drop_back())
    trampolineBB->createPhiArgument(arg->getType(), arg->getOwnershipKind());
  // In the trampoline block, build predecessor enum value for VJP successor
  // block and branch to it.
  SILBuilder trampolineBuilder(trampolineBB);
  trampolineBuilder.setCurrentDebugScope(getOpScope(termInst->getDebugScope()));
  auto *origBB = termInst->getParent();
  auto *succEnumVal =
      buildPredecessorEnumValue(trampolineBuilder, origBB, succBB, pbTupleVal);
  SmallVector<SILValue, 4> forwardedArguments(
      trampolineBB->getArguments().begin(), trampolineBB->getArguments().end());
  forwardedArguments.push_back(succEnumVal);
  trampolineBuilder.createBranch(termInst->getLoc(), vjpSuccBB,
                                 forwardedArguments);
  return trampolineBB;
}

llvm::SmallVector<SILValue, 8>
VJPCloner::Implementation::getPullbackValues(SILBasicBlock *origBB) {
  auto *vjpBB = BBMap[origBB];
  auto bbPullbackValues = pullbackValues[origBB];
  if (!origBB->isEntry()) {
    auto *predEnumArg = vjpBB->getArguments().back();
    bbPullbackValues.insert(bbPullbackValues.begin(), predEnumArg);
  }

  return bbPullbackValues;
}

TupleInst *
VJPCloner::Implementation::buildPullbackValueTupleValue(TermInst *termInst) {
  assert(termInst->getFunction() == original);
  auto loc = RegularLocation::getAutoGeneratedLocation();
  auto origBB = termInst->getParent();
  auto tupleLoweredTy =
      remapType(pullbackInfo.getLinearMapTupleLoweredType(origBB));
  auto bbPullbackValues = getPullbackValues(origBB);
  return getBuilder().createTuple(loc, tupleLoweredTy, bbPullbackValues);
}

EnumInst *VJPCloner::Implementation::buildPredecessorEnumValue(
    SILBuilder &builder, SILBasicBlock *predBB, SILBasicBlock *succBB,
    SILValue pbTupleVal) {
  auto loc = RegularLocation::getAutoGeneratedLocation();
  auto enumLoweredTy =
      remapType(pullbackInfo.getBranchingTraceEnumLoweredType(succBB));
  auto *enumEltDecl =
      pullbackInfo.lookUpBranchingTraceEnumElement(predBB, succBB);
  auto enumEltType = getOpType(enumLoweredTy.getEnumElementType(
      enumEltDecl, getModule(), TypeExpansionContext::minimal()));
  // If the predecessor block is in a loop, its predecessor enum payload is a
  // `Builtin.RawPointer`.
  if (loopInfo->getLoopFor(predBB)) {
    auto rawPtrType = SILType::getRawPointerType(getASTContext());
    assert(enumEltType == rawPtrType);
    auto pbTupleType =
      remapASTType(pullbackInfo.getLinearMapTupleType(predBB)->getCanonicalType());
    SILValue pbTupleSize =
        emitMemoryLayoutSize(Builder, loc, pbTupleType);
    auto rawBufferValue = builder.createBuiltin(
        loc,
        getASTContext().getIdentifier(
            getBuiltinName(BuiltinValueKind::AutoDiffAllocateSubcontext)),
        rawPtrType, SubstitutionMap(),
        {borrowedPullbackContextValue, pbTupleSize});
    auto typedBufferValue =
      builder.createPointerToAddress(
        loc, rawBufferValue, pbTupleVal->getType().getAddressType(),
        /*isStrict*/ true);
    builder.createStore(
        loc, pbTupleVal, typedBufferValue,
        pbTupleVal->getType().isTrivial(*pullback) ?
            StoreOwnershipQualifier::Trivial : StoreOwnershipQualifier::Init);
    return builder.createEnum(loc, rawBufferValue, enumEltDecl, enumLoweredTy);
  }
  return builder.createEnum(loc, pbTupleVal, enumEltDecl, enumLoweredTy);
}

bool VJPCloner::Implementation::run() {
  PrettyStackTraceSILFunction trace("generating VJP for", original);
  LLVM_DEBUG(getADDebugStream() << "Cloning original @" << original->getName()
                                << " to vjp @" << vjp->getName() << '\n');

  // Create entry BB and arguments.
  auto *entry = vjp->createBasicBlock();
  createEntryArguments(vjp);

  emitLinearMapContextInitializationIfNeeded();

  // Clone.
  SmallVector<SILValue, 4> entryArgs(entry->getArguments().begin(),
                                     entry->getArguments().end());
  cloneFunctionBody(original, entry, entryArgs);
  // If errors occurred, back out.
  if (errorOccurred)
    return true;

  // Merge VJP basic blocks. This is significant for control flow
  // differentiation: trampoline destination bbs are merged into trampoline bbs.
  // NOTE(TF-990): Merging basic blocks ensures that `@guaranteed` trampoline
  // bb arguments have a lifetime-ending `end_borrow` use, and is robust when
  // `-enable-strip-ownership-after-serialization` is true.
  mergeBasicBlocks(vjp);

  LLVM_DEBUG(getADDebugStream()
             << "Generated VJP for " << original->getName() << ":\n"
             << *vjp);

  // Generate pullback code.
  PullbackCloner PullbackCloner(cloner);
  if (PullbackCloner.run()) {
    errorOccurred = true;
    return true;
  }
  return errorOccurred;
}

bool VJPCloner::run() {
  bool foundError = impl.run();
#ifndef NDEBUG
  if (!foundError)
    getVJP().verify();
#endif
  return foundError;
}

} // end namespace autodiff
} // end namespace swift
