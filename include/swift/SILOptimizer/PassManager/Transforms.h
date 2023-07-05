//===--- Transforms.h - Swift Transformations  ------------------*- C++ -*-===//
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
#ifndef SWIFT_SILOPTIMIZER_PASSMANAGER_TRANSFORMS_H
#define SWIFT_SILOPTIMIZER_PASSMANAGER_TRANSFORMS_H

#include "swift/SIL/Notifications.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"


namespace swift {
  class SILModule;
  class SILFunction;
  class SILInstruction;
  class PrettyStackTraceSILFunctionTransform;

  /// The base class for all SIL-level transformations.
  class SILTransform {
  public:
    /// The kind of transformation passes we use.
    enum class TransformKind {
      Function,
      Module,
    };

    /// Stores the kind of derived class.
    const TransformKind Kind;

  protected:
    // The pass manager that runs this pass.
    SILPassManager* PM;

    // The pass kind (used by the pass manager).
    PassKind passKind = PassKind::invalidPassKind;

  public:
    /// C'tor. \p K indicates the kind of derived class.
    SILTransform(SILTransform::TransformKind K) : Kind(K), PM(0) {}

    /// D'tor.
    virtual ~SILTransform() = default;

    /// Returns the kind of derived class.
    TransformKind getKind() const { return Kind; }

    /// Returns the pass kind.
    PassKind getPassKind() const {
      assert(passKind != PassKind::invalidPassKind);
      return passKind;
    }

    /// Sets the pass kind. This should only be done in the add-functions of
    /// the pass manager.
    void setPassKind(PassKind newPassKind) {
      assert(passKind == PassKind::invalidPassKind);
      passKind = newPassKind;
    }

    /// Inject the pass manager running this pass.
    void injectPassManager(SILPassManager *PMM) { PM = PMM; }

    SILPassManager *getPassManager() const { return PM; }

    irgen::IRGenModule *getIRGenModule() {
      auto *Mod = PM->getIRGenModule();
      assert(Mod && "Expecting a valid module");
      return Mod;
    }

    /// Get the transform's (command-line) tag.
    llvm::StringRef getTag() { return PassKindTag(getPassKind()); }

    /// Get the transform's name as a C++ identifier.
    llvm::StringRef getID() { return PassKindID(getPassKind()); }

    /// Searches for an analysis of type T in the list of registered
    /// analysis. If the analysis is not found, the program terminates.
    template<typename T>
    T* getAnalysis() { return PM->getAnalysis<T>(); }

    template<typename T>
    T* getAnalysis(SILFunction *f) { return PM->getAnalysis<T>(f); }

    const SILOptions &getOptions() { return PM->getOptions(); }
  };

  /// A transformation that operates on functions.
  class SILFunctionTransform : public SILTransform {
    friend class PrettyStackTraceSILFunctionTransform;
    SILFunction *F;

  public:
    /// C'tor.
    SILFunctionTransform() : SILTransform(TransformKind::Function), F(0) {}

    /// The entry point to the transformation.
    virtual void run() = 0;

    static bool classof(const SILTransform *S) {
      return S->getKind() == TransformKind::Function;
    }

    void injectFunction(SILFunction *Func) { F = Func; }

    /// Notify the pass manager of a function \p F that needs to be
    /// processed by the function passes and the analyses.
    ///
    /// If not null, the function \p DerivedFrom is the function from which \p F
    /// is derived. This is used to limit the number of new functions which are
    /// derived from a common base function, e.g. due to specialization.
    /// The number should be small anyway, but bugs in optimizations could cause
    /// an infinite loop in the passmanager.
    void addFunctionToPassManagerWorklist(SILFunction *F,
                                          SILFunction *DerivedFrom) {
      PM->addFunctionToWorklist(F, DerivedFrom);
    }

    /// Reoptimize the current function by restarting the pass
    /// pipeline on it.
    void restartPassPipeline() { PM->restartWithCurrentFunction(this); }

    SILFunction *getFunction() { return F; }

    bool continueWithNextSubpassRun(SILInstruction *forInst = nullptr) {
      return PM->continueWithNextSubpassRun(forInst, F, this);
    }

    void invalidateAnalysis(SILAnalysis::InvalidationKind K) {
      PM->invalidateAnalysis(F, K);
    }
  };

  /// A transformation that operates on modules.
  class SILModuleTransform : public SILTransform {
    SILModule *M;

  public:
    /// C'tor.
    SILModuleTransform() : SILTransform(TransformKind::Module), M(0) {}

    /// The entry point to the transformation.
    virtual void run() = 0;

    static bool classof(const SILTransform *S) {
      return S->getKind() == TransformKind::Module;
    }

    void injectModule(SILModule *Mod) { M = Mod; }

    SILModule *getModule() { return M; }

    /// Invalidate all analysis data for the whole module.
    void invalidateAll() {
      PM->invalidateAllAnalysis();
    }

    /// Invalidate only the function \p F, using invalidation information \p K.
    void invalidateAnalysis(SILFunction *F, SILAnalysis::InvalidationKind K) {
      PM->invalidateAnalysis(F, K);
    }

    /// Invalidate the analysis data for witness- and vtables.
    void invalidateFunctionTables() {
      PM->invalidateFunctionTables();
    }

    /// Inform the pass manager that we are going to delete a function.
    void notifyWillDeleteFunction(SILFunction *F) {
      PM->notifyWillDeleteFunction(F);
    }
  };
} // end namespace swift

#endif
