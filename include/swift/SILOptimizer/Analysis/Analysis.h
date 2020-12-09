//===--- Analysis.h  - Swift Analysis ---------------------------*- C++ -*-===//
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

#ifndef SWIFT_SILOPTIMIZER_ANALYSIS_ANALYSIS_H
#define SWIFT_SILOPTIMIZER_ANALYSIS_ANALYSIS_H

#include "swift/Basic/NullablePtr.h"
#include "swift/SIL/Notifications.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Casting.h"
#include <type_traits>

namespace swift {

class SILModule;
class SILFunction;
class SILPassManager;

/// A list of the known analysis.
struct SILAnalysisKind {
  enum InnerTy {
#define ANALYSIS(NAME) NAME,
#include "Analysis.def"
  } value;

  SILAnalysisKind(InnerTy newValue) : value(newValue) {}
  operator InnerTy() const { return value; }
};

/// The base class for all SIL-level analysis.
class SILAnalysis : public DeleteNotificationHandler {
public:
  /// This is a list of values that allow passes to communicate to analysis
  /// which traits of the code were invalidated. Based on this information
  /// the analysis can decide if it needs to be invalidated. This information
  /// may refer to a specific function or to the whole module depending on
  /// the context in which it is used.
  enum InvalidationKind : unsigned {
    /// The pass does not change anything.
    Nothing = 0x0,

    /// The pass created, deleted or rearranged some instructions in a
    /// function.
    Instructions = 0x1,

    /// The pass modified some calls (apply instructions).
    ///
    /// The intention of this invalidation kind is to allow analysis that
    /// rely on a specific call graph structure to recompute themselves.
    Calls = 0x2,

    /// A pass has invalidated some branches in the program.
    ///
    /// The intention of this invalidation kind is to tell analyses like the
    /// Dominance Analysis and the PostOrder Analysis that the underlying CFG
    /// has been modified.
    Branches = 0x4,

    /// Convenience states:
    FunctionBody = Calls | Branches | Instructions,

    CallsAndInstructions = Calls | Instructions,

    BranchesAndInstructions = Branches | Instructions,

    Everything = Calls | Branches | Instructions,
  };

private:
  /// Stores the kind of derived class.
  const SILAnalysisKind kind;

  /// A lock that prevents the invalidation of this analysis. When this
  /// variable is set to True then the PassManager should not invalidate
  /// this analysis.
  bool invalidationLock;

public:
  /// Returns the kind of derived class.
  SILAnalysisKind getKind() const { return kind; }

  /// Constructor.
  SILAnalysis(SILAnalysisKind k) : kind(k), invalidationLock(false) {}

  /// Destructor.
  virtual ~SILAnalysis() {}
  
  /// Can be used to retrieve other analysis passes from \p PM, which this
  /// analysis depends on.
  virtual void initialize(SILPassManager *pm) { }

  /// Lock the analysis. This means that invalidation messages are ignored.
  void lockInvalidation() { invalidationLock = true; }

  /// Unlock the analysis. This means that invalidation messages are handled.
  void unlockInvalidation() { invalidationLock = false; }

  /// Return True if this analysis is locked and should not be invalidated.
  bool isLocked() const { return invalidationLock; }

  /// Invalidate all information in this analysis.
  virtual void invalidate() = 0;

  /// Invalidate all of the information for a specific function.
  virtual void invalidate(SILFunction *f, InvalidationKind k) = 0;

  /// Notify the analysis about a newly added or modified function.
  virtual void notifyAddedOrModifiedFunction(SILFunction *f) = 0;

  /// Notify the analysis about a function that will be deleted from the
  /// module.
  virtual void notifyWillDeleteFunction(SILFunction *f) = 0;

  /// Notify the analysis about changed witness or vtables.
  virtual void invalidateFunctionTables() = 0;

  /// Verify the state of this analysis.
  virtual void verify() const {}

  /// Verify the state of this analysis limited to this one function if
  /// possible.
  ///
  /// By default this just calls the module verify function as a sensible
  /// default so that only functions which are able to provide the function
  /// specific verification will do so.
  virtual void verify(SILFunction *F) const { verify(); }

  virtual void forcePrecompute(SILFunction *F) {}

  /// Perform a potentially more expensive verification of the state of this
  /// analysis.
  ///
  /// The purpose of this is to allow for more expensive verification that is
  /// fast enough to run at the end of a pass manager once vs in between all
  /// passes when -sil-verify-all is enabled.
  ///
  /// TODO: By default this is a no-op, but really it should call
  /// verify(). Today doing a full verification seems to catch verification
  /// errors when compiling the stdlib/overlays.
  virtual void verifyFull() const {}

  /// Verify that the function \p F can be used by the analysis.
  static void verifyFunction(SILFunction *F);
};

inline SILAnalysis::InvalidationKind
operator|(SILAnalysis::InvalidationKind lhs,
          SILAnalysis::InvalidationKind rhs) {
  using InvalidationKind = SILAnalysis::InvalidationKind;
  using UnderlyingKind = std::underlying_type<InvalidationKind>::type;
  return InvalidationKind(UnderlyingKind(lhs) | UnderlyingKind(rhs));
}

/// RAII helper for locking analyses. Locks the analysis upon construction and
/// unlocks upon destruction.
class AnalysisPreserver {
  SILAnalysis *analysis;

public:
  AnalysisPreserver(SILAnalysis *a) : analysis(a) {
    analysis->lockInvalidation();
  }
  ~AnalysisPreserver() {
    analysis->unlockInvalidation();
  }
};

/// An abstract base class that implements the boiler plate of caching and
/// invalidating analysis for specific functions.
///
/// The usage expectation is that the derived function analysis will inherit
/// from FunctionAnalysisBase and pass in as a template argument the
/// "FunctionInfoTy" struct as a template argument. The FunctionInfoTy struct
/// should represent all of the information that the analysis should store about
/// an individual function. As a toy example:
///
/// ```
/// struct TriviallyDeadAnalysisFunctionInfo {
///   bool isTriviallyDead;
/// };
///
/// class TriviallyDeadAnalysis
///   : public FunctionAnalysisBase<TriviallyDeadAnalysisFunctionInfo> { ... }
/// ```
template <typename FunctionInfoTy>
class FunctionAnalysisBase : public SILAnalysis {
  using StorageTy = llvm::DenseMap<SILFunction *,
                                   std::unique_ptr<FunctionInfoTy>>;

  /// Maps functions to their analysis provider.
  StorageTy storage;

protected:
  /// Construct a new empty function info for a specific function \p F.
  virtual std::unique_ptr<FunctionInfoTy>
  newFunctionAnalysis(SILFunction *f) = 0;

  /// Return True if the analysis should be invalidated given trait \K is
  /// preserved.
  virtual bool shouldInvalidate(SILAnalysis::InvalidationKind k) = 0;

  /// A stub function that verifies the specific AnalysisTy \p A. This is
  /// meant to be overridden by subclasses.
  virtual void verify(FunctionInfoTy *funcInfo) const {}

  void deleteAllAnalysisProviders() {
    storage.clear();
  }

public:
  /// Returns true if we have data for a specific function \p F without actually
  /// attempting to construct the function info.
  bool hasFunctionInfo(SILFunction *f) const { return storage.count(f); }

  /// Attempt to lookup up the information that the analysis has for the given
  /// function. Returns nullptr upon failure.
  NullablePtr<FunctionInfoTy> maybeGet(SILFunction *f) {
    auto iter = storage.find(f);
    if (iter == storage.end())
      return nullptr;
    return iter->second.get();
  }

  /// Returns a function info structure for a specific function \p F.
  FunctionInfoTy *get(SILFunction *f) {
    // Check that the analysis can handle this function.
    verifyFunction(f);

    auto &it = storage.FindAndConstruct(f);
    if (!it.second)
      it.second = newFunctionAnalysis(f);
    return it.second.get();
  }

  virtual void forcePrecompute(SILFunction *f) override {
    // Check that the analysis can handle this function.
    verifyFunction(f);

    auto &it = storage.FindAndConstruct(f);
    if (!it.second)
      it.second = newFunctionAnalysis(f);
  }

  /// Invalidate all information in this analysis.
  virtual void invalidate() override {
    deleteAllAnalysisProviders();
  }

  /// Helper function to remove the function info for a specific function.
  void invalidateFunction(SILFunction *f) {
    storage.erase(f);
  }

  /// Invalidate all of the information for a specific function.
  virtual void invalidate(SILFunction *f,
                          SILAnalysis::InvalidationKind k) override {
    if (shouldInvalidate(k))
      invalidateFunction(f);
  }

  /// Notify the analysis about a newly created function.
  virtual void notifyAddedOrModifiedFunction(SILFunction *f) override {}

  /// Notify the analysis about a function which will be deleted from the
  /// module.
  virtual void notifyWillDeleteFunction(SILFunction *f) override {
    invalidateFunction(f);
  }

  /// Notify the analysis about changed witness or vtables.
  virtual void invalidateFunctionTables() override {}

  FunctionAnalysisBase() {}
  virtual ~FunctionAnalysisBase() {
    deleteAllAnalysisProviders();
  }

  FunctionAnalysisBase(SILAnalysisKind k) : SILAnalysis(k), storage() {}
  FunctionAnalysisBase(const FunctionAnalysisBase &) = delete;
  FunctionAnalysisBase &operator=(const FunctionAnalysisBase &) = delete;

  /// Verify all of the AnalysisTy for all functions.
  ///
  /// This is not meant to be overridden by subclasses. Instead please override
  /// void FunctionAnalysisBase::verify(FunctionInfoTy *fInfo).
  virtual void verify() const override final {
    for (auto &entry : storage) {
      if (!entry.second)
        continue;
      verify(entry.second.get());
    }
  }

  /// Verify the FunctionInfoTy that we have stored for the specific function \p
  /// F.
  ///
  /// This is not meant to be overridden by subclasses. Instead, please
  /// override: void FunctionAnalysisBase::verify(FunctionInfoTy *fInfo).
  virtual void verify(SILFunction *f) const override final {
    auto iter = storage.find(f);
    if (iter == storage.end())
      return;
    if (!iter->second)
      return;
    verify(iter->second.get());
  }
};

/// Given a specific type of analysis and its function info. Store the
/// analysis and upon request instantiate the function info, caching the
/// function info for subsequent requests.
template <class AnalysisTy, class FunctionInfoTy>
class LazyFunctionInfo {
  SILFunction *func;
  AnalysisTy *analysis;
  NullablePtr<FunctionInfoTy> funcInfo;

public:
  LazyFunctionInfo(SILFunction *func, AnalysisTy *analysis) : func(func), analysis(analysis), funcInfo() {}

  operator FunctionInfoTy *() {
    if (funcInfo.isNull()) {
      funcInfo = analysis->get(func);
    }

    return funcInfo.get();
  }

  FunctionInfoTy *operator->() { return *this; }
};

#define ANALYSIS(NAME) SILAnalysis *create##NAME##Analysis(SILModule *);
#include "Analysis.def"

} // end namespace swift

#endif

