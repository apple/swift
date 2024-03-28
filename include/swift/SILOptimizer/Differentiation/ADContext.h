//===--- ADContext.h - Differentiation Context ----------------*- C++ -*---===//
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
// Per-module contextual information for the differentiation transform.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADCONTEXT_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADCONTEXT_H

#include "swift/SIL/ApplySite.h"
#include "swift/SILOptimizer/Differentiation/Common.h"
#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"

#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class ASTContext;
class DifferentiableFunctionExpr;
class DifferentiableFunctionInst;
class FuncDecl;
class SILDifferentiabilityWitness;
class SILFunction;
class SILModuleTransform;
class SILModule;
class SILPassManager;

namespace autodiff {

/// Stores `apply` instruction information calculated by VJP generation.
struct NestedApplyInfo {
  /// The differentiation config that is used to differentiate this `apply`
  /// instruction.
  AutoDiffConfig config;
  /// The original pullback type before reabstraction. `None` if the pullback
  /// type is not reabstracted.
  std::optional<CanSILFunctionType> originalPullbackType;
  /// Index of `apply` pullback in nested pullback call
  unsigned pullbackIdx = -1U;
  /// Pullback value itself that is memoized in some cases (e.g. pullback is
  /// called by `begin_apply`, but should be destroyed after `end_apply`).
  SILValue pullback = SILValue();
  SILValue beginApplyToken = SILValue();
};

/// Per-module contextual information for the Differentiation pass.
class ADContext {
private:
  /// Reference to the main transform.
  SILModuleTransform &transform;

  /// The module where Differentiation is performed on.
  SILModule &module;

  /// AST context.
  ASTContext &astCtx = module.getASTContext();

  /// Shared pass manager.
  SILPassManager &passManager;

  /// The worklist (stack) of `differentiable_function` instructions to be
  /// processed.
  llvm::SmallVector<DifferentiableFunctionInst *, 32>
      differentiableFunctionInsts;

  /// The worklist (stack) of `linear_function` instructions to be processed.
  llvm::SmallVector<LinearFunctionInst *, 32> linearFunctionInsts;

  /// The set of `differentiable_function` instructions that have been
  /// processed. Used to avoid reprocessing invalidated instructions.
  /// NOTE(TF-784): if we use `CanonicalizeInstruction` subclass to replace
  /// `ADContext::processDifferentiableFunctionInst`, this field may be removed.
  llvm::SmallPtrSet<DifferentiableFunctionInst *, 32>
      processedDifferentiableFunctionInsts;

  /// The set of `linear_function` instructions that have been processed. Used
  /// to avoid reprocessing invalidated instructions.
  /// NOTE(TF-784): if we use `CanonicalizeInstruction` subclass to replace
  /// `ADContext::processLinearFunctionInst`, this field may be removed.
  llvm::SmallPtrSet<LinearFunctionInst *, 32> processedLinearFunctionInsts;

  /// Mapping from witnesses to invokers.
  /// `SmallMapVector` is used for deterministic insertion order iteration.
  llvm::SmallMapVector<SILDifferentiabilityWitness *, DifferentiationInvoker,
                       32>
      invokers;

  /// Mapping from original `apply` instructions to their corresponding
  /// `NestedApplyInfo`s.
  llvm::DenseMap<FullApplySite, NestedApplyInfo> nestedApplyInfo;

  /// List of generated functions (JVPs, VJPs, pullbacks, and thunks).
  /// Saved for deletion during cleanup.
  llvm::SmallVector<SILFunction *, 32> generatedFunctions;

  /// List of references to generated functions.
  /// Saved for deletion during cleanup.
  llvm::SmallVector<SILValue, 32> generatedFunctionReferences;

  /// The AdditiveArithmetic protocol in the standard library.
  ProtocolDecl *additiveArithmeticProtocol =
      astCtx.getProtocol(KnownProtocolKind::AdditiveArithmetic);

  /// `AdditiveArithmetic.+` declaration.
  mutable FuncDecl *cachedPlusFn = nullptr;
  /// `AdditiveArithmetic.+=` declaration.
  mutable FuncDecl *cachedPlusEqualFn = nullptr;
  /// `AdditiveArithmetic.zero` declaration.
  mutable AccessorDecl *cachedZeroGetter = nullptr;

public:
  /// Construct an ADContext for the given module.
  explicit ADContext(SILModuleTransform &transform);

  // No copying.
  ADContext(const ADContext &) = delete;

  //--------------------------------------------------------------------------//
  // General utilities
  //--------------------------------------------------------------------------//

  SILModuleTransform &getTransform() const { return transform; }
  SILModule &getModule() const { return module; }
  ASTContext &getASTContext() const { return module.getASTContext(); }
  SILPassManager &getPassManager() const { return passManager; }
  Lowering::TypeConverter &getTypeConverter() { return module.Types; }

  llvm::SmallVectorImpl<DifferentiableFunctionInst *> &
  getDifferentiableFunctionInstWorklist() {
    return differentiableFunctionInsts;
  }

  llvm::SmallVectorImpl<LinearFunctionInst *> &getLinearFunctionInstWorklist() {
    return linearFunctionInsts;
  }

  /// Get or create the synthesized file for the given `SILFunction`.
  /// Used by `LinearMapInfo` for adding generated linear map struct and
  /// branching trace enum declarations.
  SynthesizedFileUnit &getOrCreateSynthesizedFile(SILFunction *original);

  /// Returns true if the given `differentiable_function` instruction has
  /// already been processed.
  bool
  isDifferentiableFunctionInstProcessed(DifferentiableFunctionInst *dfi) const {
    return processedDifferentiableFunctionInsts.count(dfi);
  }

  /// Adds the given `differentiable_function` instruction to the worklist.
  void
  markDifferentiableFunctionInstAsProcessed(DifferentiableFunctionInst *dfi) {
    processedDifferentiableFunctionInsts.insert(dfi);
  }

  /// Returns true if the given `linear_function` instruction has already been
  /// processed.
  bool isLinearFunctionInstProcessed(LinearFunctionInst *lfi) const {
    return processedLinearFunctionInsts.count(lfi);
  }

  /// Adds the given `linear_function` instruction to the worklist.
  void markLinearFunctionInstAsProcessed(LinearFunctionInst *lfi) {
    processedLinearFunctionInsts.insert(lfi);
  }

  const llvm::SmallMapVector<SILDifferentiabilityWitness *,
                             DifferentiationInvoker, 32> &
  getInvokers() const {
    return invokers;
  }

  void addInvoker(SILDifferentiabilityWitness *witness) {
    assert(!invokers.count(witness) &&
           "Differentiability witness already has an invoker");
    invokers.insert({witness, DifferentiationInvoker(witness)});
  }

  llvm::DenseMap<FullApplySite, NestedApplyInfo> &getNestedApplyInfo() {
    return nestedApplyInfo;
  }

  void recordGeneratedFunction(SILFunction *function) {
    generatedFunctions.push_back(function);
  }

  void recordGeneratedFunctionReference(SILValue functionRef) {
    generatedFunctionReferences.push_back(functionRef);
  }

  ProtocolDecl *getAdditiveArithmeticProtocol() const {
    return additiveArithmeticProtocol;
  }

  FuncDecl *getPlusDecl() const;
  FuncDecl *getPlusEqualDecl() const;
  AccessorDecl *getAdditiveArithmeticZeroGetter() const;

  /// Cleans up all the internal state.
  void cleanUp();

  /// Creates an `differentiable_function` instruction using the given builder
  /// and arguments. Erase the newly created instruction from the processed set,
  /// if it exists - it may exist in the processed set if it has the same
  /// pointer value as a previously processed and deleted instruction.
  /// TODO(TF-784): The pointer reuse is a real concern and the use of
  /// `CanonicalizeInstruction` may get rid of the need for this workaround.
  DifferentiableFunctionInst *createDifferentiableFunction(
      SILBuilder &builder, SILLocation loc, IndexSubset *parameterIndices,
      IndexSubset *resultIndices, SILValue original,
      std::optional<std::pair<SILValue, SILValue>> derivativeFunctions =
          std::nullopt);

  /// Creates a `linear_function` instruction using the given builder
  /// and arguments. Erase the newly created instruction from the processed set,
  /// if it exists - it may exist in the processed set if it has the same
  /// pointer value as a previously processed and deleted instruction.
  LinearFunctionInst *createLinearFunction(
      SILBuilder &builder, SILLocation loc, IndexSubset *parameterIndices,
      SILValue original,
      std::optional<SILValue> transposeFunction = std::nullopt);

  // Given a `differentiable_function` instruction, finds the corresponding
  // differential operator used in the AST. If no differential operator is
  // found, return nullptr.
  DifferentiableFunctionExpr *
  findDifferentialOperator(DifferentiableFunctionInst *inst);

  // Given a `linear_function` instruction, finds the corresponding differential
  // operator used in the AST. If no differential operator is found, return
  // nullptr.
  LinearFunctionExpr *findDifferentialOperator(LinearFunctionInst *inst);

  template <typename... T, typename... U>
  InFlightDiagnostic diagnose(SourceLoc loc, Diag<T...> diag,
                              U &&... args) const {
    return getASTContext().Diags.diagnose(loc, diag, std::forward<U>(args)...);
  }

  /// Given an instruction and a differentiation task associated with the
  /// parent function, emits a "not differentiable" error based on the task. If
  /// the task is indirect, emits notes all the way up to the outermost task,
  /// and emits an error at the outer task. Otherwise, emits an error directly.
  template <typename... T, typename... U>
  InFlightDiagnostic
  emitNondifferentiabilityError(SILInstruction *inst,
                                DifferentiationInvoker invoker, Diag<T...> diag,
                                U &&... args);

  /// Given a value and a differentiation task associated with the parent
  /// function, emits a "not differentiable" error based on the task. If the
  /// task is indirect, emits notes all the way up to the outermost task, and
  /// emits an error at the outer task. Otherwise, emits an error directly.
  template <typename... T, typename... U>
  InFlightDiagnostic
  emitNondifferentiabilityError(SILValue value, DifferentiationInvoker invoker,
                                Diag<T...> diag, U &&... args);

  /// Emit a "not differentiable" error based on the given differentiation task
  /// and diagnostic.
  template <typename... T, typename... U>
  InFlightDiagnostic
  emitNondifferentiabilityError(SourceLoc loc, DifferentiationInvoker invoker,
                                Diag<T...> diag, U &&... args);
};

raw_ostream &getADDebugStream();
SILLocation getValidLocation(SILValue v);
SILLocation getValidLocation(SILInstruction *inst);

template <typename... T, typename... U>
InFlightDiagnostic
ADContext::emitNondifferentiabilityError(SILValue value,
                                         DifferentiationInvoker invoker,
                                         Diag<T...> diag, U &&... args) {
  LLVM_DEBUG({
    getADDebugStream() << "Diagnosing non-differentiability.\n";
    auto &s = getADDebugStream() << "For value:\n";
    value->printInContext(s);
    getADDebugStream() << "With invoker:\n" << invoker << '\n';
  });
  // If instruction does not have a valid location, use the function location
  // as a fallback. Improves diagnostics in some cases.
  auto valueLoc = getValidLocation(value).getSourceLoc();
  return emitNondifferentiabilityError(valueLoc, invoker, diag,
                                       std::forward<U>(args)...);
}

template <typename... T, typename... U>
InFlightDiagnostic
ADContext::emitNondifferentiabilityError(SILInstruction *inst,
                                         DifferentiationInvoker invoker,
                                         Diag<T...> diag, U &&... args) {
  LLVM_DEBUG({
    getADDebugStream() << "Diagnosing non-differentiability.\n";
    auto &s = getADDebugStream() << "For instruction:\n";
    inst->printInContext(s);
    getADDebugStream() << "With invoker:\n" << invoker << '\n';
  });
  // If instruction does not have a valid location, use the function location
  // as a fallback. Improves diagnostics for `ref_element_addr` generated in
  // synthesized stored property getters.
  auto instLoc = getValidLocation(inst).getSourceLoc();
  return emitNondifferentiabilityError(instLoc, invoker, diag,
                                       std::forward<U>(args)...);
}

template <typename... T, typename... U>
InFlightDiagnostic
ADContext::emitNondifferentiabilityError(SourceLoc loc,
                                         DifferentiationInvoker invoker,
                                         Diag<T...> diag, U &&... args) {
  switch (invoker.getKind()) {
  // For `differentiable_function` instructions: if the
  // `differentiable_function` instruction comes from a differential operator,
  // emit an error on the expression and a note on the non-differentiable
  // operation. Otherwise, emit both an error and note on the
  // non-differentiation operation.
  case DifferentiationInvoker::Kind::DifferentiableFunctionInst: {
    auto *inst = invoker.getDifferentiableFunctionInst();
    if (auto *expr = findDifferentialOperator(inst)) {
      diagnose(expr->getLoc(), diag::autodiff_function_not_differentiable_error)
          .highlight(expr->getSubExpr()->getSourceRange());
      return diagnose(loc, diag, std::forward<U>(args)...);
    }
    diagnose(loc, diag::autodiff_expression_not_differentiable_error);
    return diagnose(loc, diag, std::forward<U>(args)...);
  }

  // For `linear_function` instructions: if the `linear_function` instruction
  // comes from a differential operator, emit an error on the expression and a
  // note on the non-differentiable operation. Otherwise, emit both an error and
  // note on the non-differentiation operation.
  case DifferentiationInvoker::Kind::LinearFunctionInst: {
    auto *inst = invoker.getLinearFunctionInst();
    if (auto *expr = findDifferentialOperator(inst)) {
      diagnose(expr->getLoc(), diag::autodiff_function_not_differentiable_error)
          .highlight(expr->getSubExpr()->getSourceRange());
      return diagnose(loc, diag, std::forward<U>(args)...);
    }
    diagnose(loc, diag::autodiff_expression_not_differentiable_error);
    return diagnose(loc, diag, std::forward<U>(args)...);
  }

  // For differentiability witnesses: try to find a `@differentiable` or
  // `@derivative` attribute. If an attribute is found, emit an error on it;
  // otherwise, emit an error on the original function.
  case DifferentiationInvoker::Kind::SILDifferentiabilityWitnessInvoker: {
    auto *witness = invoker.getSILDifferentiabilityWitnessInvoker();
    auto *original = witness->getOriginalFunction();
    // If the witness has an associated attribute, emit an error at its
    // location.
    if (auto *attr = witness->getAttribute()) {
      diagnose(attr->getLocation(),
               diag::autodiff_function_not_differentiable_error)
          .highlight(attr->getRangeWithAt());
      // Emit informative note.
      bool emittedNote = false;
      // If the witness comes from an implicit `@differentiable` attribute
      // inherited from a protocol requirement's `@differentiable` attribute,
      // emit a note on the inherited attribute.
      if (auto *diffAttr = dyn_cast<DifferentiableAttr>(attr)) {
        auto inheritedAttrLoc =
            diffAttr->getImplicitlyInheritedDifferentiableAttrLocation();
        if (inheritedAttrLoc.isValid()) {
          diagnose(inheritedAttrLoc,
                   diag::autodiff_implicitly_inherited_differentiable_attr_here)
              .highlight(inheritedAttrLoc);
          emittedNote = true;
        }
      }
      // Otherwise, emit a note on the original function.
      if (!emittedNote) {
        diagnose(original->getLocation().getSourceLoc(),
                 diag::autodiff_when_differentiating_function_definition);
      }
    }
    // Otherwise, emit an error on the original function.
    else {
      diagnose(original->getLocation().getSourceLoc(),
               diag::autodiff_function_not_differentiable_error);
    }
    return diagnose(loc, diag, std::forward<U>(args)...);
  }

  // For indirect differentiation, emit a "not differentiable" note on the
  // expression first. Then emit an error at the source invoker of
  // differentiation, and a "when differentiating this" note at each indirect
  // invoker.
  case DifferentiationInvoker::Kind::IndirectDifferentiation: {
    SILInstruction *inst;
    SILDifferentiabilityWitness *witness;
    std::tie(inst, witness) = invoker.getIndirectDifferentiation();
    auto invokerLookup = invokers.find(witness);
    assert(invokerLookup != invokers.end() && "Expected parent invoker");
    emitNondifferentiabilityError(
        inst, invokerLookup->second,
        diag::autodiff_expression_not_differentiable_note);
    return diagnose(loc, diag::autodiff_when_differentiating_function_call);
  }
  }
  llvm_unreachable("Invalid invoker kind"); // silences MSVC C4715
}

} // end namespace autodiff
} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_ADCONTEXT_H
