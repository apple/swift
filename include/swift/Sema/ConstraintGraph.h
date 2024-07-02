//===--- ConstraintGraph.h - Constraint Graph -------------------*- C++ -*-===//
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
// This file defines the \c ConstraintGraph class, which describes the
// relationships among the type variables within a constraint system.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_CONSTRAINT_GRAPH_H
#define SWIFT_SEMA_CONSTRAINT_GRAPH_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/Sema/CSBindings.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Compiler.h"
#include <functional>
#include <utility>

namespace swift {

class Type;
class TypeBase;
class TypeVariableType;

namespace constraints {

class Constraint;
class ConstraintGraph;
class ConstraintGraphScope;
class ConstraintSystem;
class TypeVariableBinding;

/// A single node in the constraint graph, which represents a type variable.
class ConstraintGraphNode {
public:
  explicit ConstraintGraphNode(ConstraintGraph &CG, TypeVariableType *typeVar)
      : CG(CG), TypeVar(typeVar) {}

  ConstraintGraphNode(const ConstraintGraphNode&) = delete;
  ConstraintGraphNode &operator=(const ConstraintGraphNode&) = delete;

  /// Retrieve the type variable this node represents.
  TypeVariableType *getTypeVariable() const { return TypeVar; }

  /// Retrieve the set of constraints that mention this type variable.
  ///
  /// These are the hyperedges of the graph, connecting this node to
  /// various other nodes.
  ArrayRef<Constraint *> getConstraints() const { return Constraints; }

  /// Retrieve the set of type variables that are adjacent due to fixed
  /// bindings.
  ArrayRef<TypeVariableType *> getReferencedVars() const {
    return References.getArrayRef();
  }

  ArrayRef<TypeVariableType *> getReferencedBy() const {
    return ReferencedBy.getArrayRef();
  }

  /// Retrieve all of the type variables in the same equivalence class
  /// as this type variable.
  ArrayRef<TypeVariableType *> getEquivalenceClass() const;

  inference::PotentialBindings &getCurrentBindings();

private:
  /// Determines whether the type variable associated with this node
  /// is a representative of an equivalence class.
  ///
  /// Note: The smallest equivalence class is of just one variable - itself.
  bool forRepresentativeVar() const;

  /// Retrieve all of the type variables in the same equivalence class
  /// as this type variable.
  ArrayRef<TypeVariableType *> getEquivalenceClassUnsafe() const;

  /// Add a constraint to the list of constraints.
  void addConstraint(Constraint *constraint, unsigned index);

  /// Remove a constraint from the list of constraints.
  ///
  /// Note that this only removes the constraint itself; it does not
  /// remove the corresponding adjacencies.
  unsigned removeConstraint(Constraint *constraint);

  /// Add the given type variables to this node's equivalence class.
  void addToEquivalenceClass(ArrayRef<TypeVariableType *> typeVars);

  /// Remove N last members from equivalence class of the current type variable.
  void truncateEquivalenceClass(unsigned prevSize);

  /// Add a type variable related to this type variable through fixed
  /// binding.
  void addReferencedVar(TypeVariableType *typeVar);

  /// Add a type variable referencing this type variable - this type
  /// variable occurs in fixed type of the given type variable.
  void addReferencedBy(TypeVariableType *typeVar);

  /// Remove a type variable referenced by this node through a fixed binding.
  void removeReference(TypeVariableType *typeVar);

  /// Remove a type variable which used to reference this type variable.
  void removeReferencedBy(TypeVariableType *typeVar);

  /// Binding Inference {

  /// Infer bindings from the given constraint and notify referenced variables
  /// about its arrival (if requested). This happens every time a new constraint
  /// gets added to a constraint graph node.
  void introduceToInference(Constraint *constraint);

  /// Forget about the given constraint. This happens every time a constraint
  /// gets removed for a constraint graph.
  void retractFromInference(Constraint *constraint);

  /// Re-evaluate the given constraint. This happens when there are changes
  /// in associated type variables e.g. bound/unbound to/from a fixed type,
  /// equivalence class changes.
  void reintroduceToInference(Constraint *constraint);

  /// Similar to \c introduceToInference(Constraint *, ...) this method is going
  /// to notify inference that this type variable has been bound to a concrete
  /// type.
  ///
  /// The reason why this can't simplify be a part of \c bindTypeVariable
  /// is related to the fact that it's sometimes expensive to re-compute
  /// bindings (i.e. if `DependentMemberType` is involved, because it requires
  /// a conformance lookup), so inference has to be delayed until its clear that
  /// type variable has been bound to a valid type and solver can make progress.
  void introduceToInference(Type fixedType);

  /// Opposite of \c introduceToInference(Type)
  void
  retractFromInference(Type fixedType,
                       SmallPtrSetImpl<TypeVariableType *> &referencedVars);

  /// Drop all previously collected bindings and re-infer based on the
  /// current set constraints associated with this equivalence class.
  void resetBindingSet();

  /// Notify all of the type variables that have this one (or any member of
  /// its equivalence class) referenced in their fixed type.
  ///
  /// This is a traversal up the reference change which triggers constraint
  /// re-introduction to affected type variables.
  ///
  /// This is useful in situations when type variable gets bound and unbound,
  /// or equivalence class changes.
  void notifyReferencingVars() const;

  /// Notify all of the type variables referenced by this one about a change.
  void notifyReferencedVars(
      llvm::function_ref<void(ConstraintGraphNode &)> notification);

  /// }

  /// The constraint graph this node belongs to.
  ConstraintGraph &CG;

  /// The type variable this node represents.
  TypeVariableType *TypeVar;

  /// The set of bindings associated with this type variable.
  std::optional<inference::PotentialBindings> Bindings;

  /// The vector of constraints that mention this type variable, in a stable
  /// order for iteration.
  SmallVector<Constraint *, 2> Constraints;

  /// A mapping from the set of constraints that mention this type variable
  /// to the index within the vector of constraints.
  llvm::SmallDenseMap<Constraint *, unsigned, 2> ConstraintIndex;

  /// The set of type variables that reference type variable associated
  /// with this constraint graph node.
  llvm::SmallSetVector<TypeVariableType *, 2> ReferencedBy;

  /// The set of type variables that occur within the fixed binding of
  /// this type variable.
  llvm::SmallSetVector<TypeVariableType *, 2> References;

  /// All of the type variables in the same equivalence class as this
  /// representative type variable.
  ///
  /// Note that this field is only valid for type variables that
  /// are representatives of their equivalence classes.
  mutable SmallVector<TypeVariableType *, 2> EquivalenceClass;

  /// Print this graph node.
  void print(llvm::raw_ostream &out, unsigned indent,
             PrintOptions PO = PrintOptions()) const;

  SWIFT_DEBUG_DUMP;

  /// Verify the invariants of this node within the given constraint graph.
  void verify(ConstraintGraph &cg);

  friend class ConstraintGraph;
  friend class ConstraintSystem;
  friend class TypeVariableBinding;
};

/// A graph that describes the relationships among the various type variables
/// and constraints within a constraint system.
///
/// The constraint graph is a hypergraph where the nodes are type variables and
/// the edges are constraints. Any given constraint connects a type variable to
/// zero or more other type variables. Because these adjacencies are as
/// important as the edges themselves and are expensive to calculate from the
/// constraints, each node in the graph tracks both its edges (constraints) and
/// its adjacencies (the type variables) separately.
class ConstraintGraph {
public:
  /// Constraint a constraint graph for the given constraint system.
  ConstraintGraph(ConstraintSystem &cs);

  /// Destroy the given constraint graph.
  ~ConstraintGraph();

  ConstraintGraph(const ConstraintGraph &) = delete;
  ConstraintGraph &operator=(const ConstraintGraph &) = delete;


  /// Retrieve the constraint system this graph describes.
  ConstraintSystem &getConstraintSystem() const { return CS; }

  /// Access the node corresponding to the given type variable.
  ConstraintGraphNode &operator[](TypeVariableType *typeVar) {
    return lookupNode(typeVar).first;
  }

  /// Retrieve the node and index corresponding to the given type variable.
  std::pair<ConstraintGraphNode &, unsigned> 
  lookupNode(TypeVariableType *typeVar);

  /// Add a constraint to a vertex in the graph.
  void addConstraint(TypeVariableType *typeVar, Constraint *constraint,
                     unsigned index=-1);

  /// Add a new constraint to the graph.
  void addConstraint(Constraint *constraint);

  /// Remove a constraint from a vertex in the graph.
  void removeConstraint(TypeVariableType *typeVar, Constraint *constraint);

  /// Remove a constraint from the graph.
  void removeConstraint(Constraint *constraint);

  /// Merge the two nodes for the two given type variables.
  ///
  /// The type variables must actually have been merged already; this
  /// operation merges the two nodes.
  void mergeNodes(TypeVariableType *typeVar1, TypeVariableType *typeVar2);

  /// Bind the given type variable to the given fixed type.
  void bindTypeVariable(TypeVariableType *typeVar, Type fixedType);

  /// Describes which constraints \c gatherConstraints should gather.
  enum class GatheringKind {
    /// Gather constraints associated with all of the variables within the
    /// same equivalence class as the given type variable, as well as its
    /// immediate fixed bindings.
    EquivalenceClass,
    /// Gather all constraints that mention this type variable or type variables
    /// that it is a fixed binding of. Unlike EquivalenceClass, this looks
    /// through transitive fixed bindings. This can be used to find all the
    /// constraints that may be affected when binding a type variable.
    AllMentions,
  };

  /// Gather the set of constraints that involve the given type variable,
  /// i.e., those constraints that will be affected when the type variable
  /// gets merged or bound to a fixed type.
  llvm::TinyPtrVector<Constraint *>
  gatherConstraints(TypeVariableType *typeVar,
                    GatheringKind kind,
                    llvm::function_ref<bool(Constraint *)> acceptConstraint =
                        [](Constraint *constraint) { return true; });

  /// Retrieve the type variables that correspond to nodes in the graph.
  ///
  /// The subscript operator can be used to retrieve the nodes that
  /// correspond to these type variables.
  ArrayRef<TypeVariableType *> getTypeVariables() const {
    return TypeVariables;
  }

  /// Describes a single component, as produced by the connected components
  /// algorithm.
  struct Component {
    /// The type variables in this component.
    TinyPtrVector<TypeVariableType *> typeVars;

    /// The original index of this component in the list of components,
    /// used to provide the index of where the partial solutions will occur.
    /// FIXME: This is needed due to some ordering dependencies in the
    /// merging of partial solutions, which appears to also be related
    /// DisjunctionStep::pruneOverloads() short-circuiting. It should be
    /// removed.
    unsigned solutionIndex;

  private:
    /// The number of disjunctions in this component.
    unsigned numDisjunctions = 0;

    /// The constraints in this component.
    TinyPtrVector<Constraint *> constraints;

    /// The set of components that this component depends on, such that
    /// the partial solutions of the those components need to be available
    /// before this component can be solved.
    ///
    SmallVector<unsigned, 2> dependencies;

  public:
    Component(unsigned solutionIndex) : solutionIndex(solutionIndex) { }

    /// Whether this component represents an orphaned constraint.
    bool isOrphaned() const {
      return typeVars.empty();
    }

    /// Add a constraint.
    void addConstraint(Constraint *constraint);

    const TinyPtrVector<Constraint *> &getConstraints() const {
      return constraints;
    }

    /// Records a component which this component depends on.
    void recordDependency(const Component &component);

    ArrayRef<unsigned> getDependencies() const { return dependencies; }

    unsigned getNumDisjunctions() const { return numDisjunctions; }
  };

  /// Compute the connected components of the graph.
  ///
  /// \param typeVars The type variables that should be included in the
  /// set of connected components that are returned.
  ///
  /// \returns the connected components of the graph, where each component
  /// contains the type variables and constraints specific to that component.
  SmallVector<Component, 1> computeConnectedComponents(
             ArrayRef<TypeVariableType *> typeVars);

  /// Retrieve the set of "orphaned" constraints, which are known to the
  /// constraint graph but have no type variables to anchor them.
  ArrayRef<Constraint *> getOrphanedConstraints() const {
    return OrphanedConstraints;
  }

  /// Replace the orphaned constraints with the constraints in the given list,
  /// returning the old set of orphaned constraints.
  SmallVector<Constraint *, 4> takeOrphanedConstraints() {
    auto result = std::move(OrphanedConstraints);
    OrphanedConstraints.clear();
    return result;
  }

  /// Set the orphaned constraints.
  void setOrphanedConstraints(SmallVector<Constraint *, 4> &&newConstraints) {
    OrphanedConstraints = std::move(newConstraints);
  }

  /// Set the list of orphaned constraints to a single constraint.
  ///
  /// If \c orphaned is null, just clear out the list.
  void setOrphanedConstraint(Constraint *orphaned) {
    OrphanedConstraints.clear();
    if (orphaned)
      OrphanedConstraints.push_back(orphaned);
  }

  /// Print the graph.
  void print(ArrayRef<TypeVariableType *> typeVars, llvm::raw_ostream &out);
  void dump(llvm::raw_ostream &out);
  void dumpActiveScopeChanges(llvm::raw_ostream &out, unsigned indent = 0);

  // FIXME: Potentially side-effectful.
  SWIFT_DEBUG_HELPER(void dump());

  /// Print the connected components of the graph.
  void printConnectedComponents(ArrayRef<TypeVariableType *> typeVars,
                                llvm::raw_ostream &out);

  // FIXME: Potentially side-effectful.
  SWIFT_DEBUG_HELPER(void dumpConnectedComponents());

  /// Verify the invariants of the graph.
  void verify();

  /// Optimize the constraint graph by eliminating simple transitive
  /// connections between nodes.
  void optimize();

private:
  /// Remove the node corresponding to the given type variable.
  ///
  /// This operation assumes that the any constraints that refer to
  /// this type variable have been or will be removed before other
  /// graph queries are performed.
  ///
  /// Note that this change is not recorded and cannot be undone. Use with
  /// caution.
  void removeNode(TypeVariableType *typeVar);

  /// Unbind the given type variable from the given fixed type.
  ///
  /// Note that this change is not recorded and cannot be undone. Use with
  /// caution.
  void unbindTypeVariable(TypeVariableType *typeVar, Type fixedType);

  /// Perform edge contraction on the constraint graph, merging equivalence
  /// classes until a fixed point is reached.
  bool contractEdges();

  /// The constraint system.
  ConstraintSystem &CS;

  /// The type variables in this graph, in stable order.
  std::vector<TypeVariableType *> TypeVariables;

  /// Constraints that are "orphaned" because they contain no type variables.
  SmallVector<Constraint *, 4> OrphanedConstraints;

  /// Increment the number of constraints considered per attempt
  /// to contract constraint graph edges.
  void incrementConstraintsPerContractionCounter();

  /// The kind of change made to the graph.
  enum class ChangeKind: uint8_t {
    /// Added a type variable.
    AddedTypeVariable,
    /// Added a new constraint.
    AddedConstraint,
    /// Removed an existing constraint
    RemovedConstraint,
    /// Extended the equivalence class of a type variable.
    ExtendedEquivalenceClass,
    /// Added a fixed binding for a type variable.
    BoundTypeVariable,
  };

  /// A change made to the constraint graph.
  ///
  /// Each change can be undone (once, and in reverse order) by calling the
  /// undo() method.
  class Change {
  public:
    /// The type variable and the kind of change.
    llvm::PointerIntPair<TypeVariableType *, 3, ChangeKind> TypeVarAndKind;

    union {
      struct {
        Constraint *TheConstraint;
        unsigned Index;
      } Edge;

      struct {
        /// The previous size of the equivalence class.
        unsigned PrevSize;
      } EquivClass;

      struct {
        /// The fixed type to which the type variable was bound.
        TypeBase *FixedType;
      } Binding;
    };

    Change()
      : TypeVarAndKind(nullptr, ChangeKind::AddedTypeVariable) {}

    /// Create a change that added a type variable.
    static Change addedTypeVariable(TypeVariableType *typeVar);

    /// Create a change that added a constraint.
    static Change addedConstraint(TypeVariableType *typeVar,
                                  Constraint *constraint);

    /// Create a change that removed a constraint.
    static Change removedConstraint(TypeVariableType *typeVar,
                                    Constraint *constraint,
                                    unsigned index);

    /// Create a change that extended an equivalence class.
    static Change extendedEquivalenceClass(TypeVariableType *typeVar,
                                           unsigned prevSize);

    /// Create a change that bound a type variable to a fixed type.
    static Change boundTypeVariable(TypeVariableType *typeVar, Type fixed);

    /// Undo this change, reverting the constraint graph to the state it
    /// had prior to this change.
    ///
    /// Changes must be undone in stack order.
    void undo(ConstraintGraph &cg);
  };

  /// The currently active scope, or null if we aren't tracking changes made
  /// to the constraint graph.
  ConstraintGraphScope *ActiveScope = nullptr;

  /// The set of changes made to this constraint graph.
  ///
  /// As the constraint graph is extended and mutated, additional changes are
  /// introduced into this vector. Each scope
  llvm::SmallVector<Change, 4> Changes;

  friend class ConstraintGraphScope;
};

} // end namespace constraints

} // end namespace swift

#endif // LLVM_SWIFT_SEMA_CONSTRAINT_GRAPH_H
