//===--- CSStep.h - Constraint Solver Steps -------------------------------===//
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

#ifndef SWIFT_SEMA_CSSTEP_H
#define SWIFT_SEMA_CSSTEP_H

#include "swift/AST/Types.h"
#include "swift/Sema/Constraint.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintSystem.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace llvm;

namespace swift {
namespace constraints {

class SolverStep;
class ComponentStep;

/// Represents available states which every
/// given step could be in during it's lifetime.
enum class StepState { Setup, Ready, Running, Suspended, Done };

/// Represents result of the step execution,
/// and can only be constructed by `SolverStep`.
struct StepResult {
  using Kind = ConstraintSystem::SolutionKind;

  friend class SolverStep;

private:
  Kind ResultKind;
  SmallVector<std::unique_ptr<SolverStep>, 4> NextSteps;

  StepResult(Kind kind) : ResultKind(kind) {}

  StepResult(Kind kind, std::unique_ptr<SolverStep> step) : ResultKind(kind) {
    NextSteps.push_back(std::move(step));
  }

  StepResult(Kind kind, SmallVectorImpl<std::unique_ptr<SolverStep>> &followup)
      : ResultKind(kind), NextSteps(std::move(followup)) {}

public:
  StepResult() = delete;

  Kind getKind() const { return ResultKind; }

  void transfer(SmallVectorImpl<std::unique_ptr<SolverStep>> &workList) {
    workList.reserve(NextSteps.size());
    for (auto &step : NextSteps)
      workList.push_back(std::move(step));
  }

private:
  static StepResult success() { return StepResult(Kind::Solved); }
  static StepResult failure() { return StepResult(Kind::Error); }

  static StepResult unsolved(std::unique_ptr<SolverStep> singleStep) {
    return StepResult(Kind::Unsolved, std::move(singleStep));
  }

  static StepResult
  unsolved(SmallVectorImpl<std::unique_ptr<SolverStep>> &followup) {
    return StepResult(Kind::Unsolved, followup);
  }
};

/// Represents a single independently solvable part of
/// the constraint system. And is a base class for all
/// different types of steps there are.
class SolverStep {
  friend class ConstraintSystem;

protected:
  ConstraintSystem &CS;

  StepState State = StepState::Setup;

  /// Once step is complete this is a container to hold finalized solutions.
  SmallVectorImpl<Solution> &Solutions;

public:
  explicit SolverStep(ConstraintSystem &cs,
                      SmallVectorImpl<Solution> &solutions)
      : CS(cs), Solutions(solutions) {}

  virtual ~SolverStep() {}

  /// \returns The current state of this step.
  StepState getState() const { return State; }

  /// Run preliminary setup (if needed) right
  /// before taking this step for the first time.
  virtual void setup() {}

  /// Try to move solver forward by simplifying constraints if possible.
  /// Such simplification might lead to either producing a solution, or
  /// creating a set of "follow-up" more granular steps to execute.
  ///
  /// \param prevFailed Indicate whether previous step
  ///        has failed (returned StepResult::Kind = Error),
  ///        this is useful to propagate failures when
  ///        unsolved steps are re-taken.
  ///
  /// \returns status and any follow-up steps to take before considering
  ///          this step solved or failed.
  virtual StepResult take(bool prevFailed) = 0;

  /// Try to resume previously suspended step.
  ///
  /// This happens after "follow-up" steps are done
  /// and all of the required information should be
  /// available to re-take this step.
  ///
  /// \param prevFailed Indicate whether previous step
  ///        has failed (returned StepResult::Kind = Error),
  ///        this is useful to propagate failures when
  ///        unsolved steps are re-taken.
  ///
  /// \returns status and any follow-up steps to take before considering
  ///          this step solved or failed.
  virtual StepResult resume(bool prevFailed) = 0;

  virtual void print(llvm::raw_ostream &Out) = 0;

protected:
  /// Transition this step into one of the available states.
  ///
  /// This is primarily driven by execution of the step itself and
  /// the solver, while it executes the work list.
  ///
  /// \param newState The new state this step should be in.
  void transitionTo(StepState newState) {
#ifndef NDEBUG
    // Make sure that ordering of the state transitions is correct,
    // because `setup -> ready -> running [-> suspended]* -> done`
    // is the only reasonable state transition path.
    switch (State) {
    case StepState::Setup:
      assert(newState == StepState::Ready);
      break;

    case StepState::Ready:
      assert(newState == StepState::Running);
      break;

    case StepState::Running:
      assert(newState == StepState::Suspended || newState == StepState::Done);
      break;

    case StepState::Suspended:
      assert(newState == StepState::Running);
      break;

    case StepState::Done:
      llvm_unreachable("step is already done.");
    }
#endif

    State = newState;
  }

  StepResult done(bool isSuccess) {
    transitionTo(StepState::Done);
    return isSuccess ? StepResult::success() : StepResult::failure();
  }

  StepResult replaceWith(std::unique_ptr<SolverStep> replacement) {
    transitionTo(StepState::Done);
    return StepResult(StepResult::Kind::Solved, std::move(replacement));
  }

  StepResult suspend(std::unique_ptr<SolverStep> followup) {
    transitionTo(StepState::Suspended);
    return StepResult::unsolved(std::move(followup));
  }

  StepResult suspend(SmallVectorImpl<std::unique_ptr<SolverStep>> &followup) {
    transitionTo(StepState::Suspended);
    return StepResult::unsolved(followup);
  }

  /// Erase constraint from the constraint system (include constraint graph)
  /// and return the constraint which follows it.
  ConstraintList::iterator erase(Constraint *constraint) {
    CS.CG.removeConstraint(constraint);
    return CS.InactiveConstraints.erase(constraint);
  }

  void restore(ConstraintList::iterator &iterator, Constraint *constraint) {
    CS.InactiveConstraints.insert(iterator, constraint);
    CS.CG.addConstraint(constraint);
  }

  void recordDisjunctionChoice(ConstraintLocator *disjunctionLocator,
                               unsigned index) const {
    CS.recordDisjunctionChoice(disjunctionLocator, index);
  }

  Score getCurrentScore() const { return CS.CurrentScore; }

  llvm::Optional<Score> getBestScore() const {
    return CS.solverState->BestScore;
  }

  void filterSolutions(SmallVectorImpl<Solution> &solutions, bool minimize) {
    CS.filterSolutions(solutions, minimize);
  }

  llvm::raw_ostream &getDebugLogger(bool indent = true) const {
    auto &log = llvm::errs();
    return indent ? log.indent(CS.solverState->getCurrentIndent()) : log;
  }
};

/// `SplitterStep` is responsible for running connected components
/// algorithm to determine how many independent sub-systems there are.
/// Once that's done it would create one `ComponentStep` per such
/// sub-system, and move to try to solve each and then merge partial
/// solutions produced by components into complete solution(s).
class SplitterStep final : public SolverStep {
  // Set of constraints associated with each component, after
  // component steps are complete, all of the constraints are
  // returned back to the work-list in their original order.
  SmallVector<ConstraintList, 4> Components;
  // Partial solutions associated with given step, each element
  // of the array presents a disjoint component (or follow-up step)
  // that current step has been split into.
  std::unique_ptr<SmallVector<Solution, 4>[]> PartialSolutions = nullptr;

  SmallVector<Constraint *, 4> OrphanedConstraints;

  /// Whether to include the partial results of this component in the final
  /// merged results.
  SmallVector<bool, 4> IncludeInMergedResults;

public:
  SplitterStep(ConstraintSystem &cs, SmallVectorImpl<Solution> &solutions)
      : SolverStep(cs, solutions) {}

  StepResult take(bool prevFailed) override;
  StepResult resume(bool prevFailed) override;

  void print(llvm::raw_ostream &Out) override {
    Out << "SplitterStep with #" << Components.size() << " components\n";
  }

private:
  /// If current step needs follow-up steps to get completely solved,
  /// let's compute them using connected components algorithm.
  void computeFollowupSteps(
      SmallVectorImpl<std::unique_ptr<SolverStep>> &steps);

  /// Once all of the follow-up steps are complete, let's try
  /// to merge resulting solutions together, to form final solution(s)
  /// for this step.
  ///
  /// \returns true if there are any solutions, false otherwise.
  bool mergePartialSolutions() const;
};

/// `DependentComponentSplitterStep` is responsible for composing the partial
/// solutions from other components (on which this component depends) into
/// the inputs based on which we can solve a particular component.
class DependentComponentSplitterStep final : public SolverStep {
  /// Constraints "in scope" of this step.
  ConstraintList *Constraints;

  /// Index into the parent splitter step.
  unsigned Index;

  /// The component that has dependencies.
  ConstraintGraph::Component Component;

  /// Array containing all of the partial solutions for the parent split.
  MutableArrayRef<SmallVector<Solution, 4>> AllPartialSolutions;

  /// The solutions computed the \c ComponentSteps created for each partial
  /// solution combinations. Will be merged into the final \c Solutions vector
  /// in \c resume.
  std::vector<std::unique_ptr<SmallVector<Solution, 2>>> ContextualSolutions;

  /// Take all of the constraints in this component and put them into
  /// \c Constraints.
  void injectConstraints() {
    for (auto constraint : Component.getConstraints()) {
      Constraints->erase(constraint);
      Constraints->push_back(constraint);
    }
  }

public:
  DependentComponentSplitterStep(
      ConstraintSystem &cs,
      ConstraintList *constraints,
      unsigned index,
      ConstraintGraph::Component &&component,
      MutableArrayRef<SmallVector<Solution, 4>> allPartialSolutions)
    : SolverStep(cs, allPartialSolutions[index]), Constraints(constraints),
      Index(index), Component(std::move(component)),
      AllPartialSolutions(allPartialSolutions) {
    assert(!Component.getDependencies().empty() && "Should use ComponentStep");
    injectConstraints();
  }

  StepResult take(bool prevFailed) override;
  StepResult resume(bool prevFailed) override;

  void print(llvm::raw_ostream &Out) override;
};


/// `ComponentStep` represents a set of type variables and related
/// constraints which could be solved independently. It's further
/// simplified into "binding" steps which attempt type variable and
/// disjunction choices.
class ComponentStep final : public SolverStep {
  class Scope {
    ConstraintSystem &CS;
    ConstraintSystem::SolverScope *SolverScope;

    SetVector<TypeVariableType *> TypeVars;
    ConstraintSystem::SolverScope *PrevPartialScope = nullptr;

    // The component this scope is associated with.
    ComponentStep &Component;

  public:
    Scope(ComponentStep &component);

    ~Scope() {
      delete SolverScope; // rewind back all of the changes.
      CS.solverState->PartialSolutionScope = PrevPartialScope;

      // return all of the saved type variables back to the system.
      CS.TypeVariables = std::move(TypeVars);
      // return all of the saved constraints back to the component.
      auto &constraints = *Component.Constraints;
      constraints.splice(constraints.end(), CS.InactiveConstraints);
    }
  };

  /// The position of the component in the set of
  /// components produced by "split" step.
  unsigned Index;

  /// Indicates whether this is only component produced
  /// by "split" step. This information opens optimization
  /// opportunity, because if there are no other components,
  /// constraint system doesn't have to pruned from
  /// unrelated type variables and their constraints.
  bool IsSingle;

  /// The score associated with constraint system before
  /// the component step is taken.
  Score OriginalScore;

  /// The original best score computed before any of the
  /// component steps belonging to the same "split" are taken.
  llvm::Optional<Score> OriginalBestScore;

  /// If this step depends on other smaller steps to be solved first
  /// we need to keep active scope until all of the work is done.
  std::unique_ptr<Scope> ComponentScope = nullptr;

  /// Type variables and constraints "in scope" of this step.
  TinyPtrVector<TypeVariableType *> TypeVars;
  /// Constraints "in scope" of this step.
  ConstraintList *Constraints;

  /// The set of partial solutions that should be composed before evaluating
  /// this component.
  SmallVector<const Solution *, 2> DependsOnPartialSolutions;

  /// Constraint which doesn't have any free type variables associated
  /// with it, which makes it disconnected in the graph.
  Constraint *OrphanedConstraint = nullptr;

public:
  /// Create a single component step.
  ComponentStep(ConstraintSystem &cs, unsigned index,
                ConstraintList *constraints,
                SmallVectorImpl<Solution> &solutions)
      : SolverStep(cs, solutions), Index(index), IsSingle(true),
        OriginalScore(getCurrentScore()), OriginalBestScore(getBestScore()),
        Constraints(constraints) {}

  /// Create a component step from a constraint graph component.
  ComponentStep(ConstraintSystem &cs, unsigned index,
                ConstraintList *constraints,
                ConstraintGraph::Component &&component,
                SmallVectorImpl<Solution> &solutions)
      : SolverStep(cs, solutions), Index(index), IsSingle(false),
        OriginalScore(getCurrentScore()), OriginalBestScore(getBestScore()),
        Constraints(constraints) {
    if (component.isOrphaned()) {
      assert(component.getConstraints().size() == 1);
      OrphanedConstraint = component.getConstraints().front();
    } else {
      assert(component.typeVars.size() > 0);
    }

    TypeVars = std::move(component.typeVars);

    for (auto constraint : component.getConstraints()) {
      constraints->erase(constraint);
      Constraints->push_back(constraint);
    }

    assert(component.getDependencies().empty());
  }

  /// Create a component step that composes existing partial solutions before
  /// solving constraints.
  ComponentStep(
      ConstraintSystem &cs, unsigned index,
      ConstraintList *constraints,
      const ConstraintGraph::Component &component,
      llvm::SmallVectorImpl<const Solution *> &&dependsOnPartialSolutions,
      SmallVectorImpl<Solution> &solutions)
        : SolverStep(cs, solutions), Index(index), IsSingle(false),
          OriginalScore(getCurrentScore()), OriginalBestScore(getBestScore()),
          Constraints(constraints),
          DependsOnPartialSolutions(std::move(dependsOnPartialSolutions)) {
    TypeVars = component.typeVars;
    assert(DependsOnPartialSolutions.size() ==
           component.getDependencies().size());

    for (auto constraint : component.getConstraints()) {
      constraints->erase(constraint);
      Constraints->push_back(constraint);
    }
  }

  StepResult take(bool prevFailed) override;

  StepResult resume(bool prevFailed) override { return finalize(!prevFailed); }

  void print(llvm::raw_ostream &Out) override {
    Out << "ComponentStep with at #" << Index << '\n';
  }

private:
  void setupScope() {
    // If this is a single component, there is no need
    // to preliminary modify constraint system or log anything.
    if (IsSingle)
      return;
    
    if (CS.isDebugMode()) {
      auto &log = getDebugLogger();
      log << "(solving component #" << Index << '\n';
    }
    
    ComponentScope = std::make_unique<Scope>(*this);
    
    if (CS.isDebugMode()) {
      auto &log = getDebugLogger();
      log << "Type variables in scope = "
          << "[";
      auto typeVars = CS.getTypeVariables();
      PrintOptions PO;
      PO.PrintTypesForDebugging = true;
      interleave(typeVars, [&](TypeVariableType *typeVar) {
                   Type(typeVar).print(log, PO);
                 },
                 [&] {
                   log << ", ";
                 });
      log << "]" << '\n';
    }

    // If this component has orphaned constraint attached,
    // let's return it to the graph.
    CS.CG.setOrphanedConstraint(OrphanedConstraint);
  }

  /// Finalize current component by either cleanup if sub-tasks
  /// have failed, or solution generation and minimization.
  StepResult finalize(bool isSuccess);
};

template <typename P> class BindingStep : public SolverStep {
protected:
  using Scope = ConstraintSystem::SolverScope;

  P Producer;

  /// Indicates whether any of the attempted bindings
  /// produced a solution.
  bool AnySolved = false;

  /// Active binding (scope + choice) which is currently
  /// being attempted, helps to rewind state of the
  /// constraint system back to original before attempting
  /// next binding, if any.
  llvm::Optional<std::pair<std::unique_ptr<Scope>, typename P::Element>>
      ActiveChoice;

  BindingStep(ConstraintSystem &cs, P producer,
              SmallVectorImpl<Solution> &solutions)
      : SolverStep(cs, solutions), Producer(std::move(producer)) {}

public:
  StepResult take(bool prevFailed) override {
    // Before attempting the next choice, let's check whether the constraint
    // system is too complex already.
    if (CS.isTooComplex(Solutions))
      return done(/*isSuccess=*/false);

    while (auto choice = Producer()) {
      if (shouldSkip(*choice))
        continue;

      if (shouldStopAt(*choice))
        break;

      if (CS.isDebugMode()) {
        auto &log = getDebugLogger();
        log << "(attempting ";
        choice->print(log, &CS.getASTContext().SourceMgr, CS.solverState->getCurrentIndent() + 2);
        log << '\n';
      }

      {
        auto scope = std::make_unique<Scope>(CS);
        if (attempt(*choice)) {
          ActiveChoice.emplace(std::move(scope), *choice);

          if (CS.isDebugMode()) {
            auto &log = llvm::errs();
            auto &CG = CS.getConstraintGraph();
            CG.dumpActiveScopeChanges(log, CS.solverState->getCurrentIndent());
          }
          
          return suspend(std::make_unique<SplitterStep>(CS, Solutions));
        }
      }

      if (CS.isDebugMode())
        getDebugLogger() << ")\n";

      // If this binding didn't match, let's check if we've attempted
      // enough bindings to stop, because some producers might need
      // to compute next step of bindings to try, which we'd want to avoid.
      if (shouldStopAfter(*choice))
        break;
    }

    return done(/*isSuccess=*/AnySolved);
  }

protected:
  /// Attempt to apply given binding choice to constraint system.
  /// This action is going to establish "active choice" of this step
  /// to point to a given choice.
  ///
  /// \param choice The choice to attempt.
  ///
  /// \return true if the choice has been accepted and system can be
  /// simplified further, false otherwise.
  virtual bool attempt(const typename P::Element &choice) = 0;

  /// Check whether attempting this choice could be avoided,
  /// which could speed-up solving.
  virtual bool shouldSkip(const typename P::Element &choice) const = 0;

  /// Check whether attempting binding choices should be stopped,
  /// because optimal solution has already been found.
  virtual bool shouldStopAt(const typename P::Element &choice) const = 0;

  /// Check whether attempting binding choices should be stopped,
  /// after current choice has been attempted, because optimal
  /// solution has already been found,
  virtual bool shouldStopAfter(const typename P::Element &choice) const {
    return false;
  }

  bool needsToComputeNext() const { return Producer.needsToComputeNext(); }

  ConstraintLocator *getLocator() const { return Producer.getLocator(); }
};

class TypeVariableStep final : public BindingStep<TypeVarBindingProducer> {
  using BindingContainer = inference::BindingSet;
  using Binding = inference::PotentialBinding;

  TypeVariableType *TypeVar;

  /// Indicates whether source of one of the previously
  /// attempted bindings was a literal constraint. This
  /// is useful for a performance optimization to stop
  /// attempting other bindings in certain conditions.
  bool SawFirstLiteralConstraint = false;

public:
  TypeVariableStep(BindingContainer &bindings,
                   SmallVectorImpl<Solution> &solutions)
      : BindingStep(bindings.getConstraintSystem(), {bindings}, solutions),
        TypeVar(bindings.getTypeVariable()) {}

  void setup() override;

  StepResult resume(bool prevFailed) override;

  void print(llvm::raw_ostream &Out) override {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    Out << "TypeVariableStep for " << TypeVar->getString(PO) << '\n';
  }

protected:
  bool attempt(const TypeVariableBinding &choice) override;

  bool shouldSkip(const TypeVariableBinding &choice) const override {
    // Let's always attempt types inferred from "defaultable" constraints
    // in diagnostic mode. This allows the solver to attempt i.e. `Any`
    // for collection literals and produce better diagnostics for for-in
    // statements like `for (x, y, z) in [] { ... }` when pattern type
    // could not be inferred.
    if (CS.shouldAttemptFixes())
      return false;

    // If this is a defaultable binding and we have found solutions,
    // don't explore the default binding.
    return AnySolved && choice.isDefaultable();
  }

  /// Check whether attempting type variable binding choices should
  /// be stopped, because optimal solution has already been found.
  bool shouldStopAt(const TypeVariableBinding &choice) const override {
    // Let's always attempt default types inferred from literals in diagnostic
    // mode because that could lead to better diagnostics if the problem is
    // contextual like argument/parameter conversion or collection element
    // mismatch.
    if (CS.shouldAttemptFixes())
      return false;

    // If we were able to solve this without considering
    // default literals, don't bother looking at default literals.
    return AnySolved && choice.hasDefaultedProtocol() &&
           !SawFirstLiteralConstraint;
  }

  bool shouldStopAfter(const TypeVariableBinding &choice) const override {
    // Let's always attempt additional bindings in diagnostic mode, as that
    // could lead to better diagnostic for e.g trying the unwrapped type.
    if (CS.shouldAttemptFixes())
      return false;

    // If there has been at least one solution so far
    // at a current batch of bindings is done it's a
    // success because each new batch would be less
    // and less precise.
    return AnySolved && needsToComputeNext();
  }
};

class DisjunctionStep final : public BindingStep<DisjunctionChoiceProducer> {
  Constraint *Disjunction;
  SmallVector<Constraint *, 4> DisabledChoices;
  ConstraintList::iterator AfterDisjunction;

  llvm::Optional<Score> BestNonGenericScore;
  llvm::Optional<std::pair<Constraint *, Score>> LastSolvedChoice;

public:
  DisjunctionStep(ConstraintSystem &cs, Constraint *disjunction,
                  SmallVectorImpl<Solution> &solutions)
      : BindingStep(cs, {cs, disjunction}, solutions), Disjunction(disjunction),
        AfterDisjunction(erase(disjunction)) {
    assert(Disjunction->getKind() == ConstraintKind::Disjunction);
    pruneOverloadSet(Disjunction);
    ++cs.solverState->NumDisjunctions;
  }

  ~DisjunctionStep() override {
    // Rewind back any changes left after attempting last choice.
    ActiveChoice.reset();
    // Return disjunction constraint back to the system.
    restore(AfterDisjunction, Disjunction);
    // Re-enable previously disabled overload choices.
    for (auto *choice : DisabledChoices)
      choice->setEnabled();
  }

  StepResult resume(bool prevFailed) override;

  void print(llvm::raw_ostream &Out) override {
    Out << "DisjunctionStep for ";
    Disjunction->print(Out, &CS.getASTContext().SourceMgr,
                       CS.solverState->getCurrentIndent());
    Out << '\n';
  }

private:
  bool shouldSkip(const DisjunctionChoice &choice) const override;

  /// Whether we should short-circuit a disjunction that already has a
  /// solution when we encounter the given choice.
  ///
  /// FIXME: This is performance hack, which should go away.
  ///
  /// \params choice The disjunction choice we are about to attempt.
  ///
  /// \returns true if disjunction step should be considered complete,
  ///          false otherwise.
  bool shouldStopAt(const DisjunctionChoice &choice) const override;
  bool shortCircuitDisjunctionAt(Constraint *currentChoice,
                                 Constraint *lastSuccessfulChoice) const;

  bool shouldSkipGenericOperators() const {
    if (!BestNonGenericScore)
      return false;

    // Let's skip generic overload choices only in case if
    // non-generic score indicates that there were no forced
    // unwrappings of optional(s), no unavailable overload
    // choices present in the solution, no fixes required,
    // and there are no non-trivial user or function conversions.
    auto &score = BestNonGenericScore->Data;
    return (score[SK_ForceUnchecked] == 0 && score[SK_Unavailable] == 0 &&
            score[SK_Fix] == 0 && score[SK_UserConversion] == 0 &&
            score[SK_FunctionConversion] == 0);
  }

  /// Attempt to apply given disjunction choice to constraint system.
  /// This action is going to establish "active choice" of this disjunction
  /// to point to a given choice.
  ///
  /// \param choice The choice to attempt.
  ///
  /// \return true if the choice has been accepted and system can be
  /// simplified further, false otherwise.
  bool attempt(const DisjunctionChoice &choice) override;

  // Check if selected disjunction has a representative
  // this might happen when there are multiple binary operators
  // chained together. If so, disable choices which differ
  // from currently selected representative.
  void pruneOverloadSet(Constraint *disjunction) {
    auto *choice = disjunction->getNestedConstraints().front();
    if (choice->getKind() != ConstraintKind::BindOverload)
      return;

    auto *typeVar = choice->getFirstType()->getAs<TypeVariableType>();
    if (!typeVar)
      return;

    auto *repr = typeVar->getImpl().getRepresentative(nullptr);
    if (!repr || repr == typeVar)
      return;

    for (auto overload : CS.getResolvedOverloads()) {
      auto resolved = overload.second;
      if (!resolved.boundType->isEqual(repr))
        continue;

      auto &representative = resolved.choice;
      if (!representative.isDecl())
        return;

      // Disable all of the overload choices which are different from
      // the one which is currently picked for representative.
      for (auto *constraint : disjunction->getNestedConstraints()) {
        auto choice = constraint->getOverloadChoice();
        if (!choice.isDecl() || choice.getDecl() == representative.getDecl())
          continue;

        constraint->setDisabled();
        DisabledChoices.push_back(constraint);
      }
      break;
    }
  };

  // Figure out which of the solutions has the smallest score.
  static llvm::Optional<Score>
  getBestScore(SmallVectorImpl<Solution> &solutions) {
    if (solutions.empty())
      return None;

    Score bestScore = solutions.front().getFixedScore();
    if (solutions.size() == 1)
      return bestScore;

    for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
      auto &score = solutions[i].getFixedScore();
      if (score < bestScore)
        bestScore = score;
    }
    return bestScore;
  }
};

class ConjunctionStep : public BindingStep<ConjunctionElementProducer> {
  /// Snapshot of the constraint system before conjunction.
  class SolverSnapshot {
    ConstraintSystem &CS;

    /// The conjunction this snapshot belongs to.
    Constraint *Conjunction;

    llvm::Optional<llvm::SaveAndRestore<DeclContext *>> DC = None;

    llvm::SetVector<TypeVariableType *> TypeVars;
    ConstraintList Constraints;

    /// If this conjunction has to be solved in isolation,
    /// this scope would be initialized once all of the
    /// elements are successfully solved to continue solving
    /// along the current path as-if there was no conjunction.
    std::unique_ptr<Scope> IsolationScope = nullptr;

  public:
    SolverSnapshot(ConstraintSystem &cs, Constraint *conjunction)
        : CS(cs), Conjunction(conjunction),
          TypeVars(std::move(cs.TypeVariables)) {
      auto *locator = Conjunction->getLocator();
      // If this conjunction represents a closure, we need to
      // switch declaration context over to it.
      if (locator->directlyAt<ClosureExpr>()) {
        DC.emplace(CS.DC, castToExpr<ClosureExpr>(locator->getAnchor()));
      }

      auto &CG = CS.getConstraintGraph();
      // Remove all of the current inactive constraints.
      Constraints.splice(Constraints.end(), CS.InactiveConstraints);
      // Clear constraint graph.
      for (auto &constraint : Constraints)
        CG.removeConstraint(&constraint);
    }

    void setupOuterContext(Solution solution) {
      // Re-add type variables and constraints back
      // to the constraint system.
      restore();

      // Establish isolation scope so that conjunction solution
      // and follow-up steps could be rolled back.
      IsolationScope = std::make_unique<Scope>(CS);

      // Apply solution inferred for the conjunction.
      applySolution(solution);

      // Add constraints to the graph after solution
      // has been applied to make sure that all type
      // information is available to incremental inference.
      for (auto &constraint : CS.InactiveConstraints)
        CS.CG.addConstraint(&constraint);
    }

    bool isScoped() const { return bool(IsolationScope); }

    ~SolverSnapshot() {
      if (!IsolationScope)
        restore();

      IsolationScope.reset();
      // Re-add all of the constraint to the constraint
      // graph after scope has been rolled back, to make
      // make sure the original (before conjunction)
      // state is completely restored.
      updateConstraintGraph();
    }

  private:
    void restore() {
      DC.reset();
      CS.TypeVariables = std::move(TypeVars);
      CS.InactiveConstraints.splice(CS.InactiveConstraints.end(), Constraints);
    }

    void updateConstraintGraph() {
      auto &CG = CS.getConstraintGraph();
      for (auto &constraint : CS.InactiveConstraints)
        CG.addConstraint(&constraint);
    }

    void applySolution(const Solution &solution);
  };

  /// Best solution solver reached so far.
  llvm::Optional<Score> BestScore;
  /// The score established before conjunction is attempted.
  Score CurrentScore;

  /// The number of constraint solver scopes already explored
  /// before accepting this conjunction.
  llvm::SaveAndRestore<unsigned> OuterScopeCount;

  /// The number of milliseconds until outer constraint system
  /// is considered "too complex" if timer is enabled.
  llvm::Optional<std::pair<ExpressionTimer::AnchorType, unsigned>>
      OuterTimeRemaining = None;

  /// Conjunction constraint associated with this step.
  Constraint *Conjunction;
  /// Position of the conjunction in the inactive constraints
  /// list which is required to re-instate it to the system
  /// after this step is done.
  ConstraintList::iterator AfterConjunction;

  /// Indicates that one of the elements failed inference.
  bool HadFailure = false;

  /// If conjunction has to be solved in isolation, this
  /// variable would capture the snapshot of the constraint
  /// system step before conjunction step.
  llvm::Optional<SolverSnapshot> Snapshot;

  /// A set of previously deduced solutions. This is used upon
  /// successful solution of an isolated conjunction to introduce
  /// all of the inferred information back into the outer context.
  SmallVectorImpl<Solution> &OuterSolutions;

  /// Solutions produced while attempting elements of an isolated conjunction.
  ///
  /// Note that this is what `BindingStep` is initialized with
  /// in isolated mode.
  SmallVector<Solution, 4> IsolatedSolutions;

  /// If \c ConjunctionStep::attempt modified the constraint system options,
  /// it will store the original options in this \c llvm::SaveAndRestore.
  /// Upon \c resume, these values will be restored.
  llvm::Optional<llvm::SaveAndRestore<ConstraintSystemOptions>> ModifiedOptions;

public:
  ConjunctionStep(ConstraintSystem &cs, Constraint *conjunction,
                  SmallVectorImpl<Solution> &solutions)
      : BindingStep(cs, {cs, conjunction},
                    conjunction->isIsolated() ? IsolatedSolutions : solutions),
        BestScore(getBestScore()), CurrentScore(getCurrentScore()),
        OuterScopeCount(cs.CountScopes, 0), Conjunction(conjunction),
        AfterConjunction(erase(conjunction)), OuterSolutions(solutions) {
    assert(conjunction->getKind() == ConstraintKind::Conjunction);

    // Make a snapshot of the constraint system state before conjunction.
    if (conjunction->isIsolated())
      Snapshot.emplace(cs, conjunction);

    if (cs.Timer) {
      auto remainingTime = cs.Timer->getRemainingProcessTimeInMillis();
      OuterTimeRemaining.emplace(cs.Timer->getAnchor(), remainingTime);
    }
  }

  ~ConjunctionStep() override {
    assert(!bool(ActiveChoice));

    // Return all of the type variables and constraints back.
    Snapshot.reset();

    // Restore conjunction constraint.
    restore(AfterConjunction, Conjunction);

    // Restore best score only if conjunction fails because
    // successful outcome should keep a score set by `restoreOuterState`.
    if (HadFailure) {
      auto solutionScore = Score();
      restoreBestScore();
      restoreCurrentScore(solutionScore);
    }

    if (OuterTimeRemaining) {
      auto anchor = OuterTimeRemaining->first;
      auto remainingTime = OuterTimeRemaining->second;
      CS.Timer.emplace(anchor, CS, remainingTime);
    }
  }

  StepResult resume(bool prevFailed) override;

  void print(llvm::raw_ostream &Out) override {
    Out << "ConjunctionStep for ";
    Conjunction->print(Out, &CS.getASTContext().SourceMgr,
                       CS.solverState->getCurrentIndent());
    Out << '\n';
  }

protected:
  bool attempt(const ConjunctionElement &element) override;

  /// Conjunction can't skip elements.
  bool shouldSkip(const ConjunctionElement &element) const override {
    return false;
  }

  /// Conjunction can't reject attempting any of its elements.
  bool shouldStopAt(const ConjunctionElement &element) const override {
    return false;
  }

  /// Conjunctions only stop after first failure.
  ///
  /// TODO: In diagnostic mode conjunction evaluation should stop
  ///       after first element failure and consider the rest to
  ///       be solved, in order to produce good diagnostics.
  bool shouldStopAfter(const ConjunctionElement &element) const override {
    return HadFailure;
  }

  void markAsFailed() {
    HadFailure = true;
    // During performance mode, failure to infer a type for one
    // of the elements automatically fails whole conjunction.
    //
    // TODO: In diagnostic mode, let's consider this conjunction
    // a success if at least one of its elements was solved
    // successfully by use of fixes, and ignore the rest.
    AnySolved = false;
  }

private:
  /// Restore best and current scores as they were before conjunction.
  void restoreCurrentScore(const Score &solutionScore) const {
    CS.CurrentScore = CurrentScore;
    CS.increaseScore(SK_Fix, Conjunction->getLocator(),
                     solutionScore.Data[SK_Fix]);
    CS.increaseScore(SK_Hole, Conjunction->getLocator(),
                     solutionScore.Data[SK_Hole]);
  }

  void restoreBestScore() const { CS.solverState->BestScore = BestScore; }

  // Restore constraint system state before conjunction.
  //
  // Note that this doesn't include conjunction constraint
  // itself because we don't want to re-solve it.
  void restoreOuterState(const Score &solutionScore) const;
};

} // end namespace constraints
} // end namespace swift

#endif // SWIFT_SEMA_CSSTEP_H
