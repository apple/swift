//===--- TypeCheckCaptures.cpp - Capture Analysis -------------------------===//
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
// This file implements computing capture info for closure expressions and named
// local functions.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "TypeCheckObjC.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeWalker.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/SmallPtrSet.h"
using namespace swift;

namespace {

class FindCapturedVars : public ASTWalker {
  ASTContext &Context;
  SmallVector<CapturedValue, 4> Captures;
  llvm::SmallDenseMap<ValueDecl*, unsigned, 4> captureEntryNumber;
  SourceLoc GenericParamCaptureLoc;
  SourceLoc DynamicSelfCaptureLoc;
  DynamicSelfType *DynamicSelf = nullptr;
  OpaqueValueExpr *OpaqueValue = nullptr;
  SourceLoc CaptureLoc;
  DeclContext *CurDC;
  bool NoEscape, ObjC;
  bool HasGenericParamCaptures;

public:
  FindCapturedVars(SourceLoc CaptureLoc,
                   DeclContext *CurDC,
                   bool NoEscape,
                   bool ObjC,
                   bool IsGenericFunction)
      : Context(CurDC->getASTContext()), CaptureLoc(CaptureLoc), CurDC(CurDC),
        NoEscape(NoEscape), ObjC(ObjC), HasGenericParamCaptures(IsGenericFunction) {}

  CaptureInfo getCaptureInfo() const {
    DynamicSelfType *dynamicSelfToRecord = nullptr;

    // Only local functions capture dynamic 'Self'.
    if (CurDC->getParent()->isLocalContext()) {
      if (DynamicSelfCaptureLoc.isValid())
        dynamicSelfToRecord = DynamicSelf;
    }

    return CaptureInfo(Context, Captures, dynamicSelfToRecord, OpaqueValue,
                       HasGenericParamCaptures);
  }

  bool hasGenericParamCaptures() const {
    return HasGenericParamCaptures;
  }

  SourceLoc getGenericParamCaptureLoc() const {
    return GenericParamCaptureLoc;
  }

  SourceLoc getDynamicSelfCaptureLoc() const {
    return DynamicSelfCaptureLoc;
  }

  /// Check if the type of an expression references any generic
  /// type parameters, or the dynamic Self type.
  ///
  /// Note that we do not need to distinguish inner from outer generic
  /// parameters here -- if a local function has its own inner parameter
  /// list, it also implicitly captures outer parameters, even if they're
  /// not used anywhere inside the body.
  void checkType(Type type, SourceLoc loc) {
    if (!type)
      return;

    // We want to look through type aliases here.
    type = type->getCanonicalType();
    
    class TypeCaptureWalker : public TypeWalker {
      bool ObjC;
      std::function<void(Type)> Callback;
    public:
      explicit TypeCaptureWalker(bool ObjC,
                                 std::function<void(Type)> callback)
        : ObjC(ObjC), Callback(std::move(callback)) {}
    
      Action walkToTypePre(Type ty) override {
        Callback(ty);
        // Pseudogeneric classes don't use their generic parameters so we
        // don't need to visit them.
        if (ObjC) {
          if (auto clazz = dyn_cast_or_null<ClassDecl>(ty->getAnyNominal())) {
            if (clazz->isTypeErasedGenericClass()) {
              return Action::SkipChildren;
            }
          }
        }
        return Action::Continue;
      }
    };
    // If the type contains dynamic 'Self', conservatively assume we will
    // need 'Self' metadata at runtime. We could generalize the analysis
    // used below for usages of generic parameters in Objective-C
    // extensions, and re-use it here.
    //
    // For example, forming an existential from a value of type 'Self'
    // does not need the dynamic 'Self' type -- the static type will
    // suffice. Also, just passing around a value of type 'Self' does
    // not need metadata either, since it is represented as a single
    // retainable pointer. Similarly stored property access does not
    // need it, etc.
    if (type->hasDynamicSelfType()) {
      type.walk(TypeCaptureWalker(ObjC, [&](Type t) {
        if (auto *dynamicSelf = t->getAs<DynamicSelfType>()) {
          if (DynamicSelfCaptureLoc.isInvalid()) {
            DynamicSelfCaptureLoc = loc;
            DynamicSelf = dynamicSelf;
          }
        }
      }));
    }

    // Similar to dynamic 'Self', IRGen doesn't really need type metadata
    // for class-bound archetypes in nearly as many cases as with opaque
    // archetypes.
    //
    // Perhaps this entire analysis should happen at the SILGen level,
    // instead, but even there we don't really have enough information to
    // perform it accurately.
    if (type->hasArchetype() || type->hasTypeParameter()) {
      type.walk(TypeCaptureWalker(ObjC, [&](Type t) {
        if ((t->is<ArchetypeType>() ||
             t->is<GenericTypeParamType>()) &&
            !t->isOpenedExistential() &&
            !HasGenericParamCaptures) {
          GenericParamCaptureLoc = loc;
          HasGenericParamCaptures = true;
        }
      }));
    }

    if (auto *gft = type->getAs<GenericFunctionType>()) {
      TypeCaptureWalker walker(ObjC, [&](Type t) {
        if (t->is<GenericTypeParamType>() &&
            !HasGenericParamCaptures) {
          GenericParamCaptureLoc = loc;
          HasGenericParamCaptures = true;
        }
      });

      for (const auto &param : gft->getParams())
        param.getPlainType().walk(walker);

      gft->getResult().walk(walker);
    }
  }

  /// Add the specified capture to the closure's capture list, diagnosing it
  /// if invalid.
  void addCapture(CapturedValue capture) {
    auto VD = capture.getDecl();
    
    if (auto var = dyn_cast<VarDecl>(VD)) {
      // `async let` variables cannot currently be captured.
      if (var->isAsyncLet()) {
        Context.Diags.diagnose(capture.getLoc(), diag::capture_async_let_not_supported);
        return;
      }
    }

    // Check to see if we already have an entry for this decl.
    unsigned &entryNumber = captureEntryNumber[VD];
    if (entryNumber == 0) {
      Captures.push_back(capture);
      entryNumber = Captures.size();
    } else {
      // If this already had an entry in the capture list, make sure to merge
      // the information together.  If one is noescape but the other isn't,
      // then the result is escaping.
      auto existing = Captures[entryNumber-1];
      unsigned flags = existing.getFlags() & capture.getFlags();
      capture = CapturedValue(VD, flags, existing.getLoc());
      Captures[entryNumber-1] = capture;
    }

    // Visit the type of the capture, if it isn't a class reference, since
    // we'd need the metadata to do so.
    if (VD->hasInterfaceType()
        && (!ObjC
            || !isa<VarDecl>(VD)
            || !cast<VarDecl>(VD)->getType()->hasRetainablePointerRepresentation()))
      checkType(VD->getInterfaceType(), VD->getLoc());
  }

  LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
    // We don't want to walk into lazy initializers because they're not
    // really present at this level.  We'll catch them when processing
    // the getter.
    return LazyInitializerWalking::None;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkResult<Expr *> walkToDeclRefExpr(DeclRefExpr *DRE) {
    auto *D = DRE->getDecl();

    // HACK: $interpolation variables are seen as needing to be captured. 
    // The good news is, we literally never need to capture them, so we 
    // can safely ignore them.
    // FIXME(TapExpr): This is probably caused by the scoping 
    // algorithm's ignorance of TapExpr. We should fix that.
    if (D->getBaseName() == Context.Id_dollarInterpolation)
      return Action::SkipChildren(DRE);

    // DC is the DeclContext where D was defined
    // CurDC is the DeclContext where D was referenced
    auto DC = D->getDeclContext();

    // Capture the generic parameters of the decl, unless it's a
    // local declaration in which case we will pick up generic
    // parameter references transitively.
    if (!DC->isLocalContext()) {
      if (!ObjC || !D->isObjC() || isa<ConstructorDecl>(D)) {
        if (auto subMap = DRE->getDeclRef().getSubstitutions()) {
          for (auto type : subMap.getReplacementTypes()) {
            checkType(type, DRE->getLoc());
          }
        }
      }
    }

    // Don't "capture" type definitions at all.
    if (isa<TypeDecl>(D))
      return Action::SkipChildren(DRE);

    // A local reference is not a capture.
    if (CurDC == DC || isa<TopLevelCodeDecl>(CurDC))
      return Action::SkipChildren(DRE);

    auto TmpDC = CurDC;
    while (TmpDC != nullptr) {
      // Variables defined inside TopLevelCodeDecls are semantically
      // local variables. If the reference is not from the top level,
      // we have a capture.
      if (isa<TopLevelCodeDecl>(DC) &&
          (isa<SourceFile>(TmpDC) || isa<TopLevelCodeDecl>(TmpDC)))
        break;

      if (TmpDC == DC)
        break;

      // The initializer of a lazy property will eventually get
      // recontextualized into it, so treat it as if it's already there.
      if (auto init = dyn_cast<PatternBindingInitializer>(TmpDC)) {
        if (auto lazyVar = init->getInitializedLazyVar()) {
          // If we have a getter with a body, we're already re-parented
          // everything so pretend we're inside the getter.
          if (auto getter = lazyVar->getAccessor(AccessorKind::Get)) {
            if (getter->getBody(/*canSynthesize=*/false)) {
              TmpDC = getter;
              continue;
            }
          }
        }
      }

      // We have an intervening nominal type context that is not the
      // declaration context, and the declaration context is not global.
      // This is not supported since nominal types cannot capture values.
      if (auto NTD = dyn_cast<NominalTypeDecl>(TmpDC)) {
        // Allow references to local functions from inside methods of a
        // local type, because if the local function has captures, we'll
        // diagnose them in SILGen. It's a bit unfortunate that we can't
        // ban this outright, but people rely on code like this working:
        //
        // do {
        //   func local() {}
        //   class C {
        //     func method() { local() }
        //   }
        // }
        if (!isa<FuncDecl>(D)) {
          if (DC->isLocalContext()) {
            Context.Diags.diagnose(DRE->getLoc(), diag::capture_across_type_decl,
                                   NTD->getDescriptiveKind(),
                                   D->getBaseIdentifier());

            NTD->diagnose(diag::kind_declared_here,
                          DescriptiveDeclKind::Type);

            D->diagnose(diag::decl_declared_here, D->getName());
            return Action::SkipChildren(DRE);
          }
        }
      }

      TmpDC = TmpDC->getParent();
    }

    // We walked all the way up to the root without finding the declaration,
    // so this is not a capture.
    if (TmpDC == nullptr)
      return Action::SkipChildren(DRE);

    // Only capture var decls at global scope.  Other things can be captured
    // if they are local.
    if (!isa<VarDecl>(D) && !D->isLocalCapture())
      return Action::SkipChildren(DRE);

    // We're going to capture this, compute flags for the capture.
    unsigned Flags = 0;

    // If this is a direct reference to underlying storage, then this is a
    // capture of the storage address - not a capture of the getter/setter.
    if (auto var = dyn_cast<VarDecl>(D)) {
      if (var->getAccessStrategy(DRE->getAccessSemantics(),
                                 var->supportsMutation()
                                   ? AccessKind::ReadWrite
                                   : AccessKind::Read,
                                 CurDC->getParentModule(),
                                 CurDC->getResilienceExpansion())
          .getKind() == AccessStrategy::Storage)
        Flags |= CapturedValue::IsDirect;
    }

    // If the closure is noescape, then we can capture the decl as noescape.
    if (NoEscape)
      Flags |= CapturedValue::IsNoEscape;

    addCapture(CapturedValue(D, Flags, DRE->getStartLoc()));
    return Action::SkipChildren(DRE);
  }

  void propagateCaptures(CaptureInfo captureInfo, SourceLoc loc) {
    for (auto capture : captureInfo.getCaptures()) {
      // If the decl was captured from us, it isn't captured *by* us.
      if (capture.getDecl()->getDeclContext() == CurDC)
        continue;

      // Compute adjusted flags.
      unsigned Flags = capture.getFlags();

      // The decl is captured normally, even if it was captured directly
      // in the subclosure.
      Flags &= ~CapturedValue::IsDirect;

      // If this is an escaping closure, then any captured decls are also
      // escaping, even if they are coming from an inner noescape closure.
      if (!NoEscape)
        Flags &= ~CapturedValue::IsNoEscape;

      addCapture(CapturedValue(capture.getDecl(), Flags, capture.getLoc()));
    }

    if (!HasGenericParamCaptures) {
      if (captureInfo.hasGenericParamCaptures()) {
        GenericParamCaptureLoc = loc;
        HasGenericParamCaptures = true;
      }
    }

    if (DynamicSelfCaptureLoc.isInvalid()) {
      if (captureInfo.hasDynamicSelfCapture()) {
        DynamicSelfCaptureLoc = loc;
        DynamicSelf = captureInfo.getDynamicSelfType();
      }
    }

    if (!OpaqueValue) {
      if (captureInfo.hasOpaqueValueCapture())
        OpaqueValue = captureInfo.getOpaqueValue();
    }
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      TypeChecker::computeCaptures(AFD);
      propagateCaptures(AFD->getCaptureInfo(), AFD->getLoc());
      return Action::SkipChildren();
    }

    // Don't walk into local types; we'll walk their initializers when we check
    // the local type itself.
    if (isa<NominalTypeDecl>(D))
      return Action::SkipChildren();

    return Action::Continue();
  }

  bool usesTypeMetadataOfFormalType(Expr *E) {
    // For non-ObjC closures, assume the type metadata is always used.
    if (!ObjC)
      return true;

    if (!E->getType() || E->getType()->hasError())
      return false;

    // We can use Objective-C generics in limited ways without reifying
    // their type metadata, meaning we don't need to capture their generic
    // params.

    // Look through one layer of optionality when considering the class-

    // Referring to a class-constrained generic or metatype
    // doesn't require its type metadata.
    if (auto declRef = dyn_cast<DeclRefExpr>(E))
      return (!declRef->getDecl()->isObjC()
              && !E->getType()->getWithoutSpecifierType()
                              ->hasRetainablePointerRepresentation()
              && !E->getType()->getWithoutSpecifierType()
                              ->is<AnyMetatypeType>());

    // Loading classes or metatypes doesn't require their metadata.
    if (isa<LoadExpr>(E))
      return (!E->getType()->hasRetainablePointerRepresentation()
              && !E->getType()->is<AnyMetatypeType>());

    // Accessing @objc members doesn't require type metadata.
    // rdar://problem/27796375 -- allocating init entry points for ObjC
    // initializers are generated as true Swift generics, so reify type
    // parameters.
    if (auto memberRef = dyn_cast<MemberRefExpr>(E))
      return !memberRef->getMember().getDecl()->hasClangNode();

    if (auto applyExpr = dyn_cast<ApplyExpr>(E)) {
      if (auto methodApply = dyn_cast<ApplyExpr>(applyExpr->getFn())) {
        if (auto callee = dyn_cast<DeclRefExpr>(methodApply->getFn())) {
          return !callee->getDecl()->isObjC()
            || isa<ConstructorDecl>(callee->getDecl());
        }
      }
      if (auto callee = dyn_cast<DeclRefExpr>(applyExpr->getFn())) {
        return !callee->getDecl()->isObjC()
          || isa<ConstructorDecl>(callee->getDecl());
      }
    }

    if (auto subscriptExpr = dyn_cast<SubscriptExpr>(E)) {
      return (subscriptExpr->hasDecl() &&
              !subscriptExpr->getDecl().getDecl()->isObjC());
    }

    // Getting the dynamic type of a class doesn't require type metadata.
    if (isa<DynamicTypeExpr>(E))
      return (!E->getType()->castTo<AnyMetatypeType>()->getInstanceType()
                  ->hasRetainablePointerRepresentation());

    // Building a fixed-size tuple doesn't require type metadata.
    // Approximate this for the purposes of being able to invoke @objc methods
    // by considering tuples of ObjC-representable types to not use metadata.
    if (auto tuple = dyn_cast<TupleExpr>(E)) {
      for (auto elt : tuple->getType()->castTo<TupleType>()->getElements()) {
        if (!elt.getType()->isRepresentableIn(ForeignLanguage::ObjectiveC,
                                              CurDC))
          return true;
      }
      return false;
    }

    // Coercion by itself is a no-op.
    if (isa<CoerceExpr>(E))
      return false;

    // Upcasting doesn't require type metadata.
    if (isa<DerivedToBaseExpr>(E))
      return false;
    if (isa<ArchetypeToSuperExpr>(E))
      return false;
    if (isa<CovariantReturnConversionExpr>(E))
      return false;
    if (isa<MetatypeConversionExpr>(E))
      return false;

    // Identity expressions are no-ops.
    if (isa<IdentityExpr>(E))
      return false;

    // Discarding an assignment is a no-op.
    if (isa<DiscardAssignmentExpr>(E))
      return false;

    // Opening an @objc existential or metatype is a no-op.
    if (auto open = dyn_cast<OpenExistentialExpr>(E))
      return (!open->getSubExpr()->getType()->isObjCExistentialType()
              && !open->getSubExpr()->getType()->is<AnyMetatypeType>());

    // Erasure to an ObjC existential or between metatypes doesn't require
    // type metadata.
    if (auto erasure = dyn_cast<ErasureExpr>(E)) {
      if (E->getType()->isObjCExistentialType()
          || E->getType()->is<AnyMetatypeType>())
        return false;
      
      // We also special case Any erasure in pseudogeneric contexts
      // not to rely on concrete type metadata by erasing from AnyObject
      // as a waypoint.
      if (E->getType()->isAny()
          && erasure->getSubExpr()->getType()->is<ArchetypeType>())
        return false;

      // Erasure to a Swift protocol always captures the type metadata from
      // its subexpression.
      checkType(erasure->getSubExpr()->getType(),
                erasure->getSubExpr()->getLoc());
      return true;
    }

    
    // Converting an @objc metatype to AnyObject doesn't require type
    // metadata.
    if (isa<ClassMetatypeToObjectExpr>(E)
        || isa<ExistentialMetatypeToObjectExpr>(E))
      return false;
    
    // Casting to an ObjC class doesn't require the metadata of its type
    // parameters, if any.
    if (auto cast = dyn_cast<CheckedCastExpr>(E)) {
      // If we failed to resolve the written type, we've emitted an
      // earlier diagnostic and should bail.
      const auto toTy = cast->getCastType();
      if (!toTy || toTy->hasError())
        return false;

      if (auto clazz = dyn_cast_or_null<ClassDecl>(toTy->getAnyNominal())) {
        if (clazz->isTypeErasedGenericClass()) {
          return false;
        }
      }
    }
    
    // Assigning an object doesn't require type metadata.
    if (auto assignment = dyn_cast<AssignExpr>(E))
      return assignment->getSrc()->getType() &&
        !assignment->getSrc()->getType()
            ->hasRetainablePointerRepresentation();

    return true;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (usesTypeMetadataOfFormalType(E)) {
      checkType(E->getType(), E->getLoc());
    }

    // Some kinds of expression don't really evaluate their subexpression,
    // so we don't need to traverse.
    if (isa<ObjCSelectorExpr>(E)) {
      return Action::SkipChildren(E);
    }

    if (auto *ECE = dyn_cast<ExplicitCastExpr>(E)) {
      checkType(ECE->getCastType(), ECE->getLoc());
      return Action::Continue(E);
    }

    if (auto *DRE = dyn_cast<DeclRefExpr>(E))
      return walkToDeclRefExpr(DRE);

    // Look into lazy initializers.
    if (auto *LIE = dyn_cast<LazyInitializerExpr>(E)) {
      LIE->getSubExpr()->walk(*this);
      return Action::Continue(E);
    }

    // When we see a reference to the 'super' expression, capture 'self' decl.
    if (auto *superE = dyn_cast<SuperRefExpr>(E)) {
      if (auto *selfDecl = superE->getSelf()) {
        if (CurDC->isChildContextOf(selfDecl->getDeclContext()))
          addCapture(CapturedValue(selfDecl, 0, superE->getLoc()));
      }
      return Action::SkipChildren(superE);
    }

    // Don't recur into child closures. They should already have a capture
    // list computed; we just propagate it, filtering out stuff that they
    // capture from us.
    if (auto *SubCE = dyn_cast<AbstractClosureExpr>(E)) {
      TypeChecker::computeCaptures(SubCE);
      propagateCaptures(SubCE->getCaptureInfo(), SubCE->getLoc());
      return Action::SkipChildren(E);
    }

    // Capture a placeholder opaque value.
    if (auto opaqueValue = dyn_cast<OpaqueValueExpr>(E)) {
      if (opaqueValue->isPlaceholder()) {
        assert(!OpaqueValue || OpaqueValue == opaqueValue);
        OpaqueValue = opaqueValue;
        return Action::Continue(E);
      }
    }

    return Action::Continue(E);
  }
};

} // end anonymous namespace

void TypeChecker::computeCaptures(AnyFunctionRef AFR) {
  if (AFR.getCaptureInfo().hasBeenComputed())
    return;

  if (!AFR.getBody())
    return;

  PrettyStackTraceAnyFunctionRef trace("computing captures for", AFR);

  // A generic function always captures outer generic parameters.
  bool isGeneric = false;
  auto *AFD = AFR.getAbstractFunctionDecl();
  if (AFD)
    isGeneric = (AFD->getGenericParams() != nullptr);

  auto &Context = AFR.getAsDeclContext()->getASTContext();
  FindCapturedVars finder(AFR.getLoc(),
                          AFR.getAsDeclContext(),
                          AFR.isKnownNoEscape(),
                          AFR.isObjC(),
                          isGeneric);
  AFR.getBody()->walk(finder);

  if (AFR.hasType() && !AFR.isObjC()) {
    finder.checkType(AFR.getType(), AFR.getLoc());
  }

  AFR.setCaptureInfo(finder.getCaptureInfo());

  // Compute captures for default argument expressions.
  if (auto *AFD = AFR.getAbstractFunctionDecl()) {
    for (auto *P : *AFD->getParameters()) {
      if (auto E = P->getTypeCheckedDefaultExpr()) {
        FindCapturedVars finder(E->getLoc(),
                                AFD,
                                /*isNoEscape=*/false,
                                /*isObjC=*/false,
                                /*IsGeneric*/isGeneric);
        E->walk(finder);

        if (!AFD->getDeclContext()->isLocalContext() &&
            finder.getDynamicSelfCaptureLoc().isValid()) {
          Context.Diags.diagnose(finder.getDynamicSelfCaptureLoc(),
                                 diag::dynamic_self_default_arg);
        }

        P->setDefaultArgumentCaptureInfo(finder.getCaptureInfo());
      }
    }
  }

  // Extensions of generic ObjC functions can't use generic parameters from
  // their context.
  if (AFD && finder.hasGenericParamCaptures()) {
    if (auto clazz = AFD->getParent()->getSelfClassDecl()) {
      if (clazz->isTypeErasedGenericClass()) {
        AFD->diagnose(diag::objc_generic_extension_using_type_parameter);

        // If it's possible, suggest adding @objc.
        llvm::Optional<ForeignAsyncConvention> asyncConvention;
        llvm::Optional<ForeignErrorConvention> errorConvention;
        if (!AFD->isObjC() &&
            isRepresentableInObjC(AFD, ObjCReason::MemberOfObjCMembersClass,
                                  asyncConvention, errorConvention)) {
          AFD->diagnose(
                   diag::objc_generic_extension_using_type_parameter_try_objc)
            .fixItInsert(AFD->getAttributeInsertionLoc(false), "@objc ");
        }

        Context.Diags.diagnose(
            finder.getGenericParamCaptureLoc(),
            diag::objc_generic_extension_using_type_parameter_here);
      }
    }
  }
}

static bool isLazy(PatternBindingDecl *PBD) {
  if (auto var = PBD->getSingleVar())
    return var->getAttrs().hasAttribute<LazyAttr>();
  return false;
}

void TypeChecker::checkPatternBindingCaptures(IterableDeclContext *DC) {
  for (auto member : DC->getMembers()) {
    // Ignore everything other than PBDs.
    auto *PBD = dyn_cast<PatternBindingDecl>(member);
    if (!PBD) continue;
    // Walk the initializers for all properties declared in the type with
    // an initializer.
    for (unsigned i : range(PBD->getNumPatternEntries())) {
      if (PBD->isInitializerSubsumed(i))
        continue;

      auto *init = PBD->getInit(i);
      if (init == nullptr)
        continue;

      auto *DC = PBD->getInitContext(i);
      FindCapturedVars finder(init->getLoc(),
                              DC,
                              /*NoEscape=*/false,
                              /*ObjC=*/false,
                              /*IsGenericFunction*/false);
      init->walk(finder);

      auto &ctx = DC->getASTContext();
      if (finder.getDynamicSelfCaptureLoc().isValid() && !isLazy(PBD)) {
        ctx.Diags.diagnose(finder.getDynamicSelfCaptureLoc(),
                           diag::dynamic_self_stored_property_init);
      }

      auto captures = finder.getCaptureInfo();
      PBD->setCaptureInfo(i, captures);
    }
  }
}
