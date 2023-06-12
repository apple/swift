//===--- TypeCheckPattern.cpp - Type Checking for Patterns ----------------===//
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
// This file implements semantic analysis for patterns, analyzing a
// pattern tree in both bottom-up and top-down ways.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckType.h"
#include "swift/Basic/StringExtras.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/TypeCheckRequests.h"
#include "llvm/Support/SaveAndRestore.h"
#include <utility>
using namespace swift;

/// If the given VarDecl is a computed property whose getter always returns a
/// particular enum element, return that element.
///
/// This requires the getter's body to have a certain syntactic form. It should
/// be kept in sync with importEnumCaseAlias in the ClangImporter library.
static EnumElementDecl *
extractEnumElement(DeclContext *DC, SourceLoc UseLoc,
                   const VarDecl *constant) {
  ExportContext where = ExportContext::forFunctionBody(DC, UseLoc);
  diagnoseExplicitUnavailability(constant, UseLoc, where, nullptr);

  const FuncDecl *getter = constant->getAccessor(AccessorKind::Get);
  if (!getter)
    return nullptr;

  const BraceStmt *body = getter->getBody();
  if (!body || body->getNumElements() != 1)
    return nullptr;

  auto *retStmtRaw = body->getFirstElement().dyn_cast<Stmt *>();
  auto *retStmt = dyn_cast_or_null<ReturnStmt>(retStmtRaw);
  if (!retStmt)
    return nullptr;

  auto *resultExpr = dyn_cast_or_null<ApplyExpr>(retStmt->getResult());
  if (!resultExpr)
    return nullptr;

  auto *ctorExpr = dyn_cast<DeclRefExpr>(resultExpr->getFn());
  if (!ctorExpr)
    return nullptr;

  // If the declaration we found isn't in the same nominal type as the
  // constant, ignore it.
  if (ctorExpr->getDecl()->getDeclContext()->getSelfNominalTypeDecl() !=
        constant->getDeclContext()->getSelfNominalTypeDecl())
    return nullptr;

  return dyn_cast<EnumElementDecl>(ctorExpr->getDecl());
}

/// Find the first enum element in \p foundElements.
///
/// If there are no enum elements but there are properties, attempts to map
/// an arbitrary property to an enum element using extractEnumElement.
static EnumElementDecl *
filterForEnumElement(DeclContext *DC, SourceLoc UseLoc,
                     bool unqualifiedLookup, LookupResult foundElements) {
  EnumElementDecl *foundElement = nullptr;
  VarDecl *foundConstant = nullptr;

  for (const LookupResultEntry &result : foundElements) {
    ValueDecl *e = result.getValueDecl();
    assert(e);

    // Skip if the enum element was referenced as an instance member
    if (unqualifiedLookup) {
      if (!result.getBaseDecl() ||
          !result.getBaseDecl()->getInterfaceType()->is<MetatypeType>()) {
        continue;
      }
    }

    if (auto *oe = dyn_cast<EnumElementDecl>(e)) {
      // Note that there could be multiple elements with the same
      // name, such results in a re-declaration error, so let's
      // just always pick the last element, just like in `foundConstant`
      // case.
      foundElement = oe;
      continue;
    }

    if (auto *var = dyn_cast<VarDecl>(e)) {
      foundConstant = var;
      continue;
    }
  }

  if (!foundElement && foundConstant && foundConstant->hasClangNode())
    foundElement = extractEnumElement(DC, UseLoc, foundConstant);

  return foundElement;
}

/// Find an unqualified enum element.
static EnumElementDecl *
lookupUnqualifiedEnumMemberElement(DeclContext *DC, DeclNameRef name,
                                   SourceLoc UseLoc) {
  // FIXME: We should probably pay attention to argument labels someday.
  name = name.withoutArgumentLabels();

  auto lookup =
      TypeChecker::lookupUnqualified(DC, name, UseLoc,
                                     defaultUnqualifiedLookupOptions);
  return filterForEnumElement(DC, UseLoc,
                              /*unqualifiedLookup=*/true, lookup);
}

/// Find an enum element in an enum type.
static EnumElementDecl *
lookupEnumMemberElement(DeclContext *DC, Type ty,
                        DeclNameRef name, SourceLoc UseLoc) {
  if (!ty->mayHaveMembers())
    return nullptr;

  // FIXME: We should probably pay attention to argument labels someday.
  name = name.withoutArgumentLabels();

  // Look up the case inside the enum.
  // FIXME: We should be able to tell if this is a private lookup.
  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  LookupResult foundElements =
      TypeChecker::lookupMember(DC, ty, name, UseLoc, lookupOptions);
  return filterForEnumElement(DC, UseLoc,
                              /*unqualifiedLookup=*/false, foundElements);
}

namespace {
/// Build up an \c DeclRefTypeRepr and see what it resolves to.
/// FIXME: Support DeclRefTypeRepr nodes with non-identifier base components.
struct ExprToDeclRefTypeRepr : public ASTVisitor<ExprToDeclRefTypeRepr, bool> {
  SmallVectorImpl<IdentTypeRepr *> &components;
  ASTContext &C;

  ExprToDeclRefTypeRepr(decltype(components) &components, ASTContext &C)
      : components(components), C(C) {}

  bool visitExpr(Expr *e) {
    return false;
  }
  
  bool visitTypeExpr(TypeExpr *te) {
    if (auto *TR = te->getTypeRepr())
      if (auto *ITR = dyn_cast<IdentTypeRepr>(TR)) {
        components.push_back(ITR);
        return true;
      }
    return false;
  }

  bool visitDeclRefExpr(DeclRefExpr *dre) {
    assert(components.empty() && "decl ref should be root element of expr");
    
    // Get the declared type.
    if (auto *td = dyn_cast<TypeDecl>(dre->getDecl())) {
      components.push_back(
        new (C) SimpleIdentTypeRepr(dre->getNameLoc(), td->createNameRef()));
      components.back()->setValue(td, nullptr);
      return true;
    }
    return false;
  }
  
  bool visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *udre) {
    assert(components.empty() && "decl ref should be root element of expr");
    // Track the AST location of the component.
    components.push_back(
      new (C) SimpleIdentTypeRepr(udre->getNameLoc(),
                                  udre->getName()));
    return true;
  }
  
  bool visitUnresolvedDotExpr(UnresolvedDotExpr *ude) {
    if (!visit(ude->getBase()))
      return false;
    
    assert(!components.empty() && "no components before dot expr?!");

    // Track the AST location of the new component.
    components.push_back(
      new (C) SimpleIdentTypeRepr(ude->getNameLoc(),
                                  ude->getName()));
    return true;
  }
  
  bool visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *use) {
    if (!visit(use->getSubExpr()))
      return false;
    
    assert(!components.empty() && "no components before generic args?!");
    
    // Track the AST location of the generic arguments.
    auto origComponent = components.back();
    components.back() =
      GenericIdentTypeRepr::create(C, origComponent->getNameLoc(),
                                   origComponent->getNameRef(),
                                   use->getUnresolvedParams(),
                                   SourceRange(use->getLAngleLoc(),
                                               use->getRAngleLoc()));

    return true;
  }
};
} // end anonymous namespace

namespace {
class ResolvePattern : public ASTVisitor<ResolvePattern,
                                         /*ExprRetTy=*/Pattern*,
                                         /*StmtRetTy=*/void,
                                         /*DeclRetTy=*/void,
                                         /*PatternRetTy=*/Pattern*>
{
public:
  ASTContext &Context;
  DeclContext *DC;

  ResolvePattern(DeclContext *DC) : Context(DC->getASTContext()), DC(DC) {}

  // Convert a subexpression to a pattern if possible, or wrap it in an
  // ExprPattern.
  Pattern *getSubExprPattern(Expr *E) {
    if (Pattern *p = visit(E))
      return p;

    return ExprPattern::createResolved(Context, E, DC);
  }

  /// Turn an argument list into a matching tuple or paren pattern.
  Pattern *composeTupleOrParenPattern(ArgumentList *args) {
    assert(!args->hasAnyInOutArgs());
    if (auto *unary = args->getUnlabeledUnaryExpr()) {
      auto *subPattern = getSubExprPattern(unary);
      return new (Context)
          ParenPattern(args->getLParenLoc(), subPattern, args->getRParenLoc());
    }
    SmallVector<TuplePatternElt, 4> elts;
    for (auto arg : *args) {
      auto *subPattern = getSubExprPattern(arg.getExpr());
      elts.emplace_back(arg.getLabel(), arg.getLabelLoc(), subPattern);
    }
    return TuplePattern::create(Context, args->getLParenLoc(), elts,
                                args->getRParenLoc());
  }

  // Handle productions that are always leaf patterns or are already resolved.
#define ALWAYS_RESOLVED_PATTERN(Id) \
  Pattern *visit##Id##Pattern(Id##Pattern *P) { return P; }
  ALWAYS_RESOLVED_PATTERN(Named)
  ALWAYS_RESOLVED_PATTERN(Any)
  ALWAYS_RESOLVED_PATTERN(Is)
  ALWAYS_RESOLVED_PATTERN(Paren)
  ALWAYS_RESOLVED_PATTERN(Tuple)
  ALWAYS_RESOLVED_PATTERN(EnumElement)
  ALWAYS_RESOLVED_PATTERN(Bool)
#undef ALWAYS_RESOLVED_PATTERN

  Pattern *visitBindingPattern(BindingPattern *P) {
    // Keep track of the fact that we're inside of a var/let pattern.  This
    // affects how unqualified identifiers are processed.
    P->setSubPattern(visit(P->getSubPattern()));
    
    // If the var pattern has no variables bound underneath it, then emit a
    // warning that the var/let is pointless.
    if (!P->isImplicit()) {
      bool HasVariable = false;
      P->forEachVariable([&](VarDecl *VD) { HasVariable = true; });
      
      if (!HasVariable) {
        Context.Diags
            .diagnose(P->getLoc(), diag::var_pattern_didnt_bind_variables,
                      P->getIntroducerStringRef())
            .highlight(P->getSubPattern()->getSourceRange())
            .fixItRemove(P->getLoc());
      }
    }
    
    return P;
  }

  Pattern *visitOptionalSomePattern(OptionalSomePattern *P) {
    P->setSubPattern(visit(P->getSubPattern()));
    return P;
  }

  Pattern *visitTypedPattern(TypedPattern *P) {
    P->setSubPattern(visit(P->getSubPattern()));
    return P;
  }
  
  Pattern *visitExprPattern(ExprPattern *P) {
    if (P->isResolved())
      return P;

    // Try to convert to a pattern.
    Pattern *exprAsPattern = visit(P->getSubExpr());
    // If we failed, keep the ExprPattern as is.
    if (!exprAsPattern) {
      P->setResolved(true);
      return P;
    }
    return exprAsPattern;
  }
  
  // Most exprs remain exprs and should be wrapped in ExprPatterns.
  Pattern *visitExpr(Expr *E) {
    return nullptr;
  }
  
  // Unwrap UnresolvedPatternExprs.
  Pattern *visitUnresolvedPatternExpr(UnresolvedPatternExpr *E) {
    return visit(E->getSubPattern());
  }
  
  // Convert a '_' expression to an AnyPattern.
  Pattern *visitDiscardAssignmentExpr(DiscardAssignmentExpr *E) {
    if (E->isImplicit()) {
      return AnyPattern::createImplicit(Context);
    }
    return new (Context) AnyPattern(E->getLoc());
  }
  
  // Cast expressions 'x as T' get resolved to checked cast patterns.
  // Pattern resolution occurs before sequence resolution, so the cast will
  // appear as a SequenceExpr.
  Pattern *visitSequenceExpr(SequenceExpr *E) {
    if (E->getElements().size() != 3)
      return nullptr;
    auto cast = dyn_cast<CoerceExpr>(E->getElement(1));
    if (!cast)
      return nullptr;

    Pattern *subPattern = getSubExprPattern(E->getElement(0));
    if (cast->isImplicit()) {
      return IsPattern::createImplicit(Context, cast->getCastType(), subPattern,
                                       CheckedCastKind::Unresolved);
    }
    auto *TE = new (Context) TypeExpr(cast->getCastTypeRepr());
    if (auto castTy = cast->getType())
      TE->setType(MetatypeType::get(castTy));
    return new (Context)
        IsPattern(cast->getLoc(), TE, subPattern, CheckedCastKind::Unresolved);
  }
  
  // Convert a paren expr to a pattern if it contains a pattern.
  Pattern *visitParenExpr(ParenExpr *E) {
    Pattern *subPattern = getSubExprPattern(E->getSubExpr());
    return new (Context)
        ParenPattern(E->getLParenLoc(), subPattern, E->getRParenLoc());
  }
  
  // Convert all tuples to patterns.
  Pattern *visitTupleExpr(TupleExpr *E) {
    // Construct a TuplePattern.
    SmallVector<TuplePatternElt, 4> patternElts;

    for (unsigned i = 0, e = E->getNumElements(); i != e; ++i) {
      Pattern *pattern = getSubExprPattern(E->getElement(i));
      patternElts.push_back(TuplePatternElt(E->getElementName(i),
                                            E->getElementNameLoc(i),
                                            pattern));
    }

    return TuplePattern::create(Context, E->getLoc(), patternElts,
                                E->getRParenLoc());
  }

  Pattern *convertBindingsToOptionalSome(Expr *E) {
    auto *expr = E->getSemanticsProvidingExpr();
    auto *bindExpr = dyn_cast<BindOptionalExpr>(expr);
    if (!bindExpr) {
      // Let's see if this expression prefixed with any number of '?'
      // has any other disjoint 'BindOptionalExpr' inside of it, if so,
      // we need to wrap such sub-expression into `OptionalEvaluationExpr`.
      bool hasDisjointChaining = false;
      expr->forEachChildExpr([&](Expr *subExpr) -> Expr * {
        // If there is `OptionalEvaluationExpr` in the AST
        // it means that all of possible `BindOptionalExpr`
        // which follow are covered by it.
        if (isa<OptionalEvaluationExpr>(subExpr))
          return nullptr;

        if (isa<BindOptionalExpr>(subExpr)) {
          hasDisjointChaining = true;
          return nullptr;
        }

        return subExpr;
      });

      if (hasDisjointChaining)
        E = new (Context) OptionalEvaluationExpr(E);

      return getSubExprPattern(E);
    }

    auto *subExpr = convertBindingsToOptionalSome(bindExpr->getSubExpr());
    return OptionalSomePattern::create(Context, subExpr,
                                       bindExpr->getQuestionLoc());
  }

  // Convert a x? to OptionalSome pattern.  In the AST form, this will look like
  // an OptionalEvaluationExpr with an immediate BindOptionalExpr inside of it.
  Pattern *visitOptionalEvaluationExpr(OptionalEvaluationExpr *E) {
    auto *subExpr = E->getSubExpr();
    // We only handle the case where one or more bind expressions are subexprs
    // of the optional evaluation.  Other cases are not simple postfix ?'s.
    if (!isa<BindOptionalExpr>(subExpr->getSemanticsProvidingExpr()))
      return nullptr;

    return convertBindingsToOptionalSome(subExpr);
  }


  // Unresolved member syntax '.Element' forms an EnumElement pattern. The
  // element will be resolved when we type-check the pattern.
  Pattern *visitUnresolvedMemberExpr(UnresolvedMemberExpr *ume) {
    if (ume->getName().getBaseName().isSpecial())
      return nullptr;

    return new (Context) EnumElementPattern(ume->getDotLoc(), ume->getNameLoc(),
                                            ume->getName(), nullptr, ume, DC);
  }
  
  // Member syntax 'T.Element' forms a pattern if 'T' is an enum and the
  // member name is a member of the enum.
  Pattern *visitUnresolvedDotExpr(UnresolvedDotExpr *ude) {
    SmallVector<IdentTypeRepr *, 2> components;
    if (!ExprToDeclRefTypeRepr(components, Context).visit(ude->getBase()))
      return nullptr;

    const auto options =
        TypeResolutionOptions(None) | TypeResolutionFlags::SilenceErrors;

    DeclRefTypeRepr *repr = MemberTypeRepr::create(Context, components);

    // See if the repr resolves to a type.
    const auto ty = TypeResolution::resolveContextualType(
        repr, DC, options,
        [](auto unboundTy) {
          // FIXME: Don't let unbound generic types escape type resolution.
          // For now, just return the unbound generic type.
          return unboundTy;
        },
        // FIXME: Don't let placeholder types escape type resolution.
        // For now, just return the placeholder type.
        PlaceholderType::get,
        /*packElementOpener*/ nullptr);

    auto *enumDecl = dyn_cast_or_null<EnumDecl>(ty->getAnyNominal());
    if (!enumDecl)
      return nullptr;

    EnumElementDecl *referencedElement
      = lookupEnumMemberElement(DC, ty, ude->getName(), ude->getLoc());
    if (!referencedElement)
      return nullptr;

    auto *base =
        TypeExpr::createForMemberDecl(repr, ude->getNameLoc(), enumDecl);
    base->setType(MetatypeType::get(ty));
    return new (Context)
        EnumElementPattern(base, ude->getDotLoc(), ude->getNameLoc(),
                           ude->getName(), referencedElement, nullptr, DC);
  }
  
  // A DeclRef 'E' that refers to an enum element forms an EnumElementPattern.
  Pattern *visitDeclRefExpr(DeclRefExpr *de) {
    auto *elt = dyn_cast<EnumElementDecl>(de->getDecl());
    if (!elt)
      return nullptr;
    
    // Use the type of the enum from context.
    auto enumTy = elt->getParentEnum()->getDeclaredTypeInContext();
    auto *base = TypeExpr::createImplicit(enumTy, Context);

    return new (Context)
        EnumElementPattern(base, SourceLoc(), de->getNameLoc(),
                           elt->createNameRef(), elt, nullptr, DC);
  }
  Pattern *visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *ude) {
    // FIXME: This shouldn't be needed.  It is only necessary because of the
    // poor representation of clang enum aliases and should be removed when
    // rdar://20879992 is addressed.
    //
    // Try looking up an enum element in context.
    if (EnumElementDecl *referencedElement
        = lookupUnqualifiedEnumMemberElement(DC, ude->getName(),
                                             ude->getLoc())) {
      auto *enumDecl = referencedElement->getParentEnum();
      auto enumTy = enumDecl->getDeclaredTypeInContext();
      auto *base = TypeExpr::createImplicit(enumTy, Context);

      return new (Context)
          EnumElementPattern(base, SourceLoc(), ude->getNameLoc(),
                             ude->getName(), referencedElement, nullptr, DC);
    }
      
    
    // Perform unqualified name lookup to find out what the UDRE is.
    return getSubExprPattern(TypeChecker::resolveDeclRefExpr(
        ude, DC, /*replaceInvalidRefsWithErrors=*/true));
  }

  // Call syntax forms a pattern if:
  // - the callee in 'Element(x...)' or '.Element(x...)'
  //   references an enum element. The arguments then form a tuple
  //   pattern matching the element's data.
  // - the callee in 'T(...)' is a struct or class type. The argument tuple is
  //   then required to have keywords for every argument that name properties
  //   of the type.
  Pattern *visitCallExpr(CallExpr *ce) {
    // Specialized call are not allowed anyway.
    // Let it be diagnosed as an expression.
    if (isa<UnresolvedSpecializeExpr>(ce->getFn()))
      return nullptr;

    if (isa<UnresolvedMemberExpr>(ce->getFn())) {
      auto *P = visit(ce->getFn());
      if (!P)
        return nullptr;

      auto *EEP = cast<EnumElementPattern>(P);
      EEP->setSubPattern(composeTupleOrParenPattern(ce->getArgs()));
      EEP->setUnresolvedOriginalExpr(ce);

      return P;
    }

    SmallVector<IdentTypeRepr *, 2> components;
    if (!ExprToDeclRefTypeRepr(components, Context).visit(ce->getFn()))
      return nullptr;
    
    if (components.empty())
      return nullptr;

    auto tailComponent = components.pop_back_val();
    EnumElementDecl *referencedElement = nullptr;
    TypeExpr *baseTE = nullptr;

    if (components.empty()) {
      // Only one component. Try looking up an enum element in context.
      referencedElement
        = lookupUnqualifiedEnumMemberElement(DC, tailComponent->getNameRef(),
                                             tailComponent->getLoc());
      if (!referencedElement)
        return nullptr;

      auto *enumDecl = referencedElement->getParentEnum();
      baseTE = TypeExpr::createImplicit(enumDecl->getDeclaredTypeInContext(),
                                        Context);
    } else {
      const auto options =
          TypeResolutionOptions(None) | TypeResolutionFlags::SilenceErrors;

      // Otherwise, see whether we had an enum type as the penultimate
      // component, and look up an element inside it.
      DeclRefTypeRepr *prefixRepr = MemberTypeRepr::create(Context, components);

      // See first if the entire repr resolves to a type.
      const Type enumTy = TypeResolution::resolveContextualType(
          prefixRepr, DC, options,
          [](auto unboundTy) {
            // FIXME: Don't let unbound generic types escape type
            // resolution. For now, just return the unbound generic type.
            return unboundTy;
          },
          // FIXME: Don't let placeholder types escape type resolution.
          // For now, just return the placeholder type.
          PlaceholderType::get,
          /*packElementOpener*/ nullptr);

      auto *enumDecl = dyn_cast_or_null<EnumDecl>(enumTy->getAnyNominal());
      if (!enumDecl)
        return nullptr;

      referencedElement
        = lookupEnumMemberElement(DC, enumTy,
                                  tailComponent->getNameRef(),
                                  tailComponent->getLoc());
      if (!referencedElement)
        return nullptr;

      baseTE = TypeExpr::createForMemberDecl(
          prefixRepr, tailComponent->getNameLoc(), enumDecl);
      baseTE->setType(MetatypeType::get(enumTy));
    }

    assert(baseTE && baseTE->getType() && "Didn't initialize base expression?");
    assert(!isa<GenericIdentTypeRepr>(tailComponent) &&
           "should be handled above");

    auto *subPattern = composeTupleOrParenPattern(ce->getArgs());
    return new (Context) EnumElementPattern(
        baseTE, SourceLoc(), tailComponent->getNameLoc(),
        tailComponent->getNameRef(), referencedElement, subPattern, DC);
  }
};

} // end anonymous namespace

/// Perform top-down syntactic disambiguation of a pattern. Where ambiguous
/// expr/pattern productions occur (tuples, function calls, etc.), favor the
/// pattern interpretation if it forms a valid pattern; otherwise, leave it as
/// an expression. This does no type-checking except for the bare minimum to
/// disambiguate semantics-dependent pattern forms.
Pattern *TypeChecker::resolvePattern(Pattern *P, DeclContext *DC,
                                     bool isStmtCondition) {
  P = ResolvePattern(DC).visit(P);

  TypeChecker::diagnoseDuplicateBoundVars(P);

  // If the entire pattern is "(pattern_expr (type_expr SomeType))", then this
  // is an invalid pattern.  If it were actually a value comparison (with ~=)
  // then the metatype would have had to be spelled with "SomeType.self".  What
  // they actually meant is to write "is SomeType", so we rewrite it to that
  // pattern for good QoI.
  auto &Context = DC->getASTContext();
  if (auto *EP = dyn_cast<ExprPattern>(P))
    if (auto *TE = dyn_cast<TypeExpr>(EP->getSubExpr())) {
      Context.Diags.diagnose(TE->getStartLoc(), diag::type_pattern_missing_is)
        .fixItInsert(TE->getStartLoc(), "is ");

      P = new (Context)
          IsPattern(TE->getStartLoc(), TE,
                    /*subpattern*/ nullptr, CheckedCastKind::Unresolved);
    }
  
  // Look through a TypedPattern if present.
  auto *InnerP = P;
  if (auto *TP = dyn_cast<TypedPattern>(P))
    InnerP = TP->getSubPattern();

  // If the pattern was valid, check for an implicit BindingPattern on the outer
  // level.  If so, we have an "if let" condition and we want to enforce some
  // more structure on it.
  if (isStmtCondition && isa<BindingPattern>(InnerP) && InnerP->isImplicit()) {
    auto *Body = cast<BindingPattern>(InnerP)->getSubPattern();

    // If they wrote a "x?" pattern, they probably meant "if let x".
    // Check for this and recover nicely if they wrote that.
    if (auto *OSP = dyn_cast<OptionalSomePattern>(Body)) {
      if (!OSP->getSubPattern()->isRefutablePattern()) {
        Context.Diags.diagnose(OSP->getStartLoc(),
                               diag::iflet_implicitly_unwraps)
          .highlight(OSP->getSourceRange())
          .fixItRemove(OSP->getQuestionLoc());
        return P;
      }
    }

    // If the pattern bound is some other refutable pattern, then they
    // probably meant:
    //   if case let <pattern> =
    if (Body->isRefutablePattern()) {
      Context.Diags.diagnose(P->getLoc(), diag::iflet_pattern_matching)
        .fixItInsert(P->getLoc(), "case ");
      return P;
    }

    // "if let" implicitly looks inside of an optional, so wrap it in an
    // OptionalSome pattern.
    P = OptionalSomePattern::createImplicit(Context, P, P->getEndLoc());
  }

  return P;
}

static Type
validateTypedPattern(TypedPattern *TP, DeclContext *dc,
                     TypeResolutionOptions options,
                     OpenUnboundGenericTypeFn unboundTyOpener,
                     HandlePlaceholderTypeReprFn placeholderHandler,
                     OpenPackElementFn packElementOpener) {
  if (TP->hasType()) {
    return TP->getType();
  }

  // If the pattern declares an opaque type, and applies to a single
  // variable binding, then we can bind the opaque return type from the
  // property definition.
  auto &Context = dc->getASTContext();
  auto *Repr = TP->getTypeRepr();
  if (Repr && (Repr->hasOpaque() ||
               (Context.LangOpts.hasFeature(Feature::ImplicitSome) &&
                !collectOpaqueTypeReprs(Repr, Context, dc).empty()))) {
    auto named = dyn_cast<NamedPattern>(
        TP->getSubPattern()->getSemanticsProvidingPattern());
    if (!named) {
      Context.Diags.diagnose(TP->getLoc(),
                             diag::opaque_type_unsupported_pattern);
      return ErrorType::get(Context);
    }

    auto *var = named->getDecl();
    auto opaqueDecl = var->getOpaqueResultTypeDecl();
    if (!opaqueDecl) {
      return ErrorType::get(Context);
    }

    auto opaqueTy = opaqueDecl->getDeclaredInterfaceType();
    if (opaqueTy->hasError()) {
      return ErrorType::get(Context);
    }

    return named->getDecl()->getDeclContext()->mapTypeIntoContext(opaqueTy);
  }

  const auto ty = TypeResolution::resolveContextualType(
      Repr, dc, options, unboundTyOpener, placeholderHandler,
      packElementOpener);

  if (ty->hasError()) {
    return ErrorType::get(Context);
  }

  assert(!dyn_cast_or_null<SpecifierTypeRepr>(Repr) &&
         "Didn't resolve invalid type to error type!");
  return ty;
}

Type TypeChecker::typeCheckPattern(ContextualPattern pattern) {
  DeclContext *dc = pattern.getDeclContext();
  ASTContext &ctx = dc->getASTContext();
  if (auto type = evaluateOrDefault(ctx.evaluator, PatternTypeRequest{pattern},
                                    Type())) {
    return type;
  }

  return ErrorType::get(ctx);
}

/// Apply the contextual pattern's context to the type resolution options.
static TypeResolutionOptions applyContextualPatternOptions(
    TypeResolutionOptions options, ContextualPattern pattern) {
  if (pattern.allowsInference()) {
    options |= TypeResolutionFlags::AllowUnspecifiedTypes;
  }

  return options;
}

ExprPatternMatchResult
ExprPatternMatchRequest::evaluate(Evaluator &evaluator,
                                  const ExprPattern *EP) const {
  assert(EP->isResolved() && "Must only be queried once resolved");

  auto *DC = EP->getDeclContext();
  auto &ctx = DC->getASTContext();

  // Create a 'let' binding to stand in for the RHS value.
  auto *matchVar =
      new (ctx) VarDecl(/*IsStatic*/ false, VarDecl::Introducer::Let,
                        EP->getLoc(), ctx.Id_PatternMatchVar, DC);
  matchVar->setImplicit();

  // Build the 'expr ~= var' expression.
  auto *matchOp = new (ctx) UnresolvedDeclRefExpr(
      DeclNameRef(ctx.Id_MatchOperator), DeclRefKind::BinaryOperator,
      DeclNameLoc(EP->getLoc()));
  matchOp->setImplicit();

  // Note we use getEndLoc here to have the BinaryExpr source range be the same
  // as the expr pattern source range.
  auto *matchVarRef =
      new (ctx) DeclRefExpr(matchVar, DeclNameLoc(EP->getEndLoc()),
                            /*Implicit=*/true);
  auto *matchCall = BinaryExpr::create(ctx, EP->getSubExpr(), matchOp,
                                       matchVarRef, /*implicit*/ true);
  return {matchVar, matchCall};
}

ExprPattern *
EnumElementExprPatternRequest::evaluate(Evaluator &evaluator,
                                        const EnumElementPattern *EEP) const {
  assert(EEP->hasUnresolvedOriginalExpr());
  auto *DC = EEP->getDeclContext();
  return ExprPattern::createResolved(DC->getASTContext(),
                                     EEP->getUnresolvedOriginalExpr(), DC);
}

Type PatternTypeRequest::evaluate(Evaluator &evaluator,
                                  ContextualPattern pattern) const {
  Pattern *P = pattern.getPattern();
  DeclContext *dc = pattern.getDeclContext();

  TypeResolutionOptions options(pattern.getPatternBindingDecl()
                                  ? TypeResolverContext::PatternBindingDecl
                                  : TypeResolverContext::InExpression);
  options = applyContextualPatternOptions(options, pattern);
  if (!pattern.isTopLevel()) {
    options = options.withoutContext();
  }

  auto &Context = dc->getASTContext();
  switch (P->getKind()) {
  // Type-check paren patterns by checking the sub-pattern and
  // propagating that type out.
  case PatternKind::Paren:
  case PatternKind::Binding: {
    Pattern *SP;
    if (auto *PP = dyn_cast<ParenPattern>(P))
      SP = PP->getSubPattern();
    else
      SP = cast<BindingPattern>(P)->getSubPattern();
    Type subType = TypeChecker::typeCheckPattern(
        pattern.forSubPattern(SP, /*retainTopLevel=*/true));
    if (subType->hasError())
      return ErrorType::get(Context);

    auto type = subType;
    if (P->getKind() == PatternKind::Paren)
      type = ParenType::get(Context, type);
    return type;
  }

  // If we see an explicit type annotation, coerce the sub-pattern to
  // that type.
  case PatternKind::Typed: {
    OpenUnboundGenericTypeFn unboundTyOpener = nullptr;
    HandlePlaceholderTypeReprFn placeholderHandler = nullptr;
    OpenPackElementFn packElementOpener = nullptr;
    if (pattern.allowsInference()) {
      unboundTyOpener = [](auto unboundTy) {
        // FIXME: Don't let unbound generic types escape type resolution.
        // For now, just return the unbound generic type.
        return unboundTy;
      };
      // FIXME: Don't let placeholder types escape type resolution.
      // For now, just return the placeholder type.
      placeholderHandler = PlaceholderType::get;
    }
    return validateTypedPattern(cast<TypedPattern>(P), dc, options,
                                unboundTyOpener, placeholderHandler,
                                packElementOpener);
  }

  // A wildcard or name pattern cannot appear by itself in a context
  // which requires an explicit type.
  case PatternKind::Any:
  case PatternKind::Named:
    // If we're type checking this pattern in a context that can provide type
    // information, then the lack of type information is not an error.
    if (options & TypeResolutionFlags::AllowUnspecifiedTypes)
      return Context.TheUnresolvedType;

    Context.Diags.diagnose(P->getLoc(), diag::cannot_infer_type_for_pattern);
    if (auto named = dyn_cast<NamedPattern>(P)) {
      if (auto var = named->getDecl()) {
        var->setInvalid();
      }
    }
    return ErrorType::get(Context);

  // A tuple pattern propagates its tuple-ness out.
  case PatternKind::Tuple: {
    auto tuplePat = cast<TuplePattern>(P);
    bool hadError = false;
    SmallVector<TupleTypeElt, 8> typeElts;

    for (unsigned i = 0, e = tuplePat->getNumElements(); i != e; ++i) {
      TuplePatternElt &elt = tuplePat->getElement(i);
      Type subType = TypeChecker::typeCheckPattern(
          pattern.forSubPattern(elt.getPattern(), /*retainTopLevel=*/false));
      if (subType->hasError())
        hadError = true;

      typeElts.push_back(TupleTypeElt(subType, elt.getLabel()));
    }

    if (hadError) {
      return ErrorType::get(Context);
    }

    return TupleType::get(typeElts, Context);
  }
      
  //--- Refutable patterns.
  //
  // Refutable patterns occur when checking the PatternBindingDecls in if/let,
  // while/let, and let/else conditions.
  case PatternKind::OptionalSome: {
    // Annotated if-let patterns are rewritten by TypeChecker::resolvePattern
    // to have an enclosing implicit (...)? pattern. If we can resolve the inner
    // typed pattern, the resulting pattern must have optional type.
    auto somePat = cast<OptionalSomePattern>(P);
    if (somePat->isImplicit() && isa<TypedPattern>(somePat->getSubPattern())) {
      OpenUnboundGenericTypeFn unboundTyOpener = nullptr;
      HandlePlaceholderTypeReprFn placeholderHandler = nullptr;
      OpenPackElementFn packElementOpener = nullptr;
      if (pattern.allowsInference()) {
        unboundTyOpener = [](auto unboundTy) {
          // FIXME: Don't let unbound generic types escape type resolution.
          // For now, just return the unbound generic type.
          return unboundTy;
        };
        // FIXME: Don't let placeholder types escape type resolution.
        // For now, just return the placeholder type.
        placeholderHandler = PlaceholderType::get;
      }

      const auto type =
          validateTypedPattern(cast<TypedPattern>(somePat->getSubPattern()), dc,
                               options, unboundTyOpener, placeholderHandler,
                               packElementOpener);

      if (!type->hasError()) {
        return OptionalType::get(type);
      }
    }
    LLVM_FALLTHROUGH;
  }

  case PatternKind::Is:
  case PatternKind::EnumElement:
  case PatternKind::Bool:
  case PatternKind::Expr:
    // In a let/else, these always require an initial value to match against.
    if (!(options & TypeResolutionFlags::AllowUnspecifiedTypes)) {
      Context.Diags.diagnose(P->getLoc(),
                             diag::refutable_pattern_requires_initializer);
      return ErrorType::get(Context);
    }

    return Context.TheUnresolvedType;
  }
  llvm_unreachable("bad pattern kind!");
}

/// Potentially tuple/untuple a pattern before passing it to the pattern engine.
///
/// We need to allow particular matches for backwards compatibility, so we
/// "repair" the pattern if needed. This ensures that the pattern engine
/// receives well-formed input, avoiding the need to implement an additional
/// compatibility hack there, as doing that is lot more tricky due to the
/// different cases that need to handled.
///
/// We also emit diagnostics and potentially a fix-it to help the user.
///
/// See https://github.com/apple/swift/issues/53557 and
/// https://github.com/apple/swift/issues/53611 for more discussion.
//
// type ~ (T1, ..., Tn) (n >= 2)
//   1a. pat ~ ((P1, ..., Pm)) (m >= 2) -> untuple the pattern
//   1b. pat (a single pattern, not a tuple) -> handled by pattern engine
// type ~ ((T1, ..., Tn)) (n >= 2)
//   2. pat ~ (P1, ..., Pm) (m >= 2) -> tuple the pattern
static
void repairTupleOrAssociatedValuePatternIfApplicable(
    ASTContext &Ctx,
    Pattern *&enumElementInnerPat,
    Type enumPayloadType,
    const EnumElementDecl *enumCase) {
  auto &DE = Ctx.Diags;
  bool addDeclNote = false;
  if (auto *tupleType = dyn_cast<TupleType>(enumPayloadType.getPointer())) {
    if (tupleType->getNumElements() >= 2
        && enumElementInnerPat->getKind() == PatternKind::Paren) {
      auto *semantic = enumElementInnerPat->getSemanticsProvidingPattern();
      if (auto *tuplePattern = dyn_cast<TuplePattern>(semantic)) {
        if (tuplePattern->getNumElements() >= 2) {
          auto diag = DE.diagnose(tuplePattern->getLoc(),
              diag::converting_tuple_into_several_associated_values,
              enumCase->getNameStr(), tupleType->getNumElements());
          auto subPattern =
            dyn_cast<ParenPattern>(enumElementInnerPat)->getSubPattern();

          // We might also have code like
          //
          // enum Upair { case upair(Int, Int) }
          // func f(u: Upair) { switch u { case .upair(let (x, y)): () } }
          //
          // This needs a more complex rearrangement to fix the code. So only
          // apply the fix-it if we have a tuple immediately inside.
          if (subPattern->getKind() == PatternKind::Tuple) {
            auto leadingParen = SourceRange(enumElementInnerPat->getStartLoc());
            auto trailingParen = SourceRange(enumElementInnerPat->getEndLoc());
            diag.fixItRemove(leadingParen)
                .fixItRemove(trailingParen);
          }

          addDeclNote = true;
          enumElementInnerPat = semantic;
        }
      } else {
        DE.diagnose(enumElementInnerPat->getLoc(),
          diag::found_one_pattern_for_several_associated_values,
          enumCase->getNameStr(),
          tupleType->getNumElements());
        addDeclNote = true;
      }
    }
  } else if (auto *tupleType = enumPayloadType->getAs<TupleType>()) {
    if (tupleType->getNumElements() >= 2) {
      if (auto *tuplePattern = dyn_cast<TuplePattern>(enumElementInnerPat)) {
        DE.diagnose(enumElementInnerPat->getLoc(),
                    diag::converting_several_associated_values_into_tuple,
                    enumCase->getNameStr(),
                    tupleType->getNumElements())
          .fixItInsert(enumElementInnerPat->getStartLoc(), "(")
          .fixItInsertAfter(enumElementInnerPat->getEndLoc(), ")");
        addDeclNote = true;
        enumElementInnerPat =
          new (Ctx) ParenPattern(enumElementInnerPat->getStartLoc(),
                                 enumElementInnerPat,
                                 enumElementInnerPat->getEndLoc());
      }
    }
  }

  if (addDeclNote)
    DE.diagnose(enumCase->getStartLoc(), diag::decl_declared_here,
                enumCase->getName());
}

NullablePtr<Pattern> TypeChecker::trySimplifyExprPattern(ExprPattern *EP,
                                                         Type patternTy) {
  auto *subExpr = EP->getSubExpr();
  auto &ctx = EP->getDeclContext()->getASTContext();

  if (patternTy->isBool()) {
    // The type is Bool.
    // Check if the pattern is a Bool literal
    auto *semanticSubExpr = subExpr->getSemanticsProvidingExpr();
    if (auto *BLE = dyn_cast<BooleanLiteralExpr>(semanticSubExpr)) {
      auto *BP = new (ctx) BoolPattern(BLE->getLoc(), BLE->getValue());
      BP->setType(patternTy);
      return BP;
    }
  }

  // case nil is equivalent to .none when switching on Optionals.
  if (auto *NLE = dyn_cast<NilLiteralExpr>(EP->getSubExpr())) {
    if (patternTy->getOptionalObjectType()) {
      auto *NoneEnumElement = ctx.getOptionalNoneDecl();
      auto *BaseTE = TypeExpr::createImplicit(patternTy, ctx);
      auto *EEP = new (ctx)
          EnumElementPattern(BaseTE, NLE->getLoc(), DeclNameLoc(NLE->getLoc()),
                             NoneEnumElement->createNameRef(), NoneEnumElement,
                             nullptr, EP->getDeclContext());
      EEP->setType(patternTy);
      return EEP;
    } else {
      // ...but for non-optional types it can never match! Diagnose it.
      ctx.Diags
          .diagnose(NLE->getLoc(), diag::value_type_comparison_with_nil_illegal,
                    patternTy)
          .warnUntilSwiftVersion(6);

      if (ctx.isSwiftVersionAtLeast(6))
        return nullptr;
    }
  }
  return nullptr;
}

/// Perform top-down type coercion on the given pattern.
Pattern *TypeChecker::coercePatternToType(
    ContextualPattern pattern, Type type, TypeResolutionOptions options,
    llvm::function_ref<Optional<Pattern *>(Pattern *, Type)>
        tryRewritePattern) {
  auto P = pattern.getPattern();
  auto dc = pattern.getDeclContext();
  auto &Context = dc->getASTContext();
  auto &diags = Context.Diags;

  // See if we can rewrite this using the constraint system.
  if (auto result = tryRewritePattern(P, type))
    return *result;

  options = applyContextualPatternOptions(options, pattern);
  auto subOptions = options;
  subOptions.setContext(None);
  switch (P->getKind()) {
  // For parens and vars, just set the type annotation and propagate inwards.
  case PatternKind::Paren: {
    auto PP = cast<ParenPattern>(P);
    auto sub = PP->getSubPattern();
    auto semantic = P->getSemanticsProvidingPattern();
    // If this is the payload of an enum, and the type is a single-element
    // labeled tuple, treat this as a tuple pattern. It's unlikely that the
    // user is interested in binding a variable of type (foo: Int).
    if ((options.getContext() == TypeResolverContext::EnumPatternPayload)
        && !isa<TuplePattern>(semantic)) {
      if (auto tupleType = type->getAs<TupleType>()) {
        if (tupleType->getNumElements() == 1) {
          auto element = tupleType->getElement(0);
          sub = coercePatternToType(
              pattern.forSubPattern(sub, /*retainTopLevel=*/true),
              element.getType(), subOptions, tryRewritePattern);
          if (!sub)
            return nullptr;
          TuplePatternElt elt(element.getName(), SourceLoc(), sub);
          P = TuplePattern::create(Context, PP->getLParenLoc(), elt,
                                   PP->getRParenLoc());
          if (PP->isImplicit())
            P->setImplicit();
          P->setType(type);
          return P;
        }
      }
    }

    sub = coercePatternToType(
        pattern.forSubPattern(sub, /*retainTopLevel=*/false), type, subOptions,
        tryRewritePattern);
    if (!sub)
      return nullptr;

    PP->setSubPattern(sub);
    PP->setType(sub->getType());
    return P;
  }
  case PatternKind::Binding: {
    auto VP = cast<BindingPattern>(P);

    Pattern *sub = VP->getSubPattern();
    sub = coercePatternToType(
        pattern.forSubPattern(sub, /*retainTopLevel=*/false), type, subOptions,
        tryRewritePattern);
    if (!sub)
      return nullptr;
    VP->setSubPattern(sub);
    if (sub->hasType())
      VP->setType(sub->getType());
    return P;
  }

  // If we see an explicit type annotation, coerce the sub-pattern to
  // that type.
  case PatternKind::Typed: {
    TypedPattern *TP = cast<TypedPattern>(P);
    Type patternType = TypeChecker::typeCheckPattern(pattern);
    if (!patternType->hasError()) {
      if (!type->isEqual(patternType) && !type->hasError()) {
        if (options & TypeResolutionFlags::OverrideType) {
          TP->setType(type);
          // If the pattern type has a placeholder, we need to resolve it here.
          if (patternType->hasPlaceholder()) {
            validateTypedPattern(cast<TypedPattern>(TP), dc, options, nullptr,
                                 nullptr, /*packElementOpener*/ nullptr);
          }
        } else {
          diags.diagnose(P->getLoc(), diag::pattern_type_mismatch_context,
                         type);
        }
      }
    }

    Pattern *sub = TP->getSubPattern();
    sub = coercePatternToType(
        pattern.forSubPattern(sub, /*retainTopLevel=*/false), type,
        subOptions | TypeResolutionFlags::FromNonInferredPattern,
        tryRewritePattern);
    if (!sub)
      return nullptr;

    TP->setSubPattern(sub);
    TP->setType(sub->getType());
    return P;
  }

  // For wildcard and name patterns, set the type.
  case PatternKind::Named: {
    NamedPattern *NP = cast<NamedPattern>(P);
    VarDecl *var = NP->getDecl();

    // In SIL mode, VarDecls are written as having reference storage types.
    type = type->getReferenceStorageReferent();

    // Note that the pattern's type does not include the reference storage type.
    P->setType(type);
    var->setNamingPattern(NP);

    // FIXME: This call can be removed once pattern binding validation is
    // sufficiently requestified.
    TypeChecker::checkForForbiddenPrefix(Context, var->getBaseName());

    // If we are inferring a variable to have type AnyObject.Type,
    // "()", "[()]", an uninhabited type, or optional thereof, emit a diagnostic.
    // They are probably missing a cast or didn't mean to bind to a variable.
    // We always tell them that they can silence the warning with an
    // explicit type annotation (and provide a fixit) as a note.
    Type diagTy = type->lookThroughAllOptionalTypes();
    bool isOptional = !type->getOptionalObjectType().isNull();
    if (!diagTy) diagTy = type;
    
    auto diag = diag::type_inferred_to_undesirable_type;
    bool shouldRequireType = false;
    if (NP->isImplicit()) {
      // If the whole pattern is implicit, the user didn't write it.
      // Assume the compiler knows what it's doing.
    } else if (diagTy->isEqual(Context.TheEmptyTupleType)) {
      shouldRequireType = true;
    } else if (auto MTT = diagTy->getAs<AnyMetatypeType>()) {
      if (MTT->getInstanceType()->isAnyObject())
        shouldRequireType = true;
    } else if (diagTy->isStructurallyUninhabited()) {
      shouldRequireType = true;
      diag = isOptional ? diag::type_inferred_to_undesirable_type
                        : diag::type_inferred_to_uninhabited_type;

      if (diagTy->is<TupleType>()) {
        diag = isOptional ? diag::type_inferred_to_undesirable_type
                          : diag::type_inferred_to_uninhabited_tuple_type;
      } else {
        assert((diagTy->is<EnumType>() || diagTy->is<BoundGenericEnumType>()) &&
          "unknown structurally uninhabited type");
      }
    } else if (auto *BST = diagTy->getAs<BoundGenericStructType>()) {
      if (BST->isArray())
        shouldRequireType = BST->getGenericArgs()[0]->isVoid();
    }
    
    if (shouldRequireType &&
        !options.is(TypeResolverContext::ForEachStmt) &&
        !options.is(TypeResolverContext::EditorPlaceholderExpr) &&
        !(options & TypeResolutionFlags::FromNonInferredPattern)) {
      diags.diagnose(NP->getLoc(), diag, NP->getDecl()->getName(), type,
                     NP->getDecl()->isLet());
      diags.diagnose(NP->getLoc(), diag::add_explicit_type_annotation_to_silence)
          .fixItInsertAfter(var->getNameLoc(), ": " + type->getWithoutParens()->getString());
    }

    return P;
  }
  case PatternKind::Any:
    P->setType(type);
    return P;

  // We can match a tuple pattern with a tuple type.
  // TODO: permit implicit conversions?
  case PatternKind::Tuple: {
    TuplePattern *TP = cast<TuplePattern>(P);
    bool hadError = type->hasError();
    
    // Sometimes a paren is just a paren. If the tuple pattern has a single
    // element, we can reduce it to a paren pattern.
    bool canDecayToParen = TP->getNumElements() == 1;
    auto decayToParen = [&]() -> Pattern * {
      assert(canDecayToParen);
      Pattern *sub = TP->getElement(0).getPattern();
      sub = coercePatternToType(
          pattern.forSubPattern(sub, /*retainTopLevel=*/false), type,
          subOptions, tryRewritePattern);
      if (!sub)
        return nullptr;

      if (TP->getLParenLoc().isValid()) {
        P = new (Context) ParenPattern(TP->getLParenLoc(), sub,
                                       TP->getRParenLoc());
        P->setType(sub->getType());
      } else {
        P = sub;
      }
      return P;
    };

    // The context type must be a tuple.
    TupleType *tupleTy = type->getAs<TupleType>();
    if (!tupleTy && !hadError) {
      if (canDecayToParen)
        return decayToParen();
      diags.diagnose(TP->getStartLoc(),
                     diag::tuple_pattern_in_non_tuple_context, type);
      hadError = true;
    }

    // The number of elements must match exactly.
    if (!hadError && tupleTy->getNumElements() != TP->getNumElements()) {
      if (canDecayToParen)
        return decayToParen();
      
      diags.diagnose(TP->getStartLoc(), diag::tuple_pattern_length_mismatch,
                     type);
      hadError = true;
    }

    // Coerce each tuple element to the respective type.
    P->setType(type);

    for (unsigned i = 0, e = TP->getNumElements(); i != e; ++i) {
      TuplePatternElt &elt = TP->getElement(i);

      Type CoercionType;
      if (hadError)
        CoercionType = ErrorType::get(Context);
      else
        CoercionType = tupleTy->getElement(i).getType();
      
      // If the tuple pattern had a label for the tuple element, it must match
      // the label for the tuple type being matched.
      if (!hadError && !elt.getLabel().empty() &&
          elt.getLabel() != tupleTy->getElement(i).getName()) {
        diags.diagnose(elt.getLabelLoc(), diag::tuple_pattern_label_mismatch,
                       elt.getLabel(), tupleTy->getElement(i).getName());
        hadError = true;
      }

      auto sub = coercePatternToType(
          pattern.forSubPattern(elt.getPattern(), /*retainTopLevel=*/false),
          CoercionType, subOptions, tryRewritePattern);
      if (!sub)
        return nullptr;

      if (!hadError)
        elt.setPattern(sub);
    }

    if (hadError)
      return nullptr;

    return P;
  }

  // Coerce expressions by finding a '~=' operator that can compare the
  // expression to a value of the coerced type.
  case PatternKind::Expr: {
    assert(cast<ExprPattern>(P)->isResolved()
           && "coercing unresolved expr pattern!");
    auto *EP = cast<ExprPattern>(P);

    if (auto P = trySimplifyExprPattern(EP, type))
      return P.get();

    if (TypeChecker::typeCheckExprPattern(EP, dc, type))
      return nullptr;

    return P;
  }
      
  // Coerce an 'is' pattern by determining the cast kind.
  case PatternKind::Is: {
    auto IP = cast<IsPattern>(P);

    // Type-check the type parameter.
    const auto castType = TypeResolution::resolveContextualType(
        IP->getCastTypeRepr(), dc, TypeResolverContext::InExpression,
        // FIXME: Should we really unconditionally
        // complain about unbound generics and
        // placeholders here?
        /*unboundTyOpener*/ nullptr,
        /*placeholderHandler*/ nullptr,
        /*packElementOpener*/ nullptr);
    if (castType->hasError())
      return nullptr;
    IP->setCastType(castType);

    // Determine whether we have an imbalance in the number of optionals.
    SmallVector<Type, 2> inputTypeOptionals;
    type->lookThroughAllOptionalTypes(inputTypeOptionals);
    SmallVector<Type, 2> castTypeOptionals;
    castType->lookThroughAllOptionalTypes(castTypeOptionals);

    // If we have extra optionals on the input type. Create ".some" patterns
    // wrapping the is pattern to balance out the optionals.
    int numExtraOptionals = inputTypeOptionals.size()-castTypeOptionals.size();
    if (numExtraOptionals > 0) {
      Pattern *sub = IP;
      auto extraOpts =
          llvm::drop_begin(inputTypeOptionals, castTypeOptionals.size());
      for (auto extraOptTy : llvm::reverse(extraOpts)) {
        auto some = Context.getOptionalSomeDecl();
        auto *base = TypeExpr::createImplicit(extraOptTy, Context);
        sub = new (Context) EnumElementPattern(
            base, IP->getStartLoc(), DeclNameLoc(IP->getEndLoc()),
            some->createNameRef(), nullptr, sub, dc);
        sub->setImplicit();
      }

      P = sub;
      return coercePatternToType(
          pattern.forSubPattern(P, /*retainTopLevel=*/true), type, options,
          tryRewritePattern);
    }

    CheckedCastKind castKind = TypeChecker::typeCheckCheckedCast(
        type, IP->getCastType(),
        type->hasError() ? CheckedCastContextKind::None
                         : CheckedCastContextKind::IsPattern,
        dc);
    switch (castKind) {
    case CheckedCastKind::Unresolved:
      if (type->hasError()) {
        return nullptr;
      }
      diags
          .diagnose(IP->getLoc(), diag::downcast_to_unrelated, type,
                    IP->getCastType())
          .highlight(IP->getLoc())
          .highlight(IP->getCastTypeRepr()->getSourceRange());

      IP->setCastKind(CheckedCastKind::ValueCast);
      break;
    case CheckedCastKind::Coercion:
    case CheckedCastKind::BridgingCoercion:
      // If this is an 'as' pattern coercing between two different types, then
      // it is "useful" because it is providing a different type to the
      // sub-pattern.  If this is an 'is' pattern or an 'as' pattern where the
      // types are the same, then produce a warning.
      if (!IP->getSubPattern() || type->isEqual(IP->getCastType())) {
        diags.diagnose(IP->getLoc(), diag::isa_is_always_true,
                       IP->getSubPattern() ? "as" : "is");
      }
      IP->setCastKind(castKind);
      break;

    // Valid checks.
    case CheckedCastKind::ArrayDowncast:
    case CheckedCastKind::DictionaryDowncast:
    case CheckedCastKind::SetDowncast:
    case CheckedCastKind::ValueCast:
      IP->setCastKind(castKind);
      break;
    }
    IP->setType(type);
    
    // Coerce the subpattern to the destination type.
    if (Pattern *sub = IP->getSubPattern()) {
      sub = coercePatternToType(
          pattern.forSubPattern(sub, /*retainTopLevel=*/false),
          IP->getCastType(),
          subOptions | TypeResolutionFlags::FromNonInferredPattern,
          tryRewritePattern);
      if (!sub)
        return nullptr;

      IP->setSubPattern(sub);
    }
    
    return P;
  }
      
  case PatternKind::EnumElement: {
    auto *EEP = cast<EnumElementPattern>(P);
    
    // If the element decl was not resolved (because it was spelled without a
    // type as `.Foo`), resolve it now that we have a type.
    Optional<CheckedCastKind> castKind;
    
    EnumElementDecl *elt = EEP->getElementDecl();
    
    Type enumTy;
    if (!elt) {
      elt = lookupEnumMemberElement(dc, type, EEP->getName(),
                                    EEP->getLoc());
      if (!elt) {
        if (!type->hasError()) {
          // If we have an optional type, let's try to see if the case
          // exists in its base type and if it does then synthesize an
          // OptionalSomePattern that wraps the case. This uses recursion
          // to add multiple levels of OptionalSomePattern if the optional
          // is nested.
          if (auto baseType = type->getOptionalObjectType()) {
            if (lookupEnumMemberElement(dc,
                                        baseType->lookThroughAllOptionalTypes(),
                                        EEP->getName(), EEP->getLoc())) {
              P = OptionalSomePattern::createImplicit(Context, EEP,
                                                      EEP->getEndLoc());
              return coercePatternToType(
                  pattern.forSubPattern(P, /*retainTopLevel=*/true), type,
                  options, tryRewritePattern);
            } else {
              diags.diagnose(EEP->getLoc(),
                             diag::enum_element_pattern_member_not_found,
                             EEP->getName(), type);
              return nullptr;
            }
          } else if (EEP->hasUnresolvedOriginalExpr()) {
            // If we have the original expression parse tree, try reinterpreting
            // it as an expr-pattern if enum element lookup failed, since `.foo`
            // could also refer to a static member of the context type.
            P = ExprPattern::createResolved(
                Context, EEP->getUnresolvedOriginalExpr(), dc);
            return coercePatternToType(
                pattern.forSubPattern(P, /*retainTopLevel=*/true), type,
                options, tryRewritePattern);
          }
        }
      }

      if (!elt)
        return nullptr;

      // Emit an ambiguous none diagnostic if:
      // 1) We have an Optional<T> type.
      // 2) We're matching a 'none' enum case.
      // 3) The 'none' enum case exists in T too.
      if (EEP->getName().isSimpleName("none") &&
          type->getOptionalObjectType()) {
        SmallVector<Type, 4> allOptionals;
        auto baseTyUnwrapped = type->lookThroughAllOptionalTypes(allOptionals);
        if (lookupEnumMemberElement(dc, baseTyUnwrapped, EEP->getName(),
                                    EEP->getLoc())) {
          auto baseTyName = type->getCanonicalType().getString();
          auto baseTyUnwrappedName = baseTyUnwrapped->getString();
          diags.diagnoseWithNotes(
              diags.diagnose(EEP->getLoc(), diag::optional_ambiguous_case_ref,
                             baseTyName, baseTyUnwrappedName, "none"),
              [&]() {
                // Emit a note to swap '.none' with 'nil' to match with
                // the 'none' case in Optional<T>.
                diags.diagnose(EEP->getLoc(),
                              diag::optional_fixit_ambiguous_case_ref_switch)
                    .fixItReplace(EEP->getSourceRange(), "nil");
                // Emit a note to swap '.none' with 'none?' to match with the
                // 'none' case in T. Add as many '?' as needed to look though
                // all the optionals.
                std::string fixItString = "none";
                llvm::for_each(allOptionals,
                               [&](const Type) { fixItString += "?"; });
                diags.diagnose(
                        EEP->getLoc(),
                        diag::type_fixit_optional_ambiguous_case_ref_switch,
                        fixItString)
                    .fixItReplace(EEP->getNameLoc().getSourceRange(),
                                  fixItString);
              });
        }
      }

      enumTy = type;
    } else {
      // Check if the explicitly-written enum type matches the type we're
      // coercing to.
      assert(!EEP->getParentType().isNull()
             && "enum with resolved element doesn't specify parent type?!");
      auto parentTy = EEP->getParentType();
      // If the type matches exactly, use it.
      if (parentTy->isEqual(type)) {
        enumTy = type;
      }
      // Otherwise, if the type is an unbound generic of the context type, use
      // the context type to resolve the parameters.
      else if (parentTy->hasUnboundGenericType()) {
        if (parentTy->is<UnboundGenericType>() &&
            parentTy->getAnyNominal() == type->getAnyNominal()) {
          enumTy = type;
        } else {
          diags.diagnose(EEP->getLoc(), diag::ambiguous_enum_pattern_type,
                         parentTy, type);
          return nullptr;
        }
      }
      // Otherwise, see if we can introduce a cast pattern to get from an
      // existential pattern type to the enum type.
      else if (type->isAnyExistentialType()) {
        auto foundCastKind = typeCheckCheckedCast(
            type, parentTy, CheckedCastContextKind::EnumElementPattern, dc);
        // If the cast failed, we can't resolve the pattern.
        if (foundCastKind < CheckedCastKind::First_Resolved) {
          diags.diagnose(EEP->getLoc(), diag::cannot_match_value_with_pattern,
                         type, parentTy)
              .highlight(EEP->getSourceRange());
          return nullptr;
        }

        // Otherwise, we can type-check as the enum type, and insert a cast
        // from the outer pattern type.
        castKind = foundCastKind;
        enumTy = parentTy;
      } else {
        diags.diagnose(EEP->getLoc(), diag::cannot_match_value_with_pattern,
                       type, parentTy)
            .highlight(EEP->getSourceRange());
        return nullptr;
      }
    }

    // If there is a subpattern, push the enum element type down onto it.
    auto argType = elt->getArgumentInterfaceType();
    if (EEP->hasSubPattern()) {
      Pattern *sub = EEP->getSubPattern();
      if (!elt->hasAssociatedValues()) {
        diags.diagnose(EEP->getLoc(),
                       diag::enum_element_pattern_assoc_values_mismatch,
                       EEP->getName());
        diags.diagnose(EEP->getLoc(),
                       diag::enum_element_pattern_assoc_values_remove)
          .fixItRemove(sub->getSourceRange());
        return nullptr;
      }
      
      Type elementType;
      if (argType)
        elementType = enumTy->getTypeOfMember(elt->getModuleContext(),
                                              elt, argType);
      else
        elementType = TupleType::getEmpty(Context);
      auto newSubOptions = subOptions;
      newSubOptions.setContext(TypeResolverContext::EnumPatternPayload);
      newSubOptions |= TypeResolutionFlags::FromNonInferredPattern;

      ::repairTupleOrAssociatedValuePatternIfApplicable(
        Context, sub, elementType, elt);

      sub = coercePatternToType(
          pattern.forSubPattern(sub, /*retainTopLevel=*/false), elementType,
          newSubOptions, tryRewritePattern);
      if (!sub)
        return nullptr;

      EEP->setSubPattern(sub);
    } else if (argType) {
      // Else if the element pattern has no sub-pattern but the element type has
      // associated values, expand it to be semantically equivalent to an
      // element pattern of wildcards.
      Type elementType = enumTy->getTypeOfMember(elt->getModuleContext(),
                                                 elt, argType);
      SmallVector<TuplePatternElt, 8> elements;
      if (auto *TTy = dyn_cast<TupleType>(elementType.getPointer())) {
        for (auto &elt : TTy->getElements()) {
          auto *subPattern = AnyPattern::createImplicit(Context);
          elements.push_back(TuplePatternElt(elt.getName(), SourceLoc(),
                                             subPattern));
        }
      } else {
        auto parenTy = dyn_cast<ParenType>(elementType.getPointer());
        assert(parenTy && "Associated value type is neither paren nor tuple?");
        (void)parenTy;
        
        auto *subPattern = AnyPattern::createImplicit(Context);
        elements.push_back(TuplePatternElt(Identifier(), SourceLoc(),
                                           subPattern));
      }
      Pattern *sub = TuplePattern::createSimple(Context, SourceLoc(),
                                                elements, SourceLoc());
      sub->setImplicit();
      auto newSubOptions = subOptions;
      newSubOptions.setContext(TypeResolverContext::EnumPatternPayload);
      newSubOptions |= TypeResolutionFlags::FromNonInferredPattern;
      sub = coercePatternToType(
          pattern.forSubPattern(sub, /*retainTopLevel=*/false), elementType,
          newSubOptions, tryRewritePattern);
      if (!sub)
        return nullptr;
      EEP->setSubPattern(sub);
    }

    EEP->setElementDecl(elt);
    EEP->setType(enumTy);
    EEP->setParentType(enumTy);

    // If we needed a cast, wrap the pattern in a cast pattern.
    if (castKind) {
      auto isPattern =
          IsPattern::createImplicit(Context, enumTy, EEP, *castKind);
      isPattern->setType(type);
      P = isPattern;
    }
    
    return P;
  }

  case PatternKind::OptionalSome: {
    auto *OP = cast<OptionalSomePattern>(P);
    Type elementType = type->getOptionalObjectType();

    if (elementType.isNull()) {
      auto diagID = diag::optional_element_pattern_not_valid_type;
      SourceLoc loc = OP->getQuestionLoc();
      // Produce tailored diagnostic for if/let and other conditions.
      if (OP->isImplicit()) {
        diagID = diag::condition_optional_element_pattern_not_valid_type;
        loc = OP->getLoc();
      }

      diags.diagnose(loc, diagID, type);
      return nullptr;
    }

    Pattern *sub = OP->getSubPattern();
    auto newSubOptions = subOptions;
    newSubOptions.setContext(TypeResolverContext::EnumPatternPayload);
    newSubOptions |= TypeResolutionFlags::FromNonInferredPattern;
    sub = coercePatternToType(
        pattern.forSubPattern(sub, /*retainTopLevel=*/false), elementType,
        newSubOptions, tryRewritePattern);
    if (!sub)
      return nullptr;

    OP->setSubPattern(sub);
    OP->setType(type);
    return P;
  }

  case PatternKind::Bool:
    P->setType(type);
    return P;
  }
  llvm_unreachable("bad pattern kind!");
}

/// Coerce the specified parameter list of a ClosureExpr to the specified
/// contextual type.
void TypeChecker::coerceParameterListToType(ParameterList *P,
                                            AnyFunctionType *FN) {

  // Local function to check if the given type is valid e.g. doesn't have
  // errors, type variables or unresolved types related to it.
  auto isValidType = [](Type type) -> bool {
    return !(type->hasError() || type->hasUnresolvedType());
  };

  // Local function to check whether type of given parameter
  // should be coerced to a given contextual type or not.
  auto shouldOverwriteParam = [&](ParamDecl *param) -> bool {
    return !isValidType(param->getType());
  };

  auto handleParameter = [&](ParamDecl *param, Type ty, bool forceMutable) {
    if (forceMutable)
      param->setSpecifier(ParamDecl::Specifier::InOut);

    // If contextual type is invalid and we have a valid argument type
    // trying to coerce argument to contextual type would mean erasing
    // valuable diagnostic information.
    if (isValidType(ty) || shouldOverwriteParam(param)) {
      param->setInterfaceType(ty->mapTypeOutOfContext());
    }
  };

  // Coerce each parameter to the respective type.
  ArrayRef<AnyFunctionType::Param> params = FN->getParams();
  for (unsigned i = 0, e = P->size(); i != e; ++i) {
    auto &param = P->get(i);
    assert(param->getArgumentName().empty() &&
           "Closures cannot have API names");
    
    handleParameter(param,
                    params[i].getParameterType(),
                    params[i].isInOut());
    assert(!param->isDefaultArgument() && "Closures cannot have default args");
  }
}
