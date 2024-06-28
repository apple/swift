//===--- ConstraintGraph.cpp - Constraint Graph ---------------------------===//
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
//
// This file implements the \c ConstraintGraph class, which describes the
// relationships among the type variables within a constraint system.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintGraphScope.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Basic/Assertions.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>
#include <memory>
#include <numeric>

using namespace swift;
using namespace constraints;

#define DEBUG_TYPE "ConstraintGraph"

#pragma mark Graph construction/destruction

ConstraintGraph::ConstraintGraph(ConstraintSystem &cs) : CS(cs) { }

ConstraintGraph::~ConstraintGraph() {
#ifndef NDEBUG
  // If constraint system is in an invalid state, it's
  // possible that constraint graph is corrupted as well
  // so let's not attempt to check change log.
  if (!CS.inInvalidState())
    assert(Changes.empty() && "Scope stack corrupted");
#endif

  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto &impl = TypeVariables[i]->getImpl();
    delete impl.getGraphNode();
    impl.setGraphNode(nullptr);
  }
}

#pragma mark Graph accessors

std::pair<ConstraintGraphNode &, unsigned>
ConstraintGraph::lookupNode(TypeVariableType *typeVar) {
  // Check whether we've already created a node for this type variable.
  auto &impl = typeVar->getImpl();
  if (auto nodePtr = impl.getGraphNode()) {
    assert(impl.getGraphIndex() < TypeVariables.size() && "Out-of-bounds index");
    assert(TypeVariables[impl.getGraphIndex()] == typeVar && 
           "Type variable mismatch");
    return { *nodePtr, impl.getGraphIndex() };
  }

  // Allocate the new node.
  auto nodePtr = new ConstraintGraphNode(*this, typeVar);
  unsigned index = TypeVariables.size();
  impl.setGraphNode(nodePtr);
  impl.setGraphIndex(index);

  // Record this type variable.
  TypeVariables.push_back(typeVar);

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::addedTypeVariable(typeVar));

  // If this type variable is not the representative of its equivalence class,
  // add it to its representative's set of equivalences.
  auto typeVarRep = CS.getRepresentative(typeVar);
  if (typeVar != typeVarRep)
    mergeNodes(typeVar, typeVarRep);
  else if (auto fixed = CS.getFixedType(typeVarRep)) {
    // Bind the type variable.
    bindTypeVariable(typeVar, fixed);
  }

  return { *nodePtr, index };
}

bool ConstraintGraphNode::forRepresentativeVar() const {
  auto *typeVar = getTypeVariable();
  return typeVar == typeVar->getImpl().getRepresentative(nullptr);
}

ArrayRef<TypeVariableType *> ConstraintGraphNode::getEquivalenceClass() const{
  assert(forRepresentativeVar() &&
         "Can't request equivalence class from non-representative type var");
  return getEquivalenceClassUnsafe();
}

ArrayRef<TypeVariableType *>
ConstraintGraphNode::getEquivalenceClassUnsafe() const{
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  return EquivalenceClass;
}

#pragma mark Node mutation

static bool isUsefulForReferencedVars(Constraint *constraint) {
  switch (constraint->getKind()) {
    // Don't attempt to propagate information about `Bind`s and
    // `BindOverload`s to referenced variables since they are
    // adjacent through that binding already, and there is no
    // useful information in trying to process that kind of
    // constraint.
  case ConstraintKind::Bind:
  case ConstraintKind::BindOverload:
    return false;

  default:
    return true;
  }
}

void ConstraintGraphNode::addConstraint(Constraint *constraint) {
  assert(ConstraintIndex.count(constraint) == 0 && "Constraint re-insertion");
  ConstraintIndex[constraint] = Constraints.size();
  Constraints.push_back(constraint);

  {
    introduceToInference(constraint);

    if (isUsefulForReferencedVars(constraint)) {
      notifyReferencedVars([&](ConstraintGraphNode &referencedVar) {
        referencedVar.introduceToInference(constraint);
      });
    }
  }
}

void ConstraintGraphNode::removeConstraint(Constraint *constraint) {
  auto pos = ConstraintIndex.find(constraint);
  assert(pos != ConstraintIndex.end());

  // Remove this constraint from the constraint mapping.
  auto index = pos->second;
  ConstraintIndex.erase(pos);
  assert(Constraints[index] == constraint && "Mismatched constraint");

  {
    retractFromInference(constraint);

    if (isUsefulForReferencedVars(constraint)) {
      notifyReferencedVars([&](ConstraintGraphNode &referencedVar) {
        referencedVar.retractFromInference(constraint);
      });
    }
  }

  // If this is the last constraint, just pop it off the list and we're done.
  unsigned lastIndex = Constraints.size()-1;
  if (index == lastIndex) {
    Constraints.pop_back();
    return;
  }

  // This constraint is somewhere in the middle; swap it with the last
  // constraint, so we can remove the constraint from the vector in O(1)
  // time rather than O(n) time.
  auto lastConstraint = Constraints[lastIndex];
  Constraints[index] = lastConstraint;
  ConstraintIndex[lastConstraint] = index;
  Constraints.pop_back();
}

void ConstraintGraphNode::notifyReferencingVars() const {
  SmallVector<TypeVariableType *, 4> stack;

  stack.push_back(TypeVar);

  auto updateAdjacencies = [&](TypeVariableType *typeVar) {
    for (auto *constraint : CG[typeVar].getConstraints()) {
      if (constraint->getClassification() !=
          ConstraintClassification::Relational)
        continue;

      auto lhsTy = constraint->getFirstType();
      auto rhsTy = constraint->getSecondType();

      Type affectedTy =
          ConstraintSystem::typeVarOccursInType(typeVar, lhsTy) ? rhsTy : lhsTy;

      if (auto *affectedVar = affectedTy->getAs<TypeVariableType>()) {
        auto *repr =
            affectedVar->getImpl().getRepresentative(/*record=*/nullptr);

        if (!repr->getImpl().getFixedType(/*record=*/nullptr))
          CG[repr].reintroduceToInference(constraint);
      }
    }
  };

  while (!stack.empty()) {
    auto *typeVar = stack.pop_back_val();

    // All of the relational constraints associated with this
    // variable need to get re-introduced to other mentioned
    // type variable to update their bindings.
    //
    // If variable is a representative of an equivalence class
    // it means that all members have been modified together
    // with their representative and their adjacencies have to
    // get updated as well.
    if (CG[typeVar].forRepresentativeVar()) {
      for (auto *eqVar : CG[typeVar].getEquivalenceClass()) {
        updateAdjacencies(eqVar);

        for (auto *referrer : CG[eqVar].getReferencedBy())
          stack.push_back(referrer);
      }
    } else {
      updateAdjacencies(typeVar);

      // If current type variable is referenced by some other
      // type variable as part of its fixed type it means that
      // all of the adjacencies of that variable have to be
      // notified as well otherwise they'll miss change in type.
      for (auto *referrer : CG[typeVar].getReferencedBy())
        stack.push_back(referrer);
    }
  }
}

void ConstraintGraphNode::notifyReferencedVars(
    llvm::function_ref<void(ConstraintGraphNode &)> notification) {
  for (auto *fixedBinding : getReferencedVars()) {
    notification(CG[fixedBinding]);
  }
}

void ConstraintGraphNode::addToEquivalenceClass(
       ArrayRef<TypeVariableType *> typeVars) {
  assert(forRepresentativeVar() &&
         "Can't extend equivalence class of non-representative type var");
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(getTypeVariable());
  EquivalenceClass.append(typeVars.begin(), typeVars.end());

  {
    for (auto *newMember : typeVars) {
      auto &node = CG[newMember];

      for (auto *constraint : node.getConstraints()) {
        introduceToInference(constraint);

        if (!isUsefulForReferencedVars(constraint))
          continue;

        notifyReferencedVars([&](ConstraintGraphNode &referencedVar) {
          referencedVar.introduceToInference(constraint);
        });
      }

      node.notifyReferencingVars();
    }
  }
}

void ConstraintGraphNode::truncateEquivalenceClass(unsigned prevSize) {
  llvm::SmallSetVector<TypeVariableType *, 4> disconnectedVars;
  for (auto disconnected = EquivalenceClass.begin() + prevSize;
       disconnected != EquivalenceClass.end();
       ++disconnected) {
    disconnectedVars.insert(*disconnected);
  }

  EquivalenceClass.erase(EquivalenceClass.begin() + prevSize,
                         EquivalenceClass.end());

  // We need to re-introduce each constraint associated with
  // "disconnected" member itself and to this representative.
  {
    // Re-infer bindings for the current representative.
    resetBindingSet();

    // Re-infer bindings all of the newly made representatives.
    for (auto *typeVar : disconnectedVars)
      CG[typeVar].notifyReferencingVars();
  }
}

void ConstraintGraphNode::addReferencedVar(TypeVariableType *typeVar) {
  bool inserted = References.insert(typeVar);
  assert(inserted && "Attempt to reference a duplicate type variable");
  (void)inserted;
}

void ConstraintGraphNode::addReferencedBy(TypeVariableType *typeVar) {
  bool inserted = ReferencedBy.insert(typeVar);
  assert(inserted && "Already referenced by the given type variable");
  (void)inserted;
}

void ConstraintGraphNode::removeReference(TypeVariableType *typeVar) {
  auto removed = References.remove(typeVar);
  assert(removed && "Variables are not connected");
  (void)removed;
}

void ConstraintGraphNode::removeReferencedBy(TypeVariableType *typeVar) {
  auto removed = ReferencedBy.remove(typeVar);
  assert(removed && "Variables are not connected");
  (void)removed;
}

inference::PotentialBindings &ConstraintGraphNode::getCurrentBindings() {
  assert(forRepresentativeVar());

  if (!Bindings)
    Bindings.emplace(CG.getConstraintSystem(), TypeVar);
  return *Bindings;
}

void ConstraintGraphNode::introduceToInference(Constraint *constraint) {
  if (forRepresentativeVar()) {
    auto fixedType = TypeVar->getImpl().getFixedType(/*record=*/nullptr);
    if (!fixedType)
      getCurrentBindings().infer(constraint);
  } else {
    auto *repr =
        getTypeVariable()->getImpl().getRepresentative(/*record=*/nullptr);
    CG[repr].introduceToInference(constraint);
  }
}

void ConstraintGraphNode::retractFromInference(Constraint *constraint) {
  if (forRepresentativeVar()) {
    auto fixedType = TypeVar->getImpl().getFixedType(/*record=*/nullptr);
    if (!fixedType)
      getCurrentBindings().retract(constraint);
  } else {
    auto *repr =
        getTypeVariable()->getImpl().getRepresentative(/*record=*/nullptr);
    CG[repr].retractFromInference(constraint);
  }
}

void ConstraintGraphNode::reintroduceToInference(Constraint *constraint) {
  retractFromInference(constraint);
  introduceToInference(constraint);
}

void ConstraintGraphNode::introduceToInference(Type fixedType) {
  // Notify all of the type variables that reference this one.
  //
  // Since this type variable has been replaced with a fixed type
  // all of the concrete types that reference it are going to change,
  // which means that all of the not-yet-attempted bindings should
  // change as well.
  notifyReferencingVars();

  if (!fixedType->hasTypeVariable())
    return;

  SmallPtrSet<TypeVariableType *, 4> referencedVars;
  fixedType->getTypeVariables(referencedVars);

  for (auto *referencedVar : referencedVars) {
    auto &node = CG[referencedVar];

    // Newly referred vars need to re-introduce all constraints associated
    // with this type variable since they are now going to be used in
    // all of the constraints that reference bound type variable.
    for (auto *constraint : getConstraints()) {
      if (isUsefulForReferencedVars(constraint))
        node.reintroduceToInference(constraint);
    }
  }
}

void ConstraintGraphNode::retractFromInference(
    Type fixedType, SmallPtrSetImpl<TypeVariableType *> &referencedVars) {
  // Notify referencing variables (just like in bound case) that this
  // type variable has been modified.
  notifyReferencingVars();

  // TODO: This might be an overkill but it's (currently)
  // the simplest way to reliably ensure that all of the
  // no longer related constraints have been retracted.
  for (auto *referencedVar : referencedVars) {
    auto &node = CG[referencedVar];
    if (node.forRepresentativeVar())
      node.resetBindingSet();
  }
}

void ConstraintGraphNode::resetBindingSet() {
  assert(forRepresentativeVar());

  Bindings.reset();

  auto &bindings = getCurrentBindings();
  for (auto *constraint : CG.gatherConstraints(
           TypeVar, ConstraintGraph::GatheringKind::EquivalenceClass)) {
    bindings.infer(constraint);
  }
}

#pragma mark Graph scope management
ConstraintGraphScope::ConstraintGraphScope(ConstraintGraph &CG)
  : CG(CG), ParentScope(CG.ActiveScope), NumChanges(CG.Changes.size())
{
  CG.ActiveScope = this;
}

ConstraintGraphScope::~ConstraintGraphScope() {
  // Don't attempt to rollback if constraint system ended up
  // in an invalid state.
  if (CG.CS.inInvalidState())
    return;

  // Pop changes off the stack until we hit the change could we had prior to
  // introducing this scope.
  assert(CG.Changes.size() >= NumChanges && "Scope stack corrupted");
  while (CG.Changes.size() > NumChanges) {
    CG.Changes.back().undo(CG);
    CG.Changes.pop_back();
  }

  // The active scope is now the parent scope.
  CG.ActiveScope = ParentScope;
}

ConstraintGraph::Change
ConstraintGraph::Change::addedTypeVariable(TypeVariableType *typeVar) {
  Change result;
  result.Kind = ChangeKind::AddedTypeVariable;
  result.TypeVar = typeVar;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::addedConstraint(Constraint *constraint) {
  Change result;
  result.Kind = ChangeKind::AddedConstraint;
  result.TheConstraint = constraint;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::removedConstraint(Constraint *constraint) {
  Change result;
  result.Kind = ChangeKind::RemovedConstraint;
  result.TheConstraint = constraint;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::extendedEquivalenceClass(TypeVariableType *typeVar,
                                                  unsigned prevSize) {
  Change result;
  result.Kind = ChangeKind::ExtendedEquivalenceClass;
  result.EquivClass.TypeVar = typeVar;
  result.EquivClass.PrevSize = prevSize;
  return result;
}

ConstraintGraph::Change
ConstraintGraph::Change::boundTypeVariable(TypeVariableType *typeVar,
                                           Type fixed) {
  Change result;
  result.Kind = ChangeKind::BoundTypeVariable;
  result.Binding.TypeVar = typeVar;
  result.Binding.FixedType = fixed.getPointer();
  return result;
}

void ConstraintGraph::Change::undo(ConstraintGraph &cg) {
  /// Temporarily change the active scope to null, so we don't record
  /// any changes made while performing the undo operation.
  llvm::SaveAndRestore<ConstraintGraphScope *> prevActiveScope(cg.ActiveScope,
                                                               nullptr);

  switch (Kind) {
  case ChangeKind::AddedTypeVariable:
    cg.removeNode(TypeVar);
    break;

  case ChangeKind::AddedConstraint:
    cg.removeConstraint(TheConstraint);
    break;

  case ChangeKind::RemovedConstraint:
    cg.addConstraint(TheConstraint);
    break;

  case ChangeKind::ExtendedEquivalenceClass: {
    auto &node = cg[EquivClass.TypeVar];
    node.truncateEquivalenceClass(EquivClass.PrevSize);
    break;
   }

  case ChangeKind::BoundTypeVariable:
    cg.unbindTypeVariable(Binding.TypeVar, Binding.FixedType);
    break;
  }
}

#pragma mark Graph mutation

void ConstraintGraph::removeNode(TypeVariableType *typeVar) {
  // Remove this node.
  auto &impl = typeVar->getImpl();
  unsigned index = impl.getGraphIndex();
  delete impl.getGraphNode();
  impl.setGraphNode(nullptr);

  // Remove this type variable from the list.
  unsigned lastIndex = TypeVariables.size()-1;
  if (index < lastIndex)
    TypeVariables[index] = TypeVariables[lastIndex];
  TypeVariables.pop_back();
}

void ConstraintGraph::addConstraint(Constraint *constraint) {
  // For the nodes corresponding to each type variable...
  auto referencedTypeVars = constraint->getTypeVariables();
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    auto &node = (*this)[typeVar];

    // Note the constraint within the node for that type variable.
    node.addConstraint(constraint);
  }

  // If the constraint doesn't reference any type variables, it's orphaned;
  // track it as such.
  if (referencedTypeVars.empty()) {
    OrphanedConstraints.push_back(constraint);
  }

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::addedConstraint(constraint));
}

void ConstraintGraph::removeConstraint(Constraint *constraint) {
  // For the nodes corresponding to each type variable...
  auto referencedTypeVars = constraint->getTypeVariables();
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    auto &node = (*this)[typeVar];

    // Remove the constraint.
    node.removeConstraint(constraint);
  }

  // If this is an orphaned constraint, remove it from the list.
  if (referencedTypeVars.empty()) {
    auto known = std::find(OrphanedConstraints.begin(),
                           OrphanedConstraints.end(),
                           constraint);
    assert(known != OrphanedConstraints.end() && "missing orphaned constraint");
    *known = OrphanedConstraints.back();
    OrphanedConstraints.pop_back();
  }

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::removedConstraint(constraint));
}

void ConstraintGraph::mergeNodes(TypeVariableType *typeVar1, 
                                 TypeVariableType *typeVar2) {
  assert(CS.getRepresentative(typeVar1) == CS.getRepresentative(typeVar2) &&
         "type representatives don't match");
  
  // Retrieve the node for the representative that we're merging into.
  auto typeVarRep = CS.getRepresentative(typeVar1);
  auto &repNode = (*this)[typeVarRep];

  // Retrieve the node for the non-representative.
  assert((typeVar1 == typeVarRep || typeVar2 == typeVarRep) &&
         "neither type variable is the new representative?");
  auto typeVarNonRep = typeVar1 == typeVarRep? typeVar2 : typeVar1;

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::extendedEquivalenceClass(
                        typeVarRep,
                        repNode.getEquivalenceClass().size()));

  // Merge equivalence class from the non-representative type variable.
  auto &nonRepNode = (*this)[typeVarNonRep];
  repNode.addToEquivalenceClass(nonRepNode.getEquivalenceClassUnsafe());
}

void ConstraintGraph::bindTypeVariable(TypeVariableType *typeVar, Type fixed) {
  assert(!fixed->is<TypeVariableType>() &&
         "Cannot bind to type variable; merge equivalence classes instead");

  // Record the change, if there are active scopes.
  if (ActiveScope)
    Changes.push_back(Change::boundTypeVariable(typeVar, fixed));

  auto &node = (*this)[typeVar];

  llvm::SmallPtrSet<TypeVariableType *, 4> referencedVars;
  fixed->getTypeVariables(referencedVars);

  for (auto otherTypeVar : referencedVars) {
    if (typeVar == otherTypeVar)
      continue;

    auto &otherNode = (*this)[otherTypeVar];

    otherNode.addReferencedBy(typeVar);
    node.addReferencedVar(otherTypeVar);
  }
}

void ConstraintGraph::unbindTypeVariable(TypeVariableType *typeVar, Type fixed) {
  auto &node = (*this)[typeVar];

  llvm::SmallPtrSet<TypeVariableType *, 4> referencedVars;
  fixed->getTypeVariables(referencedVars);

  for (auto otherTypeVar : referencedVars) {
    auto &otherNode = (*this)[otherTypeVar];

    otherNode.removeReferencedBy(typeVar);
    node.removeReference(otherTypeVar);
  }

  node.retractFromInference(fixed, referencedVars);
}

#pragma mark Algorithms

/// Perform a depth-first search.
///
/// \param cg The constraint graph.
/// \param typeVar The type variable we're searching from.
/// \param preVisitNode Called before traversing a node. Must return \c
/// false when the node has already been visited.
/// \param visitConstraint Called before considering a constraint. If it
/// returns \c false, that constraint will be skipped.
/// \param visitedConstraints Set of already-visited constraints, used
/// internally to avoid duplicated work.
static void depthFirstSearch(
    ConstraintGraph &cg,
    TypeVariableType *typeVar,
    llvm::function_ref<bool(TypeVariableType *)> preVisitNode,
    llvm::function_ref<bool(Constraint *)> visitConstraint,
    llvm::SmallPtrSet<Constraint *, 8> &visitedConstraints) {
  // Visit this node. If we've already seen it, bail out.
  if (!preVisitNode(typeVar))
    return;

  // Local function to visit adjacent type variables.
  auto visitAdjacencies = [&](ArrayRef<TypeVariableType *> adjTypeVars) {
    for (auto adj : adjTypeVars) {
      if (adj == typeVar)
        continue;

      // Recurse into this node.
      depthFirstSearch(cg, adj, preVisitNode, visitConstraint,
                       visitedConstraints);
    }
  };

  // Walk all of the constraints associated with this node to find related
  // nodes.
  auto &node = cg[typeVar];
  for (auto constraint : node.getConstraints()) {
    // If we've already seen this constraint, skip it.
    if (!visitedConstraints.insert(constraint).second)
      continue;

    if (visitConstraint(constraint))
      visitAdjacencies(constraint->getTypeVariables());
  }

  // Visit all of the other nodes in the equivalence class.
  auto repTypeVar = cg.getConstraintSystem().getRepresentative(typeVar);
  if (typeVar == repTypeVar) {
    // We are the representative, so visit all of the other type variables
    // in this equivalence class.
    visitAdjacencies(node.getEquivalenceClass());
  } else {
    // We are not the representative; visit the representative.
    visitAdjacencies(repTypeVar);
  }

  // Walk any type variables related via fixed bindings.
  visitAdjacencies(node.getReferencedBy());
  visitAdjacencies(node.getReferencedVars());
}

llvm::TinyPtrVector<Constraint *> ConstraintGraph::gatherConstraints(
    TypeVariableType *typeVar, GatheringKind kind,
    llvm::function_ref<bool(Constraint *)> acceptConstraintFn) {
  llvm::TinyPtrVector<Constraint *> constraints;
  // Whether we should consider this constraint at all.
  auto shouldConsiderConstraint = [&](Constraint *constraint) {
    // For a one-way constraint, only consider it when the left-hand side of
    // the binding is one of the type variables currently under consideration,
    // as only such constraints need solving for this component. Note that we
    // don't perform any other filtering, as the constraint system should be
    // responsible for checking any other conditions.
    if (constraint->isOneWayConstraint()) {
      auto lhsTypeVar = constraint->getFirstType()->castTo<TypeVariableType>();
      return CS.isActiveTypeVariable(lhsTypeVar);
    }

    return true;
  };

  auto acceptConstraint = [&](Constraint *constraint) {
    return shouldConsiderConstraint(constraint) &&
        acceptConstraintFn(constraint);
  };

  llvm::SmallPtrSet<TypeVariableType *, 4> typeVars;
  llvm::SmallPtrSet<Constraint *, 8> visitedConstraints;

  if (kind == GatheringKind::AllMentions) {
    // If we've been asked for "all mentions" of a type variable, search for
    // constraints involving both it and its fixed bindings.
    depthFirstSearch(
        *this, typeVar,
        [&](TypeVariableType *typeVar) {
          return typeVars.insert(typeVar).second;
        },
        [&](Constraint *constraint) {
          if (acceptConstraint(constraint))
            constraints.push_back(constraint);

          // Don't recurse into the constraint's type variables.
          return false;
        },
        visitedConstraints);
    return constraints;
  }

  // Otherwise only search in the type var's equivalence class and immediate
  // fixed bindings.

  // Local function to add constraints.
  auto addTypeVarConstraints = [&](TypeVariableType *adjTypeVar) {
    if (!typeVars.insert(adjTypeVar).second)
      return;

    for (auto constraint : (*this)[adjTypeVar].getConstraints()) {
      if (visitedConstraints.insert(constraint).second &&
          acceptConstraint(constraint))
        constraints.push_back(constraint);
    }
  };

  auto &reprNode = (*this)[CS.getRepresentative(typeVar)];
  auto equivClass = reprNode.getEquivalenceClass();
  for (auto typeVar : equivClass) {
    if (!typeVars.insert(typeVar).second)
      continue;

    auto &node = (*this)[typeVar];

    for (auto constraint : node.getConstraints()) {
      if (visitedConstraints.insert(constraint).second &&
          acceptConstraint(constraint))
        constraints.push_back(constraint);
    }

    for (auto adjTypeVar : node.getReferencedBy()) {
      addTypeVarConstraints(adjTypeVar);
    }

    for (auto adjTypeVar : node.getReferencedVars()) {
      addTypeVarConstraints(adjTypeVar);
    }
  }

  return constraints;
}

namespace {
  /// A union-find connected components algorithm used to find the connected
  /// components within a constraint graph.
  class ConnectedComponents {
    ConstraintGraph &cg;
    ArrayRef<TypeVariableType *> typeVars;

    /// A mapping from each type variable to its representative in a union-find
    /// data structure, excluding entries where the type variable is its own
    /// representative.
    mutable llvm::SmallDenseMap<TypeVariableType *, TypeVariableType *>
        representatives;

    /// The complete set of constraints that were visited while computing
    /// connected components.
    llvm::SmallPtrSet<Constraint *, 8> visitedConstraints;

    /// Describes the one-way incoming and outcoming adjacencies of
    /// a component within the directed graph of one-way constraints.
    struct OneWayComponent {
      /// The (uniqued) set of type variable representatives to which this
      /// component has an outgoing edge.
      TinyPtrVector<TypeVariableType *> outAdjacencies;

      /// The (uniqued) set of type variable representatives from which this
      /// component has an incoming edge.
      TinyPtrVector<TypeVariableType *> inAdjacencies;
    };

    // Adjacency list representation of the directed graph of edges for
    // one-way constraints, using type variable representatives as the
    // nodes.
    llvm::SmallDenseMap<TypeVariableType *, OneWayComponent> oneWayDigraph;

  public:
    using Component = ConstraintGraph::Component;

    /// Compute connected components for the given set of type variables
    /// in the constraint graph.
    ConnectedComponents(ConstraintGraph &cg,
                        ArrayRef<TypeVariableType *> typeVars)
        : cg(cg), typeVars(typeVars)
    {
      auto oneWayConstraints = connectedComponents();

      // If there were no one-way constraints, we're done.
      if (oneWayConstraints.empty())
        return;

      // Build the directed one-way constraint graph.
      buildOneWayConstraintGraph(oneWayConstraints);
    }

    /// Retrieve the set of components.
    SmallVector<Component, 1> getComponents() const {
      // Figure out which components have unbound type variables and/or
      // constraints. These are the only components we want to report.
      llvm::SmallDenseSet<TypeVariableType *> validComponents;
      auto &cs = cg.getConstraintSystem();
      for (auto typeVar : typeVars) {
        // If this type variable has a fixed type, skip it.
        if (cs.getFixedType(typeVar))
          continue;

        auto rep = findRepresentative(typeVar);
        validComponents.insert(rep);
      }

      for (auto &constraint : cs.getConstraints()) {
        for (auto typeVar : constraint.getTypeVariables()) {
          auto rep = findRepresentative(typeVar);
          validComponents.insert(rep);
        }
      }

      // Capture the type variables of each component.
      llvm::SmallDenseMap<TypeVariableType *, Component> components;
      SmallVector<TypeVariableType *, 4> representativeTypeVars;
      for (auto typeVar : typeVars) {
        // Find the representative. If we aren't creating a type variable
        // for this component, skip it.
        auto rep = findRepresentative(typeVar);
        if (validComponents.count(rep) == 0)
          continue;

        // If this type variable is the representative, add it to the list of
        // representatives.
        if (rep == typeVar) {
          representativeTypeVars.push_back(rep);
        }

        // Record this type variable in the set of type variables for its
        // component.
        auto &component = components.insert(
            {rep, Component(components.size())}).first->second;
        component.typeVars.push_back(typeVar);
      }

      // Retrieve the component for the given representative type variable.
      auto getComponent = [&](TypeVariableType *rep) -> Component& {
        auto component = components.find(rep);
        assert(component != components.end());
        return component->second;
      };

      // Assign each constraint to its appropriate component.
      // Note: we use the inactive constraints so that we maintain the
      // order of constraints when we re-introduce them.
      for (auto &constraint : cs.getConstraints()) {
        auto constraintTypeVars = constraint.getTypeVariables();
        if (constraintTypeVars.empty())
          continue;

        TypeVariableType *typeVar;
        if (constraint.isOneWayConstraint()) {
          // For one-way constraints, associate the constraint with the
          // left-hand type variable.
          typeVar = constraint.getFirstType()->castTo<TypeVariableType>();
        } else {
          typeVar = constraintTypeVars.front();
        }

        auto rep = findRepresentative(typeVar);
        getComponent(rep).addConstraint(&constraint);
      }

      // If we have any one-way constraint information, compute the ordering
      // of representative type variables needed to respect one-way
      // constraints while solving.
      if (!oneWayDigraph.empty()) {
        // Sort the representative type variables based on the disjunction
        // count, so
        std::sort(representativeTypeVars.begin(), representativeTypeVars.end(),
                  [&](TypeVariableType *lhs, TypeVariableType *rhs) {
                    return getComponent(lhs).getNumDisjunctions() >
                        getComponent(rhs).getNumDisjunctions();
                  });
        
        representativeTypeVars =
            computeOneWayComponentOrdering(representativeTypeVars,
                                           validComponents);

        // Fill in one-way dependency information for all of the components.
        for (auto typeVar : representativeTypeVars) {
          auto knownOneWayComponent = oneWayDigraph.find(typeVar);
          if (knownOneWayComponent == oneWayDigraph.end())
            continue;

          auto &oneWayComponent = knownOneWayComponent->second;
          auto &component = getComponent(typeVar);
          for (auto inAdj : oneWayComponent.inAdjacencies) {
            if (validComponents.count(inAdj) == 0)
              continue;

            component.recordDependency(getComponent(inAdj));
          }
        }
      }

      // Flatten the set of components.
      SmallVector<Component, 1> flatComponents;
      flatComponents.reserve(
          representativeTypeVars.size() + cg.getOrphanedConstraints().size());
      for (auto rep: representativeTypeVars) {
        assert(components.count(rep) == 1);
        flatComponents.push_back(std::move(getComponent(rep)));
      }

      // Gather orphaned constraints; each gets its own component.
      for (auto orphaned : cg.getOrphanedConstraints()) {
        flatComponents.push_back(Component(flatComponents.size()));
        flatComponents.back().addConstraint(orphaned);
      }

      // Create component ordering based on the information associated
      // with constraints in each step - e.g. number of disjunctions,
      // since components are going to be executed in LIFO order, we'd
      // want to have smaller/faster components at the back of the list.
      // When there are one-way constraints, we can't reorder them, so only
      // sort the orphaned constraints at the back. In the absence of
      // one-way constraints, sort everything.
      if (components.size() > 1) {
        auto sortStart = oneWayDigraph.empty()
            ? flatComponents.begin()
            : flatComponents.end() - cg.getOrphanedConstraints().size();
        std::sort(sortStart, flatComponents.end(),
                  [&](const Component &lhs, const Component &rhs) {
                    return lhs.getNumDisjunctions() > rhs.getNumDisjunctions();
                  });
      }

      return flatComponents;
    }

    /// Find the representative for the given type variable within the set
    /// of representatives in a union-find data structure.
    TypeVariableType *findRepresentative(TypeVariableType *typeVar) const {
      // If we don't have a record of this type variable, it is it's own
      // representative.
      auto known = representatives.find(typeVar);
      if (known == representatives.end() || known->second == typeVar)
        return typeVar;

      // Find the representative of the parent.
      auto parent = known->second;
      auto rep = findRepresentative(parent);
      representatives[typeVar] = rep;

      return rep;
    }

  private:
    /// Perform the union of two type variables in a union-find data structure
    /// used for connected components.
    ///
    /// \returns true if the two components were separate and have now been
    /// joined, \c false if they were already in the same set.
    bool unionSets(TypeVariableType *typeVar1, TypeVariableType *typeVar2) {
      auto rep1 = findRepresentative(typeVar1);
      auto rep2 = findRepresentative(typeVar2);
      if (rep1 == rep2)
        return false;

      // Reparent the type variable with the higher ID. The actual choice doesn't
      // matter, but this makes debugging easier.
      if (rep1->getID() < rep2->getID())
        representatives[rep2] = rep1;
      else
        representatives[rep1] = rep2;
      return true;
    }

    /// Perform the connected components algorithm, skipping one-way
    /// constraints.
    ///
    /// \returns the set of one-way constraints that were skipped.
    TinyPtrVector<Constraint *> connectedComponents() {
      TinyPtrVector<Constraint *> oneWayConstraints;

      // Perform a depth-first search from each type variable to identify
      // what component it is in.
      for (auto typeVar : typeVars) {
        // If we've already assigned a representative to this type variable,
        // we're done.
        if (representatives.count(typeVar) > 0)
          continue;

        // Perform a depth-first search to mark those type variables that are
        // in the same component as this type variable.
        depthFirstSearch(
            cg, typeVar,
            [&](TypeVariableType *found) {
              // If we have already seen this node, we're done.
              auto inserted = representatives.insert({found, typeVar});
              assert((inserted.second || inserted.first->second == typeVar) &&
                     "Wrong component?");

              return inserted.second;
            },
            [&](Constraint *constraint) {
              // Record and skip one-way constraints.
              if (constraint->isOneWayConstraint()) {
                oneWayConstraints.push_back(constraint);
                return false;
              }

              return true;
            },
            visitedConstraints);
      }

      return oneWayConstraints;
    }

    /// Insert the given type variable into the given vector if it isn't
    /// already present.
    static void insertIfUnique(TinyPtrVector<TypeVariableType *> &vector,
                               TypeVariableType *typeVar) {
      if (std::find(vector.begin(), vector.end(), typeVar) == vector.end())
        vector.push_back(typeVar);
    }

    /// Retrieve the (uniqued) set of type variable representations that occur
    /// within the given type.
    TinyPtrVector<TypeVariableType *>
    getRepresentativesInType(Type type) const {
      TinyPtrVector<TypeVariableType *> results;

      SmallPtrSet<TypeVariableType *, 2> typeVars;
      type->getTypeVariables(typeVars);
      for (auto typeVar : typeVars) {
        auto rep = findRepresentative(typeVar);
        insertIfUnique(results, rep);
      }

      return results;
    }

    /// Add all of the one-way constraints to the one-way digraph
    void addOneWayConstraintEdges(ArrayRef<Constraint *> oneWayConstraints) {
      for (auto constraint : oneWayConstraints) {
        auto lhsTypeReps =
            getRepresentativesInType(constraint->getFirstType());
        auto rhsTypeReps =
            getRepresentativesInType(constraint->getSecondType());

        // Add an edge from the type representatives on the right-hand side
        // of the one-way constraint to the type representatives on the
        // left-hand side, because the right-hand type variables need to
        // be solved before the left-hand type variables.
        for (auto lhsTypeRep : lhsTypeReps) {
          for (auto rhsTypeRep : rhsTypeReps) {
            if (lhsTypeRep == rhsTypeRep)
              continue;

            insertIfUnique(oneWayDigraph[rhsTypeRep].outAdjacencies,lhsTypeRep);
            insertIfUnique(oneWayDigraph[lhsTypeRep].inAdjacencies,rhsTypeRep);
          }
        }
      }
    }

    using TypeVariablePair = std::pair<TypeVariableType *, TypeVariableType *>;

    /// Build the directed graph of one-way constraints among components.
    void buildOneWayConstraintGraph(ArrayRef<Constraint *> oneWayConstraints) {
      auto &cs = cg.getConstraintSystem();
      auto &ctx = cs.getASTContext();
      bool contractedCycle = false;
      do {
        // Construct the one-way digraph from scratch.
        oneWayDigraph.clear();
        addOneWayConstraintEdges(oneWayConstraints);

        // Minimize the in-adjacencies, detecting cycles along the way.
        SmallVector<TypeVariablePair, 4> cycleEdges;
        removeIndirectOneWayInAdjacencies(cycleEdges);

        // For any contractions we need to perform due to cycles, perform a
        // union the connected components based on the type variable pairs.
        contractedCycle = false;
        for (const auto &edge : cycleEdges) {
          if (unionSets(edge.first, edge.second)) {
            if (cs.isDebugMode()) {
              auto &log = llvm::errs();
              if (cs.solverState)
                log.indent(cs.solverState->getCurrentIndent());

              log << "Collapsing one-way components for $T"
                  << edge.first->getID() << " and $T" << edge.second->getID()
                  << " due to cycle.\n";
            }

            if (ctx.Stats) {
              ++ctx.Stats->getFrontendCounters()
                  .NumCyclicOneWayComponentsCollapsed;
            }

            contractedCycle = true;
          }
        }
      } while (contractedCycle);
    }

    /// Perform a depth-first search to produce a from the given type variable,
    /// notifying the function object.
    ///
    /// \param getAdjacencies Called to retrieve the set of type variables
    /// that are adjacent to the given type variable.
    ///
    /// \param preVisit Called before visiting the adjacencies of the given
    /// type variable. When it returns \c true, the adjacencies of this type
    /// variable will be visited. When \c false, the adjacencies will not be
    /// visited and \c postVisit will not be called.
    ///
    /// \param postVisit Called after visiting the adjacencies of the given
    /// type variable.
    static void postorderDepthFirstSearchRec(
        TypeVariableType *typeVar,
        llvm::function_ref<
          ArrayRef<TypeVariableType *>(TypeVariableType *)> getAdjacencies,
        llvm::function_ref<bool(TypeVariableType *)> preVisit,
        llvm::function_ref<void(TypeVariableType *)> postVisit) {
      if (!preVisit(typeVar))
        return;

      for (auto adj : getAdjacencies(typeVar)) {
        postorderDepthFirstSearchRec(adj, getAdjacencies, preVisit, postVisit);
      }

      postVisit(typeVar);
    }

    /// Minimize the incoming adjacencies for one of the nodes in the one-way
    /// directed graph by eliminating any in-adjacencies that can also be
    /// found indirectly.
    void removeIndirectOneWayInAdjacencies(
        TypeVariableType *typeVar,
        OneWayComponent &component,
        SmallVectorImpl<TypeVariablePair> &cycleEdges) {
      // Perform a depth-first search from each of the in adjacencies to
      // this type variable, traversing each of the one-way edges backwards
      // to find all of the components whose type variables must be
      // bound before this component can be solved.
      SmallPtrSet<TypeVariableType *, 4> visited;
      SmallPtrSet<TypeVariableType *, 4> indirectlyReachable;
      SmallVector<TypeVariableType *, 4> currentPath;
      for (auto inAdj : component.inAdjacencies) {
        postorderDepthFirstSearchRec(
            inAdj,
            [&](TypeVariableType *typeVar) -> ArrayRef<TypeVariableType *> {
              // Traverse the outgoing adjacencies for the subcomponent
              auto oneWayComponent = oneWayDigraph.find(typeVar);
              if (oneWayComponent == oneWayDigraph.end()) {
                return { };
              }

              return oneWayComponent->second.inAdjacencies;
            },
            [&](TypeVariableType *typeVar) {
              // If we haven't seen this type variable yet, add it to the
              // path.
              if (visited.insert(typeVar).second) {
                currentPath.push_back(typeVar);
                return true;
              }

              // Add edges between this type variable and every other type
              // variable in the path.
              for (auto otherTypeVar : llvm::reverse(currentPath)) {
                // When we run into our own type variable, we're done.
                if (otherTypeVar == typeVar)
                  break;

                cycleEdges.push_back({typeVar, otherTypeVar});
              }

              return false;
            },
            [&](TypeVariableType *dependsOn) {
              // Remove this type variable from the path.
              assert(currentPath.back() == dependsOn);
              currentPath.pop_back();

              // Don't record dependency on ourselves.
              if (dependsOn == inAdj)
                return;

              indirectlyReachable.insert(dependsOn);
            });

        // Remove any in-adjacency of this component that is indirectly
        // reachable.
        component.inAdjacencies.erase(
            std::remove_if(component.inAdjacencies.begin(),
                           component.inAdjacencies.end(),
                           [&](TypeVariableType *inAdj) {
                             return indirectlyReachable.count(inAdj) > 0;
                           }),
            component.inAdjacencies.end());
      }
    }

    /// Minimize the incoming adjacencies for all of the nodes in the one-way
    /// directed graph by eliminating any in-adjacencies that can also be
    /// found indirectly.
    void removeIndirectOneWayInAdjacencies(
        SmallVectorImpl<TypeVariablePair> &cycleEdges)  {
      for (auto &oneWayEntry : oneWayDigraph) {
        auto typeVar = oneWayEntry.first;
        auto &component = oneWayEntry.second;
        removeIndirectOneWayInAdjacencies(typeVar, component, cycleEdges);
      }
    }

    /// Compute the order in which the components should be visited to respect
    /// one-way constraints.
    ///
    /// \param representativeTypeVars the set of type variables that
    /// represent the components, in a preferred ordering that does not
    /// account for one-way constraints.
    /// \returns the set of type variables that represent the components, in
    /// an ordering that ensures that components containing type variables
    /// that occur on the left-hand side of a one-way constraint will be
    /// solved after the components for type variables on the right-hand
    /// side of that constraint.
    SmallVector<TypeVariableType *, 4> computeOneWayComponentOrdering(
        ArrayRef<TypeVariableType *> representativeTypeVars,
        llvm::SmallDenseSet<TypeVariableType *> &validComponents) const {
      SmallVector<TypeVariableType *, 4> orderedReps;
      orderedReps.reserve(representativeTypeVars.size());
      SmallPtrSet<TypeVariableType *, 4> visited;
      for (auto rep : llvm::reverse(representativeTypeVars)) {
        // Perform a postorder depth-first search through the one-way digraph,
        // starting at this representative, to establish the dependency
        // ordering amongst components that are reachable
        // to establish the dependency ordering for the representative type
        // variables.
        postorderDepthFirstSearchRec(
            rep,
            [&](TypeVariableType *typeVar) -> ArrayRef<TypeVariableType *> {
              // Traverse the outgoing adjacencies for the subcomponent
              assert(typeVar == findRepresentative(typeVar));
              auto oneWayComponent = oneWayDigraph.find(typeVar);
              if (oneWayComponent == oneWayDigraph.end()) {
                return { };
              }

              return oneWayComponent->second.outAdjacencies;
            },
            [&](TypeVariableType *typeVar) {
              return visited.insert(typeVar).second;
            },
            [&](TypeVariableType *typeVar) {
              // Record this type variable, if it's one of the representative
              // type variables.
              if (validComponents.count(typeVar) > 0)
                orderedReps.push_back(typeVar);
            });
      }

      assert(orderedReps.size() == representativeTypeVars.size());
      return orderedReps;
    }
  };
}

void ConstraintGraph::Component::addConstraint(Constraint *constraint) {
  if (constraint->getKind() == ConstraintKind::Disjunction)
    ++numDisjunctions;

  constraints.push_back(constraint);
}

void ConstraintGraph::Component::recordDependency(const Component &component) {
  dependencies.push_back(component.solutionIndex);
}

SmallVector<ConstraintGraph::Component, 1>
ConstraintGraph::computeConnectedComponents(
           ArrayRef<TypeVariableType *> typeVars) {
  // Perform connected components via a union-find algorithm on all of the
  // constraints adjacent to these type variables.
  ConnectedComponents cc(*this, typeVars);
  return cc.getComponents();
}

bool ConstraintGraph::contractEdges() {
  // Current constraint system doesn't have any closure expressions
  // associated with it so there is nothing to here.
  if (CS.ClosureTypes.empty())
    return false;

  // For a given constraint kind, decide if we should attempt to eliminate its
  // edge in the graph.
  auto shouldContractEdge = [](ConstraintKind kind) {
    switch (kind) {
    case ConstraintKind::BindParam:
      return true;

    default:
      return false;
    }
  };

  SmallVector<Constraint *, 16> constraints;
  for (const auto &closure : CS.ClosureTypes) {
    for (const auto &param : closure.second->getParams()) {
      auto paramTy = param.getPlainType()->getAs<TypeVariableType>();
      if (!paramTy)
        continue;

      // This closure is not currently in scope.
      if (!CS.TypeVariables.count(paramTy))
        break;

      // Nothing to contract here since outside parameter
      // is already bound to a concrete type.
      if (CS.getFixedType(paramTy))
        continue;

      for (auto *constraint : (*this)[paramTy].getConstraints()) {
        // Track how many constraints did contraction algorithm iterated over.
        incrementConstraintsPerContractionCounter();

        if (shouldContractEdge(constraint->getKind()))
          constraints.push_back(constraint);
      }
    }
  }

  bool didContractEdges = false;
  for (auto *constraint : constraints) {
    auto kind = constraint->getKind();

    // Contract binding edges between type variables.
    assert(shouldContractEdge(kind));

    auto t1 = constraint->getFirstType()->getDesugaredType();
    auto t2 = constraint->getSecondType()->getDesugaredType();

    auto tyvar1 = t1->getAs<TypeVariableType>();
    auto tyvar2 = t2->getAs<TypeVariableType>();

    if (!(tyvar1 && tyvar2))
      continue;

    // If the argument is allowed to bind to `inout`, in general,
    // it's invalid to contract the edge between argument and parameter,
    // but if we can prove that there are no possible bindings
    // which result in attempt to bind `inout` type to argument
    // type variable, we should go ahead and allow (temporary)
    // contraction, because that greatly helps with performance.
    // Such action is valid because argument type variable can
    // only get its bindings from related overload, which gives
    // us enough information to decided on l-valueness.
    if (tyvar1->getImpl().canBindToInOut()) {
      bool isNotContractable = true;
      if (auto bindings = CS.getBindingsFor(tyvar1)) {
        // Holes can't be contracted.
        if (bindings.isHole())
          continue;

        for (auto &binding : bindings.Bindings) {
          auto type = binding.BindingType;
          isNotContractable = type.findIf([&](Type nestedType) -> bool {
            if (auto tv = nestedType->getAs<TypeVariableType>()) {
              if (tv->getImpl().canBindToInOut())
                return true;
            }

            return nestedType->is<InOutType>();
          });

          // If there is at least one non-contractable binding, let's
          // not risk contracting this edge.
          if (isNotContractable)
            break;
        }
      }

      if (isNotContractable)
        continue;
    }

    auto rep1 = CS.getRepresentative(tyvar1);
    auto rep2 = CS.getRepresentative(tyvar2);

    if (CS.isDebugMode()) {
      auto indent = CS.solverState ? CS.solverState->getCurrentIndent() : 0;
      auto &log = llvm::errs().indent(indent);

      log << "Contracting constraint ";
      constraint->print(log.indent(indent), &CS.getASTContext().SourceMgr,
                        indent);
      log << "\n";
    }

    // Merge the edges and retire the constraint.
    CS.retireConstraint(constraint);
    if (rep1 != rep2)
      CS.mergeEquivalenceClasses(rep1, rep2, /*updateWorkList*/ false);
    didContractEdges = true;
  }
  return didContractEdges;
}

void ConstraintGraph::optimize() {
  // Merge equivalence classes until a fixed point is reached.
  while (contractEdges()) {}
}

void ConstraintGraph::incrementConstraintsPerContractionCounter() {
  SWIFT_FUNC_STAT;
  auto &context = CS.getASTContext();
  if (auto *Stats = context.Stats) {
    ++Stats->getFrontendCounters()
        .NumConstraintsConsideredForEdgeContraction;
  }
}

#pragma mark Debugging output

void ConstraintGraphNode::print(llvm::raw_ostream &out, unsigned indent,
                                PrintOptions PO) const {
  out.indent(indent);
  Type(TypeVar).print(out, PO);
  out << ":\n";

  // Print constraints.
  if (!Constraints.empty()) {
    out.indent(indent + 2);
    out << "Constraints:\n";
    SmallVector<Constraint *, 4> sortedConstraints(Constraints.begin(),
                                                   Constraints.end());
    std::sort(sortedConstraints.begin(), sortedConstraints.end());

    for (auto constraint : sortedConstraints) {
      out.indent(indent + 4);
      constraint->print(out, &TypeVar->getASTContext().SourceMgr, indent + 4);
      out << "\n";
    }
  }

  auto printVarList = [&](ArrayRef<TypeVariableType *> typeVars) {
    SmallVector<TypeVariableType *, 4> sorted(typeVars.begin(), typeVars.end());
    std::sort(sorted.begin(), sorted.end(),
              [&](TypeVariableType *typeVar1, TypeVariableType *typeVar2) {
                return typeVar1->getID() < typeVar2->getID();
              });

    interleave(
        sorted,
        [&](TypeVariableType *typeVar) { out << typeVar->getString(PO); },
        [&out] { out << ", "; });
  };

  // Print fixed bindings.
  if (!ReferencedBy.empty()) {
    out.indent(indent + 2);
    out << "Referenced By: ";
    printVarList(getReferencedBy());
    out << "\n";
  }

  if (!References.empty()) {
    out.indent(indent + 2);
    out << "References: ";
    printVarList(getReferencedVars());
    out << "\n";
  }

  // Print equivalence class.
  if (forRepresentativeVar() && EquivalenceClass.size() > 1) {
    out.indent(indent + 2);
    out << "Equivalence class:";
    for (unsigned i = 1, n = EquivalenceClass.size(); i != n; ++i) {
      out << ' ';
      EquivalenceClass[i]->print(out, PO);
    }
    out << "\n";
  }
}

void ConstraintGraphNode::dump() const {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  print(llvm::dbgs(), 0, PO);
}

void ConstraintGraph::print(ArrayRef<TypeVariableType *> typeVars,
                            llvm::raw_ostream &out) {
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;

  for (auto typeVar : typeVars) {
    (*this)[typeVar].print(
        out, (CS.solverState ? CS.solverState->getCurrentIndent() : 0) + 2, PO);
    out << "\n";
  }
}

void ConstraintGraph::dump() {
  dump(llvm::dbgs());
}

void ConstraintGraph::dump(llvm::raw_ostream &out) {
  print(CS.getTypeVariables(), out);
}

void ConstraintGraph::dumpActiveScopeChanges(llvm::raw_ostream &out,
                                             unsigned indent) {
  if (Changes.empty())
    return;

  // Collect Changes for printing.
  std::map<TypeVariableType *, TypeBase *> tvWithboundTypes;
  std::vector<TypeVariableType *> addedTypeVars;
  std::vector<TypeVariableType *> equivTypeVars;
  std::set<Constraint *> addedConstraints;
  std::set<Constraint *> removedConstraints;
  for (unsigned int i = ActiveScope->getStartIdx(); i < Changes.size(); i++) {
    auto change = Changes[i];
    switch (change.Kind) {
    case ChangeKind::BoundTypeVariable:
      tvWithboundTypes.insert(std::pair<TypeVariableType *, TypeBase *>(
          change.Binding.TypeVar, change.Binding.FixedType));
      break;
    case ChangeKind::AddedTypeVariable:
      addedTypeVars.push_back(change.TypeVar);
      break;
    case ChangeKind::ExtendedEquivalenceClass:
      equivTypeVars.push_back(change.EquivClass.TypeVar);
      break;
    case ChangeKind::AddedConstraint:
      addedConstraints.insert(change.TheConstraint);
      break;
    case ChangeKind::RemovedConstraint:
      removedConstraints.insert(change.TheConstraint);
      break;
    }
  }

  // If there are any constraints that were both added and removed in this set
  // of Changes, remove them from both.
  std::set<Constraint *> intersects;
  set_intersection(addedConstraints.begin(), addedConstraints.end(),
                   removedConstraints.begin(), removedConstraints.end(),
                   std::inserter(intersects, intersects.begin()));
  llvm::set_subtract(addedConstraints, intersects);
  llvm::set_subtract(removedConstraints, intersects);

  // Print out Changes.
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  out.indent(indent);
  out << "(Changes:\n";
  if (!tvWithboundTypes.empty()) {
    out.indent(indent + 2);
    out << "(Newly Bound: \n";
    for (const auto &tvWithType : tvWithboundTypes) {
      out.indent(indent + 4);
      out << "> $T" << tvWithType.first->getImpl().getID() << " := ";
      tvWithType.second->print(out, PO);
      out << '\n';
    }
    out.indent(indent + 2);
    out << ")\n";
  }
  if (!addedTypeVars.empty()) {
    out.indent(indent + 2);
    auto heading = (addedTypeVars.size() > 1) ? "(New Type Variables: \n"
                                              : "(New Type Variable: \n";
    out << heading;
    for (const auto &typeVar : addedTypeVars) {
      out.indent(indent + 4);
      out << "> $T" << typeVar->getImpl().getID();
      out << '\n';
    }
    out.indent(indent + 2);
    out << ")\n";
  }
  if (!equivTypeVars.empty()) {
    out.indent(indent + 2);
    auto heading = (equivTypeVars.size() > 1) ? "(New Equivalences: \n"
                                              : "(New Equivalence: \n";
    out << heading;
    for (const auto &typeVar : equivTypeVars) {
      out.indent(indent + 4);
      out << "> $T" << typeVar->getImpl().getID();
      out << '\n';
    }
    out.indent(indent + 2);
    out << ")\n";
  }
  if (!addedConstraints.empty()) {
    out.indent(indent + 2);
    auto heading = (addedConstraints.size() > 1) ? "(Added Constraints: \n"
                                                 : "(Added Constraint: \n";
    out << heading;
    for (const auto &constraint : addedConstraints) {
      out.indent(indent + 4);
      out << "> ";
      constraint->print(out, &CS.getASTContext().SourceMgr, indent + 6);
      out << '\n';
    }
    out.indent(indent + 2);
    out << ")\n";
  }
  if (!removedConstraints.empty()) {
    out.indent(indent + 2);
    auto heading = (removedConstraints.size() > 1) ? "(Removed Constraints: \n"
                                                   : "(Removed Constraint: \n";
    out << heading;
    for (const auto &constraint : removedConstraints) {
      out.indent(indent + 4);
      out << "> ";
      constraint->print(out, &CS.getASTContext().SourceMgr, indent + 6);
      out << '\n';
    }
    out.indent(indent + 2);
    out << ")\n";
  }
  out.indent(indent);
  out << ")\n";
}

void ConstraintGraph::printConnectedComponents(
    ArrayRef<TypeVariableType *> typeVars,
    llvm::raw_ostream &out) {
  auto components = computeConnectedComponents(typeVars);
  PrintOptions PO;
  PO.PrintTypesForDebugging = true;
  for (const auto& component : components) {
    out.indent((CS.solverState ? CS.solverState->getCurrentIndent() : 0) + 2);
    out << component.solutionIndex << ": ";
    SWIFT_DEFER {
      out << '\n';
    };

    // Print all of the type variables in this connected component.
    interleave(component.typeVars,
               [&](TypeVariableType *typeVar) {
                 Type(typeVar).print(out, PO);
               },
               [&] {
                 out << ' ';
               });

    auto dependencies = component.getDependencies();
    if (dependencies.empty())
      continue;

    SmallVector<unsigned, 4> indices{dependencies.begin(), dependencies.end()};
    // Sort dependencies so output is stable.
    llvm::sort(indices);

    // Print all of the one-way components.
    out << " depends on ";
    llvm::interleave(
        indices, [&out](unsigned index) { out << index; },
        [&out] { out << ", "; });
  }
}

void ConstraintGraph::dumpConnectedComponents() {
  printConnectedComponents(CS.getTypeVariables(), llvm::dbgs());
}

#pragma mark Verification of graph invariants

/// Require that the given condition evaluate true.
///
/// If the condition is not true, complain about the problem and abort.
///
/// \param condition The actual Boolean condition.
///
/// \param complaint A string that describes the problem.
///
/// \param cg The constraint graph that failed verification.
///
/// \param node If non-null, the graph node that failed verification.
///
/// \param extraContext If provided, a function that will be called to
/// provide extra, contextual information about the failure.
static void _require(bool condition, const Twine &complaint,
                     ConstraintGraph &cg,
                     ConstraintGraphNode *node,
                     const std::function<void()> &extraContext = nullptr) {
  if (condition)
    return;

  // Complain
  llvm::dbgs() << "Constraint graph verification failed: " << complaint << '\n';
  if (extraContext)
    extraContext();

  // Print the graph.
  // FIXME: Highlight the offending node/constraint/etc.
  cg.dump(llvm::dbgs());

  abort();
}

/// Print a type variable value.
static void printValue(llvm::raw_ostream &os, TypeVariableType *typeVar) {
  typeVar->print(os);
}

/// Print a constraint value.
static void printValue(llvm::raw_ostream &os, Constraint *constraint) {
  constraint->print(os, nullptr);
}

/// Print an unsigned value.
static void printValue(llvm::raw_ostream &os, unsigned value) {
  os << value;
}

void ConstraintGraphNode::verify(ConstraintGraph &cg) {
#define require(condition, complaint) _require(condition, complaint, cg, this)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, cg, this, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, cg, this, [&] {         \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the constraint map/vector haven't gotten out of sync.
  requireSameValue(Constraints.size(), ConstraintIndex.size(),
                   "constraint vector and map have different sizes");
  for (auto info : ConstraintIndex) {
    require(info.second < Constraints.size(), "constraint index out-of-range");
    requireSameValue(info.first, Constraints[info.second],
                     "constraint map provides wrong index into vector");
  }
#undef requireSameValue
#undef requireWithContext
#undef require
}

void ConstraintGraph::verify() {
#define require(condition, complaint) \
  _require(condition, complaint, *this, nullptr)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, *this, nullptr, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, *this, nullptr, [&] {   \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the type variables are either representatives or represented
  // within their representative's equivalence class.
  // FIXME: Also check to make sure the equivalence classes aren't too large?
  for (auto typeVar : TypeVariables) {
    auto typeVarRep = CS.getRepresentative(typeVar);
    auto &repNode = (*this)[typeVarRep];
    if (typeVar != typeVarRep) {
      // This type variable should be in the equivalence class of its
      // representative.
      require(std::find(repNode.getEquivalenceClass().begin(),
                        repNode.getEquivalenceClass().end(),
                        typeVar) != repNode.getEquivalenceClass().end(),
              "type variable not present in its representative's equiv class");
    } else {
      // Each of the type variables in the same equivalence class as this type
      // should have this type variable as their representative.
      for (auto equiv : repNode.getEquivalenceClass()) {
        requireSameValue(
          typeVar, equiv->getImpl().getRepresentative(nullptr),
          "representative and an equivalent type variable's representative");
      }
    }
  }

  // Verify that our type variable map/vector are in sync.
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto typeVar = TypeVariables[i];
    auto &impl = typeVar->getImpl();
    requireSameValue(impl.getGraphIndex(), i, "wrong graph node index");
    require(impl.getGraphNode(), "null graph node");
  }

  // Verify consistency of all of the nodes in the graph.
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    auto typeVar = TypeVariables[i];
    auto &impl = typeVar->getImpl();
    impl.getGraphNode()->verify(*this);
  }

  // Collect all of the constraints known to the constraint graph.
  llvm::SmallPtrSet<Constraint *, 4> knownConstraints;
  for (auto typeVar : getTypeVariables()) {
    for (auto constraint : (*this)[typeVar].getConstraints())
      knownConstraints.insert(constraint);
  }

  // Verify that all of the constraints in the constraint system
  // are accounted for.
  for (auto &constraint : CS.getConstraints()) {
    // Check whether the constraint graph knows about this constraint.
    auto referencedTypeVars = constraint.getTypeVariables();
    requireWithContext((knownConstraints.count(&constraint) ||
                        referencedTypeVars.empty()),
                       "constraint graph doesn't know about constraint",
                       [&] {
                         llvm::dbgs() << "constraint = ";
                         printValue(llvm::dbgs(), &constraint);
                         llvm::dbgs() << "\n";
                       });

    // Make sure each of the type variables referenced knows about this
    // constraint.
    for (auto typeVar : referencedTypeVars) {
      auto nodePtr = typeVar->getImpl().getGraphNode();
      requireWithContext(nodePtr,
                         "type variable in constraint not known",
                         [&] {
                           llvm::dbgs() << "type variable = ";
                           printValue(llvm::dbgs(), typeVar);
                           llvm::dbgs() << ", constraint = ";
                           printValue(llvm::dbgs(), &constraint);
                           llvm::dbgs() << "\n";
                         });

      auto &node = *nodePtr;
      auto constraintPos = node.ConstraintIndex.find(&constraint);
      requireWithContext(constraintPos != node.ConstraintIndex.end(),
                         "type variable doesn't know about constraint",
                         [&] {
                           llvm::dbgs() << "type variable = ";
                           printValue(llvm::dbgs(), typeVar);
                           llvm::dbgs() << ", constraint = ";
                           printValue(llvm::dbgs(), &constraint);
                           llvm::dbgs() << "\n";
                         });
    }
  }

#undef requireSameValue
#undef requireWithContext
#undef require
}


