//===--- JVPCloner.cpp - JVP function generation --------------*- C++ -*---===//
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
// This file defines a helper class for generating JVP functions for automatic
// differentiation.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/JVPCloner.h"
#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Differentiation/ADContext.h"
#include "swift/SILOptimizer/Differentiation/AdjointValue.h"
#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"
#include "swift/SILOptimizer/Differentiation/LinearMapInfo.h"
#include "swift/SILOptimizer/Differentiation/PullbackCloner.h"
#include "swift/SILOptimizer/Differentiation/Thunk.h"

#include "swift/SIL/LoopInfo.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/PrettyStackTrace.h"
#include "swift/SILOptimizer/Utils/DifferentiationMangler.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/DenseMap.h"

using namespace swift;
using namespace autodiff;

namespace swift {
namespace autodiff {

class JVPCloner::Implementation final
    : public TypeSubstCloner<JVPCloner::Implementation, SILOptFunctionBuilder> {
private:
  /// The global context.
  ADContext &context;

  /// The original function.
  SILFunction *const original;

  /// The witness.
  SILDifferentiabilityWitness *const witness;

  /// The JVP function.
  SILFunction *const jvp;

  llvm::BumpPtrAllocator allocator;

  /// The differentiation invoker.
  DifferentiationInvoker invoker;

  /// Info from activity analysis on the original function.
  const DifferentiableActivityInfo &activityInfo;

  /// The loop info.
  SILLoopInfo *loopInfo;

  /// The differential info.
  LinearMapInfo differentialInfo;

  bool errorOccurred = false;

  //--------------------------------------------------------------------------//
  // Differential generation related fields
  //--------------------------------------------------------------------------//

  /// The builder for the differential function.
  TangentBuilder differentialBuilder;

  /// Mapping from original basic blocks to corresponding differential basic
  /// blocks.
  llvm::DenseMap<SILBasicBlock *, SILBasicBlock *> diffBBMap;

  /// Mapping from original basic blocks and original values to corresponding
  /// tangent values.
  llvm::DenseMap<SILValue, AdjointValue> tangentValueMap;

  /// Mapping from original basic blocks and original buffers to corresponding
  /// tangent buffers.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILValue>, SILValue> bufferMap;

  /// Mapping from differential basic blocks to differential struct arguments.
  llvm::DenseMap<SILBasicBlock *, SILArgument *> differentialStructArguments;

  /// Mapping from differential struct field declarations to differential struct
  /// elements destructured from the linear map basic block argument. In the
  /// beginning of each differential basic block, the block's differential
  /// struct is destructured into the individual elements stored here.
  llvm::DenseMap<SILBasicBlock *, SILInstructionResultArray> differentialTupleElements;

  /// An auxiliary differential local allocation builder.
  TangentBuilder diffLocalAllocBuilder;

  /// Stack buffers allocated for storing local tangent values.
  SmallVector<SILValue, 8> differentialLocalAllocations;

  /// Mapping from original blocks to differential values. Used to build
  /// differential struct instances.
  llvm::DenseMap<SILBasicBlock *, SmallVector<SILValue, 8>> differentialValues;

  //--------------------------------------------------------------------------//
  // Getters
  //--------------------------------------------------------------------------//

  ASTContext &getASTContext() const { return jvp->getASTContext(); }
  SILModule &getModule() const { return jvp->getModule(); }
  const AutoDiffConfig getConfig() const { return witness->getConfig(); }
  TangentBuilder &getDifferentialBuilder() { return differentialBuilder; }
  SILFunction &getDifferential() { return differentialBuilder.getFunction(); }
  SILArgument *getDifferentialStructArgument(SILBasicBlock *origBB) {
    return differentialStructArguments[origBB];
  }

  //--------------------------------------------------------------------------//
  // Differential tuple mapping
  //--------------------------------------------------------------------------//

  void initializeDifferentialTupleElements(SILBasicBlock *origBB,
                                           SILInstructionResultArray values);

  SILValue getDifferentialTupleElement(ApplyInst *ai);

  //--------------------------------------------------------------------------//
  // General utilities
  //--------------------------------------------------------------------------//

  /// Get the lowered SIL type of the given AST type.
  SILType getLoweredType(Type type) {
    auto jvpGenSig = jvp->getLoweredFunctionType()->getSubstGenericSignature();
    Lowering::AbstractionPattern pattern(jvpGenSig,
                                         type->getReducedType(jvpGenSig));
    return jvp->getLoweredType(pattern, type);
  }

  /// Get the lowered SIL type of the given nominal type declaration.
  SILType getNominalDeclLoweredType(NominalTypeDecl *nominal) {
    auto nominalType =
        getOpASTType(nominal->getDeclaredInterfaceType()->getCanonicalType());
    return getLoweredType(nominalType);
  }

  /// Build a differential struct value for the original block corresponding to
  /// the given terminator.
  TupleInst *buildDifferentialValueStructValue(TermInst *termInst) {
    assert(termInst->getFunction() == original);
    auto loc = termInst->getFunction()->getLocation();
    auto *origBB = termInst->getParent();
    auto *jvpBB = BBMap[origBB];
    assert(jvpBB && "Basic block mapping should exist");
    auto tupleLoweredTy =
      remapType(differentialInfo.getLinearMapTupleLoweredType(origBB));
    auto bbDifferentialValues = differentialValues[origBB];
    if (!origBB->isEntry()) {
      auto *enumArg = jvpBB->getArguments().back();
      bbDifferentialValues.insert(bbDifferentialValues.begin(), enumArg);
    }
    return getBuilder().createTuple(loc, tupleLoweredTy,
                                    bbDifferentialValues);
  }

  //--------------------------------------------------------------------------//
  // Tangent value factory methods
  //--------------------------------------------------------------------------//

  AdjointValue makeZeroTangentValue(SILType type) {
    return AdjointValue::createZero(allocator,
                                    remapSILTypeInDifferential(type));
  }

  AdjointValue makeConcreteTangentValue(SILValue value) {
    return AdjointValue::createConcrete(allocator, value);
  }

  //--------------------------------------------------------------------------//
  // Tangent materialization
  //--------------------------------------------------------------------------//

  void emitZeroIndirect(CanType type, SILValue buffer, SILLocation loc) {
    auto builder = getDifferentialBuilder();
    auto tangentSpace = getTangentSpace(type);
    assert(tangentSpace && "No tangent space for this type");
    switch (tangentSpace->getKind()) {
    case TangentSpace::Kind::TangentVector:
      builder.emitZeroIntoBuffer(loc, buffer, IsInitialization);
      return;
    case TangentSpace::Kind::Tuple: {
      auto tupleType = tangentSpace->getTuple();
      SmallVector<SILValue, 8> zeroElements;
      for (unsigned i : range(tupleType->getNumElements())) {
        auto eltAddr = builder.createTupleElementAddr(loc, buffer, i);
        emitZeroIndirect(tupleType->getElementType(i)->getCanonicalType(),
                         eltAddr, loc);
      }
      return;
    }
    }
  }

  SILValue emitZeroDirect(CanType type, SILLocation loc) {
    auto diffBuilder = getDifferentialBuilder();
    auto silType = getModule().Types.getLoweredLoadableType(
        type, TypeExpansionContext::minimal(), getModule());
    auto *buffer = diffBuilder.createAllocStack(loc, silType);
    emitZeroIndirect(type, buffer, loc);
    auto loaded = diffBuilder.emitLoadValueOperation(
        loc, buffer, LoadOwnershipQualifier::Take);
    diffBuilder.createDeallocStack(loc, buffer);
    return loaded;
  }

  SILValue materializeTangentDirect(AdjointValue val, SILLocation loc) {
    assert(val.getType().isObject());
    LLVM_DEBUG(getADDebugStream()
               << "Materializing tangents for " << val << '\n');
    switch (val.getKind()) {
    case AdjointValueKind::Zero: {
      auto zeroVal = emitZeroDirect(val.getSwiftType(), loc);
      return zeroVal;
    }
    case AdjointValueKind::Aggregate:
      llvm_unreachable(
          "Tuples and structs are not supported in forward mode yet.");
    case AdjointValueKind::Concrete:
      return val.getConcreteValue();
    }
    llvm_unreachable("Invalid adjoint value kind"); // silences MSVC C4715
  }

  SILValue materializeTangent(AdjointValue val, SILLocation loc) {
    if (val.isConcrete()) {
      LLVM_DEBUG(getADDebugStream()
                 << "Materializing tangent: Value is concrete.\n");
      return val.getConcreteValue();
    }
    LLVM_DEBUG(getADDebugStream() << "Materializing tangent: Value is "
                                     "non-concrete. Materializing directly.\n");
    return materializeTangentDirect(val, loc);
  }

  //--------------------------------------------------------------------------//
  // Tangent value mapping
  //--------------------------------------------------------------------------//

  /// Get the tangent for an original value. The given value must be in the
  /// original function.
  ///
  /// This method first tries to find an entry in `tangentValueMap`. If an entry
  /// doesn't exist, create a zero tangent.
  AdjointValue getTangentValue(SILValue originalValue) {
    assert(originalValue->getType().isObject());
    assert(originalValue->getFunction() == original);
    auto insertion = tangentValueMap.try_emplace(
        originalValue,
        makeZeroTangentValue(getRemappedTangentType(originalValue->getType())));
    return insertion.first->getSecond();
  }

  /// Map the tangent value to the given original value.
  void setTangentValue(SILBasicBlock *origBB, SILValue originalValue,
                       AdjointValue newTangentValue) {
#ifndef NDEBUG
    if (auto *defInst = originalValue->getDefiningInstruction()) {
      bool isTupleTypedApplyResult =
          isa<ApplyInst>(defInst) && originalValue->getType().is<TupleType>();
      assert(!isTupleTypedApplyResult &&
             "Should not set tangent value for tuple-typed result from `apply` "
             "instruction; use `destructure_tuple` on `apply` result and set "
             "tangent value for `destructure_tuple` results instead.");
    }
#endif
    assert(originalValue->getType().isObject());
    assert(newTangentValue.getType().isObject());
    assert(originalValue->getFunction() == original);
    LLVM_DEBUG(getADDebugStream()
               << "Setting tangent value for " << originalValue);
    // The tangent value must be in the tangent space.
    assert(newTangentValue.getType() ==
           getRemappedTangentType(originalValue->getType()));
    auto insertion =
        tangentValueMap.try_emplace(originalValue, newTangentValue);
    (void)insertion;
    assert(insertion.second && "The tangent value should not already exist.");
  }

  //--------------------------------------------------------------------------//
  // Tangent buffer mapping
  //--------------------------------------------------------------------------//

  /// Sets the tangent buffer for the original buffer. Asserts that the
  /// original buffer does not already have a tangent buffer.
  void setTangentBuffer(SILBasicBlock *origBB, SILValue originalBuffer,
                        SILValue tangentBuffer) {
    assert(originalBuffer->getType().isAddress());
    auto insertion =
        bufferMap.try_emplace({origBB, originalBuffer}, tangentBuffer);
    assert(insertion.second && "Tangent buffer already exists");
    (void)insertion;
  }

  /// Returns the tangent buffer for the original buffer. Asserts that the
  /// original buffer has a tangent buffer.
  SILValue &getTangentBuffer(SILBasicBlock *origBB, SILValue originalBuffer) {
    assert(originalBuffer->getType().isAddress());
    assert(originalBuffer->getFunction() == original);
    auto it = bufferMap.find({origBB, originalBuffer});
    assert(it != bufferMap.end() && "Tangent buffer should already exist");
    return it->getSecond();
  }

  //--------------------------------------------------------------------------//
  // Differential type calculations
  //--------------------------------------------------------------------------//

  /// Substitutes all replacement types of the given substitution map using the
  /// tangent function's substitution map.
  SubstitutionMap remapSubstitutionMapInDifferential(SubstitutionMap substMap) {
    return substMap.subst(getDifferential().getForwardingSubstitutionMap());
  }

  /// Remap any archetypes into the differential function's context.
  Type remapTypeInDifferential(Type ty) {
    if (ty->hasArchetype())
      return getDifferential().mapTypeIntoContext(ty->mapTypeOutOfContext());
    return getDifferential().mapTypeIntoContext(ty);
  }

  /// Remap any archetypes into the differential function's context.
  SILType remapSILTypeInDifferential(SILType ty) {
    if (ty.hasArchetype())
      return getDifferential().mapTypeIntoContext(ty.mapTypeOutOfContext());
    return getDifferential().mapTypeIntoContext(ty);
  }

  /// Find the tangent space of a given canonical type.
  llvm::Optional<TangentSpace> getTangentSpace(CanType type) {
    // Use witness generic signature to remap types.
    type = witness->getDerivativeGenericSignature().getReducedType(
        type);
    return type->getAutoDiffTangentSpace(
        LookUpConformanceInModule(getModule().getSwiftModule()));
  }

  /// Assuming the given type conforms to `Differentiable` after remapping,
  /// returns the associated tangent space SIL type.
  SILType getRemappedTangentType(SILType type) {
    return SILType::getPrimitiveType(
        getTangentSpace(remapSILTypeInDifferential(type).getASTType())
            ->getCanonicalType(),
        type.getCategory());
  }

  /// Set up the differential function. This includes:
  /// - Creating all differential blocks.
  /// - Creating differential entry block arguments based on the function type.
  /// - Creating tangent value mapping for original/differential parameters.
  /// - Checking for unvaried result and emitting related warnings.
  void prepareForDifferentialGeneration();

public:
  explicit Implementation(ADContext &context,
                          SILDifferentiabilityWitness *witness,
                          SILFunction *jvp, DifferentiationInvoker invoker);

  static SILFunction *
  createEmptyDifferential(ADContext &context,
                          SILDifferentiabilityWitness *witness,
                          LinearMapInfo *linearMapInfo);

  /// Run JVP generation. Returns true on error.
  bool run();

  SILFunction &getJVP() const { return *jvp; }

  void postProcess(SILInstruction *orig, SILInstruction *cloned) {
    if (errorOccurred)
      return;
    SILClonerWithScopes::postProcess(orig, cloned);
  }

  /// Remap original basic blocks.
  SILBasicBlock *remapBasicBlock(SILBasicBlock *bb) {
    auto *jvpBB = BBMap[bb];
    return jvpBB;
  }

  /// General visitor for all instructions. If any error is emitted by previous
  /// visits, bail out.
  void visit(SILInstruction *inst) {
    if (errorOccurred)
      return;
    if (differentialInfo.shouldDifferentiateInstruction(inst)) {
      LLVM_DEBUG(getADDebugStream() << "JVPCloner visited:\n[ORIG]" << *inst);
#ifndef NDEBUG
      auto diffBuilder = getDifferentialBuilder();
      auto beforeInsertion = std::prev(diffBuilder.getInsertionPoint());
#endif
      TypeSubstCloner::visit(inst);
      LLVM_DEBUG({
        auto &s = llvm::dbgs() << "[TAN] Emitted in differential:\n";
        auto afterInsertion = diffBuilder.getInsertionPoint();
        for (auto it = ++beforeInsertion; it != afterInsertion; ++it)
          s << *it;
      });
    } else {
      TypeSubstCloner::visit(inst);
    }
  }

  void visitSILInstruction(SILInstruction *inst) {
    context.emitNondifferentiabilityError(
        inst, invoker, diag::autodiff_expression_not_differentiable_note);
    errorOccurred = true;
  }

  void visitInstructionsInBlock(SILBasicBlock *bb) {
    // Destructure the differential struct to get the elements.
    auto &diffBuilder = getDifferentialBuilder();
    auto diffLoc = getDifferential().getLocation();
    auto *diffBB = diffBBMap.lookup(bb);
    auto *mainDifferentialStruct = diffBB->getArguments().back();
    diffBuilder.setInsertionPoint(diffBB);
    auto *dsi =
        diffBuilder.createDestructureTuple(diffLoc, mainDifferentialStruct);
    initializeDifferentialTupleElements(bb, dsi->getResults());
    TypeSubstCloner::visitInstructionsInBlock(bb);
  }

  // If an `apply` has active results or active inout parameters, replace it
  // with an `apply` of its JVP.
  void visitApplyInst(ApplyInst *ai) {
    bool shouldDifferentiate =
        differentialInfo.shouldDifferentiateApplySite(ai);
    // If the function has no active arguments or results, zero-initialize the
    // tangent buffers of the active indirect results.
    if (!shouldDifferentiate) {
      for (auto indResult : ai->getIndirectSILResults())
        if (activityInfo.isActive(indResult, getConfig())) {
          auto &tanBuf = getTangentBuffer(ai->getParent(), indResult);
          emitZeroIndirect(tanBuf->getType().getASTType(), tanBuf,
                           tanBuf.getLoc());
        }
    }
    // If the function should not be differentiated or its the array literal
    // initialization intrinsic, just do standard cloning.
    if (!shouldDifferentiate ||
        ArraySemanticsCall(ai, semantics::ARRAY_UNINITIALIZED_INTRINSIC)) {
      LLVM_DEBUG(getADDebugStream() << "No active results:\n" << *ai << '\n');
      TypeSubstCloner::visitApplyInst(ai);
      return;
    }

    auto loc = ai->getLoc();
    auto &builder = getBuilder();
    auto origCallee = getOpValue(ai->getCallee());
    auto originalFnTy = origCallee->getType().castTo<SILFunctionType>();

    LLVM_DEBUG(getADDebugStream() << "JVP-transforming:\n" << *ai << '\n');

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
    LLVM_DEBUG(auto &s = getADDebugStream() << "Active indices: params={";
               llvm::interleave(
                   activeParamIndices.begin(), activeParamIndices.end(),
                   [&s](unsigned i) { s << i; }, [&s] { s << ", "; });
               s << "}, results={"; llvm::interleave(
                   activeResultIndices.begin(), activeResultIndices.end(),
                   [&s](unsigned i) { s << i; }, [&s] { s << ", "; });
               s << "}\n";);

    // Form expected indices.
    auto numResults =
        ai->getSubstCalleeType()->getNumResults() +
        ai->getSubstCalleeType()->getNumIndirectMutatingParameters();
    AutoDiffConfig config(
        IndexSubset::get(getASTContext(),
                         ai->getArgumentsWithoutIndirectResults().size(),
                         activeParamIndices),
        IndexSubset::get(getASTContext(), numResults, activeResultIndices));

    // Emit the JVP.
    SILValue jvpValue;
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
            jvpValue = builder.createDifferentiableFunctionExtract(
                loc, NormalDifferentiableFunctionTypeComponent::JVP,
                borrowedDiffFunc);
            jvpValue = builder.emitCopyValueOperation(loc, jvpValue);
          });
    }

    // If JVP has not yet been found, emit an `differentiable_function`
    // instruction on the remapped  function operand and
    // an `differentiable_function_extract` instruction to get the JVP.
    // The `differentiable_function` instruction will be canonicalized during
    // the transform main loop.
    if (!jvpValue) {
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
      // In the JVP, specialization is also necessary for parity. The original
      // function operand is specialized with a remapped version of same
      // substitution map using an argument-less `partial_apply`.
      if (ai->getSubstitutionMap().empty()) {
        origCallee = builder.emitCopyValueOperation(loc, origCallee);
      } else {
        auto substMap = getOpSubstitutionMap(ai->getSubstitutionMap());
        auto jvpPartialApply = getBuilder().createPartialApply(
            ai->getLoc(), origCallee, substMap, {},
            ParameterConvention::Direct_Guaranteed);
        origCallee = jvpPartialApply;
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
                auto startLoc = arg.getLoc().getStartSourceLoc();
                auto endLoc = arg.getLoc().getEndSourceLoc();
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
                auto inoutArgIdx = resultIndex - originalFnTy->getNumResults();
                auto inoutArg =
                    *std::next(ai->getInoutArguments().begin(), inoutArgIdx);
                remappedResultType = inoutArg->getType();
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

      auto *diffFuncInst = context.createDifferentiableFunction(
          builder, loc, config.parameterIndices, config.resultIndices,
          origCallee);

      // Record the `differentiable_function` instruction.
      context.getDifferentiableFunctionInstWorklist().push_back(diffFuncInst);

      builder.emitScopedBorrowOperation(
          loc, diffFuncInst, [&](SILValue borrowedADFunc) {
            auto extractedJVP = builder.createDifferentiableFunctionExtract(
                loc, NormalDifferentiableFunctionTypeComponent::JVP,
                borrowedADFunc);
            jvpValue = builder.emitCopyValueOperation(loc, extractedJVP);
          });
      builder.emitDestroyValueOperation(loc, diffFuncInst);
    }

    // Call the JVP using the original parameters.
    SmallVector<SILValue, 8> jvpArgs;
    auto jvpFnTy = getOpType(jvpValue->getType()).castTo<SILFunctionType>();
    auto numJVPArgs =
        jvpFnTy->getNumParameters() + jvpFnTy->getNumIndirectFormalResults();
    jvpArgs.reserve(numJVPArgs);
    // Collect substituted arguments.
    for (auto origArg : ai->getArguments())
      jvpArgs.push_back(getOpValue(origArg));
    assert(jvpArgs.size() == numJVPArgs);
    // Apply the JVP.
    // The JVP should be specialized, so no substitution map is necessary.
    auto *jvpCall = getBuilder().createApply(loc, jvpValue, SubstitutionMap(),
                                             jvpArgs, ai->getApplyOptions());
    LLVM_DEBUG(getADDebugStream() << "Applied jvp function\n" << *jvpCall);

    // Release the differentiable function.
    builder.emitDestroyValueOperation(loc, jvpValue);

    // Get the JVP results (original results and differential).
    SmallVector<SILValue, 8> jvpDirectResults;
    extractAllElements(jvpCall, builder, jvpDirectResults);
    auto originalDirectResults =
        ArrayRef<SILValue>(jvpDirectResults).drop_back(1);
    auto originalDirectResult =
        joinElements(originalDirectResults, getBuilder(), jvpCall->getLoc());

    mapValue(ai, originalDirectResult);

    // Some instructions that produce the callee may have been cloned.
    // If the original callee did not have any users beyond this `apply`,
    // recursively kill the cloned callee.
    if (auto *origCallee = cast_or_null<SingleValueInstruction>(
            ai->getCallee()->getDefiningInstruction()))
      if (origCallee->hasOneUse())
        recursivelyDeleteTriviallyDeadInstructions(
            getOpValue(origCallee)->getDefiningInstruction());

    // Add the differential function for when we create the struct we partially
    // apply to the differential we are generating.
    auto differential = jvpDirectResults.back();
    auto differentialType = differentialInfo.lookUpLinearMapType(ai);
    auto originalDifferentialType =
        getOpType(differential->getType()).getAs<SILFunctionType>();
    auto loweredDifferentialType =
        getOpType(getLoweredType(differentialType)).castTo<SILFunctionType>();
    // If actual differential type does not match lowered differential type,
    // reabstract the differential using a thunk.
    if (!loweredDifferentialType->isEqual(originalDifferentialType)) {
      SILOptFunctionBuilder fb(context.getTransform());
      differential = reabstractFunction(
          builder, fb, loc, differential, loweredDifferentialType,
          [this](SubstitutionMap subs) -> SubstitutionMap {
            return this->getOpSubstitutionMap(subs);
          });
    }
    differentialValues[ai->getParent()].push_back(differential);

    // Differential emission.
    emitTangentForApplyInst(ai, config, originalDifferentialType);
  }

  void visitReturnInst(ReturnInst *ri) {
    auto loc = ri->getOperand().getLoc();
    auto *origExit = ri->getParent();
    auto &builder = getBuilder();
    auto *diffStructVal = buildDifferentialValueStructValue(ri);

    // Get the JVP value corresponding to the original functions's return value.
    auto *origRetInst = cast<ReturnInst>(origExit->getTerminator());
    auto origResult = getOpValue(origRetInst->getOperand());
    SmallVector<SILValue, 8> origResults;
    extractAllElements(origResult, builder, origResults);

    // Get and partially apply the differential.
    auto jvpSubstMap = jvp->getForwardingSubstitutionMap();
    auto *differentialRef = builder.createFunctionRef(loc, &getDifferential());
    auto *differentialPartialApply = builder.createPartialApply(
        loc, differentialRef, jvpSubstMap, {diffStructVal},
        ParameterConvention::Direct_Guaranteed);

    auto differentialType = jvp->mapTypeIntoContext(
        jvp->getConventions().getSILType(
            jvp->getLoweredFunctionType()->getResults().back(),
            jvp->getTypeExpansionContext()));
    auto differentialFnType = differentialType.castTo<SILFunctionType>();
    auto differentialSubstType =
        differentialPartialApply->getType().castTo<SILFunctionType>();

    // If necessary, convert the differential value to the returned differential
    // function type.
    SILValue differentialValue;
    if (differentialSubstType == differentialFnType) {
      differentialValue = differentialPartialApply;
    } else if (differentialSubstType
                   ->isABICompatibleWith(differentialFnType, *jvp)
                   .isCompatible()) {
      differentialValue = builder.createConvertFunction(
          loc, differentialPartialApply, differentialType,
          /*withoutActuallyEscaping*/ false);
    } else {
      llvm::report_fatal_error("Differential value type is not ABI-compatible "
                               "with the returned differential type");
    }

    // Return a tuple of the original result and differential.
    SmallVector<SILValue, 8> directResults;
    directResults.append(origResults.begin(), origResults.end());
    directResults.push_back(differentialValue);
    builder.createReturn(ri->getLoc(),
                         joinElements(directResults, builder, loc));
  }

  void visitBranchInst(BranchInst *bi) {
    llvm_unreachable("Unsupported SIL instruction.");
  }

  void visitCondBranchInst(CondBranchInst *cbi) {
    llvm_unreachable("Unsupported SIL instruction.");
  }

  void visitSwitchEnumInst(SwitchEnumInst *sei) {
    llvm_unreachable("Unsupported SIL instruction.");
  }

  void visitDifferentiableFunctionInst(DifferentiableFunctionInst *dfi) {
    // Clone `differentiable_function` from original to JVP, then add the cloned
    // instruction to the `differentiable_function` worklist.
    TypeSubstCloner::visitDifferentiableFunctionInst(dfi);
    auto *newDFI = cast<DifferentiableFunctionInst>(getOpValue(dfi));
    context.getDifferentiableFunctionInstWorklist().push_back(newDFI);
  }

  void visitLinearFunctionInst(LinearFunctionInst *lfi) {
    // Clone `linear_function` from original to JVP, then add the cloned
    // instruction to the `linear_function` worklist.
    TypeSubstCloner::visitLinearFunctionInst(lfi);
    auto *newLFI = cast<LinearFunctionInst>(getOpValue(lfi));
    context.getLinearFunctionInstWorklist().push_back(newLFI);
  }

  //--------------------------------------------------------------------------//
  // Tangent emission helpers
  //--------------------------------------------------------------------------//

#define CLONE_AND_EMIT_TANGENT(INST, ID)                                       \
  void visit##INST##Inst(INST##Inst *inst) {                                   \
    TypeSubstCloner::visit##INST##Inst(inst);                                  \
    if (differentialInfo.shouldDifferentiateInstruction(inst))                 \
      emitTangentFor##INST##Inst(inst);                                        \
  }                                                                            \
  void emitTangentFor##INST##Inst(INST##Inst *(ID))

  CLONE_AND_EMIT_TANGENT(BeginBorrow, bbi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = bbi->getLoc();
    auto tanVal = materializeTangent(getTangentValue(bbi->getOperand()), loc);
    auto tanValBorrow = diffBuilder.emitBeginBorrowOperation(loc, tanVal);
    setTangentValue(bbi->getParent(), bbi,
                    makeConcreteTangentValue(tanValBorrow));
  }

  CLONE_AND_EMIT_TANGENT(EndBorrow, ebi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = ebi->getLoc();
    auto tanVal = materializeTangent(getTangentValue(ebi->getOperand()), loc);
    diffBuilder.emitEndBorrowOperation(loc, tanVal);
  }

  CLONE_AND_EMIT_TANGENT(DestroyValue, dvi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = dvi->getLoc();
    auto tanVal = materializeTangent(getTangentValue(dvi->getOperand()), loc);
    diffBuilder.emitDestroyValueOperation(loc, tanVal);
  }

  CLONE_AND_EMIT_TANGENT(CopyValue, cvi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto tan = getTangentValue(cvi->getOperand());
    auto tanVal = materializeTangent(tan, cvi->getLoc());
    auto tanValCopy = diffBuilder.emitCopyValueOperation(cvi->getLoc(), tanVal);
    setTangentValue(cvi->getParent(), cvi,
                    makeConcreteTangentValue(tanValCopy));
  }

  /// Handle `load` instruction.
  ///   Original: y = load x
  ///    Tangent: tan[y] = load tan[x]
  void visitLoadInst(LoadInst *li) {
    TypeSubstCloner::visitLoadInst(li);
    // If an active buffer is loaded with take to a non-active value, destroy
    // the active buffer's tangent buffer.
    if (!differentialInfo.shouldDifferentiateInstruction(li)) {
      auto isTake =
          (li->getOwnershipQualifier() == LoadOwnershipQualifier::Take);
      if (isTake && activityInfo.isActive(li->getOperand(), getConfig())) {
        auto &tanBuf = getTangentBuffer(li->getParent(), li->getOperand());
        getDifferentialBuilder().emitDestroyOperation(tanBuf.getLoc(), tanBuf);
      }
      return;
    }
    // Otherwise, do standard differential cloning.
    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = li->getParent();
    auto loc = li->getLoc();
    auto tanBuf = getTangentBuffer(bb, li->getOperand());
    auto tanVal = diffBuilder.emitLoadValueOperation(
        loc, tanBuf, li->getOwnershipQualifier());
    setTangentValue(bb, li, makeConcreteTangentValue(tanVal));
  }

  /// Handle `load_borrow` instruction.
  ///   Original: y = load_borrow x
  ///    Tangent: tan[y] = load_borrow tan[x]
  CLONE_AND_EMIT_TANGENT(LoadBorrow, lbi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = lbi->getParent();
    auto loc = lbi->getLoc();
    auto tanBuf = getTangentBuffer(bb, lbi->getOperand());
    auto tanVal = diffBuilder.emitLoadBorrowOperation(loc, tanBuf);
    setTangentValue(bb, lbi, makeConcreteTangentValue(tanVal));
  }

  /// Handle `store` instruction in the differential.
  ///   Original: store x to y
  ///     Tangent: store tan[x] to tan[y]
  void visitStoreInst(StoreInst *si) {
    TypeSubstCloner::visitStoreInst(si);
    // If a non-active value is stored into an active buffer, zero-initialize
    // the active buffer's tangent buffer.
    if (!differentialInfo.shouldDifferentiateInstruction(si)) {
      if (activityInfo.isActive(si->getDest(), getConfig())) {
        auto &tanBufDest = getTangentBuffer(si->getParent(), si->getDest());
        emitZeroIndirect(tanBufDest->getType().getASTType(), tanBufDest,
                         tanBufDest.getLoc());
      }
      return;
    }
    // Otherwise, do standard differential cloning.
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = si->getLoc();
    auto tanValSrc = materializeTangent(getTangentValue(si->getSrc()), loc);
    auto &tanValDest = getTangentBuffer(si->getParent(), si->getDest());
    diffBuilder.emitStoreValueOperation(loc, tanValSrc, tanValDest,
                                        si->getOwnershipQualifier());
  }

  /// Handle `store_borrow` instruction in the differential.
  ///   Original: store_borrow x to y
  ///    Tangent: store_borrow tan[x] to tan[y]
  void visitStoreBorrowInst(StoreBorrowInst *sbi) {
    TypeSubstCloner::visitStoreBorrowInst(sbi);
    // If a non-active value is stored into an active buffer, zero-initialize
    // the active buffer's tangent buffer.
    if (!differentialInfo.shouldDifferentiateInstruction(sbi)) {
      if (activityInfo.isActive(sbi->getDest(), getConfig())) {
        auto &tanBufDest = getTangentBuffer(sbi->getParent(), sbi->getDest());
        emitZeroIndirect(tanBufDest->getType().getASTType(), tanBufDest,
                         tanBufDest.getLoc());
      }
      return;
    }
    // Otherwise, do standard differential cloning.
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = sbi->getLoc();
    auto tanValSrc = materializeTangent(getTangentValue(sbi->getSrc()), loc);
    auto &tanValDest = getTangentBuffer(sbi->getParent(), sbi->getDest());
    diffBuilder.createStoreBorrow(loc, tanValSrc, tanValDest);
  }

  /// Handle `copy_addr` instruction.
  ///   Original: copy_addr x to y
  ///    Tangent: copy_addr tan[x] to tan[y]
  void visitCopyAddrInst(CopyAddrInst *cai) {
    TypeSubstCloner::visitCopyAddrInst(cai);
    // If a non-active buffer is copied into an active buffer, zero-initialize
    // the destination buffer's tangent buffer.
    // If an active buffer is copied with take into a non-active buffer, destroy
    // the source buffer's tangent buffer.
    if (!differentialInfo.shouldDifferentiateInstruction(cai)) {
      if (activityInfo.isActive(cai->getDest(), getConfig())) {
        auto &tanBufDest = getTangentBuffer(cai->getParent(), cai->getDest());
        emitZeroIndirect(tanBufDest->getType().getASTType(), tanBufDest,
                         tanBufDest.getLoc());
      }
      if (cai->isTakeOfSrc() &&
          activityInfo.isActive(cai->getSrc(), getConfig())) {
        auto &tanBufSrc = getTangentBuffer(cai->getParent(), cai->getSrc());
        getDifferentialBuilder().emitDestroyOperation(tanBufSrc.getLoc(),
                                                      tanBufSrc);
      }
      return;
    }
    // Otherwise, do standard differential cloning.
    auto diffBuilder = getDifferentialBuilder();
    auto loc = cai->getLoc();
    auto *bb = cai->getParent();
    auto &tanSrc = getTangentBuffer(bb, cai->getSrc());
    auto tanDest = getTangentBuffer(bb, cai->getDest());
    diffBuilder.createCopyAddr(loc, tanSrc, tanDest, cai->isTakeOfSrc(),
                               cai->isInitializationOfDest());
  }

  /// Handle `unconditional_checked_cast_addr` instruction.
  ///   Original: unconditional_checked_cast_addr $X in x to $Y in y
  ///    Tangent: unconditional_checked_cast_addr $X.Tan in tan[x]
  ///                                          to $Y.Tan in tan[y]
  CLONE_AND_EMIT_TANGENT(UnconditionalCheckedCastAddr, uccai) {
    auto diffBuilder = getDifferentialBuilder();
    auto loc = uccai->getLoc();
    auto *bb = uccai->getParent();
    auto &tanSrc = getTangentBuffer(bb, uccai->getSrc());
    auto tanDest = getTangentBuffer(bb, uccai->getDest());

    diffBuilder.createUnconditionalCheckedCastAddr(
        loc, tanSrc, tanSrc->getType().getASTType(), tanDest,
        tanDest->getType().getASTType());
  }

  /// Handle `begin_access` instruction (and do differentiability checks).
  ///   Original: y = begin_access x
  ///    Tangent: tan[y] = begin_access tan[x]
  CLONE_AND_EMIT_TANGENT(BeginAccess, bai) {
    // Check for non-differentiable writes.
    if (bai->getAccessKind() == SILAccessKind::Modify) {
      if (auto *gai = dyn_cast<GlobalAddrInst>(bai->getSource())) {
        context.emitNondifferentiabilityError(
            bai, invoker,
            diag::autodiff_cannot_differentiate_writes_to_global_variables);
        errorOccurred = true;
        return;
      }
      if (auto *pbi = dyn_cast<ProjectBoxInst>(bai->getSource())) {
        context.emitNondifferentiabilityError(
            bai, invoker,
            diag::autodiff_cannot_differentiate_writes_to_mutable_captures);
        errorOccurred = true;
        return;
      }
    }

    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = bai->getParent();

    auto tanSrc = getTangentBuffer(bb, bai->getSource());
    auto *tanDest = diffBuilder.createBeginAccess(
        bai->getLoc(), tanSrc, bai->getAccessKind(), bai->getEnforcement(),
        bai->hasNoNestedConflict(), bai->isFromBuiltin());
    setTangentBuffer(bb, bai, tanDest);
  }

  /// Handle `end_access` instruction.
  ///   Original: begin_access x
  ///    Tangent: end_access tan[x]
  CLONE_AND_EMIT_TANGENT(EndAccess, eai) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = eai->getParent();
    auto loc = eai->getLoc();
    auto tanOperand = getTangentBuffer(bb, eai->getOperand());
    diffBuilder.createEndAccess(loc, tanOperand, eai->isAborting());
  }

  /// Handle `alloc_stack` instruction.
  ///   Original: y = alloc_stack $T
  ///    Tangent: tan[y] = alloc_stack $T.Tangent
  CLONE_AND_EMIT_TANGENT(AllocStack, asi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *mappedAllocStackInst = diffBuilder.createAllocStack(
        asi->getLoc(), getRemappedTangentType(asi->getElementType()),
        asi->getVarInfo());
    setTangentBuffer(asi->getParent(), asi, mappedAllocStackInst);
  }

  /// Handle `dealloc_stack` instruction.
  ///   Original: dealloc_stack x
  ///    Tangent: dealloc_stack tan[x]
  CLONE_AND_EMIT_TANGENT(DeallocStack, dsi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto tanBuf = getTangentBuffer(dsi->getParent(), dsi->getOperand());
    diffBuilder.createDeallocStack(dsi->getLoc(), tanBuf);
  }

  /// Handle `destroy_addr` instruction.
  ///   Original: destroy_addr x
  ///    Tangent: destroy_addr tan[x]
  CLONE_AND_EMIT_TANGENT(DestroyAddr, dai) {
    auto &diffBuilder = getDifferentialBuilder();
    auto tanBuf = getTangentBuffer(dai->getParent(), dai->getOperand());
    diffBuilder.createDestroyAddr(dai->getLoc(), tanBuf);
  }

  /// Handle `struct` instruction.
  ///   Original: y = struct $T (x0, x1, x2, ...)
  ///    Tangent: tan[y] = struct $T.Tangent (tan[x0], tan[x1], tan[x2], ...)
  CLONE_AND_EMIT_TANGENT(Struct, si) {
    auto &diffBuilder = getDifferentialBuilder();
    SmallVector<SILValue, 4> tangentElements;
    for (auto elem : si->getElements())
      tangentElements.push_back(getTangentValue(elem).getConcreteValue());
    auto tanExtract = diffBuilder.createStruct(
        si->getLoc(), getRemappedTangentType(si->getType()), tangentElements);
    setTangentValue(si->getParent(), si, makeConcreteTangentValue(tanExtract));
  }

  /// Handle `struct_extract` instruction.
  ///   Original: y = struct_extract x, #field
  ///    Tangent: tan[y] = struct_extract tan[x], #field'
  ///                                             ^~~~~~~
  ///                          field in tangent space corresponding to #field
  CLONE_AND_EMIT_TANGENT(StructExtract, sei) {
    assert(!sei->getField()->getAttrs().hasAttribute<NoDerivativeAttr>() &&
           "`struct_extract` with `@noDerivative` field should not be "
           "differentiated; activity analysis should not marked as varied.");
    auto diffBuilder = getDifferentialBuilder();
    auto loc = getValidLocation(sei);
    // Find the corresponding field in the tangent space.
    auto structType =
        remapSILTypeInDifferential(sei->getOperand()->getType()).getASTType();
    auto *tanField =
      getTangentStoredProperty(context, sei, structType, invoker);
    if (!tanField) {
      errorOccurred = true;
      return;
    }
    // Emit tangent `struct_extract`.
    auto tanStruct =
        materializeTangent(getTangentValue(sei->getOperand()), loc);
    auto tangentInst =
        diffBuilder.createStructExtract(loc, tanStruct, tanField);
    // Update tangent value mapping for `struct_extract` result.
    auto tangentResult = makeConcreteTangentValue(tangentInst);
    setTangentValue(sei->getParent(), sei, tangentResult);
  }

  /// Handle `struct_element_addr` instruction.
  ///   Original: y = struct_element_addr x, #field
  ///    Tangent: tan[y] = struct_element_addr tan[x], #field'
  ///                                                  ^~~~~~~
  ///                          field in tangent space corresponding to #field
  CLONE_AND_EMIT_TANGENT(StructElementAddr, seai) {
    assert(!seai->getField()->getAttrs().hasAttribute<NoDerivativeAttr>() &&
           "`struct_element_addr` with `@noDerivative` field should not be "
           "differentiated; activity analysis should not marked as varied.");
    auto diffBuilder = getDifferentialBuilder();
    auto *bb = seai->getParent();
    auto loc = getValidLocation(seai);
    // Find the corresponding field in the tangent space.
    auto structType =
        remapSILTypeInDifferential(seai->getOperand()->getType()).getASTType();
    auto *tanField =
      getTangentStoredProperty(context, seai, structType, invoker);
    if (!tanField) {
      errorOccurred = true;
      return;
    }
    // Emit tangent `struct_element_addr`.
    auto tanOperand = getTangentBuffer(bb, seai->getOperand());
    auto tangentInst =
        diffBuilder.createStructElementAddr(loc, tanOperand, tanField);
    // Update tangent buffer map for `struct_element_addr`.
    setTangentBuffer(bb, seai, tangentInst);
  }

  /// Handle `tuple` instruction.
  ///   Original: y = tuple (x0, x1, x2, ...)
  ///    Tangent: tan[y] = tuple (tan[x0], tan[x1], tan[x2], ...)
  ///                                                        ^~~
  ///                                      excluding non-differentiable elements
  CLONE_AND_EMIT_TANGENT(Tuple, ti) {
    auto diffBuilder = getDifferentialBuilder();
    // Get the tangents of all the tuple elements.
    SmallVector<SILValue, 8> tangentTupleElements;
    for (auto elem : ti->getElements()) {
      if (!getTangentSpace(elem->getType().getASTType()))
        continue;
      tangentTupleElements.push_back(
          materializeTangent(getTangentValue(elem), ti->getLoc()));
    }
    // Emit the instruction and add the tangent mapping.
    auto tanTuple =
        joinElements(tangentTupleElements, diffBuilder, ti->getLoc());
    setTangentValue(ti->getParent(), ti, makeConcreteTangentValue(tanTuple));
  }

  /// Handle `tuple_extract` instruction.
  ///   Original: y = tuple_extract x, <n>
  ///    Tangent: tan[y] = tuple_extract tan[x], <n'>
  ///                                            ^~~~
  ///                         tuple tangent space index corresponding to n
  CLONE_AND_EMIT_TANGENT(TupleExtract, tei) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = tei->getLoc();
    auto origTupleTy = tei->getOperand()->getType().castTo<TupleType>();
    unsigned tanIndex = 0;
    for (unsigned i : range(tei->getFieldIndex())) {
      if (getTangentSpace(
              origTupleTy->getElement(i).getType()->getCanonicalType()))
        ++tanIndex;
    }
    auto tanType = getRemappedTangentType(tei->getType());
    auto tanSource =
        materializeTangent(getTangentValue(tei->getOperand()), loc);
    // If the tangent value of the source does not have a tuple type, then
    // it must represent a "single element tuple type". Use it directly.
    if (!tanSource->getType().is<TupleType>()) {
      setTangentValue(tei->getParent(), tei,
                      makeConcreteTangentValue(tanSource));
    } else {
      auto tanElt =
          diffBuilder.createTupleExtract(loc, tanSource, tanIndex, tanType);
      setTangentValue(tei->getParent(), tei, makeConcreteTangentValue(tanElt));
    }
  }

  /// Handle `tuple_element_addr` instruction.
  ///   Original: y = tuple_element_addr x, <n>
  ///    Tangent: tan[y] = tuple_element_addr tan[x], <n'>
  ///                                                ^~~~
  ///                            tuple tangent space index corresponding to n
  CLONE_AND_EMIT_TANGENT(TupleElementAddr, teai) {
    auto &diffBuilder = getDifferentialBuilder();
    auto origTupleTy = teai->getOperand()->getType().castTo<TupleType>();
    unsigned tanIndex = 0;
    for (unsigned i : range(teai->getFieldIndex())) {
      if (getTangentSpace(
              origTupleTy->getElement(i).getType()->getCanonicalType()))
        ++tanIndex;
    }
    auto tanType = getRemappedTangentType(teai->getType());
    auto tanSource = getTangentBuffer(teai->getParent(), teai->getOperand());
    SILValue tanBuf;
    // If the tangent buffer of the source does not have a tuple type, then
    // it must represent a "single element tuple type". Use it directly.
    if (!tanSource->getType().is<TupleType>()) {
      tanBuf = tanSource;
    } else {
      tanBuf = diffBuilder.createTupleElementAddr(teai->getLoc(), tanSource,
                                                  tanIndex, tanType);
    }
    setTangentBuffer(teai->getParent(), teai, tanBuf);
  }

  /// Handle `destructure_tuple` instruction.
  ///   Original: (y0, y1, ...)  = destructure_tuple x, <n>
  ///    Tangent: (tan[y0], tan[y1], ...) = destructure_tuple tan[x], <n'>
  ///                                                                 ^~~~
  ///                              tuple tangent space index corresponding to n
  CLONE_AND_EMIT_TANGENT(DestructureTuple, dti) {
    assert(llvm::any_of(dti->getResults(),
                        [&](SILValue elt) {
                          return activityInfo.isActive(elt, getConfig());
                        }) &&
           "`destructure_tuple` should have at least one active result");

    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = dti->getParent();
    auto loc = dti->getLoc();

    auto tanTuple = materializeTangent(getTangentValue(dti->getOperand()), loc);
    SmallVector<SILValue, 4> tanElts;
    if (tanTuple->getType().is<TupleType>()) {
      auto *tanDti = diffBuilder.createDestructureTuple(loc, tanTuple);
      tanElts.append(tanDti->getResults().begin(), tanDti->getResults().end());
    } else {
      tanElts.push_back(tanTuple);
    }
    unsigned tanIdx = 0;
    for (auto i : range(dti->getNumResults())) {
      auto origElt = dti->getResult(i);
      if (!getTangentSpace(origElt->getType().getASTType()))
        continue;
      setTangentValue(bb, origElt, makeConcreteTangentValue(tanElts[tanIdx++]));
    }
  }

#undef CLONE_AND_EMIT_TANGENT

  /// Handle `apply` instruction, given:
  /// - The minimal indices for differentiating the `apply`.
  /// - The original non-reabstracted differential type.
  ///
  ///   Original: y = apply f(x0, x1, ...)
  ///    Tangent: tan[y] = apply diff_f(tan[x0], tan[x1], ...)
  void emitTangentForApplyInst(ApplyInst *ai, const AutoDiffConfig &applyConfig,
                               CanSILFunctionType originalDifferentialType) {
    assert(differentialInfo.shouldDifferentiateApplySite(ai));
    auto *bb = ai->getParent();
    auto loc = ai->getLoc();
    auto &diffBuilder = getDifferentialBuilder();

    // Get the differential value.
    SILValue differential = getDifferentialTupleElement(ai);
    auto differentialType = remapSILTypeInDifferential(differential->getType())
                                .castTo<SILFunctionType>();

    // Get the differential arguments.
    SmallVector<SILValue, 8> diffArgs;

    for (auto indRes : ai->getIndirectSILResults())
      diffArgs.push_back(getTangentBuffer(bb, indRes));

    auto origArgs = ai->getArgumentsWithoutIndirectResults();
    // Get the tangent value of the original arguments.
    for (auto i : indices(origArgs)) {
      auto origArg = origArgs[i];
      // If the argument is not active:
      // - Skip the element, if it is not differentiable.
      // - Otherwise, add a zero value to that location.
      if (!activityInfo.isActive(origArg, getConfig())) {
        auto origCalleeType = ai->getSubstCalleeType();
        if (!origCalleeType->isDifferentiable())
          continue;
        auto actualOrigCalleeIndices =
            origCalleeType->getDifferentiabilityParameterIndices();
        if (actualOrigCalleeIndices->contains(i)) {
          SILValue tanParam;
          if (origArg->getType().isObject()) {
            tanParam = emitZeroDirect(
                getRemappedTangentType(origArg->getType()).getASTType(), loc);
            diffArgs.push_back(tanParam);
          } else {
            tanParam = diffBuilder.createAllocStack(
                loc, getRemappedTangentType(origArg->getType()));
            emitZeroIndirect(
                getRemappedTangentType(origArg->getType()).getASTType(),
                tanParam, loc);
          }
        }
      }
      // Otherwise, if the argument is active, handle the argument normally by
      // getting its tangent value.
      else {
        SILValue tanParam;
        if (origArg->getType().isObject()) {
          tanParam = materializeTangent(getTangentValue(origArg), loc);
        } else {
          tanParam = getTangentBuffer(ai->getParent(), origArg);
        }
        diffArgs.push_back(tanParam);
        if (errorOccurred)
          return;
      }
    }

    // If callee differential was reabstracted in JVP, reabstract the callee
    // differential.
    if (!differentialType->isEqual(originalDifferentialType)) {
      SILOptFunctionBuilder fb(context.getTransform());
      differential = reabstractFunction(
          diffBuilder, fb, loc, differential, originalDifferentialType,
          [this](SubstitutionMap subs) -> SubstitutionMap {
            return this->getOpSubstitutionMap(subs);
          });
    }

    // Call the differential.
    auto *differentialCall =
        diffBuilder.createApply(loc, differential, SubstitutionMap(), diffArgs);
    diffBuilder.emitDestroyValueOperation(loc, differential);

    // Get the original `apply` results.
    SmallVector<SILValue, 8> origDirectResults;
    forEachApplyDirectResult(ai, [&](SILValue directResult) {
      origDirectResults.push_back(directResult);
    });
    SmallVector<SILValue, 8> origAllResults;
    collectAllActualResultsInTypeOrder(ai, origDirectResults, origAllResults);

    // Get the callee differential `apply` results.
    SmallVector<SILValue, 8> differentialDirectResults;
    extractAllElements(differentialCall, getDifferentialBuilder(),
                       differentialDirectResults);
    SmallVector<SILValue, 8> differentialAllResults;
    collectAllActualResultsInTypeOrder(
        differentialCall, differentialDirectResults, differentialAllResults);
    for (auto inoutArg : ai->getInoutArguments())
      origAllResults.push_back(inoutArg);
    for (auto inoutArg : differentialCall->getInoutArguments())
      differentialAllResults.push_back(inoutArg);
    assert(applyConfig.resultIndices->getNumIndices() ==
           differentialAllResults.size());

    // Set tangent values for original `apply` results.
    unsigned differentialResultIndex = 0;
    for (auto resultIndex : applyConfig.resultIndices->getIndices()) {
      auto origResult = origAllResults[resultIndex];
      auto differentialResult =
          differentialAllResults[differentialResultIndex++];
      if (origResult->getType().isObject()) {
        if (!origResult->getType().is<TupleType>()) {
          setTangentValue(bb, origResult,
                          makeConcreteTangentValue(differentialResult));
        } else if (auto *dti = getSingleDestructureTupleUser(ai)) {
          bool notSetValue = true;
          for (auto result : dti->getResults()) {
            if (activityInfo.isActive(result, getConfig())) {
              assert(notSetValue &&
                     "This was incorrectly set, should only have one active "
                     "result from the tuple.");
              notSetValue = false;
              setTangentValue(bb, result,
                              makeConcreteTangentValue(differentialResult));
            }
          }
        }
      }
    }
  }

  /// Generate a `return` instruction in the current differential basic block.
  void emitReturnInstForDifferential() {
    auto &differential = getDifferential();
    auto diffLoc = differential.getLocation();
    auto &diffBuilder = getDifferentialBuilder();

    // Collect original results.
    SmallVector<SILValue, 2> originalResults;
    collectAllDirectResultsInTypeOrder(*original, originalResults);
    // Collect differential direct results.
    SmallVector<SILValue, 8> retElts;
    for (auto i : range(originalResults.size())) {
      auto origResult = originalResults[i];
      if (!getConfig().resultIndices->contains(i))
        continue;
      auto tanVal = materializeTangent(getTangentValue(origResult), diffLoc);
      retElts.push_back(tanVal);
    }

    diffBuilder.createReturn(diffLoc,
                             joinElements(retElts, diffBuilder, diffLoc));
  }
};

//--------------------------------------------------------------------------//
// Initialization
//--------------------------------------------------------------------------//

/// Initialization helper function.
///
/// Returns the substitution map used for type remapping.
static SubstitutionMap getSubstitutionMap(SILFunction *original,
                                          SILFunction *jvp) {
  auto substMap = original->getForwardingSubstitutionMap();
  if (auto *jvpGenEnv = jvp->getGenericEnvironment()) {
    auto jvpSubstMap = jvpGenEnv->getForwardingSubstitutionMap();
    substMap = SubstitutionMap::get(
        jvpGenEnv->getGenericSignature(), QuerySubstitutionMap{jvpSubstMap},
        LookUpConformanceInSubstitutionMap(jvpSubstMap));
  }
  return substMap;
}

/// Initialization helper function.
///
/// Returns the activity info for the given original function, autodiff indices,
/// and JVP generic signature.
static const DifferentiableActivityInfo &
getActivityInfo(ADContext &context, SILFunction *original,
                const AutoDiffConfig &config, SILFunction *jvp) {
  // Get activity info of the original function.
  auto &passManager = context.getPassManager();
  auto *activityAnalysis =
      passManager.getAnalysis<DifferentiableActivityAnalysis>();
  auto &activityCollection = *activityAnalysis->get(original);
  auto &activityInfo = activityCollection.getActivityInfo(
      jvp->getLoweredFunctionType()->getSubstGenericSignature(),
      AutoDiffDerivativeFunctionKind::JVP);
  LLVM_DEBUG(activityInfo.dump(config, getADDebugStream()));
  return activityInfo;
}

JVPCloner::Implementation::Implementation(ADContext &context,
                                          SILDifferentiabilityWitness *witness,
                                          SILFunction *jvp,
                                          DifferentiationInvoker invoker)
    : TypeSubstCloner(*jvp, *witness->getOriginalFunction(),
                      getSubstitutionMap(witness->getOriginalFunction(), jvp)),
      context(context), original(witness->getOriginalFunction()),
      witness(witness), jvp(jvp), invoker(invoker),
      activityInfo(
          getActivityInfo(context, original, witness->getConfig(), jvp)),
      loopInfo(context.getPassManager().getAnalysis<SILLoopAnalysis>()
                   ->get(original)),
      differentialInfo(context, AutoDiffLinearMapKind::Differential, original,
                       jvp, witness->getConfig(), activityInfo, loopInfo),
      differentialBuilder(TangentBuilder(
          *createEmptyDifferential(context, witness, &differentialInfo),
          context)),
      diffLocalAllocBuilder(getDifferential(), context) {
  // Create empty differential function.
  context.recordGeneratedFunction(&getDifferential());
}

JVPCloner::JVPCloner(ADContext &context, SILDifferentiabilityWitness *witness,
                     SILFunction *jvp, DifferentiationInvoker invoker)
    : impl(*new Implementation(context, witness, jvp, invoker)) {}

JVPCloner::~JVPCloner() { delete &impl; }

//--------------------------------------------------------------------------//
// Differential struct mapping
//--------------------------------------------------------------------------//

void JVPCloner::Implementation::initializeDifferentialTupleElements(
  SILBasicBlock *origBB, SILInstructionResultArray values) {
  auto *diffTupleTyple = differentialInfo.getLinearMapTupleType(origBB);
  assert(diffTupleTyple->getNumElements() == values.size() &&
         "The number of differential tuple fields must equal the number of "
         "differential struct element values");
  auto res = differentialTupleElements.insert({origBB, values});
  (void)res;
  assert(res.second && "A pullback struct element already exists!");
}

/// Returns the differential tuple element value corresponding to the given
/// original block and apply inst.
SILValue JVPCloner::Implementation::getDifferentialTupleElement(ApplyInst *ai) {
  unsigned idx = differentialInfo.lookUpLinearMapIndex(ai);
    assert((idx > 0 || (idx == 0 && ai->getParentBlock()->isEntry())) &&
           "impossible linear map index");
  auto values = differentialTupleElements.lookup(ai->getParentBlock());
  assert(idx < values.size() &&
         "differential tuple element for this apply does not exist!");
  return values[idx];
}

//--------------------------------------------------------------------------//
// Tangent emission helpers
//--------------------------------------------------------------------------//

void JVPCloner::Implementation::prepareForDifferentialGeneration() {
  // Create differential blocks and arguments.
  auto &differential = getDifferential();
  auto diffLoc = differential.getLocation();
  auto *origEntry = original->getEntryBlock();
  auto origFnTy = original->getLoweredFunctionType();

  for (auto &origBB : *original) {
    auto *diffBB = differential.createBasicBlock();
    diffBBMap.insert({&origBB, diffBB});
    // If the BB is the original entry, then the differential block that we
    // just created must be the differential function's entry. Create
    // differential entry arguments and continue.
    if (&origBB == origEntry) {
      assert(diffBB->isEntry());
      createEntryArguments(&differential);
      auto *lastArg = diffBB->getArguments().back();
#ifndef NDEBUG
      auto diffTupleLoweredType = remapSILTypeInDifferential(
          differentialInfo.getLinearMapTupleLoweredType(&origBB));
      assert(lastArg->getType() == diffTupleLoweredType);
#endif
      differentialStructArguments[&origBB] = lastArg;
    }

    LLVM_DEBUG({
      auto &s = getADDebugStream()
                << "Original bb" + std::to_string(origBB.getDebugID())
                << ": To differentiate or not to differentiate?\n";
      for (auto &inst : origBB) {
        s << (differentialInfo.shouldDifferentiateInstruction(&inst) ? "[x] "
                                                                     : "[ ] ")
          << inst;
      }
    });
  }

  assert(diffBBMap.size() == 1 &&
         "Can only currently handle single basic block functions");

  // The differential function has type:
  // (arg0', ..., argn', entry_df_struct) -> result'.
  auto diffParamArgs =
      differential.getArgumentsWithoutIndirectResults().drop_back();
  assert(diffParamArgs.size() ==
         witness->getConfig().parameterIndices->getNumIndices());
  auto origParamArgs = original->getArgumentsWithoutIndirectResults();

  // TODO(TF-788): Re-enable non-varied result warning.
  /*
  // Check if result is not varied.
  SmallVector<SILValue, 8> origFormalResults;
  collectAllFormalResultsInTypeOrder(*original, origFormalResults);
   std::get<0>(pair);
  for (auto resultIndex : getConfig().results->getIndices()) {
    auto origResult = origFormalResults[resultIndex];
    // Emit warning if original result is not varied, because it will always
    // have a zero derivative.
    if (!activityInfo.isVaried(origResult, getConfig().parameters)) {
      // Emit fixit if original result has a valid source location.
      auto startLoc = origResult.getLoc().getStartSourceLoc();
      auto endLoc = origResult.getLoc().getEndSourceLoc();
      if (startLoc.isValid() && endLoc.isValid()) {
        context.diagnose(startLoc, diag::autodiff_nonvaried_result_fixit)
            .fixItInsert(startLoc, "withoutDerivative(at:")
            .fixItInsertAfter(endLoc, ")");
      }
    }
  }
  */

  // Initialize tangent mapping for parameters.
  auto diffParamsIt = getConfig().parameterIndices->begin();
  for (auto index : range(diffParamArgs.size())) {
    auto *diffArg = diffParamArgs[index];
    auto *origArg = origParamArgs[*diffParamsIt];
    ++diffParamsIt;
    if (diffArg->getType().isAddress()) {
      setTangentBuffer(origEntry, origArg, diffArg);
    } else {
      setTangentValue(origEntry, origArg, makeConcreteTangentValue(diffArg));
    }
    LLVM_DEBUG(getADDebugStream()
               << "Assigned parameter " << *diffArg
               << " as the tangent of original result " << *origArg);
  }

  // Initialize tangent mapping for original indirect results and non-wrt
  // `inout` parameters. The tangent buffers of these address values are
  // differential indirect results.

  // Collect original results.
  SmallVector<SILValue, 2> originalResults;
  collectAllFormalResultsInTypeOrder(*original, originalResults);

  // Iterate over differentiability results.
  differentialBuilder.setInsertionPoint(differential.getEntryBlock());
  auto diffIndResults = differential.getIndirectResults();
  unsigned differentialIndirectResultIndex = 0;
  for (auto resultIndex : getConfig().resultIndices->getIndices()) {
    auto origResult = originalResults[resultIndex];
    // Handle original formal indirect result.
    if (resultIndex < origFnTy->getNumResults()) {
      // Skip original direct results.
      if (origResult->getType().isObject())
        continue;
      auto diffIndResult = diffIndResults[differentialIndirectResultIndex++];
      setTangentBuffer(origEntry, origResult, diffIndResult);
      // If original indirect result is non-varied, zero-initialize its tangent
      // buffer.
      if (!activityInfo.isVaried(origResult, getConfig().parameterIndices))
        emitZeroIndirect(diffIndResult->getType().getASTType(), diffIndResult,
                         diffLoc);
      continue;
    }
    // Handle original non-wrt `inout` parameter.
    // Only original *non-wrt* `inout` parameters have corresponding
    // differential indirect results.
    auto inoutParamIndex = resultIndex - origFnTy->getNumResults();
    auto inoutParamIt = std::next(
        origFnTy->getIndirectMutatingParameters().begin(), inoutParamIndex);
    auto paramIndex =
        std::distance(origFnTy->getParameters().begin(), &*inoutParamIt);
    if (getConfig().parameterIndices->contains(paramIndex))
      continue;
    auto diffIndResult = diffIndResults[differentialIndirectResultIndex++];
    setTangentBuffer(origEntry, origResult, diffIndResult);
    // Original `inout` parameters are initialized, so their tangent buffers
    // must also be initialized.
    emitZeroIndirect(diffIndResult->getType().getASTType(), diffIndResult,
                     diffLoc);
  }
}

/*static*/ SILFunction *JVPCloner::Implementation::createEmptyDifferential(
    ADContext &context, SILDifferentiabilityWitness *witness,
    LinearMapInfo *linearMapInfo) {
  auto &module = context.getModule();
  auto *original = witness->getOriginalFunction();
  auto *jvp = witness->getJVP();
  auto origTy = original->getLoweredFunctionType();
  // Get witness generic signature for remapping types.
  // Witness generic signature may have more requirements than JVP generic
  // signature: when witness generic signature has same-type requirements
  // binding all generic parameters to concrete types, JVP function type uses
  // all the concrete types and JVP generic signature is null.
  auto witnessCanGenSig = witness->getDerivativeGenericSignature().getCanonicalSignature();
  auto lookupConformance = LookUpConformanceInModule(module.getSwiftModule());

  // Parameters of the differential are:
  // - the tangent values of the wrt parameters.
  // - the differential struct for the original entry.
  // Result of the differential is in the tangent space of the original
  // result.
  SmallVector<SILParameterInfo, 8> dfParams;
  SmallVector<SILResultInfo, 8> dfResults;
  auto origParams = origTy->getParameters();
  auto config = witness->getConfig();

  for (auto resultIndex : config.resultIndices->getIndices()) {
    if (resultIndex < origTy->getNumResults()) {
      // Handle formal original result.
      auto origResult = origTy->getResults()[resultIndex];
      origResult = origResult.getWithInterfaceType(
          origResult.getInterfaceType()->getReducedType(witnessCanGenSig));
      dfResults.push_back(
          SILResultInfo(origResult.getInterfaceType()
                            ->getAutoDiffTangentSpace(lookupConformance)
                            ->getType()
                            ->getReducedType(witnessCanGenSig),
                        origResult.getConvention()));
    } else {
      // Handle original `inout` parameter.
      auto inoutParamIndex = resultIndex - origTy->getNumResults();
      auto inoutParamIt = std::next(
          origTy->getIndirectMutatingParameters().begin(), inoutParamIndex);
      auto paramIndex =
          std::distance(origTy->getParameters().begin(), &*inoutParamIt);
      // If the original `inout` parameter is a differentiability parameter,
      // then it already has a corresponding differential parameter. Do not add
      // a corresponding differential result.
      if (config.parameterIndices->contains(paramIndex))
        continue;
      auto inoutParam = origTy->getParameters()[paramIndex];
      auto paramTan = inoutParam.getInterfaceType()->getAutoDiffTangentSpace(
          lookupConformance);
      assert(paramTan && "Parameter type does not have a tangent space?");
      dfResults.push_back(
          {paramTan->getCanonicalType(), ResultConvention::Indirect});
    }
  }

  // Add differential parameters for the requested wrt parameters.
  for (auto i : config.parameterIndices->getIndices()) {
    auto origParam = origParams[i];
    origParam = origParam.getWithInterfaceType(
        origParam.getInterfaceType()->getReducedType(witnessCanGenSig));
    dfParams.push_back(
        SILParameterInfo(origParam.getInterfaceType()
                             ->getAutoDiffTangentSpace(lookupConformance)
                             ->getType()
                             ->getReducedType(witnessCanGenSig),
                         origParam.getConvention()));
  }

  // Accept a differential struct in the differential parameter list. This is
  // the returned differential's closure context.
  auto *origEntry = original->getEntryBlock();
  auto dfTupleType =
    linearMapInfo->getLinearMapTupleLoweredType(origEntry).getASTType();
  dfParams.push_back({dfTupleType, ParameterConvention::Direct_Owned});

  Mangle::DifferentiationMangler mangler;
  auto diffName = mangler.mangleLinearMap(
      witness->getOriginalFunction()->getName(),
      AutoDiffLinearMapKind::Differential, witness->getConfig());
  // Set differential generic signature equal to JVP generic signature.
  // Do not use witness generic signature, which may have same-type requirements
  // binding all generic parameters to concrete types.
  auto diffGenericSig =
      jvp->getLoweredFunctionType()->getSubstGenericSignature();
  auto *diffGenericEnv = diffGenericSig.getGenericEnvironment();
  auto diffType = SILFunctionType::get(
      diffGenericSig, SILExtInfo::getThin(), origTy->getCoroutineKind(),
      origTy->getCalleeConvention(), dfParams, {}, dfResults, llvm::None,
      origTy->getPatternSubstitutions(), origTy->getInvocationSubstitutions(),
      original->getASTContext());

  SILOptFunctionBuilder fb(context.getTransform());
  auto linkage = jvp->isSerialized() ? SILLinkage::Public : SILLinkage::Private;
  auto *differential = fb.createFunction(
      linkage, context.getASTContext().getIdentifier(diffName).str(), diffType,
      diffGenericEnv, original->getLocation(), original->isBare(),
      IsNotTransparent, jvp->isSerialized(),
      original->isDynamicallyReplaceable(),
      original->isDistributed(),
      original->isRuntimeAccessible());
  differential->setDebugScope(
      new (module) SILDebugScope(original->getLocation(), differential));

  return differential;
}

bool JVPCloner::Implementation::run() {
  PrettyStackTraceSILFunction trace("generating JVP and differential for",
                                    original);
  LLVM_DEBUG(getADDebugStream() << "Cloning original @" << original->getName()
                                << " to jvp @" << jvp->getName() << '\n');
  // Create JVP and differential entry and arguments.
  auto *entry = jvp->createBasicBlock();
  createEntryArguments(jvp);
  prepareForDifferentialGeneration();
  // Clone.
  SmallVector<SILValue, 4> entryArgs(entry->getArguments().begin(),
                                     entry->getArguments().end());
  cloneFunctionBody(original, entry, entryArgs);
  emitReturnInstForDifferential();
  // If errors occurred, back out.
  if (errorOccurred)
    return true;
  LLVM_DEBUG(getADDebugStream()
             << "Generated JVP for " << original->getName() << ":\n"
             << *jvp);
  LLVM_DEBUG(getADDebugStream()
             << "Generated differential for " << original->getName() << ":\n"
             << getDifferential());
  return errorOccurred;
}

} // end namespace autodiff
} // end namespace swift

bool JVPCloner::run() {
  bool foundError = impl.run();
#ifndef NDEBUG
  if (!foundError)
    getJVP().verify();
#endif
  return foundError;
}

SILFunction &JVPCloner::getJVP() const { return impl.getJVP(); }
