//===--- CSApply.cpp - Constraint Application -----------------------------===//
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
// This file implements application of a solution to a constraint
// system to a particular expression, resulting in a
// fully-type-checked expression.
//
//===----------------------------------------------------------------------===//

#include "CSDiagnostics.h"
#include "CodeSynthesis.h"
#include "MiscDiagnostics.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckProtocol.h"
#include "TypeCheckType.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/Effects.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/OperatorNameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/SolutionResult.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace constraints;

/// Retrieve the fixed type for the given type variable.
Type Solution::getFixedType(TypeVariableType *typeVar) const {
  auto knownBinding = typeBindings.find(typeVar);
  assert(knownBinding != typeBindings.end());
  return knownBinding->second;
}

/// Determine whether the given type is an opened AnyObject.
///
/// This comes up in computeSubstitutions() when accessing
/// members via dynamic lookup.
static bool isOpenedAnyObject(Type type) {
  auto archetype = type->getAs<OpenedArchetypeType>();
  if (!archetype)
    return false;

  return archetype->getOpenedExistentialType()->isAnyObject();
}

SubstitutionMap
Solution::computeSubstitutions(GenericSignature sig,
                               ConstraintLocator *locator) const {
  if (sig.isNull())
    return SubstitutionMap();

  // Gather the substitutions from dependent types to concrete types.
  auto openedTypes = OpenedTypes.find(locator);

  // If we have a member reference on an existential, there are no
  // opened types or substitutions.
  if (openedTypes == OpenedTypes.end())
    return SubstitutionMap();

  TypeSubstitutionMap subs;
  for (const auto &opened : openedTypes->second)
    subs[opened.first] = getFixedType(opened.second);

  auto lookupConformanceFn =
      [&](CanType original, Type replacement,
          ProtocolDecl *protoType) -> ProtocolConformanceRef {
    if (replacement->hasError() ||
        isOpenedAnyObject(replacement) ||
        replacement->is<GenericTypeParamType>()) {
      return ProtocolConformanceRef(protoType);
    }

    // FIXME: Retrieve the conformance from the solution itself.
    return TypeChecker::conformsToProtocol(replacement, protoType,
                                   getConstraintSystem().DC->getParentModule());
  };

  return SubstitutionMap::get(sig,
                              QueryTypeSubstitutionMap{subs},
                              lookupConformanceFn);
}

static ConcreteDeclRef generateDeclRefForSpecializedCXXFunctionTemplate(
    ASTContext &ctx, AbstractFunctionDecl *oldDecl, SubstitutionMap subst,
    clang::FunctionDecl *specialized) {
  // Create a new ParameterList with the substituted type.
  auto oldFnType =
      cast<GenericFunctionType>(oldDecl->getInterfaceType().getPointer());
  auto newFnType = oldFnType->substGenericArgs(subst);
  // The constructor type is a function type as follows:
  //   (CType.Type) -> (Generic) -> CType
  // And a method's function type is as follows:
  //   (inout CType) -> (Generic) -> Void
  // In either case, we only want the result of that function type because that
  // is the function type with the generic params that need to be substituted:
  //   (Generic) -> CType
  if (isa<ConstructorDecl>(oldDecl) || oldDecl->isInstanceMember() ||
      oldDecl->isStatic())
    newFnType = cast<FunctionType>(newFnType->getResult().getPointer());
  SmallVector<ParamDecl *, 4> newParams;
  unsigned i = 0;
  for (auto paramTy : newFnType->getParams()) {
    auto *oldParamDecl = oldDecl->getParameters()->get(i);
    auto *newParamDecl =
        ParamDecl::cloneWithoutType(oldDecl->getASTContext(), oldParamDecl);
    newParamDecl->setInterfaceType(paramTy.getParameterType());
    newParams.push_back(newParamDecl);
    (void)++i;
  }
  auto *newParamList =
      ParameterList::create(ctx, SourceLoc(), newParams, SourceLoc());

  if (isa<ConstructorDecl>(oldDecl)) {
    DeclName ctorName(ctx, DeclBaseName::createConstructor(), newParamList);
    auto newCtorDecl = ConstructorDecl::createImported(
        ctx, specialized, ctorName, oldDecl->getLoc(), 
        /*failable=*/false, /*failabilityLoc=*/SourceLoc(),
        /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
        /*throws=*/false, /*throwsLoc=*/SourceLoc(), 
        newParamList, /*genericParams=*/nullptr,
        oldDecl->getDeclContext());
    return ConcreteDeclRef(newCtorDecl);
  }

  // Generate a name for the specialized function.
  std::string newNameStr;
  llvm::raw_string_ostream buffer(newNameStr);
  clang::MangleContext *mangler =
      specialized->getASTContext().createMangleContext();
  mangler->mangleName(specialized, buffer);
  buffer.flush();
  // Add all parameters as empty parameters.
  auto newName = DeclName(
      ctx, DeclName(ctx.getIdentifier(newNameStr)).getBaseName(), newParamList);

  auto newFnDecl = FuncDecl::createImported(
      ctx, oldDecl->getLoc(), newName, oldDecl->getNameLoc(),
      /*Async=*/false, oldDecl->hasThrows(), newParamList,
      newFnType->getResult(), /*GenericParams=*/nullptr,
      oldDecl->getDeclContext(), specialized);
  if (oldDecl->isStatic()) {
    newFnDecl->setStatic();
    newFnDecl->setImportAsStaticMember();
  }
  newFnDecl->setSelfAccessKind(cast<FuncDecl>(oldDecl)->getSelfAccessKind());
  return ConcreteDeclRef(newFnDecl);
}

ConcreteDeclRef
Solution::resolveConcreteDeclRef(ValueDecl *decl,
                                 ConstraintLocator *locator) const {
  if (!decl)
    return ConcreteDeclRef();

  // Get the generic signatue of the decl and compute the substitutions.
  auto sig = decl->getInnermostDeclContext()->getGenericSignatureOfContext();
  auto subst = computeSubstitutions(sig, locator);

  // Lazily instantiate function definitions for class template specializations.
  // Members of a class template specialization will be instantiated here (not
  // when imported). If this method has already be instantiated, then this is a
  // no-op.
  if (const auto *constMethod =
          dyn_cast_or_null<clang::CXXMethodDecl>(decl->getClangDecl())) {
    auto method = const_cast<clang::CXXMethodDecl *>(constMethod);
    // Make sure that this method is part of a class template specialization.
    if (method->getTemplateInstantiationPattern())
      decl->getASTContext()
          .getClangModuleLoader()
          ->getClangSema()
          .InstantiateFunctionDefinition(method->getLocation(), method);
  }

  // If this is a C++ function template, get it's specialization for the given
  // substitution map and update the decl accordingly.
  if (decl->getClangDecl() &&
      isa<clang::FunctionTemplateDecl>(decl->getClangDecl())) {
    auto *newFn =
        decl->getASTContext()
            .getClangModuleLoader()
            ->instantiateCXXFunctionTemplate(
                decl->getASTContext(),
                const_cast<clang::FunctionTemplateDecl *>(
                    cast<clang::FunctionTemplateDecl>(decl->getClangDecl())),
                subst);
    return generateDeclRefForSpecializedCXXFunctionTemplate(
        decl->getASTContext(), cast<AbstractFunctionDecl>(decl), subst, newFn);
  }

  return ConcreteDeclRef(decl, subst);
}

ConstraintLocator *Solution::getCalleeLocator(ConstraintLocator *locator,
                                              bool lookThroughApply) const {
  auto &cs = getConstraintSystem();
  return cs.getCalleeLocator(
      locator, lookThroughApply,
      [&](Expr *expr) -> Type { return getType(expr); },
      [&](Type type) -> Type { return simplifyType(type)->getRValueType(); },
      [&](ConstraintLocator *locator) -> Optional<SelectedOverload> {
        return getOverloadChoiceIfAvailable(locator);
      });
}

ConstraintLocator *
Solution::getConstraintLocator(ASTNode anchor,
                               ArrayRef<LocatorPathElt> path) const {
  auto &cs = getConstraintSystem();
  return cs.getConstraintLocator(anchor, path);
}

ConstraintLocator *
Solution::getConstraintLocator(ConstraintLocator *base,
                               ArrayRef<LocatorPathElt> path) const {
  auto &cs = getConstraintSystem();
  return cs.getConstraintLocator(base, path);
}

/// Return the implicit access kind for a MemberRefExpr with the
/// specified base and member in the specified DeclContext.
static AccessSemantics
getImplicitMemberReferenceAccessSemantics(Expr *base, VarDecl *member,
                                          DeclContext *DC) {
  // Check whether this is a member access on 'self'.
  bool isAccessOnSelf = false;
  if (auto *baseDRE = dyn_cast<DeclRefExpr>(base->getValueProvidingExpr()))
    if (auto *baseVar = dyn_cast<VarDecl>(baseDRE->getDecl()))
      isAccessOnSelf = baseVar->isSelfParameter();

  // If the value is always directly accessed from this context, do it.
  return member->getAccessSemanticsFromContext(DC, isAccessOnSelf);
}

/// This extends functionality of `Expr::isTypeReference` with
/// support for `UnresolvedDotExpr` and `UnresolvedMemberExpr`.
/// This method could be used on not yet fully type-checked AST.
bool ConstraintSystem::isTypeReference(Expr *E) {
  return E->isTypeReference(
      [&](Expr *E) -> Type { return simplifyType(getType(E)); },
      [&](Expr *E) -> Decl * {
        if (auto *UDE = dyn_cast<UnresolvedDotExpr>(E)) {
          return findResolvedMemberRef(
              getConstraintLocator(UDE, ConstraintLocator::Member));
        }

        if (auto *UME = dyn_cast<UnresolvedMemberExpr>(E)) {
          return findResolvedMemberRef(
              getConstraintLocator(UME, ConstraintLocator::UnresolvedMember));
        }

        if (isa<OverloadSetRefExpr>(E))
          return findResolvedMemberRef(
              getConstraintLocator(const_cast<Expr *>(E)));

        return nullptr;
      });
}

bool Solution::isTypeReference(Expr *E) const {
  return E->isTypeReference(
      [&](Expr *expr) -> Type { return simplifyType(getType(expr)); },
      [&](Expr *expr) -> Decl * {
        ConstraintLocator *locator = nullptr;
        if (auto *UDE = dyn_cast<UnresolvedDotExpr>(E)) {
          locator = getConstraintLocator(UDE, {ConstraintLocator::Member});
        }

        if (auto *UME = dyn_cast<UnresolvedMemberExpr>(E)) {
          locator =
              getConstraintLocator(UME, {ConstraintLocator::UnresolvedMember});
        }

        if (isa<OverloadSetRefExpr>(E))
          locator = getConstraintLocator(const_cast<Expr *>(E));

        if (locator) {
          if (auto selectedOverload = getOverloadChoiceIfAvailable(locator)) {
            const auto &choice = selectedOverload->choice;
            return choice.getDeclOrNull();
          }
        }

        return nullptr;
      });
}

bool ConstraintSystem::isStaticallyDerivedMetatype(Expr *E) {
  return E->isStaticallyDerivedMetatype(
      [&](Expr *E) -> Type { return simplifyType(getType(E)); },
      [&](Expr *E) -> bool { return isTypeReference(E); });
}

bool Solution::isStaticallyDerivedMetatype(Expr *E) const {
  return E->isStaticallyDerivedMetatype(
      [&](Expr *E) -> Type { return simplifyType(getType(E)); },
      [&](Expr *E) -> bool { return isTypeReference(E); });
}

Type ConstraintSystem::getInstanceType(TypeExpr *E) {
  if (!hasType(E))
    return Type();

  if (auto metaType = getType(E)->getAs<MetatypeType>())
    return metaType->getInstanceType();

  return ErrorType::get(getType(E)->getASTContext());
}

Type ConstraintSystem::getResultType(const AbstractClosureExpr *E) {
  return E->getResultType([&](Expr *E) -> Type { return getType(E); });
}

static bool buildObjCKeyPathString(KeyPathExpr *E,
                                   llvm::SmallVectorImpl<char> &buf) {
  for (auto &component : E->getComponents()) {
    switch (component.getKind()) {
    case KeyPathExpr::Component::Kind::OptionalChain:
    case KeyPathExpr::Component::Kind::OptionalForce:
    case KeyPathExpr::Component::Kind::OptionalWrap:
      // KVC propagates nulls, so these don't affect the key path string.
      continue;
    case KeyPathExpr::Component::Kind::Identity:
      // The identity component can be elided from the KVC string (unless it's
      // the only component, in which case we use @"self").
      continue;

    case KeyPathExpr::Component::Kind::Property: {
      // Property references must be to @objc properties.
      // TODO: If we added special properties matching KVC operators like '@sum',
      // '@count', etc. those could be mapped too.
      auto property = cast<VarDecl>(component.getDeclRef().getDecl());
      if (!property->isObjC())
        return false;
      if (!buf.empty()) {
        buf.push_back('.');
      }
      auto objcName = property->getObjCPropertyName().str();
      buf.append(objcName.begin(), objcName.end());
      continue;
    }
    case KeyPathExpr::Component::Kind::TupleElement:
    case KeyPathExpr::Component::Kind::Subscript:
      // Subscripts and tuples aren't generally represented in KVC.
      // TODO: There are some subscript forms we could map to KVC, such as
      // when indexing a Dictionary or NSDictionary by string, or when applying
      // a mapping subscript operation to Array/Set or NSArray/NSSet.
      return false;
    case KeyPathExpr::Component::Kind::Invalid:
    case KeyPathExpr::Component::Kind::UnresolvedProperty:
    case KeyPathExpr::Component::Kind::UnresolvedSubscript:
      // Don't bother building the key path string if the key path didn't even
      // resolve.
      return false;
    case KeyPathExpr::Component::Kind::DictionaryKey:
      llvm_unreachable("DictionaryKey only valid in #keyPath expressions.");
      return false;
    }
  }
  
  // If there are no non-identity components, this is the "self" key.
  if (buf.empty()) {
    auto self = StringRef("self");
    buf.append(self.begin(), self.end());
  }
  
  return true;
}

namespace {

  /// Rewrites an expression by applying the solution of a constraint
  /// system to that expression.
  class ExprRewriter : public ExprVisitor<ExprRewriter, Expr *> {
  public:
    ConstraintSystem &cs;
    DeclContext *dc;
    Solution &solution;
    Optional<SolutionApplicationTarget> target;
    bool SuppressDiagnostics;

    /// Coerce the given tuple to another tuple type.
    ///
    /// \param expr The expression we're converting.
    ///
    /// \param fromTuple The tuple type we're converting from, which is the same
    /// as \c expr->getType().
    ///
    /// \param toTuple The tuple type we're converting to.
    ///
    /// \param locator Locator describing where this tuple conversion occurs.
    ///
    /// \param sources The sources of each of the elements to be used in the
    /// resulting tuple, as provided by \c computeTupleShuffle.
    Expr *coerceTupleToTuple(Expr *expr, TupleType *fromTuple,
                             TupleType *toTuple,
                             ConstraintLocatorBuilder locator,
                             ArrayRef<unsigned> sources);

    /// Coerce a subclass, class-constrained archetype, class-constrained
    /// existential or to a superclass type.
    ///
    /// Also supports metatypes of the above.
    ///
    /// \param expr The expression to be coerced.
    /// \param toType The type to which the expression will be coerced.
    ///
    /// \return The coerced expression, whose type will be equivalent to
    /// \c toType.
    Expr *coerceSuperclass(Expr *expr, Type toType);

    /// Coerce the given value to existential type.
    ///
    /// The following conversions are supported:
    /// - concrete to existential
    /// - existential to existential
    /// - concrete metatype to existential metatype
    /// - existential metatype to existential metatype
    ///
    /// \param expr The expression to be coerced.
    /// \param toType The type to which the expression will be coerced.
    ///
    /// \return The coerced expression, whose type will be equivalent to
    /// \c toType.
    Expr *coerceExistential(Expr *expr, Type toType);

    /// Coerce an expression of (possibly unchecked) optional
    /// type to have a different (possibly unchecked) optional type.
    Expr *coerceOptionalToOptional(Expr *expr, Type toType,
                                   ConstraintLocatorBuilder locator,
                                   Optional<Pattern*> typeFromPattern = None);

    /// Coerce an expression of implicitly unwrapped optional type to its
    /// underlying value type, in the correct way for an implicit
    /// look-through.
    Expr *coerceImplicitlyUnwrappedOptionalToValue(Expr *expr, Type objTy);

    /// Peephole an array upcast.
    void peepholeArrayUpcast(ArrayExpr *expr, Type toType, bool bridged,
                             Type elementType,
                             ConstraintLocatorBuilder locator);

    /// Peephole a dictionary upcast.
    void peepholeDictionaryUpcast(DictionaryExpr *expr, Type toType,
                                  bool bridged, Type keyType,
                                  Type valueType,
                                  ConstraintLocatorBuilder locator);

    /// Try to peephole the collection upcast, eliminating the need for
    /// a separate collection-upcast expression.
    ///
    /// \returns true if the peephole operation succeeded, in which case
    /// \c expr already subsumes the upcast.
    bool peepholeCollectionUpcast(Expr *expr, Type toType,  bool bridged,
                                  ConstraintLocatorBuilder locator);

    /// Build a collection upcast expression.
    ///
    /// \param bridged Whether this is a bridging conversion, meaning that the
    /// element types themselves are bridged (vs. simply coerced).
    Expr *buildCollectionUpcastExpr(Expr *expr, Type toType,
                                    bool bridged,
                                    ConstraintLocatorBuilder locator);

    /// Build the expression that performs a bridging operation from the
    /// given expression to the given \c toType.
    Expr *buildObjCBridgeExpr(Expr *expr, Type toType,
                              ConstraintLocatorBuilder locator);

    static Type getBaseType(AnyFunctionType *fnType,
                            bool wantsRValueInstanceType = true) {
      auto params = fnType->getParams();
      assert(params.size() == 1);

      const auto &base = params.front();
      if (wantsRValueInstanceType)
        return base.getPlainType()->getMetatypeInstanceType();

      return base.getOldType();
    }

    /// Check whether it is possible to have an ObjC key path string for the keypath expression
    /// and set the key path string, if yes
    void checkAndSetObjCKeyPathString(KeyPathExpr *keyPath) {
      if (cs.getASTContext().LangOpts.EnableObjCInterop) {
        SmallString<64> compatStringBuf;
        if (buildObjCKeyPathString(keyPath, compatStringBuf)) {
            auto stringCopy = cs.getASTContext().AllocateCopy<char>(compatStringBuf.begin(),
                                                                    compatStringBuf.end());
            auto stringExpr = new (cs.getASTContext()) StringLiteralExpr(
                                  StringRef(stringCopy, compatStringBuf.size()),
                                  SourceRange(),
                                  /*implicit*/ true);
            cs.setType(
                stringExpr,
                cs.getASTContext().getStringType());
            keyPath->setObjCStringLiteralExpr(stringExpr);
        }
      }
    }

    // Returns None if the AST does not contain enough information to recover
    // substitutions; this is different from an Optional(SubstitutionMap()),
    // indicating a valid call to a non-generic operator.
    Optional<SubstitutionMap>
    getOperatorSubstitutions(ValueDecl *witness, Type refType) {
      // We have to recover substitutions in this hacky way because
      // the AST does not retain enough information to devirtualize
      // calls like this.
      auto witnessType = witness->getInterfaceType();

      // Compute the substitutions.
      auto *gft = witnessType->getAs<GenericFunctionType>();
      if (gft == nullptr) {
        if (refType->isEqual(witnessType))
          return SubstitutionMap();
        return None;
      }

      auto sig = gft->getGenericSignature();
      auto *env = sig->getGenericEnvironment();

      witnessType = FunctionType::get(gft->getParams(),
                                      gft->getResult(),
                                      gft->getExtInfo());
      witnessType = env->mapTypeIntoContext(witnessType);

      TypeSubstitutionMap subs;
      auto substType = witnessType->substituteBindingsTo(
        refType,
        [&](ArchetypeType *origType, CanType substType,
            ArchetypeType*, ArrayRef<ProtocolConformanceRef>) -> CanType {
          if (auto gpType = dyn_cast<GenericTypeParamType>(
                origType->getInterfaceType()->getCanonicalType()))
            subs[gpType] = substType;

          return substType;
        });

      // If substitution failed, it means that the protocol requirement type
      // and the witness type did not match up. The only time that this
      // should happen is when the witness is defined in a base class and
      // the actual call uses a derived class. For example,
      //
      // protocol P { func +(lhs: Self, rhs: Self) }
      // class Base : P { func +(lhs: Base, rhs: Base) {} }
      // class Derived : Base {}
      //
      // If we enter this code path with two operands of type Derived,
      // we know we're calling the protocol requirement P.+, with a
      // substituted type of (Derived, Derived) -> (). But the type of
      // the witness is (Base, Base) -> (). Just bail out and make a
      // witness method call in this rare case; SIL mandatory optimizations
      // will likely devirtualize it anyway.
      if (!substType)
        return None;

      return SubstitutionMap::get(sig,
                                  QueryTypeSubstitutionMap{subs},
                                  LookUpConformanceInModule(cs.DC->getParentModule()));
    }

  public:
    /// Build a reference to the given declaration.
    Expr *buildDeclRef(SelectedOverload overload, DeclNameLoc loc,
                       ConstraintLocatorBuilder locator, bool implicit) {
      auto choice = overload.choice;
      assert(choice.getKind() != OverloadChoiceKind::DeclViaDynamic);
      auto *decl = choice.getDecl();
      auto fullType = simplifyType(overload.openedFullType);

      // Determine the declaration selected for this overloaded reference.
      auto &ctx = cs.getASTContext();
      
      auto semantics = decl->getAccessSemanticsFromContext(cs.DC,
                                                           /*isAccessOnSelf*/false);

      // If this is a member of a nominal type, build a reference to the
      // member with an implied base type.
      if (decl->getDeclContext()->isTypeContext() && isa<FuncDecl>(decl)) {
        assert(cast<FuncDecl>(decl)->isOperator() && "Must be an operator");

        auto baseTy = getBaseType(fullType->castTo<FunctionType>());

        // Handle operator requirements found in protocols.
        if (auto proto = dyn_cast<ProtocolDecl>(decl->getDeclContext())) {
          bool isCurried = shouldBuildCurryThunk(choice, /*baseIsInstance=*/false);

          // If we have a concrete conformance, build a call to the witness.
          //
          // FIXME: This is awful. We should be able to handle this as a call to
          // the protocol requirement with Self == the concrete type, and SILGen
          // (or later) can devirtualize as appropriate.
          auto conformance =
            TypeChecker::conformsToProtocol(baseTy, proto, cs.DC->getParentModule());
          if (conformance.isConcrete()) {
            if (auto witness = conformance.getConcrete()->getWitnessDecl(decl)) {
              bool isMemberOperator = witness->getDeclContext()->isTypeContext();

              if (!isMemberOperator || !isCurried) {
                // The fullType was computed by substituting the protocol
                // requirement so it always has a (Self) -> ... curried
                // application. Strip it off if the witness was a top-level
                // function.
                Type refType;
                if (isMemberOperator)
                  refType = fullType;
                else
                  refType = fullType->castTo<AnyFunctionType>()->getResult();

                // Build the AST for the call to the witness.
                auto subMap = getOperatorSubstitutions(witness, refType);
                if (subMap) {
                  ConcreteDeclRef witnessRef(witness, *subMap);
                  auto declRefExpr =  new (ctx) DeclRefExpr(witnessRef, loc,
                                                            /*Implicit=*/false);
                  declRefExpr->setFunctionRefKind(choice.getFunctionRefKind());
                  cs.setType(declRefExpr, refType);

                  Expr *refExpr;
                  if (isMemberOperator) {
                    // If the operator is a type member, add the implicit
                    // (Self) -> ... call.
                    Expr *base =
                      TypeExpr::createImplicitHack(loc.getBaseNameLoc(), baseTy,
                                                   ctx);
                    cs.setType(base, MetatypeType::get(baseTy));

                    refExpr = new (ctx) DotSyntaxCallExpr(declRefExpr,
                                                          SourceLoc(), base);
                    auto refType = fullType->castTo<FunctionType>()->getResult();
                    cs.setType(refExpr, refType);
                  } else {
                    refExpr = declRefExpr;
                  }

                  return forceUnwrapIfExpected(refExpr, choice, locator);
                }
              }
            }
          }
        }

        // Build a reference to the member.
        Expr *base =
          TypeExpr::createImplicitHack(loc.getBaseNameLoc(), baseTy, ctx);
        cs.cacheExprTypes(base);

        return buildMemberRef(base, SourceLoc(), overload, loc, locator,
                              locator, implicit, semantics);
      }

      if (isa<TypeDecl>(decl) && !isa<ModuleDecl>(decl)) {
        auto typeExpr = TypeExpr::createImplicitHack(
            loc.getBaseNameLoc(), fullType->getMetatypeInstanceType(), ctx);
        cs.cacheType(typeExpr);
        return typeExpr;
      }

      auto ref = resolveConcreteDeclRef(decl, locator);
      auto declRefExpr =
          new (ctx) DeclRefExpr(ref, loc, implicit, semantics, fullType);
      cs.cacheType(declRefExpr);
      declRefExpr->setFunctionRefKind(choice.getFunctionRefKind());
      Expr *result = forceUnwrapIfExpected(declRefExpr, choice, locator);

      if (auto *fnDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
        if (AnyFunctionRef(fnDecl).hasExternalPropertyWrapperParameters() &&
            (declRefExpr->getFunctionRefKind() == FunctionRefKind::Compound ||
             declRefExpr->getFunctionRefKind() == FunctionRefKind::Unapplied)) {
          auto &appliedWrappers = solution.appliedPropertyWrappers[locator.getAnchor()];
          result = buildPropertyWrapperFnThunk(result, fullType->getAs<FunctionType>(), fnDecl,
                                               ref, appliedWrappers);
        }
      }

      return result;
    }

    /// Describes an opened existential that has not yet been closed.
    struct OpenedExistential {
      /// The archetype describing this opened existential.
      OpenedArchetypeType *Archetype;

      /// The existential value being opened.
      Expr *ExistentialValue;

      /// The opaque value (of archetype type) stored within the
      /// existential.
      OpaqueValueExpr *OpaqueValue;

      /// The depth of this currently-opened existential. Once the
      /// depth of the expression stack is equal to this value, the
      /// existential can be closed.
      unsigned Depth;
    };

    /// A stack of opened existentials that have not yet been closed.
    /// Ordered by decreasing depth.
    llvm::SmallVector<OpenedExistential, 2> OpenedExistentials;

    /// A stack of expressions being walked, used to compute existential depth.
    llvm::SmallVector<Expr *, 8> ExprStack;

    /// A map of apply exprs to their callee locators. This is necessary
    /// because after rewriting an apply's function expr, its callee locator
    /// will no longer be equivalent to the one stored in the solution.
    llvm::DenseMap<ApplyExpr *, ConstraintLocator *> CalleeLocators;

    /// A cache of decl references with their contextual substitutions for a
    /// given callee locator.
    llvm::DenseMap<ConstraintLocator *, ConcreteDeclRef> CachedConcreteRefs;

    /// Resolves the contextual substitutions for a reference to a declaration
    /// at a given locator. This should be preferred to
    /// Solution::resolveConcreteDeclRef as it caches the result.
    ConcreteDeclRef
    resolveConcreteDeclRef(ValueDecl *decl, ConstraintLocatorBuilder locator) {
      if (!decl)
        return ConcreteDeclRef();

      // Cache the resulting concrete reference. Ideally this would be done on
      // Solution, however unfortunately that would require a const_cast which
      // would be undefined behaviour if we ever had a `const Solution`.
      auto *loc = getConstraintSystem().getConstraintLocator(locator);
      auto &ref = CachedConcreteRefs[loc];
      if (!ref)
        ref = solution.resolveConcreteDeclRef(decl, loc);

      return ref;
    }

    /// Members which are AbstractFunctionDecls but not FuncDecls cannot
    /// mutate self.
    bool isNonMutatingMember(ValueDecl *member) {
      if (!isa<AbstractFunctionDecl>(member))
        return false;
      return !isa<FuncDecl>(member) || !cast<FuncDecl>(member)->isMutating();
    }

    /// If the expression might be a dynamic method call, return the base
    /// value for the call.
    Expr *getBaseExpr(Expr *expr) {
      // Keep going up as long as this expression is the parent's base.
      if (auto unresolvedDot = dyn_cast<UnresolvedDotExpr>(expr)) {
        return unresolvedDot->getBase();
      // Remaining cases should only come up when we're re-typechecking.
      // FIXME: really it would be much better if Sema had stricter phase
      // separation.
      } else if (auto dotSyntax = dyn_cast<DotSyntaxCallExpr>(expr)) {
        return dotSyntax->getArg();
      } else if (auto ctorRef = dyn_cast<ConstructorRefCallExpr>(expr)) {
        return ctorRef->getArg();
      } else if (auto apply = dyn_cast<ApplyExpr>(expr)) {
        return apply->getFn();
      } else if (auto lookupRef = dyn_cast<LookupExpr>(expr)) {
        return lookupRef->getBase();
      } else if (auto load = dyn_cast<LoadExpr>(expr)) {
        return load->getSubExpr();
      } else if (auto inout = dyn_cast<InOutExpr>(expr)) {
        return inout->getSubExpr();
      } else if (auto force = dyn_cast<ForceValueExpr>(expr)) {
        return force->getSubExpr();
      } else {
        return nullptr;
      }
    }

    /// Calculates the nesting depth of the current application.
    unsigned getArgCount(unsigned maxArgCount) {
      // FIXME: Walking over the ExprStack to figure out the number of argument
      // lists being applied is brittle. We should instead be checking
      // hasAppliedSelf to figure out if the self param is applied, and looking
      // at the FunctionRefKind to see if the parameter list is applied.
      unsigned e = ExprStack.size();
      unsigned argCount;

      // Starting from the current expression, count up if the expression is
      // equal to its parent expression's base.
      Expr *prev = ExprStack.back();

      for (argCount = 1; argCount < maxArgCount && argCount < e; ++argCount) {
        Expr *result = ExprStack[e - argCount - 1];
        Expr *base = getBaseExpr(result);
        if (base != prev)
          break;
        prev = result;
      }

      return argCount;
    }

    /// Open an existential value into a new, opaque value of
    /// archetype type.
    ///
    /// \param base An expression of existential type whose value will
    /// be opened.
    ///
    /// \param archetype The archetype that describes the opened existential
    /// type.
    ///
    /// \param member The member that is being referenced on the existential
    /// type.
    ///
    /// \returns An OpaqueValueExpr that provides a reference to the value
    /// stored within the expression or its metatype (if the base was a
    /// metatype).
    Expr *openExistentialReference(Expr *base, OpenedArchetypeType *archetype,
                                   ValueDecl *member) {
      assert(archetype && "archetype not already opened?");

      // Dig out the base type.
      Type baseTy = cs.getType(base);

      // Look through lvalues.
      bool isLValue = false;
      if (auto lvalueTy = baseTy->getAs<LValueType>()) {
        isLValue = true;
        baseTy = lvalueTy->getObjectType();
      }

      // Look through metatypes.
      bool isMetatype = false;
      if (auto metaTy = baseTy->getAs<AnyMetatypeType>()) {
        isMetatype = true;
        baseTy = metaTy->getInstanceType();
      }

      assert(baseTy->isAnyExistentialType() && "Type must be existential");

      // If the base was an lvalue but it will only be treated as an
      // rvalue, turn the base into an rvalue now. This results in
      // better SILGen.
      if (isLValue &&
          (isNonMutatingMember(member) ||
           member->getDeclContext()->getDeclaredInterfaceType()
             ->hasReferenceSemantics())) {
        base = cs.coerceToRValue(base);
        isLValue = false;
      }

      // Determine the number of applications that need to occur before
      // we can close this existential, and record it.
      unsigned maxArgCount = member->getNumCurryLevels();
      unsigned depth = ExprStack.size() - getArgCount(maxArgCount);

      // Invalid case -- direct call of a metatype. Has one less argument
      // application because there's no ".init".
      if (isa<ApplyExpr>(ExprStack.back()))
        depth++;

      // Create the opaque opened value. If we started with a
      // metatype, it's a metatype.
      Type opaqueType = archetype;
      if (isMetatype)
        opaqueType = MetatypeType::get(opaqueType);
      if (isLValue)
        opaqueType = LValueType::get(opaqueType);

      ASTContext &ctx = cs.getASTContext();
      auto archetypeVal =
          new (ctx) OpaqueValueExpr(base->getSourceRange(), opaqueType);
      cs.cacheType(archetypeVal);

      // Record the opened existential.
      OpenedExistentials.push_back({archetype, base, archetypeVal, depth});

      return archetypeVal;
    }

    /// Trying to close the active existential, if there is one.
    bool closeExistential(Expr *&result, ConstraintLocatorBuilder locator,
                          bool force=false) {
      if (OpenedExistentials.empty())
        return false;

      auto &record = OpenedExistentials.back();
      assert(record.Depth <= ExprStack.size());

      if (!force && record.Depth < ExprStack.size() - 1)
        return false;

      // If we had a return type of 'Self', erase it.
      Type resultTy;
      resultTy = cs.getType(result);
      if (resultTy->hasOpenedExistential(record.Archetype)) {
        Type erasedTy = resultTy->eraseOpenedExistential(record.Archetype);
        auto range = result->getSourceRange();
        result = coerceToType(result, erasedTy, locator);
        // FIXME: Implement missing tuple-to-tuple conversion
        if (result == nullptr) {
          result = new (cs.getASTContext()) ErrorExpr(range);
          cs.setType(result, erasedTy);
          // The opaque value is no longer reachable in an AST walk as
          // a result of the result above being replaced with an
          // ErrorExpr, but there is code expecting to have a type set
          // on it. Since we no longer have a reachable reference,
          // we'll null this out.
          record.OpaqueValue = nullptr;
        }
      }

      // Form the open-existential expression.
      result = new (cs.getASTContext()) OpenExistentialExpr(
                                  record.ExistentialValue,
                                  record.OpaqueValue,
                                  result, cs.getType(result));
      cs.cacheType(result);

      OpenedExistentials.pop_back();
      return true;
    }

    /// Determines if a partially-applied member reference should be
    /// converted into a fully-applied member reference with a pair of
    /// closures.
    bool shouldBuildCurryThunk(OverloadChoice choice,
                               bool baseIsInstance) {
      ValueDecl *member = choice.getDecl();
      auto isDynamic = choice.getKind() == OverloadChoiceKind::DeclViaDynamic;

      // FIXME: We should finish plumbing this through for dynamic
      // lookup as well.
      if (isDynamic || member->getAttrs().hasAttribute<OptionalAttr>())
        return false;

      // If we're inside a selector expression, don't build the thunk.
      // Were not actually going to emit the member reference, just
      // look at the AST.
      for (auto expr : ExprStack)
        if (isa<ObjCSelectorExpr>(expr))
          return false;

      // Unbound instance method references always build a thunk, even if
      // we apply the arguments (eg, SomeClass.method(self)(a)), to avoid
      // representational issues.
      if (!baseIsInstance && member->isInstanceMember())
        return true;

      // Figure out how many argument lists we need.
      unsigned maxArgCount = member->getNumCurryLevels();

      unsigned argCount = [&]() -> unsigned {
        Expr *prev = ExprStack.back();

        // FIXME: Representational gunk because "T(...)" is really
        // "T.init(...)" -- pretend it has two argument lists like
        // a real '.' call.
        if (isa<ConstructorDecl>(member) &&
            isa<CallExpr>(prev) &&
            isa<TypeExpr>(cast<CallExpr>(prev)->getFn())) {
          assert(maxArgCount == 2);
          return 2;
        }

        // Similarly, ".foo(...)" really applies two argument lists.
        if (isa<CallExpr>(prev) &&
            isa<UnresolvedMemberExpr>(cast<CallExpr>(prev)->getFn()))
          return 2;

        return getArgCount(maxArgCount);
      }();

      // If we have fewer argument lists than expected, build a thunk.
      if (argCount < maxArgCount)
        return true;

      return false;
    }

    AutoClosureExpr *buildPropertyWrapperFnThunk(
        Expr *fnRef, FunctionType *fnType, AnyFunctionRef fnDecl, ConcreteDeclRef ref,
        ArrayRef<AppliedPropertyWrapper> appliedPropertyWrappers) {
      auto &context = cs.getASTContext();
      auto paramInfo = fnType->getParams();

      SmallVector<Expr *, 4> args;
      SmallVector<Identifier, 4> argLabels;

      auto *innerParams = fnDecl.getParameters();
      SmallVector<AnyFunctionType::Param, 4> innerParamTypes;

      OptionSet<ParameterList::CloneFlags> options
        = (ParameterList::Implicit |
           ParameterList::NamedArguments);
      auto *outerParams = innerParams->clone(context, options);
      SmallVector<AnyFunctionType::Param, 4> outerParamTypes;

      unsigned appliedWrapperIndex = 0;
      for (auto i : indices(*innerParams)) {
        auto *innerParam = innerParams->get(i);
        auto *outerParam = outerParams->get(i);
        auto outerParamType = paramInfo[i].getPlainType();

        Expr *paramRef = new (context) DeclRefExpr(outerParam, DeclNameLoc(),
                                                   /*implicit=*/true);
        paramRef->setType(outerParam->isInOut()
                          ? LValueType::get(outerParamType) : outerParamType);
        cs.cacheType(paramRef);

        if (innerParam->hasAttachedPropertyWrapper()) {
          // Rewrite the parameter ref to the backing wrapper initialization
          // expression.
          auto appliedWrapper = appliedPropertyWrappers[appliedWrapperIndex++];
          auto wrapperType = appliedWrapper.wrapperType;
          auto initKind = appliedWrapper.initKind;

          using ValueKind = AppliedPropertyWrapperExpr::ValueKind;
          ValueKind valueKind = (initKind == PropertyWrapperInitKind::ProjectedValue ?
                                 ValueKind::ProjectedValue : ValueKind::WrappedValue);

          paramRef = AppliedPropertyWrapperExpr::create(context, ref, innerParam, SourceLoc(),
                                                        wrapperType, paramRef, valueKind);
          cs.cacheExprTypes(paramRef);

          // SILGen knows how to emit property-wrapped parameters, but the
          // function type needs the backing wrapper type in its param list.
          innerParamTypes.push_back(AnyFunctionType::Param(
              paramRef->getType(), innerParam->getArgumentName(),
              ParameterTypeFlags(), innerParam->getParameterName()));
        } else {
          // Rewrite the parameter ref if necessary.
          if (outerParam->isInOut()) {
            paramRef = new (context) InOutExpr(SourceLoc(), paramRef,
                                               outerParamType, /*implicit=*/true);
          } else if (innerParam->isVariadic()) {
            paramRef = new (context) VarargExpansionExpr(paramRef,
                                                         /*implicit=*/ true,
                                                         outerParamType);
          }
          cs.cacheType(paramRef);

          innerParamTypes.push_back(innerParam->toFunctionParam());
        }

        // The outer parameters are for an autoclosure, which shouldn't have
        // argument labels.
        outerParamTypes.push_back(AnyFunctionType::Param(outerParamType,
                                                         Identifier(),
                                                         paramInfo[i].getParameterFlags()));
        outerParam->setInterfaceType(outerParamType);

        if (fnDecl.getAbstractFunctionDecl())
          argLabels.push_back(innerParam->getArgumentName());

        args.push_back(paramRef);
      }

      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo fnInfo;
      fnRef->setType(
          FunctionType::get(innerParamTypes, fnType->getResult(), fnInfo));
      cs.cacheType(fnRef);

      auto *fnCall = CallExpr::createImplicit(context, fnRef, args, argLabels);
      fnCall->setType(fnType->getResult());
      cs.cacheType(fnCall);
      cs.cacheType(fnCall->getArg());

      auto discriminator = AutoClosureExpr::InvalidDiscriminator;

      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo closureInfo;
      auto *autoClosureType =
          FunctionType::get(outerParamTypes, fnType->getResult(), closureInfo);
      auto *autoClosure = new (context) AutoClosureExpr(fnCall, autoClosureType,
                                                        discriminator, cs.DC);
      autoClosure->setParameterList(outerParams);
      autoClosure->setThunkKind(AutoClosureExpr::Kind::SingleCurryThunk);
      cs.cacheType(autoClosure);

      return autoClosure;
    }

    AutoClosureExpr *buildCurryThunk(ValueDecl *member,
                                     FunctionType *selfFnTy,
                                     Expr *selfParamRef,
                                     Expr *ref,
                                     ConstraintLocatorBuilder locator) {
      auto &context = cs.getASTContext();

      OptionSet<ParameterList::CloneFlags> options
        = (ParameterList::Implicit |
           ParameterList::NamedArguments);
      auto *params = getParameterList(member)->clone(context, options);

      for (auto idx : indices(*params)) {
        auto *param = params->get(idx);
        auto arg = selfFnTy->getParams()[idx];

        param->setInterfaceType(
            arg.getParameterType()->mapTypeOutOfContext());
        param->setSpecifier(
          ParamDecl::getParameterSpecifierForValueOwnership(
            arg.getValueOwnership()));
      }

      auto resultTy = selfFnTy->getResult();
      auto discriminator = AutoClosureExpr::InvalidDiscriminator;
      auto closure =
          new (context) AutoClosureExpr(/*set body later*/nullptr, resultTy,
                                        discriminator, cs.DC);
      closure->setParameterList(params);
      closure->setType(selfFnTy);
      closure->setThunkKind(AutoClosureExpr::Kind::SingleCurryThunk);
      cs.cacheType(closure);

      auto refTy = cs.getType(ref)->castTo<FunctionType>();
      auto calleeFnType = refTy->getResult()->castTo<FunctionType>();
      auto calleeParams = calleeFnType->getParams();
      auto calleeResultTy = calleeFnType->getResult();

      auto selfParam = refTy->getParams()[0];
      auto selfParamTy = selfParam.getPlainType();

      Expr *selfOpenedRef = selfParamRef;

      // If the 'self' parameter has non-trivial ownership, adjust the
      // argument accordingly.
      switch (selfParam.getValueOwnership()) {
      case ValueOwnership::Default:
      case ValueOwnership::InOut:
        break;

      case ValueOwnership::Owned:
      case ValueOwnership::Shared:
        auto selfArgTy = ParenType::get(context,
                                        selfParam.getPlainType(),
                                        selfParam.getParameterFlags());
        selfOpenedRef->setType(selfArgTy);
        cs.cacheType(selfOpenedRef);
        break;
      }

      if (selfParamTy->hasOpenedExistential()) {
        // If we're opening an existential:
        // - the type of 'ref' inside the closure is written in terms of the
        //   open existental archetype
        // - the type of the closure, 'selfFnTy' is written in terms of the
        //   erased existential bounds
        if (selfParam.isInOut())
          selfParamTy = LValueType::get(selfParamTy);

        selfOpenedRef =
            new (context) OpaqueValueExpr(SourceLoc(), selfParamTy);
        cs.cacheType(selfOpenedRef);
      }

      // (Self) -> ...
      ApplyExpr *selfCall = new (context) DotSyntaxCallExpr(
          ref, SourceLoc(), selfOpenedRef);
      selfCall->setType(refTy->getResult());
      cs.cacheType(selfCall);

      auto &appliedWrappers = solution.appliedPropertyWrappers[locator.getAnchor()];
      if (!appliedWrappers.empty()) {
        auto fnDecl = AnyFunctionRef(dyn_cast<AbstractFunctionDecl>(member));
        auto callee = resolveConcreteDeclRef(member, locator);
        auto *closure = buildPropertyWrapperFnThunk(selfCall, calleeFnType,
                                                    fnDecl, callee, appliedWrappers);

        // FIXME: Verify ExtInfo state is correct, not working by accident.
        FunctionType::ExtInfo info;
        ref->setType(
            FunctionType::get(refTy->getParams(), selfCall->getType(), info));
        cs.cacheType(ref);

        // FIXME: There's more work to do.
        return closure;
      }

      // Pass all the closure parameters to the call.
      SmallVector<Identifier, 4> labels;
      SmallVector<SourceLoc, 4> labelLocs;
      SmallVector<Expr *, 4> args;

      for (auto idx : indices(*params)) {
        auto *param = params->get(idx);
        auto calleeParamType = calleeParams[idx].getParameterType();

        auto type = param->getType();

        Expr *paramRef =
          new (context) DeclRefExpr(param, DeclNameLoc(), /*implicit*/ true);
        paramRef->setType(
            param->isInOut()
              ? LValueType::get(type)
              : type);
        cs.cacheType(paramRef);

        paramRef = coerceToType(
            paramRef,
            param->isInOut()
              ? LValueType::get(calleeParamType)
              : calleeParamType,
            locator);

        if (param->isInOut()) {
          paramRef =
            new (context) InOutExpr(SourceLoc(), paramRef, calleeParamType,
                                    /*implicit=*/true);
          cs.cacheType(paramRef);
        } else if (param->isVariadic()) {
          paramRef =
            new (context) VarargExpansionExpr(paramRef, /*implicit*/ true);
          paramRef->setType(calleeParamType);
          cs.cacheType(paramRef);
        }

        args.push_back(paramRef);

        labels.push_back(calleeParams[idx].getLabel());
        labelLocs.push_back(SourceLoc());
      }

      Expr *closureArg;
      if (args.size() == 1 &&
          labels[0].empty() &&
          !calleeParams[0].getParameterFlags().isVariadic()) {
        closureArg = new (context) ParenExpr(SourceLoc(), args[0], SourceLoc(),
                                             /*hasTrailingClosure=*/false);
        closureArg->setImplicit();
      } else {
        closureArg = TupleExpr::create(context, SourceLoc(), args, labels, labelLocs,
                                       SourceLoc(), /*hasTrailingClosure=*/false,
                                       /*implicit=*/true);
      }

      auto argTy = AnyFunctionType::composeInput(context, calleeParams,
                                                 /*canonical*/false);
      closureArg->setType(argTy);
      cs.cacheType(closureArg);

      // (Self) -> (Args...) -> ...
      auto *closureCall =
        CallExpr::create(context, selfCall, closureArg, { }, { },
                         /*hasTrailingClosure=*/false,
                         /*implicit=*/true);
      closureCall->setType(calleeResultTy);
      cs.cacheType(closureCall);

      Expr *closureBody = closureCall;
      closureBody = coerceToType(closureCall, resultTy, locator);

      if (selfFnTy->getExtInfo().isThrowing()) {
        closureBody = new (context) TryExpr(closureBody->getStartLoc(), closureBody,
                                            cs.getType(closureBody),
                                            /*implicit=*/true);
        cs.cacheType(closureBody);
      }

      if (selfParam.getPlainType()->hasOpenedExistential()) {
        closureBody =
          new (context) OpenExistentialExpr(
            selfParamRef, cast<OpaqueValueExpr>(selfOpenedRef),
            closureBody, resultTy);
        cs.cacheType(closureBody);
      }

      closure->setBody(closureBody);

      return closure;
    }

    /// Build a new member reference with the given base and member.
    Expr *buildMemberRef(Expr *base, SourceLoc dotLoc,
                         SelectedOverload overload, DeclNameLoc memberLoc,
                         ConstraintLocatorBuilder locator,
                         ConstraintLocatorBuilder memberLocator, bool Implicit,
                         AccessSemantics semantics) {
      const auto &choice = overload.choice;
      const auto openedType = overload.openedType;

      ValueDecl *member = choice.getDecl();

      auto &context = cs.getASTContext();

      bool isSuper = base->isSuperExpr();

      // The formal type of the 'self' value for the call.
      Type baseTy = cs.getType(base)->getRValueType();

      // Figure out the actual base type, and whether we have an instance of
      // that type or its metatype.
      bool baseIsInstance = true;
      bool isExistentialMetatype = false;
      if (auto baseMeta = baseTy->getAs<AnyMetatypeType>()) {
        baseIsInstance = false;
        isExistentialMetatype = baseMeta->is<ExistentialMetatypeType>();
        baseTy = baseMeta->getInstanceType();

        // A valid reference to a static member (computed property or a method)
        // declared on a protocol is only possible if result type conforms to
        // that protocol, otherwise it would be impossible to find a witness to
        // use.
        // Such means that (for valid references) base expression here could be
        // adjusted to  point to a type conforming to a protocol as-if reference
        // has originated directly from it e.g.
        //
        // \code
        // protocol P {}
        // struct S : P {}
        //
        // extension P {
        //   static var foo: S { S() }
        // }
        //
        // _ = P.foo
        // \endcode
        //
        // Here `P.foo` would be replaced with `S.foo`
        if (!isExistentialMetatype && baseTy->is<ProtocolType>() &&
            member->isStatic()) {
          auto selfParam =
              overload.openedFullType->castTo<FunctionType>()->getParams()[0];

          Type baseTy =
              simplifyType(selfParam.getPlainType())->getMetatypeInstanceType();

          base = TypeExpr::createImplicitHack(base->getLoc(), baseTy, context);
          cs.cacheType(base);
        }
      }

      // Build a member reference.
      auto memberRef = resolveConcreteDeclRef(member, memberLocator);

      // If we're referring to a member type, it's just a type
      // reference.
      if (auto *TD = dyn_cast<TypeDecl>(member)) {
        Type refType = simplifyType(openedType);
        auto ref = TypeExpr::createForDecl(memberLoc, TD, cs.DC);
        cs.setType(ref, refType);
        auto *result = new (context) DotSyntaxBaseIgnoredExpr(
            base, dotLoc, ref, refType);
        cs.setType(result, refType);
        return result;
      }

      auto refTy = simplifyType(overload.openedFullType);

      // If we're referring to the member of a module, it's just a simple
      // reference.
      if (baseTy->is<ModuleType>()) {
        assert(semantics == AccessSemantics::Ordinary &&
               "Direct property access doesn't make sense for this");
        auto ref = new (context) DeclRefExpr(memberRef, memberLoc, Implicit);
        cs.setType(ref, refTy);
        ref->setFunctionRefKind(choice.getFunctionRefKind());
        auto *DSBI = cs.cacheType(new (context) DotSyntaxBaseIgnoredExpr(
            base, dotLoc, ref, cs.getType(ref)));
        return forceUnwrapIfExpected(DSBI, choice, memberLocator);
      }

      const bool isUnboundInstanceMember =
          (!baseIsInstance && member->isInstanceMember());
      const bool isPartialApplication =
          shouldBuildCurryThunk(choice, baseIsInstance);

      // The formal type of the 'self' value for the member's declaration.
      Type containerTy = getBaseType(refTy->castTo<FunctionType>());

      // If we have an opened existential, selfTy and baseTy will both be
      // the same opened existential type.
      Type selfTy = containerTy;

      // If we opened up an existential when referencing this member, update
      // the base accordingly.
      bool openedExistential = false;

      // For a partial application, we have to open the existential inside
      // the thunk itself.
      auto knownOpened = solution.OpenedExistentialTypes.find(
                           getConstraintSystem().getConstraintLocator(
                             memberLocator));
      if (knownOpened != solution.OpenedExistentialTypes.end()) {
        // Determine if we're going to have an OpenExistentialExpr around
        // this member reference.
        //
        // If we have a partial application of a protocol method, we open
        // the existential in the curry thunk, instead of opening it here,
        // because we won't have a 'self' value until the curry thunk is
        // applied.
        //
        // However, a partial application of a class method on a subclass
        // existential does need to open the existential, so that it can be
        // upcast to the appropriate class reference type.
        if (!isPartialApplication || !containerTy->hasOpenedExistential()) {
          // Open the existential before performing the member reference.
          base = openExistentialReference(base, knownOpened->second, member);
          baseTy = knownOpened->second;
          selfTy = baseTy;
          openedExistential = true;
        } else {
          // Erase opened existentials from the type of the thunk; we're
          // going to open the existential inside the thunk's body.
          containerTy = containerTy->eraseOpenedExistential(knownOpened->second);
          selfTy = containerTy;
        }
      }

      // References to properties with accessors and storage usually go
      // through the accessors, but sometimes are direct.
      if (auto *VD = dyn_cast<VarDecl>(member)) {
        if (semantics == AccessSemantics::Ordinary)
          semantics = getImplicitMemberReferenceAccessSemantics(base, VD, dc);
      }

      auto isDynamic = choice.getKind() == OverloadChoiceKind::DeclViaDynamic;
      if (baseIsInstance) {
        // Convert the base to the appropriate container type, turning it
        // into an lvalue if required.

        // If the base is already an lvalue with the right base type, we can
        // pass it as an inout qualified type.
        auto selfParamTy = isDynamic ? selfTy : containerTy;

        if (selfTy->isEqual(baseTy))
          if (cs.getType(base)->is<LValueType>())
            selfParamTy = InOutType::get(selfTy);

        base = coerceSelfArgumentToType(
                 base, selfParamTy, member,
                 locator.withPathElement(ConstraintLocator::MemberRefBase));
      } else {
        if (!isExistentialMetatype || openedExistential) {
          // Convert the base to an rvalue of the appropriate metatype.
          base = coerceToType(base,
                              MetatypeType::get(
                                isDynamic ? selfTy : containerTy),
                              locator.withPathElement(
                                ConstraintLocator::MemberRefBase));
        }

        if (!base)
          return nullptr;

        base = cs.coerceToRValue(base);
      }
      assert(base && "Unable to convert base?");

      // Handle dynamic references.
      if (isDynamic || member->getAttrs().hasAttribute<OptionalAttr>()) {
        base = cs.coerceToRValue(base);
        Expr *ref = new (context) DynamicMemberRefExpr(base, dotLoc, memberRef,
                                                       memberLoc);
        ref->setImplicit(Implicit);
        // FIXME: FunctionRefKind

        // Compute the type of the reference.
        Type refType = simplifyType(openedType);

        // If the base was an opened existential, erase the opened
        // existential.
        if (openedExistential) {
          refType = refType->eraseOpenedExistential(
              baseTy->castTo<OpenedArchetypeType>());
        }

        cs.setType(ref, refType);

        closeExistential(ref, locator, /*force=*/openedExistential);

        // If this attribute was inferred based on deprecated Swift 3 rules,
        // complain.
        if (auto attr = member->getAttrs().getAttribute<ObjCAttr>()) {
          if (attr->isSwift3Inferred() &&
              context.LangOpts.WarnSwift3ObjCInference ==
                  Swift3ObjCInferenceWarnings::Minimal) {
            context.Diags.diagnose(
                memberLoc, diag::expr_dynamic_lookup_swift3_objc_inference,
                member->getDescriptiveKind(), member->getName(),
                member->getDeclContext()->getSelfNominalTypeDecl()->getName());
            context.Diags
                .diagnose(member, diag::make_decl_objc,
                          member->getDescriptiveKind())
                .fixItInsert(member->getAttributeInsertionLoc(false), "@objc ");
          }
        }

        if (isDynamic) {
          // Rewrite for implicit unwrapping if the solution requires it.
          auto *dynamicLocator =
              cs.getConstraintLocator(memberLocator.withPathElement(
                  ConstraintLocator::DynamicLookupResult));

          if (solution.getDisjunctionChoice(dynamicLocator)) {
            auto *forceValue =
                new (context) ForceValueExpr(ref, ref->getEndLoc());
            auto optTy = cs.getType(forceValue->getSubExpr());
            cs.setType(forceValue, optTy->getOptionalObjectType());
            ref = forceValue;
          }
        }

        // We also need to handle the implicitly unwrap of the result
        // of the called function if that's the type checking solution
        // we ended up with.
        return forceUnwrapIfExpected(
            ref, choice, memberLocator,
            member->getAttrs().hasAttribute<OptionalAttr>());
      }

      // For properties, build member references.
      if (auto *varDecl = dyn_cast<VarDecl>(member)) {
        if (isUnboundInstanceMember) {
          assert(memberLocator.getBaseLocator() &&
                 cs.UnevaluatedRootExprs.count(
                     getAsExpr(memberLocator.getBaseLocator()->getAnchor())) &&
                 "Attempt to reference an instance member of a metatype");
          auto baseInstanceTy = cs.getType(base)
              ->getInOutObjectType()->getMetatypeInstanceType();
          base = new (context) UnevaluatedInstanceExpr(base, baseInstanceTy);
          cs.cacheType(base);
          base->setImplicit();
        }

        const auto hasDynamicSelf =
            varDecl->getValueInterfaceType()->hasDynamicSelfType();

        auto memberRefExpr
          = new (context) MemberRefExpr(base, dotLoc, memberRef,
                                        memberLoc, Implicit, semantics);
        memberRefExpr->setIsSuper(isSuper);

        if (hasDynamicSelf)
          refTy = refTy->replaceCovariantResultType(containerTy, 1);
        cs.setType(memberRefExpr, refTy->castTo<FunctionType>()->getResult());

        Expr *result = memberRefExpr;
        closeExistential(result, locator);

        // If the property is of dynamic 'Self' type, wrap an implicit
        // conversion around the resulting expression, with the destination
        // type having 'Self' swapped for the appropriate replacement
        // type -- usually the base object type.
        if (hasDynamicSelf) {
          const auto conversionTy = simplifyType(openedType);
          if (!containerTy->isEqual(conversionTy)) {
            result = cs.cacheType(new (context) CovariantReturnConversionExpr(
                result, conversionTy));
          }
        }
        return forceUnwrapIfExpected(result, choice, memberLocator);
      }

      if (member->getInterfaceType()->hasDynamicSelfType())
        refTy = refTy->replaceCovariantResultType(containerTy, 2);

      // Handle all other references.
      auto declRefExpr = new (context) DeclRefExpr(memberRef, memberLoc,
                                                   Implicit, semantics);
      declRefExpr->setFunctionRefKind(choice.getFunctionRefKind());
      declRefExpr->setType(refTy);
      cs.setType(declRefExpr, refTy);
      Expr *ref = declRefExpr;

      const auto isSuperPartialApplication = isPartialApplication && isSuper;
      if (isSuperPartialApplication) {
        // A partial application thunk consists of two nested closures:
        //
        // { self in { args... in self.method(args...) } }
        //
        // If the reference has an applied 'self', eg 'let fn = foo.method',
        // the outermost closure is wrapped inside a single ApplyExpr:
        //
        // { self in { args... in self.method(args...) } }(foo)
        //
        // This is done instead of just hoising the expression 'foo' up
        // into the closure, which would change evaluation order.
        //
        // However, for a super method reference, eg, 'let fn = super.foo',
        // the base expression is always a SuperRefExpr, possibly wrapped
        // by an upcast. Since SILGen expects super method calls to have a
        // very specific shape, we only emit a single closure here and
        // capture the original SuperRefExpr, since its evaluation does not
        // have side effects, instead of abstracting out a 'self' parameter.
        const auto selfFnTy =
            refTy->castTo<FunctionType>()->getResult()->castTo<FunctionType>();

        ref = buildCurryThunk(member, selfFnTy, base, ref, memberLocator);
      } else if (isPartialApplication) {
        auto curryThunkTy = refTy->castTo<FunctionType>();

        // Another case where we want to build a single closure is when
        // we have a partial application of a constructor on a statically-
        // derived metatype value. Again, there are no order of evaluation
        // concerns here, and keeping the call and base together in the AST
        // improves SILGen.
        if (isa<ConstructorDecl>(member) &&
            cs.isStaticallyDerivedMetatype(base)) {
          auto selfFnTy = curryThunkTy->getResult()->castTo<FunctionType>();

          // Add a useless ".self" to avoid downstream diagnostics.
          base = new (context) DotSelfExpr(base, SourceLoc(), base->getEndLoc(),
                                           cs.getType(base));
          cs.setType(base, base->getType());

          auto closure = buildCurryThunk(member, selfFnTy, base, ref,
                                         memberLocator);

          // Skip the code below -- we're not building an extra level of
          // call by applying the metatype; instead, the closure we just
          // built is the curried reference.
          return closure;
        }

        // Check if we need to open an existential stored inside 'self'.
        auto knownOpened = solution.OpenedExistentialTypes.find(
                             getConstraintSystem().getConstraintLocator(
                               memberLocator));
        if (knownOpened != solution.OpenedExistentialTypes.end()) {
          curryThunkTy =
            curryThunkTy->eraseOpenedExistential(knownOpened->second)
              ->castTo<FunctionType>();
        }

        auto discriminator = AutoClosureExpr::InvalidDiscriminator;

        // The outer closure.
        //
        //    let outerClosure = "{ self in \(closure) }"
        auto selfParam = curryThunkTy->getParams()[0];
        auto selfParamDecl = new (context) ParamDecl(
            SourceLoc(),
            /*argument label*/ SourceLoc(), Identifier(),
            /*parameter name*/ SourceLoc(), context.Id_self,
            cs.DC);

        auto selfParamTy = selfParam.getPlainType();
        bool isLValue = selfParam.isInOut();

        selfParamDecl->setInterfaceType(selfParamTy->mapTypeOutOfContext());
        selfParamDecl->setSpecifier(
          ParamDecl::getParameterSpecifierForValueOwnership(
            selfParam.getValueOwnership()));
        selfParamDecl->setImplicit();

        auto *outerParams =
            ParameterList::create(context, SourceLoc(), selfParamDecl,
                                  SourceLoc());

        // The inner closure.
        //
        //     let closure = "{ args... in self.member(args...) }"
        auto selfFnTy = curryThunkTy->getResult()->castTo<FunctionType>();

        Expr *selfParamRef =
            new (context) DeclRefExpr(selfParamDecl, DeclNameLoc(),
                                      /*implicit=*/true);

        selfParamRef->setType(
          isLValue ? LValueType::get(selfParamTy) : selfParamTy);
        cs.cacheType(selfParamRef);

        if (isLValue) {
          selfParamRef =
            new (context) InOutExpr(SourceLoc(), selfParamRef, selfParamTy,
                                    /*implicit=*/true);
          cs.cacheType(selfParamRef);
        }

        auto closure = buildCurryThunk(member, selfFnTy, selfParamRef, ref,
                                       memberLocator);

        auto outerClosure =
            new (context) AutoClosureExpr(closure, selfFnTy,
                                          discriminator, cs.DC);
        outerClosure->setThunkKind(AutoClosureExpr::Kind::DoubleCurryThunk);

        outerClosure->setParameterList(outerParams);
        outerClosure->setType(curryThunkTy);
        cs.cacheType(outerClosure);

        // Replace the DeclRefExpr with a closure expression which SILGen
        // knows how to emit.
        ref = outerClosure;
      }

      // If the member is a method with a dynamic 'Self' result type, wrap an
      // implicit function type conversion around the resulting expression,
      // with the destination type having 'Self' swapped for the appropriate
      // replacement type -- usually the base object type.
      if (!member->getDeclContext()->getSelfProtocolDecl()) {
        if (auto func = dyn_cast<AbstractFunctionDecl>(member)) {
          if (func->hasDynamicSelfResult() &&
              !baseTy->getOptionalObjectType()) {
            // FIXME: Once CovariantReturnConversionExpr (unchecked_ref_cast)
            // supports a class existential dest., consider using the opened
            // type directly to avoid recomputing the 'Self' replacement and
            // substituting.
            const Type replacementTy = getDynamicSelfReplacementType(
                baseTy, member, memberLocator.getBaseLocator());
            if (!replacementTy->isEqual(containerTy)) {
              Type conversionTy =
                  refTy->replaceCovariantResultType(replacementTy, 2);
              if (isSuperPartialApplication) {
                conversionTy =
                    conversionTy->castTo<FunctionType>()->getResult();
              }

              ref = cs.cacheType(new (context) CovariantFunctionConversionExpr(
                  ref, conversionTy));
            }
          }
        }
      }

      // The thunk that is built for a 'super' method reference does not
      // require application.
      if (isSuperPartialApplication) {
        return forceUnwrapIfExpected(ref, choice, memberLocator);
      }

      ApplyExpr *apply;
      if (isa<ConstructorDecl>(member)) {
        // FIXME: Provide type annotation.
        ref = forceUnwrapIfExpected(ref, choice, memberLocator);
        apply = new (context) ConstructorRefCallExpr(ref, base);
      } else if (isUnboundInstanceMember) {
        auto refType = cs.simplifyType(openedType);
        if (!cs.getType(ref)->isEqual(refType)) {
          ref = new (context) FunctionConversionExpr(ref, refType);
          cs.cacheType(ref);
        }

        // Reference to an unbound instance method.
        Expr *result = new (context) DotSyntaxBaseIgnoredExpr(base, dotLoc,
                                                              ref,
                                                              cs.getType(ref));
        cs.cacheType(result);
        closeExistential(result, locator, /*force=*/openedExistential);
        return forceUnwrapIfExpected(result, choice, memberLocator);
      } else {
        assert((!baseIsInstance || member->isInstanceMember()) &&
               "can't call a static method on an instance");
        ref = forceUnwrapIfExpected(ref, choice, memberLocator);
        apply = new (context) DotSyntaxCallExpr(ref, dotLoc, base);
        if (Implicit) {
          apply->setImplicit();
        }
      }

      return finishApply(apply, openedType, locator, memberLocator);
    }

    /// Convert the given literal expression via a protocol pair.
    ///
    /// This routine handles the two-step literal conversion process used
    /// by integer, float, character, extended grapheme cluster, and string
    /// literals. The first step uses \c builtinProtocol while the second
    /// step uses \c protocol.
    ///
    /// \param literal The literal expression.
    ///
    /// \param type The literal type. This type conforms to \c protocol,
    /// and may also conform to \c builtinProtocol.
    ///
    /// \param protocol The protocol that describes the literal requirement.
    ///
    /// \param literalType The name of the associated type in \c protocol that
    /// describes the argument type of the conversion function (\c
    /// literalFuncName).
    ///
    /// \param literalFuncName The name of the conversion function requirement
    /// in \c protocol.
    ///
    /// \param builtinProtocol The "builtin" form of the protocol, which
    /// always takes builtin types and can only be properly implemented
    /// by standard library types. If \c type does not conform to this
    /// protocol, it's literal type will.
    ///
    /// \param builtinLiteralFuncName The name of the conversion function
    /// requirement in \c builtinProtocol.
    ///
    /// \param brokenProtocolDiag The diagnostic to emit if the protocol
    /// is broken.
    ///
    /// \param brokenBuiltinProtocolDiag The diagnostic to emit if the builtin
    /// protocol is broken.
    ///
    /// \returns the converted literal expression.
    Expr *convertLiteralInPlace(LiteralExpr *literal, Type type,
                                ProtocolDecl *protocol, Identifier literalType,
                                DeclName literalFuncName,
                                ProtocolDecl *builtinProtocol,
                                DeclName builtinLiteralFuncName,
                                Diag<> brokenProtocolDiag,
                                Diag<> brokenBuiltinProtocolDiag);

    /// Finish a function application by performing the appropriate
    /// conversions on the function and argument expressions and setting
    /// the resulting type.
    ///
    /// \param apply The function application to finish type-checking, which
    /// may be a newly-built expression.
    ///
    /// \param openedType The "opened" type this expression had during
    /// type checking, which will be used to specialize the resulting,
    /// type-checked expression appropriately.
    ///
    /// \param locator The locator for the original expression.
    ///
    /// \param calleeLocator The locator that identifies the apply's callee.
    Expr *finishApply(ApplyExpr *apply, Type openedType,
                      ConstraintLocatorBuilder locator,
                      ConstraintLocatorBuilder calleeLocator);

    /// Build the function and argument for a `@dynamicCallable` application.
    std::pair</*fn*/ Expr *, /*arg*/ Expr *>
    buildDynamicCallable(ApplyExpr *apply, SelectedOverload selected,
                         FuncDecl *method, AnyFunctionType *methodType,
                         ConstraintLocatorBuilder applyFunctionLoc);

  private:
    /// Simplify the given type by substituting all occurrences of
    /// type variables for their fixed types.
    Type simplifyType(Type type) {
      return solution.simplifyType(type);
    }

  public:
    /// Coerce the given expression to the given type.
    ///
    /// This operation cannot fail.
    ///
    /// \param expr The expression to coerce.
    /// \param toType The type to coerce the expression to.
    /// \param locator Locator used to describe where in this expression we are.
    /// \param typeFromPattern Optionally, the caller can specify the pattern
    ///   from where the toType is derived, so that we can deliver better fixit.
    ///
    /// \returns the coerced expression, which will have type \c ToType.
    Expr *coerceToType(Expr *expr, Type toType,
                       ConstraintLocatorBuilder locator,
                       Optional<Pattern*> typeFromPattern = None);
    
    /// Coerce the given expression (which is the argument to a call) to
    /// the given parameter type.
    ///
    /// This operation cannot fail.
    ///
    /// \param arg The argument expression.
    /// \param funcType The function type.
    /// \param callee The callee for the function being applied.
    /// \param apply The ApplyExpr that forms the call.
    /// \param argLabels The argument labels provided for the call.
    /// \param locator Locator used to describe where in this expression we are.
    ///
    /// \returns the coerced expression, which will have type \c ToType.
    Expr *
    coerceCallArguments(Expr *arg, AnyFunctionType *funcType,
                        ConcreteDeclRef callee, ApplyExpr *apply,
                        ArrayRef<Identifier> argLabels,
                        ConstraintLocatorBuilder locator,
                        ArrayRef<AppliedPropertyWrapper> appliedPropertyWrappers);

    /// Coerce the given 'self' argument (e.g., for the base of a
    /// member expression) to the given type.
    ///
    /// \param expr The expression to coerce.
    ///
    /// \param baseTy The base type
    ///
    /// \param member The member being accessed.
    ///
    /// \param locator Locator used to describe where in this expression we are.
    Expr *coerceSelfArgumentToType(Expr *expr,
                                   Type baseTy, ValueDecl *member,
                                   ConstraintLocatorBuilder locator);

  private:
    /// Build a new subscript.
    ///
    /// \param base The base of the subscript.
    /// \param index The index of the subscript.
    /// \param locator The locator used to refer to the subscript.
    /// \param isImplicit Whether this is an implicit subscript.
    Expr *buildSubscript(Expr *base, Expr *index,
                         ArrayRef<Identifier> argLabels,
                         bool hasTrailingClosure,
                         ConstraintLocatorBuilder locator, bool isImplicit,
                         AccessSemantics semantics,
                         const SelectedOverload &selected) {
      // Build the new subscript.
      auto newSubscript = buildSubscriptHelper(base, index, argLabels,
                                               selected, hasTrailingClosure,
                                               locator, isImplicit, semantics);

      if (selected.choice.getKind() == OverloadChoiceKind::DeclViaDynamic) {
        // Rewrite for implicit unwrapping if the solution requires it.
        auto *dynamicLocator = cs.getConstraintLocator(
            locator, {ConstraintLocator::SubscriptMember,
                      ConstraintLocator::DynamicLookupResult});

        if (solution.getDisjunctionChoice(dynamicLocator)) {
          auto *forceValue = new (cs.getASTContext())
              ForceValueExpr(newSubscript, newSubscript->getEndLoc());
          auto optTy = cs.getType(forceValue->getSubExpr());
          cs.setType(forceValue, optTy->getOptionalObjectType());
          newSubscript = forceValue;
        }
      }

      if (selected.choice.isDecl()) {
        auto locatorKind = ConstraintLocator::SubscriptMember;
        if (selected.choice.getKind() ==
            OverloadChoiceKind::DynamicMemberLookup)
          locatorKind = ConstraintLocator::Member;

        if (selected.choice.getKind() ==
                OverloadChoiceKind::KeyPathDynamicMemberLookup &&
            !isExpr<SubscriptExpr>(locator.getAnchor()))
          locatorKind = ConstraintLocator::Member;

        newSubscript =
            forceUnwrapIfExpected(newSubscript, selected.choice,
                                  locator.withPathElement(locatorKind));
      }

      return newSubscript;
    }

    Expr *buildSubscriptHelper(Expr *base, Expr *index,
                               ArrayRef<Identifier> argLabels,
                               const SelectedOverload &selected,
                               bool hasTrailingClosure,
                               ConstraintLocatorBuilder locator,
                               bool isImplicit, AccessSemantics semantics) {
      auto choice = selected.choice;
      auto &ctx = cs.getASTContext();

      // Apply a key path if we have one.
      if (choice.getKind() == OverloadChoiceKind::KeyPathApplication) {
        auto applicationTy =
            simplifyType(selected.openedType)->castTo<FunctionType>();

        index = cs.coerceToRValue(index);
        // The index argument should be (keyPath: KeyPath<Root, Value>).
        // Dig the key path expression out of the arguments.
        auto indexKP = cast<TupleExpr>(index)->getElement(0);
        auto keyPathExprTy = cs.getType(indexKP);
        auto keyPathTy = applicationTy->getParams().front().getOldType();

        Type valueTy;
        Type baseTy;
        bool resultIsLValue;
        
        if (auto nom = keyPathTy->getAs<NominalType>()) {
          // AnyKeyPath is <T> rvalue T -> rvalue Any?
          if (nom->isAnyKeyPath()) {
            valueTy = ProtocolCompositionType::get(cs.getASTContext(), {},
                                                  /*explicit anyobject*/ false);
            valueTy = OptionalType::get(valueTy);
            resultIsLValue = false;
            base = cs.coerceToRValue(base);
            baseTy = cs.getType(base);
            // We don't really want to attempt AnyKeyPath application
            // if we know a more specific key path type is being applied.
            if (!keyPathTy->isEqual(keyPathExprTy)) {
              ctx.Diags
                  .diagnose(base->getLoc(),
                            diag::expr_smart_keypath_application_type_mismatch,
                            keyPathExprTy, baseTy)
                  .highlight(index->getSourceRange());
            }
          } else {
            llvm_unreachable("unknown key path class!");
          }
        } else {
          auto keyPathBGT = keyPathTy->castTo<BoundGenericType>();
          baseTy = keyPathBGT->getGenericArgs()[0];

          // Coerce the index to the key path's type
          indexKP = coerceToType(indexKP, keyPathTy, locator);

          // Coerce the base to the key path's expected base type.
          if (!baseTy->isEqual(cs.getType(base)->getRValueType()))
            base = coerceToType(base, baseTy, locator);

          if (keyPathBGT->isPartialKeyPath()) {
            // PartialKeyPath<T> is rvalue T -> rvalue Any
            valueTy = ProtocolCompositionType::get(cs.getASTContext(), {},
                                                 /*explicit anyobject*/ false);
            resultIsLValue = false;
            base = cs.coerceToRValue(base);
          } else {
            // *KeyPath<T, U> is T -> U, with rvalueness based on mutability
            // of base and keypath
            valueTy = keyPathBGT->getGenericArgs()[1];
        
            // The result may be an lvalue based on the base and key path kind.
            if (keyPathBGT->isKeyPath()) {
              resultIsLValue = false;
              base = cs.coerceToRValue(base);
            } else if (keyPathBGT->isWritableKeyPath()) {
              resultIsLValue = cs.getType(base)->hasLValueType();
            } else if (keyPathBGT->isReferenceWritableKeyPath()) {
              resultIsLValue = true;
              base = cs.coerceToRValue(base);
            } else {
              llvm_unreachable("unknown key path class!");
            }
          }
        }
        if (resultIsLValue)
          valueTy = LValueType::get(valueTy);
        
        auto keyPathAp = new (cs.getASTContext())
           KeyPathApplicationExpr(base, index->getStartLoc(), indexKP,
                                  index->getEndLoc(), valueTy,
                                  base->isImplicit() && index->isImplicit());
        cs.setType(keyPathAp, valueTy);
        return keyPathAp;
      }
      
      auto subscript = cast<SubscriptDecl>(choice.getDecl());

      auto baseTy = cs.getType(base)->getRValueType();
      
      bool baseIsInstance = true;
      if (auto baseMeta = baseTy->getAs<AnyMetatypeType>()) {
        baseIsInstance = false;
        baseTy = baseMeta->getInstanceType();
      }

      // Check whether the base is 'super'.
      bool isSuper = base->isSuperExpr();

      // Use the correct locator kind based on the subscript kind.
      auto locatorKind = ConstraintLocator::SubscriptMember;
      if (choice.getKind() == OverloadChoiceKind::DynamicMemberLookup)
        locatorKind = ConstraintLocator::Member;

      if (choice.getKind() == OverloadChoiceKind::KeyPathDynamicMemberLookup) {
        locatorKind = isExpr<SubscriptExpr>(locator.getAnchor())
                          ? ConstraintLocator::SubscriptMember
                          : ConstraintLocator::Member;
      }

      // If we opened up an existential when performing the subscript, open
      // the base accordingly.
      auto memberLoc = locator.withPathElement(locatorKind);
      auto knownOpened = solution.OpenedExistentialTypes.find(
          cs.getConstraintLocator(memberLoc));
      if (knownOpened != solution.OpenedExistentialTypes.end()) {
        base = openExistentialReference(base, knownOpened->second, subscript);
        baseTy = knownOpened->second;
      }

      // Compute the concrete reference to the subscript.
      auto subscriptRef = resolveConcreteDeclRef(subscript, memberLoc);

      // Coerce the index argument.
      auto openedFullFnType = simplifyType(selected.openedFullType)
                                  ->castTo<FunctionType>();
      auto fullSubscriptTy = openedFullFnType->getResult()
                                  ->castTo<FunctionType>();
      auto &appliedWrappers = solution.appliedPropertyWrappers[memberLoc.getAnchor()];
      index = coerceCallArguments(index, fullSubscriptTy, subscriptRef, nullptr,
                                  argLabels,
                                  locator.withPathElement(
                                    ConstraintLocator::ApplyArgument),
                                  appliedWrappers);
      if (!index)
        return nullptr;

      auto getType = [&](Expr *E) -> Type { return cs.getType(E); };

      // Handle dynamic lookup.
      if (choice.getKind() == OverloadChoiceKind::DeclViaDynamic ||
          subscript->getAttrs().hasAttribute<OptionalAttr>()) {
        base = coerceSelfArgumentToType(base, baseTy, subscript, locator);
        if (!base)
          return nullptr;

        // TODO: diagnose if semantics != AccessSemantics::Ordinary?
        auto subscriptExpr = DynamicSubscriptExpr::create(
            ctx, base, index, subscriptRef, isImplicit, getType);
        auto resultTy = simplifyType(selected.openedType)
                            ->castTo<FunctionType>()
                            ->getResult();
        assert(!selected.openedFullType->hasOpenedExistential()
               && "open existential archetype in AnyObject subscript type?");
        cs.setType(subscriptExpr, resultTy);
        Expr *result = subscriptExpr;
        closeExistential(result, locator);
        return result;
      }

      // Convert the base.
      auto openedBaseType =
          getBaseType(openedFullFnType, /*wantsRValue*/ false);
      auto containerTy = solution.simplifyType(openedBaseType);
      
      if (baseIsInstance) {
        base = coerceSelfArgumentToType(
          base, containerTy, subscript,
          locator.withPathElement(ConstraintLocator::MemberRefBase));
      } else {
        base = coerceToType(base,
                            MetatypeType::get(containerTy),
                            locator.withPathElement(
                              ConstraintLocator::MemberRefBase));
        
        if (!base)
          return nullptr;
        
        base = cs.coerceToRValue(base);
      }
      if (!base)
        return nullptr;

      const auto hasDynamicSelf =
          subscript->getElementInterfaceType()->hasDynamicSelfType();

      // Form the subscript expression.
      auto subscriptExpr = SubscriptExpr::create(
          ctx, base, index, subscriptRef, isImplicit, semantics, getType);
      cs.setType(subscriptExpr, fullSubscriptTy->getResult());
      subscriptExpr->setIsSuper(isSuper);
      cs.setType(subscriptExpr,
                 hasDynamicSelf
                     ? fullSubscriptTy->getResult()->replaceCovariantResultType(
                           containerTy, 0)
                     : fullSubscriptTy->getResult());

      Expr *result = subscriptExpr;
      closeExistential(result, locator);

      // If the element is of dynamic 'Self' type, wrap an implicit conversion
      // around the resulting expression, with the destination type having
      // 'Self' swapped for the appropriate replacement type -- usually the
      // base object type.
      if (hasDynamicSelf) {
        const auto conversionTy = simplifyType(
            selected.openedType->castTo<FunctionType>()->getResult());

        if (!containerTy->isEqual(conversionTy)) {
          result = cs.cacheType(
              new (ctx) CovariantReturnConversionExpr(result, conversionTy));
        }
      }

      return result;
    }

    /// Build a new reference to another constructor.
    Expr *buildOtherConstructorRef(Type openedFullType,
                                   ConcreteDeclRef ref, Expr *base,
                                   DeclNameLoc loc,
                                   ConstraintLocatorBuilder locator,
                                   bool implicit) {
      auto &ctx = cs.getASTContext();

      // The constructor was opened with the allocating type, not the
      // initializer type. Map the former into the latter.
      auto resultTy = solution.simplifyType(openedFullType);

      auto selfTy = getBaseType(resultTy->castTo<FunctionType>());

      // Also replace the result type with the base type, so that calls
      // to constructors defined in a superclass will know to cast the
      // result to the derived type.
      resultTy = resultTy->replaceCovariantResultType(selfTy, 2);

      ParameterTypeFlags flags;
      if (!selfTy->hasReferenceSemantics())
        flags = flags.withInOut(true);

      auto selfParam = AnyFunctionType::Param(selfTy, Identifier(), flags);
      resultTy = FunctionType::get({selfParam},
                                   resultTy->castTo<FunctionType>()->getResult(),
                                   resultTy->castTo<FunctionType>()->getExtInfo());

      // Build the constructor reference.
      Expr *ctorRef = cs.cacheType(
          new (ctx) OtherConstructorDeclRefExpr(ref, loc, implicit, resultTy));

      // Wrap in covariant `Self` return if needed.
      if (selfTy->hasReferenceSemantics()) {
        auto covariantTy = resultTy->replaceCovariantResultType(
          cs.getType(base)->getWithoutSpecifierType(), 2);
        if (!covariantTy->isEqual(resultTy))
          ctorRef = cs.cacheType(
               new (ctx) CovariantFunctionConversionExpr(ctorRef, covariantTy));
      }

      return ctorRef;
    }

    /// Build an implicit argument for keypath based dynamic lookup,
    /// which consists of KeyPath expression and a single component.
    ///
    /// \param keyPathTy The type of the keypath argument.
    /// \param dotLoc The location of the '.' preceding member name.
    /// \param memberLoc The locator to be associated with new argument.
    Expr *buildKeyPathDynamicMemberIndexExpr(BoundGenericType *keyPathTy,
                                             SourceLoc dotLoc,
                                             ConstraintLocator *memberLoc) {
      auto &ctx = cs.getASTContext();
      auto *anchor = getAsExpr(memberLoc->getAnchor());

      SmallVector<KeyPathExpr::Component, 2> components;

      // Let's create a KeyPath expression and fill in "parsed path"
      // after component is built.
      auto *keyPath = new (ctx) KeyPathExpr(/*backslashLoc=*/dotLoc,
                                            /*parsedRoot=*/nullptr,
                                            /*parsedPath=*/anchor,
                                            /*hasLeadingDot=*/false,
                                            /*isImplicit=*/true);
      // Type of the keypath expression we are forming is known
      // in advance, so let's set it right away.
      keyPath->setType(keyPathTy);
      cs.cacheType(keyPath);

      auto *componentLoc = cs.getConstraintLocator(
          memberLoc,
          LocatorPathElt::KeyPathDynamicMember(keyPathTy->getAnyNominal()));
      auto overload = solution.getOverloadChoice(componentLoc);

      // Looks like there is a chain of implicit `subscript(dynamicMember:)`
      // calls necessary to resolve a member reference.
      switch (overload.choice.getKind()) {
      case OverloadChoiceKind::DynamicMemberLookup:
      case OverloadChoiceKind::KeyPathDynamicMemberLookup: {
        buildKeyPathSubscriptComponent(overload, dotLoc, /*indexExpr=*/nullptr,
                                       ctx.Id_dynamicMember, componentLoc,
                                       components);
        keyPath->resolveComponents(ctx, components);
        cs.cacheExprTypes(keyPath);
        return keyPath;
      }

      default:
        break;
      }

      // We can't reuse existing expression because type-check
      // based diagnostics could hold the reference to original AST.
      Expr *componentExpr = nullptr;
      auto *dotExpr = new (ctx) KeyPathDotExpr(dotLoc);

      // Determines whether this index is built to be used for
      // one of the existing keypath components e.g. `\Lens<[Int]>.count`
      // instead of a regular expression e.g. `lens[0]`.
      bool forKeyPathComponent = false;
      // Looks like keypath dynamic member lookup was used inside
      // of a keypath expression e.g. `\Lens<[Int]>.count` where
      // `count` is referenced using dynamic lookup.
      if (auto *KPE = dyn_cast<KeyPathExpr>(anchor)) {
        auto kpElt = memberLoc->findFirst<LocatorPathElt::KeyPathComponent>();
        assert(kpElt && "no keypath component node");
        auto &origComponent = KPE->getComponents()[kpElt->getIndex()];

        using ComponentKind = KeyPathExpr::Component::Kind;
        if (origComponent.getKind() == ComponentKind::UnresolvedProperty) {
          anchor = new (ctx) UnresolvedDotExpr(
              dotExpr, dotLoc, origComponent.getUnresolvedDeclName(),
              DeclNameLoc(origComponent.getLoc()),
              /*Implicit=*/true);
        } else if (origComponent.getKind() ==
                   ComponentKind::UnresolvedSubscript) {
          anchor = SubscriptExpr::create(
              ctx, dotExpr, origComponent.getIndexExpr(), ConcreteDeclRef(),
              /*implicit=*/true, AccessSemantics::Ordinary,
              [&](Expr *expr) { return simplifyType(cs.getType(expr)); });
        } else {
          return nullptr;
        }

        anchor->setType(simplifyType(overload.openedType));
        cs.cacheType(anchor);
        forKeyPathComponent = true;
      }

      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
        componentExpr =
            forKeyPathComponent
                ? UDE
                : new (ctx) UnresolvedDotExpr(dotExpr, dotLoc, UDE->getName(),
                                              UDE->getNameLoc(),
                                              /*Implicit=*/true);

        buildKeyPathPropertyComponent(overload, UDE->getLoc(), componentLoc,
                                      components);
      } else if (auto *SE = dyn_cast<SubscriptExpr>(anchor)) {
        componentExpr = SE;
        // If this is not for a keypath component, we have to copy
        // original subscript expression because expression based
        // diagnostics might have a reference to it, so it couldn't
        // be modified.
        if (!forKeyPathComponent) {
          SmallVector<Expr *, 4> arguments;
          if (auto *TE = dyn_cast<TupleExpr>(SE->getIndex())) {
            auto args = TE->getElements();
            arguments.append(args.begin(), args.end());
          } else {
            arguments.push_back(SE->getIndex()->getSemanticsProvidingExpr());
          }

          SmallVector<TrailingClosure, 2> trailingClosures;
          if (SE->hasTrailingClosure()) {
            auto *closure = arguments.back();
            trailingClosures.push_back({closure});
          }

          componentExpr = SubscriptExpr::create(
              ctx, dotExpr, SE->getStartLoc(), arguments,
              SE->getArgumentLabels(), SE->getArgumentLabelLocs(),
              SE->getEndLoc(), trailingClosures,
              SE->hasDecl() ? SE->getDecl() : ConcreteDeclRef(),
              /*implicit=*/true, SE->getAccessSemantics());
        }

        buildKeyPathSubscriptComponent(overload, SE->getLoc(), SE->getIndex(),
                                       SE->getArgumentLabels(), componentLoc,
                                       components);
      } else {
        return nullptr;
      }

      assert(componentExpr);
      Type ty = simplifyType(cs.getType(anchor));
      componentExpr->setType(ty);
      cs.cacheType(componentExpr);

      keyPath->setParsedPath(componentExpr);
      keyPath->resolveComponents(ctx, components);
      cs.cacheExprTypes(keyPath);

      // See whether there's an equivalent ObjC key path string we can produce
      // for interop purposes.
      checkAndSetObjCKeyPathString(keyPath);

      return keyPath;
    }

    /// Bridge the given value (which is an error type) to NSError.
    Expr *bridgeErrorToObjectiveC(Expr *value) {
      auto &ctx = cs.getASTContext();

      auto nsErrorType = ctx.getNSErrorType();
      assert(nsErrorType && "Missing NSError?");

      auto result = new (ctx) BridgeToObjCExpr(value, nsErrorType);
      return cs.cacheType(result);
    }

    /// Bridge the given value to its corresponding Objective-C object
    /// type.
    ///
    /// This routine should only be used for bridging value types.
    ///
    /// \param value The value to be bridged.
    Expr *bridgeToObjectiveC(Expr *value, Type objcType) {
      auto result = new (cs.getASTContext()) BridgeToObjCExpr(value, objcType);
      return cs.cacheType(result);
    }

    /// Bridge the given object from Objective-C to its value type.
    ///
    /// This routine should only be used for bridging value types.
    ///
    /// \param object The object, whose type should already be of the type
    /// that the value type bridges through.
    ///
    /// \param valueType The value type to which we are bridging.
    ///
    /// \param conditional Whether the bridging should be conditional. If false,
    /// uses forced bridging.
    ///
    /// \returns a value of type \c valueType (optional if \c conditional) that
    /// stores the bridged result or (when \c conditional) an empty optional if
    /// conditional bridging fails.
    Expr *bridgeFromObjectiveC(Expr *object, Type valueType, bool conditional) {
      auto &ctx = cs.getASTContext();

      if (!conditional) {
        auto result = new (ctx) BridgeFromObjCExpr(object, valueType);
        return cs.cacheType(result);
      }

      // Find the _BridgedToObjectiveC protocol.
      auto bridgedProto =
          ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable);

      // Try to find the conformance of the value type to _BridgedToObjectiveC.
      auto bridgedToObjectiveCConformance
        = TypeChecker::conformsToProtocol(valueType,
                                          bridgedProto,
                                          cs.DC->getParentModule());

      FuncDecl *fn = nullptr;

      if (bridgedToObjectiveCConformance) {
        assert(bridgedToObjectiveCConformance.getConditionalRequirements()
                   .empty() &&
               "cannot conditionally conform to _BridgedToObjectiveC");
        // The conformance to _BridgedToObjectiveC is statically known.
        // Retrieve the bridging operation to be used if a static conformance
        // to _BridgedToObjectiveC can be proven.
        fn = conditional ? ctx.getConditionallyBridgeFromObjectiveCBridgeable()
                         : ctx.getForceBridgeFromObjectiveCBridgeable();
      } else {
        // Retrieve the bridging operation to be used if a static conformance
        // to _BridgedToObjectiveC cannot be proven.
        fn = conditional ? ctx.getConditionallyBridgeFromObjectiveC()
                         : ctx.getForceBridgeFromObjectiveC();
      }

      if (!fn) {
        ctx.Diags.diagnose(object->getLoc(), diag::missing_bridging_function,
                           conditional);
        return nullptr;
      }

      // Form a reference to the function. The bridging operations are generic,
      // so we need to form substitutions and compute the resulting type.
      auto genericSig = fn->getGenericSignature();

      auto subMap = SubstitutionMap::get(
          genericSig,
          [&](SubstitutableType *type) -> Type {
            assert(type->isEqual(genericSig->getGenericParams()[0]));
            return valueType;
          },
          [&](CanType origType, Type replacementType,
              ProtocolDecl *protoType) -> ProtocolConformanceRef {
            assert(bridgedToObjectiveCConformance);
            return bridgedToObjectiveCConformance;
          });

      ConcreteDeclRef fnSpecRef(fn, subMap);

      auto resultType = OptionalType::get(valueType);

      auto result = new (ctx)
          ConditionalBridgeFromObjCExpr(object, resultType, fnSpecRef);
      return cs.cacheType(result);
    }

    /// Bridge the given object from Objective-C to its value type.
    ///
    /// This routine should only be used for bridging value types.
    ///
    /// \param object The object, whose type should already be of the type
    /// that the value type bridges through.
    ///
    /// \param valueType The value type to which we are bridging.
    ///
    /// \returns a value of type \c valueType that stores the bridged result.
    Expr *forceBridgeFromObjectiveC(Expr *object, Type valueType) {
      return bridgeFromObjectiveC(object, valueType, false);
    }
    
  public:
    ExprRewriter(ConstraintSystem &cs, Solution &solution,
                 Optional<SolutionApplicationTarget> target,
                 bool suppressDiagnostics)
        : cs(cs), dc(cs.DC), solution(solution), target(target),
          SuppressDiagnostics(suppressDiagnostics) {}

    ConstraintSystem &getConstraintSystem() const { return cs; }

    /// Simplify the expression type and return the expression.
    ///
    /// This routine is used for 'simple' expressions that only need their
    /// types simplified, with no further computation.
    Expr *simplifyExprType(Expr *expr) {
      auto toType = simplifyType(cs.getType(expr));
      cs.setType(expr, toType);
      return expr;
    }

    Expr *visitErrorExpr(ErrorExpr *expr) {
      // Do nothing with error expressions.
      return expr;
    }

    Expr *visitCodeCompletionExpr(CodeCompletionExpr *expr) {
      // Do nothing with code completion expressions.
      auto toType = simplifyType(cs.getType(expr));
      cs.setType(expr, toType);
      return expr;
    }

    Expr *handleIntegerLiteralExpr(LiteralExpr *expr) {
      // If the literal has been assigned a builtin integer type,
      // don't mess with it.
      if (cs.getType(expr)->is<AnyBuiltinIntegerType>())
        return expr;

      auto &ctx = cs.getASTContext();
      ProtocolDecl *protocol = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByIntegerLiteral);
      ProtocolDecl *builtinProtocol = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByBuiltinIntegerLiteral);

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      auto type = simplifyType(cs.getType(expr));
      if (auto defaultType = TypeChecker::getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }
      if (auto floatProtocol = TypeChecker::getProtocol(
              cs.getASTContext(), expr->getLoc(),
              KnownProtocolKind::ExpressibleByFloatLiteral)) {
        if (auto defaultFloatType =
                TypeChecker::getDefaultType(floatProtocol, dc)) {
          if (defaultFloatType->isEqual(type))
            type = defaultFloatType;
        }
      }

      DeclName initName(ctx, DeclBaseName::createConstructor(),
                        {ctx.Id_integerLiteral});
      DeclName builtinInitName(ctx, DeclBaseName::createConstructor(),
                               {ctx.Id_builtinIntegerLiteral});

      auto *result = convertLiteralInPlace(
          expr, type, protocol, ctx.Id_IntegerLiteralType, initName,
          builtinProtocol, builtinInitName, diag::integer_literal_broken_proto,
          diag::builtin_integer_literal_broken_proto);
      if (result) {
        // TODO: It seems that callers expect this to have types assigned...
        result->setType(cs.getType(result));
      }
      return result;
    }
    
    Expr *visitNilLiteralExpr(NilLiteralExpr *expr) {
      auto type = simplifyType(cs.getType(expr));

      // By far the most common 'nil' literal is for Optional<T>.none.
      // We don't have to look up the witness in this case since SILGen
      // knows how to lower it directly.
      if (auto objectType = type->getOptionalObjectType()) {
        cs.setType(expr, type);
        return expr;
      }

      auto &ctx = cs.getASTContext();
      auto *protocol = TypeChecker::getProtocol(
          ctx, expr->getLoc(), KnownProtocolKind::ExpressibleByNilLiteral);

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      if (auto defaultType = TypeChecker::getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }

      DeclName initName(ctx, DeclBaseName::createConstructor(),
                        {ctx.Id_nilLiteral});
      return convertLiteralInPlace(expr, type, protocol,
                                   Identifier(), initName,
                                   nullptr,
                                   Identifier(),
                                   diag::nil_literal_broken_proto,
                                   diag::nil_literal_broken_proto);
    }

    
    Expr *visitIntegerLiteralExpr(IntegerLiteralExpr *expr) {
      return handleIntegerLiteralExpr(expr);
    }

    Expr *visitFloatLiteralExpr(FloatLiteralExpr *expr) {
      // If the literal has been assigned a builtin float type,
      // don't mess with it.
      if (cs.getType(expr)->is<BuiltinFloatType>())
        return expr;

      auto &ctx = cs.getASTContext();
      ProtocolDecl *protocol = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByFloatLiteral);
      ProtocolDecl *builtinProtocol = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByBuiltinFloatLiteral);

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      auto type = simplifyType(cs.getType(expr));
      if (auto defaultType = TypeChecker::getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }

      // Get the _MaxBuiltinFloatType decl, or look for it if it's not cached.
      auto maxFloatTypeDecl = ctx.get_MaxBuiltinFloatTypeDecl();
      // Presence of this declaration has been validated in CSGen.
      assert(maxFloatTypeDecl);

      auto maxType = maxFloatTypeDecl->getUnderlyingType();

      DeclName initName(ctx, DeclBaseName::createConstructor(),
                        {ctx.Id_floatLiteral});
      DeclName builtinInitName(ctx, DeclBaseName::createConstructor(),
                               {ctx.Id_builtinFloatLiteral});

      expr->setBuiltinType(maxType);
      return convertLiteralInPlace(
          expr, type, protocol, ctx.Id_FloatLiteralType, initName,
          builtinProtocol, builtinInitName, diag::float_literal_broken_proto,
          diag::builtin_float_literal_broken_proto);
    }

    Expr *visitBooleanLiteralExpr(BooleanLiteralExpr *expr) {
      if (cs.getType(expr) && cs.getType(expr)->is<BuiltinIntegerType>())
        return expr;

      auto &ctx = cs.getASTContext();
      ProtocolDecl *protocol = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByBooleanLiteral);
      ProtocolDecl *builtinProtocol = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByBuiltinBooleanLiteral);
      if (!protocol || !builtinProtocol)
        return nullptr;

      auto type = simplifyType(cs.getType(expr));
      DeclName initName(ctx, DeclBaseName::createConstructor(),
                        {ctx.Id_booleanLiteral});
      DeclName builtinInitName(ctx, DeclBaseName::createConstructor(),
                               {ctx.Id_builtinBooleanLiteral});
      return convertLiteralInPlace(
          expr, type, protocol, ctx.Id_BooleanLiteralType, initName,
          builtinProtocol, builtinInitName, diag::boolean_literal_broken_proto,
          diag::builtin_boolean_literal_broken_proto);
    }

    Expr *handleStringLiteralExpr(LiteralExpr *expr) {
      auto stringLiteral = dyn_cast<StringLiteralExpr>(expr);
      auto magicLiteral = dyn_cast<MagicIdentifierLiteralExpr>(expr);
      assert(bool(stringLiteral) != bool(magicLiteral) &&
             "literal must be either a string literal or a magic literal");

      auto type = simplifyType(cs.getType(expr));
      auto &ctx = cs.getASTContext();

      bool isStringLiteral = true;
      bool isGraphemeClusterLiteral = false;
      ProtocolDecl *protocol = TypeChecker::getProtocol(
          ctx, expr->getLoc(), KnownProtocolKind::ExpressibleByStringLiteral);

      if (!TypeChecker::conformsToProtocol(type, protocol, cs.DC->getParentModule())) {
        // If the type does not conform to ExpressibleByStringLiteral, it should
        // be ExpressibleByExtendedGraphemeClusterLiteral.
        protocol = TypeChecker::getProtocol(
            cs.getASTContext(), expr->getLoc(),
            KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral);
        isStringLiteral = false;
        isGraphemeClusterLiteral = true;
      }
      if (!TypeChecker::conformsToProtocol(type, protocol, cs.DC->getParentModule())) {
        // ... or it should be ExpressibleByUnicodeScalarLiteral.
        protocol = TypeChecker::getProtocol(
            cs.getASTContext(), expr->getLoc(),
            KnownProtocolKind::ExpressibleByUnicodeScalarLiteral);
        isStringLiteral = false;
        isGraphemeClusterLiteral = false;
      }

      // For type-sugar reasons, prefer the spelling of the default literal
      // type.
      if (auto defaultType = TypeChecker::getDefaultType(protocol, dc)) {
        if (defaultType->isEqual(type))
          type = defaultType;
      }

      ProtocolDecl *builtinProtocol;
      Identifier literalType;
      DeclName literalFuncName;
      DeclName builtinLiteralFuncName;
      Diag<> brokenProtocolDiag;
      Diag<> brokenBuiltinProtocolDiag;

      if (isStringLiteral) {
        literalType = ctx.Id_StringLiteralType;

        literalFuncName = DeclName(ctx, DeclBaseName::createConstructor(),
                                   {ctx.Id_stringLiteral});

        builtinProtocol = TypeChecker::getProtocol(
            cs.getASTContext(), expr->getLoc(),
            KnownProtocolKind::ExpressibleByBuiltinStringLiteral);
        builtinLiteralFuncName =
            DeclName(ctx, DeclBaseName::createConstructor(),
                     {ctx.Id_builtinStringLiteral,
                      ctx.getIdentifier("utf8CodeUnitCount"),
                      ctx.getIdentifier("isASCII")});
        if (stringLiteral)
          stringLiteral->setEncoding(StringLiteralExpr::UTF8);
        else
          magicLiteral->setStringEncoding(StringLiteralExpr::UTF8);

        brokenProtocolDiag = diag::string_literal_broken_proto;
        brokenBuiltinProtocolDiag = diag::builtin_string_literal_broken_proto;
      } else if (isGraphemeClusterLiteral) {
        literalType = ctx.Id_ExtendedGraphemeClusterLiteralType;
        literalFuncName = DeclName(ctx, DeclBaseName::createConstructor(),
                                   {ctx.Id_extendedGraphemeClusterLiteral});
        builtinLiteralFuncName =
            DeclName(ctx, DeclBaseName::createConstructor(),
                     {ctx.Id_builtinExtendedGraphemeClusterLiteral,
                      ctx.getIdentifier("utf8CodeUnitCount"),
                      ctx.getIdentifier("isASCII")});

        builtinProtocol = TypeChecker::getProtocol(
            cs.getASTContext(), expr->getLoc(),
            KnownProtocolKind::
                ExpressibleByBuiltinExtendedGraphemeClusterLiteral);
        brokenProtocolDiag =
            diag::extended_grapheme_cluster_literal_broken_proto;
        brokenBuiltinProtocolDiag =
            diag::builtin_extended_grapheme_cluster_literal_broken_proto;
      } else {
        // Otherwise, we should have just one Unicode scalar.
        literalType = ctx.Id_UnicodeScalarLiteralType;

        literalFuncName = DeclName(ctx, DeclBaseName::createConstructor(),
                                   {ctx.Id_unicodeScalarLiteral});
        builtinLiteralFuncName =
            DeclName(ctx, DeclBaseName::createConstructor(),
                     {ctx.Id_builtinUnicodeScalarLiteral});

        builtinProtocol = TypeChecker::getProtocol(
            cs.getASTContext(), expr->getLoc(),
            KnownProtocolKind::ExpressibleByBuiltinUnicodeScalarLiteral);

        brokenProtocolDiag = diag::unicode_scalar_literal_broken_proto;
        brokenBuiltinProtocolDiag =
            diag::builtin_unicode_scalar_literal_broken_proto;

        stringLiteral->setEncoding(StringLiteralExpr::OneUnicodeScalar);
      }

      return convertLiteralInPlace(expr,
                                   type,
                                   protocol,
                                   literalType,
                                   literalFuncName,
                                   builtinProtocol,
                                   builtinLiteralFuncName,
                                   brokenProtocolDiag,
                                   brokenBuiltinProtocolDiag);
    }
    
    Expr *visitStringLiteralExpr(StringLiteralExpr *expr) {
      return handleStringLiteralExpr(expr);
    }

    Expr *
    visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *expr) {
      // Figure out the string type we're converting to.
      auto openedType = cs.getType(expr);
      auto type = simplifyType(openedType);
      cs.setType(expr, type);

      auto &ctx = cs.getASTContext();
      auto loc = expr->getStartLoc();

      auto fetchProtocolInitWitness =
          [&](KnownProtocolKind protocolKind, Type type,
              ArrayRef<Identifier> argLabels) -> ConcreteDeclRef {
        auto proto = TypeChecker::getProtocol(ctx, loc, protocolKind);
        assert(proto && "Missing string interpolation protocol?");

        auto conformance =
          TypeChecker::conformsToProtocol(type, proto, cs.DC->getParentModule());
        assert(conformance && "string interpolation type conforms to protocol");

        DeclName constrName(ctx, DeclBaseName::createConstructor(), argLabels);

        ConcreteDeclRef witness =
            conformance.getWitnessByName(type->getRValueType(), constrName);
        if (!witness || !isa<AbstractFunctionDecl>(witness.getDecl()))
          return nullptr;
        return witness;
      };

      auto *interpolationProto = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByStringInterpolation);
      auto associatedTypeDecl =
          interpolationProto->getAssociatedType(ctx.Id_StringInterpolation);
      if (associatedTypeDecl == nullptr) {
        ctx.Diags.diagnose(expr->getStartLoc(),
                           diag::interpolation_broken_proto);
        return nullptr;
      }
      auto interpolationType =
        simplifyType(DependentMemberType::get(openedType, associatedTypeDecl));

      // Fetch needed witnesses.
      ConcreteDeclRef builderInit = fetchProtocolInitWitness(
          KnownProtocolKind::StringInterpolationProtocol, interpolationType,
          {ctx.Id_literalCapacity, ctx.Id_interpolationCount});
      if (!builderInit) return nullptr;
      expr->setBuilderInit(builderInit);

      ConcreteDeclRef resultInit = fetchProtocolInitWitness(
          KnownProtocolKind::ExpressibleByStringInterpolation, type,
          {ctx.Id_stringInterpolation});
      if (!resultInit) return nullptr;
      expr->setInitializer(resultInit);

      // Make the integer literals for the parameters.
      auto buildExprFromUnsigned = [&](unsigned value) {
        LiteralExpr *expr = IntegerLiteralExpr::createFromUnsigned(ctx, value);
        cs.setType(expr, ctx.getIntType());
        return handleIntegerLiteralExpr(expr);
      };

      expr->setLiteralCapacityExpr(
          buildExprFromUnsigned(expr->getLiteralCapacity()));
      expr->setInterpolationCountExpr(
          buildExprFromUnsigned(expr->getInterpolationCount()));

      // This OpaqueValueExpr represents the result of builderInit above in
      // silgen.
      OpaqueValueExpr *interpolationExpr =
          new (ctx) OpaqueValueExpr(expr->getSourceRange(), interpolationType);
      cs.setType(interpolationExpr, interpolationType);
      expr->setInterpolationExpr(interpolationExpr);

      auto appendingExpr = expr->getAppendingExpr();
      appendingExpr->setSubExpr(interpolationExpr);

      return expr;
    }
    
    Expr *visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *expr) {
      switch (expr->getKind()) {
#define MAGIC_STRING_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
      case MagicIdentifierLiteralExpr::NAME: \
        return handleStringLiteralExpr(expr);
#define MAGIC_INT_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
      case MagicIdentifierLiteralExpr::NAME: \
        return handleIntegerLiteralExpr(expr);
#define MAGIC_POINTER_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
      case MagicIdentifierLiteralExpr::NAME: \
        return expr;
#include "swift/AST/MagicIdentifierKinds.def"
      }


      llvm_unreachable("Unhandled MagicIdentifierLiteralExpr in switch.");
    }

    Expr *visitObjectLiteralExpr(ObjectLiteralExpr *expr) {
      if (cs.getType(expr) && !cs.getType(expr)->hasTypeVariable())
        return expr;

      // Figure out the type we're converting to.
      auto openedType = cs.getType(expr);
      auto type = simplifyType(openedType);
      cs.setType(expr, type);

      if (type->is<UnresolvedType>())
        return expr;

      auto &ctx = cs.getASTContext();

      Type conformingType = type;
      if (auto baseType = conformingType->getOptionalObjectType()) {
        // The type may be optional due to a failable initializer in the
        // protocol.
        conformingType = baseType;
      }

      // Find the appropriate object literal protocol.
      auto proto = TypeChecker::getLiteralProtocol(ctx, expr);
      assert(proto && "Missing object literal protocol?");
      auto conformance =
        TypeChecker::conformsToProtocol(conformingType, proto,
                                        cs.DC->getParentModule());
      assert(conformance && "object literal type conforms to protocol");

      auto constrName = TypeChecker::getObjectLiteralConstructorName(ctx, expr);

      ConcreteDeclRef witness = conformance.getWitnessByName(
          conformingType->getRValueType(), constrName);

      auto selectedOverload = solution.getOverloadChoiceIfAvailable(
          cs.getConstraintLocator(expr, ConstraintLocator::ConstructorMember));

      if (!selectedOverload)
        return nullptr;

      auto fnType =
          simplifyType(selectedOverload->openedType)->castTo<FunctionType>();

      auto newArg = coerceCallArguments(
          expr->getArg(), fnType, witness,
          /*applyExpr=*/nullptr, expr->getArgumentLabels(),
          cs.getConstraintLocator(expr, ConstraintLocator::ApplyArgument),
          /*appliedPropertyWrappers=*/{});

      expr->setInitializer(witness);
      expr->setArg(newArg);
      return expr;
    }

    // Add a forced unwrap of an expression which either has type Optional<T>
    // or is a function that returns an Optional<T>. The latter turns into a
    // conversion expression that we will hoist above the ApplyExpr
    // that needs to be forced during the process of rewriting the expression.
    //
    // forForcedOptional is used to indicate that we will further need
    // to hoist this result above an explicit force of an optional that is
    // in place for something like an @optional protocol member from
    // Objective C that we might otherwise mistake for the thing we mean to
    // force here.
    Expr *forceUnwrapResult(Expr *expr, bool forForcedOptional =false) {
      auto ty = simplifyType(cs.getType(expr));

      if (forForcedOptional)
        ty = ty->getOptionalObjectType();

      if (auto *fnTy = ty->getAs<AnyFunctionType>()) {
        auto underlyingType = cs.replaceFinalResultTypeWithUnderlying(fnTy);

        auto &ctx = cs.getASTContext();
        return cs.cacheType(new (ctx) ImplicitlyUnwrappedFunctionConversionExpr(
            expr, underlyingType));
      } else {
        return coerceImplicitlyUnwrappedOptionalToValue(
            expr, ty->getWithoutSpecifierType()->getOptionalObjectType());
      }
    }

    bool shouldForceUnwrapResult(OverloadChoice choice,
                                 ConstraintLocatorBuilder locator) {
      if (!choice.isImplicitlyUnwrappedValueOrReturnValue())
        return false;

      auto *choiceLocator = cs.getConstraintLocator(locator.withPathElement(
          ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice));

      return solution.getDisjunctionChoice(choiceLocator);
    }

    Expr *forceUnwrapIfExpected(Expr *expr, OverloadChoice choice,
                                ConstraintLocatorBuilder locator,
                                bool forForcedOptional = false) {
      if (!shouldForceUnwrapResult(choice, locator))
        return expr;

      // Force the expression if required for the solution.
      return forceUnwrapResult(expr, forForcedOptional);
    }

    Expr *visitDeclRefExpr(DeclRefExpr *expr) {
      auto locator = cs.getConstraintLocator(expr);

      // Find the overload choice used for this declaration reference.
      auto selected = solution.getOverloadChoice(locator);
      return buildDeclRef(selected, expr->getNameLoc(), locator,
                          expr->isImplicit());
    }

    Expr *visitSuperRefExpr(SuperRefExpr *expr) {
      simplifyExprType(expr);
      return expr;
    }

    Expr *visitTypeExpr(TypeExpr *expr) {
      auto toType = simplifyType(cs.getType(expr));
      assert(toType->is<MetatypeType>());
      cs.setType(expr, toType);
      return expr;
    }

    Expr *visitOtherConstructorDeclRefExpr(OtherConstructorDeclRefExpr *expr) {
      cs.setType(expr, expr->getDecl()->getInitializerInterfaceType());
      return expr;
    }

    Expr *visitDotSyntaxBaseIgnoredExpr(DotSyntaxBaseIgnoredExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitOverloadedDeclRefExpr(OverloadedDeclRefExpr *expr) {
      // Determine the declaration selected for this overloaded reference.
      auto locator = cs.getConstraintLocator(expr);
      auto selected = solution.getOverloadChoice(locator);

      return buildDeclRef(selected, expr->getNameLoc(), locator,
                          expr->isImplicit());
    }

    Expr *visitUnresolvedDeclRefExpr(UnresolvedDeclRefExpr *expr) {
      // FIXME: We should have generated an overload set from this, in which
      // case we can emit a typo-correction error here but recover well.
      return nullptr;
    }

    Expr *visitUnresolvedSpecializeExpr(UnresolvedSpecializeExpr *expr) {
      // Our specializations should have resolved the subexpr to the right type.
      return expr->getSubExpr();
    }

    Expr *visitMemberRefExpr(MemberRefExpr *expr) {
      auto memberLocator = cs.getConstraintLocator(expr,
                                                   ConstraintLocator::Member);
      auto selected = solution.getOverloadChoice(memberLocator);
      return buildMemberRef(
          expr->getBase(), expr->getDotLoc(), selected, expr->getNameLoc(),
          cs.getConstraintLocator(expr), memberLocator, expr->isImplicit(),
          expr->getAccessSemantics());
    }

    Expr *visitDynamicMemberRefExpr(DynamicMemberRefExpr *expr) {
      llvm_unreachable("already type-checked?");
    }

    Expr *visitUnresolvedMemberExpr(UnresolvedMemberExpr *expr) {
      // If constraint solving resolved this to an UnresolvedType, then we're in
      // an ambiguity tolerant mode used for diagnostic generation.  Just leave
      // this as an unresolved member reference.
      Type resultTy = simplifyType(cs.getType(expr));
      if (resultTy->hasUnresolvedType()) {
        cs.setType(expr, resultTy);
        return expr;
      }

      auto &ctx = cs.getASTContext();
      // Find the selected member and base type.
      auto memberLocator = cs.getConstraintLocator(
                             expr, ConstraintLocator::UnresolvedMember);
      auto selected = solution.getOverloadChoice(memberLocator);

      // Unresolved member lookup always happens in a metatype so dig out the
      // instance type.
      auto baseTy = selected.choice.getBaseType()->getMetatypeInstanceType();
      baseTy = simplifyType(baseTy);

      // The base expression is simply the metatype of the base type.
      // FIXME: This location info is bogus.
      auto base = TypeExpr::createImplicitHack(expr->getDotLoc(), baseTy, ctx);
      cs.cacheExprTypes(base);

      // Build the member reference.
      auto *exprLoc = cs.getConstraintLocator(expr);
      auto result = buildMemberRef(
          base, expr->getDotLoc(), selected, expr->getNameLoc(), exprLoc,
          memberLocator, expr->isImplicit(), AccessSemantics::Ordinary);
      if (!result)
        return nullptr;

      return coerceToType(result, resultTy, cs.getConstraintLocator(expr));
    }

  private:
    /// A list of "suspicious" optional injections that come from
    /// forced downcasts.
    SmallVector<InjectIntoOptionalExpr *, 4> SuspiciousOptionalInjections;

    /// Create a member reference to the given constructor.
    Expr *applyCtorRefExpr(Expr *expr, Expr *base, SourceLoc dotLoc,
                           DeclNameLoc nameLoc, bool implicit,
                           ConstraintLocator *ctorLocator,
                           SelectedOverload overload) {
      auto locator = cs.getConstraintLocator(expr);
      auto choice = overload.choice;
      assert(choice.getKind() != OverloadChoiceKind::DeclViaDynamic);
      auto *ctor = cast<ConstructorDecl>(choice.getDecl());

      // If the subexpression is a metatype, build a direct reference to the
      // constructor.
      if (cs.getType(base)->is<AnyMetatypeType>()) {
        return buildMemberRef(base, dotLoc, overload, nameLoc, locator,
                              ctorLocator, implicit, AccessSemantics::Ordinary);
      }

      // The subexpression must be either 'self' or 'super'.
      if (!base->isSuperExpr()) {
        // 'super' references have already been fully checked; handle the
        // 'self' case below.
        auto &de = cs.getASTContext().Diags;
        bool diagnoseBadInitRef = true;
        auto arg = base->getSemanticsProvidingExpr();
        if (auto dre = dyn_cast<DeclRefExpr>(arg)) {
          if (dre->getDecl()->getName() == cs.getASTContext().Id_self) {
            // We have a reference to 'self'.
            diagnoseBadInitRef = false;
            // Make sure the reference to 'self' occurs within an initializer.
            if (!dyn_cast_or_null<ConstructorDecl>(
                   cs.DC->getInnermostMethodContext())) {
              if (!SuppressDiagnostics)
                de.diagnose(dotLoc, diag::init_delegation_outside_initializer);
              return nullptr;
            }
          }
        }

        // If we need to diagnose this as a bad reference to an initializer,
        // do so now.
        if (diagnoseBadInitRef) {
          // Determine whether 'super' would have made sense as a base.
          bool hasSuper = false;
          if (auto func = cs.DC->getInnermostMethodContext()) {
            if (auto classDecl = func->getDeclContext()->getSelfClassDecl()) {
              hasSuper = classDecl->hasSuperclass();
            }
          }

          if (SuppressDiagnostics)
            return nullptr;

          de.diagnose(dotLoc, diag::bad_init_ref_base, hasSuper);
        }
      }

      // Build a partial application of the delegated initializer.
      auto callee = resolveConcreteDeclRef(ctor, ctorLocator);
      Expr *ctorRef = buildOtherConstructorRef(overload.openedFullType, callee,
                                               base, nameLoc, ctorLocator,
                                               implicit);
      auto *call = new (cs.getASTContext()) DotSyntaxCallExpr(ctorRef, dotLoc,
                                                              base);

      return finishApply(call, cs.getType(expr), locator, ctorLocator);
    }

    /// Give the deprecation warning for referring to a global function
    /// when there's a method from a conditional conformance in a smaller/closer
    /// scope.
    void
    diagnoseDeprecatedConditionalConformanceOuterAccess(UnresolvedDotExpr *UDE,
                                                        ValueDecl *choice) {
      auto getBaseName = [](DeclContext *context) -> DeclName {
        if (auto generic = context->getSelfNominalTypeDecl()) {
          return generic->getName();
        } else if (context->isModuleScopeContext())
          return context->getParentModule()->getName();
        else
          llvm_unreachable("Unsupported base");
      };

      auto result =
          TypeChecker::lookupUnqualified(cs.DC, UDE->getName(), UDE->getLoc());
      assert(result && "names can't just disappear");
      // These should all come from the same place.
      auto exampleInner = result.front();
      auto innerChoice = exampleInner.getValueDecl();
      auto innerDC = exampleInner.getDeclContext()->getInnermostTypeContext();
      auto innerParentDecl = innerDC->getSelfNominalTypeDecl();
      auto innerBaseName = getBaseName(innerDC);

      auto choiceKind = choice->getDescriptiveKind();
      auto choiceDC = choice->getDeclContext();
      auto choiceBaseName = getBaseName(choiceDC);
      auto choiceParentDecl = choiceDC->getAsDecl();
      auto choiceParentKind = choiceParentDecl
                                  ? choiceParentDecl->getDescriptiveKind()
                                  : DescriptiveDeclKind::Module;

      auto &DE = cs.getASTContext().Diags;
      DE.diagnose(UDE->getLoc(),
                  diag::warn_deprecated_conditional_conformance_outer_access,
                  UDE->getName(), choiceKind, choiceParentKind, choiceBaseName,
                  innerChoice->getDescriptiveKind(),
                  innerParentDecl->getDescriptiveKind(), innerBaseName);

      auto name = choiceBaseName.getBaseIdentifier();
      SmallString<32> namePlusDot = name.str();
      namePlusDot.push_back('.');

      DE.diagnose(UDE->getLoc(),
                  diag::fix_deprecated_conditional_conformance_outer_access,
                  namePlusDot, choiceKind, name)
          .fixItInsert(UDE->getStartLoc(), namePlusDot);
    }

    Expr *applyMemberRefExpr(Expr *expr, Expr *base, SourceLoc dotLoc,
                             DeclNameLoc nameLoc, bool implicit) {
      // If we have a constructor member, handle it as a constructor.
      auto ctorLocator = cs.getConstraintLocator(
                           expr,
                           ConstraintLocator::ConstructorMember);
      if (auto selected = solution.getOverloadChoiceIfAvailable(ctorLocator)) {
        return applyCtorRefExpr(
            expr, base, dotLoc, nameLoc, implicit, ctorLocator, *selected);
      }

      // Determine the declaration selected for this overloaded reference.
      auto memberLocator = cs.getConstraintLocator(expr,
                                                   ConstraintLocator::Member);
      auto selectedElt = solution.getOverloadChoiceIfAvailable(memberLocator);

      if (!selectedElt) {
        // If constraint solving resolved this to an UnresolvedType, then we're
        // in an ambiguity tolerant mode used for diagnostic generation.  Just
        // leave this as whatever type of member reference it already is.
        Type resultTy = simplifyType(cs.getType(expr));
        cs.setType(expr, resultTy);
        return expr;
      }

      auto selected = *selectedElt;
      if (!selected.choice.getBaseType()) {
        // This is one of the "outer alternatives", meaning the innermost
        // methods didn't work out.
        //
        // The only way to get here is via an UnresolvedDotExpr with outer
        // alternatives.
        auto UDE = cast<UnresolvedDotExpr>(expr);
        diagnoseDeprecatedConditionalConformanceOuterAccess(
            UDE, selected.choice.getDecl());

        return buildDeclRef(selected, nameLoc, memberLocator, implicit);
      }

      switch (selected.choice.getKind()) {
      case OverloadChoiceKind::DeclViaBridge: {
        base = cs.coerceToRValue(base);

        // Look through an implicitly unwrapped optional.
        auto baseTy = cs.getType(base);
        auto &ctx = cs.getASTContext();
        auto baseMetaTy = baseTy->getAs<MetatypeType>();
        auto baseInstTy = (baseMetaTy ? baseMetaTy->getInstanceType() : baseTy);
        auto classTy = ctx.getBridgedToObjC(cs.DC, baseInstTy);

        if (baseMetaTy) {
          // FIXME: We're dropping side effects in the base here!
          base = TypeExpr::createImplicitHack(base->getLoc(), classTy, ctx);
          cs.cacheExprTypes(base);
        } else {
          // Bridge the base to its corresponding Objective-C object.
          base = bridgeToObjectiveC(base, classTy);
        }

        // Fall through to build the member reference.
        LLVM_FALLTHROUGH;
      }

      case OverloadChoiceKind::Decl:
      case OverloadChoiceKind::DeclViaUnwrappedOptional:
      case OverloadChoiceKind::DeclViaDynamic:
        return buildMemberRef(base, dotLoc, selected, nameLoc,
                              cs.getConstraintLocator(expr), memberLocator,
                              implicit, AccessSemantics::Ordinary);

      case OverloadChoiceKind::TupleIndex: {
        Type toType = simplifyType(cs.getType(expr));

        auto baseTy = cs.getType(base);
        // If the base type is not a tuple l-value, access to
        // its elements supposed to be r-value as well.
        //
        // This is modeled in constraint system in a way
        // that when member type is resolved by `resolveOverload`
        // it would take r-value type of the element at
        // specified index, but if it's a type variable it
        // could still be bound to l-value later.
        if (!baseTy->is<LValueType>())
          toType = toType->getRValueType();

        // If the result type is an rvalue and the base contains lvalues,
        // need a full tuple coercion to properly load & set access kind
        // on all underlying elements before taking a single element.
        if (!toType->hasLValueType() && baseTy->hasLValueType())
          base = coerceToType(base, baseTy->getRValueType(),
                              cs.getConstraintLocator(base));

        return cs.cacheType(new (cs.getASTContext())
                            TupleElementExpr(base, dotLoc,
                                             selected.choice.getTupleIndex(),
                                             nameLoc.getBaseNameLoc(), toType));
      }

      case OverloadChoiceKind::KeyPathApplication:
        llvm_unreachable("should only happen in a subscript");

      case OverloadChoiceKind::DynamicMemberLookup:
      case OverloadChoiceKind::KeyPathDynamicMemberLookup: {
        return buildDynamicMemberLookupRef(
            expr, base, dotLoc, nameLoc.getStartLoc(), selected, memberLocator);
      }
      }

      llvm_unreachable("Unhandled OverloadChoiceKind in switch.");
    }

    /// Form a type checked expression for the index of a @dynamicMemberLookup
    /// subscript index parameter.
    Expr *buildDynamicMemberLookupIndexExpr(StringRef name, SourceLoc loc,
                                            Type literalTy) {
      // Build and type check the string literal index value to the specific
      // string type expected by the subscript.
      auto &ctx = cs.getASTContext();
      auto *nameExpr = new (ctx) StringLiteralExpr(name, loc, /*implicit*/true);
      cs.setType(nameExpr, literalTy);
      return handleStringLiteralExpr(nameExpr);
    }

    Expr *buildDynamicMemberLookupRef(Expr *expr, Expr *base, SourceLoc dotLoc,
                                      SourceLoc nameLoc,
                                      const SelectedOverload &overload,
                                      ConstraintLocator *memberLocator) {
      // Application of a DynamicMemberLookup result turns
      // a member access of `x.foo` into x[dynamicMember: "foo"], or
      // x[dynamicMember: KeyPath<T, U>]
      auto &ctx = cs.getASTContext();

      // Figure out the expected type of the lookup parameter. We know the
      // openedFullType will be "xType -> indexType -> resultType".  Dig out
      // its index type.
      auto paramTy = getTypeOfDynamicMemberIndex(overload);

      Expr *argExpr = nullptr;
      if (overload.choice.getKind() ==
          OverloadChoiceKind::DynamicMemberLookup) {
        // Build and type check the string literal index value to the specific
        // string type expected by the subscript.
        auto fieldName = overload.choice.getName().getBaseIdentifier().str();
        argExpr = buildDynamicMemberLookupIndexExpr(fieldName, nameLoc,
                                                    paramTy);
      } else {
        argExpr = buildKeyPathDynamicMemberIndexExpr(
            paramTy->castTo<BoundGenericType>(), dotLoc, memberLocator);
      }

      if (!argExpr)
        return nullptr;

      // Build a tuple so that the argument has a label.
      auto tupleTy =
          TupleType::get(TupleTypeElt(paramTy, ctx.Id_dynamicMember), ctx);

      Expr *index =
          TupleExpr::createImplicit(ctx, argExpr, ctx.Id_dynamicMember);
      index->setType(tupleTy);
      cs.cacheType(index);

      // Build and return a subscript that uses this string as the index.
      return buildSubscript(
          base, index, ctx.Id_dynamicMember,
          /*trailingClosure*/ false, cs.getConstraintLocator(expr),
          /*isImplicit*/ true, AccessSemantics::Ordinary, overload);
    }

    Type getTypeOfDynamicMemberIndex(const SelectedOverload &overload) {
      assert(overload.choice.getKind() ==
                 OverloadChoiceKind::DynamicMemberLookup ||
             overload.choice.getKind() ==
                 OverloadChoiceKind::KeyPathDynamicMemberLookup);

      auto declTy = solution.simplifyType(overload.openedFullType);
      auto subscriptTy = declTy->castTo<FunctionType>()->getResult();
      auto refFnType = subscriptTy->castTo<FunctionType>();
      assert(refFnType->getParams().size() == 1 &&
             "subscript always has one arg");
      return refFnType->getParams()[0].getPlainType();
    }

  public:
    Expr *visitUnresolvedDotExpr(UnresolvedDotExpr *expr) {
      return applyMemberRefExpr(expr, expr->getBase(), expr->getDotLoc(),
                                expr->getNameLoc(), expr->isImplicit());
    }

    Expr *visitSequenceExpr(SequenceExpr *expr) {
      llvm_unreachable("Expression wasn't parsed?");
    }

    Expr *visitArrowExpr(ArrowExpr *expr) {
      llvm_unreachable("Arrow expr wasn't converted to type?");
    }

    Expr *visitIdentityExpr(IdentityExpr *expr) {
      cs.setType(expr, cs.getType(expr->getSubExpr()));
      return expr;
    }

    Expr *visitAnyTryExpr(AnyTryExpr *expr) {
      auto *subExpr = expr->getSubExpr();
      auto type = simplifyType(cs.getType(subExpr));

      // Let's load the value associated with this try.
      if (type->hasLValueType()) {
        subExpr = coerceToType(subExpr, type->getRValueType(),
                               cs.getConstraintLocator(subExpr));

        if (!subExpr)
          return nullptr;
      }

      cs.setType(expr, cs.getType(subExpr));
      expr->setSubExpr(subExpr);

      return expr;
    }

    Expr *visitOptionalTryExpr(OptionalTryExpr *expr) {
      // Prior to Swift 5, 'try?' simply wraps the type of its sub-expression
      // in an Optional, regardless of the sub-expression type.
      //
      // In Swift 5+, the type of a 'try?' expression of static type T is:
      //  - Equal to T if T is optional
      //  - Equal to T? if T is not optional
      //
      // The result is that in Swift 5, 'try?' avoids producing nested optionals.
      
      if (!cs.getASTContext().LangOpts.isSwiftVersionAtLeast(5)) {
        // Nothing to do for Swift 4 and earlier!
        return simplifyExprType(expr);
      }
      
      Type exprType = simplifyType(cs.getType(expr));

      auto subExpr = coerceToType(expr->getSubExpr(), exprType,
                                  cs.getConstraintLocator(expr));
      if (!subExpr) return nullptr;
      expr->setSubExpr(subExpr);

      cs.setType(expr, exprType);
      return expr;
    }

    Expr *visitParenExpr(ParenExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitUnresolvedMemberChainResultExpr(
        UnresolvedMemberChainResultExpr *expr) {
      // Since this expression only exists to give the result type of an
      // unresolved member chain visibility in the AST, remove it from the AST
      // now that we have a solution and coerce the subexpr to the resulting
      // type.
      auto *subExpr = expr->getSubExpr();
      auto type = simplifyType(cs.getType(expr));
      subExpr = coerceToType(subExpr, type, cs.getConstraintLocator(subExpr));
      cs.setType(subExpr, type);
      return subExpr;
    }

    Expr *visitTupleExpr(TupleExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitSubscriptExpr(SubscriptExpr *expr) {
      auto *memberLocator =
          cs.getConstraintLocator(expr, ConstraintLocator::SubscriptMember);
      auto overload = solution.getOverloadChoiceIfAvailable(memberLocator);

      // Handles situation where there was a solution available but it didn't
      // have a proper overload selected from subscript call, might be because
      // solver was allowed to return free or unresolved types, which can
      // happen while running diagnostics on one of the expressions.
      if (!overload) {
        auto *base = expr->getBase();
        auto &de = cs.getASTContext().Diags;
        auto baseType = cs.getType(base);

        if (auto errorType = baseType->getAs<ErrorType>()) {
          de.diagnose(base->getLoc(), diag::cannot_subscript_base,
                      errorType->getOriginalType())
              .highlight(base->getSourceRange());
        } else {
          de.diagnose(base->getLoc(), diag::cannot_subscript_ambiguous_base)
              .highlight(base->getSourceRange());
        }

        return nullptr;
      }

      if (overload->choice.getKind() ==
              OverloadChoiceKind::KeyPathDynamicMemberLookup) {
        return buildDynamicMemberLookupRef(
            expr, expr->getBase(), expr->getIndex()->getStartLoc(), SourceLoc(),
            *overload, memberLocator);
      }

      return buildSubscript(
          expr->getBase(), expr->getIndex(), expr->getArgumentLabels(),
          expr->hasTrailingClosure(), cs.getConstraintLocator(expr),
          expr->isImplicit(), expr->getAccessSemantics(), *overload);
    }

    /// "Finish" an array expression by filling in the semantic expression.
    ArrayExpr *finishArrayExpr(ArrayExpr *expr) {
      Type arrayTy = cs.getType(expr);
      auto &ctx = cs.getASTContext();

      ProtocolDecl *arrayProto = TypeChecker::getProtocol(
          ctx, expr->getLoc(), KnownProtocolKind::ExpressibleByArrayLiteral);
      assert(arrayProto && "type-checked array literal w/o protocol?!");

      auto conformance =
        TypeChecker::conformsToProtocol(arrayTy, arrayProto,
                                        cs.DC->getParentModule());
      assert(conformance && "Type does not conform to protocol?");

      DeclName name(ctx, DeclBaseName::createConstructor(),
                    {ctx.Id_arrayLiteral});
      ConcreteDeclRef witness =
          conformance.getWitnessByName(arrayTy->getRValueType(), name);
      if (!witness || !isa<AbstractFunctionDecl>(witness.getDecl()))
        return nullptr;
      expr->setInitializer(witness);

      auto elementType = expr->getElementType();

      for (auto &element : expr->getElements()) {
        element = coerceToType(element, elementType,
                               cs.getConstraintLocator(element));
      }

      return expr;
    }

    Expr *visitArrayExpr(ArrayExpr *expr) {
      Type openedType = cs.getType(expr);
      Type arrayTy = simplifyType(openedType);
      cs.setType(expr, arrayTy);
      if (!finishArrayExpr(expr)) return nullptr;

      // If the array element type was defaulted, note that in the expression.
      if (solution.DefaultedConstraints.count(cs.getConstraintLocator(expr)))
        expr->setIsTypeDefaulted();

      return expr;
    }

    /// "Finish" a dictionary expression by filling in the semantic expression.
    DictionaryExpr *finishDictionaryExpr(DictionaryExpr *expr) {
      Type dictionaryTy = cs.getType(expr);

      auto &ctx = cs.getASTContext();
      ProtocolDecl *dictionaryProto = TypeChecker::getProtocol(
          cs.getASTContext(), expr->getLoc(),
          KnownProtocolKind::ExpressibleByDictionaryLiteral);

      auto conformance =
        TypeChecker::conformsToProtocol(dictionaryTy, dictionaryProto,
                                        cs.DC->getParentModule());
      if (conformance.isInvalid())
        return nullptr;

      DeclName name(ctx, DeclBaseName::createConstructor(),
                    {ctx.Id_dictionaryLiteral});
      ConcreteDeclRef witness =
          conformance.getWitnessByName(dictionaryTy->getRValueType(), name);
      if (!witness || !isa<AbstractFunctionDecl>(witness.getDecl()))
        return nullptr;
      expr->setInitializer(witness);

      auto elementType = expr->getElementType();
      for (auto &element : expr->getElements()) {
        element = coerceToType(element, elementType,
                               cs.getConstraintLocator(element));
      }

      return expr;
    }

    Expr *visitDictionaryExpr(DictionaryExpr *expr) {
      Type openedType = cs.getType(expr);
      Type dictionaryTy = simplifyType(openedType);
      cs.setType(expr, dictionaryTy);
      if (!finishDictionaryExpr(expr)) return nullptr;

      // If the dictionary key or value type was defaulted, note that in the
      // expression.
      if (solution.DefaultedConstraints.count(cs.getConstraintLocator(expr)))
        expr->setIsTypeDefaulted();

      return expr;
    }

    Expr *visitDynamicSubscriptExpr(DynamicSubscriptExpr *expr) {
      auto *memberLocator =
          cs.getConstraintLocator(expr, ConstraintLocator::SubscriptMember);
      return buildSubscript(expr->getBase(), expr->getIndex(),
                            expr->getArgumentLabels(),
                            expr->hasTrailingClosure(),
                            cs.getConstraintLocator(expr),
                            expr->isImplicit(), AccessSemantics::Ordinary,
                            solution.getOverloadChoice(memberLocator));
    }

    Expr *visitTupleElementExpr(TupleElementExpr *expr) {
      simplifyExprType(expr);
      return expr;
    }

    Expr *visitCaptureListExpr(CaptureListExpr *expr) {
      // The type of the capture list is the type of the closure contained
      // inside it.
      cs.setType(expr, cs.getType(expr->getClosureBody()));
      return expr;
    }

    Expr *visitClosureExpr(ClosureExpr *expr) {
      llvm_unreachable("Handled by the walker directly");
    }

    Expr *visitAutoClosureExpr(AutoClosureExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitInOutExpr(InOutExpr *expr) {
      auto objectTy = cs.getType(expr->getSubExpr())->getRValueType();

      // The type is simply inout of whatever the lvalue's object type was.
      cs.setType(expr, InOutType::get(objectTy));
      return expr;
    }

    Expr *visitVarargExpansionExpr(VarargExpansionExpr *expr) {
      simplifyExprType(expr);

      auto arrayTy = cs.getType(expr);
      expr->setSubExpr(coerceToType(expr->getSubExpr(), arrayTy,
                                    cs.getConstraintLocator(expr)));
      return expr;
    }

    Expr *visitDynamicTypeExpr(DynamicTypeExpr *expr) {
      Expr *base = expr->getBase();
      base = cs.coerceToRValue(base);
      expr->setBase(base);

      return simplifyExprType(expr);
    }

    Expr *visitOpaqueValueExpr(OpaqueValueExpr *expr) {
      assert(expr->isPlaceholder() && "Already type-checked");
      return expr;
    }

    Expr *visitPropertyWrapperValuePlaceholderExpr(
        PropertyWrapperValuePlaceholderExpr *expr) {
      // If there is no opaque value placeholder, the enclosing init(wrappedValue:)
      // expression cannot be reused, so we only need the original wrapped value
      // argument in the AST.
      if (!expr->getOpaqueValuePlaceholder())
        return expr->getOriginalWrappedValue();

      return expr;
    }

    Expr *visitAppliedPropertyWrapperExpr(AppliedPropertyWrapperExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitDefaultArgumentExpr(DefaultArgumentExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitApplyExpr(ApplyExpr *expr) {
      auto *calleeLoc = CalleeLocators[expr];
      assert(calleeLoc);
      return finishApply(expr, cs.getType(expr), cs.getConstraintLocator(expr),
                         calleeLoc);
    }

    Expr *visitRebindSelfInConstructorExpr(RebindSelfInConstructorExpr *expr) {
      // A non-failable initializer cannot delegate to a failable
      // initializer.
      Expr *unwrappedSubExpr = expr->getSubExpr()->getSemanticsProvidingExpr();
      Type valueTy = cs.getType(unwrappedSubExpr)->getOptionalObjectType();
      auto inCtor = cast<ConstructorDecl>(cs.DC->getInnermostMethodContext());
      if (valueTy && !inCtor->isFailable()) {
        bool isChaining;
        auto *otherCtorRef = expr->getCalledConstructor(isChaining);
        ConstructorDecl *ctor = otherCtorRef->getDecl();
        assert(ctor);

        // If the initializer we're calling is not declared as
        // checked, it's an error.
        bool isError = !ctor->isImplicitlyUnwrappedOptional();

        // If we're suppressing diagnostics, just fail.
        if (isError && SuppressDiagnostics)
          return nullptr;

        auto &ctx = cs.getASTContext();

        auto &de = cs.getASTContext().Diags;
        if (isError) {
          if (auto *optTry = dyn_cast<OptionalTryExpr>(unwrappedSubExpr)) {
            de.diagnose(optTry->getTryLoc(),
                        diag::delegate_chain_nonoptional_to_optional_try,
                        isChaining);
            de.diagnose(optTry->getTryLoc(), diag::init_delegate_force_try)
                .fixItReplace({optTry->getTryLoc(), optTry->getQuestionLoc()},
                              "try!");
            de.diagnose(inCtor->getLoc(), diag::init_propagate_failure)
                .fixItInsertAfter(inCtor->getLoc(), "?");
          } else {
            // Give the user the option of adding '!' or making the enclosing
            // initializer failable.
            de.diagnose(otherCtorRef->getLoc(),
                        diag::delegate_chain_nonoptional_to_optional,
                        isChaining, ctor->getName());
            de.diagnose(otherCtorRef->getLoc(), diag::init_force_unwrap)
                .fixItInsertAfter(expr->getEndLoc(), "!");
            de.diagnose(inCtor->getLoc(), diag::init_propagate_failure)
                .fixItInsertAfter(inCtor->getLoc(), "?");
          }
        }

        // Recover by injecting the force operation (the first option).
        Expr *newSub = new (ctx) ForceValueExpr(expr->getSubExpr(),
                                                expr->getEndLoc());
        cs.setType(newSub, valueTy);
        newSub->setImplicit();
        expr->setSubExpr(newSub);
      }

      return expr;
    }

    Expr *visitIfExpr(IfExpr *expr) {
      auto resultTy = simplifyType(cs.getType(expr));
      cs.setType(expr, resultTy);

      auto cond = cs.coerceToRValue(expr->getCondExpr());
      expr->setCondExpr(cond);

      // Coerce the then/else branches to the common type.
      expr->setThenExpr(coerceToType(expr->getThenExpr(), resultTy,
                               cs.getConstraintLocator(expr->getThenExpr())));
      expr->setElseExpr(coerceToType(expr->getElseExpr(), resultTy,
                                 cs.getConstraintLocator(expr->getElseExpr())));

      return expr;
    }
    
    Expr *visitImplicitConversionExpr(ImplicitConversionExpr *expr) {
      llvm_unreachable("Already type-checked");
    }

    Expr *visitIsExpr(IsExpr *expr) {
      // Turn the subexpression into an rvalue.
      auto &ctx = cs.getASTContext();
      auto sub = cs.coerceToRValue(expr->getSubExpr());
      expr->setSubExpr(sub);

      // Simplify and update the type we checked against.
      auto *const castTypeRepr = expr->getCastTypeRepr();

      const auto fromType = cs.getType(sub);
      const auto toType = simplifyType(cs.getType(castTypeRepr));
      expr->setCastType(toType);
      cs.setType(castTypeRepr, toType);

      auto castContextKind =
          SuppressDiagnostics ? CheckedCastContextKind::None
                              : CheckedCastContextKind::IsExpr;
      auto castKind = TypeChecker::typeCheckCheckedCast(
          fromType, toType, castContextKind, cs.DC, expr->getLoc(), sub,
          castTypeRepr->getSourceRange());

      switch (castKind) {
      case CheckedCastKind::Unresolved:
        expr->setCastKind(CheckedCastKind::ValueCast);
        break;
          
      case CheckedCastKind::Coercion:
      case CheckedCastKind::BridgingCoercion:
        expr->setCastKind(castKind);
        break;
      case CheckedCastKind::ValueCast:
        // Check the cast target is a non-foreign type
        if (auto cls = toType->getAs<ClassType>()) {
          if (cls->getDecl()->getForeignClassKind() ==
                ClassDecl::ForeignKind::CFType) {
            ctx.Diags.diagnose(expr->getLoc(), diag::isa_is_foreign_check,
                               toType);
          }
        }
        expr->setCastKind(castKind);
        break;
      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::SetDowncast:
        // Valid checks.
        expr->setCastKind(castKind);
        break;
      }

      // SIL-generation magically turns this into a Bool; make sure it can.
      if (!ctx.getBoolBuiltinInitDecl()) {
        ctx.Diags.diagnose(expr->getLoc(), diag::broken_bool);
        // Continue anyway.
      }

      // Dig through the optionals in the from/to types.
      SmallVector<Type, 2> fromOptionals;
      fromType->lookThroughAllOptionalTypes(fromOptionals);
      SmallVector<Type, 2> toOptionals;
      toType->lookThroughAllOptionalTypes(toOptionals);

      // If we have an imbalance of optionals or a collection
      // downcast, handle this as a checked cast followed by a
      // a 'hasValue' check.
      if (fromOptionals.size() != toOptionals.size() ||
          castKind == CheckedCastKind::ArrayDowncast ||
          castKind == CheckedCastKind::DictionaryDowncast ||
          castKind == CheckedCastKind::SetDowncast) {
        auto *const cast =
            ConditionalCheckedCastExpr::createImplicit(ctx, sub, toType);
        cast->setType(OptionalType::get(toType));
        cast->setCastType(toType);
        cs.setType(cast, cast->getType());

        // Type-check this conditional case.
        Expr *result = handleConditionalCheckedCastExpr(cast, castTypeRepr);
        if (!result)
          return nullptr;

        // Extract a Bool from the resulting expression.
        TypeChecker::requireOptionalIntrinsics(ctx, expr->getLoc());

        // Match the optional value against its `Some` case.
        auto *someDecl = ctx.getOptionalSomeDecl();
        auto isSomeExpr =
            new (ctx) EnumIsCaseExpr(result, castTypeRepr, someDecl);
        auto boolDecl = ctx.getBoolDecl();

        if (!boolDecl) {
          ctx.Diags.diagnose(SourceLoc(), diag::broken_bool);
        }

        cs.setType(isSomeExpr, boolDecl ? ctx.getBoolType() : Type());
        return isSomeExpr;
      }

      return expr;
    }

    /// The kind of cast we're working with for handling optional bindings.
    enum class OptionalBindingsCastKind {
      /// An explicit bridging conversion, spelled "as".
      Bridged,
      /// A forced cast, spelled "as!".
      Forced,
      /// A conditional cast, spelled "as?".
      Conditional,
    };

    /// Handle optional operands and results in an explicit cast.
    Expr *handleOptionalBindingsForCast(ExplicitCastExpr *cast, 
                                        Type finalResultType,
                                        OptionalBindingsCastKind castKind) {
      return handleOptionalBindings(cast->getSubExpr(), finalResultType,
                                    castKind,
        [&](Expr *sub, Type resultType) -> Expr* {

        // Complain about conditional casts to CF class types; they can't
        // actually be conditionally checked.
        if (castKind == OptionalBindingsCastKind::Conditional) {
          Type destValueType = resultType->getOptionalObjectType();
          auto destObjectType = destValueType;
          if (auto metaTy = destObjectType->getAs<MetatypeType>())
            destObjectType = metaTy->getInstanceType();
          if (auto destClass = destObjectType->getClassOrBoundGenericClass()) {
            if (destClass->getForeignClassKind() ==
                  ClassDecl::ForeignKind::CFType) {
              if (SuppressDiagnostics)
                return nullptr;

              auto &de = cs.getASTContext().Diags;
              de.diagnose(cast->getLoc(), diag::conditional_downcast_foreign,
                          destValueType);
              ConcreteDeclRef refDecl = sub->getReferencedDecl();
              if (refDecl) {
                de.diagnose(cast->getLoc(),
                            diag::note_explicitly_compare_cftypeid,
                            refDecl.getDecl()->getBaseName(), destValueType);
              }
            }
          }
        }

        // Set the expression as the sub-expression of the cast, then
        // use the cast as the inner operation.
        cast->setSubExpr(sub);
        cs.setType(cast, resultType);
        return cast;
      });
    }

    /// A helper function to build an operation.  The inner result type
    /// is the expected type of the operation; it will be a non-optional
    /// type unless the castKind is Conditional.
    using OperationBuilderRef =
      llvm::function_ref<Expr*(Expr *subExpr, Type innerResultType)>;

    /// Handle optional operands and results in an explicit cast.
    Expr *handleOptionalBindings(Expr *subExpr, Type finalResultType,
                                 OptionalBindingsCastKind castKind,
                                 OperationBuilderRef buildInnerOperation) {
      auto &ctx = cs.getASTContext();

      unsigned destExtraOptionals;
      bool forceExtraSourceOptionals;
      switch (castKind) {
      case OptionalBindingsCastKind::Bridged:
        destExtraOptionals = 0;
        forceExtraSourceOptionals = true;
        break;

      case OptionalBindingsCastKind::Forced:
        destExtraOptionals = 0;
        forceExtraSourceOptionals = true;
        break;

      case OptionalBindingsCastKind::Conditional:
        destExtraOptionals = 1;
        forceExtraSourceOptionals = false;
        break;
      }

      // FIXME: some of this work needs to be delayed until runtime to
      // properly account for archetypes dynamically being optional
      // types.  For example, if we're casting T to NSView?, that
      // should succeed if T=NSObject? and its value is actually nil.
      Type srcType = cs.getType(subExpr);

      SmallVector<Type, 4> srcOptionals;
      srcType = srcType->lookThroughAllOptionalTypes(srcOptionals);

      SmallVector<Type, 4> destOptionals;
      auto destValueType
        = finalResultType->lookThroughAllOptionalTypes(destOptionals);

      auto isBridgeToAnyObject =
        castKind == OptionalBindingsCastKind::Bridged &&
        destValueType->isAnyObject();

      // If the destination value type is 'AnyObject' when performing a
      // bridging operation, or if the destination value type could dynamically
      // be an optional type, leave any extra optionals on the source in place.
      // Only apply the latter condition in Swift 5 mode to best preserve
      // compatibility with Swift 4.1's casting behaviour.
      if (isBridgeToAnyObject || (ctx.isSwiftVersionAtLeast(5) &&
                                  destValueType->canDynamicallyBeOptionalType(
                                      /*includeExistential*/ false))) {
        auto destOptionalsCount = destOptionals.size() - destExtraOptionals;
        if (srcOptionals.size() > destOptionalsCount) {
          srcType = srcOptionals[destOptionalsCount];
          srcOptionals.erase(srcOptionals.begin() + destOptionalsCount,
                             srcOptionals.end());
        }
      }

      // When performing a bridging operation, if the destination type
      // is more optional than the source, we'll add extra optional injections
      // at the end.
      SmallVector<Type, 4> destOptionalInjections;
      if (castKind == OptionalBindingsCastKind::Bridged &&
          destOptionals.size() > srcOptionals.size()) {
        // Remove the extra optionals from destOptionals, but keep them around
        // separately so we can perform the injections on the final result of
        // the cast.
        auto cutPoint = destOptionals.end() - srcOptionals.size();
        destOptionalInjections.append(destOptionals.begin(), cutPoint);
        destOptionals.erase(destOptionals.begin(), cutPoint);

        finalResultType = destOptionals.empty() ? destValueType
                                                : destOptionals.front();
      }

      // Local function to add the optional injections to final result.
      auto addFinalOptionalInjections = [&](Expr *result) {
        for (auto destType : llvm::reverse(destOptionalInjections)) {
          result =
              cs.cacheType(new (ctx) InjectIntoOptionalExpr(result, destType));
        }

        return result;
      };

      // There's nothing special to do if the operand isn't optional
      // (or is insufficiently optional) and we don't need any bridging.
      if (srcOptionals.empty()
          || (srcOptionals.size() < destOptionals.size() - destExtraOptionals)) {
        Expr *result = buildInnerOperation(subExpr, finalResultType);
        if (!result) return nullptr;
        return addFinalOptionalInjections(result);
      }

      // The outermost N levels of optionals on the operand must all
      // be present or the cast fails.  The innermost M levels of
      // optionals on the operand are reflected in the requested
      // destination type, so we should map these nils into the result.
      unsigned numRequiredOptionals =
        srcOptionals.size() - (destOptionals.size() - destExtraOptionals);

      // The number of OptionalEvaluationExprs between the point of the
      // inner cast and the enclosing OptionalEvaluationExpr (exclusive)
      // which represents failure for the entire operation.
      unsigned failureDepth = destOptionals.size() - destExtraOptionals;

      // Drill down on the operand until it's non-optional.
      SourceLoc fakeQuestionLoc = subExpr->getEndLoc();
      for (unsigned i : indices(srcOptionals)) {
        Type valueType =
          (i + 1 == srcOptionals.size() ? srcType : srcOptionals[i+1]);

        // As we move into the range of mapped optionals, start
        // lowering the depth.
        unsigned depth = failureDepth;
        if (i >= numRequiredOptionals) {
          depth -= (i - numRequiredOptionals) + 1;
        } else if (forceExtraSourceOptionals) {
          // For a forced cast, force the required optionals.
          subExpr = new (ctx) ForceValueExpr(subExpr, fakeQuestionLoc);
          cs.setType(subExpr, valueType);
          subExpr->setImplicit(true);
          continue;
        }

        subExpr = cs.cacheType(new (ctx) BindOptionalExpr(
            subExpr, fakeQuestionLoc, depth, valueType));
        subExpr->setImplicit(true);
      }

      // If this is a conditional cast, the result type will always
      // have at least one level of optional, which should become the
      // type of the checked-cast expression.
      Expr *result;
      if (castKind == OptionalBindingsCastKind::Conditional) {
        assert(!destOptionals.empty() &&
               "result of checked cast is not an optional type");
        result = buildInnerOperation(subExpr, destOptionals.back());
      } else {
        result = buildInnerOperation(subExpr, destValueType);
      }
      if (!result) return nullptr;

      // If we're casting to an optional type, we need to capture the
      // final M bindings.

      if (destOptionals.size() > destExtraOptionals) {
        if (castKind == OptionalBindingsCastKind::Conditional) {
          // If the innermost cast fails, the entire expression fails.  To
          // get this behavior, we have to bind and then re-inject the result.
          // (SILGen should know how to peephole this.)
          result = cs.cacheType(new (ctx) BindOptionalExpr(
              result, result->getEndLoc(), failureDepth, destValueType));
          result->setImplicit(true);
        }

        for (unsigned i = destOptionals.size(); i != 0; --i) {
          Type destType = destOptionals[i-1];
          result =
              cs.cacheType(new (ctx) InjectIntoOptionalExpr(result, destType));
          result =
              cs.cacheType(new (ctx) OptionalEvaluationExpr(result, destType));
        }

      // Otherwise, we just need to capture the failure-depth binding.
      } else if (!forceExtraSourceOptionals) {
        result = cs.cacheType(
            new (ctx) OptionalEvaluationExpr(result, finalResultType));
      }

      return addFinalOptionalInjections(result);
    }

    bool hasForcedOptionalResult(ExplicitCastExpr *expr) {
      const auto *const TR = expr->getCastTypeRepr();
      if (TR && TR->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional) {
        auto *locator = cs.getConstraintLocator(
            expr, ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice);
        return solution.getDisjunctionChoice(locator);
      }
      return false;
    }

    Expr *visitCoerceExpr(CoerceExpr *expr) {
      // If we need to insert a force-unwrap for coercions of the form
      // 'as T!', do so now.
      if (hasForcedOptionalResult(expr)) {
        auto *coerced = visitCoerceExpr(expr, None);
        if (!coerced)
          return nullptr;

        return coerceImplicitlyUnwrappedOptionalToValue(
            coerced, cs.getType(coerced)->getOptionalObjectType());
      }

      return visitCoerceExpr(expr, None);
    }

    Expr *visitCoerceExpr(CoerceExpr *expr, Optional<unsigned> choice) {
      // Simplify and update the type we're coercing to.
      assert(expr->getCastTypeRepr());
      const auto toType = simplifyType(cs.getType(expr->getCastTypeRepr()));
      expr->setCastType(toType);
      cs.setType(expr->getCastTypeRepr(), toType);

      // If this is a literal that got converted into constructor call
      // lets put proper source information in place.
      if (expr->isLiteralInit()) {
        auto *literalInit = expr->getSubExpr();
        if (auto *call = dyn_cast<CallExpr>(literalInit)) {
          forEachExprInConstraintSystem(call->getFn(),
                                        [&](Expr *subExpr) -> Expr * {
            auto *TE = dyn_cast<TypeExpr>(subExpr);
            if (!TE)
              return subExpr;

            auto type = cs.getInstanceType(TE);

            assert(!type->hasError());

            if (!type->isEqual(toType))
              return subExpr;

            return cs.cacheType(TypeExpr::createImplicitHack(
                expr->getLoc(), toType, cs.getASTContext()));
          });
        }

        if (auto *literal = dyn_cast<NumberLiteralExpr>(literalInit)) {
          literal->setExplicitConversion();
        } else {
          literalInit->setImplicit(false);
        }

        cs.setType(expr, toType);
        // Keep the coercion around, because it contains the source range
        // for the original constructor call.
        return expr;
      }

      // Turn the subexpression into an rvalue.
      auto rvalueSub = cs.coerceToRValue(expr->getSubExpr());
      expr->setSubExpr(rvalueSub);

      // If we weren't explicitly told by the caller which disjunction choice,
      // get it from the solution to determine whether we've picked a coercion
      // or a bridging conversion.
      auto *locator = cs.getConstraintLocator(expr);

      if (!choice) {
        choice = solution.getDisjunctionChoice(locator);
      }

      // Handle the coercion/bridging of the underlying subexpression, where
      // optionality has been removed.
      if (*choice == 0) {
        // Convert the subexpression.
        Expr *sub = expr->getSubExpr();

        sub = solution.coerceToType(sub, toType, cs.getConstraintLocator(sub));
        if (!sub)
          return nullptr;

        expr->setSubExpr(sub);
        cs.setType(expr, toType);
        return expr;
      }

      // Bridging conversion.
      assert(*choice == 1 && "should be bridging");

      // Handle optional bindings.
      Expr *sub = handleOptionalBindings(expr->getSubExpr(), toType,
                                         OptionalBindingsCastKind::Bridged,
                                         [&](Expr *sub, Type toInstanceType) {
        return buildObjCBridgeExpr(sub, toInstanceType, locator);
      });

      if (!sub) return nullptr;
      expr->setSubExpr(sub);
      cs.setType(expr, toType);
      return expr;
    }

    // Rewrite ForcedCheckedCastExpr based on what the solver computed.
    Expr *visitForcedCheckedCastExpr(ForcedCheckedCastExpr *expr) {
      // The subexpression is always an rvalue.
      auto sub = cs.coerceToRValue(expr->getSubExpr());
      expr->setSubExpr(sub);

      // Simplify and update the type we're casting to.
      auto *const castTypeRepr = expr->getCastTypeRepr();

      const auto fromType = cs.getType(sub);
      auto toType = simplifyType(cs.getType(castTypeRepr));
      if (hasForcedOptionalResult(expr))
        toType = toType->getOptionalObjectType();

      expr->setCastType(toType);
      cs.setType(castTypeRepr, toType);

      auto castContextKind =
          SuppressDiagnostics ? CheckedCastContextKind::None
                              : CheckedCastContextKind::ForcedCast;

      const auto castKind = TypeChecker::typeCheckCheckedCast(
          fromType, toType, castContextKind, cs.DC, expr->getLoc(), sub,
          castTypeRepr->getSourceRange());
      switch (castKind) {
        /// Invalid cast.
      case CheckedCastKind::Unresolved:
        return nullptr;
      case CheckedCastKind::Coercion:
      case CheckedCastKind::BridgingCoercion: {
        expr->setCastKind(castKind);
        cs.setType(expr, toType);
        return expr;
      }

      // Valid casts.
      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::SetDowncast:
      case CheckedCastKind::ValueCast:
        expr->setCastKind(castKind);
        break;
      }
      
      return handleOptionalBindingsForCast(expr, simplifyType(cs.getType(expr)),
                                           OptionalBindingsCastKind::Forced);
    }

    Expr *visitConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr) {
      // Simplify and update the type we're casting to.
      auto *const castTypeRepr = expr->getCastTypeRepr();
      const auto toType = simplifyType(cs.getType(castTypeRepr));
      expr->setCastType(toType);
      cs.setType(castTypeRepr, toType);

      // If we need to insert a force-unwrap for coercions of the form
      // 'as! T!', do so now.
      if (hasForcedOptionalResult(expr)) {
        auto *coerced = handleConditionalCheckedCastExpr(expr, castTypeRepr);
        if (!coerced)
          return nullptr;

        return coerceImplicitlyUnwrappedOptionalToValue(
            coerced, cs.getType(coerced)->getOptionalObjectType());
      }

      return handleConditionalCheckedCastExpr(expr, castTypeRepr);
    }

    Expr *handleConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr,
                                           TypeRepr *castTypeRepr) {
      assert(castTypeRepr &&
             "cast requires TypeRepr; implicit casts are superfluous");

      // The subexpression is always an rvalue.
      auto &ctx = cs.getASTContext();
      auto sub = cs.coerceToRValue(expr->getSubExpr());
      expr->setSubExpr(sub);

      // Simplify and update the type we're casting to.
      const auto fromType = cs.getType(sub);
      const auto toType = expr->getCastType();

      bool isSubExprLiteral = isa<LiteralExpr>(sub);
      auto castContextKind =
          (SuppressDiagnostics || expr->isImplicit() || isSubExprLiteral)
              ? CheckedCastContextKind::None
              : CheckedCastContextKind::ConditionalCast;

      auto castKind = TypeChecker::typeCheckCheckedCast(
          fromType, toType, castContextKind, cs.DC, expr->getLoc(), sub,
          castTypeRepr->getSourceRange());
      switch (castKind) {
      // Invalid cast.
      case CheckedCastKind::Unresolved:
        // FIXME: This literal diagnostics needs to be revisited by a proposal
        // to unify casting semantics for literals.
        // https://bugs.swift.org/browse/SR-12093
        if (isSubExprLiteral) {
          auto protocol = TypeChecker::getLiteralProtocol(ctx, sub);
          // Special handle for literals conditional checked cast when they can
          // be statically coerced to the cast type.
          if (protocol && TypeChecker::conformsToProtocol(
                              toType, protocol, cs.DC->getParentModule())) {
            ctx.Diags
                .diagnose(expr->getLoc(),
                          diag::literal_conditional_downcast_to_coercion,
                          toType);
          } else {
            ctx.Diags
                .diagnose(expr->getLoc(), diag::downcast_to_unrelated, fromType,
                          toType)
                .highlight(sub->getSourceRange())
                .highlight(castTypeRepr->getSourceRange());
          }
        }
        expr->setCastKind(CheckedCastKind::ValueCast);
        break;

      case CheckedCastKind::Coercion:
      case CheckedCastKind::BridgingCoercion: {
        expr->setCastKind(castKind);
        cs.setType(expr, OptionalType::get(toType));
        return expr;
      }

      // Valid casts.
      case CheckedCastKind::ArrayDowncast:
      case CheckedCastKind::DictionaryDowncast:
      case CheckedCastKind::SetDowncast:
      case CheckedCastKind::ValueCast:
        expr->setCastKind(castKind);
        break;
      }

      return handleOptionalBindingsForCast(expr, simplifyType(cs.getType(expr)),
                                         OptionalBindingsCastKind::Conditional);
    }

    Expr *visitAssignExpr(AssignExpr *expr) {
      // Convert the source to the simplified destination type.
      auto destTy = simplifyType(cs.getType(expr->getDest()));
      auto locator =
        ConstraintLocatorBuilder(cs.getConstraintLocator(expr->getSrc()));
      Expr *src = coerceToType(expr->getSrc(), destTy->getRValueType(), locator);
      if (!src)
        return nullptr;

      expr->setSrc(src);

      if (!SuppressDiagnostics) {
        // If we're performing an assignment to a weak or unowned variable from
        // a constructor call, emit a warning that the instance will be
        // immediately deallocated.
        diagnoseUnownedImmediateDeallocation(cs.getASTContext(), expr);
      }
      return expr;
    }
    
    Expr *visitDiscardAssignmentExpr(DiscardAssignmentExpr *expr) {
      return simplifyExprType(expr);
    }
    
    Expr *visitUnresolvedPatternExpr(UnresolvedPatternExpr *expr) {
      // If we end up here, we should have diagnosed somewhere else
      // already.
      Expr *simplified = simplifyExprType(expr);
      // Invalidate 'VarDecl's inside the pattern.
      expr->getSubPattern()->forEachVariable([](VarDecl *VD) {
        VD->setInvalid();
      });
      if (!SuppressDiagnostics
          && !cs.getType(simplified)->is<UnresolvedType>()) {
        auto &de = cs.getASTContext().Diags;
        de.diagnose(simplified->getLoc(), diag::pattern_in_expr,
                    expr->getSubPattern()->getKind());
      }
      return simplified;
    }
    
    Expr *visitBindOptionalExpr(BindOptionalExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitOptionalEvaluationExpr(OptionalEvaluationExpr *expr) {
      Type optType = simplifyType(cs.getType(expr));

      // If this is an optional chain that isn't chaining anything, and if the
      // subexpression is already optional (not IUO), then this is a noop:
      // reject it.  This avoids confusion of the model (where the programmer
      // thought it was doing something) and keeps pointless ?'s out of the
      // code.
      if (!SuppressDiagnostics) {
        auto &de = cs.getASTContext().Diags;
        if (auto *Bind = dyn_cast<BindOptionalExpr>(
                        expr->getSubExpr()->getSemanticsProvidingExpr())) {
          if (cs.getType(Bind->getSubExpr())->isEqual(optType)) {
            de.diagnose(expr->getLoc(), diag::optional_chain_noop, optType)
                .fixItRemove(Bind->getQuestionLoc());
          } else {
            de.diagnose(expr->getLoc(), diag::optional_chain_isnt_chaining);
          }
        }
      }

      Expr *subExpr = coerceToType(expr->getSubExpr(), optType,
                                   cs.getConstraintLocator(expr));
      if (!subExpr) return nullptr;

      expr->setSubExpr(subExpr);
      cs.setType(expr, optType);
      return expr;
    }

    Expr *visitForceValueExpr(ForceValueExpr *expr) {
      // Check to see if we are forcing an
      // ImplicitlyUnwrappedFunctionConversionExpr.  This can happen
      // in cases where we had a ForceValueExpr of an optional for a
      // declaration for a function whose result type we need to
      // implicitly force after applying. We need to hoist the function
      // conversion above the ForceValueExpr, so that we may ultimately
      // hoist it above the ApplyExpr where we will eventually rewrite the
      // function conversion into a force of the result.
      Expr *replacement = expr;
      if (auto fnConv =
          dyn_cast<ImplicitlyUnwrappedFunctionConversionExpr>(expr->getSubExpr())) {
        auto fnConvSubExpr = fnConv->getSubExpr();
        auto fnConvSubObjTy =
          cs.getType(fnConvSubExpr)->getOptionalObjectType();
        cs.setType(expr, fnConvSubObjTy);
        expr->setSubExpr(fnConvSubExpr);
        fnConv->setSubExpr(expr);
        replacement = fnConv;
      }

      Type valueType = simplifyType(cs.getType(expr));
      cs.setType(expr, valueType);

      // Coerce the object type, if necessary.
      auto subExpr = expr->getSubExpr();
      if (auto objectTy = cs.getType(subExpr)->getOptionalObjectType()) {
        if (objectTy && !objectTy->isEqual(valueType)) {
          auto coercedSubExpr = coerceToType(subExpr,
                                             OptionalType::get(valueType),
                                             cs.getConstraintLocator(subExpr));
          
          expr->setSubExpr(coercedSubExpr);
        }
      }
      
      return replacement;
    }

    Expr *visitOpenExistentialExpr(OpenExistentialExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
    
    Expr *visitMakeTemporarilyEscapableExpr(MakeTemporarilyEscapableExpr *expr){
      llvm_unreachable("Already type-checked");
    }

    Expr *visitKeyPathApplicationExpr(KeyPathApplicationExpr *expr){
      // This should already be type-checked, but we may have had to re-
      // check it for failure diagnosis.
      return simplifyExprType(expr);
    }
    
    Expr *visitEnumIsCaseExpr(EnumIsCaseExpr *expr) {
      // Should already be type-checked.
      return simplifyExprType(expr);
    }

    Expr *visitLazyInitializerExpr(LazyInitializerExpr *expr) {
      llvm_unreachable("Already type-checked");
    }
    
    Expr *visitEditorPlaceholderExpr(EditorPlaceholderExpr *E) {
      simplifyExprType(E);
      auto valueType = cs.getType(E);
      assert(!valueType->hasUnresolvedType());

      auto &ctx = cs.getASTContext();
      // Synthesize a call to _undefined() of appropriate type.
      FuncDecl *undefinedDecl = ctx.getUndefined();
      if (!undefinedDecl) {
        ctx.Diags.diagnose(E->getLoc(), diag::missing_undefined_runtime);
        return nullptr;
      }
      DeclRefExpr *fnRef = new (ctx) DeclRefExpr(undefinedDecl, DeclNameLoc(),
                                                 /*Implicit=*/true);
      fnRef->setFunctionRefKind(FunctionRefKind::SingleApply);

      StringRef msg = "attempt to evaluate editor placeholder";
      Expr *argExpr = new (ctx) StringLiteralExpr(msg, E->getLoc(),
                                                  /*implicit*/true);

      Expr *callExpr = CallExpr::createImplicit(ctx, fnRef, { argExpr },
                                                { Identifier() });

      auto resultTy = TypeChecker::typeCheckExpression(
          callExpr, cs.DC, /*contextualInfo=*/{valueType, CTP_CannotFail});
      assert(resultTy && "Conversion cannot fail!");
      (void)resultTy;

      cs.cacheExprTypes(callExpr);
      E->setSemanticExpr(callExpr);
      return E;
    }

    Expr *visitObjCSelectorExpr(ObjCSelectorExpr *E) {
      // Dig out the reference to a declaration.
      Expr *subExpr = E->getSubExpr();
      ValueDecl *foundDecl = nullptr;
      while (subExpr) {
        // Declaration reference.
        if (auto declRef = dyn_cast<DeclRefExpr>(subExpr)) {
          foundDecl = declRef->getDecl();
          break;
        }

        // Constructor reference.
        if (auto ctorRef = dyn_cast<OtherConstructorDeclRefExpr>(subExpr)) {
          foundDecl = ctorRef->getDecl();
          break;
        }

        // Member reference.
        if (auto memberRef = dyn_cast<MemberRefExpr>(subExpr)) {
          foundDecl = memberRef->getMember().getDecl();
          break;
        }

        // Dynamic member reference.
        if (auto dynMemberRef = dyn_cast<DynamicMemberRefExpr>(subExpr)) {
          foundDecl = dynMemberRef->getMember().getDecl();
          break;
        }

        // Look through parentheses.
        if (auto paren = dyn_cast<ParenExpr>(subExpr)) {
          subExpr = paren->getSubExpr();
          continue;
        }

        // Look through "a.b" to "b".
        if (auto dotSyntax = dyn_cast<DotSyntaxBaseIgnoredExpr>(subExpr)) {
          subExpr = dotSyntax->getRHS();
          continue;
        }

        // Look through self-rebind expression.
        if (auto rebindSelf = dyn_cast<RebindSelfInConstructorExpr>(subExpr)) {
          subExpr = rebindSelf->getSubExpr();
          continue;
        }

        // Look through optional binding within the monadic "?".
        if (auto bind = dyn_cast<BindOptionalExpr>(subExpr)) {
          subExpr = bind->getSubExpr();
          continue;
        }

        // Look through optional evaluation of the monadic "?".
        if (auto optEval = dyn_cast<OptionalEvaluationExpr>(subExpr)) {
          subExpr = optEval->getSubExpr();
          continue;
        }

        // Look through an implicit force-value.
        if (auto force = dyn_cast<ForceValueExpr>(subExpr)) {
          subExpr = force->getSubExpr();
          continue;
        }

        // Look through implicit open-existential operations.
        if (auto open = dyn_cast<OpenExistentialExpr>(subExpr)) {
          if (open->isImplicit()) {
            subExpr = open->getSubExpr();
            continue;
          }
          break;
        }

        // Look to the referenced member in a self-application.
        if (auto selfApply = dyn_cast<SelfApplyExpr>(subExpr)) {
          subExpr = selfApply->getFn();
          continue;
        }

        // Look through implicit conversions.
        if (auto conversion = dyn_cast<ImplicitConversionExpr>(subExpr)) {
          subExpr = conversion->getSubExpr();
          continue;
        }

        // Look through explicit coercions.
        if (auto coercion = dyn_cast<CoerceExpr>(subExpr)) {
          subExpr = coercion->getSubExpr();
          continue;
        }

        break;
      }

      if (!subExpr) return nullptr;

      // If we didn't find any declaration at all, we're stuck.
      auto &de = cs.getASTContext().Diags;
      if (!foundDecl) {
        de.diagnose(E->getLoc(), diag::expr_selector_no_declaration)
            .highlight(subExpr->getSourceRange());
        return E;
      }

      // Check whether we found an entity that #selector could refer to.
      // If we found a method or initializer, check it.
      AbstractFunctionDecl *method = nullptr;
      if (auto func = dyn_cast<AbstractFunctionDecl>(foundDecl)) {
        // Methods and initializers.

        // If this isn't a method, complain.
        if (!func->getDeclContext()->isTypeContext()) {
          de.diagnose(E->getLoc(), diag::expr_selector_not_method,
                      func->getDeclContext()->isModuleScopeContext(),
                      func->getName())
              .highlight(subExpr->getSourceRange());
          de.diagnose(func, diag::decl_declared_here, func->getName());
          return E;
        }

        // Check that we requested a method.
        switch (E->getSelectorKind()) {
        case ObjCSelectorExpr::Method:
          break;

        case ObjCSelectorExpr::Getter:
        case ObjCSelectorExpr::Setter:
          // Complain that we cannot ask for the getter or setter of a
          // method.
          de.diagnose(E->getModifierLoc(),
                      diag::expr_selector_expected_property,
                      E->getSelectorKind() == ObjCSelectorExpr::Setter,
                      foundDecl->getDescriptiveKind(), foundDecl->getName())
              .fixItRemoveChars(E->getModifierLoc(),
                                E->getSubExpr()->getStartLoc());

          // Update the AST to reflect the fix.
          E->overrideObjCSelectorKind(ObjCSelectorExpr::Method, SourceLoc());
          break;
        }

        // Note the method we're referring to.
        method = func;
      } else if (auto var = dyn_cast<VarDecl>(foundDecl)) {
        // Properties.

        // If this isn't a property on a type, complain.
        if (!var->getDeclContext()->isTypeContext()) {
          de.diagnose(E->getLoc(), diag::expr_selector_not_property,
                      isa<ParamDecl>(var), var->getName())
              .highlight(subExpr->getSourceRange());
          de.diagnose(var, diag::decl_declared_here, var->getName());
          return E;
        }

        // Check that we requested a property getter or setter.
        switch (E->getSelectorKind()) {
        case ObjCSelectorExpr::Method: {
          bool isSettable = var->isSettable(cs.DC) &&
            var->isSetterAccessibleFrom(cs.DC);
          auto primaryDiag =
              de.diagnose(E->getLoc(), diag::expr_selector_expected_method,
                          isSettable, var->getName());
          primaryDiag.highlight(subExpr->getSourceRange());

          // The point at which we will insert the modifier.
          SourceLoc modifierLoc = E->getSubExpr()->getStartLoc();

          // If the property is settable, we don't know whether the
          // user wanted the getter or setter. Provide notes for each.
          if (isSettable) {
            // Flush the primary diagnostic. We have notes to add.
            primaryDiag.flush();

            // Add notes for the getter and setter, respectively.
            de.diagnose(modifierLoc, diag::expr_selector_add_modifier, false,
                        var->getName())
                .fixItInsert(modifierLoc, "getter: ");
            de.diagnose(modifierLoc, diag::expr_selector_add_modifier, true,
                        var->getName())
                .fixItInsert(modifierLoc, "setter: ");

            // Bail out now. We don't know what the user wanted, so
            // don't fill in the details.
            return E;
          }

          // The property is non-settable, so add "getter:".
          primaryDiag.fixItInsert(modifierLoc, "getter: ");
          E->overrideObjCSelectorKind(ObjCSelectorExpr::Getter, modifierLoc);
          method = var->getOpaqueAccessor(AccessorKind::Get);
          break;
        }

        case ObjCSelectorExpr::Getter:
          method = var->getOpaqueAccessor(AccessorKind::Get);
          break;

        case ObjCSelectorExpr::Setter:
          // Make sure we actually have a setter.
          if (!var->isSettable(cs.DC)) {
            de.diagnose(E->getLoc(), diag::expr_selector_property_not_settable,
                        var->getDescriptiveKind(), var->getName());
            de.diagnose(var, diag::decl_declared_here, var->getName());
            return E;
          }

          // Make sure the setter is accessible.
          if (!var->isSetterAccessibleFrom(cs.DC)) {
            de.diagnose(E->getLoc(),
                        diag::expr_selector_property_setter_inaccessible,
                        var->getDescriptiveKind(), var->getName());
            de.diagnose(var, diag::decl_declared_here, var->getName());
            return E;
          }

          method = var->getOpaqueAccessor(AccessorKind::Set);
          break;
        }
      } else {
        // Cannot reference with #selector.
        de.diagnose(E->getLoc(), diag::expr_selector_no_declaration)
            .highlight(subExpr->getSourceRange());
        de.diagnose(foundDecl, diag::decl_declared_here,
                    foundDecl->getName());
        return E;
      }
      assert(method && "Didn't find a method?");

      // The declaration we found must be exposed to Objective-C.
      if (!method->isObjC()) {
        // If the method declaration lies in a protocol and we're providing
        // a default implementation of the method through a protocol extension
        // and using it as a selector, then bail out as adding @objc to the
        // protocol might not be the right thing to do and could lead to
        // problems.
        if (auto protocolDecl = dyn_cast<ProtocolDecl>(foundDecl->getDeclContext())) {
          de.diagnose(E->getLoc(), diag::expr_selector_cannot_be_used,
                      foundDecl->getBaseName(), protocolDecl->getName());
          return E;
        }

        de.diagnose(E->getLoc(), diag::expr_selector_not_objc,
                    foundDecl->getDescriptiveKind(), foundDecl->getName())
            .highlight(subExpr->getSourceRange());
        de.diagnose(foundDecl, diag::make_decl_objc,
                    foundDecl->getDescriptiveKind())
            .fixItInsert(foundDecl->getAttributeInsertionLoc(false), "@objc ");
        return E;
      } else if (auto attr = foundDecl->getAttrs().getAttribute<ObjCAttr>()) {
        // If this attribute was inferred based on deprecated Swift 3 rules,
        // complain.
        if (attr->isSwift3Inferred() &&
            cs.getASTContext().LangOpts.WarnSwift3ObjCInference ==
                Swift3ObjCInferenceWarnings::Minimal) {
          de.diagnose(E->getLoc(), diag::expr_selector_swift3_objc_inference,
                      foundDecl->getDescriptiveKind(), foundDecl->getName(),
                      foundDecl->getDeclContext()
                          ->getSelfNominalTypeDecl()
                          ->getName())
              .highlight(subExpr->getSourceRange());
          de.diagnose(foundDecl, diag::make_decl_objc,
                      foundDecl->getDescriptiveKind())
              .fixItInsert(foundDecl->getAttributeInsertionLoc(false),
                           "@objc ");
        }
      }

      // Note which method we're referencing.
      E->setMethod(method);
      return E;
    }

    Expr *visitKeyPathExpr(KeyPathExpr *E) {
      if (E->isObjC()) {
        cs.setType(E, cs.getType(E->getObjCStringLiteralExpr()));
        return E;
      }

      simplifyExprType(E);
      
      if (cs.getType(E)->hasError())
        return E;

      // If a component is already resolved, then all of them should be
      // resolved, and we can let the expression be. This might happen when
      // re-checking a failed system for diagnostics.
      if (!E->getComponents().empty()
          && E->getComponents().front().isResolved()) {
        assert([&]{
          for (auto &c : E->getComponents())
            if (!c.isResolved())
              return false;
          return true;
        }());
        return E;
      }

      SmallVector<KeyPathExpr::Component, 4> resolvedComponents;

      // Resolve each of the components.
      bool didOptionalChain = false;
      bool isFunctionType = false;
      Type baseTy, leafTy;
      Type exprType = cs.getType(E);
      if (auto fnTy = exprType->getAs<FunctionType>()) {
        baseTy = fnTy->getParams()[0].getParameterType();
        leafTy = fnTy->getResult();
        isFunctionType = true;
      } else {
        auto keyPathTy = exprType->castTo<BoundGenericType>();
        baseTy = keyPathTy->getGenericArgs()[0];
        leafTy = keyPathTy->getGenericArgs()[1];
      }

      // Track the type of the current component. Once we finish projecting
      // through each component of the key path, we should reach the leafTy.
      auto componentTy = baseTy;
      for (unsigned i : indices(E->getComponents())) {
        auto &origComponent = E->getMutableComponents()[i];
        
        // If there were unresolved types, we may end up with a null base for
        // following components.
        if (!componentTy) {
          resolvedComponents.push_back(origComponent);
          continue;
        }

        auto kind = origComponent.getKind();
        auto componentLocator =
            cs.getConstraintLocator(E, LocatorPathElt::KeyPathComponent(i));

        // Get a locator such that it includes any additional elements to point
        // to the component's callee, e.g a SubscriptMember for a subscript
        // component.
        auto calleeLoc = cs.getCalleeLocator(componentLocator);

        bool isDynamicMember = false;
        // If this is an unresolved link, make sure we resolved it.
        if (kind == KeyPathExpr::Component::Kind::UnresolvedProperty ||
            kind == KeyPathExpr::Component::Kind::UnresolvedSubscript) {
          auto foundDecl = solution.getOverloadChoiceIfAvailable(calleeLoc);
          if (!foundDecl) {
            // If we couldn't resolve the component, leave it alone.
            resolvedComponents.push_back(origComponent);
            componentTy = origComponent.getComponentType();
            continue;
          }

          isDynamicMember =
              foundDecl->choice.getKind() ==
                  OverloadChoiceKind::DynamicMemberLookup ||
              foundDecl->choice.getKind() ==
                  OverloadChoiceKind::KeyPathDynamicMemberLookup;

          // If this was a @dynamicMemberLookup property, then we actually
          // form a subscript reference, so switch the kind.
          if (isDynamicMember) {
            kind = KeyPathExpr::Component::Kind::UnresolvedSubscript;
          }
        }

        switch (kind) {
        case KeyPathExpr::Component::Kind::UnresolvedProperty: {
          buildKeyPathPropertyComponent(solution.getOverloadChoice(calleeLoc),
                                        origComponent.getLoc(), calleeLoc,
                                        resolvedComponents);
          break;
        }
        case KeyPathExpr::Component::Kind::UnresolvedSubscript: {
          ArrayRef<Identifier> subscriptLabels;
          if (!isDynamicMember)
            subscriptLabels = origComponent.getSubscriptLabels();

          buildKeyPathSubscriptComponent(
              solution.getOverloadChoice(calleeLoc), origComponent.getLoc(),
              origComponent.getIndexExpr(), subscriptLabels, componentLocator,
              resolvedComponents);
          break;
        }
        case KeyPathExpr::Component::Kind::OptionalChain: {
          didOptionalChain = true;
          // Chaining always forces the element to be an rvalue.
          auto objectTy =
              componentTy->getWithoutSpecifierType()->getOptionalObjectType();
          if (componentTy->hasUnresolvedType() && !objectTy) {
            objectTy = componentTy;
          }
          assert(objectTy);
          
          auto loc = origComponent.getLoc();
          resolvedComponents.push_back(
              KeyPathExpr::Component::forOptionalChain(objectTy, loc));
          break;
        }
        case KeyPathExpr::Component::Kind::OptionalForce: {
          // Handle force optional when it is the first component e.g.
          // \String?.!.count
          if (resolvedComponents.empty()) {
            auto loc = origComponent.getLoc();
            auto objectTy = componentTy->getOptionalObjectType();
            assert(objectTy);

            resolvedComponents.push_back(
                KeyPathExpr::Component::forOptionalForce(objectTy, loc));
          } else {
            buildKeyPathOptionalForceComponent(resolvedComponents);
          }
          break;
        }
        case KeyPathExpr::Component::Kind::Invalid: {
          auto component = origComponent;
          component.setComponentType(leafTy);
          resolvedComponents.push_back(component);
          break;
        }
        case KeyPathExpr::Component::Kind::Identity: {
          auto component = origComponent;
          component.setComponentType(componentTy);
          resolvedComponents.push_back(component);
          break;
        }
        case KeyPathExpr::Component::Kind::Property:
        case KeyPathExpr::Component::Kind::Subscript:
        case KeyPathExpr::Component::Kind::OptionalWrap:
        case KeyPathExpr::Component::Kind::TupleElement:
          llvm_unreachable("already resolved");
          break;
        case KeyPathExpr::Component::Kind::DictionaryKey:
          llvm_unreachable("DictionaryKey only valid in #keyPath");
          break;
        }

        // Update "componentTy" with the result type of the last component.
        assert(!resolvedComponents.empty());
        componentTy = resolvedComponents.back().getComponentType();
      }
      
      // Wrap a non-optional result if there was chaining involved.
      if (didOptionalChain && componentTy &&
          !componentTy->hasUnresolvedType() &&
          !componentTy->getWithoutSpecifierType()->isEqual(leafTy)) {
        assert(leafTy->getOptionalObjectType()->isEqual(
            componentTy->getWithoutSpecifierType()));
        auto component = KeyPathExpr::Component::forOptionalWrap(leafTy);
        resolvedComponents.push_back(component);
        componentTy = leafTy;
      }

      // Set the resolved components, and cache their types.
      E->resolveComponents(cs.getASTContext(), resolvedComponents);
      cs.cacheExprTypes(E);

      // See whether there's an equivalent ObjC key path string we can produce
      // for interop purposes.
      checkAndSetObjCKeyPathString(E);
      
      // The final component type ought to line up with the leaf type of the
      // key path.
      assert(!componentTy || componentTy->hasUnresolvedType()
             || componentTy->getWithoutSpecifierType()->isEqual(leafTy));

      if (!isFunctionType)
        return E;

      // If we've gotten here, the user has used key path literal syntax to form
      // a closure. The type checker has given E a function type to indicate
      // this; we're going to change E's type to KeyPath<baseTy, leafTy> and
      // then wrap it in a larger closure expression with the appropriate type.

      // Compute KeyPath<baseTy, leafTy> and set E's type back to it.
      auto kpDecl = cs.getASTContext().getKeyPathDecl();
      auto keyPathTy =
          BoundGenericType::get(kpDecl, nullptr, { baseTy, leafTy });
      E->setType(keyPathTy);
      cs.cacheType(E);

      // To ensure side effects of the key path expression (mainly indices in
      // subscripts) are only evaluated once, we construct an outer closure,
      // which is immediately evaluated, and an inner closure, which it returns.
      // The result looks like this:
      //
      //     return "{ $kp$ in { $0[keyPath: $kp$] } }( \(E) )"

      auto &ctx = cs.getASTContext();
      auto discriminator = AutoClosureExpr::InvalidDiscriminator;

      // The inner closure.
      //
      //     let closure = "{ $0[keyPath: $kp$] }"
      //
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo closureInfo;
      auto closureTy =
          FunctionType::get({FunctionType::Param(baseTy)}, leafTy, closureInfo);
      auto closure = new (ctx)
          AutoClosureExpr(/*set body later*/nullptr, leafTy,
                          discriminator, cs.DC);
      auto param = new (ctx) ParamDecl(
          SourceLoc(),
          /*argument label*/ SourceLoc(), Identifier(),
          /*parameter name*/ SourceLoc(), ctx.getIdentifier("$0"), closure);
      param->setInterfaceType(baseTy->mapTypeOutOfContext());
      param->setSpecifier(ParamSpecifier::Default);
      param->setImplicit();

      // The outer closure.
      //
      //    let outerClosure = "{ $kp$ in \(closure) }"
      //
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo outerClosureInfo;
      auto outerClosureTy = FunctionType::get({FunctionType::Param(keyPathTy)},
                                              closureTy, outerClosureInfo);
      auto outerClosure = new (ctx)
          AutoClosureExpr(/*set body later*/nullptr, closureTy,
                          discriminator, cs.DC);
      auto outerParam =
          new (ctx) ParamDecl(SourceLoc(),
                              /*argument label*/ SourceLoc(), Identifier(),
                              /*parameter name*/ SourceLoc(),
                              ctx.getIdentifier("$kp$"), outerClosure);
      outerParam->setInterfaceType(keyPathTy->mapTypeOutOfContext());
      outerParam->setSpecifier(ParamSpecifier::Default);
      outerParam->setImplicit();

      // let paramRef = "$0"
      auto *paramRef = new (ctx)
          DeclRefExpr(param, DeclNameLoc(E->getLoc()), /*Implicit=*/true);
      paramRef->setType(baseTy);
      cs.cacheType(paramRef);

      // let outerParamRef = "$kp$"
      auto outerParamRef = new (ctx)
          DeclRefExpr(outerParam, DeclNameLoc(E->getLoc()), /*Implicit=*/true);
      outerParamRef->setType(keyPathTy);
      cs.cacheType(outerParamRef);

      // let application = "\(paramRef)[keyPath: \(outerParamRef)]"
      auto *application = new (ctx)
          KeyPathApplicationExpr(paramRef,
                                 E->getStartLoc(), outerParamRef, E->getEndLoc(),
                                 leafTy, /*implicit=*/true);
      cs.cacheType(application);

      // Finish up the inner closure.
      closure->setParameterList(ParameterList::create(ctx, {param}));
      closure->setBody(application);
      closure->setType(closureTy);
      cs.cacheType(closure);

      // Finish up the outer closure.
      outerClosure->setParameterList(ParameterList::create(ctx, {outerParam}));
      outerClosure->setBody(closure);
      outerClosure->setType(outerClosureTy);
      cs.cacheType(outerClosure);

      // let outerApply = "\( outerClosure )( \(E) )"
      auto outerApply = CallExpr::createImplicit(ctx, outerClosure, {E}, {});
      outerApply->setType(closureTy);
      cs.cacheExprTypes(outerApply);

      return coerceToType(outerApply, exprType, cs.getConstraintLocator(E));
    }

    void buildKeyPathOptionalForceComponent(
        SmallVectorImpl<KeyPathExpr::Component> &components) {
      assert(!components.empty());

      // Unwrap the last component type, preserving @lvalue-ness.
      auto optionalTy = components.back().getComponentType();
      Type objectTy;
      if (auto lvalue = optionalTy->getAs<LValueType>()) {
        objectTy = lvalue->getObjectType()->getOptionalObjectType();
        if (optionalTy->hasUnresolvedType() && !objectTy) {
          objectTy = optionalTy;
        }
        objectTy = LValueType::get(objectTy);
      } else {
        objectTy = optionalTy->getOptionalObjectType();
        if (optionalTy->hasUnresolvedType() && !objectTy) {
          objectTy = optionalTy;
        }
      }
      assert(objectTy);

      auto loc = components.back().getLoc();
      components.push_back(
          KeyPathExpr::Component::forOptionalForce(objectTy, loc));
    }

    void buildKeyPathPropertyComponent(
        const SelectedOverload &overload, SourceLoc componentLoc,
        ConstraintLocator *locator,
        SmallVectorImpl<KeyPathExpr::Component> &components) {
      auto resolvedTy = simplifyType(overload.openedType);
      if (auto *property = overload.choice.getDeclOrNull()) {
        // Key paths can only refer to properties currently.
        auto varDecl = cast<VarDecl>(property);
        // Key paths don't work with mutating-get properties.
        assert(!varDecl->isGetterMutating());
        // Key paths don't currently support static members.
        // There is a fix which diagnoses such situation already.
        assert(!varDecl->isStatic());

        // Compute the concrete reference to the member.
        auto ref = resolveConcreteDeclRef(property, locator);
        components.push_back(
            KeyPathExpr::Component::forProperty(ref, resolvedTy, componentLoc));
      } else {
        auto fieldIndex = overload.choice.getTupleIndex();
        components.push_back(KeyPathExpr::Component::forTupleElement(
            fieldIndex, resolvedTy, componentLoc));
      }

      if (shouldForceUnwrapResult(overload.choice, locator))
        buildKeyPathOptionalForceComponent(components);
    }

    void buildKeyPathSubscriptComponent(
        const SelectedOverload &overload,
        SourceLoc componentLoc, Expr *indexExpr,
        ArrayRef<Identifier> labels, ConstraintLocator *locator,
        SmallVectorImpl<KeyPathExpr::Component> &components) {
      auto subscript = cast<SubscriptDecl>(overload.choice.getDecl());
      assert(!subscript->isGetterMutating());
      auto memberLoc = cs.getCalleeLocator(locator);

      // Compute substitutions to refer to the member.
      auto ref = resolveConcreteDeclRef(subscript, memberLoc);

      // If this is a @dynamicMemberLookup reference to resolve a property
      // through the subscript(dynamicMember:) member, restore the
      // openedType and origComponent to its full reference as if the user
      // wrote out the subscript manually.
      bool forDynamicLookup =
          overload.choice.getKind() ==
              OverloadChoiceKind::DynamicMemberLookup ||
          overload.choice.getKind() ==
              OverloadChoiceKind::KeyPathDynamicMemberLookup;

      if (forDynamicLookup) {
        labels = cs.getASTContext().Id_dynamicMember;

        auto indexType = getTypeOfDynamicMemberIndex(overload);
        if (overload.choice.getKind() ==
            OverloadChoiceKind::KeyPathDynamicMemberLookup) {
          indexExpr = buildKeyPathDynamicMemberIndexExpr(
              indexType->castTo<BoundGenericType>(), componentLoc, memberLoc);
        } else {
          auto fieldName = overload.choice.getName().getBaseIdentifier().str();
          indexExpr = buildDynamicMemberLookupIndexExpr(fieldName, componentLoc,
                                                        indexType);
        }
        // Record the implicit subscript expr's parameter bindings and matching
        // direction as `coerceCallArguments` requires them.
        solution.recordSingleArgMatchingChoice(locator);
      }

      auto subscriptType =
          simplifyType(overload.openedType)->castTo<AnyFunctionType>();
      auto resolvedTy = subscriptType->getResult();

      // Coerce the indices to the type the subscript expects.
      auto *newIndexExpr = coerceCallArguments(
          indexExpr, subscriptType, ref,
          /*applyExpr*/ nullptr, labels,
          cs.getConstraintLocator(locator, ConstraintLocator::ApplyArgument),
          /*appliedPropertyWrappers*/ {});

      // We need to be able to hash the captured index values in order for
      // KeyPath itself to be hashable, so check that all of the subscript
      // index components are hashable and collect their conformances here.
      SmallVector<ProtocolConformanceRef, 4> conformances;

      auto hashable =
          cs.getASTContext().getProtocol(KnownProtocolKind::Hashable);

      auto fnType = overload.openedType->castTo<FunctionType>();
      SmallVector<Identifier, 4> newLabels;
      for (auto &param : fnType->getParams()) {
        newLabels.push_back(param.getLabel());

        auto indexType = simplifyType(param.getParameterType());
        // Index type conformance to Hashable protocol has been
        // verified by the solver, we just need to get it again
        // with all of the generic parameters resolved.
        auto hashableConformance =
          TypeChecker::conformsToProtocol(indexType, hashable,
                                          cs.DC->getParentModule());
        assert(hashableConformance);

        conformances.push_back(hashableConformance);
      }

      auto comp = KeyPathExpr::Component::forSubscriptWithPrebuiltIndexExpr(
          ref, newIndexExpr, cs.getASTContext().AllocateCopy(newLabels),
          resolvedTy, componentLoc,
          cs.getASTContext().AllocateCopy(conformances));
      components.push_back(comp);

      if (shouldForceUnwrapResult(overload.choice, locator))
        buildKeyPathOptionalForceComponent(components);
    }

    Expr *visitKeyPathDotExpr(KeyPathDotExpr *E) {
      llvm_unreachable("found KeyPathDotExpr in CSApply");
    }

    Expr *visitOneWayExpr(OneWayExpr *E) {
      auto type = simplifyType(cs.getType(E));
      return coerceToType(E->getSubExpr(), type, cs.getConstraintLocator(E));
    }

    Expr *visitTapExpr(TapExpr *E) {
      auto type = simplifyType(cs.getType(E));

      E->getVar()->setInterfaceType(type->mapTypeOutOfContext());

      cs.setType(E, type);
      E->setType(type);

      return E;
    }

    /// Interface for ExprWalker
    void walkToExprPre(Expr *expr) {
      // If we have an apply, make a note of its callee locator prior to
      // rewriting.
      if (auto *apply = dyn_cast<ApplyExpr>(expr)) {
        auto *calleeLoc = cs.getCalleeLocator(cs.getConstraintLocator(expr));
        CalleeLocators[apply] = calleeLoc;
      }
      ExprStack.push_back(expr);
    }

    Expr *walkToExprPost(Expr *expr) {
      Expr *result = visit(expr);

      assert(expr == ExprStack.back());
      ExprStack.pop_back();

      return result;
    }

    void finalize() {
      assert(ExprStack.empty());
      assert(OpenedExistentials.empty());

      auto &ctx = cs.getASTContext();

      // Look at all of the suspicious optional injections
      for (auto injection : SuspiciousOptionalInjections) {
        auto *cast = findForcedDowncast(ctx, injection->getSubExpr());
        if (!cast)
          continue;

        if (isa<ParenExpr>(injection->getSubExpr()))
          continue;

        ctx.Diags.diagnose(
            injection->getLoc(), diag::inject_forced_downcast,
            cs.getType(injection->getSubExpr())->getRValueType());
        auto exclaimLoc = cast->getExclaimLoc();
        ctx.Diags
            .diagnose(exclaimLoc, diag::forced_to_conditional_downcast,
                      cs.getType(injection)->getOptionalObjectType())
            .fixItReplace(exclaimLoc, "?");
        ctx.Diags
            .diagnose(cast->getStartLoc(), diag::silence_inject_forced_downcast)
            .fixItInsert(cast->getStartLoc(), "(")
            .fixItInsertAfter(cast->getEndLoc(), ")");
      }
    }

    /// Diagnose an optional injection that is probably not what the
    /// user wanted, because it comes from a forced downcast.
    void diagnoseOptionalInjection(InjectIntoOptionalExpr *injection) {
      // Check whether we have a forced downcast.
      auto *cast =
          findForcedDowncast(cs.getASTContext(), injection->getSubExpr());
      if (!cast)
        return;
      
      SuspiciousOptionalInjections.push_back(injection);
    }
  };
} // end anonymous namespace

ConcreteDeclRef
Solution::resolveLocatorToDecl(ConstraintLocator *locator) const {
  // Get the callee locator without looking through applies, ensuring we only
  // return a decl for a direct reference.
  auto *calleeLoc =
      constraintSystem->getCalleeLocator(locator, /*lookThroughApply*/ false);
  auto overload = getOverloadChoiceIfAvailable(calleeLoc);
  if (!overload)
    return ConcreteDeclRef();

  return resolveConcreteDeclRef(overload->choice.getDeclOrNull(), locator);
}

/// Returns the concrete callee which 'owns' the default argument at a given
/// index. This looks through inheritance for inherited default args.
static ConcreteDeclRef getDefaultArgOwner(ConcreteDeclRef owner,
                                          unsigned index) {
  auto *param = getParameterAt(owner.getDecl(), index);
  if (param->getDefaultArgumentKind() == DefaultArgumentKind::Inherited) {
    return getDefaultArgOwner(owner.getOverriddenDecl(), index);
  }
  return owner;
}

static bool canPeepholeTupleConversion(Expr *expr,
                                       ArrayRef<unsigned> sources) {
  if (!isa<TupleExpr>(expr))
    return false;

  for (unsigned i = 0, e = sources.size(); i != e; ++i) {
    if (sources[i] != i)
      return false;
  }

  return true;
}

Expr *ExprRewriter::coerceTupleToTuple(Expr *expr,
                                       TupleType *fromTuple,
                                       TupleType *toTuple,
                                       ConstraintLocatorBuilder locator,
                                       ArrayRef<unsigned> sources) {
  auto &ctx = cs.getASTContext();

  // If the input expression is a tuple expression, we can convert it in-place.
  if (canPeepholeTupleConversion(expr, sources)) {
    auto *tupleExpr = cast<TupleExpr>(expr);

    for (unsigned i = 0, e = tupleExpr->getNumElements(); i != e; ++i) {
      auto *fromElt = tupleExpr->getElement(i);

      // Actually convert the source element.
      auto toEltType = toTuple->getElementType(i);

      auto *toElt
        = coerceToType(fromElt, toEltType,
                      locator.withPathElement(
                        LocatorPathElt::TupleElement(i)));
      if (!toElt)
        return nullptr;

      tupleExpr->setElement(i, toElt);
    }

    tupleExpr->setType(toTuple);
    cs.cacheType(tupleExpr);

    return tupleExpr;
  }

  // Build a list of OpaqueValueExprs that matches the structure
  // of expr's type.
  //
  // Each OpaqueValueExpr's type is equal to the type of the
  // corresponding element of fromTuple.
  SmallVector<OpaqueValueExpr *, 4> destructured;
  for (unsigned i = 0, e = sources.size(); i != e; ++i) {
    auto fromEltType = fromTuple->getElementType(i);
    auto *opaqueElt =
        new (ctx) OpaqueValueExpr(expr->getSourceRange(), fromEltType);
    cs.cacheType(opaqueElt);
    destructured.push_back(opaqueElt);
  }

  // Convert each OpaqueValueExpr to the correct type.
  SmallVector<Expr *, 4> converted;
  SmallVector<Identifier, 4> labels;
  SmallVector<TupleTypeElt, 4> convertedElts;

  bool anythingShuffled = false;
  for (unsigned i = 0, e = sources.size(); i != e; ++i) {
    unsigned source = sources[i];
    auto *fromElt = destructured[source];

    // Actually convert the source element.
    auto toEltType = toTuple->getElementType(i);
    auto toLabel = toTuple->getElement(i).getName();

    // If we're shuffling positions and labels, we have to warn about this
    // conversion.
    if (i != sources[i] &&
        fromTuple->getElement(i).getName() != toLabel)
      anythingShuffled = true;

    auto *toElt
      = coerceToType(fromElt, toEltType,
                     locator.withPathElement(
                       LocatorPathElt::TupleElement(source)));
    if (!toElt)
      return nullptr;

    converted.push_back(toElt);
    labels.push_back(toLabel);
    convertedElts.emplace_back(toEltType, toLabel, ParameterTypeFlags());
  }

  // Shuffling tuple elements is an anti-pattern worthy of a diagnostic.  We
  // will form the shuffle for now, but a future compiler should decline to
  // do so and begin the process of removing them altogether.
  if (anythingShuffled) {
    cs.getASTContext().Diags.diagnose(
        expr->getLoc(), diag::warn_reordering_tuple_shuffle_deprecated);
  }

  // Create the result tuple, written in terms of the destructured
  // OpaqueValueExprs.
  auto *result = TupleExpr::createImplicit(ctx, converted, labels);
  result->setType(TupleType::get(convertedElts, ctx));
  cs.cacheType(result);

  // Create the tuple conversion.
  return cs.cacheType(
      DestructureTupleExpr::create(ctx, destructured, expr, result, toTuple));
}

static Type getMetatypeSuperclass(Type t) {
  if (auto *metaTy = t->getAs<MetatypeType>())
    return MetatypeType::get(getMetatypeSuperclass(
                               metaTy->getInstanceType()));

  if (auto *metaTy = t->getAs<ExistentialMetatypeType>())
    return ExistentialMetatypeType::get(getMetatypeSuperclass(
                                          metaTy->getInstanceType()));

  return t->getSuperclass();
}

Expr *ExprRewriter::coerceSuperclass(Expr *expr, Type toType) {
  auto &ctx = cs.getASTContext();

  auto fromType = cs.getType(expr);

  auto fromInstanceType = fromType;
  auto toInstanceType = toType;

  while (fromInstanceType->is<AnyMetatypeType>() &&
         toInstanceType->is<MetatypeType>()) {
    fromInstanceType = fromInstanceType->castTo<AnyMetatypeType>()
      ->getInstanceType();
    toInstanceType = toInstanceType->castTo<MetatypeType>()
      ->getInstanceType();
  }

  if (fromInstanceType->is<ArchetypeType>()) {
    // Coercion from archetype to its (concrete) superclass.
    auto superclass = getMetatypeSuperclass(fromType);

    expr =
      cs.cacheType(
        new (ctx) ArchetypeToSuperExpr(expr, superclass));

    if (!superclass->isEqual(toType))
      return coerceSuperclass(expr, toType);

    return expr;

  }

  if (fromInstanceType->isExistentialType()) {
    // Coercion from superclass-constrained existential to its
    // concrete superclass.
    auto fromArchetype = OpenedArchetypeType::getAny(fromType);

    auto *archetypeVal = cs.cacheType(new (ctx) OpaqueValueExpr(
        expr->getSourceRange(), fromArchetype));

    auto *result = coerceSuperclass(archetypeVal, toType);

    return cs.cacheType(
      new (ctx) OpenExistentialExpr(expr, archetypeVal, result,
                                           toType));
  }

  // Coercion from subclass to superclass.
  if (toType->is<MetatypeType>()) {
    return cs.cacheType(
      new (ctx) MetatypeConversionExpr(expr, toType));
  }

  return cs.cacheType(
    new (ctx) DerivedToBaseExpr(expr, toType));
}

/// Collect the conformances for all the protocols of an existential type.
/// If the source type is also existential, we don't want to check conformance
/// because most protocols do not conform to themselves -- however we still
/// allow the conversion here, except the ErasureExpr ends up with trivial
/// conformances.
static ArrayRef<ProtocolConformanceRef>
collectExistentialConformances(Type fromType, Type toType,
                               ModuleDecl *module) {
  auto layout = toType->getExistentialLayout();

  SmallVector<ProtocolConformanceRef, 4> conformances;
  for (auto proto : layout.getProtocols()) {
    conformances.push_back(TypeChecker::containsProtocol(
        fromType, proto->getDecl(), module));
  }

  return toType->getASTContext().AllocateCopy(conformances);
}

Expr *ExprRewriter::coerceExistential(Expr *expr, Type toType) {
  Type fromType = cs.getType(expr);
  Type fromInstanceType = fromType;
  Type toInstanceType = toType;

  // Look through metatypes
  while ((fromInstanceType->is<UnresolvedType>() ||
          fromInstanceType->is<AnyMetatypeType>()) &&
         toInstanceType->is<ExistentialMetatypeType>()) {
    if (!fromInstanceType->is<UnresolvedType>())
      fromInstanceType = fromInstanceType->castTo<AnyMetatypeType>()->getInstanceType();
    toInstanceType = toInstanceType->castTo<ExistentialMetatypeType>()->getInstanceType();
  }

  ASTContext &ctx = cs.getASTContext();

  auto conformances =
    collectExistentialConformances(fromInstanceType, toInstanceType,
                                   cs.DC->getParentModule());

  // For existential-to-existential coercions, open the source existential.
  if (fromType->isAnyExistentialType()) {
    fromType = OpenedArchetypeType::getAny(fromType);

    auto *archetypeVal = cs.cacheType(
        new (ctx) OpaqueValueExpr(expr->getSourceRange(), fromType));

    auto *result = cs.cacheType(ErasureExpr::create(ctx, archetypeVal, toType,
                                                    conformances));
    return cs.cacheType(
        new (ctx) OpenExistentialExpr(expr, archetypeVal, result,
                                      cs.getType(result)));
  }

  // Load tuples with lvalue elements.
  if (auto tupleType = fromType->getAs<TupleType>()) {
    if (tupleType->hasLValueType()) {
      expr = cs.coerceToRValue(expr);
    }
  }

  return cs.cacheType(ErasureExpr::create(ctx, expr, toType, conformances));
}

/// Given that the given expression is an implicit conversion added
/// to the target by coerceToType, find out how many OptionalEvaluationExprs
/// it includes and the target.
static unsigned getOptionalEvaluationDepth(Expr *expr, Expr *target) {
  unsigned depth = 0;
  while (true) {
    // Look through sugar expressions.
    expr = expr->getSemanticsProvidingExpr();

    // If we find the target expression, we're done.
    if (expr == target) return depth;

    // If we see an optional evaluation, the depth goes up.
    if (auto optEval = dyn_cast<OptionalEvaluationExpr>(expr)) {
      ++depth;
      expr = optEval->getSubExpr();

    // We have to handle any other expressions that can be introduced by
    // coerceToType.
    } else if (auto bind = dyn_cast<BindOptionalExpr>(expr)) {
      expr = bind->getSubExpr();
    } else if (auto force = dyn_cast<ForceValueExpr>(expr)) {
      expr = force->getSubExpr();
    } else if (auto open = dyn_cast<OpenExistentialExpr>(expr)) {
      depth += getOptionalEvaluationDepth(open->getSubExpr(),
                                          open->getOpaqueValue());
      expr = open->getExistentialValue();

    // Otherwise, look through implicit conversions.
    } else {
      expr = cast<ImplicitConversionExpr>(expr)->getSubExpr();
    }
  }
}

Expr *ExprRewriter::coerceOptionalToOptional(Expr *expr, Type toType,
                                             ConstraintLocatorBuilder locator,
                                          Optional<Pattern*> typeFromPattern) {
  auto &ctx = cs.getASTContext();
  Type fromType = cs.getType(expr);

  TypeChecker::requireOptionalIntrinsics(ctx, expr->getLoc());

  SmallVector<Type, 4> fromOptionals;
  (void)fromType->lookThroughAllOptionalTypes(fromOptionals);

  SmallVector<Type, 4> toOptionals;
  (void)toType->lookThroughAllOptionalTypes(toOptionals);

  assert(!toOptionals.empty());
  assert(!fromOptionals.empty());

  // If we are adding optionals but the types are equivalent up to the common
  // depth, peephole the optional-to-optional conversion into a series of nested
  // injections.
  auto toDepth = toOptionals.size();
  auto fromDepth = fromOptionals.size();
  if (toDepth > fromDepth &&
      toOptionals[toOptionals.size() - fromDepth]->isEqual(fromType)) {
    auto diff = toDepth - fromDepth;
    while (diff--) {
      Type type = toOptionals[diff];
      expr = cs.cacheType(new (ctx) InjectIntoOptionalExpr(expr, type));
      diagnoseOptionalInjection(cast<InjectIntoOptionalExpr>(expr));
    }

    return expr;
  }

  Type fromValueType = fromType->getOptionalObjectType();
  Type toValueType = toType->getOptionalObjectType();

  // The depth we use here will get patched after we apply the coercion.
  auto bindOptional =
      new (ctx) BindOptionalExpr(expr, expr->getSourceRange().End,
                                 /*depth*/ 0, fromValueType);

  expr = cs.cacheType(bindOptional);
  expr->setImplicit(true);
  expr = coerceToType(expr, toValueType, locator, typeFromPattern);
  if (!expr) return nullptr;

  unsigned depth = getOptionalEvaluationDepth(expr, bindOptional);
  bindOptional->setDepth(depth);

  expr = cs.cacheType(new (ctx) InjectIntoOptionalExpr(expr, toType));

  expr = cs.cacheType(new (ctx) OptionalEvaluationExpr(expr, toType));
  expr->setImplicit(true);
  return expr;
}

Expr *ExprRewriter::coerceImplicitlyUnwrappedOptionalToValue(Expr *expr, Type objTy) {
  auto optTy = cs.getType(expr);
  // Preserve l-valueness of the result.
  if (optTy->is<LValueType>())
    objTy = LValueType::get(objTy);

  expr = new (cs.getASTContext()) ForceValueExpr(expr, expr->getEndLoc(),
                                                 /* forcedIUO=*/ true);
  cs.setType(expr, objTy);
  expr->setImplicit();
  return expr;
}

/// Determine whether the given expression is a reference to an
/// unbound instance member of a type.
static bool isReferenceToMetatypeMember(ConstraintSystem &cs, Expr *expr) {
  expr = expr->getSemanticsProvidingExpr();
  if (auto dotIgnored = dyn_cast<DotSyntaxBaseIgnoredExpr>(expr))
    return cs.getType(dotIgnored->getLHS())->is<AnyMetatypeType>();
  if (auto dotSyntax = dyn_cast<DotSyntaxCallExpr>(expr))
    return cs.getType(dotSyntax->getBase())->is<AnyMetatypeType>();
  return false;
}

static bool hasCurriedSelf(ConstraintSystem &cs, ConcreteDeclRef callee,
                           ApplyExpr *apply) {
  // If we do not have a callee, return false.
  if (!callee) {
    return false;
  }

  // Only calls to members of types can have curried 'self'.
  auto calleeDecl = callee.getDecl();
  if (!calleeDecl->getDeclContext()->isTypeContext()) {
    return false;
  }

  // Would have `self`, if we're not applying it.
  if (auto *call = dyn_cast<CallExpr>(apply)) {
    if (!calleeDecl->isInstanceMember() ||
        !isReferenceToMetatypeMember(cs, call->getDirectCallee())) {
      return true;
    }
    return false;
  }

  // Operators have curried self.
  if (isa<PrefixUnaryExpr>(apply) || isa<PostfixUnaryExpr>(apply) ||
      isa<BinaryExpr>(apply)) {
    return true;
  }

  // Otherwise, we have a normal application.
  return false;
}

/// Apply the contextually Sendable flag to the given expression,
static void applyContextualClosureFlags(
      Expr *expr, bool sendable, bool forMainActor, bool implicitSelfCapture,
      bool inheritActorContext) {
  if (auto closure = dyn_cast<ClosureExpr>(expr)) {
    closure->setUnsafeConcurrent(sendable, forMainActor);
    closure->setAllowsImplicitSelfCapture(implicitSelfCapture);
    closure->setInheritsActorContext(inheritActorContext);
    return;
  }

  if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
    applyContextualClosureFlags(
        captureList->getClosureBody(), sendable, forMainActor,
        implicitSelfCapture, inheritActorContext);
  }

  if (auto identity = dyn_cast<IdentityExpr>(expr)) {
    applyContextualClosureFlags(
        identity->getSubExpr(), sendable, forMainActor,
        implicitSelfCapture, inheritActorContext);
  }
}

/// Whether this is a reference to a method on the main dispatch queue.
static bool isMainDispatchQueue(Expr *arg) {
  auto call = dyn_cast<DotSyntaxCallExpr>(arg);
  if (!call)
    return false;

  auto memberRef = dyn_cast<MemberRefExpr>(
      call->getBase()->getValueProvidingExpr());
  if (!memberRef)
    return false;

  auto member = memberRef->getMember();
  if (member.getDecl()->getName().getBaseName().userFacingName() != "main")
    return false;

  auto typeExpr = dyn_cast<TypeExpr>(
      memberRef->getBase()->getValueProvidingExpr());
  if (!typeExpr)
    return false;

  Type baseType = typeExpr->getInstanceType();
  if (!baseType)
    return false;

  auto baseNominal = baseType->getAnyNominal();
  if (!baseNominal)
    return false;

  return baseNominal->getName().str() == "DispatchQueue";
}


Expr *ExprRewriter::coerceCallArguments(
    Expr *arg, AnyFunctionType *funcType,
    ConcreteDeclRef callee,
    ApplyExpr *apply,
    ArrayRef<Identifier> argLabels,
    ConstraintLocatorBuilder locator,
    ArrayRef<AppliedPropertyWrapper> appliedPropertyWrappers) {
  assert(locator.last() && locator.last()->is<LocatorPathElt::ApplyArgument>());

  auto &ctx = getConstraintSystem().getASTContext();
  auto params = funcType->getParams();
  unsigned appliedWrapperIndex = 0;

  // Local function to produce a locator to refer to the given parameter.
  auto getArgLocator =
      [&](unsigned argIdx, unsigned paramIdx,
          ParameterTypeFlags flags) -> ConstraintLocatorBuilder {
    return locator.withPathElement(
        LocatorPathElt::ApplyArgToParam(argIdx, paramIdx, flags));
  };

  // Determine whether this application has curried self.
  bool skipCurriedSelf = apply ? hasCurriedSelf(cs, callee, apply) : true;
  // Determine the parameter bindings.
  ParameterListInfo paramInfo(params, callee.getDecl(), skipCurriedSelf);

  SmallVector<AnyFunctionType::Param, 8> args;
  AnyFunctionType::decomposeInput(cs.getType(arg), args);

  // If this application is an init(wrappedValue:) call that needs an injected
  // wrapped value placeholder, the first non-defaulted argument must be
  // wrapped in an OpaqueValueExpr.
  bool shouldInjectWrappedValuePlaceholder =
      target && target->shouldInjectWrappedValuePlaceholder(apply);

  auto injectWrappedValuePlaceholder =
      [&](Expr *arg, bool isAutoClosure = false) -> Expr * {
        auto *placeholder = PropertyWrapperValuePlaceholderExpr::create(
            ctx, arg->getSourceRange(), cs.getType(arg),
            target->propertyWrapperHasInitialWrappedValue() ? arg : nullptr,
            isAutoClosure);
        cs.cacheType(placeholder);
        cs.cacheType(placeholder->getOpaqueValuePlaceholder());
        shouldInjectWrappedValuePlaceholder = false;
        return placeholder;
      };

  // Quickly test if any further fix-ups for the argument types are necessary.
  if (AnyFunctionType::equalParams(args, params) &&
      !shouldInjectWrappedValuePlaceholder &&
      !paramInfo.anyContextualInfo())
    return arg;

  // Apply labels to arguments.
  AnyFunctionType::relabelParams(args, argLabels);

  auto unlabeledTrailingClosureIndex =
      arg->getUnlabeledTrailingClosureIndexOfPackedArgument();

  // Determine the parameter bindings that were applied.
  auto *locatorPtr = cs.getConstraintLocator(locator);
  assert(solution.argumentMatchingChoices.count(locatorPtr) == 1);
  auto parameterBindings = solution.argumentMatchingChoices.find(locatorPtr)
                               ->second.parameterBindings;

  // We should either have parentheses or a tuple.
  auto *argTuple = dyn_cast<TupleExpr>(arg);
  auto *argParen = dyn_cast<ParenExpr>(arg);
  // FIXME: Eventually, we want to enforce that we have either argTuple or
  // argParen here.

  SourceLoc lParenLoc, rParenLoc;
  if (argTuple) {
    lParenLoc = argTuple->getLParenLoc();
    rParenLoc = argTuple->getRParenLoc();
  } else if (argParen) {
    lParenLoc = argParen->getLParenLoc();
    rParenLoc = argParen->getRParenLoc();
  }

  // Local function to extract the ith argument expression, which papers
  // over some of the weirdness with tuples vs. parentheses.
  auto getArg = [&](unsigned i) -> Expr * {
    if (argTuple)
      return argTuple->getElement(i);
    assert(i == 0 && "Scalar only has a single argument");

    if (argParen)
      return argParen->getSubExpr();

    return arg;
  };

  auto getLabelLoc = [&](unsigned i) -> SourceLoc {
    if (argTuple)
      return argTuple->getElementNameLoc(i);

    assert(i == 0 && "Scalar only has a single argument");
    return SourceLoc();
  };

  SmallVector<Expr *, 4> newArgs;
  SmallVector<Identifier, 4> newLabels;
  SmallVector<SourceLoc, 4> newLabelLocs;
  SmallVector<AnyFunctionType::Param, 4> newParams;

  for (unsigned paramIdx = 0, numParams = parameterBindings.size();
       paramIdx != numParams; ++paramIdx) {
    // Extract the parameter.
    const auto &param = params[paramIdx];
    newLabels.push_back(param.getLabel());

    // Handle variadic parameters.
    if (param.isVariadic()) {
      assert(!param.isInOut());

      SmallVector<Expr *, 4> variadicArgs;

      // The first argument of this vararg parameter may have had a label;
      // save its location.
      auto &varargIndices = parameterBindings[paramIdx];
      if (!varargIndices.empty())
        newLabelLocs.push_back(getLabelLoc(varargIndices[0]));
      else
        newLabelLocs.push_back(SourceLoc());

      // Convert the arguments.
      for (auto argIdx : varargIndices) {
        auto arg = getArg(argIdx);
        auto argType = cs.getType(arg);

        // If the argument type exactly matches, this just works.
        if (argType->isEqual(param.getPlainType())) {
          variadicArgs.push_back(arg);
          continue;
        }

        // Convert the argument.
        auto convertedArg = coerceToType(
            arg, param.getPlainType(),
            getArgLocator(argIdx, paramIdx, param.getParameterFlags()));
        if (!convertedArg)
          return nullptr;

        // Add the converted argument.
        variadicArgs.push_back(convertedArg);
      }

      SourceLoc start, end;
      if (!variadicArgs.empty()) {
        start = variadicArgs.front()->getStartLoc();
        end = variadicArgs.back()->getEndLoc();
      }

      // Collect them into an ArrayExpr.
      auto *arrayExpr = ArrayExpr::create(ctx, start, variadicArgs, {}, end,
                                          param.getParameterType());
      arrayExpr->setImplicit();
      cs.cacheType(arrayExpr);

      // Wrap the ArrayExpr in a VarargExpansionExpr.
      auto *varargExpansionExpr = new (ctx)
          VarargExpansionExpr(arrayExpr,
                              /*implicit=*/true, arrayExpr->getType());
      cs.cacheType(varargExpansionExpr);

      newArgs.push_back(varargExpansionExpr);
      newParams.push_back(param);
      continue;
    }

    // Handle default arguments.
    if (parameterBindings[paramIdx].empty()) {
      auto owner = getDefaultArgOwner(callee, paramIdx);
      auto paramTy = param.getParameterType();
      auto *defArg = new (ctx)
          DefaultArgumentExpr(owner, paramIdx, arg->getStartLoc(), paramTy, dc);

      cs.cacheType(defArg);
      newArgs.push_back(defArg);
      newParams.push_back(param);
      newLabelLocs.push_back(SourceLoc());
      continue;
    }

    // Otherwise, we have a plain old ordinary argument.

    // Extract the argument used to initialize this parameter.
    assert(parameterBindings[paramIdx].size() == 1);
    unsigned argIdx = parameterBindings[paramIdx].front();
    auto arg = getArg(argIdx);
    auto argType = cs.getType(arg);

    // Save the original label location.
    newLabelLocs.push_back(getLabelLoc(argIdx));

    // Determine whether the parameter is unsafe Sendable or MainActor, and
    // record it as such.
    bool isUnsafeSendable = paramInfo.isUnsafeSendable(paramIdx);
    bool isMainActor = paramInfo.isUnsafeMainActor(paramIdx) ||
        (isUnsafeSendable && apply && isMainDispatchQueue(apply->getFn()));
    bool isImplicitSelfCapture = paramInfo.isImplicitSelfCapture(paramIdx);
    bool inheritsActorContext = paramInfo.inheritsActorContext(paramIdx);
    applyContextualClosureFlags(
        arg, isUnsafeSendable && contextUsesConcurrencyFeatures(dc),
        isMainActor, isImplicitSelfCapture, inheritsActorContext);

    // If the types exactly match, this is easy.
    auto paramType = param.getOldType();
    if (argType->isEqual(paramType) && !shouldInjectWrappedValuePlaceholder) {
      newArgs.push_back(arg);
      newParams.push_back(param);
      continue;
    }

    Expr *convertedArg = nullptr;
    auto argRequiresAutoClosureExpr = [&](const AnyFunctionType::Param &param,
                                          Type argType) {
      if (!param.isAutoClosure())
        return false;

      // Since it was allowed to pass function types to @autoclosure
      // parameters in Swift versions < 5, it has to be handled as
      // a regular function coversion by `coerceToType`.
      if (isAutoClosureArgument(arg)) {
        // In Swift >= 5 mode we only allow `@autoclosure` arguments
        // to be used by value if parameter would return a function
        // type (it just needs to get wrapped into autoclosure expr),
        // otherwise argument must always form a call.
        return cs.getASTContext().isSwiftVersionAtLeast(5);
      }

      return true;
    };

    if (paramInfo.hasExternalPropertyWrapper(paramIdx)) {
      auto *param = getParameterAt(callee.getDecl(), paramIdx);
      auto appliedWrapper = appliedPropertyWrappers[appliedWrapperIndex++];
      auto wrapperType = solution.simplifyType(appliedWrapper.wrapperType);
      auto initKind = appliedWrapper.initKind;

      using ValueKind = AppliedPropertyWrapperExpr::ValueKind;
      ValueKind valueKind = (initKind == PropertyWrapperInitKind::ProjectedValue ?
                             ValueKind::ProjectedValue : ValueKind::WrappedValue);

      arg = AppliedPropertyWrapperExpr::create(ctx, callee, param, arg->getStartLoc(),
                                               wrapperType, arg, valueKind);
      cs.cacheExprTypes(arg);
    }

    if (argRequiresAutoClosureExpr(param, argType)) {
      assert(!param.isInOut());

      // If parameter is an autoclosure, we need to make sure that:
      //   - argument type is coerced to parameter result type
      //   - impilict autoclosure is created to wrap argument expression
      //   - new types are propagated to constraint system
      auto *closureType = param.getPlainType()->castTo<FunctionType>();

      auto argLoc = getArgLocator(argIdx, paramIdx, param.getParameterFlags());

      arg = coerceToType(
          arg, closureType->getResult(),
          argLoc.withPathElement(ConstraintLocator::AutoclosureResult));

      if (shouldInjectWrappedValuePlaceholder) {
        // If init(wrappedValue:) takes an autoclosure, then we want
        // the effect of autoclosure forwarding of the placeholder
        // autoclosure. The only way to do this is to call the placeholder
        // autoclosure when passing it to the init.
        bool isDefaultWrappedValue =
            target->propertyWrapperHasInitialWrappedValue();
        auto *placeholder = injectWrappedValuePlaceholder(
            cs.buildAutoClosureExpr(arg, closureType, isDefaultWrappedValue),
            /*isAutoClosure=*/true);
        arg = CallExpr::createImplicit(ctx, placeholder, {}, {});
        arg->setType(closureType->getResult());
        cs.cacheType(arg);
      }

      convertedArg = cs.buildAutoClosureExpr(arg, closureType);
    } else {
      convertedArg = coerceToType(
          arg, paramType,
          getArgLocator(argIdx, paramIdx, param.getParameterFlags()));
    }

    // Perform the wrapped value placeholder injection
    if (shouldInjectWrappedValuePlaceholder)
      convertedArg = injectWrappedValuePlaceholder(convertedArg);

    if (!convertedArg)
      return nullptr;

    newArgs.push_back(convertedArg);

    // Make an effort to preserve the sugared type of the argument in the
    // case where there was no conversion, instead of using the parameter
    // type.
    newParams.emplace_back(cs.getType(convertedArg)->getInOutObjectType(),
                           param.getLabel(),
                           param.getParameterFlags());
  }

  assert(newArgs.size() == newParams.size());
  assert(newArgs.size() == newLabels.size());
  assert(newArgs.size() == newLabelLocs.size());

  // This is silly. SILGen gets confused if a 'self' parameter is wrapped
  // in a ParenExpr sometimes.
  if (!argTuple && !argParen &&
      (params[0].getValueOwnership() == ValueOwnership::Default ||
       params[0].getValueOwnership() == ValueOwnership::InOut)) {
    assert(newArgs.size() == 1);
    assert(!unlabeledTrailingClosureIndex);
    return newArgs[0];
  }

  // Rebuild the argument list, sharing as much structure as possible.
  auto paramType = AnyFunctionType::composeInput(ctx, newParams,
                                                 /*canonicalVararg=*/false);
  if (isa<ParenType>(paramType.getPointer())) {
    if (argParen) {
      // We already had a ParenExpr, so replace it's sub-expression.
      argParen->setSubExpr(newArgs[0]);
    } else {
      bool isImplicit = arg->isImplicit();
      arg = new (ctx) ParenExpr(
          lParenLoc, newArgs[0], rParenLoc,
          static_cast<bool>(unlabeledTrailingClosureIndex));
      arg->setImplicit(isImplicit);
    }
  } else {
    assert(isa<TupleType>(paramType.getPointer()));

    if (argTuple && newArgs.size() == argTuple->getNumElements()) {
      // The existing TupleExpr has the right number of elements,
      // replace them.
      for (unsigned i = 0, e = newArgs.size(); i != e; ++i) {
        argTuple->setElement(i, newArgs[i]);
      }
    } else {
      // Build a new TupleExpr, re-using source location information.
      arg = TupleExpr::create(ctx, lParenLoc, rParenLoc, newArgs, newLabels,
                              newLabelLocs, unlabeledTrailingClosureIndex,
                              /*implicit=*/arg->isImplicit());
    }
  }

  arg->setType(paramType);
  return cs.cacheType(arg);
}

static bool isClosureLiteralExpr(Expr *expr) {
  expr = expr->getSemanticsProvidingExpr();
  return (isa<CaptureListExpr>(expr) || isa<ClosureExpr>(expr));
}

/// Whether the given expression is a closure that should inherit
/// the actor context from where it was formed.
static bool closureInheritsActorContext(Expr *expr) {
  if (auto IE = dyn_cast<IdentityExpr>(expr))
    return closureInheritsActorContext(IE->getSubExpr());

  if (auto CLE = dyn_cast<CaptureListExpr>(expr))
    return closureInheritsActorContext(CLE->getClosureBody());

  if (auto CE = dyn_cast<ClosureExpr>(expr))
    return CE->inheritsActorContext();

  return false;
}

/// If the expression is an explicit closure expression (potentially wrapped in
/// IdentityExprs), change the type of the closure and identities to the
/// specified type and return true.  Otherwise, return false with no effect.
static bool applyTypeToClosureExpr(ConstraintSystem &cs,
                                   Expr *expr, Type toType) {
  // Look through identity expressions, like parens.
  if (auto IE = dyn_cast<IdentityExpr>(expr)) {
    if (!applyTypeToClosureExpr(cs, IE->getSubExpr(), toType)) return false;
    cs.setType(IE, toType);
    return true;
  }

  // Look through capture lists.
  if (auto CLE = dyn_cast<CaptureListExpr>(expr)) {
    if (!applyTypeToClosureExpr(cs, CLE->getClosureBody(), toType)) return false;
    cs.setType(CLE, toType);
    return true;
  }

  // If we found an explicit ClosureExpr, update its type.
  if (auto CE = dyn_cast<ClosureExpr>(expr)) {
    cs.setType(CE, toType);

    // If this closure isn't type-checked in its enclosing expression, write
    // the type into the ClosureExpr directly here, since the visitor won't.
    if (!shouldTypeCheckInEnclosingExpression(CE))
      CE->setType(toType);

    return true;
  }

  // Otherwise fail.
  return false;
}

// Look through sugar and DotSyntaxBaseIgnoredExprs.
static Expr *
getSemanticExprForDeclOrMemberRef(Expr *expr) {
  auto semanticExpr = expr->getSemanticsProvidingExpr();
  while (auto ignoredBase = dyn_cast<DotSyntaxBaseIgnoredExpr>(semanticExpr)){
    semanticExpr = ignoredBase->getRHS()->getSemanticsProvidingExpr();
  }
  return semanticExpr;
}

static void
maybeDiagnoseUnsupportedDifferentiableConversion(ConstraintSystem &cs,
                                                 Expr *expr,
                                                 AnyFunctionType *toType) {
  ASTContext &ctx = cs.getASTContext();
  Type fromType = cs.getType(expr);
  auto fromFnType = fromType->getAs<AnyFunctionType>();
  // Conversion between two different differentiable function types is not
  // yet supported.
  if (fromFnType->isDifferentiable() && toType->isDifferentiable() &&
      fromFnType->getDifferentiabilityKind() !=
          toType->getDifferentiabilityKind()) {
    ctx.Diags.diagnose(expr->getLoc(),
                       diag::invalid_differentiable_function_conversion_expr);
    return;
  }
  // Conversion from a non-`@differentiable` function to a `@differentiable` is
  // only allowed from a closure expression or a declaration/member reference.
  if (!fromFnType->isDifferentiable() && toType->isDifferentiable()) {
    std::function<void(Expr *)> maybeDiagnoseFunctionRef;
    maybeDiagnoseFunctionRef = [&](Expr *semanticExpr) {
      if (auto *capture = dyn_cast<CaptureListExpr>(semanticExpr))
        semanticExpr = capture->getClosureBody();
      if (isa<ClosureExpr>(semanticExpr)) return;
      if (auto *declRef = dyn_cast<DeclRefExpr>(semanticExpr)) {
        if (isa<AbstractFunctionDecl>(declRef->getDecl())) return;
        // If the referenced decl is a function parameter, the user may want
        // to change the declaration to be a '@differentiable' closure. Emit a
        // note with a fix-it.
        if (auto *paramDecl = dyn_cast<ParamDecl>(declRef->getDecl())) {
          ctx.Diags.diagnose(
              expr->getLoc(),
              diag::invalid_differentiable_function_conversion_expr);
          if (paramDecl->getType()->is<AnyFunctionType>()) {
            auto *typeRepr = paramDecl->getTypeRepr();
            while (auto *attributed = dyn_cast<AttributedTypeRepr>(typeRepr))
              typeRepr = attributed->getTypeRepr();
            std::string attributeString = "@differentiable";
            auto *funcTypeRepr = cast<FunctionTypeRepr>(typeRepr);
            auto paramListLoc = funcTypeRepr->getArgsTypeRepr()->getStartLoc();
            ctx.Diags.diagnose(paramDecl->getLoc(),
                    diag::invalid_differentiable_function_conversion_parameter,
                    attributeString)
                .highlight(paramDecl->getTypeRepr()->getSourceRange())
                .fixItInsert(paramListLoc, attributeString + " ");
          }
          return;
        }
      } else if (auto *memberRef = dyn_cast<MemberRefExpr>(semanticExpr)) {
        if (isa<FuncDecl>(memberRef->getMember().getDecl())) return;
      } else if (auto *dotSyntaxCall =
                     dyn_cast<DotSyntaxCallExpr>(semanticExpr)) {
        // Recurse on the function expression.
        auto *fnExpr = dotSyntaxCall->getFn()->getSemanticsProvidingExpr();
        maybeDiagnoseFunctionRef(fnExpr);
        return;
      } else if (auto *autoclosureExpr = dyn_cast<AutoClosureExpr>(semanticExpr)) {
        // Peer through curry thunks.
        if (auto *unwrappedFnExpr = autoclosureExpr->getUnwrappedCurryThunkExpr()) {
          maybeDiagnoseFunctionRef(unwrappedFnExpr);
          return;
        }
      }
      ctx.Diags.diagnose(expr->getLoc(),
                         diag::invalid_differentiable_function_conversion_expr);
    };
    maybeDiagnoseFunctionRef(getSemanticExprForDeclOrMemberRef(expr));
  }
}

static void
maybeDiagnoseUnsupportedFunctionConversion(ConstraintSystem &cs, Expr *expr,
                                           AnyFunctionType *toType) {
  auto &de = cs.getASTContext().Diags;
  Type fromType = cs.getType(expr);
  auto fromFnType = fromType->getAs<AnyFunctionType>();
  
  // Conversions to C function pointer type are limited. Since a C function
  // pointer captures no context, we can only do the necessary thunking or
  // codegen if the original function is a direct reference to a global function
  // or context-free closure or local function.
  if (toType->getRepresentation()
       == AnyFunctionType::Representation::CFunctionPointer) {
    // Can convert from an ABI-compatible C function pointer.
    if (fromFnType
        && fromFnType->getRepresentation()
            == AnyFunctionType::Representation::CFunctionPointer)
      return;
    
    // Can convert a decl ref to a global or local function that doesn't
    // capture context. Look through ignored bases too.
    // TODO: Look through static method applications to the type.
    auto semanticExpr = getSemanticExprForDeclOrMemberRef(expr);
    auto maybeDiagnoseFunctionRef = [&](FuncDecl *fn) {
      // TODO: We could allow static (or class final) functions too by
      // "capturing" the metatype in a thunk.
      if (fn->getDeclContext()->isTypeContext()) {
        de.diagnose(expr->getLoc(), diag::c_function_pointer_from_method);
      } else if (fn->getGenericParams()) {
        de.diagnose(expr->getLoc(),
                    diag::c_function_pointer_from_generic_function);
      }
    };
    
    if (auto declRef = dyn_cast<DeclRefExpr>(semanticExpr)) {
      if (auto fn = dyn_cast<FuncDecl>(declRef->getDecl())) {
        return maybeDiagnoseFunctionRef(fn);
      }
    }
    
    if (auto memberRef = dyn_cast<MemberRefExpr>(semanticExpr)) {
      if (auto fn = dyn_cast<FuncDecl>(memberRef->getMember().getDecl())) {
        return maybeDiagnoseFunctionRef(fn);
      }
    }

    // Unwrap closures with explicit capture lists.
    if (auto capture = dyn_cast<CaptureListExpr>(semanticExpr))
      semanticExpr = capture->getClosureBody();
    
    // Can convert a literal closure that doesn't capture context.
    if (auto closure = dyn_cast<ClosureExpr>(semanticExpr))
      return;

    // Diagnose cases like:
    //   func f() { print(w) }; func g(_ : @convention(c) () -> ()) {}
    //   let k = f; g(k) // error
    //   func m() { let x = 0; g({ print(x) }) } // error
    // (See also: [NOTE: diagnose-swift-to-c-convention-change])
    de.diagnose(expr->getLoc(),
                diag::invalid_c_function_pointer_conversion_expr);
  }
}

/// Build the conversion of an element in a collection upcast.
static Expr *buildElementConversion(ExprRewriter &rewriter,
                                    SourceRange srcRange, Type srcType,
                                    Type destType, bool bridged,
                                    ConstraintLocatorBuilder locator,
                                    Expr *element) {
  auto &cs = rewriter.getConstraintSystem();
  if (bridged &&
      TypeChecker::typeCheckCheckedCast(srcType, destType,
                                        CheckedCastContextKind::None, cs.DC,
                                        SourceLoc(), nullptr, SourceRange())
        != CheckedCastKind::Coercion) {
    if (auto conversion =
          rewriter.buildObjCBridgeExpr(element, destType, locator))
        return conversion;
  }

  return rewriter.coerceToType(element, destType, locator);
}

static CollectionUpcastConversionExpr::ConversionPair
buildOpaqueElementConversion(ExprRewriter &rewriter, SourceRange srcRange,
                             Type srcCollectionType, Type destCollectionType,
                             bool bridged, ConstraintLocatorBuilder locator,
                             unsigned typeArgIndex) {
  // We don't need this stuff unless we've got generalized casts.
  Type srcType = srcCollectionType->castTo<BoundGenericType>()
                                  ->getGenericArgs()[typeArgIndex];
  Type destType = destCollectionType->castTo<BoundGenericType>()
                                    ->getGenericArgs()[typeArgIndex];

  // Build the conversion.
  auto &cs = rewriter.getConstraintSystem();
  ASTContext &ctx = cs.getASTContext();
  auto opaque =
      rewriter.cs.cacheType(new (ctx) OpaqueValueExpr(srcRange, srcType));

  Expr *conversion = buildElementConversion(
      rewriter, srcRange, srcType, destType, bridged,
      locator.withPathElement(LocatorPathElt::GenericArgument(typeArgIndex)),
      opaque);

  return { opaque, conversion };
}

void ExprRewriter::peepholeArrayUpcast(ArrayExpr *expr, Type toType,
                                       bool bridged, Type elementType,
                                       ConstraintLocatorBuilder locator) {
  // Update the type of the array literal.
  cs.setType(expr, toType);
  // FIXME: finish{Array,Dictionary}Expr invoke cacheExprTypes after forming
  // the semantic expression for the dictionary literal, which will undo the
  // type we set here if this dictionary literal is nested unless we update
  // the expr type as well.
  expr->setType(toType);

  // Convert the elements.
  ConstraintLocatorBuilder innerLocator =
    locator.withPathElement(LocatorPathElt::GenericArgument(0));
  for (auto &element : expr->getElements()) {
    if (auto newElement = buildElementConversion(*this, expr->getLoc(),
                                                 cs.getType(element),
                                                 elementType,
                                                 bridged, innerLocator,
                                                 element)) {
      element = newElement;
    }
  }

  (void)finishArrayExpr(expr);
}

void ExprRewriter::peepholeDictionaryUpcast(DictionaryExpr *expr,
                                            Type toType, bool bridged,
                                            Type keyType, Type valueType,
                                            ConstraintLocatorBuilder locator) {
  // Update the type of the dictionary literal.
  cs.setType(expr, toType);
  // FIXME: finish{Array,Dictionary}Expr invoke cacheExprTypes after forming
  // the semantic expression for the dictionary literal, which will undo the
  // type we set here if this dictionary literal is nested unless we update
  // the expr type as well.
  expr->setType(toType);

  ConstraintLocatorBuilder valueLocator =
    locator.withPathElement(LocatorPathElt::GenericArgument(1));

  // Convert the elements.
  TupleTypeElt tupleTypeElts[2] = { keyType, valueType };
  auto tupleType = TupleType::get(tupleTypeElts, cs.getASTContext());
  for (auto element : expr->getElements()) {
    if (auto tuple = dyn_cast<TupleExpr>(element)) {
      auto key = tuple->getElement(0);
      if (auto newKey = buildElementConversion(*this, expr->getLoc(),
                                               cs.getType(key), keyType,
                                               bridged, valueLocator, key))
        tuple->setElement(0, newKey);

      auto value = tuple->getElement(1);
      if (auto newValue = buildElementConversion(*this, expr->getLoc(),
                                                 cs.getType(value), valueType,
                                                 bridged, valueLocator,
                                                 value)) {
        tuple->setElement(1, newValue);
      }

      cs.setType(tuple, tupleType);
      // FIXME: finish{Array,Dictionary}Expr invoke cacheExprTypes after forming
      // the semantic expression for the dictionary literal, which will undo the
      // type we set here if this dictionary literal is nested unless we update
      // the expr type as well.
      tuple->setType(tupleType);
    }
  }

  (void)finishDictionaryExpr(expr);
}

bool ExprRewriter::peepholeCollectionUpcast(Expr *expr, Type toType,
                                            bool bridged,
                                            ConstraintLocatorBuilder locator) {
  // Recur into parenthesized expressions.
  if (auto paren = dyn_cast<ParenExpr>(expr)) {
    // If we can't peephole the subexpression, we're done.
    if (!peepholeCollectionUpcast(paren->getSubExpr(), toType, bridged,
                                  locator))
      return false;

    // Update the type of this expression.
    auto parenTy = ParenType::get(cs.getASTContext(),
                                  cs.getType(paren->getSubExpr()));
    cs.setType(paren, parenTy);
    // FIXME: finish{Array,Dictionary}Expr invoke cacheExprTypes after forming
    // the semantic expression for the dictionary literal, which will undo the
    // type we set here if this dictionary literal is nested unless we update
    // the expr type as well.
    paren->setType(parenTy);
    return true;
  }

  // Array literals.
  if (auto arrayLiteral = dyn_cast<ArrayExpr>(expr)) {
    if (Optional<Type> elementType = ConstraintSystem::isArrayType(toType)) {
      peepholeArrayUpcast(arrayLiteral, toType, bridged, *elementType, locator);
      return true;
    }

    if (Optional<Type> elementType = ConstraintSystem::isSetType(toType)) {
      peepholeArrayUpcast(arrayLiteral, toType, bridged, *elementType, locator);
      return true;
    }

    return false;
  }

  // Dictionary literals.
  if (auto dictLiteral = dyn_cast<DictionaryExpr>(expr)) {
    if (auto elementType = ConstraintSystem::isDictionaryType(toType)) {
      peepholeDictionaryUpcast(dictLiteral, toType, bridged,
                               elementType->first, elementType->second,
                               locator);
      return true;
    }

    return false;
  }

  return false;
}

Expr *ExprRewriter::buildCollectionUpcastExpr(
                                            Expr *expr, Type toType,
                                            bool bridged,
                                            ConstraintLocatorBuilder locator) {
  if (peepholeCollectionUpcast(expr, toType, bridged, locator))
    return expr;

  ASTContext &ctx = cs.getASTContext();
  // Build the first value conversion.
  auto conv =
    buildOpaqueElementConversion(*this, expr->getLoc(), cs.getType(expr),
                                 toType, bridged, locator, 0);

  // For single-parameter collections, form the upcast.
  if (ConstraintSystem::isArrayType(toType) ||
      ConstraintSystem::isSetType(toType)) {
    return cs.cacheType(
              new (ctx) CollectionUpcastConversionExpr(expr, toType, {}, conv));
  }

  assert(ConstraintSystem::isDictionaryType(toType) &&
         "Unhandled collection upcast");

  // Build the second value conversion.
  auto conv2 =
    buildOpaqueElementConversion(*this, expr->getLoc(), cs.getType(expr),
                                 toType, bridged, locator, 1);

  return cs.cacheType(
           new (ctx) CollectionUpcastConversionExpr(expr, toType, conv, conv2));

}

Expr *ExprRewriter::buildObjCBridgeExpr(Expr *expr, Type toType,
                                        ConstraintLocatorBuilder locator) {
  Type fromType = cs.getType(expr);

  // Bridged collection casts always succeed, so we treat them as
  // collection "upcasts".
  if ((ConstraintSystem::isArrayType(fromType) &&
       ConstraintSystem::isArrayType(toType)) ||
      (ConstraintSystem::isDictionaryType(fromType) &&
       ConstraintSystem::isDictionaryType(toType)) ||
      (ConstraintSystem::isSetType(fromType) &&
       ConstraintSystem::isSetType(toType))) {
    return buildCollectionUpcastExpr(expr, toType, /*bridged=*/true, locator);
  }

  // Bridging from a Swift type to an Objective-C class type.
  if (toType->isAnyObject() ||
      (fromType->getRValueType()->isPotentiallyBridgedValueType() &&
       (toType->isBridgeableObjectType() || toType->isExistentialType()))) {
    // Bridging to Objective-C.
    Expr *objcExpr = bridgeToObjectiveC(expr, toType);
    if (!objcExpr)
      return nullptr;

    // We might have a coercion of a Swift type to a CF type toll-free
    // bridged to Objective-C.
    //
    // FIXME: Ideally we would instead have already recorded a restriction
    // when solving the constraint, and we wouldn't need to duplicate this
    // part of coerceToType() here.
    if (auto foreignClass = toType->getClassOrBoundGenericClass()) {
      if (foreignClass->getForeignClassKind() ==
            ClassDecl::ForeignKind::CFType) {
        return cs.cacheType(new (cs.getASTContext())
                                ForeignObjectConversionExpr(objcExpr, toType));
      }
    }

    return coerceToType(objcExpr, toType, locator);
  }

  // Bridging from an Objective-C class type to a Swift type.
  return forceBridgeFromObjectiveC(expr, toType);
}

Expr *ConstraintSystem::addImplicitLoadExpr(Expr *expr) {
  return TypeChecker::addImplicitLoadExpr(
      getASTContext(), expr, [this](Expr *expr) { return getType(expr); },
      [this](Expr *expr, Type type) { setType(expr, type); });
}

Expr *ExprRewriter::coerceToType(Expr *expr, Type toType,
                                 ConstraintLocatorBuilder locator,
                                 Optional<Pattern*> typeFromPattern) {
  auto &ctx = cs.getASTContext();

  // Diagnose conversions to invalid function types that couldn't be performed
  // beforehand because of placeholders.
  if (auto *fnTy = toType->getAs<FunctionType>()) {
    auto contextTy = cs.getContextualType(expr);
    if (cs.getConstraintLocator(locator)->isForContextualType() && contextTy &&
        contextTy->hasPlaceholder()) {
      bool hadError = TypeChecker::diagnoseInvalidFunctionType(
          fnTy, expr->getLoc(), None, dc, None);
      if (hadError)
        return nullptr;
    }
  }

  // The type we're converting from.
  Type fromType = cs.getType(expr);

  // If the types are already equivalent, we don't have to do anything.
  if (fromType->isEqual(toType))
    return expr;

  // If the solver recorded what we should do here, just do it immediately.
  auto knownRestriction = solution.ConstraintRestrictions.find(
                            { fromType->getCanonicalType(),
                              toType->getCanonicalType() });
  if (knownRestriction != solution.ConstraintRestrictions.end()) {
    switch (knownRestriction->second) {
    case ConversionRestrictionKind::DeepEquality: {
      if (toType->hasUnresolvedType())
        break;

      // HACK: Fix problem related to Swift 4 mode (with assertions),
      // since Swift 4 mode allows passing arguments with extra parens
      // to parameters which don't expect them, it should be supported
      // by "deep equality" type - Optional<T> e.g.
      // ```swift
      // func foo(_: (() -> Void)?) {}
      // func bar() -> ((()) -> Void)? { return nil }
      // foo(bar) // This expression should compile in Swift 3 mode
      // ```
      //
      // See also: https://bugs.swift.org/browse/SR-6796
      if (cs.getASTContext().isSwiftVersionAtLeast(4) &&
          !cs.getASTContext().isSwiftVersionAtLeast(5)) {
        auto obj1 = fromType->getOptionalObjectType();
        auto obj2 = toType->getOptionalObjectType();

        if (obj1 && obj2) {
          auto *fn1 = obj1->getAs<AnyFunctionType>();
          auto *fn2 = obj2->getAs<AnyFunctionType>();

          if (fn1 && fn2) {
            auto params1 = fn1->getParams();
            auto params2 = fn2->getParams();

            // This handles situations like argument: (()), parameter: ().
            if (params1.size() == 1 && params2.empty()) {
              auto tupleTy = params1.front().getOldType()->getAs<TupleType>();
              if (tupleTy && tupleTy->getNumElements() == 0)
                break;
            }
          }
        }
      }

      auto &err = llvm::errs();
      err << "fromType->getCanonicalType() = ";
      fromType->getCanonicalType()->dump(err);
      err << "toType->getCanonicalType() = ";
      toType->getCanonicalType()->dump(err);
      llvm_unreachable("Should be handled above");
    }

    case ConversionRestrictionKind::Superclass:
    case ConversionRestrictionKind::ExistentialMetatypeToMetatype:
      return coerceSuperclass(expr, toType);

    case ConversionRestrictionKind::Existential:
    case ConversionRestrictionKind::MetatypeToExistentialMetatype:
      return coerceExistential(expr, toType);

    case ConversionRestrictionKind::ClassMetatypeToAnyObject: {
      assert(ctx.LangOpts.EnableObjCInterop &&
             "metatypes can only be cast to objects w/ objc runtime!");
      return cs.cacheType(new (ctx) ClassMetatypeToObjectExpr(expr, toType));
    }
    case ConversionRestrictionKind::ExistentialMetatypeToAnyObject: {
      assert(ctx.LangOpts.EnableObjCInterop &&
             "metatypes can only be cast to objects w/ objc runtime!");
      return cs.cacheType(new (ctx)
                              ExistentialMetatypeToObjectExpr(expr, toType));
    }
    case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass: {
      return cs.cacheType(new (ctx) ProtocolMetatypeToObjectExpr(expr, toType));
    }
        
    case ConversionRestrictionKind::ValueToOptional: {
      auto toGenericType = toType->castTo<BoundGenericType>();
      assert(toGenericType->getDecl()->isOptionalDecl());
      TypeChecker::requireOptionalIntrinsics(cs.getASTContext(),
                                             expr->getLoc());

      Type valueType = toGenericType->getGenericArgs()[0];
      expr = coerceToType(expr, valueType, locator);
      if (!expr) return nullptr;

      auto *result =
          cs.cacheType(new (ctx) InjectIntoOptionalExpr(expr, toType));
      diagnoseOptionalInjection(result);
      return result;
    }

    case ConversionRestrictionKind::OptionalToOptional:
      return coerceOptionalToOptional(expr, toType, locator, typeFromPattern);

    case ConversionRestrictionKind::ArrayUpcast: {
      // Build the value conversion.
      return buildCollectionUpcastExpr(expr, toType, /*bridged=*/false,
                                       locator);
    }

    case ConversionRestrictionKind::HashableToAnyHashable: {
      // We want to check conformance on the rvalue, as that's what has
      // the Hashable conformance
      expr = cs.coerceToRValue(expr);

      // Find the conformance of the source type to Hashable.
      auto hashable = ctx.getProtocol(KnownProtocolKind::Hashable);
      auto conformance =
        TypeChecker::conformsToProtocol(
                        cs.getType(expr), hashable, cs.DC->getParentModule());
      assert(conformance && "must conform to Hashable");

      return cs.cacheType(
          new (ctx) AnyHashableErasureExpr(expr, toType, conformance));
    }

    case ConversionRestrictionKind::DictionaryUpcast: {
      // Build the value conversion.
      return buildCollectionUpcastExpr(expr, toType, /*bridged=*/false,
                                       locator);
    }

    case ConversionRestrictionKind::SetUpcast: {
      // Build the value conversion.
      return buildCollectionUpcastExpr(expr, toType, /*bridged=*/false, locator);
    }

    case ConversionRestrictionKind::InoutToPointer: {
      bool isOptional = false;
      Type unwrappedTy = toType;
      if (Type unwrapped = toType->getOptionalObjectType()) {
        isOptional = true;
        unwrappedTy = unwrapped;
      }
      PointerTypeKind pointerKind;
      auto toEltType = unwrappedTy->getAnyPointerElementType(pointerKind);
      assert(toEltType && "not a pointer type?"); (void) toEltType;

      TypeChecker::requirePointerArgumentIntrinsics(ctx, expr->getLoc());
      Expr *result =
          cs.cacheType(new (ctx) InOutToPointerExpr(expr, unwrappedTy));
      if (isOptional)
        result = cs.cacheType(new (ctx) InjectIntoOptionalExpr(result, toType));
      return result;
    }
    
    case ConversionRestrictionKind::ArrayToPointer: {
      bool isOptional = false;
      Type unwrappedTy = toType;
      if (Type unwrapped = toType->getOptionalObjectType()) {
        isOptional = true;
        unwrappedTy = unwrapped;
      }

      TypeChecker::requirePointerArgumentIntrinsics(ctx, expr->getLoc());
      Expr *result =
          cs.cacheType(new (ctx) ArrayToPointerExpr(expr, unwrappedTy));
      if (isOptional)
        result = cs.cacheType(new (ctx) InjectIntoOptionalExpr(result, toType));
      return result;
    }
    
    case ConversionRestrictionKind::StringToPointer: {
      bool isOptional = false;
      Type unwrappedTy = toType;
      if (Type unwrapped = toType->getOptionalObjectType()) {
        isOptional = true;
        unwrappedTy = unwrapped;
      }

      TypeChecker::requirePointerArgumentIntrinsics(ctx, expr->getLoc());
      Expr *result =
          cs.cacheType(new (ctx) StringToPointerExpr(expr, unwrappedTy));
      if (isOptional)
        result = cs.cacheType(new (ctx) InjectIntoOptionalExpr(result, toType));
      return result;
    }
    
    case ConversionRestrictionKind::PointerToPointer: {
      TypeChecker::requirePointerArgumentIntrinsics(ctx, expr->getLoc());
      Type unwrappedToTy = toType->getOptionalObjectType();

      // Optional to optional.
      if (Type unwrappedFromTy = cs.getType(expr)->getOptionalObjectType()) {
        assert(unwrappedToTy && "converting optional to non-optional");
        Expr *boundOptional = cs.cacheType(
            new (ctx) BindOptionalExpr(expr, SourceLoc(),
                                       /*depth*/ 0, unwrappedFromTy));
        Expr *converted = cs.cacheType(
            new (ctx) PointerToPointerExpr(boundOptional, unwrappedToTy));
        Expr *rewrapped =
            cs.cacheType(new (ctx) InjectIntoOptionalExpr(converted, toType));
        return cs.cacheType(new (ctx)
                                OptionalEvaluationExpr(rewrapped, toType));
      }

      // Non-optional to optional.
      if (unwrappedToTy) {
        Expr *converted =
            cs.cacheType(new (ctx) PointerToPointerExpr(expr, unwrappedToTy));
        return cs.cacheType(new (ctx)
                                InjectIntoOptionalExpr(converted, toType));
      }

      // Non-optional to non-optional.
      return cs.cacheType(new (ctx) PointerToPointerExpr(expr, toType));
    }

    case ConversionRestrictionKind::CFTollFreeBridgeToObjC: {
      auto foreignClass = fromType->getClassOrBoundGenericClass();
      auto objcType = foreignClass->getAttrs().getAttribute<ObjCBridgedAttr>()
                        ->getObjCClass()->getDeclaredInterfaceType();
      auto asObjCClass =
          cs.cacheType(new (ctx) ForeignObjectConversionExpr(expr, objcType));
      return coerceToType(asObjCClass, toType, locator);
    }

    case ConversionRestrictionKind::ObjCTollFreeBridgeToCF: {
      auto foreignClass = toType->getClassOrBoundGenericClass();
      auto objcType = foreignClass->getAttrs().getAttribute<ObjCBridgedAttr>()
                        ->getObjCClass()->getDeclaredInterfaceType();
      Expr *result = coerceToType(expr, objcType, locator);
      if (!result)
        return nullptr;

      return cs.cacheType(new (ctx)
                              ForeignObjectConversionExpr(result, toType));
    }

    case ConversionRestrictionKind::CGFloatToDouble:
    case ConversionRestrictionKind::DoubleToCGFloat: {
      auto conversionKind = knownRestriction->second;

      auto *argExpr = locator.trySimplifyToExpr();
      assert(argExpr);

      // Load the value for conversion.
      argExpr = cs.coerceToRValue(argExpr);

      auto *implicitInit =
          CallExpr::createImplicit(ctx, TypeExpr::createImplicit(toType, ctx),
                                   /*args=*/{argExpr},
                                   /*argLabels=*/{Identifier()});

      cs.cacheExprTypes(implicitInit->getFn());
      cs.setType(argExpr, fromType);
      cs.setType(implicitInit->getArg(),
                 ParenType::get(cs.getASTContext(), fromType));

      auto *callLocator = cs.getConstraintLocator(
          implicitInit, LocatorPathElt::ImplicitConversion(conversionKind));

      // HACK: Temporarily push the call expr onto the expr stack to make sure
      // we don't try to prematurely close an existential when applying the
      // curried member ref. This can be removed once existential opening is
      // refactored not to rely on the shape of the AST prior to rewriting.
      ExprStack.push_back(implicitInit);
      SWIFT_DEFER { ExprStack.pop_back(); };

      // We need to take information recorded for all conversions of this
      // kind and move it to a specific location where restriction is applied.
      {
        auto *memberLoc = solution.getConstraintLocator(
            callLocator, {ConstraintLocator::ApplyFunction,
                          ConstraintLocator::ConstructorMember});

        auto overload = solution.getOverloadChoice(cs.getConstraintLocator(
            ASTNode(), {LocatorPathElt::ImplicitConversion(conversionKind),
                        ConstraintLocator::ApplyFunction,
                        ConstraintLocator::ConstructorMember}));

        solution.overloadChoices.insert({memberLoc, overload});
      }

      // Record the implicit call's parameter bindings and match direction.
      solution.recordSingleArgMatchingChoice(callLocator);

      finishApply(implicitInit, toType, callLocator, callLocator);
      return implicitInit;
    }
    }
  }

  // Handle "from specific" coercions before "catch all" coercions.
  auto desugaredFromType = fromType->getDesugaredType();
  switch (desugaredFromType->getKind()) {
  // Coercions from an lvalue: load or perform implicit address-of. We perform
  // these coercions first because they are often the first step in a multi-step
  // coercion.
  case TypeKind::LValue: {
    auto fromLValue = cast<LValueType>(desugaredFromType);
    auto toIO = toType->getAs<InOutType>();
    if (!toIO)
      return coerceToType(cs.addImplicitLoadExpr(expr), toType, locator);

    // In an 'inout' operator like "i += 1", the operand is converted from
    // an implicit lvalue to an inout argument.
    assert(toIO->getObjectType()->isEqual(fromLValue->getObjectType()));
    return cs.cacheType(new (ctx) InOutExpr(expr->getStartLoc(), expr,
                                            toIO->getObjectType(),
                                            /*isImplicit*/ true));
  }

  // Coerce from a tuple to a tuple.
  case TypeKind::Tuple: {
    auto fromTuple = cast<TupleType>(desugaredFromType);
    auto toTuple = toType->getAs<TupleType>();
    if (!toTuple)
      break;

    if (fromTuple->hasLValueType() && !toTuple->hasLValueType())
      return coerceToType(cs.coerceToRValue(expr), toType, locator);

    SmallVector<unsigned, 4> sources;
    if (!computeTupleShuffle(fromTuple, toTuple, sources)) {
      return coerceTupleToTuple(expr, fromTuple, toTuple,
                                locator, sources);
    }
    break;
  }

  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::NestedArchetype:
  case TypeKind::OpaqueTypeArchetype:
    if (!cast<ArchetypeType>(desugaredFromType)->requiresClass())
      break;
    LLVM_FALLTHROUGH;

  // Coercion from a subclass to a superclass.
  //
  // FIXME: Can we rig things up so that we always have a Superclass
  // conversion restriction in this case?
  case TypeKind::DynamicSelf:
  case TypeKind::BoundGenericClass:
  case TypeKind::Class: {
    if (!toType->getClassOrBoundGenericClass())
      break;
    for (auto fromSuperClass = fromType->getSuperclass();
         fromSuperClass;
         fromSuperClass = fromSuperClass->getSuperclass()) {
      if (fromSuperClass->isEqual(toType)) {
        return coerceSuperclass(expr, toType);
      }
    }
    break;
  }

  // Coercion from one function type to another, this produces a
  // FunctionConversionExpr in its full generality.
  case TypeKind::Function: {
    auto fromFunc = cast<FunctionType>(desugaredFromType);
    auto toFunc = toType->getAs<FunctionType>();
    if (!toFunc)
      break;

    // Default argument generator must return escaping functions. Therefore, we
    // leave an explicit escape to noescape cast here such that SILGen can skip
    // the cast and emit a code for the escaping function.
    bool isInDefaultArgumentContext = false;
    if (auto initalizerCtx = dyn_cast<Initializer>(cs.DC))
      isInDefaultArgumentContext = (initalizerCtx->getInitializerKind() ==
                                    InitializerKind::DefaultArgument);
    auto toEI = toFunc->getExtInfo();
    assert(toType->is<FunctionType>());

    // Handle implicit conversions between non-@differentiable and
    // @differentiable functions.
    {
      auto fromEI = fromFunc->getExtInfo();
      auto isFromDifferentiable = fromEI.isDifferentiable();
      auto isToDifferentiable = toEI.isDifferentiable();
      // Handle implicit conversion from @differentiable.
      if (isFromDifferentiable && !isToDifferentiable) {
        fromFunc = fromFunc->getWithoutDifferentiability()
            ->castTo<FunctionType>();
        switch (fromEI.getDifferentiabilityKind()) {
        // TODO: Ban `Normal` and `Forward` cases.
        case DifferentiabilityKind::Normal:
        case DifferentiabilityKind::Forward:
        case DifferentiabilityKind::Reverse:
          expr = cs.cacheType(new (ctx)
              DifferentiableFunctionExtractOriginalExpr(expr, fromFunc));
          break;
        case DifferentiabilityKind::Linear:
          expr = cs.cacheType(new (ctx)
              LinearFunctionExtractOriginalExpr(expr, fromFunc));
          break;
        case DifferentiabilityKind::NonDifferentiable:
          llvm_unreachable("Cannot be NonDifferentiable");
        }
      }
      // Handle implicit conversion from non-@differentiable to @differentiable.
      maybeDiagnoseUnsupportedDifferentiableConversion(cs, expr, toFunc);
      if (!isFromDifferentiable && isToDifferentiable) {
        auto newEI =
            fromEI.intoBuilder()
                .withDifferentiabilityKind(toEI.getDifferentiabilityKind())
                .build();
        fromFunc = FunctionType::get(toFunc->getParams(), fromFunc->getResult(),
                                     newEI);
        switch (toEI.getDifferentiabilityKind()) {
        // TODO: Ban `Normal` and `Forward` cases.
        case DifferentiabilityKind::Normal:
        case DifferentiabilityKind::Forward:
        case DifferentiabilityKind::Reverse:
          expr = cs.cacheType(new (ctx)
                              DifferentiableFunctionExpr(expr, fromFunc));
          break;
        case DifferentiabilityKind::Linear:
          expr = cs.cacheType(new (ctx) LinearFunctionExpr(expr, fromFunc));
          break;
        case DifferentiabilityKind::NonDifferentiable:
          llvm_unreachable("Cannot be NonDifferentiable");
        }
      }
    }

    // If we have a ClosureExpr, then we can safely propagate @Sendable
    // to the closure without invalidating prior analysis.
    auto fromEI = fromFunc->getExtInfo();
    if (toEI.isSendable() && !fromEI.isSendable()) {
      auto newFromFuncType = fromFunc->withExtInfo(fromEI.withConcurrent());
      if (applyTypeToClosureExpr(cs, expr, newFromFuncType)) {
        fromFunc = newFromFuncType->castTo<FunctionType>();

        // Propagating the 'concurrent' bit might have satisfied the entire
        // conversion. If so, we're done, otherwise keep converting.
        if (fromFunc->isEqual(toType))
          return expr;
      }
    }

    // If we have a ClosureExpr, then we can safely propagate a global actor
    // to the closure without invalidating prior analysis.
    fromEI = fromFunc->getExtInfo();
    if (toEI.getGlobalActor() && !fromEI.getGlobalActor()) {
      auto newFromFuncType = fromFunc->withExtInfo(
          fromEI.withGlobalActor(toEI.getGlobalActor()));
      if (applyTypeToClosureExpr(cs, expr, newFromFuncType)) {
        fromFunc = newFromFuncType->castTo<FunctionType>();

        // Propagating the global actor bit might have satisfied the entire
        // conversion. If so, we're done, otherwise keep converting.
        if (fromFunc->isEqual(toType))
          return expr;
      }
    }

    /// Whether the given effect is polymorphic at this location.
    auto isEffectPolymorphic = [&](EffectKind kind) -> bool {
      auto last = locator.last();
      if (!(last && last->is<LocatorPathElt::ApplyArgToParam>()))
        return false;

      if (auto *call = getAsExpr<ApplyExpr>(locator.getAnchor())) {
        if (auto *declRef = dyn_cast<DeclRefExpr>(call->getFn())) {
          if (auto *fn = dyn_cast<AbstractFunctionDecl>(declRef->getDecl()))
            return fn->hasPolymorphicEffect(kind);
        }
      }

      return false;
    };

    // If we have a ClosureExpr, and we can safely propagate 'async' to the
    // closure, do that here.
    fromEI = fromFunc->getExtInfo();
    bool shouldPropagateAsync =
        !isEffectPolymorphic(EffectKind::Async) || closureInheritsActorContext(expr);
    if (toEI.isAsync() && !fromEI.isAsync() && shouldPropagateAsync) {
      auto newFromFuncType = fromFunc->withExtInfo(fromEI.withAsync());
      if (applyTypeToClosureExpr(cs, expr, newFromFuncType)) {
        fromFunc = newFromFuncType->castTo<FunctionType>();

        // Propagating 'async' might have satisfied the entire conversion.
        // If so, we're done, otherwise keep converting.
        if (fromFunc->isEqual(toType))
          return expr;
      }
    }

    // If we have a ClosureExpr, then we can safely propagate the 'no escape'
    // bit to the closure without invalidating prior analysis.
    fromEI = fromFunc->getExtInfo();
    if (toEI.isNoEscape() && !fromEI.isNoEscape()) {
      auto newFromFuncType = fromFunc->withExtInfo(fromEI.withNoEscape());
      if (!isInDefaultArgumentContext &&
          applyTypeToClosureExpr(cs, expr, newFromFuncType)) {
        fromFunc = newFromFuncType->castTo<FunctionType>();
        // Propagating the 'no escape' bit might have satisfied the entire
        // conversion.  If so, we're done, otherwise keep converting.
        if (fromFunc->isEqual(toType))
          return expr;
      } else if (isInDefaultArgumentContext) {
        // First apply the conversion *without* noescape attribute.
        if (!newFromFuncType->isEqual(toType)) {
          auto escapingToFuncTy =
              toFunc->withExtInfo(toEI.withNoEscape(false));
          maybeDiagnoseUnsupportedFunctionConversion(cs, expr, toFunc);
          expr = cs.cacheType(
              new (ctx) FunctionConversionExpr(expr, escapingToFuncTy));
        }
        // Apply an explict function conversion *only* for the escape to
        // noescape conversion. This conversion will be stripped by the
        // default argument generator. (We can't return a @noescape function)
        auto newExpr =
            cs.cacheType(new (ctx) FunctionConversionExpr(expr, toFunc));
        return newExpr;
      }
    }

    maybeDiagnoseUnsupportedFunctionConversion(cs, expr, toFunc);

    return cs.cacheType(new (ctx) FunctionConversionExpr(expr, toType));
  }

  // Coercions from one metatype to another.
  case TypeKind::Metatype: {
    if (auto toMeta = toType->getAs<MetatypeType>())
      return cs.cacheType(new (ctx) MetatypeConversionExpr(expr, toMeta));
    LLVM_FALLTHROUGH;
  }
  // Coercions from metatype to objects.
  case TypeKind::ExistentialMetatype: {
    auto fromMeta = cast<AnyMetatypeType>(desugaredFromType);
    if (toType->isAnyObject()) {
      assert(cs.getASTContext().LangOpts.EnableObjCInterop
             && "metatype-to-object conversion requires objc interop");
      if (fromMeta->is<MetatypeType>()) {
        assert(fromMeta->getInstanceType()->mayHaveSuperclass()
               && "metatype-to-object input should be a class metatype");
        return cs.cacheType(new (ctx) ClassMetatypeToObjectExpr(expr, toType));
      }
      
      if (fromMeta->is<ExistentialMetatypeType>()) {
        assert(fromMeta->getInstanceType()->getCanonicalType()
                       ->getExistentialLayout().requiresClass()
               && "metatype-to-object input should be a class metatype");
        return cs.cacheType(new (ctx)
                                ExistentialMetatypeToObjectExpr(expr, toType));
      }
      
      llvm_unreachable("unhandled metatype kind");
    }
    
    if (auto toClass = toType->getClassOrBoundGenericClass()) {
      if (toClass->getName() == cs.getASTContext().Id_Protocol
          && toClass->getModuleContext()->getName()
              == cs.getASTContext().Id_ObjectiveC) {
        assert(cs.getASTContext().LangOpts.EnableObjCInterop
               && "metatype-to-object conversion requires objc interop");
        assert(fromMeta->is<MetatypeType>()
               && fromMeta->getInstanceType()->is<ProtocolType>()
               && "protocol-metatype-to-Protocol only works for single "
                  "protocols");
        return cs.cacheType(new (ctx)
                                ProtocolMetatypeToObjectExpr(expr, toType));
      }
    }

    break;
  }

#define SUGARED_TYPE(Name, Parent) case TypeKind::Name:
#define BUILTIN_TYPE(Name, Parent) case TypeKind::Name:
#define UNCHECKED_TYPE(Name, Parent) case TypeKind::Name:
#define ARTIFICIAL_TYPE(Name, Parent) case TypeKind::Name:
#define TYPE(Name, Parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Error:
  case TypeKind::InOut:
  case TypeKind::Module:
  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Protocol:
  case TypeKind::ProtocolComposition:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
  case TypeKind::GenericFunction:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    break;
  }

  // "Catch all" coercions.
  auto desugaredToType = toType->getDesugaredType();
  switch (desugaredToType->getKind()) {
  // Coercions from a type to an existential type.
  case TypeKind::ExistentialMetatype:
  case TypeKind::ProtocolComposition:
  case TypeKind::Protocol:
    return coerceExistential(expr, toType);

  // Coercion to Optional<T>.
  case TypeKind::BoundGenericEnum: {
    auto toGenericType = cast<BoundGenericEnumType>(desugaredToType);
    if (!toGenericType->getDecl()->isOptionalDecl())
      break;
    TypeChecker::requireOptionalIntrinsics(ctx, expr->getLoc());

    if (cs.getType(expr)->getOptionalObjectType())
      return coerceOptionalToOptional(expr, toType, locator, typeFromPattern);

    Type valueType = toGenericType->getGenericArgs()[0];
    expr = coerceToType(expr, valueType, locator);
    if (!expr) return nullptr;

    auto *result = cs.cacheType(new (ctx) InjectIntoOptionalExpr(expr, toType));
    diagnoseOptionalInjection(result);
    return result;
  }

  case TypeKind::BoundGenericStruct: {
    auto toStruct = cast<BoundGenericStructType>(desugaredToType);
    if (!toStruct->isArray() && !toStruct->isDictionary())
      break;

    if (toStruct->getDecl() == cs.getType(expr)->getAnyNominal())
      return buildCollectionUpcastExpr(expr, toType, /*bridged=*/false,
                                       locator);

    break;
  }

#define SUGARED_TYPE(Name, Parent) case TypeKind::Name:
#define BUILTIN_TYPE(Name, Parent) case TypeKind::Name:
#define UNCHECKED_TYPE(Name, Parent) case TypeKind::Name:
#define ARTIFICIAL_TYPE(Name, Parent) case TypeKind::Name:
#define TYPE(Name, Parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Error:
  case TypeKind::Module:
  case TypeKind::Tuple:
  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::BoundGenericClass:
  case TypeKind::Metatype:
  case TypeKind::DynamicSelf:
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::NestedArchetype:
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
  case TypeKind::Function:
  case TypeKind::GenericFunction:
  case TypeKind::LValue:
  case TypeKind::InOut:
    break;
  }

  // Unresolved types come up in diagnostics for lvalue and inout types.
  if (fromType->hasUnresolvedType() || toType->hasUnresolvedType())
    return cs.cacheType(new (ctx) UnresolvedTypeConversionExpr(expr, toType));

  // Use an opaque type to abstract a value of the underlying concrete type.
  if (toType->getAs<OpaqueTypeArchetypeType>()) {
    return cs.cacheType(new (ctx) UnderlyingToOpaqueExpr(expr, toType));
  }
  
  llvm_unreachable("Unhandled coercion");
}

/// Detect whether an assignment to \c baseExpr.member in the given
/// decl context can potentially be initialization of a property wrapper.
static bool isPotentialPropertyWrapperInit(Expr *baseExpr,
                                           ValueDecl *member,
                                           DeclContext *UseDC) {
  // Member is not a wrapped property
  auto *VD = dyn_cast<VarDecl>(member);
  if (!(VD && VD->hasAttachedPropertyWrapper()))
    return false;

  // Assignment to a wrapped property can only be re-written to
  // initialization in an init.
  auto *CD = dyn_cast<ConstructorDecl>(UseDC);
  if (!CD)
    return false;

  // This is not an assignment on self
  if (!baseExpr->isSelfExprOf(CD))
    return false;

  return true;
}

/// Adjust the given type to become the self type when referring to
/// the given member.
static Type adjustSelfTypeForMember(Expr *baseExpr,
                                    Type baseTy, ValueDecl *member,
                                    DeclContext *UseDC) {
  assert(!baseTy->is<LValueType>());

  auto inOutTy = baseTy->getAs<InOutType>();
  if (!inOutTy)
    return baseTy;

  auto baseObjectTy = inOutTy->getObjectType();

  if (isa<ConstructorDecl>(member))
    return baseObjectTy;

  if (auto func = dyn_cast<FuncDecl>(member)) {
    // If 'self' is an inout type, turn the base type into an lvalue
    // type with the same qualifiers.
    if (func->isMutating())
      return baseTy;

    // Otherwise, return the rvalue type.
    return baseObjectTy;
  }

  // If the base of the access is mutable, then we may be invoking a getter or
  // setter that requires the base to be mutable.
  auto *SD = cast<AbstractStorageDecl>(member);
  bool isSettableFromHere =
      SD->isSettable(UseDC) && SD->isSetterAccessibleFrom(UseDC);

  // If neither the property's getter nor its setter are mutating,
  // the base can be an rvalue unless the assignment is potentially
  // initializing a property wrapper. If the assignment can be re-
  // written to property wrapper initialization, the base type should
  // be an lvalue.
  if (!SD->isGetterMutating() &&
      (!isSettableFromHere || !SD->isSetterMutating()) &&
      !isPotentialPropertyWrapperInit(baseExpr, member, UseDC))
    return baseObjectTy;

  if (isa<SubscriptDecl>(member))
    return baseTy;

  return LValueType::get(baseObjectTy);
}

Expr *
ExprRewriter::coerceSelfArgumentToType(Expr *expr,
                                       Type baseTy, ValueDecl *member,
                                       ConstraintLocatorBuilder locator) {
  Type toType = adjustSelfTypeForMember(expr, baseTy, member, dc);

  // If our expression already has the right type, we're done.
  Type fromType = cs.getType(expr);
  if (fromType->isEqual(toType))
    return expr;

  // If we're coercing to an rvalue type, just do it.
  auto toInOutTy = toType->getAs<InOutType>();
  if (!toInOutTy)
    return coerceToType(expr, toType, locator);

  assert(fromType->is<LValueType>() && "Can only convert lvalues to inout");

  auto &ctx = cs.getASTContext();

  // Use InOutExpr to convert it to an explicit inout argument for the
  // receiver.
  return cs.cacheType(new (ctx) InOutExpr(expr->getStartLoc(), expr, 
                                          toInOutTy->getInOutObjectType(),
                                          /*isImplicit*/ true));
}

Expr *ExprRewriter::convertLiteralInPlace(
    LiteralExpr *literal, Type type, ProtocolDecl *protocol,
    Identifier literalType, DeclName literalFuncName,
    ProtocolDecl *builtinProtocol, DeclName builtinLiteralFuncName,
    Diag<> brokenProtocolDiag, Diag<> brokenBuiltinProtocolDiag) {
  // If coercing a literal to an unresolved type, we don't try to look up the
  // witness members, just do it.
  if (type->is<UnresolvedType>()) {
    cs.setType(literal, type);
    return literal;
  }

  // Check whether this literal type conforms to the builtin protocol. If so,
  // initialize via the builtin protocol.
  if (builtinProtocol) {
    auto builtinConformance = TypeChecker::conformsToProtocol(
        type, builtinProtocol, cs.DC->getParentModule());
    if (builtinConformance) {
      // Find the witness that we'll use to initialize the type via a builtin
      // literal.
      auto witness = builtinConformance.getWitnessByName(
          type->getRValueType(), builtinLiteralFuncName);
      if (!witness || !isa<AbstractFunctionDecl>(witness.getDecl()))
        return nullptr;

      // Form a reference to the builtin conversion function.

      // Set the builtin initializer.
      dyn_cast<BuiltinLiteralExpr>(literal)->setBuiltinInitializer(witness);

      // The literal expression has this type.
      cs.setType(literal, type);

      return literal;
    }
  }

  // This literal type must conform to the (non-builtin) protocol.
  assert(protocol && "requirements should have stopped recursion");
  auto conformance = TypeChecker::conformsToProtocol(type, protocol,
                                                     cs.DC->getParentModule());
  assert(conformance && "must conform to literal protocol");

  // Dig out the literal type and perform a builtin literal conversion to it.
  if (!literalType.empty()) {
    // Extract the literal type.
    Type builtinLiteralType =
        conformance.getTypeWitnessByName(type, literalType);
    if (builtinLiteralType->hasError())
      return nullptr;

    // Perform the builtin conversion.
    if (!convertLiteralInPlace(literal, builtinLiteralType, nullptr,
                               Identifier(), DeclName(), builtinProtocol,
                               builtinLiteralFuncName, brokenProtocolDiag,
                               brokenBuiltinProtocolDiag))
      return nullptr;
  }

  // Find the witness that we'll use to initialize the literal value.
  auto witness =
      conformance.getWitnessByName(type->getRValueType(), literalFuncName);
  if (!witness || !isa<AbstractFunctionDecl>(witness.getDecl()))
    return nullptr;

  // Set the initializer.
  literal->setInitializer(witness);

  // The literal expression has this type.
  cs.setType(literal, type);

  return literal;
}

// Returns true if the given method and method type are a valid
// `@dynamicCallable` required `func dynamicallyCall` method.
static bool isValidDynamicCallableMethod(FuncDecl *method,
                                         AnyFunctionType *methodType) {
  auto &ctx = method->getASTContext();
  if (method->getBaseIdentifier() != ctx.Id_dynamicallyCall)
    return false;
  if (methodType->getParams().size() != 1)
    return false;
  auto argumentLabel = methodType->getParams()[0].getLabel();
  if (argumentLabel != ctx.Id_withArguments &&
      argumentLabel != ctx.Id_withKeywordArguments)
    return false;
  return true;
}

// Build a reference to a `callAsFunction` method.
static Expr *buildCallAsFunctionMethodRef(
    ExprRewriter &rewriter, ApplyExpr *apply, SelectedOverload selected,
    ConstraintLocator *calleeLoc) {
  assert(calleeLoc->isLastElement<LocatorPathElt::ImplicitCallAsFunction>());
  assert(cast<FuncDecl>(selected.choice.getDecl())->isCallAsFunctionMethod());

  // Create direct reference to `callAsFunction` method.
  auto *fn = apply->getFn();
  auto *arg = apply->getArg();

  // HACK: Temporarily push the fn expr onto the expr stack to make sure we
  // don't try to prematurely close an existential when applying the curried
  // member ref. This can be removed once existential opening is refactored not
  // to rely on the shape of the AST prior to rewriting.
  rewriter.ExprStack.push_back(fn);
  SWIFT_DEFER {
    rewriter.ExprStack.pop_back();
  };

  auto *declRef = rewriter.buildMemberRef(
      fn, /*dotLoc*/ SourceLoc(), selected, DeclNameLoc(arg->getStartLoc()),
      calleeLoc, calleeLoc, /*implicit*/ true, AccessSemantics::Ordinary);
  if (!declRef)
    return nullptr;
  declRef->setImplicit(apply->isImplicit());
  return declRef;
}

// Resolve `@dynamicCallable` applications.
std::pair<Expr *, Expr *>
ExprRewriter::buildDynamicCallable(ApplyExpr *apply, SelectedOverload selected,
                                   FuncDecl *method,
                                   AnyFunctionType *methodType,
                                   ConstraintLocatorBuilder loc) {
  auto &ctx = cs.getASTContext();
  auto *fn = apply->getFn();

  TupleExpr *arg = dyn_cast<TupleExpr>(apply->getArg());
  if (auto parenExpr = dyn_cast<ParenExpr>(apply->getArg()))
    arg = TupleExpr::createImplicit(ctx, parenExpr->getSubExpr(), {});

  // Get resolved `dynamicallyCall` method and verify it.
  assert(isValidDynamicCallableMethod(method, methodType));
  auto params = methodType->getParams();
  auto argumentType = params[0].getParameterType();

  // Determine which method was resolved: a `withArguments` method or a
  // `withKeywordArguments` method.
  auto argumentLabel = methodType->getParams()[0].getLabel();
  bool useKwargsMethod = argumentLabel == ctx.Id_withKeywordArguments;

  // HACK: Temporarily push the fn expr onto the expr stack to make sure we
  // don't try to prematurely close an existential when applying the curried
  // member ref. This can be removed once existential opening is refactored not
  // to rely on the shape of the AST prior to rewriting.
  ExprStack.push_back(fn);
  SWIFT_DEFER {
    ExprStack.pop_back();
  };

  // Construct expression referencing the `dynamicallyCall` method.
  auto member = buildMemberRef(fn, SourceLoc(), selected,
                               DeclNameLoc(), loc, loc,
                               /*implicit=*/true, AccessSemantics::Ordinary);

  // Construct argument to the method (either an array or dictionary
  // expression).
  Expr *argument = nullptr;
  if (!useKwargsMethod) {
    argument = ArrayExpr::create(ctx, SourceLoc(), arg->getElements(),
                                 {}, SourceLoc());
    cs.setType(argument, argumentType);
    finishArrayExpr(cast<ArrayExpr>(argument));
  } else {
    auto dictLitProto =
        ctx.getProtocol(KnownProtocolKind::ExpressibleByDictionaryLiteral);
    auto conformance =
        TypeChecker::conformsToProtocol(argumentType, dictLitProto,
                                        cs.DC->getParentModule());
    auto keyType = conformance.getTypeWitnessByName(argumentType, ctx.Id_Key);
    auto valueType =
        conformance.getTypeWitnessByName(argumentType, ctx.Id_Value);
    SmallVector<Identifier, 4> names;
    SmallVector<Expr *, 4> dictElements;
    for (unsigned i = 0, n = arg->getNumElements(); i < n; ++i) {
      Expr *labelExpr =
        new (ctx) StringLiteralExpr(arg->getElementName(i).get(),
                                    arg->getElementNameLoc(i),
                                    /*Implicit*/ true);
      cs.setType(labelExpr, keyType);
      handleStringLiteralExpr(cast<LiteralExpr>(labelExpr));

      Expr *valueExpr = coerceToType(arg->getElement(i), valueType, loc);
      assert(valueExpr && "Failed to coerce?");
      Expr *pair = TupleExpr::createImplicit(ctx, {labelExpr, valueExpr}, {});
      auto eltTypes = { TupleTypeElt(keyType), TupleTypeElt(valueType) };
      cs.setType(pair, TupleType::get(eltTypes, ctx));
      dictElements.push_back(pair);
    }
    argument = DictionaryExpr::create(ctx, SourceLoc(), dictElements, {},
                                      SourceLoc());
    cs.setType(argument, argumentType);
    finishDictionaryExpr(cast<DictionaryExpr>(argument));
  }
  argument->setImplicit();

  // Build the argument list expr.
  argument = TupleExpr::createImplicit(ctx, {argument}, {argumentLabel});
  cs.setType(argument,
             TupleType::get({TupleTypeElt(argumentType, argumentLabel)}, ctx));

  return std::make_pair(member, argument);
}

Expr *ExprRewriter::finishApply(ApplyExpr *apply, Type openedType,
                                ConstraintLocatorBuilder locator,
                                ConstraintLocatorBuilder calleeLocator) {
  auto &ctx = cs.getASTContext();

  auto *arg = apply->getArg();
  auto *fn = apply->getFn();

  auto finishApplyOfDeclWithSpecialTypeCheckingSemantics
    = [&](ApplyExpr *apply,
          ConcreteDeclRef declRef,
          Type openedType) -> Expr* {
      switch (TypeChecker::getDeclTypeCheckingSemantics(declRef.getDecl())) {
      case DeclTypeCheckingSemantics::TypeOf: {
        // Resolve into a DynamicTypeExpr.
        auto arg = apply->getArg();

        SmallVector<Identifier, 2> argLabelsScratch;

        auto &appliedWrappers = solution.appliedPropertyWrappers[calleeLocator.getAnchor()];
        auto fnType = cs.getType(fn)->getAs<FunctionType>();
        arg = coerceCallArguments(arg, fnType, declRef,
                                  apply,
                                  apply->getArgumentLabels(argLabelsScratch),
                                  locator.withPathElement(
                                    ConstraintLocator::ApplyArgument),
                                  appliedWrappers);
        if (!arg) {
          return nullptr;
        }

        if (auto tuple = dyn_cast<TupleExpr>(arg))
          arg = tuple->getElements()[0];

        auto replacement = new (ctx)
          DynamicTypeExpr(apply->getFn()->getLoc(),
                          apply->getArg()->getStartLoc(),
                          arg,
                          apply->getArg()->getEndLoc(),
                          Type());
        cs.setType(replacement, simplifyType(openedType));
        return replacement;
      }
      
      case DeclTypeCheckingSemantics::WithoutActuallyEscaping: {
        // Resolve into a MakeTemporarilyEscapableExpr.
        auto arg = cast<TupleExpr>(apply->getArg());
        assert(arg->getNumElements() == 2 && "should have two arguments");
        auto nonescaping = arg->getElements()[0];
        auto body = arg->getElements()[1];
        auto bodyTy = cs.getType(body)->getWithoutSpecifierType();
        auto bodyFnTy = bodyTy->castTo<FunctionType>();
        auto escapableParams = bodyFnTy->getParams();
        auto resultType = bodyFnTy->getResult();
        
        // The body is immediately called, so is obviously noescape.
        bodyFnTy = cast<FunctionType>(
          bodyFnTy->withExtInfo(bodyFnTy->getExtInfo().withNoEscape()));
        body = coerceToType(body, bodyFnTy, locator);
        assert(body && "can't make nonescaping?!");

        auto escapable = new (ctx)
            OpaqueValueExpr(apply->getFn()->getSourceRange(), Type());
        cs.setType(escapable, escapableParams[0].getOldType());

        auto getType = [&](Expr *E) -> Type { return cs.getType(E); };

        auto callSubExpr = CallExpr::createImplicit(ctx, body,
                                                    {escapable}, {}, getType);
        cs.cacheSubExprTypes(callSubExpr);
        cs.setType(callSubExpr->getArg(),
                   AnyFunctionType::composeInput(ctx,
                                                 escapableParams, false));
        cs.setType(callSubExpr, resultType);
        
        auto replacement = new (ctx)
          MakeTemporarilyEscapableExpr(apply->getFn()->getLoc(),
                                       apply->getArg()->getStartLoc(),
                                       nonescaping,
                                       callSubExpr,
                                       apply->getArg()->getEndLoc(),
                                       escapable,
                                       apply);
        cs.setType(replacement, resultType);
        return replacement;
      }
      
      case DeclTypeCheckingSemantics::OpenExistential: {
        // Resolve into an OpenExistentialExpr.
        auto arg = cast<TupleExpr>(apply->getArg());
        assert(arg->getNumElements() == 2 && "should have two arguments");

        auto existential = cs.coerceToRValue(arg->getElements()[0]);
        auto body = cs.coerceToRValue(arg->getElements()[1]);

        auto bodyFnTy = cs.getType(body)->castTo<FunctionType>();
        auto openedTy = getBaseType(bodyFnTy, /*wantsRValue*/ false);
        auto resultTy = bodyFnTy->getResult();

        // The body is immediately called, so is obviously noescape.
        bodyFnTy = cast<FunctionType>(
          bodyFnTy->withExtInfo(bodyFnTy->getExtInfo().withNoEscape()));
        body = coerceToType(body, bodyFnTy, locator);
        assert(body && "can't make nonescaping?!");

        auto openedInstanceTy = openedTy;
        auto existentialInstanceTy = cs.getType(existential);
        if (auto metaTy = openedTy->getAs<MetatypeType>()) {
          openedInstanceTy = metaTy->getInstanceType();
          existentialInstanceTy = existentialInstanceTy
            ->castTo<ExistentialMetatypeType>()
            ->getInstanceType();
        }
        assert(openedInstanceTy->castTo<OpenedArchetypeType>()
                   ->getOpenedExistentialType()
                   ->isEqual(existentialInstanceTy));

        auto opaqueValue =
            new (ctx) OpaqueValueExpr(apply->getSourceRange(), openedTy);
        cs.setType(opaqueValue, openedTy);

        auto getType = [&](Expr *E) -> Type { return cs.getType(E); };

        auto callSubExpr = CallExpr::createImplicit(ctx, body, {opaqueValue}, {}, getType);
        cs.cacheSubExprTypes(callSubExpr);
        cs.setType(callSubExpr, resultTy);
        
        auto replacement = new (ctx)
          OpenExistentialExpr(existential, opaqueValue, callSubExpr,
                              resultTy);
        cs.setType(replacement, resultTy);
        return replacement;
      }
      
      case DeclTypeCheckingSemantics::Normal:
        return nullptr;
      }

      llvm_unreachable("Unhandled DeclTypeCheckingSemantics in switch.");
    };

  // Resolve the callee for the application if we have one.
  ConcreteDeclRef callee;
  auto *calleeLoc = cs.getConstraintLocator(calleeLocator);
  auto overload = solution.getOverloadChoiceIfAvailable(calleeLoc);
  if (overload) {
    auto *decl = overload->choice.getDeclOrNull();
    callee = resolveConcreteDeclRef(decl, calleeLoc);
  }

  // If this is an implicit call to a `callAsFunction` method, build the
  // appropriate member reference.
  if (cs.getType(fn)->getRValueType()->isCallableNominalType(dc)) {
    fn = buildCallAsFunctionMethodRef(*this, apply, *overload, calleeLoc);
    if (!fn)
      return nullptr;
  }

  // Resolve a `@dynamicCallable` application.
  auto applyFunctionLoc =
      locator.withPathElement(ConstraintLocator::ApplyFunction);
  if (auto selected = solution.getOverloadChoiceIfAvailable(
          cs.getConstraintLocator(applyFunctionLoc))) {
    auto *method = dyn_cast<FuncDecl>(selected->choice.getDecl());
    auto methodType =
        simplifyType(selected->openedType)->getAs<AnyFunctionType>();
    if (method && methodType) {
      // Handle a call to a @dynamicCallable method.
      if (isValidDynamicCallableMethod(method, methodType))
        std::tie(fn, arg) = buildDynamicCallable(
            apply, *selected, method, methodType, applyFunctionLoc);
    }
  }

  // The function is always an rvalue.
  fn = cs.coerceToRValue(fn);

  // Resolve applications of decls with special semantics.
  if (auto declRef =
      dyn_cast<DeclRefExpr>(getSemanticExprForDeclOrMemberRef(fn))) {
    if (auto special =
        finishApplyOfDeclWithSpecialTypeCheckingSemantics(apply,
                                                          declRef->getDeclRef(),
                                                          openedType)) {
      return special;
    }
  }
  
  bool unwrapResult = false;
  if (auto *IUOFnTy = dyn_cast<ImplicitlyUnwrappedFunctionConversionExpr>(fn)) {
    unwrapResult = true;
    fn = IUOFnTy->getSubExpr();
  }

  // If we're applying a function that resulted from a covariant
  // function conversion, strip off that conversion.
  // FIXME: It would be nicer if we could build the ASTs properly in the
  // first shot.
  Type covariantResultType;
  if (auto covariant = dyn_cast<CovariantFunctionConversionExpr>(fn)) {
    // Strip off one layer of application from the covariant result.
    covariantResultType
      = cs.getType(covariant)->castTo<AnyFunctionType>()->getResult();
   
    // Use the subexpression as the function.
    fn = covariant->getSubExpr();
  }

  // An immediate application of a closure literal is always noescape.
  if (isClosureLiteralExpr(fn)) {
    if (auto fnTy = cs.getType(fn)->getAs<FunctionType>()) {
      fnTy = cast<FunctionType>(
        fnTy->withExtInfo(fnTy->getExtInfo().withNoEscape()));
      fn = coerceToType(fn, fnTy, locator);
    }
  }

  apply->setFn(fn);

  // For function application, convert the argument to the input type of
  // the function.
  SmallVector<Identifier, 2> argLabelsScratch;
  if (auto fnType = cs.getType(fn)->getAs<FunctionType>()) {
    auto &appliedWrappers = solution.appliedPropertyWrappers[calleeLocator.getAnchor()];
    arg = coerceCallArguments(arg, fnType, callee, apply,
                              apply->getArgumentLabels(argLabelsScratch),
                              locator.withPathElement(
                                  ConstraintLocator::ApplyArgument),
                              appliedWrappers);
    if (!arg) {
      return nullptr;
    }

    apply->setArg(arg);
    cs.setType(apply, fnType->getResult());

    solution.setExprTypes(apply);
    Expr *result = TypeChecker::substituteInputSugarTypeForResult(apply);
    cs.cacheExprTypes(result);

    // If we have a covariant result type, perform the conversion now.
    if (covariantResultType) {
      if (covariantResultType->is<FunctionType>())
        result = cs.cacheType(new (ctx) CovariantFunctionConversionExpr(
            result, covariantResultType));
      else
        result = cs.cacheType(new (ctx) CovariantReturnConversionExpr(
            result, covariantResultType));
    }

    // Try closing the existential, if there is one.
    closeExistential(result, locator);

    if (unwrapResult)
      return forceUnwrapResult(result);

    return result;
  }

  // FIXME: handle unwrapping everywhere else
  assert(!unwrapResult);

  // If this is an UnresolvedType in the system, preserve it.
  if (cs.getType(fn)->is<UnresolvedType>()) {
    cs.setType(apply, cs.getType(fn));
    return apply;
  }

  // We have a type constructor.
  auto metaTy = cs.getType(fn)->castTo<AnyMetatypeType>();
  auto ty = metaTy->getInstanceType();

  // If we're "constructing" a tuple type, it's simply a conversion.
  if (auto tupleTy = ty->getAs<TupleType>()) {
    // FIXME: Need an AST to represent this properly.
    return coerceToType(apply->getArg(), tupleTy, locator);
  }

  // We're constructing a value of nominal type. Look for the constructor or
  // enum element to use.
  auto *ctorLocator =
      cs.getConstraintLocator(locator, {ConstraintLocator::ApplyFunction,
                                        ConstraintLocator::ConstructorMember});
  auto selected = solution.getOverloadChoiceIfAvailable(ctorLocator);
  if (!selected) {
    assert(ty->hasError() || ty->hasUnresolvedType());
    cs.setType(apply, ty);
    return apply;
  }

  assert(ty->getNominalOrBoundGenericNominal() || ty->is<DynamicSelfType>() ||
         ty->isExistentialType() || ty->is<ArchetypeType>());

  // Consider the constructor decl reference expr 'implicit', but the
  // constructor call expr itself has the apply's 'implicitness'.
  Expr *declRef = buildMemberRef(fn, /*dotLoc=*/SourceLoc(), *selected,
                                 DeclNameLoc(fn->getEndLoc()), locator,
                                 ctorLocator, /*implicit=*/true,
                                 AccessSemantics::Ordinary);
  if (!declRef)
    return nullptr;
  declRef->setImplicit(apply->isImplicit());
  apply->setFn(declRef);

  // Tail-recur to actually call the constructor.
  return finishApply(apply, openedType, locator, ctorLocator);
}

// Return the precedence-yielding parent of 'expr', along with the index of
// 'expr' as the child of that parent. The precedence-yielding parent is the
// nearest ancestor of 'expr' which imposes a minimum precedence on 'expr'.
// Right now that just means skipping over TupleExpr instances that only exist
// to hold arguments to binary operators.
static std::pair<Expr *, unsigned> getPrecedenceParentAndIndex(Expr *expr,
                                                               Expr *rootExpr)
{
  auto parentMap = rootExpr->getParentMap();
  auto it = parentMap.find(expr);
  if (it == parentMap.end()) {
    return { nullptr, 0 };
  }
  Expr *parent = it->second;

  // Handle all cases where the answer isn't just going to be { parent, 0 }.
  if (auto tuple = dyn_cast<TupleExpr>(parent)) {
    // Get index of expression in tuple.
    auto tupleElems = tuple->getElements();
    auto elemIt = std::find(tupleElems.begin(), tupleElems.end(), expr);
    assert(elemIt != tupleElems.end() && "expr not found in parent TupleExpr");
    unsigned index = elemIt - tupleElems.begin();

    it = parentMap.find(parent);
    if (it != parentMap.end()) {
      Expr *gparent = it->second;

      // Was this tuple just constructed for a binop?
      if (isa<BinaryExpr>(gparent)) {
        return { gparent, index };
      }
    }

    // Must be a tuple literal, function arg list, collection, etc.
    return { parent, index };
  } else if (auto ifExpr = dyn_cast<IfExpr>(parent)) {
    unsigned index;
    if (expr == ifExpr->getCondExpr()) {
      index = 0;
    } else if (expr == ifExpr->getThenExpr()) {
      index = 1;
    } else if (expr == ifExpr->getElseExpr()) {
      index = 2;
    } else {
      llvm_unreachable("expr not found in parent IfExpr");
    }
    return { ifExpr, index };
  } else if (auto assignExpr = dyn_cast<AssignExpr>(parent)) {
    unsigned index;
    if (expr == assignExpr->getSrc()) {
      index = 0;
    } else if (expr == assignExpr->getDest()) {
      index = 1;
    } else {
      llvm_unreachable("expr not found in parent AssignExpr");
    }
    return { assignExpr, index };
  }

  return { parent, 0 };
}

/// Return true if, when replacing "<expr>" with "<expr> op <something>",
/// parentheses must be added around "<expr>" to allow the new operator
/// to bind correctly.
bool swift::exprNeedsParensInsideFollowingOperator(
    DeclContext *DC, Expr *expr,
    PrecedenceGroupDecl *followingPG) {
  if (expr->isInfixOperator()) {
    auto exprPG = TypeChecker::lookupPrecedenceGroupForInfixOperator(DC, expr);
    if (!exprPG) return true;

    return DC->getASTContext().associateInfixOperators(exprPG, followingPG)
             != Associativity::Left;
  }

  // We want to parenthesize a 'try?' on the LHS, but we don't care about
  // capturing the new operator inside a 'try' or 'try!'.
  if (isa<OptionalTryExpr>(expr))
    return true;

  return false;
}

/// Return true if, when replacing "<expr>" with "<expr> op <something>"
/// within the given root expression, parentheses must be added around
/// the new operator to prevent it from binding incorrectly in the
/// surrounding context.
bool swift::exprNeedsParensOutsideFollowingOperator(
    DeclContext *DC, Expr *expr, Expr *rootExpr,
    PrecedenceGroupDecl *followingPG) {
  Expr *parent;
  unsigned index;
  std::tie(parent, index) = getPrecedenceParentAndIndex(expr, rootExpr);
  if (!parent || isa<TupleExpr>(parent)) {
    return false;
  }

  if (auto parenExp = dyn_cast<ParenExpr>(parent))
    if (!parenExp->isImplicit())
      return false;

  if (parent->isInfixOperator()) {
    auto parentPG = TypeChecker::lookupPrecedenceGroupForInfixOperator(DC,
                                                                       parent);
    if (!parentPG) return true;

    // If the index is 0, this is on the LHS of the parent.
    auto &Context = DC->getASTContext();
    if (index == 0) {
      return Context.associateInfixOperators(followingPG, parentPG)
               != Associativity::Left;
    } else {
      return Context.associateInfixOperators(parentPG, followingPG)
               != Associativity::Right;
    }
  }

  return true;
}

bool swift::exprNeedsParensBeforeAddingNilCoalescing(DeclContext *DC,
                                                     Expr *expr) {
  auto &ctx = DC->getASTContext();
  auto asPG = TypeChecker::lookupPrecedenceGroup(
                  DC, ctx.Id_NilCoalescingPrecedence, SourceLoc())
                  .getSingle();
  if (!asPG)
    return true;
  return exprNeedsParensInsideFollowingOperator(DC, expr, asPG);
}

bool swift::exprNeedsParensAfterAddingNilCoalescing(DeclContext *DC,
                                                    Expr *expr,
                                                    Expr *rootExpr) {
  auto &ctx = DC->getASTContext();
  auto asPG = TypeChecker::lookupPrecedenceGroup(
                  DC, ctx.Id_NilCoalescingPrecedence, SourceLoc())
                  .getSingle();
  if (!asPG)
    return true;
  return exprNeedsParensOutsideFollowingOperator(DC, expr, rootExpr, asPG);
}

namespace {
  class SetExprTypes : public ASTWalker {
    const Solution &solution;

  public:
    explicit SetExprTypes(const Solution &solution)
        : solution(solution) {}

    Expr *walkToExprPost(Expr *expr) override {
      auto &cs = solution.getConstraintSystem();
      auto exprType = cs.getType(expr);
      exprType = solution.simplifyType(exprType);
      // assert((!expr->getType() || expr->getType()->isEqual(exprType)) &&
      //       "Mismatched types!");
      assert(!exprType->hasTypeVariable() &&
             "Should not write type variable into expression!");
      assert(!exprType->hasPlaceholder() &&
             "Should not write type placeholders into expression!");
      expr->setType(exprType);

      if (auto kp = dyn_cast<KeyPathExpr>(expr)) {
        for (auto i : indices(kp->getComponents())) {
          Type componentType;
          if (cs.hasType(kp, i)) {
            componentType = solution.simplifyType(cs.getType(kp, i));
            assert(!componentType->hasTypeVariable() &&
                   "Should not write type variable into key-path component");
            assert(!componentType->hasPlaceholder() &&
                   "Should not write type placeholder into key-path component");
            kp->getMutableComponents()[i].setComponentType(componentType);
          }
        }
      }

      return expr;
    }

    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }
  };

  class ExprWalker : public ASTWalker {
    ExprRewriter &Rewriter;
    SmallVector<ClosureExpr *, 4> ClosuresToTypeCheck;
    SmallVector<std::pair<TapExpr *, DeclContext *>, 4> TapsToTypeCheck;

  public:
    ExprWalker(ExprRewriter &Rewriter) : Rewriter(Rewriter) { }

    bool shouldWalkIntoPropertyWrapperPlaceholderValue() override {
      // Property wrapper placeholder underlying values are filled in
      // with already-type-checked expressions. Don't walk into them.
      return false;
    }

    const SmallVectorImpl<ClosureExpr *> &getClosuresToTypeCheck() const {
      return ClosuresToTypeCheck;
    }

    const SmallVectorImpl<std::pair<TapExpr *, DeclContext *>> &getTapsToTypeCheck() const {
      return TapsToTypeCheck;
    }

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // For closures, update the parameter types and check the body.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        rewriteFunction(closure);

        if (AnyFunctionRef(closure).hasExternalPropertyWrapperParameters()) {
          return { false, rewriteClosure(closure) };
        }

        return { false, closure };
      }

      if (auto tap = dyn_cast_or_null<TapExpr>(expr)) {
        // We remember the DeclContext because the code to handle
        // single-expression-body closures above changes it.
        TapsToTypeCheck.push_back(std::make_pair(tap, Rewriter.dc));
      }

      if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
        // Rewrite captures.
        for (const auto &capture : captureList->getCaptureList()) {
          (void)rewriteTarget(SolutionApplicationTarget(capture.PBD));
        }
      }

      Rewriter.walkToExprPre(expr);
      return { true, expr };
    }

    Expr *walkToExprPost(Expr *expr) override {
      return Rewriter.walkToExprPost(expr);
    }

    /// Ignore statements.
    std::pair<bool, Stmt *> walkToStmtPre(Stmt *stmt) override {
      return { false, stmt };
    }

    /// Ignore declarations.
    bool walkToDeclPre(Decl *decl) override { return false; }

    /// Rewrite the target, producing a new target.
    Optional<SolutionApplicationTarget>
    rewriteTarget(SolutionApplicationTarget target);

    AutoClosureExpr *rewriteClosure(ClosureExpr *closure) {
      auto &solution = Rewriter.solution;
      auto closureType = solution.simplifyType(solution.getType(closure));
      FunctionType *closureFnType = closureType->castTo<FunctionType>();

      // Apply types to synthesized property wrapper vars.
      for (auto *param : *closure->getParameters()) {
        if (!param->hasAttachedPropertyWrapper())
          continue;

        // Set the interface type of each property wrapper synthesized var
        auto *backingVar = param->getPropertyWrapperBackingProperty();
        auto backingType =
            solution.simplifyType(solution.getType(backingVar))->mapTypeOutOfContext();
        backingVar->setInterfaceType(backingType);

        if (auto *projectionVar = param->getPropertyWrapperProjectionVar()) {
          projectionVar->setInterfaceType(
              solution.simplifyType(solution.getType(projectionVar)));
        }

        auto *wrappedValueVar = param->getPropertyWrapperWrappedValueVar();
        auto wrappedValueType = solution.simplifyType(solution.getType(wrappedValueVar));
        wrappedValueVar->setInterfaceType(wrappedValueType->getWithoutSpecifierType());

        if (param->hasImplicitPropertyWrapper()) {
          if (wrappedValueType->is<LValueType>())
            wrappedValueVar->setImplInfo(StorageImplInfo::getMutableComputed());

          // Add an explicit property wrapper attribute, which is needed for
          // synthesizing the accessors.
          auto &context = wrappedValueVar->getASTContext();
          auto *typeExpr = TypeExpr::createImplicit(backingType, context);
          auto *attr = CustomAttr::create(context, SourceLoc(), typeExpr, /*implicit=*/true);
          wrappedValueVar->getAttrs().add(attr);
        }
      }

      TypeChecker::checkParameterList(closure->getParameters(), closure);

      auto &appliedWrappers = Rewriter.solution.appliedPropertyWrappers[closure];
      return Rewriter.buildPropertyWrapperFnThunk(closure, closureFnType, closure,
                                                  ConcreteDeclRef(), appliedWrappers);
    }

    /// Rewrite the function for the given solution.
    ///
    /// \returns true if an error occurred.
    bool rewriteFunction(AnyFunctionRef fn) {
      auto result = Rewriter.cs.applySolution(
          Rewriter.solution, fn, Rewriter.dc,
          [&](SolutionApplicationTarget target) {
            auto resultTarget = rewriteTarget(target);
            if (resultTarget) {
              if (auto expr = resultTarget->getAsExpr())
                Rewriter.solution.setExprTypes(expr);
            }

            return resultTarget;
          });

      switch (result) {
      case SolutionApplicationToFunctionResult::Success: {
        if (auto closure = dyn_cast_or_null<ClosureExpr>(
                fn.getAbstractClosureExpr()))
          TypeChecker::checkClosureAttributes(closure);
        return false;
      }

      case SolutionApplicationToFunctionResult::Failure:
        return true;

      case SolutionApplicationToFunctionResult::Delay: {
        if (!Rewriter.cs.Options
                .contains(ConstraintSystemFlags::LeaveClosureBodyUnchecked)) {
          auto closure = cast<ClosureExpr>(fn.getAbstractClosureExpr());
          ClosuresToTypeCheck.push_back(closure);
        }
        return false;
      }
      }
    }
  };
} // end anonymous namespace

Expr *ConstraintSystem::coerceToRValue(Expr *expr) {
  return TypeChecker::coerceToRValue(
      getASTContext(), expr, [&](Expr *expr) { return getType(expr); },
      [&](Expr *expr, Type type) { setType(expr, type); });
}

namespace {
  /// Function object to compare source locations, putting invalid
  /// locations at the end.
  class CompareExprSourceLocs {
    SourceManager &sourceMgr;

  public:
    explicit CompareExprSourceLocs(SourceManager &sourceMgr)
      : sourceMgr(sourceMgr) { }

    bool operator()(ASTNode lhs, ASTNode rhs) const {
      if (static_cast<bool>(lhs) != static_cast<bool>(rhs)) {
        return static_cast<bool>(lhs);
      }

      auto lhsLoc = getLoc(lhs);
      auto rhsLoc = getLoc(rhs);
      if (lhsLoc.isValid() != rhsLoc.isValid())
        return lhsLoc.isValid();

      return sourceMgr.isBeforeInBuffer(lhsLoc, rhsLoc);
    }
  };

}

/// Emit the fixes computed as part of the solution, returning true if we were
/// able to emit an error message, or false if none of the fixits worked out.
bool ConstraintSystem::applySolutionFixes(const Solution &solution) {
  /// Collect the fixes on a per-expression basis.
  llvm::SmallDenseMap<ASTNode, SmallVector<ConstraintFix *, 4>> fixesPerAnchor;
  for (auto *fix : solution.Fixes) {
    fixesPerAnchor[fix->getAnchor()].push_back(fix);
  }

  // Collect all of the expressions that have fixes, and sort them by
  // source ordering.
  SmallVector<ASTNode, 4> orderedAnchors;
  for (const auto &fix : fixesPerAnchor) {
    orderedAnchors.push_back(fix.getFirst());
  }
  std::sort(orderedAnchors.begin(), orderedAnchors.end(),
            CompareExprSourceLocs(Context.SourceMgr));

  // Walk over each of the expressions, diagnosing fixes.
  bool diagnosedAnyErrors = false;

  for (auto anchor : orderedAnchors) {
    // Coalesce fixes with the same locator to avoid duplicating notes.
    auto fixes = fixesPerAnchor[anchor];

    using ConstraintFixVector = llvm::SmallVector<ConstraintFix *, 4>;
    llvm::SmallMapVector<ConstraintLocator *,
        llvm::SmallMapVector<FixKind, ConstraintFixVector, 4>, 4> aggregatedFixes;
    for (auto *fix : fixes)
      aggregatedFixes[fix->getLocator()][fix->getKind()].push_back(fix);

    for (auto fixesPerLocator : aggregatedFixes) {
      for (auto fixesPerKind : fixesPerLocator.second) {
        auto fixes = fixesPerKind.second;
        auto *primaryFix = fixes[0];
        ArrayRef<ConstraintFix *> secondaryFixes{fixes.begin() + 1, fixes.end()};

        auto diagnosed =
            primaryFix->coalesceAndDiagnose(solution, secondaryFixes);
        if (primaryFix->isWarning()) {
          assert(diagnosed && "warnings should always be diagnosed");
          (void)diagnosed;
        } else {
          diagnosedAnyErrors |= diagnosed;
        }
      }
    }
  }

  return diagnosedAnyErrors;
}

/// For the initializer of an `async let`, wrap it in an autoclosure and then
/// a call to that autoclosure, so that the code for the child task is captured
/// entirely within the autoclosure. This captures the semantics of the
/// operation but not the timing, e.g., the call itself will actually occur
/// when one of the variables in the async let is referenced.
static Expr *wrapAsyncLetInitializer(
      ConstraintSystem &cs, Expr *initializer, DeclContext *dc) {
  // Form the autoclosure type. It is always 'async', and will be 'throws'.
  Type initializerType = initializer->getType();
  bool throws = TypeChecker::canThrow(initializer);
  auto extInfo = ASTExtInfoBuilder()
    .withAsync()
    .withConcurrent()
    .withThrows(throws)
    .build();

  // Form the autoclosure expression. The actual closure here encapsulates the
  // child task.
  auto closureType = FunctionType::get({ }, initializerType, extInfo);
  ASTContext &ctx = dc->getASTContext();
  Expr *autoclosureExpr = cs.buildAutoClosureExpr(
      initializer, closureType, /*isDefaultWrappedValue=*/false,
      /*isAsyncLetWrapper=*/true);

  // Call the autoclosure so that the AST types line up. SILGen will ignore the
  // actual calls and translate them into a different mechanism.
  auto autoclosureCall = CallExpr::createImplicit(
      ctx, autoclosureExpr, { }, { });
  autoclosureCall->setType(initializerType);
  autoclosureCall->setThrows(throws);

  // For a throwing expression, wrap the call in a 'try'.
  Expr *resultInit = autoclosureCall;
  if (throws) {
    resultInit = new (ctx) TryExpr(
        SourceLoc(), resultInit, initializerType, /*implicit=*/true);
  }

  // Wrap the call in an 'await'.
  resultInit = new (ctx) AwaitExpr(
      SourceLoc(), resultInit, initializerType, /*implicit=*/true);

  cs.cacheExprTypes(resultInit);
  return resultInit;
}

/// Apply the given solution to the initialization target.
///
/// \returns the resulting initialization expression.
static Optional<SolutionApplicationTarget> applySolutionToInitialization(
    Solution &solution, SolutionApplicationTarget target,
    Expr *initializer) {
  auto wrappedVar = target.getInitializationWrappedVar();
  Type initType;
  if (wrappedVar) {
    initType = solution.getType(initializer);
  } else {
    initType = solution.getType(target.getInitializationPattern());
  }

  {
    // Figure out what type the constraints decided on.
    auto ty = solution.simplifyType(initType);
    initType = ty->getRValueType()->reconstituteSugar(/*recursive =*/false);
  }

  // Convert the initializer to the type of the pattern.
  auto &cs = solution.getConstraintSystem();
  auto locator = cs.getConstraintLocator(
      initializer, LocatorPathElt::ContextualType(CTP_Initialization));
  initializer = solution.coerceToType(initializer, initType, locator);
  if (!initializer)
    return None;

  SolutionApplicationTarget resultTarget = target;
  resultTarget.setExpr(initializer);

  // Record the property wrapper type and note that the initializer has
  // been subsumed by the backing property.
  if (wrappedVar) {
    ASTContext &ctx = cs.getASTContext();
    wrappedVar->getParentPatternBinding()->setInitializerSubsumed(0);
    ctx.setSideCachedPropertyWrapperBackingPropertyType(
        wrappedVar, initType->mapTypeOutOfContext());

    // Record the semantic initializer on the outermost property wrapper.
    wrappedVar->getAttachedPropertyWrappers().front()
        ->setSemanticInit(initializer);
  }

  // Coerce the pattern to the type of the initializer.
  TypeResolutionOptions options =
      isa<EditorPlaceholderExpr>(initializer->getSemanticsProvidingExpr())
      ? TypeResolverContext::EditorPlaceholderExpr
      : target.getInitializationPatternBindingDecl()
        ? TypeResolverContext::PatternBindingDecl
        : TypeResolverContext::InExpression;
  options |= TypeResolutionFlags::OverrideType;

  // Determine the type of the pattern.
  Type finalPatternType = initializer->getType();
  if (wrappedVar) {
    if (!finalPatternType->hasError() && !finalPatternType->is<TypeVariableType>())
      finalPatternType = computeWrappedValueType(wrappedVar, finalPatternType);
  }

  if (finalPatternType->hasDependentMember())
    return None;

  finalPatternType = finalPatternType->reconstituteSugar(/*recursive =*/false);

  // Apply the solution to the pattern as well.
  auto contextualPattern = target.getContextualPattern();
  if (auto coercedPattern = TypeChecker::coercePatternToType(
          contextualPattern, finalPatternType, options)) {
    resultTarget.setPattern(coercedPattern);
  } else {
    return None;
  }

  // For an async let, wrap the initializer appropriately to make it a child
  // task.
  if (auto patternBinding = target.getInitializationPatternBindingDecl()) {
    if (patternBinding->isAsyncLet()) {
      resultTarget.setExpr(
          wrapAsyncLetInitializer(
            cs, resultTarget.getAsExpr(), resultTarget.getDeclContext()));
    }
  }

  return resultTarget;
}

/// Apply the given solution to the for-each statement target.
///
/// \returns the resulting initialization expression.
static Optional<SolutionApplicationTarget> applySolutionToForEachStmt(
    Solution &solution, SolutionApplicationTarget target, Expr *sequence) {
  auto resultTarget = target;
  auto &forEachStmtInfo = resultTarget.getForEachStmtInfo();

  // Simplify the various types.
  forEachStmtInfo.elementType =
      solution.simplifyType(forEachStmtInfo.elementType);
  forEachStmtInfo.iteratorType =
      solution.simplifyType(forEachStmtInfo.iteratorType);
  forEachStmtInfo.initType =
      solution.simplifyType(forEachStmtInfo.initType);
  forEachStmtInfo.sequenceType =
      solution.simplifyType(forEachStmtInfo.sequenceType);

  // Coerce the sequence to the sequence type.
  auto &cs = solution.getConstraintSystem();
  auto locator = cs.getConstraintLocator(target.getAsExpr());
  sequence = solution.coerceToType(
     sequence, forEachStmtInfo.sequenceType, locator);
  if (!sequence)
    return None;

  resultTarget.setExpr(sequence);

  // Get the conformance of the sequence type to the Sequence protocol.
  auto stmt = forEachStmtInfo.stmt;
  auto sequenceProto = TypeChecker::getProtocol(
      cs.getASTContext(), stmt->getForLoc(), 
      stmt->getAwaitLoc().isValid() ? 
        KnownProtocolKind::AsyncSequence : KnownProtocolKind::Sequence);
  auto sequenceConformance = TypeChecker::conformsToProtocol(
      forEachStmtInfo.sequenceType, sequenceProto, cs.DC->getParentModule());
  assert(!sequenceConformance.isInvalid() &&
         "Couldn't find sequence conformance");

  // Coerce the pattern to the element type.
  TypeResolutionOptions options(TypeResolverContext::ForEachStmt);
  options |= TypeResolutionFlags::OverrideType;

  // Apply the solution to the pattern as well.
  auto contextualPattern = target.getContextualPattern();
  if (auto coercedPattern = TypeChecker::coercePatternToType(
          contextualPattern, forEachStmtInfo.initType, options)) {
    resultTarget.setPattern(coercedPattern);
  } else {
    return None;
  }

  // Apply the solution to the filtering condition, if there is one.
  auto dc = target.getDeclContext();
  if (forEachStmtInfo.whereExpr) {
    auto *boolDecl = dc->getASTContext().getBoolDecl();
    assert(boolDecl);
    Type boolType = boolDecl->getDeclaredInterfaceType();
    assert(boolType);

    SolutionApplicationTarget whereTarget(
        forEachStmtInfo.whereExpr, dc, CTP_Condition, boolType,
        /*isDiscarded=*/false);
    auto newWhereTarget = cs.applySolution(solution, whereTarget);
    if (!newWhereTarget)
      return None;

    forEachStmtInfo.whereExpr = newWhereTarget->getAsExpr();
  }

  // Invoke iterator() to get an iterator from the sequence.
  ASTContext &ctx = cs.getASTContext();
  VarDecl *iterator;
  Type nextResultType = OptionalType::get(forEachStmtInfo.elementType);
  {
    // Create a local variable to capture the iterator.
    std::string name;
    if (auto np = dyn_cast_or_null<NamedPattern>(stmt->getPattern()))
      name = "$"+np->getBoundName().str().str();
    name += "$generator";

    iterator = new (ctx) VarDecl(
        /*IsStatic*/ false, VarDecl::Introducer::Var, stmt->getInLoc(),
        ctx.getIdentifier(name), dc);
    iterator->setInterfaceType(
        forEachStmtInfo.iteratorType->mapTypeOutOfContext());
    iterator->setImplicit();

    auto genPat = new (ctx) NamedPattern(iterator);
    genPat->setImplicit();

    // TODO: test/DebugInfo/iteration.swift requires this extra info to
    // be around.
    PatternBindingDecl::createImplicit(
        ctx, StaticSpellingKind::None, genPat,
        new (ctx) OpaqueValueExpr(stmt->getInLoc(), nextResultType),
        dc, /*VarLoc*/ stmt->getForLoc());
  }

  // Create the iterator variable.
  auto *varRef = TypeChecker::buildCheckedRefExpr(
      iterator, dc, DeclNameLoc(stmt->getInLoc()), /*implicit*/ true);

  // Convert that Optional<Element> value to the type of the pattern.
  auto optPatternType = OptionalType::get(forEachStmtInfo.initType);
  if (!optPatternType->isEqual(nextResultType)) {
    OpaqueValueExpr *elementExpr = new (ctx) OpaqueValueExpr(
        stmt->getInLoc(), nextResultType->getOptionalObjectType(),
        /*isPlaceholder=*/true);
    Expr *convertElementExpr = elementExpr;
    if (TypeChecker::typeCheckExpression(
            convertElementExpr, dc,
            /*contextualInfo=*/{forEachStmtInfo.initType, CTP_CoerceOperand})
            .isNull()) {
      return None;
    }
    elementExpr->setIsPlaceholder(false);
    stmt->setElementExpr(elementExpr);
    stmt->setConvertElementExpr(convertElementExpr);
  }

  // Write the result back into the AST.
  stmt->setSequence(resultTarget.getAsExpr());
  stmt->setPattern(resultTarget.getContextualPattern().getPattern());
  stmt->setSequenceConformance(sequenceConformance);
  stmt->setWhere(forEachStmtInfo.whereExpr);
  stmt->setIteratorVar(iterator);
  stmt->setIteratorVarRef(varRef);

  return resultTarget;
}

Optional<SolutionApplicationTarget>
ExprWalker::rewriteTarget(SolutionApplicationTarget target) {
  // Rewriting the target might abort in case one of visit methods returns
  // nullptr. In this case, no more walkToExprPost calls are issues and thus
  // nodes which were pushed on the Rewriter's ExprStack in walkToExprPre are
  // not popped of the stack again in walkTokExprPost. Usually, that's not an
  // issue if rewriting completely terminates because the ExprStack is never
  // used again. Here, however, we recover from a rewriting failure and continue
  // using the Rewriter. To make sure we don't continue with an ExprStack that
  // is still in the state when rewriting was aborted, save it here and restore
  // it once rewritingt this target has finished.
  llvm::SaveAndRestore<SmallVector<Expr *, 8>> RestoreExprStack(
      Rewriter.ExprStack);
  auto &solution = Rewriter.solution;

  // Apply the solution to the target.
  SolutionApplicationTarget result = target;
  if (auto expr = target.getAsExpr()) {
    Expr *rewrittenExpr = expr->walk(*this);
    if (!rewrittenExpr)
      return None;

    /// Handle special cases for expressions.
    switch (target.getExprContextualTypePurpose()) {
    case CTP_Initialization: {
      auto initResultTarget = applySolutionToInitialization(
          solution, target, rewrittenExpr);
      if (!initResultTarget)
        return None;

      result = *initResultTarget;
      break;
    }

    case CTP_ForEachStmt: {
      auto forEachResultTarget = applySolutionToForEachStmt(
          solution, target, rewrittenExpr);
      if (!forEachResultTarget)
        return None;

      result = *forEachResultTarget;
      break;
    }

    case CTP_Unused:
    case CTP_ReturnStmt:
    case swift::CTP_ReturnSingleExpr:
    case swift::CTP_YieldByValue:
    case swift::CTP_YieldByReference:
    case swift::CTP_ThrowStmt:
    case swift::CTP_EnumCaseRawValue:
    case swift::CTP_DefaultParameter:
    case swift::CTP_AutoclosureDefaultParameter:
    case swift::CTP_CalleeResult:
    case swift::CTP_CallArgument:
    case swift::CTP_ClosureResult:
    case swift::CTP_ArrayElement:
    case swift::CTP_DictionaryKey:
    case swift::CTP_DictionaryValue:
    case swift::CTP_CoerceOperand:
    case swift::CTP_AssignSource:
    case swift::CTP_SubscriptAssignSource:
    case swift::CTP_Condition:
    case swift::CTP_WrappedProperty:
    case swift::CTP_ComposedPropertyWrapper:
    case swift::CTP_CannotFail:
      result.setExpr(rewrittenExpr);
      break;
    }
  } else if (auto stmtCondition = target.getAsStmtCondition()) {
    for (auto &condElement : *stmtCondition) {
      switch (condElement.getKind()) {
      case StmtConditionElement::CK_Availability:
        continue;

      case StmtConditionElement::CK_Boolean: {
        auto condExpr = condElement.getBoolean();
        auto finalCondExpr = condExpr->walk(*this);
        if (!finalCondExpr)
          return None;

        // Load the condition if needed.
        solution.setExprTypes(finalCondExpr);
        if (finalCondExpr->getType()->hasLValueType()) {
          ASTContext &ctx = solution.getConstraintSystem().getASTContext();
          finalCondExpr = TypeChecker::addImplicitLoadExpr(ctx, finalCondExpr);
        }

        condElement.setBoolean(finalCondExpr);
        continue;
      }

      case StmtConditionElement::CK_PatternBinding: {
        ConstraintSystem &cs = solution.getConstraintSystem();
        auto target = *cs.getSolutionApplicationTarget(&condElement);
        auto resolvedTarget = rewriteTarget(target);
        if (!resolvedTarget)
          return None;

        solution.setExprTypes(resolvedTarget->getAsExpr());
        condElement.setInitializer(resolvedTarget->getAsExpr());
        condElement.setPattern(resolvedTarget->getInitializationPattern());
        continue;
      }
      }
    }

    return target;
  } else if (auto caseLabelItem = target.getAsCaseLabelItem()) {
    ConstraintSystem &cs = solution.getConstraintSystem();
    auto info = *cs.getCaseLabelItemInfo(*caseLabelItem);

    // Figure out the pattern type.
    Type patternType = solution.simplifyType(solution.getType(info.pattern));
    patternType = patternType->reconstituteSugar(/*recursive=*/false);

    // Coerce the pattern to its appropriate type.
    TypeResolutionOptions patternOptions(TypeResolverContext::InExpression);
    patternOptions |= TypeResolutionFlags::OverrideType;
    auto contextualPattern =
        ContextualPattern::forRawPattern(info.pattern,
                                         target.getDeclContext());
    if (auto coercedPattern = TypeChecker::coercePatternToType(
            contextualPattern, patternType, patternOptions)) {
      (*caseLabelItem)->setPattern(coercedPattern, /*resolved=*/true);
    } else {
      return None;
    }

    // If there is a guard expression, coerce that.
    if (auto guardExpr = info.guardExpr) {
      guardExpr = guardExpr->walk(*this);
      if (!guardExpr)
        return None;

      // FIXME: Feels like we could leverage existing code more.
      Type boolType = cs.getASTContext().getBoolType();
      guardExpr = solution.coerceToType(
          guardExpr, boolType, cs.getConstraintLocator(info.guardExpr));
      if (!guardExpr)
        return None;

      (*caseLabelItem)->setGuardExpr(guardExpr);
      solution.setExprTypes(guardExpr);
    }

    return target;
  } else if (auto patternBinding = target.getAsPatternBinding()) {
    ConstraintSystem &cs = solution.getConstraintSystem();
    for (unsigned index : range(patternBinding->getNumPatternEntries())) {
      // Find the solution application target for this.
      auto knownTarget = *cs.getSolutionApplicationTarget(
          {patternBinding, index});

      // Rewrite the target.
      auto resultTarget = rewriteTarget(knownTarget);
      if (!resultTarget)
        return None;

      patternBinding->setPattern(
          index, resultTarget->getInitializationPattern(),
          resultTarget->getDeclContext());
      patternBinding->setInit(index, resultTarget->getAsExpr());
    }

    return target;
  } else if (auto *wrappedVar = target.getAsUninitializedWrappedVar()) {
    // Get the outermost wrapper type from the solution
    auto outermostWrapper = wrappedVar->getAttachedPropertyWrappers().front();
    auto backingType = solution.simplifyType(
        solution.getType(outermostWrapper->getTypeExpr()));

    auto &ctx = solution.getConstraintSystem().getASTContext();
    ctx.setSideCachedPropertyWrapperBackingPropertyType(
        wrappedVar, backingType->mapTypeOutOfContext());

    return target;
  } else {
    auto fn = *target.getAsFunction();
    if (rewriteFunction(fn))
      return None;

    result.setFunctionBody(fn.getBody());
  }

  // Follow-up tasks.
  auto &cs = solution.getConstraintSystem();
  if (auto resultExpr = result.getAsExpr()) {
    Expr *expr = target.getAsExpr();
    assert(expr && "Can't have expression result without expression target");

    // We are supposed to use contextual type only if it is present and
    // this expression doesn't represent the implicit return of the single
    // expression function which got deduced to be `Never`.
    Type convertType = target.getExprConversionType();
    auto shouldCoerceToContextualType = [&]() {
      return convertType &&
          !target.isOptionalSomePatternInit() &&
          !(solution.getType(resultExpr)->isUninhabited() &&
            cs.getContextualTypePurpose(target.getAsExpr())
              == CTP_ReturnSingleExpr);
    };

    // If we're supposed to convert the expression to some particular type,
    // do so now.
    if (shouldCoerceToContextualType()) {
      resultExpr =
          Rewriter.coerceToType(resultExpr, solution.simplifyType(convertType),
                                cs.getConstraintLocator(resultExpr));
    } else if (cs.getType(resultExpr)->hasLValueType() &&
               !target.isDiscardedExpr()) {
      // We referenced an lvalue. Load it.
      resultExpr = Rewriter.coerceToType(
          resultExpr, cs.getType(resultExpr)->getRValueType(),
          cs.getConstraintLocator(resultExpr));
    }

    if (!resultExpr)
      return None;

    // For an @autoclosure default parameter type, add the autoclosure
    // conversion.
    if (FunctionType *autoclosureParamType =
            target.getAsAutoclosureParamType()) {
      resultExpr = cs.buildAutoClosureExpr(resultExpr, autoclosureParamType);
    }

    solution.setExprTypes(resultExpr);
    result.setExpr(resultExpr);

    if (cs.isDebugMode()) {
      auto &log = llvm::errs();
      log << "---Type-checked expression---\n";
      resultExpr->dump(log);
      log << "\n";
    }
  }

  return result;
}

/// Apply a given solution to the expression, producing a fully
/// type-checked expression.
Optional<SolutionApplicationTarget> ConstraintSystem::applySolution(
    Solution &solution, SolutionApplicationTarget target) {
  // If any fixes needed to be applied to arrive at this solution, resolve
  // them to specific expressions.
  if (!solution.Fixes.empty()) {
    if (shouldSuppressDiagnostics())
      return None;

    bool diagnosedErrorsViaFixes = applySolutionFixes(solution);
    // If all of the available fixes would result in a warning,
    // we can go ahead and apply this solution to AST.
    if (!llvm::all_of(solution.Fixes, [](const ConstraintFix *fix) {
          return fix->isWarning();
        })) {
      // If we already diagnosed any errors via fixes, that's it.
      if (diagnosedErrorsViaFixes)
        return None;

      // If we didn't manage to diagnose anything well, so fall back to
      // diagnosing mining the system to construct a reasonable error message.
      diagnoseFailureFor(target);
      return None;
    }
  }

  // If there are no fixes recorded but score indicates that there
  // should have been at least one, let's fail application and
  // produce a fallback diagnostic to highlight the problem.
  {
    const auto &score = solution.getFixedScore();
    if (score.Data[SK_Fix] > 0 || score.Data[SK_Hole] > 0) {
      maybeProduceFallbackDiagnostic(target);
      return None;
    }
  }

  ExprRewriter rewriter(*this, solution, target, shouldSuppressDiagnostics());
  ExprWalker walker(rewriter);
  auto resultTarget = walker.rewriteTarget(target);
  if (!resultTarget)
    return None;

  // Visit closures that have non-single expression bodies.
  bool hadError = false;

  for (auto *closure : walker.getClosuresToTypeCheck())
    hadError |= TypeChecker::typeCheckClosureBody(closure);

  // Tap expressions too; they should or should not be
  // type-checked under the same conditions as closure bodies.
  for (auto tuple : walker.getTapsToTypeCheck()) {
    auto tap = std::get<0>(tuple);
    auto tapDC = std::get<1>(tuple);
    hadError |= TypeChecker::typeCheckTapBody(tap, tapDC);
  }

  // If any of them failed to type check, bail.
  if (hadError)
    return None;

  rewriter.finalize();

  return resultTarget;
}

Expr *Solution::coerceToType(Expr *expr, Type toType,
                             ConstraintLocator *locator,
                             Optional<Pattern*> typeFromPattern) {
  auto &cs = getConstraintSystem();
  ExprRewriter rewriter(cs, *this, None, /*suppressDiagnostics=*/false);
  Expr *result = rewriter.coerceToType(expr, toType, locator, typeFromPattern);
  if (!result)
    return nullptr;

  setExprTypes(result);
  rewriter.finalize();
  return result;
}

bool Solution::hasType(ASTNode node) const {
  auto result = nodeTypes.find(node);
  if (result != nodeTypes.end())
    return true;

  auto &cs = getConstraintSystem();
  return cs.hasType(node);
}

Type Solution::getType(ASTNode node) const {
  auto result = nodeTypes.find(node);
  if (result != nodeTypes.end())
    return result->second;

  auto &cs = getConstraintSystem();
  return cs.getType(node);
}

Type Solution::getResolvedType(ASTNode node) const {
  return simplifyType(getType(node));
}

void Solution::setExprTypes(Expr *expr) const {
  if (!expr)
    return;

  SetExprTypes SET(*this);
  expr->walk(SET);
}

/// MARK: SolutionResult implementation.

SolutionResult SolutionResult::forSolved(Solution &&solution) {
  SolutionResult result(Kind::Success);
  void *memory = malloc(sizeof(Solution));
  result.solutions = new (memory) Solution(std::move(solution));
  result.numSolutions = 1;
  return result;
}

SolutionResult SolutionResult::forAmbiguous(
    MutableArrayRef<Solution> solutions) {
  assert(solutions.size() > 1 && "Not actually ambiguous");
  SolutionResult result(Kind::Ambiguous);
  result.solutions =
      (Solution *)malloc(sizeof(Solution) * solutions.size());
  result.numSolutions = solutions.size();
  std::uninitialized_copy(std::make_move_iterator(solutions.begin()),
                          std::make_move_iterator(solutions.end()),
                          result.solutions);
  return result;
}

SolutionResult::~SolutionResult() {
  assert((!requiresDiagnostic() || emittedDiagnostic) &&
         "SolutionResult was destroyed without emitting a diagnostic");

  for (unsigned i : range(numSolutions)) {
    solutions[i].~Solution();
  }
  free(solutions);
}

const Solution &SolutionResult::getSolution() const {
  assert(numSolutions == 1 && "Wrong number of solutions");
  return solutions[0];
}

Solution &&SolutionResult::takeSolution() && {
  assert(numSolutions == 1 && "Wrong number of solutions");
  return std::move(solutions[0]);
}

ArrayRef<Solution> SolutionResult::getAmbiguousSolutions() const {
  assert(getKind() == Ambiguous);
  return makeArrayRef(solutions, numSolutions);
}

MutableArrayRef<Solution> SolutionResult::takeAmbiguousSolutions() && {
  assert(getKind() == Ambiguous);
  markAsDiagnosed();
  return MutableArrayRef<Solution>(solutions, numSolutions);
}

SolutionApplicationTarget SolutionApplicationTarget::walk(ASTWalker &walker) {
  switch (kind) {
  case Kind::expression: {
    SolutionApplicationTarget result = *this;
    result.setExpr(getAsExpr()->walk(walker));
    return result;
  }

  case Kind::function:
    return SolutionApplicationTarget(
        *getAsFunction(),
        cast_or_null<BraceStmt>(getFunctionBody()->walk(walker)));

  case Kind::stmtCondition:
    for (auto &condElement : stmtCondition.stmtCondition) {
      condElement = *condElement.walk(walker);
    }
    return *this;

  case Kind::caseLabelItem:
    if (auto newPattern =
            caseLabelItem.caseLabelItem->getPattern()->walk(walker)) {
      caseLabelItem.caseLabelItem->setPattern(
          newPattern, caseLabelItem.caseLabelItem->isPatternResolved());
    }
    if (auto guardExpr = caseLabelItem.caseLabelItem->getGuardExpr()) {
      if (auto newGuardExpr = guardExpr->walk(walker))
        caseLabelItem.caseLabelItem->setGuardExpr(newGuardExpr);
    }

    return *this;

  case Kind::patternBinding:
    return *this;

  case Kind::uninitializedWrappedVar:
    return *this;
  }

  llvm_unreachable("invalid target kind");
}
