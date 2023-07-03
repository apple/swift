//===--- RewriteLoop.h - Identities between rewrite rules -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REWRITELOOP_H
#define SWIFT_REWRITELOOP_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include "Symbol.h"
#include "Term.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class RewriteSystem;
struct RewritePathEvaluator;


/// Records an evaluation step in a rewrite path.
struct RewriteStep {
  enum StepKind : unsigned {
    ///
    /// *** Rewrite step kinds introduced by Knuth-Bendix completion ***
    ///

    /// Apply a rewrite rule to the term at the top of the primary stack.
    ///
    /// Formally, this is a whiskered, oriented rewrite rule. For example,
    /// given a rule (X => Y) and the term A.X.B, the application at
    /// offset 1 yields A.Y.B.
    ///
    /// This can be represented as A.(X => Y).B.
    ///
    /// Similarly, going in the other direction, if we start from A.Y.B
    /// and apply the inverse rule, we get A.(Y => X).B.
    ///
    /// The StartOffset field encodes the offset where to apply the rule.
    ///
    /// The Arg field encodes the rule to apply.
    Rule,

    /// The term at the top of the primary stack must be a term ending with a
    /// superclass or concrete type symbol.
    ///
    /// If not inverted: prepend the prefix to each substitution.
    ///
    /// If inverted: strip the prefix from each substitution.
    ///
    /// The StartOffset field encodes the length of the prefix.
    PrefixSubstitutions,

    ///
    /// *** Rewrite step kinds introduced by simplifySubstitutions() ***
    ///

    /// Move a term from the primary stack to the secondary stack (if not
    /// inverted) or the secondary stack to primary stack (if inverted).
    Shift,

    /// If not inverted: the top of the primary stack must be a term ending
    /// with a superclass or concrete type symbol:
    ///
    ///    T.[concrete: C<...> with <X1, X2...>]
    ///
    /// Each concrete substitution Xn is pushed onto the primary stack,
    /// producing:
    ///
    ///    T.[concrete: C<...> with <X1, X2...>] X1 X2...
    ///
    /// If inverted: pop concrete substitutions Xn from the primary stack,
    /// which must follow a term ending with a superclass or concrete type
    /// symbol:
    ///
    ///    T.[concrete: C<...> with <X1, X2...>] X1 X2...
    ///
    /// The Arg field encodes the number of substitutions.
    ///
    /// Used by RewriteSystem::simplifyLeftHandSideSubstitutions().
    Decompose,

    ///
    /// *** Rewrite step kinds introduced by the property map ***
    ///

    /// If not inverted: the top of the primary stack must be a term T.[p1].[p2]
    /// ending in a pair of property symbols [p1] and [p2], where [p1] < [p2].
    /// The symbol [p2] is dropped, leaving behind the term T.[p1].
    ///
    /// If inverted: the top of the primary stack must be a term T.[p1]
    /// ending in a property symbol [p1]. The rewrite system must have a
    /// recorded relation for the pair ([p1], [p2]). The symbol [p2] is added
    /// to the end of the term, leaving behind the term T.[p1].[p2].
    ///
    /// The Arg field stores the result of calling
    /// RewriteSystem::recordRelation().
    Relation,

    /// A generalization of `Decompose` that can replace structural components
    /// of the type with concrete types, using a TypeDifference that has been
    /// computed previously.
    ///
    /// The Arg field is a TypeDifference ID, returned from
    /// RewriteSystem::registerTypeDifference().
    ///
    /// Say the TypeDifference LHS is [concrete: C<...> with <X1, X2...>], and
    /// say the TypeDifference RHS is [concrete: C'<...> with <X', X2', ...>].
    ///
    /// Note that the LHS and RHS may have a different number of substitutions.
    ///
    /// If not inverted: the top of the primary stack must be a term ending
    /// with the RHS of the TypeDifference:
    ///
    ///    T.[concrete: C'<...> with <X1', X2'...>]
    ///
    /// First, the symbol at the end of the term is replaced by the LHS of the
    /// TypeDifference:
    ///
    ///    T.[concrete: C<...> with <X1, X2...>]
    ///
    /// Then, each substitution of the LHS is pushed on the primary stack, with
    /// the transforms of the TypeDifference applied:
    ///
    /// - If (n, f(Xn)) appears in TypeDifference::SameTypes, then we push
    ///   f(Xn).
    /// - If (n, [concrete: D]) appears in TypeDifference::ConcreteTypes, then
    ///   we push Xn.[concrete: D].
    /// - Otherwise, we push Xn.
    ///
    /// This gives you something like:
    ///
    ///    T.[concrete: C<...> with <X1, X2, X3...>] X1 f(X2) X3.[concrete: D]
    ///
    /// If inverted: the above is performed in reverse, leaving behind the
    /// term ending with the TypeDifference RHS at the top of the primary stack:
    ///
    ///    T.[concrete: C'<...> with <X1', X2'...>]
    ///
    /// Used by RewriteSystem::simplifyLeftHandSideSubstitutions().
    DecomposeConcrete,

    /// For decomposing the left hand side of an induced rule in concrete type
    /// unification, using a TypeDifference that has been computed previously.
    ///
    /// The Arg field is a TypeDifference ID together with a substitution index
    /// of the TypeDifference LHS which identifies the induced rule.
    ///
    /// Say the TypeDifference LHS is [concrete: C<...> with <X1, X2...>], and
    /// say the TypeDifference RHS is [concrete: C'<...> with <X', X2', ...>].
    ///
    /// Note that the LHS and RHS may have a different number of substitutions.
    ///
    /// Furthermore, let T be the base term of the TypeDifference, meaning that
    /// the TypeDifference was derived from a pair of concrete type rules
    /// (T.[LHS] => T) and (T.[RHS] => T).
    ///
    /// If not inverted: the top of the primary stack must be the term Xn,
    /// where n is the substitution index of the type difference.
    ///
    /// Then, the term T.[LHS] is pushed on the primary stack.
    ///
    /// If inverted: the top of the primary stack must be T.[LHS], which is
    /// popped. The next term must be the term Xn.
    ///
    /// Used by buildRewritePathForInducedRule() in PropertyMap.cpp.
    LeftConcreteProjection,

    /// For introducing the right hand side of an induced rule in concrete type
    /// unification, using a TypeDifference that has been computed previously.
    ///
    /// If not inverted: the top of the primary stack must be the term f(Xn),
    /// where n is the substitution index of the type difference. There are
    /// three cases:
    ///
    /// - The substitution index appears in the SameTypes list of the
    ///   TypeDifference. In this case, f(Xn) is the right hand side of the
    ///   entry in the SameTypes list.
    ///
    /// - The substitution index appears in the ConcreteTypes list of the
    ///   TypeDifference. In this case, f(Xn) is Xn.[concrete: D] where D
    ///   is the right hand side of the entry in the ConcreteTypes list.
    ///
    /// - The substitution index does not appear in either list, in which case
    ///   it is unchanged and f(Xn) == Xn.
    ///
    /// The term f(Xn) is replaced with the original substitution Xn at the
    /// top of the primary stack.
    ///
    /// Then, the term T.[RHS] is pushed on the primary stack.
    ///
    /// If inverted: the top of the primary stack must be T.[RHS], which is
    /// popped. The next term must be the term f(Xn), which is replaced with
    /// Xn.
    ///
    /// Used by buildRewritePathForInducedRule() in PropertyMap.cpp.
    RightConcreteProjection
  };

  /// The rewrite step kind.
  StepKind Kind : 4;

  /// If false, the step replaces an occurrence of the rule's left hand side
  /// with the right hand side. If true, vice versa.
  unsigned Inverse : 1;

  /// The size of the left whisker, which is the position within the term where
  /// the rule is being applied. In A.(X => Y).B, this is |A|=1.
  unsigned StartOffset : 13;

  /// The size of the right whisker, which is the length of the remaining suffix
  /// after the rule is applied. In A.(X => Y).B, this is |B|=1.
  unsigned EndOffset : 13;

  /// If Kind is Rule, the index of the rule in the rewrite system.
  ///
  /// If Kind is PrefixSubstitutions, the length of the prefix to add or remove
  /// at the beginning of each concrete substitution.
  ///
  /// If Kind is Decompose, the number of substitutions to push or pop.
  ///
  /// If Kind is Relation, the relation index returned from
  /// RewriteSystem::recordRelation().
  ///
  /// If Kind is DecomposeConcrete, the type difference ID returned from
  /// RewriteSystem::recordTypeDifference().
  ///
  /// If Kind is LeftConcreteProjection or RightConcreteProjection, the
  /// type difference returned from RewriteSystem::recordTypeDifference()
  /// in the most significant 16 bits, together with the substitution index
  /// in the least significant 16 bits. See getConcreteProjectionArg(),
  /// getTypeDifference() and getSubstitutionIndex().
  unsigned Arg;

  RewriteStep(StepKind kind, unsigned startOffset, unsigned endOffset,
              unsigned arg, bool inverse) {
    Kind = kind;

    StartOffset = startOffset;
    assert(StartOffset == startOffset && "Overflow");
    EndOffset = endOffset;
    assert(EndOffset == endOffset && "Overflow");
    Arg = arg;
    assert(Arg == arg && "Overflow");
    Inverse = inverse;
  }

  static RewriteStep forRewriteRule(unsigned startOffset, unsigned endOffset,
                                    unsigned ruleID, bool inverse) {
    return RewriteStep(Rule, startOffset, endOffset, ruleID, inverse);
  }

  static RewriteStep forPrefixSubstitutions(unsigned length, unsigned endOffset,
                                            bool inverse) {
    return RewriteStep(PrefixSubstitutions, /*startOffset=*/0, endOffset,
                       /*arg=*/length, inverse);
  }

  static RewriteStep forShift(bool inverse) {
    return RewriteStep(Shift, /*startOffset=*/0, /*endOffset=*/0,
                       /*arg=*/0, inverse);
  }

  static RewriteStep forDecompose(unsigned numSubstitutions, bool inverse) {
    return RewriteStep(Decompose, /*startOffset=*/0, /*endOffset=*/0,
                       /*arg=*/numSubstitutions, inverse);
  }

  static RewriteStep forRelation(unsigned startOffset, unsigned relationID,
                                 bool inverse) {
    return RewriteStep(Relation, startOffset, /*endOffset=*/0,
                       /*arg=*/relationID, inverse);
  }

  static RewriteStep forDecomposeConcrete(unsigned differenceID, bool inverse) {
    return RewriteStep(DecomposeConcrete, /*startOffset=*/0, /*endOffset=*/0,
                       /*arg=*/differenceID, inverse);
  }

  static RewriteStep forLeftConcreteProjection(unsigned differenceID,
                                               unsigned substitutionIndex,
                                               bool inverse) {
    unsigned arg = getConcreteProjectionArg(differenceID, substitutionIndex);
    return RewriteStep(LeftConcreteProjection,
                       /*startOffset=*/0, /*endOffset=*/0,
                       arg, inverse);
  }

  static RewriteStep forRightConcreteProjection(unsigned differenceID,
                                                unsigned substitutionIndex,
                                                bool inverse) {
    unsigned arg = getConcreteProjectionArg(differenceID, substitutionIndex);
    return RewriteStep(RightConcreteProjection,
                       /*startOffset=*/0, /*endOffset=*/0,
                       arg, inverse);
  }

  bool isInContext() const {
    return StartOffset > 0 || EndOffset > 0;
  }

  bool pushesTermsOnStack() const {
    switch (Kind) {
    case RewriteStep::Rule:
    case RewriteStep::PrefixSubstitutions:
    case RewriteStep::Relation:
    case RewriteStep::Shift:
      return false;

    case RewriteStep::Decompose:
    case RewriteStep::DecomposeConcrete:
    case RewriteStep::LeftConcreteProjection:
    case RewriteStep::RightConcreteProjection:
      return true;
    }

    llvm_unreachable("Bad step kind");
  }

  void invert() {
    Inverse = !Inverse;
  }

  unsigned getRuleID() const {
    assert(Kind == RewriteStep::Rule);
    return Arg;
  }

  unsigned getTypeDifferenceID() const {
    assert(Kind == RewriteStep::LeftConcreteProjection ||
           Kind == RewriteStep::RightConcreteProjection);
    return (Arg >> 16) & 0xffff;
  }

  unsigned getSubstitutionIndex() const {
    assert(Kind == RewriteStep::LeftConcreteProjection ||
           Kind == RewriteStep::RightConcreteProjection);
    return Arg & 0xffff;
  }

  void dump(llvm::raw_ostream &out,
            RewritePathEvaluator &evaluator,
            const RewriteSystem &system) const;

  bool isInverseOf(const RewriteStep &other) const;

  bool maybeSwapRewriteSteps(RewriteStep &other,
                             const RewriteSystem &system);

private:
  static unsigned getConcreteProjectionArg(unsigned differenceID,
                                           unsigned substitutionIndex) {
    assert(differenceID <= 0xffff);
    assert(substitutionIndex <= 0xffff);

    return (differenceID << 16) | substitutionIndex;
  }
};

/// Records a sequence of zero or more rewrite rules applied to a term.
class RewritePath {
  llvm::SmallVector<RewriteStep, 3> Steps;

public:
  bool empty() const {
    return Steps.empty();
  }

  unsigned size() const {
    return Steps.size();
  }

  void add(RewriteStep step) {
    Steps.push_back(step);
  }

  // Horizontal composition of paths.
  void append(const RewritePath &other) {
    Steps.append(other.begin(), other.end());
  }

  void resize(unsigned newSize) {
    assert(newSize <= size());
    Steps.erase(Steps.begin() + newSize, Steps.end());
  }

  decltype(Steps)::const_iterator begin() const {
    return Steps.begin();
  }

  decltype(Steps)::const_iterator end() const {
    return Steps.end();
  }

  RewritePath splitCycleAtRule(unsigned ruleID) const;

  bool replaceRulesWithPaths(llvm::function_ref<const RewritePath *(unsigned)> fn);

  bool replaceRuleWithPath(unsigned ruleID, const RewritePath &path);

  llvm::SmallVector<unsigned, 1>
  findRulesAppearingOnceInEmptyContext(const MutableTerm &term,
                                       const RewriteSystem &system) const;

  void invert();

  bool computeFreelyReducedForm();

  bool computeCyclicallyReducedForm(MutableTerm &basepoint,
                                    const RewriteSystem &system);

  bool computeLeftCanonicalForm(const RewriteSystem &system);

  bool computeNormalForm(const RewriteSystem &system);

  void dump(llvm::raw_ostream &out,
            MutableTerm term,
            const RewriteSystem &system) const;

  void dumpLong(llvm::raw_ostream &out,
                MutableTerm term,
                const RewriteSystem &system) const;
};

/// A loop (3-cell) that rewrites the basepoint back to the basepoint.
class RewriteLoop {
public:
  MutableTerm Basepoint;
  RewritePath Path;

private:
  /// Cached value for findRulesAppearingOnceInEmptyContext().
  llvm::SmallVector<unsigned, 1> RulesInEmptyContext;

  /// Cached value for getProjectionCount().
  unsigned ProjectionCount : 15;

  /// Cached value for getDecomposeCount().
  unsigned DecomposeCount : 15;

  /// Cached value for hasConcreteTypeAliasRule().
  unsigned HasConcreteTypeAliasRule : 1;

  /// A useful loop contains at least one rule in empty context, even if that
  /// rule appears multiple times or also in non-empty context. The only loops
  /// that are elimination candidates contain a rule in empty context *exactly
  /// once*. A useful loop can become an elimination candidate after
  /// normalization.
  unsigned Useful : 1;

  /// Loops are deleted once they are no longer useful, as defined above.
  unsigned Deleted : 1;

  /// If true, Useful, RulesInEmptyContext, ProjectionCount, and DecomposeCount
  /// should be recomputed.
  unsigned Dirty : 1;

  void recompute(const RewriteSystem &system);

public:
  RewriteLoop(MutableTerm basepoint, RewritePath path)
    : Basepoint(basepoint), Path(path) {
    ProjectionCount = 0;
    DecomposeCount = 0;
    HasConcreteTypeAliasRule = 0;
    Useful = 0;
    Deleted = 0;

    // Initially, cached values are not valid because they have not been
    // computed yet.
    Dirty = 1;
  }

  bool isDeleted() const {
    return Deleted;
  }

  void markDeleted() {
    assert(!Deleted);
    Deleted = 1;
  }

  /// This must be called after changing 'Path'.
  void markDirty() {
    Dirty = 1;
  }

  bool isUseful(const RewriteSystem &system) const;

  llvm::ArrayRef<unsigned>
  findRulesAppearingOnceInEmptyContext(const RewriteSystem &system) const;

  unsigned getProjectionCount(const RewriteSystem &system) const;

  unsigned getDecomposeCount(const RewriteSystem &system) const;

  bool hasConcreteTypeAliasRule(const RewriteSystem &system) const;

  void computeNormalForm(const RewriteSystem &system);

  void verify(const RewriteSystem &system) const;

  void dump(llvm::raw_ostream &out, const RewriteSystem &system) const;
};

/// Return value of RewritePathEvaluator::applyRewriteRule();
struct AppliedRewriteStep {
  Term lhs;
  Term rhs;
  MutableTerm prefix;
  MutableTerm suffix;
};

/// A rewrite path is a list of instructions for a two-stack interpreter.
///
/// - Shift moves a term from the primary stack to the secondary stack
///   (if not inverted) or secondary to primary (if inverted).
///
/// - Decompose splits off the substitutions from a superclass or concrete type
///   symbol at the top of the primary stack (if not inverted) or assembles a
///   new superclass or concrete type symbol at the top of the primary stack
///   (if inverted).
///
/// - All other rewrite step kinds manipulate the term at the top of the primary
///   stack.
///
struct RewritePathEvaluator {
  /// The primary stack. Most rewrite steps operate on the top of this stack.
  llvm::SmallVector<MutableTerm, 2> Primary;

  /// The secondary stack. The 'Shift' rewrite step moves terms between the
  /// primary and secondary stacks.
  llvm::SmallVector<MutableTerm, 2> Secondary;

  explicit RewritePathEvaluator(const MutableTerm &term) {
    Primary.push_back(term);
  }

  void checkPrimary() const;
  void checkSecondary() const;

  MutableTerm &getCurrentTerm();

  /// We're "in context" if we're in the middle of rewriting concrete
  /// substitutions.
  bool isInContext() const {
    assert(Primary.size() > 0);
    return (Primary.size() > 1 || Secondary.size() > 0);
  }

  void apply(const RewriteStep &step,
             const RewriteSystem &system);

  AppliedRewriteStep applyRewriteRule(const RewriteStep &step,
                                      const RewriteSystem &system);

  std::pair<MutableTerm, MutableTerm>
  applyPrefixSubstitutions(const RewriteStep &step,
                           const RewriteSystem &system);

  void applyShift(const RewriteStep &step,
                  const RewriteSystem &system);

  void applyDecompose(const RewriteStep &step,
                      const RewriteSystem &system);

  AppliedRewriteStep
  applyRelation(const RewriteStep &step,
                const RewriteSystem &system);

  void applyDecomposeConcrete(const RewriteStep &step,
                              const RewriteSystem &system);

  void applyLeftConcreteProjection(const RewriteStep &step,
                                   const RewriteSystem &system);

  void applyRightConcreteProjection(const RewriteStep &step,
                                    const RewriteSystem &system);

  void dump(llvm::raw_ostream &out) const;
};

} // end namespace rewriting

} // end namespace swift

#endif
