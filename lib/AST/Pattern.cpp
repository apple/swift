//===--- Pattern.cpp - Swift Language Pattern-Matching ASTs ---------------===//
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
//  This file implements the Pattern class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Pattern.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

#define PATTERN(Id, _) \
  static_assert(IsTriviallyDestructible<Id##Pattern>::value, \
                "Patterns are BumpPtrAllocated; the d'tor is never called");
#include "swift/AST/PatternNodes.def"

DescriptivePatternKind Pattern::getDescriptiveKind() const {
#define TRIVIAL_PATTERN_KIND(Kind)                                             \
  case PatternKind::Kind:                                                      \
    return DescriptivePatternKind::Kind

  switch (getKind()) {
    TRIVIAL_PATTERN_KIND(Paren);
    TRIVIAL_PATTERN_KIND(Tuple);
    TRIVIAL_PATTERN_KIND(Named);
    TRIVIAL_PATTERN_KIND(Any);
    TRIVIAL_PATTERN_KIND(Typed);
    TRIVIAL_PATTERN_KIND(Is);
    TRIVIAL_PATTERN_KIND(EnumElement);
    TRIVIAL_PATTERN_KIND(OptionalSome);
    TRIVIAL_PATTERN_KIND(Bool);
    TRIVIAL_PATTERN_KIND(Expr);

  case PatternKind::Binding:
    return cast<BindingPattern>(this)->isLet() ? DescriptivePatternKind::Let
                                               : DescriptivePatternKind::Var;
  }
#undef TRIVIAL_PATTERN_KIND
  llvm_unreachable("bad DescriptivePatternKind");
}

StringRef Pattern::getKindName(PatternKind K) {
  switch (K) {
#define PATTERN(Id, Parent) case PatternKind::Id: return #Id;
#include "swift/AST/PatternNodes.def"
  }
  llvm_unreachable("bad PatternKind");
}

StringRef Pattern::getDescriptivePatternKindName(DescriptivePatternKind K) {
#define ENTRY(Kind, String)                                                    \
  case DescriptivePatternKind::Kind:                                           \
    return String
  switch (K) {
    ENTRY(Paren, "parenthesized pattern");
    ENTRY(Tuple, "tuple pattern");
    ENTRY(Named, "pattern variable binding");
    ENTRY(Any, "'_' pattern");
    ENTRY(Typed, "pattern type annotation");
    ENTRY(Is, "prefix 'is' pattern");
    ENTRY(EnumElement, "enum case matching pattern");
    ENTRY(OptionalSome, "optional pattern");
    ENTRY(Bool, "bool matching pattern");
    ENTRY(Expr, "expression pattern");
    ENTRY(Var, "'var' binding pattern");
    ENTRY(Let, "'let' binding pattern");
  }
#undef ENTRY
  llvm_unreachable("bad DescriptivePatternKind");
}

// Metaprogram to verify that every concrete class implements
// a 'static bool classof(const Pattern*)'.
template <bool fn(const Pattern*)> struct CheckClassOfPattern {
  static const bool IsImplemented = true;
};
template <> struct CheckClassOfPattern<Pattern::classof> {
  static const bool IsImplemented = false;
};

#define PATTERN(ID, PARENT) \
static_assert(CheckClassOfPattern<ID##Pattern::classof>::IsImplemented, \
              #ID "Pattern is missing classof(const Pattern*)");
#include "swift/AST/PatternNodes.def"

// Metaprogram to verify that every concrete class implements
// 'SourceRange getSourceRange()'.
typedef const char (&TwoChars)[2];
template<typename Class> 
inline char checkSourceRangeType(SourceRange (Class::*)() const);
inline TwoChars checkSourceRangeType(SourceRange (Pattern::*)() const);

/// getSourceRange - Return the full source range of the pattern.
SourceRange Pattern::getSourceRange() const {
  switch (getKind()) {
#define PATTERN(ID, PARENT) \
case PatternKind::ID: \
static_assert(sizeof(checkSourceRangeType(&ID##Pattern::getSourceRange)) == 1, \
              #ID "Pattern is missing getSourceRange()"); \
return cast<ID##Pattern>(this)->getSourceRange();
#include "swift/AST/PatternNodes.def"
  }
  
  llvm_unreachable("pattern type not handled!");
}

void Pattern::setDelayedInterfaceType(Type interfaceTy, DeclContext *dc) {
  assert(interfaceTy->hasTypeParameter() && "Not an interface type");
  Ty = interfaceTy;
  ASTContext &ctx = interfaceTy->getASTContext();
  ctx.DelayedPatternContexts[this] = dc;
  Bits.Pattern.hasInterfaceType = true;
}

Type Pattern::getType() const {
  assert(hasType());

  // If this pattern has an interface type, map it into the context type.
  if (Bits.Pattern.hasInterfaceType) {
    ASTContext &ctx = Ty->getASTContext();

    // Retrieve the generic environment to use for the mapping.
    auto found = ctx.DelayedPatternContexts.find(this);
    assert(found != ctx.DelayedPatternContexts.end());
    auto dc = found->second;

    if (auto genericEnv = dc->getGenericEnvironmentOfContext()) {
      ctx.DelayedPatternContexts.erase(this);
      Ty = genericEnv->mapTypeIntoContext(Ty);
      const_cast<Pattern*>(this)->Bits.Pattern.hasInterfaceType = false;
    }
  }

  return Ty;
}

/// getLoc - Return the caret location of the pattern.
SourceLoc Pattern::getLoc() const {
  switch (getKind()) {
#define PATTERN(ID, PARENT) \
  case PatternKind::ID: \
    if (&Pattern::getLoc != &ID##Pattern::getLoc) \
      return cast<ID##Pattern>(this)->getLoc(); \
    break;
#include "swift/AST/PatternNodes.def"
  }

  return getStartLoc();
}

void Pattern::collectVariables(SmallVectorImpl<VarDecl *> &variables) const {
  forEachVariable([&](VarDecl *VD) { variables.push_back(VD); });
}

VarDecl *Pattern::getSingleVar() const {
  auto pattern = getSemanticsProvidingPattern();
  if (auto named = dyn_cast<NamedPattern>(pattern))
    return named->getDecl();

  return nullptr;
}

namespace {
  class WalkToVarDecls : public ASTWalker {
    const std::function<void(VarDecl*)> &fn;
  public:
    
    WalkToVarDecls(const std::function<void(VarDecl*)> &fn)
    : fn(fn) {}

    /// Walk everything that's available; there shouldn't be macro expansions
    /// that matter anyway.
    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PostWalkResult<Pattern *> walkToPatternPost(Pattern *P) override {
      // Handle vars.
      if (auto *Named = dyn_cast<NamedPattern>(P))
        fn(Named->getDecl());
      return Action::Continue(P);
    }

    // Only walk into an expression insofar as it doesn't open a new scope -
    // that is, don't walk into a closure body.
    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (isa<ClosureExpr>(E)) {
        return Action::SkipChildren(E);
      }
      return Action::Continue(E);
    }

    // Don't walk into anything else.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      return Action::SkipChildren(S);
    }
    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::SkipChildren();
    }
    PreWalkAction walkToParameterListPre(ParameterList *PL) override {
      return Action::SkipChildren();
    }
    PreWalkAction walkToDeclPre(Decl *D) override {
      return Action::SkipChildren();
    }
  };
} // end anonymous namespace


/// apply the specified function to all variables referenced in this
/// pattern.
void Pattern::forEachVariable(llvm::function_ref<void(VarDecl *)> fn) const {
  switch (getKind()) {
  case PatternKind::Any:
  case PatternKind::Bool:
    return;

  case PatternKind::Is:
    if (auto SP = cast<IsPattern>(this)->getSubPattern())
      SP->forEachVariable(fn);
    return;

  case PatternKind::Named:
    fn(cast<NamedPattern>(this)->getDecl());
    return;

  case PatternKind::Paren:
  case PatternKind::Typed:
  case PatternKind::Binding:
    return getSemanticsProvidingPattern()->forEachVariable(fn);

  case PatternKind::Tuple:
    for (auto elt : cast<TuplePattern>(this)->getElements())
      elt.getPattern()->forEachVariable(fn);
    return;

  case PatternKind::EnumElement:
    if (auto SP = cast<EnumElementPattern>(this)->getSubPattern())
      SP->forEachVariable(fn);
    return;

    case PatternKind::OptionalSome:
    cast<OptionalSomePattern>(this)->getSubPattern()->forEachVariable(fn);
    return;

  case PatternKind::Expr:
    // An ExprPattern only exists before sema has resolved a refutable pattern
    // into a concrete pattern.  We have to use an AST Walker to find the
    // VarDecls buried down inside of it.
    const_cast<Pattern*>(this)->walk(WalkToVarDecls(fn));
    return;
  }
}

/// apply the specified function to all pattern nodes recursively in
/// this pattern.  This is a pre-order traversal.
void Pattern::forEachNode(llvm::function_ref<void(Pattern*)> f) {
  f(this);

  switch (getKind()) {
  // Leaf patterns have no recursion.
  case PatternKind::Any:
  case PatternKind::Named:
  case PatternKind::Expr:// FIXME: expr nodes are not modeled right in general.
  case PatternKind::Bool:
    return;

  case PatternKind::Is:
    if (auto SP = cast<IsPattern>(this)->getSubPattern())
      SP->forEachNode(f);
    return;

  case PatternKind::Paren:
    return cast<ParenPattern>(this)->getSubPattern()->forEachNode(f);
  case PatternKind::Typed:
    return cast<TypedPattern>(this)->getSubPattern()->forEachNode(f);
  case PatternKind::Binding:
    return cast<BindingPattern>(this)->getSubPattern()->forEachNode(f);

  case PatternKind::Tuple:
    for (auto elt : cast<TuplePattern>(this)->getElements())
      elt.getPattern()->forEachNode(f);
    return;

  case PatternKind::EnumElement: {
    auto *OP = cast<EnumElementPattern>(this);
    if (OP->hasSubPattern())
      OP->getSubPattern()->forEachNode(f);
    return;
  }
  case PatternKind::OptionalSome:
    cast<OptionalSomePattern>(this)->getSubPattern()->forEachNode(f);
    return;
  }
}

bool Pattern::hasStorage() const {
  bool HasStorage = false;
  forEachVariable([&](VarDecl *VD) {
    if (VD->hasStorage())
      HasStorage = true;
  });

  return HasStorage;
}

bool Pattern::hasAnyMutableBindings() const {
  auto HasMutable = false;
  forEachVariable([&](VarDecl *VD) {
    if (!VD->isLet())
      HasMutable = true;
  });
  return HasMutable;
}

OptionalSomePattern *OptionalSomePattern::create(ASTContext &ctx,
                                                 Pattern *subPattern,
                                                 SourceLoc questionLoc) {
  return new (ctx) OptionalSomePattern(ctx, subPattern, questionLoc);
}

OptionalSomePattern *
OptionalSomePattern::createImplicit(ASTContext &ctx, Pattern *subPattern,
                                    SourceLoc questionLoc) {
  auto *P = OptionalSomePattern::create(ctx, subPattern, questionLoc);
  P->setImplicit();
  return P;
}

EnumElementDecl *OptionalSomePattern::getElementDecl() const {
  return Ctx.getOptionalSomeDecl();
}

/// Return true if this is a non-resolved ExprPattern which is syntactically
/// irrefutable.
static bool isIrrefutableExprPattern(const ExprPattern *EP) {
  // If the pattern is resolved, it must be irrefutable.
  if (EP->isResolved()) return false;

  auto expr = EP->getSubExpr();
  while (true) {
    // Drill into parens.
    if (auto parens = dyn_cast<ParenExpr>(expr)) {
      expr = parens->getSubExpr();
      continue;
    }

      // A '_' is an untranslated AnyPattern.
    if (isa<DiscardAssignmentExpr>(expr))
      return true;

    // Everything else is non-exhaustive.
    return false;
  }
}

/// Return true if this pattern (or a subpattern) is refutable.
bool Pattern::isRefutablePattern() const {
  bool foundRefutablePattern = false;
  const_cast<Pattern*>(this)->forEachNode([&](Pattern *Node) {

    // If this is an always matching 'is' pattern, then it isn't refutable.
    if (auto *is = dyn_cast<IsPattern>(Node))
      if (is->getCastKind() == CheckedCastKind::Coercion ||
          is->getCastKind() == CheckedCastKind::BridgingCoercion)
        return;

    // If this is an ExprPattern that isn't resolved yet, do some simple
    // syntactic checks.
    // FIXME: This is unsound, since type checking will turn other more
    // complicated patterns into non-refutable forms.
    if (auto *ep = dyn_cast<ExprPattern>(Node))
      if (isIrrefutableExprPattern(ep))
        return;

    switch (Node->getKind()) {
#define PATTERN(ID, PARENT) case PatternKind::ID: break;
#define REFUTABLE_PATTERN(ID, PARENT) \
case PatternKind::ID: foundRefutablePattern = true; break;
#include "swift/AST/PatternNodes.def"
    }
  });
    
  return foundRefutablePattern;
}

/// Find the name directly bound by this pattern.  When used as a
/// tuple element in a function signature, such names become part of
/// the type.
Identifier Pattern::getBoundName() const {
  if (auto *NP = dyn_cast<NamedPattern>(getSemanticsProvidingPattern()))
    return NP->getBoundName();
  return Identifier();
}

Identifier NamedPattern::getBoundName() const {
  return Var->getName();
}


/// Allocate a new pattern that matches a tuple.
TuplePattern *TuplePattern::create(ASTContext &C, SourceLoc lp,
                                   ArrayRef<TuplePatternElt> elts,
                                   SourceLoc rp) {
#ifndef NDEBUG
  if (elts.size() == 1)
    assert(!elts[0].getLabel().empty());
#endif

  unsigned n = elts.size();
  void *buffer = C.Allocate(totalSizeToAlloc<TuplePatternElt>(n),
                            alignof(TuplePattern));
  TuplePattern *pattern = ::new (buffer) TuplePattern(lp, n, rp);
  std::uninitialized_copy(elts.begin(), elts.end(),
                          pattern->getTrailingObjects<TuplePatternElt>());
  return pattern;
}

Pattern *TuplePattern::createSimple(ASTContext &C, SourceLoc lp,
                                    ArrayRef<TuplePatternElt> elements,
                                    SourceLoc rp) {
  assert(lp.isValid() == rp.isValid());

  if (elements.size() == 1 &&
      elements[0].getLabel().empty()) {
    auto &first = const_cast<TuplePatternElt&>(elements.front());
    return new (C) ParenPattern(lp, first.getPattern(), rp);
  }

  return create(C, lp, elements, rp);
}

SourceRange TuplePattern::getSourceRange() const {
  if (LPLoc.isValid())
    return { LPLoc, RPLoc };
  auto Fields = getElements();
  if (Fields.empty())
    return {};
  return { Fields.front().getPattern()->getStartLoc(),
           Fields.back().getPattern()->getEndLoc() };
}

TypedPattern::TypedPattern(Pattern *pattern, TypeRepr *tr)
  : Pattern(PatternKind::Typed), SubPattern(pattern), PatTypeRepr(tr) {
  Bits.TypedPattern.IsPropagatedType = false;
}

SourceLoc TypedPattern::getLoc() const {
  if (SubPattern->isImplicit() && PatTypeRepr)
    return PatTypeRepr->getSourceRange().Start;

  return SubPattern->getLoc();
}

SourceRange TypedPattern::getSourceRange() const {
  if (isImplicit() || isPropagatedType()) {
    // If a TypedPattern is implicit, then its type is definitely implicit, so
    // we should ignore its location.  On the other hand, the sub-pattern can
    // be explicit or implicit.
    return SubPattern->getSourceRange();
  }

  if (!PatTypeRepr)
    return SourceRange();

  if (SubPattern->isImplicit())
    return PatTypeRepr->getSourceRange();

  return { SubPattern->getSourceRange().Start,
           PatTypeRepr->getSourceRange().End };
}

IsPattern::IsPattern(SourceLoc IsLoc, TypeExpr *CastTy, Pattern *SubPattern,
                     CheckedCastKind Kind)
    : Pattern(PatternKind::Is), IsLoc(IsLoc), SubPattern(SubPattern),
      CastKind(Kind), CastType(CastTy) {
  assert(IsLoc.isValid() == CastTy->getLoc().isValid());
}

IsPattern *IsPattern::createImplicit(ASTContext &Ctx, Type castTy,
                                     Pattern *SubPattern,
                                     CheckedCastKind Kind) {
  assert(castTy);
  auto *CastTE = TypeExpr::createImplicit(castTy, Ctx);
  auto *ip = new (Ctx) IsPattern(SourceLoc(), CastTE, SubPattern, Kind);
  ip->setImplicit();
  return ip;
}

SourceRange IsPattern::getSourceRange() const {
  SourceLoc beginLoc = SubPattern ? SubPattern->getSourceRange().Start : IsLoc;
  SourceLoc endLoc = (isImplicit() ? beginLoc : CastType->getEndLoc());
  return {beginLoc, endLoc};
}

Type IsPattern::getCastType() const { return CastType->getInstanceType(); }
void IsPattern::setCastType(Type type) {
  assert(type);
  CastType->setType(MetatypeType::get(type));
}

TypeRepr *IsPattern::getCastTypeRepr() const { return CastType->getTypeRepr(); }

ExprPattern *ExprPattern::createParsed(ASTContext &ctx, Expr *E,
                                       DeclContext *DC) {
  return new (ctx) ExprPattern(E, DC, /*isResolved*/ false);
}

ExprPattern *ExprPattern::createResolved(ASTContext &ctx, Expr *E,
                                         DeclContext *DC) {
  return new (ctx) ExprPattern(E, DC, /*isResolved*/ true);
}

ExprPattern *ExprPattern::createImplicit(ASTContext &ctx, Expr *E,
                                         DeclContext *DC) {
  auto *EP = ExprPattern::createResolved(ctx, E, DC);
  EP->setImplicit();
  return EP;
}

Expr *ExprPattern::getMatchExpr() const {
  auto &eval = DC->getASTContext().evaluator;
  return evaluateOrDefault(eval, ExprPatternMatchRequest{this}, llvm::None)
      .getMatchExpr();
}

VarDecl *ExprPattern::getMatchVar() const {
  auto &eval = DC->getASTContext().evaluator;
  return evaluateOrDefault(eval, ExprPatternMatchRequest{this}, llvm::None)
      .getMatchVar();
}

SourceLoc EnumElementPattern::getStartLoc() const {
  return (ParentType && !ParentType->isImplicit())
             ? ParentType->getSourceRange().Start
             : DotLoc.isValid() ? DotLoc : NameLoc.getBaseNameLoc();
}

SourceLoc EnumElementPattern::getEndLoc() const {
  if (SubPattern && SubPattern->getSourceRange().isValid()) {
    return SubPattern->getSourceRange().End;
  }
  return NameLoc.getEndLoc();
}

TypeRepr *EnumElementPattern::getParentTypeRepr() const {
  if (!ParentType)
    return nullptr;
  return ParentType->getTypeRepr();
}

Type EnumElementPattern::getParentType() const {
  if (!ParentType)
    return Type();
  return ParentType->getInstanceType();
}

void EnumElementPattern::setParentType(Type type) {
  assert(type);
  if (ParentType) {
    ParentType->setType(MetatypeType::get(type));
  } else {
    ParentType = TypeExpr::createImplicit(type, type->getASTContext());
  }
}

SourceLoc ExprPattern::getLoc() const {
  return getSubExpr()->getLoc();
}

SourceRange ExprPattern::getSourceRange() const {
  return getSubExpr()->getSourceRange();
}

// See swift/Basic/Statistic.h for declaration: this enables tracing Patterns, is
// defined here to avoid too much layering violation / circular linkage
// dependency.

struct PatternTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const Pattern *P = static_cast<const Pattern *>(Entity);
    if (const NamedPattern *NP = dyn_cast<NamedPattern>(P)) {
      OS << NP->getBoundName();
    }
  }
  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const Pattern *P = static_cast<const Pattern *>(Entity);
    P->getSourceRange().print(OS, *SM, false);
  }
};

static PatternTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const Pattern *>() {
  return &TF;
}


ContextualPattern ContextualPattern::forPatternBindingDecl(
    PatternBindingDecl *pbd, unsigned index) {
  return ContextualPattern(
      pbd->getPattern(index), /*isTopLevel=*/true, pbd, index);
}

DeclContext *ContextualPattern::getDeclContext() const {
  if (auto pbd = getPatternBindingDecl())
    return pbd->getDeclContext();

  return declOrContext.get<DeclContext *>();
}

PatternBindingDecl *ContextualPattern::getPatternBindingDecl() const {
  return declOrContext.dyn_cast<PatternBindingDecl *>();
}

bool ContextualPattern::allowsInference() const {
  if (auto pbd = getPatternBindingDecl()) {
    return pbd->isInitialized(index) ||
        pbd->isDefaultInitializableViaPropertyWrapper(index);
  }

  return true;
}

void swift::simple_display(llvm::raw_ostream &out,
                           const ContextualPattern &pattern) {
  simple_display(out, pattern.getPattern());
}

void swift::simple_display(llvm::raw_ostream &out, const Pattern *pattern) {
  out << "(pattern @ " << pattern << ")";
}

SourceLoc swift::extractNearestSourceLoc(const Pattern *pattern) {
  return pattern->getLoc();
}
