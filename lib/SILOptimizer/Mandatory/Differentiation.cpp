//===--- Differentiation.cpp - SIL Automatic Differentiation --*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements automatic differentiation.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Differentiation/ADContext.h"
#include "swift/SILOptimizer/Differentiation/JVPCloner.h"
#include "swift/SILOptimizer/Differentiation/Thunk.h"
#include "swift/SILOptimizer/Differentiation/VJPCloner.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/DifferentiationMangler.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace swift::autodiff;
using llvm::DenseMap;
using llvm::SmallDenseMap;
using llvm::SmallDenseSet;
using llvm::SmallMapVector;
using llvm::SmallSet;

/// This flag enables experimental `@differentiable(_linear)` function
/// transposition.
static llvm::cl::opt<bool> EnableExperimentalLinearMapTransposition(
    "enable-experimental-linear-map-transposition", llvm::cl::init(false));

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// Given a dumpable value, dumps it to `llvm::dbgs()`.
template <typename T> static inline void debugDump(T &v) {
  LLVM_DEBUG(llvm::dbgs() << "\n==== BEGIN DEBUG DUMP ====\n"
                          << v << "\n==== END DEBUG DUMP ====\n");
}

namespace {

class DifferentiationTransformer {
private:
  /// Reference to the main transform.
  SILModuleTransform &transform;

  /// Context necessary for performing the transformations.
  ADContext context;

  /// Promotes the given `differentiable_function` instruction to a valid
  /// `@differentiable` function-typed value.
  SILValue promoteToDifferentiableFunction(DifferentiableFunctionInst *inst,
                                           SILBuilder &builder, SILLocation loc,
                                           DifferentiationInvoker invoker);

  /// Given a `linear_function` instruction that is missing a transpose operand,
  /// return a new `linear_function` instruction with the transpose filled in.
  SILValue promoteToLinearFunction(LinearFunctionInst *inst,
                                   SILBuilder &builder, SILLocation loc,
                                   DifferentiationInvoker invoker);

public:
  /// Construct an `DifferentiationTransformer` for the given module.
  explicit DifferentiationTransformer(SILModuleTransform &transform)
      : transform(transform), context(transform) {}

  SILModuleTransform &getTransform() { return transform; }

  ADContext &getContext() { return context; }

  /// Canonicalize the given witness, filling in derivative functions if
  /// missing.
  ///
  /// Generated derivative functions have the same linkage as the witness.
  ///
  /// \param serializeFunctions specifies whether generated functions should be
  ///        serialized.
  bool canonicalizeDifferentiabilityWitness(
      SILDifferentiabilityWitness *witness, DifferentiationInvoker invoker,
      IsSerialized_t serializeFunctions);

  /// Process the given `differentiable_function` instruction, filling in
  /// missing derivative functions if necessary.
  bool processDifferentiableFunctionInst(DifferentiableFunctionInst *dfi);

  /// Process the given `linear_function` instruction, filling in the missing
  /// transpose function if necessary.
  bool processLinearFunctionInst(LinearFunctionInst *lfi);
};

} // end anonymous namespace

/// If the original function doesn't have a return, it cannot be differentiated.
/// Returns true if error is emitted.
static bool diagnoseNoReturn(ADContext &context, SILFunction *original,
                             DifferentiationInvoker invoker) {
  if (original->findReturnBB() != original->end())
    return false;
  context.emitNondifferentiabilityError(
      original->getLocation().getEndSourceLoc(), invoker,
      diag::autodiff_missing_return);
  return true;
}

/// If the original function contains unsupported control flow, emit a "control
/// flow unsupported" error at appropriate source locations. Returns true if
/// error is emitted.
///
/// Update as control flow support is added.
static bool diagnoseUnsupportedControlFlow(ADContext &context,
                                           SILFunction *original,
                                           DifferentiationInvoker invoker) {
  if (original->size() <= 1)
    return false;
  // Diagnose unsupported branching terminators.
  for (auto &bb : *original) {
    auto *term = bb.getTerminator();
    // Check supported branching terminators.
    if (isa<BranchInst>(term) || isa<CondBranchInst>(term) ||
        isa<SwitchEnumInst>(term) || isa<SwitchEnumAddrInst>(term) ||
        isa<CheckedCastBranchInst>(term) ||
        isa<CheckedCastAddrBranchInst>(term) || isa<TryApplyInst>(term))
      continue;
    // If terminator is an unsupported branching terminator, emit an error.
    if (term->isBranch()) {
      context.emitNondifferentiabilityError(
          term, invoker, diag::autodiff_control_flow_not_supported);
      return true;
    }
  }
  return false;
}

/// Check whether the given requirements are satisfied, with the given
/// derivative generic signature (containing requirements), and substitution
/// map. Returns true if error is emitted.
static bool diagnoseUnsatisfiedRequirements(ADContext &context,
                                            CanSILFunctionType origFnTy,
                                            GenericSignature derivativeGenSig,
                                            SubstitutionMap substMap,
                                            DifferentiationInvoker invoker,
                                            SourceLoc loc) {
  // If the original function is polymorphic and its generic signature is the
  // same as the derivative generic signature, then the requirements are
  // satisfied. This check is necessary because the subsequent logic does not
  // correctly handle polymorphic original functions.
  // TODO(TF-1055): Can be removed after we have a robust solution for TF-1055.
  if (origFnTy->getInvocationGenericSignature() && derivativeGenSig &&
      origFnTy->getInvocationGenericSignature()->isEqual(derivativeGenSig))
    return false;

  // If there are no derivative requirements, return false.
  auto requirements = derivativeGenSig.getRequirements();
  if (requirements.empty())
    return false;
  // Iterate through all requirements and check whether they are satisfied.
  auto *swiftModule = context.getModule().getSwiftModule();
  SmallVector<Requirement, 2> unsatisfiedRequirements;
  for (auto req : requirements) {
    auto firstType = req.getFirstType();
    Type secondType;
    // Substitute first and second types using the given substitution map,
    // looking up conformances in the current module, if possible.
    if (auto substFirstType =
            firstType.subst(QuerySubstitutionMap{substMap},
                            LookUpConformanceInModule(swiftModule))) {
      firstType = substFirstType;
    }
    if (req.getKind() != RequirementKind::Layout) {
      secondType = req.getSecondType();
      if (auto substSecondType =
              secondType.subst(QuerySubstitutionMap{substMap},
                               LookUpConformanceInModule(swiftModule))) {
        secondType = substSecondType;
      }
    }
    switch (req.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement not supported here");

    // Check layout requirements.
    case RequirementKind::Layout: {
      auto layout = req.getLayoutConstraint();
      switch (layout->getKind()) {
      case LayoutConstraintKind::Class:
        if (!firstType->satisfiesClassConstraint())
          unsatisfiedRequirements.push_back(req);
        continue;
      default:
        // TODO: Check other layout requirements. Note that `@differentiable`
        // attribute type-checking does not yet support layout requirements in
        // where clauses; layout requirements in derivative generic signatures
        // can be formed only from `differentiable_function` instructions whose
        // original function operand is generic with layout requirements.
        break;
      }
      continue;
    }
    // Check same type requirements.
    case RequirementKind::SameType:
      // If the first type does not equal the second type, then record the
      // unsatisfied requirement.
      if (!firstType->isEqual(secondType))
        unsatisfiedRequirements.push_back(req);
      continue;
    // Check superclass requirements.
    case RequirementKind::Superclass: {
      // If the second type is not an exact superclass of second type, then
      // record the unsatisfied requirement.
      if (!secondType->isExactSuperclassOf(firstType))
        unsatisfiedRequirements.push_back(req);
      continue;
    }
    // Check conformance requirements.
    case RequirementKind::Conformance: {
      auto *protocol = req.getProtocolDecl();
      assert(protocol && "Expected protocol in generic signature requirement");
      // If the first type does not conform to the second type in the current
      // module, then record the unsatisfied requirement.
      if (!swiftModule->lookupConformance(firstType, protocol))
        unsatisfiedRequirements.push_back(req);
      continue;
    }
    }
  }
  if (unsatisfiedRequirements.empty())
    return false;
  // Diagnose unsatisfied requirements.
  std::string reqText;
  llvm::raw_string_ostream stream(reqText);
  interleave(
      unsatisfiedRequirements,
      [&](Requirement req) { req.print(stream, PrintOptions()); },
      [&] { stream << ", "; });
  context.emitNondifferentiabilityError(
      loc, invoker, diag::autodiff_function_assoc_func_unmet_requirements,
      stream.str());
  return true;
}

//===----------------------------------------------------------------------===//
// Code emission utilities
//===----------------------------------------------------------------------===//

/// Given an apply site, emit copies of all parameters and place them in
/// `copiedArgs`. Any buffers that need to be destroyed will be added to
/// `newArgsToDestroy`. Any new buffers that need to be deallocated will be
/// added to `newBuffersToDealloc`. This helper is used for duplicating an
/// apply site.
static void copyParameterArgumentsForApply(
    ApplySite applySite, SmallVectorImpl<SILValue> &copiedArgs,
    SmallVectorImpl<SILValue> &newArgsToDestroy,
    SmallVectorImpl<AllocStackInst *> &newBuffersToDealloc) {
  LLVM_DEBUG({
    auto &s = getADDebugStream() << "Copying arguments from apply site: ";
    applySite.getInstruction()->print(s);
  });
  auto loc = applySite.getLoc();
  copiedArgs.reserve(applySite.getNumArguments());
  SILBuilderWithScope copyBuilder(applySite.getInstruction());
  for (auto &argOperand : applySite.getArgumentOperands()) {
    auto arg = argOperand.get();
    auto argConv = applySite.getArgumentConvention(argOperand);
    auto collectNewArg = [&](SILValue newArg) {
      copiedArgs.push_back(newArg);
      if (argConv.isGuaranteedConvention() &&
          argConv != SILArgumentConvention::Indirect_InoutAliasable)
        newArgsToDestroy.push_back(newArg);
    };
    // Copy the argument if it's to be owned by the newly created closure.
    // Objects are to be retained.
    if (arg->getType().isObject()) {
      auto newArg = arg;
      if (newArg->getOwnershipKind() != OwnershipKind::None)
        newArg = copyBuilder.emitCopyValueOperation(loc, arg);
      collectNewArg(newArg);
      continue;
    }
    // Addresses depend on argument conventions.
    // If the argument is an aliasable inout reference, do not copy the
    // argument since it's a `@noescape` capture.
    if (argConv == SILArgumentConvention::Indirect_InoutAliasable) {
      collectNewArg(arg);
      continue;
    }
    // Otherwise, it must be address-only. Create a new buffer and perform
    // `copy_addr`.
    auto *argCopy = copyBuilder.createAllocStack(loc, arg->getType());
    newBuffersToDealloc.push_back(argCopy);
    copyBuilder.createCopyAddr(loc, arg, argCopy, IsNotTake, IsInitialization);
    collectNewArg(argCopy);
  }
}

/// When a function value is used in an instruction (usually `apply`), there may
/// be conversion instructions in between, e.g. `thin_to_thick_function`. Given
/// a new function value and an old function value, this helper function
/// recursively converts the new function just like how the old function is
/// converted.
///
/// If the new function's generic signature is specified, it is used
/// to create substitution maps for reapplied `partial_apply` instructions.
static SILValue reapplyFunctionConversion(
    ADContext &context, SILValue newFunc, SILValue oldFunc,
    SILValue oldConvertedFunc, SILBuilder &builder, SILLocation loc,
    SmallVectorImpl<AllocStackInst *> &newBuffersToDealloc,
    IndexSubset *parameterIndices, IndexSubset *resultIndices,
    GenericSignature newFuncGenSig = GenericSignature()) {
  // If the old func is the new func, then there's no conversion.
  if (oldFunc == oldConvertedFunc)
    return newFunc;
  // Handle a few instruction cases.
  // copy_value
  if (auto *cvi = dyn_cast<CopyValueInst>(oldConvertedFunc)) {
    // Note: no `copy_value` is needed for the re-converted function because the
    // caller of `reapplyFunctionConversion` should consume the re-converted
    // function.
    return reapplyFunctionConversion(
        context, newFunc, oldFunc, cvi->getOperand(), builder, loc,
        newBuffersToDealloc, parameterIndices, resultIndices, newFuncGenSig);
  }
  // begin_borrow
  if (auto *bbi = dyn_cast<BeginBorrowInst>(oldConvertedFunc)) {
    // Note: no `begin_borrow` is needed for the re-converted function because
    // the caller of `reapplyFunctionConversion` should consume the re-converted
    // function.
    return reapplyFunctionConversion(
        context, newFunc, oldFunc, bbi->getOperand(), builder, loc,
        newBuffersToDealloc, parameterIndices, resultIndices, newFuncGenSig);
  }
  // convert_function
  if (auto *cfi = dyn_cast<ConvertFunctionInst>(oldConvertedFunc)) {
    return reapplyFunctionConversion(
        context, newFunc, oldFunc, cfi->getOperand(), builder, loc,
        newBuffersToDealloc, parameterIndices, resultIndices, newFuncGenSig);
  }
  // thin_to_thick_function
  if (auto *tttfi = dyn_cast<ThinToThickFunctionInst>(oldConvertedFunc)) {
    auto innerNewFunc = reapplyFunctionConversion(
        context, newFunc, oldFunc, tttfi->getOperand(), builder, loc,
        newBuffersToDealloc, parameterIndices, resultIndices, newFuncGenSig);
    auto operandFnTy = innerNewFunc->getType().castTo<SILFunctionType>();
    auto thickTy = operandFnTy->getWithRepresentation(
        SILFunctionTypeRepresentation::Thick);
    auto silTy = SILType::getPrimitiveObjectType(thickTy);
    return builder.createThinToThickFunction(loc, innerNewFunc, silTy);
  }
  // partial_apply
  if (auto *pai = dyn_cast<PartialApplyInst>(oldConvertedFunc)) {
    SmallVector<SILValue, 8> newArgs;
    newArgs.reserve(pai->getNumArguments());
    SmallVector<SILValue, 1> newArgsToDestroy;
    copyParameterArgumentsForApply(pai, newArgs, newArgsToDestroy,
                                   newBuffersToDealloc);
    auto innerNewFunc = reapplyFunctionConversion(
        context, newFunc, oldFunc, pai->getCallee(), builder, loc,
        newBuffersToDealloc, parameterIndices, resultIndices, newFuncGenSig);
    // Reabstraction thunk `partial_apply` reapplications require special
    // support. Reabstraction thunk JVP/VJP expects a `@differentiable`
    // function-typed argument to avoid opaque function non-differentiability
    // errors. Thus, `partial_apply` reapplications must first form a
    // `differentiable_function` of the function-typed thunk argument.
    auto isReabstractionThunkCallee = [&]() -> bool {
      auto *fri = dyn_cast<FunctionRefInst>(oldFunc);
      return fri && fri->getReferencedFunction()->isThunk() ==
                        IsReabstractionThunk;
    };
    if (isReabstractionThunkCallee()) {
      assert(newArgs.size() == 1 &&
             "Expected reabstraction thunk to be partially applied with only "
             "one argument");
      auto *dfi = context.createDifferentiableFunction(
          builder, loc, parameterIndices, resultIndices, newArgs.back());
      context.getDifferentiableFunctionInstWorklist().push_back(dfi);
      newArgs.back() = dfi;
    }
    // Compute substitution map for reapplying `partial_apply`.
    // - If reapplied function is not polymorphic, use empty substitution map
    //   regardless of the original `partial_apply`'s substitution map.
    //   - This case is triggered for reapplying `partial_apply` where `newFunc`
    //     is a `differentiability_witness_function` where the witness generic
    //     signature has all concrete parameters while the original function's
    //     generic signature does not. In this case, the original function type
    //     is polymorphic while derivative function types are not (specialized
    //     with concrete types from same-type requirements).
    // - Otherwise, if `newFuncGenSig` is not specified, use the original
    //   `partial_apply`'s substitution map.
    // - Otherwise, if `newFuncGenSig` is specified, combine it with the
    //   original `partial_apply`'s substitution map.
    SubstitutionMap substMap;
    if (innerNewFunc->getType().castTo<SILFunctionType>()->isPolymorphic()) {
      if (!newFuncGenSig) {
        substMap = pai->getSubstitutionMap();
      } else {
        substMap = SubstitutionMap::get(
            newFuncGenSig, QuerySubstitutionMap{pai->getSubstitutionMap()},
            LookUpConformanceInModule(builder.getModule().getSwiftModule()));
      }
    }
    return builder.createPartialApply(loc, innerNewFunc, substMap, newArgs,
                                      ParameterConvention::Direct_Guaranteed);
  }
  llvm_unreachable("Unhandled function conversion instruction");
}

/// Emits a reference to a derivative function of `original`, differentiated
/// with respect to a superset of `desiredIndices`. Returns the `SILValue` for
/// the derivative function and the actual indices that the derivative function
/// is with respect to.
///
/// Returns `None` on failure, signifying that a diagnostic has been emitted
/// using `invoker`.
static llvm::Optional<std::pair<SILValue, AutoDiffConfig>>
emitDerivativeFunctionReference(
    DifferentiationTransformer &transformer, SILBuilder &builder,
    const AutoDiffConfig &desiredConfig, AutoDiffDerivativeFunctionKind kind,
    SILValue original, DifferentiationInvoker invoker,
    SmallVectorImpl<AllocStackInst *> &newBuffersToDealloc) {
  ADContext &context = transformer.getContext();

  // If `original` is itself an `DifferentiableFunctionExtractInst` whose kind
  // matches the given kind and desired differentiation parameter indices,
  // simply extract the derivative function of its function operand, retain the
  // derivative function, and return it.
  if (auto *inst = original->getDefiningInstruction())
    if (auto *dfei = dyn_cast<DifferentiableFunctionExtractInst>(inst))
      if (dfei->getExtractee() ==
          NormalDifferentiableFunctionTypeComponent::Original)
        original = dfei->getOperand();

  // If `original` is a `@differentiable` function, just extract the
  // derivative function.
  if (auto diffableFnType = original->getType().castTo<SILFunctionType>()) {
    if (diffableFnType->isDifferentiable()) {
      auto paramIndices =
          diffableFnType->getDifferentiabilityParameterIndices();
      for (auto i : desiredConfig.parameterIndices->getIndices()) {
        if (!paramIndices->contains(i)) {
          context.emitNondifferentiabilityError(
              original, invoker,
              diag::
                  autodiff_function_noderivative_parameter_not_differentiable);
          return llvm::None;
        }
      }
      auto borrowedDiffFunc =
          builder.emitBeginBorrowOperation(original.getLoc(), original);
      SILValue derivativeFn = builder.createDifferentiableFunctionExtract(
          borrowedDiffFunc.getLoc(), kind, borrowedDiffFunc);
      if (derivativeFn->getOwnershipKind() != OwnershipKind::None)
        derivativeFn =
            builder.emitCopyValueOperation(original.getLoc(), derivativeFn);
      builder.emitEndBorrowOperation(original.getLoc(), borrowedDiffFunc);
      return std::make_pair(derivativeFn, desiredConfig);
    }
  }

  // Handle `function_ref` original function.
  if (auto *originalFRI =
          peerThroughFunctionConversions<FunctionRefInst>(original)) {
    auto loc = originalFRI->getLoc();
    auto *originalFn = originalFRI->getReferencedFunction();
    auto originalFnTy = originalFn->getLoweredFunctionType();
    auto *desiredParameterIndices = desiredConfig.parameterIndices;
    auto *desiredResultIndices = desiredConfig.resultIndices;
    // NOTE(TF-893): Extending capacity is necessary when `originalFnTy` has
    // parameters corresponding to captured variables.
    // TODO: If possible, change `autodiff::getLoweredParameterIndices` to
    // take `CaptureInfo` into account.
    if (originalFnTy->getNumParameters() >
        desiredParameterIndices->getCapacity()) {
      desiredParameterIndices = desiredParameterIndices->extendingCapacity(
          context.getASTContext(), originalFnTy->getNumParameters());
    }
    // Look up a differentiability witness with the exact configuration.
    auto *minimalWitness = getExactDifferentiabilityWitness(
        context.getModule(), originalFn, desiredParameterIndices,
        desiredResultIndices);
    // Otherwise, look up a differentiability witness with a minimal superset
    // configuration.
    if (!minimalWitness)
      minimalWitness = getOrCreateMinimalASTDifferentiabilityWitness(
          context.getModule(), originalFn, DifferentiabilityKind::Reverse,
          desiredParameterIndices, desiredResultIndices);
    // If no minimal witness exists, check non-differentiable cases before
    // creating a new private differentiability witness.
    if (!minimalWitness) {
      // If the function is intentionally marked as being opaque to
      // differentiation, then we should not create a task for it.
      if (originalFn->hasSemanticsAttr("autodiff.opaque")) {
        context.emitNondifferentiabilityError(
            original, invoker,
            diag::autodiff_opaque_function_not_differentiable);
        return llvm::None;
      }
      // Check and diagnose non-differentiable arguments.
      auto originalFnTy = originalFn->getLoweredFunctionType();
      for (unsigned paramIndex : range(originalFnTy->getNumParameters())) {
        if (desiredConfig.isWrtParameter(paramIndex) &&
            !originalFnTy->getParameters()[paramIndex]
                 .getSILStorageInterfaceType()
                 .isDifferentiable(context.getModule())) {
          auto diag = context.emitNondifferentiabilityError(
              original, invoker, diag::autodiff_nondifferentiable_argument);
          return llvm::None;
        }
      }
      // Check and diagnose non-differentiable results.
      for (auto resultIndex : desiredResultIndices->getIndices()) {
        SILType resultType;
        if (resultIndex >= originalFnTy->getNumResults()) {
          auto semanticResultParamIdx = resultIndex - originalFnTy->getNumResults();
          auto semanticResultParam =
              *std::next(originalFnTy->getAutoDiffSemanticResultsParameters().begin(),
                         semanticResultParamIdx);
          resultType = semanticResultParam.getSILStorageInterfaceType();
        } else {
          resultType = originalFnTy->getResults()[resultIndex]
                           .getSILStorageInterfaceType();
        }
        if (!resultType.isDifferentiable(context.getModule())) {
          context.emitNondifferentiabilityError(
              original, invoker, diag::autodiff_nondifferentiable_result);
          return llvm::None;
        }
      }
      // Check and diagnose external declarations.
      if (originalFn->isExternalDeclaration()) {
        context.emitNondifferentiabilityError(
            original, invoker,
            diag::autodiff_external_nondifferentiable_function);
        return llvm::None;
      }
      // Sanity check passed. Create a new differentiability witness and
      // canonicalize it.
      GenericSignature contextualDerivativeGenSig = GenericSignature();
      if (invoker.getKind() ==
          DifferentiationInvoker::Kind::IndirectDifferentiation)
        contextualDerivativeGenSig =
            invoker.getIndirectDifferentiation()
                .second->getDerivativeGenericSignature();
      auto derivativeConstrainedGenSig =
          autodiff::getConstrainedDerivativeGenericSignature(
              originalFn->getLoweredFunctionType(), desiredParameterIndices,
              contextualDerivativeGenSig,
              LookUpConformanceInModule(context.getModule().getSwiftModule()));
      minimalWitness = SILDifferentiabilityWitness::createDefinition(
          context.getModule(), SILLinkage::Private, originalFn,
          DifferentiabilityKind::Reverse, desiredParameterIndices,
          desiredResultIndices, derivativeConstrainedGenSig, /*jvp*/ nullptr,
          /*vjp*/ nullptr, /*isSerialized*/ false);
      if (transformer.canonicalizeDifferentiabilityWitness(
              minimalWitness, invoker, IsNotSerialized))
        return llvm::None;
    }
    assert(minimalWitness);
    if (original->getFunction()->isSerialized() &&
        !hasPublicVisibility(minimalWitness->getLinkage())) {
      enum { Inlinable = 0, DefaultArgument = 1 };
      unsigned fragileKind = Inlinable;
      // FIXME: This is not a very robust way of determining if the function is
      // a default argument. Also, we have not exhaustively listed all the kinds
      // of fragility.
      if (original->getFunction()->getLinkage() == SILLinkage::PublicNonABI)
        fragileKind = DefaultArgument;
      context.emitNondifferentiabilityError(
          original, invoker, diag::autodiff_private_derivative_from_fragile,
          fragileKind,
          isa_and_nonnull<AbstractClosureExpr>(
              originalFRI->getLoc().getAsASTNode<Expr>()));
      return llvm::None;
    }
    // TODO(TF-482): Move generic requirement checking logic to
    // `getExactDifferentiabilityWitness` and
    // `getOrCreateMinimalASTDifferentiabilityWitness`.
    // Get the substitution map for checking unmet generic requirements.
    // By default, use the forwarding substitution map of the original function.
    // If the original callee is a `partial_apply` or `apply` instruction, use
    // its substitution map instead.
    auto substMap = original->getFunction()->getForwardingSubstitutionMap();
    if (auto *pai =
            peerThroughFunctionConversions<PartialApplyInst>(original)) {
      substMap = pai->getSubstitutionMap();
    } else if (auto *ai = peerThroughFunctionConversions<ApplyInst>(original)) {
      substMap = ai->getSubstitutionMap();
    }
    if (diagnoseUnsatisfiedRequirements(
            context, original->getType().castTo<SILFunctionType>(),
            minimalWitness->getDerivativeGenericSignature(), substMap, invoker,
            original.getLoc().getSourceLoc()))
      return llvm::None;
    DifferentiabilityWitnessFunctionKind witnessKind;
    switch (kind) {
    case AutoDiffDerivativeFunctionKind::JVP:
      witnessKind = DifferentiabilityWitnessFunctionKind::JVP;
      break;
    case AutoDiffDerivativeFunctionKind::VJP:
      witnessKind = DifferentiabilityWitnessFunctionKind::VJP;
      break;
    }
    auto *derivativeFnRef = builder.createDifferentiabilityWitnessFunction(
        loc, witnessKind, minimalWitness);
    auto convertedRef = reapplyFunctionConversion(
        context, derivativeFnRef, originalFRI, original, builder, loc,
        newBuffersToDealloc, desiredConfig.parameterIndices,
        desiredConfig.resultIndices,
        derivativeFnRef->getType()
            .getASTType()
            ->castTo<SILFunctionType>()
            ->getSubstGenericSignature());
    return std::make_pair(convertedRef, minimalWitness->getConfig());
  }

  // Handle `witness_method`.
  if (auto *witnessMethod =
          peerThroughFunctionConversions<WitnessMethodInst>(original)) {
    auto loc = witnessMethod->getLoc();
    auto requirementDeclRef = witnessMethod->getMember();
    auto *requirementDecl = requirementDeclRef.getAbstractFunctionDecl();
    // If requirement declaration does not have any derivative function
    // configurations, produce an error.
    if (requirementDecl->getDerivativeFunctionConfigurations().empty()) {
      context.emitNondifferentiabilityError(
          original, invoker, diag::autodiff_protocol_member_not_differentiable);
      return llvm::None;
    }
    // Find the minimal derivative configuration: minimal parameter indices and
    // corresponding derivative generic signature. If it does not exist, produce
    // an error.
    IndexSubset *minimalASTParamIndices = nullptr;
    auto minimalConfig = findMinimalDerivativeConfiguration(
        requirementDecl, desiredConfig.parameterIndices,
        minimalASTParamIndices);
    if (!minimalConfig) {
      context.emitNondifferentiabilityError(
          original, invoker,
          diag::autodiff_member_subset_indices_not_differentiable);
      return llvm::None;
    }
    // Emit a `witness_method` instruction for the derivative function.
    auto originalType = witnessMethod->getType().castTo<SILFunctionType>();
    auto assocType = originalType->getAutoDiffDerivativeFunctionType(
        minimalConfig->parameterIndices, minimalConfig->resultIndices, kind,
        context.getTypeConverter(),
        LookUpConformanceInModule(builder.getModule().getSwiftModule()));
    auto *autoDiffFuncId = AutoDiffDerivativeFunctionIdentifier::get(
        kind, minimalASTParamIndices, minimalConfig->derivativeGenericSignature,
        context.getASTContext());
    auto *ref = builder.createWitnessMethod(
        loc, witnessMethod->getLookupType(), witnessMethod->getConformance(),
        requirementDeclRef.asAutoDiffDerivativeFunction(autoDiffFuncId),
        SILType::getPrimitiveObjectType(assocType));
    auto convertedRef = reapplyFunctionConversion(
        context, ref, witnessMethod, original, builder, loc,
        newBuffersToDealloc, desiredConfig.parameterIndices,
        desiredConfig.resultIndices);
    return std::make_pair(convertedRef, *minimalConfig);
  }

  // Handle `class_method`.
  if (auto *classMethod =
          peerThroughFunctionConversions<ClassMethodInst>(original)) {
    auto loc = classMethod->getLoc();
    auto methodDeclRef = classMethod->getMember();
    auto *methodDecl = methodDeclRef.getAbstractFunctionDecl();
    // If method declaration does not have any derivative function
    // configurations, produce an error.
    if (methodDecl->getDerivativeFunctionConfigurations().empty()) {
      context.emitNondifferentiabilityError(
          original, invoker, diag::autodiff_class_member_not_differentiable);
      return llvm::None;
    }
    // Find the minimal derivative configuration: minimal parameter indices and
    // corresponding derivative generic signature. If it does not exist, produce
    // an error.
    IndexSubset *minimalASTParamIndices = nullptr;
    auto minimalConfig = findMinimalDerivativeConfiguration(
        methodDecl, desiredConfig.parameterIndices, minimalASTParamIndices);
    if (!minimalConfig) {
      context.emitNondifferentiabilityError(
          original, invoker,
          diag::autodiff_member_subset_indices_not_differentiable);
      return llvm::None;
    }
    // Emit a `class_method` instruction for the derivative function.
    auto originalType = classMethod->getType().castTo<SILFunctionType>();
    auto assocType = originalType->getAutoDiffDerivativeFunctionType(
        minimalConfig->parameterIndices, minimalConfig->resultIndices, kind,
        context.getTypeConverter(),
        LookUpConformanceInModule(builder.getModule().getSwiftModule()));
    auto *autoDiffFuncId = AutoDiffDerivativeFunctionIdentifier::get(
        kind, minimalASTParamIndices, minimalConfig->derivativeGenericSignature,
        context.getASTContext());
    auto *ref = builder.createClassMethod(
        loc, classMethod->getOperand(),
        methodDeclRef.asAutoDiffDerivativeFunction(autoDiffFuncId),
        SILType::getPrimitiveObjectType(assocType));
    auto convertedRef = reapplyFunctionConversion(
        context, ref, classMethod, original, builder, loc, newBuffersToDealloc,
        desiredConfig.parameterIndices, desiredConfig.resultIndices);
    return std::make_pair(convertedRef, *minimalConfig);
  }

  // Emit the general opaque function error.
  context.emitNondifferentiabilityError(
      original, invoker, diag::autodiff_opaque_function_not_differentiable);
  return llvm::None;
}

//===----------------------------------------------------------------------===//
// `SILDifferentiabilityWitness` processing
//===----------------------------------------------------------------------===//

static SILFunction *createEmptyVJP(ADContext &context,
                                   SILDifferentiabilityWitness *witness,
                                   IsSerialized_t isSerialized) {
  auto original = witness->getOriginalFunction();
  auto config = witness->getConfig();
  LLVM_DEBUG({
    auto &s = getADDebugStream();
    s << "Creating VJP for " << original->getName() << ":\n\t";
    s << "Original type: " << original->getLoweredFunctionType() << "\n\t";
    s << "Config: " << config << "\n\t";
  });

  auto &module = context.getModule();
  auto originalTy = original->getLoweredFunctionType();

  // === Create an empty VJP. ===
  Mangle::DifferentiationMangler mangler;
  auto vjpName = mangler.mangleDerivativeFunction(
      original->getName(), AutoDiffDerivativeFunctionKind::VJP, config);
  auto vjpCanGenSig = witness->getDerivativeGenericSignature().getCanonicalSignature();
  GenericEnvironment *vjpGenericEnv = nullptr;
  if (vjpCanGenSig && !vjpCanGenSig->areAllParamsConcrete())
    vjpGenericEnv = vjpCanGenSig.getGenericEnvironment();
  auto vjpType = originalTy->getAutoDiffDerivativeFunctionType(
      config.parameterIndices, config.resultIndices,
      AutoDiffDerivativeFunctionKind::VJP,
      module.Types, LookUpConformanceInModule(module.getSwiftModule()),
      vjpCanGenSig,
      /*isReabstractionThunk*/ original->isThunk() == IsReabstractionThunk);

  SILOptFunctionBuilder fb(context.getTransform());
  auto *vjp = fb.createFunction(
      witness->getLinkage(),
      context.getASTContext().getIdentifier(vjpName).str(), vjpType,
      vjpGenericEnv, original->getLocation(), original->isBare(),
      IsNotTransparent, isSerialized, original->isDynamicallyReplaceable(),
      original->isDistributed(),
      original->isRuntimeAccessible());
  vjp->setDebugScope(new (module) SILDebugScope(original->getLocation(), vjp));

  LLVM_DEBUG(llvm::dbgs() << "VJP type: " << vjp->getLoweredFunctionType()
                          << "\n");
  return vjp;
}

static SILFunction *createEmptyJVP(ADContext &context,
                                   SILDifferentiabilityWitness *witness,
                                   IsSerialized_t isSerialized) {
  auto original = witness->getOriginalFunction();
  auto config = witness->getConfig();
  LLVM_DEBUG({
    auto &s = getADDebugStream();
    s << "Creating JVP for " << original->getName() << ":\n\t";
    s << "Original type: " << original->getLoweredFunctionType() << "\n\t";
    s << "Config: " << config << "\n\t";
  });

  auto &module = context.getModule();
  auto originalTy = original->getLoweredFunctionType();

  Mangle::DifferentiationMangler mangler;
  auto jvpName = mangler.mangleDerivativeFunction(
      original->getName(), AutoDiffDerivativeFunctionKind::JVP, config);
  auto jvpCanGenSig = witness->getDerivativeGenericSignature().getCanonicalSignature();
  GenericEnvironment *jvpGenericEnv = nullptr;
  if (jvpCanGenSig && !jvpCanGenSig->areAllParamsConcrete())
    jvpGenericEnv = jvpCanGenSig.getGenericEnvironment();
  auto jvpType = originalTy->getAutoDiffDerivativeFunctionType(
      config.parameterIndices, config.resultIndices,
      AutoDiffDerivativeFunctionKind::JVP,
      module.Types, LookUpConformanceInModule(module.getSwiftModule()),
      jvpCanGenSig,
      /*isReabstractionThunk*/ original->isThunk() == IsReabstractionThunk);

  SILOptFunctionBuilder fb(context.getTransform());
  auto *jvp = fb.createFunction(
      witness->getLinkage(),
      context.getASTContext().getIdentifier(jvpName).str(), jvpType,
      jvpGenericEnv, original->getLocation(), original->isBare(),
      IsNotTransparent, isSerialized, original->isDynamicallyReplaceable(),
      original->isDistributed(),
      original->isRuntimeAccessible());
  jvp->setDebugScope(new (module) SILDebugScope(original->getLocation(), jvp));

  LLVM_DEBUG(llvm::dbgs() << "JVP type: " << jvp->getLoweredFunctionType()
                          << "\n");
  return jvp;
}

/// Apply the fatal error function with the given name of type
/// `@convention(thin) () -> Never` in `f`.
static void emitFatalError(ADContext &context, SILFunction *f,
                           StringRef fatalErrorFuncName) {
  auto *entry = f->createBasicBlock();
  createEntryArguments(f);
  SILBuilder builder(entry);
  auto loc = f->getLocation();
  // Destroy all owned arguments to pass ownership verification.
  for (auto *arg : entry->getArguments())
    if (arg->getOwnershipKind() == OwnershipKind::Owned)
      builder.emitDestroyOperation(loc, arg);
  // Fatal error with a nice message.
  auto neverTy =
      context.getModule().getASTContext().getNeverType()->getCanonicalType();
  auto neverResultInfo = SILResultInfo(neverTy, ResultConvention::Unowned);
  // Fatal error function must have type `@convention(thin) () -> Never`.
  auto fatalErrorFnType = SILFunctionType::get(
      /*genericSig*/ nullptr, SILFunctionType::ExtInfo::getThin(),
      SILCoroutineKind::None, ParameterConvention::Direct_Unowned, {},
      /*interfaceYields*/ {}, neverResultInfo,
      /*interfaceErrorResults*/ llvm::None, {}, {}, context.getASTContext());
  auto fnBuilder = SILOptFunctionBuilder(context.getTransform());
  auto *fatalErrorFn = fnBuilder.getOrCreateFunction(
      loc, fatalErrorFuncName, SILLinkage::PublicExternal, fatalErrorFnType,
      IsNotBare, IsNotTransparent, IsNotSerialized, IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible, ProfileCounter(), IsNotThunk);
  auto *fatalErrorFnRef = builder.createFunctionRef(loc, fatalErrorFn);
  builder.createApply(loc, fatalErrorFnRef, SubstitutionMap(), {});
  builder.createUnreachable(loc);
}

/// Returns true on error.
bool DifferentiationTransformer::canonicalizeDifferentiabilityWitness(
    SILDifferentiabilityWitness *witness, DifferentiationInvoker invoker,
    IsSerialized_t serializeFunctions) {
  std::string traceMessage;
  llvm::raw_string_ostream OS(traceMessage);
  OS << "processing ";
  witness->print(OS);
  OS << " on";
  OS.flush();
  PrettyStackTraceSILFunction trace(
      traceMessage.c_str(), witness->getOriginalFunction());

  assert(witness->isDefinition());

  // If the JVP doesn't exist, need to synthesize it.
  if (!witness->getJVP()) {
    // Diagnose:
    // - Functions with no return.
    // - Functions with unsupported control flow.
    if (context.getASTContext()
            .LangOpts.hasFeature(Feature::ForwardModeDifferentiation) &&
        (diagnoseNoReturn(context, witness->getOriginalFunction(), invoker) ||
         diagnoseUnsupportedControlFlow(
             context, witness->getOriginalFunction(), invoker)))
      return true;

    // Create empty JVP.
    auto *jvp = createEmptyJVP(context, witness, serializeFunctions);
    witness->setJVP(jvp);
    context.recordGeneratedFunction(jvp);

    // For now, only do JVP generation if the flag is enabled and if custom VJP
    // does not exist. If custom VJP exists but custom JVP does not, skip JVP
    // generation because generated JVP may not match semantics of custom VJP.
    // Instead, create an empty JVP.
    if (context.getASTContext()
            .LangOpts.hasFeature(Feature::ForwardModeDifferentiation) &&
        !witness->getVJP()) {
      // JVP and differential generation do not currently support functions with
      // multiple basic blocks.
      if (witness->getOriginalFunction()->size() > 1) {
        context.emitNondifferentiabilityError(
            witness->getOriginalFunction()->getLocation().getSourceLoc(),
            invoker, diag::autodiff_jvp_control_flow_not_supported);
        return true;
      }
      // Emit JVP function.
      JVPCloner cloner(context, witness, jvp, invoker);
      if (cloner.run())
        return true;
    } else {
      // If JVP generation is disabled or a user-defined custom VJP function
      // exists, fatal error with a nice message.
      emitFatalError(context, jvp,
                     "_fatalErrorForwardModeDifferentiationDisabled");
      LLVM_DEBUG(getADDebugStream()
                 << "Generated empty JVP for "
                 << witness->getOriginalFunction()->getName() << ":\n"
                 << *jvp);
    }
  }

  // If the VJP doesn't exist, need to synthesize it.
  if (!witness->getVJP()) {
    // Diagnose:
    // - Functions with no return.
    // - Functions with unsupported control flow.
    if (diagnoseNoReturn(context, witness->getOriginalFunction(), invoker) ||
        diagnoseUnsupportedControlFlow(
            context, witness->getOriginalFunction(), invoker))
      return true;

    // Create empty VJP.
    auto *vjp = createEmptyVJP(context, witness, serializeFunctions);
    witness->setVJP(vjp);
    context.recordGeneratedFunction(vjp);
    // Emit VJP function.
    VJPCloner cloner(context, witness, vjp, invoker);
    return cloner.run();
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Differentiation pass implementation
//===----------------------------------------------------------------------===//

/// The automatic differentiation pass.
namespace {
class Differentiation : public SILModuleTransform {
public:
  Differentiation() : SILModuleTransform() {}
  void run() override;
};
} // end anonymous namespace

/// Given a curry thunk application, clone the thunk to return a
/// `@differentiable` function-typed value and apply the cloned thunk.
///
/// Curry thunk type: `(Self) -> (T, ...) -> U`.
/// Cloned thunk type: `(Self) -> @differentiable (T, ...) -> U`.
static SILValue promoteCurryThunkApplicationToDifferentiableFunction(
    DifferentiationTransformer &dt, DifferentiableFunctionInst *dfi,
    SILBuilder &builder, SILLocation loc, DifferentiationInvoker invoker) {
  auto origFnOperand = dfi->getOriginalFunction();
  auto *parameterIndices = dfi->getParameterIndices();
  auto *resultIndices = dfi->getResultIndices();
  auto &context = dt.getContext();

  // Check for curry thunk application:
  // - The original function operand must be an `apply` instruction.
  // - The `apply` callee must be a `function_ref` instruction.
  // - The callee must return a function-typed value.
  auto *ai = dyn_cast<ApplyInst>(origFnOperand);
  if (!ai)
    return nullptr;
  auto *thunkRef = dyn_cast<FunctionRefInst>(ai->getCallee());
  if (!thunkRef)
    return nullptr;
  auto *thunk = thunkRef->getReferencedFunction();
  auto thunkTy = thunk->getLoweredFunctionType();
  auto thunkResult = thunkTy->getSingleResult();
  auto resultFnTy = thunkResult.getInterfaceType()->getAs<SILFunctionType>();
  if (!resultFnTy)
    return nullptr;

  // Create a new curry thunk.
  AutoDiffConfig desiredConfig(parameterIndices, resultIndices);
  // TODO(TF-685): Use more principled mangling for thunks.
  auto newThunkName = "AD__" + thunk->getName().str() +
                      "__differentiable_curry_thunk_" + desiredConfig.mangle();

  // Construct new curry thunk type with `@differentiable` function
  // result.
  auto diffResultFnTy = resultFnTy->getWithExtInfo(
      resultFnTy->getExtInfo()
          .intoBuilder()
          .withDifferentiabilityKind(DifferentiabilityKind::Reverse)
          .build());
  auto newThunkResult = thunkResult.getWithInterfaceType(diffResultFnTy);
  auto thunkType = SILFunctionType::get(
      thunkTy->getSubstGenericSignature(), thunkTy->getExtInfo(),
      thunkTy->getCoroutineKind(), thunkTy->getCalleeConvention(),
      thunkTy->getParameters(), {}, {newThunkResult}, {},
      thunkTy->getPatternSubstitutions(), thunkTy->getInvocationSubstitutions(),
      thunkTy->getASTContext());

  // Construct new curry thunk, returning a `@differentiable` function.
  SILOptFunctionBuilder fb(dt.getTransform());
  auto *newThunk = fb.getOrCreateFunction(
      loc, newThunkName, getSpecializedLinkage(thunk, thunk->getLinkage()),
      thunkType, thunk->isBare(), thunk->isTransparent(), thunk->isSerialized(),
      thunk->isDynamicallyReplaceable(), thunk->isDistributed(),
      thunk->isRuntimeAccessible(),
      ProfileCounter(), thunk->isThunk());
  // If new thunk is newly created: clone the old thunk body, wrap the
  // returned function value with an `differentiable_function`
  // instruction, and process the `differentiable_function` instruction.
  if (newThunk->empty()) {
    newThunk->setGenericEnvironment(thunkType->getSubstGenericSignature().getGenericEnvironment());

    BasicTypeSubstCloner cloner(thunk, newThunk);
    cloner.cloneFunction();
    auto *retInst = cast<ReturnInst>(newThunk->findReturnBB()->getTerminator());
    auto returnValue = retInst->getOperand();
    // Create `differentiable_function` instruction directly after the
    // defining instruction (e.g. `partial_apply`) of the returned value.
    // Note: `differentiable_function` is not created at the end of the
    // new thunk to avoid `alloc_stack`/`dealloc_stack` ordering issues.
    SILBuilderWithScope dfiBuilder(
        std::next(returnValue->getDefiningInstruction()->getIterator()));
    auto *dfi = context.createDifferentiableFunction(
        dfiBuilder, loc, parameterIndices, resultIndices, returnValue);
    dfiBuilder.setInsertionPoint(newThunk->findReturnBB());
    dfiBuilder.createReturn(loc, dfi);
    retInst->eraseFromParent();

    context.recordGeneratedFunction(newThunk);
    context.getDifferentiableFunctionInstWorklist().push_back(dfi);
    if (dt.processDifferentiableFunctionInst(dfi))
      return nullptr;
  }

  // Apply the new curry thunk.
  auto *newThunkRef = builder.createFunctionRef(loc, newThunk);
  context.recordGeneratedFunctionReference(newThunkRef);
  SmallVector<SILValue, 8> newArgs;
  SmallVector<SILValue, 8> newArgsToDestroy;
  SmallVector<AllocStackInst *, 1> newBuffersToDealloc;
  copyParameterArgumentsForApply(ai, newArgs, newArgsToDestroy,
                                 newBuffersToDealloc);
  auto *newApply = builder.createApply(
      loc, newThunkRef, ai->getSubstitutionMap(), newArgs,
      ai->getApplyOptions());
  for (auto arg : newArgsToDestroy)
    builder.emitDestroyOperation(loc, arg);
  for (auto *alloc : newBuffersToDealloc)
    builder.createDeallocStack(loc, alloc);
  return newApply;
}

SILValue DifferentiationTransformer::promoteToDifferentiableFunction(
    DifferentiableFunctionInst *dfi, SILBuilder &builder, SILLocation loc,
    DifferentiationInvoker invoker) {
  auto &astCtx = context.getASTContext();
  auto origFnOperand = dfi->getOriginalFunction();
  auto origFnTy = origFnOperand->getType().castTo<SILFunctionType>();
  auto *parameterIndices = dfi->getParameterIndices();
  auto *resultIndices = dfi->getResultIndices();

  if (auto diffFn = promoteCurryThunkApplicationToDifferentiableFunction(
          *this, dfi, builder, loc, invoker))
    return diffFn;

  AutoDiffConfig desiredConfig(parameterIndices, resultIndices);
  SmallVector<SILValue, 2> derivativeFns;
  SmallVector<AllocStackInst *, 2> newBuffersToDealloc;
  for (auto derivativeFnKind : {AutoDiffDerivativeFunctionKind::JVP,
                                AutoDiffDerivativeFunctionKind::VJP}) {
    auto derivativeFnAndIndices = emitDerivativeFunctionReference(
        *this, builder, desiredConfig, derivativeFnKind, origFnOperand,
        invoker, newBuffersToDealloc);
    // Show an error at the operator, highlight the argument, and show a note
    // at the definition site of the argument.
    if (!derivativeFnAndIndices)
      return nullptr;

    auto derivativeFn = derivativeFnAndIndices->first;
    context.recordGeneratedFunctionReference(derivativeFn);

    // If desired indices are a subset of actual indices, create a "subset
    // indices thunk" and destroy the emitted derivative function reference.
    // - For JVPs: the thunked JVP returns a differential taking fewer
    //   parameters (using `.zero` for the dropped parameters).
    // - For VJPs: the thunked VJP returns a pullback that drops the unused
    //   tangent values.
    auto actualConfig = derivativeFnAndIndices->second;
    // NOTE: `desiredIndices` may come from a partially-applied function and
    // have smaller capacity than `actualIndices`. We expect this logic to go
    // away when we support `@differentiable` partial apply.
    // if (actualIndices != desiredIndices) { // TODO: Re-enable.
    auto extendedDesiredParameterIndices =
        desiredConfig.parameterIndices->extendingCapacity(
            astCtx, actualConfig.parameterIndices->getCapacity());
    if (!actualConfig.parameterIndices->equals(extendedDesiredParameterIndices)
        || !actualConfig.resultIndices->equals(desiredConfig.resultIndices)) {
      // Destroy the already emitted derivative function reference because it
      // is no longer used.
      builder.emitDestroyValueOperation(loc, derivativeFn);
      // Check if underlying original function reference has been partially
      // applied with arguments. If so, produce an error: parameter subset
      // thunks do not yet support this case because partially applied arguments
      // cannot be propagated to parameter subset thunks.
      auto didPartiallyApplyArguments = [](SILValue original) {
        while (auto *pai =
                   peerThroughFunctionConversions<PartialApplyInst>(original)) {
          if (pai->getNumArguments() > 0)
            return true;
          original = pai->getCallee();
        }
        return false;
      };
      if (didPartiallyApplyArguments(origFnOperand)) {
        context.emitNondifferentiabilityError(
            origFnOperand, invoker,
            diag::autodiff_cannot_param_subset_thunk_partially_applied_orig_fn);
        return nullptr;
      }
      // Create the parameter subset thunk.
      assert(actualConfig.parameterIndices->isSupersetOf(
          extendedDesiredParameterIndices));
      SILFunction *thunk;
      SubstitutionMap interfaceSubs;
      SILOptFunctionBuilder fb(transform);
      std::tie(thunk, interfaceSubs) =
          getOrCreateSubsetParametersThunkForDerivativeFunction(
              fb, origFnOperand, derivativeFn, derivativeFnKind, desiredConfig,
              actualConfig, context);
      auto *thunkFRI = builder.createFunctionRef(loc, thunk);
      if (auto genSig =
              thunk->getLoweredFunctionType()->getSubstGenericSignature()) {
        derivativeFn =
            builder.createPartialApply(loc, thunkFRI, interfaceSubs, {},
                                       ParameterConvention::Direct_Guaranteed);
      } else {
        derivativeFn = thunkFRI;
      }
    }
    auto expectedDerivativeFnTy = origFnTy->getAutoDiffDerivativeFunctionType(
        parameterIndices, resultIndices, derivativeFnKind,
        context.getTypeConverter(),
        LookUpConformanceInModule(context.getModule().getSwiftModule()));
    // If `derivativeFn` is `@convention(thin)` but is expected to be
    // `@convention(thick)`, emit a `thin_to_thick` instruction.
    if (expectedDerivativeFnTy->getRepresentation() ==
            SILFunctionTypeRepresentation::Thick &&
        derivativeFn->getType()
                .castTo<SILFunctionType>()
                ->getRepresentation() == SILFunctionTypeRepresentation::Thin) {
      derivativeFn = builder.createThinToThickFunction(
          loc, derivativeFn,
          SILType::getPrimitiveObjectType(expectedDerivativeFnTy));
    }
    // If derivative function value's type is not ABI-compatible with the
    // expected derivative function type (i.e. parameter and result conventions
    // do not match), perform reabstraction.
    auto abiCompatibility = expectedDerivativeFnTy->isABICompatibleWith(
        derivativeFn->getType().castTo<SILFunctionType>(), *dfi->getFunction());
    if (!abiCompatibility.isCompatible()) {
      SILOptFunctionBuilder fb(context.getTransform());
      auto newDerivativeFn = reabstractFunction(
          builder, fb, loc, derivativeFn, expectedDerivativeFnTy,
          [](SubstitutionMap substMap) { return substMap; });
      derivativeFn = newDerivativeFn;
      assert(expectedDerivativeFnTy
                 ->isABICompatibleWith(
                     derivativeFn->getType().castTo<SILFunctionType>(),
                     *dfi->getFunction())
                 .isCompatible());
    }

    derivativeFns.push_back(derivativeFn);
  }
  // Deallocate temporary buffers used for creating derivative functions.
  for (auto *buf : llvm::reverse(newBuffersToDealloc))
    builder.createDeallocStack(loc, buf);

  // If our original copy does not have none ownership, copy it.
  if (origFnOperand->getOwnershipKind() != OwnershipKind::None)
    origFnOperand = builder.emitCopyValueOperation(loc, origFnOperand);
  auto *newDiffFn = context.createDifferentiableFunction(
      builder, loc, parameterIndices, resultIndices, origFnOperand,
      std::make_pair(derivativeFns[0], derivativeFns[1]));
  context.getDifferentiableFunctionInstWorklist().push_back(dfi);
  return newDiffFn;
}

SILValue DifferentiationTransformer::promoteToLinearFunction(
    LinearFunctionInst *lfi, SILBuilder &builder, SILLocation loc,
    DifferentiationInvoker invoker) {
  // Note: for now, this function creates a new `linear_function` instruction
  // with an undef transpose function operand. Eventually, a legitimate
  // transpose function operand should be created and used.
  auto origFnOperand = lfi->getOriginalFunction();
  if (origFnOperand->getOwnershipKind() != OwnershipKind::None)
    origFnOperand = builder.emitCopyValueOperation(loc, origFnOperand);
  auto *parameterIndices = lfi->getParameterIndices();
  auto originalType = origFnOperand->getType().castTo<SILFunctionType>();
  auto transposeFnType = originalType->getAutoDiffTransposeFunctionType(
      parameterIndices, context.getTypeConverter(),
      LookUpConformanceInModule(builder.getModule().getSwiftModule()));
  auto transposeType = SILType::getPrimitiveObjectType(transposeFnType);
  auto transposeFn = SILUndef::get(transposeType, builder.getFunction());
  auto *newLinearFn = context.createLinearFunction(
      builder, loc, parameterIndices, origFnOperand, SILValue(transposeFn));
  context.getLinearFunctionInstWorklist().push_back(lfi);
  return newLinearFn;
}

bool DifferentiationTransformer::processDifferentiableFunctionInst(
    DifferentiableFunctionInst *dfi) {
  PrettyStackTraceSILNode dfiTrace("canonicalizing `differentiable_function`",
                                   dfi);
  PrettyStackTraceSILFunction fnTrace("...in", dfi->getFunction());
  LLVM_DEBUG({
    auto &s = getADDebugStream() << "Processing DifferentiableFunctionInst:\n";
    dfi->printInContext(s);
  });

  // If `dfi` already has derivative functions, do not process.
  if (dfi->hasDerivativeFunctions())
    return false;

  SILFunction *parent = dfi->getFunction();
  auto loc = dfi->getLoc();
  SILBuilderWithScope builder(dfi);
  auto differentiableFnValue =
      promoteToDifferentiableFunction(dfi, builder, loc, dfi);
  // Mark `dfi` as processed so that it won't be reprocessed after deletion.
  context.markDifferentiableFunctionInstAsProcessed(dfi);
  if (!differentiableFnValue)
    return true;
  // Replace all uses of `dfi`.
  dfi->replaceAllUsesWith(differentiableFnValue);
  // Destroy the original operand.
  builder.emitDestroyValueOperation(loc, dfi->getOriginalFunction());
  dfi->eraseFromParent();
  transform.invalidateAnalysis(parent,
                               SILAnalysis::InvalidationKind::FunctionBody);
  return false;
}

bool DifferentiationTransformer::processLinearFunctionInst(
    LinearFunctionInst *lfi) {
  PrettyStackTraceSILNode dfiTrace("canonicalizing `linear_function`", lfi);
  PrettyStackTraceSILFunction fnTrace("...in", lfi->getFunction());
  LLVM_DEBUG({
    auto &s = getADDebugStream() << "Processing LinearFunctionInst:\n";
    lfi->printInContext(s);
  });

  // If `lfi` already has a transpose function, do not process.
  if (lfi->hasTransposeFunction())
    return false;

  SILFunction *parent = lfi->getFunction();
  auto loc = lfi->getLoc();
  SILBuilderWithScope builder(lfi);
  auto linearFnValue = promoteToLinearFunction(lfi, builder, loc, lfi);
  // Mark `lfi` as processed so that it won't be reprocessed after deletion.
  context.markLinearFunctionInstAsProcessed(lfi);
  if (!linearFnValue)
    return true;
  // Replace all uses of `lfi`.
  lfi->replaceAllUsesWith(linearFnValue);
  // Destroy the original operand.
  builder.emitDestroyValueOperation(loc, lfi->getOriginalFunction());
  lfi->eraseFromParent();

  transform.invalidateAnalysis(parent,
                               SILAnalysis::InvalidationKind::FunctionBody);
  return false;
}

/// Automatic differentiation transform entry.
void Differentiation::run() {
  auto &module = *getModule();
  auto &astCtx = module.getASTContext();
  debugDump(module);

  // A transformation helper.
  DifferentiationTransformer transformer(*this);
  ADContext &context = transformer.getContext();

  bool errorOccurred = false;

  // Register all the SIL differentiability witnesses in the module that trigger
  // differentiation.
  for (auto &witness : module.getDifferentiabilityWitnesses()) {
    if (witness.isDeclaration())
      continue;
    context.addInvoker(&witness);
  }

  // Register all the `differentiable_function` and `linear_function`
  // instructions in the module that trigger differentiation.
  for (SILFunction &f : module) {
    for (SILBasicBlock &bb : f) {
      for (SILInstruction &i : bb) {
        if (auto *dfi = dyn_cast<DifferentiableFunctionInst>(&i)) {
          context.getDifferentiableFunctionInstWorklist().push_back(dfi);
        } else if (auto *lfi = dyn_cast<LinearFunctionInst>(&i)) {
          // If linear map transposition is not enabled and an uncanonical
          // `linear_function` instruction is encountered, emit a diagnostic.
          // FIXME(https://github.com/apple/swift/issues/54256): Finish support for linear map transposition.
          if (!EnableExperimentalLinearMapTransposition) {
            if (!lfi->hasTransposeFunction()) {
              astCtx.Diags.diagnose(
                lfi->getLoc().getSourceLoc(),
                diag::autodiff_conversion_to_linear_function_not_supported);
              errorOccurred = true;
            }
          }
          context.getLinearFunctionInstWorklist().push_back(lfi);
        }
      }
    }
  }

  // If nothing has triggered differentiation, there's nothing to do.
  if (context.getInvokers().empty() &&
      context.getDifferentiableFunctionInstWorklist().empty() &&
      context.getLinearFunctionInstWorklist().empty())
    return;

  // Differentiation relies on the stdlib (the Swift module).
  // If it's not imported, it's an internal error.
  if (!astCtx.getStdlibModule()) {
    astCtx.Diags.diagnose(SourceLoc(),
                          diag::autodiff_internal_swift_not_imported);
    return;
  }
  if (!astCtx.getLoadedModule(astCtx.Id_Differentiation)) {
    SourceLoc loc;
    if (!context.getInvokers().empty()) {
      loc = context.getInvokers().front().second.getLocation();
    } else {
      assert(!context.getDifferentiableFunctionInstWorklist().empty());
      loc = context.getDifferentiableFunctionInstWorklist()
                .pop_back_val()
                ->getLoc()
                .getSourceLoc();
    }
    astCtx.Diags.diagnose(loc,
                          diag::autodiff_differentiation_module_not_imported);
    return;
  }

  // Process all invokers.
  for (auto invokerPair : context.getInvokers()) {
    auto *witness = invokerPair.first;
    auto invoker = invokerPair.second;
    if (transformer.canonicalizeDifferentiabilityWitness(
            witness, invoker, witness->getOriginalFunction()->isSerialized()))
      errorOccurred = true;
  }

  // Iteratively process `differentiable_function` instruction worklist.
  while (!context.getDifferentiableFunctionInstWorklist().empty()) {
    auto *dfi = context.getDifferentiableFunctionInstWorklist().pop_back_val();
    // Skip instructions that have been already been processed.
    if (context.isDifferentiableFunctionInstProcessed(dfi))
      continue;
    errorOccurred |= transformer.processDifferentiableFunctionInst(dfi);
  }

  // Iteratively process `linear_function` instruction worklist.
  while (!context.getLinearFunctionInstWorklist().empty()) {
    auto *lfi = context.getLinearFunctionInstWorklist().pop_back_val();
    // Skip instructions that have been already been processed.
    if (context.isLinearFunctionInstProcessed(lfi))
      continue;
    errorOccurred |= transformer.processLinearFunctionInst(lfi);
  }

  // If any error occurred while processing witnesses or
  // `differentiable_function` instructions, clean up.
  if (errorOccurred) {
    context.cleanUp();
    return;
  }

  LLVM_DEBUG(getADDebugStream() << "All differentiation finished\n");
}

//===----------------------------------------------------------------------===//
// Pass creation
//===----------------------------------------------------------------------===//

SILTransform *swift::createDifferentiation() { return new Differentiation; }
