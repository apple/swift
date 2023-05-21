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
#include "TypeCheckMacros.h"
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
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
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

bool Solution::hasFixedType(TypeVariableType *typeVar) const {
  auto knownBinding = typeBindings.find(typeVar);
  return knownBinding != typeBindings.end();
}

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
  if (!archetype || !archetype->isRoot())
    return false;

  return (archetype->requiresClass() &&
          !archetype->hasRequirements());
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
  for (const auto &opened : openedTypes->second) {
    auto type = getFixedType(opened.second);
    if (opened.first->isParameterPack() && !type->is<PackType>())
      type = PackType::getSingletonPackExpansion(type);
    subs[opened.first] = type;
  }

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

// Lazily instantiate function definitions for class template specializations.
// Members of a class template specialization will be instantiated here (not
// when imported). If this method has already be instantiated, then this is a
// no-op.
static void maybeInstantiateCXXMethodDefinition(ValueDecl *decl) {
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
}

ConcreteDeclRef
Solution::resolveConcreteDeclRef(ValueDecl *decl,
                                 ConstraintLocator *locator) const {
  if (!decl)
    return ConcreteDeclRef();

  // Get the generic signature of the decl and compute the substitutions.
  auto sig = decl->getInnermostDeclContext()->getGenericSignatureOfContext();
  auto subst = computeSubstitutions(sig, locator);

  maybeInstantiateCXXMethodDefinition(decl);

  // If this is a C++ function template, get it's specialization for the given
  // substitution map and update the decl accordingly.
  if (isa_and_nonnull<clang::FunctionTemplateDecl>(decl->getClangDecl())) {
    auto moduleLoader = decl->getASTContext().getClangModuleLoader();
    return moduleLoader->getCXXFunctionTemplateSpecialization(subst, decl);
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
    case KeyPathExpr::Component::Kind::CodeCompletion:
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
    Optional<SyntacticElementTarget> target;
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
    /// \param locator Locator describing where this existential conversion occurs.
    ///
    /// \return The coerced expression, whose type will be equivalent to
    /// \c toType.
    Expr *coerceExistential(Expr *expr, Type toType,
                            ConstraintLocatorBuilder locator);

    /// Coerce an expression of (possibly unchecked) optional
    /// type to have a different (possibly unchecked) optional type.
    Expr *coerceOptionalToOptional(Expr *expr, Type toType,
                                   ConstraintLocatorBuilder locator);

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
      auto *env = sig.getGenericEnvironment();

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
                                  LookUpConformanceInModule(dc->getParentModule()));
    }

    /// Determine whether the given reference is to a method on
    /// a remote distributed actor in the given context.
    bool isDistributedThunk(ConcreteDeclRef ref, Expr *context);

  public:
    /// Build a reference to the given declaration.
    Expr *buildDeclRef(SelectedOverload overload, DeclNameLoc loc,
                       ConstraintLocatorBuilder locator, bool implicit) {
      auto choice = overload.choice;
      assert(choice.getKind() != OverloadChoiceKind::DeclViaDynamic);
      auto *decl = choice.getDecl();
      Type fullType = simplifyType(overload.openedFullType);
      Type adjustedFullType = simplifyType(overload.adjustedOpenedFullType);

      // Determine the declaration selected for this overloaded reference.
      auto &ctx = cs.getASTContext();
      
      auto semantics = decl->getAccessSemanticsFromContext(dc,
                                                           /*isAccessOnSelf*/false);

      // If this is a member of a nominal type, build a reference to the
      // member with an implied base type.
      if (decl->getDeclContext()->isTypeContext() && isa<FuncDecl>(decl)) {
        assert(cast<FuncDecl>(decl)->isOperator() && "Must be an operator");

        auto baseTy = getBaseType(adjustedFullType->castTo<FunctionType>());

        // Handle operator requirements found in protocols.
        if (auto proto = dyn_cast<ProtocolDecl>(decl->getDeclContext())) {
          bool isCurried = shouldBuildCurryThunk(choice, /*baseIsInstance=*/false);

          // If we have a concrete conformance, build a call to the witness.
          //
          // FIXME: This is awful. We should be able to handle this as a call to
          // the protocol requirement with Self == the concrete type, and SILGen
          // (or later) can devirtualize as appropriate.
          auto conformance =
            TypeChecker::conformsToProtocol(baseTy, proto, dc->getParentModule());
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
                  refType = adjustedFullType;
                else
                  refType = adjustedFullType->castTo<AnyFunctionType>()->getResult();

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

                    refExpr =
                        DotSyntaxCallExpr::create(ctx, declRefExpr, SourceLoc(),
                                                  Argument::unlabeled(base));
                    auto refType = adjustedFullType->castTo<FunctionType>()->getResult();
                    cs.setType(refExpr, refType);
                  } else {
                    refExpr = declRefExpr;
                  }

                  return forceUnwrapIfExpected(refExpr, locator);
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
            loc.getBaseNameLoc(), adjustedFullType->getMetatypeInstanceType(), ctx);
        cs.cacheType(typeExpr);
        return typeExpr;
      }

      auto ref = resolveConcreteDeclRef(decl, locator);
      auto declRefExpr =
          new (ctx) DeclRefExpr(ref, loc, implicit, semantics, fullType);
      cs.cacheType(declRefExpr);
      declRefExpr->setFunctionRefKind(choice.getFunctionRefKind());
      Expr *result = adjustTypeForDeclReference(
          declRefExpr, fullType, adjustedFullType);
      result = forceUnwrapIfExpected(result, locator);

      if (auto *fnDecl = dyn_cast<AbstractFunctionDecl>(decl)) {
        if (AnyFunctionRef(fnDecl).hasExternalPropertyWrapperParameters() &&
            (declRefExpr->getFunctionRefKind() == FunctionRefKind::Compound ||
             declRefExpr->getFunctionRefKind() == FunctionRefKind::Unapplied)) {
          result = buildSingleCurryThunk(result, fnDecl, locator);
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
      } else if (auto selfApply = dyn_cast<SelfApplyExpr>(expr)) {
        return selfApply->getBase();
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

      // Look through inout.
      bool isLValue = false;
      InOutExpr *origInOutBase = dyn_cast<InOutExpr>(base);
      if (origInOutBase) {
        base = origInOutBase->getSubExpr();
        baseTy = baseTy->getInOutObjectType();
        isLValue = true;
      }

      // Look through lvalues.
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
      if (isLValue && !origInOutBase &&
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

      // Re-apply inout if needed.
      Expr *resultExpr = archetypeVal;
      if (origInOutBase) {
        resultExpr = new (ctx) InOutExpr(
            origInOutBase->getLoc(), resultExpr, opaqueType->getRValueType());
        cs.cacheType(resultExpr);
      }

      return resultExpr;
    }

    /// Try to close the innermost active existential, if there is one.
    bool closeExistential(Expr *&result, ConstraintLocatorBuilder locator,
                          bool force) {
      if (OpenedExistentials.empty())
        return false;

      auto &record = OpenedExistentials.back();
      assert(record.Depth <= ExprStack.size());

      if (!force && record.Depth < ExprStack.size() - 1)
        return false;

      // If we had a return type of 'Self', erase it.
      Type resultTy;
      resultTy = cs.getType(result);
      if (resultTy->hasOpenedExistentialWithRoot(record.Archetype)) {
        Type erasedTy = resultTy->typeEraseOpenedArchetypesWithRoot(
            record.Archetype, dc);
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

    /// Close any active existentials.
    bool closeExistentials(Expr *&result, ConstraintLocatorBuilder locator,
                           bool force=false) {
      bool closedAny = false;
      while (closeExistential(result, locator, force)) {
        force = false;
        closedAny = true;
      }

      return closedAny;
    }

    /// When we have a reference to a declaration whose type in context is
    /// different from its normal interface type, introduce the appropriate
    /// conversions. This can happen due to `@preconcurrency`.
    Expr *adjustTypeForDeclReference(
        Expr *expr, Type openedType, Type adjustedOpenedType,
        llvm::function_ref<Type(Type)> getNewType = [](Type type) {
          return type;
        }) {
      // If the types are the same, do nothing.
      if (openedType->isEqual(adjustedOpenedType))
        return expr;

      auto &context = cs.getASTContext();

      // For an RValue function type, use a standard function conversion.
      if (openedType->is<AnyFunctionType>()) {
        expr = new (context) FunctionConversionExpr(
            expr, getNewType(adjustedOpenedType));
        cs.cacheType(expr);
        return expr;
      }

      // For any kind of LValue, use an ABISafeConversion.
      if (openedType->hasLValueType()) {
        assert(adjustedOpenedType->hasLValueType() && "lvalueness mismatch?");

        expr = new (context) ABISafeConversionExpr(
                                expr, getNewType(adjustedOpenedType));
        cs.cacheType(expr);
        return expr;
      }

      // If we have an optional type, wrap it up in a monadic '?' and recurse.
      if (Type objectType = openedType->getOptionalObjectType()) {
        Type adjustedRefType = getNewType(adjustedOpenedType);
        Type adjustedObjectType = adjustedRefType->getOptionalObjectType();
        assert(adjustedObjectType && "Not an optional?");

        expr = new (context) BindOptionalExpr(expr, SourceLoc(), 0, objectType);
        cs.cacheType(expr);
        expr = adjustTypeForDeclReference(expr, objectType, adjustedObjectType);
        expr = new (context) InjectIntoOptionalExpr(expr, adjustedRefType);
        cs.cacheType(expr);
        expr = new (context) OptionalEvaluationExpr(expr, adjustedRefType);
        cs.cacheType(expr);
        return expr;
      }

      assert(false && "Unhandled adjustment");
      return expr;
    }

    /// Determines if a partially-applied member reference should be
    /// converted into a fully-applied member reference with a pair of
    /// closures.
    bool shouldBuildCurryThunk(OverloadChoice choice,
                               bool baseIsInstance) {
      ValueDecl *member = choice.getDecl();

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

      // Bound member references that are '@objc optional' or found via dynamic
      // lookup are always represented via DynamicMemberRefExpr instead of a
      // curry thunk.
      if (member->getAttrs().hasAttribute<OptionalAttr>() ||
          choice.getKind() == OverloadChoiceKind::DeclViaDynamic)
        return false;

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

    /// Build the call inside the body of a single curry thunk
    /// "{ args in base.fn(args) }".
    ///
    /// \param baseExpr The captured base expression, if warranted.
    /// \param fnExpr The expression to be called by consecutively applying
    /// the optional \p baseExpr and thunk parameters.
    /// \param declOrClosure The underlying function-like declaration or
    /// closure we're going to call.
    /// \param thunkParamList The enclosing thunk's parameter list.
    /// \param locator The locator pinned on the function reference carried
    /// by \p fnExpr. If the function has associated applied property wrappers,
    /// the locator is used to pull them in.
    ApplyExpr *buildSingleCurryThunkBodyCall(Expr *baseExpr, Expr *fnExpr,
                                             DeclContext *declOrClosure,
                                             ParameterList *thunkParamList,
                                             ConstraintLocatorBuilder locator) {
      auto &ctx = cs.getASTContext();
      auto *const fnTy = cs.getType(fnExpr)->castTo<FunctionType>();
      auto *calleeFnTy = fnTy;

      if (baseExpr) {
        // Coerce the base expression to the container type.
        const auto calleeSelfParam = calleeFnTy->getParams().front();
        baseExpr =
            coerceToType(baseExpr, calleeSelfParam.getOldType(), locator);

        // Uncurry the callee type in the presence of a base expression; we
        // want '(args) -> result' vs. '(self) -> (args) -> result'.
        calleeFnTy = calleeFnTy->getResult()->castTo<FunctionType>();
      }

      const auto &appliedPropertyWrappers =
          solution.appliedPropertyWrappers[locator.getAnchor()];
      const auto calleeDeclRef = resolveConcreteDeclRef(
          dyn_cast<AbstractFunctionDecl>(declOrClosure), locator);

      auto *const calleeParamList = getParameterList(declOrClosure);
      const auto calleeParams = calleeFnTy->getParams();

      // Rebuild the callee params: SILGen knows how to emit property-wrapped
      // parameters, but the corresponding parameter types need to match the
      // backing wrapper types.
      SmallVector<AnyFunctionType::Param, 4> newCalleeParams;
      newCalleeParams.reserve(calleeParams.size());

      // Build the argument list for the call.
      SmallVector<Argument, 4> args;
      unsigned appliedWrapperIndex = 0;
      for (const auto idx : indices(*thunkParamList)) {
        auto *const thunkParamDecl = thunkParamList->get(idx);

        const auto calleeParam = calleeParams[idx];
        const auto calleeParamType = calleeParam.getParameterType();
        const auto thunkParamType = thunkParamDecl->getType();

        Expr *paramRef = new (ctx)
            DeclRefExpr(thunkParamDecl, DeclNameLoc(), /*implicit*/ true);
        paramRef->setType(thunkParamDecl->isInOut()
                              ? LValueType::get(thunkParamType)
                              : thunkParamType);
        cs.cacheType(paramRef);

        paramRef = coerceToType(paramRef,
                                thunkParamDecl->isInOut()
                                    ? LValueType::get(calleeParamType)
                                    : calleeParamType,
                                locator);

        auto *const calleeParamDecl = calleeParamList->get(idx);
        if (calleeParamDecl->hasExternalPropertyWrapper()) {
          // Rewrite the parameter ref to the backing wrapper initialization
          // expression.
          auto &appliedWrapper = appliedPropertyWrappers[appliedWrapperIndex++];

          using ValueKind = AppliedPropertyWrapperExpr::ValueKind;
          ValueKind valueKind = (appliedWrapper.initKind ==
                                         PropertyWrapperInitKind::ProjectedValue
                                     ? ValueKind::ProjectedValue
                                     : ValueKind::WrappedValue);

          paramRef = AppliedPropertyWrapperExpr::create(
              ctx, calleeDeclRef, calleeParamDecl, SourceLoc(),
              appliedWrapper.wrapperType, paramRef, valueKind);
          cs.cacheExprTypes(paramRef);

          newCalleeParams.push_back(calleeParam.withType(paramRef->getType()));

          // TODO: inout
          // FIXME: vararg
        } else {
          if (thunkParamDecl->isInOut()) {
            paramRef =
                new (ctx) InOutExpr(SourceLoc(), paramRef, calleeParamType,
                                    /*implicit=*/true);
          } else if (thunkParamDecl->isVariadic()) {
            assert(calleeParamType->isEqual(paramRef->getType()));
            paramRef = VarargExpansionExpr::createParamExpansion(ctx, paramRef);
          }
          cs.cacheType(paramRef);

          newCalleeParams.push_back(calleeParam);
        }

        args.emplace_back(SourceLoc(), calleeParam.getLabel(), paramRef);
      }

      // SILGen knows how to emit property-wrapped parameters, but the
      // corresponding parameter types need to match the backing wrapper types.
      // To handle this, build a new callee function type out of the adjusted
      // callee params, hand it over to the conditional 'self' call, and use it
      // to update the type of the called expression with respect to whether
      // it's 'self'-curried.
      auto *const newCalleeFnTy = FunctionType::get(
          newCalleeParams, calleeFnTy->getResult(), calleeFnTy->getExtInfo());

      // If given, apply the base expression to the curried 'self'
      // parameter first.
      if (baseExpr) {
        fnExpr->setType(FunctionType::get(fnTy->getParams(), newCalleeFnTy,
                                          fnTy->getExtInfo()));
        cs.cacheType(fnExpr);

        fnExpr = DotSyntaxCallExpr::create(ctx, fnExpr, SourceLoc(),
                                           Argument::unlabeled(baseExpr));
      }
      fnExpr->setType(newCalleeFnTy);
      cs.cacheType(fnExpr);

      // Finally, apply the argument list to the callee.
      ApplyExpr *callExpr = CallExpr::createImplicit(
          ctx, fnExpr, ArgumentList::createImplicit(ctx, args));
      callExpr->setType(calleeFnTy->getResult());
      cs.cacheType(callExpr);

      return callExpr;
    }

    /// Build a "{ args in base.fn(args) }" single-expression curry thunk.
    ///
    /// \param baseExpr The base expression to be captured, if warranted.
    /// \param fnExpr The expression to be called by consecutively applying
    /// the optional \p baseExpr and thunk parameters.
    /// \param declOrClosure The underlying function-like declaration or
    /// closure we're going to call.
    /// \param thunkTy The type of the thunk.
    /// \param locator The locator pinned on the function reference carried
    /// by \p fnExpr. If the function has associated applied property wrappers,
    /// the locator is used to pull them in.
    AutoClosureExpr *buildSingleCurryThunk(Expr *baseExpr, Expr *fnExpr,
                                           DeclContext *declOrClosure,
                                           FunctionType *thunkTy,
                                           ConstraintLocatorBuilder locator) {
      auto &ctx = cs.getASTContext();

      const OptionSet<ParameterList::CloneFlags> options =
          (ParameterList::Implicit | ParameterList::NamedArguments);
      auto *const thunkParamList =
          getParameterList(declOrClosure)->clone(ctx, options);

      for (const auto idx : indices(*thunkParamList)) {
        auto *param = thunkParamList->get(idx);
        auto arg = thunkTy->getParams()[idx];

        param->setInterfaceType(arg.getParameterType()->mapTypeOutOfContext());
        param->setSpecifier(ParamDecl::getParameterSpecifierForValueOwnership(
            arg.getValueOwnership()));
      }

      auto *const thunk =
          new (ctx) AutoClosureExpr(/*set body later*/ nullptr, thunkTy, dc);
      thunk->setParameterList(thunkParamList);
      thunk->setThunkKind(AutoClosureExpr::Kind::SingleCurryThunk);
      cs.cacheType(thunk);
      
      // If the `self` type is existential, it must be opened.
      OpaqueValueExpr *baseOpened = nullptr;
      Expr *origBaseExpr = baseExpr;
      if (baseExpr) {
        auto baseTy = cs.getType(baseExpr);
        if (baseTy->isAnyExistentialType()) {
          Type openedTy = solution.OpenedExistentialTypes.lookup(
              cs.getConstraintLocator(locator));
          assert(openedTy);

          Type opaqueValueTy = openedTy;
          if (baseTy->is<ExistentialMetatypeType>())
            opaqueValueTy = MetatypeType::get(opaqueValueTy);

          baseOpened = new (ctx) OpaqueValueExpr(SourceLoc(), opaqueValueTy);
          cs.cacheType(baseOpened);
          baseExpr = baseOpened;
        }
      }

      Expr *thunkBody = buildSingleCurryThunkBodyCall(
          baseExpr, fnExpr, declOrClosure, thunkParamList, locator);
          
      // If we called a function with a dynamic 'Self' result, we may need some
      // special handling.
      if (baseExpr) {
        if (auto *fnDecl = dyn_cast<AbstractFunctionDecl>(declOrClosure)) {
          if (fnDecl->hasDynamicSelfResult()) {
            Type convTy;

            if (cs.getType(baseExpr)->hasOpenedExistential()) {
              // FIXME: Sometimes we need to convert to an opened existential
              // first, because CovariantReturnConversionExpr does not support
              // direct conversions from a class C to an existential C & P.
              convTy = cs.getType(baseExpr)->getMetatypeInstanceType();
              convTy =
                  thunkTy->getResult()->replaceCovariantResultType(convTy, 0);
            } else {
              convTy = thunkTy->getResult();
            }

            if (!thunkBody->getType()->isEqual(convTy)) {
              thunkBody = cs.cacheType(
                  new (ctx) CovariantReturnConversionExpr(thunkBody, convTy));
            }
          }
        }
      }

      // Now, coerce to the result type of the thunk.
      thunkBody = coerceToType(thunkBody, thunkTy->getResult(), locator);

      // Close up the existential if necessary.
      if (baseOpened) {
        thunkBody = new (ctx) OpenExistentialExpr(origBaseExpr,
                                                  baseOpened,
                                                  thunkBody,
                                                  thunkBody->getType());
        cs.cacheType(thunkBody);
      }

      if (thunkTy->getExtInfo().isThrowing()) {
        thunkBody = new (ctx)
            TryExpr(thunkBody->getStartLoc(), thunkBody, cs.getType(thunkBody),
                    /*implicit=*/true);
        cs.cacheType(thunkBody);
      }

      thunk->setBody(thunkBody);

      return thunk;
    }

    /// Build a "{ args in fn(args) }" single-expression curry thunk.
    ///
    /// \param fnExpr The expression to be called by applying the thunk
    /// parameters.
    /// \param declOrClosure The underlying function-like declaration or
    /// closure we're going to call.
    /// \param locator The locator pinned on the function reference carried
    /// by \p fnExpr. If the function has associated applied property wrappers,
    /// the locator is used to pull them in.
    AutoClosureExpr *buildSingleCurryThunk(Expr *fnExpr,
                                           DeclContext *declOrClosure,
                                           ConstraintLocatorBuilder locator) {
      auto *const thunkTy = cs.getType(fnExpr)->castTo<FunctionType>();

      return buildSingleCurryThunk(/*baseExpr=*/nullptr, fnExpr, declOrClosure,
                                   thunkTy, locator);
    }

    /// Build a "{ args in base.fn(args) }" single-expression curry thunk.
    ///
    /// \param baseExpr The base expression to be captured.
    /// \param fnExpr The expression to be called by consecutively applying
    /// the \p baseExpr and thunk parameters.
    /// \param declOrClosure The underlying function-like declaration or
    /// closure we're going to call.
    /// \param locator The locator pinned on the function reference carried
    /// by \p fnExpr. If the function has associated applied property wrappers,
    /// the locator is used to pull them in.
    AutoClosureExpr *buildSingleCurryThunk(Expr *baseExpr, Expr *fnExpr,
                                           DeclContext *declOrClosure,
                                           ConstraintLocatorBuilder locator) {
      assert(baseExpr);
      auto *const thunkTy = cs.getType(fnExpr)
                                ->castTo<FunctionType>()
                                ->getResult()
                                ->castTo<FunctionType>();

      return buildSingleCurryThunk(baseExpr, fnExpr, declOrClosure, thunkTy,
                                   locator);
    }

    /// Build a "{ self in { args in self.fn(args) } }" nested curry thunk.
    ///
    /// \param memberRef The expression to be called in the inner thunk by
    /// consecutively applying the captured outer thunk's 'self' parameter and
    /// the parameters of the inner thunk.
    /// \param member The underlying function declaration to be called.
    /// \param outerThunkTy The type of the outer thunk.
    /// \param memberLocator The locator pinned on the member reference. If the
    /// function has associated applied property wrappers, the locator is used
    /// to pull them in.
    AutoClosureExpr *
    buildDoubleCurryThunk(DeclRefExpr *memberRef, ValueDecl *member,
                          FunctionType *outerThunkTy,
                          ConstraintLocatorBuilder memberLocator,
                          DeclNameLoc memberLoc, bool isDynamicLookup) {
      auto &ctx = cs.getASTContext();

      const auto selfThunkParam = outerThunkTy->getParams().front();
      const auto selfThunkParamTy = selfThunkParam.getPlainType();

      // Build the 'self' param for the outer thunk, "{ self in ... }".
      auto *const selfParamDecl =
          new (ctx) ParamDecl(SourceLoc(),
                              /*argument label*/ SourceLoc(), Identifier(),
                              /*parameter name*/ SourceLoc(), ctx.Id_self, dc);
      selfParamDecl->setInterfaceType(selfThunkParamTy->mapTypeOutOfContext());
      selfParamDecl->setSpecifier(
          ParamDecl::getParameterSpecifierForValueOwnership(
              selfThunkParam.getValueOwnership()));
      selfParamDecl->setImplicit();

      // Build a reference to the 'self' parameter.
      Expr *selfParamRef = new (ctx) DeclRefExpr(selfParamDecl, DeclNameLoc(),
                                                 /*implicit=*/true);
      selfParamRef->setType(selfThunkParam.isInOut()
                                ? LValueType::get(selfThunkParamTy)
                                : selfThunkParamTy);
      cs.cacheType(selfParamRef);

      if (selfThunkParam.isInOut()) {
        selfParamRef =
            new (ctx) InOutExpr(SourceLoc(), selfParamRef, selfThunkParamTy,
                                /*implicit=*/true);
        cs.cacheType(selfParamRef);
      }

      bool hasOpenedExistential = false;
      Expr *selfOpenedRef = selfParamRef;

      // If the 'self' parameter type is existential, it must be opened.
      if (selfThunkParamTy->isAnyExistentialType()) {
        Type openedTy = solution.OpenedExistentialTypes.lookup(
            cs.getConstraintLocator(memberLocator));
        assert(openedTy);

        hasOpenedExistential = true;

        // If we're opening an existential:
        // - The type of 'memberRef' inside the thunk is written in terms of the
        //   opened existential archetype.
        // - The type of the thunk is written in terms of the
        //   erased existential bounds.
        Type opaqueValueTy = openedTy;
        if (selfThunkParamTy->is<ExistentialMetatypeType>())
          opaqueValueTy = MetatypeType::get(opaqueValueTy);

        if (selfThunkParam.isInOut())
          opaqueValueTy = LValueType::get(opaqueValueTy);

        selfOpenedRef = new (ctx) OpaqueValueExpr(SourceLoc(), opaqueValueTy);
        cs.cacheType(selfOpenedRef);
      }

      Expr *outerThunkBody = nullptr;

      // For an @objc optional member or a member found via dynamic lookup,
      // build a dynamic member reference. Otherwise, build a nested
      // "{ args... in self.member(args...) }" thunk that calls the member.
      if (isDynamicLookup || member->getAttrs().hasAttribute<OptionalAttr>()) {
        auto *const selfCalleeTy =
            cs.getType(memberRef)->castTo<FunctionType>();

        outerThunkBody = new (ctx) DynamicMemberRefExpr(
            selfOpenedRef, SourceLoc(),
            resolveConcreteDeclRef(member, memberLocator), memberLoc);
        outerThunkBody->setImplicit(true);
        outerThunkBody->setType(selfCalleeTy->getResult());
        cs.cacheType(outerThunkBody);

        outerThunkBody = coerceToType(outerThunkBody, outerThunkTy->getResult(),
                                      memberLocator);

        // Close the existential if warranted.
        if (hasOpenedExistential) {
          outerThunkBody = new (ctx) OpenExistentialExpr(
              selfParamRef, cast<OpaqueValueExpr>(selfOpenedRef),
              outerThunkBody, outerThunkBody->getType());
          cs.cacheType(outerThunkBody);
        }
      } else {
        auto *innerThunk = buildSingleCurryThunk(
            selfOpenedRef, memberRef, cast<DeclContext>(member),
            outerThunkTy->getResult()->castTo<FunctionType>(), memberLocator);

        // Rewrite the body to close the existential if warranted.
        if (hasOpenedExistential) {
          auto *body = innerThunk->getSingleExpressionBody();
          body = new (ctx) OpenExistentialExpr(
              selfParamRef, cast<OpaqueValueExpr>(selfOpenedRef), body,
              body->getType());
          cs.cacheType(body);

          innerThunk->setBody(body);
        }

        outerThunkBody = innerThunk;
      }

      // Finally, construct the outer thunk.
      auto *outerThunk =
          new (ctx) AutoClosureExpr(outerThunkBody, outerThunkTy, dc);
      outerThunk->setThunkKind(AutoClosureExpr::Kind::DoubleCurryThunk);
      outerThunk->setParameterList(
          ParameterList::create(ctx, SourceLoc(), selfParamDecl, SourceLoc()));
      cs.cacheType(outerThunk);

      return outerThunk;
    }

    /// Build a new member reference with the given base and member.
    Expr *buildMemberRef(Expr *base, SourceLoc dotLoc,
                         SelectedOverload overload, DeclNameLoc memberLoc,
                         ConstraintLocatorBuilder locator,
                         ConstraintLocatorBuilder memberLocator, bool Implicit,
                         AccessSemantics semantics) {
      const auto &choice = overload.choice;
      const auto openedType = overload.openedType;
      const auto adjustedOpenedType = overload.adjustedOpenedType;

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
        if (auto existential = baseTy->getAs<ExistentialType>())
          baseTy = existential->getConstraintType();

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
              overload.adjustedOpenedFullType->castTo<FunctionType>()->getParams()[0];

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
        Type refType = simplifyType(adjustedOpenedType);
        auto ref = TypeExpr::createForDecl(memberLoc, TD, dc);
        cs.setType(ref, refType);
        auto *result = new (context) DotSyntaxBaseIgnoredExpr(
            base, dotLoc, ref, refType);
        cs.setType(result, refType);
        return result;
      }

      Type refTy = simplifyType(overload.openedFullType);
      Type adjustedRefTy = simplifyType(overload.adjustedOpenedFullType);

      // If we're referring to the member of a module, it's just a simple
      // reference.
      if (baseTy->is<ModuleType>()) {
        assert(semantics == AccessSemantics::Ordinary &&
               "Direct property access doesn't make sense for this");
        auto *dre = new (context) DeclRefExpr(memberRef, memberLoc, Implicit);
        cs.setType(dre, refTy);
        dre->setFunctionRefKind(choice.getFunctionRefKind());
        Expr *ref = cs.cacheType(new (context) DotSyntaxBaseIgnoredExpr(
            base, dotLoc, dre, refTy));

        ref = adjustTypeForDeclReference(ref, refTy, adjustedRefTy);
        return forceUnwrapIfExpected(ref, memberLocator);
      }

      const bool isUnboundInstanceMember =
          (!baseIsInstance && member->isInstanceMember());
      const bool needsCurryThunk =
          shouldBuildCurryThunk(choice, baseIsInstance);

      // The formal type of the 'self' value for the member's declaration.
      Type containerTy = getBaseType(refTy->castTo<FunctionType>());

      // If we have an opened existential, selfTy and baseTy will both be
      // the same opened existential type.
      Type selfTy = containerTy;

      // If we opened up an existential when referencing this member, update
      // the base accordingly.
      bool openedExistential = false;

      auto knownOpened = solution.OpenedExistentialTypes.find(
                           getConstraintSystem().getConstraintLocator(
                             memberLocator));
      if (knownOpened != solution.OpenedExistentialTypes.end()) {
        // Determine if we're going to have an OpenExistentialExpr around
        // this member reference.
        //
        // For an unbound reference to a method, always open the existential
        // inside the curry thunk, because we won't have a 'self' value until
        // the curry thunk is applied.
        //
        // For a partial application of a protocol method, open the existential
        // inside the curry thunk as well. This reduces abstraction and
        // post-factum function type conversions, and results in better SILGen.
        //
        // For a partial application of a class instance method, however, we
        // always want the thunk to accept a class to avoid potential
        // abstraction, so the existential base must be opened eagerly in order
        // to be upcast to the appropriate class reference type before it is
        //  passed to the thunk.
        if (!needsCurryThunk ||
            (!member->getDeclContext()->getSelfProtocolDecl() &&
             baseIsInstance && member->isInstanceMember())) {
          // Open the existential before performing the member reference.
          base = openExistentialReference(base, knownOpened->second, member);
          baseTy = knownOpened->second;
          selfTy = baseTy;
          openedExistential = true;
        } else {
          // Erase opened existentials from the type of the thunk; we're
          // going to open the existential inside the thunk's body.
          containerTy = containerTy->typeEraseOpenedArchetypesWithRoot(
              knownOpened->second, dc);
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
        // The base of an unbound reference is unused, and thus a conversion
        // is not necessary.
        if (!isUnboundInstanceMember) {
          if (!isExistentialMetatype || openedExistential) {
            // Convert the base to an rvalue of the appropriate metatype.
            base = coerceToType(
                base, MetatypeType::get(isDynamic ? selfTy : containerTy),
                locator.withPathElement(ConstraintLocator::MemberRefBase));
          }
        }

        if (!base)
          return nullptr;

        base = cs.coerceToRValue(base);
      }
      assert(base && "Unable to convert base?");

      if (isDynamic || member->getAttrs().hasAttribute<OptionalAttr>()) {
        // If the @objc attribute was inferred based on deprecated Swift 3
        // rules, complain at this use site.
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
      }

      // Handle dynamic references.
      if (!needsCurryThunk &&
          (isDynamic || member->getAttrs().hasAttribute<OptionalAttr>())) {
        base = cs.coerceToRValue(base);
        Expr *ref = new (context) DynamicMemberRefExpr(base, dotLoc, memberRef,
                                                       memberLoc);
        ref->setImplicit(Implicit);
        // FIXME: FunctionRefKind

        auto computeRefType = [&](Type openedType) {
          // Compute the type of the reference.
          Type refType = simplifyType(openedType);

          // If the base was an opened existential, erase the opened
          // existential.
          if (openedExistential) {
            refType = refType->typeEraseOpenedArchetypesWithRoot(
                baseTy->castTo<OpenedArchetypeType>(), dc);
          }

          return refType;
        };

        Type refType = computeRefType(openedType);
        cs.setType(ref, refType);

        // Adjust the declaration reference type, if required.
        ref = adjustTypeForDeclReference(
            ref, openedType, adjustedOpenedType, computeRefType);

        closeExistentials(ref, locator, /*force=*/openedExistential);

        // We also need to handle the implicitly unwrap of the result
        // of the called function if that's the type checking solution
        // we ended up with.
        return forceUnwrapIfExpected(ref, memberLocator);
      }

      // For properties, build member references.
      if (auto *varDecl = dyn_cast<VarDecl>(member)) {
        // \returns result of the given function type
        auto resultType = [](Type fnTy) -> Type {
          return fnTy->castTo<FunctionType>()->getResult();
        };

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

        if (hasDynamicSelf) {
          refTy = refTy->replaceCovariantResultType(containerTy, 1);
          adjustedRefTy = adjustedRefTy->replaceCovariantResultType(
              containerTy, 1);
        }

        cs.setType(memberRefExpr, resultType(refTy));

        Expr *result = memberRefExpr;
        result = adjustTypeForDeclReference(result, resultType(refTy),
                                                    resultType(adjustedRefTy));
        closeExistentials(result, locator);

        // If the property is of dynamic 'Self' type, wrap an implicit
        // conversion around the resulting expression, with the destination
        // type having 'Self' swapped for the appropriate replacement
        // type -- usually the base object type.
        if (hasDynamicSelf) {
          const auto conversionTy = simplifyType(adjustedOpenedType);
          if (!containerTy->isEqual(conversionTy)) {
            result = cs.cacheType(new (context) CovariantReturnConversionExpr(
                result, conversionTy));
          }
        }
        return forceUnwrapIfExpected(result, memberLocator);
      }

      auto *func = dyn_cast<FuncDecl>(member);
      if (func && func->getResultInterfaceType()->hasDynamicSelfType()) {
        refTy = refTy->replaceCovariantResultType(containerTy, 2);
        adjustedRefTy = adjustedRefTy->replaceCovariantResultType(
            containerTy, 2);
      }

      // Handle all other references.
      auto declRefExpr = new (context) DeclRefExpr(memberRef, memberLoc,
                                                   Implicit, semantics);
      declRefExpr->setFunctionRefKind(choice.getFunctionRefKind());
      declRefExpr->setType(refTy);
      cs.setType(declRefExpr, refTy);
      Expr *ref = declRefExpr;

      ref = adjustTypeForDeclReference(ref, refTy, adjustedRefTy);

      // A partial application thunk consists of two nested closures:
      //
      // { self in { args... in self.method(args...) } }
      //
      // If the reference has an applied 'self', eg 'let fn = foo.method',
      // the outermost closure is wrapped inside a single ApplyExpr:
      //
      // { self in { args... in self.method(args...) } }(foo)
      //
      // This is done instead of just hoisting the expression 'foo' up
      // into the closure, which would change evaluation order.
      //
      // However, for a super method reference, eg, 'let fn = super.foo',
      // the base expression is always a SuperRefExpr, possibly wrapped
      // by an upcast. Since SILGen expects super method calls to have a
      // very specific shape, we only emit a single closure here and
      // capture the original SuperRefExpr, since its evaluation does not
      // have side effects, instead of abstracting out a 'self' parameter.
      const auto isSuperPartialApplication = needsCurryThunk && isSuper;
      if (isSuperPartialApplication) {
        ref = buildSingleCurryThunk(base, declRefExpr,
                                    cast<AbstractFunctionDecl>(member),
                                    memberLocator);
      } else if (needsCurryThunk) {
        // Another case where we want to build a single closure is when
        // we have a partial application of a static member. It is better
        // to either push the base reference down into the closure (if it's
        // just a literal type reference, in which case there are no order of
        // operation concerns with capturing vs. evaluating it in the closure),
        // or to evaluate the base as a capture and hand it down via the
        // capture list.
        if (isa<ConstructorDecl>(member) || member->isStatic()) {
          if (cs.isStaticallyDerivedMetatype(base)) {
            // Add a useless ".self" to avoid downstream diagnostics.
            base = new (context) DotSelfExpr(base, SourceLoc(), base->getEndLoc(),
                                             cs.getType(base));
            cs.setType(base, base->getType());

            auto *closure = buildSingleCurryThunk(
                base, declRefExpr, cast<AbstractFunctionDecl>(member),
                memberLocator);

            // Skip the code below -- we're not building an extra level of
            // call by applying the metatype; instead, the closure we just
            // built is the curried reference.
            return closure;
          } else {
            // Add a useless ".self" to avoid downstream diagnostics, in case
            // the type ref is still a TypeExpr.
            base = new (context) DotSelfExpr(base, SourceLoc(), base->getEndLoc(),
                                             cs.getType(base));
            // Introduce a capture variable.
            cs.cacheType(base);
            solution.setExprTypes(base);
            auto capture = new (context) VarDecl(/*static*/ false,
                                                 VarDecl::Introducer::Let,
                                                 SourceLoc(),
                                                 context.getIdentifier("$base$"),
                                                 dc);
            capture->setImplicit();
            capture->setInterfaceType(base->getType()->mapTypeOutOfContext());
            
            NamedPattern *capturePat = new (context) NamedPattern(capture);
            capturePat->setImplicit();
            capturePat->setType(base->getType());
            
            auto capturePBE = PatternBindingEntry(capturePat,
                                                  SourceLoc(), base, dc);
            auto captureDecl = PatternBindingDecl::create(context, SourceLoc(),
                                                          {}, SourceLoc(),
                                                          capturePBE,
                                                          dc);
            captureDecl->setImplicit();
            
            // Write the closure in terms of the capture.
            auto baseRef = new (context)
              DeclRefExpr(capture, DeclNameLoc(base->getLoc()), /*implicit*/ true);
            baseRef->setType(base->getType());
            cs.cacheType(baseRef);
            
            auto *closure = buildSingleCurryThunk(
              baseRef, declRefExpr, cast<AbstractFunctionDecl>(member),
              simplifyType(adjustedOpenedType)->castTo<FunctionType>(),
              memberLocator);
              
            // Wrap the closure in a capture list.
            auto captureEntry = CaptureListEntry(captureDecl);
            auto captureExpr = CaptureListExpr::create(context, captureEntry,
                                                       closure);
            captureExpr->setImplicit();
            captureExpr->setType(cs.getType(closure));
            cs.cacheType(captureExpr);
            
            Expr *finalExpr = captureExpr;
            closeExistentials(finalExpr, locator,
                              /*force*/ openedExistential);
            return finalExpr;
          }
        }

        FunctionType *curryThunkTy = nullptr;
        if (isUnboundInstanceMember) {
          // For an unbound reference to a method, all conversions, including
          // dynamic 'Self' handling, are done within the thunk to support
          // the edge case of an unbound reference to a 'Self'-returning class
          // method on a protocol metatype. The result of calling the method
          // must be downcast to the opened archetype before being erased to the
          // subclass existential to cope with the expectations placed
          // on 'CovariantReturnConversionExpr'.
          curryThunkTy = simplifyType(adjustedOpenedType)->castTo<FunctionType>();
        } else {
          curryThunkTy = adjustedRefTy->castTo<FunctionType>();

          // Check if we need to open an existential stored inside 'self'.
          auto knownOpened = solution.OpenedExistentialTypes.find(
              getConstraintSystem().getConstraintLocator(memberLocator));
          if (knownOpened != solution.OpenedExistentialTypes.end()) {
            curryThunkTy =
                curryThunkTy
                    ->typeEraseOpenedArchetypesWithRoot(knownOpened->second, dc)
                    ->castTo<FunctionType>();
          }
        }

        // Replace the DeclRefExpr with a closure expression which SILGen
        // knows how to emit.
        ref = buildDoubleCurryThunk(declRefExpr, member, curryThunkTy,
                                    memberLocator, memberLoc, isDynamic);
      }

      // If the member is a method with a dynamic 'Self' result type, wrap an
      // implicit function type conversion around the resulting expression,
      // with the destination type having 'Self' swapped for the appropriate
      // replacement type -- usually the base object type.
      //
      // Note: For unbound references this is handled inside the thunk.
      if (!isUnboundInstanceMember &&
          !member->getDeclContext()->getSelfProtocolDecl()) {
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
                  adjustedRefTy->replaceCovariantResultType(replacementTy, 2);
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
        return forceUnwrapIfExpected(ref, memberLocator);
      }

      ApplyExpr *apply;
      if (isa<ConstructorDecl>(member)) {
        // FIXME: Provide type annotation.
        ref = forceUnwrapIfExpected(ref, memberLocator);
        apply = ConstructorRefCallExpr::create(context, ref, base);
      } else if (isUnboundInstanceMember) {
        ref = adjustTypeForDeclReference(
            ref, cs.getType(ref), cs.simplifyType(adjustedOpenedType));

        // Reference to an unbound instance method.
        Expr *result = new (context) DotSyntaxBaseIgnoredExpr(base, dotLoc,
                                                              ref,
                                                              cs.getType(ref));
        cs.cacheType(result);
        closeExistentials(result, locator, /*force=*/openedExistential);
        return forceUnwrapIfExpected(result, memberLocator);
      } else {
        assert((!baseIsInstance || member->isInstanceMember()) &&
               "can't call a static method on an instance");
        ref = forceUnwrapIfExpected(ref, memberLocator);
        apply = DotSyntaxCallExpr::create(context, ref, dotLoc, Argument::unlabeled(base));
        if (Implicit) {
          apply->setImplicit();
        }
      }

      return finishApply(apply, adjustedOpenedType, locator, memberLocator);
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

    /// Build the function and argument list for a `@dynamicCallable`
    /// application.
    std::pair</*fn*/ Expr *, ArgumentList *>
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
    ///
    /// \returns the coerced expression, which will have type \c ToType.
    Expr *coerceToType(Expr *expr, Type toType,
                       ConstraintLocatorBuilder locator);

    /// Coerce the arguments in the provided argument list to their matching
    /// parameter types.
    ///
    /// This operation cannot fail.
    ///
    /// \param args The argument list.
    /// \param funcType The function type.
    /// \param callee The callee for the function being applied.
    /// \param apply The ApplyExpr that forms the call.
    /// \param locator Locator used to describe where in this expression we are.
    ///
    /// \returns The resulting ArgumentList.
    ArgumentList *coerceCallArguments(
        ArgumentList *args, AnyFunctionType *funcType, ConcreteDeclRef callee,
        ApplyExpr *apply, ConstraintLocatorBuilder locator,
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
    /// \param args The argument list of the subscript.
    /// \param locator The locator used to refer to the subscript.
    /// \param isImplicit Whether this is an implicit subscript.
    Expr *buildSubscript(Expr *base, ArgumentList *args,
                         ConstraintLocatorBuilder locator,
                         ConstraintLocatorBuilder memberLocator,
                         bool isImplicit, AccessSemantics semantics,
                         const SelectedOverload &selected) {
      // Build the new subscript.
      auto newSubscript = buildSubscriptHelper(base, args, selected,
                                               locator, isImplicit, semantics);
      return forceUnwrapIfExpected(newSubscript, memberLocator,
                                   IUOReferenceKind::ReturnValue);
    }

    Expr *buildSubscriptHelper(Expr *base, ArgumentList *args,
                               const SelectedOverload &selected,
                               ConstraintLocatorBuilder locator,
                               bool isImplicit, AccessSemantics semantics) {
      auto choice = selected.choice;
      auto &ctx = cs.getASTContext();

      // Apply a key path if we have one.
      if (choice.getKind() == OverloadChoiceKind::KeyPathApplication) {
        auto applicationTy =
            simplifyType(selected.adjustedOpenedType)->castTo<FunctionType>();

        // The index argument should be (keyPath: KeyPath<Root, Value>).
        // Dig the key path expression out of the arguments.
        auto *indexKP = args->getUnaryExpr();
        assert(indexKP);
        indexKP = cs.coerceToRValue(indexKP);
        auto keyPathExprTy = cs.getType(indexKP);
        auto keyPathTy = applicationTy->getParams().front().getOldType();

        Type valueTy;
        Type baseTy;
        bool resultIsLValue;
        
        if (auto nom = keyPathTy->getAs<NominalType>()) {
          // AnyKeyPath is <T> rvalue T -> rvalue Any?
          if (nom->isAnyKeyPath()) {
            valueTy = ctx.getAnyExistentialType();
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
                  .highlight(args->getSourceRange());
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
            valueTy = ctx.getAnyExistentialType();
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

        auto keyPathAp = new (cs.getASTContext()) KeyPathApplicationExpr(
            base, args->getStartLoc(), indexKP, args->getEndLoc(), valueTy,
            base->isImplicit() && args->isImplicit());
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

      // If we opened up an existential when performing the subscript, open
      // the base accordingly.
      auto memberLoc = cs.getCalleeLocator(cs.getConstraintLocator(locator));
      auto knownOpened = solution.OpenedExistentialTypes.find(memberLoc);
      if (knownOpened != solution.OpenedExistentialTypes.end()) {
        base = openExistentialReference(base, knownOpened->second, subscript);
        baseTy = knownOpened->second;
      }

      // Compute the concrete reference to the subscript.
      auto subscriptRef = resolveConcreteDeclRef(subscript, memberLoc);

      // Coerce the index argument.
      auto openedFullFnType = simplifyType(selected.adjustedOpenedFullType)
                                  ->castTo<FunctionType>();
      auto fullSubscriptTy = openedFullFnType->getResult()
                                  ->castTo<FunctionType>();
      auto &appliedWrappers =
          solution.appliedPropertyWrappers[memberLoc->getAnchor()];
      args = coerceCallArguments(
          args, fullSubscriptTy, subscriptRef, nullptr,
          locator.withPathElement(ConstraintLocator::ApplyArgument),
          appliedWrappers);
      if (!args)
        return nullptr;

      // Handle dynamic lookup.
      if (choice.getKind() == OverloadChoiceKind::DeclViaDynamic ||
          subscript->getAttrs().hasAttribute<OptionalAttr>()) {
        base = coerceSelfArgumentToType(base, baseTy, subscript, locator);
        if (!base)
          return nullptr;

        // TODO: diagnose if semantics != AccessSemantics::Ordinary?
        auto subscriptExpr = DynamicSubscriptExpr::create(
            ctx, base, args, subscriptRef, isImplicit);
        auto resultTy = simplifyType(selected.adjustedOpenedType)
                            ->castTo<FunctionType>()
                            ->getResult();
        assert(!selected.adjustedOpenedFullType->hasOpenedExistential()
               && "open existential archetype in AnyObject subscript type?");
        cs.setType(subscriptExpr, resultTy);
        Expr *result = subscriptExpr;
        closeExistentials(result, locator);
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
          ctx, base, args, subscriptRef, isImplicit, semantics);
      cs.setType(subscriptExpr, fullSubscriptTy->getResult());
      subscriptExpr->setIsSuper(isSuper);
      cs.setType(subscriptExpr,
                 hasDynamicSelf
                     ? fullSubscriptTy->getResult()->replaceCovariantResultType(
                           containerTy, 0)
                     : fullSubscriptTy->getResult());

      Expr *result = subscriptExpr;
      closeExistentials(result, locator);

      // If the element is of dynamic 'Self' type, wrap an implicit conversion
      // around the resulting expression, with the destination type having
      // 'Self' swapped for the appropriate replacement type -- usually the
      // base object type.
      if (hasDynamicSelf) {
        const auto conversionTy = simplifyType(
            selected.adjustedOpenedType->castTo<FunctionType>()->getResult());

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
      auto *resultTy =
          solution.simplifyType(openedFullType)->castTo<FunctionType>();

      const auto selfTy = getBaseType(resultTy);

      ParameterTypeFlags flags;
      if (!selfTy->hasReferenceSemantics())
        flags = flags.withInOut(true);

      auto selfParam = AnyFunctionType::Param(selfTy, Identifier(), flags);
      resultTy = FunctionType::get({selfParam}, resultTy->getResult(),
                                   resultTy->getExtInfo());

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
    Expr *buildKeyPathDynamicMemberArgExpr(BoundGenericType *keyPathTy,
                                           SourceLoc dotLoc,
                                           ConstraintLocator *memberLoc) {
      using Component = KeyPathExpr::Component;
      auto &ctx = cs.getASTContext();
      auto *anchor = getAsExpr(memberLoc->getAnchor());

      auto makeKeyPath = [&](ArrayRef<Component> components) -> Expr * {
        auto *kp = KeyPathExpr::createImplicit(ctx, /*backslashLoc*/ dotLoc,
                                               components, anchor->getEndLoc());
        kp->setType(keyPathTy);
        cs.cacheExprTypes(kp);

        // See whether there's an equivalent ObjC key path string we can produce
        // for interop purposes.
        checkAndSetObjCKeyPathString(kp);
        return kp;
      };

      SmallVector<Component, 2> components;
      auto *componentLoc = cs.getConstraintLocator(
          memberLoc,
          LocatorPathElt::KeyPathDynamicMember(keyPathTy->getAnyNominal()));
      auto overload = solution.getOverloadChoice(componentLoc);

      // Looks like there is a chain of implicit `subscript(dynamicMember:)`
      // calls necessary to resolve a member reference.
      switch (overload.choice.getKind()) {
      case OverloadChoiceKind::DynamicMemberLookup:
      case OverloadChoiceKind::KeyPathDynamicMemberLookup: {
        buildKeyPathSubscriptComponent(overload, dotLoc, /*args=*/nullptr,
                                       componentLoc, components);
        return makeKeyPath(components);
      }

      default:
        break;
      }

      if (auto *KPE = dyn_cast<KeyPathExpr>(anchor)) {
        // Looks like keypath dynamic member lookup was used inside
        // of a keypath expression e.g. `\Lens<[Int]>.count` where
        // `count` is referenced using dynamic lookup.
        auto kpElt = memberLoc->findFirst<LocatorPathElt::KeyPathComponent>();
        assert(kpElt && "no keypath component node");
        auto &comp = KPE->getComponents()[kpElt->getIndex()];

        if (comp.getKind() == Component::Kind::UnresolvedProperty) {
          buildKeyPathPropertyComponent(overload, comp.getLoc(), componentLoc,
                                        components);
        } else if (comp.getKind() == Component::Kind::UnresolvedSubscript) {
          buildKeyPathSubscriptComponent(overload, comp.getLoc(),
                                         comp.getSubscriptArgs(), componentLoc,
                                         components);
        } else {
          return nullptr;
        }
        return makeKeyPath(components);
      }

      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor)) {
        buildKeyPathPropertyComponent(overload, UDE->getLoc(), componentLoc,
                                      components);
      } else if (auto *SE = dyn_cast<SubscriptExpr>(anchor)) {
        buildKeyPathSubscriptComponent(overload, SE->getLoc(), SE->getArgs(),
                                       componentLoc, components);
      } else {
        return nullptr;
      }
      return makeKeyPath(components);
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
                                          dc->getParentModule());

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
            assert(type->isEqual(genericSig.getGenericParams()[0]));
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
                 Optional<SyntacticElementTarget> target,
                 bool suppressDiagnostics)
        : cs(cs), dc(target ? target->getDeclContext() : cs.DC),
          solution(solution), target(target),
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

      if (!TypeChecker::conformsToProtocol(type, protocol, dc->getParentModule())) {
        // If the type does not conform to ExpressibleByStringLiteral, it should
        // be ExpressibleByExtendedGraphemeClusterLiteral.
        protocol = TypeChecker::getProtocol(
            cs.getASTContext(), expr->getLoc(),
            KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral);
        isStringLiteral = false;
        isGraphemeClusterLiteral = true;
      }
      if (!TypeChecker::conformsToProtocol(type, protocol, dc->getParentModule())) {
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
          TypeChecker::conformsToProtocol(type, proto, dc->getParentModule());
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
        LiteralExpr *expr = IntegerLiteralExpr::createFromUnsigned(ctx, value, loc);
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

    Expr *visitRegexLiteralExpr(RegexLiteralExpr *expr) {
      simplifyExprType(expr);
      expr->setInitializer(
          cs.getASTContext().getRegexInitDecl(cs.getType(expr)));
      return expr;
    }

    Expr *visitMagicIdentifierLiteralExpr(MagicIdentifierLiteralExpr *expr) {
#if SWIFT_SWIFT_PARSER
      auto &ctx = cs.getASTContext();
      if (ctx.LangOpts.hasFeature(Feature::BuiltinMacros)) {
        auto expandedType = solution.simplifyType(solution.getType(expr));
        cs.setType(expr, expandedType);

        auto locator = cs.getConstraintLocator(expr);
        auto overload = solution.getOverloadChoice(locator);

        auto macro = cast<MacroDecl>(overload.choice.getDecl());
        ConcreteDeclRef macroRef = resolveConcreteDeclRef(macro, locator);
        auto expansion = new (ctx) MacroExpansionExpr(
            dc, expr->getStartLoc(), DeclNameRef(macro->getName()),
            DeclNameLoc(expr->getLoc()), SourceLoc(), {}, SourceLoc(), nullptr,
            MacroRole::Expression, /*isImplicit=*/true, expandedType);
        expansion->setMacroRef(macroRef);
        (void)evaluateOrDefault(
            ctx.evaluator, ExpandMacroExpansionExprRequest{expansion}, None);
        if (expansion->getRewritten()) {
          cs.cacheExprTypes(expansion);
          return expansion;
        }

        // Fall through to use old implementation.
      }
#endif

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
                                        dc->getParentModule());
      assert(conformance && "object literal type conforms to protocol");

      auto constrName = TypeChecker::getObjectLiteralConstructorName(ctx, expr);

      ConcreteDeclRef witness = conformance.getWitnessByName(
          conformingType->getRValueType(), constrName);

      auto selectedOverload = solution.getOverloadChoiceIfAvailable(
          cs.getConstraintLocator(expr, ConstraintLocator::ConstructorMember));

      if (!selectedOverload)
        return nullptr;

      auto fnType =
          simplifyType(selectedOverload->adjustedOpenedType)->castTo<FunctionType>();

      auto newArgs = coerceCallArguments(
          expr->getArgs(), fnType, witness, /*applyExpr=*/nullptr,
          cs.getConstraintLocator(expr, ConstraintLocator::ApplyArgument),
          /*appliedPropertyWrappers=*/{});

      expr->setInitializer(witness);
      expr->setArgs(newArgs);
      return expr;
    }

    /// Add an implicit force unwrap of an expression that references an
    /// implicitly unwrapped optional T!.
    Expr *forceUnwrapIUO(Expr *expr) {
      auto optTy = cs.getType(expr);
      auto objectTy = optTy->getWithoutSpecifierType()->getOptionalObjectType();
      assert(objectTy && "Trying to unwrap non-optional?");

      // Preserve l-valueness of the result.
      if (optTy->is<LValueType>())
        objectTy = LValueType::get(objectTy);

      expr = new (cs.getASTContext()) ForceValueExpr(expr, expr->getEndLoc(),
                                                     /*forcedIUO*/ true);
      cs.setType(expr, objectTy);
      expr->setImplicit();
      return expr;
    }

    /// Retrieve the number of implicit force unwraps required for an implicitly
    /// unwrapped optional reference at a given locator.
    unsigned getIUOForceUnwrapCount(ConstraintLocatorBuilder locator,
                                    IUOReferenceKind refKind) {
      // Adjust the locator depending on the type of IUO reference.
      auto loc = locator;
      switch (refKind) {
      case IUOReferenceKind::ReturnValue:
        loc = locator.withPathElement(ConstraintLocator::FunctionResult);
        break;
      case IUOReferenceKind::Value:
        break;
      }
      auto *iuoLocator = cs.getConstraintLocator(
          loc, {ConstraintLocator::ImplicitlyUnwrappedDisjunctionChoice});
      auto *dynamicLocator = cs.getConstraintLocator(
          loc, {ConstraintLocator::DynamicLookupResult});

      // First check whether we recorded an implicit unwrap for an IUO.
      unsigned unwrapCount = 0;
      if (solution.DisjunctionChoices.lookup(iuoLocator))
        unwrapCount += 1;

      // Next check if we recorded an implicit unwrap for dynamic lookup.
      if (solution.DisjunctionChoices.lookup(dynamicLocator))
        unwrapCount += 1;

      return unwrapCount;
    }

    Expr *
    forceUnwrapIfExpected(Expr *expr, ConstraintLocatorBuilder locator,
                          IUOReferenceKind refKind = IUOReferenceKind::Value) {
      auto unwrapCount = getIUOForceUnwrapCount(locator, refKind);
      for (unsigned i = 0; i < unwrapCount; ++i)
        expr = forceUnwrapIUO(expr);
      return expr;
    }

    Expr *visitDeclRefExpr(DeclRefExpr *expr) {
      auto locator = cs.getConstraintLocator(expr);

      // Check whether this is a reference to `__buildSelf`, and if so,
      // replace it with a type expression with fully resolved type.
      if (auto *var = dyn_cast<VarDecl>(expr->getDecl())) {
        auto &ctx = cs.getASTContext();
        if (var->getName() == ctx.Id_builderSelf) {
          assert(expr->isImplicit() && var->isImplicit());
          auto builderTy =
            solution.getResolvedType(var)->getMetatypeInstanceType();

          return cs.cacheType(
            TypeExpr::createImplicitHack(expr->getLoc(), builderTy, ctx));
        }
      }

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
                   dc->getInnermostMethodContext())) {
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
          if (auto func = dc->getInnermostMethodContext()) {
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
      Expr *ctorRef = buildOtherConstructorRef(overload.adjustedOpenedFullType, callee,
                                               base, nameLoc, ctorLocator,
                                               implicit);
      auto *call =
          DotSyntaxCallExpr::create(cs.getASTContext(), ctorRef, dotLoc,
                                    Argument::unlabeled(base));

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
          TypeChecker::lookupUnqualified(dc, UDE->getName(), UDE->getLoc());
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
        auto classTy = ctx.getBridgedToObjC(dc, baseInstTy);

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

      case OverloadChoiceKind::MaterializePack: {
        auto baseTy = solution.getResolvedType(base);

        // Load the base tuple if necessary, materialization
        // operates on r-value types only.
        if (baseTy->is<LValueType>())
          base = coerceToType(base, baseTy->getRValueType(),
                              cs.getConstraintLocator(base));

        auto packType = solution.getResolvedType(expr);
        return cs.cacheType(
            MaterializePackExpr::create(cs.getASTContext(),
                                        base, expr->getEndLoc(),
                                        packType));
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

    /// Form a type checked expression for the argument of a
    /// @dynamicMemberLookup subscript index parameter.
    Expr *buildDynamicMemberLookupArgExpr(StringRef name, SourceLoc loc,
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
        argExpr = buildDynamicMemberLookupArgExpr(fieldName, nameLoc, paramTy);
      } else {
        argExpr = buildKeyPathDynamicMemberArgExpr(
            paramTy->castTo<BoundGenericType>(), dotLoc, memberLocator);
      }

      if (!argExpr)
        return nullptr;

      // Build an argument list.
      auto *argList =
          ArgumentList::forImplicitSingle(ctx, ctx.Id_dynamicMember, argExpr);
      // Build and return a subscript that uses this string as the index.
      return buildSubscript(base, argList, cs.getConstraintLocator(expr),
                            memberLocator, /*isImplicit*/ true,
                            AccessSemantics::Ordinary, overload);
    }

    Type getTypeOfDynamicMemberIndex(const SelectedOverload &overload) {
      assert(overload.choice.isAnyDynamicMemberLookup());

      auto declTy = solution.simplifyType(overload.adjustedOpenedFullType);
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

    Expr *visitCopyExpr(CopyExpr *expr) {
      auto toType = simplifyType(cs.getType(expr));
      cs.setType(expr, toType);

      auto *subExpr = expr->getSubExpr();
      auto type = simplifyType(cs.getType(subExpr));

      // Let's load the value associated with this try.
      if (type->hasLValueType()) {
        subExpr = coerceToType(subExpr, type->getRValueType(),
                               cs.getConstraintLocator(subExpr));

        if (!subExpr)
          return nullptr;
      }

      expr->setSubExpr(subExpr);

      return expr;
    }

    Expr *visitConsumeExpr(ConsumeExpr *expr) {
      auto toType = simplifyType(cs.getType(expr));
      cs.setType(expr, toType);

      auto *subExpr = expr->getSubExpr();
      auto type = simplifyType(cs.getType(subExpr));

      // Let's load the value associated with this consume.
      if (type->hasLValueType()) {
        subExpr = coerceToType(subExpr, type->getRValueType(),
                               cs.getConstraintLocator(subExpr));

        if (!subExpr)
          return nullptr;
      }

      expr->setSubExpr(subExpr);

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
      Expr *result = expr;
      auto type = simplifyType(cs.getType(expr));

      // A ParenExpr can end up with a tuple type if it contains
      // a pack expansion. Rewrite it to a TupleExpr.
      if (dyn_cast<PackExpansionExpr>(expr->getSubExpr())) {
        auto &ctx = cs.getASTContext();
        result = TupleExpr::create(ctx, expr->getLParenLoc(),
                                   {expr->getSubExpr()},
                                   /*elementNames=*/{},
                                   /*elementNameLocs=*/{},
                                   expr->getRParenLoc(),
                                   expr->isImplicit());
      }

      cs.setType(result, type);
      return result;
    }

    Expr *visitUnresolvedMemberChainResultExpr(
        UnresolvedMemberChainResultExpr *expr) {
      // Since this expression only exists to give the result type of an
      // unresolved member chain visibility in the AST, remove it from the AST
      // now that we have a solution and coerce the subexpr to the resulting
      // type.
      auto *subExpr = expr->getSubExpr();
      auto type = simplifyType(cs.getType(expr));
      subExpr = coerceToType(
          subExpr, type,
          cs.getConstraintLocator(
              expr, ConstraintLocator::UnresolvedMemberChainResult));
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

      if (overload->choice.isKeyPathDynamicMemberLookup()) {
        return buildDynamicMemberLookupRef(
            expr, expr->getBase(), expr->getArgs()->getStartLoc(), SourceLoc(),
            *overload, memberLocator);
      }

      return buildSubscript(expr->getBase(), expr->getArgs(),
                            cs.getConstraintLocator(expr), memberLocator,
                            expr->isImplicit(), expr->getAccessSemantics(),
                            *overload);
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
                                        dc->getParentModule());
      assert(conformance && "Type does not conform to protocol?");

      DeclName name(ctx, DeclBaseName::createConstructor(),
                    {ctx.Id_arrayLiteral});
      ConcreteDeclRef witness =
          conformance.getWitnessByName(arrayTy->getRValueType(), name);
      if (!witness || !isa<AbstractFunctionDecl>(witness.getDecl()))
        return nullptr;
      expr->setInitializer(witness);

      auto elementType = expr->getElementType();

      for (unsigned i = 0, n = expr->getNumElements(); i != n; ++i) {
        expr->setElement(
            i, coerceToType(expr->getElement(i), elementType,
                            cs.getConstraintLocator(
                                expr, {LocatorPathElt::TupleElement(i)})));
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
                                        dc->getParentModule());
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

      for (unsigned i = 0, n = expr->getNumElements(); i != n; ++i) {
        expr->setElement(
            i, coerceToType(expr->getElement(i), elementType,
                            cs.getConstraintLocator(
                                expr, {LocatorPathElt::TupleElement(i)})));
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
      return buildSubscript(expr->getBase(), expr->getArgs(),
                            cs.getConstraintLocator(expr), memberLocator,
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

    Expr *visitPackExpansionExpr(PackExpansionExpr *expr) {
      simplifyExprType(expr);

      // Set the opened pack element environment for this pack expansion.
      auto expansionTy = cs.getType(expr)->castTo<PackExpansionType>();
      auto *locator = cs.getConstraintLocator(expr);
      auto *environment = cs.getPackElementEnvironment(locator,
          expansionTy->getCountType()->getCanonicalType());
      expr->setGenericEnvironment(environment);

      return expr;
    }

    Expr *visitPackElementExpr(PackElementExpr *expr) {
      return simplifyExprType(expr);
    }

    Expr *visitMaterializePackExpr(MaterializePackExpr *expr) {
      llvm_unreachable("MaterializePackExpr already type-checked");
    }

    Expr *visitDynamicTypeExpr(DynamicTypeExpr *expr) {
      Expr *base = expr->getBase();
      base = cs.coerceToRValue(base);
      expr->setBase(base);

      return simplifyExprType(expr);
    }

    Expr *visitOpaqueValueExpr(OpaqueValueExpr *expr) {
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
      Expr *subExpr = expr->getSubExpr();
      auto *inCtor = cast<ConstructorDecl>(dc->getInnermostMethodContext());

      // A non-failable initializer cannot delegate nor chain to
      //  * a throwing initializer via 'try?'
      //  * a failable initializer, unless the failability kind is IUO or the
      //    result is explicitly forced
      if (!inCtor->isFailable() && cs.getType(subExpr)->isOptional()) {
        bool isChaining;
        auto *otherCtorRef = expr->getCalledConstructor(isChaining);
        auto *otherCtor = otherCtorRef->getDecl();
        assert(otherCtor);

        Expr *newSubExpr = subExpr;
        auto &de = cs.getASTContext().Diags;

        // Look through 'try', 'try!', and identity expressions.
        subExpr = subExpr->getValueProvidingExpr();

        // Diagnose if we find a 'try?'.
        // FIXME: We could put up with occurrences of 'try?' if they do not apply
        // directly to the called ctor, e.g. 'try? try self.init()', or if the
        // called ctor isn't throwing.
        if (auto *OTE = dyn_cast<OptionalTryExpr>(subExpr)) {
          if (SuppressDiagnostics)
            return nullptr;

          // Give the user the option of using 'try!' or making the enclosing
          // initializer failable.
          de.diagnose(OTE->getTryLoc(),
                      diag::delegate_chain_nonoptional_to_optional_try,
                      isChaining);
          de.diagnose(OTE->getTryLoc(), diag::init_delegate_force_try)
              .fixItReplace({OTE->getTryLoc(), OTE->getQuestionLoc()}, "try!");
          de.diagnose(inCtor->getLoc(), diag::init_propagate_failure)
              .fixItInsertAfter(inCtor->getLoc(), "?");

          subExpr = OTE->getSubExpr();
        }

        while (true) {
          subExpr = subExpr->getSemanticsProvidingExpr();

          // Look through optional injections.
          if (auto *IIOE = dyn_cast<InjectIntoOptionalExpr>(subExpr)) {
            subExpr = IIOE->getSubExpr();
            continue;
          }

          // Look through all try expressions.
          if (auto *ATE = dyn_cast<AnyTryExpr>(subExpr)) {
            subExpr = ATE->getSubExpr();
            continue;
          }

          break;
        }

        // If we're still hitting an optional and the called ctor is failable,
        // diagnose only if the failability kind is not IUO. Note that the
        // expression type alone is not always indicative of the ctor's
        // failability, because it could be declared on 'Optional' itself.
        if (cs.getType(subExpr)->isOptional() && otherCtor->isFailable()) {
          if (!otherCtor->isImplicitlyUnwrappedOptional()) {
            if (SuppressDiagnostics)
              return nullptr;

            // Give the user the option of adding '!' or making the enclosing
            // initializer failable.
            de.diagnose(otherCtorRef->getLoc(),
                        diag::delegate_chain_nonoptional_to_optional,
                        isChaining, otherCtor->getName());
            de.diagnose(otherCtorRef->getLoc(), diag::init_force_unwrap)
                .fixItInsertAfter(expr->getEndLoc(), "!");
            de.diagnose(inCtor->getLoc(), diag::init_propagate_failure)
                .fixItInsertAfter(inCtor->getLoc(), "?");
          }

          // Recover by injecting the force operation (the first option).
          newSubExpr = forceUnwrapIUO(newSubExpr);
        }

        expr->setSubExpr(newSubExpr);
      }

      return expr;
    }

    Expr *visitTernaryExpr(TernaryExpr *expr) {
      auto resultTy = simplifyType(cs.getType(expr));
      cs.setType(expr, resultTy);

      auto cond = cs.coerceToRValue(expr->getCondExpr());
      expr->setCondExpr(cond);

      // Coerce the then/else branches to the common type.
      expr->setThenExpr(coerceToType(
          expr->getThenExpr(), resultTy,
          cs.getConstraintLocator(expr, LocatorPathElt::TernaryBranch(true))));
      expr->setElseExpr(coerceToType(
          expr->getElseExpr(), resultTy,
          cs.getConstraintLocator(expr, LocatorPathElt::TernaryBranch(false))));

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

      auto castKind = TypeChecker::typeCheckCheckedCast(
          fromType, toType, CheckedCastContextKind::IsExpr, dc);

      switch (castKind) {
      case CheckedCastKind::Unresolved:
        expr->setCastKind(CheckedCastKind::ValueCast);
        break;
          
      case CheckedCastKind::Coercion:
      case CheckedCastKind::BridgingCoercion:
      case CheckedCastKind::ValueCast:
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
      auto *coerced = visitCoerceExprImpl(expr);
      if (!coerced)
        return nullptr;

      // If we need to insert a force-unwrap for coercions of the form
      // 'as T!', do so now.
      if (hasForcedOptionalResult(expr))
        coerced = forceUnwrapIUO(coerced);

      return coerced;
    }

    Expr *visitCoerceExprImpl(CoerceExpr *expr) {
      if (auto *castTypeRepr = expr->getCastTypeRepr()) {
        // Simplify and update the type we're coercing to.
        auto toType = simplifyType(cs.getType(castTypeRepr));
        expr->setCastType(toType);
        cs.setType(castTypeRepr, toType);
      } else {
        assert(expr->isImplicit());
        assert(expr->getCastType());
      }

      cs.setType(expr, expr->getCastType());

      // If this is a literal that got converted into constructor call
      // lets put proper source information in place.
      if (expr->isLiteralInit()) {
        auto toType = expr->getCastType();
        auto *literalInit = expr->getSubExpr();
        if (auto *call = dyn_cast<CallExpr>(literalInit)) {
          cs.forEachExpr(call->getFn(), [&](Expr *subExpr) -> Expr * {
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
      auto *locator =
          cs.getConstraintLocator(expr, ConstraintLocator::CoercionOperand);
      auto choice = solution.getDisjunctionChoice(locator);

      // Handle the coercion/bridging of the underlying subexpression, where
      // optionality has been removed.
      if (choice == 0) {
        // Convert the subexpression.
        Expr *sub = expr->getSubExpr();

        sub = solution.coerceToType(sub, expr->getCastType(),
                                    cs.getConstraintLocator(sub));
        if (!sub)
          return nullptr;

        expr->setSubExpr(sub);
        return expr;
      }

      // Bridging conversion.
      assert(choice == 1 && "should be bridging");

      // Handle optional bindings.
      Expr *sub = handleOptionalBindings(
          expr->getSubExpr(), expr->getCastType(),
          OptionalBindingsCastKind::Bridged,
          [&](Expr *sub, Type toInstanceType) {
            return buildObjCBridgeExpr(sub, toInstanceType, locator);
          });

      if (!sub)
        return nullptr;

      expr->setSubExpr(sub);
      return expr;
    }

    // Rewrite ForcedCheckedCastExpr based on what the solver computed.
    Expr *visitForcedCheckedCastExpr(ForcedCheckedCastExpr *expr) {
      // The subexpression is always an rvalue.
      auto sub = cs.coerceToRValue(expr->getSubExpr());
      expr->setSubExpr(sub);

      const auto fromType = cs.getType(sub);

      Type toType;
      SourceRange castTypeRange;

      // Simplify and update the type we're casting to.
      if (auto *const castTypeRepr = expr->getCastTypeRepr()) {
        toType = simplifyType(cs.getType(castTypeRepr));
        castTypeRange = castTypeRepr->getSourceRange();

        cs.setType(castTypeRepr, toType);
        expr->setCastType(toType);
      } else {
        assert(expr->isImplicit());
        assert(expr->getCastType());

        toType = expr->getCastType();
      }

      if (hasForcedOptionalResult(expr))
        toType = toType->getOptionalObjectType();

      const auto castKind = TypeChecker::typeCheckCheckedCast(
          fromType, toType, CheckedCastContextKind::ForcedCast, dc);
      switch (castKind) {
        /// Invalid cast.
      case CheckedCastKind::Unresolved:
        if (expr->isImplicit())
          return nullptr;

        expr->setCastKind(CheckedCastKind::ValueCast);
        break;
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
      auto *const castTypeRepr = expr->getCastTypeRepr();

      // If there is no type repr, it means this is implicit cast which
      // should have a type set.
      if (!castTypeRepr) {
        assert(expr->isImplicit());
        assert(expr->getCastType());

        auto sub = cs.coerceToRValue(expr->getSubExpr());
        expr->setSubExpr(sub);

        return expr;
      }

      // Simplify and update the type we're casting to.
      const auto toType = simplifyType(cs.getType(castTypeRepr));
      expr->setCastType(toType);

      cs.setType(castTypeRepr, toType);

      // If we need to insert a force-unwrap for coercions of the form
      // 'as! T!', do so now.
      if (hasForcedOptionalResult(expr)) {
        auto *coerced = handleConditionalCheckedCastExpr(expr, castTypeRepr);
        if (!coerced)
          return nullptr;

        return forceUnwrapIUO(coerced);
      }

      return handleConditionalCheckedCastExpr(expr, castTypeRepr);
    }

    Expr *handleConditionalCheckedCastExpr(ConditionalCheckedCastExpr *expr,
                                           TypeRepr *castTypeRepr) {
      assert(castTypeRepr &&
             "cast requires TypeRepr; implicit casts are superfluous");

      // The subexpression is always an rvalue.
      auto sub = cs.coerceToRValue(expr->getSubExpr());
      expr->setSubExpr(sub);

      // Simplify and update the type we're casting to.
      const auto fromType = cs.getType(sub);
      const auto toType = expr->getCastType();

      auto castKind = TypeChecker::typeCheckCheckedCast(
          fromType, toType, CheckedCastContextKind::ConditionalCast, dc);
      switch (castKind) {
      // Invalid cast.
      case CheckedCastKind::Unresolved:
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
      // Conversion is recorded as anchored on an assignment itself by
      // constraint generator and that has to be preserved here in case
      // anything depends on the locator (i.e. Double<->CGFloat implicit
      // conversion).
      Expr *src = coerceToType(expr->getSrc(), destTy->getRValueType(),
                               cs.getConstraintLocator(expr));
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
                    expr->getSubPattern()->getDescriptiveKind());
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
      return simplifyExprType(expr);
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

      auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {argExpr});
      Expr *callExpr = CallExpr::createImplicit(ctx, fnRef, argList);

      auto resultTy = TypeChecker::typeCheckExpression(
          callExpr, dc, /*contextualInfo=*/{valueType, CTP_CannotFail});
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
          bool isSettable = var->isSettable(dc) &&
            var->isSetterAccessibleFrom(dc);
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
          if (!var->isSettable(dc)) {
            de.diagnose(E->getLoc(), diag::expr_selector_property_not_settable,
                        var->getDescriptiveKind(), var->getName());
            de.diagnose(var, diag::decl_declared_here, var->getName());
            return E;
          }

          // Make sure the setter is accessible.
          if (!var->isSetterAccessibleFrom(dc)) {
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

          isDynamicMember = foundDecl->choice.isAnyDynamicMemberLookup();

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
          buildKeyPathSubscriptComponent(solution.getOverloadChoice(calleeLoc),
                                         origComponent.getLoc(),
                                         origComponent.getSubscriptArgs(),
                                         componentLocator, resolvedComponents);
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
        case KeyPathExpr::Component::Kind::Invalid:
        case KeyPathExpr::Component::Kind::CodeCompletion: {
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
      E->setComponents(cs.getASTContext(), resolvedComponents);
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
      // subscripts) are only evaluated once, we use a capture list to evaluate
      // the key path immediately and capture it in the function value created.
      // The result looks like:
      //
      //     return "{ [$kp$ = \(E)] in $0[keyPath: $kp$] }"

      auto &ctx = cs.getASTContext();

      FunctionType::ExtInfo closureInfo;
      auto closureTy =
          FunctionType::get({FunctionType::Param(baseTy)}, leafTy, closureInfo);
      auto closure = new (ctx)
          AutoClosureExpr(/*set body later*/nullptr, leafTy, dc);

      auto param = new (ctx) ParamDecl(
          SourceLoc(),
          /*argument label*/ SourceLoc(), Identifier(),
          /*parameter name*/ SourceLoc(), ctx.getIdentifier("$0"), closure);
      param->setInterfaceType(baseTy->mapTypeOutOfContext());
      param->setSpecifier(ParamSpecifier::Default);
      param->setImplicit();
      
      auto params = ParameterList::create(ctx, SourceLoc(),
                                          param, SourceLoc());

      closure->setParameterList(params);

      // The capture list.
      VarDecl *outerParam = new (ctx) VarDecl(/*static*/ false,
                                          VarDecl::Introducer::Let,
                                          SourceLoc(),
                                          ctx.getIdentifier("$kp$"),
                                          dc);
      outerParam->setImplicit();
      outerParam->setInterfaceType(keyPathTy->mapTypeOutOfContext());
      
      NamedPattern *outerParamPat = new (ctx) NamedPattern(outerParam);
      outerParamPat->setImplicit();
      outerParamPat->setType(keyPathTy);
      
      auto outerParamPBE = PatternBindingEntry(outerParamPat,
                                               SourceLoc(), E, dc);
      solution.setExprTypes(E);
      auto outerParamDecl = PatternBindingDecl::create(ctx, SourceLoc(),
                                                       {}, SourceLoc(),
                                                       outerParamPBE,
                                                       dc);
      outerParamDecl->setImplicit();
      auto outerParamCapture = CaptureListEntry(outerParamDecl);
      auto captureExpr = CaptureListExpr::create(ctx, outerParamCapture,
                                                 closure);
      captureExpr->setImplicit();

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
      
      captureExpr->setType(closureTy);
      cs.cacheType(captureExpr);
      
      return coerceToType(captureExpr, exprType, cs.getConstraintLocator(E));
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
      auto resolvedTy = simplifyType(overload.adjustedOpenedType);
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

      auto unwrapCount =
          getIUOForceUnwrapCount(locator, IUOReferenceKind::Value);
      for (unsigned i = 0; i < unwrapCount; ++i)
        buildKeyPathOptionalForceComponent(components);
    }

    void buildKeyPathSubscriptComponent(
        const SelectedOverload &overload, SourceLoc componentLoc,
        ArgumentList *args, ConstraintLocator *locator,
        SmallVectorImpl<KeyPathExpr::Component> &components) {
      auto &ctx = cs.getASTContext();
      auto subscript = cast<SubscriptDecl>(overload.choice.getDecl());
      assert(!subscript->isGetterMutating());
      auto memberLoc = cs.getCalleeLocator(locator);

      // Compute substitutions to refer to the member.
      auto ref = resolveConcreteDeclRef(subscript, memberLoc);

      // If this is a @dynamicMemberLookup reference to resolve a property
      // through the subscript(dynamicMember:) member, restore the
      // openedType and origComponent to its full reference as if the user
      // wrote out the subscript manually.
      if (overload.choice.isAnyDynamicMemberLookup()) {
        auto indexType = getTypeOfDynamicMemberIndex(overload);
        Expr *argExpr = nullptr;
        if (overload.choice.isKeyPathDynamicMemberLookup()) {
          argExpr = buildKeyPathDynamicMemberArgExpr(
              indexType->castTo<BoundGenericType>(), componentLoc, memberLoc);
        } else {
          auto fieldName = overload.choice.getName().getBaseIdentifier().str();
          argExpr = buildDynamicMemberLookupArgExpr(fieldName, componentLoc,
                                                    indexType);
        }
        args = ArgumentList::forImplicitSingle(ctx, ctx.Id_dynamicMember,
                                               argExpr);
        // Record the implicit subscript expr's parameter bindings and matching
        // direction as `coerceCallArguments` requires them.
        solution.recordSingleArgMatchingChoice(locator);
      }

      auto subscriptType =
          simplifyType(overload.adjustedOpenedType)->castTo<AnyFunctionType>();
      auto resolvedTy = subscriptType->getResult();

      // Coerce the indices to the type the subscript expects.
      args = coerceCallArguments(
          args, subscriptType, ref, /*applyExpr*/ nullptr,
          cs.getConstraintLocator(locator, ConstraintLocator::ApplyArgument),
          /*appliedPropertyWrappers*/ {});

      // We need to be able to hash the captured index values in order for
      // KeyPath itself to be hashable, so check that all of the subscript
      // index components are hashable and collect their conformances here.
      SmallVector<ProtocolConformanceRef, 4> conformances;

      auto hashable = ctx.getProtocol(KnownProtocolKind::Hashable);

      auto fnType = overload.adjustedOpenedType->castTo<FunctionType>();
      SmallVector<Identifier, 4> newLabels;
      for (auto &param : fnType->getParams()) {
        newLabels.push_back(param.getLabel());

        auto indexType = simplifyType(param.getParameterType());
        // Index type conformance to Hashable protocol has been
        // verified by the solver, we just need to get it again
        // with all of the generic parameters resolved.
        auto hashableConformance =
          TypeChecker::conformsToProtocol(indexType, hashable,
                                          dc->getParentModule());
        assert(hashableConformance);

        conformances.push_back(hashableConformance);
      }

      auto comp = KeyPathExpr::Component::forSubscript(
          ctx, ref, args, resolvedTy, ctx.AllocateCopy(conformances));
      components.push_back(comp);

      auto unwrapCount =
          getIUOForceUnwrapCount(memberLoc, IUOReferenceKind::ReturnValue);
      for (unsigned i = 0; i < unwrapCount; ++i)
        buildKeyPathOptionalForceComponent(components);
    }

    Expr *visitKeyPathDotExpr(KeyPathDotExpr *E) {
      llvm_unreachable("found KeyPathDotExpr in CSApply");
    }

    Expr *visitSingleValueStmtExpr(SingleValueStmtExpr *E) {
      llvm_unreachable("Handled by the walker directly");
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

    Expr *visitTypeJoinExpr(TypeJoinExpr *E) {
      llvm_unreachable("already type-checked?");
    }

    Expr *visitMacroExpansionExpr(MacroExpansionExpr *E) {
      auto expandedType = solution.simplifyType(solution.getType(E));
      cs.setType(E, expandedType);

      auto locator = cs.getConstraintLocator(E);
      auto overload = solution.getOverloadChoice(locator);

      auto macro = cast<MacroDecl>(overload.choice.getDecl());
      ConcreteDeclRef macroRef = resolveConcreteDeclRef(macro, locator);
      E->setMacroRef(macroRef);
      E->setType(expandedType);

      // FIXME: Expansion should be lazy.
      // i.e. 'ExpandMacroExpansionExprRequest' should be sinked into
      // 'getRewritten()', and performed on-demand.
      if (!cs.Options.contains(ConstraintSystemFlags::DisableMacroExpansions) &&
          // Do not expand macros inside macro arguments. For example for
          // '#stringify(#assert(foo))' when typechecking `#assert(foo)`,
          // we don't want to expand it.
          llvm::none_of(makeArrayRef(ExprStack).drop_back(1),
                       [](Expr *E) { return isa<MacroExpansionExpr>(E); })) {
        (void)evaluateOrDefault(cs.getASTContext().evaluator,
                                ExpandMacroExpansionExprRequest{E}, None);
      }

      cs.cacheExprTypes(E);
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
  auto *param = getParameterAt(owner, index);
  assert(param);
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
    convertedElts.emplace_back(toEltType, toLabel);
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
    fromInstanceType = fromInstanceType->getMetatypeInstanceType();
    toInstanceType = toInstanceType->getMetatypeInstanceType();
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
    auto fromArchetype =
        OpenedArchetypeType::getAny(fromType->getCanonicalType(),
                                    dc->getGenericSignatureOfContext());

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
        fromType, proto, module, false, /*allowMissing=*/true));
  }

  return toType->getASTContext().AllocateCopy(conformances);
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
                                             ConstraintLocatorBuilder locator) {
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
  expr = coerceToType(expr, toValueType, locator);
  if (!expr) return nullptr;

  unsigned depth = getOptionalEvaluationDepth(expr, bindOptional);
  bindOptional->setDepth(depth);

  expr = cs.cacheType(new (ctx) InjectIntoOptionalExpr(expr, toType));

  expr = cs.cacheType(new (ctx) OptionalEvaluationExpr(expr, toType));
  expr->setImplicit(true);
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
      Expr *expr, bool implicitSelfCapture, bool inheritActorContext) {
  if (auto closure = dyn_cast<ClosureExpr>(expr)) {
    closure->setAllowsImplicitSelfCapture(implicitSelfCapture);
    closure->setInheritsActorContext(inheritActorContext);
    return;
  }

  if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
    applyContextualClosureFlags(
        captureList->getClosureBody(), implicitSelfCapture,
        inheritActorContext);
  }

  if (auto identity = dyn_cast<IdentityExpr>(expr)) {
    applyContextualClosureFlags(
        identity->getSubExpr(), implicitSelfCapture, inheritActorContext);
  }
}

// For variadic generic declarations we need to compute a substituted
// version of bindings because all of the packs are exploaded in the
// substituted function type.
//
// \code
// func fn<each T>(_: repeat each T) {}
//
// fn("", 42)
// \endcode
//
// The type of `fn` in the call is `(String, Int) -> Void` but bindings
// have only one parameter at index `0` with two argument positions: 0, 1.
static bool shouldSubstituteParameterBindings(ConcreteDeclRef callee) {
  auto subst = callee.getSubstitutions();
  if (subst.empty())
    return false;

  auto sig = subst.getGenericSignature();
  return llvm::any_of(
      sig.getGenericParams(),
      [&](const GenericTypeParamType *GP) { return GP->isParameterPack(); });
}

/// Compute parameter binding substitutions by exploding pack expansions
/// into multiple bindings (if they matched more than one argument) and
/// ignoring empty ones.
static void computeParameterBindingsSubstitutions(
    ConcreteDeclRef callee, ArrayRef<AnyFunctionType::Param> params,
    ArrayRef<ParamBinding> origBindings,
    SmallVectorImpl<ParamBinding> &substitutedBindings) {
  for (unsigned bindingIdx = 0, numBindings = origBindings.size();
       bindingIdx != numBindings; ++bindingIdx) {
    if (origBindings[bindingIdx].size() > 1) {
      const auto &param = params[substitutedBindings.size()];
      if (!param.isVariadic()) {
#ifndef NDEBUG
        auto *PD = getParameterAt(callee.getDecl(), bindingIdx);
        assert(PD && PD->getInterfaceType()->is<PackExpansionType>());
#endif
        // Explode binding set to match substituted function parameters.
        for (auto argIdx : origBindings[bindingIdx])
          substitutedBindings.push_back({argIdx});
        continue;
      }
    }

    const auto &bindings = origBindings[bindingIdx];
    if (bindings.size() == 0) {
      auto *PD = getParameterAt(callee.getDecl(), bindingIdx);
      // Skip pack expansions with no arguments because they are not
      // present in the substituted function type.
      if (PD->getInterfaceType()->is<PackExpansionType>())
        continue;
    }

    substitutedBindings.push_back(bindings);
  }
}

ArgumentList *ExprRewriter::coerceCallArguments(
    ArgumentList *args, AnyFunctionType *funcType, ConcreteDeclRef callee,
    ApplyExpr *apply, ConstraintLocatorBuilder locator,
    ArrayRef<AppliedPropertyWrapper> appliedPropertyWrappers) {
  assert(locator.endsWith<LocatorPathElt::ApplyArgument>());

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
  auto matches = args->matches(params, [&](Expr *E) { return cs.getType(E); });
  if (matches && !shouldInjectWrappedValuePlaceholder &&
      !paramInfo.anyContextualInfo()) {
    return args;
  }

  // Determine the parameter bindings that were applied.
  auto *locatorPtr = cs.getConstraintLocator(locator);
  assert(solution.argumentMatchingChoices.count(locatorPtr) == 1);
  auto parameterBindings = solution.argumentMatchingChoices.find(locatorPtr)
                               ->second.parameterBindings;
  bool shouldSubstituteBindings = shouldSubstituteParameterBindings(callee);

  SmallVector<ParamBinding, 4> substitutedBindings;
  if (shouldSubstituteBindings) {
    computeParameterBindingsSubstitutions(callee, params, parameterBindings,
                                          substitutedBindings);
  } else {
    substitutedBindings = parameterBindings;
  }

  SmallVector<Argument, 4> newArgs;
  for (unsigned paramIdx = 0, numParams = substitutedBindings.size();
       paramIdx != numParams; ++paramIdx) {
    // Extract the parameter.
    const auto &param = params[paramIdx];
    auto paramLabel = param.getLabel();

    // Handle variadic parameters.
    if (param.isVariadic()) {
      assert(!param.isInOut());

      SmallVector<Expr *, 4> variadicArgs;

      // The first argument of this vararg parameter may have had a label;
      // save its location.
      auto &varargIndices = substitutedBindings[paramIdx];
      SourceLoc labelLoc;
      if (!varargIndices.empty())
        labelLoc = args->getLabelLoc(varargIndices[0]);

      // Convert the arguments.
      for (auto argIdx : varargIndices) {
        auto *arg = args->getExpr(argIdx);
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
      auto *varargExpansionExpr =
          VarargExpansionExpr::createArrayExpansion(ctx, arrayExpr);
      cs.cacheType(varargExpansionExpr);

      newArgs.push_back(Argument(labelLoc, paramLabel, varargExpansionExpr));
      continue;
    }

    // Handle default arguments.
    if (substitutedBindings[paramIdx].empty()) {
      auto paramIdxForDefault = paramIdx;
      // If bindings were substituted we need to find "original"
      // (or contextless) parameter index for the default argument.
      if (shouldSubstituteBindings) {
        auto *paramList = getParameterList(callee.getDecl());
        assert(paramList);
        paramIdxForDefault =
            paramList->getOrigParamIndex(callee.getSubstitutions(), paramIdx);
      }

      auto owner = getDefaultArgOwner(callee, paramIdx);
      auto paramTy = param.getParameterType();
      auto *defArg = new (ctx) DefaultArgumentExpr(
          owner, paramIdxForDefault, args->getStartLoc(), paramTy, dc);

      cs.cacheType(defArg);
      newArgs.emplace_back(SourceLoc(), param.getLabel(), defArg);
      continue;
    }

    // Otherwise, we have a plain old ordinary argument.

    // Extract the argument used to initialize this parameter.
    assert(substitutedBindings[paramIdx].size() == 1);
    unsigned argIdx = substitutedBindings[paramIdx].front();
    auto arg = args->get(argIdx);
    auto *argExpr = arg.getExpr();
    auto argType = cs.getType(argExpr);

    // Update the argument label to match the parameter. This may be necessary
    // for things like trailing closures and args to property wrapper params.
    arg.setLabel(param.getLabel());

    // Determine whether the closure argument should be treated as having
    // implicit self capture or inheriting actor context.
    bool isImplicitSelfCapture = paramInfo.isImplicitSelfCapture(paramIdx);
    bool inheritsActorContext = paramInfo.inheritsActorContext(paramIdx);
    applyContextualClosureFlags(
        argExpr, isImplicitSelfCapture, inheritsActorContext);

    // If the types exactly match, this is easy.
    auto paramType = param.getOldType();
    if (argType->isEqual(paramType) && !shouldInjectWrappedValuePlaceholder) {
      newArgs.push_back(arg);
      continue;
    }

    Expr *convertedArg = nullptr;
    auto argRequiresAutoClosureExpr = [&](const AnyFunctionType::Param &param,
                                          Type argType) {
      if (!param.isAutoClosure())
        return false;

      // Since it was allowed to pass function types to @autoclosure
      // parameters in Swift versions < 5, it has to be handled as
      // a regular function conversion by `coerceToType`.
      if (isAutoClosureArgument(argExpr)) {
        // In Swift >= 5 mode we only allow `@autoclosure` arguments
        // to be used by value if parameter would return a function
        // type (it just needs to get wrapped into autoclosure expr),
        // otherwise argument must always form a call.
        return cs.getASTContext().isSwiftVersionAtLeast(5);
      }

      return true;
    };

    if (paramInfo.hasExternalPropertyWrapper(paramIdx)) {
      auto *paramDecl = getParameterAt(callee, paramIdx);
      assert(paramDecl);

      auto appliedWrapper = appliedPropertyWrappers[appliedWrapperIndex++];
      auto wrapperType = solution.simplifyType(appliedWrapper.wrapperType);
      auto initKind = appliedWrapper.initKind;

      AppliedPropertyWrapperExpr::ValueKind valueKind;
      PropertyWrapperValuePlaceholderExpr *generatorArg;
      auto initInfo = paramDecl->getPropertyWrapperInitializerInfo();
      if (initKind == PropertyWrapperInitKind::ProjectedValue) {
        valueKind = AppliedPropertyWrapperExpr::ValueKind::ProjectedValue;
        generatorArg = initInfo.getProjectedValuePlaceholder();
      } else {
        valueKind = AppliedPropertyWrapperExpr::ValueKind::WrappedValue;
        generatorArg = initInfo.getWrappedValuePlaceholder();
      }

      // Coerce the property wrapper argument type to the input type of
      // the property wrapper generator function. The wrapper generator
      // has the same generic signature as the enclosing function, so we
      // can use substitutions from the callee.
      Type generatorInputType =
          generatorArg->getType().subst(callee.getSubstitutions());
      auto argLoc = getArgLocator(argIdx, paramIdx, param.getParameterFlags());

      if (generatorArg->isAutoClosure()) {
        auto *closureType = generatorInputType->castTo<FunctionType>();
        argExpr = coerceToType(
            argExpr, closureType->getResult(),
            argLoc.withPathElement(ConstraintLocator::AutoclosureResult));
        argExpr = cs.buildAutoClosureExpr(argExpr, closureType, dc);
      }

      argExpr = coerceToType(argExpr, generatorInputType, argLoc);

      // Wrap the argument in an applied property wrapper expr, which will
      // later turn into a call to the property wrapper generator function.
      argExpr = AppliedPropertyWrapperExpr::create(ctx, callee, paramDecl,
                                                   argExpr->getStartLoc(),
                                                   wrapperType, argExpr,
                                                   valueKind);
      cs.cacheExprTypes(argExpr);
    }

    auto argLoc = getArgLocator(argIdx, paramIdx, param.getParameterFlags());

    // If the argument is an existential type that has been opened, perform
    // the open operation.
    if (argType->getWithoutSpecifierType()->isAnyExistentialType() &&
        paramType->hasOpenedExistential()) {
      // FIXME: Look for an opened existential and use it. We need to
      // know how far out we need to go to close the existentials. Huh.
      auto knownOpened = solution.OpenedExistentialTypes.find(
          cs.getConstraintLocator(argLoc));
      if (knownOpened != solution.OpenedExistentialTypes.end()) {
        argExpr = openExistentialReference(
            argExpr, knownOpened->second, callee.getDecl());
        argType = cs.getType(argExpr);
      }
    }

    if (argRequiresAutoClosureExpr(param, argType)) {
      assert(!param.isInOut());

      // If parameter is an autoclosure, we need to make sure that:
      //   - argument type is coerced to parameter result type
      //   - implicit autoclosure is created to wrap argument expression
      //   - new types are propagated to constraint system
      auto *closureType = param.getPlainType()->castTo<FunctionType>();

      argExpr = coerceToType(
          argExpr, closureType->getResult(),
          argLoc.withPathElement(ConstraintLocator::AutoclosureResult));

      if (shouldInjectWrappedValuePlaceholder) {
        // If init(wrappedValue:) takes an autoclosure, then we want
        // the effect of autoclosure forwarding of the placeholder
        // autoclosure. The only way to do this is to call the placeholder
        // autoclosure when passing it to the init.
        bool isDefaultWrappedValue =
            target->propertyWrapperHasInitialWrappedValue();
        auto *placeholder = injectWrappedValuePlaceholder(
            cs.buildAutoClosureExpr(argExpr, closureType, dc,
                                    isDefaultWrappedValue),
            /*isAutoClosure=*/true);
        argExpr = CallExpr::createImplicitEmpty(ctx, placeholder);
        argExpr->setType(closureType->getResult());
        cs.cacheType(argExpr);
      }

      convertedArg = cs.buildAutoClosureExpr(argExpr, closureType, dc);
    } else {
      convertedArg = coerceToType(argExpr, paramType, argLoc);
    }

    // Perform the wrapped value placeholder injection
    if (shouldInjectWrappedValuePlaceholder)
      convertedArg = injectWrappedValuePlaceholder(convertedArg);

    if (!convertedArg)
      return nullptr;

    // Write back the rewritten argument to the original argument list. This
    // ensures it has the same semantic argument information as the rewritten
    // argument list, which may be required by IDE logic.
    args->setExpr(argIdx, convertedArg);

    arg.setExpr(convertedArg);
    newArgs.push_back(arg);
  }
  return ArgumentList::createTypeChecked(ctx, args, newArgs);
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
    if (!applyTypeToClosureExpr(cs, IE->getSubExpr(), toType))
      return false;

    auto subExprTy = cs.getType(IE->getSubExpr());
    if (isa<ParenExpr>(IE)) {
      cs.setType(IE, ParenType::get(cs.getASTContext(), subExprTy));
    } else {
      cs.setType(IE, subExprTy);
    }
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

    // If solution application for this closure is delayed, let's write the
    // type into the ClosureExpr directly here, since the visitor won't.
    if (!CE->hasSingleExpressionBody())
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
  if (bridged && TypeChecker::typeCheckCheckedCast(
                     srcType, destType, CheckedCastContextKind::None,
                     rewriter.dc) != CheckedCastKind::Coercion) {
    if (auto conversion =
          rewriter.buildObjCBridgeExpr(element, destType, locator))
        return conversion;
  }

  return rewriter.coerceToType(element, destType, locator);
}

static CollectionUpcastConversionExpr::ConversionPair
buildOpaqueElementConversion(ExprRewriter &rewriter, SourceRange srcRange,
                             Type srcType, Type destType,
                             bool bridged, ConstraintLocatorBuilder locator,
                             unsigned typeArgIndex) {
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
    if (Type elementType = toType->isArrayType()) {
      peepholeArrayUpcast(arrayLiteral, toType, bridged, elementType, locator);
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
  auto fromArgs = cs.getType(expr)->castTo<BoundGenericType>()->getGenericArgs();
  auto toArgs = toType->castTo<BoundGenericType>()->getGenericArgs();
  auto conv =
    buildOpaqueElementConversion(*this, expr->getLoc(),
                                 fromArgs[0], toArgs[0],
                                 bridged, locator, 0);

  // For single-parameter collections, form the upcast.
  if (toType->isArrayType() || ConstraintSystem::isSetType(toType)) {
    return cs.cacheType(
              new (ctx) CollectionUpcastConversionExpr(expr, toType, {}, conv));
  }

  assert(ConstraintSystem::isDictionaryType(toType) &&
         "Unhandled collection upcast");

  // Build the second value conversion.
  auto conv2 =
    buildOpaqueElementConversion(*this, expr->getLoc(),
                                 fromArgs[1], toArgs[1],
                                 bridged, locator, 1);

  return cs.cacheType(
           new (ctx) CollectionUpcastConversionExpr(expr, toType, conv, conv2));

}

Expr *ExprRewriter::buildObjCBridgeExpr(Expr *expr, Type toType,
                                        ConstraintLocatorBuilder locator) {
  Type fromType = cs.getType(expr);

  // Bridged collection casts always succeed, so we treat them as
  // collection "upcasts".
  if ((fromType->isArrayType() && toType->isArrayType())
      || (ConstraintSystem::isDictionaryType(fromType)
          && ConstraintSystem::isDictionaryType(toType))
      || (ConstraintSystem::isSetType(fromType)
          && ConstraintSystem::isSetType(toType))) {
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

Expr *ExprRewriter::coerceExistential(Expr *expr, Type toType,
                                      ConstraintLocatorBuilder locator) {
  Type fromType = cs.getType(expr);
  Type fromInstanceType = fromType;
  Type toInstanceType = toType;

  // Look through metatypes
  while ((fromInstanceType->is<UnresolvedType>() ||
          fromInstanceType->is<AnyMetatypeType>()) &&
         toInstanceType->is<ExistentialMetatypeType>()) {
    if (!fromInstanceType->is<UnresolvedType>())
      fromInstanceType = fromInstanceType->castTo<AnyMetatypeType>()->getInstanceType();
    toInstanceType = toInstanceType->castTo<ExistentialMetatypeType>()->getExistentialInstanceType();
  }

  ASTContext &ctx = cs.getASTContext();

  auto conformances =
    collectExistentialConformances(fromInstanceType, toInstanceType,
                                   dc->getParentModule());

  // Use the requirements of any parameterized protocols to build out fake
  // argument conversions that can be used to infer opaque types.
  SmallVector<CollectionUpcastConversionExpr::ConversionPair, 4> argConversions;

  auto fromConstraintType = fromInstanceType;
  if (auto existential = fromConstraintType->getAs<ExistentialType>())
    fromConstraintType = existential->getConstraintType();

  auto toConstraintType = toInstanceType;
  if (auto existential = toConstraintType->getAs<ExistentialType>())
    toConstraintType = existential->getConstraintType();

  auto fromPPT = fromConstraintType->getAs<ParameterizedProtocolType>();
  auto toPPT = toConstraintType->getAs<ParameterizedProtocolType>();

  if (fromPPT && toPPT) {
    assert(fromPPT->getArgs().size() >= toPPT->getArgs().size());
    for (unsigned i = 0; i < toPPT->getArgs().size(); ++i) {
      auto firstTy = fromPPT->getArgs()[i];
      auto secondTy = toPPT->getArgs()[i];
      auto conv =
        buildOpaqueElementConversion(*this, expr->getLoc(),
                                     firstTy,
                                     secondTy,
                                     /*bridged*/ false,
                                     locator, i);
      argConversions.push_back(conv);
    }
  } else if ((fromPPT || toPPT) &&
             !fromInstanceType->isExistentialType()) {
    auto parameterized = fromConstraintType;
    auto base = toConstraintType;
    if (toPPT)
      std::swap(parameterized, base);

    SmallVector<Requirement, 4> reqs;
    parameterized->castTo<ParameterizedProtocolType>()
                 ->getRequirements(base, reqs);

    for (unsigned i = 0; i < reqs.size(); ++i) {
      const auto &req = reqs[i];
      assert(req.getKind() == RequirementKind::SameType);
      auto conv =
        buildOpaqueElementConversion(*this, expr->getLoc(),
                                     req.getFirstType(),
                                     req.getSecondType(),
                                     /*bridged*/ false,
                                     locator, i);
      argConversions.push_back(conv);
    }
  }

  // For existential-to-existential coercions, open the source existential.
  if (fromType->isAnyExistentialType()) {
    fromType = OpenedArchetypeType::getAny(fromType->getCanonicalType(),
                                           dc->getGenericSignatureOfContext());

    auto *archetypeVal = cs.cacheType(
        new (ctx) OpaqueValueExpr(expr->getSourceRange(), fromType));

    auto *result = cs.cacheType(ErasureExpr::create(ctx, archetypeVal, toType,
                                                    conformances,
                                                    argConversions));
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

  return cs.cacheType(ErasureExpr::create(ctx, expr, toType,
                                          conformances, argConversions));
}

Expr *ConstraintSystem::addImplicitLoadExpr(Expr *expr) {
  return TypeChecker::addImplicitLoadExpr(
      getASTContext(), expr, [this](Expr *expr) { return getType(expr); },
      [this](Expr *expr, Type type) { setType(expr, type); });
}

Expr *ExprRewriter::coerceToType(Expr *expr, Type toType,
                                 ConstraintLocatorBuilder locator) {
  auto &ctx = cs.getASTContext();

  // Diagnose conversions to invalid function types that couldn't be performed
  // beforehand because of placeholders.
  if (auto *fnTy = toType->getAs<FunctionType>()) {
    auto contextTy = cs.getContextualType(expr, /*forConstraint=*/false);
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
      // See also: https://github.com/apple/swift/issues/49345
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
      return coerceExistential(expr, toType, locator);

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
      return coerceOptionalToOptional(expr, toType, locator);

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
                        cs.getType(expr), hashable, dc->getParentModule());
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

    case ConversionRestrictionKind::InoutToPointer:
    case ConversionRestrictionKind::InoutToCPointer: {
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

    case ConversionRestrictionKind::ArrayToPointer:
    case ConversionRestrictionKind::ArrayToCPointer: {
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
    
    case ConversionRestrictionKind::PointerToPointer:
    case ConversionRestrictionKind::PointerToCPointer: {
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

      auto shouldUseCoercedExpr = [&]() {
        // If conversion wraps the whole body of a single-expression closure,
        // let's use the passed-in expression since the closure itself doesn't
        // get updated until coercion is done.
        if (locator.endsWith<LocatorPathElt::ClosureBody>())
          return true;

        // Contextual type locator always uses the original version of
        // expression (before any coercions have been applied) because
        // otherwise it wouldn't be possible to find the overload choice.
        if (locator.endsWith<LocatorPathElt::ContextualType>())
          return true;

        // In all other cases use the expression associated with locator.
        return false;
      };

      auto *argExpr =
          shouldUseCoercedExpr() ? expr : locator.trySimplifyToExpr();
      assert(argExpr);

      // Source requires implicit conversion to match destination
      // type but the conversion itself is recorded on assignment.
      if (auto *assignment = dyn_cast<AssignExpr>(argExpr))
        argExpr = assignment->getSrc();

      // Load the value for conversion.
      argExpr = cs.coerceToRValue(argExpr);

      auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {argExpr});
      auto *implicitInit = CallExpr::createImplicit(
          ctx, TypeExpr::createImplicit(toType, ctx), argList);

      cs.cacheExprTypes(implicitInit->getFn());
      cs.setType(argExpr, fromType);

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

        ConstraintLocator *baseLoc =
            cs.getImplicitValueConversionLocator(locator, conversionKind);

        auto overload =
            solution.getOverloadChoice(solution.getConstraintLocator(
                baseLoc, {ConstraintLocator::ApplyFunction,
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

  // Use an opaque type to abstract a value of the underlying concrete type.
  // The full check here would be that `toType` and `fromType` are structurally
  // equal except in any position where `toType` has an opaque archetype. The
  // below is just an approximate check since the above would be expensive to
  // verify and still relies on the type checker ensuing `fromType` is
  // compatible with any opaque archetypes.
  if (toType->getCanonicalType()->hasOpaqueArchetype() &&
      cs.getConstraintLocator(locator)->isForContextualType()) {
    // Find the opaque type declaration. We need its generic signature.
    OpaqueTypeDecl *opaqueDecl = nullptr;
    bool found = toType->getCanonicalType().findIf([&](Type type) {
      if (auto opaqueType = type->getAs<OpaqueTypeArchetypeType>()) {
        opaqueDecl = opaqueType->getDecl();
        return true;
      }

      return false;
    });
    (void)found;
    assert(found && "No opaque type archetype?");

    // Compute the substitutions for the opaque type declaration.
    auto opaqueLocator = solution.getConstraintSystem().getOpenOpaqueLocator(
        locator, opaqueDecl);
    SubstitutionMap substitutions = solution.computeSubstitutions(
        opaqueDecl->getOpaqueInterfaceGenericSignature(), opaqueLocator);

    // If we don't have substitutions, this is an opaque archetype from
    // another declaration being manipulated, and not an erasure of a
    // concrete type to an opaque type inside its defining declaration.
    if (!substitutions.empty()) {
      // Compute the underlying type by replacing all opaque archetypes with
      // the fixed type of their opened type.
      auto underlyingType = toType.subst(
        [&](SubstitutableType *type) -> Type {
          if (auto *opaqueType = type->getAs<OpaqueTypeArchetypeType>()) {
            if (opaqueType->getDecl() == opaqueDecl) {
              return opaqueType->getInterfaceType().subst(substitutions);
            }
          }
          return type;
        },
        LookUpConformanceInModule(cs.DC->getParentModule()),
        SubstFlags::SubstituteOpaqueArchetypes);

      // Coerce the result expression to the underlying type.
      // FIXME: Wrong locator?
      auto *subExpr = coerceToType(expr, underlyingType, locator);

      return cs.cacheType(
          new (ctx) UnderlyingToOpaqueExpr(subExpr, toType, substitutions));
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

  case TypeKind::Pack: {
    llvm_unreachable("Unimplemented!");
  }

  case TypeKind::PackExpansion: {
    auto toExpansionType = toType->getAs<PackExpansionType>();
    auto *expansion = dyn_cast<PackExpansionExpr>(expr);

    auto *elementEnv = expansion->getGenericEnvironment();
    auto toElementType = elementEnv->mapContextualPackTypeIntoElementContext(
        toExpansionType->getPatternType());

    auto *pattern = coerceToType(expansion->getPatternExpr(),
                                 toElementType, locator);
    auto *packEnv = cs.DC->getGenericEnvironmentOfContext();
    auto patternType = packEnv->mapElementTypeIntoPackContext(toElementType);
    auto shapeType = toExpansionType->getCountType();
    auto expansionTy = PackExpansionType::get(patternType, shapeType);

    expansion->setPatternExpr(pattern);
    expansion->setType(expansionTy);
    return cs.cacheType(expansion);
  }

  case TypeKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleType should not show up here");

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
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype:
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
    if (auto initializerCtx = dyn_cast<Initializer>(dc))
      isInDefaultArgumentContext = (initializerCtx->getInitializerKind() ==
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
      if (!locator.endsWith<LocatorPathElt::ApplyArgToParam>())
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
        // Apply an explicit function conversion *only* for the escape to
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
  case TypeKind::ParameterizedProtocol:
  case TypeKind::Existential:
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
  case TypeKind::Existential:
  case TypeKind::ExistentialMetatype:
  case TypeKind::ProtocolComposition:
  case TypeKind::ParameterizedProtocol:
  case TypeKind::Protocol:
    return coerceExistential(expr, toType, locator);

  // Coercion to Optional<T>.
  case TypeKind::BoundGenericEnum: {
    auto toGenericType = cast<BoundGenericEnumType>(desugaredToType);
    if (!toGenericType->getDecl()->isOptionalDecl())
      break;
    TypeChecker::requireOptionalIntrinsics(ctx, expr->getLoc());

    if (cs.getType(expr)->getOptionalObjectType())
      return coerceOptionalToOptional(expr, toType, locator);

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
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
  case TypeKind::Function:
  case TypeKind::GenericFunction:
  case TypeKind::LValue:
  case TypeKind::InOut:
  case TypeKind::Pack:
  case TypeKind::PackExpansion:
    break;

  case TypeKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleType should not show up here");
  }

  // Unresolved types come up in diagnostics for lvalue and inout types.
  if (fromType->hasUnresolvedType() || toType->hasUnresolvedType())
    return cs.cacheType(new (ctx) UnresolvedTypeConversionExpr(expr, toType));

  llvm::errs() << "Unhandled coercion:\n";
  fromType->dump(llvm::errs());
  toType->dump(llvm::errs());
  abort();
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
        type, builtinProtocol, dc->getParentModule());
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
                                                     dc->getParentModule());
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
  auto *args = apply->getArgs();

  // HACK: Temporarily push the fn expr onto the expr stack to make sure we
  // don't try to prematurely close an existential when applying the curried
  // member ref. This can be removed once existential opening is refactored not
  // to rely on the shape of the AST prior to rewriting.
  rewriter.ExprStack.push_back(fn);
  SWIFT_DEFER {
    rewriter.ExprStack.pop_back();
  };

  auto *declRef = rewriter.buildMemberRef(
      fn, /*dotLoc*/ SourceLoc(), selected, DeclNameLoc(args->getStartLoc()),
      calleeLoc, calleeLoc, /*implicit*/ true, AccessSemantics::Ordinary);
  if (!declRef)
    return nullptr;
  declRef->setImplicit(apply->isImplicit());
  return declRef;
}

// Resolve `@dynamicCallable` applications.
std::pair<Expr *, ArgumentList *> ExprRewriter::buildDynamicCallable(
    ApplyExpr *apply, SelectedOverload selected, FuncDecl *method,
    AnyFunctionType *methodType, ConstraintLocatorBuilder loc) {
  auto &ctx = cs.getASTContext();
  auto *fn = apply->getFn();

  auto *args = apply->getArgs();

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
  Expr *argExpr = nullptr;
  if (!useKwargsMethod) {
    argExpr = ArrayExpr::create(ctx, SourceLoc(), args->getArgExprs(), {},
                                SourceLoc());
    cs.setType(argExpr, argumentType);
    finishArrayExpr(cast<ArrayExpr>(argExpr));
  } else {
    auto dictLitProto =
        ctx.getProtocol(KnownProtocolKind::ExpressibleByDictionaryLiteral);
    auto conformance =
        TypeChecker::conformsToProtocol(argumentType, dictLitProto,
                                        dc->getParentModule());
    auto keyType = conformance.getTypeWitnessByName(argumentType, ctx.Id_Key);
    auto valueType =
        conformance.getTypeWitnessByName(argumentType, ctx.Id_Value);
    SmallVector<Identifier, 4> names;
    SmallVector<Expr *, 4> dictElements;
    for (auto arg : *args) {
      Expr *labelExpr =
        new (ctx) StringLiteralExpr(arg.getLabel().get(), arg.getLabelLoc(),
                                    /*Implicit*/ true);
      cs.setType(labelExpr, keyType);
      handleStringLiteralExpr(cast<LiteralExpr>(labelExpr));

      Expr *valueExpr = coerceToType(arg.getExpr(), valueType, loc);
      assert(valueExpr && "Failed to coerce?");
      Expr *pair = TupleExpr::createImplicit(ctx, {labelExpr, valueExpr}, {});
      auto eltTypes = { TupleTypeElt(keyType), TupleTypeElt(valueType) };
      cs.setType(pair, TupleType::get(eltTypes, ctx));
      dictElements.push_back(pair);
    }
    argExpr = DictionaryExpr::create(ctx, SourceLoc(), dictElements, {},
                                     SourceLoc());
    cs.setType(argExpr, argumentType);
    finishDictionaryExpr(cast<DictionaryExpr>(argExpr));
  }
  argExpr->setImplicit();

  auto *argList = ArgumentList::forImplicitSingle(ctx, argumentLabel, argExpr);
  return std::make_pair(member, argList);
}

Expr *ExprRewriter::finishApply(ApplyExpr *apply, Type openedType,
                                ConstraintLocatorBuilder locator,
                                ConstraintLocatorBuilder calleeLocator) {
  auto &ctx = cs.getASTContext();

  auto args = apply->getArgs();
  auto *fn = apply->getFn();

  auto finishApplyOfDeclWithSpecialTypeCheckingSemantics
    = [&](ApplyExpr *apply,
          ConcreteDeclRef declRef,
          Type openedType) -> Expr* {
      switch (TypeChecker::getDeclTypeCheckingSemantics(declRef.getDecl())) {
      case DeclTypeCheckingSemantics::TypeOf: {
        // Resolve into a DynamicTypeExpr.
        auto args = apply->getArgs();

        auto &appliedWrappers = solution.appliedPropertyWrappers[calleeLocator.getAnchor()];
        auto fnType = cs.getType(fn)->getAs<FunctionType>();
        args = coerceCallArguments(
            args, fnType, declRef, apply,
            locator.withPathElement(ConstraintLocator::ApplyArgument),
            appliedWrappers);
        if (!args)
          return nullptr;

        auto replacement = new (ctx)
            DynamicTypeExpr(apply->getFn()->getLoc(), args->getStartLoc(),
                            args->getExpr(0), args->getEndLoc(), Type());
        cs.setType(replacement, simplifyType(openedType));
        return replacement;
      }
      
      case DeclTypeCheckingSemantics::WithoutActuallyEscaping: {
        // Resolve into a MakeTemporarilyEscapableExpr.
        auto *args = apply->getArgs();
        assert(args->size() == 2 && "should have two arguments");
        auto *nonescaping = args->getExpr(0);
        auto *body = args->getExpr(1);
        auto bodyTy = cs.getType(body)->getWithoutSpecifierType();
        auto bodyFnTy = bodyTy->castTo<FunctionType>();
        auto resultType = bodyFnTy->getResult();
        
        // The body is immediately called, so is obviously noescape.
        // Coerce the argument function to be escaping even if it happens to
        // be nonescaping, since we need the dynamic state of the escaping
        // closure to do the dynamic noescape check.
        auto bodyArgFnTy = bodyFnTy->getParams()[0].getPlainType()
          ->castTo<FunctionType>();
          
        bodyArgFnTy = cast<FunctionType>(
          bodyArgFnTy->withExtInfo(bodyArgFnTy->getExtInfo().withNoEscape(false)));
        bodyFnTy = cast<FunctionType>(
          FunctionType::get(bodyFnTy->getParams()[0].withType(bodyArgFnTy),
                            bodyFnTy->getResult())
            ->withExtInfo(bodyFnTy->getExtInfo().withNoEscape()));
        body = coerceToType(body, bodyFnTy, locator);
        assert(body && "can't make nonescaping?!");

        auto escapable = new (ctx)
            OpaqueValueExpr(apply->getFn()->getSourceRange(), Type());
        cs.setType(escapable, bodyArgFnTy);

        auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {escapable});
        auto callSubExpr = CallExpr::createImplicit(ctx, body, argList);
        cs.cacheSubExprTypes(callSubExpr);
        cs.setType(callSubExpr, resultType);
        
        auto replacement = new (ctx)
          MakeTemporarilyEscapableExpr(apply->getFn()->getLoc(),
                                       apply->getArgs()->getStartLoc(),
                                       nonescaping,
                                       callSubExpr,
                                       apply->getArgs()->getEndLoc(),
                                       escapable,
                                       apply);
        cs.setType(replacement, resultType);
        return replacement;
      }
      
      case DeclTypeCheckingSemantics::OpenExistential: {
        // Resolve into an OpenExistentialExpr.
        auto *args = apply->getArgs();
        assert(args->size() == 2 && "should have two arguments");

        auto *existential = cs.coerceToRValue(args->getExpr(0));
        auto *body = cs.coerceToRValue(args->getExpr(1));

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
            ->getExistentialInstanceType();
        }
        assert(openedInstanceTy->castTo<OpenedArchetypeType>()
                   ->getExistentialType()
                   ->isEqual(existentialInstanceTy));

        auto opaqueValue =
            new (ctx) OpaqueValueExpr(apply->getSourceRange(), openedTy);
        cs.setType(opaqueValue, openedTy);

        auto *argList = ArgumentList::forImplicitUnlabeled(ctx, {opaqueValue});
        auto callSubExpr = CallExpr::createImplicit(ctx, body, argList);
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

  // Make sure we have a function type that is callable. This helps ensure
  // Type::mayBeCallable stays up-to-date.
  auto fnRValueTy = cs.getType(fn)->getRValueType();
  assert(fnRValueTy->mayBeCallable(dc));

  // If this is an implicit call to a `callAsFunction` method, build the
  // appropriate member reference.
  if (fnRValueTy->isCallAsFunctionType(dc)) {
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
        simplifyType(selected->adjustedOpenedType)->getAs<AnyFunctionType>();
    if (method && methodType) {
      // Handle a call to a @dynamicCallable method.
      if (isValidDynamicCallableMethod(method, methodType))
        std::tie(fn, args) = buildDynamicCallable(apply, *selected, method,
                                                  methodType, applyFunctionLoc);
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
  if (auto fnType = cs.getType(fn)->getAs<FunctionType>()) {
    auto &appliedWrappers = solution.appliedPropertyWrappers[calleeLocator.getAnchor()];
    args = coerceCallArguments(
        args, fnType, callee, apply,
        locator.withPathElement(ConstraintLocator::ApplyArgument),
        appliedWrappers);
    if (!args)
      return nullptr;

    apply->setArgs(args);
    cs.setType(apply, fnType->getResult());

    // If this is a call to a distributed method thunk,
    // let's mark the call as implicitly throwing.
    if (isDistributedThunk(callee, apply->getFn())) {
      auto *FD = cast<AbstractFunctionDecl>(callee.getDecl());
      if (!FD->hasThrows())
        apply->setImplicitlyThrows(true);
    }

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

    // Try closing existentials, if there are any.
    closeExistentials(result, locator);

    // We may also need to force the result for an IUO. We don't apply this on
    // SelfApplyExprs, as the force unwraps should be inserted at the result of
    // main application, not on the curried member reference.
    if (!isa<SelfApplyExpr>(apply)) {
      result = forceUnwrapIfExpected(result, calleeLocator,
                                     IUOReferenceKind::ReturnValue);
    }
    return result;
  }

  // FIXME: Handle unwrapping everywhere else.

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
    auto *packed = apply->getArgs()->packIntoImplicitTupleOrParen(
        ctx, [&](Expr *E) { return cs.getType(E); });
    cs.cacheType(packed);
    auto *result = coerceToType(packed, tupleTy, cs.getConstraintLocator(packed));
    // Resetting the types of tuple is necessary because
    // `packIntoImplicitTupleOrParen` sets types in AST
    // where `coerceToType` only updates constraint system
    // cache. This creates a mismatch that would not be
    // corrected by `setExprTypes` if this tuple is used
    // as an argument that is wrapped in an autoclosure.
    solution.setExprTypes(result);
    return result;
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
  auto *ctorCall = finishApply(apply, openedType, locator, ctorLocator);

  // Check whether this is a situation like `T(...) { ... }` where `T` is
  // a callable type and trailing closure(s) are associated with implicit
  // `.callAsFunction` instead of constructor.
  {
    auto callAsFunction =
        solution.ImplicitCallAsFunctionRoots.find(ctorLocator);
    if (callAsFunction != solution.ImplicitCallAsFunctionRoots.end()) {
      auto *dotExpr = callAsFunction->second;
      auto resultTy = solution.getResolvedType(dotExpr);

      auto *implicitCall = CallExpr::createImplicit(
          cs.getASTContext(), ctorCall,
          solution.getArgumentList(cs.getConstraintLocator(
              dotExpr, ConstraintLocator::ApplyArgument)));

      implicitCall->setType(resultTy);
      cs.cacheType(implicitCall);

      auto *memberCalleeLoc =
          cs.getConstraintLocator(dotExpr,
                                  {ConstraintLocator::ApplyFunction,
                                   ConstraintLocator::ImplicitCallAsFunction},
                                  /*summaryFlags=*/0);

      return finishApply(implicitCall, resultTy, cs.getConstraintLocator(dotExpr),
                         memberCalleeLoc);
    }
  }

  return ctorCall;
}

bool ExprRewriter::isDistributedThunk(ConcreteDeclRef ref, Expr *context) {
  auto *FD = dyn_cast_or_null<AbstractFunctionDecl>(ref.getDecl());
  if (!(FD && FD->isInstanceMember() && FD->isDistributed()))
    return false;

  if (!isa<SelfApplyExpr>(context))
    return false;

  auto *actor = getReferencedParamOrCapture(
      cast<SelfApplyExpr>(context)->getBase(),
      [&](OpaqueValueExpr *opaqueValue) -> Expr * {
        for (const auto &existential : OpenedExistentials) {
          if (existential.OpaqueValue == opaqueValue)
            return existential.ExistentialValue;
        }
        return nullptr;
      });

  if (!actor)
    return false;

  // If this is a method reference on an potentially isolated
  // actor then it cannot be a remote thunk.
  bool isPotentiallyIsolated = isPotentiallyIsolatedActor(
      actor,
    [&](ParamDecl *P) {
    return P->isIsolated() ||
           llvm::is_contained(solution.isolatedParams, P);
  });

  // Adjust the declaration context to the innermost context that is neither
  // a local function nor a closure, so that the actor reference is checked
  auto referenceDC = dc;
  while (true) {
    switch (referenceDC->getContextKind()) {
    case DeclContextKind::AbstractClosureExpr:
    case DeclContextKind::Initializer:
    case DeclContextKind::SerializedLocal:
      referenceDC = referenceDC->getParent();
      continue;

      case DeclContextKind::AbstractFunctionDecl:
      case DeclContextKind::GenericTypeDecl:
      case DeclContextKind::SubscriptDecl:
        if (auto value = dyn_cast<ValueDecl>(referenceDC->getAsDecl())) {
          if (value->isLocalCapture()) {
            referenceDC = referenceDC->getParent();
            continue;
          }
        }
        break;

      case DeclContextKind::EnumElementDecl:
      case DeclContextKind::ExtensionDecl:
      case DeclContextKind::FileUnit:
      case DeclContextKind::Package:
      case DeclContextKind::Module:
      case DeclContextKind::TopLevelCodeDecl:
      case DeclContextKind::MacroDecl:
        break;
    }

    break;
  }

  // Create a simple actor reference, assuming that we might be in a
  // non-isolated context but knowing whether it's potentially isolated.
  // We only care about the "distributed" flag.
  ReferencedActor actorRef = ReferencedActor(
      actor, isPotentiallyIsolated, ReferencedActor::NonIsolatedContext);
  auto refResult = ActorReferenceResult::forReference(
      ref, context->getLoc(), referenceDC, None, actorRef);
  switch (refResult) {
  case ActorReferenceResult::ExitsActorToNonisolated:
  case ActorReferenceResult::SameConcurrencyDomain:
    return false;

  case ActorReferenceResult::EntersActor:
    return refResult.options.contains(ActorReferenceResult::Flags::Distributed);
  }
}

// Return the precedence-yielding parent of 'expr', along with the index of
// 'expr' as the child of that parent. The precedence-yielding parent is the
// nearest ancestor of 'expr' which imposes a minimum precedence on 'expr'.
static std::pair<Expr *, unsigned> getPrecedenceParentAndIndex(
    Expr *expr, llvm::function_ref<Expr *(const Expr *)> getParent) {
  auto *parent = getParent(expr);
  if (!parent)
    return { nullptr, 0 };

  // Look through an unresolved chain wrappers, try, and await exprs, as they
  // have no effect on precedence; they will associate the same with any parent
  // operator as their sub-expression would.
  while (isa<UnresolvedMemberChainResultExpr>(parent) ||
         isa<AnyTryExpr>(parent) || isa<AwaitExpr>(parent)) {
    expr = parent;
    parent = getParent(parent);
    if (!parent)
      return { nullptr, 0 };
  }

  // Handle all cases where the answer isn't just going to be { parent, 0 }.
  if (auto tuple = dyn_cast<TupleExpr>(parent)) {
    // Get index of expression in tuple.
    auto tupleElems = tuple->getElements();
    auto elemIt = std::find(tupleElems.begin(), tupleElems.end(), expr);
    assert(elemIt != tupleElems.end() && "expr not found in parent TupleExpr");
    unsigned index = elemIt - tupleElems.begin();
    return { tuple, index };
  } else if (auto *BE = dyn_cast<BinaryExpr>(parent)) {
    if (BE->getLHS() == expr)
      return {parent, 0};
    if (BE->getRHS() == expr)
      return {parent, 1};
  } else if (auto *ternary = dyn_cast<TernaryExpr>(parent)) {
    unsigned index;
    if (expr == ternary->getCondExpr()) {
      index = 0;
    } else if (expr == ternary->getThenExpr()) {
      index = 1;
    } else if (expr == ternary->getElseExpr()) {
      index = 2;
    } else {
      llvm_unreachable("expr not found in parent TernaryExpr");
    }
    return { ternary, index };
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
    auto exprPG = TypeChecker::lookupPrecedenceGroupForInfixOperator(
        DC, expr, /*diagnose=*/false);
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
    DeclContext *DC, Expr *expr, PrecedenceGroupDecl *followingPG,
    llvm::function_ref<Expr *(const Expr *)> getParent) {
  Expr *parent;
  unsigned index;
  std::tie(parent, index) = getPrecedenceParentAndIndex(expr, getParent);
  if (!parent)
    return false;

  // If this is a call argument, no parens are needed.
  if (auto *args = parent->getArgs()) {
    if (!args->isImplicit() && args->findArgumentExpr(expr))
      return false;
  }

  // If this is a key-path, no parens needed if it's an arg of one of the
  // components.
  if (auto *KP = dyn_cast<KeyPathExpr>(parent)) {
    if (KP->findComponentWithSubscriptArg(expr))
      return false;
  }

  if (isa<ParenExpr>(parent) || isa<TupleExpr>(parent)) {
    if (!parent->isImplicit())
      return false;
  }

  if (isa<ClosureExpr>(parent) || isa<CollectionExpr>(parent))
    return false;

  if (parent->isInfixOperator()) {
    auto parentPG = TypeChecker::lookupPrecedenceGroupForInfixOperator(
        DC, parent, /*diagnose=*/false);
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

bool swift::exprNeedsParensAfterAddingNilCoalescing(
    DeclContext *DC, Expr *expr,
    llvm::function_ref<Expr *(const Expr *)> getParent) {
  auto &ctx = DC->getASTContext();
  auto asPG = TypeChecker::lookupPrecedenceGroup(
                  DC, ctx.Id_NilCoalescingPrecedence, SourceLoc())
                  .getSingle();
  if (!asPG)
    return true;
  return exprNeedsParensOutsideFollowingOperator(DC, expr, asPG, getParent);
}

namespace {
  class SetExprTypes : public ASTWalker {
    const Solution &solution;

  public:
    explicit SetExprTypes(const Solution &solution)
        : solution(solution) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      auto &cs = solution.getConstraintSystem();
      auto exprType = cs.getType(expr);
      exprType = solution.simplifyType(exprType);
      // assert((!expr->getType() || expr->getType()->isEqual(exprType)) &&
      //       "Mismatched types!");
      assert(!exprType->getRecursiveProperties().isSolverAllocated() &&
             "Should not write solver allocated type into expression!");
      assert(!exprType->hasPlaceholder() &&
             "Should not write type placeholders into expression!");
      expr->setType(exprType);

      if (auto kp = dyn_cast<KeyPathExpr>(expr)) {
        for (auto i : indices(kp->getComponents())) {
          Type componentType;
          if (cs.hasType(kp, i)) {
            componentType = solution.simplifyType(cs.getType(kp, i));
            assert(
                !componentType->getRecursiveProperties().isSolverAllocated() &&
                "Should not write solver allocated type into key-path "
                "component!");
            assert(!componentType->hasPlaceholder() &&
                   "Should not write type placeholder into key-path component");
            kp->getMutableComponents()[i].setComponentType(componentType);
          }
        }
      }

      return Action::Continue(expr);
    }

    /// Ignore statements.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      return Action::SkipChildren(stmt);
    }

    /// Ignore declarations.
    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }
  };

  class ExprWalker : public ASTWalker {
    ExprRewriter &Rewriter;
    SmallVector<ClosureExpr *, 4> ClosuresToTypeCheck;
    SmallVector<std::pair<TapExpr *, DeclContext *>, 4> TapsToTypeCheck;

  public:
    ExprWalker(ExprRewriter &Rewriter) : Rewriter(Rewriter) { }

    ~ExprWalker() {
      assert(ClosuresToTypeCheck.empty());
      assert(TapsToTypeCheck.empty());
    }

    bool shouldWalkIntoPropertyWrapperPlaceholderValue() override {
      // Property wrapper placeholder underlying values are filled in
      // with already-type-checked expressions. Don't walk into them.
      return false;
    }

    /// Check if there are any closures or tap expressions left to process separately.
    bool hasDelayedTasks() {
      return !ClosuresToTypeCheck.empty() || !TapsToTypeCheck.empty();
    }

    /// Process delayed closure bodies and `Tap` expressions.
    ///
    /// \returns true if any part of the processing fails.
    bool processDelayed() {
      bool hadError = false;
      auto &solution = Rewriter.solution;
      auto &cs = solution.getConstraintSystem();

      while (!ClosuresToTypeCheck.empty()) {
        auto *closure = ClosuresToTypeCheck.pop_back_val();
        // If experimental multi-statement closure support
        // is enabled, solution should have all of required
        // information.
        //
        // Note that in this mode `ClosuresToTypeCheck` acts
        // as a stack because multi-statement closures could
        // have other multi-statement closures in the body.
        if (cs.participatesInInference(closure)) {
          hadError |= cs.applySolutionToBody(
              solution, closure, Rewriter.dc,
              [&](SyntacticElementTarget target) {
                auto resultTarget = rewriteTarget(target);
                if (resultTarget) {
                  if (auto expr = resultTarget->getAsExpr())
                    solution.setExprTypes(expr);
                }

                return resultTarget;
              });

          if (!hadError) {
            TypeChecker::checkClosureAttributes(closure);
            TypeChecker::checkParameterList(closure->getParameters(), closure);
          }

          continue;
        }

        hadError |= TypeChecker::typeCheckClosureBody(closure);
      }

      // Tap expressions too; they should or should not be
      // type-checked under the same conditions as closure bodies.
      {
        for (const auto &tuple : TapsToTypeCheck) {
          auto tap = std::get<0>(tuple);
          auto tapDC = std::get<1>(tuple);
          hadError |= TypeChecker::typeCheckTapBody(tap, tapDC);
        }
        TapsToTypeCheck.clear();
      }

      return hadError;
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      // For closures, update the parameter types and check the body.
      if (auto closure = dyn_cast<ClosureExpr>(expr)) {
        rewriteFunction(closure);

        if (AnyFunctionRef(closure).hasExternalPropertyWrapperParameters()) {
          return Action::SkipChildren(rewriteClosure(closure));
        }

        return Action::SkipChildren(closure);
      }

      if (auto *SVE = dyn_cast<SingleValueStmtExpr>(expr)) {
        rewriteSingleValueStmtExpr(SVE);
        return Action::SkipChildren(SVE);
      }

      if (auto tap = dyn_cast<TapExpr>(expr)) {
        // We remember the DeclContext because the code to handle
        // single-expression-body closures above changes it.
        TapsToTypeCheck.push_back(std::make_pair(tap, Rewriter.dc));
      }

      if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
        // Rewrite captures.
        for (const auto &capture : captureList->getCaptureList()) {
          (void)rewriteTarget(SyntacticElementTarget(capture.PBD));
        }
      }

      Rewriter.walkToExprPre(expr);
      return Action::Continue(expr);
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      auto *result = Rewriter.walkToExprPost(expr);
      if (!result)
        return Action::Stop();

      return Action::Continue(result);
    }

    /// Ignore statements.
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *stmt) override {
      return Action::SkipChildren(stmt);
    }

    /// Ignore declarations.
    PreWalkAction walkToDeclPre(Decl *decl) override {
      return Action::SkipChildren();
    }

    /// Rewrite the target, producing a new target.
    Optional<SyntacticElementTarget>
    rewriteTarget(SyntacticElementTarget target);

    AutoClosureExpr *rewriteClosure(ClosureExpr *closure) {
      auto &solution = Rewriter.solution;

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
              solution.simplifyType(solution.getType(projectionVar))->mapTypeOutOfContext());
        }

        auto *wrappedValueVar = param->getPropertyWrapperWrappedValueVar();
        auto wrappedValueType =
            solution.simplifyType(solution.getType(wrappedValueVar))->mapTypeOutOfContext();
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

      return Rewriter.buildSingleCurryThunk(
          closure, closure, Rewriter.cs.getConstraintLocator(closure));
    }

    /// Rewrite the function for the given solution.
    ///
    /// \returns true if an error occurred.
    bool rewriteFunction(AnyFunctionRef fn) {
      auto result = Rewriter.cs.applySolution(
          Rewriter.solution, fn, Rewriter.dc,
          [&](SyntacticElementTarget target) {
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

    bool rewriteSingleValueStmtExpr(SingleValueStmtExpr *SVE) {
      auto &solution = Rewriter.solution;
      auto resultTy = solution.getResolvedType(SVE);
      Rewriter.cs.setType(SVE, resultTy);

      SmallVector<Expr *, 4> scratch;
      SmallPtrSet<Expr *, 4> exprBranches;
      for (auto *branch : SVE->getSingleExprBranches(scratch))
        exprBranches.insert(branch);

      return Rewriter.cs.applySolutionToSingleValueStmt(
          solution, SVE, solution.getDC(), [&](SyntacticElementTarget target) {
            // We need to fixup the conversion type to the full result type,
            // not the branch result type. This is necessary as there may be
            // an additional conversion required for the branch.
            if (auto *E = target.getAsExpr()) {
              if (exprBranches.contains(E))
                target.setExprConversionType(resultTy);
            }
            auto resultTarget = rewriteTarget(target);
            if (!resultTarget)
              return resultTarget;

            if (auto expr = resultTarget->getAsExpr())
              solution.setExprTypes(expr);

            return resultTarget;
          });
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
        if (!primaryFix->isFatal()) {
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
      initializer, closureType, dc, /*isDefaultWrappedValue=*/false,
      /*isAsyncLetWrapper=*/true);

  // Call the autoclosure so that the AST types line up. SILGen will ignore the
  // actual calls and translate them into a different mechanism.
  auto autoclosureCall = CallExpr::createImplicitEmpty(ctx, autoclosureExpr);
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
static Optional<SyntacticElementTarget>
applySolutionToInitialization(Solution &solution, SyntacticElementTarget target,
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
      target.getAsExpr(), LocatorPathElt::ContextualType(CTP_Initialization));
  initializer = solution.coerceToType(initializer, initType, locator);
  if (!initializer)
    return None;

  SyntacticElementTarget resultTarget = target;
  resultTarget.setExpr(initializer);

  // Record the property wrapper type and note that the initializer has
  // been subsumed by the backing property.
  if (wrappedVar) {
    ASTContext &ctx = cs.getASTContext();
    ctx.setSideCachedPropertyWrapperBackingPropertyType(
        wrappedVar, initType->mapTypeOutOfContext());

    // Record the semantic initializer on the outermost property wrapper.
    wrappedVar->getOutermostAttachedPropertyWrapper()->setSemanticInit(
        initializer);

    // If this is a wrapped parameter, we're done.
    if (isa<ParamDecl>(wrappedVar))
      return resultTarget;

    wrappedVar->getParentPatternBinding()->setInitializerSubsumed(0);
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
  if (target.isAsyncLetInitializer()) {
    resultTarget.setExpr(wrapAsyncLetInitializer(
        cs, resultTarget.getAsExpr(), resultTarget.getDeclContext()));
  }

  // If this property has an opaque result type, set the underlying type
  // substitutions based on the initializer.
  if (auto var = resultTarget.getInitializationPattern()->getSingleVar()) {
    SubstitutionMap substitutions;
    if (auto opaque = var->getOpaqueResultTypeDecl()) {
      resultTarget.getAsExpr()->forEachChildExpr([&](Expr *expr) -> Expr * {
        if (auto coercionExpr = dyn_cast<UnderlyingToOpaqueExpr>(expr)) {
          auto newSubstitutions =
              coercionExpr->substitutions.mapReplacementTypesOutOfContext();
          if (substitutions.empty()) {
            substitutions = newSubstitutions;
          } else {
            assert(substitutions.getCanonical() ==
                       newSubstitutions.getCanonical());
          }
        }
        return expr;
      });

      opaque->setUniqueUnderlyingTypeSubstitutions(substitutions);
    }
  }


  return resultTarget;
}

/// Apply the given solution to the for-each statement target.
///
/// \returns the resulting initialization expression.
static Optional<SyntacticElementTarget> applySolutionToForEachStmt(
    Solution &solution, SyntacticElementTarget target,
    llvm::function_ref<Optional<SyntacticElementTarget>(SyntacticElementTarget)>
        rewriteTarget) {
  auto resultTarget = target;
  auto &forEachStmtInfo = resultTarget.getForEachStmtInfo();
  auto *stmt = target.getAsForEachStmt();
  auto *parsedSequence = stmt->getParsedSequence();
  bool isAsync = stmt->getAwaitLoc().isValid();

  // Simplify the various types.
  forEachStmtInfo.sequenceType =
      solution.simplifyType(forEachStmtInfo.sequenceType);
  forEachStmtInfo.elementType =
      solution.simplifyType(forEachStmtInfo.elementType);
  forEachStmtInfo.initType =
      solution.simplifyType(forEachStmtInfo.initType);

  auto &cs = solution.getConstraintSystem();
  auto *dc = target.getDeclContext();

  // First, let's apply the solution to the sequence expression.
  {
    auto *makeIteratorVar = forEachStmtInfo.makeIteratorVar;

    auto makeIteratorTarget = *cs.getTargetFor({makeIteratorVar, /*index=*/0});

    auto rewrittenTarget = rewriteTarget(makeIteratorTarget);
    if (!rewrittenTarget)
      return None;

    // Set type-checked initializer and mark it as such.
    {
      makeIteratorVar->setInit(/*index=*/0, rewrittenTarget->getAsExpr());
      makeIteratorVar->setInitializerChecked(/*index=*/0);
    }

    stmt->setIteratorVar(makeIteratorVar);
  }

  // Now, `$iterator.next()` call.
  {
    auto nextTarget = *cs.getTargetFor(forEachStmtInfo.nextCall);

    auto rewrittenTarget = rewriteTarget(nextTarget);
    if (!rewrittenTarget)
      return None;

    Expr *nextCall = rewrittenTarget->getAsExpr();
    // Wrap a call to `next()` into `try await` since `AsyncIteratorProtocol`
    // witness could be `async throws`.
    if (isAsync) {
      // Cannot use `forEachChildExpr` here because we need to
      // to wrap a call in `try` and then stop immediately after.
      struct TryInjector : ASTWalker {
        ASTContext &C;
        const Solution &S;

        bool ShouldStop = false;

        TryInjector(ASTContext &ctx, const Solution &solution)
            : C(ctx), S(solution) {}

        MacroWalking getMacroWalkingBehavior() const override {
          return MacroWalking::Expansion;
        }

        PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
          if (ShouldStop)
            return Action::Stop();

          if (auto *call = dyn_cast<CallExpr>(E)) {
            // There is a single call expression in `nextCall`.
            ShouldStop = true;

            auto nextRefType =
              S.getResolvedType(call->getFn())->castTo<FunctionType>();

            // If the inferred witness is throwing, we need to wrap the call
            // into `try` expression.
            if (nextRefType->isThrowing()) {
              auto *tryExpr = TryExpr::createImplicit(
                  C, /*tryLoc=*/call->getStartLoc(), call, call->getType());
              // Cannot stop here because we need to make sure that
              // the new expression gets injected into AST.
              return Action::SkipChildren(tryExpr);
            }
          }

          return Action::Continue(E);
        }
      };

      nextCall->walk(TryInjector(cs.getASTContext(), solution));
    }

    stmt->setNextCall(nextCall);
  }

  // Coerce the pattern to the element type.
  {
    TypeResolutionOptions options(TypeResolverContext::ForEachStmt);
    options |= TypeResolutionFlags::OverrideType;

    // Apply the solution to the pattern as well.
    auto contextualPattern = target.getContextualPattern();
    auto coercedPattern = TypeChecker::coercePatternToType(
        contextualPattern, forEachStmtInfo.initType, options);
    if (!coercedPattern)
      return None;

    stmt->setPattern(coercedPattern);
    resultTarget.setPattern(coercedPattern);
  }

  // Apply the solution to the filtering condition, if there is one.
  if (auto *whereExpr = stmt->getWhere()) {
    auto whereTarget = *cs.getTargetFor(whereExpr);

    auto rewrittenTarget = rewriteTarget(whereTarget);
    if (!rewrittenTarget)
      return None;

    stmt->setWhere(rewrittenTarget->getAsExpr());
  }

  // Convert that Optional<Element> value to the type of the pattern.
  auto optPatternType = OptionalType::get(forEachStmtInfo.initType);
  Type nextResultType = OptionalType::get(forEachStmtInfo.elementType);
  if (!optPatternType->isEqual(nextResultType)) {
    ASTContext &ctx = cs.getASTContext();
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

  // Get the conformance of the sequence type to the Sequence protocol.
  {
    auto sequenceProto = TypeChecker::getProtocol(
        cs.getASTContext(), stmt->getForLoc(),
        stmt->getAwaitLoc().isValid() ? KnownProtocolKind::AsyncSequence
                                      : KnownProtocolKind::Sequence);

    auto type = forEachStmtInfo.sequenceType->getRValueType();
    if (type->isExistentialType()) {
      auto *contextualLoc = solution.getConstraintLocator(
          parsedSequence, LocatorPathElt::ContextualType(CTP_ForEachSequence));
      type = Type(solution.OpenedExistentialTypes[contextualLoc]);
    }
    auto sequenceConformance = TypeChecker::conformsToProtocol(
        type, sequenceProto, dc->getParentModule());
    assert(!sequenceConformance.isInvalid() &&
           "Couldn't find sequence conformance");
    stmt->setSequenceConformance(sequenceConformance);
  }

  return resultTarget;
}

Optional<SyntacticElementTarget>
ExprWalker::rewriteTarget(SyntacticElementTarget target) {
  // Rewriting the target might abort in case one of visit methods returns
  // nullptr. In this case, no more walkToExprPost calls are issues and thus
  // nodes which were pushed on the Rewriter's ExprStack in walkToExprPre are
  // not popped of the stack again in walkTokExprPost. Usually, that's not an
  // issue if rewriting completely terminates because the ExprStack is never
  // used again. Here, however, we recover from a rewriting failure and continue
  // using the Rewriter. To make sure we don't continue with an ExprStack that
  // is still in the state when rewriting was aborted, save it here and restore
  // it once rewriting this target has finished.
  llvm::SaveAndRestore<SmallVector<Expr *, 8>> RestoreExprStack(
      Rewriter.ExprStack);
  auto &solution = Rewriter.solution;

  // Apply the solution to the target.
  SyntacticElementTarget result = target;
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

    case CTP_Unused:
    case CTP_CaseStmt:
    case CTP_ReturnStmt:
    case CTP_ExprPattern:
    case CTP_ForEachStmt:
    case CTP_ForEachSequence:
    case CTP_ReturnSingleExpr:
    case CTP_YieldByValue:
    case CTP_YieldByReference:
    case CTP_ThrowStmt:
    case CTP_DiscardStmt:
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
    case CTP_SingleValueStmtBranch:
      result.setExpr(rewrittenExpr);
      break;
    }
  } else if (auto stmtCondition = target.getAsStmtCondition()) {
    auto &cs = solution.getConstraintSystem();

    for (auto &condElement : *stmtCondition) {
      switch (condElement.getKind()) {
      case StmtConditionElement::CK_Availability:
        continue;

      case StmtConditionElement::CK_HasSymbol: {
        auto info = condElement.getHasSymbolInfo();
        auto target = *cs.getTargetFor(&condElement);
        auto resolvedTarget = rewriteTarget(target);
        if (!resolvedTarget) {
          info->setInvalid();
          return None;
        }

        auto rewrittenExpr = resolvedTarget->getAsExpr();
        info->setSymbolExpr(rewrittenExpr);
        info->setReferencedDecl(
            TypeChecker::getReferencedDeclForHasSymbolCondition(rewrittenExpr));
        continue;
      }

      case StmtConditionElement::CK_Boolean: {
        auto target = *cs.getTargetFor(&condElement);
        auto resolvedTarget = rewriteTarget(target);
        if (!resolvedTarget)
          return None;

        condElement.setBoolean(resolvedTarget->getAsExpr());
        continue;
      }

      case StmtConditionElement::CK_PatternBinding: {
        auto target = *cs.getTargetFor(&condElement);
        auto resolvedTarget = rewriteTarget(target);
        if (!resolvedTarget)
          return None;

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

    // Check whether this enum element is resolved via ~= application.
    if (auto *enumElement = dyn_cast<EnumElementPattern>(info.pattern)) {
      if (auto target = cs.getTargetFor(enumElement)) {
        auto *EP = target->getExprPattern();
        auto enumType = solution.getResolvedType(EP);

        auto *matchCall = target->getAsExpr();

        auto *result = matchCall->walk(*this);
        if (!result)
          return None;

        {
          auto *matchVar = EP->getMatchVar();
          matchVar->setInterfaceType(enumType->mapTypeOutOfContext());
        }

        EP->setMatchExpr(result);
        EP->setType(enumType);

        (*caseLabelItem)->setPattern(EP, /*resolved=*/true);
        return target;
      }
    }

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
    if (auto *guardExpr = info.guardExpr) {
      auto target = *cs.getTargetFor(guardExpr);
      auto resultTarget = rewriteTarget(target);
      if (!resultTarget)
        return None;

      (*caseLabelItem)->setGuardExpr(resultTarget->getAsExpr());
    }

    return target;
  } else if (auto patternBinding = target.getAsPatternBinding()) {
    ConstraintSystem &cs = solution.getConstraintSystem();
    for (unsigned index : range(patternBinding->getNumPatternEntries())) {
      if (patternBinding->isInitializerChecked(index))
        continue;

      // Find the target for this.
      auto knownTarget = *cs.getTargetFor({patternBinding, index});

      // Rewrite the target.
      auto resultTarget = rewriteTarget(knownTarget);
      if (!resultTarget)
        return None;

      auto *pattern = resultTarget->getInitializationPattern();
      // Record that the pattern has been fully validated,
      // this is important for subsequent call to typeCheckDecl
      // because otherwise it would try to re-typecheck pattern.
      patternBinding->setPattern(index, pattern, resultTarget->getDeclContext(),
                                 /*isFullyValidated=*/true);

      if (patternBinding->isExplicitlyInitialized(index) ||
          (patternBinding->isDefaultInitializable(index) &&
           pattern->hasStorage())) {
        patternBinding->setInit(index, resultTarget->getAsExpr());
        patternBinding->setInitializerChecked(index);
      }
    }

    return target;
  } else if (auto *wrappedVar = target.getAsUninitializedWrappedVar()) {
    // Get the outermost wrapper type from the solution
    auto outermostWrapper = wrappedVar->getOutermostAttachedPropertyWrapper();
    auto backingType = solution.simplifyType(
        solution.getType(outermostWrapper->getTypeExpr()));

    auto &ctx = solution.getConstraintSystem().getASTContext();
    ctx.setSideCachedPropertyWrapperBackingPropertyType(
        wrappedVar, backingType->mapTypeOutOfContext());

    return target;
  } else if (auto *pattern = target.getAsUninitializedVar()) {
    TypeResolutionOptions options(TypeResolverContext::PatternBindingDecl);

    auto contextualPattern = target.getContextualPattern();
    auto patternType = target.getTypeOfUninitializedVar();
    auto *PB = target.getPatternBindingOfUninitializedVar();

    // If this is a placeholder variable, let's override its type with
    // inferred one.
    if (isPlaceholderVar(PB)) {
      patternType = solution.getResolvedType(PB->getSingleVar());
      options |= TypeResolutionFlags::OverrideType;
    }

    if (auto coercedPattern = TypeChecker::coercePatternToType(
            contextualPattern, patternType, options)) {
      auto resultTarget = target;
      resultTarget.setPattern(coercedPattern);
      return resultTarget;
    }

    return None;
  } else if (auto *forEach = target.getAsForEachStmt()) {
    auto forEachResultTarget = applySolutionToForEachStmt(
        solution, target, [&](SyntacticElementTarget target) {
          auto resultTarget = rewriteTarget(target);
          if (resultTarget) {
            if (auto expr = resultTarget->getAsExpr())
              solution.setExprTypes(expr);
          }

          return resultTarget;
        });
    if (!forEachResultTarget)
      return None;

    result = *forEachResultTarget;
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
      if (!convertType)
        return false;

      if (convertType->hasPlaceholder())
        return false;

      if (target.isOptionalSomePatternInit())
        return false;

      if (solution.getResolvedType(resultExpr)->isUninhabited() ||
          solution.simplifyType(convertType)->isVoid()) {
        auto contextPurpose = cs.getContextualTypePurpose(target.getAsExpr());
        if (contextPurpose == CTP_ReturnSingleExpr ||
            contextPurpose == CTP_SingleValueStmtBranch) {
          return false;
        }
      }
      return true;
    };

    // If we're supposed to convert the expression to some particular type,
    // do so now.
    if (shouldCoerceToContextualType()) {
      auto contextualTypePurpose = target.getExprContextualTypePurpose();

      auto *locator = target.getExprConvertTypeLocator();
      if (!locator) {
        // Bodies of single-expression closures use a special locator
        // for contextual type conversion to make sure that result is
        // convertible to `Void` when `return` is not used explicitly.
        auto *closure = dyn_cast<ClosureExpr>(target.getDeclContext());
        if (closure && closure->hasSingleExpressionBody() &&
            contextualTypePurpose == CTP_ClosureResult) {
          auto *returnStmt =
              castToStmt<ReturnStmt>(closure->getBody()->getLastElement());

          locator = cs.getConstraintLocator(
              closure, LocatorPathElt::ClosureBody(
                           /*hasReturn=*/!returnStmt->isImplicit()));
        } else {
          locator = cs.getConstraintLocator(
              expr, LocatorPathElt::ContextualType(contextualTypePurpose));
        }
      }
      assert(locator);

      resultExpr = Rewriter.coerceToType(
          resultExpr, solution.simplifyType(convertType), locator);
    } else if (cs.getType(resultExpr)->hasLValueType() &&
               !target.isDiscardedExpr()) {
      // We referenced an lvalue. Load it.
      resultExpr = Rewriter.coerceToType(
          resultExpr, cs.getType(resultExpr)->getRValueType(),
          cs.getConstraintLocator(expr,
                                  LocatorPathElt::ContextualType(
                                      target.getExprContextualTypePurpose())));
    }

    if (!resultExpr)
      return None;

    // For an @autoclosure default parameter type, add the autoclosure
    // conversion.
    if (FunctionType *autoclosureParamType =
            target.getAsAutoclosureParamType()) {
      resultExpr = cs.buildAutoClosureExpr(resultExpr, autoclosureParamType,
                                           target.getDeclContext());
    }

    solution.setExprTypes(resultExpr);
    result.setExpr(resultExpr);

    if (cs.isDebugMode()) {
      // If target is a multi-statement closure or
      // a tap expression, expression will not be fully
      // type checked until these expressions are visited in
      // processDelayed().
      bool isPartial = false;
      resultExpr->forEachChildExpr([&](Expr *child) -> Expr * {
        if (auto *closure = dyn_cast<ClosureExpr>(child)) {
          if (!closure->hasSingleExpressionBody()) {
            isPartial = true;
            return nullptr;
          }
        }
        if (isa<TapExpr>(child)) {
          isPartial = true;
          return nullptr;
        }
      return child;
      });
      
      auto &log = llvm::errs();
      if (isPartial) {
        log << "\n---Partially type-checked expression---\n";
      } else {
        log << "\n---Type-checked expression---\n";
      }
      resultExpr->dump(log);
      log << "\n";
    }
  }

  return result;
}

/// Apply a given solution to the expression, producing a fully
/// type-checked expression.
Optional<SyntacticElementTarget>
ConstraintSystem::applySolution(Solution &solution,
                                SyntacticElementTarget target) {
  // If any fixes needed to be applied to arrive at this solution, resolve
  // them to specific expressions.
  unsigned numResolvableFixes = 0;
  if (!solution.Fixes.empty()) {
    if (shouldSuppressDiagnostics())
      return None;

    bool diagnosedErrorsViaFixes = applySolutionFixes(solution);
    bool canApplySolution = true;
    for (const auto fix : solution.Fixes) {
      if (fix->isFatal())
        canApplySolution = false;
      if (fix->impact() == SK_Fix && !fix->isFatal())
        ++numResolvableFixes;
    }

    // If all of the available fixes would result in a warning,
    // we can go ahead and apply this solution to AST.
    if (!canApplySolution) {
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
    if (score.Data[SK_Fix] > numResolvableFixes || score.Data[SK_Hole] > 0) {
      maybeProduceFallbackDiagnostic(target);
      return None;
    }
  }

  ExprRewriter rewriter(*this, solution, target, shouldSuppressDiagnostics());
  ExprWalker walker(rewriter);
  auto resultTarget = walker.rewriteTarget(target);
  if (!resultTarget)
    return None;

  auto needsPostProcessing = walker.hasDelayedTasks();
  
  // Visit closures that have non-single expression bodies, tap expressions,
  // and possibly other types of AST nodes which could only be processed
  // after contextual expression.
  bool hadError = walker.processDelayed();

  // If any of them failed to type check, bail.
  if (hadError)
    return None;
  
  if (isDebugMode()) {
    // If we had partially type-checked expressions, lets print
    // fully type-checked target after processDelayed is done.
    auto node = target.getAsASTNode();
    if (node && needsPostProcessing) {
      auto &log = llvm::errs();
      log << "\n---Fully type-checked target---\n";
      node.dump(log);
      log << "\n";
    }
  }
  
  rewriter.finalize();

  return resultTarget;
}

Expr *
Solution::coerceToType(Expr *expr, Type toType, ConstraintLocator *locator) {
  auto &cs = getConstraintSystem();
  ExprRewriter rewriter(cs, *this, None, /*suppressDiagnostics=*/false);
  Expr *result = rewriter.coerceToType(expr, toType, locator);
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

bool Solution::hasType(const KeyPathExpr *KP, unsigned ComponentIndex) const {
  assert(KP && "Expected non-null key path parameter!");
  return keyPathComponentTypes.find(std::make_pair(KP, ComponentIndex))
            != keyPathComponentTypes.end();
}

Type Solution::getType(ASTNode node) const {
  auto result = nodeTypes.find(node);
  if (result != nodeTypes.end())
    return result->second;

  auto &cs = getConstraintSystem();
  return cs.getType(node);
}

Type Solution::getType(const KeyPathExpr *KP, unsigned I) const {
  assert(hasType(KP, I) && "Expected type to have been set!");
  return keyPathComponentTypes.find(std::make_pair(KP, I))->second;
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

SolutionResult SolutionResult::forTooComplex(Optional<SourceRange> affected) {
  SolutionResult result(Kind::TooComplex);
  result.TooComplexAt = affected;
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
