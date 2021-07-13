//===--- GenericSignatureBuilder.cpp - Generic Requirement Builder --------===//
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
// Support for collecting a set of generic requirements, both explicitly stated
// and inferred, and computing the archetypes and required witness tables from
// those requirements.
//
//===----------------------------------------------------------------------===//

#include "GenericSignatureBuilderImpl.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/AST/TypeRepr.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include <algorithm>

using namespace swift;
using llvm::DenseMap;

/// Define this to 1 to enable expensive assertions.
#define SWIFT_GSB_EXPENSIVE_ASSERTIONS 0

namespace {
  typedef GenericSignatureBuilder::RequirementSource RequirementSource;
  typedef GenericSignatureBuilder::FloatingRequirementSource
    FloatingRequirementSource;
  typedef GenericSignatureBuilder::ConstraintResult ConstraintResult;
  typedef GenericSignatureBuilder::PotentialArchetype PotentialArchetype;
  typedef GenericSignatureBuilder::ConcreteConstraint ConcreteConstraint;
  template<typename T> using Constraint =
    GenericSignatureBuilder::Constraint<T>;
  typedef GenericSignatureBuilder::EquivalenceClass EquivalenceClass;
  typedef EquivalenceClass::DerivedSameTypeComponent DerivedSameTypeComponent;
  typedef GenericSignatureBuilder::DelayedRequirement DelayedRequirement;
  typedef GenericSignatureBuilder::ResolvedType ResolvedType;
  typedef GenericSignatureBuilder::RequirementRHS RequirementRHS;
  typedef GenericSignatureBuilder::ExplicitRequirement ExplicitRequirement;
} // end anonymous namespace

namespace llvm {
  // Equivalence classes are bump-ptr-allocated.
  template <> struct ilist_alloc_traits<EquivalenceClass> {
    static void deleteNode(EquivalenceClass *ptr) { ptr->~EquivalenceClass(); }
  };
}

#define DEBUG_TYPE "Generic signature builder"
STATISTIC(NumPotentialArchetypes, "# of potential archetypes");
STATISTIC(NumEquivalenceClassesAllocated, "# of equivalence classes allocated");
STATISTIC(NumEquivalenceClassesFreed, "# of equivalence classes freed");
STATISTIC(NumConformances, "# of conformances tracked");
STATISTIC(NumConformanceConstraints, "# of conformance constraints tracked");
STATISTIC(NumSameTypeConstraints, "# of same-type constraints tracked");
STATISTIC(NumConcreteTypeConstraints,
          "# of same-type-to-concrete constraints tracked");
STATISTIC(NumSuperclassConstraints, "# of superclass constraints tracked");
STATISTIC(NumSuperclassConstraintsExtra,
          "# of superclass constraints that add no information");
STATISTIC(NumLayoutConstraints, "# of layout constraints tracked");
STATISTIC(NumLayoutConstraintsExtra,
          "# of layout constraints  that add no information");
STATISTIC(NumSelfDerived, "# of self-derived constraints removed");
STATISTIC(NumArchetypeAnchorCacheHits,
          "# of hits in the archetype anchor cache");
STATISTIC(NumArchetypeAnchorCacheMisses,
          "# of misses in the archetype anchor cache");
STATISTIC(NumNestedTypeCacheHits,
         "# of hits in the equivalence class nested type cache");
STATISTIC(NumNestedTypeCacheMisses,
         "# of misses in the equivalence class nested type cache");
STATISTIC(NumProcessDelayedRequirements,
          "# of times we process delayed requirements");
STATISTIC(NumProcessDelayedRequirementsUnchanged,
          "# of times we process delayed requirements without change");
STATISTIC(NumDelayedRequirementConcrete,
          "Delayed requirements resolved as concrete");
STATISTIC(NumDelayedRequirementResolved,
          "Delayed requirements resolved");
STATISTIC(NumDelayedRequirementUnresolved,
          "Delayed requirements left unresolved");
STATISTIC(NumConditionalRequirementsAdded,
          "# of conditional requirements added");
STATISTIC(NumRewriteMinimizations,
          "# of rewrite system minimizations performed");
STATISTIC(NumRewriteRhsSimplified,
          "# of rewrite rule right-hand sides simplified");
STATISTIC(NumRewriteRhsSimplifiedToLhs,
          "# of rewrite rule right-hand sides simplified to lhs (and removed)");
STATISTIC(NumRewriteRulesRedundant,
          "# of rewrite rules that are redundant (and removed)");
STATISTIC(NumSignaturesRebuiltWithoutRedundantRequirements,
          "# of generic signatures which had a concretized conformance requirement");

namespace  {

/// A purely-relative rewrite path consisting of a (possibly empty)
/// sequence of associated type references.
using RelativeRewritePath = ArrayRef<AssociatedTypeDecl *>;

class AnchorPathCache;

/// Describes a rewrite path, which contains an optional base (generic
/// parameter) followed by a sequence of associated type references.
class RewritePath {
  Optional<GenericParamKey> base;
  TinyPtrVector<AssociatedTypeDecl *> path;

public:
  RewritePath() { }

  enum PathOrder {
    Forward,
    Reverse,
  };

  /// Form a rewrite path given an optional base and a relative rewrite path.
  RewritePath(Optional<GenericParamKey> base, RelativeRewritePath path,
              PathOrder order);

  /// Retrieve the base of the given rewrite path.
  ///
  /// When present, it indicates that the entire path will be rebased on
  /// the given base generic parameter. This is required for describing
  /// rewrites on type parameters themselves, e.g., T == U.
  ///
  /// When absent, the path is relative to the root of the tree from which
  /// the search began.
  Optional<GenericParamKey> getBase() const { return base; }

  /// Retrieve the sequence of associated type references that describes
  /// the path.
  ArrayRef<AssociatedTypeDecl *> getPath() const { return path; }

  /// Whether this path is completely empty.
  bool isEmpty() const { return getBase() == None && getPath().empty(); }

  /// whether this describes a valid path.
  explicit operator bool() const { return !isEmpty(); }

  /// Decompose a type into a path.
  ///
  /// \returns the path, or None if it contained unresolved dependent member
  /// types.
  static RewritePath createPath(Type type);

  /// Decompose a type into a path.
  ///
  /// \param path Will be filled in with the components of the path, in
  /// reverse order.
  ///
  /// \returns the generic parameter at the start of the path.
  static GenericParamKey createPath(
                                Type type,
                                SmallVectorImpl<AssociatedTypeDecl *> &path);

  /// Compute the longer common prefix between this path and \c other.
  RewritePath commonPath(const RewritePath &other) const;

  /// Form a canonical, dependent type.
  ///
  /// This requires that either the rewrite path have a base, or the
  /// \c baseEquivClass to be non-null (which substitutes in a base).
  CanType formDependentType(ASTContext &ctx,
                            AnchorPathCache *anchorPathCache = nullptr) const;

  /// Compare the given rewrite paths.
  int compare(const RewritePath &other) const;

  /// Print this path.
  void print(llvm::raw_ostream &out) const;

  SWIFT_DEBUG_DUMP {
    print(llvm::errs());
  }

  friend bool operator==(const RewritePath &lhs, const RewritePath &rhs) {
    return lhs.getBase() == rhs.getBase() && lhs.getPath() == rhs.getPath();
  }
};

/// A cache that lazily computes the anchor path for the given equivalence
/// class.
class AnchorPathCache {
  GenericSignatureBuilder &builder;
  EquivalenceClass &equivClass;
  Optional<RewritePath> anchorPath;

public:
  AnchorPathCache(GenericSignatureBuilder &builder,
                  EquivalenceClass &equivClass)
    : builder(builder), equivClass(equivClass) { }

  const RewritePath &getAnchorPath() {
    if (anchorPath) return *anchorPath;

    anchorPath = RewritePath::createPath(equivClass.getAnchor(builder, { }));
    return *anchorPath;
  }
};

/// A node within the prefix tree that is used to match associated type
/// references.
class RewriteTreeNode {
  /// The associated type that leads to this node.
  ///
  /// The bit indicates whether there is a rewrite rule for this particular
  /// node. If the bit is not set, \c rewrite is invalid.
  llvm::PointerIntPair<AssociatedTypeDecl *, 1, bool> assocTypeAndHasRewrite;

  /// The sequence of associated types to which a reference to this associated
  /// type (from the equivalence class root) can be rewritten. This field is
  /// only valid when the bit of \c assocTypeAndHasRewrite is set.
  ///
  /// Consider a requirement "Self.A.B.C == C". This will be encoded as
  /// a prefix tree starting at the equivalence class for Self with
  /// the following nodes:
  ///
  /// (assocType: A,
  ///   children: [
  ///     (assocType: B,
  ///       children: [
  ///         (assocType: C, rewrite: [C], children: [])
  ///       ])
  ///   ])
  RewritePath rewrite;

  /// The child nodes, which extend the sequence to be matched.
  ///
  /// The child nodes are sorted by the associated type declaration
  /// pointers, so we can perform binary searches quickly.
  llvm::TinyPtrVector<RewriteTreeNode *> children;

public:
  ~RewriteTreeNode();

  RewriteTreeNode(AssociatedTypeDecl *assocType)
    : assocTypeAndHasRewrite(assocType, false) { }

  /// Retrieve the associated type declaration one must match to use this
  /// node, which may the
  AssociatedTypeDecl *getMatch() const {
    return assocTypeAndHasRewrite.getPointer();
  }

  /// Determine whether this particular node has a rewrite rule.
  bool hasRewriteRule() const {
    return assocTypeAndHasRewrite.getInt();
  }

  /// Set a new rewrite rule for this particular node. This can only be
  /// performed once.
  void setRewriteRule(RewritePath replacementPath) {
    assert(!hasRewriteRule());
    assocTypeAndHasRewrite.setInt(true);
    rewrite = replacementPath;
  }

  /// Remove the rewrite rule.
  void removeRewriteRule() {
    assert(hasRewriteRule());
    assocTypeAndHasRewrite.setInt(false);
  }

  /// Retrieve the path to which this node will be rewritten.
  const RewritePath &getRewriteRule() const & {
    assert(hasRewriteRule());
    return rewrite;
  }

  /// Retrieve the path to which this node will be rewritten.
  RewritePath &&getRewriteRule() && {
    assert(hasRewriteRule());
    return std::move(rewrite);
  }

  /// Add a new rewrite rule to this tree node.
  ///
  /// \param matchPath The path of associated type declarations that must
  /// be matched to produce a rewrite.
  ///
  /// \param replacementPath The sequence of associated type declarations
  /// with which a match will be replaced.
  ///
  /// \returns true if a rewrite rule was added, and false if it already
  /// existed.
  bool addRewriteRule(RelativeRewritePath matchPath,
                      RewritePath replacementPath);

  /// Enumerate all of the paths to which the given matched path can be
  /// rewritten.
  ///
  /// \param matchPath The path to match.
  ///
  /// \param callback A callback that will be invoked with (prefix, rewrite)
  /// pairs, where \c prefix is the length of the matching prefix of
  /// \c matchPath that matched and \c rewrite is the path to which it can
  /// be rewritten.
  void enumerateRewritePaths(
               RelativeRewritePath matchPath,
               llvm::function_ref<void(unsigned, RewritePath)> callback) const {
    return enumerateRewritePathsImpl(matchPath, callback, /*depth=*/0);
  }

private:
  void enumerateRewritePathsImpl(
               RelativeRewritePath matchPath,
               llvm::function_ref<void(unsigned, RewritePath)> callback,
               unsigned depth) const;

public:

  /// Find the best rewrite rule to match the given path.
  ///
  /// \param path The path to match.
  /// \param prefixLength The length of the prefix leading up to \c path.
  Optional<std::pair<unsigned, RewritePath>>
  bestRewritePath(GenericParamKey base, RelativeRewritePath path,
            unsigned prefixLength);

  /// Merge the given rewrite tree into \c other.
  ///
  /// \returns true if any rules were created by this merge.
  bool mergeInto(RewriteTreeNode *other);

  /// An action to perform for the given rule
  class RuleAction {
    enum Kind {
      /// No action; continue traversal.
      None,

      /// Stop traversal.
      Stop,

      /// Remove the given rule completely.
      Remove,

      /// Replace the right-hand side of the rule with the given new path.
      Replace,
    } kind;

    RewritePath path;

    RuleAction(Kind kind, RewritePath path = {})
      : kind(kind), path(path) { }

    friend class RewriteTreeNode;

  public:
    static RuleAction none() { return RuleAction(None); }
    static RuleAction stop() { return RuleAction(Stop); }
    static RuleAction remove() { return RuleAction(Remove); }

    static RuleAction replace(RewritePath path) {
      return RuleAction(Replace, std::move(path));
    }

    operator Kind() const { return kind; }
  };

  /// Callback function for enumerating rules in a tree.
  using EnumerateCallback =
    RuleAction(RelativeRewritePath lhs, const RewritePath &rhs);

  /// Enumerate all of the rewrite rules, calling \c fn with the left and
  /// right-hand sides of each rule.
  ///
  /// \returns true if the action function returned \c Stop at any point.
  bool enumerateRules(llvm::function_ref<EnumerateCallback> fn,
                      bool temporarilyDisableVisitedRule = false) {
    SmallVector<AssociatedTypeDecl *, 4> lhs;
    return enumerateRulesRec(fn, temporarilyDisableVisitedRule, lhs);
  }

  SWIFT_DEBUG_DUMP;

  /// Dump the tree.
  void dump(llvm::raw_ostream &out, bool lastChild = true) const;

private:
  /// Enumerate all of the rewrite rules, calling \c fn with the left and
  /// right-hand sides of each rule.
  ///
  /// \returns true if the action function returned \c Stop at any point.
  bool enumerateRulesRec(llvm::function_ref<EnumerateCallback> &fn,
                         bool temporarilyDisableVisitedRule,
                         llvm::SmallVectorImpl<AssociatedTypeDecl *> &lhs);
};
}

/// A representation of an explicit requirement, used for diagnosing redundant
/// requirements.
///
/// Just like the Requirement data type, this stores the requirement kind and
/// a right hand side (either another type, a protocol, or a layout constraint).
///
/// However, instead of a subject type, we store an explicit requirement source,
/// from which the subject type can be obtained.
class GenericSignatureBuilder::ExplicitRequirement {
  llvm::PointerIntPair<const RequirementSource *, 2, RequirementKind> sourceAndKind;
  RequirementRHS rhs;

public:
  ExplicitRequirement(RequirementKind kind,
                      const RequirementSource *source,
                      RequirementRHS rhs)
    : sourceAndKind(source, kind), rhs(rhs) {
    if (auto type = rhs.dyn_cast<Type>())
      this->rhs = type->getCanonicalType();
  }

  static std::pair<RequirementKind, RequirementRHS>
  getKindAndRHS(const RequirementSource *source,
                ASTContext &ctx) {
    switch (source->kind) {
    // If we have a protocol requirement source whose parent is the root, then
    // we have an implied constraint coming from a protocol's requirement
    // signature:
    //
    // Explicit: T -> ProtocolRequirement: Self.Iterator (in Sequence)
    //
    // The root's subject type (T) is the subject of the constraint. The next
    // innermost child -- the ProtocolRequirement source -- stores the
    // conformed-to protocol declaration.
    //
    // Therefore the original constraint was 'T : Sequence'.
    case RequirementSource::ProtocolRequirement:
    case RequirementSource::InferredProtocolRequirement: {
      auto *proto = source->getProtocolDecl();
      return std::make_pair(RequirementKind::Conformance, proto);
    }

    // If we have a superclass or concrete source whose parent is the root, then
    // we have an implied conformance constraint -- either this:
    //
    // Explicit: T -> Superclass: [MyClass : P]
    //
    // or this:
    //
    // Explicit: T -> Concrete: [MyStruct : P]
    //
    // The original constraint was either 'T : MyClass' or 'T == MyStruct',
    // respectively.
    case RequirementSource::Superclass:
    case RequirementSource::Concrete: {
      Type conformingType = source->getStoredType();
      if (conformingType) {
        // A concrete requirement source for a self-conforming exitential type
        // stores the original type, and not the conformance, because there is
        // no way to recover the original type from the conformance.
        assert(conformingType->isExistentialType());
      } else {
        auto conformance = source->getProtocolConformance();
        if (conformance.isConcrete())
          conformingType = conformance.getConcrete()->getType();
        else {
          // If the conformance was abstract or invalid, we're dealing with
          // invalid code. We have no way to recover the superclass type, so
          // just stick an ErrorType in there.
          conformingType = ErrorType::get(ctx);
        }
      }

      auto kind = (source->kind == RequirementSource::Superclass
                   ? RequirementKind::Superclass
                   : RequirementKind::SameType);
      return std::make_pair(kind, conformingType);
    }

    // If we have a layout source whose parent is the root, then
    // we have an implied layout constraint:
    //
    // Explicit: T -> Layout: MyClass
    //
    // The original constraint was the superclass constraint 'T : MyClass'
    // (*not* a layout constraint -- this is the layout constraint *implied*
    // by a superclass constraint, ensuring that it supercedes an explicit
    // 'T : AnyObject' constraint).
    case RequirementSource::Layout: {
      auto type = source->getStoredType();
      return std::make_pair(RequirementKind::Superclass, type);
    }

    case RequirementSource::RequirementSignatureSelf:
      return std::make_pair(RequirementKind::Conformance,
                            source->getProtocolDecl());

    default: {
      source->dump(llvm::errs(), &ctx.SourceMgr, 0);
      llvm::errs() << "\n";
      llvm_unreachable("Unhandled source kind");
    }
    }
  }

  RequirementKind getKind() const {
    return sourceAndKind.getInt();
  }

  Type getSubjectType() const {
    return getSource()->getStoredType();
  }

  const RequirementSource *getSource() const {
    return sourceAndKind.getPointer();
  }

  RequirementRHS getRHS() const {
    return rhs;
  }

  void dump(llvm::raw_ostream &out, SourceManager *SM) const;

  static ExplicitRequirement getEmptyKey() {
    return ExplicitRequirement(RequirementKind::Conformance, nullptr, nullptr);
  }

  static ExplicitRequirement getTombstoneKey() {
    return ExplicitRequirement(RequirementKind::Superclass, nullptr, Type());
  }

  friend llvm::hash_code hash_value(const ExplicitRequirement &req) {
    using llvm::hash_value;

    llvm::hash_code first = hash_value(req.sourceAndKind.getOpaqueValue());
    llvm::hash_code second = hash_value(req.rhs.getOpaqueValue());

    return llvm::hash_combine(first, second);
  }

  friend bool operator==(const ExplicitRequirement &lhs,
                         const ExplicitRequirement &rhs) {
    if (lhs.sourceAndKind.getOpaqueValue()
          != rhs.sourceAndKind.getOpaqueValue())
      return false;

    if (lhs.rhs.getOpaqueValue()
          != rhs.rhs.getOpaqueValue())
      return false;

    return true;
  }

  friend bool operator!=(const ExplicitRequirement &lhs,
                         const ExplicitRequirement &rhs) {
    return !(lhs == rhs);
  }
};

namespace llvm {

template<> struct DenseMapInfo<swift::GenericSignatureBuilder::ExplicitRequirement> {
  static swift::GenericSignatureBuilder::ExplicitRequirement getEmptyKey() {
    return swift::GenericSignatureBuilder::ExplicitRequirement::getEmptyKey();
  }
  static swift::GenericSignatureBuilder::ExplicitRequirement getTombstoneKey() {
    return swift::GenericSignatureBuilder::ExplicitRequirement::getTombstoneKey();
  }
  static unsigned getHashValue(
      const swift::GenericSignatureBuilder::ExplicitRequirement &req) {
    return hash_value(req);
  }
  static bool isEqual(const swift::GenericSignatureBuilder::ExplicitRequirement &lhs,
                      const swift::GenericSignatureBuilder::ExplicitRequirement &rhs) {
    return lhs == rhs;
  }

};

}

namespace {
  struct ConflictingConcreteTypeRequirement {
    ExplicitRequirement otherRequirement;
    Type resolvedConcreteType;
    RequirementRHS otherRHS;
  };
}

struct GenericSignatureBuilder::Implementation {
  /// Allocator.
  llvm::BumpPtrAllocator Allocator;

  /// The generic parameters that this generic signature builder is working
  /// with.
  SmallVector<Type, 4> GenericParams;

  /// The potential archetypes for the generic parameters in \c GenericParams.
  SmallVector<PotentialArchetype *, 4> PotentialArchetypes;

  /// The requirement sources used in this generic signature builder.
  llvm::FoldingSet<RequirementSource> RequirementSources;

  /// The set of requirements that have been delayed for some reason.
  SmallVector<DelayedRequirement, 4> DelayedRequirements;

  /// The set of equivalence classes.
  llvm::iplist<EquivalenceClass> EquivalenceClasses;

  /// Equivalence classes that are not currently being used.
  std::vector<void *> FreeEquivalenceClasses;

  /// The roots of the rewrite tree, keyed on the canonical, dependent
  /// types.
  DenseMap<CanType, std::unique_ptr<RewriteTreeNode>> RewriteTreeRoots;

  /// The generation number for the term-rewriting system, which is
  /// increased every time a new rule gets added.
  unsigned RewriteGeneration = 0;

  /// The generation at which the term-rewriting system was last minimized.
  unsigned LastRewriteMinimizedGeneration = 0;

  /// The generation number, which is incremented whenever we successfully
  /// introduce a new constraint.
  unsigned Generation = 0;

  /// The generation at which we last processed all of the delayed requirements.
  unsigned LastProcessedGeneration = 0;

  /// Whether we are currently processing delayed requirements.
  bool ProcessingDelayedRequirements = false;

  /// Whether we are currently minimizing the term-rewriting system.
  bool MinimizingRewriteSystem = false;

  /// Whether there were any errors.
  bool HadAnyError = false;

  /// Set this to true to get some debug output.
  bool DebugRedundantRequirements = false;

  /// All explicit non-same type requirements that were added to the builder.
  SmallSetVector<ExplicitRequirement, 2> ExplicitRequirements;

  /// All explicit same-type requirements that were added to the builder.
  SmallVector<ExplicitRequirement, 2> ExplicitSameTypeRequirements;

  /// Whether we are rebuilding a signature without redundant conformance
  /// requirements.
  bool RebuildingWithoutRedundantConformances = false;

  /// A mapping of redundant explicit requirements to the best root requirement
  /// that implies them. Built by computeRedundantRequirements().
  using RedundantRequirementMap =
      llvm::DenseMap<ExplicitRequirement,
                     llvm::SmallDenseSet<ExplicitRequirement, 2>>;

  RedundantRequirementMap RedundantRequirements;

  /// Requirements which conflict with other requirements, for example if a
  /// there are two unrelated superclass requirements on the same type,
  /// the second one is recorded here. Built by computeRedundantRequirements().
  llvm::DenseMap<ExplicitRequirement, RequirementRHS> ConflictingRequirements;

  /// Pairs of conflicting concrete type vs. superclass/layout/conformance
  /// requirements.
  std::vector<ConflictingConcreteTypeRequirement>
      ConflictingConcreteTypeRequirements;

  llvm::DenseSet<ExplicitRequirement> ExplicitConformancesImpliedByConcrete;

  llvm::DenseMap<std::pair<CanType, ProtocolDecl *>,
                 ConformanceAccessPath> ConformanceAccessPaths;

  std::vector<std::pair<CanType, ConformanceAccessPath>>
      CurrentConformanceAccessPaths;

#ifndef NDEBUG
  /// Whether we've already computed redundant requiremnts.
  bool computedRedundantRequirements = false;

  /// Whether we've already finalized the builder.
  bool finalized = false;
#endif

  /// Tear down an implementation.
  ~Implementation();

  /// Allocate a new equivalence class with the given representative.
  EquivalenceClass *allocateEquivalenceClass(
                                       PotentialArchetype *representative);

  /// Deallocate the given equivalence class, returning it to the free list.
  void deallocateEquivalenceClass(EquivalenceClass *equivClass);

  /// Retrieve the rewrite tree root for the given anchor type.
  RewriteTreeNode *getRewriteTreeRootIfPresent(CanType anchor);

  /// Retrieve the rewrite tree root for the given anchor type,
  /// creating it if needed.
  RewriteTreeNode *getOrCreateRewriteTreeRoot(CanType anchor);

  /// Minimize the rewrite tree by minimizing the right-hand sides and
  /// removing redundant rules.
  void minimizeRewriteTree(GenericSignatureBuilder &builder);

private:
  /// Minimize the right-hand sides of the rewrite tree, simplifying them
  /// as far as possible and removing any changes that result in trivial
  /// rules.
  void minimizeRewriteTreeRhs(GenericSignatureBuilder &builder);

  /// Minimize the right-hand sides of the rewrite tree, simplifying them
  /// as far as possible and removing any changes that result in trivial
  /// rules.
  void removeRewriteTreeRedundancies(GenericSignatureBuilder &builder);
};

#pragma mark Memory management
GenericSignatureBuilder::Implementation::~Implementation() {
  for (auto pa : PotentialArchetypes)
    pa->~PotentialArchetype();
}

EquivalenceClass *
GenericSignatureBuilder::Implementation::allocateEquivalenceClass(
                                          PotentialArchetype *representative) {
  void *mem;
  if (FreeEquivalenceClasses.empty()) {
    // Allocate a new equivalence class.
    mem = Allocator.Allocate<EquivalenceClass>();
  } else {
    // Take an equivalence class from the free list.
    mem = FreeEquivalenceClasses.back();
    FreeEquivalenceClasses.pop_back();
  }

  auto equivClass = new (mem) EquivalenceClass(representative);
  EquivalenceClasses.push_back(equivClass);

  ++NumEquivalenceClassesAllocated;

  return equivClass;
}

void GenericSignatureBuilder::Implementation::deallocateEquivalenceClass(
                                               EquivalenceClass *equivClass) {
  EquivalenceClasses.erase(equivClass);
  FreeEquivalenceClasses.push_back(equivClass);

  ++NumEquivalenceClassesFreed;
}

#pragma mark Requirement sources

#ifndef NDEBUG
bool RequirementSource::isAcceptableStorageKind(Kind kind,
                                                StorageKind storageKind) {
  switch (kind) {
  case Explicit:
  case Inferred:
  case RequirementSignatureSelf:
  case NestedTypeNameMatch:
  case EquivalentType:
  case Layout:
    switch (storageKind) {
    case StorageKind::StoredType:
      return true;

    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
    case StorageKind::None:
      return false;
    }

  case Parent:
    switch (storageKind) {
    case StorageKind::AssociatedTypeDecl:
      return true;

    case StorageKind::StoredType:
    case StorageKind::ProtocolConformance:
    case StorageKind::None:
      return false;
    }

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    switch (storageKind) {
    case StorageKind::StoredType:
      return true;

    case StorageKind::ProtocolConformance:
    case StorageKind::AssociatedTypeDecl:
    case StorageKind::None:
      return false;
    }

  case Superclass:
  case Concrete:
    switch (storageKind) {
    case StorageKind::StoredType:
    case StorageKind::ProtocolConformance:
      return true;

    case StorageKind::AssociatedTypeDecl:
    case StorageKind::None:
      return false;
    }
  }

  llvm_unreachable("Unhandled RequirementSourceKind in switch.");
}
#endif

const void *RequirementSource::getOpaqueStorage1() const {
  switch (storageKind) {
  case StorageKind::None:
    return nullptr;

  case StorageKind::ProtocolConformance:
    return storage.conformance;

  case StorageKind::StoredType:
    return storage.type;

  case StorageKind::AssociatedTypeDecl:
    return storage.assocType;
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

const void *RequirementSource::getOpaqueStorage2() const {
  if (numTrailingObjects(OverloadToken<ProtocolDecl *>()) == 1)
    return getTrailingObjects<ProtocolDecl *>()[0];
  if (numTrailingObjects(OverloadToken<SourceLoc>()) == 1)
    return getTrailingObjects<SourceLoc>()[0].getOpaquePointerValue();

  return nullptr;
}

const void *RequirementSource::getOpaqueStorage3() const {
  if (numTrailingObjects(OverloadToken<ProtocolDecl *>()) == 1 &&
      numTrailingObjects(OverloadToken<SourceLoc>()) == 1)
    return getTrailingObjects<SourceLoc>()[0].getOpaquePointerValue();

  return nullptr;
}

bool RequirementSource::isInferredRequirement() const {
  for (auto source = this; source; source = source->parent) {
    switch (source->kind) {
    case Inferred:
    case InferredProtocolRequirement:
    case NestedTypeNameMatch:
      return true;

    case EquivalentType:
      return false;

    case Concrete:
    case Explicit:
    case Parent:
    case ProtocolRequirement:
    case RequirementSignatureSelf:
    case Superclass:
    case Layout:
      break;
    }
  }

  return false;
}

unsigned RequirementSource::classifyDiagKind() const {
  if (isInferredRequirement()) return 2;
  if (isDerivedRequirement()) return 1;
  return 0;
}

bool RequirementSource::isDerivedRequirement() const {
  switch (kind) {
  case Explicit:
  case Inferred:
    return false;

  case NestedTypeNameMatch:
  case Parent:
  case Superclass:
  case Concrete:
  case RequirementSignatureSelf:
  case Layout:
  case EquivalentType:
    return true;

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    // Requirements based on protocol requirements are derived unless they are
    // direct children of the requirement-signature source, in which case we
    // need to keep them for the requirement signature.
    return parent->kind != RequirementSignatureSelf;
  }

  llvm_unreachable("Unhandled RequirementSourceKind in switch.");
}

bool RequirementSource::isDerivedNonRootRequirement() const {
  return (isDerivedRequirement() &&
          kind != RequirementSource::RequirementSignatureSelf);
}

bool RequirementSource::shouldDiagnoseRedundancy(bool primary) const {
  return !isInferredRequirement() && getLoc().isValid() &&
         (!primary || !isDerivedRequirement());
}

bool RequirementSource::isSelfDerivedSource(GenericSignatureBuilder &builder,
                                            Type type) const {
  return getMinimalConformanceSource(builder, type, /*proto=*/nullptr)
    != this;
}

/// Replace 'Self' in the given dependent type (\c depTy) with the given
/// dependent type, producing a type that refers to
/// the nested type. This limited operation makes sure that it does not
/// create any new potential archetypes along the way, so it should only be
/// used in cases where we're reconstructing something that we know exists.
static Type replaceSelfWithType(Type selfType, Type depTy) {
  if (auto depMemTy = depTy->getAs<DependentMemberType>()) {
    Type baseType = replaceSelfWithType(selfType, depMemTy->getBase());
    assert(depMemTy->getAssocType() && "Missing associated type");
    return DependentMemberType::get(baseType, depMemTy->getAssocType());
  }

  assert(depTy->is<GenericTypeParamType>() && "missing Self?");
  return selfType;
}

/// Determine whether the given protocol requirement is self-derived when it
/// occurs within the requirement signature of its own protocol.
static bool isSelfDerivedProtocolRequirementInProtocol(
                                             const RequirementSource *source,
                                             const ProtocolDecl *selfProto,
                                             GenericSignatureBuilder &builder) {
  assert(source->isProtocolRequirement());

  // This can only happen if the requirement points comes from the protocol
  // itself.
  if (source->getProtocolDecl() != selfProto) return false;

  // This only applies if the parent is not the anchor for computing the
  // requirement signature. Anywhere else, we can use the protocol requirement.
  if (source->parent->kind == RequirementSource::RequirementSignatureSelf)
    return false;

  // If the relative type of the protocol requirement itself is in the
  // same equivalence class as what we've proven with this requirement,
  // it's a self-derived requirement.
  return
    builder.resolveEquivalenceClass(source->getAffectedType(),
                                    ArchetypeResolutionKind::WellFormed) ==
      builder.resolveEquivalenceClass(source->getStoredType(),
                                      ArchetypeResolutionKind::AlreadyKnown);
}

const RequirementSource *RequirementSource::getMinimalConformanceSource(
                                             GenericSignatureBuilder &builder,
                                             Type currentType,
                                             ProtocolDecl *proto) const {
  // If it's not a derived requirement, it's not self-derived.
  if (!isDerivedRequirement()) return this;

  /// Keep track of all of the requirements we've seen along the way. If
  /// we see the same requirement twice, we have found a shorter path.
  llvm::DenseMap<std::pair<EquivalenceClass *, ProtocolDecl *>,
                 const RequirementSource *>
    constraintsSeen;

  /// Note that we've now seen a new constraint (described on an equivalence
  /// class).
  auto addConstraint = [&](EquivalenceClass *equivClass, ProtocolDecl *proto,
                           const RequirementSource *source)
      -> const RequirementSource * {
    auto &storedSource = constraintsSeen[{equivClass, proto}];
    if (storedSource) return storedSource;

    storedSource = source;
    return nullptr;
  };

  // Note that we've now seen a new constraint, returning true if we've seen
  // it before.
  auto addTypeConstraint = [&](Type type, ProtocolDecl *proto,
                           const RequirementSource *source)
      -> const RequirementSource * {
    auto equivClass =
        builder.resolveEquivalenceClass(type,
                                        ArchetypeResolutionKind::WellFormed);
    assert(equivClass && "Not a well-formed type?");
    return addConstraint(equivClass, proto, source);
  };

  const ProtocolDecl *requirementSignatureSelfProto = nullptr;

  Optional<std::pair<const RequirementSource *, const RequirementSource *>>
    redundantSubpath;
  bool isSelfDerived = visitPotentialArchetypesAlongPath(
          [&](Type parentType, const RequirementSource *source) {
    switch (source->kind) {
    case ProtocolRequirement:
    case InferredProtocolRequirement: {
      // If the base has been made concrete, note it.
      auto parentEquivClass =
          builder.resolveEquivalenceClass(parentType,
                                          ArchetypeResolutionKind::WellFormed);
      assert(parentEquivClass && "Not a well-formed type?");

      // The parent potential archetype must conform to the protocol in which
      // this requirement resides. Add this constraint.
      if (auto startOfPath =
              addConstraint(parentEquivClass, source->getProtocolDecl(),
                            source->parent)) {
        // We found a redundant subpath; record it and stop the algorithm.
        assert(startOfPath != source->parent);
        redundantSubpath = { startOfPath, source->parent };
        return true;
      }

      // If this is a self-derived protocol requirement, fail.
      if (requirementSignatureSelfProto &&
          isSelfDerivedProtocolRequirementInProtocol(
                                               source,
                                               requirementSignatureSelfProto,
                                               builder)) {
        redundantSubpath = { source->getRoot(), source->parent };
        return true;
      }

      // No redundancy thus far.
      return false;
    }

    case Parent:
      // FIXME: Ad hoc detection of recursive same-type constraints.
      return !proto &&
        builder.areInSameEquivalenceClass(parentType, currentType);

    case Concrete:
    case Superclass:
    case Layout:
    case EquivalentType:
      return false;

    case RequirementSignatureSelf:
      // Note the protocol whose requirement signature the requirement is
      // based on.
      requirementSignatureSelfProto = source->getProtocolDecl();
      LLVM_FALLTHROUGH;

    case Explicit:
    case Inferred:
    case NestedTypeNameMatch:
      return false;
    }
    llvm_unreachable("unhandled kind");
  }).isNull();

  // If we didn't already find a redundancy, check our end state.
  if (!redundantSubpath && proto) {
    if (auto startOfPath = addTypeConstraint(currentType, proto, this)) {
      redundantSubpath = { startOfPath, this };
      assert(startOfPath != this);
      isSelfDerived = true;
    }
  }

  // If we saw a constraint twice, it's self-derived.
  if (redundantSubpath) {
    assert(isSelfDerived && "Not considered self-derived?");
    auto shorterSource =
      withoutRedundantSubpath(builder,
                              redundantSubpath->first,
                              redundantSubpath->second);
    return shorterSource
      ->getMinimalConformanceSource(builder, currentType, proto);
  }

  // It's self-derived but we don't have a redundant subpath to eliminate.
  if (isSelfDerived)
    return nullptr;

  return this;
}

#define REQUIREMENT_SOURCE_FACTORY_BODY(ProfileArgs, ConstructorArgs,      \
                                        NumProtocolDecls, WrittenReq)      \
  llvm::FoldingSetNodeID nodeID;                                           \
  Profile ProfileArgs;                                                     \
                                                                           \
  void *insertPos = nullptr;                                               \
  if (auto known =                                                         \
        builder.Impl->RequirementSources.FindNodeOrInsertPos(nodeID,       \
                                                             insertPos))   \
    return known;                                                          \
                                                                           \
  unsigned size =                                                          \
    totalSizeToAlloc<ProtocolDecl *, SourceLoc>(               \
                                           NumProtocolDecls,               \
                                           WrittenReq.isValid()? 1 : 0);   \
  void *mem =                                                              \
    builder.Impl->Allocator.Allocate(size, alignof(RequirementSource));    \
  auto result = new (mem) RequirementSource ConstructorArgs;               \
  builder.Impl->RequirementSources.InsertNode(result, insertPos);          \
  return result

const RequirementSource *RequirementSource::forAbstract(
                                            GenericSignatureBuilder &builder,
                                            Type rootType) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Explicit, nullptr, rootType.getPointer(),
                         nullptr, nullptr),
                        (Explicit, rootType, nullptr, SourceLoc()),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::forExplicit(
                  GenericSignatureBuilder &builder,
                  Type rootType, SourceLoc writtenLoc) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Explicit, nullptr, rootType.getPointer(),
                         writtenLoc.getOpaquePointerValue(), nullptr),
                        (Explicit, rootType, nullptr, writtenLoc),
                        0, writtenLoc);
}

const RequirementSource *RequirementSource::forInferred(
                                              GenericSignatureBuilder &builder,
                                              Type rootType,
                                              SourceLoc writtenLoc) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
      (nodeID, Inferred, nullptr, rootType.getPointer(),
       writtenLoc.getOpaquePointerValue(), nullptr),
       (Inferred, rootType, nullptr, writtenLoc),
       0, writtenLoc);
}

const RequirementSource *RequirementSource::forRequirementSignature(
                                              GenericSignatureBuilder &builder,
                                              Type rootType,
                                              ProtocolDecl *protocol) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, RequirementSignatureSelf, nullptr,
                         rootType.getPointer(), protocol, nullptr),
                        (RequirementSignatureSelf, rootType, protocol,
                         SourceLoc()),
                        1, SourceLoc());

}

const RequirementSource *RequirementSource::forNestedTypeNameMatch(
                                             GenericSignatureBuilder &builder,
                                             Type rootType) {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, NestedTypeNameMatch, nullptr,
                         rootType.getPointer(), nullptr, nullptr),
                        (NestedTypeNameMatch, rootType, nullptr,
                         SourceLoc()),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::viaProtocolRequirement(
            GenericSignatureBuilder &builder, Type dependentType,
            ProtocolDecl *protocol, bool inferred, SourceLoc writtenLoc) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID,
                         inferred ? InferredProtocolRequirement
                                  : ProtocolRequirement,
                         this,
                         dependentType.getPointer(), protocol,
                         writtenLoc.getOpaquePointerValue()),
                        (inferred ? InferredProtocolRequirement
                                  : ProtocolRequirement,
                         this, dependentType,
                         protocol, writtenLoc),
                        1, writtenLoc);
}

const RequirementSource *RequirementSource::viaSuperclass(
                                    GenericSignatureBuilder &builder,
                                    ProtocolConformanceRef conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Superclass, this, conformance.getOpaqueValue(),
                         nullptr, nullptr),
                        (Superclass, this, conformance),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::viaConcrete(
                                    GenericSignatureBuilder &builder,
                                    ProtocolConformanceRef conformance) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Concrete, this, conformance.getOpaqueValue(),
                         nullptr, nullptr),
                        (Concrete, this, conformance),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::viaConcrete(
                                    GenericSignatureBuilder &builder,
                                    Type existentialType) const {
  assert(existentialType->isExistentialType());
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Concrete, this, existentialType.getPointer(),
                         nullptr, nullptr),
                        (Concrete, this, existentialType),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::viaParent(
                                      GenericSignatureBuilder &builder,
                                      AssociatedTypeDecl *assocType) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Parent, this, assocType, nullptr, nullptr),
                        (Parent, this, assocType),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::viaLayout(
                           GenericSignatureBuilder &builder,
                           Type superclass) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, Layout, this, superclass.getPointer(),
                         nullptr, nullptr),
                        (Layout, this, superclass),
                        0, SourceLoc());
}

const RequirementSource *RequirementSource::viaEquivalentType(
                                           GenericSignatureBuilder &builder,
                                           Type newType) const {
  REQUIREMENT_SOURCE_FACTORY_BODY(
                        (nodeID, EquivalentType, this, newType.getPointer(),
                         nullptr, nullptr),
                        (EquivalentType, this, newType),
                        0, SourceLoc());
}

#undef REQUIREMENT_SOURCE_FACTORY_BODY

const RequirementSource *RequirementSource::withoutRedundantSubpath(
                                        GenericSignatureBuilder &builder,
                                        const RequirementSource *start,
                                        const RequirementSource *end) const {
  // Replace the end with the start; the caller has guaranteed that they
  // produce the same thing.
  if (this == end) {
#ifndef NDEBUG
    // Sanity check: make sure the 'start' precedes the 'end'.
    bool foundStart = false;
    for (auto source = this; source; source = source->parent) {
      if (source == start) {
        foundStart = true;
        break;
      }
    }
    assert(foundStart && "Start doesn't precede end!");
#endif
    return start;
  }

  switch (kind) {
  case Explicit:
  case Inferred:
  case RequirementSignatureSelf:
  case NestedTypeNameMatch:
    llvm_unreachable("Subpath end doesn't occur within path");

  case ProtocolRequirement:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaProtocolRequirement(builder, getStoredType(),
                               getProtocolDecl(), /*inferred=*/false,
                               getSourceLoc());

  case InferredProtocolRequirement:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaProtocolRequirement(builder, getStoredType(),
                               getProtocolDecl(), /*inferred=*/true,
                               getSourceLoc());

  case Concrete:
    if (auto existentialType = getStoredType()) {
      assert(existentialType->isExistentialType());
      return parent->withoutRedundantSubpath(builder, start, end)
        ->viaConcrete(builder, existentialType);
    } else {
      return parent->withoutRedundantSubpath(builder, start, end)
        ->viaConcrete(builder, getProtocolConformance());
    }

  case Layout:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaLayout(builder, getStoredType());

  case EquivalentType:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaEquivalentType(builder, Type(storage.type));

  case Parent:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaParent(builder, getAssociatedType());

  case Superclass:
    return parent->withoutRedundantSubpath(builder, start, end)
      ->viaSuperclass(builder, getProtocolConformance());
  }
  llvm_unreachable("unhandled kind");
}

const RequirementSource *RequirementSource::getRoot() const {
  auto root = this;
  while (auto parent = root->parent)
    root = parent;
  return root;
}

Type RequirementSource::getRootType() const {
  /// Find the root.
  auto root = getRoot();

  // We're at the root, so it's in the inline storage.
  assert(root->storageKind == StorageKind::StoredType);
  return Type(root->storage.type);
}

Type RequirementSource::getAffectedType() const {
  return visitPotentialArchetypesAlongPath(
                         [](Type, const RequirementSource *) {
                           return false;
                         });
}

Type
RequirementSource::visitPotentialArchetypesAlongPath(
     llvm::function_ref<bool(Type, const RequirementSource *)> visitor) const {
  switch (kind) {
  case RequirementSource::Parent: {
    Type parentType = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentType) return nullptr;

    if (visitor(parentType, this)) return nullptr;

    return replaceSelfWithType(parentType,
                               getAssociatedType()->getDeclaredInterfaceType());
  }

  case RequirementSource::NestedTypeNameMatch:
  case RequirementSource::Explicit:
  case RequirementSource::Inferred:
  case RequirementSource::RequirementSignatureSelf: {
    Type rootType = getRootType();
    if (visitor(rootType, this)) return nullptr;

    return rootType;
  }

  case RequirementSource::Concrete:
  case RequirementSource::Superclass:
  case RequirementSource::Layout:
    return parent->visitPotentialArchetypesAlongPath(visitor);

  case RequirementSource::EquivalentType: {
    auto parentType = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentType) return nullptr;

    if (visitor(parentType, this)) return nullptr;

    return Type(storage.type);
  }

  case RequirementSource::ProtocolRequirement:
  case RequirementSource::InferredProtocolRequirement: {
    Type parentType = parent->visitPotentialArchetypesAlongPath(visitor);
    if (!parentType) return nullptr;

    if (visitor(parentType, this)) return nullptr;

    return replaceSelfWithType(parentType, getStoredType());
  }
  }
  llvm_unreachable("unhandled kind");
}

Type RequirementSource::getStoredType() const {
  switch (storageKind) {
  case StorageKind::None:
  case StorageKind::ProtocolConformance:
  case StorageKind::AssociatedTypeDecl:
    return Type();

  case StorageKind::StoredType:
    return storage.type;
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

ProtocolDecl *RequirementSource::getProtocolDecl() const {
  switch (storageKind) {
  case StorageKind::None:
    return nullptr;

  case StorageKind::StoredType:
    if (isProtocolRequirement() || kind == RequirementSignatureSelf)
      return getTrailingObjects<ProtocolDecl *>()[0];
    return nullptr;

  case StorageKind::ProtocolConformance:
    return getProtocolConformance().getRequirement();

  case StorageKind::AssociatedTypeDecl:
    return storage.assocType->getProtocol();
  }

  llvm_unreachable("Unhandled StorageKind in switch.");
}

SourceLoc RequirementSource::getLoc() const {
  // Don't produce locations for protocol requirements unless the parent is
  // the protocol self.
  // FIXME: We should have a better notion of when to emit diagnostics
  // for a particular requirement, rather than turning on/off location info.
  // Locations that fall into this category should be advisory, emitted via
  // notes rather than as the normal location.
  if (isProtocolRequirement() && parent &&
      parent->kind != RequirementSignatureSelf)
    return parent->getLoc();

  auto loc = getSourceLoc();
  if (loc.isValid())
    return loc;

  if (parent)
    return parent->getLoc();

  if (kind == RequirementSignatureSelf)
    return getProtocolDecl()->getLoc();

  return SourceLoc();
}

/// Compute the path length of a requirement source, counting only the number
/// of \c ProtocolRequirement elements.
static unsigned sourcePathLength(const RequirementSource *source) {
  unsigned count = 0;
  for (; source; source = source->parent) {
    if (source->isProtocolRequirement())
      ++count;
  }
  return count;
}

int RequirementSource::compare(const RequirementSource *other) const {
  // Prefer the derived option, if there is one.
  bool thisIsDerived = this->isDerivedRequirement();
  bool otherIsDerived = other->isDerivedRequirement();
  if (thisIsDerived != otherIsDerived)
    return thisIsDerived ? -1 : +1;

  // Prefer the shorter path.
  unsigned thisLength = sourcePathLength(this);
  unsigned otherLength = sourcePathLength(other);
  if (thisLength != otherLength)
    return thisLength < otherLength ? -1 : +1;

  // FIXME: Arbitrary hack to allow later requirement sources to stomp on
  // earlier ones. We need a proper ordering here.
  return +1;
}

void RequirementSource::dump() const {
  dump(llvm::errs(), nullptr, 0);
  llvm::errs() << "\n";
}

/// Dump the constraint source.
void RequirementSource::dump(llvm::raw_ostream &out, SourceManager *srcMgr,
                             unsigned indent) const {
  // FIXME: Implement for real, so we actually dump the structure.
  out.indent(indent);
  print(out, srcMgr);
}

void RequirementSource::print() const {
  print(llvm::errs(), nullptr);
}

void RequirementSource::print(llvm::raw_ostream &out,
                              SourceManager *srcMgr) const {
  if (parent) {
    parent->print(out, srcMgr);
    out << " -> ";
  } else {
    out << getRootType().getString() << ": ";
  }

  switch (kind) {
  case Concrete:
    out << "Concrete";
    break;

  case Explicit:
    out << "Explicit";
    break;

  case Inferred:
    out << "Inferred";
    break;

  case NestedTypeNameMatch:
    out << "Nested type match";
    break;

  case Parent:
    out << "Parent";
    break;

  case ProtocolRequirement:
    out << "Protocol requirement";
    break;

  case InferredProtocolRequirement:
    out << "Inferred protocol requirement";
    break;

  case RequirementSignatureSelf:
    out << "Requirement signature self";
    break;

  case Superclass:
    out << "Superclass";
    break;

  case Layout:
    out << "Layout";
    break;

  case EquivalentType:
    out << "Equivalent type";
    break;
  }

  // Local function to dump a source location, if we can.
  auto dumpSourceLoc = [&](SourceLoc loc) {
    if (!srcMgr) return;
    if (loc.isInvalid()) return;

    unsigned bufferID = srcMgr->findBufferContainingLoc(loc);

    auto lineAndCol = srcMgr->getPresumedLineAndColumnForLoc(loc, bufferID);
    out << " @ " << lineAndCol.first << ':' << lineAndCol.second;
  };

  switch (storageKind) {
  case StorageKind::None:
    break;

  case StorageKind::StoredType:
    if (auto proto = getProtocolDecl()) {
      out << " (via " << storage.type->getString() << " in " << proto->getName()
          << ")";
    }
    break;

  case StorageKind::ProtocolConformance: {
    auto conformance = getProtocolConformance();
    if (conformance.isConcrete()) {
      out << " (" << conformance.getConcrete()->getType()->getString() << ": "
          << conformance.getConcrete()->getProtocol()->getName() << ")";
    } else {
      out << " (abstract " << conformance.getRequirement()->getName() << ")";
    }
    break;
  }

  case StorageKind::AssociatedTypeDecl:
    out << " (" << storage.assocType->getProtocol()->getName()
        << "::" << storage.assocType->getName() << ")";
    break;
  }

  if (auto loc = getLoc()) {
    dumpSourceLoc(loc);
  }
}

/// Form the dependent type such that the given protocol's \c Self can be
/// replaced by \c baseType to reach \c type.
static Type formProtocolRelativeType(ProtocolDecl *proto,
                                     Type baseType,
                                     Type type) {
  // Error case: hand back the erroneous type.
  if (type->hasError())
    return type;

  // Basis case: we've hit the base potential archetype.
  if (baseType->isEqual(type))
    return proto->getSelfInterfaceType();

  // Recursive case: form a dependent member type.
  auto depMemTy = type->castTo<DependentMemberType>();
  Type newBaseType = formProtocolRelativeType(proto, baseType,
                                              depMemTy->getBase());
  auto assocType = depMemTy->getAssocType();
  return DependentMemberType::get(newBaseType, assocType);
}

const RequirementSource *FloatingRequirementSource::getSource(
                                              GenericSignatureBuilder &builder,
                                              ResolvedType type) const {
  switch (kind) {
  case Resolved:
    return source;

  case Explicit: {
    auto depType = type.getDependentType(builder);
    return RequirementSource::forExplicit(builder, depType, loc);
  }

  case Inferred: {
    auto depType = type.getDependentType(builder);
    return RequirementSource::forInferred(builder, depType, loc);
  }

  case ProtocolRequirement:
  case InferredProtocolRequirement: {
    auto depType = type.getDependentType();

    // Derive the dependent type on which this requirement was written. It is
    // the path from the requirement source on which this requirement is based
    // to the potential archetype on which the requirement is being placed.
    auto baseSourceType = source->getAffectedType();

    auto dependentType =
      formProtocolRelativeType(protocol, baseSourceType, depType);

    return source
      ->viaProtocolRequirement(builder, dependentType, protocol,
                               kind == InferredProtocolRequirement,
                               loc);
  }

  case NestedTypeNameMatch: {
    auto depType = type.getDependentType(builder);
    return RequirementSource::forNestedTypeNameMatch(builder, depType);
  }
  }

  llvm_unreachable("unhandled kind");
}

SourceLoc FloatingRequirementSource::getLoc() const {
  if (kind == Inferred || isExplicit()) {
    if (loc.isValid())
      return loc;
  }

  if (source)
    return source->getLoc();

  return SourceLoc();
}

bool FloatingRequirementSource::isDerived() const {
  switch (kind) {
  case Explicit:
  case Inferred:
  case NestedTypeNameMatch:
    return false;

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    switch (source->kind) {
    case RequirementSource::RequirementSignatureSelf:
      return false;

    case RequirementSource::Concrete:
    case RequirementSource::Explicit:
    case RequirementSource::Inferred:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Parent:
    case RequirementSource::ProtocolRequirement:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::Superclass:
    case RequirementSource::Layout:
    case RequirementSource::EquivalentType:
      return true;
    }

  case Resolved:
    return source->isDerivedRequirement();
  }
  llvm_unreachable("unhandled kind");
}

bool FloatingRequirementSource::isExplicit() const {
  switch (kind) {
  case Explicit:
    return true;

  case Inferred:
  case NestedTypeNameMatch:
    return false;

  case ProtocolRequirement:
    // Requirements implied by other protocol conformance requirements are
    // implicit, except when computing a requirement signature, where
    // non-inferred ones are explicit, to allow flagging of redundant
    // requirements.
    switch (source->kind) {
    case RequirementSource::RequirementSignatureSelf:
      return true;

    case RequirementSource::Concrete:
    case RequirementSource::Explicit:
    case RequirementSource::Inferred:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Parent:
    case RequirementSource::ProtocolRequirement:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::Superclass:
    case RequirementSource::Layout:
    case RequirementSource::EquivalentType:
      return false;
    }

  case InferredProtocolRequirement:
    return false;

  case Resolved:
    switch (source->kind) {
    case RequirementSource::Explicit:
      return true;

    case RequirementSource::ProtocolRequirement:
      return source->parent->kind == RequirementSource::RequirementSignatureSelf;

    case RequirementSource::Inferred:
    case RequirementSource::InferredProtocolRequirement:
    case RequirementSource::RequirementSignatureSelf:
    case RequirementSource::Concrete:
    case RequirementSource::NestedTypeNameMatch:
    case RequirementSource::Parent:
    case RequirementSource::Superclass:
    case RequirementSource::Layout:
    case RequirementSource::EquivalentType:
      return false;
    }
  }
  llvm_unreachable("unhandled kind");
}


FloatingRequirementSource FloatingRequirementSource::asInferred(
                                          const TypeRepr *typeRepr) const {
  auto loc = typeRepr ? typeRepr->getStartLoc() : SourceLoc();
  switch (kind) {
  case Explicit:
    return forInferred(loc);

  case Inferred:
  case Resolved:
  case NestedTypeNameMatch:
    return *this;

  case ProtocolRequirement:
  case InferredProtocolRequirement:
    return viaProtocolRequirement(source, protocol, loc, /*inferred=*/true);
  }
  llvm_unreachable("unhandled kind");
}

bool FloatingRequirementSource::isRecursive(
                                    GenericSignatureBuilder &builder) const {
  llvm::SmallSet<std::pair<CanType, ProtocolDecl *>, 32> visitedAssocReqs;
  for (auto storedSource = source; storedSource;
       storedSource = storedSource->parent) {
    // FIXME: isRecursive() is completely misnamed
    if (storedSource->kind == RequirementSource::EquivalentType)
      return true;

    if (!storedSource->isProtocolRequirement())
      continue;

    if (!visitedAssocReqs.insert(
                          {storedSource->getStoredType()->getCanonicalType(),
                           storedSource->getProtocolDecl()}).second)
      return true;
  }

  return false;
}

GenericSignatureBuilder::PotentialArchetype::PotentialArchetype(
    PotentialArchetype *parent, AssociatedTypeDecl *assocType)
    : parent(parent) {
  ++NumPotentialArchetypes;
  assert(parent != nullptr && "Not a nested type?");
  assert(assocType->getOverriddenDecls().empty());
  depType = CanDependentMemberType::get(parent->getDependentType(), assocType);
}


GenericSignatureBuilder::PotentialArchetype::PotentialArchetype(
    GenericTypeParamType *genericParam)
    : parent(nullptr) {
  ++NumPotentialArchetypes;
  depType = genericParam->getCanonicalType();
}

GenericSignatureBuilder::PotentialArchetype::~PotentialArchetype() {
  for (const auto &nested : NestedTypes) {
    for (auto pa : nested.second) {
      pa->~PotentialArchetype();
    }
  }
}

std::string GenericSignatureBuilder::PotentialArchetype::getDebugName() const {
  llvm::SmallString<64> result;

  auto parent = getParent();
  if (!parent) {
    return depType.getString();
  }

  // Nested types.
  result += parent->getDebugName();

  // When building the name for debugging purposes, include the protocol into
  // which the associated type or type alias was resolved.
  auto *proto = getResolvedType()->getProtocol();

  result.push_back('[');
  result.push_back('.');
  result.append(proto->getName().str());
  result.push_back(']');

  result.push_back('.');
  result.append(getResolvedType()->getName().str());

  return result.str().str();
}

unsigned GenericSignatureBuilder::PotentialArchetype::getNestingDepth() const {
  unsigned Depth = 0;
  for (auto P = getParent(); P; P = P->getParent())
    ++Depth;
  return Depth;
}

void EquivalenceClass::addMember(PotentialArchetype *pa) {
  assert(find(members, pa) == members.end() &&
         "Already have this potential archetype!");
  members.push_back(pa);
  if (members.back()->getNestingDepth() < members.front()->getNestingDepth()) {
    MutableArrayRef<PotentialArchetype *> mutMembers = members;
    std::swap(mutMembers.front(), mutMembers.back());
  }
}

bool EquivalenceClass::recordConformanceConstraint(
                                 GenericSignatureBuilder &builder,
                                 ResolvedType type,
                                 ProtocolDecl *proto,
                                 const RequirementSource *source) {
  // If we haven't seen a conformance to this protocol yet, add it.
  bool inserted = false;
  auto known = conformsTo.find(proto);
  if (known == conformsTo.end()) {
    known = conformsTo.insert({ proto, { }}).first;
    inserted = true;
    modified(builder);
    ++NumConformances;

    // If there is a concrete type that resolves this conformance requirement,
    // record the conformance.
    if (!builder.resolveConcreteConformance(type, proto)) {
      // Otherwise, determine whether there is a superclass constraint where the
      // superclass conforms to this protocol.
      (void)builder.resolveSuperConformance(type, proto);
    }
  }

  // Record this conformance source.
  known->second.push_back({type.getUnresolvedType(), proto, source});
  ++NumConformanceConstraints;

  return inserted;
}

Optional<ConcreteConstraint>
EquivalenceClass::findAnyConcreteConstraintAsWritten(Type preferredType) const {
  // If we don't have a concrete type, there's no source.
  if (!concreteType) return None;

  // Go look for a source with source-location information.
  Optional<ConcreteConstraint> result;
  for (const auto &constraint : concreteTypeConstraints) {
    if (constraint.source->getLoc().isValid()) {
      result = constraint;
      if (!preferredType ||
          constraint.getSubjectDependentType({ })->isEqual(preferredType))
        return result;
    }
  }

  return result;
}

Optional<ConcreteConstraint>
EquivalenceClass::findAnySuperclassConstraintAsWritten(
                                                   Type preferredType) const {
  // If we don't have a superclass, there's no source.
  if (!superclass) return None;

  // Go look for a source with source-location information.
  Optional<ConcreteConstraint> result;
  for (const auto &constraint : superclassConstraints) {
    if (constraint.source->getLoc().isValid() &&
        constraint.value->isEqual(superclass)) {
      result = constraint;

      if (!preferredType ||
          constraint.getSubjectDependentType({ })->isEqual(preferredType))
        return result;
    }
  }

  return result;
}

bool EquivalenceClass::isConformanceSatisfiedBySuperclass(
                                                    ProtocolDecl *proto) const {
  auto known = conformsTo.find(proto);
  assert(known != conformsTo.end() && "doesn't conform to this protocol");
  for (const auto &constraint: known->second) {
    if (constraint.source->kind == RequirementSource::Superclass)
      return true;
  }

  return false;
}

/// Compare two associated types.
static int compareAssociatedTypes(AssociatedTypeDecl *assocType1,
                                  AssociatedTypeDecl *assocType2) {
  // - by name.
  if (int result = assocType1->getName().str().compare(
                                              assocType2->getName().str()))
    return result;

  // Prefer an associated type with no overrides (i.e., an anchor) to one
  // that has overrides.
  bool hasOverridden1 = !assocType1->getOverriddenDecls().empty();
  bool hasOverridden2 = !assocType2->getOverriddenDecls().empty();
  if (hasOverridden1 != hasOverridden2)
    return hasOverridden1 ? +1 : -1;

  // - by protocol, so t_n_m.`P.T` < t_n_m.`Q.T` (given P < Q)
  auto proto1 = assocType1->getProtocol();
  auto proto2 = assocType2->getProtocol();
  if (int compareProtocols = TypeDecl::compare(proto1, proto2))
    return compareProtocols;

  // Error case: if we have two associated types with the same name in the
  // same protocol, just tie-break based on address.
  if (assocType1 != assocType2)
    return assocType1 < assocType2 ? -1 : +1;

  return 0;
}

static void lookupConcreteNestedType(NominalTypeDecl *decl,
                                     Identifier name,
                                     SmallVectorImpl<TypeDecl *> &concreteDecls) {
  SmallVector<ValueDecl *, 2> foundMembers;
  decl->getParentModule()->lookupQualified(
      decl, DeclNameRef(name),
      NL_QualifiedDefault | NL_OnlyTypes | NL_ProtocolMembers,
      foundMembers);
  for (auto member : foundMembers)
    concreteDecls.push_back(cast<TypeDecl>(member));
}

static auto findBestConcreteNestedType(SmallVectorImpl<TypeDecl *> &concreteDecls) {
  return std::min_element(concreteDecls.begin(), concreteDecls.end(),
                          [](TypeDecl *type1, TypeDecl *type2) {
                            return TypeDecl::compare(type1, type2) < 0;
                          });
}

TypeDecl *EquivalenceClass::lookupNestedType(
                             GenericSignatureBuilder &builder,
                             Identifier name,
                             SmallVectorImpl<TypeDecl *> *otherConcreteTypes) {
  // Populates the result structures from the given cache entry.
  auto populateResult = [&](const CachedNestedType &cache) -> TypeDecl * {
    if (otherConcreteTypes)
      otherConcreteTypes->clear();

    // If there aren't any types in the cache, we're done.
    if (cache.types.empty()) return nullptr;

    // The first type in the cache is always the final result.
    // Collect the rest in the concrete-declarations list, if needed.
    if (otherConcreteTypes) {
      for (auto type : ArrayRef<TypeDecl *>(cache.types).slice(1)) {
        otherConcreteTypes->push_back(type);
      }
    }

    return cache.types.front();
  };

  // If we have a cached value that is up-to-date, use that.
  auto cached = nestedTypeNameCache.find(name);
  if (cached != nestedTypeNameCache.end() &&
      cached->second.numConformancesPresent == conformsTo.size() &&
      (!superclass ||
       cached->second.superclassPresent == superclass->getCanonicalType()) &&
      (!concreteType ||
        cached->second.concreteTypePresent == concreteType->getCanonicalType())) {
    ++NumNestedTypeCacheHits;
    return populateResult(cached->second);
  }

  // Cache miss; go compute the result.
  ++NumNestedTypeCacheMisses;

  // Look for types with the given name in protocols that we know about.
  AssociatedTypeDecl *bestAssocType = nullptr;
  SmallVector<TypeDecl *, 4> concreteDecls;
  for (const auto &conforms : conformsTo) {
    ProtocolDecl *proto = conforms.first;

    // Look for an associated type and/or concrete type with this name.
    for (auto member : proto->lookupDirect(name)) {
      // If this is an associated type, record whether it is the best
      // associated type we've seen thus far.
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        // Retrieve the associated type anchor.
        assocType = assocType->getAssociatedTypeAnchor();

        if (!bestAssocType ||
             compareAssociatedTypes(assocType, bestAssocType) < 0)
          bestAssocType = assocType;

        continue;
      }

      // If this is another type declaration, record it.
      if (auto type = dyn_cast<TypeDecl>(member)) {
        concreteDecls.push_back(type);
        continue;
      }
    }
  }

  // If we haven't found anything yet but have a concrete type or a superclass,
  // look for a type in that.
  // FIXME: Shouldn't we always look here?
  if (!bestAssocType && concreteDecls.empty()) {
    Type typeToSearch = concreteType ? concreteType : superclass;
    if (typeToSearch)
      if (auto *decl = typeToSearch->getAnyNominal())
        lookupConcreteNestedType(decl, name, concreteDecls);
  }

  // Form the new cache entry.
  CachedNestedType entry;
  entry.numConformancesPresent = conformsTo.size();
  entry.superclassPresent =
    superclass ? superclass->getCanonicalType() : CanType();
  entry.concreteTypePresent =
    concreteType ? concreteType->getCanonicalType() : CanType();
  if (bestAssocType) {
    entry.types.push_back(bestAssocType);
    entry.types.insert(entry.types.end(),
                       concreteDecls.begin(), concreteDecls.end());
    assert(bestAssocType->getOverriddenDecls().empty() &&
           "Lookup should never keep a non-anchor associated type");
  } else if (!concreteDecls.empty()) {
    // Find the best concrete type.
    auto bestConcreteTypeIter = findBestConcreteNestedType(concreteDecls);

    // Put the best concrete type first; the rest will follow.
    entry.types.push_back(*bestConcreteTypeIter);
    entry.types.insert(entry.types.end(),
                       concreteDecls.begin(), bestConcreteTypeIter);
    entry.types.insert(entry.types.end(),
                       bestConcreteTypeIter + 1, concreteDecls.end());
  }

  return populateResult((nestedTypeNameCache[name] = std::move(entry)));
}

static Type getSugaredDependentType(Type type,
                                    TypeArrayView<GenericTypeParamType> params) {
  if (params.empty())
    return type;

  if (auto *gp = type->getAs<GenericTypeParamType>()) {
    unsigned index = GenericParamKey(gp).findIndexIn(params);
    return Type(params[index]);
  }

  auto *dmt = type->castTo<DependentMemberType>();
  return DependentMemberType::get(getSugaredDependentType(dmt->getBase(), params),
                                  dmt->getAssocType());
}

Type EquivalenceClass::getAnchor(
                            GenericSignatureBuilder &builder,
                            TypeArrayView<GenericTypeParamType> genericParams) {
  // Substitute into the anchor with the given generic parameters.
  auto substAnchor = [&] {
    if (genericParams.empty()) return archetypeAnchorCache.anchor;
    return getSugaredDependentType(archetypeAnchorCache.anchor, genericParams);
  };

  // Check whether the cache is valid.
  if (archetypeAnchorCache.anchor &&
      archetypeAnchorCache.lastGeneration == builder.Impl->Generation) {
    ++NumArchetypeAnchorCacheHits;
    return substAnchor();
  }

  // Check whether we already have an anchor, in which case we
  // can simplify it further.
  if (archetypeAnchorCache.anchor) {
    // Record the cache miss.
    ++NumArchetypeAnchorCacheMisses;

    // Update the anchor by simplifying it further.
    archetypeAnchorCache.anchor =
      builder.getCanonicalTypeParameter(archetypeAnchorCache.anchor);
    archetypeAnchorCache.lastGeneration = builder.Impl->Generation;
    return substAnchor();
  }

  // Record the cache miss and update the cache.
  ++NumArchetypeAnchorCacheMisses;
  archetypeAnchorCache.anchor =
    builder.getCanonicalTypeParameter(
      members.front()->getDependentType());
  archetypeAnchorCache.lastGeneration = builder.Impl->Generation;

#ifndef NDEBUG
  // All members must produce the same anchor.
  for (auto member : members) {
    auto anchorType =
      builder.getCanonicalTypeParameter(
                                    member->getDependentType());
    assert(anchorType->isEqual(archetypeAnchorCache.anchor) &&
           "Inconsistent anchor computation");
  }
#endif

  return substAnchor();
}

Type EquivalenceClass::getTypeInContext(GenericSignatureBuilder &builder,
                                        GenericEnvironment *genericEnv) {
  auto genericParams = genericEnv->getGenericParams();

  // The anchor descr
  Type anchor = getAnchor(builder, genericParams);

  // If this equivalence class is mapped to a concrete type, produce that
  // type.
  if (concreteType) {
    if (recursiveConcreteType)
      return ErrorType::get(anchor);

    // Prevent recursive substitution.
    this->recursiveConcreteType = true;
    SWIFT_DEFER {
      this->recursiveConcreteType = false;
    };

    return genericEnv->mapTypeIntoContext(concreteType,
                                          builder.getLookupConformanceFn());
  }

  // Local function to check whether we have a generic parameter that has
  // already been recorded
  auto getAlreadyRecoveredGenericParam = [&]() -> Type {
    auto genericParam = anchor->getAs<GenericTypeParamType>();
    if (!genericParam) return Type();

    auto type = genericEnv->getMappingIfPresent(genericParam);
    if (!type) return Type();

    // We already have a mapping for this generic parameter in the generic
    // environment. Return it.
    return *type;
  };

  AssociatedTypeDecl *assocType = nullptr;
  ArchetypeType *parentArchetype = nullptr;
  if (auto depMemTy = anchor->getAs<DependentMemberType>()) {
    // Resolve the equivalence class of the parent.
    auto parentEquivClass =
      builder.resolveEquivalenceClass(
                          depMemTy->getBase(),
                          ArchetypeResolutionKind::CompleteWellFormed);
    if (!parentEquivClass)
      return ErrorType::get(anchor);

    // Map the parent type into this context.
    parentArchetype =
      parentEquivClass->getTypeInContext(builder, genericEnv)
                      ->castTo<ArchetypeType>();

    // If we already have a nested type with this name, return it.
    assocType = depMemTy->getAssocType();
    if (auto nested =
          parentArchetype->getNestedTypeIfKnown(assocType->getName())) {
      return *nested;
    }

    // We will build the archetype below.
  } else if (auto result = getAlreadyRecoveredGenericParam()) {
    // Return already-contextualized generic type parameter.
    return result;
  }

  // Substitute into the superclass.
  Type superclass = this->recursiveSuperclassType ? Type() : this->superclass;
  if (superclass && superclass->hasTypeParameter()) {
    // Prevent recursive substitution.
    this->recursiveSuperclassType = true;
    SWIFT_DEFER {
      this->recursiveSuperclassType = false;
    };

    superclass = genericEnv->mapTypeIntoContext(
                                            superclass,
                                            builder.getLookupConformanceFn());
    if (superclass->is<ErrorType>())
      superclass = Type();

    // We might have recursively recorded the archetype; if so, return early.
    // FIXME: This should be detectable before we end up building archetypes.
    if (auto result = getAlreadyRecoveredGenericParam())
      return result;
  }

  // Build a new archetype.

  // Collect the protocol conformances for the archetype.
  SmallVector<ProtocolDecl *, 4> protos;
  for (const auto &conforms : conformsTo) {
    auto proto = conforms.first;

    if (!isConformanceSatisfiedBySuperclass(proto))
      protos.push_back(proto);
  }

  ArchetypeType *archetype;
  ASTContext &ctx = builder.getASTContext();
  if (parentArchetype) {
    // Create a nested archetype.
    auto *depMemTy = anchor->castTo<DependentMemberType>();
    archetype = NestedArchetypeType::getNew(ctx, parentArchetype, depMemTy,
                                            protos, superclass, layout);

    // Register this archetype with its parent.
    parentArchetype->registerNestedType(assocType->getName(), archetype);
  } else {
    // Create a top-level archetype.
    auto genericParam = anchor->castTo<GenericTypeParamType>();
    archetype = PrimaryArchetypeType::getNew(ctx, genericEnv, genericParam,
                                             protos, superclass, layout);

    // Register the archetype with the generic environment.
    genericEnv->addMapping(genericParam, archetype);
  }

  return archetype;
}

void EquivalenceClass::dump(llvm::raw_ostream &out,
                            GenericSignatureBuilder *builder) const {
  auto dumpSource = [&](const RequirementSource *source) {
    source->dump(out, &builder->getASTContext().SourceMgr, 4);
    if (source->isDerivedRequirement())
      out << " [derived]";
    out << "\n";
  };

  out << "Equivalence class represented by "
    << members.front()->getRepresentative()->getDebugName() << ":\n";
  out << "Members: ";
  interleave(members, [&](PotentialArchetype *pa) {
    out << pa->getDebugName();
  }, [&]() {
    out << ", ";
  });
  out << "\n";
  out << "Conformance constraints:\n";
  for (auto entry : conformsTo) {
    out << "  " << entry.first->getNameStr() << "\n";
    for (auto constraint : entry.second) {
      dumpSource(constraint.source);
    }
  }

  out << "Same-type constraints:\n";
  for (auto constraint : sameTypeConstraints) {
    out << "  " << constraint.getSubjectDependentType({})
        << " == " << constraint.value << "\n";
      dumpSource(constraint.source);
  }

  if (concreteType) {
    out << "Concrete type: " << concreteType.getString() << "\n";
    for (auto constraint : concreteTypeConstraints) {
      out << "  " << constraint.getSubjectDependentType({})
          << " == " << constraint.value << "\n";
      dumpSource(constraint.source);
    }
  }
  if (superclass) {
    out << "Superclass: " << superclass.getString() << "\n";
    for (auto constraint : superclassConstraints) {
      out << "  " << constraint.getSubjectDependentType({})
          << " : " << constraint.value << "\n";
      dumpSource(constraint.source);
    }
  }
  if (layout) {
    out << "Layout: " << layout.getString() << "\n";
    for (auto constraint : layoutConstraints) {
      out << "  " << constraint.getSubjectDependentType({})
          << " : " << constraint.value << "\n";
      dumpSource(constraint.source);
    }
  }

  if (!delayedRequirements.empty()) {
    out << "Delayed requirements:\n";
    for (const auto &req : delayedRequirements) {
      req.dump(out);
      out << "\n";
    }
  }

  out << "\n";

  if (builder) {
    CanType anchorType =
      const_cast<EquivalenceClass *>(this)->getAnchor(*builder, { })
        ->getCanonicalType();
    if (auto rewriteRoot =
          builder->Impl->getRewriteTreeRootIfPresent(anchorType)) {
      out << "---Rewrite tree---\n";
      rewriteRoot->dump(out);
    }
  }
}

void EquivalenceClass::dump(GenericSignatureBuilder *builder) const {
  dump(llvm::errs(), builder);
}

void DelayedRequirement::dump(llvm::raw_ostream &out) const {
  // Print LHS.
  if (auto lhsPA = lhs.dyn_cast<PotentialArchetype *>())
    out << lhsPA->getDebugName();
  else
    lhs.get<swift::Type>().print(out);

  switch (kind) {
  case Type:
  case Layout:
    out << ": ";
    break;

  case SameType:
    out << " == ";
    break;
  }

  // Print RHS.
  if (auto rhsPA = rhs.dyn_cast<PotentialArchetype *>())
    out << rhsPA->getDebugName();
  else if (auto rhsType = rhs.dyn_cast<swift::Type>())
    rhsType.print(out);
  else
    rhs.get<LayoutConstraint>().print(out);
}

void DelayedRequirement::dump() const {
  dump(llvm::errs());
  llvm::errs() << "\n";
}

ConstraintResult GenericSignatureBuilder::handleUnresolvedRequirement(
                                   RequirementKind kind,
                                   UnresolvedType lhs,
                                   UnresolvedRequirementRHS rhs,
                                   FloatingRequirementSource source,
                                   EquivalenceClass *unresolvedEquivClass,
                                   UnresolvedHandlingKind unresolvedHandling) {
  // Record the delayed requirement.
  DelayedRequirement::Kind delayedKind;
  switch (kind) {
  case RequirementKind::Conformance:
  case RequirementKind::Superclass:
    delayedKind = DelayedRequirement::Type;
    break;

  case RequirementKind::Layout:
    delayedKind = DelayedRequirement::Layout;
    break;

  case RequirementKind::SameType:
    delayedKind = DelayedRequirement::SameType;
    break;
  }

  if (unresolvedEquivClass) {
    unresolvedEquivClass->delayedRequirements.push_back(
                                          {delayedKind, lhs, rhs, source});
  } else {
    Impl->DelayedRequirements.push_back({delayedKind, lhs, rhs, source});
  }

  switch (unresolvedHandling) {
  case UnresolvedHandlingKind::GenerateConstraints:
    return ConstraintResult::Resolved;

  case UnresolvedHandlingKind::GenerateUnresolved:
    return ConstraintResult::Unresolved;
  }
  llvm_unreachable("unhandled handling");
}

void GenericSignatureBuilder::addConditionalRequirements(
    ProtocolConformanceRef conformance, ModuleDecl *inferForModule) {
  // When rebuilding a signature, don't add requirements inferred from conditional
  // conformances, since we've already added them to our ExplicitRequirements list.
  //
  // FIXME: We need to handle conditional conformances earlier, in the same place
  // as other forms of requirement inference, to eliminate this bit of state.
  if (Impl->RebuildingWithoutRedundantConformances)
    return;

  // Abstract conformances don't have associated decl-contexts/modules, but also
  // don't have conditional requirements.
  if (conformance.isConcrete()) {
    auto source = FloatingRequirementSource::forInferred(SourceLoc());
    for (auto requirement : conformance.getConditionalRequirements()) {
      addRequirement(requirement, source, inferForModule);
      ++NumConditionalRequirementsAdded;
    }
  }
}

const RequirementSource *
GenericSignatureBuilder::resolveConcreteConformance(ResolvedType type,
                                                    ProtocolDecl *proto) {
  auto equivClass = type.getEquivalenceClass(*this);
  auto concrete = equivClass->concreteType;
  if (!concrete) return nullptr;

  // Conformance to this protocol is redundant; update the requirement source
  // appropriately.
  const RequirementSource *concreteSource;
  if (auto writtenSource =
        equivClass->findAnyConcreteConstraintAsWritten(nullptr))
    concreteSource = writtenSource->source;
  else
    concreteSource = equivClass->concreteTypeConstraints.front().source;

  // Lookup the conformance of the concrete type to this protocol.
  auto conformance = lookupConformance(concrete, proto);
  if (conformance.isInvalid()) {
    if (!concrete->hasError() && concreteSource->getLoc().isValid()) {
      Impl->HadAnyError = true;

      Diags.diagnose(concreteSource->getLoc(),
                     diag::requires_generic_param_same_type_does_not_conform,
                     concrete, proto->getName());
    }

    Impl->HadAnyError = true;
    equivClass->invalidConcreteType = true;
    return nullptr;
  }

  if (concrete->isExistentialType()) {
    // If we have an existential type, record the original type, and
    // not the conformance.
    //
    // The conformance must be a self-conformance, and self-conformances
    // do not record the original type in the case where a derived
    // protocol self-conforms to a base protocol; for example:
    //
    // @objc protocol Base {}
    //
    // @objc protocol Derived {}
    //
    // struct S<T : Base> {}
    //
    // extension S where T == Derived {}
    assert(isa<SelfProtocolConformance>(conformance.getConcrete()));
    concreteSource = concreteSource->viaConcrete(*this, concrete);
  } else {
    concreteSource = concreteSource->viaConcrete(*this, conformance);
  }

  equivClass->recordConformanceConstraint(*this, type, proto, concreteSource);

  // Only infer conditional requirements from explicit sources.
  bool hasExplicitSource = llvm::any_of(
      equivClass->concreteTypeConstraints,
      [](const ConcreteConstraint &constraint) {
        return (!constraint.source->isDerivedRequirement() &&
                constraint.source->getLoc().isValid());
      });

  if (hasExplicitSource) {
    addConditionalRequirements(conformance, /*inferForModule=*/nullptr);
  }

  return concreteSource;
}
const RequirementSource *GenericSignatureBuilder::resolveSuperConformance(
                                                        ResolvedType type,
                                                        ProtocolDecl *proto) {
  // Get the superclass constraint.
  auto equivClass = type.getEquivalenceClass(*this);
  Type superclass = equivClass->superclass;
  if (!superclass) return nullptr;

  // Lookup the conformance of the superclass to this protocol.
  auto conformance = lookupConformance(superclass, proto);
  if (conformance.isInvalid())
    return nullptr;

  assert(!conformance.isAbstract());

  // Conformance to this protocol is redundant; update the requirement source
  // appropriately.
  const RequirementSource *superclassSource;
  if (auto writtenSource =
        equivClass->findAnySuperclassConstraintAsWritten())
    superclassSource = writtenSource->source;
  else
    superclassSource = equivClass->superclassConstraints.front().source;

  superclassSource = superclassSource->viaSuperclass(*this, conformance);
  equivClass->recordConformanceConstraint(*this, type, proto, superclassSource);

  // Only infer conditional requirements from explicit sources.
  bool hasExplicitSource = llvm::any_of(
    equivClass->superclassConstraints,
    [](const ConcreteConstraint &constraint) {
      return (!constraint.source->isDerivedRequirement() &&
              constraint.source->getLoc().isValid());
    });

  if (hasExplicitSource) {
    addConditionalRequirements(conformance, /*inferForModule=*/nullptr);
  }

  return superclassSource;
}

CanType ResolvedType::getDependentType() const {
  return storage.get<PotentialArchetype *>()
      ->getDependentType();
}

Type ResolvedType::getDependentType(GenericSignatureBuilder &builder) const {
  return storage.get<PotentialArchetype *>()
      ->getDependentType(builder.getGenericParams());
}

auto PotentialArchetype::getOrCreateEquivalenceClass(
                                       GenericSignatureBuilder &builder) const
    -> EquivalenceClass * {
  // The equivalence class is stored on the representative.
  auto representative = getRepresentative();
  if (representative != this)
    return representative->getOrCreateEquivalenceClass(builder);

  // If we already have an equivalence class, return it.
  if (auto equivClass = getEquivalenceClassIfPresent())
    return equivClass;

  // Create a new equivalence class.
  auto equivClass =
    builder.Impl->allocateEquivalenceClass(
      const_cast<PotentialArchetype *>(this));
  representativeOrEquivClass = equivClass;
  return equivClass;
}

auto PotentialArchetype::getRepresentative() const -> PotentialArchetype * {
  auto representative =
    representativeOrEquivClass.dyn_cast<PotentialArchetype *>();
  if (!representative)
    return const_cast<PotentialArchetype *>(this);

  // Find the representative.
  PotentialArchetype *result = representative;
  while (auto nextRepresentative =
           result->representativeOrEquivClass.dyn_cast<PotentialArchetype *>())
    result = nextRepresentative;

  // Perform (full) path compression.
  const PotentialArchetype *fixUp = this;
  while (auto nextRepresentative =
           fixUp->representativeOrEquivClass.dyn_cast<PotentialArchetype *>()) {
    fixUp->representativeOrEquivClass = nextRepresentative;
    fixUp = nextRepresentative;
  }

  return result;
}

/// Canonical ordering for dependent types.
int swift::compareDependentTypes(Type type1, Type type2) {
  // Fast-path check for equality.
  if (type1->isEqual(type2)) return 0;

  // Ordering is as follows:
  // - Generic params
  auto gp1 = type1->getAs<GenericTypeParamType>();
  auto gp2 = type2->getAs<GenericTypeParamType>();
  if (gp1 && gp2)
    return GenericParamKey(gp1) < GenericParamKey(gp2) ? -1 : +1;

  // A generic parameter is always ordered before a nested type.
  if (static_cast<bool>(gp1) != static_cast<bool>(gp2))
    return gp1 ? -1 : +1;

  // - Dependent members
  auto depMemTy1 = type1->castTo<DependentMemberType>();
  auto depMemTy2 = type2->castTo<DependentMemberType>();

  // - by base, so t_0_n.`P.T` < t_1_m.`P.T`
  if (int compareBases =
        compareDependentTypes(depMemTy1->getBase(), depMemTy2->getBase()))
    return compareBases;

  // - by name, so t_n_m.`P.T` < t_n_m.`P.U`
  if (int compareNames = depMemTy1->getName().str().compare(
                                                  depMemTy2->getName().str()))
    return compareNames;

  auto *assocType1 = depMemTy1->getAssocType();
  auto *assocType2 = depMemTy2->getAssocType();
  if (int result = compareAssociatedTypes(assocType1, assocType2))
    return result;

  return 0;
}

/// Compare two dependent paths to determine which is better.
static int compareDependentPaths(ArrayRef<AssociatedTypeDecl *> path1,
                                 ArrayRef<AssociatedTypeDecl *> path2) {
  // Shorter paths win.
  if (path1.size() != path2.size())
    return path1.size() < path2.size() ? -1 : 1;

  // The paths are the same length, so order by comparing the associted
  // types.
  for (unsigned index : indices(path1)) {
    if (int result = compareAssociatedTypes(path1[index], path2[index]))
      return result;
  }

  // Identical paths.
  return 0;
}

namespace {
  /// Function object used to suppress conflict diagnoses when we know we'll
  /// see them again later.
  struct SameTypeConflictCheckedLater {
    void operator()(Type type1, Type type2) const { }
  };
} // end anonymous namespace

// Give a nested type the appropriately resolved concrete type, based off a
// parent PA that has a concrete type.
static void concretizeNestedTypeFromConcreteParent(
    GenericSignatureBuilder::PotentialArchetype *parent,
    GenericSignatureBuilder::PotentialArchetype *nestedPA,
    GenericSignatureBuilder &builder) {
  auto parentEquiv = parent->getEquivalenceClassIfPresent();
  assert(parentEquiv && "can't have a concrete type without an equiv class");

  bool isSuperclassConstrained = false;
  auto concreteParent = parentEquiv->concreteType;
  if (!concreteParent) {
    isSuperclassConstrained = true;
    concreteParent = parentEquiv->superclass;
  }
  assert(concreteParent &&
         "attempting to resolve concrete nested type of non-concrete PA");

  // These requirements are all implied based on the parent's concrete
  // conformance.
  auto assocType = nestedPA->getResolvedType();
  if (!assocType) return;

  auto proto = assocType->getProtocol();

  // If we don't already have a conformance of the parent to this protocol,
  // add it now; it was elided earlier.
  if (parentEquiv->conformsTo.count(proto) == 0) {
    auto source = (!isSuperclassConstrained
                   ? parentEquiv->concreteTypeConstraints.front().source
                   : parentEquiv->superclassConstraints.front().source);
    parentEquiv->recordConformanceConstraint(builder, parent, proto, source);
  }

  assert(parentEquiv->conformsTo.count(proto) > 0 &&
         "No conformance requirement");
  const RequirementSource *parentConcreteSource = nullptr;

  for (const auto &constraint : parentEquiv->conformsTo.find(proto)->second) {
    if (!isSuperclassConstrained) {
      if (constraint.source->kind == RequirementSource::Concrete) {
        parentConcreteSource = constraint.source;
      }
    } else {
      if (constraint.source->kind == RequirementSource::Superclass) {
        parentConcreteSource = constraint.source;
      }
    }
  }

  // Error condition: parent did not conform to this protocol, so there is no
  // way to resolve the nested type via concrete conformance.
  if (!parentConcreteSource) return;

  auto source = parentConcreteSource->viaParent(builder, assocType);
  auto conformance = parentConcreteSource->getProtocolConformance();

  Type witnessType;
  if (conformance.isConcrete()) {
    witnessType =
      conformance.getConcrete()->getTypeWitness(assocType);
    if (!witnessType)
      return; // FIXME: should we delay here?
  } else {
    // Otherwise we have an abstract conformance to an opaque result type.
    assert(conformance.isAbstract());
    auto archetype = concreteParent->castTo<ArchetypeType>();
    witnessType = archetype->getNestedType(assocType->getName());
  }

  builder.addSameTypeRequirement(
         nestedPA, witnessType, source,
         GenericSignatureBuilder::UnresolvedHandlingKind::GenerateConstraints,
         SameTypeConflictCheckedLater());
}

PotentialArchetype *PotentialArchetype::getOrCreateNestedType(
    GenericSignatureBuilder &builder, AssociatedTypeDecl *assocType,
    ArchetypeResolutionKind kind) {
  if (!assocType)
    return nullptr;

  // Always refer to the archetype anchor.
  assocType = assocType->getAssociatedTypeAnchor();

  Identifier name = assocType->getName();

  SWIFT_DEFER {
    // If we were asked for a complete, well-formed archetype, make sure we
    // process delayed requirements if anything changed.
    if (kind == ArchetypeResolutionKind::CompleteWellFormed)
      builder.processDelayedRequirements();
  };

  // Look for a potential archetype with the appropriate associated type.
  auto knownNestedTypes = NestedTypes.find(name);
  if (knownNestedTypes != NestedTypes.end()) {
    for (auto existingPA : knownNestedTypes->second) {
      // Do we have an associated-type match?
      if (assocType && existingPA->getResolvedType() == assocType) {
        return existingPA;
      }
    }
  }

  if (kind == ArchetypeResolutionKind::AlreadyKnown)
    return nullptr;

  // We don't have a result potential archetype, so we need to add one.

  // Creating a new potential archetype in an equivalence class is a
  // modification.
  getOrCreateEquivalenceClass(builder)->modified(builder);

  void *mem = builder.Impl->Allocator.Allocate<PotentialArchetype>();
  auto *resultPA = new (mem) PotentialArchetype(this, assocType);

  NestedTypes[name].push_back(resultPA);
  builder.addedNestedType(resultPA);

  // If we know something concrete about the parent PA, we need to propagate
  // that information to this new archetype.
  if (auto equivClass = getEquivalenceClassIfPresent()) {
    if (equivClass->concreteType || equivClass->superclass)
      concretizeNestedTypeFromConcreteParent(this, resultPA, builder);
  }

  return resultPA;
}

void ArchetypeType::resolveNestedType(
                                    std::pair<Identifier, Type> &nested) const {
  auto genericEnv = getGenericEnvironment();
  auto &builder = *genericEnv->getGenericSignatureBuilder();

  Type interfaceType = getInterfaceType();
  Type memberInterfaceType =
    DependentMemberType::get(interfaceType, nested.first);
  auto resolved =
    builder.maybeResolveEquivalenceClass(
                                  memberInterfaceType,
                                  ArchetypeResolutionKind::CompleteWellFormed,
                                  /*wantExactPotentialArchetype=*/false);
  if (!resolved) {
    nested.second = ErrorType::get(interfaceType);
    return;
  }

  Type result;
  if (auto concrete = resolved.getAsConcreteType()) {
    result = concrete;
  } else {
    auto *equivClass = resolved.getEquivalenceClass(builder);
    result = equivClass->getTypeInContext(builder, genericEnv);
  }

  assert(!nested.second ||
         nested.second->isEqual(result) ||
         (nested.second->hasError() && result->hasError()));
  nested.second = result;
}

Type GenericSignatureBuilder::PotentialArchetype::getDependentType(
                      TypeArrayView<GenericTypeParamType> genericParams) const {
  auto depType = getDependentType();
  return getSugaredDependentType(depType, genericParams);
}

void GenericSignatureBuilder::PotentialArchetype::dump() const {
  dump(llvm::errs(), nullptr, 0);
}

void GenericSignatureBuilder::PotentialArchetype::dump(llvm::raw_ostream &Out,
                                                       SourceManager *SrcMgr,
                                                       unsigned Indent) const {
  // Print name.
  if (Indent == 0 || isGenericParam())
    Out << getDebugName();
  else
    Out.indent(Indent) << getResolvedType()->getName();

  auto equivClass = getEquivalenceClassIfPresent();

  // Print superclass.
  if (equivClass && equivClass->superclass) {
    for (const auto &constraint : equivClass->superclassConstraints) {
      if (!constraint.isSubjectEqualTo(this)) continue;

      Out << " : ";
      constraint.value.print(Out);

      Out << " ";
      if (!constraint.source->isDerivedRequirement())
        Out << "*";
      Out << "[";
      constraint.source->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print concrete type.
  if (equivClass && equivClass->concreteType) {
    for (const auto &constraint : equivClass->concreteTypeConstraints) {
      if (!constraint.isSubjectEqualTo(this)) continue;

      Out << " == ";
      constraint.value.print(Out);

      Out << " ";
      if (!constraint.source->isDerivedRequirement())
        Out << "*";
      Out << "[";
      constraint.source->print(Out, SrcMgr);
      Out << "]";
    }
  }

  // Print requirements.
  if (equivClass) {
    bool First = true;
    for (const auto &entry : equivClass->conformsTo) {
      for (const auto &constraint : entry.second) {
        if (!constraint.isSubjectEqualTo(this)) continue;

        if (First) {
          First = false;
          Out << ": ";
        } else {
          Out << " & ";
        }

        Out << constraint.value->getName().str() << " ";
        if (!constraint.source->isDerivedRequirement())
          Out << "*";
        Out << "[";
        constraint.source->print(Out, SrcMgr);
        Out << "]";
      }
    }
  }

  if (getRepresentative() != this) {
    Out << " [represented by " << getRepresentative()->getDebugName() << "]";
  }

  if (getEquivalenceClassMembers().size() > 1) {
    Out << " [equivalence class ";
    bool isFirst = true;
    for (auto equiv : getEquivalenceClassMembers()) {
      if (equiv == this) continue;

      if (isFirst) isFirst = false;
      else Out << ", ";

      Out << equiv->getDebugName();
    }
    Out << "]";
  }

  Out << "\n";

  // Print nested types.
  for (const auto &nestedVec : NestedTypes) {
    for (auto nested : nestedVec.second) {
      nested->dump(Out, SrcMgr, Indent + 2);
    }
  }
}

#pragma mark Rewrite tree
RewritePath::RewritePath(Optional<GenericParamKey> base,
                         RelativeRewritePath path,
                         PathOrder order)
  : base(base)
{
  switch (order) {
  case Forward:
    this->path.insert(this->path.begin(), path.begin(), path.end());
    break;

  case Reverse:
    this->path.insert(this->path.begin(), path.rbegin(), path.rend());
    break;
  }
}

RewritePath RewritePath::createPath(Type type) {
  SmallVector<AssociatedTypeDecl *, 4> path;
  auto genericParam = createPath(type, path);
  return RewritePath(genericParam, path, Reverse);
}

GenericParamKey RewritePath::createPath(
                                Type type,
                                SmallVectorImpl<AssociatedTypeDecl *> &path) {
  while (auto depMemTy = type->getAs<DependentMemberType>()) {
    auto assocType = depMemTy->getAssocType();
    assert(assocType && "Unresolved dependent member type");
    path.push_back(assocType);
    type = depMemTy->getBase();
  }

  return type->castTo<GenericTypeParamType>();
}

RewritePath RewritePath::commonPath(const RewritePath &other) const {
  assert(getBase().hasValue() && other.getBase().hasValue());

  if (*getBase() != *other.getBase()) return RewritePath();

  // Find the longest common prefix.
  RelativeRewritePath path1 = getPath();
  RelativeRewritePath path2 = other.getPath();
  if (path1.size() > path2.size())
    std::swap(path1, path2);
  unsigned prefixLength =
    std::mismatch(path1.begin(), path1.end(), path2.begin()).first
      - path1.begin();

  // Form the common path.
  return RewritePath(getBase(), path1.slice(0, prefixLength), Forward);
}

/// Form a dependent type with the given generic parameter, then following the
/// path of associated types.
static Type formDependentType(GenericTypeParamType *base,
                              RelativeRewritePath path) {
  return std::accumulate(path.begin(), path.end(), Type(base),
                         [](Type type, AssociatedTypeDecl *assocType) -> Type {
                           return DependentMemberType::get(type, assocType);
                         });
}

/// Form a dependent type with the (canonical) generic parameter for the given
/// parameter key, then following the path of associated types.
static Type formDependentType(ASTContext &ctx, GenericParamKey genericParam,
                              RelativeRewritePath path) {
  return formDependentType(GenericTypeParamType::get(genericParam.Depth,
                                                     genericParam.Index,
                                                     ctx),
                           path);
}

CanType RewritePath::formDependentType(
                                     ASTContext &ctx,
                                     AnchorPathCache *anchorPathCache) const {
  if (auto base = getBase())
    return CanType(::formDependentType(ctx, *base, getPath()));

  assert(anchorPathCache && "Need an anchor path cache");
  const RewritePath &anchorPath = anchorPathCache->getAnchorPath();

  // Add the relative path to the anchor path.
  SmallVector<AssociatedTypeDecl *, 4> absolutePath;
  absolutePath.append(anchorPath.getPath().begin(),
                      anchorPath.getPath().end());
  absolutePath.append(getPath().begin(), getPath().end());
  return CanType(::formDependentType(ctx, *anchorPath.getBase(),
                                     absolutePath));

}

int RewritePath::compare(const RewritePath &other) const {
  // Prefer relative to absolute paths.
  if (getBase().hasValue() != other.getBase().hasValue()) {
    return other.getBase().hasValue() ? -1 : 1;
  }

  // Order based on the bases.
  if (getBase() && *getBase() != *other.getBase())
    return (*getBase() < *other.getBase()) ? -1 : 1;

  // Order based on the path contents.
  return compareDependentPaths(getPath(), other.getPath());
}

void RewritePath::print(llvm::raw_ostream &out) const {
  out << "[";

  if (getBase()) {
    out << "(" << getBase()->Depth << ", " << getBase()->Index << ")";
    if (!getPath().empty()) out << " -> ";
  }

  llvm::interleave(
      getPath().begin(), getPath().end(),
      [&](AssociatedTypeDecl *assocType) {
        out.changeColor(raw_ostream::BLUE);
        out << assocType->getProtocol()->getName() << "."
            << assocType->getName();
        out.resetColor();
      },
      [&] { out << " -> "; });
  out << "]";
}

RewriteTreeNode::~RewriteTreeNode() {
  for (auto child : children)
    delete child;
}

namespace {
/// Function object used to order rewrite tree nodes based on the address
/// of the associated type.
class OrderTreeRewriteNode {
  bool compare(AssociatedTypeDecl *lhs, AssociatedTypeDecl *rhs) const {
    // Make sure null pointers precede everything else.
    if (static_cast<bool>(lhs) != static_cast<bool>(rhs))
      return static_cast<bool>(rhs);

    // Use std::less to provide a defined ordering.
    return std::less<AssociatedTypeDecl *>()(lhs, rhs);
  }

public:
  bool operator()(RewriteTreeNode *lhs, AssociatedTypeDecl *rhs) const {
    return compare(lhs->getMatch(), rhs);
  }

  bool operator()(AssociatedTypeDecl *lhs, RewriteTreeNode *rhs) const {
    return compare(lhs, rhs->getMatch());
  }

  bool operator()(RewriteTreeNode *lhs, RewriteTreeNode *rhs) const {
    return compare(lhs->getMatch(), rhs->getMatch());
  }
};
}

bool RewriteTreeNode::addRewriteRule(RelativeRewritePath matchPath,
                                     RewritePath replacementPath) {
  // If the match path is empty, we're adding the rewrite rule to this node.
  if (matchPath.empty()) {
    // If we don't already have a rewrite rule, add it.
    if (!hasRewriteRule()) {
      setRewriteRule(replacementPath);
      return true;
    }

    // If we already have this rewrite rule, we're done.
    if (getRewriteRule() == replacementPath) return false;

    // Check whether any of the continuation children matches.
    auto insertPos = children.begin();
    while (insertPos != children.end() && !(*insertPos)->getMatch()) {
      if ((*insertPos)->hasRewriteRule() &&
          (*insertPos)->getRewriteRule() == replacementPath)
        return false;

      ++insertPos;
    }

    // We already have a rewrite rule, so add a new child with a
    // null associated type match to hold the rewrite rule.
    auto newChild = new RewriteTreeNode(nullptr);
    newChild->setRewriteRule(replacementPath);
    children.insert(insertPos, newChild);
    return true;
  }

  // Find (or create) a child node describing the next step in the match.
  auto matchFront = matchPath.front();
  auto childPos =
    std::lower_bound(children.begin(), children.end(), matchFront,
                     OrderTreeRewriteNode());
  if (childPos == children.end() || (*childPos)->getMatch() != matchFront) {
    childPos = children.insert(childPos, new RewriteTreeNode(matchFront));
  }

  // Add the rewrite rule to the child.
  return (*childPos)->addRewriteRule(matchPath.slice(1), replacementPath);
}

void RewriteTreeNode::enumerateRewritePathsImpl(
                       RelativeRewritePath matchPath,
                       llvm::function_ref<void(unsigned, RewritePath)> callback,
                       unsigned depth) const {
  // Determine whether we know anything about the next step in the path.
  auto childPos =
    depth < matchPath.size()
      ? std::lower_bound(children.begin(), children.end(),
                         matchPath[depth], OrderTreeRewriteNode())
      : children.end();
  if (childPos != children.end() &&
      (*childPos)->getMatch() == matchPath[depth]) {
    // Try to match the rest of the path.
    (*childPos)->enumerateRewritePathsImpl(matchPath, callback, depth + 1);
  }

  // If we have a rewrite rule at this position, invoke it.
  if (hasRewriteRule()) {
    // Invoke the callback with the first result.
    callback(depth, rewrite);
  }

  // Walk any children with NULL associated types; they might have more matches.
  for (auto otherRewrite : children) {
    if (otherRewrite->getMatch()) break;
    otherRewrite->enumerateRewritePathsImpl(matchPath, callback, depth);
  }
}

Optional<std::pair<unsigned, RewritePath>>
RewriteTreeNode::bestRewritePath(GenericParamKey base, RelativeRewritePath path,
                           unsigned prefixLength) {
  Optional<std::pair<unsigned, RewritePath>> best;
  unsigned bestAdjustedLength = 0;
  enumerateRewritePaths(path,
                        [&](unsigned length, RewritePath path) {
    // Determine how much of the original path will be replaced by the rewrite.
    unsigned adjustedLength = length;
    bool changesBase = false;
    if (auto newBase = path.getBase()) {
      adjustedLength += prefixLength;

      // If the base is unchanged, make sure we're reducing the length.
      changesBase = *newBase != base;
      if (!changesBase && adjustedLength <= path.getPath().size())
        return;
    }

    if (adjustedLength == 0 && !changesBase) return;

    if (adjustedLength > bestAdjustedLength || !best ||
        (adjustedLength == bestAdjustedLength &&
         path.compare(best->second) < 0)) {
      best = { length, path };
      bestAdjustedLength = adjustedLength;
    }
  });

  return best;
}

bool RewriteTreeNode::mergeInto(RewriteTreeNode *other) {
  // FIXME: A destructive version of this operation would be more efficient,
  // since we generally don't care about \c other after doing this.
  bool anyAdded = false;
  (void)enumerateRules([other, &anyAdded](RelativeRewritePath lhs,
                                          const RewritePath &rhs) {
    if (other->addRewriteRule(lhs, rhs))
      anyAdded = true;
    return RuleAction::none();
  });

  return anyAdded;
}

bool RewriteTreeNode::enumerateRulesRec(
                            llvm::function_ref<EnumerateCallback> &fn,
                            bool temporarilyDisableVisitedRule,
                            llvm::SmallVectorImpl<AssociatedTypeDecl *> &lhs) {
  if (auto assocType = getMatch())
    lhs.push_back(assocType);

  SWIFT_DEFER {
    if (getMatch())
      lhs.pop_back();
  };

  // If there is a rewrite rule, invoke the callback.
  if (hasRewriteRule()) {
    // If we're supposed to temporarily disabled the visited rule, do so
    // now.
    Optional<RewritePath> rewriteRule;
    if (temporarilyDisableVisitedRule) {
      rewriteRule = std::move(*this).getRewriteRule();
      removeRewriteRule();
    }

    // Make sure that we put the rewrite rule back in place if we moved it
    // aside.
    SWIFT_DEFER {
      if (temporarilyDisableVisitedRule && rewriteRule)
        setRewriteRule(*std::move(rewriteRule));
    };

    switch (auto action =
                fn(lhs, rewriteRule ? *rewriteRule : getRewriteRule())) {
    case RuleAction::None:
      break;

    case RuleAction::Stop:
      return true;

    case RuleAction::Remove:
      if (temporarilyDisableVisitedRule)
        rewriteRule = None;
      else
        removeRewriteRule();
      break;

    case RuleAction::Replace:
      if (temporarilyDisableVisitedRule) {
        rewriteRule = std::move(action.path);
      } else {
        removeRewriteRule();
        setRewriteRule(action.path);
      }
      break;
    }
  }

  // Recurse into the child nodes.
  for (auto child : children) {
    if (child->enumerateRulesRec(fn, temporarilyDisableVisitedRule, lhs))
      return true;
  }

  return false;
}

void RewriteTreeNode::dump() const {
  dump(llvm::errs());
}

void RewriteTreeNode::dump(llvm::raw_ostream &out, bool lastChild) const {
  std::string prefixStr;

  std::function<void(const RewriteTreeNode *, bool lastChild)> print;
  print = [&](const RewriteTreeNode *node, bool lastChild) {
    out << prefixStr << " `--";

    // Print the node name.
    out.changeColor(raw_ostream::GREEN);
    if (auto assoc = node->getMatch())
      out << assoc->getProtocol()->getName() << "." << assoc->getName();
    else
      out << "(cont'd)";
    out.resetColor();

    // Print the rewrite, if there is one.
    if (node->hasRewriteRule()) {
      out << " --> ";
      node->rewrite.print(out);
    }

    out << "\n";

    // Print children.
    prefixStr += ' ';
    prefixStr += (lastChild ? ' ' : '|');
    prefixStr += "  ";

    for (auto child : node->children) {
      print(child, child == node->children.back());
    }

    prefixStr.erase(prefixStr.end() - 4, prefixStr.end());
  };

  print(this, lastChild);
}

RewriteTreeNode *
GenericSignatureBuilder::Implementation::getRewriteTreeRootIfPresent(
                                          CanType anchor) {
  auto known = RewriteTreeRoots.find(anchor);
  if (known != RewriteTreeRoots.end()) return known->second.get();

  return nullptr;
}

RewriteTreeNode *
GenericSignatureBuilder::Implementation::getOrCreateRewriteTreeRoot(
                                          CanType anchor) {
  if (auto *root = getRewriteTreeRootIfPresent(anchor))
    return root;

  auto &root = RewriteTreeRoots[anchor];
  root = std::make_unique<RewriteTreeNode>(nullptr);
  return root.get();
}

void GenericSignatureBuilder::Implementation::minimizeRewriteTree(
                                            GenericSignatureBuilder &builder) {
  // Only perform minimization if the term-rewriting tree has changed.
  if (LastRewriteMinimizedGeneration == RewriteGeneration
      || MinimizingRewriteSystem)
    return;

  ++NumRewriteMinimizations;
  llvm::SaveAndRestore<bool> minimizingRewriteSystem(MinimizingRewriteSystem,
                                                     true);
  SWIFT_DEFER {
    LastRewriteMinimizedGeneration = RewriteGeneration;
  };

  minimizeRewriteTreeRhs(builder);
  removeRewriteTreeRedundancies(builder);
}

void GenericSignatureBuilder::Implementation::minimizeRewriteTreeRhs(
                                            GenericSignatureBuilder &builder) {
  assert(MinimizingRewriteSystem);

  // Minimize the right-hand sides of each rule in the tree.
  for (auto &equivClass : EquivalenceClasses) {
    CanType anchorType = equivClass.getAnchor(builder, { })->getCanonicalType();
    auto root = RewriteTreeRoots.find(anchorType);
    if (root == RewriteTreeRoots.end()) continue;

    AnchorPathCache anchorPathCache(builder, equivClass);

    ASTContext &ctx = builder.getASTContext();
    root->second->enumerateRules([&](RelativeRewritePath lhs,
                                     const RewritePath &rhs) {
      // Compute the type of the right-hand side.
      Type rhsType = rhs.formDependentType(ctx, &anchorPathCache);
      if (!rhsType) return RewriteTreeNode::RuleAction::none();

      // Compute the canonical type for the right-hand side.
      Type canonicalRhsType = builder.getCanonicalTypeParameter(rhsType);

      // If the canonicalized result is equivalent to the right-hand side we
      // had, there's nothing to do.
      if (rhsType->isEqual(canonicalRhsType))
        return RewriteTreeNode::RuleAction::none();

      // We have a canonical replacement path. Determine its encoding and
      // perform the replacement.
      ++NumRewriteRhsSimplified;

      // Determine replacement path, which might be relative to the anchor.
      auto canonicalRhsPath = RewritePath::createPath(canonicalRhsType);
      auto anchorPath = anchorPathCache.getAnchorPath();
      if (auto prefix = anchorPath.commonPath(canonicalRhsPath)) {
        unsigned prefixLength = prefix.getPath().size();
        RelativeRewritePath replacementRhsPath =
          canonicalRhsPath.getPath().slice(prefixLength);

        // If the left and right-hand sides are equivalent, just remove the
        // rule.
        if (lhs == replacementRhsPath) {
          ++NumRewriteRhsSimplifiedToLhs;
          return RewriteTreeNode::RuleAction::remove();
        }

        RewritePath replacementRhs(None, replacementRhsPath,
                                   RewritePath::Forward);
        return RewriteTreeNode::RuleAction::replace(std::move(replacementRhs));
      }

      return RewriteTreeNode::RuleAction::replace(canonicalRhsPath);
    });
  }
}

void GenericSignatureBuilder::Implementation::removeRewriteTreeRedundancies(
                                            GenericSignatureBuilder &builder) {
  assert(MinimizingRewriteSystem);

  // Minimize the right-hand sides of each rule in the tree.
  for (auto &equivClass : EquivalenceClasses) {
    CanType anchorType = equivClass.getAnchor(builder, { })->getCanonicalType();
    auto root = RewriteTreeRoots.find(anchorType);
    if (root == RewriteTreeRoots.end()) continue;

    AnchorPathCache anchorPathCache(builder, equivClass);

    ASTContext &ctx = builder.getASTContext();
    root->second->enumerateRules([&](RelativeRewritePath lhs,
                                     const RewritePath &rhs) {
      /// Left-hand side type.
      Type lhsType = RewritePath(None, lhs, RewritePath::Forward)
                       .formDependentType(ctx, &anchorPathCache);
      if (!lhsType) return RewriteTreeNode::RuleAction::none();

      // Simplify the left-hand type.
      Type simplifiedLhsType = builder.getCanonicalTypeParameter(lhsType);

      // Compute the type of the right-hand side.
      Type rhsType = rhs.formDependentType(ctx, &anchorPathCache);
      if (!rhsType) return RewriteTreeNode::RuleAction::none();

      if (simplifiedLhsType->isEqual(rhsType)) {
        ++NumRewriteRulesRedundant;
        return RewriteTreeNode::RuleAction::remove();
      }

      return RewriteTreeNode::RuleAction::none();
    },
    /*temporarilyDisableVisitedRule=*/true);
  }
}

bool GenericSignatureBuilder::addSameTypeRewriteRule(CanType type1,
                                                     CanType type2) {
  // We already effectively have this rewrite rule.
  if (type1 == type2) return false;

  auto path1 = RewritePath::createPath(type1);
  auto path2 = RewritePath::createPath(type2);

  // Look for a common prefix. When we have one, form a rewrite rule using
  // relative paths.
  if (auto prefix = path1.commonPath(path2)) {
    // Find the better relative rewrite path.
    RelativeRewritePath relPath1
      = path1.getPath().slice(prefix.getPath().size());
    RelativeRewritePath relPath2
      = path2.getPath().slice(prefix.getPath().size());
    // Order the paths so that we go to the more-canonical path.
    if (compareDependentPaths(relPath1, relPath2) < 0)
      std::swap(relPath1, relPath2);

    // Find the anchor for the prefix.
    CanType commonType = prefix.formDependentType(getASTContext());
    CanType commonAnchor =
      getCanonicalTypeParameter(commonType)->getCanonicalType();

    // Add the rewrite rule.
    auto root = Impl->getOrCreateRewriteTreeRoot(commonAnchor);
    return root->addRewriteRule(
                          relPath1,
                          RewritePath(None, relPath2, RewritePath::Forward));
  }

  // Otherwise, form a rewrite rule with absolute paths.

  // Find the better path and make sure it's in path2.
  if (compareDependentTypes(type1, type2) < 0) {
    std::swap(path1, path2);
    std::swap(type1, type2);
  }

  // Add the rewrite rule.
  Type firstBase =
    GenericTypeParamType::get(path1.getBase()->Depth, path1.getBase()->Index,
                              getASTContext());
  CanType baseAnchor =
    getCanonicalTypeParameter(firstBase)->getCanonicalType();
  auto root = Impl->getOrCreateRewriteTreeRoot(baseAnchor);
  return root->addRewriteRule(path1.getPath(), path2);
}

Type GenericSignatureBuilder::getCanonicalTypeParameter(Type type) {
  auto initialPath = RewritePath::createPath(type);
  auto genericParamType =
    GenericTypeParamType::get(initialPath.getBase()->Depth,
                              initialPath.getBase()->Index,
                              getASTContext());

  unsigned startIndex = 0;
  Type currentType = genericParamType;
  SmallVector<AssociatedTypeDecl *, 4> path(initialPath.getPath().begin(),
                                            initialPath.getPath().end());
  bool simplified = false;
  do {
    CanType currentAnchor = currentType->getCanonicalType();
    if (auto rootNode = Impl->getRewriteTreeRootIfPresent(currentAnchor)) {
      // Find the best rewrite rule for the path starting at startIndex.
      auto match =
        rootNode->bestRewritePath(genericParamType,
                            llvm::makeArrayRef(path).slice(startIndex),
                            startIndex);

      // If we have a match, replace the matched path with the replacement
      // path.
      if (match) {
        // Determine the range in the path which we'll be replacing.
        unsigned replaceStartIndex = match->second.getBase() ? 0 : startIndex;
        unsigned replaceEndIndex = startIndex + match->first;

        // Overwrite the beginning of the match.
        auto replacementPath = match->second.getPath();
        assert((replaceEndIndex - replaceStartIndex) >= replacementPath.size());
        auto replacementStartPos = path.begin() + replaceStartIndex;
        std::copy(replacementPath.begin(), replacementPath.end(),
                  replacementStartPos);

        // Erase the rest.
        path.erase(replacementStartPos + replacementPath.size(),
                   path.begin() + replaceEndIndex);

        // If this is an absolute path, use the new base.
        if (auto newBase = match->second.getBase()) {
          genericParamType =
            GenericTypeParamType::get(newBase->Depth, newBase->Index,
                                      getASTContext());
        }

        // Move back to the beginning; we may have opened up other rewrites.
        simplified = true;
        startIndex = 0;
        currentType = genericParamType;
        continue;
      }
    }

    // If we've hit the end of the path, we're done.
    if (startIndex >= path.size()) break;

    currentType = DependentMemberType::get(currentType, path[startIndex++]);
  } while (true);

  return formDependentType(genericParamType, path);
}

#pragma mark Equivalence classes
EquivalenceClass::EquivalenceClass(PotentialArchetype *representative)
  : recursiveConcreteType(false), invalidConcreteType(false),
    recursiveSuperclassType(false)
{
  members.push_back(representative);
}

void EquivalenceClass::modified(GenericSignatureBuilder &builder) {
  ++builder.Impl->Generation;

  // Transfer any delayed requirements to the primary queue, because they
  // might be resolvable now.
  builder.Impl->DelayedRequirements.append(delayedRequirements.begin(),
                                           delayedRequirements.end());
  delayedRequirements.clear();
}

GenericSignatureBuilder::GenericSignatureBuilder(
                               ASTContext &ctx)
  : Context(ctx), Diags(Context.Diags), Impl(new Implementation) {
  if (auto *Stats = Context.Stats)
    ++Stats->getFrontendCounters().NumGenericSignatureBuilders;
}

GenericSignatureBuilder::GenericSignatureBuilder(
                                         GenericSignatureBuilder &&other)
  : Context(other.Context), Diags(other.Diags), Impl(std::move(other.Impl))
{
  other.Impl.reset();

  if (Impl) {
    // Update the generic parameters to their canonical types.
    for (auto &gp : Impl->GenericParams) {
      gp = gp->getCanonicalType()->castTo<GenericTypeParamType>();
    }
  }
}

GenericSignatureBuilder::~GenericSignatureBuilder() = default;

auto
GenericSignatureBuilder::getLookupConformanceFn()
    -> LookUpConformanceInBuilder {
  return LookUpConformanceInBuilder(this);
}

ProtocolConformanceRef
GenericSignatureBuilder::LookUpConformanceInBuilder::operator()(
    CanType dependentType, Type conformingReplacementType,
    ProtocolDecl *conformedProtocol) const {
  // Lookup conformances for opened existential.
  if (conformingReplacementType->isOpenedExistential()) {
    return conformedProtocol->getModuleContext()->lookupConformance(
        conformingReplacementType, conformedProtocol);
  }
  return builder->lookupConformance(conformingReplacementType,
                                    conformedProtocol);
}

ProtocolConformanceRef
GenericSignatureBuilder::lookupConformance(Type conformingReplacementType,
                                           ProtocolDecl *conformedProtocol) {
  if (conformingReplacementType->isTypeParameter())
    return ProtocolConformanceRef(conformedProtocol);

  // Figure out which module to look into.
  // FIXME: When lookupConformance() starts respecting modules, we'll need
  // to do some filtering here.
  ModuleDecl *searchModule = conformedProtocol->getParentModule();
  return searchModule->lookupConformance(conformingReplacementType,
                                         conformedProtocol);
}

/// Resolve any unresolved dependent member types using the given builder.
static Type resolveDependentMemberTypes(
                                GenericSignatureBuilder &builder,
                                Type type,
                                ArchetypeResolutionKind resolutionKind) {
  if (!type->hasTypeParameter()) return type;

  return type.transformRec([&resolutionKind,
                            &builder](TypeBase *type) -> Optional<Type> {
    if (!type->isTypeParameter())
      return None;

    auto resolved = builder.maybeResolveEquivalenceClass(
        Type(type), resolutionKind, true);

    if (!resolved)
      return ErrorType::get(Type(type));

    if (auto concreteType = resolved.getAsConcreteType())
      return concreteType;

    // Map the type parameter to an equivalence class.
    auto equivClass = resolved.getEquivalenceClassIfPresent();

    // If there is a concrete type in this equivalence class, use that.
    if (equivClass && equivClass->concreteType) {
      // .. unless it's recursive.
      if (equivClass->recursiveConcreteType)
        return ErrorType::get(Type(type));

      // Prevent recursive substitution.
      equivClass->recursiveConcreteType = true;
      SWIFT_DEFER {
        equivClass->recursiveConcreteType = false;
      };

      return resolveDependentMemberTypes(builder, equivClass->concreteType,
                                         resolutionKind);
    }

    return resolved.getDependentType(builder);
  });
}

static Type getStructuralType(TypeDecl *typeDecl, bool keepSugar) {
  if (auto typealias = dyn_cast<TypeAliasDecl>(typeDecl)) {
    if (typealias->getUnderlyingTypeRepr() != nullptr) {
      auto type = typealias->getStructuralType();
      if (!keepSugar)
        if (auto *aliasTy = cast<TypeAliasType>(type.getPointer()))
          return aliasTy->getSinglyDesugaredType();
      return type;
    }
    if (!keepSugar)
      return typealias->getUnderlyingType();
  }

  return typeDecl->getDeclaredInterfaceType();
}

static Type substituteConcreteType(Type parentType,
                                   TypeDecl *concreteDecl) {
  if (parentType->is<ErrorType>() ||
      parentType->is<UnresolvedType>())
    return parentType;

  auto *dc = concreteDecl->getDeclContext();

  // Form an unsubstituted type referring to the given type declaration,
  // for use in an inferred same-type requirement.
  auto type = getStructuralType(concreteDecl, /*keepSugar=*/true);

  auto subMap = parentType->getContextSubstitutionMap(
      dc->getParentModule(), dc);

  // If the type has unresolved DependentMemberTypes, we can't use
  // Type::subst() if a generic parameter is replaced with a concrete
  // type. Instead, perform a "shallow" substitution where we replace
  // generic parameter types but leave DependentMemberTypes as-is.
  // This means we will end up back in maybeResolveEquivalenceClass(),
  // where we will perform the name lookup required to resolve any
  // DependentMemberTypes with a concrete base.
  if (type->findUnresolvedDependentMemberType()) {
    return type.transform([&](Type t) {
      if (t->is<GenericTypeParamType>()) {
        return t.subst(subMap);
      }
      return t;
    });
  }

  return type.subst(subMap);
}

ResolvedType GenericSignatureBuilder::maybeResolveEquivalenceClass(
                                    Type type,
                                    ArchetypeResolutionKind resolutionKind,
                                    bool wantExactPotentialArchetype) {
  // The equivalence class of a generic type is known directly.
  if (auto genericParam = type->getAs<GenericTypeParamType>()) {
    unsigned index = GenericParamKey(genericParam).findIndexIn(
                                                            getGenericParams());
    if (index < Impl->GenericParams.size()) {
      return ResolvedType(Impl->PotentialArchetypes[index]);
    }

    return ResolvedType::forUnresolved(nullptr);
  }

  // The equivalence class of a dependent member type is determined by its
  // base equivalence class, if there is one.
  if (auto depMemTy = type->getAs<DependentMemberType>()) {
    // Find the equivalence class of the base.
    auto resolvedBase =
      maybeResolveEquivalenceClass(depMemTy->getBase(),
                                   resolutionKind,
                                   wantExactPotentialArchetype);
    if (!resolvedBase) return resolvedBase;

    // If the base is concrete, so is this member.
    if (auto parentType = resolvedBase.getAsConcreteType()) {
      TypeDecl *concreteDecl = depMemTy->getAssocType();
      if (!concreteDecl) {
        // If we have an unresolved dependent member type, perform a
        // name lookup.
        if (auto *decl = parentType->getAnyNominal()) {
          SmallVector<TypeDecl *, 2> concreteDecls;
          lookupConcreteNestedType(decl, depMemTy->getName(), concreteDecls);

          if (concreteDecls.empty())
            return ResolvedType::forUnresolved(nullptr);

          auto bestConcreteTypeIter = findBestConcreteNestedType(concreteDecls);
          concreteDecl = *bestConcreteTypeIter;
        }
      }

      auto concreteType = substituteConcreteType(parentType, concreteDecl);
      return maybeResolveEquivalenceClass(concreteType, resolutionKind,
                                          wantExactPotentialArchetype);
    }

    // Find the nested type declaration for this.
    auto baseEquivClass = resolvedBase.getEquivalenceClass(*this);

    // Retrieve the "smallest" type in the equivalence class, by depth, and
    // use that to find a nested potential archetype. We used the smallest
    // type by depth to limit expansion of the type graph.
    PotentialArchetype *basePA;
    if (wantExactPotentialArchetype) {
      basePA = resolvedBase.getPotentialArchetypeIfKnown();
      if (!basePA)
        return ResolvedType::forUnresolved(baseEquivClass);
    } else {
      basePA = baseEquivClass->members.front();
    }

    if (auto assocType = depMemTy->getAssocType()) {
      // Check whether this associated type references a protocol to which
      // the base conforms. If not, it's either concrete or unresolved.
      auto *proto = assocType->getProtocol();
      if (baseEquivClass->conformsTo.find(proto)
          == baseEquivClass->conformsTo.end()) {
        if (baseEquivClass->concreteType &&
            lookupConformance(baseEquivClass->concreteType,
                              proto)) {
          // Fall through
        } else if (baseEquivClass->superclass &&
                   lookupConformance(baseEquivClass->superclass,
                                     proto)) {
          // Fall through
        } else {
          return ResolvedType::forUnresolved(baseEquivClass);
        }

        // FIXME: Instead of falling through, we ought to return a concrete
        // type here, but then we fail to update a nested PotentialArchetype
        // if one happens to already exist. It would be cleaner if concrete
        // types never had nested PotentialArchetypes.
      }

      auto nestedPA =
        basePA->getOrCreateNestedType(*this, assocType, resolutionKind);
      if (!nestedPA)
        return ResolvedType::forUnresolved(baseEquivClass);

      return ResolvedType(nestedPA);
    } else {
      auto *concreteDecl =
          baseEquivClass->lookupNestedType(*this, depMemTy->getName());

      if (!concreteDecl)
        return ResolvedType::forUnresolved(baseEquivClass);

      Type parentType;
      auto *proto = concreteDecl->getDeclContext()->getSelfProtocolDecl();
      if (!proto) {
        parentType = (baseEquivClass->concreteType
                      ? baseEquivClass->concreteType
                      : baseEquivClass->superclass);
      } else {
        if (baseEquivClass->concreteType &&
            lookupConformance(baseEquivClass->concreteType,
                              proto)) {
          parentType = baseEquivClass->concreteType;
        } else if (baseEquivClass->superclass &&
                   lookupConformance(baseEquivClass->superclass,
                                     proto)) {
          parentType = baseEquivClass->superclass;
        } else {
          parentType = basePA->getDependentType(getGenericParams());
        }
      }

      auto concreteType = substituteConcreteType(parentType, concreteDecl);
      return maybeResolveEquivalenceClass(concreteType, resolutionKind,
                                          wantExactPotentialArchetype);
    }
  }

  // If it's not a type parameter, it won't directly resolve to one.
  // FIXME: Generic typealiases contradict the assumption above.
  // If there is a type parameter somewhere in this type, resolve it.
  if (type->hasTypeParameter()) {
    Type resolved = resolveDependentMemberTypes(*this, type, resolutionKind);
    if (resolved->hasError() && !type->hasError())
      return ResolvedType::forUnresolved(nullptr);

    type = resolved;
  }

  return ResolvedType::forConcrete(type);
}

EquivalenceClass *GenericSignatureBuilder::resolveEquivalenceClass(
                                    Type type,
                                    ArchetypeResolutionKind resolutionKind) {
  if (auto resolved =
        maybeResolveEquivalenceClass(type, resolutionKind,
                                     /*wantExactPotentialArchetype=*/false))
    return resolved.getEquivalenceClass(*this);

  return nullptr;
}

auto GenericSignatureBuilder::resolve(UnresolvedType paOrT,
                                      FloatingRequirementSource source)
    -> ResolvedType {
  if (auto pa = paOrT.dyn_cast<PotentialArchetype *>())
    return ResolvedType(pa);

  // Determine what kind of resolution we want.
  ArchetypeResolutionKind resolutionKind =
    ArchetypeResolutionKind::WellFormed;
  if (source.isDerived() && source.isRecursive(*this))
    resolutionKind = ArchetypeResolutionKind::AlreadyKnown;

  Type type = paOrT.get<Type>();
  return maybeResolveEquivalenceClass(type, resolutionKind,
                                      /*wantExactPotentialArchetype=*/true);
}

bool GenericSignatureBuilder::areInSameEquivalenceClass(Type type1,
                                                        Type type2) {
  return resolveEquivalenceClass(type1, ArchetypeResolutionKind::WellFormed)
    == resolveEquivalenceClass(type2, ArchetypeResolutionKind::WellFormed);
}

Type GenericSignatureBuilder::getCanonicalTypeInContext(Type type,
                           TypeArrayView<GenericTypeParamType> genericParams) {
  // All the contextual canonicality rules apply to type parameters, so if the
  // type doesn't involve any type parameters, it's already canonical.
  if (!type->hasTypeParameter())
    return type;

  // Replace non-canonical type parameters.
  return type.transformRec([&](TypeBase *component) -> Optional<Type> {
    if (!isa<GenericTypeParamType>(component) &&
        !isa<DependentMemberType>(component))
      return None;

    // Find the equivalence class for this dependent type.
    auto resolved = maybeResolveEquivalenceClass(
                      Type(component),
                      ArchetypeResolutionKind::CompleteWellFormed,
                      /*wantExactPotentialArchetype=*/false);
    if (!resolved) return None;

    if (auto concrete = resolved.getAsConcreteType())
      return getCanonicalTypeInContext(concrete, genericParams);

    auto equivClass = resolved.getEquivalenceClass(*this);
    if (!equivClass) return None;

    // If there is a concrete type in this equivalence class, use that.
    if (auto concrete = equivClass->concreteType) {
      // .. unless it's recursive.
      if (equivClass->recursiveConcreteType)
        return ErrorType::get(Type(type));

      // Prevent recursive substitution.
      equivClass->recursiveConcreteType = true;
      SWIFT_DEFER {
        equivClass->recursiveConcreteType = false;
      };

      return getCanonicalTypeInContext(concrete, genericParams);
    }

    return equivClass->getAnchor(*this, genericParams);
  });
}

ConformanceAccessPath
GenericSignatureBuilder::getConformanceAccessPath(Type type,
                                                  ProtocolDecl *protocol,
                                                  GenericSignature sig) {
  auto canType = getCanonicalTypeInContext(type, { })->getCanonicalType();
  assert(canType->isTypeParameter());

  // Check if we've already cached the result before doing anything else.
  auto found = Impl->ConformanceAccessPaths.find(
      std::make_pair(canType, protocol));
  if (found != Impl->ConformanceAccessPaths.end()) {
    return found->second;
  }

  auto *Stats = Context.Stats;

  FrontendStatsTracer tracer(Stats, "get-conformance-access-path");

  auto recordPath = [&](CanType type, ProtocolDecl *proto,
                        ConformanceAccessPath path) {
    // Add the path to the buffer.
    Impl->CurrentConformanceAccessPaths.emplace_back(type, path);

    // Add the path to the map.
    auto key = std::make_pair(type, proto);
    auto inserted = Impl->ConformanceAccessPaths.insert(
        std::make_pair(key, path));
    assert(inserted.second);
    (void) inserted;

    if (Stats)
      ++Stats->getFrontendCounters().NumConformanceAccessPathsRecorded;
  };

  // If this is the first time we're asked to look up a conformance access path,
  // visit all of the root conformance requirements in our generic signature and
  // add them to the buffer.
  if (Impl->ConformanceAccessPaths.empty()) {
    for (const auto &req : sig->getRequirements()) {
      // We only care about conformance requirements.
      if (req.getKind() != RequirementKind::Conformance)
        continue;

      auto rootType = req.getFirstType()->getCanonicalType();
      auto *rootProto = req.getProtocolDecl();

      ConformanceAccessPath::Entry root(rootType, rootProto);
      ArrayRef<ConformanceAccessPath::Entry> path(root);
      ConformanceAccessPath result(Context.AllocateCopy(path));

      recordPath(rootType, rootProto, result);
    }
  }

  // We enumerate conformance access paths in lexshort order until we find the
  // path whose corresponding type canonicalizes to the one we are looking for.
  while (true) {
    auto found = Impl->ConformanceAccessPaths.find(
        std::make_pair(canType, protocol));
    if (found != Impl->ConformanceAccessPaths.end()) {
      return found->second;
    }

    assert(Impl->CurrentConformanceAccessPaths.size() > 0);

    // The buffer consists of all conformance access paths of length N.
    // Swap it out with an empty buffer, and fill it with all paths of
    // length N+1.
    std::vector<std::pair<CanType, ConformanceAccessPath>> oldPaths;
    std::swap(Impl->CurrentConformanceAccessPaths, oldPaths);

    for (const auto &pair : oldPaths) {
      const auto &lastElt = pair.second.back();
      auto *lastProto = lastElt.second;

      // A copy of the current path, populated as needed.
      SmallVector<ConformanceAccessPath::Entry, 4> entries;

      for (const auto &req : lastProto->getRequirementSignature()) {
        // We only care about conformance requirements.
        if (req.getKind() != RequirementKind::Conformance)
          continue;

        auto nextSubjectType = req.getFirstType()->getCanonicalType();
        auto *nextProto = req.getProtocolDecl();

        // Compute the canonical anchor for this conformance requirement.
        auto nextType = replaceSelfWithType(pair.first, nextSubjectType);
        auto nextCanType = getCanonicalTypeInContext(nextType, { })
            ->getCanonicalType();

        // Skip "derived via concrete" sources.
        if (!nextCanType->isTypeParameter())
          continue;

        // If we've already seen a path for this conformance, skip it and
        // don't add it to the buffer. Note that because we iterate over
        // conformance access paths in lexshort order, the existing
        // conformance access path is shorter than the one we found just now.
        if (Impl->ConformanceAccessPaths.count(
                std::make_pair(nextCanType, nextProto)))
          continue;

        if (entries.empty()) {
          // Fill our temporary vector.
          entries.insert(entries.begin(),
                         pair.second.begin(),
                         pair.second.end());
        }

        // Add the next entry.
        entries.emplace_back(nextSubjectType, nextProto);
        ConformanceAccessPath result = Context.AllocateCopy(entries);
        entries.pop_back();

        recordPath(nextCanType, nextProto, result);
      }
    }
  }
}

TypeArrayView<GenericTypeParamType>
GenericSignatureBuilder::getGenericParams() const {
  return TypeArrayView<GenericTypeParamType>(Impl->GenericParams);
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamDecl *GenericParam) {
  addGenericParameter(
     GenericParam->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
}

bool GenericSignatureBuilder::addGenericParameterRequirements(
                                           GenericTypeParamDecl *GenericParam) {
  GenericParamKey Key(GenericParam);
  auto PA = Impl->PotentialArchetypes[Key.findIndexIn(getGenericParams())];
  
  // Add the requirements from the declaration.
  return isErrorResult(
           addInheritedRequirements(GenericParam, PA, nullptr,
                                    GenericParam->getModuleContext()));
}

void GenericSignatureBuilder::addGenericParameter(GenericTypeParamType *GenericParam) {
  auto params = getGenericParams();
  (void)params;
  assert(params.empty() ||
         ((GenericParam->getDepth() == params.back()->getDepth() &&
           GenericParam->getIndex() == params.back()->getIndex() + 1) ||
          (GenericParam->getDepth() > params.back()->getDepth() &&
           GenericParam->getIndex() == 0)));

  // Create a potential archetype for this type parameter.
  auto PA = new (Impl->Allocator) PotentialArchetype(GenericParam);
  Impl->GenericParams.push_back(GenericParam);
  Impl->PotentialArchetypes.push_back(PA);
}

/// Visit all of the types that show up in the list of inherited
/// types.
static ConstraintResult visitInherited(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    llvm::function_ref<ConstraintResult(Type, const TypeRepr *)> visitType) {
  // Local function that (recursively) adds inherited types.
  ConstraintResult result = ConstraintResult::Resolved;
  std::function<void(Type, const TypeRepr *)> visitInherited;

  visitInherited = [&](Type inheritedType, const TypeRepr *typeRepr) {
    // Decompose explicitly-written protocol compositions.
    if (auto composition = dyn_cast_or_null<CompositionTypeRepr>(typeRepr)) {
      if (auto compositionType
            = inheritedType->getAs<ProtocolCompositionType>()) {
        unsigned index = 0;
        for (auto memberType : compositionType->getMembers()) {
          visitInherited(memberType, composition->getTypes()[index]);
          ++index;
        }

        return;
      }
    }

    auto recursiveResult = visitType(inheritedType, typeRepr);
    if (isErrorResult(recursiveResult) && !isErrorResult(result))
      result = recursiveResult;
  };

  // Visit all of the inherited types.
  auto typeDecl = decl.dyn_cast<const TypeDecl *>();
  auto extDecl = decl.dyn_cast<const ExtensionDecl *>();
  ASTContext &ctx = typeDecl ? typeDecl->getASTContext()
                             : extDecl->getASTContext();
  auto &evaluator = ctx.evaluator;
  ArrayRef<InheritedEntry> inheritedTypes =
      typeDecl ? typeDecl->getInherited()
               : extDecl->getInherited();
  for (unsigned index : indices(inheritedTypes)) {
    Type inheritedType
      = evaluateOrDefault(evaluator,
                          InheritedTypeRequest{decl, index,
                            TypeResolutionStage::Structural},
                          Type());
    if (!inheritedType) continue;

    const auto &inherited = inheritedTypes[index];
    visitInherited(inheritedType, inherited.getTypeRepr());
  }

  return result;
}

ConstraintResult GenericSignatureBuilder::expandConformanceRequirement(
                                            ResolvedType selfType,
                                            ProtocolDecl *proto,
                                            const RequirementSource *source,
                                            bool onlySameTypeConstraints) {
  auto protocolSubMap = SubstitutionMap::getProtocolSubstitutions(
      proto, selfType.getDependentType(*this), ProtocolConformanceRef(proto));

  // Use the requirement signature to avoid rewalking the entire protocol.  This
  // cannot compute the requirement signature directly, because that may be
  // infinitely recursive: this code is also used to construct it.
  if (!proto->isComputingRequirementSignature()) {
    auto innerSource =
      FloatingRequirementSource::viaProtocolRequirement(source, proto,
                                                        /*inferred=*/false);
    for (const auto &req : proto->getRequirementSignature()) {
      // If we're only looking at same-type constraints, skip everything else.
      if (onlySameTypeConstraints && req.getKind() != RequirementKind::SameType)
        continue;

      auto substReq = req.subst(protocolSubMap);
      auto reqResult = substReq
                           ? addRequirement(*substReq, innerSource, nullptr)
                           : ConstraintResult::Conflicting;
      if (isErrorResult(reqResult)) return reqResult;
    }

    return ConstraintResult::Resolved;
  }

  if (!onlySameTypeConstraints) {
    // Add all of the inherited protocol requirements, recursively.
    auto inheritedReqResult =
      addInheritedRequirements(proto, selfType.getUnresolvedType(), source,
                               proto->getModuleContext());
    if (isErrorResult(inheritedReqResult))
      return inheritedReqResult;
  }

  // Add any requirements in the where clause on the protocol.
  WhereClauseOwner(proto).visitRequirements(TypeResolutionStage::Structural,
      [&](const Requirement &req, RequirementRepr *reqRepr) {
        // If we're only looking at same-type constraints, skip everything else.
        if (onlySameTypeConstraints &&
            req.getKind() != RequirementKind::SameType)
          return false;

        auto innerSource = FloatingRequirementSource::viaProtocolRequirement(
            source, proto, reqRepr->getSeparatorLoc(), /*inferred=*/false);
        addRequirement(req, reqRepr, innerSource,
                       &protocolSubMap, nullptr);
        return false;
      });

  if (proto->isObjC()) {
    // @objc implies an inferred AnyObject constraint.
    auto innerSource =
      FloatingRequirementSource::viaProtocolRequirement(source, proto,
                                                        /*inferred=*/true);
    addLayoutRequirementDirect(selfType,
                         LayoutConstraint::getLayoutConstraint(
                             LayoutConstraintKind::Class,
                             getASTContext()),
                         innerSource);

    return ConstraintResult::Resolved;
  }

  // Remaining logic is not relevant in ObjC protocol cases.

  // Collect all of the inherited associated types and typealiases in the
  // inherited protocols (recursively).
  llvm::MapVector<Identifier, TinyPtrVector<TypeDecl *>> inheritedTypeDecls;
  {
    proto->walkInheritedProtocols(
        [&](ProtocolDecl *inheritedProto) -> TypeWalker::Action {
      if (inheritedProto == proto) return TypeWalker::Action::Continue;

      for (auto req : inheritedProto->getMembers()) {
        if (auto typeReq = dyn_cast<TypeDecl>(req)) {
          // Ignore generic types
          if (auto genReq = dyn_cast<GenericTypeDecl>(req))
            if (genReq->getGenericParams())
              continue;

          inheritedTypeDecls[typeReq->getName()].push_back(typeReq);
        }
      }
      return TypeWalker::Action::Continue;
    });
  }

  // Local function to find the insertion point for the protocol's "where"
  // clause, as well as the string to start the insertion ("where" or ",");
  auto getProtocolWhereLoc = [&]() -> Located<const char *> {
    // Already has a trailing where clause.
    if (auto trailing = proto->getTrailingWhereClause())
      return { ", ", trailing->getRequirements().back().getSourceRange().End };

    // Inheritance clause.
    return { " where ", proto->getInherited().back().getSourceRange().End };
  };

  // Retrieve the set of requirements that a given associated type declaration
  // produces, in the form that would be seen in the where clause.
  const auto getAssociatedTypeReqs = [&](const AssociatedTypeDecl *assocType,
                                         const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      interleave(assocType->getInherited(), [&](TypeLoc inheritedType) {
        out << assocType->getName() << ": ";
        if (auto inheritedTypeRepr = inheritedType.getTypeRepr())
          inheritedTypeRepr->print(out);
        else
          inheritedType.getType().print(out);
      }, [&] {
        out << ", ";
      });

      if (const auto whereClause = assocType->getTrailingWhereClause()) {
        if (!assocType->getInherited().empty())
          out << ", ";

        whereClause->print(out, /*printWhereKeyword*/false);
      }
    }
    return result;
  };

  // Retrieve the requirement that a given typealias introduces when it
  // overrides an inherited associated type with the same name, as a string
  // suitable for use in a where clause.
  auto getConcreteTypeReq = [&](TypeDecl *type, const char *start) {
    std::string result;
    {
      llvm::raw_string_ostream out(result);
      out << start;
      out << type->getName() << " == ";
      if (auto typealias = dyn_cast<TypeAliasDecl>(type)) {
        if (auto underlyingTypeRepr = typealias->getUnderlyingTypeRepr())
          underlyingTypeRepr->print(out);
        else
          typealias->getUnderlyingType().print(out);
      } else {
        type->print(out);
      }
    }
    return result;
  };

  // An inferred same-type requirement between the two type declarations
  // within this protocol or a protocol it inherits.
  auto addInferredSameTypeReq = [&](TypeDecl *first, TypeDecl *second) {
    Type firstType = getStructuralType(first, /*keepSugar=*/false);
    Type secondType = getStructuralType(second, /*keepSugar=*/false);

    auto inferredSameTypeSource =
      FloatingRequirementSource::viaProtocolRequirement(
                    source, proto, SourceLoc(), /*inferred=*/true);

    auto rawReq = Requirement(RequirementKind::SameType, firstType, secondType);
    if (auto req = rawReq.subst(protocolSubMap))
      addRequirement(*req, inferredSameTypeSource, proto->getParentModule());
  };

  // Add requirements for each of the associated types.
  for (auto assocTypeDecl : proto->getAssociatedTypeMembers()) {
    // Add requirements placed directly on this associated type.
    Type assocType =
      DependentMemberType::get(selfType.getDependentType(*this), assocTypeDecl);
    if (!onlySameTypeConstraints) {
      auto assocResult =
        addInheritedRequirements(assocTypeDecl, assocType, source,
                                 /*inferForModule=*/nullptr);
      if (isErrorResult(assocResult))
        return assocResult;
    }

    // Add requirements from this associated type's where clause.
    WhereClauseOwner(assocTypeDecl).visitRequirements(
        TypeResolutionStage::Structural,
        [&](const Requirement &req, RequirementRepr *reqRepr) {
          // If we're only looking at same-type constraints, skip everything else.
          if (onlySameTypeConstraints &&
              req.getKind() != RequirementKind::SameType)
            return false;

          auto innerSource = FloatingRequirementSource::viaProtocolRequirement(
              source, proto, reqRepr->getSeparatorLoc(), /*inferred=*/false);
          addRequirement(req, reqRepr, innerSource, &protocolSubMap,
                         /*inferForModule=*/nullptr);
          return false;
        });

    // Check whether we inherited any types with the same name.
    auto knownInherited =
      inheritedTypeDecls.find(assocTypeDecl->getName());
    if (knownInherited == inheritedTypeDecls.end()) continue;

    bool shouldWarnAboutRedeclaration =
      source->kind == RequirementSource::RequirementSignatureSelf &&
      !assocTypeDecl->getAttrs().hasAttribute<NonOverrideAttr>() &&
      !assocTypeDecl->getAttrs().hasAttribute<OverrideAttr>() &&
      !assocTypeDecl->hasDefaultDefinitionType() &&
      (!assocTypeDecl->getInherited().empty() ||
        assocTypeDecl->getTrailingWhereClause() ||
        getASTContext().LangOpts.WarnImplicitOverrides);
    for (auto inheritedType : knownInherited->second) {
      // If we have inherited associated type...
      if (auto inheritedAssocTypeDecl =
            dyn_cast<AssociatedTypeDecl>(inheritedType)) {
        // Complain about the first redeclaration.
        if (shouldWarnAboutRedeclaration) {
          auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
          auto fixItWhere = getProtocolWhereLoc();
          Diags.diagnose(assocTypeDecl,
                         diag::inherited_associated_type_redecl,
                         assocTypeDecl->getName(),
                         inheritedFromProto->getDeclaredInterfaceType())
            .fixItInsertAfter(
                      fixItWhere.Loc,
                      getAssociatedTypeReqs(assocTypeDecl, fixItWhere.Item))
            .fixItRemove(assocTypeDecl->getSourceRange());

          Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                         inheritedAssocTypeDecl->getName());

          shouldWarnAboutRedeclaration = false;
        }

        continue;
      }

      // We inherited a type; this associated type will be identical
      // to that typealias.
      if (source->kind == RequirementSource::RequirementSignatureSelf) {
        auto inheritedOwningDecl =
            inheritedType->getDeclContext()->getSelfNominalTypeDecl();
        Diags.diagnose(assocTypeDecl,
                       diag::associated_type_override_typealias,
                       assocTypeDecl->getName(),
                       inheritedOwningDecl->getDescriptiveKind(),
                       inheritedOwningDecl->getDeclaredInterfaceType());
      }

      addInferredSameTypeReq(assocTypeDecl, inheritedType);
    }

    inheritedTypeDecls.erase(knownInherited);
    continue;
  }

  // Check all remaining inherited type declarations to determine if
  // this protocol has a non-associated-type type with the same name.
  inheritedTypeDecls.remove_if(
    [&](const std::pair<Identifier, TinyPtrVector<TypeDecl *>> &inherited) {
      const auto name = inherited.first;
      for (auto found : proto->lookupDirect(name)) {
        // We only want concrete type declarations.
        auto type = dyn_cast<TypeDecl>(found);
        if (!type || isa<AssociatedTypeDecl>(type)) continue;

        // Ignore nominal types. They're always invalid declarations.
        if (isa<NominalTypeDecl>(type))
          continue;

        // ... from the same module as the protocol.
        if (type->getModuleContext() != proto->getModuleContext()) continue;

        // Ignore types defined in constrained extensions; their equivalence
        // to the associated type would have to be conditional, which we cannot
        // model.
        if (auto ext = dyn_cast<ExtensionDecl>(type->getDeclContext())) {
          if (ext->isConstrainedExtension()) continue;
        }

        // We found something.
        bool shouldWarnAboutRedeclaration =
          source->kind == RequirementSource::RequirementSignatureSelf;

        for (auto inheritedType : inherited.second) {
          // If we have inherited associated type...
          if (auto inheritedAssocTypeDecl =
                dyn_cast<AssociatedTypeDecl>(inheritedType)) {
            // Infer a same-type requirement between the typealias' underlying
            // type and the inherited associated type.
            addInferredSameTypeReq(inheritedAssocTypeDecl, type);

            // Warn that one should use where clauses for this.
            if (shouldWarnAboutRedeclaration) {
              auto inheritedFromProto = inheritedAssocTypeDecl->getProtocol();
              auto fixItWhere = getProtocolWhereLoc();
              Diags.diagnose(type,
                             diag::typealias_override_associated_type,
                             name,
                             inheritedFromProto->getDeclaredInterfaceType())
                .fixItInsertAfter(fixItWhere.Loc,
                                  getConcreteTypeReq(type, fixItWhere.Item))
                .fixItRemove(type->getSourceRange());
              Diags.diagnose(inheritedAssocTypeDecl, diag::decl_declared_here,
                             inheritedAssocTypeDecl->getName());

              shouldWarnAboutRedeclaration = false;
            }

            continue;
          }

          // Two typealiases that should be the same.
          addInferredSameTypeReq(inheritedType, type);
        }

        // We can remove this entry.
        return true;
      }

      return false;
  });

  // Infer same-type requirements among inherited type declarations.
  for (auto &entry : inheritedTypeDecls) {
    if (entry.second.size() < 2) continue;

    auto firstDecl = entry.second.front();
    for (auto otherDecl : ArrayRef<TypeDecl *>(entry.second).slice(1)) {
      addInferredSameTypeReq(firstDecl, otherDecl);
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addConformanceRequirement(
                               ResolvedType type,
                               ProtocolDecl *proto,
                               FloatingRequirementSource source) {
  auto resolvedSource = source.getSource(*this, type);

  if (!resolvedSource->isDerivedRequirement()) {
    Impl->ExplicitRequirements.insert(
        ExplicitRequirement(RequirementKind::Conformance,
                            resolvedSource, proto));
  }

  // Add the conformance requirement, bailing out earlier if we've already
  // seen it.
  auto equivClass = type.getEquivalenceClass(*this);
  if (!equivClass->recordConformanceConstraint(*this, type, proto,
                                               resolvedSource))
    return ConstraintResult::Resolved;

  return expandConformanceRequirement(type, proto, resolvedSource,
                                      /*onlySameTypeRequirements=*/false);
}

ConstraintResult GenericSignatureBuilder::addLayoutRequirementDirect(
                                             ResolvedType type,
                                             LayoutConstraint layout,
                                             FloatingRequirementSource source) {
  auto resolvedSource = source.getSource(*this, type);

  if (!resolvedSource->isDerivedRequirement()) {
    Impl->ExplicitRequirements.insert(
        ExplicitRequirement(RequirementKind::Layout,
                            resolvedSource, layout));
  }

  // Update the layout in the equivalence class, if we didn't have one already.
  bool anyChanges = updateLayout(type, layout);

  // Record this layout constraint.
  auto equivClass = type.getEquivalenceClass(*this);
  equivClass->layoutConstraints.push_back({type.getUnresolvedType(),
                                           layout, resolvedSource});
  equivClass->modified(*this);
  ++NumLayoutConstraints;
  if (!anyChanges) ++NumLayoutConstraintsExtra;

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addLayoutRequirement(
                                             UnresolvedType subject,
                                             LayoutConstraint layout,
                                             FloatingRequirementSource source,
                                             UnresolvedHandlingKind unresolvedHandling) {
  // Resolve the subject.
  auto resolvedSubject = resolve(subject, source);
  if (!resolvedSubject) {
    return handleUnresolvedRequirement(
                               RequirementKind::Layout, subject,
                               layout, source,
                               resolvedSubject.getUnresolvedEquivClass(),
                               unresolvedHandling);
  }

  // If this layout constraint applies to a concrete type, we can fully
  // resolve it now.
  if (auto concreteType = resolvedSubject.getAsConcreteType()) {
    // If a layout requirement was explicitly written on a concrete type,
    // complain.
    if (source.isExplicit() && source.getLoc().isValid()) {
      Impl->HadAnyError = true;

      Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                     concreteType);
      return ConstraintResult::Concrete;
    }

    // FIXME: Check whether the layout constraint makes sense for this
    // concrete type!

    return ConstraintResult::Resolved;
  }

  return addLayoutRequirementDirect(resolvedSubject, layout, source);
}

bool GenericSignatureBuilder::updateSuperclass(
                                           ResolvedType type,
                                           Type superclass,
                                           FloatingRequirementSource source) {
  auto equivClass = type.getEquivalenceClass(*this);

  // Local function to handle the update of superclass conformances
  // when the superclass constraint changes.
  auto updateSuperclassConformances = [&] {
    for (const auto &conforms : equivClass->conformsTo) {
      (void)resolveSuperConformance(type, conforms.first);
    }

    // Eagerly resolve any existing nested types to their concrete forms (others
    // will be "concretized" as they are constructed, in getNestedType).
    for (auto equivT : equivClass->members) {
      for (auto nested : equivT->getNestedTypes()) {
        concretizeNestedTypeFromConcreteParent(equivT, nested.second.front(),
                                               *this);
      }
    }
  };

  // If we haven't yet recorded a superclass constraint for this equivalence
  // class, do so now.
  if (!equivClass->superclass) {
    equivClass->superclass = superclass;
    updateSuperclassConformances();

    // Presence of a superclass constraint implies a _Class layout
    // constraint.
    auto layoutReqSource =
      source.getSource(*this, type)->viaLayout(*this, superclass);

    auto layout =
      LayoutConstraint::getLayoutConstraint(
        superclass->getClassOrBoundGenericClass()->isObjC()
          ? LayoutConstraintKind::Class
          : LayoutConstraintKind::NativeClass,
        getASTContext());
    addLayoutRequirementDirect(type, layout, layoutReqSource);
    return true;
  }

  // T already has a superclass; make sure it's related.
  auto existingSuperclass = equivClass->superclass;
  // TODO: In principle, this could be isBindableToSuperclassOf instead of
  // isExactSubclassOf. If you had:
  //
  //   class Foo<T>
  //   class Bar: Foo<Int>
  //
  //   func foo<T, U where U: Foo<T>, U: Bar>(...) { ... }
  //
  // then the second constraint should be allowed, constraining U to Bar
  // and secondarily imposing a T == Int constraint.
  if (existingSuperclass->isExactSuperclassOf(superclass)) {
    equivClass->superclass = superclass;

    // We've strengthened the bound, so update superclass conformances.
    updateSuperclassConformances();
    return true;
  }

  return false;
}

bool GenericSignatureBuilder::updateLayout(ResolvedType type,
                                           LayoutConstraint layout) {
  auto equivClass = type.getEquivalenceClass(*this);

  if (!equivClass->layout) {
    equivClass->layout = layout;
    return true;
  }

  // Try to merge layout constraints.
  auto mergedLayout = equivClass->layout.merge(layout);
  if (mergedLayout->isKnownLayout() && mergedLayout != equivClass->layout) {
    equivClass->layout = mergedLayout;
    return true;
  }

  return false;
}

ConstraintResult GenericSignatureBuilder::addSuperclassRequirementDirect(
                                            ResolvedType type,
                                            Type superclass,
                                            FloatingRequirementSource source) {
  auto resolvedSource = source.getSource(*this, type);

  if (!resolvedSource->isDerivedRequirement()) {
    Impl->ExplicitRequirements.insert(
        ExplicitRequirement(RequirementKind::Superclass,
                            resolvedSource, superclass));
  }

  // Record the constraint.
  auto equivClass = type.getEquivalenceClass(*this);
  equivClass->superclassConstraints.push_back(
    ConcreteConstraint{type.getUnresolvedType(), superclass, resolvedSource});
  equivClass->modified(*this);
  ++NumSuperclassConstraints;

  // Update the equivalence class with the constraint.
  if (!updateSuperclass(type, superclass, resolvedSource))
    ++NumSuperclassConstraintsExtra;

  return ConstraintResult::Resolved;
}

/// Map an unresolved type to a requirement right-hand-side.
static GenericSignatureBuilder::UnresolvedRequirementRHS
toUnresolvedRequirementRHS(GenericSignatureBuilder::UnresolvedType unresolved) {
  if (auto pa = unresolved.dyn_cast<PotentialArchetype *>())
    return pa;

  return unresolved.dyn_cast<Type>();
}

ConstraintResult GenericSignatureBuilder::addTypeRequirement(
    UnresolvedType subject, UnresolvedType constraint,
    FloatingRequirementSource source, UnresolvedHandlingKind unresolvedHandling,
    ModuleDecl *inferForModule) {
  // Resolve the constraint.
  auto resolvedConstraint = resolve(constraint, source);
  if (!resolvedConstraint) {
    return handleUnresolvedRequirement(
                             RequirementKind::Conformance, subject,
                             toUnresolvedRequirementRHS(constraint), source,
                             resolvedConstraint.getUnresolvedEquivClass(),
                             unresolvedHandling);
  }

  // The right-hand side needs to be concrete.
  Type constraintType = resolvedConstraint.getAsConcreteType();
  if (!constraintType) {
    constraintType = resolvedConstraint.getDependentType(*this);
    assert(constraintType && "No type to express resolved constraint?");
  }

  // Resolve the subject. If we can't, delay the constraint.
  auto resolvedSubject = resolve(subject, source);
  if (!resolvedSubject) {
    auto recordedKind =
      constraintType->isExistentialType()
        ? RequirementKind::Conformance
        : RequirementKind::Superclass;
    return handleUnresolvedRequirement(
                               recordedKind, subject, constraintType,
                               source,
                               resolvedSubject.getUnresolvedEquivClass(),
                               unresolvedHandling);
  }

  // If the resolved subject is concrete, there may be things we can infer (if it
  // conditionally conforms to the protocol), and we can probably perform
  // diagnostics here.
  if (auto subjectType = resolvedSubject.getAsConcreteType()) {
    if (constraintType->isExistentialType()) {
      auto layout = constraintType->getExistentialLayout();
      for (auto *proto : layout.getProtocols()) {
        // We have a pure concrete type, and there's no guarantee of dependent
        // type that makes sense to use here, but, in practice, all
        // getLookupConformanceFns used in here don't use that parameter anyway.
        auto dependentType = CanType();
        auto conformance =
            getLookupConformanceFn()(dependentType, subjectType, proto->getDecl());

        // FIXME: diagnose if there's no conformance.
        if (conformance) {
          // Only infer conditional requirements from explicit sources.
          if (!source.isDerived()) {
            addConditionalRequirements(conformance, inferForModule);
          }
        }
      }
    }

    // One cannot explicitly write a constraint on a concrete type.
    if (source.isExplicit()) {
      if (source.getLoc().isValid() && !subjectType->hasError()) {
        Impl->HadAnyError = true;
        Diags.diagnose(source.getLoc(), diag::requires_not_suitable_archetype,
                       subjectType);
      }

      return ConstraintResult::Concrete;
    }

    return ConstraintResult::Resolved;
  }

  // Check whether we have a reasonable constraint type at all.
  if (!constraintType->isExistentialType() &&
      !constraintType->getClassOrBoundGenericClass()) {
    if (source.getLoc().isValid() && !constraintType->hasError()) {
      Impl->HadAnyError = true;

      auto invalidConstraint = Constraint<Type>(
          {subject, constraintType, source.getSource(*this, resolvedSubject)});
      invalidIsaConstraints.push_back(invalidConstraint);
    }

    return ConstraintResult::Conflicting;
  }

  // Protocol requirements.
  if (constraintType->isExistentialType()) {
    bool anyErrors = false;
    auto layout = constraintType->getExistentialLayout();

    if (auto layoutConstraint = layout.getLayoutConstraint()) {
      if (isErrorResult(addLayoutRequirementDirect(resolvedSubject,
                                                   layoutConstraint,
                                                   source)))
        anyErrors = true;
    }

    if (auto superclass = layout.explicitSuperclass) {
      if (isErrorResult(addSuperclassRequirementDirect(resolvedSubject,
                                                       superclass,
                                                       source)))
        anyErrors = true;
    }

    for (auto *proto : layout.getProtocols()) {
      auto *protoDecl = proto->getDecl();
      if (isErrorResult(addConformanceRequirement(resolvedSubject, protoDecl,
                                                  source)))
        anyErrors = true;
    }

    return anyErrors ? ConstraintResult::Conflicting
                     : ConstraintResult::Resolved;
  }

  // Superclass constraint.
  return addSuperclassRequirementDirect(resolvedSubject, constraintType,
                                        source);
}

void GenericSignatureBuilder::addedNestedType(PotentialArchetype *nestedPA) {
  // If there was already another type with this name within the parent
  // potential archetype, equate this type with that one.
  auto parentPA = nestedPA->getParent();
  auto &allNested = parentPA->NestedTypes[nestedPA->getResolvedType()->getName()];
  assert(!allNested.empty());
  assert(allNested.back() == nestedPA);
  if (allNested.size() > 1) {
    auto firstPA = allNested.front();
    auto inferredSource =
      FloatingRequirementSource::forNestedTypeNameMatch(
        nestedPA->getResolvedType()->getName());

    addSameTypeRequirement(firstPA, nestedPA, inferredSource,
                           UnresolvedHandlingKind::GenerateConstraints);
    return;
  }

  // If our parent type is not the representative, equate this nested
  // potential archetype to the equivalent nested type within the
  // representative.
  auto parentRepPA = parentPA->getRepresentative();
  if (parentPA == parentRepPA) return;

  PotentialArchetype *existingPA =
    parentRepPA->getOrCreateNestedType(*this,
                                       nestedPA->getResolvedType(),
                                       ArchetypeResolutionKind::WellFormed);

  auto sameNamedSource =
    FloatingRequirementSource::forNestedTypeNameMatch(
                                       nestedPA->getResolvedType()->getName());
  addSameTypeRequirement(existingPA, nestedPA, sameNamedSource,
                         UnresolvedHandlingKind::GenerateConstraints);
}

ConstraintResult
GenericSignatureBuilder::addSameTypeRequirementBetweenTypeParameters(
                                         ResolvedType type1, ResolvedType type2,
                                         const RequirementSource *source)
{
  ++NumSameTypeConstraints;

  Type depType1 = type1.getDependentType(*this);
  Type depType2 = type2.getDependentType(*this);

  if (!source->isDerivedRequirement()) {
    Impl->ExplicitSameTypeRequirements.emplace_back(RequirementKind::SameType,
                                                    source, depType1);
  }

  // Record the same-type constraint, and bail out if it was already known.
  auto equivClass = type1.getEquivalenceClassIfPresent();
  auto equivClass2 = type2.getEquivalenceClassIfPresent();
  if (depType1->isEqual(depType2) ||
      ((equivClass || equivClass2) && equivClass == equivClass2)) {
    // Make sure we have an equivalence class in which we can record the
    // same-type constraint.
    if (!equivClass) {
      if (equivClass2) {
        equivClass = equivClass2;
      } else {
        equivClass = type1.getEquivalenceClass(*this);
      }
    }

    // FIXME: We could determine equivalence based on both sides canonicalizing
    // to the same type. 
    equivClass->sameTypeConstraints.push_back({depType1, depType2, source});
    return ConstraintResult::Resolved;
  }

  // Both sides are type parameters; equate them.
  // FIXME: Realizes potential archetypes far too early.
  auto OrigT1 = type1.getPotentialArchetypeIfKnown();
  auto OrigT2 = type2.getPotentialArchetypeIfKnown();

  // Operate on the representatives
  auto T1 = OrigT1->getRepresentative();
  auto T2 = OrigT2->getRepresentative();

  // Decide which potential archetype is to be considered the representative.
  // We prefer potential archetypes with lower nesting depths, because it
  // prevents us from unnecessarily building deeply nested potential archetypes.
  unsigned nestingDepth1 = T1->getNestingDepth();
  unsigned nestingDepth2 = T2->getNestingDepth();
  if (nestingDepth2 < nestingDepth1) {
    std::swap(T1, T2);
    std::swap(OrigT1, OrigT2);
    std::swap(equivClass, equivClass2);
  }

  // T1 must have an equivalence class; create one if we don't already have
  // one.
  if (!equivClass)
    equivClass = T1->getOrCreateEquivalenceClass(*this);

  // Record this same-type constraint.
  // Let's keep type order in the new constraint the same as it's written
  // in source, which makes it much easier to diagnose later.
  equivClass->sameTypeConstraints.push_back({depType1, depType2, source});

  // Determine the anchor types of the two equivalence classes.
  CanType anchor1 = equivClass->getAnchor(*this, { })->getCanonicalType();
  CanType anchor2 =
    (equivClass2 ? equivClass2->getAnchor(*this, { })
                 : getCanonicalTypeParameter(T2->getDependentType()))
      ->getCanonicalType();

  // Merge the equivalence classes.
  equivClass->modified(*this);
  auto equivClass1Members = equivClass->members;
  auto equivClass2Members = T2->getEquivalenceClassMembers();
  for (auto equiv : equivClass2Members)
    equivClass->addMember(equiv);

  // Grab the old equivalence class, if present. We'll deallocate it at the end.
  SWIFT_DEFER {
    if (equivClass2)
      Impl->deallocateEquivalenceClass(equivClass2);
  };

  // Consider the second equivalence class to be modified.
  // Transfer Same-type requirements and delayed requirements.
  if (equivClass2) {
    // Mark as modified and transfer deplayed requirements to the primary queue.
    equivClass2->modified(*this);
    equivClass->sameTypeConstraints.insert(
                                   equivClass->sameTypeConstraints.end(),
                                   equivClass2->sameTypeConstraints.begin(),
                                   equivClass2->sameTypeConstraints.end());
  }

  // Combine the rewrite rules.
  auto *rewriteRoot1 = Impl->getOrCreateRewriteTreeRoot(anchor1);
  auto *rewriteRoot2 = Impl->getOrCreateRewriteTreeRoot(anchor2);
  assert(rewriteRoot1 && rewriteRoot2 &&
         "Couldn't create/retrieve rewrite tree root");
  // Merge the second rewrite tree into the first.
  if (rewriteRoot2->mergeInto(rewriteRoot1))
    ++Impl->RewriteGeneration;
  Impl->RewriteTreeRoots.erase(anchor2);

  // Add a rewrite rule to map the anchor of T2 down to the anchor of T1.
  if (addSameTypeRewriteRule(anchor2, anchor1))
    ++Impl->RewriteGeneration;

  // Same-type-to-concrete requirements.
  bool t1IsConcrete = !equivClass->concreteType.isNull();
  bool t2IsConcrete = equivClass2 && !equivClass2->concreteType.isNull();
  if (t2IsConcrete) {
    if (t1IsConcrete) {
      (void)addSameTypeRequirement(equivClass->concreteType,
                                   equivClass2->concreteType, source,
                                   UnresolvedHandlingKind::GenerateConstraints,
                                   SameTypeConflictCheckedLater());
    } else {
      equivClass->concreteType = equivClass2->concreteType;
      equivClass->invalidConcreteType = equivClass2->invalidConcreteType;
    }

    equivClass->concreteTypeConstraints.insert(
                                 equivClass->concreteTypeConstraints.end(),
                                 equivClass2->concreteTypeConstraints.begin(),
                                 equivClass2->concreteTypeConstraints.end());

    for (const auto &conforms : equivClass->conformsTo)
      (void)resolveConcreteConformance(T1, conforms.first);
  }

  // Make T1 the representative of T2, merging the equivalence classes.
  T2->representativeOrEquivClass = T1;

  // Layout requirements.
  if (equivClass2 && equivClass2->layout) {
    if (!equivClass->layout) {
      equivClass->layout = equivClass2->layout;
      equivClass->layoutConstraints = std::move(equivClass2->layoutConstraints);
    } else {
      updateLayout(T1, equivClass2->layout);
      equivClass->layoutConstraints.insert(
                                   equivClass->layoutConstraints.end(),
                                   equivClass2->layoutConstraints.begin(),
                                   equivClass2->layoutConstraints.end());
    }
  }

  // Superclass requirements.
  if (equivClass2 && equivClass2->superclass) {
    const RequirementSource *source2;
    if (auto existingSource2 =
          equivClass2->findAnySuperclassConstraintAsWritten(
            OrigT2->getDependentType()))
      source2 = existingSource2->source;
    else
      source2 = equivClass2->superclassConstraints.front().source;

    // Add the superclass constraints from the second equivalence class.
    equivClass->superclassConstraints.insert(
                                   equivClass->superclassConstraints.end(),
                                   equivClass2->superclassConstraints.begin(),
                                   equivClass2->superclassConstraints.end());

    (void)updateSuperclass(T1, equivClass2->superclass, source2);
  }

  // Add all of the protocol conformance requirements of T2 to T1.
  if (equivClass2) {
    for (const auto &entry : equivClass2->conformsTo) {
      equivClass->recordConformanceConstraint(*this, T1, entry.first,
                                              entry.second.front().source);

      auto &constraints1 = equivClass->conformsTo[entry.first];
      // FIXME: Go through recordConformanceConstraint()?
      constraints1.insert(constraints1.end(),
                          entry.second.begin() + 1,
                          entry.second.end());
    }
  }

  // Recursively merge the associated types of T2 into T1.
  auto dependentT1 = T1->getDependentType(getGenericParams());
  SmallPtrSet<Identifier, 4> visited;
  for (auto equivT2 : equivClass2Members) {
    for (auto T2Nested : equivT2->NestedTypes) {
      // Only visit each name once.
      if (!visited.insert(T2Nested.first).second) continue;

      // If T1 is concrete but T2 is not, concretize the nested types of T2.
      if (t1IsConcrete && !t2IsConcrete) {
        concretizeNestedTypeFromConcreteParent(T1, T2Nested.second.front(),
                                               *this);
        continue;
      }

      // Otherwise, make the nested types equivalent.
      AssociatedTypeDecl *assocTypeT2 = nullptr;
      for (auto T2 : T2Nested.second) {
        assocTypeT2 = T2->getResolvedType();
        if (assocTypeT2) break;
      }

      if (!assocTypeT2) continue;

      Type nestedT1 = DependentMemberType::get(dependentT1, assocTypeT2);
      if (isErrorResult(
            addSameTypeRequirement(
               nestedT1, T2Nested.second.front(),
               FloatingRequirementSource::forNestedTypeNameMatch(
                                                      assocTypeT2->getName()),
               UnresolvedHandlingKind::GenerateConstraints)))
        return ConstraintResult::Conflicting;
    }
  }

  // If T2 is concrete but T1 was not, concretize the nested types of T1.
  visited.clear();
  if (t2IsConcrete && !t1IsConcrete) {
    for (auto equivT1 : equivClass1Members) {
      for (auto T1Nested : equivT1->NestedTypes) {
        // Only visit each name once.
        if (!visited.insert(T1Nested.first).second) continue;

        concretizeNestedTypeFromConcreteParent(T2, T1Nested.second.front(),
                                               *this);
      }
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementToConcrete(
                                           ResolvedType type,
                                           Type concrete,
                                           const RequirementSource *source) {
  if (!source->isDerivedRequirement()) {
    Impl->ExplicitSameTypeRequirements.emplace_back(RequirementKind::SameType,
                                                    source, concrete);
  }

  auto equivClass = type.getEquivalenceClass(*this);

  // Record the concrete type and its source.
  equivClass->concreteTypeConstraints.push_back(
    ConcreteConstraint{type.getUnresolvedType(), concrete, source});
  equivClass->modified(*this);
  ++NumConcreteTypeConstraints;

  // If we've already been bound to a type, match that type.
  if (equivClass->concreteType) {
    return addSameTypeRequirement(equivClass->concreteType, concrete, source,
                                  UnresolvedHandlingKind::GenerateConstraints,
                                  SameTypeConflictCheckedLater());

  }

  // Record the requirement.
  equivClass->concreteType = concrete;

  // Make sure the concrete type fulfills the conformance requirements of
  // this equivalence class.
  for (const auto &conforms : equivClass->conformsTo) {
    if (!resolveConcreteConformance(type, conforms.first))
      return ConstraintResult::Conflicting;
  }

  // Eagerly resolve any existing nested types to their concrete forms (others
  // will be "concretized" as they are constructed, in getNestedType).
  for (auto equivT : equivClass->members) {
    for (auto nested : equivT->getNestedTypes()) {
      concretizeNestedTypeFromConcreteParent(equivT, nested.second.front(),
                                             *this);
    }
  }

  return ConstraintResult::Resolved;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementBetweenConcrete(
    Type type1, Type type2, FloatingRequirementSource source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  // Local class to handle matching the two sides of the same-type constraint.
  class ReqTypeMatcher : public TypeMatcher<ReqTypeMatcher> {
    GenericSignatureBuilder &builder;
    FloatingRequirementSource source;
    Type outerType1, outerType2;
    llvm::function_ref<void(Type, Type)> diagnoseMismatch;

  public:
    ReqTypeMatcher(GenericSignatureBuilder &builder,
                   FloatingRequirementSource source,
                   Type outerType1, Type outerType2,
                   llvm::function_ref<void(Type, Type)> diagnoseMismatch)
        : builder(builder), source(source), outerType1(outerType1),
          outerType2(outerType2), diagnoseMismatch(diagnoseMismatch) {}

    bool mismatch(TypeBase *firstType, TypeBase *secondType,
                  Type sugaredFirstType) {
      // If the mismatch was in the first layer (i.e. what was fed to
      // addSameTypeRequirementBetweenConcrete), then this is a fundamental
      // mismatch, and we need to diagnose it. This is what breaks the mutual
      // recursion between addSameTypeRequirement and
      // addSameTypeRequirementBetweenConcrete.
      if (outerType1->isEqual(firstType) && outerType2->isEqual(secondType)) {
        diagnoseMismatch(sugaredFirstType, secondType);
        return false;
      }

      auto failed = builder.addSameTypeRequirement(
          sugaredFirstType, Type(secondType), source,
          UnresolvedHandlingKind::GenerateConstraints, diagnoseMismatch);
      return !isErrorResult(failed);
    }
  } matcher(*this, source, type1, type2, diagnoseMismatch);

  if (matcher.match(type1, type2)) {
    // Warn if neither side of the requirement contains a type parameter.
    if (source.isTopLevel() && source.getLoc().isValid()) {
      Diags.diagnose(source.getLoc(),
                     diag::requires_no_same_type_archetype,
                     type1, type2);
    }

    return ConstraintResult::Resolved;
  }

  return ConstraintResult::Conflicting;
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirement(
                                 UnresolvedType paOrT1,
                                 UnresolvedType paOrT2,
                                 FloatingRequirementSource source,
                                 UnresolvedHandlingKind unresolvedHandling) {
  return addSameTypeRequirement(paOrT1, paOrT2, source, unresolvedHandling,
                                [&](Type type1, Type type2) {
      Impl->HadAnyError = true;
      if (source.getLoc().isValid() &&
          !type1->hasError() &&
          !type2->hasError()) {
        Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                       type1, type2);
      }
    });
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirement(
    UnresolvedType paOrT1, UnresolvedType paOrT2,
    FloatingRequirementSource source,
    UnresolvedHandlingKind unresolvedHandling,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {

  auto resolved1 = resolve(paOrT1, source);
  if (!resolved1) {
    return handleUnresolvedRequirement(RequirementKind::SameType, paOrT1,
                                       toUnresolvedRequirementRHS(paOrT2),
                                       source,
                                       resolved1.getUnresolvedEquivClass(),
                                       unresolvedHandling);
  }

  auto resolved2 = resolve(paOrT2, source);
  if (!resolved2) {
    return handleUnresolvedRequirement(RequirementKind::SameType, paOrT1,
                                       toUnresolvedRequirementRHS(paOrT2),
                                       source,
                                       resolved2.getUnresolvedEquivClass(),
                                       unresolvedHandling);
  }

  return addSameTypeRequirementDirect(resolved1, resolved2, source,
                                      diagnoseMismatch);
}

ConstraintResult GenericSignatureBuilder::addSameTypeRequirementDirect(
    ResolvedType type1, ResolvedType type2,
    FloatingRequirementSource source,
    llvm::function_ref<void(Type, Type)> diagnoseMismatch) {
  auto concreteType1 = type1.getAsConcreteType();
  auto concreteType2 = type2.getAsConcreteType();

  // If both sides of the requirement are concrete, equate them.
  if (concreteType1 && concreteType2) {
    return addSameTypeRequirementBetweenConcrete(concreteType1,
                                                 concreteType2, source,
                                                 diagnoseMismatch);
  }

  // If one side is concrete, map the other side to that concrete type.
  if (concreteType1) {
    return addSameTypeRequirementToConcrete(type2, concreteType1,
                                            source.getSource(*this, type2));
  }

  if (concreteType2) {
    return addSameTypeRequirementToConcrete(type1, concreteType2,
                                            source.getSource(*this, type1));
  }

  return addSameTypeRequirementBetweenTypeParameters(
                     type1, type2,
                     source.getSource(*this, type2));
}

ConstraintResult GenericSignatureBuilder::addInheritedRequirements(
                             TypeDecl *decl,
                             UnresolvedType type,
                             const RequirementSource *parentSource,
                             ModuleDecl *inferForModule) {
  // Local function to get the source.
  auto getFloatingSource = [&](const TypeRepr *typeRepr, bool forInferred) {
    auto loc = typeRepr ? typeRepr->getStartLoc() : SourceLoc();

    if (parentSource) {
      ProtocolDecl *proto;
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(decl)) {
        proto = assocType->getProtocol();
      } else {
        proto = cast<ProtocolDecl>(decl);
      }

      return FloatingRequirementSource::viaProtocolRequirement(
        parentSource, proto, loc, forInferred);
    }

    // We are inferring requirements.
    if (forInferred) {
      return FloatingRequirementSource::forInferred(loc);
    }

    // Explicit requirement.
    return FloatingRequirementSource::forExplicit(loc);
  };

  auto visitType = [&](Type inheritedType, const TypeRepr *typeRepr) {
    if (inferForModule) {
      inferRequirements(*inferForModule, inheritedType,
                        getFloatingSource(typeRepr, /*forInferred=*/true));
    }

    return addTypeRequirement(
        type, inheritedType, getFloatingSource(typeRepr,
                                               /*forInferred=*/false),
        UnresolvedHandlingKind::GenerateConstraints, inferForModule);
  };

  return visitInherited(decl, visitType);
}

ConstraintResult
GenericSignatureBuilder::addRequirement(const Requirement &req,
                                        FloatingRequirementSource source,
                                        ModuleDecl *inferForModule) {
  return addRequirement(req, nullptr, source, nullptr, inferForModule);
}

ConstraintResult
GenericSignatureBuilder::addRequirement(const Requirement &req,
                                        const RequirementRepr *reqRepr,
                                        FloatingRequirementSource source,
                                        const SubstitutionMap *subMap,
                                        ModuleDecl *inferForModule) {
  // Local substitution for types in the requirement.
  auto subst = [&](Type t) {
    if (subMap)
      return t.subst(*subMap);

    return t;
  };

  auto firstType = subst(req.getFirstType());
  switch (req.getKind()) {
  case RequirementKind::Superclass:
  case RequirementKind::Conformance: {
    auto secondType = subst(req.getSecondType());

    if (inferForModule) {
      inferRequirements(*inferForModule, firstType,
                        source.asInferred(
                          RequirementRepr::getFirstTypeRepr(reqRepr)));
      inferRequirements(*inferForModule, secondType,
                        source.asInferred(
                          RequirementRepr::getSecondTypeRepr(reqRepr)));
    }

    return addTypeRequirement(firstType, secondType, source,
                              UnresolvedHandlingKind::GenerateConstraints,
                              inferForModule);
  }

  case RequirementKind::Layout: {
    if (inferForModule) {
      inferRequirements(*inferForModule, firstType,
                        source.asInferred(
                          RequirementRepr::getFirstTypeRepr(reqRepr)));
    }

    return addLayoutRequirement(firstType, req.getLayoutConstraint(), source,
                                UnresolvedHandlingKind::GenerateConstraints);
  }

  case RequirementKind::SameType: {
    auto secondType = subst(req.getSecondType());

    if (inferForModule) {
      inferRequirements(*inferForModule, firstType,
                        source.asInferred(
                          RequirementRepr::getFirstTypeRepr(reqRepr)));
      inferRequirements(*inferForModule, secondType,
                        source.asInferred(
                          RequirementRepr::getSecondTypeRepr(reqRepr)));
    }

    return addSameTypeRequirement(
        firstType, secondType, source,
        UnresolvedHandlingKind::GenerateConstraints,
        [&](Type type1, Type type2) {
          Impl->HadAnyError = true;
          if (source.getLoc().isValid() &&
              !type1->hasError() &&
              !type2->hasError()) {
            Diags.diagnose(source.getLoc(), diag::requires_same_concrete_type,
                           type1, type2);
          }
        });
  }
  }

  llvm_unreachable("Unhandled requirement?");
}

/// AST walker that infers requirements from type representations.
class GenericSignatureBuilder::InferRequirementsWalker : public TypeWalker {
  ModuleDecl &module;
  GenericSignatureBuilder &Builder;
  FloatingRequirementSource source;

public:
  InferRequirementsWalker(ModuleDecl &module,
                          GenericSignatureBuilder &builder,
                          FloatingRequirementSource source)
    : module(module), Builder(builder), source(source) { }

  Action walkToTypePre(Type ty) override {
    // Unbound generic types are the result of recovered-but-invalid code, and
    // don't have enough info to do any useful substitutions.
    if (ty->is<UnboundGenericType>())
      return Action::Stop;

    return Action::Continue;
  }

  Action walkToTypePost(Type ty) override {
    // Infer from generic typealiases.
    if (auto TypeAlias = dyn_cast<TypeAliasType>(ty.getPointer())) {
      auto decl = TypeAlias->getDecl();
      auto genericSig = decl->getGenericSignature();
      if (!genericSig)
        return Action::Continue;

      auto subMap = TypeAlias->getSubstitutionMap();
      for (const auto &rawReq : genericSig->getRequirements()) {
        if (auto req = rawReq.subst(subMap))
          Builder.addRequirement(*req, source, nullptr);
      }

      return Action::Continue;
    }

    // Infer requirements from `@differentiable` function types.
    // For all non-`@noDerivative` parameter and result types:
    // - `@differentiable`, `@differentiable(_forward)`, or
    //   `@differentiable(reverse)`: add `T: Differentiable` requirement.
    // - `@differentiable(_linear)`: add
    //   `T: Differentiable`, `T == T.TangentVector` requirements.
    if (auto *fnTy = ty->getAs<AnyFunctionType>()) {
      auto &ctx = Builder.getASTContext();
      auto *differentiableProtocol =
          ctx.getProtocol(KnownProtocolKind::Differentiable);
      if (differentiableProtocol && fnTy->isDifferentiable()) {
        auto addConformanceConstraint = [&](Type type, ProtocolDecl *protocol) {
          Requirement req(RequirementKind::Conformance, type,
                          protocol->getDeclaredInterfaceType());
          Builder.addRequirement(req, source, nullptr);
        };
        auto addSameTypeConstraint = [&](Type firstType,
                                         AssociatedTypeDecl *assocType) {
          auto *protocol = assocType->getProtocol();
          auto conf = Builder.lookupConformance(firstType, protocol);
          auto secondType = conf.getAssociatedType(
              firstType, assocType->getDeclaredInterfaceType());
          Requirement req(RequirementKind::SameType, firstType, secondType);
          Builder.addRequirement(req, source, nullptr);
        };
        auto *tangentVectorAssocType =
            differentiableProtocol->getAssociatedType(ctx.Id_TangentVector);
        auto addRequirements = [&](Type type, bool isLinear) {
          addConformanceConstraint(type, differentiableProtocol);
          if (isLinear)
            addSameTypeConstraint(type, tangentVectorAssocType);
        };
        auto constrainParametersAndResult = [&](bool isLinear) {
          for (auto &param : fnTy->getParams())
            if (!param.isNoDerivative())
              addRequirements(param.getPlainType(), isLinear);
          addRequirements(fnTy->getResult(), isLinear);
        };
        // Add requirements.
        constrainParametersAndResult(fnTy->getDifferentiabilityKind() ==
                                     DifferentiabilityKind::Linear);
      }
    }

    if (!ty->isSpecialized())
      return Action::Continue;

    // Infer from generic nominal types.
    auto decl = ty->getAnyNominal();
    if (!decl) return Action::Continue;

    // FIXME: The GSB and the request evaluator both detect a cycle here if we
    // force a recursive generic signature.  We should look into moving cycle
    // detection into the generic signature request(s) - see rdar://55263708
    if (!decl->hasComputedGenericSignature())
      return Action::Continue;
    
    auto genericSig = decl->getGenericSignature();
    if (!genericSig)
      return Action::Continue;

    /// Retrieve the substitution.
    auto subMap = ty->getContextSubstitutionMap(&module, decl);

    // Handle the requirements.
    // FIXME: Inaccurate TypeReprs.
    for (const auto &rawReq : genericSig->getRequirements()) {
      if (auto req = rawReq.subst(subMap))
        Builder.addRequirement(*req, source, nullptr);
    }

    return Action::Continue;
  }
};

void GenericSignatureBuilder::inferRequirements(
                                          ModuleDecl &module,
                                          Type type,
                                          FloatingRequirementSource source) {
  if (!type)
    return;

  // FIXME: Crummy source-location information.
  InferRequirementsWalker walker(module, *this, source);
  type.walk(walker);
}

void GenericSignatureBuilder::inferRequirements(
                                          ModuleDecl &module,
                                          ParameterList *params) {
  for (auto P : *params) {
    inferRequirements(module, P->getInterfaceType(),
                      FloatingRequirementSource::forInferred(
                        P->getTypeRepr()->getStartLoc()));
  }
}

namespace swift {
  template<typename T>
  bool operator<(const Constraint<T> &lhs, const Constraint<T> &rhs) {
    // FIXME: Awful.
    auto lhsSource = lhs.getSubjectDependentType({ });
    auto rhsSource = rhs.getSubjectDependentType({ });
    if (int result = compareDependentTypes(lhsSource, rhsSource))
      return result < 0;

    if (int result = lhs.source->compare(rhs.source))
      return result < 0;

    return false;
  }

  template<typename T>
  bool operator==(const Constraint<T> &lhs, const Constraint<T> &rhs){
    return lhs.hasSameSubjectAs(rhs) &&
           lhs.value == rhs.value &&
           lhs.source == rhs.source;
  }

  template<>
  bool operator==(const Constraint<Type> &lhs, const Constraint<Type> &rhs){
    return lhs.hasSameSubjectAs(rhs) &&
           lhs.value->isEqual(rhs.value) &&
           lhs.source == rhs.source;
  }
} // namespace swift

namespace {
  /// Retrieve the representative constraint that will be used for diagnostics.
  template<typename T>
  Optional<Constraint<T>> findRepresentativeConstraint(
                            GenericSignatureBuilder &builder,
                            ArrayRef<Constraint<T>> constraints,
                            RequirementKind kind,
                            llvm::function_ref<bool(const Constraint<T> &)>
                                                   isSuitableRepresentative) {
    Optional<Constraint<T>> fallbackConstraint;

    // Find a representative constraint.
    Optional<Constraint<T>> representativeConstraint;
    for (const auto &constraint : constraints) {
      // Make sure we have a constraint to fall back on.
      if (!fallbackConstraint)
        fallbackConstraint = constraint;

      // If this isn't a suitable representative constraint, ignore it.
      if (!isSuitableRepresentative(constraint))
        continue;

      // Check whether this constraint is better than the best we've seen so far
      // at being the representative constraint against which others will be
      // compared.
      if (!representativeConstraint) {
        representativeConstraint = constraint;
        continue;
      }

      if (kind != RequirementKind::SameType) {
        // We prefer non-redundant explicit constraints over everything else.
        bool thisIsNonRedundantExplicit =
            (!constraint.source->isDerivedNonRootRequirement() &&
             !builder.isRedundantExplicitRequirement(
               ExplicitRequirement(
                 kind, constraint.source, constraint.value)));
        bool representativeIsNonRedundantExplicit =
            (!representativeConstraint->source->isDerivedNonRootRequirement() &&
             !builder.isRedundantExplicitRequirement(
               ExplicitRequirement(
                 kind, representativeConstraint->source, representativeConstraint->value)));

        if (thisIsNonRedundantExplicit != representativeIsNonRedundantExplicit) {
          if (thisIsNonRedundantExplicit)
            representativeConstraint = constraint;
          continue;
        }
      }

      // We prefer derived constraints to non-derived constraints.
      bool thisIsDerived = constraint.source->isDerivedRequirement();
      bool representativeIsDerived =
        representativeConstraint->source->isDerivedRequirement();
      if (thisIsDerived != representativeIsDerived) {
        if (thisIsDerived)
          representativeConstraint = constraint;

        continue;
      }

      // We prefer constraints that are explicit to inferred constraints.
      bool thisIsInferred = constraint.source->isInferredRequirement();
      bool representativeIsInferred =
        representativeConstraint->source->isInferredRequirement();
      if (thisIsInferred != representativeIsInferred) {
        if (thisIsInferred)
          representativeConstraint = constraint;

        continue;
      }

      // We prefer constraints with locations to constraints without locations.
      bool thisHasValidSourceLoc = constraint.source->getLoc().isValid();
      bool representativeHasValidSourceLoc =
        representativeConstraint->source->getLoc().isValid();
      if (thisHasValidSourceLoc != representativeHasValidSourceLoc) {
        if (thisHasValidSourceLoc)
          representativeConstraint = constraint;

        continue;
      }

      // Otherwise, order via the constraint itself.
      if (constraint < *representativeConstraint)
        representativeConstraint = constraint;
    }

    return representativeConstraint ? representativeConstraint
                                    : fallbackConstraint;
  }
} // end anonymous namespace

/// For each potential archetype within the given equivalence class that is
/// an associated type, expand the protocol requirements for the enclosing
/// protocol.
static void expandSameTypeConstraints(GenericSignatureBuilder &builder,
                                      EquivalenceClass *equivClass) {
  auto genericParams = builder.getGenericParams();
  auto existingMembers = equivClass->members;
  for (auto pa : existingMembers) {
    auto dependentType = pa->getDependentType(genericParams);
    for (const auto &conforms : equivClass->conformsTo) {
      auto proto = conforms.first;

      // Check whether we already have a conformance constraint for this
      // potential archetype.
      bool alreadyFound = false;
      const RequirementSource *conformsSource = nullptr;
      for (const auto &constraint : conforms.second) {
        auto *minimal = constraint.source->getMinimalConformanceSource(
            builder, constraint.getSubjectDependentType({ }), proto);

        if (minimal == nullptr)
          continue;

        if (minimal->getAffectedType()->isEqual(dependentType)) {
          alreadyFound = true;
          break;
        }

        // Capture the source for later use, skipping
        if (!conformsSource &&
            minimal->kind != RequirementSource::RequirementSignatureSelf)
          conformsSource = minimal;
      }

      if (alreadyFound) continue;
      if (!conformsSource) continue;

      // Pick a source at random and reseat it on this potential archetype.
      auto source = conformsSource->viaEquivalentType(builder, dependentType);

      // Expand same-type constraints.
      builder.expandConformanceRequirement(pa, proto, source,
                                           /*onlySameTypeConstraints=*/true);
    }
  }
}

void GenericSignatureBuilder::ExplicitRequirement::dump(
    llvm::raw_ostream &out, SourceManager *SM) const {
  switch (getKind()) {
  case RequirementKind::Conformance:
    out << "conforms_to: ";
    break;
  case RequirementKind::Layout:
    out << "layout: ";
    break;
  case RequirementKind::Superclass:
    out << "superclass: ";
    break;
  case RequirementKind::SameType:
    out << "same_type: ";
    break;
  }

  out << getSubjectType();
  if (getKind() == RequirementKind::SameType)
    out << " == ";
  else
    out << " : ";

  if (auto type = rhs.dyn_cast<Type>())
    out << type;
  else if (auto *proto = rhs.dyn_cast<ProtocolDecl *>())
    out << proto->getName();
  else
    out << rhs.get<LayoutConstraint>();

  out << "(source: ";
  getSource()->dump(out, SM, 0);
  out << ")";
}

static bool typeImpliesLayoutConstraint(Type t, LayoutConstraint layout) {
  if (layout->isRefCounted() && t->satisfiesClassConstraint())
    return true;

  return false;
}

static bool typeConflictsWithLayoutConstraint(Type t, LayoutConstraint layout) {
  if (layout->isClass() && !t->satisfiesClassConstraint())
    return true;

  return false;
}

static bool isConcreteConformance(const EquivalenceClass &equivClass,
                                  ProtocolDecl *proto,
                                  GenericSignatureBuilder &builder) {
  if (equivClass.concreteType &&
      builder.lookupConformance(equivClass.concreteType, proto)) {
    return true;
  } else if (equivClass.superclass &&
           builder.lookupConformance(equivClass.superclass, proto)) {
    return true;
  }

  return false;
}

void GenericSignatureBuilder::getBaseRequirements(
    GenericSignatureBuilder::GetKindAndRHS getKindAndRHS,
    const RequirementSource *source,
    const ProtocolDecl *requirementSignatureSelfProto,
    SmallVectorImpl<ExplicitRequirement> &result) {
  // If we're building a generic signature, the base requirement is
  // built from the root of the path.
  if (source->parent == nullptr) {
    RequirementKind kind;
    RequirementRHS rhs;
    std::tie(kind, rhs) = getKindAndRHS();
    result.push_back(ExplicitRequirement(kind, source, rhs));
    return;
  }

  // If we're building a requirement signature, there can be multiple
  // base requirements from the same protocol appearing along the path.
  if (requirementSignatureSelfProto != nullptr &&
      source->isProtocolRequirement() &&
      source->getProtocolDecl() == requirementSignatureSelfProto) {
    auto *shortSource = source->withoutRedundantSubpath(
        *this, source->getRoot(), source->parent);
    RequirementKind kind;
    RequirementRHS rhs;
    std::tie(kind, rhs) = getKindAndRHS();
    result.push_back(ExplicitRequirement(kind, shortSource, rhs));
  }

  getBaseRequirements(
      [&]() { return ExplicitRequirement::getKindAndRHS(source, Context); },
      source->parent, requirementSignatureSelfProto, result);
}

Optional<ExplicitRequirement>
GenericSignatureBuilder::isValidRequirementDerivationPath(
    llvm::SmallDenseSet<ExplicitRequirement, 4> &visited,
    RequirementKind otherKind,
    const RequirementSource *otherSource,
    RequirementRHS otherRHS,
    const ProtocolDecl *requirementSignatureSelfProto) {
  if (auto *Stats = Context.Stats)
    ++Stats->getFrontendCounters().NumRedundantRequirementSteps;

  SmallVector<ExplicitRequirement, 2> result;
  getBaseRequirements(
      [&]() { return std::make_pair(otherKind, otherRHS); },
      otherSource, requirementSignatureSelfProto, result);
  assert(result.size() > 0);

  for (const auto &otherReq : result) {
    // Don't consider paths that are based on the requirement
    // itself; such a path doesn't "prove" this requirement,
    // since if we drop the requirement the path is no longer
    // valid.
    if (visited.count(otherReq))
      return None;

    SWIFT_DEFER {
      visited.erase(otherReq);
    };
    visited.insert(otherReq);

    auto otherSubjectType = otherReq.getSubjectType();

    // If our requirement is based on a path involving some other
    // redundant requirement, see if we can derive the redundant
    // requirement using requirements we haven't visited yet.
    // If not, we go ahead and drop it from consideration.
    //
    // We need to do this because sometimes, we don't record all possible
    // requirement sources that derive a given requirement.
    //
    // For example, when a nested type of a type parameter is concretized by
    // adding a superclass requirement, we only record the requirement source
    // for the concrete conformance the first time. A subsequently-added
    // superclass requirement on the same parent type does not record a
    // redundant concrete conformance for the child type.
    if (isRedundantExplicitRequirement(otherReq)) {
      // If we have a redundant explicit requirement source, it really is
      // redundant; there's no other derivation that would not be redundant.
      if (!otherSource->isDerivedNonRootRequirement())
        return None;

      auto *equivClass = resolveEquivalenceClass(otherSubjectType,
                                           ArchetypeResolutionKind::AlreadyKnown);
      assert(equivClass &&
             "Explicit requirement names an unknown equivalence class?");

      switch (otherReq.getKind()) {
      case RequirementKind::Conformance: {
        auto *proto = otherReq.getRHS().get<ProtocolDecl *>();

        auto found = equivClass->conformsTo.find(proto);
        assert(found != equivClass->conformsTo.end());

        bool foundValidDerivation = false;
        for (const auto &constraint : found->second) {
          if (isValidRequirementDerivationPath(
                  visited, otherReq.getKind(),
                  constraint.source, proto,
                  requirementSignatureSelfProto)) {
            foundValidDerivation = true;
            break;
          }
        }

        if (!foundValidDerivation)
          return None;

        break;
      }

      case RequirementKind::Superclass: {
        auto superclass = getCanonicalTypeInContext(
          otherReq.getRHS().get<Type>(), { });

        for (const auto &constraint : equivClass->superclassConstraints) {
          auto otherSuperclass = getCanonicalTypeInContext(
              constraint.value, { });

          if (superclass->isExactSuperclassOf(otherSuperclass)) {
            bool foundValidDerivation = false;
            if (isValidRequirementDerivationPath(
                    visited, otherReq.getKind(),
                    constraint.source, otherSuperclass,
                    requirementSignatureSelfProto)) {
              foundValidDerivation = true;
              break;
            }

            if (!foundValidDerivation)
              return None;
          }
        }

        break;
      }

      case RequirementKind::Layout: {
        auto layout = otherReq.getRHS().get<LayoutConstraint>();

        for (const auto &constraint : equivClass->layoutConstraints) {
          auto otherLayout = constraint.value;

          if (layout == otherLayout) {
            bool foundValidDerivation = false;
            if (isValidRequirementDerivationPath(
                    visited, otherReq.getKind(),
                    constraint.source, otherLayout,
                    requirementSignatureSelfProto)) {
              foundValidDerivation = true;
              break;
            }

            if (!foundValidDerivation)
              return None;
          }
        }

        break;
      }

      case RequirementKind::SameType:
        llvm_unreachable("Should not see same type requirements here");
      }
    }

    if (auto *depMemType = otherSubjectType->getAs<DependentMemberType>()) {
      // If 'req' is based on some other conformance requirement
      // `T.[P.]A : Q', we want to make sure that we have a
      // non-redundant derivation for 'T : P'.
      auto baseType = depMemType->getBase();
      auto *proto = depMemType->getAssocType()->getProtocol();

      auto *equivClass = resolveEquivalenceClass(baseType,
                                           ArchetypeResolutionKind::AlreadyKnown);
      assert(equivClass &&
             "Explicit requirement names an unknown equivalence class?");

      auto found = equivClass->conformsTo.find(proto);
      assert(found != equivClass->conformsTo.end());

      bool foundValidDerivation = false;
      for (const auto &constraint : found->second) {
        if (isValidRequirementDerivationPath(
                visited, RequirementKind::Conformance,
                constraint.source, proto,
                requirementSignatureSelfProto)) {
          foundValidDerivation = true;
          break;
        }
      }

      if (!foundValidDerivation)
        return None;
    }
  }

  return result.front();
}

template<typename T, typename Filter>
void GenericSignatureBuilder::checkIfRequirementCanBeDerived(
    const ExplicitRequirement &req,
    const std::vector<Constraint<T>> &constraints,
    const ProtocolDecl *requirementSignatureSelfProto,
    Filter filter) {
  assert(!constraints.empty());

  for (const auto &constraint : constraints) {
    if (filter(constraint))
      continue;

    // If this requirement can be derived from a set of
    // non-redundant base requirements, then this requirement
    // is redundant.
    llvm::SmallDenseSet<ExplicitRequirement, 4> visited;
    visited.insert(req);

    if (auto representative = isValidRequirementDerivationPath(
            visited, req.getKind(),
            constraint.source,
            constraint.value,
            requirementSignatureSelfProto)) {
      Impl->RedundantRequirements[req].insert(*representative);
    }
  }
}

static bool involvesNonSelfSubjectTypes(const RequirementSource *source) {
  while (source->kind != RequirementSource::RequirementSignatureSelf) {
    if (source->isProtocolRequirement() &&
        !source->getStoredType()->is<GenericTypeParamType>())
      return true;

    source = source->parent;
  }

  return false;
}

void GenericSignatureBuilder::computeRedundantRequirements(
    const ProtocolDecl *requirementSignatureSelfProto) {
  assert(!Impl->computedRedundantRequirements &&
         "Already computed redundant requirements");
#ifndef NDEBUG
  Impl->computedRedundantRequirements = true;
#endif

  FrontendStatsTracer tracer(Context.Stats,
                             "compute-redundant-requirements");

  // This sort preserves iteration order with the legacy algorithm.
  SmallVector<ExplicitRequirement, 2> requirements(
      Impl->ExplicitRequirements.begin(),
      Impl->ExplicitRequirements.end());
  std::stable_sort(requirements.begin(), requirements.end(),
                   [](const ExplicitRequirement &req1,
                      const ExplicitRequirement &req2) {
                      if (req1.getSource()->isInferredRequirement() &&
                          !req2.getSource()->isInferredRequirement())
                        return true;

                      if (compareDependentTypes(req1.getSubjectType(),
                                                req2.getSubjectType()) > 0)
                        return true;

                      return false;
                   });

  for (const auto &req : requirements) {
    auto subjectType = req.getSubjectType();

    auto *equivClass = resolveEquivalenceClass(subjectType,
                                         ArchetypeResolutionKind::AlreadyKnown);
    assert(equivClass &&
           "Explicit requirement names an unknown equivalence class?");

    Type resolvedConcreteType;
    if (equivClass->concreteType) {
      resolvedConcreteType =
          getCanonicalTypeInContext(equivClass->concreteType,
                                    getGenericParams());
    }

    Type resolvedSuperclass;
    if (equivClass->superclass) {
      resolvedSuperclass =
          getCanonicalTypeInContext(equivClass->superclass,
                                    getGenericParams());
    }

    switch (req.getKind()) {
    case RequirementKind::Conformance: {
      // TODO: conflicts with concrete
      auto *proto = req.getRHS().get<ProtocolDecl *>();

      auto found = equivClass->conformsTo.find(proto);
      assert(found != equivClass->conformsTo.end());

      // If this is a conformance requirement on 'Self', only consider it
      // redundant if it can be derived from other sources involving 'Self'.
      //
      // What this means in practice is that we will never try to build
      // a witness table for an inherited conformance from an associated
      // conformance.
      //
      // This is important since witness tables for inherited conformances
      // are realized eagerly before the witness table for the original
      // conformance, and allowing more complex requirement sources for
      // inherited conformances could introduce cycles at runtime.
      bool isProtocolRefinement =
          (requirementSignatureSelfProto &&
           equivClass->getAnchor(*this, { })->is<GenericTypeParamType>());

      checkIfRequirementCanBeDerived(
          req, found->second,
          requirementSignatureSelfProto,
          [&](const Constraint<ProtocolDecl *> &constraint) {
            if (isProtocolRefinement)
              return involvesNonSelfSubjectTypes(constraint.source);
            return false;
          });

      if (isConcreteConformance(*equivClass, proto, *this))
        Impl->ExplicitConformancesImpliedByConcrete.insert(req);

      break;
    }

    case RequirementKind::Superclass: {
      auto superclass = getCanonicalTypeInContext(
          req.getRHS().get<Type>(), { });

      if (!superclass->isExactSuperclassOf(resolvedSuperclass)) {
        // Case 1. We have a requirement 'T : C', and we've
        // previously resolved the superclass of 'T' to some
        // class 'D' where 'C' is not a superclass of 'D'.
        //
        // This means 'T : C' conflicts with some other
        // requirement that implies 'T : D'.
        Impl->ConflictingRequirements[req] = resolvedSuperclass;

        checkIfRequirementCanBeDerived(
            req, equivClass->superclassConstraints,
            requirementSignatureSelfProto,
            [&](const Constraint<Type> &constraint) {
              auto otherSuperclass = getCanonicalTypeInContext(
                 constraint.value, { });
              // Filter out requirements where the superclass
              // is not equal to the resolved superclass.
              if (!resolvedSuperclass->isEqual(otherSuperclass))
               return true;

              return false;
            });
      } else {
        // Case 2. We have a requirement 'T : C', and we've
        // previously resolved the superclass of 'T' to some
        // class 'D' where 'C' is a superclass of 'D'.
        //
        // This means that 'T : C' is made redundant by any
        // requirement that implies 'T : B', such that 'C' is a
        // superclass of 'B'.
        checkIfRequirementCanBeDerived(
            req, equivClass->superclassConstraints,
            requirementSignatureSelfProto,
            [&](const Constraint<Type> &constraint) {
              auto otherSuperclass = getCanonicalTypeInContext(
                 constraint.value, { });
              // Filter out requirements where the superclass is less
              // specific than the original requirement.
              if (!superclass->isExactSuperclassOf(otherSuperclass))
               return true;

              return false;
            });

        if (resolvedConcreteType) {
          // We have a superclass requirement 'T : C' and a same-type
          // requirement 'T == D'.
          if (resolvedSuperclass->isExactSuperclassOf(resolvedConcreteType)) {
            // 'C' is a superclass of 'D', so 'T : C' is redundant.
            checkIfRequirementCanBeDerived(
                req, equivClass->concreteTypeConstraints,
                requirementSignatureSelfProto,
                [&](const Constraint<Type> &constraint) {
                  auto otherType = getCanonicalTypeInContext(
                      constraint.value, { });
                  if (!resolvedSuperclass->isExactSuperclassOf(otherType))
                    return true;

                  return false;
                });
          } else {
            // 'C' is not a superclass of 'D'; we have a conflict.
            Impl->ConflictingConcreteTypeRequirements.push_back(
                {req, resolvedConcreteType, resolvedSuperclass});
          }
        }
      }

      break;
    }

    case RequirementKind::Layout: {
      // TODO: less-specific constraints
      // TODO: conflicts with other layout constraints
      // TODO: conflicts with concrete and superclass
      auto layout = req.getRHS().get<LayoutConstraint>();

      if (!layout.merge(equivClass->layout)->isKnownLayout()) {
        // Case 1. We have a requirement 'T : L1', and we've
        // previously resolved the layout of 'T' to some
        // layout constraint 'L2', where 'L1' and 'L2' are not
        // mergeable.
        //
        // This means that 'T : L1' conflicts with some other
        // requirement that implies 'T : L2'.
        Impl->ConflictingRequirements[req] = equivClass->layout;

        checkIfRequirementCanBeDerived(
            req, equivClass->layoutConstraints,
            requirementSignatureSelfProto,
            [&](const Constraint<LayoutConstraint> &constraint) {
              auto otherLayout = constraint.value;
              // Filter out requirements where the layout is not
              // equal to the resolved layout.
              if (equivClass->layout != otherLayout)
               return true;

              return false;
            });
      } else if (layout == equivClass->layout) {
        // Case 2. We have a requirement 'T : L1', and we know
        // the most specific resolved layout for 'T' is also 'L1'.
        //
        // This means that 'T : L1' is made redundant by any
        // requirement that implies 'T : L1'.
        checkIfRequirementCanBeDerived(
            req, equivClass->layoutConstraints,
            requirementSignatureSelfProto,
            [&](const Constraint<LayoutConstraint> &constraint) {
              // Filter out requirements where the implied layout is
              // not equal to the resolved layout.
              auto otherLayout = constraint.value;
              if (layout != otherLayout)
                return true;

              return false;
            });

        if (resolvedConcreteType) {
          auto layout = req.getRHS().get<LayoutConstraint>();
          if (typeImpliesLayoutConstraint(resolvedConcreteType,
                                          layout)) {
            Impl->ExplicitConformancesImpliedByConcrete.insert(req);

            checkIfRequirementCanBeDerived(
                req, equivClass->concreteTypeConstraints,
                requirementSignatureSelfProto,
                [&](const Constraint<Type> &constraint) {
                  // Filter out requirements where the concrete type
                  // does not have the resolved layout.
                  auto otherType = getCanonicalTypeInContext(
                      constraint.value, { });
                  if (!typeImpliesLayoutConstraint(otherType, layout))
                    return true;

                  return false;
                });
          } else if (typeConflictsWithLayoutConstraint(resolvedConcreteType,
                                                       layout)) {
            // We have a concrete type requirement 'T == C' and 'C' does
            // not satisfy 'L1'.
            Impl->ConflictingConcreteTypeRequirements.push_back(
                {req, resolvedConcreteType, equivClass->layout});
          }
        }
      } else {
        // Case 3. We have a requirement 'T : L1', and we know
        // the most specific resolved layout for 'T' is some other
        // layout 'L2', such that 'L2' is mergeable with 'L1'.
        //
        // This means that 'T : L1' is made redundant by any
        // requirement that implies 'T : L3', where L3 is
        // mergeable with L1.
        checkIfRequirementCanBeDerived(
            req, equivClass->layoutConstraints,
            requirementSignatureSelfProto,
            [&](const Constraint<LayoutConstraint> &constraint) {
              auto otherLayout = constraint.value;
              // Filter out requirements where the implied layout
              // is not mergeable with the original requirement's
              // layout.
              if (!layout.merge(otherLayout)->isKnownLayout())
                return true;

              return false;
            });

        if (resolvedConcreteType) {
          auto layout = req.getRHS().get<LayoutConstraint>();
          if (typeImpliesLayoutConstraint(resolvedConcreteType, layout)) {
            Impl->ExplicitConformancesImpliedByConcrete.insert(req);

            checkIfRequirementCanBeDerived(
                req, equivClass->concreteTypeConstraints,
                requirementSignatureSelfProto,
                [&](const Constraint<Type> &constraint) {
                  // Filter out requirements where the concrete type
                  // does not have the resolved layout.
                  auto otherType = getCanonicalTypeInContext(
                      constraint.value, { });

                  if (!typeImpliesLayoutConstraint(otherType,
                                                   equivClass->layout))
                    return true;

                  return false;
                });
          }
        }
      }

      break;
    }

    case RequirementKind::SameType:
      llvm_unreachable("Should not see same type requirements here");
    }
  }
}

void
GenericSignatureBuilder::finalize(TypeArrayView<GenericTypeParamType> genericParams,
                                  bool allowConcreteGenericParams,
                                  const ProtocolDecl *requirementSignatureSelfProto) {
  diagnoseProtocolRefinement(requirementSignatureSelfProto);
  diagnoseRedundantRequirements();
  diagnoseConflictingConcreteTypeRequirements(requirementSignatureSelfProto);

  {
    // In various places below, we iterate over the list of equivalence classes
    // and call getMinimalConformanceSource(). Unfortunately, this function
    // ends up calling maybeResolveEquivalenceClass(), which can delete equivalence
    // classes. The workaround is to first iterate safely over a copy of the list,
    // and pre-compute all minimal conformance sources, before proceeding with the
    // rest of the function.
    //
    // FIXME: This is not even correct, because we may not have reached fixed point
    // after one round of this. getMinimalConformanceSource() should be removed
    // instead.
    SmallVector<PotentialArchetype *, 8> equivalenceClassPAs;
    for (auto &equivClass : Impl->EquivalenceClasses) {
      equivalenceClassPAs.push_back(equivClass.members.front());
    }

    for (auto *pa : equivalenceClassPAs) {
      auto &equivClass = *pa->getOrCreateEquivalenceClass(*this);

      // Copy the vector and iterate over the copy to avoid iterator invalidation
      // issues.
      auto conformsTo = equivClass.conformsTo;
      for (auto entry : conformsTo) {
        for (const auto &constraint : entry.second) {
          (void) constraint.source->getMinimalConformanceSource(
              *this, constraint.getSubjectDependentType({ }), entry.first);
        }
      }
    }
  }

  assert(!Impl->finalized && "Already finalized builder");
#ifndef NDEBUG
  Impl->finalized = true;
#endif

  // Local function (+ cache) describing the set of equivalence classes
  // directly referenced by the concrete same-type constraint of the given
  // equivalence class.
  llvm::DenseMap<EquivalenceClass *,
                 SmallPtrSet<EquivalenceClass *, 4>> concreteEquivClasses;
  auto getConcreteReferencedEquivClasses
      = [&](EquivalenceClass *equivClass)
          -> SmallPtrSet<EquivalenceClass *, 4> {
    auto known = concreteEquivClasses.find(equivClass);
    if (known != concreteEquivClasses.end())
      return known->second;

    SmallPtrSet<EquivalenceClass *, 4> referencedEquivClasses;
    if (!equivClass->concreteType ||
        !equivClass->concreteType->hasTypeParameter())
      return referencedEquivClasses;

    equivClass->concreteType.visit([&](Type type) {
      if (type->isTypeParameter()) {
        if (auto referencedEquivClass =
              resolveEquivalenceClass(type,
                                      ArchetypeResolutionKind::AlreadyKnown)) {
          referencedEquivClasses.insert(referencedEquivClass);
        }
      }
    });

    concreteEquivClasses[equivClass] = referencedEquivClasses;
    return referencedEquivClasses;
  };

  /// Check whether the given type references the archetype.
  auto isRecursiveConcreteType = [&](EquivalenceClass *equivClass,
                                     bool isSuperclass) {
    SmallPtrSet<EquivalenceClass *, 4> visited;
    SmallVector<EquivalenceClass *, 4> stack;
    stack.push_back(equivClass);
    visited.insert(equivClass);

    // Check whether the specific type introduces recursion.
    auto checkTypeRecursion = [&](Type type) {
      if (!type->hasTypeParameter()) return false;

      return type.findIf([&](Type type) {
        if (type->isTypeParameter()) {
          if (auto referencedEquivClass =
                resolveEquivalenceClass(
                                    type,
                                    ArchetypeResolutionKind::AlreadyKnown)) {
            if (referencedEquivClass == equivClass) return true;

            if (visited.insert(referencedEquivClass).second)
              stack.push_back(referencedEquivClass);
          }
        }

        return false;
      });
    };

    while (!stack.empty()) {
      auto currentEquivClass = stack.back();
      stack.pop_back();

      // If we're checking superclasses, do so now.
      if (isSuperclass && currentEquivClass->superclass &&
          checkTypeRecursion(currentEquivClass->superclass)) {
        return true;
      }

      // Otherwise, look for the equivalence classes referenced by
      // same-type constraints.
      for (auto referencedEquivClass :
             getConcreteReferencedEquivClasses(currentEquivClass)) {
        // If we found a reference to the original archetype, it's recursive.
        if (referencedEquivClass == equivClass) return true;

        if (visited.insert(referencedEquivClass).second)
          stack.push_back(referencedEquivClass);
      }
    }

    return false;
  };

  // Check for recursive or conflicting same-type bindings and superclass
  // constraints.
  for (auto &equivClass : Impl->EquivalenceClasses) {
    if (equivClass.concreteType) {
      // Check for recursive same-type bindings.
      if (isRecursiveConcreteType(&equivClass, /*isSuperclass=*/false)) {
        if (auto constraint =
              equivClass.findAnyConcreteConstraintAsWritten()) {
          Impl->HadAnyError = true;

          Diags.diagnose(constraint->source->getLoc(),
                         diag::recursive_same_type_constraint,
                         constraint->getSubjectDependentType(genericParams),
                         constraint->value);
        }

        equivClass.recursiveConcreteType = true;
      } else {
        checkConcreteTypeConstraints(genericParams, &equivClass);
      }
    }

    // Check for recursive superclass bindings.
    if (equivClass.superclass) {
      if (isRecursiveConcreteType(&equivClass, /*isSuperclass=*/true)) {
        if (auto source = equivClass.findAnySuperclassConstraintAsWritten()) {
          Impl->HadAnyError = true;

          Diags.diagnose(source->source->getLoc(),
                         diag::recursive_superclass_constraint,
                         source->getSubjectDependentType(genericParams),
                         equivClass.superclass);
        }

        equivClass.recursiveSuperclassType = true;
      }
    }
  }

  if (!Impl->ExplicitSameTypeRequirements.empty()) {
    // FIXME: Expand all conformance requirements. This is expensive :(
    for (auto &equivClass : Impl->EquivalenceClasses) {
      expandSameTypeConstraints(*this, &equivClass);
    }

    // Check same-type constraints.
    for (auto &equivClass : Impl->EquivalenceClasses) {
      checkSameTypeConstraints(genericParams, &equivClass);
    }
  }

  // Check for generic parameters which have been made concrete or equated
  // with each other.
  if (!allowConcreteGenericParams) {
    SmallPtrSet<PotentialArchetype *, 4> visited;
    
    unsigned depth = 0;
    for (const auto gp : getGenericParams())
      depth = std::max(depth, gp->getDepth());

    for (const auto &pa : Impl->PotentialArchetypes) {
      auto rep = pa->getRepresentative();

      if (pa->getDependentType()->getRootGenericParam()->getDepth() < depth)
        continue;

      if (!visited.insert(rep).second)
        continue;

      // Don't allow a generic parameter to be equivalent to a concrete type,
      // because then we don't actually have a parameter.
      auto equivClass = rep->getOrCreateEquivalenceClass(*this);
      if (equivClass->concreteType &&
          !equivClass->concreteType->is<ErrorType>()) {
        if (auto constraint = equivClass->findAnyConcreteConstraintAsWritten()){
          Impl->HadAnyError = true;

          Diags.diagnose(constraint->source->getLoc(),
                         diag::requires_generic_param_made_equal_to_concrete,
                         rep->getDependentType(genericParams));
        }
        continue;
      }

      // Don't allow two generic parameters to be equivalent, because then we
      // don't actually have two parameters.
      for (auto other : rep->getEquivalenceClassMembers()) {
        // If it isn't a generic parameter, skip it.
        if (other == pa || !other->isGenericParam()) continue;

        // Try to find an exact constraint that matches 'other'.
        auto repConstraint =
          findRepresentativeConstraint<Type>(
            *this,
            equivClass->sameTypeConstraints,
            RequirementKind::SameType,
            [pa, other](const Constraint<Type> &constraint) {
              return (constraint.isSubjectEqualTo(pa) &&
                      constraint.value->isEqual(other->getDependentType())) ||
                (constraint.isSubjectEqualTo(other) &&
                 constraint.value->isEqual(pa->getDependentType()));
            });


         // Otherwise, just take any old constraint.
        if (!repConstraint) {
          repConstraint =
            findRepresentativeConstraint<Type>(
              *this,
              equivClass->sameTypeConstraints,
              RequirementKind::SameType,
              [](const Constraint<Type> &constraint) {
                return true;
              });
        }

        if (repConstraint && repConstraint->source->getLoc().isValid()) {
          Impl->HadAnyError = true;

          Diags.diagnose(repConstraint->source->getLoc(),
                         diag::requires_generic_params_made_equal,
                         pa->getDependentType(genericParams),
                         other->getDependentType(genericParams));
        }
        break;
      }
    }
  }
  
  // Emit a diagnostic if we recorded any constraints where the constraint
  // type was not constrained to a protocol or class. Provide a fix-it if
  // allowConcreteGenericParams is true or the subject type is a member type.
  if (!invalidIsaConstraints.empty()) {
    for (auto constraint : invalidIsaConstraints) {
      auto subjectType = constraint.getSubjectDependentType(getGenericParams());
      auto constraintType = constraint.value;
      auto source = constraint.source;
      auto loc = source->getLoc();

      Diags.diagnose(loc, diag::requires_conformance_nonprotocol,
                     subjectType, constraintType);
      
      auto getNameWithoutSelf = [&](std::string subjectTypeName) {
        std::string selfSubstring = "Self.";
        
        if (subjectTypeName.rfind(selfSubstring, 0) == 0) {
          return subjectTypeName.erase(0, selfSubstring.length());
        }
        
        return subjectTypeName;
      };

      if (allowConcreteGenericParams ||
          (subjectType->is<DependentMemberType>() &&
           !source->isProtocolRequirement())) {
        auto subjectTypeName = subjectType.getString();
        auto subjectTypeNameWithoutSelf = getNameWithoutSelf(subjectTypeName);
        Diags.diagnose(loc, diag::requires_conformance_nonprotocol_fixit,
                       subjectTypeNameWithoutSelf, constraintType.getString())
             .fixItReplace(loc, " == ");
      }
    }

    invalidIsaConstraints.clear();
  }
}

/// Turn a requirement right-hand side into an unresolved type.
static GenericSignatureBuilder::UnresolvedType asUnresolvedType(
                        GenericSignatureBuilder::UnresolvedRequirementRHS rhs) {
  if (auto pa = rhs.dyn_cast<PotentialArchetype *>())
    return GenericSignatureBuilder::UnresolvedType(pa);

  return GenericSignatureBuilder::UnresolvedType(rhs.get<Type>());
}

void GenericSignatureBuilder::processDelayedRequirements() {
  // If we're already up-to-date, do nothing.
  if (Impl->Generation == Impl->LastProcessedGeneration) { return; }

  // If there are no delayed requirements, do nothing.
  if (Impl->DelayedRequirements.empty()) { return; }

  if (Impl->ProcessingDelayedRequirements) { return; }

  ++NumProcessDelayedRequirements;

  llvm::SaveAndRestore<bool> processing(Impl->ProcessingDelayedRequirements,
                                        true);
  bool anyChanges = false;
  SWIFT_DEFER {
    Impl->LastProcessedGeneration = Impl->Generation;
    if (!anyChanges)
      ++NumProcessDelayedRequirementsUnchanged;
  };

  bool anySolved;
  do {
    // Steal the delayed requirements so we can reprocess them.
    anySolved = false;
    auto delayed = std::move(Impl->DelayedRequirements);
    Impl->DelayedRequirements.clear();

    // Process delayed requirements.
    for (const auto &req : delayed) {
      // Reprocess the delayed requirement.
      ConstraintResult reqResult;
      switch (req.kind) {
      case DelayedRequirement::Type:
        reqResult =
            addTypeRequirement(req.lhs, asUnresolvedType(req.rhs), req.source,
                               UnresolvedHandlingKind::GenerateUnresolved,
                               /*inferForModule=*/nullptr);
        break;

      case DelayedRequirement::Layout:
        reqResult = addLayoutRequirement(
                           req.lhs, req.rhs.get<LayoutConstraint>(), req.source,
                           UnresolvedHandlingKind::GenerateUnresolved);
        break;

      case DelayedRequirement::SameType:
        reqResult = addSameTypeRequirement(
                               req.lhs, asUnresolvedType(req.rhs), req.source,
                               UnresolvedHandlingKind::GenerateUnresolved);
        break;
      }

      // Update our state based on what happened.
      switch (reqResult) {
      case ConstraintResult::Concrete:
        ++NumDelayedRequirementConcrete;
        anySolved = true;
        break;

      case ConstraintResult::Conflicting:
        anySolved = true;
        break;

      case ConstraintResult::Resolved:
        ++NumDelayedRequirementResolved;
        anySolved = true;
        break;

      case ConstraintResult::Unresolved:
        // Add the requirement back.
        ++NumDelayedRequirementUnresolved;
        break;
      }
    }

    if (anySolved) {
      anyChanges = true;
    }
  } while (anySolved);
}

namespace {
  /// Remove self-derived sources from the given vector of constraints.
  template<typename T>
  void removeSelfDerived(GenericSignatureBuilder &builder,
                         std::vector<Constraint<T>> &constraints,
                         ProtocolDecl *proto,
                         bool allCanBeSelfDerived = false) {
    auto genericParams = builder.getGenericParams();
    SmallVector<Constraint<T>, 4> minimalSources;
    constraints.erase(
      std::remove_if(constraints.begin(), constraints.end(),
        [&](const Constraint<T> &constraint) {
          auto minimalSource =
            constraint.source->getMinimalConformanceSource(
                         builder,
                         constraint.getSubjectDependentType(genericParams),
                         proto);
          if (minimalSource != constraint.source) {
            // The minimal source is smaller than the original source, so the
            // original source is self-derived.
            ++NumSelfDerived;

            if (minimalSource) {
              // Record a constraint with a minimized source.
              minimalSources.push_back(
                           {constraint.subject,
                             constraint.value,
                             minimalSource});
            }

            return true;
          }

          return false;
        }),
      constraints.end());

    // If we found any minimal sources, add them now, avoiding introducing any
    // redundant sources.
    if (!minimalSources.empty()) {
      // Collect the sources we already know about.
      SmallPtrSet<const RequirementSource *, 4> sources;
      for (const auto &constraint : constraints) {
        sources.insert(constraint.source);
      }

      // Add any minimal sources we didn't know about.
      for (const auto &minimalSource : minimalSources) {
        if (sources.insert(minimalSource.source).second) {
          constraints.push_back(minimalSource);
        }
      }
    }

    assert((!constraints.empty() || allCanBeSelfDerived) &&
           "All constraints were self-derived!");
  }
} // end anonymous namespace

template<typename T>
Constraint<T> GenericSignatureBuilder::checkConstraintList(
                           TypeArrayView<GenericTypeParamType> genericParams,
                           std::vector<Constraint<T>> &constraints,
                           RequirementKind kind,
                           llvm::function_ref<bool(const Constraint<T> &)>
                             isSuitableRepresentative,
                           llvm::function_ref<
                             ConstraintRelation(const Constraint<T>&)>
                               checkConstraint,
                           Optional<Diag<unsigned, Type, T, T>>
                             conflictingDiag,
                           Diag<Type, T> redundancyDiag,
                           Diag<unsigned, Type, T> otherNoteDiag) {
  assert(!constraints.empty() && "No constraints?");

  // Sort the constraints, so we get a deterministic ordering of diagnostics.
  std::sort(constraints.begin(), constraints.end());

  // Find a representative constraint.
  auto representativeConstraint =
    findRepresentativeConstraint<T>(*this, constraints, kind,
                                    isSuitableRepresentative);

  // Local function to provide a note describing the representative constraint.
  auto noteRepresentativeConstraint = [&] {
    if (representativeConstraint->source->getLoc().isInvalid()) return;

    Diags.diagnose(representativeConstraint->source->getLoc(),
                   otherNoteDiag,
                   representativeConstraint->source->classifyDiagKind(),
                   representativeConstraint->getSubjectDependentType(
                                                               genericParams),
                   representativeConstraint->value);
  };

  // Go through the concrete constraints looking for redundancies.
  bool diagnosedConflictingRepresentative = false;
  for (const auto &constraint : constraints) {
    // Leave the representative alone.
    if (representativeConstraint && constraint == *representativeConstraint)
      continue;

    switch (checkConstraint(constraint)) {
    case ConstraintRelation::Unrelated:
      continue;

    case ConstraintRelation::Conflicting: {
      // Figure out what kind of subject we have; it will affect the
      // diagnostic.
      auto getSubjectType =
        [&](Type subjectType) -> std::pair<unsigned, Type> {
          unsigned kind;
          if (auto gp = subjectType->getAs<GenericTypeParamType>()) {
            if (gp->getDecl() &&
                isa<ProtocolDecl>(gp->getDecl()->getDeclContext())) {
              kind = 1;
              subjectType = cast<ProtocolDecl>(gp->getDecl()->getDeclContext())
                              ->getDeclaredInterfaceType();
            } else {
              kind = 0;
            }
          } else {
            kind = 2;
          }

          return std::make_pair(kind, subjectType);
        };


      // The requirement conflicts. If this constraint has a location, complain
      // about it.
      if (constraint.source->getLoc().isValid()) {
        Impl->HadAnyError = true;

        auto subject =
          getSubjectType(constraint.getSubjectDependentType(genericParams));
        Diags.diagnose(constraint.source->getLoc(), *conflictingDiag,
                       subject.first, subject.second,
                       constraint.value,
                       representativeConstraint->value);

        noteRepresentativeConstraint();
        break;
      }

      // If the representative itself conflicts and we haven't diagnosed it yet,
      // do so now.
      if (!diagnosedConflictingRepresentative &&
          representativeConstraint->source->getLoc().isValid()) {
        Impl->HadAnyError = true;

        auto subject =
          getSubjectType(
            representativeConstraint->getSubjectDependentType(genericParams));
        Diags.diagnose(representativeConstraint->source->getLoc(),
                       *conflictingDiag,
                       subject.first, subject.second,
                       representativeConstraint->value,
                       constraint.value);

        diagnosedConflictingRepresentative = true;
        break;
      }
      break;
    }

    case ConstraintRelation::Redundant:
      // If this requirement is not derived or inferred (but has a useful
      // location) complain that it is redundant.
      if (constraint.source->shouldDiagnoseRedundancy(true) &&
          representativeConstraint &&
          representativeConstraint->source->shouldDiagnoseRedundancy(false)) {
        Diags.diagnose(constraint.source->getLoc(),
                       redundancyDiag,
                       constraint.getSubjectDependentType(genericParams),
                       constraint.value);

        noteRepresentativeConstraint();
      }
      break;
    }
  }

  return *representativeConstraint;
}

/// Determine whether this is a redundantly inheritable Objective-C protocol.
///
/// A redundantly-inheritable Objective-C protocol is one where we will
/// silently accept a directly-stated redundant conformance to this protocol,
/// and emit this protocol in the list of "inherited" protocols. There are
/// two cases where we allow this:
///
//    1) For a protocol defined in Objective-C, so that we will match Clang's
///      behavior, and
///   2) For an @objc protocol defined in Swift that directly inherits from
///      JavaScriptCore's JSExport, which depends on this behavior.
static bool isRedundantlyInheritableObjCProtocol(
                                             ProtocolDecl *proto,
                                             const RequirementSource *source) {
  if (!proto->isObjC()) return false;

  // Only do this for the requirement signature computation.
  auto parentSource = source->parent;
  if (!parentSource ||
      parentSource->kind != RequirementSource::RequirementSignatureSelf)
    return false;

  // Check the two conditions in which we will suppress the diagnostic and
  // emit the redundant inheritance.
  auto inheritingProto = parentSource->getProtocolDecl();
  if (!inheritingProto->hasClangNode() && !proto->getName().is("JSExport"))
    return false;

  // If the inheriting protocol already has @_restatedObjCConformance with
  // this protocol, we're done.
  for (auto *attr : inheritingProto->getAttrs()
                      .getAttributes<RestatedObjCConformanceAttr>()) {
    if (attr->Proto == proto) return true;
  }

  // Otherwise, add @_restatedObjCConformance.
  auto &ctx = proto->getASTContext();
  inheritingProto->getAttrs().add(new (ctx) RestatedObjCConformanceAttr(proto));
  return true;
}

/// Diagnose any conformance requirements on 'Self' that are not derived from
/// the protocol's inheritance clause.
void GenericSignatureBuilder::diagnoseProtocolRefinement(
    const ProtocolDecl *requirementSignatureSelfProto) {
  if (requirementSignatureSelfProto == nullptr)
    return;

  auto selfType = requirementSignatureSelfProto->getSelfInterfaceType();
  auto *equivClass = resolveEquivalenceClass(
      selfType, ArchetypeResolutionKind::AlreadyKnown);
  assert(equivClass != nullptr);

  for (auto pair : equivClass->conformsTo) {
    auto *proto = pair.first;
    bool found = false;
    SourceLoc loc;
    for (const auto &constraint : pair.second) {
      if (!involvesNonSelfSubjectTypes(constraint.source)) {
        found = true;
        break;
      }

      auto *parent = constraint.source;
      while (parent->kind != RequirementSource::RequirementSignatureSelf) {
        loc = parent->getLoc();
        if (loc)
          break;
      }
    }

    if (!found) {
      requirementSignatureSelfProto->diagnose(
          diag::missing_protocol_refinement,
          const_cast<ProtocolDecl *>(requirementSignatureSelfProto),
          proto);

      if (loc.isValid()) {
        Context.Diags.diagnose(loc, diag::redundant_conformance_here,
                               selfType, proto);
      }
    }
  }
}

void GenericSignatureBuilder::diagnoseRedundantRequirements(
    bool onlyDiagnoseExplicitConformancesImpliedByConcrete) const {
  for (const auto &req : Impl->ExplicitRequirements) {
    auto *source = req.getSource();
    auto loc = source->getLoc();

    // Don't diagnose anything without a source location.
    if (loc.isInvalid())
      continue;

    // Don't diagnose inferred requirements.
    if (source->isInferredRequirement())
      continue;

    // Check if its actually redundant.
    auto found = Impl->RedundantRequirements.find(req);
    if (found == Impl->RedundantRequirements.end())
      continue;

    if (onlyDiagnoseExplicitConformancesImpliedByConcrete &&
        Impl->ExplicitConformancesImpliedByConcrete.count(req) == 0)
      continue;

    // Don't diagnose explicit requirements that are implied by
    // inferred requirements.
    if (llvm::all_of(found->second,
                     [&](const ExplicitRequirement &otherReq) {
                       return otherReq.getSource()->isInferredRequirement();
                     }))
      continue;

    auto subjectType = getSugaredDependentType(req.getSubjectType(),
                                               getGenericParams());

    switch (req.getKind()) {
    case RequirementKind::Conformance: {
      auto *proto = req.getRHS().get<ProtocolDecl *>();

      // If this conformance requirement recursively makes a protocol
      // conform to itself, don't complain here, because we diagnose
      // the circular inheritance elsewhere.
      {
        auto rootSource = source->getRoot();
        if (proto == rootSource->getProtocolDecl() &&
            rootSource->kind == RequirementSource::RequirementSignatureSelf &&
            rootSource->getRootType()->isEqual(source->getAffectedType())) {
          break;
        }
      }

      // If this is a redundantly inherited Objective-C protocol, treat it
      // as "unrelated" to silence the warning about the redundant
      // conformance.
      if (isRedundantlyInheritableObjCProtocol(proto, source))
        break;

      Context.Diags.diagnose(loc, diag::redundant_conformance_constraint,
                             subjectType, proto);

      for (auto otherReq : found->second) {
        auto *otherSource = otherReq.getSource();
        if (otherSource->isInferredRequirement())
          continue;

        auto otherLoc = otherSource->getLoc();
        if (otherLoc.isInvalid())
          continue;

        Context.Diags.diagnose(otherLoc, diag::redundant_conformance_here,
                               subjectType, proto);
      }

      break;
    }

    case RequirementKind::Layout: {
      auto layout = req.getRHS().get<LayoutConstraint>();

      auto conflict = Impl->ConflictingRequirements.find(req);
      if (conflict != Impl->ConflictingRequirements.end()) {
        Impl->HadAnyError = true;

        auto otherLayout = conflict->second.get<LayoutConstraint>();
        Context.Diags.diagnose(loc, diag::conflicting_layout_constraints,
                               subjectType, layout, otherLayout);

        for (auto otherReq : found->second) {
          auto *otherSource = otherReq.getSource();
          auto otherLoc = otherSource->getLoc();
          if (otherLoc.isInvalid())
            continue;

          Context.Diags.diagnose(otherLoc, diag::conflicting_layout_constraint,
                                 subjectType, otherLayout);
        }
      } else {
        Context.Diags.diagnose(loc, diag::redundant_layout_constraint,
                               subjectType, layout);

        for (auto otherReq : found->second) {
          auto *otherSource = otherReq.getSource();
          if (otherSource->isInferredRequirement())
            continue;

          auto otherLoc = otherSource->getLoc();
          if (otherLoc.isInvalid())
            continue;

          Context.Diags.diagnose(otherLoc, diag::previous_layout_constraint,
                                 subjectType, layout);
        }
      }

      break;
    }

    case RequirementKind::Superclass: {
      auto superclass = req.getRHS().get<Type>();

      auto conflict = Impl->ConflictingRequirements.find(req);
      if (conflict != Impl->ConflictingRequirements.end()) {
        Impl->HadAnyError = true;

        auto otherSuperclass = conflict->second.get<Type>();

        Context.Diags.diagnose(loc, diag::conflicting_superclass_constraints,
                               subjectType, superclass, otherSuperclass);

        for (auto otherReq : found->second) {
          auto *otherSource = otherReq.getSource();
          auto otherLoc = otherSource->getLoc();
          if (otherLoc.isInvalid())
            continue;

          Context.Diags.diagnose(otherLoc, diag::conflicting_superclass_constraint,
                                 subjectType, otherSuperclass);
        }
      } else {
        Context.Diags.diagnose(loc, diag::redundant_superclass_constraint,
                               subjectType, superclass);

        for (auto otherReq : found->second) {
          auto *otherSource = otherReq.getSource();
          if (otherSource->isInferredRequirement())
            continue;

          auto otherLoc = otherSource->getLoc();
          if (otherLoc.isInvalid())
            continue;

          Context.Diags.diagnose(otherLoc, diag::superclass_redundancy_here,
                                 subjectType, superclass);
        }
      }

      break;
    }

    case RequirementKind::SameType:
      // TODO
      break;
    }
  }
}

void GenericSignatureBuilder::diagnoseConflictingConcreteTypeRequirements(
    const ProtocolDecl *requirementSignatureSelfProto) {
  for (auto pair : Impl->ConflictingConcreteTypeRequirements) {
    auto subjectType = pair.otherRequirement.getSubjectType();

    auto *equivClass = resolveEquivalenceClass(subjectType,
                                         ArchetypeResolutionKind::AlreadyKnown);
    assert(equivClass &&
           "Explicit requirement names an unknown equivalence class?");

    auto foundConcreteRequirement = llvm::find_if(
        equivClass->concreteTypeConstraints,
        [&](const Constraint<Type> &constraint) {
          SmallVector<ExplicitRequirement, 2> result;
          getBaseRequirements(
              [&]() {
                return std::make_pair(RequirementKind::SameType,
                                      constraint.value);
              },
              constraint.source, requirementSignatureSelfProto, result);

        for (const auto &otherReq : result) {
          if (isRedundantExplicitRequirement(otherReq))
            return false;
        }

        return true;
      });

    assert(foundConcreteRequirement != equivClass->concreteTypeConstraints.end());

    SourceLoc loc = foundConcreteRequirement->source->getLoc();
    SourceLoc otherLoc = pair.otherRequirement.getSource()->getLoc();

    if (loc.isInvalid() && otherLoc.isInvalid())
      continue;

    SourceLoc subjectLoc = (loc.isInvalid() ? otherLoc : loc);

    Impl->HadAnyError = true;

    switch (pair.otherRequirement.getKind()) {
    case RequirementKind::Superclass: {
      Context.Diags.diagnose(subjectLoc, diag::type_does_not_inherit,
                             subjectType, pair.resolvedConcreteType,
                             pair.otherRHS.get<Type>());

      if (otherLoc.isValid()) {
        Context.Diags.diagnose(otherLoc, diag::superclass_redundancy_here,
                               subjectType, pair.otherRHS.get<Type>());
      }

      break;
    }

    case RequirementKind::Layout: {
      Context.Diags.diagnose(subjectLoc, diag::type_is_not_a_class,
                             subjectType, pair.resolvedConcreteType, Type());

      if (otherLoc.isValid()) {
        Context.Diags.diagnose(otherLoc, diag::previous_layout_constraint,
                               subjectType, pair.otherRHS.get<LayoutConstraint>());
      }

      break;
    }

    case RequirementKind::Conformance:
    case RequirementKind::SameType:
      llvm_unreachable("TODO");
    }

    if (loc.isValid()) {
      Context.Diags.diagnose(loc, diag::same_type_redundancy_here,
                             1, subjectType, pair.resolvedConcreteType);
    }
  }
}

namespace swift {
  bool operator<(const DerivedSameTypeComponent &lhs,
                 const DerivedSameTypeComponent &rhs) {
    return compareDependentTypes(lhs.type, rhs.type) < 0;
  }
} // namespace swift

/// Find the representative in a simple union-find data structure of
/// integral values.
static unsigned findRepresentative(SmallVectorImpl<unsigned> &parents,
                                   unsigned index) {
  if (parents[index] == index) return index;

  return parents[index] = findRepresentative(parents, parents[index]);
}

/// Union the same-type components denoted by \c index1 and \c index2.
///
/// \param successThreshold Returns true when two sets have been joined
/// and both representatives are below the threshold. The default of 0
/// is equivalent to \c successThreshold == parents.size().
///
/// \returns \c true if the two components were separate and have now
/// been joined; \c false if they were already in the same set.
static bool unionSets(SmallVectorImpl<unsigned> &parents,
                      unsigned index1, unsigned index2,
                      unsigned successThreshold = 0) {
  // Find the representatives of each component class.
  unsigned rep1 = findRepresentative(parents, index1);
  unsigned rep2 = findRepresentative(parents, index2);
  if (rep1 == rep2) return false;

  // Point at the lowest-numbered representative.
  if (rep1 < rep2)
    parents[rep2] = rep1;
  else
    parents[rep1] = rep2;

  return (successThreshold == 0) ||
    (rep1 < successThreshold && rep2 < successThreshold);
}

/// Computes the ordered set of archetype anchors required to form a minimum
/// spanning tree among the connected components formed by only the derived
/// same-type requirements within the equivalence class \c equivClass.
///
/// The equivalence class contains all potential archetypes that are made
/// equivalent by the known set of same-type constraints, which includes both
/// directly-stated same-type constraints (e.g., \c T.A == T.B) as well as
/// same-type constraints that are implied either because the names coincide
/// (e.g., \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
///
/// The equivalence class of the given representative potential archetype
/// (\c rep) is formed from a graph whose vertices are the potential archetypes
/// and whose edges are the same-type constraints. These edges include both
/// directly-stated same-type constraints (e.g., \c T.A == T.B) as well as
/// same-type constraints that are implied either because the names coincide
/// (e.g., \c T[.P1].A == T[.P2].A) or due to a requirement in a protocol.
/// The equivalence class forms a single connected component.
///
/// Within that graph is a subgraph that includes only those edges that are
/// implied (and, therefore, excluding those edges that were explicitly stated).
/// The connected components within that subgraph describe the potential
/// archetypes that would be equivalence even with all of the (explicit)
/// same-type constraints removed.
///
/// The entire equivalence class can be restored by introducing edges between
/// the connected components. This function computes a minimal, canonicalized
/// set of edges (same-type constraints) needed to describe the equivalence
/// class, which is suitable for the generation of the canonical generic
/// signature.
///
/// The resulting set of "edges" is returned as a set of vertices, one per
/// connected component (of the subgraph). Each is the anchor for that
/// connected component (as determined by \c compareDependentTypes()), and the
/// set itself is ordered by \c compareDependentTypes(). The actual set of
/// canonical edges connects vertex i to vertex i+1 for i in 0..<size-1.
static void computeDerivedSameTypeComponents(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf){
  // Set up the array of "parents" in the union-find data structure.
  llvm::SmallDenseMap<CanType, unsigned> parentIndices;
  SmallVector<unsigned, 4> parents;
  for (unsigned i : indices(equivClass->members)) {
    Type depType = equivClass->members[i]->getDependentType();
    parentIndices[depType->getCanonicalType()] = parents.size();
    parents.push_back(i);
  }

  // Walk all of the same-type constraints, performing a union-find operation.
  for (const auto &constraint : equivClass->sameTypeConstraints) {
    // Treat nested-type-name-match constraints specially.
    if (constraint.source->getRoot()->kind ==
          RequirementSource::NestedTypeNameMatch)
      continue;

    // Skip non-derived constraints.
    if (!constraint.source->isDerivedRequirement()) continue;

    CanType source =
      constraint.getSubjectDependentType({ })->getCanonicalType();
    CanType target = constraint.value->getCanonicalType();

    assert(parentIndices.count(source) == 1 && "Missing source");
    assert(parentIndices.count(target) == 1 && "Missing target");
    unionSets(parents, parentIndices[source], parentIndices[target]);
  }

  // Compute and record the components.
  auto &components = equivClass->derivedSameTypeComponents;
  for (unsigned i : indices(equivClass->members)) {
    auto pa = equivClass->members[i];
    auto depType = pa->getDependentType();

    // Find the representative of this set.
    assert(parentIndices.count(depType) == 1 && "Unknown member?");
    unsigned index = parentIndices[depType];
    unsigned representative = findRepresentative(parents, index);

    // If this is the representative, add a component for it.
    if (representative == index) {
      componentOf[depType] = components.size();
      components.push_back(DerivedSameTypeComponent{depType, nullptr});
      continue;
    }

    // This is not the representative; point at the component of the
    // representative.
    CanType representativeDepTy =
      equivClass->members[representative]->getDependentType();
    assert(componentOf.count(representativeDepTy) == 1 &&
           "Missing representative component?");
    unsigned componentIndex = componentOf[representativeDepTy];
    componentOf[depType] = componentIndex;

    // If this is a better anchor, record it.
    if (compareDependentTypes(depType, components[componentIndex].type) < 0) {
      components[componentIndex].type = depType;
    }
  }

  // If there is a concrete type, figure out the best concrete type anchor
  // per component.
  auto genericParams = builder.getGenericParams();
  for (const auto &concrete : equivClass->concreteTypeConstraints) {
    // Dig out the component associated with constraint.
    Type subjectType = concrete.getSubjectDependentType(genericParams);
    assert(componentOf.count(subjectType->getCanonicalType()) > 0);
    auto &component = components[componentOf[subjectType->getCanonicalType()]];

    // FIXME: Skip self-derived sources. This means our attempts to "stage"
    // construction of self-derived sources really don't work, because we
    // discover more information later, so we need a more on-line or
    // iterative approach.
    if (concrete.source->isSelfDerivedSource(builder, subjectType))
      continue;

    // If it has a better source than we'd seen before for this component,
    // keep it.
    auto &bestConcreteTypeSource = component.concreteTypeSource;
    if (!bestConcreteTypeSource ||
        concrete.source->compare(bestConcreteTypeSource) < 0)
      bestConcreteTypeSource = concrete.source;
  }
}

namespace {
  /// An edge in the same-type constraint graph that spans two different
  /// components.
  struct IntercomponentEdge {
    unsigned source;
    unsigned target;
    Constraint<Type> constraint;
    bool isSelfDerived = false;

    IntercomponentEdge(unsigned source, unsigned target,
                       const Constraint<Type> &constraint)
      : source(source), target(target), constraint(constraint)
    {
      assert(source != target && "Not an intercomponent edge");
      if (this->source > this->target) std::swap(this->source, this->target);
    }

    friend bool operator<(const IntercomponentEdge &lhs,
                          const IntercomponentEdge &rhs) {
      if (lhs.source != rhs.source)
        return lhs.source < rhs.source;
      if (lhs.target != rhs.target)
        return lhs.target < rhs.target;

      // Prefer non-inferred requirement sources.
      bool lhsIsInferred = lhs.constraint.source->isInferredRequirement();
      bool rhsIsInferred = rhs.constraint.source->isInferredRequirement();
      if (lhsIsInferred != rhsIsInferred)
        return rhsIsInferred;;

      return lhs.constraint < rhs.constraint;
    }

    SWIFT_DEBUG_DUMP;
  };
}

void IntercomponentEdge::dump() const {
  llvm::errs() << constraint.getSubjectDependentType({ }).getString() << " -- "
    << constraint.value << ": ";
  constraint.source->print(llvm::errs(), nullptr);
  llvm::errs() << "\n";
}

/// Determine whether the removal of the given edge will disconnect the
/// nodes \c from and \c to within the given equivalence class.
static bool removalDisconnectsEquivalenceClass(
               EquivalenceClass *equivClass,
               llvm::SmallDenseMap<CanType, unsigned> &componentOf,
               std::vector<IntercomponentEdge> &sameTypeEdges,
               unsigned edgeIndex,
               CanType fromDepType,
               CanType toDepType) {
  // Which component are "from" and "to" in within the intercomponent edges?
  assert(componentOf.count(fromDepType) > 0);
  auto fromComponentIndex = componentOf[fromDepType];

  assert(componentOf.count(toDepType) > 0);
  auto toComponentIndex = componentOf[toDepType];

  // If they're in the same component, they're always connected (due to
  // derived edges).
  if (fromComponentIndex == toComponentIndex) return false;

  /// Describes the parents in the equivalence classes we're forming.
  SmallVector<unsigned, 4> parents;
  for (unsigned i : range(equivClass->derivedSameTypeComponents.size())) {
    parents.push_back(i);
  }

  for (const auto existingEdgeIndex : indices(sameTypeEdges)) {
    if (existingEdgeIndex == edgeIndex) continue;

    const auto &edge = sameTypeEdges[existingEdgeIndex];
    if (edge.isSelfDerived) continue;

    if (unionSets(parents, edge.source, edge.target) &&
        findRepresentative(parents, fromComponentIndex) ==
          findRepresentative(parents, toComponentIndex))
      return false;
  }

  const auto &edge = sameTypeEdges[edgeIndex];

  return !unionSets(parents, edge.source, edge.target) ||
    findRepresentative(parents, fromComponentIndex) !=
      findRepresentative(parents, toComponentIndex);
}

static AssociatedTypeDecl *takeMemberOfDependentMemberType(Type &type) {
  if (auto depMemTy = type->getAs<DependentMemberType>()) {
    type = depMemTy->getBase();
    return depMemTy->getAssocType();
  }

  return nullptr;
}

static bool isSelfDerivedNestedTypeNameMatchEdge(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf,
              std::vector<IntercomponentEdge> &sameTypeEdges,
              unsigned edgeIndex) {
  const auto &edge = sameTypeEdges[edgeIndex];
  auto genericParams = builder.getGenericParams();
  Type sourceType = edge.constraint.getSubjectDependentType(genericParams);
  Type target = edge.constraint.value;

  DependentMemberType *sourceDepMemTy;
  while ((sourceDepMemTy = sourceType->getAs<DependentMemberType>()) &&
         sourceDepMemTy->getAssocType() ==
           takeMemberOfDependentMemberType(target)) {
    sourceType = sourceDepMemTy->getBase();

    auto targetEquivClass =
      builder.maybeResolveEquivalenceClass(target,
                                           ArchetypeResolutionKind::WellFormed,
                                           false)
        .getEquivalenceClassIfPresent();
    if (targetEquivClass == equivClass &&
        builder.maybeResolveEquivalenceClass(
                                     sourceType,
                                     ArchetypeResolutionKind::WellFormed,
                                     /*wantExactPotentialArchetype=*/false)
          .getEquivalenceClass(builder) == equivClass &&
        !removalDisconnectsEquivalenceClass(equivClass, componentOf,
                                            sameTypeEdges, edgeIndex,
                                            sourceType->getCanonicalType(),
                                            target->getCanonicalType()))
      return true;
  }

  return false;
}

/// Collapse same-type components using the "delayed" requirements of the
/// equivalence class.
///
/// This operation looks through the delayed requirements within the equivalence
/// class to find paths that connect existing potential archetypes.
static void collapseSameTypeComponentsThroughDelayedRequirements(
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf,
              SmallVectorImpl<unsigned> &collapsedParents,
              unsigned &remainingComponents) {
  unsigned numCollapsedParents = collapsedParents.size();

  /// "Virtual" components for types that aren't resolve to potential
  /// archetypes.
  llvm::SmallDenseMap<CanType, unsigned> virtualComponents;

  /// Retrieve the component for a type representing a virtual component
  auto getTypeVirtualComponent = [&](CanType canType) {
    auto knownActual = componentOf.find(canType);
    if (knownActual != componentOf.end())
      return knownActual->second;

    auto knownVirtual = virtualComponents.find(canType);
    if (knownVirtual != virtualComponents.end())
      return knownVirtual->second;

    unsigned component = collapsedParents.size();
    collapsedParents.push_back(component);
    virtualComponents[canType] = component;
    return component;
  };

  /// Retrieve the component for the given potential archetype.
  auto getPotentialArchetypeVirtualComponent = [&](PotentialArchetype *pa) {
    if (pa->getEquivalenceClassIfPresent() == equivClass)
      return getTypeVirtualComponent(pa->getDependentType());

    // We found a potential archetype in another equivalence class. Treat it
    // as a "virtual" component representing that potential archetype's
    // equivalence class.
    return getTypeVirtualComponent(
             pa->getRepresentative()->getDependentType());
  };

  for (const auto &delayedReq : equivClass->delayedRequirements) {
    // Only consider same-type requirements.
    if (delayedReq.kind != DelayedRequirement::SameType) continue;

    unsigned lhsComponent;
    if (auto lhsPA = delayedReq.lhs.dyn_cast<PotentialArchetype *>())
      lhsComponent = getPotentialArchetypeVirtualComponent(lhsPA);
    else
      lhsComponent = getTypeVirtualComponent(delayedReq.lhs.get<Type>()
          ->getCanonicalType());

    unsigned rhsComponent;
    if (auto rhsPA = delayedReq.rhs.dyn_cast<PotentialArchetype *>())
      rhsComponent = getPotentialArchetypeVirtualComponent(rhsPA);
    else
      rhsComponent = getTypeVirtualComponent(delayedReq.rhs.get<Type>()
          ->getCanonicalType());

    // Collapse the sets
    if (unionSets(collapsedParents, lhsComponent, rhsComponent,
                  numCollapsedParents) &&
        lhsComponent < numCollapsedParents &&
        rhsComponent < numCollapsedParents)
      --remainingComponents;
  }

  /// Remove any additional collapsed parents we added.
  collapsedParents.erase(collapsedParents.begin() + numCollapsedParents,
                         collapsedParents.end());
}

/// Collapse same-type components within an equivalence class, minimizing the
/// number of requirements required to express the equivalence class.
static void collapseSameTypeComponents(
              GenericSignatureBuilder &builder,
              EquivalenceClass *equivClass,
              llvm::SmallDenseMap<CanType, unsigned> &componentOf,
              std::vector<IntercomponentEdge> &sameTypeEdges) {
  SmallVector<unsigned, 4> collapsedParents;
  for (unsigned i : indices(equivClass->derivedSameTypeComponents)) {
    collapsedParents.push_back(i);
  }

  unsigned remainingComponents = equivClass->derivedSameTypeComponents.size();
  for (unsigned edgeIndex : indices(sameTypeEdges)) {
    auto &edge = sameTypeEdges[edgeIndex];

    // If this edge is self-derived, remove it.
    if (isSelfDerivedNestedTypeNameMatchEdge(builder, equivClass, componentOf,
                                             sameTypeEdges, edgeIndex)) {
      // Note that this edge is self-derived, so we don't consider it again.
      edge.isSelfDerived = true;

      auto &constraints = equivClass->sameTypeConstraints;
      auto known =
        std::find_if(constraints.begin(), constraints.end(),
                     [&](const Constraint<Type> &existing) {
                       // Check the requirement source, first.
                       if (existing.source != edge.constraint.source)
                         return false;

                       return
                         (existing.hasSameSubjectAs(edge.constraint) &&
                          existing.value->isEqual(edge.constraint.value)) ||
                         (existing.isSubjectEqualTo(edge.constraint.value) &&
                          edge.constraint.isSubjectEqualTo(existing.value));
                     });
      assert(known != constraints.end());
      constraints.erase(known);
      continue;
    }

    // Otherwise, collapse the derived same-type components along this edge,
    // because it's derived.
    if (unionSets(collapsedParents, edge.source, edge.target))
      --remainingComponents;
  }

  if (remainingComponents > 1) {
    // Collapse same-type components by looking at the delayed requirements.
    collapseSameTypeComponentsThroughDelayedRequirements(
      equivClass, componentOf, collapsedParents, remainingComponents);
  }

  // If needed, collapse the same-type components merged by a derived
  // nested-type-name-match edge.
  unsigned maxComponents = equivClass->derivedSameTypeComponents.size();
  if (remainingComponents < maxComponents) {
    std::vector<DerivedSameTypeComponent> newComponents;
    std::vector<unsigned> newIndices(maxComponents, maxComponents);

    for (unsigned oldIndex : range(0, maxComponents)) {
      auto &oldComponent = equivClass->derivedSameTypeComponents[oldIndex];
      unsigned oldRepresentativeIndex =
        findRepresentative(collapsedParents, oldIndex);

      // If this is the representative, it's a new component; record it.
      if (oldRepresentativeIndex == oldIndex) {
        assert(newIndices[oldIndex] == maxComponents &&
               "Already saw this component?");
        unsigned newIndex = newComponents.size();
        newIndices[oldIndex] = newIndex;
        newComponents.push_back(
          {oldComponent.type, oldComponent.concreteTypeSource});
        continue;
      }

      // This is not the representative; merge it into the representative
      // component.
      auto newRepresentativeIndex = newIndices[oldRepresentativeIndex];
      assert(newRepresentativeIndex != maxComponents &&
             "Representative should have come earlier");
      auto &newComponent = newComponents[newRepresentativeIndex];

      // If the old component has a better anchor, keep it.
      if (compareDependentTypes(oldComponent.type, newComponent.type) < 0) {
        newComponent.type = oldComponent.type;
      }

      // If the old component has a better concrete type source, keep it.
      if (!newComponent.concreteTypeSource ||
          (oldComponent.concreteTypeSource &&
           oldComponent.concreteTypeSource
             ->compare(newComponent.concreteTypeSource) < 0))
        newComponent.concreteTypeSource = oldComponent.concreteTypeSource;
    }

    // Move the new results into place.
    equivClass->derivedSameTypeComponents = std::move(newComponents);
  }

  // Sort the components.
  llvm::array_pod_sort(equivClass->derivedSameTypeComponents.begin(),
                       equivClass->derivedSameTypeComponents.end());
}

void GenericSignatureBuilder::checkSameTypeConstraints(
                          TypeArrayView<GenericTypeParamType> genericParams,
                          EquivalenceClass *equivClass) {
  if (!equivClass->derivedSameTypeComponents.empty())
    return;

  // Remove self-derived constraints.
  removeSelfDerived(*this, equivClass->sameTypeConstraints,
                    /*proto=*/nullptr,
                    /*allCanBeSelfDerived=*/true);

  // Sort the constraints, so we get a deterministic ordering of diagnostics.
  llvm::array_pod_sort(equivClass->sameTypeConstraints.begin(),
                       equivClass->sameTypeConstraints.end());

  // Compute the components in the subgraph of the same-type constraint graph
  // that includes only derived constraints.
  llvm::SmallDenseMap<CanType, unsigned> componentOf;
  computeDerivedSameTypeComponents(*this, equivClass, componentOf);

  // Go through all of the same-type constraints, collecting all of the
  // non-derived constraints to put them into bins: intra-component and
  // inter-component.

  // Intra-component edges are stored per-component, so we can perform
  // diagnostics within each component.
  unsigned numComponents = equivClass->derivedSameTypeComponents.size();
  std::vector<std::vector<Constraint<Type>>>
    intracomponentEdges(numComponents,
                        std::vector<Constraint<Type>>());

  // Intercomponent edges are stored as one big list, which tracks the
  // source/target components.
  std::vector<IntercomponentEdge> intercomponentEdges;
  std::vector<IntercomponentEdge> nestedTypeNameMatchEdges;
  for (const auto &constraint : equivClass->sameTypeConstraints) {
    // If the source/destination are identical, complain.
    if (constraint.isSubjectEqualTo(constraint.value)) {
      if (constraint.source->shouldDiagnoseRedundancy(true)) {
        Diags.diagnose(constraint.source->getLoc(),
                       diag::redundant_same_type_constraint,
                       constraint.getSubjectDependentType(genericParams),
                       constraint.value);
      }

      continue;
    }

    // Determine which component each of the source/destination fall into.
    CanType subjectType =
      constraint.getSubjectDependentType({ })->getCanonicalType();
    assert(componentOf.count(subjectType) > 0 &&
           "unknown potential archetype?");
    unsigned firstComponentIdx = componentOf[subjectType];
    assert(componentOf.count(constraint.value->getCanonicalType()) > 0 &&
           "unknown potential archetype?");
    unsigned secondComponentIdx =
      componentOf[constraint.value->getCanonicalType()];

    // Separately track nested-type-name-match constraints.
    if (constraint.source->getRoot()->kind ==
          RequirementSource::NestedTypeNameMatch) {
      // If this is an intercomponent edge, record it separately.
      if (firstComponentIdx != secondComponentIdx) {
        nestedTypeNameMatchEdges.push_back(
          IntercomponentEdge(firstComponentIdx, secondComponentIdx, constraint));
      }

      continue;
    }

    // If both vertices are within the same component, this is an
    // intra-component edge. Record it as such.
    if (firstComponentIdx == secondComponentIdx) {
      intracomponentEdges[firstComponentIdx].push_back(constraint);
      continue;
    }

    // Otherwise, it's an intercomponent edge, which is never derived.
    assert(!constraint.source->isDerivedRequirement() &&
           "Must not be derived");

    // Ignore inferred requirements; we don't want to diagnose them.
    intercomponentEdges.push_back(
      IntercomponentEdge(firstComponentIdx, secondComponentIdx, constraint));
  }

  // Walk through each of the components, checking the intracomponent edges.
  // This will diagnose any explicitly-specified requirements within a
  // component, all of which are redundant.
  for (auto &constraints : intracomponentEdges) {
    if (constraints.empty()) continue;

    checkConstraintList<Type>(
      genericParams, constraints, RequirementKind::SameType,
      [](const Constraint<Type> &) { return true; },
      [](const Constraint<Type> &constraint) {
        // Ignore nested-type-name-match constraints.
        if (constraint.source->getRoot()->kind ==
              RequirementSource::NestedTypeNameMatch)
          return ConstraintRelation::Unrelated;

        return ConstraintRelation::Redundant;
      },
      None,
      diag::redundant_same_type_constraint,
      diag::previous_same_type_constraint);
  }

  // Diagnose redundant same-type constraints across components. First,
  // sort the edges so that edges that between the same component pairs
  // occur next to each other.
  llvm::array_pod_sort(intercomponentEdges.begin(), intercomponentEdges.end());

  // Diagnose and erase any redundant edges between the same two components.
  intercomponentEdges.erase(
    std::unique(
      intercomponentEdges.begin(), intercomponentEdges.end(),
      [&](const IntercomponentEdge &lhs,
          const IntercomponentEdge &rhs) {
        // If either the source or target is different, we have
        // different elements.
        if (lhs.source != rhs.source || lhs.target != rhs.target)
          return false;

        // Check whethe we should diagnose redundancy for both constraints.
        if (!lhs.constraint.source->shouldDiagnoseRedundancy(true) ||
            !rhs.constraint.source->shouldDiagnoseRedundancy(false))
          return true;

        Diags.diagnose(lhs.constraint.source->getLoc(),
                       diag::redundant_same_type_constraint,
                       lhs.constraint.getSubjectDependentType(genericParams),
                       lhs.constraint.value);
        Diags.diagnose(rhs.constraint.source->getLoc(),
                       diag::previous_same_type_constraint,
                       rhs.constraint.source->classifyDiagKind(),
                       rhs.constraint.getSubjectDependentType(genericParams),
                       rhs.constraint.value);
        return true;
      }),
    intercomponentEdges.end());

  // If we have more intercomponent edges than are needed to form a spanning
  // tree, complain about redundancies. Note that the edges we have must
  // connect all of the components, or else we wouldn't have an equivalence
  // class.
  if (intercomponentEdges.size() > numComponents - 1) {
    // First let's order all of the intercomponent edges
    // as written in source, this helps us to diagnose
    // all of the duplicate constraints in correct order.
    std::vector<IntercomponentEdge *> sourceOrderedEdges;
    for (auto &edge : intercomponentEdges)
      sourceOrderedEdges.push_back(&edge);

    llvm::array_pod_sort(
        sourceOrderedEdges.begin(), sourceOrderedEdges.end(),
        [](IntercomponentEdge *const *a, IntercomponentEdge *const *b) -> int {
          auto &sourceMgr = (*a)->constraint.value->getASTContext().SourceMgr;

          auto locA = (*a)->constraint.source->getLoc();
          auto locB = (*b)->constraint.source->getLoc();

          // Put invalid locations after valid ones.
          if (locA.isInvalid() || locB.isInvalid()) {
            if (locA.isInvalid() != locB.isInvalid())
              return locA.isValid() ? 1 : -1;

            return 0;
          }

          auto bufferA = sourceMgr.findBufferContainingLoc(locA);
          auto bufferB = sourceMgr.findBufferContainingLoc(locB);

          if (bufferA != bufferB)
            return bufferA < bufferB ? -1 : 1;

          auto offsetA = sourceMgr.getLocOffsetInBuffer(locA, bufferA);
          auto offsetB = sourceMgr.getLocOffsetInBuffer(locB, bufferB);

          return offsetA < offsetB ? -1 : (offsetA == offsetB ? 0 : 1);
        });

    auto isDiagnosable = [](const IntercomponentEdge &edge, bool isPrimary) {
      return edge.constraint.source->shouldDiagnoseRedundancy(isPrimary);
    };

    using EquivClass = llvm::DenseMap<Type, unsigned>;
    llvm::DenseMap<Type, EquivClass> equivalences;
    // The idea here is to form an equivalence class per representative
    // (picked from each edge constraint in type parameter order) and
    // propagate all new equivalent types up the chain until duplicate
    // entry is found, that entry is going to point to previous
    // declaration and is going to mark current edge as a duplicate of
    // such entry.
    for (auto edgeIdx : indices(sourceOrderedEdges)) {
      const auto &edge = *sourceOrderedEdges[edgeIdx];

      Type lhs = edge.constraint.getSubjectDependentType(genericParams);
      Type rhs = edge.constraint.value;

      // Make sure that representative for equivalence class is picked
      // in canonical type parameter order.
      if (compareDependentTypes(rhs, lhs) < 0)
        std::swap(lhs, rhs);

      // Index of the previous declaration of the same-type constraint
      // which current edge might be a duplicate of.
      Optional<unsigned> previousIndex;

      bool isDuplicate = false;
      auto &representative = equivalences[lhs];
      if (representative.insert({rhs, edgeIdx}).second) {
        // Since this is a new equivalence, and right-hand side might
        // be a representative of some other equivalence class,
        // its existing members have to be merged up.
        auto RHSEquivClass = equivalences.find(rhs);
        if (RHSEquivClass != equivalences.end()) {
          auto &equivClass = RHSEquivClass->getSecond();
          representative.insert(equivClass.begin(), equivClass.end());
        }

        // If left-hand side is involved in any other equivalences
        // let's propagate new information up the chain.
        for (auto &e : equivalences) {
          auto &repr = e.first;
          auto &equivClass = e.second;

          if (repr->isEqual(lhs) || !equivClass.count(lhs))
            continue;

          if (!equivClass.insert({rhs, edgeIdx}).second) {
            // Even if "previous" edge is not diagnosable we
            // still need to produce diagnostic about main duplicate.
            isDuplicate = true;

            auto prevIdx = equivClass[rhs];
            if (!isDiagnosable(intercomponentEdges[prevIdx],
                               /*isPrimary=*/false))
              continue;

            // If there is a diagnosable duplicate equivalence,
            // it means that we've found our previous declaration.
            previousIndex = prevIdx;
            break;
          }
        }
      } else {
        // Looks like this is a situation like T.A == T.B, ..., T.B == T.A
        previousIndex = representative[rhs];
        isDuplicate = true;
      }

      if (!isDuplicate || !isDiagnosable(edge, /*isPrimary=*/true))
        continue;

      Diags.diagnose(edge.constraint.source->getLoc(),
                     diag::redundant_same_type_constraint,
                     edge.constraint.getSubjectDependentType(genericParams),
                     edge.constraint.value);

      if (previousIndex) {
        auto &prevEquiv = sourceOrderedEdges[*previousIndex]->constraint;
        Diags.diagnose(
            prevEquiv.source->getLoc(), diag::previous_same_type_constraint,
            prevEquiv.source->classifyDiagKind(),
            prevEquiv.getSubjectDependentType(genericParams), prevEquiv.value);
      }
    }
  }

  collapseSameTypeComponents(*this, equivClass, componentOf,
                             nestedTypeNameMatchEdges);
}

void GenericSignatureBuilder::checkConcreteTypeConstraints(
                              TypeArrayView<GenericTypeParamType> genericParams,
                              EquivalenceClass *equivClass) {
  // Resolve any thus-far-unresolved dependent types.
  Type resolvedConcreteType =
    getCanonicalTypeInContext(equivClass->concreteType, genericParams);

  removeSelfDerived(*this, equivClass->concreteTypeConstraints,
                    /*proto=*/nullptr,
                    /*allCanBeSelfDerived=*/true);

  // This can occur if the combination of a superclass requirement and
  // protocol conformance requirement force a type to become concrete.
  if (equivClass->concreteTypeConstraints.empty())
    return;

  checkConstraintList<Type>(
    genericParams, equivClass->concreteTypeConstraints, RequirementKind::SameType,
    [&](const ConcreteConstraint &constraint) {
      if (constraint.value->isEqual(resolvedConcreteType))
        return true;

      auto resolvedType = getCanonicalTypeInContext(constraint.value, { });
      return resolvedType->isEqual(resolvedConcreteType);
    },
    [&](const Constraint<Type> &constraint) {
      Type concreteType = constraint.value;

      // If the concrete type is equivalent, the constraint is redundant.
      if (concreteType->isEqual(equivClass->concreteType))
        return ConstraintRelation::Redundant;

      // If either has a type parameter or type variable, call them unrelated.
      if (concreteType->hasTypeParameter() ||
          equivClass->concreteType->hasTypeParameter() ||
          concreteType->hasTypeVariable() ||
          equivClass->concreteType->hasTypeVariable())
        return ConstraintRelation::Unrelated;

      return ConstraintRelation::Conflicting;
    },
    diag::same_type_conflict,
    diag::redundant_same_type_to_concrete,
    diag::same_type_redundancy_here);
}

bool GenericSignatureBuilder::isRedundantExplicitRequirement(
    const ExplicitRequirement &req) const {
  assert(Impl->computedRedundantRequirements &&
         "Must ensure computeRedundantRequirements() is called first");
  auto &redundantReqs = Impl->RedundantRequirements;
  return (redundantReqs.find(req) != redundantReqs.end());
}

static Optional<Requirement> createRequirement(RequirementKind kind,
                                               Type depTy,
                                               RequirementRHS rhs,
                           TypeArrayView<GenericTypeParamType> genericParams) {

  depTy = getSugaredDependentType(depTy, genericParams);

  if (auto type = rhs.dyn_cast<Type>()) {
    if (type->hasError())
      return None;

    // Drop requirements involving concrete types containing
    // unresolved associated types.
    if (type->findUnresolvedDependentMemberType())
      return None;

    if (type->isTypeParameter())
      type = getSugaredDependentType(type, genericParams);

    return Requirement(kind, depTy, type);
  } else if (auto *proto = rhs.dyn_cast<ProtocolDecl *>()) {
    auto type = proto->getDeclaredInterfaceType();
    return Requirement(kind, depTy, type);
  } else {
    auto layoutConstraint = rhs.get<LayoutConstraint>();
    return Requirement(kind, depTy, layoutConstraint);
  }
}

/// Determine the canonical ordering of requirements.
static unsigned getRequirementKindOrder(RequirementKind kind) {
  switch (kind) {
  case RequirementKind::Conformance: return 2;
  case RequirementKind::Superclass: return 0;
  case RequirementKind::SameType: return 3;
  case RequirementKind::Layout: return 1;
  }
  llvm_unreachable("unhandled kind");
}

static int compareRequirements(const Requirement *lhsPtr,
                               const Requirement *rhsPtr) {
  auto &lhs = *lhsPtr;
  auto &rhs = *rhsPtr;

  int compareLHS =
    compareDependentTypes(lhs.getFirstType(), rhs.getFirstType());

  if (compareLHS != 0)
    return compareLHS;

  int compareKind = (getRequirementKindOrder(lhs.getKind()) -
                     getRequirementKindOrder(rhs.getKind()));

  if (compareKind != 0)
    return compareKind;

  // We should only have multiple conformance requirements.
  assert(lhs.getKind() == RequirementKind::Conformance);

  int compareProtos =
    TypeDecl::compare(lhs.getProtocolDecl(), rhs.getProtocolDecl());

  assert(compareProtos != 0 && "Duplicate conformance requirement");

  return compareProtos;
}

void GenericSignatureBuilder::enumerateRequirements(
                   TypeArrayView<GenericTypeParamType> genericParams,
                   SmallVectorImpl<Requirement> &requirements) {
  auto recordRequirement = [&](RequirementKind kind,
                               Type depTy,
                               RequirementRHS rhs) {
    if (auto req = createRequirement(kind, depTy, rhs, genericParams))
      requirements.push_back(*req);
    else
      Impl->HadAnyError = true;
  };

  // Collect all non-same type requirements.
  for (auto &req : Impl->ExplicitRequirements) {
    if (isRedundantExplicitRequirement(req))
      continue;

    auto depTy = getCanonicalTypeInContext(
        req.getSubjectType(), { });

    // FIXME: This should be an assert once we ensure that concrete
    // same-type requirements always mark other requirements on the
    // same subject type as redundant or conflicting.
    if (!depTy->isTypeParameter())
      continue;

    auto rhs = req.getRHS();
    if (auto constraintType = rhs.dyn_cast<Type>()) {
      rhs = getCanonicalTypeInContext(constraintType, genericParams);
    }

    recordRequirement(req.getKind(), depTy, rhs);
  }

  // Collect all same type requirements.
  if (!Impl->ExplicitSameTypeRequirements.empty()) {
    for (auto &equivClass : Impl->EquivalenceClasses) {
      if (equivClass.derivedSameTypeComponents.empty()) {
        checkSameTypeConstraints(genericParams, &equivClass);
      }

      for (unsigned i : indices(equivClass.derivedSameTypeComponents)) {
        // Dig out the subject type and its corresponding component.
        auto &component = equivClass.derivedSameTypeComponents[i];
        Type subjectType = component.type;

        assert(!subjectType->hasError());
        assert(!subjectType->findUnresolvedDependentMemberType());

        // If this equivalence class is bound to a concrete type, equate the
        // anchor with a concrete type.
        if (Type concreteType = equivClass.concreteType) {
          concreteType = getCanonicalTypeInContext(concreteType, genericParams);

          // If the parent of this anchor is also a concrete type, don't
          // create a requirement.
          if (!subjectType->is<GenericTypeParamType>() &&
              maybeResolveEquivalenceClass(
                subjectType->castTo<DependentMemberType>()->getBase(),
                ArchetypeResolutionKind::WellFormed,
                /*wantExactPotentialArchetype=*/false)
                .getEquivalenceClass(*this)->concreteType)
            continue;

          // Drop recursive and invalid concrete-type constraints.
          if (equivClass.recursiveConcreteType ||
              equivClass.invalidConcreteType)
            continue;

          // Filter out derived requirements... except for concrete-type
          // requirements on generic parameters. The exception is due to
          // the canonicalization of generic signatures, which never
          // eliminates generic parameters even when they have been
          // mapped to a concrete type.
          if (subjectType->is<GenericTypeParamType>() ||
              component.concreteTypeSource == nullptr ||
              !component.concreteTypeSource->isDerivedRequirement()) {
            recordRequirement(RequirementKind::SameType,
                              subjectType, concreteType);
          }
          continue;
        }

        // If we're at the last anchor in the component, do nothing;
        if (i + 1 != equivClass.derivedSameTypeComponents.size()) {
          // Form a same-type constraint from this anchor within the component
          // to the next.
          // FIXME: Distinguish between explicit and inferred here?
          auto &nextComponent = equivClass.derivedSameTypeComponents[i + 1];
          Type otherSubjectType = nextComponent.type;

          recordRequirement(RequirementKind::SameType,
                            subjectType, otherSubjectType);
        }
      }
    }
  }

  // Sort the requirements in canonical order.
  llvm::array_pod_sort(requirements.begin(), requirements.end(),
                       compareRequirements);
}

void GenericSignatureBuilder::dump() {
  dump(llvm::errs());
}

void GenericSignatureBuilder::dump(llvm::raw_ostream &out) {
  out << "Potential archetypes:\n";
  for (auto pa : Impl->PotentialArchetypes) {
    pa->dump(out, &Context.SourceMgr, 2);
  }
  out << "\n";

  out << "Equivalence classes:\n";
  for (auto &equiv : Impl->EquivalenceClasses) {
    equiv.dump(out, this);
  }
  out << "\n";
}

void GenericSignatureBuilder::addGenericSignature(GenericSignature sig) {
  if (!sig) return;

  for (auto param : sig->getGenericParams())
    addGenericParameter(param);

  for (auto &reqt : sig->getRequirements())
    addRequirement(reqt, FloatingRequirementSource::forAbstract(), nullptr);
}

#ifndef NDEBUG

static void checkGenericSignature(CanGenericSignature canSig,
                                  GenericSignatureBuilder &builder) {
  PrettyStackTraceGenericSignature debugStack("checking", canSig);

  auto canonicalRequirements = canSig->getRequirements();

  // Check that the signature is canonical.
  for (unsigned idx : indices(canonicalRequirements)) {
    debugStack.setRequirement(idx);

    const auto &reqt = canonicalRequirements[idx];

    // Left-hand side must be canonical in its context.
    // Check canonicalization of requirement itself.
    switch (reqt.getKind()) {
    case RequirementKind::Superclass:
      assert(canSig->isCanonicalTypeInContext(reqt.getFirstType(), builder) &&
             "Left-hand side is not canonical");
      assert(canSig->isCanonicalTypeInContext(reqt.getSecondType(), builder) &&
             "Superclass type isn't canonical in its own context");
      break;

    case RequirementKind::Layout:
      assert(canSig->isCanonicalTypeInContext(reqt.getFirstType(), builder) &&
             "Left-hand side is not canonical");
      break;

    case RequirementKind::SameType: {
      auto isCanonicalAnchor = [&](Type type) {
        if (auto *dmt = type->getAs<DependentMemberType>())
          return canSig->isCanonicalTypeInContext(dmt->getBase(), builder);
        return type->is<GenericTypeParamType>();
      };

      auto firstType = reqt.getFirstType();
      auto secondType = reqt.getSecondType();
      assert(isCanonicalAnchor(firstType));

      if (reqt.getSecondType()->isTypeParameter()) {
        assert(isCanonicalAnchor(secondType));
        assert(compareDependentTypes(firstType, secondType) < 0 &&
               "Out-of-order type parameters in same-type constraint");
      } else {
        assert(canSig->isCanonicalTypeInContext(secondType, builder) &&
               "Concrete same-type isn't canonical in its own context");
      }
      break;
    }

    case RequirementKind::Conformance:
      assert(canSig->isCanonicalTypeInContext(reqt.getFirstType(), builder) &&
             "Left-hand side is not canonical");
      assert(reqt.getFirstType()->isTypeParameter() &&
             "Left-hand side must be a type parameter");
      assert(isa<ProtocolType>(reqt.getSecondType().getPointer()) &&
             "Right-hand side of conformance isn't a protocol type");
      break;
    }

    // From here on, we're only interested in requirements beyond the first.
    if (idx == 0) continue;

    // Make sure that the left-hand sides are in nondecreasing order.
    const auto &prevReqt = canonicalRequirements[idx-1];
    int compareLHS =
      compareDependentTypes(prevReqt.getFirstType(), reqt.getFirstType());
    assert(compareLHS <= 0 && "Out-of-order left-hand sides");

    // If we have two same-type requirements where the left-hand sides differ
    // but fall into the same equivalence class, we can check the form.
    if (compareLHS < 0 && reqt.getKind() == RequirementKind::SameType &&
        prevReqt.getKind() == RequirementKind::SameType &&
        canSig->areSameTypeParameterInContext(prevReqt.getFirstType(),
                                              reqt.getFirstType(),
                                              builder)) {
      // If it's a it's a type parameter, make sure the equivalence class is
      // wired together sanely.
      if (prevReqt.getSecondType()->isTypeParameter()) {
        assert(prevReqt.getSecondType()->isEqual(reqt.getFirstType()) &&
               "same-type constraints within an equiv. class are out-of-order");
      } else {
        // Otherwise, the concrete types must match up.
        assert(prevReqt.getSecondType()->isEqual(reqt.getSecondType()) &&
               "inconsistent concrete same-type constraints in equiv. class");
      }
    }

    // If we have a concrete same-type requirement, we shouldn't have any
    // other requirements on the same type.
    if (reqt.getKind() == RequirementKind::SameType &&
        !reqt.getSecondType()->isTypeParameter()) {
      assert(compareLHS < 0 &&
             "Concrete subject type should not have any other requirements");
    }

    assert(compareRequirements(&prevReqt, &reqt) < 0 &&
           "Out-of-order requirements");
  }
}
#endif

static Type stripBoundDependentMemberTypes(Type t) {
  if (auto *depMemTy = t->getAs<DependentMemberType>()) {
    return DependentMemberType::get(
      stripBoundDependentMemberTypes(depMemTy->getBase()),
      depMemTy->getName());
  }

  return t;
}

static Requirement stripBoundDependentMemberTypes(Requirement req) {
  auto subjectType = stripBoundDependentMemberTypes(req.getFirstType());

  switch (req.getKind()) {
  case RequirementKind::Conformance:
    return Requirement(RequirementKind::Conformance, subjectType,
                       req.getSecondType());

  case RequirementKind::Superclass:
  case RequirementKind::SameType:
    return Requirement(req.getKind(), subjectType,
                       req.getSecondType().transform([](Type t) {
                         return stripBoundDependentMemberTypes(t);
                       }));

  case RequirementKind::Layout:
    return Requirement(RequirementKind::Layout, subjectType,
                       req.getLayoutConstraint());
  }

  llvm_unreachable("Bad requirement kind");
}

GenericSignature GenericSignatureBuilder::rebuildSignatureWithoutRedundantRequirements(
                                          bool allowConcreteGenericParams,
                                          const ProtocolDecl *requirementSignatureSelfProto) && {
  NumSignaturesRebuiltWithoutRedundantRequirements++;

  GenericSignatureBuilder newBuilder(Context);
  newBuilder.Impl->RebuildingWithoutRedundantConformances = true;

  for (auto param : getGenericParams())
    newBuilder.addGenericParameter(param);

  const RequirementSource *requirementSignatureSource = nullptr;
  if (auto *proto = const_cast<ProtocolDecl *>(requirementSignatureSelfProto)) {
    auto selfType = proto->getSelfInterfaceType();
    requirementSignatureSource =
        RequirementSource::forRequirementSignature(newBuilder, selfType, proto);

    // Add the conformance requirement 'Self : Proto' directly without going
    // through addConformanceRequirement(), since the latter calls
    // expandConformanceRequirement(), which we want to skip since we're
    // re-adding the requirements directly below.
    auto resolvedType = ResolvedType(newBuilder.Impl->PotentialArchetypes[0]);
    auto equivClass = resolvedType.getEquivalenceClass(newBuilder);

    (void) equivClass->recordConformanceConstraint(newBuilder, resolvedType, proto,
                                                   requirementSignatureSource);
  }

  auto getRebuiltSource = [&](const RequirementSource *source) {
    assert(!source->isDerivedRequirement());

    if (auto *proto = const_cast<ProtocolDecl *>(requirementSignatureSelfProto)) {
      return FloatingRequirementSource::viaProtocolRequirement(
          requirementSignatureSource, proto, source->getLoc(),
          source->isInferredRequirement());
    }

    if (source->isInferredRequirement())
      return FloatingRequirementSource::forInferred(source->getLoc());

    return FloatingRequirementSource::forExplicit(source->getLoc());
  };

  for (const auto &req : Impl->ExplicitRequirements) {
    if (Impl->DebugRedundantRequirements) {
      req.dump(llvm::dbgs(), &Context.SourceMgr);
      llvm::dbgs() << "\n";
    }

    assert(req.getKind() != RequirementKind::SameType &&
           "Should not see same-type requirement here");

    if (isRedundantExplicitRequirement(req) &&
        Impl->ExplicitConformancesImpliedByConcrete.count(req)) {
      if (Impl->DebugRedundantRequirements) {
        llvm::dbgs() << "... skipping\n";
      }

      continue;
    }

    auto subjectType = req.getSubjectType();
    subjectType = stripBoundDependentMemberTypes(subjectType);
    auto resolvedSubjectType =
        resolveDependentMemberTypes(*this, subjectType,
                                    ArchetypeResolutionKind::WellFormed);

    // This can occur if the combination of a superclass requirement and
    // protocol conformance requirement force a type to become concrete.
    //
    // FIXME: Is there a more principled formulation of this?
    if (req.getKind() == RequirementKind::Superclass &&
        !resolvedSubjectType->isTypeParameter()) {
      newBuilder.addRequirement(Requirement(RequirementKind::SameType,
                                            subjectType, resolvedSubjectType),
                                getRebuiltSource(req.getSource()), nullptr);
      continue;
    }

    assert(resolvedSubjectType->isTypeParameter());

    if (auto optReq = createRequirement(req.getKind(), resolvedSubjectType,
                                        req.getRHS(), getGenericParams())) {
      auto newReq = stripBoundDependentMemberTypes(*optReq);
      newBuilder.addRequirement(newReq, getRebuiltSource(req.getSource()),
                                nullptr);
    } else {
      Impl->HadAnyError = true;
    }
  }

  for (const auto &req : Impl->ExplicitSameTypeRequirements) {
    if (Impl->DebugRedundantRequirements) {
      req.dump(llvm::dbgs(), &Context.SourceMgr);
      llvm::dbgs() << "\n";
    }

    auto resolveType = [this](Type t) -> Type {
      t = stripBoundDependentMemberTypes(t);
      if (t->is<GenericTypeParamType>()) {
        return t;
      } else if (auto *depMemTy = t->getAs<DependentMemberType>()) {
        auto resolvedBaseTy =
            resolveDependentMemberTypes(*this, depMemTy->getBase(),
                                        ArchetypeResolutionKind::WellFormed);


        if (resolvedBaseTy->isTypeParameter()) {
          return DependentMemberType::get(resolvedBaseTy, depMemTy->getName());
        } else {
          return resolveDependentMemberTypes(*this, t,
                                             ArchetypeResolutionKind::WellFormed);
        }
      } else {
        return t;
      }
    };

    auto subjectType = resolveType(req.getSubjectType());
    auto constraintType = resolveType(req.getRHS().get<Type>());

    auto newReq = stripBoundDependentMemberTypes(
        Requirement(RequirementKind::SameType,
                    subjectType, constraintType));

    if (Impl->DebugRedundantRequirements) {
      llvm::dbgs() << "=> ";
      newReq.dump(llvm::dbgs());
      llvm::dbgs() << "\n";
    }
    newBuilder.addRequirement(newReq, getRebuiltSource(req.getSource()),
                              nullptr);
  }

  // Wipe out the internal state of the old builder, since we don't need it anymore.
  Impl.reset();

  // Build a new signature using the new builder.
  return std::move(newBuilder).computeGenericSignature(
      allowConcreteGenericParams,
      requirementSignatureSelfProto);
}

GenericSignature GenericSignatureBuilder::computeGenericSignature(
                                          bool allowConcreteGenericParams,
                                          const ProtocolDecl *requirementSignatureSelfProto) && {
  // Process any delayed requirements that we can handle now.
  processDelayedRequirements();

  computeRedundantRequirements(requirementSignatureSelfProto);

  // If any of our explicit conformance requirements were implied by
  // superclass or concrete same-type requirements, we have to build the
  // signature again, since dropping the redundant conformance requirements
  // changes the canonical type computation.
  //
  // However, if we already diagnosed an error, don't do this, because
  // we might end up emitting duplicate diagnostics.
  //
  // Also, don't do this when building a requirement signature.
  if (!Impl->RebuildingWithoutRedundantConformances &&
      !Impl->HadAnyError &&
      !Impl->ExplicitConformancesImpliedByConcrete.empty()) {
    diagnoseRedundantRequirements(
        /*onlyDiagnoseExplicitConformancesImpliedByConcrete=*/true);

    if (Impl->DebugRedundantRequirements) {
      llvm::dbgs() << "Going to rebuild signature\n";
    }
    return std::move(*this).rebuildSignatureWithoutRedundantRequirements(
        allowConcreteGenericParams,
        requirementSignatureSelfProto);
  }

  if (Impl->RebuildingWithoutRedundantConformances) {
    if (Impl->DebugRedundantRequirements) {
      llvm::dbgs() << "Rebuilding signature\n";
    }

#ifndef NDEBUG
    for (const auto &req : Impl->ExplicitConformancesImpliedByConcrete) {
      if (isRedundantExplicitRequirement(req)) {
        llvm::errs() << "Rebuilt signature still had redundant conformance requirement\n";
        req.dump(llvm::errs(), &Context.SourceMgr);
        llvm::errs() << "\n";
        abort();
      }
    }
#endif
  }

  // Diagnose redundant requirements, check for recursive concrete types
  // and compute minimized same-type requirements.
  finalize(getGenericParams(),
           allowConcreteGenericParams,
           requirementSignatureSelfProto);

  // Collect all non-redundant explicit requirements.
  SmallVector<Requirement, 4> requirements;
  enumerateRequirements(getGenericParams(), requirements);

  // Form the generic signature.
  auto sig = GenericSignature::get(getGenericParams(), requirements);

#ifndef NDEBUG
  if (!Impl->HadAnyError) {
    checkGenericSignature(sig.getCanonicalSignature(), *this);
  }
#endif

  // When we can, move this generic signature builder to make it the canonical
  // builder, rather than constructing a new generic signature builder that
  // will produce the same thing.
  //
  // We cannot do this when there were errors.
  //
  // Also, we cannot do this when building a requirement signature.
  if (requirementSignatureSelfProto == nullptr &&
      !Impl->HadAnyError) {
    // Register this generic signature builder as the canonical builder for the
    // given signature.
    Context.registerGenericSignatureBuilder(sig, std::move(*this));
  }

  // Wipe out the internal state, ensuring that nobody uses this builder for
  // anything more.
  Impl.reset();

  return sig;
}

#pragma mark Generic signature verification

void GenericSignatureBuilder::verifyGenericSignature(ASTContext &context,
                                                     GenericSignature sig) {
  llvm::errs() << "Validating generic signature: ";
  sig->print(llvm::errs());
  llvm::errs() << "\n";

  // Try building a new signature having the same requirements.
  auto genericParams = sig->getGenericParams();
  auto requirements = sig->getRequirements();

  {
    PrettyStackTraceGenericSignature debugStack("verifying", sig);

    // Form a new generic signature builder.
    GenericSignatureBuilder builder(context);

    // Add the generic parameters.
    for (auto gp : genericParams)
      builder.addGenericParameter(gp);

    // Add the requirements.
    auto source = FloatingRequirementSource::forAbstract();
    for (auto req : requirements)
      builder.addRequirement(req, source, nullptr);

    // If there were any errors, the signature was invalid.
    if (builder.Impl->HadAnyError) {
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_valid,
                             sig->getAsString());
    }

    // Form a generic signature from the result.
    auto newSig =
      std::move(builder).computeGenericSignature(
                                      /*allowConcreteGenericParams=*/true);

    // The new signature should be equal.
    if (!newSig->isEqual(sig)) {
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_equal,
                             sig->getAsString(), newSig->getAsString());
    }
  }

  // Try removing each requirement in turn.
  for (unsigned victimIndex : indices(requirements)) {
    PrettyStackTraceGenericSignature debugStack("verifying", sig, victimIndex);

    // Form a new generic signature builder.
    GenericSignatureBuilder builder(context);

    // Add the generic parameters.
    for (auto gp : genericParams)
      builder.addGenericParameter(gp);

    // Add the requirements *except* the victim.
    auto source = FloatingRequirementSource::forAbstract();
    for (unsigned i : indices(requirements)) {
      if (i != victimIndex)
        builder.addRequirement(requirements[i], source, nullptr);
    }

    // If there were any errors, we formed an invalid signature, so
    // just continue.
    if (builder.Impl->HadAnyError) continue;

    // Form a generic signature from the result.
    auto newSig =
      std::move(builder).computeGenericSignature(
                                      /*allowConcreteGenericParams=*/true);

    // If the removed requirement is satisfied by the new generic signature,
    // it is redundant. Complain.
    if (newSig->isRequirementSatisfied(requirements[victimIndex])) {
      SmallString<32> reqString;
      {
        llvm::raw_svector_ostream out(reqString);
        requirements[victimIndex].print(out, PrintOptions());
      }
      context.Diags.diagnose(SourceLoc(), diag::generic_signature_not_minimal,
                             reqString, sig->getAsString());
    }
  }
}

void GenericSignatureBuilder::verifyGenericSignaturesInModule(
                                                        ModuleDecl *module) {
  LoadedFile *loadedFile = nullptr;
  for (auto fileUnit : module->getFiles()) {
    loadedFile = dyn_cast<LoadedFile>(fileUnit);
    if (loadedFile) break;
  }

  if (!loadedFile) return;

  // Check all of the (canonical) generic signatures.
  SmallVector<GenericSignature, 8> allGenericSignatures;
  SmallPtrSet<CanGenericSignature, 4> knownGenericSignatures;
  (void)loadedFile->getAllGenericSignatures(allGenericSignatures);
  ASTContext &context = module->getASTContext();
  for (auto genericSig : allGenericSignatures) {
    // Check whether this is the first time we've checked this (canonical)
    // signature.
    auto canGenericSig = genericSig.getCanonicalSignature();
    if (!knownGenericSignatures.insert(canGenericSig).second) continue;

    verifyGenericSignature(context, canGenericSig);
  }
}
      
/// Check whether the inputs to the \c AbstractGenericSignatureRequest are
/// all canonical.
static bool isCanonicalRequest(GenericSignature baseSignature,
                               ArrayRef<GenericTypeParamType *> genericParams,
                               ArrayRef<Requirement> requirements) {
  if (baseSignature && !baseSignature->isCanonical())
    return false;

  for (auto gp : genericParams) {
    if (!gp->isCanonical())
      return false;
  }

  for (const auto &req : requirements) {
    if (!req.isCanonical())
      return false;
  }

  return true;
}

GenericSignature
AbstractGenericSignatureRequest::evaluate(
         Evaluator &evaluator,
         const GenericSignatureImpl *baseSignature,
         SmallVector<GenericTypeParamType *, 2> addedParameters,
         SmallVector<Requirement, 2> addedRequirements) const {
  // If nothing is added to the base signature, just return the base
  // signature.
  if (addedParameters.empty() && addedRequirements.empty())
    return baseSignature;

  ASTContext &ctx = addedParameters.empty()
      ? addedRequirements.front().getFirstType()->getASTContext()
      : addedParameters.front()->getASTContext();

  // If there are no added requirements, we can form the signature directly
  // with the added parameters.
  if (addedRequirements.empty()) {
    ArrayRef<Requirement> requirements;
    if (baseSignature) {
      addedParameters.insert(addedParameters.begin(),
                             baseSignature->getGenericParams().begin(),
                             baseSignature->getGenericParams().end());
      requirements = baseSignature->getRequirements();
    }

    return GenericSignature::get(addedParameters, requirements);
  }

  // If the request is non-canonical, we won't need to build our own
  // generic signature builder.
  if (!isCanonicalRequest(baseSignature, addedParameters, addedRequirements)) {
    // Canonicalize the inputs so we can form the canonical request.
    GenericSignature canBaseSignature;
    if (baseSignature)
      canBaseSignature = baseSignature->getCanonicalSignature();

    SmallVector<GenericTypeParamType *, 2> canAddedParameters;
    canAddedParameters.reserve(addedParameters.size());
    for (auto gp : addedParameters) {
      auto canGP = gp->getCanonicalType()->castTo<GenericTypeParamType>();
      canAddedParameters.push_back(canGP);
    }

    SmallVector<Requirement, 2> canAddedRequirements;
    canAddedRequirements.reserve(addedRequirements.size());
    for (const auto &req : addedRequirements) {
      canAddedRequirements.push_back(req.getCanonical());
    }

    // Build the canonical signature.
    auto canSignatureResult = evaluator(
        AbstractGenericSignatureRequest{
          canBaseSignature.getPointer(), std::move(canAddedParameters),
          std::move(canAddedRequirements)});
    if (!canSignatureResult || !*canSignatureResult)
      return GenericSignature();

    // Substitute in the original generic parameters to form the sugared
    // result the original request wanted.
    auto canSignature = *canSignatureResult;
    SmallVector<GenericTypeParamType *, 2> resugaredParameters;
    resugaredParameters.reserve(canSignature->getGenericParams().size());
    if (baseSignature) {
      resugaredParameters.append(baseSignature->getGenericParams().begin(),
                                 baseSignature->getGenericParams().end());
    }
    resugaredParameters.append(addedParameters.begin(), addedParameters.end());
    assert(resugaredParameters.size() ==
               canSignature->getGenericParams().size());

    SmallVector<Requirement, 2> resugaredRequirements;
    resugaredRequirements.reserve(canSignature->getRequirements().size());
    for (const auto &req : canSignature->getRequirements()) {
      auto resugaredReq = req.subst(
          [&](SubstitutableType *type) {
            if (auto gp = dyn_cast<GenericTypeParamType>(type)) {
              unsigned ordinal = canSignature->getGenericParamOrdinal(gp);
              return Type(resugaredParameters[ordinal]);
            }
            return Type(type);
          },
          MakeAbstractConformanceForGenericType(),
          SubstFlags::AllowLoweredTypes);
      resugaredRequirements.push_back(*resugaredReq);
    }

    return GenericSignature::get(resugaredParameters, resugaredRequirements);
  }

  // Create a generic signature that will form the signature.
  GenericSignatureBuilder builder(ctx);
  if (baseSignature)
    builder.addGenericSignature(baseSignature);

  auto source =
    GenericSignatureBuilder::FloatingRequirementSource::forAbstract();

  for (auto param : addedParameters)
    builder.addGenericParameter(param);

  for (const auto &req : addedRequirements)
    builder.addRequirement(req, source, nullptr);

  return std::move(builder).computeGenericSignature(
      /*allowConcreteGenericParams=*/true);
}

GenericSignature
InferredGenericSignatureRequest::evaluate(
        Evaluator &evaluator, ModuleDecl *parentModule,
        const GenericSignatureImpl *parentSig,
        GenericParamSource paramSource,
        SmallVector<Requirement, 2> addedRequirements,
        SmallVector<TypeLoc, 2> inferenceSources,
        bool allowConcreteGenericParams) const {
      
  GenericSignatureBuilder builder(parentModule->getASTContext());
      
  // If there is a parent context, add the generic parameters and requirements
  // from that context.
  builder.addGenericSignature(parentSig);

  DeclContext *lookupDC = nullptr;

  const auto visitRequirement = [&](const Requirement &req,
                                    RequirementRepr *reqRepr) {
    const auto source = FloatingRequirementSource::forExplicit(
      reqRepr->getSeparatorLoc());

    // If we're extending a protocol and adding a redundant requirement,
    // for example, `extension Foo where Self: Foo`, then emit a
    // diagnostic.

    if (auto decl = lookupDC->getAsDecl()) {
      if (auto extDecl = dyn_cast<ExtensionDecl>(decl)) {
        auto extType = extDecl->getDeclaredInterfaceType();
        auto extSelfType = extDecl->getSelfInterfaceType();
        auto reqLHSType = req.getFirstType();
        auto reqRHSType = req.getSecondType();

        if (extType->isExistentialType() &&
            reqLHSType->isEqual(extSelfType) &&
            reqRHSType->isEqual(extType)) {

          auto &ctx = extDecl->getASTContext();
          ctx.Diags.diagnose(extDecl->getLoc(),
                             diag::protocol_extension_redundant_requirement,
                             extType->getString(),
                             extSelfType->getString(),
                             reqRHSType->getString());
        }
      }
    }

    builder.addRequirement(req, reqRepr, source, nullptr,
                            lookupDC->getParentModule());
    return false;
  };

  GenericParamList *genericParams = nullptr;
  if (auto params = paramSource.dyn_cast<GenericParamList *>())
      genericParams = params;
  else
      genericParams = paramSource.get<GenericContext *>()->getGenericParams();

  if (genericParams) {
    // Extensions never have a parent signature.
    if (genericParams->getOuterParameters())
      assert(parentSig == nullptr);

    // Type check the generic parameters, treating all generic type
    // parameters as dependent, unresolved.
    SmallVector<GenericParamList *, 2> gpLists;
    for (auto *outerParams = genericParams;
         outerParams != nullptr;
         outerParams = outerParams->getOuterParameters()) {
      gpLists.push_back(outerParams);
    }

    // The generic parameter lists MUST appear from innermost to outermost.
    // We walk them backwards to order outer requirements before
    // inner requirements.
    for (auto &genericParams : llvm::reverse(gpLists)) {
      assert(genericParams->size() > 0 &&
             "Parsed an empty generic parameter list?");

      // First, add the generic parameters to the generic signature builder.
      // Do this before checking the inheritance clause, since it may
      // itself be dependent on one of these parameters.
      for (const auto param : *genericParams)
        builder.addGenericParameter(param);

      // Add the requirements for each of the generic parameters to the builder.
      // Now, check the inheritance clauses of each parameter.
      for (const auto param : *genericParams)
        builder.addGenericParameterRequirements(param);

      // Determine where and how to perform name lookup.
      lookupDC = genericParams->begin()[0]->getDeclContext();

      // Add the requirements clause to the builder.
      WhereClauseOwner(lookupDC, genericParams)
        .visitRequirements(TypeResolutionStage::Structural,
                           visitRequirement);
    }
  }

  if (auto *ctx = paramSource.dyn_cast<GenericContext *>()) {
    // The declaration might have a trailing where clause.
    if (auto *where = ctx->getTrailingWhereClause()) {
      // Determine where and how to perform name lookup.
      lookupDC = ctx;

      WhereClauseOwner(lookupDC, where).visitRequirements(
        TypeResolutionStage::Structural, visitRequirement);
    }
  }
      
  /// Perform any remaining requirement inference.
  for (auto sourcePair : inferenceSources) {
    auto *typeRepr = sourcePair.getTypeRepr();
    auto source =
      FloatingRequirementSource::forInferred(
        typeRepr ? typeRepr->getStartLoc() : SourceLoc());

    builder.inferRequirements(*parentModule,
                              sourcePair.getType(),
                              source);
  }
  
  // Finish by adding any remaining requirements.
  auto source =
    FloatingRequirementSource::forInferred(SourceLoc());
      
  for (const auto &req : addedRequirements)
    builder.addRequirement(req, source, parentModule);
  
  return std::move(builder).computeGenericSignature(
      allowConcreteGenericParams);
}
