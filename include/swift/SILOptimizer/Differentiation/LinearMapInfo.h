//===--- LinearMapInfo.h --------------------------------------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Linear map struct and branching trace enum information for differentiation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_LINEARMAPINFO_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_LINEARMAPINFO_H

#include "swift/AST/AutoDiff.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {

class SILFunction;
class SILLoopInfo;

namespace autodiff {

class ADContext;

/// Linear map struct and branching trace enum information for an original
/// function and derivative function (JVP or VJP).
///
/// Linear map structs contain all callee linear maps produced in a JVP/VJP
/// basic block. A linear map struct is created for each basic block in the
/// original function, and a linear map struct field is created for every active
/// `apply` in the original basic block.
///
/// Branching trace enums model the control flow graph of the original function.
/// A branching trace enum is created for each basic block in the original
/// function, and a branching trace enum case is created for every basic block
/// predecessor/successor. This supports control flow differentiation: JVP/VJP
/// functions build branching trace enums to record an execution trace. Indirect
/// branching trace enums are created for basic blocks that are in loops.
///
/// Linear map struct values and branching trace enum values are constructed in
/// JVP/VJP functions and consumed in pullback/differential functions.
class LinearMapInfo {
private:
  /// The linear map kind.
  AutoDiffLinearMapKind kind;

  /// The original function.
  SILFunction *const original;

  /// The derivative function.
  SILFunction *const derivative;

  /// Activity info of the original function.
  const DifferentiableActivityInfo &activityInfo;

  /// The original function's loop info.
  SILLoopInfo *loopInfo;

  /// Differentiation indices of the function.
  const AutoDiffConfig config;

  /// Mapping from original basic blocks to linear map tuple types.
  llvm::DenseMap<SILBasicBlock *, TupleType *> linearMapTuples;

  /// Mapping from original basic blocks to branching trace enums.
  /// For pullbacks: these are predecessor enums.
  /// For differentials: these are successor enums.
  llvm::DenseMap<SILBasicBlock *, EnumDecl *> branchingTraceDecls;

  /// Mapping from `apply` / `begin_apply` instructions in the original function to the
  /// corresponding linear map tuple type index.
  llvm::DenseMap<FullApplySite, unsigned> linearMapIndexMap;

  /// Mapping from predecessor-successor basic block pairs in the original
  /// function to the corresponding branching trace enum case.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILBasicBlock *>, EnumElementDecl *>
      branchingTraceEnumCases;

  /// A synthesized file unit.
  SynthesizedFileUnit &synthesizedFile;

  /// A type converter, used to compute struct/enum SIL types.
  Lowering::TypeConverter &typeConverter;

  /// True, if a heap-allocated context is required. For example, when there are
  /// any loops
  bool heapAllocatedContext = false;

private:
  /// Remaps the given type into the derivative function's context.
  SILType remapTypeInDerivative(SILType ty);

  /// Retrieves the file unit that contains implicit declarations in the
  /// current Swift module.
  SynthesizedFileUnit &getSynthesizedFile() { return synthesizedFile; }

  /// Creates an enum declaration with the given JVP/VJP generic signature,
  /// whose cases represent the predecessors/successors of the given original
  /// block.
  EnumDecl *createBranchingTraceDecl(SILBasicBlock *originalBB,
                                     CanGenericSignature genericSig);
  void populateBranchingTraceDecl(SILBasicBlock *originalBB,
                                  SILLoopInfo *loopInfo);

  /// Given an `apply` / `begin_apply` instruction, conditionally gets a linear
  /// map tuple field AST type for its linear map function if it is active.
  Type getLinearMapType(ADContext &context, FullApplySite fai);

  /// Generates linear map struct and branching enum declarations for the given
  /// function. Linear map structs are populated with linear map fields and a
  /// branching enum field.
  void generateDifferentiationDataStructures(ADContext &context,
                                             SILFunction *derivative);

public:
  bool shouldDifferentiateApplySite(FullApplySite applySite);
  bool shouldDifferentiateInstruction(SILInstruction *inst);

  LinearMapInfo(const LinearMapInfo &) = delete;
  LinearMapInfo &operator=(const LinearMapInfo &) = delete;

  explicit LinearMapInfo(ADContext &context, AutoDiffLinearMapKind kind,
                         SILFunction *original, SILFunction *derivative,
                         const AutoDiffConfig &config,
                         const DifferentiableActivityInfo &activityInfo,
                         SILLoopInfo *loopInfo);

  /// Returns the linear map tuple associated with the given original block.
  TupleType *getLinearMapTupleType(SILBasicBlock *origBB) const {
    return linearMapTuples.lookup(origBB);
  }

  /// Returns the lowered SIL type of the linear map tuple associated with the
  /// given original block.
  SILType getLinearMapTupleLoweredType(SILBasicBlock *origBB) const {
    auto derivativeGenSig =
        derivative->getLoweredFunctionType()->getSubstGenericSignature();
    auto linMapTupleType =
      getLinearMapTupleType(origBB)->getReducedType(derivativeGenSig);
    Lowering::AbstractionPattern pattern(derivativeGenSig, linMapTupleType);
    return typeConverter.getLoweredType(pattern, linMapTupleType,
                                        TypeExpansionContext::minimal());
  }

  /// Returns the branching trace enum associated with the given original block.
  EnumDecl *getBranchingTraceDecl(SILBasicBlock *origBB) const {
    return branchingTraceDecls.lookup(origBB);
  }

  /// Returns the lowered SIL type of the branching trace enum associated with
  /// the given original block.
  SILType getBranchingTraceEnumLoweredType(SILBasicBlock *origBB) const {
    auto *traceDecl = getBranchingTraceDecl(origBB);
    auto traceDeclType =
        traceDecl->getDeclaredInterfaceType()->getCanonicalType();
    Lowering::AbstractionPattern pattern(
        derivative->getLoweredFunctionType()->getSubstGenericSignature(),
        traceDeclType);
    return typeConverter.getLoweredType(pattern, traceDeclType,
                                        TypeExpansionContext::minimal());
  }

  /// Returns the enum element in the given successor block's branching trace
  /// enum corresponding to the given predecessor block.
  EnumElementDecl *
  lookUpBranchingTraceEnumElement(SILBasicBlock *origPredBB,
                                  SILBasicBlock *origSuccBB) const {
    assert(origPredBB->getParent() == original);
    return branchingTraceEnumCases.lookup({origPredBB, origSuccBB});
  }

  /// Finds the linear map index in the pullback tuple for the given
  /// `apply` / `begin_apply` instruction in the original function.
  unsigned lookUpLinearMapIndex(FullApplySite fas) const {
    assert(fas->getFunction() == original);
    auto lookup = linearMapIndexMap.find(fas);
    assert(lookup != linearMapIndexMap.end() &&
           "No linear map field corresponding to the given `apply`");
    return lookup->getSecond();
  }

  Type lookUpLinearMapType(FullApplySite fas) const {
    unsigned idx = lookUpLinearMapIndex(fas);
    return getLinearMapTupleType(fas->getParent())->getElement(idx).getType();
  }

  bool hasHeapAllocatedContext() const {
    return heapAllocatedContext;
  }
};

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_LINEARMAPINFO_H
