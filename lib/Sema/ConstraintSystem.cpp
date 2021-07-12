//===--- ConstraintSystem.cpp - Constraint-based Type Checking ------------===//
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
// This file implements the constraint-based type checker, anchored by the
// \c ConstraintSystem class, which provides type checking and type
// inference for expressions.
//
//===----------------------------------------------------------------------===//
#include "CSDiagnostics.h"
#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckType.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "swift/Sema/CSFix.h"
#include "swift/Sema/ConstraintGraph.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/SolutionResult.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Format.h"

using namespace swift;
using namespace constraints;
using namespace inference;

#define DEBUG_TYPE "ConstraintSystem"

ExpressionTimer::ExpressionTimer(Expr *E, ConstraintSystem &CS)
  : E(E),
      Context(CS.getASTContext()),
      StartTime(llvm::TimeRecord::getCurrentTime()),
      PrintDebugTiming(CS.getASTContext().TypeCheckerOpts.DebugTimeExpressions),
      PrintWarning(true) {
}

ExpressionTimer::~ExpressionTimer() {
  auto elapsed = getElapsedProcessTimeInFractionalSeconds();
  unsigned elapsedMS = static_cast<unsigned>(elapsed * 1000);

  if (PrintDebugTiming) {
    // Round up to the nearest 100th of a millisecond.
    llvm::errs() << llvm::format("%0.2f", ceil(elapsed * 100000) / 100)
                 << "ms\t";
    E->getLoc().print(llvm::errs(), Context.SourceMgr);
    llvm::errs() << "\n";
  }

  if (!PrintWarning)
    return;

  const auto WarnLimit = getWarnLimit();
  if (WarnLimit != 0 && elapsedMS >= WarnLimit && E->getLoc().isValid())
    Context.Diags.diagnose(E->getLoc(), diag::debug_long_expression,
                           elapsedMS, WarnLimit)
      .highlight(E->getSourceRange());
}


ConstraintSystem::ConstraintSystem(DeclContext *dc,
                                   ConstraintSystemOptions options)
  : Context(dc->getASTContext()), DC(dc), Options(options),
    Arena(dc->getASTContext(), Allocator),
    CG(*new ConstraintGraph(*this))
{
  assert(DC && "context required");
  // Respect the global debugging flag, but turn off debugging while
  // parsing and loading other modules.
  if (Context.TypeCheckerOpts.DebugConstraintSolver &&
      DC->getParentModule()->isMainModule()) {
    Options |= ConstraintSystemFlags::DebugConstraints;
  }
  if (Context.LangOpts.UseClangFunctionTypes)
    Options |= ConstraintSystemFlags::UseClangFunctionTypes;
}

ConstraintSystem::~ConstraintSystem() {
  delete &CG;
}

void ConstraintSystem::incrementScopeCounter() {
  ++CountScopes;
  // FIXME: (transitional) increment the redundant "always-on" counter.
  if (auto *Stats = getASTContext().Stats)
    ++Stats->getFrontendCounters().NumConstraintScopes;
}

void ConstraintSystem::incrementLeafScopes() {
  if (auto *Stats = getASTContext().Stats)
    ++Stats->getFrontendCounters().NumLeafScopes;
}

bool ConstraintSystem::hasFreeTypeVariables() {
  // Look for any free type variables.
  return llvm::any_of(TypeVariables, [](const TypeVariableType *typeVar) {
    return !typeVar->getImpl().hasRepresentativeOrFixed();
  });
}

void ConstraintSystem::addTypeVariable(TypeVariableType *typeVar) {
  TypeVariables.insert(typeVar);

  // Notify the constraint graph.
  (void)CG[typeVar];
}

void ConstraintSystem::mergeEquivalenceClasses(TypeVariableType *typeVar1,
                                               TypeVariableType *typeVar2,
                                               bool updateWorkList) {
  assert(typeVar1 == getRepresentative(typeVar1) &&
         "typeVar1 is not the representative");
  assert(typeVar2 == getRepresentative(typeVar2) &&
         "typeVar2 is not the representative");
  assert(typeVar1 != typeVar2 && "cannot merge type with itself");
  typeVar1->getImpl().mergeEquivalenceClasses(typeVar2, getSavedBindings());

  // Merge nodes in the constraint graph.
  CG.mergeNodes(typeVar1, typeVar2);

  if (updateWorkList) {
    addTypeVariableConstraintsToWorkList(typeVar1);
  }
}

/// Determine whether the given type variables occurs in the given type.
bool ConstraintSystem::typeVarOccursInType(TypeVariableType *typeVar,
                                           Type type,
                                           bool *involvesOtherTypeVariables) {
  SmallPtrSet<TypeVariableType *, 4> typeVars;
  type->getTypeVariables(typeVars);

  bool occurs = typeVars.count(typeVar);
  if (involvesOtherTypeVariables) {
    *involvesOtherTypeVariables =
        occurs ? typeVars.size() > 1 : !typeVars.empty();
  }

  return occurs;
}

void ConstraintSystem::assignFixedType(TypeVariableType *typeVar, Type type,
                                       bool updateState,
                                       bool notifyBindingInference) {
  assert(!type->hasError() &&
         "Should not be assigning a type involving ErrorType!");

  typeVar->getImpl().assignFixedType(type, getSavedBindings());

  if (!updateState)
    return;

  if (!type->isTypeVariableOrMember()) {
    // If this type variable represents a literal, check whether we picked the
    // default literal type. First, find the corresponding protocol.
    //
    // If we have the constraint graph, we can check all type variables in
    // the equivalence class. This is the More Correct path.
    // FIXME: Eliminate the less-correct path.
    auto typeVarRep = getRepresentative(typeVar);
    for (auto *tv : CG[typeVarRep].getEquivalenceClass()) {
      auto locator = tv->getImpl().getLocator();
      if (!(locator && (locator->directlyAt<CollectionExpr>() ||
                        locator->directlyAt<LiteralExpr>())))
          continue;

      auto *literalProtocol = TypeChecker::getLiteralProtocol(
          getASTContext(), castToExpr(locator->getAnchor()));
      if (!literalProtocol)
        continue;

      // If the protocol has a default type, check it.
      if (auto defaultType = TypeChecker::getDefaultType(literalProtocol, DC)) {
        // Check whether the nominal types match. This makes sure that we
        // properly handle Array vs. Array<T>.
        if (defaultType->getAnyNominal() != type->getAnyNominal()) {
          increaseScore(SK_NonDefaultLiteral);
        }
      }

      break;
    }
  }

  // Notify the constraint graph.
  CG.bindTypeVariable(typeVar, type);
  addTypeVariableConstraintsToWorkList(typeVar);

  if (notifyBindingInference)
    CG[typeVar].introduceToInference(type);
}

void ConstraintSystem::addTypeVariableConstraintsToWorkList(
       TypeVariableType *typeVar) {
  // Activate the constraints affected by a change to this type variable.
  auto gatheringKind = ConstraintGraph::GatheringKind::AllMentions;
  for (auto *constraint : CG.gatherConstraints(typeVar, gatheringKind))
    if (!constraint->isActive())
      activateConstraint(constraint);
}

/// Retrieve a dynamic result signature for the given declaration.
static std::tuple<char, ObjCSelector, CanType>
getDynamicResultSignature(ValueDecl *decl) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    // Handle functions.
    auto type = func->getMethodInterfaceType();
    return std::make_tuple(func->isStatic(), func->getObjCSelector(),
                           type->getCanonicalType());
  }

  if (auto asd = dyn_cast<AbstractStorageDecl>(decl)) {
    auto ty = asd->getInterfaceType();

    // Strip off a generic signature if we have one. This matches the logic
    // for methods, and ensures that we don't take a protocol's generic
    // signature into account for a subscript requirement.
    if (auto *genericFn = ty->getAs<GenericFunctionType>()) {
      ty = FunctionType::get(genericFn->getParams(), genericFn->getResult(),
                             genericFn->getExtInfo());
    }

    // Handle properties and subscripts, anchored by the getter's selector.
    return std::make_tuple(asd->isStatic(), asd->getObjCGetterSelector(),
                           ty->getCanonicalType());
  }

  llvm_unreachable("Not a valid @objc member");
}

LookupResult &ConstraintSystem::lookupMember(Type base, DeclNameRef name) {
  // Check whether we've already performed this lookup.
  auto &result = MemberLookups[{base, name}];
  if (result) return *result;

  // Lookup the member.
  result = TypeChecker::lookupMember(DC, base, name, defaultMemberLookupOptions);

  // If we aren't performing dynamic lookup, we're done.
  if (!*result || !base->isAnyObject())
    return *result;

  // We are performing dynamic lookup. Filter out redundant results early.
  llvm::DenseMap<std::tuple<char, ObjCSelector, CanType>, ValueDecl *> known;
  bool anyRemovals = false;
  for (const auto &entry : *result) {
    auto *decl = entry.getValueDecl();

    // Remove invalid declarations so the constraint solver doesn't need to
    // cope with them.
    if (decl->isInvalid()) {
      anyRemovals = true;
      continue;
    }

    // If this is the first entry with the signature, record it.
    auto &uniqueEntry = known[getDynamicResultSignature(decl)];
    if (!uniqueEntry) {
      uniqueEntry = decl;
      continue;
    }

    // We have duplication; note that we'll need to remove something,
    anyRemovals = true;

    // If the entry we recorded was unavailable but this new entry is not,
    // replace the recorded entry with this one.
    if (isDeclUnavailable(uniqueEntry) && !isDeclUnavailable(decl)) {
      uniqueEntry = decl;
    }
  }

  // If there's anything to remove, filter it out now.
  if (anyRemovals) {
    result->filter([&](LookupResultEntry entry, bool isOuter) -> bool {
      auto *decl = entry.getValueDecl();

      // Remove invalid declarations so the constraint solver doesn't need to
      // cope with them.
      if (decl->isInvalid())
        return false;

      return known[getDynamicResultSignature(decl)] == decl;
    });
  }

  return *result;
}

ArrayRef<Type> ConstraintSystem::
getAlternativeLiteralTypes(KnownProtocolKind kind) {
  unsigned index;

  switch (kind) {
#define PROTOCOL_WITH_NAME(Id, Name) \
  case KnownProtocolKind::Id: llvm_unreachable("Not a literal protocol");
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, Name, __, ___)
#include "swift/AST/KnownProtocols.def"

  case KnownProtocolKind::ExpressibleByArrayLiteral:     index = 0; break;
  case KnownProtocolKind::ExpressibleByDictionaryLiteral:index = 1; break;
  case KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral: index = 2;
    break;
  case KnownProtocolKind::ExpressibleByFloatLiteral: index = 3; break;
  case KnownProtocolKind::ExpressibleByIntegerLiteral: index = 4; break;
  case KnownProtocolKind::ExpressibleByStringInterpolation: index = 5; break;
  case KnownProtocolKind::ExpressibleByStringLiteral: index = 6; break;
  case KnownProtocolKind::ExpressibleByNilLiteral: index = 7; break;
  case KnownProtocolKind::ExpressibleByBooleanLiteral: index = 8; break;
  case KnownProtocolKind::ExpressibleByUnicodeScalarLiteral: index = 9; break;
  case KnownProtocolKind::ExpressibleByColorLiteral: index = 10; break;
  case KnownProtocolKind::ExpressibleByImageLiteral: index = 11; break;
  case KnownProtocolKind::ExpressibleByFileReferenceLiteral: index = 12; break;
  }
  static_assert(NumAlternativeLiteralTypes == 13, "Wrong # of literal types");

  // If we already looked for alternative literal types, return those results.
  if (AlternativeLiteralTypes[index])
    return *AlternativeLiteralTypes[index];

  SmallVector<Type, 4> types;

  // Some literal kinds are related.
  switch (kind) {
#define PROTOCOL_WITH_NAME(Id, Name) \
  case KnownProtocolKind::Id: llvm_unreachable("Not a literal protocol");
#define EXPRESSIBLE_BY_LITERAL_PROTOCOL_WITH_NAME(Id, Name, __, ___)
#include "swift/AST/KnownProtocols.def"

  case KnownProtocolKind::ExpressibleByArrayLiteral:
  case KnownProtocolKind::ExpressibleByDictionaryLiteral:
    break;

  case KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral:
  case KnownProtocolKind::ExpressibleByStringInterpolation:
  case KnownProtocolKind::ExpressibleByStringLiteral:
  case KnownProtocolKind::ExpressibleByUnicodeScalarLiteral:
    break;

  case KnownProtocolKind::ExpressibleByIntegerLiteral:
    // Integer literals can be treated as floating point literals.
    if (auto floatProto = getASTContext().getProtocol(
                            KnownProtocolKind::ExpressibleByFloatLiteral)) {
      if (auto defaultType = TypeChecker::getDefaultType(floatProto, DC)) {
        types.push_back(defaultType);
      }
    }
    break;

  case KnownProtocolKind::ExpressibleByFloatLiteral:
    break;

  case KnownProtocolKind::ExpressibleByNilLiteral:
  case KnownProtocolKind::ExpressibleByBooleanLiteral:
    break;
  case KnownProtocolKind::ExpressibleByColorLiteral:
  case KnownProtocolKind::ExpressibleByImageLiteral:
  case KnownProtocolKind::ExpressibleByFileReferenceLiteral:
    break;
  }

  AlternativeLiteralTypes[index] = allocateCopy(types);
  return *AlternativeLiteralTypes[index];
}

bool ConstraintSystem::containsCodeCompletionLoc(Expr *expr) const {
  SourceRange range = expr->getSourceRange();
  if (range.isInvalid())
    return false;
  return Context.SourceMgr.rangeContainsCodeCompletionLoc(range);
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
    ASTNode anchor, ArrayRef<ConstraintLocator::PathElement> path) {
  auto summaryFlags = ConstraintLocator::getSummaryFlagsForPath(path);
  return getConstraintLocator(anchor, path, summaryFlags);
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
    ASTNode anchor, ArrayRef<ConstraintLocator::PathElement> path,
    unsigned summaryFlags) {
  assert(summaryFlags == ConstraintLocator::getSummaryFlagsForPath(path));

  // Check whether a locator with this anchor + path already exists.
  llvm::FoldingSetNodeID id;
  ConstraintLocator::Profile(id, anchor, path);
  void *insertPos = nullptr;
  auto locator = ConstraintLocators.FindNodeOrInsertPos(id, insertPos);
  if (locator)
    return locator;

  // Allocate a new locator and add it to the set.
  locator = ConstraintLocator::create(getAllocator(), anchor, path,
                                      summaryFlags);
  ConstraintLocators.InsertNode(locator, insertPos);
  return locator;
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
                     const ConstraintLocatorBuilder &builder) {
  // If the builder has an empty path, just extract its base locator.
  if (builder.hasEmptyPath()) {
    return builder.getBaseLocator();
  }

  // We have to build a new locator. Extract the paths from the builder.
  SmallVector<LocatorPathElt, 4> path;
  auto anchor = builder.getLocatorParts(path);
  return getConstraintLocator(anchor, path, builder.getSummaryFlags());
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
    ConstraintLocator *locator,
    ArrayRef<ConstraintLocator::PathElement> newElts) {
  auto oldPath = locator->getPath();
  SmallVector<ConstraintLocator::PathElement, 4> newPath;
  newPath.append(oldPath.begin(), oldPath.end());
  newPath.append(newElts.begin(), newElts.end());
  return getConstraintLocator(locator->getAnchor(), newPath);
}

ConstraintLocator *ConstraintSystem::getConstraintLocator(
    const ConstraintLocatorBuilder &builder,
    ArrayRef<ConstraintLocator::PathElement> newElts) {
  SmallVector<ConstraintLocator::PathElement, 4> newPath;
  auto anchor = builder.getLocatorParts(newPath);
  newPath.append(newElts.begin(), newElts.end());
  return getConstraintLocator(anchor, newPath);
}

ConstraintLocator *ConstraintSystem::getCalleeLocator(
    ConstraintLocator *locator, bool lookThroughApply,
    llvm::function_ref<Type(Expr *)> getType,
    llvm::function_ref<Type(Type)> simplifyType,
    llvm::function_ref<Optional<SelectedOverload>(ConstraintLocator *)>
        getOverloadFor) {
  if (auto conversion =
          locator->findLast<LocatorPathElt::ImplicitConversion>()) {
    if (conversion->is(ConversionRestrictionKind::DoubleToCGFloat) ||
        conversion->is(ConversionRestrictionKind::CGFloatToDouble)) {
      return getConstraintLocator(
          ASTNode(), {*conversion, ConstraintLocator::ApplyFunction,
                      ConstraintLocator::ConstructorMember});
    }
  }

  auto anchor = locator->getAnchor();
  assert(bool(anchor) && "Expected an anchor!");

  auto path = locator->getPath();
  {
    // If we have a locator for a member found through key path dynamic member
    // lookup, then we need to chop off the elements after the
    // KeyPathDynamicMember element to get the callee locator.
    auto iter = path.rbegin();
    if (locator->findLast<LocatorPathElt::KeyPathDynamicMember>(iter)) {
      auto newPath = path.drop_back(iter - path.rbegin());
      return getConstraintLocator(anchor, newPath);
    }
  }

  if (locator->findLast<LocatorPathElt::DynamicCallable>()) {
    return getConstraintLocator(anchor, LocatorPathElt::ApplyFunction());
  }

  if (locator->isLastElement<LocatorPathElt::ArgumentAttribute>()) {
    return getConstraintLocator(anchor, path.drop_back());
  }

  // If we have a locator that starts with a key path component element, we
  // may have a callee given by a property or subscript component.
  if (auto componentElt =
          locator->getFirstElementAs<LocatorPathElt::KeyPathComponent>()) {
    auto *kpExpr = castToExpr<KeyPathExpr>(anchor);
    auto component = kpExpr->getComponents()[componentElt->getIndex()];

    using ComponentKind = KeyPathExpr::Component::Kind;
    switch (component.getKind()) {
    case ComponentKind::UnresolvedSubscript:
    case ComponentKind::Subscript:
      // For a subscript the callee is given by 'component -> subscript member'.
      return getConstraintLocator(
          anchor, {*componentElt, ConstraintLocator::SubscriptMember});
    case ComponentKind::UnresolvedProperty:
    case ComponentKind::Property:
      // For a property, the choice is just given by the component.
      return getConstraintLocator(anchor, *componentElt);
    case ComponentKind::TupleElement:
      llvm_unreachable("Not implemented by CSGen");
      break;
    case ComponentKind::Invalid:
    case ComponentKind::OptionalForce:
    case ComponentKind::OptionalChain:
    case ComponentKind::OptionalWrap:
    case ComponentKind::Identity:
    case ComponentKind::DictionaryKey:
      // These components don't have any callee associated, so just continue.
      break;
    }
  }

  // Make sure we handle subscripts before looking at apply exprs. We don't
  // want to return a subscript member locator for an expression such as x[](y),
  // as its callee is not the subscript, but rather the function it returns.
  if (isExpr<SubscriptExpr>(anchor))
    return getConstraintLocator(anchor, ConstraintLocator::SubscriptMember);

  auto getSpecialFnCalleeLoc = [&](Type fnTy) -> ConstraintLocator * {
    fnTy = simplifyType(fnTy);
    // It's okay for function type to contain type variable(s) e.g.
    // opened generic function types, but not to be one.
    assert(!fnTy->is<TypeVariableType>());

    // For an apply of a metatype, we have a short-form constructor. Unlike
    // other locators to callees, these are anchored on the apply expression
    // rather than the function expr.
    if (fnTy->is<AnyMetatypeType>()) {
      return getConstraintLocator(anchor,
                                  {LocatorPathElt::ApplyFunction(),
                                   LocatorPathElt::ConstructorMember()});
    }

    // Handle an apply of a nominal type which supports callAsFunction.
    if (fnTy->isCallableNominalType(DC)) {
      return getConstraintLocator(anchor,
                                  {LocatorPathElt::ApplyFunction(),
                                   LocatorPathElt::ImplicitCallAsFunction()});
    }

    // Handling an apply for a nominal type that supports @dynamicCallable.
    if (fnTy->hasDynamicCallableAttribute()) {
      return getConstraintLocator(anchor, LocatorPathElt::ApplyFunction());
    }

    return nullptr;
  };

  if (lookThroughApply) {
    if (auto *applyExpr = getAsExpr<ApplyExpr>(anchor)) {
      auto *fnExpr = applyExpr->getFn();

      // Handle special cases for applies of non-function types.
      if (auto *loc = getSpecialFnCalleeLoc(getType(fnExpr)))
        return loc;

      // Otherwise fall through and look for locators anchored on the function
      // expr. For CallExprs, this can look through things like parens and
      // optional chaining.
      if (auto *callExpr = getAsExpr<CallExpr>(anchor)) {
        anchor = callExpr->getDirectCallee();
      } else {
        anchor = fnExpr;
      }
    }
  }

  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    return getConstraintLocator(
        anchor, TypeChecker::getSelfForInitDelegationInConstructor(DC, UDE)
                    ? ConstraintLocator::ConstructorMember
                    : ConstraintLocator::Member);
  }

  if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    return getConstraintLocator(UME, ConstraintLocator::UnresolvedMember);
  }

  if (isExpr<MemberRefExpr>(anchor))
    return getConstraintLocator(anchor, ConstraintLocator::Member);

  if (isExpr<ObjectLiteralExpr>(anchor))
    return getConstraintLocator(anchor, ConstraintLocator::ConstructorMember);

  return getConstraintLocator(anchor);
}

/// Extend the given depth map by adding depths for all of the subexpressions
/// of the given expression.
static void extendDepthMap(
   Expr *expr,
   llvm::DenseMap<Expr *, std::pair<unsigned, Expr *>> &depthMap) {
  class RecordingTraversal : public ASTWalker {
    SmallVector<ClosureExpr *, 4> Closures;

  public:
    llvm::DenseMap<Expr *, std::pair<unsigned, Expr *>> &DepthMap;
    unsigned Depth = 0;

    explicit RecordingTraversal(
        llvm::DenseMap<Expr *, std::pair<unsigned, Expr *>> &depthMap)
        : DepthMap(depthMap) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      DepthMap[E] = {Depth, Parent.getAsExpr()};
      ++Depth;

      if (auto CE = dyn_cast<ClosureExpr>(E))
        Closures.push_back(CE);

      return { true, E };
    }

    Expr *walkToExprPost(Expr *E) override {
      if (auto CE = dyn_cast<ClosureExpr>(E)) {
        assert(Closures.back() == CE);
        Closures.pop_back();
      }

      --Depth;
      return E;
    }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *S) override {
      if (auto RS = dyn_cast<ReturnStmt>(S)) {
        // For return statements, treat the parent of the return expression
        // as the closure itself.
        if (RS->hasResult() && !Closures.empty()) {
          llvm::SaveAndRestore<ParentTy> SavedParent(Parent, Closures.back());
          auto E = RS->getResult();
          E->walk(*this);
          return { false, S };
        }
      }

      return { true, S };
    }
  };

  RecordingTraversal traversal(depthMap);
  expr->walk(traversal);
}

Optional<std::pair<unsigned, Expr *>> ConstraintSystem::getExprDepthAndParent(
    Expr *expr) {
  // Bring the set of expression weights up to date.
  while (NumInputExprsInWeights < InputExprs.size()) {
    extendDepthMap(InputExprs[NumInputExprsInWeights], ExprWeights);
    ++NumInputExprsInWeights;
  }

  auto e = ExprWeights.find(expr);
  if (e != ExprWeights.end())
    return e->second;

  return None;
}

Type ConstraintSystem::openUnboundGenericType(
    GenericTypeDecl *decl, Type parentTy, ConstraintLocatorBuilder locator) {
  if (parentTy) {
    parentTy = replaceInferableTypesWithTypeVars(parentTy, locator);
  }

  // Open up the generic type.
  OpenedTypeMap replacements;
  openGeneric(decl->getDeclContext(), decl->getGenericSignature(), locator,
              replacements);

  recordOpenedTypes(locator, replacements);

  if (parentTy) {
    auto subs = parentTy->getContextSubstitutions(decl->getDeclContext());
    for (auto pair : subs) {
      auto found = replacements.find(
        cast<GenericTypeParamType>(pair.first));
      if (found == replacements.end()) {
        // Can happen with invalid generic code.
        continue;
      }

      addConstraint(ConstraintKind::Bind, found->second, pair.second,
                    locator);
    }
  }

  // Map the generic parameters to their corresponding type variables.
  llvm::SmallVector<Type, 2> arguments;
  for (auto gp : decl->getInnermostGenericParamTypes()) {
    auto found = replacements.find(
      cast<GenericTypeParamType>(gp->getCanonicalType()));
    assert(found != replacements.end() &&
           "Missing generic parameter?");
    arguments.push_back(found->second);
  }

  // FIXME: For some reason we can end up with unbound->getDecl()
  // pointing at a generic TypeAliasDecl here. If we find a way to
  // handle generic TypeAliases elsewhere, this can just become a
  // call to BoundGenericType::get().
  return TypeResolution::forContextual(DC, None, /*unboundTyOpener*/ nullptr,
                                       /*placeholderHandler*/ nullptr)
      .applyUnboundGenericArguments(decl, parentTy, SourceLoc(), arguments);
}

static void checkNestedTypeConstraints(ConstraintSystem &cs, Type type,
                                       ConstraintLocatorBuilder locator) {
  // If this is a type defined inside of constrainted extension, let's add all
  // of the generic requirements to the constraint system to make sure that it's
  // something we can use.
  GenericTypeDecl *decl = nullptr;
  Type parentTy;
  SubstitutionMap subMap;

  if (auto *NAT = dyn_cast<TypeAliasType>(type.getPointer())) {
    decl = NAT->getDecl();
    parentTy = NAT->getParent();
    subMap = NAT->getSubstitutionMap();
  } else if (auto *AGT = type->getAs<AnyGenericType>()) {
    decl = AGT->getDecl();
    parentTy = AGT->getParent();
    // the context substitution map is fine here, since we can't be adding more
    // info than that, unlike a typealias
  }

  if (!parentTy)
    return;

  // If this decl is generic, the constraints are handled when the generic
  // parameters are applied, so we don't have to handle them here (which makes
  // getting the right substitution maps easier).
  if (!decl || decl->isGeneric())
    return;

  // struct A<T> {
  //   let foo: [T]
  // }
  //
  // extension A : Codable where T: Codable {
  //   enum CodingKeys: String, CodingKey {
  //     case foo = "foo"
  //   }
  // }
  //
  // Reference to `A.CodingKeys.foo` would point to `A` as an
  // unbound generic type. Conditional requirements would be
  // added when `A` is "opened". Les delay this check until then.
  if (parentTy->hasUnboundGenericType())
    return;

  auto extension = dyn_cast<ExtensionDecl>(decl->getDeclContext());
  if (extension && extension->isConstrainedExtension()) {
    auto contextSubMap = parentTy->getContextSubstitutionMap(
        extension->getParentModule(), extension->getSelfNominalTypeDecl());
    if (!subMap) {
      // The substitution map wasn't set above, meaning we should grab the map
      // for the extension itself.
      subMap = parentTy->getContextSubstitutionMap(extension->getParentModule(),
                                                   extension);
    }

    if (auto signature = decl->getGenericSignature()) {
      cs.openGenericRequirements(
          extension, signature, /*skipProtocolSelfConstraint*/ true, locator,
          [&](Type type) {
            // Why do we look in two substitution maps? We have to use the
            // context substitution map to find types, because we need to
            // avoid thinking about them when handling the constraints, or all
            // the requirements in the signature become tautologies (if the
            // extension has 'T == Int', subMap will map T -> Int, so the
            // requirement becomes Int == Int no matter what the actual types
            // are here). However, we need the conformances for the extension
            // because the requirements might look like `T: P, T.U: Q`, where
            // U is an associated type of protocol P.
            return type.subst(QuerySubstitutionMap{contextSubMap},
                              LookUpConformanceInSubstitutionMap(subMap));
          });
    }
  }

  // And now make sure sure the parent is okay, for things like X<T>.Y.Z.
  checkNestedTypeConstraints(cs, parentTy, locator);
}

Type ConstraintSystem::replaceInferableTypesWithTypeVars(
    Type type, ConstraintLocatorBuilder locator) {
  assert(!type->getCanonicalType()->hasTypeParameter());

  if (!type->hasUnboundGenericType() && !type->hasPlaceholder())
    return type;

  type = type.transform([&](Type type) -> Type {
      if (auto unbound = type->getAs<UnboundGenericType>()) {
        return openUnboundGenericType(unbound->getDecl(), unbound->getParent(),
                                      locator);
      } else if (auto *placeholderTy = type->getAs<PlaceholderType>()) {
        if (auto *placeholderRepr = placeholderTy->getOriginator()
                                        .dyn_cast<PlaceholderTypeRepr *>()) {

          return createTypeVariable(
              getConstraintLocator(
                  locator, LocatorPathElt::PlaceholderType(placeholderRepr)),
              TVO_CanBindToNoEscape | TVO_PrefersSubtypeBinding |
                  TVO_CanBindToHole);
        }
      }

      return type;
    });

  if (!type)
    return ErrorType::get(getASTContext());

  return type;
}

Type ConstraintSystem::openType(Type type, OpenedTypeMap &replacements) {
  assert(!type->hasUnboundGenericType());

  if (!type->hasTypeParameter())
    return type;

  return type.transform([&](Type type) -> Type {
      assert(!type->is<GenericFunctionType>());

      // Replace a generic type parameter with its corresponding type variable.
      if (auto genericParam = type->getAs<GenericTypeParamType>()) {
        auto known = replacements.find(
          cast<GenericTypeParamType>(genericParam->getCanonicalType()));
        // FIXME: This should be an assert, however protocol generic signatures
        // drop outer generic parameters.
        // assert(known != replacements.end());
        if (known == replacements.end())
          return ErrorType::get(getASTContext());
        return known->second;
      }

      return type;
    });
}

FunctionType *ConstraintSystem::openFunctionType(
       AnyFunctionType *funcType,
       ConstraintLocatorBuilder locator,
       OpenedTypeMap &replacements,
       DeclContext *outerDC) {
  if (auto *genericFn = funcType->getAs<GenericFunctionType>()) {
    auto signature = genericFn->getGenericSignature();

    openGenericParameters(outerDC, signature, replacements, locator);

    openGenericRequirements(
        outerDC, signature, /*skipProtocolSelfConstraint=*/false, locator,
        [&](Type type) -> Type { return openType(type, replacements); });

    funcType = genericFn->substGenericArgs(
        [&](Type type) { return openType(type, replacements); });
  }

  return funcType->castTo<FunctionType>();
}

Optional<Type> ConstraintSystem::isArrayType(Type type) {
  if (auto boundStruct = type->getAs<BoundGenericStructType>()) {
    if (boundStruct->getDecl() == type->getASTContext().getArrayDecl())
      return boundStruct->getGenericArgs()[0];
  }

  return None;
}

Optional<std::pair<Type, Type>> ConstraintSystem::isDictionaryType(Type type) {
  if (auto boundStruct = type->getAs<BoundGenericStructType>()) {
    if (boundStruct->getDecl() == type->getASTContext().getDictionaryDecl()) {
      auto genericArgs = boundStruct->getGenericArgs();
      return std::make_pair(genericArgs[0], genericArgs[1]);
    }
  }

  return None;
}

Optional<Type> ConstraintSystem::isSetType(Type type) {
  if (auto boundStruct = type->getAs<BoundGenericStructType>()) {
    if (boundStruct->getDecl() == type->getASTContext().getSetDecl())
      return boundStruct->getGenericArgs()[0];
  }

  return None;
}

bool ConstraintSystem::isAnyHashableType(Type type) {
  if (auto st = type->getAs<StructType>()) {
    auto &ctx = type->getASTContext();
    return st->getDecl() == ctx.getAnyHashableDecl();
  }

  return false;
}

Type ConstraintSystem::getFixedTypeRecursive(Type type,
                                             TypeMatchOptions &flags,
                                             bool wantRValue) const {

  if (wantRValue)
    type = type->getRValueType();

  if (auto depMemType = type->getAs<DependentMemberType>()) {
    if (!depMemType->getBase()->isTypeVariableOrMember()) return type;

    // FIXME: Perform a more limited simplification?
    Type newType = simplifyType(type);
    if (newType.getPointer() == type.getPointer()) return type;

    // Once we've simplified a dependent member type, we need to generate a
    // new constraint.
    flags |= TMF_GenerateConstraints;

    return getFixedTypeRecursive(newType, flags, wantRValue);
  }

  if (auto typeVar = type->getAs<TypeVariableType>()) {
    if (auto fixed = getFixedType(typeVar))
      return getFixedTypeRecursive(fixed, flags, wantRValue);

    return getRepresentative(typeVar);
  }

  return type;
}

TypeVariableType *ConstraintSystem::isRepresentativeFor(
    TypeVariableType *typeVar, ConstraintLocator::PathElementKind kind) const {
  // We only attempt to look for this if type variable is
  // a representative.
  if (getRepresentative(typeVar) != typeVar)
    return nullptr;

  auto &CG = getConstraintGraph();
  auto result = CG.lookupNode(typeVar);
  auto equivalence = result.first.getEquivalenceClass();
  auto member = llvm::find_if(equivalence, [=](TypeVariableType *eq) {
    auto *loc = eq->getImpl().getLocator();
    if (!loc)
      return false;

    auto path = loc->getPath();
    return !path.empty() && path.back().getKind() == kind;
  });

  if (member == equivalence.end())
    return nullptr;

  return *member;
}

static Optional<std::pair<VarDecl *, Type>>
getPropertyWrapperInformationFromOverload(
    SelectedOverload resolvedOverload, DeclContext *DC,
    llvm::function_ref<Optional<std::pair<VarDecl *, Type>>(VarDecl *)>
        getInformation) {
  if (auto *decl =
          dyn_cast_or_null<VarDecl>(resolvedOverload.choice.getDeclOrNull())) {
    if (auto declInformation = getInformation(decl)) {
      Type type;
      VarDecl *memberDecl;
      std::tie(memberDecl, type) = *declInformation;
      if (Type baseType = resolvedOverload.choice.getBaseType()) {
        type =
            baseType->getTypeOfMember(DC->getParentModule(), memberDecl, type);
      }
      return std::make_pair(decl, type);
    }
  }
  return None;
}

Optional<std::pair<VarDecl *, Type>>
ConstraintSystem::getPropertyWrapperProjectionInfo(
    SelectedOverload resolvedOverload) {
  return getPropertyWrapperInformationFromOverload(
      resolvedOverload, DC,
      [](VarDecl *decl) -> Optional<std::pair<VarDecl *, Type>> {
        if (!decl->hasAttachedPropertyWrapper())
          return None;

        auto projectionVar = decl->getPropertyWrapperProjectionVar();
        if (!projectionVar)
          return None;

        return std::make_pair(projectionVar,
                              projectionVar->getInterfaceType());
      });
}

Optional<std::pair<VarDecl *, Type>>
ConstraintSystem::getPropertyWrapperInformation(
    SelectedOverload resolvedOverload) {
  return getPropertyWrapperInformationFromOverload(
      resolvedOverload, DC,
      [](VarDecl *decl) -> Optional<std::pair<VarDecl *, Type>> {
        if (!decl->hasAttachedPropertyWrapper())
          return None;

        return std::make_pair(decl,
                              decl->getPropertyWrapperBackingPropertyType());
      });
}

Optional<std::pair<VarDecl *, Type>>
ConstraintSystem::getWrappedPropertyInformation(
    SelectedOverload resolvedOverload) {
  return getPropertyWrapperInformationFromOverload(
      resolvedOverload, DC,
      [](VarDecl *decl) -> Optional<std::pair<VarDecl *, Type>> {
        if (auto wrapped = decl->getOriginalWrappedProperty())
          return std::make_pair(decl, wrapped->getInterfaceType());

        return None;
      });
}

/// Does a var or subscript produce an l-value?
///
/// \param baseType - the type of the base on which this object
///   is being accessed; must be null if and only if this is not
///   a type member
static bool
doesStorageProduceLValue(AbstractStorageDecl *storage, Type baseType,
                         DeclContext *useDC,
                         ConstraintLocator *memberLocator = nullptr) {
  const DeclRefExpr *base = nullptr;
  if (memberLocator) {
    if (auto *const E = getAsExpr(memberLocator->getAnchor())) {
      if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
        base = dyn_cast<DeclRefExpr>(MRE->getBase());
      } else if (auto *UDE = dyn_cast<UnresolvedDotExpr>(E)) {
        base = dyn_cast<DeclRefExpr>(UDE->getBase());
      }
    }
  }

  // Unsettable storage decls always produce rvalues.
  if (!storage->isSettable(useDC, base))
    return false;

  if (!storage->isSetterAccessibleFrom(useDC))
    return false;

  // If there is no base, or the base is an lvalue, then a reference
  // produces an lvalue.
  if (!baseType || baseType->is<LValueType>())
    return true;

  // The base is an rvalue type. The only way an accessor can
  // produce an lvalue is if we have a property where both the
  // getter and setter are nonmutating.
  return (!storage->isGetterMutating() &&
          !storage->isSetterMutating());
}

Type ConstraintSystem::getUnopenedTypeOfReference(
    VarDecl *value, Type baseType, DeclContext *UseDC,
    ConstraintLocator *memberLocator, bool wantInterfaceType) {
  return ConstraintSystem::getUnopenedTypeOfReference(
      value, baseType, UseDC,
      [&](VarDecl *var) -> Type {
        if (Type type = getTypeIfAvailable(var))
          return type;

        if (!var->hasInterfaceType()) {
          return ErrorType::get(getASTContext());
        }

        return wantInterfaceType ? var->getInterfaceType() : var->getType();
      },
      memberLocator, wantInterfaceType);
}

Type ConstraintSystem::getUnopenedTypeOfReference(
    VarDecl *value, Type baseType, DeclContext *UseDC,
    llvm::function_ref<Type(VarDecl *)> getType,
    ConstraintLocator *memberLocator, bool wantInterfaceType) {
  Type requestedType =
      getType(value)->getWithoutSpecifierType()->getReferenceStorageReferent();

  // If we're dealing with contextual types, and we referenced this type from
  // a different context, map the type.
  if (!wantInterfaceType && requestedType->hasArchetype()) {
    auto valueDC = value->getDeclContext();
    if (valueDC != UseDC) {
      Type mapped = requestedType->mapTypeOutOfContext();
      requestedType = UseDC->mapTypeIntoContext(mapped);
    }
  }

  // Qualify storage declarations with an lvalue when appropriate.
  // Otherwise, they yield rvalues (and the access must be a load).
  if (doesStorageProduceLValue(value, baseType, UseDC, memberLocator) &&
      !requestedType->hasError()) {
    return LValueType::get(requestedType);
  }

  return requestedType;
}

void ConstraintSystem::recordOpenedTypes(
       ConstraintLocatorBuilder locator,
       const OpenedTypeMap &replacements) {
  if (replacements.empty())
    return;

  // If the last path element is an archetype or associated type, ignore it.
  SmallVector<LocatorPathElt, 2> pathElts;
  auto anchor = locator.getLocatorParts(pathElts);
  if (!pathElts.empty() &&
      pathElts.back().getKind() == ConstraintLocator::GenericParameter)
    return;

  // If the locator is empty, ignore it.
  if (!anchor && pathElts.empty())
    return;

  ConstraintLocator *locatorPtr = getConstraintLocator(locator);
  assert(locatorPtr && "No locator for opened types?");
#if false
  assert(std::find_if(OpenedTypes.begin(), OpenedTypes.end(),
                      [&](const std::pair<ConstraintLocator *,
                          ArrayRef<OpenedType>> &entry) {
                        return entry.first == locatorPtr;
                      }) == OpenedTypes.end() &&
         "already registered opened types for this locator");
#endif

  OpenedType* openedTypes
    = Allocator.Allocate<OpenedType>(replacements.size());
  std::copy(replacements.begin(), replacements.end(), openedTypes);
  OpenedTypes.push_back({ locatorPtr,
    llvm::makeArrayRef(openedTypes,
                       replacements.size()) });
}

/// Determine how many levels of argument labels should be removed from the
/// function type when referencing the given declaration.
static unsigned getNumRemovedArgumentLabels(ValueDecl *decl,
                                            bool isCurriedInstanceReference,
                                            FunctionRefKind functionRefKind) {
  unsigned numParameterLists = decl->getNumCurryLevels();
  switch (functionRefKind) {
  case FunctionRefKind::Unapplied:
  case FunctionRefKind::Compound:
    // Always remove argument labels from unapplied references and references
    // that use a compound name.
    return numParameterLists;

  case FunctionRefKind::SingleApply:
    // If we have fewer than two parameter lists, leave the labels.
    if (numParameterLists < 2)
      return 0;

    // If this is a curried reference to an instance method, where 'self' is
    // being applied, e.g., "ClassName.instanceMethod(self)", remove the
    // argument labels from the resulting function type. The 'self' parameter is
    // always unlabeled, so this operation is a no-op for the actual application.
    return isCurriedInstanceReference ? numParameterLists : 1;

  case FunctionRefKind::DoubleApply:
    // Never remove argument labels from a double application.
    return 0;
  }

  llvm_unreachable("Unhandled FunctionRefKind in switch.");
}

/// Replaces property wrapper types in the parameter list of the given function type
/// with the wrapped-value or projected-value types (depending on argument label).
static FunctionType *
unwrapPropertyWrapperParameterTypes(ConstraintSystem &cs, AbstractFunctionDecl *funcDecl,
                                    FunctionRefKind functionRefKind, FunctionType *functionType,
                                    ConstraintLocatorBuilder locator) {
  // Only apply property wrappers to unapplied references to functions.
  if (!(functionRefKind == FunctionRefKind::Compound ||
        functionRefKind == FunctionRefKind::Unapplied)) {
    return functionType;
  }

  auto *paramList = funcDecl->getParameters();
  auto paramTypes = functionType->getParams();
  SmallVector<AnyFunctionType::Param, 4> adjustedParamTypes;

  DeclNameLoc nameLoc;
  auto *ref = getAsExpr(locator.getAnchor());
  if (auto *declRef = dyn_cast<DeclRefExpr>(ref)) {
    nameLoc = declRef->getNameLoc();
  } else if (auto *dotExpr = dyn_cast<UnresolvedDotExpr>(ref)) {
    nameLoc = dotExpr->getNameLoc();
  } else if (auto *overloadedRef = dyn_cast<OverloadedDeclRefExpr>(ref)) {
    nameLoc = overloadedRef->getNameLoc();
  } else if (auto *memberExpr = dyn_cast<UnresolvedMemberExpr>(ref)) {
    nameLoc = memberExpr->getNameLoc();
  }

  for (unsigned i : indices(*paramList)) {
    Identifier argLabel;
    if (functionRefKind == FunctionRefKind::Compound) {
      auto &context = cs.getASTContext();
      auto argLabelLoc = nameLoc.getArgumentLabelLoc(i);
      auto argLabelToken = Lexer::getTokenAtLocation(context.SourceMgr, argLabelLoc);
      argLabel = context.getIdentifier(argLabelToken.getText());
    }

    auto *paramDecl = paramList->get(i);
    if (!paramDecl->hasAttachedPropertyWrapper() && !argLabel.hasDollarPrefix()) {
      adjustedParamTypes.push_back(paramTypes[i]);
      continue;
    }

    auto *wrappedType = cs.createTypeVariable(cs.getConstraintLocator(locator), 0);
    auto paramType = paramTypes[i].getParameterType();
    auto paramLabel = paramTypes[i].getLabel();
    auto paramInternalLabel = paramTypes[i].getInternalLabel();
    adjustedParamTypes.push_back(AnyFunctionType::Param(
        wrappedType, paramLabel, ParameterTypeFlags(), paramInternalLabel));
    cs.applyPropertyWrapperToParameter(paramType, wrappedType, paramDecl, argLabel,
                                       ConstraintKind::Equal, locator);
  }

  return FunctionType::get(adjustedParamTypes, functionType->getResult(),
                           functionType->getExtInfo());
}

/// Determine whether the given locator is for a witness or requirement.
static bool isRequirementOrWitness(const ConstraintLocatorBuilder &locator) {
  if (auto last = locator.last()) {
    return last->getKind() == ConstraintLocator::ProtocolRequirement ||
           last->getKind() == ConstraintLocator::Witness;
  }

  return false;
}

std::pair<Type, Type>
ConstraintSystem::getTypeOfReference(ValueDecl *value,
                                     FunctionRefKind functionRefKind,
                                     ConstraintLocatorBuilder locator,
                                     DeclContext *useDC) {
  if (value->getDeclContext()->isTypeContext() && isa<FuncDecl>(value)) {
    // Unqualified lookup can find operator names within nominal types.
    auto func = cast<FuncDecl>(value);
    assert(func->isOperator() && "Lookup should only find operators");

    OpenedTypeMap replacements;

    AnyFunctionType *funcType = func->getInterfaceType()
        ->castTo<AnyFunctionType>();
    if (!isRequirementOrWitness(locator))
      funcType = applyGlobalActorType(funcType, func, useDC);
    auto openedType = openFunctionType(
        funcType, locator, replacements, func->getDeclContext());

    // If we opened up any type variables, record the replacements.
    recordOpenedTypes(locator, replacements);

    // If this is a method whose result type is dynamic Self, replace
    // DynamicSelf with the actual object type.
    if (func->getResultInterfaceType()->hasDynamicSelfType()) {
      auto params = openedType->getParams();
      assert(params.size() == 1);
      Type selfTy = params.front().getPlainType()->getMetatypeInstanceType();
      openedType = openedType->replaceCovariantResultType(selfTy, 2)
                        ->castTo<FunctionType>();
    }

    // The reference implicitly binds 'self'.
    return {openedType, openedType->getResult()};
  }

  // Unqualified reference to a local or global function.
  if (auto funcDecl = dyn_cast<AbstractFunctionDecl>(value)) {
    OpenedTypeMap replacements;

    auto funcType = funcDecl->getInterfaceType()->castTo<AnyFunctionType>();
    auto numLabelsToRemove = getNumRemovedArgumentLabels(
        funcDecl, /*isCurriedInstanceReference=*/false, functionRefKind);

    if (!isRequirementOrWitness(locator))
      funcType = applyGlobalActorType(funcType, funcDecl, useDC);

    auto openedType = openFunctionType(funcType, locator, replacements,
                                       funcDecl->getDeclContext())
                          ->removeArgumentLabels(numLabelsToRemove);
    openedType = unwrapPropertyWrapperParameterTypes(*this, funcDecl, functionRefKind,
                                                     openedType->getAs<FunctionType>(),
                                                     locator);

    // If we opened up any type variables, record the replacements.
    recordOpenedTypes(locator, replacements);

    return { openedType, openedType };
  }

  // Unqualified reference to a type.
  if (auto typeDecl = dyn_cast<TypeDecl>(value)) {
    // Resolve the reference to this type declaration in our current context.
    auto type =
        TypeResolution::forContextual(useDC, TypeResolverContext::InExpression,
                                      /*unboundTyOpener*/ nullptr,
                                      /*placeholderHandler*/ nullptr)
            .resolveTypeInContext(typeDecl, /*foundDC*/ nullptr,
                                  /*isSpecialized=*/false);

    checkNestedTypeConstraints(*this, type, locator);

    // Convert any placeholder types and open generics.
    type = replaceInferableTypesWithTypeVars(type, locator);

    // Module types are not wrapped in metatypes.
    if (type->is<ModuleType>())
      return { type, type };

    // If it's a value reference, refer to the metatype.
    type = MetatypeType::get(type);
    return { type, type };
  }

  // Only remaining case: unqualified reference to a property.
  auto *varDecl = cast<VarDecl>(value);

  // Determine the type of the value, opening up that type if necessary.
  bool wantInterfaceType = !varDecl->getDeclContext()->isLocalContext();
  Type valueType =
      getUnopenedTypeOfReference(varDecl, Type(), useDC, /*base=*/nullptr,
                                 wantInterfaceType);

  assert(!valueType->hasUnboundGenericType() &&
         !valueType->hasTypeParameter());
  return { valueType, valueType };
}

/// Bind type variables for archetypes that are determined from
/// context.
///
/// For example, if we are opening a generic function type
/// nested inside another function, we must bind the outer
/// generic parameters to context archetypes, because the
/// nested function can "capture" these outer generic parameters.
///
/// Another case where this comes up is if a generic type is
/// nested inside a function. We don't support codegen for this
/// yet, but again we need to bind any outer generic parameters
/// to context archetypes, because they're not free.
///
/// A final case we have to handle, even though it is invalid, is
/// when a type is nested inside another protocol. We bind the
/// protocol type variable for the protocol Self to an unresolved
/// type, since it will conform to anything. This of course makes
/// no sense, but we can't leave the type variable dangling,
/// because then we crash later.
///
/// If we ever do want to allow nominal types to be nested inside
/// protocols, the key is to set their declared type to a
/// NominalType whose parent is the 'Self' generic parameter, and
/// not the ProtocolType. Then, within a conforming type context,
/// we can 'reparent' the NominalType to that concrete type, and
/// resolve references to associated types inside that NominalType
/// relative to this concrete 'Self' type.
///
/// Also, of course IRGen would have to know to store the 'Self'
/// metadata as an extra hidden generic parameter in the metadata
/// of such a type, etc.
static void bindArchetypesFromContext(
    ConstraintSystem &cs,
    DeclContext *outerDC,
    ConstraintLocator *locatorPtr,
    const OpenedTypeMap &replacements) {

  auto bindPrimaryArchetype = [&](Type paramTy, Type contextTy) {
    auto found = replacements.find(cast<GenericTypeParamType>(
                                     paramTy->getCanonicalType()));

    // We might not have a type variable for this generic parameter
    // because either we're opening up an UnboundGenericType,
    // in which case we only want to infer the innermost generic
    // parameters, or because this generic parameter was constrained
    // away into a concrete type.
    if (found != replacements.end()) {
      auto typeVar = found->second;
      cs.addConstraint(ConstraintKind::Bind, typeVar, contextTy,
                       locatorPtr);
    }
  };

  // Find the innermost non-type context.
  for (const auto *parentDC = outerDC;
       !parentDC->isModuleScopeContext();
       parentDC = parentDC->getParent()) {
    if (parentDC->isTypeContext()) {
      if (parentDC != outerDC && parentDC->getSelfProtocolDecl()) {
        auto selfTy = parentDC->getSelfInterfaceType();
        auto contextTy = cs.getASTContext().TheUnresolvedType;
        bindPrimaryArchetype(selfTy, contextTy);
      }
      continue;
    }

    // If it's not generic, there's nothing to do.
    auto genericSig = parentDC->getGenericSignatureOfContext();
    if (!genericSig)
      break;

    for (auto *paramTy : genericSig->getGenericParams()) {
      Type contextTy = cs.DC->mapTypeIntoContext(paramTy);
      bindPrimaryArchetype(paramTy, contextTy);
    }

    break;
  }
}

void ConstraintSystem::openGeneric(
       DeclContext *outerDC,
       GenericSignature sig,
       ConstraintLocatorBuilder locator,
       OpenedTypeMap &replacements) {
  if (!sig)
    return;

  openGenericParameters(outerDC, sig, replacements, locator);

  // Add the requirements as constraints.
  openGenericRequirements(
      outerDC, sig, /*skipProtocolSelfConstraint=*/false, locator,
      [&](Type type) { return openType(type, replacements); });
}

void ConstraintSystem::openGenericParameters(DeclContext *outerDC,
                                             GenericSignature sig,
                                             OpenedTypeMap &replacements,
                                             ConstraintLocatorBuilder locator) {
  assert(sig);

  // Create the type variables for the generic parameters.
  for (auto gp : sig->getGenericParams()) {
    auto *paramLocator = getConstraintLocator(
        locator.withPathElement(LocatorPathElt::GenericParameter(gp)));

    auto typeVar = createTypeVariable(paramLocator, TVO_PrefersSubtypeBinding |
                                                    TVO_CanBindToHole);
    auto result = replacements.insert(std::make_pair(
        cast<GenericTypeParamType>(gp->getCanonicalType()), typeVar));

    assert(result.second);
    (void)result;
  }

  auto *baseLocator = getConstraintLocator(
      locator.withPathElement(LocatorPathElt::OpenedGeneric(sig)));

  bindArchetypesFromContext(*this, outerDC, baseLocator, replacements);
}

void ConstraintSystem::openGenericRequirements(
    DeclContext *outerDC, GenericSignature signature,
    bool skipProtocolSelfConstraint, ConstraintLocatorBuilder locator,
    llvm::function_ref<Type(Type)> substFn) {
  auto requirements = signature->getRequirements();
  for (unsigned pos = 0, n = requirements.size(); pos != n; ++pos) {
    const auto &req = requirements[pos];

    Optional<Requirement> openedReq;
    auto openedFirst = substFn(req.getFirstType());

    auto kind = req.getKind();
    switch (kind) {
    case RequirementKind::Conformance: {
      auto protoDecl = req.getProtocolDecl();
      // Determine whether this is the protocol 'Self' constraint we should
      // skip.
      if (skipProtocolSelfConstraint && protoDecl == outerDC &&
          protoDecl->getSelfInterfaceType()->isEqual(req.getFirstType()))
        continue;
      openedReq = Requirement(kind, openedFirst, req.getSecondType());
      break;
    }
    case RequirementKind::Superclass:
    case RequirementKind::SameType:
      openedReq = Requirement(kind, openedFirst, substFn(req.getSecondType()));
      break;
    case RequirementKind::Layout:
      openedReq = Requirement(kind, openedFirst, req.getLayoutConstraint());
      break;
    }

    auto openedGenericLoc =
        locator.withPathElement(LocatorPathElt::OpenedGeneric(signature));
    addConstraint(*openedReq,
                  openedGenericLoc.withPathElement(
                      LocatorPathElt::TypeParameterRequirement(pos, kind)));
  }
}

/// Add the constraint on the type used for the 'Self' type for a member
/// reference.
///
/// \param cs The constraint system.
///
/// \param objectTy The type of the object that we're using to access the
/// member.
///
/// \param selfTy The instance type of the context in which the member is
/// declared.
static void addSelfConstraint(ConstraintSystem &cs, Type objectTy, Type selfTy,
                              ConstraintLocatorBuilder locator){
  assert(!selfTy->is<ProtocolType>());

  // Otherwise, use a subtype constraint for classes to cope with inheritance.
  if (selfTy->getClassOrBoundGenericClass()) {
    cs.addConstraint(ConstraintKind::Subtype, objectTy, selfTy,
                     cs.getConstraintLocator(locator));
    return;
  }

  // Otherwise, the types must be equivalent.
  cs.addConstraint(ConstraintKind::Bind, objectTy, selfTy,
                   cs.getConstraintLocator(locator));
}

Type constraints::getDynamicSelfReplacementType(
    Type baseObjTy, const ValueDecl *member, ConstraintLocator *memberLocator) {
  // Constructions must always have their dynamic 'Self' result type replaced
  // with the base object type, 'super' or not.
  if (isa<ConstructorDecl>(member))
    return baseObjTy;

  const SuperRefExpr *SuperExpr = nullptr;
  if (auto *E = getAsExpr(memberLocator->getAnchor())) {
    if (auto *LE = dyn_cast<LookupExpr>(E)) {
      SuperExpr = dyn_cast<SuperRefExpr>(LE->getBase());
    } else if (auto *UDE = dyn_cast<UnresolvedDotExpr>(E)) {
      SuperExpr = dyn_cast<SuperRefExpr>(UDE->getBase());
    }
  }

  // For anything else that isn't 'super', we want it to be the base
  // object type.
  if (!SuperExpr)
    return baseObjTy;

  // 'super' is special in that we actually want dynamic 'Self' to behave
  // as if the base were 'self'.
  const auto *selfDecl = SuperExpr->getSelf();
  return selfDecl->getDeclContext()
      ->getInnermostTypeContext()
      ->mapTypeIntoContext(selfDecl->getInterfaceType())
      ->getMetatypeInstanceType();
}

std::pair<Type, Type>
ConstraintSystem::getTypeOfMemberReference(
    Type baseTy, ValueDecl *value, DeclContext *useDC,
    bool isDynamicResult,
    FunctionRefKind functionRefKind,
    ConstraintLocator *locator,
    OpenedTypeMap *replacementsPtr) {
  // Figure out the instance type used for the base.
  Type resolvedBaseTy = getFixedTypeRecursive(baseTy, /*wantRValue=*/true);

  // If the base is a module type, just use the type of the decl.
  if (resolvedBaseTy->is<ModuleType>()) {
    return getTypeOfReference(value, functionRefKind, locator, useDC);
  }

  // Check to see if the self parameter is applied, in which case we'll want to
  // strip it off later.
  auto hasAppliedSelf = doesMemberRefApplyCurriedSelf(resolvedBaseTy, value);

  auto baseObjTy = resolvedBaseTy->getMetatypeInstanceType();
  FunctionType::Param baseObjParam(baseObjTy);

  // Indicates whether this is a valid reference to a static member on a
  // protocol metatype. Such a reference is only valid if performed through
  // leading dot syntax e.g. `foo(.bar)` where implicit base is a protocol
  // metatype and `bar` is static member declared in a protocol  or its
  // extension.
  bool isStaticMemberRefOnProtocol = false;
  if (resolvedBaseTy->is<MetatypeType>() && baseObjTy->isExistentialType() &&
      value->isStatic()) {
    isStaticMemberRefOnProtocol =
        locator->isLastElement<LocatorPathElt::UnresolvedMember>();
  }

  if (auto *typeDecl = dyn_cast<TypeDecl>(value)) {
    assert(!isa<ModuleDecl>(typeDecl) && "Nested module?");

    auto memberTy = TypeChecker::substMemberTypeWithBase(DC->getParentModule(),
                                                         typeDecl, baseObjTy);

    checkNestedTypeConstraints(*this, memberTy, locator);

    // Convert any placeholders and open any generics.
    memberTy = replaceInferableTypesWithTypeVars(memberTy, locator);

    // Wrap it in a metatype.
    memberTy = MetatypeType::get(memberTy);

    auto openedType = FunctionType::get({baseObjParam}, memberTy);
    return { openedType, memberTy };
  }

  // Figure out the declaration context to use when opening this type.
  DeclContext *innerDC = value->getInnermostDeclContext();
  DeclContext *outerDC = value->getDeclContext();

  // Open the type of the generic function or member of a generic type.
  Type openedType;
  OpenedTypeMap localReplacements;
  auto &replacements = replacementsPtr ? *replacementsPtr : localReplacements;
  unsigned numRemovedArgumentLabels = getNumRemovedArgumentLabels(
      value, /*isCurriedInstanceReference*/ !hasAppliedSelf, functionRefKind);

  AnyFunctionType *funcType;

  if (isa<AbstractFunctionDecl>(value) ||
      isa<EnumElementDecl>(value)) {
    // This is the easy case.
    funcType = value->getInterfaceType()->castTo<AnyFunctionType>();

    if (!isRequirementOrWitness(locator))
      funcType = applyGlobalActorType(funcType, value, useDC);
  } else {
    // For a property, build a type (Self) -> PropType.
    // For a subscript, build a type (Self) -> (Indices...) -> ElementType.
    //
    // If the access is mutating, wrap the storage type in an lvalue type.
    Type refType;
    if (auto *subscript = dyn_cast<SubscriptDecl>(value)) {
      auto elementTy = subscript->getElementInterfaceType();

      if (doesStorageProduceLValue(subscript, baseTy, useDC, locator))
        elementTy = LValueType::get(elementTy);

      // See ConstraintSystem::resolveOverload() -- optional and dynamic
      // subscripts are a special case, because the optionality is
      // applied to the result type and not the type of the reference.
      if (!isRequirementOrWitness(locator)) {
        if (subscript->getAttrs().hasAttribute<OptionalAttr>() ||
            isDynamicResult)
          elementTy = OptionalType::get(elementTy->getRValueType());
      }

      auto indices = subscript->getInterfaceType()
                              ->castTo<AnyFunctionType>()->getParams();
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo info;
      refType = FunctionType::get(indices, elementTy, info);
    } else {
      refType = getUnopenedTypeOfReference(cast<VarDecl>(value), baseTy, useDC,
                                           locator,
                                           /*wantInterfaceType=*/true);
    }

    auto selfTy = outerDC->getSelfInterfaceType();

    // If this is a reference to an instance member that applies self,
    // where self is a value type and the base type is an lvalue, wrap it in an
    // inout type.
    auto selfFlags = ParameterTypeFlags();
    if (value->isInstanceMember() && hasAppliedSelf &&
        !outerDC->getDeclaredInterfaceType()->hasReferenceSemantics() &&
        baseTy->is<LValueType>() &&
        !selfTy->hasError())
      selfFlags = selfFlags.withInOut(true);

    // If the storage is generic, add a generic signature.
    FunctionType::Param selfParam(selfTy, Identifier(), selfFlags);
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    if (auto sig = innerDC->getGenericSignatureOfContext()) {
      GenericFunctionType::ExtInfo info;
      funcType = GenericFunctionType::get(sig, {selfParam}, refType, info);
    } else {
      FunctionType::ExtInfo info;
      funcType = FunctionType::get({selfParam}, refType, info);
    }
  }

  // While opening member function type, let's delay opening requirements
  // to allow contextual types to affect the situation.
  if (auto *genericFn = funcType->getAs<GenericFunctionType>()) {
    openGenericParameters(outerDC, genericFn->getGenericSignature(),
                          replacements, locator);

    openedType = genericFn->substGenericArgs(
        [&](Type type) { return openType(type, replacements); });
  } else {
    openedType = funcType;
  }

  openedType = openedType->removeArgumentLabels(numRemovedArgumentLabels);

  // If we are looking at a member of an existential, open the existential.
  Type baseOpenedTy = baseObjTy;

  if (isStaticMemberRefOnProtocol) {
    // In diagnostic mode, let's not try to replace base type
    // if there is already a known issue associated with this
    // reference e.g. it might be incorrect initializer call
    // or result type is invalid.
    if (!(shouldAttemptFixes() && hasFixFor(getConstraintLocator(locator)))) {
      if (auto concreteSelf =
              getConcreteReplacementForProtocolSelfType(value)) {
        // Concrete type replacing `Self` could be generic, so we need
        // to make sure that it's opened before use.
        baseOpenedTy = openType(concreteSelf, replacements);
        baseObjTy = baseOpenedTy;
      }
    }
  } else if (baseObjTy->isExistentialType()) {
    auto openedArchetype = OpenedArchetypeType::get(baseObjTy);
    OpenedExistentialTypes.push_back(
        {getConstraintLocator(locator), openedArchetype});
    baseOpenedTy = openedArchetype;
  }

  // Constrain the 'self' object type.
  auto openedParams = openedType->castTo<FunctionType>()->getParams();
  assert(openedParams.size() == 1);

  Type selfObjTy = openedParams.front().getPlainType()->getMetatypeInstanceType();
  if (outerDC->getSelfProtocolDecl()) {
    // For a protocol, substitute the base object directly. We don't need a
    // conformance constraint because we wouldn't have found the declaration
    // if it didn't conform.
    addConstraint(ConstraintKind::Bind, baseOpenedTy, selfObjTy,
                  getConstraintLocator(locator));
  } else if (!isDynamicResult) {
    addSelfConstraint(*this, baseOpenedTy, selfObjTy, locator);
  }

  // Open generic requirements after self constraint has been
  // applied and contextual types have been propagated. This
  // helps diagnostics because instead of self type conversion
  // failing we'll get a generic requirement constraint failure
  // if mismatch is related to generic parameters which is much
  // easier to diagnose.
  if (auto *genericFn = funcType->getAs<GenericFunctionType>()) {
    openGenericRequirements(
        outerDC, genericFn->getGenericSignature(),
        /*skipProtocolSelfConstraint=*/true, locator,
        [&](Type type) { return openType(type, replacements); });
  }

  if (auto *funcDecl = dyn_cast<AbstractFunctionDecl>(value)) {
    auto *fullFunctionType = openedType->getAs<AnyFunctionType>();

    // Strip off the 'self' parameter
    auto *functionType = fullFunctionType->getResult()->getAs<FunctionType>();
    functionType = unwrapPropertyWrapperParameterTypes(*this, funcDecl, functionRefKind,
                                                       functionType, locator);
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo info;
    openedType =
        FunctionType::get(fullFunctionType->getParams(), functionType, info);
  }

  // Compute the type of the reference.
  Type type = openedType;

  // Cope with dynamic 'Self'.
  if (!outerDC->getSelfProtocolDecl()) {
    const auto replacementTy =
        getDynamicSelfReplacementType(baseObjTy, value, locator);

    if (auto func = dyn_cast<AbstractFunctionDecl>(value)) {
      if (func->hasDynamicSelfResult() &&
          !baseObjTy->getOptionalObjectType()) {
        type = type->replaceCovariantResultType(replacementTy, 2);
      }
    } else if (auto *decl = dyn_cast<SubscriptDecl>(value)) {
      if (decl->getElementInterfaceType()->hasDynamicSelfType()) {
        type = type->replaceCovariantResultType(replacementTy, 2);
      }
    } else if (auto *decl = dyn_cast<VarDecl>(value)) {
      if (decl->getValueInterfaceType()->hasDynamicSelfType()) {
        type = type->replaceCovariantResultType(replacementTy, 1);
      }
    }
  }

  if (hasAppliedSelf) {
    // For a static member referenced through a metatype or an instance
    // member referenced through an instance, strip off the 'self'.
    type = type->castTo<FunctionType>()->getResult();
  } else {
    // For an unbound instance method reference, replace the 'Self'
    // parameter with the base type.
    type = type->replaceSelfParameterType(baseObjTy);
  }

  // When accessing protocol members with an existential base, replace
  // the 'Self' type parameter with the existential type, since formally
  // the access will operate on existentials and not type parameters.
  if (!isDynamicResult &&
      baseObjTy->isExistentialType() &&
      outerDC->getSelfProtocolDecl()) {
    auto selfTy = replacements[
      cast<GenericTypeParamType>(outerDC->getSelfInterfaceType()
                                 ->getCanonicalType())];
    type = type.transform([&](Type t) -> Type {
      if (t->is<TypeVariableType>())
        if (t->isEqual(selfTy))
          return baseObjTy;
      if (auto *metatypeTy = t->getAs<MetatypeType>())
        if (metatypeTy->getInstanceType()->isEqual(selfTy))
          return ExistentialMetatypeType::get(baseObjTy);
      return t;
    });
  }

  // Construct an idealized parameter type of the initializer associated
  // with object literal, which generally simplifies the first label
  // (e.g. "colorLiteralRed:") by stripping all the redundant stuff about
  // literals (leaving e.g. "red:").
  {
    auto anchor = locator->getAnchor();
    if (auto *OLE = getAsExpr<ObjectLiteralExpr>(anchor)) {
      auto fnType = type->castTo<FunctionType>();

      SmallVector<AnyFunctionType::Param, 4> params(fnType->getParams().begin(),
                                                    fnType->getParams().end());

      switch (OLE->getLiteralKind()) {
      case ObjectLiteralExpr::colorLiteral:
        params[0] = params[0].withLabel(Context.getIdentifier("red"));
        break;

      case ObjectLiteralExpr::fileLiteral:
      case ObjectLiteralExpr::imageLiteral:
        params[0] = params[0].withLabel(Context.getIdentifier("resourceName"));
        break;
      }

      type =
          FunctionType::get(params, fnType->getResult(), fnType->getExtInfo());
    }
  }

  // If we opened up any type variables, record the replacements.
  recordOpenedTypes(locator, replacements);

  return { openedType, type };
}

Type ConstraintSystem::getEffectiveOverloadType(ConstraintLocator *locator,
                                                const OverloadChoice &overload,
                                                bool allowMembers,
                                                DeclContext *useDC) {
  switch (overload.getKind()) {
  case OverloadChoiceKind::Decl:
    // Declaration choices are handled below.
    break;

  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaDynamic:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
  case OverloadChoiceKind::DynamicMemberLookup:
  case OverloadChoiceKind::KeyPathDynamicMemberLookup:
  case OverloadChoiceKind::KeyPathApplication:
  case OverloadChoiceKind::TupleIndex:
    return Type();
  }

  auto decl = overload.getDecl();

  // Ignore type declarations.
  if (isa<TypeDecl>(decl))
    return Type();

  // Declarations returning unwrapped optionals don't have a single effective
  // type.
  if (decl->isImplicitlyUnwrappedOptional())
    return Type();

  // In a pattern binding initializer, all of its bound variables have no
  // effective overload type.
  if (auto *PBI = dyn_cast<PatternBindingInitializer>(useDC)) {
    if (auto *VD = dyn_cast<VarDecl>(decl)) {
      if (PBI->getBinding() == VD->getParentPatternBinding()) {
        return Type();
      }
    }
  }

  // Retrieve the interface type.
  auto type = decl->getInterfaceType();
  if (type->hasError()) {
    return Type();
  }

  // If we have a generic function type, drop the generic signature; we don't
  // need it for this comparison.
  if (auto genericFn = type->getAs<GenericFunctionType>()) {
    type = FunctionType::get(genericFn->getParams(),
                             genericFn->getResult(),
                             genericFn->getExtInfo());
  }

  // If this declaration is within a type context, we might not be able
  // to handle it.
  if (decl->getDeclContext()->isTypeContext()) {
    if (!allowMembers)
      return Type();

    const auto withDynamicSelfResultReplaced = [&](Type type,
                                                   unsigned uncurryLevel) {
      const Type baseObjTy = overload.getBaseType()
                                 ->getRValueType()
                                 ->getMetatypeInstanceType()
                                 ->lookThroughAllOptionalTypes();

      return type->replaceCovariantResultType(
          getDynamicSelfReplacementType(baseObjTy, decl, locator),
          uncurryLevel);
    };

    if (auto subscript = dyn_cast<SubscriptDecl>(decl)) {
      auto elementTy = subscript->getElementInterfaceType();

      if (doesStorageProduceLValue(subscript, overload.getBaseType(), useDC))
        elementTy = LValueType::get(elementTy);
      else if (elementTy->hasDynamicSelfType()) {
        elementTy = withDynamicSelfResultReplaced(elementTy,
                                                  /*uncurryLevel=*/0);
      }

      // See ConstraintSystem::resolveOverload() -- optional and dynamic
      // subscripts are a special case, because the optionality is
      // applied to the result type and not the type of the reference.
      if (subscript->getAttrs().hasAttribute<OptionalAttr>())
        elementTy = OptionalType::get(elementTy->getRValueType());

      auto indices = subscript->getInterfaceType()
                       ->castTo<AnyFunctionType>()->getParams();
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo info;
      type = FunctionType::get(indices, elementTy, info);
    } else if (auto var = dyn_cast<VarDecl>(decl)) {
      type = var->getValueInterfaceType();
      if (doesStorageProduceLValue(var, overload.getBaseType(), useDC)) {
        type = LValueType::get(type);
      } else if (type->hasDynamicSelfType()) {
        type = withDynamicSelfResultReplaced(type, /*uncurryLevel=*/0);
      }
    } else if (isa<AbstractFunctionDecl>(decl) || isa<EnumElementDecl>(decl)) {
      if (decl->isInstanceMember() &&
          (!overload.getBaseType() ||
           !overload.getBaseType()->getAnyNominal()))
        return Type();

      // Cope with 'Self' returns.
      if (!decl->getDeclContext()->getSelfProtocolDecl()) {
        if (isa<AbstractFunctionDecl>(decl) &&
            cast<AbstractFunctionDecl>(decl)->hasDynamicSelfResult()) {
          if (!overload.getBaseType())
            return Type();

          if (!overload.getBaseType()->getOptionalObjectType()) {
            // `Int??(0)` if we look through all optional types for `Self`
            // we'll end up with incorrect type `Int?` for result because
            // the actual result type is `Int??`.
            if (isa<ConstructorDecl>(decl) && overload.getBaseType()
                                                  ->getRValueType()
                                                  ->getMetatypeInstanceType()
                                                  ->getOptionalObjectType())
              return Type();

            type = withDynamicSelfResultReplaced(type, /*uncurryLevel=*/2);
          }
        }
      }

      type = type->castTo<FunctionType>()->getResult();
    }
  }

  // Handle "@objc optional" for non-subscripts; subscripts are handled above.
  if (decl->getAttrs().hasAttribute<OptionalAttr>() &&
      !isa<SubscriptDecl>(decl))
    type = OptionalType::get(type->getRValueType());

  return type;
}

void ConstraintSystem::addOverloadSet(Type boundType,
                                      ArrayRef<OverloadChoice> choices,
                                      DeclContext *useDC,
                                      ConstraintLocator *locator,
                                      Optional<unsigned> favoredIndex) {
  // If there is a single choice, add the bind overload directly.
  if (choices.size() == 1) {
    addBindOverloadConstraint(boundType, choices.front(), locator, useDC);
    return;
  }

  SmallVector<Constraint *, 4> candidates;
  generateConstraints(candidates, boundType, choices, useDC, locator,
                      favoredIndex);
  // For an overload set (disjunction) from newly generated candidates.
  addOverloadSet(candidates, locator);
}

void ConstraintSystem::addOverloadSet(ArrayRef<Constraint *> choices,
                                      ConstraintLocator *locator) {
  assert(!choices.empty() && "Empty overload set");

  // If there is a single choice, attempt it right away.
  if (choices.size() == 1) {
    simplifyConstraint(*choices.front());
    return;
  }

  auto *disjunction =
      Constraint::createDisjunction(*this, choices, locator, ForgetChoice);
  addUnsolvedConstraint(disjunction);
  if (simplifyAppliedOverloads(disjunction, locator))
    retireFailedConstraint(disjunction);
}

/// If we're resolving an overload set with a decl that has special type
/// checking semantics, compute the type of the reference.  For now, follow
/// the lead of \c getTypeOfMemberReference and return a pair of
/// the full opened type and the reference's type.
static std::pair<Type, Type> getTypeOfReferenceWithSpecialTypeCheckingSemantics(
    ConstraintSystem &CS, ConstraintLocator *locator,
    DeclTypeCheckingSemantics semantics) {
  switch (semantics) {
  case DeclTypeCheckingSemantics::Normal:
    llvm_unreachable("Decl does not have special type checking semantics!");

  case DeclTypeCheckingSemantics::TypeOf: {
    // Proceed with a "DynamicType" operation. This produces an existential
    // metatype from existentials, or a concrete metatype from non-
    // existentials (as seen from the current abstraction level), which can't
    // be expressed in the type system currently.
    auto input = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToNoEscape);
    auto output = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionResult),
        TVO_CanBindToNoEscape);

    FunctionType::Param inputArg(input,
                                 CS.getASTContext().getIdentifier("of"));

    CS.addConstraint(
        ConstraintKind::DynamicTypeOf, output, input,
        CS.getConstraintLocator(locator, ConstraintLocator::DynamicType));
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo info;
    auto refType = FunctionType::get({inputArg}, output, info);
    return {refType, refType};
  }
  case DeclTypeCheckingSemantics::WithoutActuallyEscaping: {
    // Proceed with a "WithoutActuallyEscaping" operation. The body closure
    // receives a copy of the argument closure that is temporarily made
    // @escaping.
    auto noescapeClosure = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToNoEscape);
    auto escapeClosure = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToNoEscape);
    CS.addConstraint(ConstraintKind::EscapableFunctionOf, escapeClosure,
                     noescapeClosure, CS.getConstraintLocator(locator));
    auto result = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionResult),
        TVO_CanBindToNoEscape);
    FunctionType::Param arg(escapeClosure);
    auto bodyClosure = FunctionType::get(arg, result,
                                         FunctionType::ExtInfoBuilder()
                                             .withNoEscape(true)
                                             .withAsync(true)
                                             .withThrows(true)
                                             .build());
    FunctionType::Param args[] = {
      FunctionType::Param(noescapeClosure),
      FunctionType::Param(bodyClosure, CS.getASTContext().getIdentifier("do")),
    };

    auto refType = FunctionType::get(args, result,
                                     FunctionType::ExtInfoBuilder()
                                         .withNoEscape(false)
                                         .withAsync(true)
                                         .withThrows(true)
                                         .build());
    return {refType, refType};
  }
  case DeclTypeCheckingSemantics::OpenExistential: {
    // The body closure receives a freshly-opened archetype constrained by the
    // existential type as its input.
    auto openedTy = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToNoEscape);
    auto existentialTy = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToNoEscape);
    CS.addConstraint(ConstraintKind::OpenedExistentialOf, openedTy,
                     existentialTy, CS.getConstraintLocator(locator));
    auto result = CS.createTypeVariable(
        CS.getConstraintLocator(locator, ConstraintLocator::FunctionResult),
        TVO_CanBindToNoEscape);
    FunctionType::Param bodyArgs[] = {FunctionType::Param(openedTy)};
    auto bodyClosure = FunctionType::get(bodyArgs, result,
                                         FunctionType::ExtInfoBuilder()
                                             .withNoEscape(true)
                                             .withThrows(true)
                                             .build());
    FunctionType::Param args[] = {
      FunctionType::Param(existentialTy),
      FunctionType::Param(bodyClosure, CS.getASTContext().getIdentifier("do")),
    };
    auto refType = FunctionType::get(args, result,
                                     FunctionType::ExtInfoBuilder()
                                         .withNoEscape(false)
                                         .withThrows(true)
                                         .build());
    return {refType, refType};
  }
  }

  llvm_unreachable("Unhandled DeclTypeCheckingSemantics in switch.");
}

/// Try to identify and fix failures related to partial function application
/// e.g. partial application of `init` or 'mutating' instance methods.
static std::pair<bool, unsigned>
isInvalidPartialApplication(ConstraintSystem &cs,
                            const AbstractFunctionDecl *member,
                            ConstraintLocator *locator) {
  auto *UDE = getAsExpr<UnresolvedDotExpr>(locator->getAnchor());
  if (UDE == nullptr)
    return {false,0};

  auto baseTy =
      cs.simplifyType(cs.getType(UDE->getBase()))->getWithoutSpecifierType();

  auto isInvalidIfPartiallyApplied = [&]() {
    if (auto *FD = dyn_cast<FuncDecl>(member)) {
      // 'mutating' instance methods cannot be partially applied.
      if (FD->isMutating())
        return true;

      // Instance methods cannot be referenced on 'super' from a static
      // context.
      if (UDE->getBase()->isSuperExpr() &&
          baseTy->is<MetatypeType>() &&
          !FD->isStatic())
        return true;
    }

    // Another unsupported partial application is related
    // to constructor delegation via 'self.init' or 'super.init'.
    //
    // Note that you can also write 'self.init' or 'super.init'
    // inside a static context -- since 'self' is a metatype there
    // it doesn't have the special delegation meaning that it does
    // in the body of a constructor.
    if (isa<ConstructorDecl>(member) && !baseTy->is<MetatypeType>()) {
      // Check for a `super.init` delegation...
      if (UDE->getBase()->isSuperExpr())
        return true;

      // ... and `self.init` delegation. Note that in a static context,
      // `self.init` is just an ordinary partial application; it's OK
      // because there's no associated instance for delegation.
      if (auto *DRE = dyn_cast<DeclRefExpr>(UDE->getBase())) {
        if (auto *baseDecl = DRE->getDecl()) {
          if (baseDecl->getBaseName() == cs.getASTContext().Id_self)
            return true;
        }
      }
    }

    return false;
  };

  if (!isInvalidIfPartiallyApplied())
    return {false,0};

  // If base is a metatype it would be ignored (unless this is an initializer
  // call), but if it is some other type it means that we have a single
  // application level already.
  unsigned level = 0;
  if (!baseTy->is<MetatypeType>())
    ++level;

  if (auto *call = dyn_cast_or_null<CallExpr>(cs.getParentExpr(UDE))) {
    level += 1;
  }

  return {true, level};
}

std::pair<Type, bool> ConstraintSystem::adjustTypeOfOverloadReference(
    const OverloadChoice &choice, ConstraintLocator *locator,
    Type boundType, Type refType) {
  // If the declaration is unavailable, note that in the score.
  if (isDeclUnavailable(choice.getDecl(), locator))
    increaseScore(SK_Unavailable);

  bool bindConstraintCreated = false;
  const auto kind = choice.getKind();
  if (kind != OverloadChoiceKind::DeclViaDynamic &&
      !isRequirementOrWitness(locator) &&
      choice.getDecl()->getAttrs().hasAttribute<OptionalAttr>() &&
      !isa<SubscriptDecl>(choice.getDecl())) {
    // For a non-subscript declaration that is an optional
    // requirement in a protocol, strip off the lvalue-ness (FIXME:
    // one cannot assign to such declarations for now) and make a
    // reference to that declaration be optional.
    //
    // Subscript declarations are handled within
    // getTypeOfMemberReference(); their result types are optional.

    // Deal with values declared as implicitly unwrapped, or
    // functions with return types that are implicitly unwrapped.
    // TODO: Move this logic to bindOverloadType.
    if (choice.isImplicitlyUnwrappedValueOrReturnValue()) {
      // Build the disjunction to attempt binding both T? and T (or
      // function returning T? and function returning T).
      Type ty = createTypeVariable(locator,
                                   TVO_CanBindToLValue | TVO_CanBindToNoEscape);
      buildDisjunctionForImplicitlyUnwrappedOptional(ty, refType, locator);
      addConstraint(ConstraintKind::Bind, boundType,
                    OptionalType::get(ty->getRValueType()), locator);
      bindConstraintCreated = true;
    }

    // TODO: Move this to getTypeOfMemberReference.
    refType = OptionalType::get(refType->getRValueType());
  }

  switch (kind) {
  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
  case OverloadChoiceKind::TupleIndex:
  case OverloadChoiceKind::KeyPathApplication:
    return {refType, bindConstraintCreated};
  case OverloadChoiceKind::DeclViaDynamic: {
    // TODO: Move the IUO handling logic here to bindOverloadType.
    if (isa<SubscriptDecl>(choice.getDecl())) {
      // We always expect function type for subscripts.
      auto fnTy = refType->castTo<AnyFunctionType>();
      if (choice.isImplicitlyUnwrappedValueOrReturnValue()) {
        auto resultTy = fnTy->getResult();
        // We expect the element type to be a double-optional.
        auto optTy = resultTy->getOptionalObjectType();
        assert(optTy->getOptionalObjectType());

        // For our original type T -> U?? we will generate:
        // A disjunction V = { U?, U }
        // and a disjunction boundType = { T -> V?, T -> V }
        Type ty = createTypeVariable(locator, TVO_CanBindToNoEscape);

        buildDisjunctionForImplicitlyUnwrappedOptional(ty, optTy, locator);

        // Create a new function type with an optional of this type
        // variable as the result type.
        if (auto *genFnTy = fnTy->getAs<GenericFunctionType>()) {
          fnTy = GenericFunctionType::get(
              genFnTy->getGenericSignature(), genFnTy->getParams(),
              OptionalType::get(ty), genFnTy->getExtInfo());
        } else {
          fnTy = FunctionType::get(fnTy->getParams(), OptionalType::get(ty),
                                   fnTy->getExtInfo());
        }
      }

      buildDisjunctionForDynamicLookupResult(boundType, fnTy, locator);
    } else {
      Type ty = refType;

      // If this is something we need to implicitly unwrap, set up a
      // new type variable and disjunction that will allow us to make
      // the choice of whether to do so.
      if (choice.isImplicitlyUnwrappedValueOrReturnValue()) {
        // Duplicate the structure of boundType, with fresh type
        // variables. We'll create a binding disjunction using this,
        // selecting between options for refType, which is either
        // Optional or a function type returning Optional.
        assert(boundType->hasTypeVariable());
        ty = boundType.transform([this](Type elTy) -> Type {
          if (auto *tv = dyn_cast<TypeVariableType>(elTy.getPointer())) {
            return createTypeVariable(tv->getImpl().getLocator(),
                                      tv->getImpl().getRawOptions());
          }
          return elTy;
        });

        buildDisjunctionForImplicitlyUnwrappedOptional(
            ty, refType->getRValueType(), locator);
      }

      // Build the disjunction to attempt binding both T? and T (or
      // function returning T? and function returning T).
      buildDisjunctionForDynamicLookupResult(
          boundType, OptionalType::get(ty->getRValueType()), locator);

      // We store an Optional of the originally resolved type in the
      // overload set.
      // TODO: Move this to getTypeOfMemberReference.
      refType = OptionalType::get(refType->getRValueType());
    }

    return {refType, /*bindConstraintCreated*/ true};
  }
  case OverloadChoiceKind::DynamicMemberLookup:
  case OverloadChoiceKind::KeyPathDynamicMemberLookup:
    return {refType, bindConstraintCreated};
  }

  llvm_unreachable("Unhandled OverloadChoiceKind in switch.");
}

/// Walk a closure AST to determine its effects.
///
/// \returns a function's extended info describing the effects, as
/// determined syntactically.
FunctionType::ExtInfo ConstraintSystem::closureEffects(ClosureExpr *expr) {
  auto known = closureEffectsCache.find(expr);
  if (known != closureEffectsCache.end())
    return known->second;

  // A walker that looks for 'try' and 'throw' expressions
  // that aren't nested within closures, nested declarations,
  // or exhaustive catches.
  class FindInnerThrows : public ASTWalker {
    ConstraintSystem &CS;
    DeclContext *DC;
    bool FoundThrow = false;

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // If we've found a 'try', record it and terminate the traversal.
      if (isa<TryExpr>(expr)) {
        FoundThrow = true;
        return { false, nullptr };
      }

      // Don't walk into a 'try!' or 'try?'.
      if (isa<ForceTryExpr>(expr) || isa<OptionalTryExpr>(expr)) {
        return { false, expr };
      }

      // Do not recurse into other closures.
      if (isa<ClosureExpr>(expr))
        return { false, expr };

      return { true, expr };
    }

    bool walkToDeclPre(Decl *decl) override {
      // Do not walk into function or type declarations.
      if (!isa<PatternBindingDecl>(decl))
        return false;

      return true;
    }

    bool isSyntacticallyExhaustive(DoCatchStmt *stmt) {
      for (auto catchClause : stmt->getCatches()) {
        for (auto &LabelItem : catchClause->getMutableCaseLabelItems()) {
          if (isSyntacticallyExhaustive(catchClause->getStartLoc(),
                                        LabelItem))
            return true;
        }
      }

      return false;
    }

    bool isSyntacticallyExhaustive(SourceLoc CatchLoc,
                                   CaseLabelItem &LabelItem) {
      // If it's obviously non-exhaustive, great.
      if (LabelItem.getGuardExpr())
        return false;

      // If we can show that it's exhaustive without full
      // type-checking, great.
      if (LabelItem.isSyntacticallyExhaustive())
        return true;

      // Okay, resolve the pattern.
      Pattern *pattern = LabelItem.getPattern();
      if (!LabelItem.isPatternResolved()) {
        pattern = TypeChecker::resolvePattern(pattern, DC,
                                       /*isStmtCondition*/false);
        if (!pattern) return false;

        // Save that aside while we explore the type.
        LabelItem.setPattern(pattern, /*resolved=*/true);
      }

      // Require the pattern to have a particular shape: a number
      // of is-patterns applied to an irrefutable pattern.
      pattern = pattern->getSemanticsProvidingPattern();
      while (auto isp = dyn_cast<IsPattern>(pattern)) {
        Type castType;
        if (auto castTypeRepr = isp->getCastTypeRepr()) {
          castType = TypeResolution::forContextual(
                         DC, TypeResolverContext::InExpression,
                         /*unboundTyOpener*/ nullptr,
                         /*placeholderHandler*/ nullptr)
                         .resolveType(castTypeRepr);
        } else {
          castType = isp->getCastType();
        }

        if (castType->hasError()) {
          return false;
        }

        if (!isp->hasSubPattern()) {
          pattern = nullptr;
          break;
        } else {
          pattern = isp->getSubPattern()->getSemanticsProvidingPattern();
        }
      }
      if (pattern && pattern->isRefutablePattern()) {
        return false;
      }

      // Okay, now it should be safe to coerce the pattern.
      // Pull the top-level pattern back out.
      pattern = LabelItem.getPattern();
      Type exnType = CS.getASTContext().getErrorDecl()->getDeclaredInterfaceType();

      if (!exnType)
        return false;
      auto contextualPattern =
          ContextualPattern::forRawPattern(pattern, DC);
      pattern = TypeChecker::coercePatternToType(
        contextualPattern, exnType, TypeResolverContext::InExpression);
      if (!pattern)
        return false;

      LabelItem.setPattern(pattern, /*resolved=*/true);
      return LabelItem.isSyntacticallyExhaustive();
    }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      // If we've found a 'throw', record it and terminate the traversal.
      if (isa<ThrowStmt>(stmt)) {
        FoundThrow = true;
        return { false, nullptr };
      }

      // Handle do/catch differently.
      if (auto doCatch = dyn_cast<DoCatchStmt>(stmt)) {
        // Only walk into the 'do' clause of a do/catch statement
        // if the catch isn't syntactically exhaustive.
        if (!isSyntacticallyExhaustive(doCatch)) {
          if (!doCatch->getBody()->walk(*this))
            return { false, nullptr };
        }

        // Walk into all the catch clauses.
        for (auto catchClause : doCatch->getCatches()) {
          if (!catchClause->walk(*this))
            return { false, nullptr };
        }

        // We've already walked all the children we care about.
        return { false, stmt };
      }

      if (auto forEach = dyn_cast<ForEachStmt>(stmt)) {
        if (forEach->getTryLoc().isValid()) {
          FoundThrow = true;
          return { false, nullptr };
        }
      }

      return { true, stmt };
    }

  public:
    FindInnerThrows(ConstraintSystem &cs, DeclContext *dc)
        : CS(cs), DC(dc) {}

    bool foundThrow() { return FoundThrow; }
  };

  // If either 'throws' or 'async' was explicitly specified, use that
  // set of effects.
  bool throws = expr->getThrowsLoc().isValid();
  bool async = expr->getAsyncLoc().isValid();
  bool concurrent = expr->getAttrs().hasAttribute<SendableAttr>();
  if (throws || async) {
    return ASTExtInfoBuilder()
      .withThrows(throws)
      .withAsync(async)
      .withConcurrent(concurrent)
      .build();
  }

  // Scan the body to determine the effects.
  auto body = expr->getBody();
  if (!body)
    return ASTExtInfoBuilder().withConcurrent(concurrent).build();

  auto throwFinder = FindInnerThrows(*this, expr);
  body->walk(throwFinder);
  auto result = ASTExtInfoBuilder()
                    .withThrows(throwFinder.foundThrow())
                    .withAsync(bool(findAsyncNode(expr)))
                    .withConcurrent(concurrent)
                    .build();
  closureEffectsCache[expr] = result;
  return result;
}

bool ConstraintSystem::isAsynchronousContext(DeclContext *dc) {
  if (auto func = dyn_cast<AbstractFunctionDecl>(dc))
    return func->isAsyncContext();

  if (auto closure = dyn_cast<ClosureExpr>(dc))
    return closureEffects(closure).isAsync();

  return false;
}

void ConstraintSystem::bindOverloadType(
    const SelectedOverload &overload, Type boundType,
    ConstraintLocator *locator, DeclContext *useDC,
    llvm::function_ref<void(unsigned int, Type, ConstraintLocator *)>
        verifyThatArgumentIsHashable) {
  auto choice = overload.choice;
  auto openedType = overload.openedType;

  auto bindTypeOrIUO = [&](Type ty) {
    if (choice.isImplicitlyUnwrappedValueOrReturnValue()) {
      // Build the disjunction to attempt binding both T? and T (or
      // function returning T? and function returning T).
      buildDisjunctionForImplicitlyUnwrappedOptional(boundType, ty, locator);
    } else {
      // Add the type binding constraint.
      addConstraint(ConstraintKind::Bind, boundType, ty, locator);
    }
  };
  switch (choice.getKind()) {
  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
  case OverloadChoiceKind::TupleIndex:
  case OverloadChoiceKind::KeyPathApplication:
  case OverloadChoiceKind::DeclViaDynamic:
    bindTypeOrIUO(openedType);
    return;
  case OverloadChoiceKind::DynamicMemberLookup: {
    // DynamicMemberLookup results are always a (dynamicMember:T1)->T2
    // subscript.
    auto refFnType = openedType->castTo<FunctionType>();

    // Before we drop the argument type on the floor, we need to constrain it
    // to having a literal conformance to ExpressibleByStringLiteral.  This
    // makes the index default to String if otherwise unconstrained.
    assert(refFnType->getParams().size() == 1 &&
           "subscript always has one arg");
    auto argType = refFnType->getParams()[0].getPlainType();

    auto stringLiteral =
        TypeChecker::getProtocol(getASTContext(), choice.getDecl()->getLoc(),
                                 KnownProtocolKind::ExpressibleByStringLiteral);
    if (!stringLiteral)
      return;

    addConstraint(ConstraintKind::LiteralConformsTo, argType,
                  stringLiteral->getDeclaredInterfaceType(), locator);

    // If this is used inside of the keypath expression, we need to make
    // sure that argument is Hashable.
    if (isExpr<KeyPathExpr>(locator->getAnchor()))
      verifyThatArgumentIsHashable(0, argType, locator);

    // The resolved decl is for subscript(dynamicMember:), however the original
    // member constraint was for a property. Therefore we need to bind to the
    // result type.
    bindTypeOrIUO(refFnType->getResult());
    return;
  }
  case OverloadChoiceKind::KeyPathDynamicMemberLookup: {
    auto *fnType = openedType->castTo<FunctionType>();
    assert(fnType->getParams().size() == 1 &&
           "subscript always has one argument");
    // Parameter type is KeyPath<T, U> where `T` is a root type
    // and U is a leaf type (aka member type).
    auto keyPathTy =
        fnType->getParams()[0].getPlainType()->castTo<BoundGenericType>();

    auto *keyPathDecl = keyPathTy->getAnyNominal();
    assert(isKnownKeyPathDecl(getASTContext(), keyPathDecl) &&
           "parameter is supposed to be a keypath");

    auto *keyPathLoc = getConstraintLocator(
        locator, LocatorPathElt::KeyPathDynamicMember(keyPathDecl));

    auto rootTy = keyPathTy->getGenericArgs()[0];
    auto leafTy = keyPathTy->getGenericArgs()[1];

    // Member would either point to mutable or immutable property, we
    // don't which at the moment, so let's allow its type to be l-value.
    auto memberTy = createTypeVariable(keyPathLoc, TVO_CanBindToLValue |
                                                       TVO_CanBindToNoEscape);
    // Attempt to lookup a member with a give name in the root type and
    // assign result to the leaf type of the keypath.
    bool isSubscriptRef = locator->isSubscriptMemberRef();
    DeclNameRef memberName = isSubscriptRef
                           ? DeclNameRef::createSubscript()
                           // FIXME: Should propagate name-as-written through.
                           : DeclNameRef(choice.getName());

    addValueMemberConstraint(LValueType::get(rootTy), memberName, memberTy,
                             useDC,
                             isSubscriptRef ? FunctionRefKind::DoubleApply
                                            : FunctionRefKind::Unapplied,
                             /*outerAlternatives=*/{}, keyPathLoc);

    // In case of subscript things are more compicated comparing to "dot"
    // syntax, because we have to get "applicable function" constraint
    // associated with index expression and re-bind it to match "member type"
    // looked up by dynamically.
    if (isSubscriptRef) {
      // Make sure that regular subscript declarations (if any) are
      // preferred over key path dynamic member lookup.
      increaseScore(SK_KeyPathSubscript);

      auto dynamicResultTy = boundType->castTo<TypeVariableType>();
      auto constraints = getConstraintGraph().gatherConstraints(
          dynamicResultTy, ConstraintGraph::GatheringKind::EquivalenceClass,
          [](Constraint *constraint) {
            return constraint->getKind() == ConstraintKind::ApplicableFunction;
          });

      assert(constraints.size() == 1);
      auto *applicableFn = constraints.front();
      retireConstraint(applicableFn);

      // Original subscript expression e.g. `<base>[0]` generated following
      // constraint `($T_A0, [$T_A1], ...) -> $T_R applicable fn $T_S` where
      // `$T_S` is supposed to be bound to each subscript choice e.g.
      // `(Int) -> Int`.
      //
      // Here is what we need to do to make this work as-if expression was
      // `<base>[dynamicMember: \.[0]]`:
      // - Right-hand side function type would have to get a new result type
      //   since it would have to point to result type of `\.[0]`, arguments
      //   though should stay the same.
      // - Left-hand side `$T_S` is going to point to a new "member type"
      //   we are looking up based on the root type of the key path.
      // - Original result type `$T_R` is going to represent result of
      //   the `[dynamicMember: \.[0]]` invocation.

      // Result of the `WritableKeyPath` is going to be l-value type,
      // let's adjust l-valueness of the result type to accommodate that.
      //
      // This is required because we are binding result of the subscript
      // to its "member type" which becomes dynamic result type. We could
      // form additional `applicable fn` constraint here and bind it to a
      // function type, but it would create inconsistency with how properties
      // are handled, which means more special handling in CSApply.
      if (keyPathDecl == getASTContext().getWritableKeyPathDecl() ||
          keyPathDecl == getASTContext().getReferenceWritableKeyPathDecl())
        dynamicResultTy->getImpl().setCanBindToLValue(getSavedBindings(),
                                                      /*enabled=*/true);

      auto fnType = applicableFn->getFirstType()->castTo<FunctionType>();

      auto subscriptResultTy = createTypeVariable(
          getConstraintLocator(locator->getAnchor(),
                               ConstraintLocator::FunctionResult),
          TVO_CanBindToLValue | TVO_CanBindToNoEscape);

      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo info;
      auto adjustedFnTy =
          FunctionType::get(fnType->getParams(), subscriptResultTy, info);

      ConstraintLocatorBuilder kpLocBuilder(keyPathLoc);
      addConstraint(
          ConstraintKind::ApplicableFunction, adjustedFnTy, memberTy,
          kpLocBuilder.withPathElement(ConstraintLocator::ApplyFunction));

      addConstraint(ConstraintKind::Bind, dynamicResultTy, fnType->getResult(),
                    keyPathLoc);

      addConstraint(ConstraintKind::Equal, subscriptResultTy, leafTy,
                    keyPathLoc);
    } else {
      // Since member type is going to be bound to "leaf" generic parameter
      // of the keypath, it has to be an r-value always, so let's add a new
      // constraint to represent that conversion instead of loading member
      // type into "leaf" directly.
      addConstraint(ConstraintKind::Equal, memberTy, leafTy, keyPathLoc);
    }

    if (isExpr<KeyPathExpr>(locator->getAnchor()))
      verifyThatArgumentIsHashable(0, keyPathTy, locator);

    // The resolved decl is for subscript(dynamicMember:), however the
    // original member constraint was either for a property, or we've
    // re-purposed the overload type variable to represent the result type of
    // the subscript. In both cases, we need to bind to the result type.
    bindTypeOrIUO(fnType->getResult());
    return;
  }
  }
  llvm_unreachable("Unhandled OverloadChoiceKind in switch.");
}

void ConstraintSystem::resolveOverload(ConstraintLocator *locator,
                                       Type boundType,
                                       OverloadChoice choice,
                                       DeclContext *useDC) {
  // Add a conformance constraint to make sure that given type conforms
  // to Hashable protocol, which is important for key path subscript
  // components.
  auto verifyThatArgumentIsHashable = [&](unsigned index, Type argType,
                                          ConstraintLocator *locator) {
    if (auto *hashable = TypeChecker::getProtocol(
            argType->getASTContext(), choice.getDecl()->getLoc(),
            KnownProtocolKind::Hashable)) {
      addConstraint(ConstraintKind::ConformsTo, argType,
                    hashable->getDeclaredInterfaceType(),
                    getConstraintLocator(
                        locator, LocatorPathElt::TupleElement(index)));
    }
  };

  // Determine the type to which we'll bind the overload set's type.
  Type refType;
  Type openedFullType;

  bool bindConstraintCreated = false;
  switch (auto kind = choice.getKind()) {
  case OverloadChoiceKind::Decl:
  case OverloadChoiceKind::DeclViaBridge:
  case OverloadChoiceKind::DeclViaDynamic:
  case OverloadChoiceKind::DeclViaUnwrappedOptional:
  case OverloadChoiceKind::DynamicMemberLookup:
  case OverloadChoiceKind::KeyPathDynamicMemberLookup: {
    // If we refer to a top-level decl with special type-checking semantics,
    // handle it now.
    const auto semantics =
        TypeChecker::getDeclTypeCheckingSemantics(choice.getDecl());
    if (semantics != DeclTypeCheckingSemantics::Normal) {
      std::tie(openedFullType, refType) =
          getTypeOfReferenceWithSpecialTypeCheckingSemantics(*this, locator,
                                                             semantics);
      // Declarations with special type checking semantics do not require
      // any further adjustments to the constraint system. Break out of
      // here so we don't do any more work.
      break;
    } else if (auto baseTy = choice.getBaseType()) {
      // Retrieve the type of a reference to the specific declaration choice.
      assert(!baseTy->hasTypeParameter());

      std::tie(openedFullType, refType)
        = getTypeOfMemberReference(baseTy, choice.getDecl(), useDC,
                                   (kind == OverloadChoiceKind::DeclViaDynamic),
                                   choice.getFunctionRefKind(),
                                   locator, nullptr);
    } else {
      std::tie(openedFullType, refType)
        = getTypeOfReference(choice.getDecl(),
                             choice.getFunctionRefKind(), locator, useDC);
    }

    // For a non-subscript declaration found via dynamic lookup, strip
    // off the lvalue-ness (FIXME: as a temporary hack. We eventually
    // want this to work) and make a reference to that declaration be
    // an implicitly unwrapped optional.
    //
    // Subscript declarations are handled within
    // getTypeOfMemberReference(); their result types are unchecked
    // optional.
    std::tie(refType, bindConstraintCreated) =
        adjustTypeOfOverloadReference(choice, locator, boundType, refType);
    break;
  }

  case OverloadChoiceKind::TupleIndex:
    if (auto lvalueTy = choice.getBaseType()->getAs<LValueType>()) {
      // When the base of a tuple lvalue, the member is always an lvalue.
      auto tuple = lvalueTy->getObjectType()->castTo<TupleType>();
      refType = tuple->getElementType(choice.getTupleIndex())->getRValueType();
      refType = LValueType::get(refType);
    } else {
      // When the base is a tuple rvalue, the member is always an rvalue.
      auto tuple = choice.getBaseType()->castTo<TupleType>();
      refType = tuple->getElementType(choice.getTupleIndex())->getRValueType();
    }
    break;

  case OverloadChoiceKind::KeyPathApplication: {
    // Key path application looks like a subscript(keyPath: KeyPath<Base, T>).
    // The element type is T or @lvalue T based on the key path subtype and
    // the mutability of the base.
    auto keyPathIndexTy = createTypeVariable(
        getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToInOut);
    auto elementTy = createTypeVariable(
            getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
            TVO_CanBindToLValue | TVO_CanBindToNoEscape);
    auto elementObjTy = createTypeVariable(
        getConstraintLocator(locator, ConstraintLocator::FunctionArgument),
        TVO_CanBindToNoEscape);
    addConstraint(ConstraintKind::Equal, elementTy, elementObjTy, locator);

    // The element result is an lvalue or rvalue based on the key path class.
    addKeyPathApplicationConstraint(
                  keyPathIndexTy, choice.getBaseType(), elementTy, locator);

    FunctionType::Param indices[] = {
      FunctionType::Param(keyPathIndexTy, getASTContext().Id_keyPath),
    };
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo subscriptInfo;
    auto subscriptTy = FunctionType::get(indices, elementTy, subscriptInfo);

    FunctionType::Param baseParam(choice.getBaseType());
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo fullInfo;
    auto fullTy = FunctionType::get({baseParam}, subscriptTy, fullInfo);
    openedFullType = fullTy;
    refType = subscriptTy;

    // Increase the score so that actual subscripts get preference.
    increaseScore(SK_KeyPathSubscript);
    break;
  }
  }
  assert(!refType->hasTypeParameter() && "Cannot have a dependent type here");

  if (auto *decl = choice.getDeclOrNull()) {
    // If we're choosing an asynchronous declaration within a synchronous
    // context, or vice-versa, increase the async/async mismatch score.
    if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
      if (!func->hasPolymorphicEffect(EffectKind::Async) &&
          func->isAsyncContext() != isAsynchronousContext(useDC)) {
        increaseScore(
            func->isAsyncContext() ? SK_AsyncInSyncMismatch : SK_SyncInAsync);
      }
    }

    // If we're binding to an init member, the 'throws' need to line up
    // between the bound and reference types.
    if (auto CD = dyn_cast<ConstructorDecl>(decl)) {
      auto boundFunctionType = boundType->getAs<AnyFunctionType>();

      if (boundFunctionType &&
          CD->hasThrows() != boundFunctionType->isThrowing()) {
        boundType = boundFunctionType->withExtInfo(
            boundFunctionType->getExtInfo().withThrows());
      }
    }

    if (isa<SubscriptDecl>(decl)) {
      if (locator->isResultOfKeyPathDynamicMemberLookup() ||
          locator->isKeyPathSubscriptComponent()) {
        // Subscript type has a format of (Self[.Type) -> (Arg...) -> Result
        auto declTy = openedFullType->castTo<FunctionType>();
        auto subscriptTy = declTy->getResult()->castTo<FunctionType>();
        // If we have subscript, each of the arguments has to conform to
        // Hashable, because it would be used as a component inside key path.
        for (auto index : indices(subscriptTy->getParams())) {
          const auto &param = subscriptTy->getParams()[index];
          verifyThatArgumentIsHashable(index, param.getParameterType(), locator);
        }
      }
    }

    if (auto *afd = dyn_cast<AbstractFunctionDecl>(decl)) {
      // Check whether applying this overload would result in invalid
      // partial function application e.g. partial application of
      // mutating method or initializer.

      // This check is supposed to be performed without
      // `shouldAttemptFixes` because name lookup can't
      // detect that particular partial application is
      // invalid, so it has to return all of the candidates.

      bool isInvalidPartialApply;
      unsigned level;

      std::tie(isInvalidPartialApply, level) =
          isInvalidPartialApplication(*this, afd, locator);

      if (isInvalidPartialApply) {
        // No application at all e.g. `Foo.bar`.
        if (level == 0) {
          // Swift 4 and earlier failed to diagnose a reference to a mutating
          // method without any applications at all, which would get
          // miscompiled into a function with undefined behavior. Warn for
          // source compatibility.
          bool isWarning = !getASTContext().isSwiftVersionAtLeast(5);
          (void)recordFix(
              AllowInvalidPartialApplication::create(isWarning, *this, locator));
        } else if (level == 1) {
          // `Self` parameter is applied, e.g. `foo.bar` or `Foo.bar(&foo)`
          (void)recordFix(AllowInvalidPartialApplication::create(
              /*isWarning=*/false, *this, locator));
        }

        // Otherwise both `Self` and arguments are applied,
        // e.g. `foo.bar()` or `Foo.bar(&foo)()`, and there is nothing to do.
      }
    }
  }

  // Note that we have resolved this overload.
  auto overload = SelectedOverload{choice, openedFullType, refType, boundType};
  auto result = ResolvedOverloads.insert({locator, overload});
  assert(result.second && "Already resolved this overload?");
  (void)result;

  // In some cases we already created the appropriate bind constraints.
  if (!bindConstraintCreated) {
    bindOverloadType(overload, boundType, locator, useDC,
                     verifyThatArgumentIsHashable);
  }

  if (isDebugMode()) {
    PrintOptions PO;
    PO.PrintTypesForDebugging = true;
    llvm::errs().indent(solverState ? solverState->depth * 2 : 2)
      << "(overload set choice binding "
      << boundType->getString(PO) << " := "
      << refType->getString(PO) << ")\n";
  }

  // If this overload is disfavored, note that.
  if (choice.isDecl() &&
      choice.getDecl()->getAttrs().hasAttribute<DisfavoredOverloadAttr>()) {
    increaseScore(SK_DisfavoredOverload);
  }

  if (choice.isFallbackMemberOnUnwrappedBase()) {
    increaseScore(SK_UnresolvedMemberViaOptional);
  }
}

Type ConstraintSystem::simplifyTypeImpl(Type type,
    llvm::function_ref<Type(TypeVariableType *)> getFixedTypeFn) const {
  return type.transform([&](Type type) -> Type {
    if (auto tvt = dyn_cast<TypeVariableType>(type.getPointer()))
      return getFixedTypeFn(tvt);

    // If this is a dependent member type for which we end up simplifying
    // the base to a non-type-variable, perform lookup.
    if (auto depMemTy = dyn_cast<DependentMemberType>(type.getPointer())) {
      // Simplify the base.
      Type newBase = simplifyTypeImpl(depMemTy->getBase(), getFixedTypeFn);

      if (newBase->isPlaceholder()) {
        return PlaceholderType::get(getASTContext(), depMemTy);
      }

      // If nothing changed, we're done.
      if (newBase.getPointer() == depMemTy->getBase().getPointer())
        return type;

      // Dependent member types should only be created for associated types.
      auto assocType = depMemTy->getAssocType();
      assert(depMemTy->getAssocType() && "Expected associated type!");

      // FIXME: It's kind of weird in general that we have to look
      // through lvalue, inout and IUO types here
      Type lookupBaseType = newBase->getWithoutSpecifierType();
      if (auto selfType = lookupBaseType->getAs<DynamicSelfType>())
        lookupBaseType = selfType->getSelfType();

      if (lookupBaseType->mayHaveMembers()) {
        auto *proto = assocType->getProtocol();
        auto conformance = DC->getParentModule()->lookupConformance(
          lookupBaseType, proto);
        if (!conformance) {
          // If the base type doesn't conform to the associatedtype's protocol,
          // there will be a missing conformance fix applied in diagnostic mode,
          // so the concrete dependent member type is considered a "hole" in
          // order to continue solving.
          auto memberTy = DependentMemberType::get(lookupBaseType, assocType);
          if (shouldAttemptFixes() &&
              getPhase() == ConstraintSystemPhase::Solving) {
            return PlaceholderType::get(getASTContext(), memberTy);
          }

          return memberTy;
        }

        auto subs = SubstitutionMap::getProtocolSubstitutions(
            proto, lookupBaseType, conformance);
        auto result = assocType->getDeclaredInterfaceType().subst(subs);
        if (!result->hasError())
          return result;
      }

      return DependentMemberType::get(lookupBaseType, assocType);
    }

    return type;
  });
}

Type ConstraintSystem::simplifyType(Type type) const {
  if (!type->hasTypeVariable())
    return type;

  // Map type variables down to the fixed types of their representatives.
  return simplifyTypeImpl(type,
      [&](TypeVariableType *tvt) -> Type {
        if (auto fixed = getFixedType(tvt))
          return simplifyType(fixed);

        return getRepresentative(tvt);
      });
}

void Solution::recordSingleArgMatchingChoice(ConstraintLocator *locator) {
  auto &cs = getConstraintSystem();
  assert(argumentMatchingChoices.find(locator) ==
             argumentMatchingChoices.end() &&
         "recording multiple bindings for same locator");
  argumentMatchingChoices.insert(
      {cs.getConstraintLocator(locator, ConstraintLocator::ApplyArgument),
       MatchCallArgumentResult::forArity(1)});
}

Type Solution::simplifyType(Type type) const {
  if (!(type->hasTypeVariable() || type->hasPlaceholder()))
    return type;

  // Map type variables to fixed types from bindings.
  auto &cs = getConstraintSystem();
  auto resolvedType = cs.simplifyTypeImpl(
      type, [&](TypeVariableType *tvt) -> Type { return getFixedType(tvt); });

  // Placeholders shouldn't be reachable through a solution, they are only
  // useful to determine what went wrong exactly.
  if (resolvedType->hasPlaceholder()) {
    return resolvedType.transform([&](Type type) {
      return type->isPlaceholder() ? Type(cs.getASTContext().TheUnresolvedType)
                                   : type;
    });
  }

  return resolvedType;
}

size_t Solution::getTotalMemory() const {
  return sizeof(*this) + typeBindings.getMemorySize() +
         overloadChoices.getMemorySize() +
         ConstraintRestrictions.getMemorySize() +
         llvm::capacity_in_bytes(Fixes) + DisjunctionChoices.getMemorySize() +
         OpenedTypes.getMemorySize() + OpenedExistentialTypes.getMemorySize() +
         (DefaultedConstraints.size() * sizeof(void *));
}

DeclContext *Solution::getDC() const { return constraintSystem->DC; }

DeclName OverloadChoice::getName() const {
  switch (getKind()) {
    case OverloadChoiceKind::Decl:
    case OverloadChoiceKind::DeclViaDynamic:
    case OverloadChoiceKind::DeclViaBridge:
    case OverloadChoiceKind::DeclViaUnwrappedOptional:
      return getDecl()->getName();

    case OverloadChoiceKind::KeyPathApplication:
      // TODO: This should probably produce subscript(keyPath:), but we
      // don't currently pre-filter subscript overload sets by argument
      // keywords, so "subscript" is still the name that keypath subscripts
      // are looked up by.
      return DeclBaseName::createSubscript();

    case OverloadChoiceKind::DynamicMemberLookup:
    case OverloadChoiceKind::KeyPathDynamicMemberLookup:
      return DeclName(DynamicMember.getPointer());

    case OverloadChoiceKind::TupleIndex:
      llvm_unreachable("no name!");
  }

  llvm_unreachable("Unhandled OverloadChoiceKind in switch.");
}

bool OverloadChoice::isImplicitlyUnwrappedValueOrReturnValue() const {
  if (!isDecl())
    return false;

  auto *decl = getDecl();
  if (!decl->isImplicitlyUnwrappedOptional())
    return false;

  auto itfType = decl->getInterfaceType();
  if (!itfType->getAs<AnyFunctionType>())
    return true;

  switch (getFunctionRefKind()) {
  case FunctionRefKind::Unapplied:
  case FunctionRefKind::Compound:
    return false;
  case FunctionRefKind::SingleApply:
  case FunctionRefKind::DoubleApply:
    return true;
  }
  llvm_unreachable("unhandled kind");
}

SolutionResult ConstraintSystem::salvage() {
  if (isDebugMode()) {
    llvm::errs() << "---Attempting to salvage and emit diagnostics---\n";
  }

  setPhase(ConstraintSystemPhase::Diagnostics);

  // Attempt to solve again, capturing all states that come from our attempts to
  // select overloads or bind type variables.
  //
  // FIXME: can this be removed?  We need to arrange for recordFixes to be
  // eliminated.
  SmallVector<Solution, 2> viable;
  viable.clear();

  {
    // Set up solver state.
    SolverState state(*this, FreeTypeVariableBinding::Disallow);
    state.recordFixes = true;

    // Solve the system.
    solveImpl(viable);

    // Before removing any "fixed" solutions, let's check
    // if ambiguity is caused by fixes and diagnose if possible.
    if (diagnoseAmbiguityWithFixes(viable))
      return SolutionResult::forAmbiguous(viable);

    // Check whether we have a best solution; this can happen if we found
    // a series of fixes that worked.
    if (auto best = findBestSolution(viable, /*minimize=*/true)) {
      if (*best != 0)
        viable[0] = std::move(viable[*best]);
      viable.erase(viable.begin() + 1, viable.end());
      return SolutionResult::forSolved(std::move(viable[0]));
    }

    // FIXME: If we were able to actually fix things along the way,
    // we may have to hunt for the best solution. For now, we don't care.

    // Remove solutions that require fixes; the fixes in those systems should
    // be diagnosed rather than any ambiguity.
    auto hasFixes = [](const Solution &sol) { return !sol.Fixes.empty(); };
    auto newEnd = std::remove_if(viable.begin(), viable.end(), hasFixes);
    viable.erase(newEnd, viable.end());

    // If there are multiple solutions, try to diagnose an ambiguity.
    if (viable.size() > 1) {
      if (isDebugMode()) {
        auto &log = llvm::errs();
        log << "---Ambiguity error: " << viable.size()
            << " solutions found---\n";
        int i = 0;
        for (auto &solution : viable) {
          log << "---Ambiguous solution #" << i++ << "---\n";
          solution.dump(log);
          log << "\n";
        }
      }

      if (diagnoseAmbiguity(viable)) {
        return SolutionResult::forAmbiguous(viable);
      }
    }

    // Fall through to produce diagnostics.
  }

  if (getExpressionTooComplex(viable))
    return SolutionResult::forTooComplex();

  // Could not produce a specific diagnostic; punt to the client.
  return SolutionResult::forUndiagnosedError();
}

static void diagnoseOperatorAmbiguity(ConstraintSystem &cs,
                                      Identifier operatorName,
                                      ArrayRef<Solution> solutions,
                                      ConstraintLocator *locator) {
  auto &ctx = cs.getASTContext();
  auto &DE = ctx.Diags;
  auto *anchor = castToExpr(locator->getAnchor());
  auto *applyExpr = cast<ApplyExpr>(cs.getParentExpr(anchor));

  auto isEnumWithAssociatedValues = [](Type type) -> bool {
    if (auto *enumType = type->getAs<EnumType>())
      return !enumType->getDecl()->hasOnlyCasesWithoutAssociatedValues();
    return false;
  };

  const auto &solution = solutions.front();
  if (auto *binaryOp = dyn_cast<BinaryExpr>(applyExpr)) {
    auto *lhs = binaryOp->getArg()->getElement(0);
    auto *rhs = binaryOp->getArg()->getElement(1);

    auto lhsType =
        solution.simplifyType(solution.getType(lhs))->getRValueType();
    auto rhsType =
        solution.simplifyType(solution.getType(rhs))->getRValueType();

    if (lhsType->isEqual(rhsType)) {
      DE.diagnose(anchor->getLoc(), diag::cannot_apply_binop_to_same_args,
                  operatorName.str(), lhsType)
          .highlight(lhs->getSourceRange())
          .highlight(rhs->getSourceRange());

      if (isStandardComparisonOperator(binaryOp->getFn()) &&
          isEnumWithAssociatedValues(lhsType)) {
        DE.diagnose(applyExpr->getLoc(),
                    diag::no_binary_op_overload_for_enum_with_payload,
                    operatorName.str());
        return;
      }
    } else if (operatorName == ctx.Id_MatchOperator) {
      DE.diagnose(anchor->getLoc(), diag::cannot_match_expr_pattern_with_value,
                  lhsType, rhsType);
    } else {
      DE.diagnose(anchor->getLoc(), diag::cannot_apply_binop_to_args,
                  operatorName.str(), lhsType, rhsType)
          .highlight(lhs->getSourceRange())
          .highlight(rhs->getSourceRange());
    }
  } else {
    auto argType = solution.simplifyType(solution.getType(applyExpr->getArg()));
    DE.diagnose(anchor->getLoc(), diag::cannot_apply_unop_to_arg,
                operatorName.str(), argType->getRValueType());
  }

  std::set<std::string> parameters;
  for (const auto &solution : solutions) {
    auto overload = solution.getOverloadChoice(locator);
    auto overloadType = overload.openedType;
    // Let's suggest only concrete overloads here.
    // Notes are going to take care of the rest,
    // since printing types like `(Self, Self)` is not
    // really useful.
    if (overloadType->hasTypeVariable())
      continue;

    if (auto *fnType = overloadType->getAs<FunctionType>())
      parameters.insert(
          FunctionType::getParamListAsString(fnType->getParams()));
  }

  // All of the overload choices had generic parameters like `Self`.
  if (parameters.empty())
    return;

  DE.diagnose(anchor->getLoc(), diag::suggest_partial_overloads,
              /*isResult=*/false, operatorName.str(),
              llvm::join(parameters, ", "));
}

std::string swift::describeGenericType(ValueDecl *GP, bool includeName) {
  if (!GP)
    return "";

  Decl *parent = nullptr;
  if (auto *AT = dyn_cast<AssociatedTypeDecl>(GP)) {
    parent = AT->getProtocol();
  } else {
    auto *dc = GP->getDeclContext();
    parent = dc->getInnermostDeclarationDeclContext();
  }

  if (!parent)
    return "";

  llvm::SmallString<64> result;
  llvm::raw_svector_ostream OS(result);

  OS << Decl::getDescriptiveKindName(GP->getDescriptiveKind());

  if (includeName && GP->hasName())
    OS << " '" << GP->getBaseName() << "'";

  OS << " of ";
  OS << Decl::getDescriptiveKindName(parent->getDescriptiveKind());
  if (auto *decl = dyn_cast<ValueDecl>(parent)) {
    if (decl->hasName())
      OS << " '" << decl->getName() << "'";
  }

  return OS.str().str();
}

/// Special handling of conflicts associated with generic arguments.
///
/// func foo<T>(_: T, _: T) {}
/// func bar(x: Int, y: Float) {
///   foo(x, y)
/// }
///
/// It's done by first retrieving all generic parameters from each solution,
/// filtering bindings into a distinct set and diagnosing any differences.
static bool diagnoseConflictingGenericArguments(ConstraintSystem &cs,
                                                const SolutionDiff &diff,
                                                ArrayRef<Solution> solutions) {
  if (!diff.overloads.empty())
    return false;

  bool noFixes = llvm::all_of(solutions, [](const Solution &solution) -> bool {
     const auto score = solution.getFixedScore();
     return score.Data[SK_Fix] == 0 && solution.Fixes.empty();
  });

  bool allMismatches =
      llvm::all_of(solutions, [](const Solution &solution) -> bool {
        return llvm::all_of(
            solution.Fixes, [](const ConstraintFix *fix) -> bool {
              return fix->getKind() == FixKind::AllowArgumentTypeMismatch ||
                     fix->getKind() == FixKind::AllowFunctionTypeMismatch ||
                     fix->getKind() == FixKind::AllowTupleTypeMismatch;
            });
      });

  if (!noFixes && !allMismatches)
    return false;

  auto &DE = cs.getASTContext().Diags;

  llvm::SmallDenseMap<TypeVariableType *,
                      std::pair<GenericTypeParamType *, SourceLoc>, 4>
      genericParams;
  // Consider only representative type variables shared across
  // all of the solutions.
  for (auto *typeVar : cs.getTypeVariables()) {
    if (auto *GP = typeVar->getImpl().getGenericParameter()) {
      auto *locator = typeVar->getImpl().getLocator();
      auto *repr = cs.getRepresentative(typeVar);
      // If representative is another generic parameter let's
      // use its generic parameter type instead of originator's,
      // but it's possible that generic parameter is equated to
      // some other type e.g.
      //
      // func foo<T>(_: T) -> T {}
      //
      // In this case when reference to function `foo` is "opened"
      // type variable representing `T` would be equated to
      // type variable representing a result type of the reference.
      if (auto *reprGP = repr->getImpl().getGenericParameter())
        GP = reprGP;

      genericParams[repr] = {GP, getLoc(locator->getAnchor())};
    }
  }

  llvm::SmallDenseMap<std::pair<GenericTypeParamType *, SourceLoc>,
                      SmallVector<Type, 4>>
      conflicts;

  for (const auto &entry : genericParams) {
    auto *typeVar = entry.first;
    auto GP = entry.second;

    llvm::SmallSetVector<Type, 4> arguments;
    for (const auto &solution : solutions) {
      auto type = solution.typeBindings.lookup(typeVar);
      // Contextual opaque result type is uniquely identified by
      // declaration it's associated with, so we have to compare
      // declarations instead of using pointer equality on such types.
      if (auto *opaque = type->getAs<OpaqueTypeArchetypeType>()) {
        auto *decl = opaque->getDecl();
        arguments.remove_if([&](Type argType) -> bool {
          if (auto *otherOpaque = argType->getAs<OpaqueTypeArchetypeType>()) {
            return decl == otherOpaque->getDecl();
          }
          return false;
        });
      }

      arguments.insert(type);
    }

    if (arguments.size() > 1)
      conflicts[GP].append(arguments.begin(), arguments.end());
  }

  auto getGenericTypeDecl = [&](ArchetypeType *archetype) -> ValueDecl * {
    auto type = archetype->getInterfaceType();

    if (auto *GTPT = type->getAs<GenericTypeParamType>())
      return GTPT->getDecl();

    if (auto *DMT = type->getAs<DependentMemberType>())
      return DMT->getAssocType();

    return nullptr;
  };

  bool diagnosed = false;
  for (auto &conflict : conflicts) {
    SourceLoc loc;
    GenericTypeParamType *GP;

    std::tie(GP, loc) = conflict.first;
    auto conflictingArguments = conflict.second;

    llvm::SmallString<64> arguments;
    llvm::raw_svector_ostream OS(arguments);

    interleave(
        conflictingArguments,
        [&](Type argType) {
          OS << "'" << argType << "'";

          if (auto *opaque = argType->getAs<OpaqueTypeArchetypeType>()) {
            auto *decl = opaque->getDecl()->getNamingDecl();
            OS << " (result type of '" << decl->getBaseName().userFacingName()
               << "')";
            return;
          }

          if (auto archetype = argType->getAs<ArchetypeType>()) {
            if (auto *GTD = getGenericTypeDecl(archetype))
              OS << " (" << describeGenericType(GTD) << ")";
          }
        },
        [&OS] { OS << " vs. "; });

    DE.diagnose(loc, diag::conflicting_arguments_for_generic_parameter, GP,
                OS.str());
    diagnosed = true;
  }

  return diagnosed;
}

/// Diagnose ambiguity related to overloaded declarations where only
/// *some* of the overload choices have ephemeral pointer warnings/errors
/// associated with them. Such situations have be handled specifically
/// because ephemeral fixes do not affect the score.
///
/// If all of the overloads have ephemeral fixes associated with them
/// it's much easier to diagnose through notes associated with each fix.
static bool
diagnoseAmbiguityWithEphemeralPointers(ConstraintSystem &cs,
                                       ArrayRef<Solution> solutions) {
  unsigned numSolutionsWithFixes = 0;
  for (const auto &solution : solutions) {
    if (solution.Fixes.empty()) {
      continue;
    }

    if (!llvm::all_of(solution.Fixes, [](const ConstraintFix *fix) {
          return fix->getKind() == FixKind::TreatEphemeralAsNonEphemeral;
        }))
      return false;

    numSolutionsWithFixes += 1;
  }

  // If all or no solutions have fixes for ephemeral pointers, let's
  // let `diagnoseAmbiguityWithFixes` diagnose the problem.
  if (numSolutionsWithFixes == 0 ||
      numSolutionsWithFixes == solutions.size())
    return false;

  // If only some of the solutions have ephemeral pointer fixes
  // let's let `diagnoseAmbiguity` diagnose the problem either
  // with affected argument or related declaration e.g. function ref.
  return cs.diagnoseAmbiguity(solutions);
}

static bool diagnoseAmbiguityWithContextualType(
    ConstraintSystem &cs, SolutionDiff &solutionDiff,
    ArrayRef<std::pair<const Solution *, const ConstraintFix *>> aggregateFix,
    ArrayRef<Solution> solutions) {
  // Diagnose only if contextual failure is associated with every solution.
  if (aggregateFix.size() < solutions.size())
    return false;

  auto getResultType =
      [](const std::pair<const Solution *, const ConstraintFix *> &entry)
      -> Type {
    auto &solution = *entry.first;
    auto anchor = entry.second->getLocator()->getAnchor();
    return solution.simplifyType(solution.getType(anchor));
  };

  auto resultType = getResultType(aggregateFix.front());
  // If right-hand side of the conversion (result of the the AST node)
  // is the same across all of the solutions let's diagnose it as if
  // it it as a single failure.
  if (llvm::all_of(
          aggregateFix,
          [&](const std::pair<const Solution *, const ConstraintFix *> &entry) {
            return resultType->isEqual(getResultType(entry));
          })) {
    auto &fix = aggregateFix.front();
    return fix.second->diagnose(*fix.first, /*asNote=*/false);
  }

  // If result types are different it could only mean that this is an attempt
  // to convert a reference to, or call of overloaded declaration to a
  // particular type.

  auto &solution = *aggregateFix.front().first;
  auto *locator = aggregateFix.front().second->getLocator();
  auto *calleeLocator = solution.getCalleeLocator(locator);

  auto result =
      llvm::find_if(solutionDiff.overloads,
                    [&calleeLocator](const SolutionDiff::OverloadDiff &entry) {
                      return entry.locator == calleeLocator;
                    });

  if (result == solutionDiff.overloads.end())
    return false;

  auto &DE = cs.getASTContext().Diags;

  auto anchor = locator->getAnchor();
  auto name = result->choices.front().getName();
  DE.diagnose(getLoc(anchor), diag::no_candidates_match_result_type,
              name.getBaseName().userFacingName(),
              cs.getContextualType(anchor));

  for (const auto &solution : solutions) {
    auto overload = solution.getOverloadChoice(calleeLocator);
    if (auto *decl = overload.choice.getDeclOrNull()) {
      auto loc = decl->getLoc();
      if (loc.isInvalid())
        continue;

      auto type = solution.simplifyType(overload.boundType);

      if (isExpr<ApplyExpr>(anchor) || isExpr<SubscriptExpr>(anchor)) {
        auto fnType = type->castTo<FunctionType>();
        DE.diagnose(
            loc, diag::cannot_convert_candidate_result_to_contextual_type,
            decl->getName(), fnType->getResult(), cs.getContextualType(anchor));
      } else {
        DE.diagnose(loc, diag::found_candidate_type, type);
      }
    }
  }

  return true;
}

static bool diagnoseAmbiguity(
    ConstraintSystem &cs, const SolutionDiff::OverloadDiff &ambiguity,
    ArrayRef<std::pair<const Solution *, const ConstraintFix *>> aggregateFix,
    ArrayRef<Solution> solutions) {
  auto *locator = aggregateFix.front().second->getLocator();
  auto anchor = aggregateFix.front().second->getAnchor();

  auto &DE = cs.getASTContext().Diags;

  llvm::SmallPtrSet<ValueDecl *, 4> localAmbiguity;
  {
    for (auto &entry : aggregateFix) {
      const auto &solution = entry.first;
      const auto &overload = solution->getOverloadChoice(ambiguity.locator);
      auto *choice = overload.choice.getDeclOrNull();

      // It's not possible to diagnose different kinds of overload choices.
      if (!choice)
        return false;

      localAmbiguity.insert(choice);
    }
  }

  if (localAmbiguity.empty())
    return false;

  // If all of the fixes are rooted in the same choice.
  if (localAmbiguity.size() == 1) {
    auto &primaryFix = aggregateFix.front();
    return primaryFix.second->diagnose(*primaryFix.first);
  }

  {
    auto fixKind = aggregateFix.front().second->getKind();
    if (llvm::all_of(
            aggregateFix, [&](const std::pair<const Solution *,
                                              const ConstraintFix *> &entry) {
              auto &fix = entry.second;
              return fix->getKind() == fixKind && fix->getLocator() == locator;
            })) {
      auto *primaryFix = aggregateFix.front().second;
      if (primaryFix->diagnoseForAmbiguity(aggregateFix))
        return true;
    }
  }

  auto *decl = *localAmbiguity.begin();
  auto *commonCalleeLocator = ambiguity.locator;

  bool diagnosed = true;
  {
    DiagnosticTransaction transaction(DE);

    auto commonAnchor = commonCalleeLocator->getAnchor();
    if (auto *callExpr = getAsExpr<CallExpr>(commonAnchor))
      commonAnchor = callExpr->getDirectCallee();

    const auto name = decl->getName();

    // Emit an error message for the ambiguity.
    if (locator->isForContextualType()) {
      auto baseName = name.getBaseName();
      DE.diagnose(getLoc(commonAnchor), diag::no_candidates_match_result_type,
                  baseName.userFacingName(), cs.getContextualType(anchor));
    } else if (name.isOperator()) {
      auto *anchor = castToExpr(commonCalleeLocator->getAnchor());

      // If operator is "applied" e.g. `1 + 2` there are tailored
      // diagnostics in case of ambiguity, but if it's referenced
      // e.g. `arr.sort(by: <)` it's better to produce generic error
      // and a note per candidate.
      if (auto *parentExpr = cs.getParentExpr(anchor)) {
        if (isa<ApplyExpr>(parentExpr)) {
          diagnoseOperatorAmbiguity(cs, name.getBaseIdentifier(), solutions,
                                    commonCalleeLocator);
          return true;
        }
      }

      DE.diagnose(anchor->getLoc(), diag::no_overloads_match_exactly_in_call,
                  /*isApplication=*/false, decl->getDescriptiveKind(),
                  name.isSpecial(), name.getBaseName());
    } else {
      bool isApplication =
          llvm::any_of(cs.ArgumentInfos, [&](const auto &argInfo) {
            return argInfo.first->getAnchor() == commonAnchor;
          });

      DE.diagnose(getLoc(commonAnchor),
                  diag::no_overloads_match_exactly_in_call, isApplication,
                  decl->getDescriptiveKind(), name.isSpecial(),
                  name.getBaseName());
    }

    // Produce candidate notes
    SmallPtrSet<ValueDecl *, 4> distinctChoices;
    llvm::SmallSet<CanType, 4> candidateTypes;
    for (const auto &solution : solutions) {
      auto overload = solution.getOverloadChoice(commonCalleeLocator);
      auto *decl = overload.choice.getDecl();
      auto type = solution.simplifyType(overload.openedType);
      // Skip if we've already produced a note for this overload
      if (!distinctChoices.insert(decl).second)
        continue;

      auto noteLoc =
          decl->getLoc().isInvalid() ? getLoc(commonAnchor) : decl->getLoc();

      if (solution.Fixes.size() == 1) {
        diagnosed &=
            solution.Fixes.front()->diagnose(solution, /*asNote*/ true);
      } else if (llvm::all_of(solution.Fixes, [&](ConstraintFix *fix) {
                   return fix->getLocator()
                       ->findLast<LocatorPathElt::ApplyArgument>()
                       .hasValue();
                 })) {
        // All fixes have to do with arguments, so let's show the parameter
        // lists.
        //
        // It's possible that function type is wrapped in an optional
        // if it's from `@objc optional` method, so we need to ignore that.
        auto *fn =
            type->lookThroughAllOptionalTypes()->getAs<AnyFunctionType>();
        assert(fn);

        if (fn->getNumParams() == 1) {
          const auto &param = fn->getParams()[0];
          DE.diagnose(noteLoc, diag::candidate_has_invalid_argument_at_position,
                      solution.simplifyType(param.getPlainType()),
                      /*position=*/1, param.isInOut());
        } else {
          DE.diagnose(noteLoc, diag::candidate_partial_match,
                      fn->getParamListAsString(fn->getParams()));
        }
      } else {
        // Emit a general "found candidate" note
        if (decl->getLoc().isInvalid()) {
          if (candidateTypes.insert(type->getCanonicalType()).second)
            DE.diagnose(getLoc(commonAnchor), diag::found_candidate_type, type);
        } else {
          DE.diagnose(noteLoc, diag::found_candidate);
        }
      }
    }

    // If not all of the fixes produced a note, we can't diagnose this.
    if (!diagnosed)
      transaction.abort();
  }

  return diagnosed;
}

bool ConstraintSystem::diagnoseAmbiguityWithFixes(
    SmallVectorImpl<Solution> &solutions) {
  if (solutions.empty())
    return false;

  SolutionDiff solutionDiff(solutions);

  if (diagnoseConflictingGenericArguments(*this, solutionDiff, solutions))
    return true;

  if (auto bestScore = solverState->BestScore) {
    solutions.erase(llvm::remove_if(solutions,
                                    [&](const Solution &solution) {
                                      return solution.getFixedScore() >
                                             *bestScore;
                                    }),
                    solutions.end());

    if (llvm::all_of(solutions, [&](const Solution &solution) {
          auto score = solution.getFixedScore();
          return score.Data[SK_Fix] == 0 && solution.Fixes.empty();
        }))
      return false;
  }

  if (solutions.size() < 2)
    return false;

  if (diagnoseAmbiguityWithEphemeralPointers(*this, solutions))
    return true;

  if (isDebugMode()) {
    auto &log = llvm::errs();
    log << "--- Ambiguity: Considering #" << solutions.size()
        << " solutions with fixes ---\n";
    int i = 0;
    for (auto &solution : solutions) {
      log << "--- Solution #" << i++ << "---\n";
      solution.dump(log);
      log << "\n";
    }
  }

  // Algorithm is as follows:
  //
  // a. Aggregate all of the available fixes based on callee locator;
  // b. For each ambiguous overload match aggregated fixes and diagnose;
  // c. Discard all of the fixes which have been already considered
  //    as part of overload diagnostics;
  // d. Diagnose remaining (uniqued based on kind + locator) fixes
  //    iff they appear in all of the solutions.

  using Fix = std::pair<const Solution *, const ConstraintFix *>;

  llvm::SmallSetVector<Fix, 4> fixes;
  for (auto &solution : solutions) {
    for (auto *fix : solution.Fixes)
      fixes.insert({&solution, fix});
  }

  llvm::MapVector<ConstraintLocator *, SmallVector<Fix, 4>> fixesByCallee;
  llvm::SmallVector<Fix, 4> contextualFixes;

  for (const auto &entry : fixes) {
    const auto &solution = *entry.first;
    const auto *fix = entry.second;

    auto *locator = fix->getLocator();

    if (locator->isForContextualType()) {
      contextualFixes.push_back({&solution, fix});
      continue;
    }

    auto *calleeLocator = solution.getCalleeLocator(locator);
    fixesByCallee[calleeLocator].push_back({&solution, fix});
  }

  bool diagnosed = false;

  // All of the fixes which have been considered already.
  llvm::SmallSetVector<Fix, 4> consideredFixes;

  for (const auto &ambiguity : solutionDiff.overloads) {
    auto fixes = fixesByCallee.find(ambiguity.locator);
    if (fixes == fixesByCallee.end())
      continue;

    auto aggregate = fixes->second;
    diagnosed |= ::diagnoseAmbiguity(*this, ambiguity, aggregate, solutions);

    consideredFixes.insert(aggregate.begin(), aggregate.end());
  }

  if (diagnoseAmbiguityWithContextualType(*this, solutionDiff, contextualFixes,
                                          solutions)) {
    consideredFixes.insert(contextualFixes.begin(), contextualFixes.end());
    diagnosed |= true;
  }

  // Remove all of the fixes which have been attached to ambiguous
  // overload choices.
  fixes.set_subtract(consideredFixes);

  llvm::MapVector<std::pair<FixKind, ConstraintLocator *>, SmallVector<Fix, 4>>
      fixesByKind;

  for (const auto &entry : fixes) {
    const auto *fix = entry.second;
    fixesByKind[{fix->getKind(), fix->getLocator()}].push_back(
        {entry.first, fix});
  }

  // If leftover fix is contained in all of the solutions let's
  // diagnose it as ambiguity.
  for (const auto &entry : fixesByKind) {
    if (llvm::all_of(solutions, [&](const Solution &solution) -> bool {
          return llvm::any_of(
              solution.Fixes, [&](const ConstraintFix *fix) -> bool {
                return std::make_pair(fix->getKind(), fix->getLocator()) ==
                       entry.first;
              });
        })) {
      auto &aggregate = entry.second;
      diagnosed |= aggregate.front().second->diagnoseForAmbiguity(aggregate);
    }
  }

  return diagnosed;
}

/// Determine the number of distinct overload choices in the
/// provided set.
static unsigned countDistinctOverloads(ArrayRef<OverloadChoice> choices) {
  llvm::SmallPtrSet<void *, 4> uniqueChoices;
  unsigned result = 0;
  for (auto choice : choices) {
    if (uniqueChoices.insert(choice.getOpaqueChoiceSimple()).second)
      ++result;
  }
  return result;
}

/// Determine the name of the overload in a set of overload choices.
static DeclName getOverloadChoiceName(ArrayRef<OverloadChoice> choices) {
  DeclName name;
  for (auto choice : choices) {
    if (!choice.isDecl())
      continue;

    const DeclName nextName = choice.getDecl()->getName();
    if (!name) {
      name = nextName;
      continue;
    }

    if (name != nextName) {
      // Assume all choices have the same base name and only differ in
      // argument labels. This may not be a great assumption, but we don't
      // really have a way to recover for diagnostics otherwise.
      return name.getBaseName();
    }
  }

  return name;
}

/// Extend the given index map with all of the subexpressions in the given
/// expression.
static void extendPreorderIndexMap(
    Expr *expr, llvm::DenseMap<Expr *, unsigned> &indexMap) {
  class RecordingTraversal : public ASTWalker {
  public:
    llvm::DenseMap<Expr *, unsigned> &IndexMap;
    unsigned Index = 0;

    explicit RecordingTraversal(llvm::DenseMap<Expr *, unsigned> &indexMap)
      : IndexMap(indexMap) { }

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      IndexMap[E] = Index;
      ++Index;
      return { true, E };
    }
  };

  RecordingTraversal traversal(indexMap);
  expr->walk(traversal);
}

bool ConstraintSystem::diagnoseAmbiguity(ArrayRef<Solution> solutions) {
  // Produce a diff of the solutions.
  SolutionDiff diff(solutions);

  // Find the locators which have the largest numbers of distinct overloads.
  Optional<unsigned> bestOverload;
  // Overloads are scored by lexicographical comparison of (# of distinct
  // overloads, depth, *reverse* of the index). N.B. - cannot be used for the
  // reversing: the score version of index == 0 should be > than that of 1, but
  // -0 == 0 < UINT_MAX == -1, whereas ~0 == UINT_MAX > UINT_MAX - 1 == ~1.
  auto score = [](unsigned distinctOverloads, unsigned depth, unsigned index) {
    return std::make_tuple(distinctOverloads, depth, ~index);
  };
  auto bestScore = score(0, 0, std::numeric_limits<unsigned>::max());

  // Get a map of expressions to their depths and post-order traversal indices.
  // Heuristically, all other things being equal, we should complain about the
  // ambiguous expression that (1) has the most overloads, (2) is deepest, or
  // (3) comes earliest in the expression.
  llvm::DenseMap<Expr *, unsigned> indexMap;
  for (auto expr : InputExprs) {
    extendPreorderIndexMap(expr, indexMap);
  }

  for (unsigned i = 0, n = diff.overloads.size(); i != n; ++i) {
    auto &overload = diff.overloads[i];

    // If we can't resolve the locator to an anchor expression with no path,
    // we can't diagnose this well.
    auto *anchor = getAsExpr(simplifyLocatorToAnchor(overload.locator));
    if (!anchor)
      continue;

    auto it = indexMap.find(castToExpr(anchor));
    if (it == indexMap.end())
      continue;
    unsigned index = it->second;

    auto optDepth = getExprDepth(castToExpr(anchor));
    if (!optDepth)
      continue;
    unsigned depth = *optDepth;

    // If we don't have a name to hang on to, it'll be hard to diagnose this
    // overload.
    if (!getOverloadChoiceName(overload.choices))
      continue;

    unsigned distinctOverloads = countDistinctOverloads(overload.choices);

    // We need at least two overloads to make this interesting.
    if (distinctOverloads < 2)
      continue;

    // If we have more distinct overload choices for this locator than for
    // prior locators, just keep this locator.
    auto thisScore = score(distinctOverloads, depth, index);
    if (thisScore > bestScore) {
      bestScore = thisScore;
      bestOverload = i;
      continue;
    }

    // We have better results. Ignore this one.
  }

  // FIXME: Should be able to pick the best locator, e.g., based on some
  // depth-first numbering of expressions.
  if (bestOverload) {
    auto &overload = diff.overloads[*bestOverload];
    // FIXME: We would prefer to emit the name as written, but that information
    // is not sufficiently centralized in the AST.
    DeclNameRef name(getOverloadChoiceName(overload.choices));
    auto anchor = simplifyLocatorToAnchor(overload.locator);

    // Emit the ambiguity diagnostic.
    auto &DE = getASTContext().Diags;
    DE.diagnose(getLoc(anchor),
                name.isOperator() ? diag::ambiguous_operator_ref
                                  : diag::ambiguous_decl_ref,
                name);

    TrailingClosureAmbiguityFailure failure(solutions, anchor,
                                            overload.choices);
    if (failure.diagnoseAsNote())
      return true;

    // Emit candidates.  Use a SmallPtrSet to make sure only emit a particular
    // candidate once.  FIXME: Why is one candidate getting into the overload
    // set multiple times? (See also tryDiagnoseTrailingClosureAmbiguity.)
    SmallPtrSet<Decl *, 8> EmittedDecls;
    for (auto choice : overload.choices) {
      switch (choice.getKind()) {
      case OverloadChoiceKind::Decl:
      case OverloadChoiceKind::DeclViaDynamic:
      case OverloadChoiceKind::DeclViaBridge:
      case OverloadChoiceKind::DeclViaUnwrappedOptional:
        // FIXME: show deduced types, etc, etc.
        if (EmittedDecls.insert(choice.getDecl()).second)
          DE.diagnose(choice.getDecl(), diag::found_candidate);
        break;

      case OverloadChoiceKind::KeyPathApplication:
      case OverloadChoiceKind::DynamicMemberLookup:
      case OverloadChoiceKind::KeyPathDynamicMemberLookup:
        // Skip key path applications and dynamic member lookups, since we don't
        // want them to noise up unrelated subscript diagnostics.
        break;

      case OverloadChoiceKind::TupleIndex:
        // FIXME: Actually diagnose something here.
        break;
      }
    }

    return true;
  }

  // FIXME: If we inferred different types for literals (for example),
  // could diagnose ambiguity that way as well.

  return false;
}

ConstraintLocator *
constraints::simplifyLocator(ConstraintSystem &cs, ConstraintLocator *locator,
                             SourceRange &range) {
  auto path = locator->getPath();
  auto anchor = locator->getAnchor();
  simplifyLocator(anchor, path, range);

  // If we didn't simplify anything, just return the input.
  if (anchor == locator->getAnchor() &&
      path.size() == locator->getPath().size()) {
    return locator;
  }

  // If the old locator didn't have any summary flags, neither will the
  // simplified version, as it must contain a subset of the path elements.
  if (locator->getSummaryFlags() == 0)
    return cs.getConstraintLocator(anchor, path, /*summaryFlags*/ 0);

  return cs.getConstraintLocator(anchor, path);
}

void constraints::simplifyLocator(ASTNode &anchor,
                                  ArrayRef<LocatorPathElt> &path,
                                  SourceRange &range) {
  range = SourceRange();

  while (!path.empty()) {
    switch (path[0].getKind()) {
    case ConstraintLocator::ApplyArgument: {
      // Extract application argument.
      if (auto applyExpr = getAsExpr<ApplyExpr>(anchor)) {
        anchor = applyExpr->getArg();
        path = path.slice(1);
        continue;
      }

      if (auto subscriptExpr = getAsExpr<SubscriptExpr>(anchor)) {
        anchor = subscriptExpr->getIndex();
        path = path.slice(1);

        // TODO: It would be better if the index expression was always wrapped
        // in a ParenExpr (if there is no label).
        if (!(isExpr<TupleExpr>(anchor) || isExpr<ParenExpr>(anchor)) &&
            !path.empty() && path[0].is<LocatorPathElt::ApplyArgToParam>()) {
          path = path.slice(1);
        }
        continue;
      }

      if (auto objectLiteralExpr = getAsExpr<ObjectLiteralExpr>(anchor)) {
        anchor = objectLiteralExpr->getArg();
        path = path.slice(1);
        continue;
      }

      break;
    }

    case ConstraintLocator::DynamicCallable: {
      path = path.slice(1);
      continue;
    }

    case ConstraintLocator::ApplyFunction:
    case ConstraintLocator::FunctionResult:
      // Extract application function.
      if (auto applyExpr = getAsExpr<ApplyExpr>(anchor)) {
        anchor = applyExpr->getFn();
        path = path.slice(1);
        continue;
      }

      // The subscript itself is the function.
      if (auto subscriptExpr = getAsExpr<SubscriptExpr>(anchor)) {
        anchor = subscriptExpr;
        path = path.slice(1);
        continue;
      }

      break;

    case ConstraintLocator::AutoclosureResult:
    case ConstraintLocator::LValueConversion:
    case ConstraintLocator::DynamicType:
    case ConstraintLocator::UnresolvedMember:
    case ConstraintLocator::ImplicitCallAsFunction:
      // Arguments in autoclosure positions, lvalue and rvalue adjustments,
      // unresolved members, and implicit callAsFunction references are
      // implicit.
      path = path.slice(1);
      continue;

    case ConstraintLocator::NamedTupleElement:
    case ConstraintLocator::TupleElement: {
      // Extract tuple element.
      auto elt = path[0].castTo<LocatorPathElt::AnyTupleElement>();
      unsigned index = elt.getIndex();
      if (auto tupleExpr = getAsExpr<TupleExpr>(anchor)) {
        if (index < tupleExpr->getNumElements()) {
          anchor = tupleExpr->getElement(index);
          path = path.slice(1);
          continue;
        }
      }

      if (auto *CE = getAsExpr<CollectionExpr>(anchor)) {
        if (index < CE->getNumElements()) {
          anchor = CE->getElement(index);
          path = path.slice(1);
          continue;
        }
      }
      break;
    }

    case ConstraintLocator::ApplyArgToParam: {
      auto elt = path[0].castTo<LocatorPathElt::ApplyArgToParam>();
      // Extract tuple element.
      if (auto tupleExpr = getAsExpr<TupleExpr>(anchor)) {
        unsigned index = elt.getArgIdx();
        if (index < tupleExpr->getNumElements()) {
          anchor = tupleExpr->getElement(index);
          path = path.slice(1);
          continue;
        }
      }

      // Extract subexpression in parentheses.
      if (auto parenExpr = getAsExpr<ParenExpr>(anchor)) {
        // This simplication request could be for a synthesized argument.
        if (elt.getArgIdx() == 0) {
          anchor = parenExpr->getSubExpr();
          path = path.slice(1);
          continue;
        }
      }
      break;
    }
    case ConstraintLocator::ConstructorMember:
      if (auto typeExpr = getAsExpr<TypeExpr>(anchor)) {
        // This is really an implicit 'init' MemberRef, so point at the base,
        // i.e. the TypeExpr.
        range = SourceRange();
        anchor = typeExpr;
        path = path.slice(1);
        continue;
      }
      LLVM_FALLTHROUGH;

    case ConstraintLocator::Member:
    case ConstraintLocator::MemberRefBase:
      if (auto UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
        range = UDE->getNameLoc().getSourceRange();
        anchor = UDE->getBase();
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::SubscriptMember:
      if (isExpr<SubscriptExpr>(anchor)) {
        path = path.slice(1);
        continue;
      }
      break;

    case ConstraintLocator::ClosureBody:
    case ConstraintLocator::ClosureResult:
      if (auto CE = getAsExpr<ClosureExpr>(anchor)) {
        if (CE->hasSingleExpressionBody()) {
          anchor = CE->getSingleExpressionBody();
          path = path.slice(1);
          continue;
        }
      }
      break;

    case ConstraintLocator::ContextualType:
      // This was just for identifying purposes, strip it off.
      path = path.slice(1);
      continue;

    case ConstraintLocator::KeyPathComponent: {
      auto elt = path[0].castTo<LocatorPathElt::KeyPathComponent>();

      // If the next element is an ApplyArgument, we can simplify by looking
      // into the index expression.
      if (path.size() < 2 ||
          path[1].getKind() != ConstraintLocator::ApplyArgument)
        break;

      if (auto *kpe = getAsExpr<KeyPathExpr>(anchor)) {
        auto component = kpe->getComponents()[elt.getIndex()];
        auto indexExpr = component.getIndexExpr();
        assert(indexExpr && "Trying to apply a component without an index?");
        anchor = indexExpr;
        path = path.slice(2);
        continue;
      }
      break;
    }

    case ConstraintLocator::Condition: {
      anchor = castToExpr<IfExpr>(anchor)->getCondExpr();
      path = path.slice(1);
      continue;
    }

    case ConstraintLocator::TernaryBranch: {
      auto branch = path[0].castTo<LocatorPathElt::TernaryBranch>();
      auto *ifExpr = castToExpr<IfExpr>(anchor);

      anchor = branch.forThen() ? ifExpr->getThenExpr() : ifExpr->getElseExpr();
      path = path.slice(1);
      continue;
    }

    case ConstraintLocator::KeyPathDynamicMember: {
      // Key path dynamic member lookup should be completely transparent.
      path = path.slice(1);
      continue;
    }

    case ConstraintLocator::ArgumentAttribute: {
      // At this point we should have already found argument expression
      // this attribute belogs to, so we can leave this element in place
      // because it points out exact location useful for diagnostics.
      break;
    }

    case ConstraintLocator::ResultBuilderBodyResult: {
      path = path.slice(1);
      break;
    }

    case ConstraintLocator::UnresolvedMemberChainResult: {
      auto *resultExpr = castToExpr<UnresolvedMemberChainResultExpr>(anchor);
      anchor = resultExpr->getSubExpr();
      path = path.slice(1);
      continue;
    }

    default:
      // FIXME: Lots of other cases to handle.
      break;
    }

    // If we get here, we couldn't simplify the path further.
    break;
  }
}

ASTNode constraints::simplifyLocatorToAnchor(ConstraintLocator *locator) {
  if (!locator)
    return nullptr;

  auto anchor = locator->getAnchor();
  if (!anchor)
    return {};

  SourceRange range;
  auto path = locator->getPath();
  simplifyLocator(anchor, path, range);

  // We only want the new anchor if all the path elements have been simplified
  // away.
  return path.empty() ? anchor : nullptr;
}

Expr *constraints::getArgumentExpr(ASTNode node, unsigned index) {
  auto *expr = getAsExpr(node);
  if (!expr)
    return nullptr;

  Expr *argExpr = nullptr;
  if (auto *AE = dyn_cast<ApplyExpr>(expr))
    argExpr = AE->getArg();
  else if (auto *SE = dyn_cast<SubscriptExpr>(expr))
    argExpr = SE->getIndex();
  else
    return nullptr;

  if (auto *PE = dyn_cast<ParenExpr>(argExpr)) {
    assert(index == 0);
    return PE->getSubExpr();
  }

  if (auto *tuple = dyn_cast<TupleExpr>(argExpr)) {
    return (tuple->getNumElements() > index) ? tuple->getElement(index)
                                             : nullptr;
  }

  return nullptr;
}

bool constraints::isAutoClosureArgument(Expr *argExpr) {
  if (!argExpr)
    return false;

  if (auto *DRE = dyn_cast<DeclRefExpr>(argExpr)) {
    if (auto *param = dyn_cast<ParamDecl>(DRE->getDecl()))
      return param->isAutoClosure();
  }

  return false;
}

bool constraints::hasAppliedSelf(ConstraintSystem &cs,
                                 const OverloadChoice &choice) {
  return hasAppliedSelf(choice, [&cs](Type type) -> Type {
    return cs.getFixedTypeRecursive(type, /*wantRValue=*/true);
  });
}

bool constraints::hasAppliedSelf(const OverloadChoice &choice,
                                 llvm::function_ref<Type(Type)> getFixedType) {
  auto *decl = choice.getDeclOrNull();
  if (!decl)
    return false;

  auto baseType = choice.getBaseType();
  if (baseType)
    baseType = getFixedType(baseType)->getRValueType();

  // In most cases where we reference a declaration with a curried self
  // parameter, it gets dropped from the type of the reference.
  return decl->hasCurriedSelf() &&
         doesMemberRefApplyCurriedSelf(baseType, decl);
}

bool constraints::conformsToKnownProtocol(DeclContext *dc, Type type,
                                          KnownProtocolKind protocol) {
  if (auto *proto =
          TypeChecker::getProtocol(dc->getASTContext(), SourceLoc(), protocol))
    return (bool)TypeChecker::conformsToProtocol(type, proto, dc);
  return false;
}

/// Check whether given type conforms to `RawPepresentable` protocol
/// and return the witness type.
Type constraints::isRawRepresentable(ConstraintSystem &cs, Type type) {
  auto *DC = cs.DC;

  auto rawReprType = TypeChecker::getProtocol(
      cs.getASTContext(), SourceLoc(), KnownProtocolKind::RawRepresentable);
  if (!rawReprType)
    return Type();

  auto conformance = TypeChecker::conformsToProtocol(type, rawReprType, DC);
  if (conformance.isInvalid())
    return Type();

  return conformance.getTypeWitnessByName(type, cs.getASTContext().Id_RawValue);
}

Type constraints::isRawRepresentable(
    ConstraintSystem &cs, Type type,
    KnownProtocolKind rawRepresentableProtocol) {
  Type rawTy = isRawRepresentable(cs, type);
  if (!rawTy ||
      !conformsToKnownProtocol(cs.DC, rawTy, rawRepresentableProtocol))
    return Type();

  return rawTy;
}

void ConstraintSystem::generateConstraints(
    SmallVectorImpl<Constraint *> &constraints, Type type,
    ArrayRef<OverloadChoice> choices, DeclContext *useDC,
    ConstraintLocator *locator, Optional<unsigned> favoredIndex,
    bool requiresFix,
    llvm::function_ref<ConstraintFix *(unsigned, const OverloadChoice &)>
        getFix) {
  auto recordChoice = [&](SmallVectorImpl<Constraint *> &choices,
                          unsigned index, const OverloadChoice &overload,
                          bool isFavored = false) {
    auto *fix = getFix(index, overload);
    // If fix is required but it couldn't be determined, this
    // choice has be filtered out.
    if (requiresFix && !fix)
      return;

    auto *choice = fix ? Constraint::createFixedChoice(*this, type, overload,
                                                       useDC, fix, locator)
                       : Constraint::createBindOverload(*this, type, overload,
                                                        useDC, locator);

    if (isFavored)
      choice->setFavored();

    choices.push_back(choice);
  };

  if (favoredIndex) {
    const auto &choice = choices[*favoredIndex];
    assert(
        (!choice.isDecl() || !isDeclUnavailable(choice.getDecl(), locator)) &&
        "Cannot make unavailable decl favored!");
    recordChoice(constraints, *favoredIndex, choice, /*isFavored=*/true);
  }

  for (auto index : indices(choices)) {
    if (favoredIndex && (*favoredIndex == index))
      continue;

    recordChoice(constraints, index, choices[index]);
  }
}

ConstraintLocator *
ConstraintSystem::getArgumentInfoLocator(ConstraintLocator *locator) {
  auto anchor = locator->getAnchor();

  // An empty locator which code completion uses for member references.
  if (anchor.isNull() && locator->getPath().empty())
    return nullptr;

  // Applies and unresolved member exprs can have callee locators that are
  // dependent on the type of their function, which may not have been resolved
  // yet. Therefore we need to handle them specially.
  if (auto *apply = getAsExpr<ApplyExpr>(anchor)) {
    auto *fnExpr = getArgumentLabelTargetExpr(apply->getFn());
    return getConstraintLocator(fnExpr);
  }

  if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor))
    return getConstraintLocator(UME);

  auto path = locator->getPath();
  {
    // If this is for a dynamic member reference, the argument info is for the
    // original call-site, which we can get by stripping away the
    // KeyPathDynamicMember elements.
    auto iter = path.begin();
    if (locator->findFirst<LocatorPathElt::KeyPathDynamicMember>(iter)) {
      ArrayRef<LocatorPathElt> newPath(path.begin(), iter);
      return getConstraintLocator(anchor, newPath);
    }
  }

  return getCalleeLocator(locator);
}

Optional<ConstraintSystem::ArgumentInfo>
ConstraintSystem::getArgumentInfo(ConstraintLocator *locator) {
  if (!locator)
    return None;

  if (auto *infoLocator = getArgumentInfoLocator(locator)) {
    auto known = ArgumentInfos.find(infoLocator);
    if (known != ArgumentInfos.end())
      return known->second;
  }
  return None;
}

/// Given an apply expr, returns true if it is expected to have a direct callee
/// overload, resolvable using `getChoiceFor`. Otherwise, returns false.
static bool shouldHaveDirectCalleeOverload(const CallExpr *callExpr) {
  auto *fnExpr = callExpr->getDirectCallee();

  // An apply of an apply/subscript doesn't have a direct callee.
  if (isa<ApplyExpr>(fnExpr) || isa<SubscriptExpr>(fnExpr))
    return false;

  // Applies of closures don't have callee overloads.
  if (isa<ClosureExpr>(fnExpr))
    return false;

  // No direct callee for a try!/try?.
  if (isa<ForceTryExpr>(fnExpr) || isa<OptionalTryExpr>(fnExpr))
    return false;

  // If we have an intermediate cast, there's no direct callee.
  if (isa<ExplicitCastExpr>(fnExpr))
    return false;

  // No direct callee for an if expr.
  if (isa<IfExpr>(fnExpr))
    return false;

  // Assume that anything else would have a direct callee.
  return true;
}

Type Solution::resolveInterfaceType(Type type) const {
  auto resolvedType = type.transform([&](Type type) -> Type {
    if (auto *tvt = type->getAs<TypeVariableType>()) {
      // If this type variable is for a generic parameter, return that.
      if (auto *gp = tvt->getImpl().getGenericParameter())
        return gp;

      // Otherwise resolve its fixed type, mapped out of context.
      auto fixed = simplifyType(tvt);
      return resolveInterfaceType(fixed->mapTypeOutOfContext());
    }
    if (auto *dmt = type->getAs<DependentMemberType>()) {
      // For a dependent member, first resolve the base.
      auto newBase = resolveInterfaceType(dmt->getBase());

      // Then reconstruct using its associated type.
      assert(dmt->getAssocType());
      return DependentMemberType::get(newBase, dmt->getAssocType());
    }
    return type;
  });

  assert(!resolvedType->hasArchetype());
  return resolvedType;
}

Optional<FunctionArgApplyInfo>
Solution::getFunctionArgApplyInfo(ConstraintLocator *locator) const {
  auto &cs = getConstraintSystem();

  // It's only valid to use `&` in argument positions, but we need
  // to figure out exactly where it was used.
  if (auto *argExpr = getAsExpr<InOutExpr>(locator->getAnchor())) {
    auto argInfo = cs.isArgumentExpr(argExpr);
    assert(argInfo && "Incorrect use of `inout` expression");

    Expr *call;
    unsigned argIdx;

    std::tie(call, argIdx) = *argInfo;

    ParameterTypeFlags flags;
    locator = cs.getConstraintLocator(
        call, {ConstraintLocator::ApplyArgument,
               LocatorPathElt::ApplyArgToParam(argIdx, argIdx,
                                               flags.withInOut(true))});
  }

  auto anchor = locator->getAnchor();
  auto path = locator->getPath();

  // Look for the apply-arg-to-param element in the locator's path. We may
  // have to look through other elements that are generated from an argument
  // conversion such as GenericArgument for an optional-to-optional conversion,
  // and OptionalPayload for a value-to-optional conversion.
  auto iter = path.rbegin();
  auto applyArgElt = locator->findLast<LocatorPathElt::ApplyArgToParam>(iter);
  if (!applyArgElt)
    return None;

  auto nextIter = iter + 1;
  assert(!locator->findLast<LocatorPathElt::ApplyArgToParam>(nextIter) &&
         "Multiple ApplyArgToParam components?");

  // Form a new locator that ends at the apply-arg-to-param element, and
  // simplify it to get the full argument expression.
  auto argPath = path.drop_back(iter - path.rbegin());
  auto *argLocator = getConstraintLocator(anchor, argPath);

  auto *argExpr = castToExpr(simplifyLocatorToAnchor(argLocator));

  // If we were unable to simplify down to the argument expression, we don't
  // know what this is.
  if (!argExpr)
    return None;

  Optional<OverloadChoice> choice;
  Type rawFnType;
  auto *calleeLocator = getCalleeLocator(argLocator);
  if (auto overload = getOverloadChoiceIfAvailable(calleeLocator)) {
    // If we have resolved an overload for the callee, then use that to get the
    // function type and callee.
    choice = overload->choice;
    rawFnType = overload->openedType;
  } else {
    // If we didn't resolve an overload for the callee, we should be dealing
    // with a call of an arbitrary function expr.
    auto *call = castToExpr<CallExpr>(anchor);
    rawFnType = getType(call->getFn());

    // If callee couldn't be resolved due to expression
    // issues e.g. it's a reference to an invalid member
    // let's just return here.
    if (simplifyType(rawFnType)->is<UnresolvedType>())
      return None;

    assert(!shouldHaveDirectCalleeOverload(call) &&
             "Should we have resolved a callee for this?");
  }

  // Try to resolve the function type by loading lvalues and looking through
  // optional types, which can occur for expressions like `fn?(5)`.
  auto *fnType = simplifyType(rawFnType)
                     ->getRValueType()
                     ->lookThroughAllOptionalTypes()
                     ->getAs<FunctionType>();
  if (!fnType)
    return None;

  // Resolve the interface type for the function. Note that this may not be a
  // function type, for example it could be a generic parameter.
  Type fnInterfaceType;
  auto *callee = choice ? choice->getDeclOrNull() : nullptr;
  if (callee && callee->hasInterfaceType()) {
    // If we have a callee with an interface type, we can use it. This is
    // preferable to resolveInterfaceType, as this will allow us to get a
    // GenericFunctionType for generic decls.
    //
    // Note that it's possible to find a callee without an interface type. This
    // can happen for example with closure parameters, where the interface type
    // isn't set until the solution is applied. In that case, use
    // resolveInterfaceType.
    fnInterfaceType = callee->getInterfaceType();

    // Strip off the curried self parameter if necessary.
    if (hasAppliedSelf(
            *choice, [this](Type type) -> Type { return simplifyType(type); }))
      fnInterfaceType = fnInterfaceType->castTo<AnyFunctionType>()->getResult();

    if (auto *fn = fnInterfaceType->getAs<AnyFunctionType>()) {
      assert(fn->getNumParams() == fnType->getNumParams() &&
             "Parameter mismatch?");
      (void)fn;
    }
  } else {
    fnInterfaceType = resolveInterfaceType(rawFnType);
  }

  auto argIdx = applyArgElt->getArgIdx();
  auto paramIdx = applyArgElt->getParamIdx();

  return FunctionArgApplyInfo(cs.getParentExpr(argExpr), argExpr, argIdx,
                              simplifyType(getType(argExpr)), paramIdx,
                              fnInterfaceType, fnType, callee);
}

bool constraints::isKnownKeyPathType(Type type) {
  if (auto *BGT = type->getAs<BoundGenericType>())
    return isKnownKeyPathDecl(type->getASTContext(), BGT->getDecl());
  return false;
}

bool constraints::isKnownKeyPathDecl(ASTContext &ctx, ValueDecl *decl) {
  return decl == ctx.getKeyPathDecl() || decl == ctx.getWritableKeyPathDecl() ||
         decl == ctx.getReferenceWritableKeyPathDecl() ||
         decl == ctx.getPartialKeyPathDecl() || decl == ctx.getAnyKeyPathDecl();
}

bool constraints::hasExplicitResult(ClosureExpr *closure) {
  auto &ctx = closure->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           ClosureHasExplicitResultRequest{closure}, false);
}

Type constraints::getConcreteReplacementForProtocolSelfType(ValueDecl *member) {
  auto *DC = member->getDeclContext();

  if (!DC->getSelfProtocolDecl())
    return Type();

  GenericSignature signature;
  if (auto *genericContext = member->getAsGenericContext()) {
    signature = genericContext->getGenericSignature();
  } else {
    signature = DC->getGenericSignatureOfContext();
  }

  auto selfTy = DC->getProtocolSelfType();
  return signature->getConcreteType(selfTy);
}

static bool isOperator(Expr *expr, StringRef expectedName) {
  auto name = getOperatorName(expr);
  return name ? name->is(expectedName) : false;
}

Optional<Identifier> constraints::getOperatorName(Expr *expr) {
  ValueDecl *choice = nullptr;
  if (auto *ODRE = dyn_cast_or_null<OverloadedDeclRefExpr>(expr)) {
    choice = ODRE->getDecls().front();
  } else if (auto *DRE = dyn_cast_or_null<DeclRefExpr>(expr)) {
    choice = DRE->getDecl();
  } else {
    return None;
  }

  if (auto *FD = dyn_cast_or_null<AbstractFunctionDecl>(choice))
    return FD->getBaseIdentifier();

  return None;
}

bool constraints::isPatternMatchingOperator(ASTNode node) {
  auto *expr = getAsExpr(node);
  if (!expr) return false;

  return isOperator(expr, "~=");
}

bool constraints::isStandardComparisonOperator(ASTNode node) {
  auto *expr = getAsExpr(node);
  if (!expr) return false;

  if (auto opName = getOperatorName(expr)) {
    return opName->isStandardComparisonOperator();
  }
  return false;
}

Optional<std::pair<Expr *, unsigned>>
ConstraintSystem::isArgumentExpr(Expr *expr) {
  auto *argList = getParentExpr(expr);
  if (!argList) {
    return None;
  }

  if (isa<ParenExpr>(argList)) {
    for (;;) {
      auto *parent = getParentExpr(argList);
      if (!parent)
        return None;

      if (isa<TupleExpr>(parent)) {
        argList = parent;
        break;
      }

      // Drop all of the semantically insignificant parens
      // that might be wrapping an argument e.g. `test(((42)))`
      if (isa<ParenExpr>(parent)) {
        argList = parent;
        continue;
      }

      break;
    }
  }

  if (!(isa<ParenExpr>(argList) || isa<TupleExpr>(argList)))
    return None;

  auto *application = getParentExpr(argList);
  if (!application)
    return None;

  if (!(isa<ApplyExpr>(application) || isa<SubscriptExpr>(application) ||
        isa<ObjectLiteralExpr>(application)))
    return None;

  unsigned argIdx = 0;
  if (auto *tuple = dyn_cast<TupleExpr>(argList)) {
    auto arguments = tuple->getElements();

    for (auto idx : indices(arguments)) {
      if (arguments[idx]->getSemanticsProvidingExpr() == expr) {
        argIdx = idx;
        break;
      }
    }
  }

  return std::make_pair(application, argIdx);
}

bool constraints::isOperatorArgument(ConstraintLocator *locator,
                                     StringRef expectedOperator) {
  if (!locator->findLast<LocatorPathElt::ApplyArgToParam>())
    return false;

  if (auto *AE = getAsExpr<ApplyExpr>(locator->getAnchor())) {
    if (isa<PrefixUnaryExpr>(AE) || isa<BinaryExpr>(AE) ||
        isa<PostfixUnaryExpr>(AE))
      return expectedOperator.empty() ||
             isOperator(AE->getFn(), expectedOperator);
  }

  return false;
}

bool constraints::isArgumentOfPatternMatchingOperator(
    ConstraintLocator *locator) {
  auto *binaryOp = getAsExpr<BinaryExpr>(locator->getAnchor());
  if (!(binaryOp && binaryOp->isImplicit()))
    return false;
  return isPatternMatchingOperator(binaryOp->getFn());
}

bool constraints::isArgumentOfReferenceEqualityOperator(
    ConstraintLocator *locator) {
  return isOperatorArgument(locator, "===") ||
         isOperatorArgument(locator, "!==");
}

ConversionEphemeralness
ConstraintSystem::isConversionEphemeral(ConversionRestrictionKind conversion,
                                        ConstraintLocatorBuilder locator) {
  switch (conversion) {
  case ConversionRestrictionKind::ArrayToPointer:
  case ConversionRestrictionKind::StringToPointer:
    // Always ephemeral.
    return ConversionEphemeralness::Ephemeral;
  case ConversionRestrictionKind::InoutToPointer: {

    // Ephemeral, except if the expression is a reference to a global or
    // static stored variable, or a directly accessed stored property on such a
    // variable.

    auto isDirectlyAccessedStoredVar = [&](ValueDecl *decl) -> bool {
      auto *asd = dyn_cast_or_null<AbstractStorageDecl>(decl);
      if (!asd)
        return false;

      // Check what access strategy is used for a read-write access. It must be
      // direct-to-storage in order for the conversion to be non-ephemeral.
      auto access = asd->getAccessStrategy(
          AccessSemantics::Ordinary, AccessKind::ReadWrite,
          DC->getParentModule(), DC->getResilienceExpansion());
      return access.getKind() == AccessStrategy::Storage;
    };

    SourceRange range;
    auto *argLoc = simplifyLocator(*this, getConstraintLocator(locator), range);
    auto *subExpr =
        castToExpr(argLoc->getAnchor())->getSemanticsProvidingExpr();

    // Look through an InOutExpr if we have one. This is usually the case, but
    // might not be if e.g we're applying an 'add missing &' fix.
    if (auto *ioe = dyn_cast<InOutExpr>(subExpr))
      subExpr = ioe->getSubExpr();

    while (true) {
      subExpr = subExpr->getSemanticsProvidingExpr();

      // Look through force unwraps, which can be modelled as physical lvalue
      // components.
      if (auto *fve = dyn_cast<ForceValueExpr>(subExpr)) {
        subExpr = fve->getSubExpr();
        continue;
      }

      // Look through a member reference if it's directly accessed.
      if (auto *ude = dyn_cast<UnresolvedDotExpr>(subExpr)) {
        auto overload = findSelectedOverloadFor(ude);

        // If we didn't find an overload, it hasn't been resolved yet.
        if (!overload)
          return ConversionEphemeralness::Unresolved;

        // Tuple indices are always non-ephemeral.
        auto *base = ude->getBase();
        if (overload->choice.getKind() == OverloadChoiceKind::TupleIndex) {
          subExpr = base;
          continue;
        }

        // If we don't have a directly accessed declaration associated with the
        // choice, it's ephemeral.
        auto *member = overload->choice.getDeclOrNull();
        if (!isDirectlyAccessedStoredVar(member))
          return ConversionEphemeralness::Ephemeral;

        // If we found a static member, the conversion is non-ephemeral. We can
        // stop iterating as there's nothing interesting about the base.
        if (member->isStatic())
          return ConversionEphemeralness::NonEphemeral;

        // For an instance member, the base must be an @lvalue struct type.
        if (auto *lvt = simplifyType(getType(base))->getAs<LValueType>()) {
          auto *nominal = lvt->getObjectType()->getAnyNominal();
          if (nominal && isa<StructDecl>(nominal)) {
            subExpr = base;
            continue;
          }
        }
        return ConversionEphemeralness::Ephemeral;
      }

      break;
    }

    auto getBaseEphemeralness =
        [&](ValueDecl *base) -> ConversionEphemeralness {
      // We must have a base decl that's directly accessed.
      if (!isDirectlyAccessedStoredVar(base))
        return ConversionEphemeralness::Ephemeral;

      // The base decl must either be static or global in order for it to be
      // non-ephemeral.
      if (base->isStatic() || base->getDeclContext()->isModuleScopeContext()) {
        return ConversionEphemeralness::NonEphemeral;
      } else {
        return ConversionEphemeralness::Ephemeral;
      }
    };

    // Fast path: We have a direct decl ref.
    if (auto *dre = dyn_cast<DeclRefExpr>(subExpr))
      return getBaseEphemeralness(dre->getDecl());

    // Otherwise, try to find an overload for the base.
    if (auto baseOverload = findSelectedOverloadFor(subExpr))
      return getBaseEphemeralness(baseOverload->choice.getDeclOrNull());

    // If we didn't find a base overload for a unresolved member or overloaded
    // decl, it hasn't been resolved yet.
    if (isa<UnresolvedMemberExpr>(subExpr) ||
        isa<OverloadedDeclRefExpr>(subExpr))
      return ConversionEphemeralness::Unresolved;

    // Otherwise, we don't know what we're dealing with. Default to ephemeral.
    return ConversionEphemeralness::Ephemeral;
  }
  case ConversionRestrictionKind::DeepEquality:
  case ConversionRestrictionKind::Superclass:
  case ConversionRestrictionKind::Existential:
  case ConversionRestrictionKind::MetatypeToExistentialMetatype:
  case ConversionRestrictionKind::ExistentialMetatypeToMetatype:
  case ConversionRestrictionKind::ValueToOptional:
  case ConversionRestrictionKind::OptionalToOptional:
  case ConversionRestrictionKind::ClassMetatypeToAnyObject:
  case ConversionRestrictionKind::ExistentialMetatypeToAnyObject:
  case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass:
  case ConversionRestrictionKind::PointerToPointer:
  case ConversionRestrictionKind::ArrayUpcast:
  case ConversionRestrictionKind::DictionaryUpcast:
  case ConversionRestrictionKind::SetUpcast:
  case ConversionRestrictionKind::HashableToAnyHashable:
  case ConversionRestrictionKind::CFTollFreeBridgeToObjC:
  case ConversionRestrictionKind::ObjCTollFreeBridgeToCF:
  case ConversionRestrictionKind::CGFloatToDouble:
  case ConversionRestrictionKind::DoubleToCGFloat:
    // @_nonEphemeral has no effect on these conversions, so treat them as all
    // being non-ephemeral in order to allow their passing to an @_nonEphemeral
    // parameter.
    return ConversionEphemeralness::NonEphemeral;
  }
  llvm_unreachable("invalid conversion restriction kind");
}

Expr *ConstraintSystem::buildAutoClosureExpr(Expr *expr,
                                             FunctionType *closureType,
                                             bool isDefaultWrappedValue,
                                             bool isAsyncLetWrapper) {
  auto &Context = DC->getASTContext();
  bool isInDefaultArgumentContext = false;
  if (auto *init = dyn_cast<Initializer>(DC)) {
    auto initKind = init->getInitializerKind();
    isInDefaultArgumentContext =
        initKind == InitializerKind::DefaultArgument ||
        (initKind == InitializerKind::PatternBinding && isDefaultWrappedValue);
  }

  auto info = closureType->getExtInfo();
  auto newClosureType = closureType;

  if (isInDefaultArgumentContext && info.isNoEscape())
    newClosureType = closureType->withExtInfo(info.withNoEscape(false))
                         ->castTo<FunctionType>();

  auto *closure = new (Context) AutoClosureExpr(
      expr, newClosureType, AutoClosureExpr::InvalidDiscriminator, DC);

  closure->setParameterList(ParameterList::createEmpty(Context));

  if (isAsyncLetWrapper)
    closure->setThunkKind(AutoClosureExpr::Kind::AsyncLet);

  Expr *result = closure;

  if (!newClosureType->isEqual(closureType)) {
    assert(isInDefaultArgumentContext);
    assert(newClosureType
               ->withExtInfo(newClosureType->getExtInfo().withNoEscape(true))
               ->isEqual(closureType));
    result = new (Context) FunctionConversionExpr(closure, closureType);
  }

  cacheExprTypes(result);
  return result;
}

Expr *ConstraintSystem::buildTypeErasedExpr(Expr *expr, DeclContext *dc,
                                            Type contextualType,
                                            ContextualTypePurpose purpose) {
  if (!(purpose == CTP_ReturnStmt || purpose == CTP_ReturnSingleExpr))
    return expr;

  auto *decl = dyn_cast_or_null<ValueDecl>(dc->getAsDecl());
  if (!decl ||
      !(decl->isDynamic() || decl->getDynamicallyReplacedDecl()))
    return expr;

  auto *opaque = contextualType->getAs<OpaqueTypeArchetypeType>();
  if (!opaque)
    return expr;

  auto protocols = opaque->getConformsTo();
  if (protocols.size() != 1)
    return expr;

  auto *PD = protocols.front();
  auto *attr = PD->getAttrs().getAttribute<TypeEraserAttr>();
  if (!attr)
    return expr;

  auto typeEraser = attr->getResolvedType(PD);
  assert(typeEraser && "Failed to resolve eraser type!");
  auto &ctx = dc->getASTContext();
  return CallExpr::createImplicit(ctx,
                                  TypeExpr::createImplicit(typeEraser, ctx),
                                  {expr}, {ctx.Id_erasing});
}

/// If an UnresolvedDotExpr, SubscriptMember, etc has been resolved by the
/// constraint system, return the decl that it references.
ValueDecl *ConstraintSystem::findResolvedMemberRef(ConstraintLocator *locator) {
  // See if we have a resolution for this member.
  auto overload = findSelectedOverloadFor(locator);
  if (!overload)
    return nullptr;

  // We only want to handle the simplest decl binding.
  auto choice = overload->choice;
  if (choice.getKind() != OverloadChoiceKind::Decl)
    return nullptr;

  return choice.getDecl();
}

SolutionApplicationTarget::SolutionApplicationTarget(
    Expr *expr, DeclContext *dc, ContextualTypePurpose contextualPurpose,
    TypeLoc convertType, bool isDiscarded) {
  // Verify that a purpose was specified if a convertType was.  Note that it is
  // ok to have a purpose without a convertType (which is used for call
  // return types).
  assert((!convertType.getType() || contextualPurpose != CTP_Unused) &&
         "Purpose for conversion type was not specified");

  // Take a look at the conversion type to check to make sure it is sensible.
  if (auto type = convertType.getType()) {
    // If we're asked to convert to an UnresolvedType, then ignore the request.
    // This happens when CSDiags nukes a type.
    if (type->is<UnresolvedType>() ||
        (type->is<MetatypeType>() && type->hasUnresolvedType())) {
      convertType = TypeLoc();
      contextualPurpose = CTP_Unused;
    }
  }

  kind = Kind::expression;
  expression.expression = expr;
  expression.dc = dc;
  expression.contextualPurpose = contextualPurpose;
  expression.convertType = convertType;
  expression.pattern = nullptr;
  expression.propertyWrapper.wrappedVar = nullptr;
  expression.propertyWrapper.innermostWrappedValueInit = nullptr;
  expression.propertyWrapper.hasInitialWrappedValue = false;
  expression.isDiscarded = isDiscarded;
  expression.bindPatternVarsOneWay = false;
  expression.initialization.patternBinding = nullptr;
  expression.initialization.patternBindingIndex = 0;
}

void SolutionApplicationTarget::maybeApplyPropertyWrapper() {
  assert(kind == Kind::expression);
  assert(expression.contextualPurpose == CTP_Initialization);
  auto singleVar = expression.pattern->getSingleVar();
  if (!singleVar)
    return;

  auto wrapperAttrs = singleVar->getAttachedPropertyWrappers();
  if (wrapperAttrs.empty())
    return;

  // If the outermost property wrapper is directly initialized, form the
  // call.
  auto &ctx = singleVar->getASTContext();
  auto outermostWrapperAttr = wrapperAttrs.front();
  Expr *backingInitializer;
  if (Expr *initializer = expression.expression) {
    if (!isa<PropertyWrapperValuePlaceholderExpr>(initializer)) {
      expression.propertyWrapper.hasInitialWrappedValue = true;
    }
    // Form init(wrappedValue:) call(s).
    Expr *wrappedInitializer = buildPropertyWrapperInitCall(
        singleVar, Type(), initializer, PropertyWrapperInitKind::WrappedValue,
        [&](ApplyExpr *innermostInit) {
          expression.propertyWrapper.innermostWrappedValueInit = innermostInit;
        });
    if (!wrappedInitializer)
      return;

    backingInitializer = wrappedInitializer;
  } else {
    Type outermostWrapperType =
        singleVar->getAttachedPropertyWrapperType(0);
    if (!outermostWrapperType)
      return;

    bool isImplicit = false;

    // Retrieve the outermost wrapper argument. If there isn't one, we're
    // performing default initialization.
    auto outermostArg = outermostWrapperAttr->getArg();
    if (!outermostArg) {
      SourceLoc fakeParenLoc = outermostWrapperAttr->getRange().End;
      outermostArg = TupleExpr::createEmpty(
          ctx, fakeParenLoc, fakeParenLoc, /*Implicit=*/true);
      isImplicit = true;
    }

    SourceLoc typeLoc;
    if (auto *repr = outermostWrapperAttr->getTypeRepr()) {
      typeLoc = repr->getLoc();
    }
    auto typeExpr =
        TypeExpr::createImplicitHack(typeLoc, outermostWrapperType, ctx);
    backingInitializer = CallExpr::create(
        ctx, typeExpr, outermostArg,
        outermostWrapperAttr->getArgumentLabels(),
        outermostWrapperAttr->getArgumentLabelLocs(),
        /*hasTrailingClosure=*/false,
        /*implicit=*/isImplicit);
  }
  wrapperAttrs[0]->setSemanticInit(backingInitializer);

  // Note that we have applied to property wrapper, so we can adjust
  // the initializer type later.
  expression.propertyWrapper.wrappedVar = singleVar;
  expression.expression = backingInitializer;
  expression.convertType = {outermostWrapperAttr->getTypeRepr(),
                            outermostWrapperAttr->getType()};
}

SolutionApplicationTarget SolutionApplicationTarget::forInitialization(
    Expr *initializer, DeclContext *dc, Type patternType, Pattern *pattern,
    bool bindPatternVarsOneWay) {
  // Determine the contextual type for the initialization.
  TypeLoc contextualType;
  if (!(isa<OptionalSomePattern>(pattern) && !pattern->isImplicit()) &&
      patternType && !patternType->is<UnresolvedType>()) {
    contextualType = TypeLoc::withoutLoc(patternType);

    // Only provide a TypeLoc if it makes sense to allow diagnostics.
    if (auto *typedPattern = dyn_cast<TypedPattern>(pattern)) {
      const Pattern *inner = typedPattern->getSemanticsProvidingPattern();
      if (isa<NamedPattern>(inner) || isa<AnyPattern>(inner)) {
        contextualType = TypeLoc(typedPattern->getTypeRepr());
        if (typedPattern->hasType())
          contextualType.setType(typedPattern->getType());
        else
          contextualType.setType(patternType);
      }
    }
  }

  SolutionApplicationTarget target(
      initializer, dc, CTP_Initialization, contextualType,
      /*isDiscarded=*/false);
  target.expression.pattern = pattern;
  target.expression.bindPatternVarsOneWay = bindPatternVarsOneWay;
  target.maybeApplyPropertyWrapper();
  return target;
}

SolutionApplicationTarget SolutionApplicationTarget::forInitialization(
    Expr *initializer, DeclContext *dc, Type patternType,
    PatternBindingDecl *patternBinding, unsigned patternBindingIndex,
    bool bindPatternVarsOneWay) {
    auto result = forInitialization(
        initializer, dc, patternType,
        patternBinding->getPattern(patternBindingIndex), bindPatternVarsOneWay);
    result.expression.initialization.patternBinding = patternBinding;
    result.expression.initialization.patternBindingIndex = patternBindingIndex;
    return result;
}

SolutionApplicationTarget SolutionApplicationTarget::forForEachStmt(
    ForEachStmt *stmt, ProtocolDecl *sequenceProto, DeclContext *dc,
    bool bindPatternVarsOneWay) {
  SolutionApplicationTarget target(
      stmt->getSequence(), dc, CTP_ForEachStmt,
      sequenceProto->getDeclaredInterfaceType(), /*isDiscarded=*/false);
  target.expression.pattern = stmt->getPattern();
  target.expression.bindPatternVarsOneWay =
    bindPatternVarsOneWay || (stmt->getWhere() != nullptr);
  target.expression.forEachStmt.stmt = stmt;
  target.expression.forEachStmt.whereExpr = stmt->getWhere();
  return target;
}

SolutionApplicationTarget
SolutionApplicationTarget::forUninitializedWrappedVar(VarDecl *wrappedVar) {
  return SolutionApplicationTarget(wrappedVar);
}

ContextualPattern
SolutionApplicationTarget::getContextualPattern() const {
  assert(kind == Kind::expression);
  assert(expression.contextualPurpose == CTP_Initialization ||
         expression.contextualPurpose == CTP_ForEachStmt);
  if (expression.contextualPurpose == CTP_Initialization &&
      expression.initialization.patternBinding) {
    return ContextualPattern::forPatternBindingDecl(
        expression.initialization.patternBinding,
        expression.initialization.patternBindingIndex);
  }

  return ContextualPattern::forRawPattern(expression.pattern, expression.dc);
}

bool SolutionApplicationTarget::infersOpaqueReturnType() const {
  assert(kind == Kind::expression);
  switch (expression.contextualPurpose) {
  case CTP_Initialization:
    if (Type convertType = expression.convertType.getType()) {
      return convertType->is<OpaqueTypeArchetypeType>();
    }
    return false;

  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
    if (Type convertType = expression.convertType.getType()) {
      if (auto opaqueType = convertType->getAs<OpaqueTypeArchetypeType>()) {
        auto dc = getDeclContext();
        if (auto func = dyn_cast<AbstractFunctionDecl>(dc)) {
          return opaqueType->getDecl()->isOpaqueReturnTypeOfFunction(func);
        }
      }
    }
    return false;

  default:
    return false;
  }
}

bool SolutionApplicationTarget::contextualTypeIsOnlyAHint() const {
  assert(kind == Kind::expression);
  switch (expression.contextualPurpose) {
  case CTP_Initialization:
    return !infersOpaqueReturnType() && !isOptionalSomePatternInit();
  case CTP_ForEachStmt:
    return true;
  case CTP_Unused:
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
  case CTP_YieldByValue:
  case CTP_YieldByReference:
  case CTP_ThrowStmt:
  case CTP_EnumCaseRawValue:
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
  case CTP_CalleeResult:
  case CTP_CallArgument:
  case CTP_ClosureResult:
  case CTP_ArrayElement:
  case CTP_DictionaryKey:
  case CTP_DictionaryValue:
  case CTP_CoerceOperand:
  case CTP_AssignSource:
  case CTP_SubscriptAssignSource:
  case CTP_Condition:
  case CTP_WrappedProperty:
  case CTP_ComposedPropertyWrapper:
  case CTP_CannotFail:
    return false;
  }
  llvm_unreachable("invalid contextual type");
}

/// Given a specific expression and the remnants of the failed constraint
/// system, produce a specific diagnostic.
///
/// This is guaranteed to always emit an error message.
///
void ConstraintSystem::diagnoseFailureFor(SolutionApplicationTarget target) {
  setPhase(ConstraintSystemPhase::Diagnostics);

  SWIFT_DEFER { setPhase(ConstraintSystemPhase::Finalization); };

  auto &DE = getASTContext().Diags;
  if (auto expr = target.getAsExpr()) {
    if (auto *assignment = dyn_cast<AssignExpr>(expr)) {
      if (isa<DiscardAssignmentExpr>(assignment->getDest()))
        expr = assignment->getSrc();
    }

    // Look through RebindSelfInConstructorExpr to avoid weird Sema issues.
    if (auto *RB = dyn_cast<RebindSelfInConstructorExpr>(expr))
      expr = RB->getSubExpr();

    // Unresolved/Anonymous ClosureExprs are common enough that we should give
    // them tailored diagnostics.
    if (auto *closure = dyn_cast<ClosureExpr>(expr->getValueProvidingExpr())) {
      DE.diagnose(closure->getLoc(), diag::cannot_infer_closure_type)
        .highlight(closure->getSourceRange());
      return;
    }

    // If no one could find a problem with this expression or constraint system,
    // then it must be well-formed... but is ambiguous.  Handle this by
    // diagnostic various cases that come up.
    DE.diagnose(expr->getLoc(), diag::type_of_expression_is_ambiguous)
        .highlight(expr->getSourceRange());
  } else if (auto *wrappedVar = target.getAsUninitializedWrappedVar()) {
    auto *wrapper = wrappedVar->getAttachedPropertyWrappers().back();
    Type propertyType = wrappedVar->getInterfaceType();
    Type wrapperType = wrapper->getType();

    // Emit the property wrapper fallback diagnostic
    wrappedVar->diagnose(diag::property_wrapper_incompatible_property,
                         propertyType, wrapperType);
    if (auto nominal = wrapperType->getAnyNominal()) {
      nominal->diagnose(diag::property_wrapper_declared_here,
                        nominal->getName());
    }
  } else {
    // Emit a poor fallback message.
    DE.diagnose(target.getAsFunction()->getLoc(),
                diag::failed_to_produce_diagnostic);
  }
}

bool ConstraintSystem::isDeclUnavailable(const Decl *D,
                                         ConstraintLocator *locator) const {
  // First check whether this declaration is universally unavailable.
  if (D->getAttrs().isUnavailable(getASTContext()))
    return true;

  return TypeChecker::isDeclarationUnavailable(D, DC, [&] {
    SourceLoc loc;

    if (locator) {
      if (auto anchor = locator->getAnchor())
        loc = getLoc(anchor);
    }

    return TypeChecker::overApproximateAvailabilityAtLocation(loc, DC);
  });
}

bool ConstraintSystem::isConformanceUnavailable(ProtocolConformanceRef conformance,
                                                ConstraintLocator *locator) const {
  if (!conformance.isConcrete())
    return false;

  auto *concrete = conformance.getConcrete();
  auto *rootConf = concrete->getRootConformance();
  auto *ext = dyn_cast<ExtensionDecl>(rootConf->getDeclContext());
  if (ext == nullptr)
    return false;

  return isDeclUnavailable(ext, locator);
}

/// If we aren't certain that we've emitted a diagnostic, emit a fallback
/// diagnostic.
void ConstraintSystem::maybeProduceFallbackDiagnostic(
    SolutionApplicationTarget target) const {
  if (Options.contains(ConstraintSystemFlags::SuppressDiagnostics))
    return;

  // Before producing fatal error here, let's check if there are any "error"
  // diagnostics already emitted or waiting to be emitted. Because they are
  // a better indication of the problem.
  ASTContext &ctx = getASTContext();
  if (ctx.Diags.hadAnyError() || ctx.hasDelayedConformanceErrors())
    return;

  ctx.Diags.diagnose(target.getLoc(), diag::failed_to_produce_diagnostic);
}

SourceLoc constraints::getLoc(ASTNode anchor) {
  if (auto *E = anchor.dyn_cast<Expr *>()) {
    return E->getLoc();
  } else if (auto *T = anchor.dyn_cast<TypeRepr *>()) {
    return T->getLoc();
  } else if (auto *V = anchor.dyn_cast<Decl *>()) {
    if (auto VD = dyn_cast<VarDecl>(V))
      return VD->getNameLoc();
    return anchor.getStartLoc();
  } else if (auto *S = anchor.dyn_cast<Stmt *>()) {
    return S->getStartLoc();
  } else {
    return anchor.get<Pattern *>()->getLoc();
  }
}

SourceRange constraints::getSourceRange(ASTNode anchor) {
  return anchor.getSourceRange();
}

static Optional<Requirement> getRequirement(ConstraintSystem &cs,
                                            ConstraintLocator *reqLocator) {
  auto reqLoc = reqLocator->getLastElementAs<LocatorPathElt::AnyRequirement>();
  if (!reqLoc)
    return None;

  if (reqLoc->isConditionalRequirement()) {
    auto conformanceRef =
        reqLocator->findLast<LocatorPathElt::ConformanceRequirement>();
    assert(conformanceRef && "Invalid locator for a conditional requirement");

    auto conformance = conformanceRef->getConformance();
    return conformance->getConditionalRequirements()[reqLoc->getIndex()];
  }

  if (auto openedGeneric =
          reqLocator->findLast<LocatorPathElt::OpenedGeneric>()) {
    auto signature = openedGeneric->getSignature();
    return signature->getRequirements()[reqLoc->getIndex()];
  }

  return None;
}

static Optional<std::pair<GenericTypeParamType *, RequirementKind>>
getRequirementInfo(ConstraintSystem &cs, ConstraintLocator *reqLocator) {
  auto requirement = getRequirement(cs, reqLocator);
  if (!requirement)
    return None;

  auto *GP = requirement->getFirstType()->getAs<GenericTypeParamType>();
  if (!GP)
    return None;

  auto path = reqLocator->getPath();
  auto iter = path.rbegin();
  auto openedGeneric =
      reqLocator->findLast<LocatorPathElt::OpenedGeneric>(iter);
  assert(openedGeneric);

  auto newPath = path.drop_back(iter - path.rbegin() + 1);
  auto *baseLoc = cs.getConstraintLocator(reqLocator->getAnchor(), newPath);

  auto openedTypes = cs.getOpenedTypes();
  auto substitutions = llvm::find_if(
      openedTypes,
      [&baseLoc](
          const std::pair<ConstraintLocator *, ArrayRef<OpenedType>> &entry) {
        return entry.first == baseLoc;
      });

  if (substitutions == openedTypes.end())
    return None;

  auto replacement =
      llvm::find_if(substitutions->second, [&GP](const OpenedType &entry) {
        auto *typeVar = entry.second;
        return typeVar->getImpl().getGenericParameter() == GP;
      });

  if (replacement == substitutions->second.end())
    return None;

  auto *repr = cs.getRepresentative(replacement->second);
  return std::make_pair(repr->getImpl().getGenericParameter(),
                        requirement->getKind());
}

bool ConstraintSystem::isFixedRequirement(ConstraintLocator *reqLocator,
                                          Type requirementTy) {
  if (auto reqInfo = getRequirementInfo(*this, reqLocator)) {
    auto *GP = reqInfo->first;
    auto reqKind = static_cast<unsigned>(reqInfo->second);
    return FixedRequirements.count(
        std::make_tuple(GP, reqKind, requirementTy.getPointer()));
  }

  return false;
}

void ConstraintSystem::recordFixedRequirement(ConstraintLocator *reqLocator,
                                              Type requirementTy) {
  if (auto reqInfo = getRequirementInfo(*this, reqLocator)) {
    auto *GP = reqInfo->first;
    auto reqKind = static_cast<unsigned>(reqInfo->second);
    FixedRequirements.insert(
        std::make_tuple(GP, reqKind, requirementTy.getPointer()));
  }
}

// Replace any error types encountered with placeholders.
Type ConstraintSystem::getVarType(const VarDecl *var) {
  auto type = var->getType();

  // If this declaration is used as part of a code completion
  // expression, solver needs to glance over the fact that
  // it might be invalid to avoid failing constraint generation
  // and produce completion results.
  if (!isForCodeCompletion())
    return type;

  return type.transform([&](Type type) {
    if (!type->is<ErrorType>())
      return type;
    return PlaceholderType::get(Context, const_cast<VarDecl *>(var));
  });
}

bool ConstraintSystem::isReadOnlyKeyPathComponent(
    const AbstractStorageDecl *storage, SourceLoc referenceLoc) {
  // See whether key paths can store to this component. (Key paths don't
  // get any special power from being formed in certain contexts, such
  // as the ability to assign to `let`s in initialization contexts, so
  // we pass null for the DC to `isSettable` here.)
  if (!getASTContext().isSwiftVersionAtLeast(5)) {
    // As a source-compatibility measure, continue to allow
    // WritableKeyPaths to be formed in the same conditions we did
    // in previous releases even if we should not be able to set
    // the value in this context.
    if (!storage->isSettable(DC)) {
      // A non-settable component makes the key path read-only, unless
      // a reference-writable component shows up later.
      return true;
    }
  } else if (!storage->isSettable(nullptr) ||
             !storage->isSetterAccessibleFrom(DC)) {
    // A non-settable component makes the key path read-only, unless
    // a reference-writable component shows up later.
    return true;
  }

  // If the setter is unavailable, then the keypath ought to be read-only
  // in this context.
  if (auto setter = storage->getOpaqueAccessor(AccessorKind::Set)) {
    ExportContext where = ExportContext::forFunctionBody(DC, referenceLoc);
    auto maybeUnavail =
        TypeChecker::checkDeclarationAvailability(setter, where);
    if (maybeUnavail.hasValue()) {
      return true;
    }
  }

  return false;
}

TypeVarBindingProducer::TypeVarBindingProducer(BindingSet &bindings)
    : BindingProducer(bindings.getConstraintSystem(),
                      bindings.getTypeVariable()->getImpl().getLocator()),
      TypeVar(bindings.getTypeVariable()), CanBeNil(bindings.canBeNil()) {
  if (bindings.isDirectHole()) {
    auto *locator = getLocator();
    // If this type variable is associated with a code completion token
    // and it failed to infer any bindings let's adjust holes's locator
    // to point to a code completion token to avoid attempting to "fix"
    // this problem since its rooted in the fact that constraint system
    // is under-constrained.
    if (bindings.getAssociatedCodeCompletionToken()) {
      locator =
          CS.getConstraintLocator(bindings.getAssociatedCodeCompletionToken());
    }

    Bindings.push_back(Binding::forHole(TypeVar, locator));
    return;
  }

  // A binding to `Any` which should always be considered as a last resort.
  Optional<Binding> Any;

  auto addBinding = [&](const Binding &binding) {
    // Adjust optionality of existing bindings based on presence of
    // `ExpressibleByNilLiteral` requirement.
    if (requiresOptionalAdjustment(binding)) {
      Bindings.push_back(
          binding.withType(OptionalType::get(binding.BindingType)));
    } else if (binding.BindingType->isAny()) {
      Any.emplace(binding);
    } else {
      Bindings.push_back(binding);
    }
  };

  for (const auto &binding : bindings.Bindings) {
    addBinding(binding);
  }

  // Infer defaults based on "uncovered" literal protocol requirements.
  for (const auto &info : bindings.Literals) {
    const auto &literal = info.second;

    if (!literal.viableAsBinding())
      continue;

    // We need to figure out whether this is a direct conformance
    // requirement or inferred transitive one to identify binding
    // kind correctly.
    addBinding({literal.getDefaultType(),
                literal.isDirectRequirement() ? BindingKind::Subtypes
                                              : BindingKind::Supertypes,
                literal.getSource()});
  }

  // Let's always consider `Any` to be a last resort binding because
  // it's always better to infer concrete type and erase it if required
  // by the context.
  if (Any) {
    Bindings.push_back(*Any);
  }

  {
    bool noBindings = Bindings.empty();

    for (const auto &entry : bindings.Defaults) {
      auto *constraint = entry.second;
      if (noBindings) {
        // If there are no direct or transitive bindings to attempt
        // let's add defaults to the list right away.
        Bindings.push_back(getDefaultBinding(constraint));
      } else {
        // Otherwise let's delay attempting default bindings
        // until all of the direct & transitive bindings and
        // their derivatives have been attempted.
        DelayedDefaults.push_back(constraint);
      }
    }
  }
}

bool TypeVarBindingProducer::requiresOptionalAdjustment(
    const Binding &binding) const {
  // If type variable can't be `nil` then adjustment is
  // not required.
  if (!CanBeNil)
    return false;

  if (binding.Kind == BindingKind::Supertypes) {
    auto type = binding.BindingType->getRValueType();
    // If the type doesn't conform to ExpressibleByNilLiteral,
    // produce an optional of that type as a potential binding. We
    // overwrite the binding in place because the non-optional type
    // will fail to type-check against the nil-literal conformance.
    bool conformsToExprByNilLiteral = false;
    if (auto *nominalBindingDecl = type->getAnyNominal()) {
      SmallVector<ProtocolConformance *, 2> conformances;
      conformsToExprByNilLiteral = nominalBindingDecl->lookupConformance(
          CS.DC->getParentModule(),
          CS.getASTContext().getProtocol(
              KnownProtocolKind::ExpressibleByNilLiteral),
          conformances);
    }
    return !conformsToExprByNilLiteral;
  } else if (binding.isDefaultableBinding() && binding.BindingType->isAny()) {
    return true;
  }

  return false;
}

PotentialBinding
TypeVarBindingProducer::getDefaultBinding(Constraint *constraint) const {
  assert(constraint->getKind() == ConstraintKind::Defaultable ||
         constraint->getKind() == ConstraintKind::DefaultClosureType);

  auto type = constraint->getSecondType();
  Binding binding{type, BindingKind::Exact, constraint};
  return requiresOptionalAdjustment(binding)
             ? binding.withType(OptionalType::get(type))
             : binding;
}

ValueDecl *constraints::getOverloadChoiceDecl(Constraint *choice) {
  if (choice->getKind() != ConstraintKind::BindOverload)
    return nullptr;
  return choice->getOverloadChoice().getDeclOrNull();
}

bool constraints::isOperatorDisjunction(Constraint *disjunction) {
  assert(disjunction->getKind() == ConstraintKind::Disjunction);

  auto choices = disjunction->getNestedConstraints();
  assert(!choices.empty());

  auto *decl = getOverloadChoiceDecl(choices.front());
  return decl ? decl->isOperator() : false;
}

ASTNode constraints::findAsyncNode(ClosureExpr *closure) {
  auto *body = closure->getBody();
  if (!body)
    return ASTNode();

  // A walker that looks for 'async' and 'await' expressions
  // that aren't nested within closures or nested declarations.
  class FindInnerAsync : public ASTWalker {
    ASTNode AsyncNode;

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // If we've found an 'await', record it and terminate the traversal.
      if (isa<AwaitExpr>(expr)) {
        AsyncNode = expr;
        return {false, nullptr};
      }

      // Do not recurse into other closures.
      if (isa<ClosureExpr>(expr))
        return {false, expr};

      return {true, expr};
    }

    bool walkToDeclPre(Decl *decl) override {
      // Do not walk into function or type declarations.
      if (auto *patternBinding = dyn_cast<PatternBindingDecl>(decl)) {
        if (patternBinding->isAsyncLet())
          AsyncNode = patternBinding;

        return true;
      }

      return false;
    }

    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      if (auto forEach = dyn_cast<ForEachStmt>(stmt)) {
        if (forEach->getAwaitLoc().isValid()) {
          AsyncNode = forEach;
          return {false, nullptr};
        }
      }

      return {true, stmt};
    }

  public:
    ASTNode getAsyncNode() { return AsyncNode; }
  };

  FindInnerAsync asyncFinder;
  body->walk(asyncFinder);

  return asyncFinder.getAsyncNode();
}
