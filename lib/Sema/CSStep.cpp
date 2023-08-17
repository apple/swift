//===--- CSStep.cpp - Constraint Solver Steps -----------------------------===//
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
//
// This file implements the \c SolverStep class and its related types,
// which is used by constraint solver to do iterative solving.
//
//===----------------------------------------------------------------------===//

#include "CSStep.h"
#include "TypeChecker.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace swift;
using namespace constraints;

ComponentStep::Scope::Scope(ComponentStep &component)
    : CS(component.CS), Component(component) {
  TypeVars = std::move(CS.TypeVariables);

  for (auto *typeVar : component.TypeVars)
    CS.addTypeVariable(typeVar);

  auto &workList = CS.InactiveConstraints;
  workList.splice(workList.end(), *component.Constraints);

  SolverScope = new ConstraintSystem::SolverScope(CS);
  PrevPartialScope = CS.solverState->PartialSolutionScope;
  CS.solverState->PartialSolutionScope = SolverScope;
}

StepResult SplitterStep::take(bool prevFailed) {
  // "split" is considered a failure if previous step failed,
  // or there is a failure recorded by constraint system, or
  // system can't be simplified.
  if (prevFailed || CS.failedConstraint || CS.simplify())
    return done(/*isSuccess=*/false);

  SmallVector<std::unique_ptr<SolverStep>, 4> followup;
  // Try to run "connected components" algorithm and split
  // type variables and their constraints into independent
  // sub-systems to solve.
  computeFollowupSteps(followup);

  // If there is only one step, there is no reason to
  // try to merge solutions, "split" step should be considered
  // done and replaced by a single component step.
  if (followup.size() < 2)
    return replaceWith(std::move(followup.front()));

  /// Wait until all of the steps are done.
  return suspend(followup);
}

StepResult SplitterStep::resume(bool prevFailed) {
  // Restore the state of the constraint system to before split.
  CS.CG.setOrphanedConstraints(std::move(OrphanedConstraints));
  auto &workList = CS.InactiveConstraints;
  for (auto &component : Components)
    workList.splice(workList.end(), component);

  // If we came back to this step and previous (one of the components)
  // failed, it means that we can't solve this step either.
  if (prevFailed)
    return done(/*isSuccess=*/false);

  // Otherwise let's try to merge partial solutions together
  // and form a complete solution(s) for this split.
  return done(mergePartialSolutions());
}

void SplitterStep::computeFollowupSteps(
    SmallVectorImpl<std::unique_ptr<SolverStep>> &steps) {
  // Compute next steps based on that connected components
  // algorithm tells us is splittable.

  auto &CG = CS.getConstraintGraph();
  // Contract the edges of the constraint graph.
  CG.optimize();

  // Compute the connected components of the constraint graph.
  auto components = CG.computeConnectedComponents(CS.getTypeVariables());
  unsigned numComponents = components.size();
  if (numComponents < 2) {
    steps.push_back(std::make_unique<ComponentStep>(
        CS, 0, &CS.InactiveConstraints, Solutions));
    return;
  }

  if (CS.isDebugMode()) {
    auto &log = getDebugLogger();
    auto indent = CS.solverState->getCurrentIndent();
    // Verify that the constraint graph is valid.
    CG.verify();

    log.indent(indent) << "---Constraint graph---\n";
    CG.print(CS.getTypeVariables(), log);

    log.indent(indent) << "---Connected components---\n";
    CG.printConnectedComponents(CS.getTypeVariables(), log);
  }

  // Take the orphaned constraints, because they'll go into a component now.
  OrphanedConstraints = CG.takeOrphanedConstraints();

  IncludeInMergedResults.resize(numComponents, true);
  Components.resize(numComponents);
  PartialSolutions = std::unique_ptr<SmallVector<Solution, 4>[]>(
      new SmallVector<Solution, 4>[numComponents]);

  // Add components.
  for (unsigned i : indices(components)) {
    unsigned solutionIndex = components[i].solutionIndex;

    // If there are no dependencies, build a normal component step.
    if (components[i].getDependencies().empty()) {
      steps.push_back(std::make_unique<ComponentStep>(
          CS, solutionIndex, &Components[i], std::move(components[i]),
          PartialSolutions[solutionIndex]));
      continue;
    }

    // Note that the partial results from any dependencies of this component
    // need not be included in the final merged results, because they'll
    // already be part of the partial results for this component.
    for (auto dependsOn : components[i].getDependencies()) {
      IncludeInMergedResults[dependsOn] = false;
    }

    // Otherwise, build a dependent component "splitter" step, which
    // handles all combinations of incoming partial solutions.
    steps.push_back(std::make_unique<DependentComponentSplitterStep>(
        CS, &Components[i], solutionIndex, std::move(components[i]),
        llvm::makeMutableArrayRef(PartialSolutions.get(), numComponents)));
  }

  assert(CS.InactiveConstraints.empty() && "Missed a constraint");
}

namespace {
  /// Retrieve the size of a container.
  template<typename Container>
  unsigned getSize(const Container &container) {
    return container.size();
  }

  /// Retrieve the size of a container referenced by a pointer.
  template<typename Container>
  unsigned getSize(const Container *container) {
    return container->size();
  }

  /// Identity getSize() for cases where we are working with a count.
  unsigned getSize(unsigned size) {
    return size;
  }

  /// Compute the next combination of indices into the given array of
  /// containers.
  /// \param containers Containers (each of which must have a `size()`) in
  /// which the indices will be generated.
  /// \param indices The current indices into the containers, which will
  /// be updated to represent the next combination.
  /// \returns true to indicate that \c indices contains the next combination,
  /// or \c false to indicate that there are no combinations left.
  template<typename Container>
  bool nextCombination(ArrayRef<Container> containers,
                       MutableArrayRef<unsigned> indices) {
    assert(containers.size() == indices.size() &&
           "Indices should have been initialized to the same size with 0s");
    unsigned numIndices = containers.size();
    for (unsigned n = numIndices; n > 0; --n) {
      ++indices[n - 1];

      // If we haven't run out of solutions yet, we're done.
      if (indices[n - 1] < getSize(containers[n - 1]))
        break;

      // If we ran out of solutions at the first position, we're done.
      if (n == 1) {
        return false;
      }

      // Zero out the indices from here to the end.
      for (unsigned i = n - 1; i != numIndices; ++i)
        indices[i] = 0;
    }

    return true;
  }
}

bool SplitterStep::mergePartialSolutions() const {
  assert(Components.size() >= 2);

  // Compute the # of partial solutions that will be merged for each
  // component. Components that shouldn't be included will get a count of 1,
  // an we'll skip them later.
  auto numComponents = Components.size();
  SmallVector<unsigned, 2> countsVec;
  countsVec.reserve(numComponents);
  for (unsigned idx : range(numComponents)) {
    countsVec.push_back(
        IncludeInMergedResults[idx] ? PartialSolutions[idx].size() : 1);
  }

  // Produce all combinations of partial solutions.
  ArrayRef<unsigned> counts = countsVec;
  SmallVector<unsigned, 2> indices(numComponents, 0);
  bool anySolutions = false;
  size_t solutionMemory = 0;
  do {
    // Create a new solver scope in which we apply all of the relevant partial
    // solutions.
    ConstraintSystem::SolverScope scope(CS);
    for (unsigned i : range(numComponents)) {
      if (!IncludeInMergedResults[i])
        continue;

      CS.applySolution(PartialSolutions[i][indices[i]]);
    }

    // This solution might be worse than the best solution found so far.
    // If so, skip it.
    if (!CS.worseThanBestSolution()) {
      // Finalize this solution.
      auto solution = CS.finalize();
      solutionMemory += solution.getTotalMemory();
      if (CS.isDebugMode()) {
        auto &log = getDebugLogger();
        log << "(composed solution:";
        CS.CurrentScore.print(log);
        log << ")\n";
      }

      // Save this solution.
      Solutions.push_back(std::move(solution));
      anySolutions = true;
    }

    // Since merging partial solutions can go exponential, make sure we didn't
    // pass the "too complex" thresholds including allocated memory and time.
    if (CS.isTooComplex(solutionMemory))
      return false;

  } while (nextCombination(counts, indices));

  return anySolutions;
}

StepResult DependentComponentSplitterStep::take(bool prevFailed) {
  // "split" is considered a failure if previous step failed,
  // or there is a failure recorded by constraint system, or
  // system can't be simplified.
  if (prevFailed || CS.getFailedConstraint() || CS.simplify())
    return done(/*isSuccess=*/false);

  // Figure out the sets of partial solutions that this component depends on.
  SmallVector<const SmallVector<Solution, 4> *, 2> dependsOnSets;
  for (auto index : Component.getDependencies()) {
    dependsOnSets.push_back(&AllPartialSolutions[index]);
  }

  // Produce all combinations of partial solutions for the inputs.
  SmallVector<std::unique_ptr<SolverStep>, 4> followup;
  SmallVector<unsigned, 2> indices(Component.getDependencies().size(), 0);
  auto dependsOnSetsRef = llvm::makeArrayRef(dependsOnSets);
  do {
    // Form the set of input partial solutions.
    SmallVector<const Solution *, 2> dependsOnSolutions;
    for (auto index : swift::indices(indices)) {
      dependsOnSolutions.push_back(&(*dependsOnSets[index])[indices[index]]);
    }
    ContextualSolutions.push_back(std::make_unique<SmallVector<Solution, 2>>());

    followup.push_back(std::make_unique<ComponentStep>(
        CS, Index, Constraints, Component, std::move(dependsOnSolutions),
        *ContextualSolutions.back()));
  } while (nextCombination(dependsOnSetsRef, indices));

  /// Wait until all of the component steps are done.
  return suspend(followup);
}

StepResult DependentComponentSplitterStep::resume(bool prevFailed) {
  for (auto &ComponentStepSolutions : ContextualSolutions) {
    Solutions.append(std::make_move_iterator(ComponentStepSolutions->begin()),
                     std::make_move_iterator(ComponentStepSolutions->end()));
  }
  return done(/*isSuccess=*/!Solutions.empty());
}

void DependentComponentSplitterStep::print(llvm::raw_ostream &Out) {
  Out << "DependentComponentSplitterStep for dependencies on [";
  interleave(
      Component.getDependencies(), [&](unsigned index) { Out << index; },
      [&] { Out << ", "; });
  Out << "]\n";
}

StepResult ComponentStep::take(bool prevFailed) {
  // One of the previous components created by "split"
  // failed, it means that we can't solve this component.
  if ((prevFailed && DependsOnPartialSolutions.empty()) ||
      CS.isTooComplex(Solutions))
    return done(/*isSuccess=*/false);

  // Setup active scope, only if previous component didn't fail.
  setupScope();

  // If there are any dependent partial solutions to compose, do so now.
  if (!DependsOnPartialSolutions.empty()) {
    for (auto partial : DependsOnPartialSolutions) {
      CS.applySolution(*partial);
    }

    // Activate all of the one-way constraints.
    SmallVector<Constraint *, 4> oneWayConstraints;
    for (auto &constraint : CS.InactiveConstraints) {
      if (constraint.isOneWayConstraint())
        oneWayConstraints.push_back(&constraint);
    }
    for (auto constraint : oneWayConstraints) {
      CS.activateConstraint(constraint);
    }

    // Simplify again.
    if (CS.failedConstraint || CS.simplify())
      return done(/*isSuccess=*/false);
  }

  /// Try to figure out what this step is going to be,
  /// after the scope has been established.
  SmallString<64> potentialBindings;
  llvm::raw_svector_ostream bos(potentialBindings);

  auto bestBindings = CS.determineBestBindings([&](const BindingSet &bindings) {
    if (CS.isDebugMode() && bindings.hasViableBindings()) {
      bos.indent(CS.solverState->getCurrentIndent() + 2);
      bos << "(";
      bindings.dump(bos, CS.solverState->getCurrentIndent() + 2);
      bos << ")\n";
    }
  });

  auto *disjunction = CS.selectDisjunction();
  auto *conjunction = CS.selectConjunction();

  if (CS.isDebugMode()) {
    SmallVector<Constraint *, 4> disjunctions;
    CS.collectDisjunctions(disjunctions);
    std::vector<std::string> overloadDisjunctions;
    for (const auto &disjunction : disjunctions) {
      PrintOptions PO;
      PO.PrintTypesForDebugging = true;

      auto constraints = disjunction->getNestedConstraints();
      if (constraints[0]->getKind() == ConstraintKind::BindOverload)
        overloadDisjunctions.push_back(
            constraints[0]->getFirstType()->getString(PO));
    }

    if (!potentialBindings.empty() || !overloadDisjunctions.empty()) {
      auto &log = getDebugLogger();
      log << "(Potential Binding(s): " << '\n';
      log << potentialBindings;
    }

    if (!overloadDisjunctions.empty()) {
      auto &log = getDebugLogger();
      log.indent(CS.solverState->getCurrentIndent() + 2);
      log << "Disjunction(s) = [";
      interleave(overloadDisjunctions, log, ", ");
      log << "]\n";
    }

    if (!potentialBindings.empty() || !overloadDisjunctions.empty()) {
      auto &log = getDebugLogger();
      log << ")\n";
    }
  }

  enum class StepKind { Binding, Disjunction, Conjunction };

  auto chooseStep = [&]() -> Optional<StepKind> {
    // Bindings usually happen first, but sometimes we want to prioritize a
    // disjunction or conjunction.
    if (bestBindings) {
      if (disjunction && !bestBindings->favoredOverDisjunction(disjunction))
        return StepKind::Disjunction;

      if (conjunction && !bestBindings->favoredOverConjunction(conjunction))
        return StepKind::Conjunction;

      return StepKind::Binding;
    }
    if (disjunction)
      return StepKind::Disjunction;

    if (conjunction)
      return StepKind::Conjunction;

    return None;
  };

  if (auto step = chooseStep()) {
    switch (*step) {
    case StepKind::Binding:
      return suspend(
          std::make_unique<TypeVariableStep>(*bestBindings, Solutions));
    case StepKind::Disjunction:
      return suspend(
          std::make_unique<DisjunctionStep>(CS, disjunction, Solutions));
    case StepKind::Conjunction:
      return suspend(
          std::make_unique<ConjunctionStep>(CS, conjunction, Solutions));
    }
    llvm_unreachable("Unhandled case in switch!");
  }

  if (!CS.solverState->allowsFreeTypeVariables() && CS.hasFreeTypeVariables()) {
    // If there are no disjunctions or type variables to bind
    // we can't solve this system unless we have free type variables
    // allowed in the solution.
    return finalize(/*isSuccess=*/false);
  }

  auto printConstraints = [&](const ConstraintList &constraints) {
    for (auto &constraint : constraints)
      constraint.print(
          getDebugLogger().indent(CS.solverState->getCurrentIndent()),
          &CS.getASTContext().SourceMgr, CS.solverState->getCurrentIndent());
  };

  // If we don't have any disjunction or type variable choices left, we're done
  // solving. Make sure we don't have any unsolved constraints left over, using
  // report_fatal_error to make sure we trap in debug builds and fail the step
  // in release builds.
  if (!CS.ActiveConstraints.empty()) {
    if (CS.isDebugMode()) {
      getDebugLogger() << "(failed due to remaining active constraints:\n";
      printConstraints(CS.ActiveConstraints);
      getDebugLogger() << ")\n";
    }

    CS.InvalidState = true;
    return finalize(/*isSuccess=*/false);
  }

  if (!CS.solverState->allowsFreeTypeVariables()) {
    if (!CS.InactiveConstraints.empty()) {
      if (CS.isDebugMode()) {
        getDebugLogger() << "(failed due to remaining inactive constraints:\n";
        printConstraints(CS.InactiveConstraints);
        getDebugLogger() << ")\n";
      }

      CS.InvalidState = true;
      return finalize(/*isSuccess=*/false);
    }
  }

  // If this solution is worse than the best solution we've seen so far,
  // skip it.
  if (CS.worseThanBestSolution())
    return finalize(/*isSuccess=*/false);

  // If we only have relational or member constraints and are allowing
  // free type variables, save the solution.
  for (auto &constraint : CS.InactiveConstraints) {
    switch (constraint.getClassification()) {
    case ConstraintClassification::Relational:
    case ConstraintClassification::Member:
      continue;
    default:
      return finalize(/*isSuccess=*/false);
    }
  }

  auto solution = CS.finalize();
  if (CS.isDebugMode()) {
    auto &log = getDebugLogger();
    log << "(found solution:";
    getCurrentScore().print(log);
    log << ")\n";
  }

  Solutions.push_back(std::move(solution));
  return finalize(/*isSuccess=*/true);
}

StepResult ComponentStep::finalize(bool isSuccess) {
  // If this was a single component, there is nothing to be done,
  // because it represents the whole constraint system at some
  // point of the solver path.
  if (IsSingle)
    return done(isSuccess);

  // Rewind all modifications done to constraint system.
  ComponentScope.reset();

  if (CS.isDebugMode()) {
    auto &log = getDebugLogger();
    log << (isSuccess ? "finished" : "failed") << " component #" << Index
        << ")\n";
  }

  // If we came either back to this step and previous
  // (either disjunction or type var) failed, it means
  // that component as a whole has failed.
  if (!isSuccess)
    return done(/*isSuccess=*/false);

  assert(!Solutions.empty() && "No Solutions?");

  // For each of the partial solutions, subtract off the current score.
  // It doesn't contribute.
  for (auto &solution : Solutions)
    solution.getFixedScore() -= OriginalScore;

  // Restore the original best score.
  CS.solverState->BestScore = OriginalBestScore;

  // When there are multiple partial solutions for a given connected component,
  // rank those solutions to pick the best ones. This limits the number of
  // combinations we need to produce; in the common case, down to a single
  // combination.
  filterSolutions(Solutions, /*minimize=*/true);
  return done(/*isSuccess=*/true);
}

void TypeVariableStep::setup() {
  ++CS.solverState->NumTypeVariablesBound;
}

bool TypeVariableStep::attempt(const TypeVariableBinding &choice) {
  ++CS.solverState->NumTypeVariableBindings;

  if (choice.hasDefaultedProtocol())
    SawFirstLiteralConstraint = true;

  // Try to solve the system with typeVar := type
  return choice.attempt(CS);
}

StepResult TypeVariableStep::resume(bool prevFailed) {
  assert(ActiveChoice);

  // If there was no failure in the sub-path it means
  // that active binding has a solution.
  AnySolved |= !prevFailed;

  bool shouldStop = shouldStopAfter(ActiveChoice->second);
  // Rewind back all of the changes made to constraint system.
  ActiveChoice.reset();

  if (CS.isDebugMode())
    getDebugLogger() << ")\n";

  // Let's check if we should stop right before
  // attempting any new bindings.
  if (shouldStop)
    return done(/*isSuccess=*/AnySolved);

  // Attempt next type variable binding.
  return take(prevFailed);
}

StepResult DisjunctionStep::resume(bool prevFailed) {
  // If disjunction step is re-taken and there should be
  // active choice, let's see if it has be solved or not.
  assert(ActiveChoice);

  // If choice (sub-path) has failed, it's okay, other
  // choices have to be attempted regardless, since final
  // decision could be made only after attempting all
  // of the choices, so let's just ignore failed ones.
  if (!prevFailed) {
    auto &choice = ActiveChoice->second;
    auto score = getBestScore(Solutions);

    if (!choice.isGenericOperator() && choice.isSymmetricOperator()) {
      if (!BestNonGenericScore || score < BestNonGenericScore) {
        BestNonGenericScore = score;
        if (shouldSkipGenericOperators()) {
          // The disjunction choice producer shouldn't do the work
          // to partition the generic operator choices if generic
          // operators are going to be skipped.
          Producer.setNeedsGenericOperatorOrdering(false);
        }
      }
    }

    AnySolved = true;
    // Remember the last successfully solved choice,
    // it would be useful when disjunction is exhausted.
    LastSolvedChoice = {choice, *score};
  }

  // Rewind back the constraint system information.
  ActiveChoice.reset();

  if (CS.isDebugMode())
    getDebugLogger() << ")\n";

  // Attempt next disjunction choice (if any left).
  return take(prevFailed);
}

bool IsDeclRefinementOfRequest::evaluate(Evaluator &evaluator,
                                         ValueDecl *declA,
                                         ValueDecl *declB) const {
  auto *typeA = declA->getInterfaceType()->getAs<GenericFunctionType>();
  auto *typeB = declB->getInterfaceType()->getAs<GenericFunctionType>();

  if (!typeA || !typeB)
    return false;

  auto genericSignatureA = typeA->getGenericSignature();
  auto genericSignatureB = typeB->getGenericSignature();

  // Substitute generic parameters with their archetypes in each generic function.
  Type substTypeA = typeA->substGenericArgs(
      genericSignatureA.getGenericEnvironment()->getForwardingSubstitutionMap());
  Type substTypeB = typeB->substGenericArgs(
      genericSignatureB.getGenericEnvironment()->getForwardingSubstitutionMap());

  // Attempt to substitute archetypes from the second type with archetypes in the
  // same structural position in the first type.
  TypeSubstitutionMap substMap;
  substTypeB = substTypeB->substituteBindingsTo(substTypeA,
      [&](ArchetypeType *origType, CanType substType,
          ArchetypeType *, ArrayRef<ProtocolConformanceRef>) -> CanType {
    auto interfaceTy =
        origType->getInterfaceType()->getCanonicalType()->getAs<SubstitutableType>();

    // Make sure any duplicate bindings are equal to the one already recorded.
    // Otherwise, the substitution has conflicting generic arguments.
    auto bound = substMap.find(interfaceTy);
    if (bound != substMap.end() && !bound->second->isEqual(substType))
      return CanType();

    substMap[interfaceTy] = substType;
    return substType;
  });

  if (!substTypeB)
    return false;

  auto result = TypeChecker::checkGenericArguments(
      declA->getDeclContext()->getParentModule(),
      genericSignatureB.getRequirements(),
      QueryTypeSubstitutionMap{ substMap });

  if (result != CheckGenericArgumentsResult::Success)
    return false;

  return substTypeA->isEqual(substTypeB);
}

bool TypeChecker::isDeclRefinementOf(ValueDecl *declA, ValueDecl *declB) {
  return evaluateOrDefault(declA->getASTContext().evaluator,
                           IsDeclRefinementOfRequest{ declA, declB },
                           false);
}

bool DisjunctionStep::shouldSkip(const DisjunctionChoice &choice) const {
  auto &ctx = CS.getASTContext();

  auto skip = [&](std::string reason) -> bool {
    if (CS.isDebugMode()) {
      auto &log = getDebugLogger();
      log << "(skipping " + reason + " ";
      choice.print(log, &ctx.SourceMgr, CS.solverState->getCurrentIndent());
      log << ")\n";
    }

    return true;
  };


  // Skip disabled overloads in the diagnostic mode if they do not have a
  // fix attached to them e.g. overloads where labels didn't match up.
  if (choice.isDisabled())
    return skip("disabled");

  // Skip unavailable overloads (unless in diagnostic mode).
  if (choice.isUnavailable() && !CS.shouldAttemptFixes())
    return skip("unavailable");

  if (ctx.TypeCheckerOpts.DisableConstraintSolverPerformanceHacks)
    return false;

  // If the solver already found a solution with a better overload choice that
  // can be unconditionally substituted by the current choice, skip the current
  // choice.
  if (LastSolvedChoice && LastSolvedChoice->second == getCurrentScore() &&
      choice.isGenericOperator()) {
    auto *declA = LastSolvedChoice->first->getOverloadChoice().getDecl();
    auto *declB = static_cast<Constraint *>(choice)->getOverloadChoice().getDecl();

    if (declA->getBaseIdentifier().isArithmeticOperator() &&
        TypeChecker::isDeclRefinementOf(declA, declB)) {
      return skip("subtype");
    }
  }

  // Don't attempt to solve for generic operators if we already have
  // a non-generic solution.

  // FIXME: Less-horrible but still horrible hack to attempt to
  //        speed things up. Skip the generic operators if we
  //        already have a solution involving non-generic operators,
  //        but continue looking for a better non-generic operator
  //        solution.
  if (BestNonGenericScore && choice.isGenericOperator()) {
    auto &score = BestNonGenericScore->Data;

    // Not all of the unary operators have `CGFloat` overloads,
    // so in order to preserve previous behavior (and overall
    // best solution) with implicit Double<->CGFloat conversion
    // we need to allow attempting generic operators for such cases.
    if (score[SK_ImplicitValueConversion] > 0 && choice.isUnaryOperator())
      return false;

    if (shouldSkipGenericOperators())
      return skip("generic");
  }

  return false;
}

bool DisjunctionStep::shouldStopAt(const DisjunctionChoice &choice) const {
  if (!LastSolvedChoice)
    return false;

  auto *lastChoice = LastSolvedChoice->first;
  auto delta = LastSolvedChoice->second - getCurrentScore();
  bool hasUnavailableOverloads = delta.Data[SK_Unavailable] > 0;
  bool hasFixes = delta.Data[SK_Fix] > 0;
  bool hasAsyncMismatch = delta.Data[SK_AsyncInSyncMismatch] > 0;
  auto isBeginningOfPartition = choice.isBeginningOfPartition();

  // Attempt to short-circuit evaluation of this disjunction only
  // if the disjunction choice we are comparing to did not involve:
  //   1. selecting unavailable overloads
  //   2. result in fixes being applied to reach a solution
  //   3. selecting an overload that results in an async/sync mismatch
  return !hasUnavailableOverloads && !hasFixes && !hasAsyncMismatch &&
         (isBeginningOfPartition ||
          shortCircuitDisjunctionAt(choice, lastChoice));
}

bool swift::isSIMDOperator(ValueDecl *value) {
  if (!value)
    return false;

  auto func = dyn_cast<FuncDecl>(value);
  if (!func)
    return false;

  if (!func->isOperator())
    return false;

  auto nominal = func->getDeclContext()->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  if (nominal->getName().empty())
    return false;

  return nominal->getName().str().startswith_insensitive("simd");
}

bool DisjunctionStep::shortCircuitDisjunctionAt(
    Constraint *currentChoice, Constraint *lastSuccessfulChoice) const {
  auto &ctx = CS.getASTContext();

  // Anything without a fix is better than anything with a fix.
  if (currentChoice->getFix() && !lastSuccessfulChoice->getFix())
    return true;

  if (ctx.TypeCheckerOpts.DisableConstraintSolverPerformanceHacks)
    return false;

  if (auto restriction = currentChoice->getRestriction()) {
    // Non-optional conversions are better than optional-to-optional
    // conversions.
    if (*restriction == ConversionRestrictionKind::OptionalToOptional)
      return true;

    // Array-to-pointer conversions are better than inout-to-pointer
    // conversions.
    if (auto successfulRestriction = lastSuccessfulChoice->getRestriction()) {
      if (*successfulRestriction == ConversionRestrictionKind::ArrayToPointer &&
          *restriction == ConversionRestrictionKind::InoutToPointer)
        return true;
    }
  }

  // Implicit conversions are better than checked casts.
  if (currentChoice->getKind() == ConstraintKind::CheckedCast)
    return true;

  return false;
}

bool DisjunctionStep::attempt(const DisjunctionChoice &choice) {
  ++CS.solverState->NumDisjunctionTerms;

  // If the disjunction requested us to, remember which choice we
  // took for it.
  if (auto *disjunctionLocator = getLocator()) {
    auto index = choice.getIndex();
    recordDisjunctionChoice(disjunctionLocator, index);

    // Implicit unwraps of optionals are worse solutions than those
    // not involving implicit unwraps.
    if (!disjunctionLocator->getPath().empty()) {
      auto kind = disjunctionLocator->getPath().back().getKind();
      if (kind == ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice ||
          kind == ConstraintLocator::DynamicLookupResult) {
        assert(index == 0 || index == 1);
        if (index == 1)
          CS.increaseScore(SK_ForceUnchecked, disjunctionLocator);
      }
    }
  }

  return choice.attempt(CS);
}

bool ConjunctionStep::attempt(const ConjunctionElement &element) {
  ++CS.solverState->NumConjunctionTerms;

  // Outside or previous element score doesn't affect
  // subsequent elements.
  CS.solverState->BestScore.reset();

  // Apply solution inferred for all the previous elements
  // because this element could reference declarations
  // established in previous element(s).
  if (!Solutions.empty()) {
    assert(Solutions.size() == 1);
    // Note that solution is removed here. This is done
    // because we want build a single complete solution
    // incrementally.
    CS.applySolution(Solutions.pop_back_val());
  }

  // Make sure that element is solved in isolation
  // by dropping all scoring information.
  CS.CurrentScore = Score();

  // Reset the scope counter to avoid "too complex" failures
  // when closure has a lot of elements in the body.
  CS.CountScopes = 0;

  // If timer is enabled, let's reset it so that each element
  // (expression) gets a fresh time slice to get solved. This
  // is important for closures with large number of statements
  // in them.
  if (CS.Timer) {
    CS.Timer.emplace(element.getLocator(), CS);
  }

  auto success = element.attempt(CS);

  // If element attempt has failed, mark whole conjunction
  // as a failure.
  if (!success)
    markAsFailed();

  return success;
}

StepResult ConjunctionStep::resume(bool prevFailed) {
  // Return from the follow-up splitter step that
  // attempted to apply information gained from the
  // isolated constraint to the outer context.
  if (Snapshot && Snapshot->isScoped()) {
    Snapshot.reset();
    if (CS.isDebugMode())
      getDebugLogger() << ")\n";
    
    return done(/*isSuccess=*/!prevFailed);
  }

  // If conjunction step is re-taken and there should be
  // active choice, let's see if it has be solved or not.
  assert(ActiveChoice);

  // Rewind back the constraint system information.
  ActiveChoice.reset();

  if (CS.isDebugMode())
    getDebugLogger() << ")\n";

  // Check whether it makes sense to continue solving
  // this conjunction. Note that for conjunction constraint
  // to be considered a success all of its elements have
  // to produce a single solution.
  {
    auto failConjunction = [&]() {
      markAsFailed();
      return done(/*isSuccess=*/false);
    };

    if (prevFailed)
      return failConjunction();

    // There could be a local ambiguity related to
    // the current element, let's try to resolve it.
    if (Solutions.size() > 1)
      filterSolutions(Solutions, /*minimize=*/true);

    // In diagnostic mode we need to stop a conjunction
    // but consider it successful if there are:
    //
    // - More than one solution for this element. Ambiguity
    //   needs to get propagated back to the outer context
    //   to be diagnosed.
    // - A single solution that requires one or more fixes,
    //   continuing would result in more errors associated
    //   with the failed element.
    if (CS.shouldAttemptFixes()) {
      if (Solutions.size() > 1)
        Producer.markExhausted();

      if (Solutions.size() == 1) {
        auto score = Solutions.front().getFixedScore();
        if (score.Data[SK_Fix] > 0 && !CS.getASTContext().CompletionCallback)
          Producer.markExhausted();
      }
    } else if (Solutions.size() != 1) {
      return failConjunction();
    }

    // Since there is only one solution, let's
    // consider this element as solved.
    AnySolved = true;
  }

  // After all of the elements have been checked, let's
  // see if conjunction was successful and if so, continue
  // solving along the current path until complete
  // solution is reached.
  if (Producer.isExhausted()) {
    // If one of the elements failed, that means while
    // conjunction failed with it.
    if (HadFailure)
      return done(/*isSuccess=*/false);

    // If this was an isolated conjunction solver needs to do
    // the following:
    //
    // a. Return all of the previously out-of-scope constraints;
    // b. Apply solution reached for the conjunction;
    // c. Continue solving along this path to reach a
    //    complete solution using type information
    //    inferred from this step.
    if (Conjunction->isIsolated()) {
      if (CS.isDebugMode()) {
        auto &log = getDebugLogger();
        log << "(applying conjunction result to outer context\n";
      }

      assert(
          Snapshot &&
          "Isolated conjunction requires a snapshot of the constraint system");

      // In diagnostic mode it's valid for an element to have
      // multiple solutions. Ambiguity just needs to be merged
      // into the outer context to be property diagnosed.
      if (Solutions.size() > 1) {
        assert(CS.shouldAttemptFixes());

        // Restore all outer type variables, constraints
        // and scoring information.
        Snapshot.reset();

        // Apply all of the information deduced from the
        // conjunction (up to the point of ambiguity)
        // back to the outer context and form a joined solution.
        unsigned numSolutions = 0;
        for (auto &solution : Solutions) {
          ConstraintSystem::SolverScope scope(CS);

          CS.applySolution(solution);

          // `applySolution` changes best/current scores
          // of the constraint system, so they have to be
          // restored right afterwards because score of the
          // element does contribute to the overall score.
          restoreBestScore();
          restoreCurrentScore(solution.getFixedScore());

          // Transform all of the unbound outer variables into
          // placeholders since we are not going to solve for
          // each ambiguous solution.
          {
            unsigned numHoles = 0;
            for (auto *typeVar : CS.getTypeVariables()) {
              if (!typeVar->getImpl().hasRepresentativeOrFixed()) {
                CS.assignFixedType(
                    typeVar, PlaceholderType::get(CS.getASTContext(), typeVar));
                ++numHoles;
              }
            }
            CS.increaseScore(SK_Hole, Conjunction->getLocator(), numHoles);
          }

          if (CS.worseThanBestSolution())
            continue;

          // Note that `worseThanBestSolution` isn't checked
          // here because `Solutions` were pre-filtered, and
          // outer score is the same for all of them.
          OuterSolutions.push_back(CS.finalize());
          ++numSolutions;
        }

        return done(/*isSuccess=*/numSolutions > 0);
      }

      auto solution = Solutions.pop_back_val();
      auto score = solution.getFixedScore();

      // Restore outer type variables and prepare to solve
      // constraints associated with outer context together
      // with information deduced from the conjunction.
      Snapshot->setupOuterContext(std::move(solution));

      // Pretend that conjunction never happened.
      restoreOuterState(score);

      // Now that all of the information from the conjunction has
      // been applied, let's attempt to solve the outer scope.
      return suspend(std::make_unique<SplitterStep>(CS, OuterSolutions));
    }
  }

  // Attempt next conjunction choice.
  return take(prevFailed);
}

void ConjunctionStep::restoreOuterState(const Score &solutionScore) const {
  // Restore best/current score, since upcoming step is going to
  // work with outer scope in relation to the conjunction.
  restoreBestScore();
  restoreCurrentScore(solutionScore);

  // Active all of the previously out-of-scope constraints
  // because conjunction can propagate type information up
  // by allowing its elements to reference type variables
  // from outer scope (e.g. variable declarations and or captures).
  {
    CS.ActiveConstraints.splice(CS.ActiveConstraints.end(),
                                CS.InactiveConstraints);
    for (auto &constraint : CS.ActiveConstraints)
      constraint.setActive(true);
  }
}

void ConjunctionStep::SolverSnapshot::applySolution(const Solution &solution) {
  CS.applySolution(solution);

  if (!CS.shouldAttemptFixes())
    return;

  // If inference succeeded, we are done.
  auto score = solution.getFixedScore();
  if (score.Data[SK_Fix] == 0)
    return;

  // If this conjunction represents a closure and inference
  // has failed, let's bind all of unresolved type variables
  // in its interface type to holes to avoid extraneous
  // fixes produced by outer context.
  auto locator = Conjunction->getLocator();
  if (locator->directlyAt<ClosureExpr>()) {
    auto closureTy =
        CS.getClosureType(castToExpr<ClosureExpr>(locator->getAnchor()));
    CS.recordTypeVariablesAsHoles(closureTy);
  }

  // Same for a SingleValueStmtExpr, turn any unresolved type variables present
  // in its type into holes.
  if (locator->isForSingleValueStmtConjunction()) {
    auto *SVE = castToExpr<SingleValueStmtExpr>(locator->getAnchor());
    CS.recordTypeVariablesAsHoles(CS.getType(SVE));
  }
}
