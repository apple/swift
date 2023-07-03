//===--- FunctionSignatureOpts.h --------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_TRANSFORMS_FUNCTIONSIGNATUREOPTS_H
#define SWIFT_SILOPTIMIZER_TRANSFORMS_FUNCTIONSIGNATUREOPTS_H

#include "swift/Basic/LLVM.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Analysis/CallerAnalysis.h"
#include "swift/SILOptimizer/Analysis/RCIdentityAnalysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {

/// A structure that maintains all of the information about a specific
/// SILArgument that we are tracking.
struct ArgumentDescriptor {
  /// The argument that we are tracking original data for.
  SILFunctionArgument *Arg;

  /// Parameter Info.
  llvm::Optional<SILParameterInfo> PInfo;

  /// The original index of this argument.
  unsigned Index;

  /// The original decl of this Argument.
  const ValueDecl *Decl;

  /// Was this parameter originally dead?
  bool IsEntirelyDead;

  /// Was this argument completely removed already?
  ///
  /// TODO: Could we make ArgumentDescriptors just optional and once they have
  /// been consumed no longer process them?
  bool WasErased;

  /// Should the argument be exploded ?
  bool Explode;

  /// This parameter is owned to guaranteed.
  bool OwnedToGuaranteed;

  /// Is this parameter an indirect result?
  bool IsIndirectResult;

  /// If non-empty, this is the set of releases in the return block of
  /// the callee associated with this parameter if it is @owned. If it
  /// is empty then we could not find any such releases.
  TinyPtrVector<SILInstruction *> CalleeRelease;

  /// The same as CalleeRelease, but the releases are post-dominated
  /// by the throw block, if it is a function which has a throw block.
  TinyPtrVector<SILInstruction *> CalleeReleaseInThrowBlock;

  /// The projection tree of this arguments.
  ProjectionTree ProjTree;

  ArgumentDescriptor() = delete;

  /// Initialize this argument descriptor with all information from A that we
  /// use in our optimization.
  ///
  /// *NOTE* We cache a lot of data from the argument and maintain a reference
  /// to the original argument. The reason why we do this is to make sure we
  /// have access to the original argument's state if we modify the argument
  /// when optimizing.
  ArgumentDescriptor(
      SILFunctionArgument *A,
      llvm::SpecificBumpPtrAllocator<ProjectionTreeNode> &Allocator)
      : Arg(A), PInfo(A->getKnownParameterInfo()), Index(A->getIndex()),
        Decl(A->getDecl()), IsEntirelyDead(false), WasErased(false),
        Explode(false), OwnedToGuaranteed(false),
        IsIndirectResult(A->isIndirectResult()), CalleeRelease(),
        CalleeReleaseInThrowBlock(),
        ProjTree(A->getModule(), A->getType(), Allocator) {
    if (!A->isIndirectResult()) {
      PInfo = Arg->getKnownParameterInfo();
    }
  }

  ArgumentDescriptor(const ArgumentDescriptor &) = delete;
  ArgumentDescriptor(ArgumentDescriptor &&) = default;
  ArgumentDescriptor &operator=(const ArgumentDescriptor &) = delete;
  ArgumentDescriptor &operator=(ArgumentDescriptor &&) = default;

  /// \returns true if this argument's convention is P.
  bool hasConvention(SILArgumentConvention P) const {
    return Arg->hasConvention(P);
  }

  /// Returns true if all function signature opt passes are able to process
  /// this.
  bool canOptimizeLiveArg() const {
    if (Arg->getType().isObject()) {
      return true;
    }

    // @in arguments of generic types can be processed.
    if (Arg->getType().hasArchetype() &&
        Arg->getType().isAddress() &&
        (Arg->hasConvention(SILArgumentConvention::Indirect_In) ||
         Arg->hasConvention(SILArgumentConvention::Indirect_In_Guaranteed)))
      return true;
    return false;
  }

  llvm::Optional<ValueOwnershipKind>
  getTransformedOwnershipKind(SILType SubTy) {
    if (IsEntirelyDead)
      return llvm::None;
    if (SubTy.isTrivial(*Arg->getFunction()))
      return llvm::Optional<ValueOwnershipKind>(OwnershipKind::None);
    if (OwnedToGuaranteed)
      return llvm::Optional<ValueOwnershipKind>(OwnershipKind::Guaranteed);
    return Arg->getOwnershipKind();
  }
};

/// A structure that maintains all of the information about a specific
/// direct result that we are tracking.
struct ResultDescriptor {
  /// The original parameter info of this argument.
  SILResultInfo ResultInfo;

  /// If non-null, this is the release in the return block of the callee, which
  /// is associated with this parameter if it is @owned. If the parameter is not
  /// @owned or we could not find such a release in the callee, this is null.
  llvm::SmallSetVector<SILInstruction *, 1> CalleeRetain;

  /// This is owned to guaranteed.
  bool OwnedToGuaranteed;

  /// Initialize this argument descriptor with all information from A that we
  /// use in our optimization.
  ///
  /// *NOTE* We cache a lot of data from the argument and maintain a reference
  /// to the original argument. The reason why we do this is to make sure we
  /// have access to the original argument's state if we modify the argument
  /// when optimizing.
  ResultDescriptor() {}
  ResultDescriptor(SILResultInfo RI)
      : ResultInfo(RI), CalleeRetain(), OwnedToGuaranteed(false) {}

  ResultDescriptor(const ResultDescriptor &) = delete;
  ResultDescriptor(ResultDescriptor &&) = default;
  ResultDescriptor &operator=(const ResultDescriptor &) = delete;
  ResultDescriptor &operator=(ResultDescriptor &&) = default;

  /// \returns true if this argument's ParameterConvention is P.
  bool hasConvention(ResultConvention R) const {
    return ResultInfo.getConvention() == R;
  }
};

struct FunctionSignatureTransformDescriptor {
  /// The original function that we are analyzing/transforming.
  SILFunction *OriginalFunction;

  /// The new optimize function that we will create.
  NullablePtr<SILFunction> OptimizedFunction;

  /// A map from a pre-transformed argument to a post-transformed argument.
  llvm::SmallDenseMap<int, int> &AIM;

  /// Set to true if we are going to modify self during our transformation.
  ///
  /// TODO: Rename to willModifySelfArgument.
  bool shouldModifySelfArgument;

  /// Keep a "view" of precompiled information on arguments that we use
  /// during our optimization.
  MutableArrayRef<ArgumentDescriptor> ArgumentDescList;

  /// Keep a "view" of precompiled information on the direct results that we
  /// will use during our optimization.
  MutableArrayRef<ResultDescriptor> ResultDescList;

  /// Are we going to make a change to this function?
  bool Changed;

  /// Does this function only have direct callers. In such a case we know that
  /// all thunks we create will be eliminated so we can be more aggressive.
  bool hasOnlyDirectInModuleCallers;

  /// Return a function name based on the current state of ArgumentDescList and
  /// ResultDescList.
  ///
  /// FIXME: Change this to take a SmallString as an out parameter?
  std::string createOptimizedSILFunctionName();

  /// Return a function type based on the current state of ArgumentDescList and
  /// ResultDescList.
  CanSILFunctionType createOptimizedSILFunctionType();

  /// Compute the optimized function type based on the given argument
  /// descriptor.
  void computeOptimizedArgInterface(ArgumentDescriptor &A,
                                    SmallVectorImpl<SILParameterInfo> &O);

  /// Setup the thunk arguments based on the given argument descriptor info.
  /// Every transformation must defines this interface. Default implementation
  /// simply passes it through.
  void addThunkArgument(ArgumentDescriptor &AD, SILBuilder &Builder,
                        SILBasicBlock *BB, SmallVectorImpl<SILValue> &NewArgs);

  /// Whether specializing the function will result in a thunk with the same
  /// signature as the original function that calls through to the specialized
  /// function.
  ///
  /// Such a thunk is necessary if there is (or could be) code that calls the
  /// function which we are unable to specialize to match the function's
  /// specialization.
  bool willSpecializationIntroduceThunk() {
    return !hasOnlyDirectInModuleCallers ||
           OriginalFunction->isPossiblyUsedExternally() ||
          OriginalFunction->isAvailableExternally();
  }
};

class FunctionSignatureTransform {
  SILOptFunctionBuilder &FunctionBuilder;

  /// A struct that contains all data that we use during our
  /// transformation. This is an initial step towards splitting this struct into
  /// multiple "transforms" that can be tested independently of each other.
  FunctionSignatureTransformDescriptor TransformDescriptor;

  /// The RC identity analysis we are using.
  RCIdentityAnalysis *RCIA;

  /// Post order analysis we are using.
  EpilogueARCAnalysis *EA;

private:
  /// ----------------------------------------------------------///
  /// Dead argument transformation.                             ///
  /// ----------------------------------------------------------///
  /// Find any dead argument opportunities.
  bool DeadArgumentAnalyzeParameters();
  /// Modify the current function so that later function signature analysis
  /// are more effective.
  void DeadArgumentTransformFunction();
  /// Remove the dead argument once the new function is created.
  void DeadArgumentFinalizeOptimizedFunction();

  /// ----------------------------------------------------------///
  /// Owned to guaranteed transformation.                       ///
  /// ----------------------------------------------------------///
  bool OwnedToGuaranteedAnalyzeResults();
  bool OwnedToGuaranteedAnalyzeParameters();

  /// Modify the current function so that later function signature analysis
  /// are more effective.
  void OwnedToGuaranteedTransformFunctionResults();
  void OwnedToGuaranteedTransformFunctionParameters();

  /// Find any owned to guaranteed opportunities.
  bool OwnedToGuaranteedAnalyze();

  /// Do the actual owned to guaranteed transformations.
  void OwnedToGuaranteedTransform();

  /// Set up epilogue work for the thunk result based in the given argument.
  void OwnedToGuaranteedAddResultRelease(ResultDescriptor &RD,
                                         SILBuilder &Builder, SILFunction *F);

  /// Set up epilogue work for the thunk argument based in the given argument.
  void OwnedToGuaranteedAddArgumentRelease(ArgumentDescriptor &AD,
                                           SILBuilder &Builder, SILFunction *F);

  /// Add the release for converted arguments and result.
  void OwnedToGuaranteedFinalizeThunkFunction(SILBuilder &B, SILFunction *F);

  /// ----------------------------------------------------------///
  /// Argument explosion transformation.                        ///
  /// ----------------------------------------------------------///
  /// Find any argument explosion opportunities.
  bool ArgumentExplosionAnalyzeParameters();
  /// Explode the argument in the optimized function and replace the uses of
  /// the original argument.
  void ArgumentExplosionFinalizeOptimizedFunction();

  /// Take ArgumentDescList and ResultDescList and create an optimized function
  /// based on the current function we are analyzing. This also has the side
  /// effect of turning the current function into a thunk.
  void createFunctionSignatureOptimizedFunction();

public:
  /// Constructor.
  FunctionSignatureTransform(
      SILOptFunctionBuilder &FunctionBuilder, SILFunction *F,
      RCIdentityAnalysis *RCIA, EpilogueARCAnalysis *EA,
      Mangle::FunctionSignatureSpecializationMangler &Mangler,
      llvm::SmallDenseMap<int, int> &AIM,
      llvm::SmallVector<ArgumentDescriptor, 4> &ADL,
      llvm::SmallVector<ResultDescriptor, 4> &RDL,
      bool hasOnlyDirectInModuleCallers)
      : FunctionBuilder(FunctionBuilder),
        TransformDescriptor{F,   nullptr, AIM,   false,
                            ADL, RDL,     false, hasOnlyDirectInModuleCallers},
        RCIA(RCIA), EA(EA) {}

  /// Return the optimized function.
  SILFunction *getOptimizedFunction() {
    return TransformDescriptor.OptimizedFunction.getPtrOrNull();
  }

  /// Run the optimization.
  bool run(bool hasCaller);

  /// Run dead argument elimination of partially applied functions.
  ///
  /// After this optimization CapturePropagation can replace the partial_apply
  /// by a direct reference to the specialized function.
  bool removeDeadArgs(int minPartialAppliedArgs);
};

} // namespace swift

#endif
