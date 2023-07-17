//===--- PullbackCloner.cpp - Pullback function generation ---*- C++ -*----===//
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
// This file defines a helper class for generating pullback functions for
// automatic differentiation.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/PullbackCloner.h"
#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Differentiation/ADContext.h"
#include "swift/SILOptimizer/Differentiation/AdjointValue.h"
#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"
#include "swift/SILOptimizer/Differentiation/LinearMapInfo.h"
#include "swift/SILOptimizer/Differentiation/Thunk.h"
#include "swift/SILOptimizer/Differentiation/VJPCloner.h"

#include "swift/AST/Expr.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/PassManager/PrettyStackTrace.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"

namespace swift {

class SILDifferentiabilityWitness;
class SILBasicBlock;
class SILFunction;
class SILInstruction;

namespace autodiff {

class ADContext;
class VJPCloner;

/// The implementation class for `PullbackCloner`.
///
/// The implementation class is a `SILInstructionVisitor`. Effectively, it acts
/// as a `SILCloner` that visits basic blocks in post-order and that visits
/// instructions per basic block in reverse order. This visitation order is
/// necessary for generating pullback functions, whose control flow graph is
/// ~a transposed version of the original function's control flow graph.
class PullbackCloner::Implementation final
    : public SILInstructionVisitor<PullbackCloner::Implementation> {

public:
  explicit Implementation(VJPCloner &vjpCloner);

private:
  /// The parent VJP cloner.
  VJPCloner &vjpCloner;

  /// Dominance info for the original function.
  DominanceInfo *domInfo = nullptr;

  /// Post-dominance info for the original function.
  PostDominanceInfo *postDomInfo = nullptr;

  /// Post-order info for the original function.
  PostOrderFunctionInfo *postOrderInfo = nullptr;

  /// Mapping from original basic blocks to corresponding pullback basic blocks.
  /// Pullback basic blocks always have the predecessor as the single argument.
  llvm::DenseMap<SILBasicBlock *, SILBasicBlock *> pullbackBBMap;

  /// Mapping from original basic blocks and original values to corresponding
  /// adjoint values.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILValue>, AdjointValue> valueMap;

  /// Mapping from original basic blocks and original values to corresponding
  /// adjoint buffers.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILValue>, SILValue> bufferMap;

  /// Mapping from pullback struct field declarations to pullback struct
  /// elements destructured from the linear map basic block argument. In the
  /// beginning of each pullback basic block, the block's pullback struct is
  /// destructured into individual elements stored here.
  llvm::DenseMap<SILBasicBlock*, SmallVector<SILValue, 4>> pullbackTupleElements;

  /// Mapping from original basic blocks and successor basic blocks to
  /// corresponding pullback trampoline basic blocks. Trampoline basic blocks
  /// take additional arguments in addition to the predecessor enum argument.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILBasicBlock *>, SILBasicBlock *>
      pullbackTrampolineBBMap;

  /// Mapping from original basic blocks to dominated active values.
  llvm::DenseMap<SILBasicBlock *, SmallVector<SILValue, 8>> activeValues;

  /// Mapping from original basic blocks and original active values to
  /// corresponding pullback block arguments.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILValue>, SILArgument *>
      activeValuePullbackBBArgumentMap;

  /// Mapping from original basic blocks to local temporary values to be cleaned
  /// up. This is populated when pullback emission is run on one basic block and
  /// cleaned before processing another basic block.
  llvm::DenseMap<SILBasicBlock *, SmallSetVector<SILValue, 64>>
      blockTemporaries;

  /// The scope cloner.
  ScopeCloner scopeCloner;

  /// The main builder.
  TangentBuilder builder;

  /// An auxiliary local allocation builder.
  TangentBuilder localAllocBuilder;

  /// The original function's exit block.
  SILBasicBlock *originalExitBlock = nullptr;

  /// Stack buffers allocated for storing local adjoint values.
  SmallVector<AllocStackInst *, 64> functionLocalAllocations;

  /// A set used to remember local allocations that were destroyed.
  llvm::SmallDenseSet<SILValue> destroyedLocalAllocations;

  /// The seed arguments of the pullback function.
  SmallVector<SILArgument *, 4> seeds;

  /// The `AutoDiffLinearMapContext` object, if any.
  SILValue contextValue = nullptr;

  llvm::BumpPtrAllocator allocator;

  bool errorOccurred = false;

  ADContext &getContext() const { return vjpCloner.getContext(); }
  SILModule &getModule() const { return getContext().getModule(); }
  ASTContext &getASTContext() const { return getPullback().getASTContext(); }
  SILFunction &getOriginal() const { return vjpCloner.getOriginal(); }
  SILDifferentiabilityWitness *getWitness() const {
    return vjpCloner.getWitness();
  }
  DifferentiationInvoker getInvoker() const { return vjpCloner.getInvoker(); }
  LinearMapInfo &getPullbackInfo() const { return vjpCloner.getPullbackInfo(); }
  const AutoDiffConfig &getConfig() const { return vjpCloner.getConfig(); }
  const DifferentiableActivityInfo &getActivityInfo() const {
    return vjpCloner.getActivityInfo();
  }

  //--------------------------------------------------------------------------//
  // Pullback struct mapping
  //--------------------------------------------------------------------------//

  void initializePullbackTupleElements(SILBasicBlock *origBB,
                                       SILInstructionResultArray values) {
    auto *pbTupleTyple = getPullbackInfo().getLinearMapTupleType(origBB);
    assert(pbTupleTyple->getNumElements() == values.size() &&
           "The number of pullback tuple fields must equal the number of "
           "pullback tuple element values");
    auto res = pullbackTupleElements.insert({origBB, { values.begin(), values.end() }});
    (void)res;
    assert(res.second && "A pullback tuple element already exists!");
  }

  void initializePullbackTupleElements(SILBasicBlock *origBB,
                                       const llvm::ArrayRef<SILArgument *> &values) {
    auto *pbTupleTyple = getPullbackInfo().getLinearMapTupleType(origBB);
    assert(pbTupleTyple->getNumElements() == values.size() &&
           "The number of pullback tuple fields must equal the number of "
           "pullback tuple element values");
    auto res = pullbackTupleElements.insert({origBB, { values.begin(), values.end() }});
    (void)res;
    assert(res.second && "A pullback struct element already exists!");
  }

  /// Returns the pullback tuple element value corresponding to the given
  /// original block and apply inst.
  SILValue getPullbackTupleElement(ApplyInst *ai) {
    unsigned idx = getPullbackInfo().lookUpLinearMapIndex(ai);
    assert((idx > 0 || (idx == 0 && ai->getParentBlock()->isEntry())) &&
           "impossible linear map index");
    auto values = pullbackTupleElements.lookup(ai->getParentBlock());
    assert(idx < values.size() &&
           "pullback tuple element for this apply does not exist!");
    return values[idx];
  }

  /// Returns the pullback tuple element value corresponding to the predecessor
  /// for the given original block.
  SILValue getPullbackPredTupleElement(SILBasicBlock *origBB) {
    assert(!origBB->isEntry() && "no predecessors for entry block");
    auto values = pullbackTupleElements.lookup(origBB);
    assert(values.size() && "pullback tuple cannot be empty");
    return values[0];
  }

  //--------------------------------------------------------------------------//
  // Type transformer
  //--------------------------------------------------------------------------//

  /// Get the type lowering for the given AST type.
  const Lowering::TypeLowering &getTypeLowering(Type type) {
    auto pbGenSig =
        getPullback().getLoweredFunctionType()->getSubstGenericSignature();
    Lowering::AbstractionPattern pattern(pbGenSig,
                                         type->getReducedType(pbGenSig));
    return getPullback().getTypeLowering(pattern, type);
  }

  /// Remap any archetypes into the current function's context.
  SILType remapType(SILType ty) {
    if (ty.hasArchetype())
      ty = ty.mapTypeOutOfContext();
    auto remappedType = ty.getASTType()->getReducedType(
        getPullback().getLoweredFunctionType()->getSubstGenericSignature());
    auto remappedSILType =
        SILType::getPrimitiveType(remappedType, ty.getCategory());
    return getPullback().mapTypeIntoContext(remappedSILType);
  }

  llvm::Optional<TangentSpace> getTangentSpace(CanType type) {
    // Use witness generic signature to remap types.
    type =
        getWitness()->getDerivativeGenericSignature().getReducedType(
            type);
    return type->getAutoDiffTangentSpace(
        LookUpConformanceInModule(getModule().getSwiftModule()));
  }

  /// Returns the tangent value category of the given value.
  SILValueCategory getTangentValueCategory(SILValue v) {
    // Tangent value category table:
    //
    // Let $L be a loadable type and $*A be an address-only type.
    //
    // Original type | Tangent type loadable? | Tangent value category and type
    // --------------|------------------------|--------------------------------
    // $L            | loadable               | object, $L' (no mismatch)
    // $*A           | loadable               | address, $*L' (create a buffer)
    // $L            | address-only           | address, $*A' (no alternative)
    // $*A           | address-only           | address, $*A' (no alternative)

    // TODO(https://github.com/apple/swift/issues/55523): Make "tangent value category" depend solely on whether the tangent type is loadable or address-only.
    //
    // For loadable tangent types, using symbolic adjoint values instead of
    // concrete adjoint buffers is more efficient.

    // Quick check: if the value has an address type, the tangent value category
    // is currently always "address".
    if (v->getType().isAddress())
      return SILValueCategory::Address;
    // If the value has an object type and the tangent type is not address-only,
    // then the tangent value category is "object".
    auto tanSpace = getTangentSpace(remapType(v->getType()).getASTType());
    auto tanASTType = tanSpace->getCanonicalType();
    if (v->getType().isObject() && getTypeLowering(tanASTType).isLoadable())
      return SILValueCategory::Object;
    // Otherwise, the tangent value category is "address".
    return SILValueCategory::Address;
  }

  /// Assuming the given type conforms to `Differentiable` after remapping,
  /// returns the associated tangent space type.
  SILType getRemappedTangentType(SILType type) {
    return SILType::getPrimitiveType(
        getTangentSpace(remapType(type).getASTType())->getCanonicalType(),
        type.getCategory());
  }

  /// Substitutes all replacement types of the given substitution map using the
  /// pullback function's substitution map.
  SubstitutionMap remapSubstitutionMap(SubstitutionMap substMap) {
    return substMap.subst(getPullback().getForwardingSubstitutionMap());
  }

  //--------------------------------------------------------------------------//
  // Temporary value management
  //--------------------------------------------------------------------------//

  /// Record a temporary value for cleanup before its block's terminator.
  SILValue recordTemporary(SILValue value) {
    assert(value->getType().isObject());
    assert(value->getFunction() == &getPullback());
    auto inserted = blockTemporaries[value->getParentBlock()].insert(value);
    (void)inserted;
    LLVM_DEBUG(getADDebugStream() << "Recorded temporary " << value);
    assert(inserted && "Temporary already recorded?");
    return value;
  }

  /// Clean up all temporary values for the given pullback block.
  void cleanUpTemporariesForBlock(SILBasicBlock *bb, SILLocation loc) {
    assert(bb->getParent() == &getPullback());
    LLVM_DEBUG(getADDebugStream() << "Cleaning up temporaries for pullback bb"
                                  << bb->getDebugID() << '\n');
    for (auto temp : blockTemporaries[bb])
      builder.emitDestroyValueOperation(loc, temp);
    blockTemporaries[bb].clear();
  }

  //--------------------------------------------------------------------------//
  // Adjoint value factory methods
  //--------------------------------------------------------------------------//

  AdjointValue makeZeroAdjointValue(SILType type) {
    return AdjointValue::createZero(allocator, remapType(type));
  }

  AdjointValue makeConcreteAdjointValue(SILValue value) {
    return AdjointValue::createConcrete(allocator, value);
  }

  AdjointValue makeAggregateAdjointValue(SILType type,
                                         ArrayRef<AdjointValue> elements) {
    return AdjointValue::createAggregate(allocator, remapType(type), elements);
  }

  //--------------------------------------------------------------------------//
  // Adjoint value materialization
  //--------------------------------------------------------------------------//

  /// Materializes an adjoint value. The type of the given adjoint value must be
  /// loadable.
  SILValue materializeAdjointDirect(AdjointValue val, SILLocation loc) {
    assert(val.getType().isObject());
    LLVM_DEBUG(getADDebugStream()
               << "Materializing adjoint for " << val << '\n');
    SILValue result;
    switch (val.getKind()) {
    case AdjointValueKind::Zero:
      result = recordTemporary(builder.emitZero(loc, val.getSwiftType()));
      break;
    case AdjointValueKind::Aggregate: {
      SmallVector<SILValue, 8> elements;
      for (auto i : range(val.getNumAggregateElements())) {
        auto eltVal = materializeAdjointDirect(val.getAggregateElement(i), loc);
        elements.push_back(builder.emitCopyValueOperation(loc, eltVal));
      }
      if (val.getType().is<TupleType>())
        result = recordTemporary(
            builder.createTuple(loc, val.getType(), elements));
      else
        result = recordTemporary(
            builder.createStruct(loc, val.getType(), elements));
      break;
    }
    case AdjointValueKind::Concrete:
      result = val.getConcreteValue();
      break;
    }
    if (auto debugInfo = val.getDebugInfo())
      builder.createDebugValue(
          debugInfo->first.getLocation(), result, debugInfo->second);
    return result;
  }

  /// Materializes an adjoint value indirectly to a SIL buffer.
  void materializeAdjointIndirect(AdjointValue val, SILValue destAddress,
                                  SILLocation loc) {
    assert(destAddress->getType().isAddress());
    switch (val.getKind()) {
    /// If adjoint value is a symbolic zero, emit a call to
    /// `AdditiveArithmetic.zero`.
    case AdjointValueKind::Zero:
      builder.emitZeroIntoBuffer(loc, destAddress, IsInitialization);
      break;
    /// If adjoint value is a symbolic aggregate (tuple or struct), recursively
    /// materialize the symbolic tuple or struct, filling the
    /// buffer.
    case AdjointValueKind::Aggregate: {
      if (auto *tupTy = val.getSwiftType()->getAs<TupleType>()) {
        for (auto idx : range(val.getNumAggregateElements())) {
          auto eltTy = SILType::getPrimitiveAddressType(
              tupTy->getElementType(idx)->getCanonicalType());
          auto *eltBuf =
              builder.createTupleElementAddr(loc, destAddress, idx, eltTy);
          materializeAdjointIndirect(val.getAggregateElement(idx), eltBuf, loc);
        }
      } else if (auto *structDecl =
                     val.getSwiftType()->getStructOrBoundGenericStruct()) {
        auto fieldIt = structDecl->getStoredProperties().begin();
        for (unsigned i = 0; fieldIt != structDecl->getStoredProperties().end();
             ++fieldIt, ++i) {
          auto eltBuf =
              builder.createStructElementAddr(loc, destAddress, *fieldIt);
          materializeAdjointIndirect(val.getAggregateElement(i), eltBuf, loc);
        }
      } else {
        llvm_unreachable("Not an aggregate type");
      }
      break;
    }
    /// If adjoint value is concrete, it is already materialized. Store it in
    /// the destination address.
    case AdjointValueKind::Concrete:
      auto concreteVal = val.getConcreteValue();
      builder.emitStoreValueOperation(loc, concreteVal, destAddress,
                                      StoreOwnershipQualifier::Init);
      break;
    }
  }

  //--------------------------------------------------------------------------//
  // Adjoint value mapping
  //--------------------------------------------------------------------------//

  /// Returns true if the given value in the original function has a
  /// corresponding adjoint value.
  bool hasAdjointValue(SILBasicBlock *origBB, SILValue originalValue) const {
    assert(origBB->getParent() == &getOriginal());
    assert(originalValue->getType().isObject());
    return valueMap.count({origBB, originalValue});
  }

  /// Initializes the adjoint value for the original value. Asserts that the
  /// original value does not already have an adjoint value.
  void setAdjointValue(SILBasicBlock *origBB, SILValue originalValue,
                       AdjointValue adjointValue) {
    LLVM_DEBUG(getADDebugStream()
               << "Setting adjoint value for " << originalValue);
    assert(origBB->getParent() == &getOriginal());
    assert(originalValue->getType().isObject());
    assert(getTangentValueCategory(originalValue) == SILValueCategory::Object);
    assert(adjointValue.getType().isObject());
    assert(originalValue->getFunction() == &getOriginal());
    // The adjoint value must be in the tangent space.
    assert(adjointValue.getType() ==
           getRemappedTangentType(originalValue->getType()));
    // Try to assign a debug variable.
    if (auto debugInfo = findDebugLocationAndVariable(originalValue)) {
      LLVM_DEBUG({
        auto &s = getADDebugStream();
        s << "Found debug variable: \"" << debugInfo->second.Name
          << "\"\nLocation: ";
        debugInfo->first.getLocation().print(s, getASTContext().SourceMgr);
        s << '\n';
      });
      adjointValue.setDebugInfo(*debugInfo);
    } else {
      LLVM_DEBUG(getADDebugStream() << "No debug variable found.\n");
    }
    // Insert into dictionary.
    auto insertion =
        valueMap.try_emplace({origBB, originalValue}, adjointValue);
    LLVM_DEBUG(getADDebugStream()
               << "The new adjoint value, replacing the existing one, is: "
               << insertion.first->getSecond() << '\n');
    if (!insertion.second)
      insertion.first->getSecond() = adjointValue;
  }

  /// Returns the adjoint value for a value in the original function.
  ///
  /// This method first tries to find an existing entry in the adjoint value
  /// mapping. If no entry exists, creates a zero adjoint value.
  AdjointValue getAdjointValue(SILBasicBlock *origBB, SILValue originalValue) {
    assert(origBB->getParent() == &getOriginal());
    assert(originalValue->getType().isObject());
    assert(getTangentValueCategory(originalValue) == SILValueCategory::Object);
    assert(originalValue->getFunction() == &getOriginal());
    auto insertion = valueMap.try_emplace(
        {origBB, originalValue},
        makeZeroAdjointValue(getRemappedTangentType(originalValue->getType())));
    auto it = insertion.first;
    return it->getSecond();
  }

  /// Adds `newAdjointValue` to the adjoint value for `originalValue` and sets
  /// the sum as the new adjoint value.
  void addAdjointValue(SILBasicBlock *origBB, SILValue originalValue,
                       AdjointValue newAdjointValue, SILLocation loc) {
    assert(origBB->getParent() == &getOriginal());
    assert(originalValue->getType().isObject());
    assert(newAdjointValue.getType().isObject());
    assert(originalValue->getFunction() == &getOriginal());
    LLVM_DEBUG(getADDebugStream()
               << "Adding adjoint value for " << originalValue);
    // The adjoint value must be in the tangent space.
    assert(newAdjointValue.getType() ==
           getRemappedTangentType(originalValue->getType()));
    // Try to assign a debug variable.
    if (auto debugInfo = findDebugLocationAndVariable(originalValue)) {
      LLVM_DEBUG({
        auto &s = getADDebugStream();
        s << "Found debug variable: \"" << debugInfo->second.Name
          << "\"\nLocation: ";
        debugInfo->first.getLocation().print(s, getASTContext().SourceMgr);
        s << '\n';
      });
      newAdjointValue.setDebugInfo(*debugInfo);
    } else {
      LLVM_DEBUG(getADDebugStream() << "No debug variable found.\n");
    }
    auto insertion =
        valueMap.try_emplace({origBB, originalValue}, newAdjointValue);
    auto inserted = insertion.second;
    if (inserted)
      return;
    // If adjoint already exists, accumulate the adjoint onto the existing
    // adjoint.
    auto it = insertion.first;
    auto existingValue = it->getSecond();
    valueMap.erase(it);
    auto adjVal = accumulateAdjointsDirect(existingValue, newAdjointValue, loc);
    // If the original value is the `Array` result of an
    // `array.uninitialized_intrinsic` application, accumulate adjoint buffers
    // for the array element addresses.
    accumulateArrayLiteralElementAddressAdjoints(origBB, originalValue, adjVal,
                                                 loc);
    setAdjointValue(origBB, originalValue, adjVal);
  }

  /// Get the pullback block argument corresponding to the given original block
  /// and active value.
  SILArgument *getActiveValuePullbackBlockArgument(SILBasicBlock *origBB,
                                                   SILValue activeValue) {
    assert(getTangentValueCategory(activeValue) == SILValueCategory::Object);
    assert(origBB->getParent() == &getOriginal());
    auto pullbackBBArg =
        activeValuePullbackBBArgumentMap[{origBB, activeValue}];
    assert(pullbackBBArg);
    assert(pullbackBBArg->getParent() == getPullbackBlock(origBB));
    return pullbackBBArg;
  }

  //--------------------------------------------------------------------------//
  // Adjoint value accumulation
  //--------------------------------------------------------------------------//

  /// Given two adjoint values, accumulates them and returns their sum.
  AdjointValue accumulateAdjointsDirect(AdjointValue lhs, AdjointValue rhs,
                                        SILLocation loc);

  //--------------------------------------------------------------------------//
  // Adjoint buffer mapping
  //--------------------------------------------------------------------------//

  /// If the given original value is an address projection, returns a
  /// corresponding adjoint projection to be used as its adjoint buffer.
  ///
  /// Helper function for `getAdjointBuffer`.
  SILValue getAdjointProjection(SILBasicBlock *origBB, SILValue originalValue);

  /// Returns the adjoint buffer for the original value.
  ///
  /// This method first tries to find an existing entry in the adjoint buffer
  /// mapping. If no entry exists, creates a zero adjoint buffer.
  SILValue getAdjointBuffer(SILBasicBlock *origBB, SILValue originalValue) {
    assert(getTangentValueCategory(originalValue) == SILValueCategory::Address);
    assert(originalValue->getFunction() == &getOriginal());
    auto insertion = bufferMap.try_emplace({origBB, originalValue}, SILValue());
    if (!insertion.second) // not inserted
      return insertion.first->getSecond();

    // If the original buffer is a projection, return a corresponding projection
    // into the adjoint buffer.
    if (auto adjProj = getAdjointProjection(origBB, originalValue))
      return (bufferMap[{origBB, originalValue}] = adjProj);

    LLVM_DEBUG(getADDebugStream() << "Creating new adjoint buffer for "
               << originalValue
               << "in bb" << origBB->getDebugID() << '\n');

    auto bufType = getRemappedTangentType(originalValue->getType());
    // Set insertion point for local allocation builder: before the last local
    // allocation, or at the start of the pullback function's entry if no local
    // allocations exist yet.
    auto debugInfo = findDebugLocationAndVariable(originalValue);
    SILLocation loc = debugInfo ? debugInfo->first.getLocation()
                                : RegularLocation::getAutoGeneratedLocation();
    llvm::SmallString<32> adjName;
    auto *newBuf = createFunctionLocalAllocation(
        bufType, loc, /*zeroInitialize*/ true,
        debugInfo.transform(
          [&](AdjointValue::DebugInfo di) {
            llvm::raw_svector_ostream adjNameStream(adjName);
            SILDebugVariable &dv = di.second;
            dv.ArgNo = 0;
            adjNameStream << "derivative of '" << dv.Name << "'";
            if (SILDebugLocation origBBLoc = origBB->front().getDebugLocation()) {
              adjNameStream << " in scope at ";
              origBBLoc.getLocation().print(adjNameStream, getASTContext().SourceMgr);
            }
            adjNameStream << " (scope #" << origBB->getDebugID() << ")";
            dv.Name = adjName;
            return dv;
          }));
    return (insertion.first->getSecond() = newBuf);
  }

  /// Initializes the adjoint buffer for the original value. Asserts that the
  /// original value does not already have an adjoint buffer.
  void setAdjointBuffer(SILBasicBlock *origBB, SILValue originalValue,
                        SILValue adjointBuffer) {
    assert(getTangentValueCategory(originalValue) == SILValueCategory::Address);
    auto insertion =
        bufferMap.try_emplace({origBB, originalValue}, adjointBuffer);
    assert(insertion.second && "Adjoint buffer already exists");
    (void)insertion;
  }

  /// Accumulates `rhsAddress` into the adjoint buffer corresponding to the
  /// original value.
  void addToAdjointBuffer(SILBasicBlock *origBB, SILValue originalValue,
                          SILValue rhsAddress, SILLocation loc) {
    assert(getTangentValueCategory(originalValue) ==
               SILValueCategory::Address &&
           rhsAddress->getType().isAddress());
    assert(originalValue->getFunction() == &getOriginal());
    assert(rhsAddress->getFunction() == &getPullback());
    auto adjointBuffer = getAdjointBuffer(origBB, originalValue);

    LLVM_DEBUG(getADDebugStream() << "Adding"
               << rhsAddress << "to adjoint ("
               << adjointBuffer << ") of "
               << originalValue
               << "in bb" << origBB->getDebugID() << '\n');

    builder.emitInPlaceAdd(loc, adjointBuffer, rhsAddress);
  }

  /// Returns a next insertion point for creating a local allocation: either
  /// before the previous local allocation, or at the start of the pullback
  /// entry if no local allocations exist.
  ///
  /// Helper for `createFunctionLocalAllocation`.
  SILBasicBlock::iterator getNextFunctionLocalAllocationInsertionPoint() {
    // If there are no local allocations, insert at the pullback entry start.
    if (functionLocalAllocations.empty())
      return getPullback().getEntryBlock()->begin();
    // Otherwise, insert before the last local allocation. Inserting before
    // rather than after ensures that allocation and zero initialization
    // instructions are grouped together.
    auto lastLocalAlloc = functionLocalAllocations.back();
    return lastLocalAlloc->getDefiningInstruction()->getIterator();
  }

  /// Creates and returns a local allocation with the given type.
  ///
  /// Local allocations are created uninitialized in the pullback entry and
  /// deallocated in the pullback exit. All local allocations not in
  /// `destroyedLocalAllocations` are also destroyed in the pullback exit.
  ///
  /// Helper for `getAdjointBuffer`.
  AllocStackInst *createFunctionLocalAllocation(
      SILType type, SILLocation loc, bool zeroInitialize = false,
      llvm::Optional<SILDebugVariable> varInfo = llvm::None) {
    // Set insertion point for local allocation builder: before the last local
    // allocation, or at the start of the pullback function's entry if no local
    // allocations exist yet.
    localAllocBuilder.setInsertionPoint(
        getPullback().getEntryBlock(),
        getNextFunctionLocalAllocationInsertionPoint());
    // Create and return local allocation.
    auto *alloc = localAllocBuilder.createAllocStack(loc, type, varInfo);
    functionLocalAllocations.push_back(alloc);
    // Zero-initialize if requested.
    if (zeroInitialize)
      localAllocBuilder.emitZeroIntoBuffer(loc, alloc, IsInitialization);
    return alloc;
  }

  //--------------------------------------------------------------------------//
  // Optional differentiation
  //--------------------------------------------------------------------------//

  /// Given a `wrappedAdjoint` value of type `T.TangentVector`, creates an
  /// `Optional<T>.TangentVector` value from it and adds it to the adjoint value
  /// of `optionalValue`.
  ///
  /// `wrappedAdjoint` may be an object or address value, both cases are
  /// handled.
  void accumulateAdjointForOptional(SILBasicBlock *bb, SILValue optionalValue,
                                    SILValue wrappedAdjoint);

  //--------------------------------------------------------------------------//
  // Array literal initialization differentiation
  //--------------------------------------------------------------------------//

  /// Given the adjoint value of an array initialized from an
  /// `array.uninitialized_intrinsic` application and an array element index,
  /// returns an `alloc_stack` containing the adjoint value of the array element
  /// at the given index by applying `Array.TangentVector.subscript`.
  AllocStackInst *getArrayAdjointElementBuffer(SILValue arrayAdjoint,
                                               int eltIndex, SILLocation loc);

  /// Given the adjoint value of an array initialized from an
  /// `array.uninitialized_intrinsic` application, accumulates the adjoint
  /// value's elements into the adjoint buffers of its element addresses.
  void accumulateArrayLiteralElementAddressAdjoints(
      SILBasicBlock *origBB, SILValue originalValue,
      AdjointValue arrayAdjointValue, SILLocation loc);

  //--------------------------------------------------------------------------//
  // CFG mapping
  //--------------------------------------------------------------------------//

  SILBasicBlock *getPullbackBlock(SILBasicBlock *originalBlock) {
    return pullbackBBMap.lookup(originalBlock);
  }

  SILBasicBlock *getPullbackTrampolineBlock(SILBasicBlock *originalBlock,
                                            SILBasicBlock *successorBlock) {
    return pullbackTrampolineBBMap.lookup({originalBlock, successorBlock});
  }

  //--------------------------------------------------------------------------//
  // Debug info
  //--------------------------------------------------------------------------//

  const SILDebugScope *remapScope(const SILDebugScope *DS) {
    return scopeCloner.getOrCreateClonedScope(DS);
  }

  //--------------------------------------------------------------------------//
  // Debugging utilities
  //--------------------------------------------------------------------------//

  void printAdjointValueMapping() {
    // Group original/adjoint values by basic block.
    llvm::DenseMap<SILBasicBlock *, llvm::DenseMap<SILValue, AdjointValue>> tmp;
    for (auto pair : valueMap) {
      auto origPair = pair.first;
      auto *origBB = origPair.first;
      auto origValue = origPair.second;
      auto adjValue = pair.second;
      tmp[origBB].insert({origValue, adjValue});
    }
    // Print original/adjoint values per basic block.
    auto &s = getADDebugStream() << "Adjoint value mapping:\n";
    for (auto &origBB : getOriginal()) {
      if (!pullbackBBMap.count(&origBB))
        continue;
      auto bbValueMap = tmp[&origBB];
      s << "bb" << origBB.getDebugID();
      s << " (size " << bbValueMap.size() << "):\n";
      for (auto valuePair : bbValueMap) {
        auto origValue = valuePair.first;
        auto adjValue = valuePair.second;
        s << "ORIG: " << origValue;
        s << "ADJ: " << adjValue << '\n';
      }
      s << '\n';
    }
  }

  void printAdjointBufferMapping() {
    // Group original/adjoint buffers by basic block.
    llvm::DenseMap<SILBasicBlock *, llvm::DenseMap<SILValue, SILValue>> tmp;
    for (auto pair : bufferMap) {
      auto origPair = pair.first;
      auto *origBB = origPair.first;
      auto origBuf = origPair.second;
      auto adjBuf = pair.second;
      tmp[origBB][origBuf] = adjBuf;
    }
    // Print original/adjoint buffers per basic block.
    auto &s = getADDebugStream() << "Adjoint buffer mapping:\n";
    for (auto &origBB : getOriginal()) {
      if (!pullbackBBMap.count(&origBB))
        continue;
      auto bbBufferMap = tmp[&origBB];
      s << "bb" << origBB.getDebugID();
      s << " (size " << bbBufferMap.size() << "):\n";
      for (auto valuePair : bbBufferMap) {
        auto origBuf = valuePair.first;
        auto adjBuf = valuePair.second;
        s << "ORIG: " << origBuf;
        s << "ADJ: " << adjBuf << '\n';
      }
      s << '\n';
    }
  }

public:
  //--------------------------------------------------------------------------//
  // Entry point
  //--------------------------------------------------------------------------//

  /// Performs pullback generation on the empty pullback function. Returns true
  /// if any error occurs.
  bool run();

  /// Performs pullback generation on the empty pullback function, given that
  /// the original function is a "semantic member accessor".
  ///
  /// "Semantic member accessors" are attached to member properties that have a
  /// corresponding tangent stored property in the parent `TangentVector` type.
  /// These accessors have special-case pullback generation based on their
  /// semantic behavior.
  ///
  /// Returns true if any error occurs.
  bool runForSemanticMemberAccessor();
  bool runForSemanticMemberGetter();
  bool runForSemanticMemberSetter();

  /// If original result is non-varied, it will always have a zero derivative.
  /// Skip full pullback generation and simply emit zero derivatives for wrt
  /// parameters.
  void emitZeroDerivativesForNonvariedResult(SILValue origNonvariedResult);

  /// Public helper so that our users can get the underlying newly created
  /// function.
  SILFunction &getPullback() const { return vjpCloner.getPullback(); }

  using TrampolineBlockSet = SmallPtrSet<SILBasicBlock *, 4>;

  /// Determines the pullback successor block for a given original block and one
  /// of its predecessors. When a trampoline block is necessary, emits code into
  /// the trampoline block to trampoline the original block's active value's
  /// adjoint values.
  ///
  /// Populates `pullbackTrampolineBlockMap`, which maps active values' adjoint
  /// values to the pullback successor blocks in which they are used. This
  /// allows us to release those values in pullback successor blocks that do not
  /// use them.
  SILBasicBlock *
  buildPullbackSuccessor(SILBasicBlock *origBB, SILBasicBlock *origPredBB,
                         llvm::SmallDenseMap<SILValue, TrampolineBlockSet>
                             &pullbackTrampolineBlockMap);

  /// Emits pullback code in the corresponding pullback block.
  void visitSILBasicBlock(SILBasicBlock *bb);

  void visit(SILInstruction *inst) {
    if (errorOccurred)
      return;

    LLVM_DEBUG(getADDebugStream()
               << "PullbackCloner visited:\n[ORIG]" << *inst);
#ifndef NDEBUG
    auto beforeInsertion = std::prev(builder.getInsertionPoint());
#endif
    SILInstructionVisitor::visit(inst);
    LLVM_DEBUG({
      auto &s = llvm::dbgs() << "[ADJ] Emitted in pullback (pb bb" <<
        builder.getInsertionBB()->getDebugID() << "):\n";
      auto afterInsertion = builder.getInsertionPoint();
      for (auto it = ++beforeInsertion; it != afterInsertion; ++it)
        s << *it;
    });
  }

  /// Fallback instruction visitor for unhandled instructions.
  /// Emit a general non-differentiability diagnostic.
  void visitSILInstruction(SILInstruction *inst) {
    LLVM_DEBUG(getADDebugStream()
               << "Unhandled instruction in PullbackCloner: " << *inst);
    getContext().emitNondifferentiabilityError(
        inst, getInvoker(), diag::autodiff_expression_not_differentiable_note);
    errorOccurred = true;
  }

  /// Handle `apply` instruction.
  ///   Original: (y0, y1, ...) = apply @fn (x0, x1, ...)
  ///    Adjoint: (adj[x0], adj[x1], ...) += apply @fn_pullback (adj[y0], ...)
  void visitApplyInst(ApplyInst *ai) {
    assert(getPullbackInfo().shouldDifferentiateApplySite(ai));
    // Skip `array.uninitialized_intrinsic` applications, which have special
    // `store` and `copy_addr` support.
    if (ArraySemanticsCall(ai, semantics::ARRAY_UNINITIALIZED_INTRINSIC))
      return;
    auto loc = ai->getLoc();
    auto *bb = ai->getParent();
    // Handle `array.finalize_intrinsic` applications.
    // `array.finalize_intrinsic` semantically behaves like an identity
    // function.
    if (ArraySemanticsCall(ai, semantics::ARRAY_FINALIZE_INTRINSIC)) {
      assert(ai->getNumArguments() == 1 &&
             "Expected intrinsic to have one operand");
      // Accumulate result's adjoint into argument's adjoint.
      auto adjResult = getAdjointValue(bb, ai);
      auto origArg = ai->getArgumentsWithoutIndirectResults().front();
      addAdjointValue(bb, origArg, adjResult, loc);
      return;
    }
    // Replace a call to a function with a call to its pullback.
    auto &nestedApplyInfo = getContext().getNestedApplyInfo();
    auto applyInfoLookup = nestedApplyInfo.find(ai);
    // If no `NestedApplyInfo` was found, then this task doesn't need to be
    // differentiated.
    if (applyInfoLookup == nestedApplyInfo.end()) {
      // Must not be active.
      assert(!getActivityInfo().isActive(ai, getConfig()));
      return;
    }
    auto applyInfo = applyInfoLookup->getSecond();

    // Get the original result of the `apply` instruction.
    SmallVector<SILValue, 8> origDirectResults;
    forEachApplyDirectResult(ai, [&](SILValue directResult) {
      origDirectResults.push_back(directResult);
    });
    SmallVector<SILValue, 8> origAllResults;
    collectAllActualResultsInTypeOrder(ai, origDirectResults, origAllResults);
    // Append `inout` arguments after original results.
    for (auto paramIdx : applyInfo.config.parameterIndices->getIndices()) {
      auto paramInfo = ai->getSubstCalleeConv().getParamInfoForSILArg(
          ai->getNumIndirectResults() + paramIdx);
      if (!paramInfo.isIndirectMutating())
        continue;
      origAllResults.push_back(
          ai->getArgumentsWithoutIndirectResults()[paramIdx]);
    }

    // Get callee pullback arguments.
    SmallVector<SILValue, 8> args;

    // Handle callee pullback indirect results.
    // Create local allocations for these and destroy them after the call.
    auto pullback = getPullbackTupleElement(ai);
    auto pullbackType =
        remapType(pullback->getType()).castTo<SILFunctionType>();

    auto actualPullbackType = applyInfo.originalPullbackType
                                  ? *applyInfo.originalPullbackType
                                  : pullbackType;
    actualPullbackType = actualPullbackType->getUnsubstitutedType(getModule());
    SmallVector<AllocStackInst *, 4> pullbackIndirectResults;
    for (auto indRes : actualPullbackType->getIndirectFormalResults()) {
      auto *alloc = builder.createAllocStack(
          loc, remapType(indRes.getSILStorageInterfaceType()));
      pullbackIndirectResults.push_back(alloc);
      args.push_back(alloc);
    }

    // Collect callee pullback formal arguments.
    for (auto resultIndex : applyInfo.config.resultIndices->getIndices()) {
      assert(resultIndex < origAllResults.size());
      auto origResult = origAllResults[resultIndex];
      // Get the seed (i.e. adjoint value of the original result).
      SILValue seed;
      switch (getTangentValueCategory(origResult)) {
      case SILValueCategory::Object:
        seed = materializeAdjointDirect(getAdjointValue(bb, origResult), loc);
        break;
      case SILValueCategory::Address:
        seed = getAdjointBuffer(bb, origResult);
        break;
      }
      args.push_back(seed);
    }

    // If callee pullback was reabstracted in VJP, reabstract callee pullback.
    if (applyInfo.originalPullbackType) {
      SILOptFunctionBuilder fb(getContext().getTransform());
      pullback = reabstractFunction(
          builder, fb, loc, pullback, *applyInfo.originalPullbackType,
          [this](SubstitutionMap subs) -> SubstitutionMap {
            return this->remapSubstitutionMap(subs);
          });
    }

    // Call the callee pullback.
    auto *pullbackCall = builder.createApply(loc, pullback, SubstitutionMap(),
                                             args);
    builder.emitDestroyValueOperation(loc, pullback);

    // Extract all results from `pullbackCall`.
    SmallVector<SILValue, 8> dirResults;
    extractAllElements(pullbackCall, builder, dirResults);
    // Get all results in type-defined order.
    SmallVector<SILValue, 8> allResults;
    collectAllActualResultsInTypeOrder(pullbackCall, dirResults, allResults);

    LLVM_DEBUG({
      auto &s = getADDebugStream();
      s << "All results of the nested pullback call:\n";
      llvm::for_each(allResults, [&](SILValue v) { s << v; });
    });

    // Accumulate adjoints for original differentiation parameters.
    auto allResultsIt = allResults.begin();
    for (unsigned i : applyInfo.config.parameterIndices->getIndices()) {
      auto origArg = ai->getArgument(ai->getNumIndirectResults() + i);
      // Skip adjoint accumulation for `inout` arguments.
      auto paramInfo = ai->getSubstCalleeConv().getParamInfoForSILArg(
          ai->getNumIndirectResults() + i);
      if (paramInfo.isIndirectMutating())
        continue;
      auto tan = *allResultsIt++;
      if (tan->getType().isAddress()) {
        addToAdjointBuffer(bb, origArg, tan, loc);
      } else {
        if (origArg->getType().isAddress()) {
          auto *tmpBuf = builder.createAllocStack(loc, tan->getType());
          builder.emitStoreValueOperation(loc, tan, tmpBuf,
                                          StoreOwnershipQualifier::Init);
          addToAdjointBuffer(bb, origArg, tmpBuf, loc);
          builder.emitDestroyAddrAndFold(loc, tmpBuf);
          builder.createDeallocStack(loc, tmpBuf);
        } else {
          recordTemporary(tan);
          addAdjointValue(bb, origArg, makeConcreteAdjointValue(tan), loc);
        }
      }
    }
    // Destroy unused pullback direct results. Needed for pullback results from
    // VJPs extracted from `@differentiable` function callees, where the
    // `@differentiable` function's differentiation parameter indices are a
    // superset of the active `apply` parameter indices.
    while (allResultsIt != allResults.end()) {
      auto unusedPullbackDirectResult = *allResultsIt++;
      if (unusedPullbackDirectResult->getType().isAddress())
        continue;
      builder.emitDestroyValueOperation(loc, unusedPullbackDirectResult);
    }
    // Destroy and deallocate pullback indirect results.
    for (auto *alloc : llvm::reverse(pullbackIndirectResults)) {
      builder.emitDestroyAddrAndFold(loc, alloc);
      builder.createDeallocStack(loc, alloc);
    }
  }

  void visitBeginApplyInst(BeginApplyInst *bai) {
    // Diagnose `begin_apply` instructions.
    // Coroutine differentiation is not yet supported.
    getContext().emitNondifferentiabilityError(
        bai, getInvoker(), diag::autodiff_coroutines_not_supported);
    errorOccurred = true;
    return;
  }

  /// Handle `struct` instruction.
  ///   Original: y = struct (x0, x1, x2, ...)
  ///    Adjoint: adj[x0] += struct_extract adj[y], #x0
  ///             adj[x1] += struct_extract adj[y], #x1
  ///             adj[x2] += struct_extract adj[y], #x2
  ///             ...
  void visitStructInst(StructInst *si) {
    auto *bb = si->getParent();
    auto loc = si->getLoc();
    auto *structDecl = si->getStructDecl();
    switch (getTangentValueCategory(si)) {
    case SILValueCategory::Object: {
      auto av = getAdjointValue(bb, si);
      switch (av.getKind()) {
      case AdjointValueKind::Zero: {
        for (auto *field : structDecl->getStoredProperties()) {
          auto fv = si->getFieldValue(field);
          addAdjointValue(
              bb, fv,
              makeZeroAdjointValue(getRemappedTangentType(fv->getType())), loc);
        }
        break;
      }
      case AdjointValueKind::Concrete: {
        auto adjStruct = materializeAdjointDirect(std::move(av), loc);
        auto *dti = builder.createDestructureStruct(si->getLoc(), adjStruct);

        // Find the struct `TangentVector` type.
        auto structTy = remapType(si->getType()).getASTType();
#ifndef NDEBUG
        auto tangentVectorTy = getTangentSpace(structTy)->getCanonicalType();
        assert(!getTypeLowering(tangentVectorTy).isAddressOnly());
        assert(tangentVectorTy->getStructOrBoundGenericStruct());
#endif

        // Accumulate adjoints for the fields of the `struct` operand.
        unsigned fieldIndex = 0;
        for (auto it = structDecl->getStoredProperties().begin();
             it != structDecl->getStoredProperties().end();
             ++it, ++fieldIndex) {
          VarDecl *field = *it;
          if (field->getAttrs().hasAttribute<NoDerivativeAttr>())
            continue;
          // Find the corresponding field in the tangent space.
          auto *tanField = getTangentStoredProperty(
              getContext(), field, structTy, loc, getInvoker());
          if (!tanField) {
            errorOccurred = true;
            return;
          }
          auto tanElt = dti->getResult(fieldIndex);
          addAdjointValue(bb, si->getFieldValue(field),
                          makeConcreteAdjointValue(tanElt), si->getLoc());
        }
        break;
      }
      case AdjointValueKind::Aggregate: {
        // Note: All user-called initializations go through the calls to the
        // initializer, and synthesized initializers only have one level of
        // struct formation which will not result into any aggregate adjoint
        // values.
        llvm_unreachable(
            "Aggregate adjoint values should not occur for `struct` "
            "instructions");
      }
      }
      break;
    }
    case SILValueCategory::Address: {
      auto adjBuf = getAdjointBuffer(bb, si);
      // Find the struct `TangentVector` type.
      auto structTy = remapType(si->getType()).getASTType();
      // Accumulate adjoints for the fields of the `struct` operand.
      unsigned fieldIndex = 0;
      for (auto it = structDecl->getStoredProperties().begin();
           it != structDecl->getStoredProperties().end(); ++it, ++fieldIndex) {
        VarDecl *field = *it;
        if (field->getAttrs().hasAttribute<NoDerivativeAttr>())
          continue;
        // Find the corresponding field in the tangent space.
        auto *tanField = getTangentStoredProperty(getContext(), field, structTy,
                                                  loc, getInvoker());
        if (!tanField) {
          errorOccurred = true;
          return;
        }
        auto *adjFieldBuf =
            builder.createStructElementAddr(loc, adjBuf, tanField);
        auto fieldValue = si->getFieldValue(field);
        switch (getTangentValueCategory(fieldValue)) {
        case SILValueCategory::Object: {
          auto adjField = builder.emitLoadValueOperation(
              loc, adjFieldBuf, LoadOwnershipQualifier::Copy);
          recordTemporary(adjField);
          addAdjointValue(bb, fieldValue, makeConcreteAdjointValue(adjField),
                          loc);
          break;
        }
        case SILValueCategory::Address: {
          addToAdjointBuffer(bb, fieldValue, adjFieldBuf, loc);
          break;
        }
        }
      }
    } break;
    }
  }

  /// Handle `struct_extract` instruction.
  ///   Original: y = struct_extract x, #field
  ///    Adjoint: adj[x] += struct (0, ..., #field': adj[y], ..., 0)
  ///                                       ^~~~~~~
  ///                     field in tangent space corresponding to #field
  void visitStructExtractInst(StructExtractInst *sei) {
    auto *bb = sei->getParent();
    auto loc = getValidLocation(sei);
    // Find the corresponding field in the tangent space.
    auto structTy = remapType(sei->getOperand()->getType()).getASTType();
    auto *tanField =
        getTangentStoredProperty(getContext(), sei, structTy, getInvoker());
    // Check the `struct_extract` operand's value tangent category.
    switch (getTangentValueCategory(sei->getOperand())) {
    case SILValueCategory::Object: {
      auto tangentVectorTy = getTangentSpace(structTy)->getCanonicalType();
      auto *tangentVectorDecl =
          tangentVectorTy->getStructOrBoundGenericStruct();
      assert(tangentVectorDecl);
      auto tangentVectorSILTy =
          SILType::getPrimitiveObjectType(tangentVectorTy);
      assert(tanField && "Invalid projections should have been diagnosed");
      // Accumulate adjoint for the `struct_extract` operand.
      auto av = getAdjointValue(bb, sei);
      switch (av.getKind()) {
      case AdjointValueKind::Zero:
        addAdjointValue(bb, sei->getOperand(),
                        makeZeroAdjointValue(tangentVectorSILTy), loc);
        break;
      case AdjointValueKind::Concrete:
      case AdjointValueKind::Aggregate: {
        SmallVector<AdjointValue, 8> eltVals;
        for (auto *field : tangentVectorDecl->getStoredProperties()) {
          if (field == tanField) {
            eltVals.push_back(av);
          } else {
            auto substMap = tangentVectorTy->getMemberSubstitutionMap(
                field->getModuleContext(), field);
            auto fieldTy = field->getType().subst(substMap);
            auto fieldSILTy = getTypeLowering(fieldTy).getLoweredType();
            assert(fieldSILTy.isObject());
            eltVals.push_back(makeZeroAdjointValue(fieldSILTy));
          }
        }
        addAdjointValue(bb, sei->getOperand(),
                        makeAggregateAdjointValue(tangentVectorSILTy, eltVals),
                        loc);
      }
      }
      break;
    }
    case SILValueCategory::Address: {
      auto adjBase = getAdjointBuffer(bb, sei->getOperand());
      auto *adjBaseElt =
          builder.createStructElementAddr(loc, adjBase, tanField);
      // Check the `struct_extract`'s value tangent category.
      switch (getTangentValueCategory(sei)) {
      case SILValueCategory::Object: {
        auto adjElt = getAdjointValue(bb, sei);
        auto concreteAdjElt = materializeAdjointDirect(adjElt, loc);
        auto concreteAdjEltCopy =
            builder.emitCopyValueOperation(loc, concreteAdjElt);
        auto *alloc = builder.createAllocStack(loc, adjElt.getType());
        builder.emitStoreValueOperation(loc, concreteAdjEltCopy, alloc,
                                        StoreOwnershipQualifier::Init);
        builder.emitInPlaceAdd(loc, adjBaseElt, alloc);
        builder.createDestroyAddr(loc, alloc);
        builder.createDeallocStack(loc, alloc);
        break;
      }
      case SILValueCategory::Address: {
        auto adjElt = getAdjointBuffer(bb, sei);
        builder.emitInPlaceAdd(loc, adjBaseElt, adjElt);
        break;
      }
      }
      break;
    }
    }
  }

  /// Handle `ref_element_addr` instruction.
  ///   Original: y = ref_element_addr x, <n>
  ///    Adjoint: adj[x] += struct (0, ..., #field': adj[y], ..., 0)
  ///                                       ^~~~~~~
  ///                     field in tangent space corresponding to #field
  void visitRefElementAddrInst(RefElementAddrInst *reai) {
    auto *bb = reai->getParent();
    auto loc = reai->getLoc();
    auto adjBuf = getAdjointBuffer(bb, reai);
    auto classOperand = reai->getOperand();
    auto classType = remapType(reai->getOperand()->getType()).getASTType();
    auto *tanField =
        getTangentStoredProperty(getContext(), reai, classType, getInvoker());
    assert(tanField && "Invalid projections should have been diagnosed");
    switch (getTangentValueCategory(classOperand)) {
    case SILValueCategory::Object: {
      auto classTy = remapType(classOperand->getType()).getASTType();
      auto tangentVectorTy = getTangentSpace(classTy)->getCanonicalType();
      auto tangentVectorSILTy =
          SILType::getPrimitiveObjectType(tangentVectorTy);
      auto *tangentVectorDecl =
          tangentVectorTy->getStructOrBoundGenericStruct();
      // Accumulate adjoint for the `ref_element_addr` operand.
      SmallVector<AdjointValue, 8> eltVals;
      for (auto *field : tangentVectorDecl->getStoredProperties()) {
        if (field == tanField) {
          auto adjElt = builder.emitLoadValueOperation(
              reai->getLoc(), adjBuf, LoadOwnershipQualifier::Copy);
          eltVals.push_back(makeConcreteAdjointValue(adjElt));
          recordTemporary(adjElt);
        } else {
          auto substMap = tangentVectorTy->getMemberSubstitutionMap(
              field->getModuleContext(), field);
          auto fieldTy = field->getType().subst(substMap);
          auto fieldSILTy = getTypeLowering(fieldTy).getLoweredType();
          assert(fieldSILTy.isObject());
          eltVals.push_back(makeZeroAdjointValue(fieldSILTy));
        }
      }
      addAdjointValue(bb, classOperand,
                      makeAggregateAdjointValue(tangentVectorSILTy, eltVals),
                      loc);
      break;
    }
    case SILValueCategory::Address: {
      auto adjBufClass = getAdjointBuffer(bb, classOperand);
      auto adjBufElt =
          builder.createStructElementAddr(loc, adjBufClass, tanField);
      builder.emitInPlaceAdd(loc, adjBufElt, adjBuf);
      break;
    }
    }
  }

  /// Handle `tuple` instruction.
  ///   Original: y = tuple (x0, x1, x2, ...)
  ///    Adjoint: (adj[x0], adj[x1], adj[x2], ...) += destructure_tuple adj[y]
  ///                                         ^~~
  ///                         excluding non-differentiable elements
  void visitTupleInst(TupleInst *ti) {
    auto *bb = ti->getParent();
    auto loc = ti->getLoc();
    switch (getTangentValueCategory(ti)) {
    case SILValueCategory::Object: {
      auto av = getAdjointValue(bb, ti);
      switch (av.getKind()) {
      case AdjointValueKind::Zero:
        for (auto elt : ti->getElements()) {
          if (!getTangentSpace(elt->getType().getASTType()))
            continue;
          addAdjointValue(
              bb, elt,
              makeZeroAdjointValue(getRemappedTangentType(elt->getType())),
              loc);
        }
        break;
      case AdjointValueKind::Concrete: {
        auto adjVal = av.getConcreteValue();
        auto adjValCopy = builder.emitCopyValueOperation(loc, adjVal);
        SmallVector<SILValue, 4> adjElts;
        if (!adjVal->getType().getAs<TupleType>()) {
          recordTemporary(adjValCopy);
          adjElts.push_back(adjValCopy);
        } else {
          auto *dti = builder.createDestructureTuple(loc, adjValCopy);
          for (auto adjElt : dti->getResults())
            recordTemporary(adjElt);
          adjElts.append(dti->getResults().begin(), dti->getResults().end());
        }
        // Accumulate adjoints for `tuple` operands, skipping the
        // non-`Differentiable` ones.
        unsigned adjIndex = 0;
        for (auto i : range(ti->getNumOperands())) {
          if (!getTangentSpace(ti->getOperand(i)->getType().getASTType()))
            continue;
          auto adjElt = adjElts[adjIndex++];
          addAdjointValue(bb, ti->getOperand(i),
                          makeConcreteAdjointValue(adjElt), loc);
        }
        break;
      }
      case AdjointValueKind::Aggregate:
        unsigned adjIndex = 0;
        for (auto i : range(ti->getElements().size())) {
          if (!getTangentSpace(ti->getElement(i)->getType().getASTType()))
            continue;
          addAdjointValue(bb, ti->getElement(i),
                          av.getAggregateElement(adjIndex++), loc);
        }
        break;
      }
      break;
    }
    case SILValueCategory::Address: {
      auto adjBuf = getAdjointBuffer(bb, ti);
      // Accumulate adjoints for `tuple` operands, skipping the
      // non-`Differentiable` ones.
      unsigned adjIndex = 0;
      for (auto i : range(ti->getNumOperands())) {
        if (!getTangentSpace(ti->getOperand(i)->getType().getASTType()))
          continue;
        auto adjBufElt =
            builder.createTupleElementAddr(loc, adjBuf, adjIndex++);
        auto adjElt = getAdjointBuffer(bb, ti->getOperand(i));
        builder.emitInPlaceAdd(loc, adjElt, adjBufElt);
      }
      break;
    }
    }
  }

  /// Handle `tuple_extract` instruction.
  ///   Original: y = tuple_extract x, <n>
  ///    Adjoint: adj[x] += tuple (0, 0, ..., adj[y], ..., 0, 0)
  ///                                         ^~~~~~
  ///                            n'-th element, where n' is tuple tangent space
  ///                            index corresponding to n
  void visitTupleExtractInst(TupleExtractInst *tei) {
    auto *bb = tei->getParent();
    auto tupleTanTy = getRemappedTangentType(tei->getOperand()->getType());
    auto av = getAdjointValue(bb, tei);
    switch (av.getKind()) {
    case AdjointValueKind::Zero:
      addAdjointValue(bb, tei->getOperand(), makeZeroAdjointValue(tupleTanTy),
                      tei->getLoc());
      break;
    case AdjointValueKind::Aggregate:
    case AdjointValueKind::Concrete: {
      auto tupleTy = tei->getTupleType();
      auto tupleTanTupleTy = tupleTanTy.getAs<TupleType>();
      if (!tupleTanTupleTy) {
        addAdjointValue(bb, tei->getOperand(), av, tei->getLoc());
        break;
      }
      SmallVector<AdjointValue, 8> elements;
      unsigned adjIdx = 0;
      for (unsigned i : range(tupleTy->getNumElements())) {
        if (!getTangentSpace(
                tupleTy->getElement(i).getType()->getCanonicalType()))
          continue;
        if (tei->getFieldIndex() == i)
          elements.push_back(av);
        else
          elements.push_back(makeZeroAdjointValue(
              getRemappedTangentType(SILType::getPrimitiveObjectType(
                  tupleTanTupleTy->getElementType(adjIdx++)
                      ->getCanonicalType()))));
      }
      if (elements.size() == 1) {
        addAdjointValue(bb, tei->getOperand(), elements.front(), tei->getLoc());
        break;
      }
      addAdjointValue(bb, tei->getOperand(),
                      makeAggregateAdjointValue(tupleTanTy, elements),
                      tei->getLoc());
      break;
    }
    }
  }

  /// Handle `destructure_tuple` instruction.
  ///   Original: (y0, ..., yn) = destructure_tuple x
  ///    Adjoint: adj[x].0 += adj[y0]
  ///             ...
  ///             adj[x].n += adj[yn]
  void visitDestructureTupleInst(DestructureTupleInst *dti) {
    auto *bb = dti->getParent();
    auto loc = dti->getLoc();
    auto tupleTanTy = getRemappedTangentType(dti->getOperand()->getType());
    // Check the `destructure_tuple` operand's value tangent category.
    switch (getTangentValueCategory(dti->getOperand())) {
    case SILValueCategory::Object: {
      SmallVector<AdjointValue, 8> adjValues;
      for (auto origElt : dti->getResults()) {
        // Skip non-`Differentiable` tuple elements.
        if (!getTangentSpace(remapType(origElt->getType()).getASTType()))
          continue;
        adjValues.push_back(getAdjointValue(bb, origElt));
      }
      // Handle tuple tangent type.
      // Add adjoints for every tuple element that has a tangent space.
      if (tupleTanTy.is<TupleType>()) {
        assert(adjValues.size() > 1);
        addAdjointValue(bb, dti->getOperand(),
                        makeAggregateAdjointValue(tupleTanTy, adjValues), loc);
      }
      // Handle non-tuple tangent type.
      // Add adjoint for the single tuple element that has a tangent space.
      else {
        assert(adjValues.size() == 1);
        addAdjointValue(bb, dti->getOperand(), adjValues.front(), loc);
      }
      break;
    }
    case SILValueCategory::Address: {
      auto adjBuf = getAdjointBuffer(bb, dti->getOperand());
      unsigned adjIndex = 0;
      for (auto origElt : dti->getResults()) {
        // Skip non-`Differentiable` tuple elements.
        if (!getTangentSpace(remapType(origElt->getType()).getASTType()))
          continue;
        // Handle tuple tangent type.
        // Add adjoints for every tuple element that has a tangent space.
        if (tupleTanTy.is<TupleType>()) {
          auto adjEltBuf = getAdjointBuffer(bb, origElt);
          auto adjBufElt =
              builder.createTupleElementAddr(loc, adjBuf, adjIndex);
          builder.emitInPlaceAdd(loc, adjBufElt, adjEltBuf);
        }
        // Handle non-tuple tangent type.
        // Add adjoint for the single tuple element that has a tangent space.
        else {
          auto adjEltBuf = getAdjointBuffer(bb, origElt);
          addToAdjointBuffer(bb, dti->getOperand(), adjEltBuf, loc);
        }
        ++adjIndex;
      }
      break;
    }
    }
  }

  /// Handle `load` or `load_borrow` instruction
  ///   Original: y = load/load_borrow x
  ///    Adjoint: adj[x] += adj[y]
  void visitLoadOperation(SingleValueInstruction *inst) {
    assert(isa<LoadInst>(inst) || isa<LoadBorrowInst>(inst));
    auto *bb = inst->getParent();
    auto loc = inst->getLoc();
    switch (getTangentValueCategory(inst)) {
    case SILValueCategory::Object: {
      auto adjVal = materializeAdjointDirect(getAdjointValue(bb, inst), loc);
      // Allocate a local buffer and store the adjoint value. This buffer will
      // be used for accumulation into the adjoint buffer.
      auto adjBuf = builder.createAllocStack(
          loc, adjVal->getType(), SILDebugVariable());
      auto copy = builder.emitCopyValueOperation(loc, adjVal);
      builder.emitStoreValueOperation(loc, copy, adjBuf,
                                      StoreOwnershipQualifier::Init);
      // Accumulate the adjoint value in the local buffer into the adjoint
      // buffer.
      addToAdjointBuffer(bb, inst->getOperand(0), adjBuf, loc);
      builder.emitDestroyAddr(loc, adjBuf);
      builder.createDeallocStack(loc, adjBuf);
      break;
    }
    case SILValueCategory::Address: {
      auto adjBuf = getAdjointBuffer(bb, inst);
      addToAdjointBuffer(bb, inst->getOperand(0), adjBuf, loc);
      break;
    }
    }
  }
  void visitLoadInst(LoadInst *li) { visitLoadOperation(li); }
  void visitLoadBorrowInst(LoadBorrowInst *lbi) { visitLoadOperation(lbi); }

  /// Handle `store` or `store_borrow` instruction.
  ///   Original: store/store_borrow x to y
  ///    Adjoint: adj[x] += load adj[y]; adj[y] = 0
  void visitStoreOperation(SILBasicBlock *bb, SILLocation loc, SILValue origSrc,
                           SILValue origDest) {
    auto adjBuf = getAdjointBuffer(bb, origDest);
    switch (getTangentValueCategory(origSrc)) {
    case SILValueCategory::Object: {
      auto adjVal = builder.emitLoadValueOperation(
          loc, adjBuf, LoadOwnershipQualifier::Take);
      recordTemporary(adjVal);
      addAdjointValue(bb, origSrc, makeConcreteAdjointValue(adjVal), loc);
      builder.emitZeroIntoBuffer(loc, adjBuf, IsInitialization);
      break;
    }
    case SILValueCategory::Address: {
      addToAdjointBuffer(bb, origSrc, adjBuf, loc);
      builder.emitZeroIntoBuffer(loc, adjBuf, IsNotInitialization);
      break;
    }
    }
  }
  void visitStoreInst(StoreInst *si) {
    visitStoreOperation(si->getParent(), si->getLoc(), si->getSrc(),
                        si->getDest());
  }
  void visitStoreBorrowInst(StoreBorrowInst *sbi) {
    visitStoreOperation(sbi->getParent(), sbi->getLoc(), sbi->getSrc(),
                        sbi);
  }

  /// Handle `copy_addr` instruction.
  ///   Original: copy_addr x to y
  ///    Adjoint: adj[x] += adj[y]; adj[y] = 0
  void visitCopyAddrInst(CopyAddrInst *cai) {
    auto *bb = cai->getParent();
    auto adjDest = getAdjointBuffer(bb, cai->getDest());
    addToAdjointBuffer(bb, cai->getSrc(), adjDest, cai->getLoc());
    builder.emitZeroIntoBuffer(cai->getLoc(), adjDest, IsNotInitialization);
  }

  /// Handle any ownership instruction that deals with values: copy_value,
  /// move_value, begin_borrow.
  ///   Original: y = copy_value x
  ///    Adjoint: adj[x] += adj[y]
  void visitValueOwnershipInst(SingleValueInstruction *svi) {
    assert(svi->getNumOperands() == 1);
    auto *bb = svi->getParent();
    switch (getTangentValueCategory(svi)) {
    case SILValueCategory::Object: {
      auto adj = getAdjointValue(bb, svi);
      addAdjointValue(bb, svi->getOperand(0), adj, svi->getLoc());
      break;
    }
    case SILValueCategory::Address: {
      auto adjDest = getAdjointBuffer(bb, svi);
      addToAdjointBuffer(bb, svi->getOperand(0), adjDest, svi->getLoc());
      builder.emitZeroIntoBuffer(svi->getLoc(), adjDest, IsNotInitialization);
      break;
    }
    }
  }

  /// Handle `copy_value` instruction.
  ///   Original: y = copy_value x
  ///    Adjoint: adj[x] += adj[y]
  void visitCopyValueInst(CopyValueInst *cvi) { visitValueOwnershipInst(cvi); }

  /// Handle `begin_borrow` instruction.
  ///   Original: y = begin_borrow x
  ///    Adjoint: adj[x] += adj[y]
  void visitBeginBorrowInst(BeginBorrowInst *bbi) {
    visitValueOwnershipInst(bbi);
  }

  /// Handle `move_value` instruction.
  ///   Original: y = move_value x
  ///    Adjoint: adj[x] += adj[y]
  void visitMoveValueInst(MoveValueInst *mvi) { visitValueOwnershipInst(mvi); }

  /// Handle `begin_access` instruction.
  ///   Original: y = begin_access x
  ///    Adjoint: nothing
  void visitBeginAccessInst(BeginAccessInst *bai) {
    // Check for non-differentiable writes.
    if (bai->getAccessKind() == SILAccessKind::Modify) {
      if (isa<GlobalAddrInst>(bai->getSource())) {
        getContext().emitNondifferentiabilityError(
            bai, getInvoker(),
            diag::autodiff_cannot_differentiate_writes_to_global_variables);
        errorOccurred = true;
        return;
      }
      if (isa<ProjectBoxInst>(bai->getSource())) {
        getContext().emitNondifferentiabilityError(
            bai, getInvoker(),
            diag::autodiff_cannot_differentiate_writes_to_mutable_captures);
        errorOccurred = true;
        return;
      }
    }
  }

  /// Handle `unconditional_checked_cast_addr` instruction.
  ///   Original: y = unconditional_checked_cast_addr x
  ///    Adjoint: adj[x] += unconditional_checked_cast_addr adj[y]
  void visitUnconditionalCheckedCastAddrInst(
      UnconditionalCheckedCastAddrInst *uccai) {
    auto *bb = uccai->getParent();
    auto adjDest = getAdjointBuffer(bb, uccai->getDest());
    auto adjSrc = getAdjointBuffer(bb, uccai->getSrc());
    auto castBuf = builder.createAllocStack(uccai->getLoc(), adjSrc->getType());
    builder.createUnconditionalCheckedCastAddr(
        uccai->getLoc(), adjDest, adjDest->getType().getASTType(), castBuf,
        adjSrc->getType().getASTType());
    addToAdjointBuffer(bb, uccai->getSrc(), castBuf, uccai->getLoc());
    builder.emitDestroyAddrAndFold(uccai->getLoc(), castBuf);
    builder.createDeallocStack(uccai->getLoc(), castBuf);
    builder.emitZeroIntoBuffer(uccai->getLoc(), adjDest, IsInitialization);
  }

  /// Handle `unchecked_ref_cast` instruction.
  ///   Original: y = unchecked_ref_cast x
  ///    Adjoint: adj[x] += adj[y]
  ///             (assuming adj[x] and adj[y] have the same type)
  void visitUncheckedRefCastInst(UncheckedRefCastInst *urci) {
    auto *bb = urci->getParent();
    assert(urci->getOperand()->getType().isObject());
    assert(getRemappedTangentType(urci->getOperand()->getType()) ==
               getRemappedTangentType(urci->getType()) &&
           "Operand/result must have the same `TangentVector` type");
    switch (getTangentValueCategory(urci)) {
    case SILValueCategory::Object: {
      auto adj = getAdjointValue(bb, urci);
      addAdjointValue(bb, urci->getOperand(), adj, urci->getLoc());
      break;
    }
    case SILValueCategory::Address: {
      auto adjDest = getAdjointBuffer(bb, urci);
      addToAdjointBuffer(bb, urci->getOperand(), adjDest, urci->getLoc());
      builder.emitZeroIntoBuffer(urci->getLoc(), adjDest, IsNotInitialization);
      break;
    }
    }
  }

  /// Handle `upcast` instruction.
  ///   Original: y = upcast x
  ///    Adjoint: adj[x] += adj[y]
  ///             (assuming adj[x] and adj[y] have the same type)
  void visitUpcastInst(UpcastInst *ui) {
    auto *bb = ui->getParent();
    assert(ui->getOperand()->getType().isObject());
    assert(getRemappedTangentType(ui->getOperand()->getType()) ==
               getRemappedTangentType(ui->getType()) &&
           "Operand/result must have the same `TangentVector` type");
    switch (getTangentValueCategory(ui)) {
    case SILValueCategory::Object: {
      auto adj = getAdjointValue(bb, ui);
      addAdjointValue(bb, ui->getOperand(), adj, ui->getLoc());
      break;
    }
    case SILValueCategory::Address: {
      auto adjDest = getAdjointBuffer(bb, ui);
      addToAdjointBuffer(bb, ui->getOperand(), adjDest, ui->getLoc());
      builder.emitZeroIntoBuffer(ui->getLoc(), adjDest, IsNotInitialization);
      break;
    }
    }
  }

  /// Handle `unchecked_take_enum_data_addr` instruction.
  /// Currently, only `Optional`-typed operands are supported.
  ///   Original: y = unchecked_take_enum_data_addr x : $*Enum, #Enum.Case
  ///    Adjoint: adj[x] += $Enum.TangentVector(adj[y])
  void
  visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *utedai) {
    auto *bb = utedai->getParent();
    auto adjDest = getAdjointBuffer(bb, utedai);
    auto enumTy = utedai->getOperand()->getType();
    auto *optionalEnumDecl = getASTContext().getOptionalDecl();
    // Only `Optional`-typed operands are supported for now. Diagnose all other
    // enum operand types.
    if (enumTy.getASTType().getEnumOrBoundGenericEnum() != optionalEnumDecl) {
      LLVM_DEBUG(getADDebugStream()
                 << "Unhandled instruction in PullbackCloner: " << *utedai);
      getContext().emitNondifferentiabilityError(
          utedai, getInvoker(),
          diag::autodiff_expression_not_differentiable_note);
      errorOccurred = true;
      return;
    }
    accumulateAdjointForOptional(bb, utedai->getOperand(), adjDest);
    builder.emitZeroIntoBuffer(utedai->getLoc(), adjDest, IsNotInitialization);
  }

#define NOT_DIFFERENTIABLE(INST, DIAG) void visit##INST##Inst(INST##Inst *inst);
#undef NOT_DIFFERENTIABLE

#define NO_ADJOINT(INST)                                                       \
  void visit##INST##Inst(INST##Inst *inst) {}
  // Terminators.
  NO_ADJOINT(Return)
  NO_ADJOINT(Branch)
  NO_ADJOINT(CondBranch)

  // Address projections.
  NO_ADJOINT(StructElementAddr)
  NO_ADJOINT(TupleElementAddr)

  // Array literal initialization address projections.
  NO_ADJOINT(PointerToAddress)
  NO_ADJOINT(IndexAddr)

  // Memory allocation/access.
  NO_ADJOINT(AllocStack)
  NO_ADJOINT(DeallocStack)
  NO_ADJOINT(EndAccess)

  // Debugging/reference counting instructions.
  NO_ADJOINT(DebugValue)
  NO_ADJOINT(RetainValue)
  NO_ADJOINT(RetainValueAddr)
  NO_ADJOINT(ReleaseValue)
  NO_ADJOINT(ReleaseValueAddr)
  NO_ADJOINT(StrongRetain)
  NO_ADJOINT(StrongRelease)
  NO_ADJOINT(UnownedRetain)
  NO_ADJOINT(UnownedRelease)
  NO_ADJOINT(StrongRetainUnowned)
  NO_ADJOINT(DestroyValue)
  NO_ADJOINT(DestroyAddr)

  // Value ownership.
  NO_ADJOINT(EndBorrow)
#undef NO_ADJOINT
};

PullbackCloner::Implementation::Implementation(VJPCloner &vjpCloner)
    : vjpCloner(vjpCloner), scopeCloner(getPullback()),
      builder(getPullback(), getContext()),
      localAllocBuilder(getPullback(), getContext()) {
  // Get dominance and post-order info for the original function.
  auto &passManager = getContext().getPassManager();
  auto *domAnalysis = passManager.getAnalysis<DominanceAnalysis>();
  auto *postDomAnalysis = passManager.getAnalysis<PostDominanceAnalysis>();
  auto *postOrderAnalysis = passManager.getAnalysis<PostOrderAnalysis>();
  auto *original = &vjpCloner.getOriginal();
  domInfo = domAnalysis->get(original);
  postDomInfo = postDomAnalysis->get(original);
  postOrderInfo = postOrderAnalysis->get(original);
  // Initialize `originalExitBlock`.
  auto origExitIt = original->findReturnBB();
  assert(origExitIt != original->end() &&
         "Functions without returns must have been diagnosed");
  originalExitBlock = &*origExitIt;
  localAllocBuilder.setCurrentDebugScope(
       remapScope(originalExitBlock->getTerminator()->getDebugScope()));
}

PullbackCloner::PullbackCloner(VJPCloner &vjpCloner)
    : impl(*new Implementation(vjpCloner)) {}

PullbackCloner::~PullbackCloner() { delete &impl; }

//--------------------------------------------------------------------------//
// Entry point
//--------------------------------------------------------------------------//

bool PullbackCloner::run() {
  bool foundError = impl.run();
#ifndef NDEBUG
  if (!foundError)
    impl.getPullback().verify();
#endif
  return foundError;
}

bool PullbackCloner::Implementation::run() {
  PrettyStackTraceSILFunction trace("generating pullback for", &getOriginal());
  auto &original = getOriginal();
  auto &pullback = getPullback();
  auto pbLoc = getPullback().getLocation();
  LLVM_DEBUG(getADDebugStream() << "Running PullbackCloner on\n" << original);

  // Collect original formal results.
  SmallVector<SILValue, 8> origFormalResults;
  collectAllFormalResultsInTypeOrder(original, origFormalResults);
  for (auto resultIndex : getConfig().resultIndices->getIndices()) {
    auto origResult = origFormalResults[resultIndex];
    // If original result is non-varied, it will always have a zero derivative.
    // Skip full pullback generation and simply emit zero derivatives for wrt
    // parameters.
    //
    // NOTE(TF-876): This shortcut is currently necessary for functions
    // returning non-varied result with >1 basic block where some basic blocks
    // have no dominated active values; control flow differentiation does not
    // handle this case. See TF-876 for context.
    if (!getActivityInfo().isVaried(origResult, getConfig().parameterIndices)) {
      emitZeroDerivativesForNonvariedResult(origResult);
      return false;
    }
  }

  // Collect dominated active values in original basic blocks.
  // Adjoint values of dominated active values are passed as pullback block
  // arguments.
  DominanceOrder domOrder(original.getEntryBlock(), domInfo);
  // Keep track of visited values.
  SmallPtrSet<SILValue, 8> visited;
  while (auto *bb = domOrder.getNext()) {
    auto &bbActiveValues = activeValues[bb];
    // If the current block has an immediate dominator, append the immediate
    // dominator block's active values to the current block's active values.
    if (auto *domNode = domInfo->getNode(bb)->getIDom()) {
      auto &domBBActiveValues = activeValues[domNode->getBlock()];
      bbActiveValues.append(domBBActiveValues.begin(), domBBActiveValues.end());
    }
    // If `v` is active and has not been visited, records it as an active value
    // in the original basic block.
    // For active values unsupported by differentiation, emits a diagnostic and
    // returns true. Otherwise, returns false.
    auto recordValueIfActive = [&](SILValue v) -> bool {
      // If value is not active, skip.
      if (!getActivityInfo().isActive(v, getConfig()))
        return false;
      // If active value has already been visited, skip.
      if (visited.count(v))
        return false;
      // Mark active value as visited.
      visited.insert(v);

      // Diagnose unsupported active values.
      auto type = v->getType();
      // Do not emit remaining activity-related diagnostics for semantic member
      // accessors, which have special-case pullback generation.
      if (isSemanticMemberAccessor(&original))
        return false;
      // Diagnose active enum values. Differentiation of enum values requires
      // special adjoint value handling and is not yet supported. Diagnose
      // only the first active enum value to prevent too many diagnostics.
      //
      // Do not diagnose `Optional`-typed values, which will have special-case
      // differentiation support.
      if (auto *enumDecl = type.getEnumOrBoundGenericEnum()) {
        if (!type.getASTType()->isOptional()) {
          getContext().emitNondifferentiabilityError(
              v, getInvoker(), diag::autodiff_enums_unsupported);
          errorOccurred = true;
          return true;
        }
      }
      // Diagnose unsupported stored property projections.
      if (isa<StructExtractInst>(v) || isa<RefElementAddrInst>(v) ||
          isa<StructElementAddrInst>(v)) {
        auto *inst = cast<SingleValueInstruction>(v);
        assert(inst->getNumOperands() == 1);
        auto baseType = remapType(inst->getOperand(0)->getType()).getASTType();
        if (!getTangentStoredProperty(getContext(), inst, baseType,
                                      getInvoker())) {
          errorOccurred = true;
          return true;
        }
      }
      // Skip address projections.
      // Address projections do not need their own adjoint buffers; they
      // become projections into their adjoint base buffer.
      if (Projection::isAddressProjection(v))
        return false;
      // Record active value.
      bbActiveValues.push_back(v);
      return false;
    };
    // Record all active values in the basic block.
    for (auto *arg : bb->getArguments())
      if (recordValueIfActive(arg))
        return true;
    for (auto &inst : *bb) {
      for (auto op : inst.getOperandValues())
        if (recordValueIfActive(op))
          return true;
      for (auto result : inst.getResults())
        if (recordValueIfActive(result))
          return true;
    }
    domOrder.pushChildren(bb);
  }

  // Create pullback blocks and arguments, visiting original blocks using BFS
  // starting from the original exit block. Unvisited original basic blocks
  // (e.g unreachable blocks) are not relevant for pullback generation and thus
  // ignored.
  // The original blocks in traversal order for pullback generation.
  SmallVector<SILBasicBlock *, 8> originalBlocks;
  // The workqueue used for bookkeeping during the breadth-first traversal.
  BasicBlockWorkqueue workqueue = {originalExitBlock};

  // Perform BFS from the original exit block.
  {
    while (auto *BB = workqueue.pop()) {
      originalBlocks.push_back(BB);

      for (auto *nextBB : BB->getPredecessorBlocks()) {
        workqueue.pushIfNotVisited(nextBB);
      }
    }
  }

  for (auto *origBB : originalBlocks) {
    auto *pullbackBB = pullback.createBasicBlock();
    pullbackBBMap.insert({origBB, pullbackBB});
    auto pbTupleLoweredType =
        remapType(getPullbackInfo().getLinearMapTupleLoweredType(origBB));
    // If the BB is the original exit, then the pullback block that we just
    // created must be the pullback function's entry. For the pullback entry,
    // create entry arguments and continue to the next block.
    if (origBB == originalExitBlock) {
      assert(pullbackBB->isEntry());
      createEntryArguments(&pullback);
      auto *origTerm = originalExitBlock->getTerminator();
      builder.setCurrentDebugScope(remapScope(origTerm->getDebugScope()));
      builder.setInsertionPoint(pullbackBB);
      // Obtain the context object, if any, and the top-level subcontext, i.e.
      // the main pullback struct.
      if (getPullbackInfo().hasHeapAllocatedContext()) {
        // The last argument is the context object (`Builtin.NativeObject`).
        contextValue = pullbackBB->getArguments().back();
        assert(contextValue->getType() ==
               SILType::getNativeObjectType(getASTContext()));
        // Load the pullback context.
        auto subcontextAddr = emitProjectTopLevelSubcontext(
            builder, pbLoc, contextValue, pbTupleLoweredType);
        SILValue mainPullbackTuple = builder.createLoad(
            pbLoc, subcontextAddr,
            pbTupleLoweredType.isTrivial(getPullback()) ?
                LoadOwnershipQualifier::Trivial : LoadOwnershipQualifier::Copy);
        auto *dsi = builder.createDestructureTuple(pbLoc, mainPullbackTuple);
        initializePullbackTupleElements(origBB, dsi->getAllResults());
      } else {
        // Obtain and destructure pullback struct elements.
        unsigned numVals = pbTupleLoweredType.getAs<TupleType>()->getNumElements();
        initializePullbackTupleElements(origBB,
                                        pullbackBB->getArguments().take_back(numVals));
      }

      continue;
    }

    // Get all active values in the original block.
    // If the original block has no active values, continue.
    auto &bbActiveValues = activeValues[origBB];
    if (bbActiveValues.empty())
      continue;

    // Otherwise, if the original block has active values:
    // - For each active buffer in the original block, allocate a new local
    //   buffer in the pullback entry. (All adjoint buffers are allocated in
    //   the pullback entry and deallocated in the pullback exit.)
    // - For each active value in the original block, add adjoint value
    //   arguments to the pullback block.
    for (auto activeValue : bbActiveValues) {
      // Handle the active value based on its value category.
      switch (getTangentValueCategory(activeValue)) {
      case SILValueCategory::Address: {
        // Allocate and zero initialize a new local buffer using
        // `getAdjointBuffer`.
        builder.setCurrentDebugScope(
            remapScope(originalExitBlock->getTerminator()->getDebugScope()));
        builder.setInsertionPoint(pullback.getEntryBlock());
        getAdjointBuffer(origBB, activeValue);
        break;
      }
      case SILValueCategory::Object: {
        // Create and register pullback block argument for the active value.
        auto *pullbackArg = pullbackBB->createPhiArgument(
            getRemappedTangentType(activeValue->getType()),
            OwnershipKind::Owned);
        activeValuePullbackBBArgumentMap[{origBB, activeValue}] = pullbackArg;
        recordTemporary(pullbackArg);
        break;
      }
      }
    }
    // Add a pullback tuple argument.
    auto *pbTupleArg = pullbackBB->createPhiArgument(pbTupleLoweredType,
                                                     OwnershipKind::Owned);
    // Destructure the pullback struct to get the elements.
    builder.setCurrentDebugScope(
        remapScope(origBB->getTerminator()->getDebugScope()));
    builder.setInsertionPoint(pullbackBB);
    auto *dsi = builder.createDestructureTuple(pbLoc, pbTupleArg);
    initializePullbackTupleElements(origBB, dsi->getResults());

    // - Create pullback trampoline blocks for each successor block of the
    //   original block. Pullback trampoline blocks only have a pullback
    //   struct argument. They branch from a pullback successor block to the
    //   pullback original block, passing adjoint values of active values.
    for (auto *succBB : origBB->getSuccessorBlocks()) {
      // Skip generating pullback block for original unreachable blocks.
      if (!workqueue.isVisited(succBB))
        continue;
      auto *pullbackTrampolineBB = pullback.createBasicBlockBefore(pullbackBB);
      pullbackTrampolineBBMap.insert({{origBB, succBB}, pullbackTrampolineBB});
      // Get the enum element type (i.e. the pullback struct type). The enum
      // element type may be boxed if the enum is indirect.
      auto enumLoweredTy =
          getPullbackInfo().getBranchingTraceEnumLoweredType(succBB);
      auto *enumEltDecl =
          getPullbackInfo().lookUpBranchingTraceEnumElement(origBB, succBB);
      auto enumEltType = remapType(enumLoweredTy.getEnumElementType(
          enumEltDecl, getModule(), TypeExpansionContext::minimal()));
      pullbackTrampolineBB->createPhiArgument(enumEltType,
                                              OwnershipKind::Owned);
    }
  }

  auto *pullbackEntry = pullback.getEntryBlock();
  auto pbTupleLoweredType =
    remapType(getPullbackInfo().getLinearMapTupleLoweredType(originalExitBlock));
  unsigned numVals = (getPullbackInfo().hasHeapAllocatedContext() ?
                      1 : pbTupleLoweredType.getAs<TupleType>()->getNumElements());
  (void)numVals;

  // The pullback function has type:
  // `(seed0, seed1, ..., (exit_pb_tuple_el0, ..., )|context_obj) -> (d_arg0, ..., d_argn)`.
  auto pbParamArgs = pullback.getArgumentsWithoutIndirectResults();
  assert(getConfig().resultIndices->getNumIndices() == pbParamArgs.size() - numVals &&
         pbParamArgs.size() >= 1);
  // Assign adjoints for original result.
  builder.setCurrentDebugScope(
      remapScope(originalExitBlock->getTerminator()->getDebugScope()));
  builder.setInsertionPoint(pullbackEntry,
                            getNextFunctionLocalAllocationInsertionPoint());
  unsigned seedIndex = 0;
  for (auto resultIndex : getConfig().resultIndices->getIndices()) {
    auto origResult = origFormalResults[resultIndex];
    auto *seed = pbParamArgs[seedIndex];
    if (seed->getType().isAddress()) {
      // If the seed argument is an `inout` parameter, assign it directly as
      // the adjoint buffer of the original result.
      auto seedParamInfo =
          pullback.getLoweredFunctionType()->getParameters()[seedIndex];
      if (seedParamInfo.isIndirectInOut()) {
        setAdjointBuffer(originalExitBlock, origResult, seed);
      }
      // Otherwise, assign a copy of the seed argument as the adjoint buffer of
      // the original result.
      else {
        auto *seedBufCopy =
            createFunctionLocalAllocation(seed->getType(), pbLoc);
        builder.createCopyAddr(pbLoc, seed, seedBufCopy, IsNotTake,
                               IsInitialization);
        setAdjointBuffer(originalExitBlock, origResult, seedBufCopy);
        LLVM_DEBUG(getADDebugStream()
                   << "Assigned seed buffer " << *seedBufCopy
                   << " as the adjoint of original indirect result "
                   << origResult);
      }
    } else {
      addAdjointValue(originalExitBlock, origResult, makeConcreteAdjointValue(seed),
                      pbLoc);
      LLVM_DEBUG(getADDebugStream()
                 << "Assigned seed " << *seed
                 << " as the adjoint of original result " << origResult);
    }
    ++seedIndex;
  }

  // If the original function is an accessor with special-case pullback
  // generation logic, do special-case generation.
  if (isSemanticMemberAccessor(&original)) {
    if (runForSemanticMemberAccessor())
      return true;
  }
  // Otherwise, perform standard pullback generation.
  // Visit original blocks in post-order and perform differentiation
  // in corresponding pullback blocks. If errors occurred, back out.
  else {
    for (auto *bb : originalBlocks) {
      visitSILBasicBlock(bb);
      if (errorOccurred)
        return true;
    }
  }

  // Prepare and emit a `return` in the pullback exit block.
  auto *origEntry = getOriginal().getEntryBlock();
  auto *pbExit = getPullbackBlock(origEntry);
  builder.setCurrentDebugScope(pbExit->back().getDebugScope());
  builder.setInsertionPoint(pbExit);

  // This vector will contain all the materialized return elements.
  SmallVector<SILValue, 8> retElts;
  // This vector will contain all indirect parameter adjoint buffers.
  SmallVector<SILValue, 4> indParamAdjoints;
  // This vector will identify the locations where initialization is needed.
  SmallBitVector outputsToInitialize;

  auto conv = getOriginal().getConventions();
  auto origParams = getOriginal().getArgumentsWithoutIndirectResults();

  // Materializes the return element corresponding to the parameter
  // `parameterIndex` into the `retElts` vector.
  auto addRetElt = [&](unsigned parameterIndex) -> void {
    auto origParam = origParams[parameterIndex];
    switch (getTangentValueCategory(origParam)) {
    case SILValueCategory::Object: {
      auto pbVal = getAdjointValue(origEntry, origParam);
      auto val = materializeAdjointDirect(pbVal, pbLoc);
      auto newVal = builder.emitCopyValueOperation(pbLoc, val);
      retElts.push_back(newVal);
      break;
    }
    case SILValueCategory::Address: {
      auto adjBuf = getAdjointBuffer(origEntry, origParam);
      indParamAdjoints.push_back(adjBuf);
      outputsToInitialize.push_back(
        !conv.getParameters()[parameterIndex].isIndirectMutating());
      break;
    }
    }
  };
  SmallVector<SILArgument *, 4> pullbackIndirectResults(
        getPullback().getIndirectResults().begin(),
        getPullback().getIndirectResults().end());

  // Collect differentiation parameter adjoints.
  // Do a first pass to collect non-inout values.
  for (auto i : getConfig().parameterIndices->getIndices()) {
    if (!conv.getParameters()[i].isIndirectMutating()) {
       addRetElt(i);
     }
  }

  // Do a second pass for all inout parameters, however this is only necessary
  // for functions with multiple basic blocks.  For functions with a single
  // basic block adjoint accumulation for those parameters is already done by
  // per-instruction visitors.
  if (getOriginal().size() > 1) {
    const auto &pullbackConv = pullback.getConventions();
    SmallVector<SILArgument *, 1> pullbackInOutArgs;
    for (auto pullbackArg : enumerate(pullback.getArgumentsWithoutIndirectResults())) {
      if (pullbackConv.getParameters()[pullbackArg.index()].isIndirectMutating())
        pullbackInOutArgs.push_back(pullbackArg.value());
    }

    unsigned pullbackInoutArgumentIdx = 0;
    for (auto i : getConfig().parameterIndices->getIndices()) {
      // Skip non-inout parameters.
      if (!conv.getParameters()[i].isIndirectMutating())
        continue;

      // For functions with multiple basic blocks, accumulation is needed
      // for `inout` parameters because pullback basic blocks have different
      // adjoint buffers.
      pullbackIndirectResults.push_back(pullbackInOutArgs[pullbackInoutArgumentIdx++]);
      addRetElt(i);
    }
  }

  // Copy them to adjoint indirect results.
  assert(indParamAdjoints.size() == pullbackIndirectResults.size() &&
         "Indirect parameter adjoint count mismatch");
  unsigned currentIndex = 0;
  for (auto pair : zip(indParamAdjoints, pullbackIndirectResults)) {
    auto source = std::get<0>(pair);
    auto *dest = std::get<1>(pair);
    if (outputsToInitialize[currentIndex]) {
      builder.createCopyAddr(pbLoc, source, dest, IsTake, IsInitialization);
    } else {
      builder.createCopyAddr(pbLoc, source, dest, IsTake, IsNotInitialization);
    }
    currentIndex++;
    // Prevent source buffer from being deallocated, since the underlying
    // value is moved.
    destroyedLocalAllocations.insert(source);
  }

  // Emit cleanups for all local values.
  cleanUpTemporariesForBlock(pbExit, pbLoc);
  // Deallocate local allocations.
  for (auto alloc : functionLocalAllocations) {
    // Assert that local allocations have at least one use.
    // Buffers should not be allocated needlessly.
    assert(!alloc->use_empty());
    if (!destroyedLocalAllocations.count(alloc)) {
      builder.emitDestroyAddrAndFold(pbLoc, alloc);
      destroyedLocalAllocations.insert(alloc);
    }
    builder.createDeallocStack(pbLoc, alloc);
  }
  builder.createReturn(pbLoc, joinElements(retElts, builder, pbLoc));

#ifndef NDEBUG
  bool leakFound = false;
  // Ensure all temporaries have been cleaned up.
  for (auto &bb : pullback) {
    for (auto temp : blockTemporaries[&bb]) {
      if (blockTemporaries[&bb].count(temp)) {
        leakFound = true;
        getADDebugStream() << "Found leaked temporary:\n" << temp;
      }
    }
  }
  // Ensure all local allocations have been cleaned up.
  for (auto localAlloc : functionLocalAllocations) {
    if (!destroyedLocalAllocations.count(localAlloc)) {
      leakFound = true;
      getADDebugStream() << "Found leaked local buffer:\n" << localAlloc;
    }
  }
  assert(!leakFound && "Leaks found!");
#endif

  LLVM_DEBUG(getADDebugStream()
             << "Generated pullback for " << original.getName() << ":\n"
             << pullback);
  return errorOccurred;
}

void PullbackCloner::Implementation::emitZeroDerivativesForNonvariedResult(
    SILValue origNonvariedResult) {
  auto &pullback = getPullback();
  auto pbLoc = getPullback().getLocation();
  /*
  // TODO(TF-788): Re-enable non-varied result warning.
  // Emit fixit if original non-varied result has a valid source location.
  auto startLoc = origNonvariedResult.getLoc().getStartSourceLoc();
  auto endLoc = origNonvariedResult.getLoc().getEndSourceLoc();
  if (startLoc.isValid() && endLoc.isValid()) {
    getContext().diagnose(startLoc, diag::autodiff_nonvaried_result_fixit)
        .fixItInsert(startLoc, "withoutDerivative(at:")
        .fixItInsertAfter(endLoc, ")");
  }
  */
  LLVM_DEBUG(getADDebugStream() << getOriginal().getName()
                                << " has non-varied result, returning zero"
                                   " for all pullback results\n");
  auto *pullbackEntry = pullback.createBasicBlock();
  createEntryArguments(&pullback);
  builder.setCurrentDebugScope(
      remapScope(originalExitBlock->getTerminator()->getDebugScope()));
  builder.setInsertionPoint(pullbackEntry);
  // Destroy all owned arguments.
  for (auto *arg : pullbackEntry->getArguments())
    if (arg->getOwnershipKind() == OwnershipKind::Owned)
      builder.emitDestroyOperation(pbLoc, arg);
  // Return zero for each result.
  SmallVector<SILValue, 4> directResults;
  auto indirectResultIt = pullback.getIndirectResults().begin();
  for (auto resultInfo : pullback.getLoweredFunctionType()->getResults()) {
    auto resultType =
        pullback.mapTypeIntoContext(resultInfo.getInterfaceType())
            ->getCanonicalType();
    if (resultInfo.isFormalDirect())
      directResults.push_back(builder.emitZero(pbLoc, resultType));
    else
      builder.emitZeroIntoBuffer(pbLoc, *indirectResultIt++, IsInitialization);
  }
  builder.createReturn(pbLoc, joinElements(directResults, builder, pbLoc));
  LLVM_DEBUG(getADDebugStream()
             << "Generated pullback for " << getOriginal().getName() << ":\n"
             << pullback);
}

void PullbackCloner::Implementation::accumulateAdjointForOptional(
    SILBasicBlock *bb, SILValue optionalValue, SILValue wrappedAdjoint) {
  auto pbLoc = getPullback().getLocation();
  // Handle `switch_enum` on `Optional`.
  // `Optional<T>`
  auto optionalTy = remapType(optionalValue->getType());
  assert(optionalTy.getASTType()->isOptional());
  // `T`
  auto wrappedType = optionalTy.getOptionalObjectType();
  // `T.TangentVector`
  auto wrappedTanType = remapType(wrappedAdjoint->getType());
  // `Optional<T.TangentVector>`
  auto optionalOfWrappedTanType = SILType::getOptionalType(wrappedTanType);
  // `Optional<T>.TangentVector`
  auto optionalTanTy = getRemappedTangentType(optionalTy);
  auto *optionalTanDecl = optionalTanTy.getNominalOrBoundGenericNominal();
  // Look up the `Optional<T>.TangentVector.init` declaration.
  auto initLookup =
      optionalTanDecl->lookupDirect(DeclBaseName::createConstructor());
  ConstructorDecl *constructorDecl = nullptr;
  for (auto *candidate : initLookup) {
    auto candidateModule = candidate->getModuleContext();
    if (candidateModule->getName() ==
            builder.getASTContext().Id_Differentiation ||
        candidateModule->isStdlibModule()) {
      assert(!constructorDecl && "Multiple `Optional.TangentVector.init`s");
      constructorDecl = cast<ConstructorDecl>(candidate);
#ifdef NDEBUG
      break;
#endif
    }
  }
  assert(constructorDecl && "No `Optional.TangentVector.init`");

  // Allocate a local buffer for the `Optional` adjoint value.
  auto *optTanAdjBuf = builder.createAllocStack(pbLoc, optionalTanTy);
  // Find `Optional<T.TangentVector>.some` EnumElementDecl.
  auto someEltDecl = builder.getASTContext().getOptionalSomeDecl();

  // Initialize an `Optional<T.TangentVector>` buffer from `wrappedAdjoint` as
  // the input for `Optional<T>.TangentVector.init`.
  auto *optArgBuf = builder.createAllocStack(pbLoc, optionalOfWrappedTanType);
  if (optionalOfWrappedTanType.isLoadableOrOpaque(builder.getFunction())) {
    // %enum = enum $Optional<T.TangentVector>, #Optional.some!enumelt,
    //         %wrappedAdjoint : $T
    auto *enumInst = builder.createEnum(pbLoc, wrappedAdjoint, someEltDecl,
                                        optionalOfWrappedTanType);
    // store %enum to %optArgBuf
    builder.emitStoreValueOperation(pbLoc, enumInst, optArgBuf,
                                    StoreOwnershipQualifier::Init);
  } else {
    // %enumAddr = init_enum_data_addr %optArgBuf $Optional<T.TangentVector>,
    //                                 #Optional.some!enumelt
    auto *enumAddr = builder.createInitEnumDataAddr(
        pbLoc, optArgBuf, someEltDecl, wrappedTanType.getAddressType());
    // copy_addr %wrappedAdjoint to [init] %enumAddr
    builder.createCopyAddr(pbLoc, wrappedAdjoint, enumAddr, IsNotTake,
                           IsInitialization);
    // inject_enum_addr %optArgBuf : $*Optional<T.TangentVector>,
    //                  #Optional.some!enumelt
    builder.createInjectEnumAddr(pbLoc, optArgBuf, someEltDecl);
  }

  // Apply `Optional<T>.TangentVector.init`.
  SILOptFunctionBuilder fb(getContext().getTransform());
  // %init_fn = function_ref @Optional<T>.TangentVector.init
  auto *initFn = fb.getOrCreateFunction(pbLoc, SILDeclRef(constructorDecl),
                                        NotForDefinition);
  auto *initFnRef = builder.createFunctionRef(pbLoc, initFn);
  auto *diffProto =
      builder.getASTContext().getProtocol(KnownProtocolKind::Differentiable);
  auto *swiftModule = getModule().getSwiftModule();
  auto diffConf =
      swiftModule->lookupConformance(wrappedType.getASTType(), diffProto);
  assert(!diffConf.isInvalid() && "Missing conformance to `Differentiable`");
  auto subMap = SubstitutionMap::get(
      initFn->getLoweredFunctionType()->getSubstGenericSignature(),
      ArrayRef<Type>(wrappedType.getASTType()), {diffConf});
  // %metatype = metatype $Optional<T>.TangentVector.Type
  auto metatypeType = CanMetatypeType::get(optionalTanTy.getASTType(),
                                           MetatypeRepresentation::Thin);
  auto metatypeSILType = SILType::getPrimitiveObjectType(metatypeType);
  auto metatype = builder.createMetatype(pbLoc, metatypeSILType);
  // apply %init_fn(%optTanAdjBuf, %optArgBuf, %metatype)
  builder.createApply(pbLoc, initFnRef, subMap,
                      {optTanAdjBuf, optArgBuf, metatype});
  builder.createDeallocStack(pbLoc, optArgBuf);

  // Accumulate adjoint for the incoming `Optional` value.
  addToAdjointBuffer(bb, optionalValue, optTanAdjBuf, pbLoc);
  builder.emitDestroyAddr(pbLoc, optTanAdjBuf);
  builder.createDeallocStack(pbLoc, optTanAdjBuf);
}

SILBasicBlock *PullbackCloner::Implementation::buildPullbackSuccessor(
    SILBasicBlock *origBB, SILBasicBlock *origPredBB,
    SmallDenseMap<SILValue, TrampolineBlockSet> &pullbackTrampolineBlockMap) {
  // Get the pullback block and optional pullback trampoline block of the
  // predecessor block.
  auto *pullbackBB = getPullbackBlock(origPredBB);
  auto *pullbackTrampolineBB = getPullbackTrampolineBlock(origPredBB, origBB);
  // If the predecessor block does not have a corresponding pullback
  // trampoline block, then the pullback successor is the pullback block.
  if (!pullbackTrampolineBB)
    return pullbackBB;

  // Otherwise, the pullback successor is the pullback trampoline block,
  // which branches to the pullback block and propagates adjoint values of
  // active values.
  assert(pullbackTrampolineBB->getNumArguments() == 1);
  auto loc = origBB->getParent()->getLocation();
  SmallVector<SILValue, 8> trampolineArguments;

  // Propagate adjoint values/buffers of active values/buffers to
  // predecessor blocks.
  auto &predBBActiveValues = activeValues[origPredBB];
  llvm::SmallSet<std::pair<SILValue, SILValue>, 32> propagatedAdjoints;
  for (auto activeValue : predBBActiveValues) {
    LLVM_DEBUG(getADDebugStream()
               << "Propagating adjoint of active value " << activeValue
               << "from bb" << origBB->getDebugID()
               << " to predecessors' (bb" << origPredBB->getDebugID()
               << ") pullback blocks\n");
    switch (getTangentValueCategory(activeValue)) {
    case SILValueCategory::Object: {
      auto activeValueAdj = getAdjointValue(origBB, activeValue);
      auto concreteActiveValueAdj =
          materializeAdjointDirect(activeValueAdj, loc);

      if (!pullbackTrampolineBlockMap.count(concreteActiveValueAdj)) {
        concreteActiveValueAdj =
            builder.emitCopyValueOperation(loc, concreteActiveValueAdj);
        setAdjointValue(origBB, activeValue,
                        makeConcreteAdjointValue(concreteActiveValueAdj));
      }
      auto insertion = pullbackTrampolineBlockMap.try_emplace(
          concreteActiveValueAdj, TrampolineBlockSet());
      auto &blockSet = insertion.first->getSecond();
      blockSet.insert(pullbackTrampolineBB);
      trampolineArguments.push_back(concreteActiveValueAdj);

      // If the pullback block does not yet have a registered adjoint
      // value for the active value, set the adjoint value to the
      // forwarded adjoint value argument.
      // TODO: Hoist this logic out of loop over predecessor blocks to
      // remove the `hasAdjointValue` check.
      if (!hasAdjointValue(origPredBB, activeValue)) {
        auto *pullbackBBArg =
            getActiveValuePullbackBlockArgument(origPredBB, activeValue);
        auto forwardedArgAdj = makeConcreteAdjointValue(pullbackBBArg);
        setAdjointValue(origPredBB, activeValue, forwardedArgAdj);
      }
      break;
    }
    case SILValueCategory::Address: {
      // Propagate adjoint buffers using `copy_addr`.
      auto adjBuf = getAdjointBuffer(origBB, activeValue);
      auto predAdjBuf = getAdjointBuffer(origPredBB, activeValue);
      if (propagatedAdjoints.insert({adjBuf, predAdjBuf}).second)
        builder.createCopyAddr(loc, adjBuf, predAdjBuf, IsNotTake,
                               IsNotInitialization);
      break;
    }
    }
  }

  // Propagate pullback struct argument.
  TangentBuilder pullbackTrampolineBBBuilder(
      pullbackTrampolineBB, getContext());
  pullbackTrampolineBBBuilder.setCurrentDebugScope(
      remapScope(origPredBB->getTerminator()->getDebugScope()));

  auto *pullbackTrampolineBBArg = pullbackTrampolineBB->getArguments().front();
  if (vjpCloner.getLoopInfo()->getLoopFor(origPredBB)) {
    assert(pullbackTrampolineBBArg->getType() ==
               SILType::getRawPointerType(getASTContext()));
    auto pbTupleType =
      remapType(getPullbackInfo().getLinearMapTupleLoweredType(origPredBB));
    auto predPbTupleAddr = pullbackTrampolineBBBuilder.createPointerToAddress(
        loc, pullbackTrampolineBBArg, pbTupleType.getAddressType(),
        /*isStrict*/ true);
    auto predPbStructVal = pullbackTrampolineBBBuilder.createLoad(
        loc, predPbTupleAddr,
        pbTupleType.isTrivial(getPullback()) ?
            LoadOwnershipQualifier::Trivial : LoadOwnershipQualifier::Copy);
    trampolineArguments.push_back(predPbStructVal);
  } else {
    trampolineArguments.push_back(pullbackTrampolineBBArg);
  }
  // Branch from pullback trampoline block to pullback block.
  pullbackTrampolineBBBuilder.createBranch(loc, pullbackBB,
                                           trampolineArguments);
  return pullbackTrampolineBB;
}

void PullbackCloner::Implementation::visitSILBasicBlock(SILBasicBlock *bb) {
  auto pbLoc = getPullback().getLocation();
  // Get the corresponding pullback basic block.
  auto *pbBB = getPullbackBlock(bb);
  builder.setInsertionPoint(pbBB);

  LLVM_DEBUG({
    auto &s = getADDebugStream()
              << "Original bb" + std::to_string(bb->getDebugID())
              << ": To differentiate or not to differentiate?\n";
    for (auto &inst : llvm::reverse(*bb)) {
      s << (getPullbackInfo().shouldDifferentiateInstruction(&inst) ? "[x] "
                                                                    : "[ ] ")
        << inst;
    }
  });

  // Visit each instruction in reverse order.
  for (auto &inst : llvm::reverse(*bb)) {
    if (!getPullbackInfo().shouldDifferentiateInstruction(&inst))
      continue;
    // Differentiate instruction.
    builder.setCurrentDebugScope(remapScope(inst.getDebugScope()));
    visit(&inst);
    if (errorOccurred)
      return;
  }

  // Emit a branching terminator for the block.
  // If the original block is the original entry, then the pullback block is
  // the pullback exit. This is handled specially in
  // `PullbackCloner::Implementation::run()`, so we leave the block
  // non-terminated.
  if (bb->isEntry())
    return;

  // Otherwise, add a `switch_enum` terminator for non-exit
  // pullback blocks.
  // 1. Get the pullback struct pullback block argument.
  // 2. Extract the predecessor enum value from the pullback struct value.
  auto *predEnum = getPullbackInfo().getBranchingTraceDecl(bb);
  (void)predEnum;
  auto predEnumVal = getPullbackPredTupleElement(bb);

  // Propagate adjoint values from active basic block arguments to
  // incoming values (predecessor terminator operands).
  for (auto *bbArg : bb->getArguments()) {
    if (!getActivityInfo().isActive(bbArg, getConfig()))
      continue;
    LLVM_DEBUG(getADDebugStream() << "Propagating adjoint value for active bb"
               << bb->getDebugID() << " argument: "
               << *bbArg);

    // Get predecessor terminator operands.
    SmallVector<std::pair<SILBasicBlock *, SILValue>, 4> incomingValues;
    if (bbArg->getSingleTerminatorOperands(incomingValues)) {
      // Returns true if the given terminator instruction is a `switch_enum` on
      // an `Optional`-typed value. `switch_enum` instructions require
      // special-case adjoint value propagation for the operand.
      auto isSwitchEnumInstOnOptional =
        [&ctx = getASTContext()](TermInst *termInst) {
          if (!termInst)
            return false;
          if (auto *sei = dyn_cast<SwitchEnumInst>(termInst)) {
            auto operandTy = sei->getOperand()->getType();
            return operandTy.getASTType()->isOptional();
          }
          return false;
        };

      // Check the tangent value category of the active basic block argument.
      switch (getTangentValueCategory(bbArg)) {
        // If argument has a loadable tangent value category: materialize adjoint
        // value of the argument, create a copy, and set the copy as the adjoint
        // value of incoming values.
      case SILValueCategory::Object: {
        auto bbArgAdj = getAdjointValue(bb, bbArg);
        auto concreteBBArgAdj = materializeAdjointDirect(bbArgAdj, pbLoc);
        auto concreteBBArgAdjCopy =
          builder.emitCopyValueOperation(pbLoc, concreteBBArgAdj);
        for (auto pair : incomingValues) {
          auto *predBB = std::get<0>(pair);
          auto incomingValue = std::get<1>(pair);
          // Handle `switch_enum` on `Optional`.
          auto termInst = bbArg->getSingleTerminator();
          if (isSwitchEnumInstOnOptional(termInst)) {
            accumulateAdjointForOptional(bb, incomingValue, concreteBBArgAdjCopy);
          } else {
            blockTemporaries[getPullbackBlock(predBB)].insert(
              concreteBBArgAdjCopy);
            setAdjointValue(predBB, incomingValue,
                            makeConcreteAdjointValue(concreteBBArgAdjCopy));
          }
        }
        break;
      }
      // If argument has an address tangent value category: materialize adjoint
      // value of the argument, create a copy, and set the copy as the adjoint
      // value of incoming values.
      case SILValueCategory::Address: {
        auto bbArgAdjBuf = getAdjointBuffer(bb, bbArg);
        for (auto pair : incomingValues) {
          auto incomingValue = std::get<1>(pair);
          // Handle `switch_enum` on `Optional`.
          auto termInst = bbArg->getSingleTerminator();
          if (isSwitchEnumInstOnOptional(termInst))
            accumulateAdjointForOptional(bb, incomingValue, bbArgAdjBuf);
          else
            addToAdjointBuffer(bb, incomingValue, bbArgAdjBuf, pbLoc);
        }
        break;
      }
      }
    } else
      llvm::report_fatal_error("do not know how to handle this incoming bb argument");
  }

  // 3. Build the pullback successor cases for the `switch_enum`
  //    instruction. The pullback successors correspond to the predecessors
  //    of the current original block.
  SmallVector<std::pair<EnumElementDecl *, SILBasicBlock *>, 4>
      pullbackSuccessorCases;
  // A map from active values' adjoint values to the trampoline blocks that
  // are using them.
  SmallDenseMap<SILValue, TrampolineBlockSet> pullbackTrampolineBlockMap;
  SmallDenseMap<SILBasicBlock *, SILBasicBlock *> origPredpullbackSuccBBMap;
  for (auto *predBB : bb->getPredecessorBlocks()) {
    auto *pullbackSuccBB =
        buildPullbackSuccessor(bb, predBB, pullbackTrampolineBlockMap);
    origPredpullbackSuccBBMap[predBB] = pullbackSuccBB;
    auto *enumEltDecl =
        getPullbackInfo().lookUpBranchingTraceEnumElement(predBB, bb);
    pullbackSuccessorCases.push_back({enumEltDecl, pullbackSuccBB});
  }
  // Values are trampolined by only a subset of pullback successor blocks.
  // Other successors blocks should destroy the value.
  for (auto pair : pullbackTrampolineBlockMap) {
    auto value = pair.getFirst();
    // The set of trampoline BBs that are users of `value`.
    auto &userTrampolineBBSet = pair.getSecond();
    // For each pullback successor block that does not trampoline the value,
    // release the value.
    for (auto origPredPbSuccPair : origPredpullbackSuccBBMap) {
      auto *origPred = origPredPbSuccPair.getFirst();
      auto *pbSucc = origPredPbSuccPair.getSecond();
      if (userTrampolineBBSet.count(pbSucc))
        continue;
      TangentBuilder pullbackSuccBuilder(pbSucc->begin(), getContext());
      pullbackSuccBuilder.setCurrentDebugScope(
          remapScope(origPred->getTerminator()->getDebugScope()));
      pullbackSuccBuilder.emitDestroyValueOperation(pbLoc, value);
    }
  }
  // Emit cleanups for all block-local temporaries.
  cleanUpTemporariesForBlock(pbBB, pbLoc);
  // Branch to pullback successor blocks.
  assert(pullbackSuccessorCases.size() == predEnum->getNumElements());
  builder.createSwitchEnum(pbLoc, predEnumVal, /*DefaultBB*/ nullptr,
                           pullbackSuccessorCases, llvm::None, ProfileCounter(),
                           OwnershipKind::Owned);
}

//--------------------------------------------------------------------------//
// Member accessor pullback generation
//--------------------------------------------------------------------------//

bool PullbackCloner::Implementation::runForSemanticMemberAccessor() {
  auto &original = getOriginal();
  auto *accessor = cast<AccessorDecl>(original.getDeclContext()->getAsDecl());
  switch (accessor->getAccessorKind()) {
  case AccessorKind::Get:
    return runForSemanticMemberGetter();
  case AccessorKind::Set:
    return runForSemanticMemberSetter();
  // TODO(https://github.com/apple/swift/issues/55084): Support `modify` accessors.
  default:
    llvm_unreachable("Unsupported accessor kind; inconsistent with "
                     "`isSemanticMemberAccessor`?");
  }
}

bool PullbackCloner::Implementation::runForSemanticMemberGetter() {
  auto &original = getOriginal();
  auto &pullback = getPullback();
  auto pbLoc = getPullback().getLocation();

  auto *accessor = cast<AccessorDecl>(original.getDeclContext()->getAsDecl());
  assert(accessor->getAccessorKind() == AccessorKind::Get);

  auto *origEntry = original.getEntryBlock();
  auto *pbEntry = pullback.getEntryBlock();
  builder.setCurrentDebugScope(
      remapScope(origEntry->getScopeOfFirstNonMetaInstruction()));
  builder.setInsertionPoint(pbEntry);

  // Get getter argument and result values.
  //   Getter type: $(Self) -> Result
  // Pullback type: $(Result') -> Self'
  assert(original.getLoweredFunctionType()->getNumParameters() == 1);
  assert(pullback.getLoweredFunctionType()->getNumParameters() == 1);
  assert(pullback.getLoweredFunctionType()->getNumResults() == 1);
  SILValue origSelf = original.getArgumentsWithoutIndirectResults().front();

  SmallVector<SILValue, 8> origFormalResults;
  collectAllFormalResultsInTypeOrder(original, origFormalResults);
  assert(getConfig().resultIndices->getNumIndices() == 1 &&
         "Getter should have one semantic result");
  auto origResult = origFormalResults[*getConfig().resultIndices->begin()];

  auto tangentVectorSILTy = pullback.getConventions().getResults().front()
      .getSILStorageType(getModule(),
                         pullback.getLoweredFunctionType(),
                         TypeExpansionContext::minimal());
  auto tangentVectorTy = tangentVectorSILTy.getASTType();
  auto *tangentVectorDecl = tangentVectorTy->getStructOrBoundGenericStruct();

  // Look up the corresponding field in the tangent space.
  auto *origField = cast<VarDecl>(accessor->getStorage());
  auto baseType = remapType(origSelf->getType()).getASTType();
  auto *tanField = getTangentStoredProperty(getContext(), origField, baseType,
                                            pbLoc, getInvoker());
  if (!tanField) {
    errorOccurred = true;
    return true;
  }

  // Switch based on the base tangent struct's value category.
  switch (getTangentValueCategory(origSelf)) {
  case SILValueCategory::Object: {
    auto adjResult = getAdjointValue(origEntry, origResult);
    switch (adjResult.getKind()) {
    case AdjointValueKind::Zero:
      addAdjointValue(origEntry, origSelf,
                      makeZeroAdjointValue(tangentVectorSILTy), pbLoc);
      break;
    case AdjointValueKind::Concrete:
    case AdjointValueKind::Aggregate: {
      SmallVector<AdjointValue, 8> eltVals;
      for (auto *field : tangentVectorDecl->getStoredProperties()) {
        if (field == tanField) {
          eltVals.push_back(adjResult);
        } else {
          auto substMap = tangentVectorTy->getMemberSubstitutionMap(
              field->getModuleContext(), field);
          auto fieldTy = field->getType().subst(substMap);
          auto fieldSILTy = getTypeLowering(fieldTy).getLoweredType();
          assert(fieldSILTy.isObject());
          eltVals.push_back(makeZeroAdjointValue(fieldSILTy));
        }
      }
      addAdjointValue(origEntry, origSelf,
                      makeAggregateAdjointValue(tangentVectorSILTy, eltVals),
                      pbLoc);
    }
    }
    break;
  }
  case SILValueCategory::Address: {
    assert(pullback.getIndirectResults().size() == 1);
    auto pbIndRes = pullback.getIndirectResults().front();
    auto *adjSelf = createFunctionLocalAllocation(
        pbIndRes->getType().getObjectType(), pbLoc);
    setAdjointBuffer(origEntry, origSelf, adjSelf);
    for (auto *field : tangentVectorDecl->getStoredProperties()) {
      auto *adjSelfElt = builder.createStructElementAddr(pbLoc, adjSelf, field);
      // Non-tangent fields get a zero.
      if (field != tanField) {
        builder.emitZeroIntoBuffer(pbLoc, adjSelfElt, IsInitialization);
        continue;
      }
      // Switch based on the property's value category.
      switch (getTangentValueCategory(origResult)) {
      case SILValueCategory::Object: {
        auto adjResult = getAdjointValue(origEntry, origResult);
        auto adjResultValue = materializeAdjointDirect(adjResult, pbLoc);
        auto adjResultValueCopy =
            builder.emitCopyValueOperation(pbLoc, adjResultValue);
        builder.emitStoreValueOperation(pbLoc, adjResultValueCopy, adjSelfElt,
                                        StoreOwnershipQualifier::Init);
        break;
      }
      case SILValueCategory::Address: {
        auto adjResult = getAdjointBuffer(origEntry, origResult);
        builder.createCopyAddr(pbLoc, adjResult, adjSelfElt, IsTake,
                               IsInitialization);
        destroyedLocalAllocations.insert(adjResult);
        break;
      }
      }
    }
    break;
  }
  }
  return false;
}

bool PullbackCloner::Implementation::runForSemanticMemberSetter() {
  auto &original = getOriginal();
  auto &pullback = getPullback();
  auto pbLoc = getPullback().getLocation();

  auto *accessor = cast<AccessorDecl>(original.getDeclContext()->getAsDecl());
  assert(accessor->getAccessorKind() == AccessorKind::Set);

  auto *origEntry = original.getEntryBlock();
  auto *pbEntry = pullback.getEntryBlock();
  builder.setCurrentDebugScope(
      remapScope(origEntry->getScopeOfFirstNonMetaInstruction()));
  builder.setInsertionPoint(pbEntry);

  // Get setter argument values.
  //              Setter type: $(inout Self, Argument) -> ()
  // Pullback type (wrt self): $(inout Self') -> ()
  // Pullback type (wrt both): $(inout Self') -> Argument'
  assert(original.getLoweredFunctionType()->getNumParameters() == 2);
  assert(pullback.getLoweredFunctionType()->getNumParameters() == 1);
  assert(pullback.getLoweredFunctionType()->getNumResults() == 0 ||
         pullback.getLoweredFunctionType()->getNumResults() == 1);

  SILValue origArg = original.getArgumentsWithoutIndirectResults()[0];
  SILValue origSelf = original.getArgumentsWithoutIndirectResults()[1];

  // Look up the corresponding field in the tangent space.
  auto *origField = cast<VarDecl>(accessor->getStorage());
  auto baseType = remapType(origSelf->getType()).getASTType();
  auto *tanField = getTangentStoredProperty(getContext(), origField, baseType,
                                            pbLoc, getInvoker());
  if (!tanField) {
    errorOccurred = true;
    return true;
  }

  auto adjSelf = getAdjointBuffer(origEntry, origSelf);
  auto *adjSelfElt = builder.createStructElementAddr(pbLoc, adjSelf, tanField);
  // Switch based on the property's value category.
  switch (getTangentValueCategory(origArg)) {
  case SILValueCategory::Object: {
    auto adjArg = builder.emitLoadValueOperation(pbLoc, adjSelfElt,
                                                 LoadOwnershipQualifier::Take);
    setAdjointValue(origEntry, origArg, makeConcreteAdjointValue(adjArg));
    blockTemporaries[pbEntry].insert(adjArg);
    break;
  }
  case SILValueCategory::Address: {
    addToAdjointBuffer(origEntry, origArg, adjSelfElt, pbLoc);
    builder.emitDestroyOperation(pbLoc, adjSelfElt);
    break;
  }
  }
  builder.emitZeroIntoBuffer(pbLoc, adjSelfElt, IsInitialization);

  return false;
}

//--------------------------------------------------------------------------//
// Adjoint buffer mapping
//--------------------------------------------------------------------------//

SILValue PullbackCloner::Implementation::getAdjointProjection(
    SILBasicBlock *origBB, SILValue originalProjection) {
  // Handle `struct_element_addr`.
  // Adjoint projection: a `struct_element_addr` into the base adjoint buffer.
  if (auto *seai = dyn_cast<StructElementAddrInst>(originalProjection)) {
    assert(!seai->getField()->getAttrs().hasAttribute<NoDerivativeAttr>() &&
           "`@noDerivative` struct projections should never be active");
    auto adjSource = getAdjointBuffer(origBB, seai->getOperand());
    auto structType = remapType(seai->getOperand()->getType()).getASTType();
    auto *tanField =
        getTangentStoredProperty(getContext(), seai, structType, getInvoker());
    assert(tanField && "Invalid projections should have been diagnosed");
    return builder.createStructElementAddr(seai->getLoc(), adjSource, tanField);
  }
  // Handle `tuple_element_addr`.
  // Adjoint projection: a `tuple_element_addr` into the base adjoint buffer.
  if (auto *teai = dyn_cast<TupleElementAddrInst>(originalProjection)) {
    auto source = teai->getOperand();
    auto adjSource = getAdjointBuffer(origBB, source);
    if (!adjSource->getType().is<TupleType>())
      return adjSource;
    auto origTupleTy = source->getType().castTo<TupleType>();
    unsigned adjIndex = 0;
    for (unsigned i : range(teai->getFieldIndex())) {
      if (getTangentSpace(
              origTupleTy->getElement(i).getType()->getCanonicalType()))
        ++adjIndex;
    }
    return builder.createTupleElementAddr(teai->getLoc(), adjSource, adjIndex);
  }
  // Handle `ref_element_addr`.
  // Adjoint projection: a local allocation initialized with the corresponding
  // field value from the class's base adjoint value.
  if (auto *reai = dyn_cast<RefElementAddrInst>(originalProjection)) {
    assert(!reai->getField()->getAttrs().hasAttribute<NoDerivativeAttr>() &&
           "`@noDerivative` class projections should never be active");
    auto loc = reai->getLoc();
    // Get the class operand, stripping `begin_borrow`.
    auto classOperand = stripBorrow(reai->getOperand());
    auto classType = remapType(reai->getOperand()->getType()).getASTType();
    auto *tanField =
        getTangentStoredProperty(getContext(), reai->getField(), classType,
                                 reai->getLoc(), getInvoker());
    assert(tanField && "Invalid projections should have been diagnosed");
    // Create a local allocation for the element adjoint buffer.
    auto eltTanType = tanField->getValueInterfaceType()->getCanonicalType();
    auto eltTanSILType =
        remapType(SILType::getPrimitiveAddressType(eltTanType));
    auto *eltAdjBuffer = createFunctionLocalAllocation(eltTanSILType, loc);
    // Check the class operand's `TangentVector` value category.
    switch (getTangentValueCategory(classOperand)) {
    case SILValueCategory::Object: {
      // Get the class operand's adjoint value. Currently, it must be a
      // `TangentVector` struct.
      auto adjClass =
          materializeAdjointDirect(getAdjointValue(origBB, classOperand), loc);
      builder.emitScopedBorrowOperation(
          loc, adjClass, [&](SILValue borrowedAdjClass) {
            // Initialize the element adjoint buffer with the base adjoint
            // value.
            auto *adjElt =
                builder.createStructExtract(loc, borrowedAdjClass, tanField);
            auto adjEltCopy = builder.emitCopyValueOperation(loc, adjElt);
            builder.emitStoreValueOperation(loc, adjEltCopy, eltAdjBuffer,
                                            StoreOwnershipQualifier::Init);
          });
      return eltAdjBuffer;
    }
    case SILValueCategory::Address: {
      // Get the class operand's adjoint buffer. Currently, it must be a
      // `TangentVector` struct.
      auto adjClass = getAdjointBuffer(origBB, classOperand);
      // Initialize the element adjoint buffer with the base adjoint buffer.
      auto *adjElt = builder.createStructElementAddr(loc, adjClass, tanField);
      builder.createCopyAddr(loc, adjElt, eltAdjBuffer, IsNotTake,
                             IsInitialization);
      return eltAdjBuffer;
    }
    }
  }
  // Handle `begin_access`.
  // Adjoint projection: the base adjoint buffer itself.
  if (auto *bai = dyn_cast<BeginAccessInst>(originalProjection)) {
    auto adjBase = getAdjointBuffer(origBB, bai->getOperand());
    if (errorOccurred)
      return (bufferMap[{origBB, originalProjection}] = SILValue());
    // Return the base buffer's adjoint buffer.
    return adjBase;
  }
  // Handle `array.uninitialized_intrinsic` application element addresses.
  // Adjoint projection: a local allocation initialized by applying
  // `Array.TangentVector.subscript` to the base array's adjoint value.
  auto *ai =
      getAllocateUninitializedArrayIntrinsicElementAddress(originalProjection);
  auto *definingInst = dyn_cast_or_null<SingleValueInstruction>(
      originalProjection->getDefiningInstruction());
  bool isAllocateUninitializedArrayIntrinsicElementAddress =
      ai && definingInst &&
      (isa<PointerToAddressInst>(definingInst) ||
       isa<IndexAddrInst>(definingInst));
  if (isAllocateUninitializedArrayIntrinsicElementAddress) {
    // Get the array element index of the result address.
    int eltIndex = 0;
    if (auto *iai = dyn_cast<IndexAddrInst>(definingInst)) {
      auto *ili = cast<IntegerLiteralInst>(iai->getIndex());
      eltIndex = ili->getValue().getLimitedValue();
    }
    // Get the array adjoint value.
    SILValue arrayAdjoint;
    assert(ai && "Expected `array.uninitialized_intrinsic` application");
    for (auto use : ai->getUses()) {
      auto *dti = dyn_cast<DestructureTupleInst>(use->getUser());
      if (!dti)
        continue;
      assert(!arrayAdjoint && "Array adjoint already found");
      // The first `destructure_tuple` result is the `Array` value.
      auto arrayValue = dti->getResult(0);
      arrayAdjoint = materializeAdjointDirect(
          getAdjointValue(origBB, arrayValue), definingInst->getLoc());
    }
    assert(arrayAdjoint && "Array does not have adjoint value");
    // Apply `Array.TangentVector.subscript` to get array element adjoint value.
    auto *eltAdjBuffer =
        getArrayAdjointElementBuffer(arrayAdjoint, eltIndex, ai->getLoc());
    return eltAdjBuffer;
  }
  return SILValue();
}

//----------------------------------------------------------------------------//
// Adjoint value accumulation
//----------------------------------------------------------------------------//

AdjointValue PullbackCloner::Implementation::accumulateAdjointsDirect(
    AdjointValue lhs, AdjointValue rhs, SILLocation loc) {
  LLVM_DEBUG(getADDebugStream() << "Materializing adjoint directly.\nLHS: "
                                << lhs << "\nRHS: " << rhs << '\n');
  switch (lhs.getKind()) {
  // x
  case AdjointValueKind::Concrete: {
    auto lhsVal = lhs.getConcreteValue();
    switch (rhs.getKind()) {
    // x + y
    case AdjointValueKind::Concrete: {
      auto rhsVal = rhs.getConcreteValue();
      auto sum = recordTemporary(builder.emitAdd(loc, lhsVal, rhsVal));
      return makeConcreteAdjointValue(sum);
    }
    // x + 0 => x
    case AdjointValueKind::Zero:
      return lhs;
    // x + (y, z) => (x.0 + y, x.1 + z)
    case AdjointValueKind::Aggregate:
      SmallVector<AdjointValue, 8> newElements;
      auto lhsTy = lhsVal->getType().getASTType();
      auto lhsValCopy = builder.emitCopyValueOperation(loc, lhsVal);
      if (lhsTy->is<TupleType>()) {
        auto elts = builder.createDestructureTuple(loc, lhsValCopy);
        llvm::for_each(elts->getResults(),
                       [this](SILValue result) { recordTemporary(result); });
        for (auto i : indices(elts->getResults())) {
          auto rhsElt = rhs.getAggregateElement(i);
          newElements.push_back(accumulateAdjointsDirect(
              makeConcreteAdjointValue(elts->getResult(i)), rhsElt, loc));
        }
      } else if (lhsTy->getStructOrBoundGenericStruct()) {
        auto elts =
            builder.createDestructureStruct(lhsVal.getLoc(), lhsValCopy);
        llvm::for_each(elts->getResults(),
                       [this](SILValue result) { recordTemporary(result); });
        for (unsigned i : indices(elts->getResults())) {
          auto rhsElt = rhs.getAggregateElement(i);
          newElements.push_back(accumulateAdjointsDirect(
              makeConcreteAdjointValue(elts->getResult(i)), rhsElt, loc));
        }
      } else {
        llvm_unreachable("Not an aggregate type");
      }
      return makeAggregateAdjointValue(lhsVal->getType(), newElements);
    }
  }
  // 0
  case AdjointValueKind::Zero:
    // 0 + x => x
    return rhs;
  // (x, y)
  case AdjointValueKind::Aggregate:
    switch (rhs.getKind()) {
    // (x, y) + z => (z.0 + x, z.1 + y)
    case AdjointValueKind::Concrete:
      return accumulateAdjointsDirect(rhs, lhs, loc);
    // x + 0 => x
    case AdjointValueKind::Zero:
      return lhs;
    // (x, y) + (z, w) => (x + z, y + w)
    case AdjointValueKind::Aggregate: {
      SmallVector<AdjointValue, 8> newElements;
      for (auto i : range(lhs.getNumAggregateElements()))
        newElements.push_back(accumulateAdjointsDirect(
            lhs.getAggregateElement(i), rhs.getAggregateElement(i), loc));
      return makeAggregateAdjointValue(lhs.getType(), newElements);
    }
    }
  }
  llvm_unreachable("Invalid adjoint value kind"); // silences MSVC C4715
}

//----------------------------------------------------------------------------//
// Array literal initialization differentiation
//----------------------------------------------------------------------------//

void PullbackCloner::Implementation::
    accumulateArrayLiteralElementAddressAdjoints(SILBasicBlock *origBB,
                                                 SILValue originalValue,
                                                 AdjointValue arrayAdjointValue,
                                                 SILLocation loc) {
  // Return if the original value is not the `Array` result of an
  // `array.uninitialized_intrinsic` application.
  auto *dti = dyn_cast_or_null<DestructureTupleInst>(
      originalValue->getDefiningInstruction());
  if (!dti)
    return;
  if (!ArraySemanticsCall(dti->getOperand(),
                          semantics::ARRAY_UNINITIALIZED_INTRINSIC))
    return;
  if (originalValue != dti->getResult(0))
    return;
  // Accumulate the array's adjoint value into the adjoint buffers of its
  // element addresses: `pointer_to_address` and `index_addr` instructions.
  LLVM_DEBUG(getADDebugStream()
             << "Accumulating adjoint value for array literal into element "
                "address adjoint buffers"
             << originalValue);
  auto arrayAdjoint = materializeAdjointDirect(arrayAdjointValue, loc);
  builder.setCurrentDebugScope(remapScope(dti->getDebugScope()));
  builder.setInsertionPoint(arrayAdjoint->getParentBlock());
  for (auto use : dti->getResult(1)->getUses()) {
    auto *ptai = dyn_cast<PointerToAddressInst>(use->getUser());
    auto adjBuf = getAdjointBuffer(origBB, ptai);
    auto *eltAdjBuf = getArrayAdjointElementBuffer(arrayAdjoint, 0, loc);
    builder.emitInPlaceAdd(loc, adjBuf, eltAdjBuf);
    for (auto use : ptai->getUses()) {
      if (auto *iai = dyn_cast<IndexAddrInst>(use->getUser())) {
        auto *ili = cast<IntegerLiteralInst>(iai->getIndex());
        auto eltIndex = ili->getValue().getLimitedValue();
        auto adjBuf = getAdjointBuffer(origBB, iai);
        auto *eltAdjBuf =
            getArrayAdjointElementBuffer(arrayAdjoint, eltIndex, loc);
        builder.emitInPlaceAdd(loc, adjBuf, eltAdjBuf);
      }
    }
  }
}

AllocStackInst *PullbackCloner::Implementation::getArrayAdjointElementBuffer(
    SILValue arrayAdjoint, int eltIndex, SILLocation loc) {
  auto &ctx = builder.getASTContext();
  auto arrayTanType = cast<StructType>(arrayAdjoint->getType().getASTType());
  auto arrayType = arrayTanType->getParent()->castTo<BoundGenericStructType>();
  auto eltTanType = arrayType->getGenericArgs().front()->getCanonicalType();
  auto eltTanSILType = remapType(SILType::getPrimitiveAddressType(eltTanType));
  // Get `function_ref` and generic signature of
  // `Array.TangentVector.subscript.getter`.
  auto *arrayTanStructDecl = arrayTanType->getStructOrBoundGenericStruct();
  auto subscriptLookup =
      arrayTanStructDecl->lookupDirect(DeclBaseName::createSubscript());
  SubscriptDecl *subscriptDecl = nullptr;
  for (auto *candidate : subscriptLookup) {
    auto candidateModule = candidate->getModuleContext();
    if (candidateModule->getName() == ctx.Id_Differentiation ||
        candidateModule->isStdlibModule()) {
      assert(!subscriptDecl && "Multiple `Array.TangentVector.subscript`s");
      subscriptDecl = cast<SubscriptDecl>(candidate);
#ifdef NDEBUG
      break;
#endif
    }
  }
  assert(subscriptDecl && "No `Array.TangentVector.subscript`");
  auto *subscriptGetterDecl =
      subscriptDecl->getOpaqueAccessor(AccessorKind::Get);
  assert(subscriptGetterDecl && "No `Array.TangentVector.subscript` getter");
  SILOptFunctionBuilder fb(getContext().getTransform());
  auto *subscriptGetterFn = fb.getOrCreateFunction(
      loc, SILDeclRef(subscriptGetterDecl), NotForDefinition);
  // %subscript_fn = function_ref @Array.TangentVector<T>.subscript.getter
  auto *subscriptFnRef = builder.createFunctionRef(loc, subscriptGetterFn);
  auto subscriptFnGenSig =
      subscriptGetterFn->getLoweredFunctionType()->getSubstGenericSignature();
  // Apply `Array.TangentVector.subscript.getter` to get array element adjoint
  // buffer.
  // %index_literal = integer_literal $Builtin.IntXX, <index>
  auto builtinIntType =
      SILType::getPrimitiveObjectType(ctx.getIntDecl()
                                          ->getStoredProperties()
                                          .front()
                                          ->getInterfaceType()
                                          ->getCanonicalType());
  auto *eltIndexLiteral =
      builder.createIntegerLiteral(loc, builtinIntType, eltIndex);
  auto intType = SILType::getPrimitiveObjectType(
      ctx.getIntType()->getCanonicalType());
  // %index_int = struct $Int (%index_literal)
  auto *eltIndexInt = builder.createStruct(loc, intType, {eltIndexLiteral});
  auto *swiftModule = getModule().getSwiftModule();
  auto *diffProto = ctx.getProtocol(KnownProtocolKind::Differentiable);
  auto diffConf = swiftModule->lookupConformance(eltTanType, diffProto);
  assert(!diffConf.isInvalid() && "Missing conformance to `Differentiable`");
  auto *addArithProto = ctx.getProtocol(KnownProtocolKind::AdditiveArithmetic);
  auto addArithConf = swiftModule->lookupConformance(eltTanType, addArithProto);
  assert(!addArithConf.isInvalid() &&
         "Missing conformance to `AdditiveArithmetic`");
  auto subMap = SubstitutionMap::get(subscriptFnGenSig, {eltTanType},
                                     {addArithConf, diffConf});
  // %elt_adj = alloc_stack $T.TangentVector
  // Create and register a local allocation.
  auto *eltAdjBuffer = createFunctionLocalAllocation(
      eltTanSILType, loc, /*zeroInitialize*/ true);
  // Immediately destroy the emitted zero value.
  // NOTE: It is not efficient to emit a zero value then immediately destroy
  // it. However, it was the easiest way to to avoid "lifetime mismatch in
  // predecessors" memory lifetime verification errors for control flow
  // differentiation.
  // Perhaps we can avoid emitting a zero value if local allocations are created
  // per pullback bb instead of all in the pullback entry: TF-1075.
  builder.emitDestroyOperation(loc, eltAdjBuffer);
  // apply %subscript_fn<T.TangentVector>(%elt_adj, %index_int, %array_adj)
  builder.createApply(loc, subscriptFnRef, subMap,
                      {eltAdjBuffer, eltIndexInt, arrayAdjoint});
  return eltAdjBuffer;
}

} // end namespace autodiff
} // end namespace swift
