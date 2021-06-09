//===--- PassManager.h  - Swift Pass Manager --------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/Notifications.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/PassPipeline.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include <vector>

#ifndef SWIFT_SILOPTIMIZER_PASSMANAGER_PASSMANAGER_H
#define SWIFT_SILOPTIMIZER_PASSMANAGER_PASSMANAGER_H

namespace swift {

class SILFunction;
class SILFunctionTransform;
class SILModule;
class SILModuleTransform;
class SILOptions;
class SILTransform;
class SILPassManager;
class SILCombiner;

namespace irgen {
class IRGenModule;
}

/// The main entrypoint for executing a pipeline pass on a SIL module.
void executePassPipelinePlan(SILModule *SM, const SILPassPipelinePlan &plan,
                             bool isMandatory = false,
                             irgen::IRGenModule *IRMod = nullptr);

/// Utility class to invoke passes in libswift.
class LibswiftPassInvocation {
  /// Backlink to the pass manager.
  SILPassManager *passManager;
  
  /// Non-null if this is an instruction pass, invoked from SILCombine.
  SILCombiner *silCombiner;

  /// All slabs, allocated by the pass.
  SILModule::SlabList allocatedSlabs;

public:
  LibswiftPassInvocation(SILPassManager *passManager, SILCombiner *silCombiner) :
    passManager(passManager), silCombiner(silCombiner) {}

  FixedSizeSlab *allocSlab(FixedSizeSlab *afterSlab);

  FixedSizeSlab *freeSlab(FixedSizeSlab *slab);

  /// The top-level API to erase an instruction, called from the Swift pass.
  void eraseInstruction(SILInstruction *inst);

  /// Called by the pass when changes are made to the SIL.
  void notifyChanges(SILAnalysis::InvalidationKind invalidationKind);

  /// Called by the pass manager when the pass has finished.
  void finishedPassRun();
};

/// The SIL pass manager.
class SILPassManager {
  friend class ExecuteSILPipelineRequest;

  /// The module that the pass manager will transform.
  SILModule *Mod;

  /// An optional IRGenModule associated with this PassManager.
  irgen::IRGenModule *IRMod;

  /// The list of transformations to run.
  llvm::SmallVector<SILTransform *, 16> Transformations;

  /// A list of registered analysis.
  llvm::SmallVector<SILAnalysis *, 16> Analyses;

  /// An entry in the FunctionWorkList.
  struct WorklistEntry {
    WorklistEntry(SILFunction *F) : F(F) { }

    SILFunction *F;

    /// The current position in the transform-list.
    unsigned PipelineIdx = 0;

    /// How many times the pipeline was restarted for the function.
    unsigned NumRestarts = 0;
  };

  /// The worklist of functions to be processed by function passes.
  std::vector<WorklistEntry> FunctionWorklist;

  // Name of the current optimization stage for diagnostics.
  std::string StageName;

  /// The number of passes run so far.
  unsigned NumPassesRun = 0;

  /// For invoking Swift passes in libswift.
  LibswiftPassInvocation libswiftPassInvocation;

  /// Change notifications, collected during a bridged pass run.
  SILAnalysis::InvalidationKind changeNotifications =
      SILAnalysis::InvalidationKind::Nothing;

  /// A mask which has one bit for each pass. A one for a pass-bit means that
  /// the pass doesn't need to run, because nothing has changed since the
  /// previous run of that pass.
  typedef std::bitset<(size_t)PassKind::AllPasses_Last + 1> CompletedPasses;
  
  /// A completed-passes mask for each function.
  llvm::DenseMap<SILFunction *, CompletedPasses> CompletedPassesMap;

  /// Stores for each function the number of levels of specializations it is
  /// derived from an original function. E.g. if a function is a signature
  /// optimized specialization of a generic specialization, it has level 2.
  /// This is used to avoid an infinite amount of functions pushed on the
  /// worklist (e.g. caused by a bug in a specializing optimization).
  llvm::DenseMap<SILFunction *, int> DerivationLevels;

  /// Set to true when a pass invalidates an analysis.
  bool CurrentPassHasInvalidated = false;

  /// True if we need to stop running passes and restart again on the
  /// same function.
  bool RestartPipeline = false;

  /// If true, passes are also run for functions which have
  /// OptimizationMode::NoOptimization.
  bool isMandatory = false;

  /// The notification handler for this specific SILPassManager.
  ///
  /// This is not owned by the pass manager, it is owned by the SILModule which
  /// is guaranteed to outlive any pass manager associated with it. We keep this
  /// bare pointer to ensure that we can deregister the notification after this
  /// pass manager is destroyed.
  DeserializationNotificationHandler *deserializationNotificationHandler;

  /// C'tor. It creates and registers all analysis passes, which are defined
  /// in Analysis.def. This is private as it should only be used by
  /// ExecuteSILPipelineRequest.
  SILPassManager(SILModule *M, bool isMandatory, irgen::IRGenModule *IRMod);

public:
  const SILOptions &getOptions() const;

  /// Searches for an analysis of type T in the list of registered
  /// analysis. If the analysis is not found, the program terminates.
  template<typename T>
  T *getAnalysis() {
    for (SILAnalysis *A : Analyses)
      if (auto *R = llvm::dyn_cast<T>(A))
        return R;

    llvm_unreachable("Unable to find analysis for requested type.");
  }

  template<typename T>
  T *getAnalysis(SILFunction *f) {
    for (SILAnalysis *A : Analyses) {
      if (A->getKind() == T::getAnalysisKind())
        return static_cast<FunctionAnalysisBase<T> *>(A)->get(f);
    }
    llvm_unreachable("Unable to find analysis for requested type.");
  }

  /// \returns the module that the pass manager owns.
  SILModule *getModule() { return Mod; }

  /// \returns the associated IGenModule or null if this is not an IRGen
  /// pass manager.
  irgen::IRGenModule *getIRGenModule() { return IRMod; }

  LibswiftPassInvocation *getLibswiftPassInvocation() {
    return &libswiftPassInvocation;
  }

  /// Restart the function pass pipeline on the same function
  /// that is currently being processed.
  void restartWithCurrentFunction(SILTransform *T);
  void clearRestartPipeline() { RestartPipeline = false; }
  bool shouldRestartPipeline() { return RestartPipeline; }

  /// Iterate over all analysis and invalidate them.
  void invalidateAllAnalysis() {
    // Invalidate the analysis (unless they are locked)
    for (auto AP : Analyses)
      if (!AP->isLocked())
        AP->invalidate();

    CurrentPassHasInvalidated = true;

    // Assume that all functions have changed. Clear all masks of all functions.
    CompletedPassesMap.clear();
  }

  /// Notify the pass manager of a newly create function for tracing.
  void notifyOfNewFunction(SILFunction *F, SILTransform *T);

  /// Add the function \p F to the function pass worklist.
  /// If not null, the function \p DerivedFrom is the function from which \p F
  /// is derived. This is used to avoid an infinite amount of functions pushed
  /// on the worklist (e.g. caused by a bug in a specializing optimization).
  void addFunctionToWorklist(SILFunction *F, SILFunction *DerivedFrom);

  /// Iterate over all analysis and notify them of the function.
  ///
  /// This function does not necessarily have to be newly created function. It
  /// is the job of the analysis to make sure no extra work is done if the
  /// particular analysis has been done on the function.
  void notifyAnalysisOfFunction(SILFunction *F) {
    for (auto AP : Analyses) {
      AP->notifyAddedOrModifiedFunction(F);
    }
  }

  /// Broadcast the invalidation of the function to all analysis.
  void invalidateAnalysis(SILFunction *F,
                          SILAnalysis::InvalidationKind K) {
    // Invalidate the analysis (unless they are locked)
    for (auto AP : Analyses)
      if (!AP->isLocked())
        AP->invalidate(F, K);
    
    CurrentPassHasInvalidated = true;
    // Any change let all passes run again.
    CompletedPassesMap[F].reset();
  }

  /// Iterate over all analysis and notify them of a change in witness-
  /// or vtables.
  void invalidateFunctionTables() {
    // Invalidate the analysis (unless they are locked)
    for (auto AP : Analyses)
      if (!AP->isLocked())
        AP->invalidateFunctionTables();

    CurrentPassHasInvalidated = true;

    // Assume that all functions have changed. Clear all masks of all functions.
    CompletedPassesMap.clear();
  }

  /// Iterate over all analysis and notify them of a deleted function.
  void notifyWillDeleteFunction(SILFunction *F) {
    // Invalidate the analysis (unless they are locked)
    for (auto AP : Analyses)
      if (!AP->isLocked())
        AP->notifyWillDeleteFunction(F);

    CurrentPassHasInvalidated = true;
    // Any change let all passes run again.
    CompletedPassesMap[F].reset();
  }

  void notifyPassChanges(SILAnalysis::InvalidationKind invalidationKind) {
    changeNotifications = (SILAnalysis::InvalidationKind)
        (changeNotifications | invalidationKind);
  }

  /// Reset the state of the pass manager and remove all transformation
  /// owned by the pass manager. Analysis passes will be kept.
  void resetAndRemoveTransformations();

  /// Set the name of the current optimization stage.
  ///
  /// This is useful for debugging.
  void setStageName(llvm::StringRef NextStage = "");

  /// Get the name of the current optimization stage.
  ///
  /// This is useful for debugging.
  StringRef getStageName() const;

  /// D'tor.
  ~SILPassManager();

  /// Verify all analyses.
  void verifyAnalyses() const {
    for (auto *A : Analyses) {
      A->verify();
    }
  }

  /// Precompute all analyses.
  void forcePrecomputeAnalyses(SILFunction *F) {
    for (auto *A : Analyses) {
      A->forcePrecompute(F);
    }
  }

  /// Verify all analyses, limiting the verification to just this one function
  /// if possible.
  ///
  /// Discussion: We leave it up to the analyses to decide how to implement
  /// this. If no override is provided the SILAnalysis should just call the
  /// normal verify method.
  void verifyAnalyses(SILFunction *F) const {
    for (auto *A : Analyses) {
      A->verify(F);
    }
  }

  void executePassPipelinePlan(const SILPassPipelinePlan &Plan);

private:
  void execute();

  /// Add a pass of a specific kind.
  void addPass(PassKind Kind);

  /// Add a pass with a given name.
  void addPassForName(StringRef Name);

  /// Run the \p TransIdx'th SIL module transform over all the functions in
  /// the module.
  void runModulePass(unsigned TransIdx);

  /// Run the \p TransIdx'th pass on the function \p F.
  void runPassOnFunction(unsigned TransIdx, SILFunction *F);

  /// Run the passes in Transform from \p FromTransIdx to \p ToTransIdx.
  void runFunctionPasses(unsigned FromTransIdx, unsigned ToTransIdx);

  /// Helper function to check if the function pass should be run mandatorily
  /// All passes in mandatory pass pipeline and ownership model elimination are
  /// mandatory function passes.
  bool isMandatoryFunctionPass(SILFunctionTransform *);

  /// A helper function that returns (based on SIL stage and debug
  /// options) whether we should continue running passes.
  bool continueTransforming();

  /// Return true if all analyses are unlocked.
  bool analysesUnlocked();

  /// Dumps information about the pass with index \p TransIdx to llvm::dbgs().
  void dumpPassInfo(const char *Title, SILTransform *Tr, SILFunction *F);

  /// Dumps information about the pass with index \p TransIdx to llvm::dbgs().
  void dumpPassInfo(const char *Title, unsigned TransIdx,
                    SILFunction *F = nullptr);

  /// Displays the call graph in an external dot-viewer.
  /// This function is meant for use from the debugger.
  /// When asserts are disabled, this is a NoOp.
  void viewCallGraph();
};

inline void LibswiftPassInvocation::
notifyChanges(SILAnalysis::InvalidationKind invalidationKind) {
  passManager->notifyPassChanges(invalidationKind);
}

} // end namespace swift

#endif
