//===--- Pattern.h - Swift Language Pattern-Matching ASTs -------*- C++ -*-===//
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
// This file defines the Pattern class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PATTERN_H
#define SWIFT_PATTERN_H

#include "swift/Basic/AnyValue.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/type_traits.h"
#include "swift/AST/Decl.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/OptionSet.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {
  class ASTContext;
  class Expr;
  enum class CheckedCastKind : unsigned;
  class TypeExpr;

/// PatternKind - The classification of different kinds of
/// value-matching pattern.
enum class PatternKind : uint8_t {
#define PATTERN(ID, PARENT) ID,
#define LAST_PATTERN(ID) Last_Pattern = ID,
#include "PatternNodes.def"
};
enum : unsigned { NumPatternKindBits =
  countBitsUsed(static_cast<unsigned>(PatternKind::Last_Pattern)) };

/// Diagnostic printing of PatternKinds.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, PatternKind kind);

/// Pattern - Base class for all patterns in Swift.
///
/// Patterns match a value and binds new variables to parts or all of it
/// The patterns contain the reference to the variable declaration that is being
/// assigned to, / or bound, in an assignment.
/// e.g. In `let (a, _) = (1, 2)`, the pattern is the part after the `let` and
/// before the `=`. This pattern is a TuplePattern, which contains a
/// NamedPattern representing the variable declaration of `A` and an AnyPattern.
/// Patterns also appear in switch cases. In this example `case .some(let foo)`,
/// where we unwrap an optional in a switch-case, there is an
/// EnumElementPattern matching the `.some`, a BindingPattern for the `let`,
/// and a `NamedPattern` referencing the variable declaration of `foo`.
class alignas(8) Pattern {
protected:
  union { uint64_t OpaqueBits;

  SWIFT_INLINE_BITFIELD_BASE(Pattern, bitmax(NumPatternKindBits,8)+1+1,
    Kind : bitmax(NumPatternKindBits,8),
    isImplicit : 1,
    hasInterfaceType : 1
  );

  SWIFT_INLINE_BITFIELD_FULL(TuplePattern, Pattern, 32,
    : NumPadBits,
    NumElements : 32
  );

  SWIFT_INLINE_BITFIELD(TypedPattern, Pattern, 1,
    IsPropagatedType : 1
  );

  SWIFT_INLINE_BITFIELD(BoolPattern, Pattern, 1,
    Value : 1
  );

  SWIFT_INLINE_BITFIELD(BindingPattern, Pattern, 1,
    /// True if this is a let pattern, false if a var pattern.
    IsLet : 1
  );

  } Bits;

  Pattern(PatternKind kind) {
    Bits.OpaqueBits = 0;
    Bits.Pattern.Kind = unsigned(kind);
    Bits.Pattern.isImplicit = false;
    Bits.Pattern.hasInterfaceType = false;
  }

private:
  /// The checked type of the pattern.
  ///
  /// if \c Bits.Pattern.hasInterfaceType, this stores the interface type of the
  /// pattern, which will be lazily resolved to the contextual type using
  /// the environment in \c ASTContext::DelayedPatternContexts.
  mutable Type Ty;

public:
  PatternKind getKind() const { return PatternKind(Bits.Pattern.Kind); }

  /// Retrieve the name of the given pattern kind.
  ///
  /// This name should only be used for debugging dumps and other
  /// developer aids, and should never be part of a diagnostic or exposed
  /// to the user of the compiler in any way.
  static StringRef getKindName(PatternKind K);

  /// A pattern is implicit if it is compiler-generated and there
  /// exists no source code for it.
  bool isImplicit() const { return Bits.Pattern.isImplicit; }
  void setImplicit() { Bits.Pattern.isImplicit = true; }

  /// Find the smallest subpattern which obeys the property that matching it is
  /// equivalent to matching this pattern.
  ///
  /// Looks through ParenPattern, BindingPattern, and TypedPattern.
  Pattern *getSemanticsProvidingPattern();
  const Pattern *getSemanticsProvidingPattern() const {
    return const_cast<Pattern*>(this)->getSemanticsProvidingPattern();
  }

  /// Returns whether this pattern has been type-checked yet.
  bool hasType() const { return !Ty.isNull(); }

  /// If this pattern has been type-checked, return the type it
  /// matches.
  Type getType() const;

  /// Set the type of this pattern, given that it was previously not
  /// type-checked.
  void setType(Type ty) {
    assert(!ty || !ty->hasTypeVariable());
    Ty = ty;
  }

  /// Retrieve the delayed interface type of this pattern, if it has one.
  ///
  /// Note: this is used for delayed deserialization logic.
  Type getDelayedInterfaceType() const {
    if (Bits.Pattern.hasInterfaceType) return Ty;
    return nullptr;
  }

  /// Set the type of this pattern as an interface type whose resolution to
  /// a context type will be performed lazily.
  ///
  /// \param dc The context in which the type will be resolved.
  void setDelayedInterfaceType(Type interfaceTy, DeclContext *dc);

  /// Overwrite the type of this pattern.
  void overwriteType(Type ty) { assert(hasType()); Ty = ty; }

  /// Returns the name directly bound by this pattern, or the null
  /// identifier if the pattern does not bind a name directly.
  Identifier getBoundName() const;

  /// If this pattern binds a single variable without any
  /// destructuring or conditionalizing, return that variable.
  VarDecl *getSingleVar() const;

  SourceRange getSourceRange() const;
  SourceLoc getStartLoc() const { return getSourceRange().Start; }
  SourceLoc getEndLoc() const { return getSourceRange().End; }
  SourceLoc getLoc() const;

  /// Collect the set of variables referenced in the given pattern.
  void collectVariables(SmallVectorImpl<VarDecl *> &variables) const;

  /// apply the specified function to all variables referenced in this
  /// pattern.
  void forEachVariable(llvm::function_ref<void(VarDecl *)> f) const;

  /// Returns true if \p vd is in the pattern.
  bool containsVarDecl(const VarDecl *inputVD) const {
    bool result = false;
    forEachVariable([&](VarDecl *vd) { result |= inputVD == vd; });
    return result;
  }

  /// apply the specified function to all pattern nodes recursively in
  /// this pattern.  This is a pre-order traversal.
  void forEachNode(llvm::function_ref<void(Pattern *)> f);

  void forEachNode(llvm::function_ref<void(const Pattern *)> f) const {
    llvm::function_ref<void(Pattern *)> f2 = f;
    const_cast<Pattern *>(this)->forEachNode(f2);
  }

  /// Return true if this pattern (or a subpattern) is refutable.
  bool isRefutablePattern() const;

  bool isNeverDefaultInitializable() const;

  /// Mark all vardecls in this pattern as having an owning statement for
  /// the pattern.
  void markOwnedByStatement(Stmt *S) {
    forEachVariable([&](VarDecl *VD) {
      VD->setParentPatternStmt(S);
    });
  }

  /// Does this binding declare something that requires storage?
  bool hasStorage() const;

  /// Does this pattern have any mutable 'var' bindings?
  bool hasAnyMutableBindings() const;

  static bool classof(const Pattern *P) { return true; }

  //*** Allocation Routines ************************************************/

  void *operator new(size_t bytes, const ASTContext &C);

  // Make placement new and vanilla new/delete illegal for Patterns.
  void *operator new(size_t bytes) = delete;
  void operator delete(void *data) = delete;
  void *operator new(size_t bytes, void *data) = delete;

  void print(llvm::raw_ostream &OS,
             const PrintOptions &Options = PrintOptions()) const;
  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &OS, unsigned Indent = 0) const;

  /// walk - This recursively walks the AST rooted at this pattern.
  Pattern *walk(ASTWalker &walker);
  Pattern *walk(ASTWalker &&walker) { return walk(walker); }
};

/// A pattern consisting solely of grouping parentheses around a
/// different pattern.
class ParenPattern : public Pattern {
  SourceLoc LPLoc, RPLoc;
  Pattern *SubPattern;
public:
  ParenPattern(SourceLoc lp, Pattern *sub, SourceLoc rp)
    : Pattern(PatternKind::Paren),
      LPLoc(lp), RPLoc(rp), SubPattern(sub) {
    assert(lp.isValid() == rp.isValid());
  }

  static ParenPattern *createImplicit(ASTContext &Context, Pattern *sub) {
    auto *PP = new (Context) ParenPattern(SourceLoc(), sub, SourceLoc());
    PP->setImplicit();
    return PP;
  }

  Pattern *getSubPattern() { return SubPattern; }
  const Pattern *getSubPattern() const { return SubPattern; }
  void setSubPattern(Pattern *p) { SubPattern = p; }

  SourceLoc getLParenLoc() const { return LPLoc; }
  SourceLoc getRParenLoc() const { return RPLoc; }
  SourceRange getSourceRange() const { return SourceRange(LPLoc, RPLoc); }
  SourceLoc getLoc() const { return SubPattern->getLoc(); }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Paren;
  }
};

/// An element of a tuple pattern.
///
/// The fully general form of this is something like:
///    label: (pattern) = initexpr
///
/// The Init and DefArgKind fields are only used in argument lists for
/// functions.  They are not parsed as part of normal pattern grammar.
class TuplePatternElt {
  Identifier Label;
  SourceLoc LabelLoc;
  Pattern *ThePattern;

public:
  TuplePatternElt() = default;
  explicit TuplePatternElt(Pattern *P) : ThePattern(P) {}

  TuplePatternElt(Identifier Label, SourceLoc LabelLoc, Pattern *p)
    : Label(Label), LabelLoc(LabelLoc), ThePattern(p) {}

  Identifier getLabel() const { return Label; }
  SourceLoc getLabelLoc() const { return LabelLoc; }
  void setLabel(Identifier I, SourceLoc Loc) {
    Label = I;
    LabelLoc = Loc;
  }

  Pattern *getPattern() { return ThePattern; }
  const Pattern *getPattern() const {
    return ThePattern;
  }

  void setPattern(Pattern *p) { ThePattern = p; }
};

/// A pattern consisting of a tuple of patterns.
///
/// The tuple pattern really represents parentheses in the position of a pattern
/// around one or more other patterns. This could be another tuple pattern, or
/// a mixed listing of the other pattern types. The other patterns are stored in
/// a list of TuplePatternElt's.
/// e.g. `let (a, _) = (1, 2)`, this tuple pattern contains a NamedPattern and
/// an AnyPattern.
class TuplePattern final : public Pattern,
    private llvm::TrailingObjects<TuplePattern, TuplePatternElt> {
  friend TrailingObjects;
  SourceLoc LPLoc, RPLoc;

  TuplePattern(SourceLoc lp, unsigned numElements, SourceLoc rp)
      : Pattern(PatternKind::Tuple), LPLoc(lp), RPLoc(rp) {
    Bits.TuplePattern.NumElements = numElements;
    assert(lp.isValid() == rp.isValid());
  }

public:
  static TuplePattern *create(ASTContext &C, SourceLoc lp,
                              ArrayRef<TuplePatternElt> elements, SourceLoc rp);

  static TuplePattern *createImplicit(ASTContext &C,
                                      ArrayRef<TuplePatternElt> elements) {
    auto *TP = create(C, SourceLoc(), elements, SourceLoc());
    TP->setImplicit();
    return TP;
  }

  /// Create either a tuple pattern or a paren pattern, depending
  /// on the elements.
  static Pattern *createSimple(ASTContext &C, SourceLoc lp,
                               ArrayRef<TuplePatternElt> elements,
                               SourceLoc rp);

  unsigned getNumElements() const {
    return Bits.TuplePattern.NumElements;
  }

  MutableArrayRef<TuplePatternElt> getElements() {
    return {getTrailingObjects<TuplePatternElt>(), getNumElements()};
  }
  ArrayRef<TuplePatternElt> getElements() const {
    return {getTrailingObjects<TuplePatternElt>(), getNumElements()};
  }

  const TuplePatternElt &getElement(unsigned i) const {return getElements()[i];}
  TuplePatternElt &getElement(unsigned i) { return getElements()[i]; }

  SourceLoc getLParenLoc() const { return LPLoc; }
  SourceLoc getRParenLoc() const { return RPLoc; }
  SourceRange getSourceRange() const;

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Tuple;
  }
};

/// A pattern which binds a name to an arbitrary value of its type.
///
/// This represents the name of a single variable.
/// e.g `var a: Int?` includes the NamedPattern `a`, as well as the TypedPattern
/// representing `Int?`.
class NamedPattern : public Pattern {
  VarDecl *const Var;

public:
  explicit NamedPattern(VarDecl *Var)
      : Pattern(PatternKind::Named), Var(Var) { }

  static NamedPattern *createImplicit(ASTContext &Ctx, VarDecl *Var) {
    auto *NP = new (Ctx) NamedPattern(Var);
    NP->setImplicit();
    return NP;
  }

  VarDecl *getDecl() const { return Var; }
  Identifier getBoundName() const;
  StringRef getNameStr() const { return Var->getNameStr(); }

  SourceLoc getLoc() const { return Var->getLoc(); }
  SourceRange getSourceRange() const { return Var->getSourceRange(); }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Named;
  }
};

/// A pattern which matches an arbitrary value of a type, but does not
/// bind a name to it.  This is spelled "_". A `default:` case label will also
/// implicitly match an AnyPattern.
class AnyPattern : public Pattern {
  SourceLoc Loc;

public:
  explicit AnyPattern(SourceLoc Loc)
      : Pattern(PatternKind::Any), Loc(Loc) { }

  static AnyPattern *createImplicit(ASTContext &Context) {
    auto *AP = new (Context) AnyPattern(SourceLoc());
    AP->setImplicit();
    return AP;
  }

  SourceLoc getLoc() const { return Loc; }
  SourceRange getSourceRange() const { return Loc; }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Any;
  }
};

/// A pattern which matches a sub-pattern and annotates it with a
/// type. It is a compile-time error if the pattern does not statically match
/// a value of the type. This is different from IsPattern, which is a refutable
/// dynamic type match.
class TypedPattern : public Pattern {
  Pattern *SubPattern;
  TypeRepr *PatTypeRepr;

public:
  /// Creates a new TypedPattern which annotates the provided sub-pattern with
  /// the provided TypeRepr. If 'implicit' is true, the pattern will be
  /// set to implicit. If false, it will not. If 'implicit' is not provided,
  /// then the pattern will be set to 'implicit' if there is a provided TypeRepr
  /// which has a valid SourceRange.
  TypedPattern(Pattern *pattern, TypeRepr *tr);

  /// Creates an implicit typed pattern annotating the provided sub-pattern
  /// with a given type.
  static TypedPattern *
  createImplicit(ASTContext &ctx, Pattern *pattern, Type type) {
    auto tp = new (ctx) TypedPattern(pattern, /*typeRepr*/nullptr);
    if (!type.isNull())
      tp->setType(type);
    tp->setImplicit();
    return tp;
  }

  /// True if the type in this \c TypedPattern was propagated from a different
  /// \c TypedPattern.
  ///
  /// For example, in:
  /// \code
  ///   var a, b: Int, c, d: Double
  /// \endcode
  /// 'a' and 'c' will have this bit set to true.
  bool isPropagatedType() const {
    return Bits.TypedPattern.IsPropagatedType;
  }
  void setPropagatedType() {
    Bits.TypedPattern.IsPropagatedType = true;
  }

  Pattern *getSubPattern() { return SubPattern; }
  const Pattern *getSubPattern() const { return SubPattern; }
  void setSubPattern(Pattern *p) { SubPattern = p; }

  TypeRepr *getTypeRepr() const { return PatTypeRepr; }

  SourceLoc getLoc() const;
  SourceRange getSourceRange() const;

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Typed;
  }
};

/// A pattern which performs a dynamic type check. The match succeeds if the
/// class, archetype, or existential value is dynamically of the given type.
///
/// TODO: Introduce type refinement of the value being matched.
class IsPattern : public Pattern {
  SourceLoc IsLoc;

  Pattern *SubPattern;

  /// The semantics of the type check (class downcast, archetype-to-concrete,
  /// etc.)
  CheckedCastKind CastKind;

  /// The type being checked for.
  TypeExpr *CastType;

public:
  IsPattern(SourceLoc IsLoc, TypeExpr *CastTy, Pattern *SubPattern,
            CheckedCastKind Kind);

  static IsPattern *createImplicit(ASTContext &Ctx, Type castTy,
                                   Pattern *SubPattern, CheckedCastKind Kind);

  CheckedCastKind getCastKind() const { return CastKind; }
  void setCastKind(CheckedCastKind kind) { CastKind = kind; }

  bool hasSubPattern() const { return SubPattern; }
  Pattern *getSubPattern() { return SubPattern; }
  const Pattern *getSubPattern() const { return SubPattern; }
  void setSubPattern(Pattern *p) { SubPattern = p; }

  SourceLoc getLoc() const { return IsLoc; }
  SourceRange getSourceRange() const;

  void setCastType(Type castTy);
  Type getCastType() const;
  TypeRepr *getCastTypeRepr() const;

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Is;
  }
};

/// A pattern that matches an enum case. If the enum value is in the matching
/// case, then the value is extracted. If there is a subpattern, it is then
/// matched against the associated value for the case.
class EnumElementPattern : public Pattern {
  TypeExpr *ParentType;
  SourceLoc DotLoc;
  DeclNameLoc NameLoc;
  DeclNameRef Name;
  PointerUnion<EnumElementDecl *, Expr*> ElementDeclOrUnresolvedOriginalExpr;
  Pattern /*nullable*/ *SubPattern;

public:
  EnumElementPattern(TypeExpr *ParentType, SourceLoc DotLoc,
                     DeclNameLoc NameLoc, DeclNameRef Name,
                     EnumElementDecl *Element, Pattern *SubPattern)
      : Pattern(PatternKind::EnumElement), ParentType(ParentType),
        DotLoc(DotLoc), NameLoc(NameLoc), Name(Name),
        ElementDeclOrUnresolvedOriginalExpr(Element), SubPattern(SubPattern) {
    assert(ParentType && "Missing parent type?");
  }

  /// Create an unresolved EnumElementPattern for a `.foo` pattern relying on
  /// contextual type.
  EnumElementPattern(SourceLoc DotLoc, DeclNameLoc NameLoc, DeclNameRef Name,
                     Pattern *SubPattern, Expr *UnresolvedOriginalExpr)
      : Pattern(PatternKind::EnumElement), ParentType(nullptr), DotLoc(DotLoc),
        NameLoc(NameLoc), Name(Name),
        ElementDeclOrUnresolvedOriginalExpr(UnresolvedOriginalExpr),
        SubPattern(SubPattern) {}

  bool hasSubPattern() const { return SubPattern; }

  const Pattern *getSubPattern() const {
    return SubPattern;
  }

  Pattern *getSubPattern() {
    return SubPattern;
  }

  void setSubPattern(Pattern *p) { SubPattern = p; }

  DeclNameRef getName() const { return Name; }

  EnumElementDecl *getElementDecl() const {
    return ElementDeclOrUnresolvedOriginalExpr.dyn_cast<EnumElementDecl*>();
  }
  void setElementDecl(EnumElementDecl *d) {
    ElementDeclOrUnresolvedOriginalExpr = d;
  }

  Expr *getUnresolvedOriginalExpr() const {
    return ElementDeclOrUnresolvedOriginalExpr.get<Expr*>();
  }
  bool hasUnresolvedOriginalExpr() const {
    return ElementDeclOrUnresolvedOriginalExpr.is<Expr*>();
  }
  void setUnresolvedOriginalExpr(Expr *e) {
    ElementDeclOrUnresolvedOriginalExpr = e;
  }

  DeclNameLoc getNameLoc() const { return NameLoc; }
  SourceLoc getLoc() const { return NameLoc.getBaseNameLoc(); }
  SourceLoc getStartLoc() const;
  SourceLoc getEndLoc() const;
  SourceRange getSourceRange() const { return {getStartLoc(), getEndLoc()}; }

  TypeRepr *getParentTypeRepr() const;

  void setParentType(Type ty);
  Type getParentType() const;

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::EnumElement;
  }
};

/// A pattern that matches an enum case. If the enum value is in the matching
/// case, then the value is extracted. If there is a subpattern, it is then
/// matched against the associated value for the case.
class BoolPattern : public Pattern {
  SourceLoc NameLoc;

public:
  BoolPattern(SourceLoc NameLoc, bool Value)
      : Pattern(PatternKind::Bool), NameLoc(NameLoc) {
    Bits.BoolPattern.Value = Value;
  }

  bool getValue() const { return Bits.BoolPattern.Value; }
  void setValue(bool v) { Bits.BoolPattern.Value = v; }

  SourceLoc getNameLoc() const { return NameLoc; }
  SourceLoc getLoc() const { return NameLoc; }
  SourceLoc getStartLoc() const {
    return NameLoc;
  }
  SourceLoc getEndLoc() const {
    return NameLoc;
  }
  SourceRange getSourceRange() const { return {getStartLoc(), getEndLoc()}; }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Bool;
  }
};

/// A pattern "x?" which matches ".Some(x)".
class OptionalSomePattern : public Pattern {
  Pattern *SubPattern;
  SourceLoc QuestionLoc;
  EnumElementDecl *ElementDecl = nullptr;

public:
  explicit OptionalSomePattern(Pattern *SubPattern,
                               SourceLoc QuestionLoc)
  : Pattern(PatternKind::OptionalSome), SubPattern(SubPattern),
    QuestionLoc(QuestionLoc) { }

  SourceLoc getQuestionLoc() const { return QuestionLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(SubPattern->getStartLoc(), QuestionLoc);
  }

  const Pattern *getSubPattern() const { return SubPattern; }
  Pattern *getSubPattern() { return SubPattern; }
  void setSubPattern(Pattern *p) { SubPattern = p; }

  EnumElementDecl *getElementDecl() const { return ElementDecl; }
  void setElementDecl(EnumElementDecl *d) { ElementDecl = d; }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::OptionalSome;
  }
};

/// A pattern which matches a value obtained by evaluating an expression.
/// The match will be tested using user-defined '~=' operator function lookup;
/// the match succeeds if 'patternValue ~= matchedValue' produces a true value.
class ExprPattern : public Pattern {
  llvm::PointerIntPair<Expr *, 1, bool> SubExprAndIsResolved;

  /// An expression constructed during type-checking that produces a call to the
  /// '~=' operator comparing the match expression on the left to the matched
  /// value on the right.
  Expr *MatchExpr;

  /// An implicit variable used to represent the RHS value of the match.
  VarDecl *MatchVar;

public:
  /// Construct an ExprPattern.
  ExprPattern(Expr *e, bool isResolved, Expr *matchExpr, VarDecl *matchVar);

  /// Construct an unresolved ExprPattern.
  ExprPattern(Expr *e)
    : ExprPattern(e, false, nullptr, nullptr)
  {}

  /// Construct a resolved ExprPattern.
  ExprPattern(Expr *e, Expr *matchExpr, VarDecl *matchVar)
    : ExprPattern(e, true, matchExpr, matchVar)
  {}

  Expr *getSubExpr() const { return SubExprAndIsResolved.getPointer(); }
  void setSubExpr(Expr *e) { SubExprAndIsResolved.setPointer(e); }

  Expr *getMatchExpr() const { return MatchExpr; }
  void setMatchExpr(Expr *e) {
    assert(isResolved() && "cannot set match fn for unresolved expr patter");
    MatchExpr = e;
  }

  VarDecl *getMatchVar() const { return MatchVar; }
  void setMatchVar(VarDecl *v) {
    assert(isResolved() && "cannot set match var for unresolved expr patter");
    MatchVar = v;
  }

  SourceLoc getLoc() const;
  SourceRange getSourceRange() const;

  /// True if pattern resolution has been applied to the subexpression.
  bool isResolved() const { return SubExprAndIsResolved.getInt(); }
  void setResolved(bool isResolved) { SubExprAndIsResolved.setInt(isResolved); }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Expr;
  }
};

/// A pattern which introduces variable bindings. This pattern node has no
/// semantics of its own, but has a syntactic effect on the subpattern. Bare
/// identifiers in the subpattern create new variable bindings instead of being
/// parsed as expressions referencing existing entities.
class BindingPattern : public Pattern {
  SourceLoc VarLoc;
  Pattern *SubPattern;
public:
  BindingPattern(SourceLoc loc, bool isLet, Pattern *sub)
      : Pattern(PatternKind::Binding), VarLoc(loc), SubPattern(sub) {
    Bits.BindingPattern.IsLet = isLet;
  }

  static BindingPattern *createImplicit(ASTContext &Ctx, bool isLet,
                                        Pattern *sub) {
    auto *VP = new (Ctx) BindingPattern(SourceLoc(), isLet, sub);
    VP->setImplicit();
    return VP;
  }

  bool isLet() const { return Bits.BindingPattern.IsLet; }

  SourceLoc getLoc() const { return VarLoc; }
  SourceRange getSourceRange() const {
    SourceLoc EndLoc = SubPattern->getSourceRange().End;
    if (EndLoc.isInvalid())
      return VarLoc;
    return {VarLoc, EndLoc};
  }

  const Pattern *getSubPattern() const { return SubPattern; }
  Pattern *getSubPattern() { return SubPattern; }
  void setSubPattern(Pattern *p) { SubPattern = p; }

  static bool classof(const Pattern *P) {
    return P->getKind() == PatternKind::Binding;
  }
};

inline Pattern *Pattern::getSemanticsProvidingPattern() {
  if (auto *pp = dyn_cast<ParenPattern>(this))
    return pp->getSubPattern()->getSemanticsProvidingPattern();
  if (auto *tp = dyn_cast<TypedPattern>(this))
    return tp->getSubPattern()->getSemanticsProvidingPattern();
  if (auto *vp = dyn_cast<BindingPattern>(this))
    return vp->getSubPattern()->getSemanticsProvidingPattern();
  return this;
}

/// Describes a pattern and the context in which it occurs.
class ContextualPattern {
  /// The pattern and whether this is the top-level pattern.
  llvm::PointerIntPair<Pattern *, 1, bool> patternAndTopLevel;

  /// Either the declaration context or the enclosing pattern binding
  /// declaration.
  llvm::PointerUnion<PatternBindingDecl *, DeclContext *> declOrContext;

  /// Index into the pattern binding declaration, when there is one.
  unsigned index = 0;

  ContextualPattern(
      Pattern *pattern, bool topLevel,
      llvm::PointerUnion<PatternBindingDecl *, DeclContext *> declOrContext,
      unsigned index
    ) : patternAndTopLevel(pattern, topLevel),
        declOrContext(declOrContext),
        index(index) { }

public:
  /// Produce a contextual pattern for a pattern binding declaration entry.
  static ContextualPattern forPatternBindingDecl(
     PatternBindingDecl *pbd, unsigned index);

  /// Produce a contextual pattern for a raw pattern that always allows
  /// inference.
  static ContextualPattern forRawPattern(Pattern *pattern, DeclContext *dc) {
    return ContextualPattern(pattern, /*topLevel=*/true, dc, /*index=*/0);
  }

  /// Retrieve a contextual pattern for the given subpattern.
  ContextualPattern forSubPattern(
      Pattern *subpattern, bool retainTopLevel) const {
    return ContextualPattern(
        subpattern, isTopLevel() && retainTopLevel, declOrContext, index);
  }

  /// Retrieve the pattern.
  Pattern *getPattern() const {
    return patternAndTopLevel.getPointer();
  }

  /// Whether this is the top-level pattern in this context.
  bool isTopLevel() const {
    return patternAndTopLevel.getInt();
  }

  /// Retrieve the declaration context of the pattern.
  DeclContext *getDeclContext() const;

  /// Retrieve the pattern binding declaration that owns this pattern, if
  /// there is one.
  PatternBindingDecl *getPatternBindingDecl() const;

  /// Retrieve the index into the pattern binding declaration for the top-level
  /// pattern.
  unsigned getPatternBindingIndex() const {
    assert(getPatternBindingDecl() != nullptr);
    return index;
  }

  /// Whether this pattern allows type inference, e.g., from an initializer
  /// expression.
  bool allowsInference() const;

  friend llvm::hash_code hash_value(const ContextualPattern &pattern) {
    return llvm::hash_combine(pattern.getPattern(),
                              pattern.isTopLevel(),
                              pattern.declOrContext);
  }

  friend bool operator==(const ContextualPattern &lhs,
                         const ContextualPattern &rhs) {
    return lhs.patternAndTopLevel == rhs.patternAndTopLevel &&
        lhs.declOrContext == rhs.declOrContext;
  }

  friend bool operator!=(const ContextualPattern &lhs,
                         const ContextualPattern &rhs) {
    return !(lhs == rhs);
  }
};

void simple_display(llvm::raw_ostream &out, const ContextualPattern &pattern);

} // end namespace swift

#endif
