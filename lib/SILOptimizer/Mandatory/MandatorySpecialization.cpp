//===--- MandatorySpecialization.cpp -
//  Specialize functions marked with "must_specialize" attribute ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-mandatory-specializer"

#include "swift/AST/DiagnosticsSema.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"

using namespace swift;

namespace {

//===----------------------------------------------------------------------===//
//                            MandatorySpecializationPass
//===----------------------------------------------------------------------===//

class MandatorySpecializationPass : public SILModuleTransform {
  SILOptFunctionBuilder *functionBuilder;
  llvm::SmallPtrSet<SILFunction *, 32> visitedFunctions;
  llvm::SmallPtrSet<SILInstruction *, 32> mustSpecializeApplySites;

  // For any given function, find all of its callers.
  llvm::DenseMap<SILFunction *, SmallVector<SILFunction *, 4>> functionCallers;
  llvm::DenseMap<SILFunction *, SmallVector<FullApplySite, 4>> applySites;

  // "recursivelySpecializeCalls" is called after "correctCallerFnAttributes"
  // so we know any function without a concrete set of generic substitutions is
  // marked "must_specialize". "recursivelySpecializeCalls" should be passed a
  // function WITH OUT the must_specialize attribute. This means that all of its
  // apply sites of "must_specialize" functions are concrete, so specilization
  // should never fail. As stated above, we start at the top level with
  // functions that aren't marked as "must_specialize". Once a specialized
  // function is created, it is passed to "recursivelySpecializeCalls" (hence
  // the "recursively" part of the name). The newly created (and specialized)
  // function is known to only have concrete apply sites, so everything should
  // just work, and again, specialization of functions called in the newly
  // specialized function will never fail.
  void recursivelySpecializeCalls(SILFunction &fn) {
    // Don't create an infinite loop.
    if (!visitedFunctions.insert(&fn).second)
      return;

    SmallVector<SILInstruction *, 64> worklist;
    for (auto &bb : fn) {
      for (auto &inst : bb) {
        worklist.push_back(&inst);
      }
    }

    for (auto *inst : worklist) {
      if (!FullApplySite::isa(inst))
        continue;

      FullApplySite apply(inst);
      SILFunction *callee = apply.getCalleeFunction();
      if (!callee->hasSemanticsAttr(semantics::MUST_SPECIALIZE))
        continue;

      // If the caller and callee are both fragile, preserve the fragility when
      // cloning the callee. Otherwise, strip it off so that we can optimize
      // the body more.
      //
      // If it is OnoneSupport consider all specializations as non-serialized
      // as we do not SIL serialize their bodies.
      // It is important to set this flag here, because it affects the
      // mangling of the specialization's name.
      IsSerialized_t serialized = IsNotSerialized;
      if (fn.isSerialized() && callee->isSerialized())
        serialized = getModule()->isOptimizedOnoneSupportModule()
                         ? IsNotSerialized
                         : IsSerializable;

      OptRemark::Emitter optRemarkEmitter(DEBUG_TYPE, fn);

      ReabstractionInfo reInfo(
          getModule()->getSwiftModule(), getModule()->isWholeModule(), apply,
          callee, apply.getSubstitutionMap(), serialized,
          /*ConvertIndirectToDirect=*/true, &optRemarkEmitter);
      assert(reInfo.canBeSpecialized());

      GenericFuncSpecializer functionSpecializer(
          *functionBuilder, callee, apply.getSubstitutionMap(), reInfo);
      SILFunction *specializedFn = functionSpecializer.lookupSpecialization();
      if (!specializedFn) {
        specializedFn = functionSpecializer.tryCreateSpecialization(true);
        specializedFn->removeSemanticsAttr(semantics::MUST_SPECIALIZE);
      }
      assert(specializedFn);

      mustSpecializeApplySites.erase(apply.getInstruction());

      replaceWithSpecializedFunction(apply, specializedFn, reInfo);
      recursivelyDeleteTriviallyDeadInstructions(apply.getInstruction(), true);

      // It's important to do this top-down so we do it recursively.
      recursivelySpecializeCalls(*specializedFn);
    }
  }

  void buildCallGraph(SILFunction &fn) {
    for (auto &bb : fn) {
      for (auto &inst : bb) {
        if (!FullApplySite::isa(&inst))
          continue;

        FullApplySite apply(&inst);
        applySites[&fn].push_back(apply);
        SILFunction *callee = apply.getCalleeFunction();
        functionCallers[callee].push_back(&fn);
      }
    }
  }

  // Make sure that any functions that call functions that are marked as
  // 'must_specialize' are themselves marked as 'must_specialize'.
  void correctCallerFnAttributes(
      SILFunction &fn, SmallPtrSet<SILFunction *, 32> &correctedFunctions) {
    if (!fn.hasSemanticsAttr(semantics::MUST_SPECIALIZE) ||
        !correctedFunctions.insert(&fn).second)
      return;

    for (SILFunction *caller : functionCallers[&fn]) {
      // If this *is* a generic function and it doesn't have this attribute,
      // make sure we correct that. If this is not a generic function, then it
      // means we've reached the "top".
      if (!caller->getLoweredFunctionType()->getInvocationGenericSignature())
        continue;

      // If all apply sites of "must_specialize" functions have full concrete
      // substitutions, then it also means we've reached the "top".
      if (llvm::all_of(applySites[caller], [](FullApplySite apply) {
            return !apply.getCalleeFunction()->hasSemanticsAttr(
                       semantics::MUST_SPECIALIZE) ||
                   llvm::all_of(
                       apply.getSubstitutionMap().getReplacementTypes(),
                       [](Type type) { return type->getAnyGeneric(); });
          }))
        continue;

      // Otherwise, propagate the attribute up to the caller.
      if (!caller->hasSemanticsAttr(semantics::MUST_SPECIALIZE))
        caller->addSemanticsAttr(semantics::MUST_SPECIALIZE);
      correctCallerFnAttributes(*caller, correctedFunctions);
    }
  }

  /// \returns true if verification failed and the compiler will exit.
  bool verifySpecialized() {
    bool failed = false;
    // Error if there are any left over calls to 'must_specialize' functions.
    for (auto *applyInst : mustSpecializeApplySites) {
      FullApplySite apply(applyInst);
      // If someone calls this function, we'll diagnose the error in the caller
      // function.
      if (functionCallers[apply.getFunction()].size())
        continue;

      auto fnName = Demangle::demangleSymbolAsString(
          apply.getFunction()->getName(),
          Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
      auto calleeName = Demangle::demangleSymbolAsString(
          apply.getCalleeFunction()->getName(),
          Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
      getModule()->getASTContext().Diags.diagnose(
          apply.getFunction()->getLocation().getSourceLoc(),
          diag::must_specialize_function_never_specialized, {fnName});
      getModule()->getASTContext().Diags.diagnose(
          applyInst->getLoc().getSourceLoc(),
          diag::note_must_specialize_function_called_here, {calleeName});
      failed = true;
    }

    // Check that all functions marked 'must_specialize' are not publically
    // available.
    for (auto &fn : getModule()->getFunctions()) {
      if (fn.hasSemanticsAttr(semantics::MUST_SPECIALIZE) &&
          hasPublicVisibility(fn.getLinkage())) {
        auto fnName = Demangle::demangleSymbolAsString(
            fn.getName(),
            Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
        getModule()->getASTContext().Diags.diagnose(
            fn.getLocation().getSourceLoc(),
            diag::must_specialize_function_not_public, {fnName});
        failed = true;
      }
    }

    return failed;
  }

  // Delete all "must_specialize" functions. They can no longer be used.
  bool deleteDeadFunctions() {
    bool deletedFunctions = false;
    SmallVector<SILFunction *, 64> functions;
    for (auto &fn : getModule()->getFunctions())
      functions.push_back(&fn);
    for (SILFunction *fn : functions) {
      if (fn->hasSemanticsAttr(semantics::MUST_SPECIALIZE)) {
        deletedFunctions = true;
        notifyWillDeleteFunction(fn);
        getModule()->eraseFunction(fn);
      }
    }
    return deletedFunctions;
  }

public:
  MandatorySpecializationPass() = default;

  /// The entry point to the transformation.
  void run() override {
    SILOptFunctionBuilder localFunctionBuilder(*this);
    functionBuilder = &localFunctionBuilder;

    for (SILFunction &fn : getModule()->getFunctions()) {
      buildCallGraph(fn);
    }

    SmallPtrSet<SILFunction *, 32> correctedFunctions;
    for (SILFunction &fn : getModule()->getFunctions()) {
      correctCallerFnAttributes(fn, correctedFunctions);
    }

    for (SILFunction &fn : getModule()->getFunctions()) {
      for (auto &bb : fn) {
        for (auto &inst : bb) {
          if (!FullApplySite::isa(&inst))
            continue;

          FullApplySite apply(&inst);
          SILFunction *callee = apply.getCalleeFunction();
          if (callee->hasSemanticsAttr(semantics::MUST_SPECIALIZE))
            mustSpecializeApplySites.insert(apply.getInstruction());
        }
      }
    }

    for (SILFunction &fn : getModule()->getFunctions()) {
      // Only try to specialize the calls in top-level functions.
      if (!fn.hasSemanticsAttr(semantics::MUST_SPECIALIZE))
        recursivelySpecializeCalls(fn);
    }

    // If this returns true, it means we're going to fail to compile. So just
    // exit.
    if (verifySpecialized())
      return;

    // If we don't delete any functions it means that there are no function
    // marked as "must_specialize".
    if (deleteDeadFunctions()) {
      invalidateAll();
      invalidateFunctionTables();
      getModule()->invalidateSILLoaderCaches();
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createMandatorySpecialization() {
  return new MandatorySpecializationPass();
}
