//===--- CallerAnalysis.h - Determine callees per call site -----*- C++ -*-===//
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

#ifndef SWIFT_SILOPTIMIZER_ANALYSIS_CALLERANALYSIS_H
#define SWIFT_SILOPTIMIZER_ANALYSIS_CALLERANALYSIS_H

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"

namespace swift {

/// A lazy caller analysis that works by only recomputing its state upon
/// an ask for information.
///
/// This laziness is implemented by the pass invalidating its internal state for
/// a function F when receiving an invalidation message for F and then adding F
/// to a recompute list. When the method getFunctionInfo is called, the
/// recompute list is used to recompute all of the invalidated state.
///
/// Originally, swift had an eagerly recomputed caller analysis. This was found
/// to cause large compile time problems since after every invalidation we
/// needed to walk every function in the module to update any invalidated
/// state. This in combination with a sequence of invalidating function passes
/// can lead to many invalidations even if the information is not used.
///
/// NOTE: We use the term caller in a loose way here since in this analysis, we
/// consider partial applies of the function to be "callers" of the function.
class CallerAnalysis final : public SILAnalysis {
public:
  class FunctionInfo;

private:
  struct CallerInfo;
  struct ApplySiteFinderVisitor;

  /// Current module we are analyzing.
  SILModule &mod;

  /// A map between all the functions and their callsites in the module.
  mutable llvm::DenseMap<SILFunction *, FunctionInfo> funcInfos;

  /// A set of functions that needs to be recomputed before we can serve
  /// queries.
  llvm::SetVector<SILFunction *> recomputeFunctionList;

public:
  CallerAnalysis(SILModule *m);

  static bool classof(const SILAnalysis *s) {
    return s->getKind() == SILAnalysisKind::Caller;
  }

  /// Invalidate all information in this analysis.
  void invalidate() override;

  /// Invalidate all of the information for a specific caller function.
  void invalidate(SILFunction *caller, InvalidationKind k) override {
    // Should we invalidate based on the invalidation kind.
    bool shouldInvalidate = k & InvalidationKind::CallsAndInstructions;
    if (!shouldInvalidate)
      return;

    // This function has become "unknown" to us. Invalidate any callsite
    // information related to this function.
    invalidateKnownCallees(caller);

    // Make sure this function is recomputed next time.
    recomputeFunctionList.insert(caller);
  }

  /// Notify the analysis about a newly created or modified function.
  void notifyAddedOrModifiedFunction(SILFunction *f) override {
    auto &fInfo = getOrInsertFunctionInfo(f);
    (void)fInfo;
    recomputeFunctionList.insert(f);
  }

  /// Notify the analysis about a function which will be deleted from the
  /// module.
  void notifyWillDeleteFunction(SILFunction *f) override;

  /// Notify the analysis about changed witness or vtables.
  ///
  /// Today this is unimplemented since the CallerAnalysis does not attempt...
  /// yet... to understand class_method or witness_method. Once that is
  /// is implemented,
  void invalidateFunctionTables() override {}

  /// Look up the function info that we have stored for f, recomputing all
  /// invalidating parts of the call graph.
  ///
  /// Warning: The returned FunctionInfo is only alive until the next call to
  /// `getFunctionInfo`.
  const FunctionInfo &getFunctionInfo(SILFunction *f) const;

  SWIFT_DEBUG_DUMP;

  /// Print the state of the caller analysis as a sequence of yaml documents for
  /// each callee we are tracking.
  void print(llvm::raw_ostream &os) const;

  /// Print the state of the caller analysis as a sequence of yaml documents for
  /// each callee we are tracking to the passed in file path.
  SWIFT_DEBUG_DUMPER(print(const char *filePath));

  void verify() const override;
  void verify(SILFunction *f) const override;

private:
  /// Iterate over all the call sites in the function and update
  /// CallInfo.
  void processFunctionCallSites(SILFunction *f);

  /// This function is about to become "unknown" to us. Invalidate any
  /// callsite information related to it.
  ///
  /// This is a function that just looks up the function info for f and then
  /// calls:
  ///
  /// void invalidateKnownCallees(SILFunction *caller,
  ///       FunctionInfo &callerInfo);
  void invalidateKnownCallees(SILFunction *f);

  /// Using the passed in caller info and caller function, eliminate the edge
  /// inbetween the caller and its callees.
  ///
  /// NOTE: This does not remove the "book keeping" backedges from the caller
  /// function to its own set of callers. This must be invalidated by using
  /// invalidateAllInfo.
  void invalidateKnownCallees(SILFunction *caller, FunctionInfo &callerInfo);

  /// Invalidate both the known callees of f and the known callers of f.
  void invalidateAllInfo(SILFunction *f, FunctionInfo &fInfo);

  /// Helper method that reprocesses all elements of recomputeFunctionList and
  /// then clears the function list.
  ///
  /// NOTE: This is the part of the analysis that performs the lazy
  /// recomputation upon access.
  void processRecomputeFunctionList() {
    for (auto &f : recomputeFunctionList) {
      processFunctionCallSites(f);
    }
    recomputeFunctionList.clear();
  }

  /// Internal only way for getting a caller info. Will insert f if needed and
  /// _WILL NOT_ preform any recomputation of the callgraph.
  FunctionInfo &getOrInsertFunctionInfo(SILFunction *f);

  /// Internal only method for unsafely getting the FunctionInfo for the
  /// SILFunction \p f.
  ///
  /// This is valid since we assume that we have maintained our datastructures
  /// via notifications for functions being added/destroyed.
  ///
  /// NOTE: In asserts builds this routine asserts if we do not have function
  /// info for f.
  /// NOTE: This routine _WILL NOT_ perform any recomputation of the callgraph.
  FunctionInfo &unsafeGetFunctionInfo(SILFunction *f);

  /// const_cast version of FunctionInfo &unsafeGetFunctionInfo(SILFunction *).
  const FunctionInfo &unsafeGetFunctionInfo(SILFunction *f) const;

  /// Helper function for verification that hoists out the callerInfo.
  void verify(SILFunction *caller, const FunctionInfo &callerInfo) const;
};

/// Auxiliary information that we store about a specific caller.
struct CallerAnalysis::CallerInfo {
  /// Given a SILFunction F that contains at least one partial apply of the
  /// given function, map F to the minimum number of partial applied
  /// arguments of any partial application in F.
  ///
  /// By storing the minimum number of partial applied arguments, we are able
  /// to decide quickly if we are able to eliminate dead captured arguments.
  ///
  /// NOTE: We know that we will never have more than 2^16 generic arguments due
  /// to the runtime implementation.
  uint8_t numPartiallyAppliedArguments[2];

  /// A boolean flag to say if we actually have partially applied arguments.
  ///
  /// Otherwise it would be unable to distinguish not having partially applied
  /// arguments from having zero partially applied argument.
  bool hasPartiallyAppliedArguments : 1;

  /// True if this caller performs at least one full application of the
  /// callee.
  bool hasFullApply : 1;

  /// True if this caller can guarantee that all direct caller's of this
  /// function inside of it can be found.
  ///
  /// NOTE: This does not imply that a function can not be called
  /// indirectly. That is a separate query that is type system specific.
  bool isDirectCallerSetComplete : 1;

  llvm::Optional<unsigned> getNumPartiallyAppliedArguments() const {
    if (!hasPartiallyAppliedArguments) {
      return llvm::None;
    }

    auto *x = reinterpret_cast<const uint16_t *>(numPartiallyAppliedArguments);
    return unsigned(*x);
  }

  void setNumPartiallyAppliedArguments(unsigned newValue) {
    hasPartiallyAppliedArguments = true;
    auto *x = reinterpret_cast<uint16_t *>(numPartiallyAppliedArguments);
    x[0] = uint16_t(newValue);
  }
};

/// This is a representation of the caller information that we have associated
/// with a specific function.
///
/// NOTE: If needed, this can be extended to contain the callsites of the
/// function. Today we do not provide any functionality that requires knowledge
/// of exact callsites, so it would just be a waste of memory.
class CallerAnalysis::FunctionInfo {
  friend class CallerAnalysis;
  using CallerInfo = CallerAnalysis::CallerInfo;
  struct YAMLRepresentation;

  // A static_assert to make sure that CallerInfo remains 3 bytes for
  // performance reasons. We are going to store a bunch of these in callerStates
  // so we want them to be as small as possible.
  static_assert(sizeof(CallerAnalysis::CallerInfo) == 3,
                "Expected caller info to be 3 bytes for performance reasons");

  /// A map from a function containing uses of a function_ref of the callee to
  /// the state that we store about the caller's body.
  llvm::SmallMapVector<SILFunction *, CallerInfo, 1> callerStates;

  /// Private helper type to reduce 80 column violations.
  using CallerStatesValueType = decltype(callerStates)::value_type;

  /// This set vector maintains edges from a function to its callees.
  ///
  /// This is used in caller analysis to quickly invalidate all of the callee
  /// information associated with a function when the function's body is
  /// invalidated.
  ///
  /// NOTE: If a function is deleted, we can not only use this callee set. We
  /// also have to eliminate caller edges pointing at the function.
  ///
  /// \see CallerAnalysis::invalidateExistingCalleeRelation.
  llvm::SmallSetVector<SILFunction *, 1> calleeStates;

  /// True if this function is something that could be called via a vtable or
  /// a witness table. This does not include escaping uses.
  ///
  /// TODO: For now this is very conservative and is only set to false if we
  /// have a function whose representation never can escape. In future cases, we
  /// should consider refining this to take into account the compilation
  /// visibility of a protocol conformance or class.
  bool mayHaveIndirectCallers : 1;

  /// Whether the function is sufficiently visible to be called by a different
  /// module.
  bool mayHaveExternalCallers : 1;

public:
  FunctionInfo(SILFunction *f);

  /// Returns true if and only if the analysis was able to find all direct
  /// callers of this function /and/ if the function can not be called
  /// indirectly given visibility and compilation mode.
  ///
  /// This implies that the function can be replaced by an optimized form of the
  /// function (e.g. a specialized function) without needing to introduce a
  /// thunk since we can rewrite all of the callers to call the new function.
  bool foundAllCallers() const {
    return hasOnlyCompleteDirectCallerSets() && !mayHaveIndirectCallers &&
           !mayHaveExternalCallers;
  }

  /// Returns true if this function has at least one direct caller.
  bool hasDirectCaller() const {
    return callerStates.size() &&
           llvm::any_of(callerStates, [](const CallerStatesValueType &v) {
             return v.second.hasFullApply;
           });
  }

  /// Returns the minimum number of partially applied arguments over all partial
  /// applications of this function.
  ///
  /// This is useful information to have since in many cases, all of the partial
  /// applies of a function partially apply the same number of arguments. In
  /// such a case, we may be able to eliminate some of the partially applied
  /// arguments without increasing code-size.
  ///
  /// NOTE: This returning non-zero /does not/ mean that we found all such apply
  /// sites.
  unsigned getMinPartialAppliedArgs() const {
    if (callerStates.empty())
      return 0;

    bool foundArg = false;
    unsigned minArgs = UINT_MAX;
    for (const auto &iter : callerStates) {
      if (auto numArgs = iter.second.getNumPartiallyAppliedArguments()) {
        foundArg = true;
        minArgs = std::min(minArgs, numArgs.value());
      }
    }

    return foundArg ? minArgs : 0;
  }

  /// Returns true if we were able to find all direct callers of this function.
  ///
  /// NOTE: The function may still be called indirectly. To ascertain if we
  /// found all of the function's direct callers and that it does not have any
  /// indirect callers, \see FunctionInfo::foundAllCallers().
  bool hasOnlyCompleteDirectCallerSets() const {
    return llvm::all_of(callerStates, [](const CallerStatesValueType &v) {
      return v.second.isDirectCallerSetComplete;
    });
  }

  /// Return a range containing the partial and full apply sites that we found
  /// in the given caller for our callee.
  auto getAllReferencingCallers() const
      -> decltype(llvm::make_range(callerStates.begin(), callerStates.end())) {
    return llvm::make_range(callerStates.begin(), callerStates.end());
  }

  SWIFT_DEBUG_DUMP;

  void print(llvm::raw_ostream &os) const;
};

} // end namespace swift

#endif
