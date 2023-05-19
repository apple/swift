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
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/PassPipeline.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Chrono.h"
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

/// Utility class to invoke Swift passes.
class SwiftPassInvocation {
  /// Backlink to the pass manager.
  SILPassManager *passManager;

  /// The current transform.
  SILTransform *transform = nullptr;

  /// The currently optimized function.
  SILFunction *function = nullptr;

  /// Non-null if this is an instruction pass, invoked from SILCombine.
  SILCombiner *silCombiner = nullptr;

  /// Change notifications, collected during a pass run.
  SILAnalysis::InvalidationKind changeNotifications =
      SILAnalysis::InvalidationKind::Nothing;

  /// All slabs, allocated by the pass.
  SILModule::SlabList allocatedSlabs;

  static constexpr int BlockSetCapacity = 8;
  char blockSetStorage[sizeof(BasicBlockSet) * BlockSetCapacity];
  bool aliveBlockSets[BlockSetCapacity];
  int numBlockSetsAllocated = 0;

  static constexpr int NodeSetCapacity = 8;
  char nodeSetStorage[sizeof(NodeSet) * NodeSetCapacity];
  bool aliveNodeSets[NodeSetCapacity];
  int numNodeSetsAllocated = 0;

  bool needFixStackNesting = false;

  void endPassRunChecks();

public:
  SwiftPassInvocation(SILPassManager *passManager, SILFunction *function,
                         SILCombiner *silCombiner) :
    passManager(passManager), function(function), silCombiner(silCombiner) {}

  SwiftPassInvocation(SILPassManager *passManager) :
    passManager(passManager) {}

  SILPassManager *getPassManager() const { return passManager; }
  
  SILTransform *getTransform() const { return transform; }

  SILFunction *getFunction() const { return function; }

  FixedSizeSlab *allocSlab(FixedSizeSlab *afterSlab);

  FixedSizeSlab *freeSlab(FixedSizeSlab *slab);

  BasicBlockSet *allocBlockSet();

  void freeBlockSet(BasicBlockSet *set);

  NodeSet *allocNodeSet();

  void freeNodeSet(NodeSet *set);

  /// The top-level API to erase an instruction, called from the Swift pass.
  void eraseInstruction(SILInstruction *inst);

  /// Called by the pass when changes are made to the SIL.
  void notifyChanges(SILAnalysis::InvalidationKind invalidationKind);

  /// Called by the pass manager before the pass starts running.
  void startModulePassRun(SILModuleTransform *transform);

  /// Called by the pass manager before the pass starts running.
  void startFunctionPassRun(SILFunctionTransform *transform);

  /// Called by the SILCombiner before the instruction pass starts running.
  void startInstructionPassRun(SILInstruction *inst);

  /// Called by the pass manager when the pass has finished.
  void finishedModulePassRun();

  /// Called by the pass manager when the pass has finished.
  void finishedFunctionPassRun();

  /// Called by the SILCombiner when the instruction pass has finished.
  void finishedInstructionPassRun();
  
  void beginTransformFunction(SILFunction *function);

  void endTransformFunction();

  void setNeedFixStackNesting(bool newValue) { needFixStackNesting = newValue; }
  bool getNeedFixStackNesting() const { return needFixStackNesting; }
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
  unsigned numSubpassesRun = 0;

  unsigned maxNumPassesToRun = UINT_MAX;
  unsigned maxNumSubpassesToRun = UINT_MAX;

  /// For invoking Swift passes.
  SwiftPassInvocation swiftPassInvocation;

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

  bool currentPassDependsOnCalleeBodies = false;

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

  std::chrono::nanoseconds totalPassRuntime = std::chrono::nanoseconds(0);

public:
  /// C'tor. It creates and registers all analysis passes, which are defined
  /// in Analysis.def.
  SILPassManager(SILModule *M, bool isMandatory, irgen::IRGenModule *IRMod);

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

  SwiftPassInvocation *getSwiftPassInvocation() {
    return &swiftPassInvocation;
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

  void setDependingOnCalleeBodies() {
    currentPassDependsOnCalleeBodies = true;
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
  void verifyAnalyses() const;

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
  void verifyAnalyses(SILFunction *F) const;

  void executePassPipelinePlan(const SILPassPipelinePlan &Plan);

  bool continueWithNextSubpassRun(SILInstruction *forInst, SILFunction *function,
                                  SILTransform *trans);

  static bool isPassDisabled(StringRef passName);
  static bool isInstructionPassDisabled(StringRef instName);
  static bool disablePassesForFunction(SILFunction *function);

private:
  bool doPrintBefore(SILTransform *T, SILFunction *F);

  bool doPrintAfter(SILTransform *T, SILFunction *F, bool PassChangedSIL);

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
  void dumpPassInfo(const char *Title, SILTransform *Tr, SILFunction *F,
                    int passIdx = -1);

  /// Dumps information about the pass with index \p TransIdx to llvm::dbgs().
  void dumpPassInfo(const char *Title, unsigned TransIdx,
                    SILFunction *F = nullptr);

  /// Displays the call graph in an external dot-viewer.
  /// This function is meant for use from the debugger.
  /// When asserts are disabled, this is a NoOp.
  void viewCallGraph();
};

inline void SwiftPassInvocation::
notifyChanges(SILAnalysis::InvalidationKind invalidationKind) {
    changeNotifications = (SILAnalysis::InvalidationKind)
        (changeNotifications | invalidationKind);
}

} // end namespace swift

#endif
