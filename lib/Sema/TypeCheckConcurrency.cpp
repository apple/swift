//===--- TypeCheckConcurrency.cpp - Concurrency ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements type checking support for Swift's concurrency model.
//
//===----------------------------------------------------------------------===//
#include "MiscDiagnostics.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckDistributed.h"
#include "TypeCheckInvertible.h"
#include "TypeChecker.h"
#include "TypeCheckType.h"
#include "swift/Strings.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/Basic/Assertions.h"
#include "swift/Sema/IDETypeChecking.h"

using namespace swift;

/// Determine whether it makes sense to infer an attribute in the given
/// context.
static bool shouldInferAttributeInContext(const DeclContext *dc) {
  if (auto *file = dyn_cast<FileUnit>(dc->getModuleScopeContext())) {
    switch (file->getKind()) {
    case FileUnitKind::Source:
      // Check what kind of source file we have.
      if (auto sourceFile = dc->getParentSourceFile()) {
        switch (sourceFile->Kind) {
        case SourceFileKind::Interface:
          // Interfaces have explicitly called-out Sendable conformances.
          return false;

        case SourceFileKind::DefaultArgument:
        case SourceFileKind::Library:
        case SourceFileKind::MacroExpansion:
        case SourceFileKind::Main:
        case SourceFileKind::SIL:
          return true;
        }
      }
      break;

    case FileUnitKind::Builtin:
    case FileUnitKind::SerializedAST:
    case FileUnitKind::Synthesized:
      return false;

    case FileUnitKind::ClangModule:
    case FileUnitKind::DWARFModule:
      return true;
    }

    return true;
  }

  return false;
}

void swift::addAsyncNotes(AbstractFunctionDecl const* func) {
  assert(func);
  if (!isa<DestructorDecl>(func) && !isa<AccessorDecl>(func)) {
    auto note =
        func->diagnose(diag::note_add_async_to_function, func);

    if (func->hasThrows()) {
      auto replacement = func->getAttrs().hasAttribute<RethrowsAttr>()
                        ? "async rethrows"
                        : "async throws";

      note.fixItReplace(SourceRange(func->getThrowsLoc()), replacement);
    } else if (func->getParameters()->getRParenLoc().isValid()) {
      note.fixItInsert(func->getParameters()->getRParenLoc().getAdvancedLoc(1),
                       " async");
    }
  }
}

static bool requiresFlowIsolation(ActorIsolation typeIso,
                                  ConstructorDecl const *ctor) {
  assert(ctor->isDesignatedInit());

  auto ctorIso = getActorIsolation(const_cast<ConstructorDecl *>(ctor));

  // Regardless of async-ness, a mismatch in isolation means we need to be
  // flow-sensitive.
  if (typeIso != ctorIso)
    return true;

  // Otherwise, if it's an actor instance, then it depends on async-ness.
  switch (typeIso.getKind()) {
  case ActorIsolation::GlobalActor:
  case ActorIsolation::Unspecified:
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe:
    return false;

  case ActorIsolation::Erased:
    llvm_unreachable("constructor cannot have erased isolation");

  case ActorIsolation::ActorInstance:
      return !(ctor->hasAsync()); // need flow-isolation for non-async.
  };
}

bool swift::usesFlowSensitiveIsolation(AbstractFunctionDecl const *fn) {
  if (!fn)
    return false;

  // Only designated constructors or destructors use this kind of isolation.
  if (auto const* ctor = dyn_cast<ConstructorDecl>(fn)) {
    if (!ctor->isDesignatedInit())
      return false;
  } else if (!isa<DestructorDecl>(fn)) {
    return false;
  }

  auto *dc = fn->getDeclContext();
  if (!dc)
    return false;

  // Must be part of a nominal type.
  auto *nominal = dc->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  // If it's part of an actor type, then its deinit and some of its inits use
  // flow-isolation.
  if (nominal->isAnyActor()) {
    if (isa<DestructorDecl>(fn))
      return true;

    // construct an isolation corresponding to the type.
    auto actorTypeIso = ActorIsolation::forActorInstanceSelf(
        const_cast<AbstractFunctionDecl *>(fn));

    return requiresFlowIsolation(actorTypeIso, cast<ConstructorDecl>(fn));
  }

  // Otherwise, the type must be isolated to a global actor.
  auto nominalIso = getActorIsolation(nominal);
  if (!nominalIso.isGlobalActor())
    return false;

  // if it's a deinit, then it's flow-isolated.
  if (isa<DestructorDecl>(fn))
    return true;

  return requiresFlowIsolation(nominalIso, cast<ConstructorDecl>(fn));
}

bool IsActorRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal) const {
  // Protocols are actors if they inherit from `Actor`.
  if (auto protocol = dyn_cast<ProtocolDecl>(nominal)) {
    auto &ctx = protocol->getASTContext();
    auto *actorProtocol = ctx.getProtocol(KnownProtocolKind::Actor);
    if (!actorProtocol)
      return false;

    return (protocol == actorProtocol ||
            protocol->inheritsFrom(actorProtocol));
  }

  // Class declarations are actors if they were declared with "actor".
  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if (!classDecl)
    return false;

  return classDecl->isExplicitActor();
}

bool IsDefaultActorRequest::evaluate(
    Evaluator &evaluator, ClassDecl *classDecl, ModuleDecl *M,
    ResilienceExpansion expansion) const {
  // If the class isn't an actor, it's not a default actor.
  if (!classDecl->isActor())
    return false;

  // Distributed actors were not able to have custom executors until Swift 5.9,
  // so in order to avoid wrongly treating a resilient distributed actor from another
  // module as not-default we need to handle this case explicitly.
  if (classDecl->isDistributedActor()) {
    ASTContext &ctx = classDecl->getASTContext();
    auto customExecutorAvailability =
        ctx.getConcurrencyDistributedActorWithCustomExecutorAvailability();

    auto actorAvailability = TypeChecker::overApproximateAvailabilityAtLocation(
        classDecl->getStartLoc(),
        classDecl);

    if (!actorAvailability.isContainedIn(customExecutorAvailability)) {
      // Any 'distributed actor' declared with availability lower than the
      // introduction of custom executors for distributed actors, must be treated as default actor,
      // even if it were to declared the unowned executor property, as older compilers
      // do not have the logic to handle that case.
      return true;
    }
  }

  // If the class is resilient from the perspective of the module
  // module, it's not a default actor.
  if (classDecl->isForeign() || classDecl->isResilient(M, expansion))
    return false;

  // Check whether the class has explicit custom-actor methods.

  // If we synthesized the unownedExecutor property, we should've
  // added a semantics attribute to it (if it was actually a default
  // actor).
  bool foundExecutorPropertyImpl = false;
  bool isDefaultActor = false;
  if (auto executorProperty = classDecl->getUnownedExecutorProperty()) {
    foundExecutorPropertyImpl = true;
    isDefaultActor = isDefaultActor ||
        executorProperty->getAttrs().hasSemanticsAttr(SEMANTICS_DEFAULT_ACTOR);
  }

  // Only if we found one of the executor properties, do we return the status of default or not,
  // based on the findings of the semantics attribute of that located property.
  if (foundExecutorPropertyImpl) {
    if (!isDefaultActor &&
        classDecl->getASTContext().LangOpts.isConcurrencyModelTaskToThread() &&
        !AvailableAttr::isUnavailable(classDecl)) {
      classDecl->diagnose(
          diag::concurrency_task_to_thread_model_custom_executor,
          "task-to-thread concurrency model");
    }

    return isDefaultActor;
  }

  // Otherwise, we definitely are a default actor.
  return true;
}

VarDecl *GlobalActorInstanceRequest::evaluate(
    Evaluator &evaluator, NominalTypeDecl *nominal) const {
  auto globalActorAttr = nominal->getAttrs().getAttribute<GlobalActorAttr>();
  if (!globalActorAttr)
    return nullptr;

  // Ensure that the actor protocol has been loaded.
  ASTContext &ctx = nominal->getASTContext();
  auto actorProto = ctx.getProtocol(KnownProtocolKind::Actor);
  if (!actorProto) {
    nominal->diagnose(diag::concurrency_lib_missing, "Actor");
    return nullptr;
  }

  // Non-final classes cannot be global actors.
  if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
    if (!classDecl->isSemanticallyFinal()) {
      nominal->diagnose(diag::global_actor_non_final_class, nominal->getName())
        .highlight(globalActorAttr->getRangeWithAt());
    }
  }

  // Global actors have a static property "shared" that provides an actor
  // instance. The value must be of Actor type, which is validated by
  // conformance to the 'GlobalActor' protocol.
  SmallVector<ValueDecl *, 4> decls;
  nominal->lookupQualified(
      nominal, DeclNameRef(ctx.Id_shared),
      nominal->getLoc(), NL_QualifiedDefault, decls);
  for (auto decl : decls) {
    auto var = dyn_cast<VarDecl>(decl);
    if (!var)
      continue;

    if (var->getDeclContext() == nominal && var->isStatic())
      return var;
  }

  return nullptr;
}

std::optional<std::pair<CustomAttr *, NominalTypeDecl *>>
swift::checkGlobalActorAttributes(SourceLoc loc, DeclContext *dc,
                                  ArrayRef<CustomAttr *> attrs) {
  ASTContext &ctx = dc->getASTContext();

  CustomAttr *globalActorAttr = nullptr;
  NominalTypeDecl *globalActorNominal = nullptr;
  for (auto attr : attrs) {
    // Figure out which nominal declaration this custom attribute refers to.
    auto *nominal = evaluateOrDefault(ctx.evaluator,
                                      CustomAttrNominalRequest{attr, dc},
                                      nullptr);

    if (!nominal)
      continue;

    // We are only interested in global actor types.
    if (!nominal->isGlobalActor())
      continue;

    // Only a single global actor can be applied to a given entity.
    if (globalActorAttr) {
      ctx.Diags.diagnose(
          loc, diag::multiple_global_actors, globalActorNominal->getName(),
          nominal->getName());
      continue;
    }

    globalActorAttr = const_cast<CustomAttr *>(attr);
    globalActorNominal = nominal;
  }

  if (!globalActorAttr)
    return std::nullopt;

  return std::make_pair(globalActorAttr, globalActorNominal);
}

std::optional<std::pair<CustomAttr *, NominalTypeDecl *>>
GlobalActorAttributeRequest::evaluate(
    Evaluator &evaluator,
    llvm::PointerUnion<Decl *, ClosureExpr *> subject) const {
  DeclContext *dc = nullptr;
  DeclAttributes *declAttrs = nullptr;
  SourceLoc loc;
  if (auto decl = subject.dyn_cast<Decl *>()) {
    dc = decl->getDeclContext();
    declAttrs = &decl->getAttrs();
    // HACK: `getLoc`, when querying the attr from a serialized decl,
    // depending on deserialization order, may launch into arbitrary
    // type-checking when querying interface types of such decls. Which,
    // in turn, may do things like query (to print) USRs. This ends up being
    // prone to request evaluator cycles.
    //
    // Because this only applies to serialized decls, we can be confident
    // that they already went through this type-checking as primaries, so,
    // for now, to avoid cycles, we simply ignore the locs on serialized decls
    // only.
    // This is a workaround for rdar://79563942
    loc = decl->getLoc(/* SerializedOK */ false);
  } else {
    auto closure = subject.get<ClosureExpr *>();
    dc = closure;
    declAttrs = &closure->getAttrs();
    loc = closure->getLoc();
  }

  // Collect the attributes.
  SmallVector<CustomAttr *, 2> attrs;
  for (auto attr : declAttrs->getAttributes<CustomAttr>()) {
    auto mutableAttr = const_cast<CustomAttr *>(attr);
    attrs.push_back(mutableAttr);
  }

  // Look for a global actor attribute.
  auto result = checkGlobalActorAttributes(loc, dc, attrs);
  if (!result)
    return std::nullopt;

  // Closures can always have a global actor attached.
  if (auto closure = subject.dyn_cast<ClosureExpr *>()) {
    return result;
  }

  // Check that a global actor attribute makes sense on this kind of
  // declaration.
  auto decl = subject.get<Decl *>();

  // no further checking required if it's from a serialized module.
  if (decl->getDeclContext()->getParentSourceFile() == nullptr)
    return result;

  auto isStoredInstancePropertyOfStruct = [](VarDecl *var) {
    if (var->isStatic() || !var->isOrdinaryStoredProperty())
      return false;

    auto *nominal = var->getDeclContext()->getSelfNominalTypeDecl();
    return isa_and_nonnull<StructDecl>(nominal) &&
           !isWrappedValueOfPropWrapper(var);
  };

  auto globalActorAttr = result->first;
  if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
    // Nominal types are okay...
    if (auto classDecl = dyn_cast<ClassDecl>(nominal)){
      if (classDecl->isActor()) {
        // ... except for actors.
        nominal->diagnose(diag::global_actor_on_actor_class, nominal->getName())
            .highlight(globalActorAttr->getRangeWithAt());
        return std::nullopt;
      }
    }
  } else if (auto storage = dyn_cast<AbstractStorageDecl>(decl)) {
    // Subscripts and properties are fine...
    if (auto var = dyn_cast<VarDecl>(storage)) {

      // ... but not if it's an async-context top-level global
      if (var->isTopLevelGlobal() &&
          (var->getDeclContext()->isAsyncContext() ||
           var->getASTContext().LangOpts.StrictConcurrencyLevel >=
             StrictConcurrency::Complete)) {
        var->diagnose(diag::global_actor_top_level_var)
            .highlight(globalActorAttr->getRangeWithAt());
        return std::nullopt;
      }

      // ... and not if it's local property
      if (var->getDeclContext()->isLocalContext()) {
        var->diagnose(diag::global_actor_on_local_variable, var->getName())
            .highlight(globalActorAttr->getRangeWithAt());
        return std::nullopt;
      }
    }
  } else if (isa<ExtensionDecl>(decl)) {
    // Extensions are okay.
  } else if (isa<ConstructorDecl>(decl) || isa<FuncDecl>(decl)) {
    // None of the accessors/addressors besides a getter are allowed
    // to have a global actor attribute.
    if (auto *accessor = dyn_cast<AccessorDecl>(decl)) {
      if (!accessor->isGetter()) {
        decl->diagnose(diag::global_actor_disallowed,
                       decl->getDescriptiveKind())
            .warnUntilSwiftVersion(6)
            .fixItRemove(globalActorAttr->getRangeWithAt());

        auto &ctx = decl->getASTContext();
        auto *storage = accessor->getStorage();
        // Let's suggest to move the attribute to the storage if
        // this is an accessor/addressor of a property of subscript.
        if (storage->getDeclContext()->isTypeContext()) {
          auto canMoveAttr = [&]() {
            // If enclosing declaration has a global actor,
            // skip the suggestion.
            if (storage->getGlobalActorAttr())
              return false;

            // Global actor attribute cannot be applied to
            // an instance stored property of a struct.
            if (auto *var = dyn_cast<VarDecl>(storage)) {
              return !isStoredInstancePropertyOfStruct(var);
            }

            return true;
          };

          if (canMoveAttr()) {
            decl->diagnose(diag::move_global_actor_attr_to_storage_decl,
                           storage)
                .fixItInsert(
                    storage->getAttributeInsertionLoc(/*forModifier=*/false),
                    llvm::Twine("@", result->second->getNameStr()).str());
          }
        }

        // In Swift 6, once the diag above is an error, it is disallowed.
        if (ctx.isSwiftVersionAtLeast(6))
          return std::nullopt;
      }
    }
    // Functions are okay.
  } else {
    // Everything else is disallowed.
    decl->diagnose(diag::global_actor_disallowed, decl->getDescriptiveKind());
    return std::nullopt;
  }

  return result;
}

Type swift::getExplicitGlobalActor(ClosureExpr *closure) {
  // Look at the explicit attribute.
  auto globalActorAttr =
      evaluateOrDefault(closure->getASTContext().evaluator,
                        GlobalActorAttributeRequest{closure}, std::nullopt);
  if (!globalActorAttr)
    return Type();

  Type globalActor = evaluateOrDefault(
      closure->getASTContext().evaluator,
      CustomAttrTypeRequest{
        globalActorAttr->first, closure, CustomAttrTypeKind::GlobalActor},
        Type());
  if (!globalActor || globalActor->hasError())
    return Type();

  return globalActor;
}

/// A 'let' declaration is safe across actors if it is either
/// nonisolated or it is accessed from within the same module.
static bool varIsSafeAcrossActors(const ModuleDecl *fromModule,
                                  VarDecl *var,
                                  const ActorIsolation &varIsolation,
                                  ActorReferenceResult::Options &options) {

  bool accessWithinModule =
      (fromModule == var->getDeclContext()->getParentModule());

  if (varIsolation.getKind() == ActorIsolation::NonisolatedUnsafe)
    return true;

  if (!var->isLet()) {
    // A mutable storage of a value type accessed from within the module is
    // okay.
    if (dyn_cast_or_null<StructDecl>(var->getDeclContext()->getAsDecl()) &&
        !var->isStatic() &&
        var->hasStorage() &&
        var->getTypeInContext()->isSendableType() &&
        accessWithinModule) {
      return true;
    }
    // Otherwise, must be immutable.
    return false;
  }

  switch (varIsolation) {
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe:
  case ActorIsolation::Unspecified:
    // if nonisolated, it's OK
    return true;

  case ActorIsolation::Erased:
    llvm_unreachable("variable cannot have erased isolation");

  case ActorIsolation::ActorInstance:
  case ActorIsolation::GlobalActor:
    // If it's explicitly 'nonisolated', it's okay.
    if (var->getAttrs().hasAttribute<NonisolatedAttr>())
      return true;

    // Static 'let's are initialized upon first access, so they cannot be
    // synchronously accessed across actors.
    if (var->isGlobalStorage() && var->isLazilyInitializedGlobal()) {
      // Compiler versions <= 5.9 accepted this code, so downgrade to a
      // warning prior to Swift 6.
      options = ActorReferenceResult::Flags::Preconcurrency;
      return false;
    }

    // If it's distributed, generally variable access is not okay...
    if (auto nominalParent = var->getDeclContext()->getSelfNominalTypeDecl()) {
      if (nominalParent->isDistributedActor())
        return false;
    }

    // If the type is not 'Sendable', it's unsafe
    if (!var->getTypeInContext()->isSendableType()) {
      // Compiler versions <= 5.10 treated this variable as nonisolated,
      // so downgrade async access errors in the effects checker to
      // warnings prior to Swift 6.
      if (accessWithinModule)
        options = ActorReferenceResult::Flags::Preconcurrency;

      return false;
    }

    // If it's actor-isolated but in the same module, then it's OK too.
    return accessWithinModule;
  }
}

bool swift::isLetAccessibleAnywhere(const ModuleDecl *fromModule,
                                    VarDecl *let,
                                    ActorReferenceResult::Options &options) {
  auto isolation = getActorIsolation(let);
  return varIsSafeAcrossActors(fromModule, let, isolation, options);
}

bool swift::isLetAccessibleAnywhere(const ModuleDecl *fromModule,
                                    VarDecl *let) {
  ActorReferenceResult::Options options = std::nullopt;
  return isLetAccessibleAnywhere(fromModule, let, options);
}

namespace {
  /// Describes the important parts of a partial apply thunk.
  struct PartialApplyThunkInfo {
    Expr *base;
    Expr *fn;
    bool isEscaping;
  };
}

/// Try to decompose a call that might be an invocation of a partial apply
/// thunk.
static std::optional<PartialApplyThunkInfo>
decomposePartialApplyThunk(ApplyExpr *apply, Expr *parent) {
  // Check for a call to the outer closure in the thunk.
  auto outerAutoclosure = dyn_cast<AutoClosureExpr>(apply->getFn());
  if (!outerAutoclosure || outerAutoclosure->getThunkKind() !=
                               AutoClosureExpr::Kind::DoubleCurryThunk)
    return std::nullopt;

  auto *unarySelfArg = apply->getArgs()->getUnlabeledUnaryExpr();
  assert(unarySelfArg &&
         "Double curry should start with a unary (Self) -> ... arg");

  auto memberFn = outerAutoclosure->getUnwrappedCurryThunkExpr();
  if (!memberFn)
    return std::nullopt;

  // Determine whether the partial apply thunk was immediately converted to
  // noescape.
  bool isEscaping = true;
  if (auto conversion = dyn_cast_or_null<FunctionConversionExpr>(parent)) {
    auto fnType = conversion->getType()->getAs<FunctionType>();
    isEscaping = fnType && !fnType->isNoEscape();
  }

  return PartialApplyThunkInfo{unarySelfArg, memberFn, isEscaping};
}

/// Find the immediate member reference in the given expression.
static std::optional<std::pair<ConcreteDeclRef, SourceLoc>>
findReference(Expr *expr) {
  // Look through a function conversion.
  if (auto fnConv = dyn_cast<FunctionConversionExpr>(expr))
    expr = fnConv->getSubExpr();

  if (auto declRef = dyn_cast<DeclRefExpr>(expr))
    return std::make_pair(declRef->getDeclRef(), declRef->getLoc());

  if (auto otherCtor = dyn_cast<OtherConstructorDeclRefExpr>(expr)) {
    return std::make_pair(otherCtor->getDeclRef(), otherCtor->getLoc());
  }

  Expr *inner = expr->getValueProvidingExpr();
  if (inner != expr)
    return findReference(inner);

  return std::nullopt;
}

/// Return true if the callee of an ApplyExpr is async
///
/// Note that this must be called after the implicitlyAsync flag has been set,
/// or implicitly async calls will not return the correct value.
static bool isAsyncCall(
    llvm::PointerUnion<ApplyExpr *, LookupExpr *> call) {

  if (auto *apply = call.dyn_cast<ApplyExpr *>()) {
    if (apply->isImplicitlyAsync())
      return true;

    // Effectively the same as doing a
    // `cast_or_null<FunctionType>(call->getFn()->getType())`, check the
    // result of that and then checking `isAsync` if it's defined.
    Type funcTypeType = apply->getFn()->getType();
    if (!funcTypeType)
      return false;
    AnyFunctionType *funcType = funcTypeType->getAs<AnyFunctionType>();
    if (!funcType)
      return false;
    return funcType->isAsync();
  }

  auto *lookup = call.get<LookupExpr *>();
  if (lookup->isImplicitlyAsync())
    return true;

  return isAsyncDecl(lookup->getDecl());
}

/// Determine whether we should diagnose data races within the current context.
///
/// By default, we do this only in code that makes use of concurrency
/// features.
static bool shouldDiagnoseExistingDataRaces(const DeclContext *dc);

/// Determine whether this closure should be treated as Sendable.
///
/// \param forActorIsolation Whether this check is for the purposes of
/// determining whether the closure must be non-isolated.
static bool isSendableClosure(
    const AbstractClosureExpr *closure, bool forActorIsolation) {
  if (auto explicitClosure = dyn_cast<ClosureExpr>(closure)) {
    if (forActorIsolation && explicitClosure->inheritsActorContext()) {
      return false;
    }
  }

  if (auto type = closure->getType()) {
    if (auto fnType = type->getAs<AnyFunctionType>())
      if (fnType->isSendable())
        return true;
  }

  return false;
}

/// Returns true if this closure acts as an inference boundary in the AST. An
/// inference boundary is an expression in the AST where we newly infer
/// isolation different from our parent decl context.
///
/// Examples:
///
///   1. a @Sendable closure.
///   2. a closure literal passed to a sending parameter.
///
/// NOTE: This does not mean that it has nonisolated isolation since for
/// instance one could define an @MainActor closure in a nonisolated
/// function. That @MainActor closure would act as an Isolation Inference
/// Boundary.
///
/// \arg forActorIsolation we currently have two slightly varying semantics
/// here. If this is set, then we assuming that we are being called recursively
/// while walking up a decl context path to determine the actor isolation of a
/// closure. In such a case, we do not want to be a boundary if we should
/// inheritActorContext. In other contexts though, we want to determine if the
/// closure is part of an init or deinit. In such a case, we are walking up the
/// decl context chain and we want to stop if we see a sending parameter since
/// in such a case, the sending closure parameter is known to not be part of the
/// init or deinit.
static bool
isIsolationInferenceBoundaryClosure(const AbstractClosureExpr *closure,
                                    bool forActorIsolation) {
  if (auto *ce = dyn_cast<ClosureExpr>(closure)) {
    if (!forActorIsolation) {
      // For example, one would along this path see if for flow sensitive
      // isolation the closure is part of an init or deinit.
      if (ce->isPassedToSendingParameter())
        return true;
    } else {
      // This is for actor isolation. If we have inheritActorContext though, we
      // do not want to do anything since we are part of our parent's isolation.
      if (!ce->inheritsActorContext() && ce->isPassedToSendingParameter())
        return true;
    }
  }

  return isSendableClosure(closure, forActorIsolation);
}

/// Add Fix-It text for the given nominal type to adopt Sendable.
static void addSendableFixIt(
    const NominalTypeDecl *nominal, InFlightDiagnostic &diag, bool unchecked) {
  if (nominal->getInherited().empty()) {
    SourceLoc fixItLoc = nominal->getBraces().Start;
    diag.fixItInsert(fixItLoc,
                     unchecked ? ": @unchecked Sendable" : ": Sendable");
  } else {
    auto fixItLoc = nominal->getInherited().getEndLoc();
    diag.fixItInsertAfter(fixItLoc,
                          unchecked ? ", @unchecked Sendable" : ", Sendable");
  }
}

/// Add Fix-It text for the given generic param declaration type to adopt
/// Sendable.
static void addSendableFixIt(const GenericTypeParamDecl *genericArgument,
                             InFlightDiagnostic &diag, bool unchecked) {
  if (genericArgument->getInherited().empty()) {
    auto fixItLoc = genericArgument->getLoc();
    diag.fixItInsertAfter(fixItLoc,
                          unchecked ? ": @unchecked Sendable" : ": Sendable");
  } else {
    auto fixItLoc = genericArgument->getInherited().getEndLoc();
    diag.fixItInsertAfter(fixItLoc,
                          unchecked ? ", @unchecked Sendable" : ", Sendable");
  }
}

static bool shouldDiagnoseExistingDataRaces(const DeclContext *dc) {
  return contextRequiresStrictConcurrencyChecking(dc, [](const AbstractClosureExpr *) {
    return Type();
  },
  [](const ClosureExpr *closure) {
    return closure->isIsolatedByPreconcurrency();
  });
}

bool SendableCheckContext::isExplicitSendableConformance() const {
  if (!conformanceCheck)
    return false;

  switch (*conformanceCheck) {
  case SendableCheck::Explicit:
    return true;

  case SendableCheck::ImpliedByStandardProtocol:
  case SendableCheck::Implicit:
  case SendableCheck::ImplicitForExternallyVisible:
    return false;
  }
}

DiagnosticBehavior SendableCheckContext::defaultDiagnosticBehavior() const {
  // If we're not supposed to diagnose existing data races from this context,
  // ignore the diagnostic entirely.
  if (!isExplicitSendableConformance() &&
      !shouldDiagnoseExistingDataRaces(fromDC))
    return DiagnosticBehavior::Ignore;

  return DiagnosticBehavior::Warning;
}

DiagnosticBehavior
SendableCheckContext::implicitSendableDiagnosticBehavior() const {
  switch (fromDC->getASTContext().LangOpts.StrictConcurrencyLevel) {
  case StrictConcurrency::Targeted:
    // Limited checking only diagnoses implicit Sendable within contexts that
    // have adopted concurrency.
    if (shouldDiagnoseExistingDataRaces(fromDC))
      return DiagnosticBehavior::Warning;

    LLVM_FALLTHROUGH;

  case StrictConcurrency::Minimal:
    // Explicit Sendable conformances always diagnose, even when strict
    // strict checking is disabled.
    if (isExplicitSendableConformance())
      return DiagnosticBehavior::Warning;

    return DiagnosticBehavior::Ignore;

  case StrictConcurrency::Complete:
    return defaultDiagnosticBehavior();
  }
}

/// Determine whether the given nominal type has an explicit Sendable
/// conformance (regardless of its availability).
bool swift::hasExplicitSendableConformance(NominalTypeDecl *nominal,
                                           bool applyModuleDefault) {
  ASTContext &ctx = nominal->getASTContext();
  auto nominalModule = nominal->getParentModule();

  // In a concurrency-checked module, a missing conformance is equivalent to
  // an explicitly unavailable one. If we want to apply this rule, do so now.
  if (applyModuleDefault && nominalModule->isConcurrencyChecked())
    return true;

  // Look for any conformance to `Sendable`.
  auto proto = ctx.getProtocol(KnownProtocolKind::Sendable);
  if (!proto)
    return false;

  // Look for a conformance. If it's present and not (directly) missing,
  // we're done.
  auto conformance = nominalModule->lookupConformance(
      nominal->getDeclaredInterfaceType(), proto, /*allowMissing=*/true);
  return conformance &&
      !(isa<BuiltinProtocolConformance>(conformance.getConcrete()) &&
        cast<BuiltinProtocolConformance>(
          conformance.getConcrete())->isMissing());
}

/// Determine the diagnostic behavior for a Sendable reference to the given
/// nominal type.
DiagnosticBehavior SendableCheckContext::diagnosticBehavior(
    NominalTypeDecl *nominal) const {
  if (hasExplicitSendableConformance(nominal))
    return DiagnosticBehavior::Warning;

  DiagnosticBehavior defaultBehavior = implicitSendableDiagnosticBehavior();

  // If we are checking an implicit Sendable conformance, don't suppress
  // diagnostics for declarations in the same module. We want them to make
  // enclosing inferred types non-Sendable.
  if (defaultBehavior == DiagnosticBehavior::Ignore &&
      nominal->getParentSourceFile() &&
      conformanceCheck && isImplicitSendableCheck(*conformanceCheck))
    return DiagnosticBehavior::Warning;

  return defaultBehavior;
}

std::optional<DiagnosticBehavior>
swift::getConcurrencyDiagnosticBehaviorLimit(NominalTypeDecl *nominal,
                                             const DeclContext *fromDC,
                                             bool ignoreExplicitConformance) {
  ModuleDecl *importedModule = nullptr;
  if (nominal->getAttrs().hasAttribute<PreconcurrencyAttr>()) {
    // If the declaration itself has the @preconcurrency attribute,
    // respect it.
    importedModule = nominal->getParentModule();
  } else {
    // Determine whether this nominal type is visible via a @preconcurrency
    // import.
    auto import = nominal->findImport(fromDC);
    auto sourceFile = fromDC->getParentSourceFile();

    if (!import || !import->options.contains(ImportFlags::Preconcurrency))
      return std::nullopt;

    if (sourceFile)
      sourceFile->setImportUsedPreconcurrency(*import);

    importedModule = import->module.importedModule;
  }

  // When the type is explicitly non-Sendable, @preconcurrency imports
  // downgrade the diagnostic to a warning in Swift 6.
  if (!ignoreExplicitConformance &&
      hasExplicitSendableConformance(nominal))
    return DiagnosticBehavior::Warning;

  // When the type is implicitly non-Sendable, `@preconcurrency` suppresses
  // diagnostics until the imported module enables Swift 6.
  return importedModule->isConcurrencyChecked()
      ? DiagnosticBehavior::Warning
      : DiagnosticBehavior::Ignore;
}

std::optional<DiagnosticBehavior>
SendableCheckContext::preconcurrencyBehavior(
    Decl *decl,
    bool ignoreExplicitConformance) const {
  if (!decl)
    return std::nullopt;

  if (auto *nominal = dyn_cast<NominalTypeDecl>(decl)) {
    return getConcurrencyDiagnosticBehaviorLimit(nominal, fromDC,
                                                 ignoreExplicitConformance);
  }

  return std::nullopt;
}

static bool shouldDiagnosePreconcurrencyImports(SourceFile &sf) {
  switch (sf.Kind) {
  case SourceFileKind::Interface:
  case SourceFileKind::SIL:
      return false;

  case SourceFileKind::DefaultArgument:
  case SourceFileKind::Library:
  case SourceFileKind::Main:
  case SourceFileKind::MacroExpansion:
      return true;
  }
}

bool swift::diagnoseSendabilityErrorBasedOn(
    NominalTypeDecl *nominal, SendableCheckContext fromContext,
    llvm::function_ref<bool(DiagnosticBehavior)> diagnose) {
  auto behavior = DiagnosticBehavior::Unspecified;

  if (nominal) {
    behavior = fromContext.diagnosticBehavior(nominal);
  } else {
    behavior = fromContext.implicitSendableDiagnosticBehavior();
  }

  bool wasSuppressed = diagnose(behavior);

  SourceFile *sourceFile = fromContext.fromDC->getParentSourceFile();
  if (sourceFile && shouldDiagnosePreconcurrencyImports(*sourceFile)) {
    bool emittedDiagnostics =
        behavior != DiagnosticBehavior::Ignore && !wasSuppressed;

    // When the type is explicitly Sendable *or* explicitly non-Sendable, we
    // assume it has been audited and `@preconcurrency` is not recommended even
    // though it would actually affect the diagnostic.
    bool nominalIsImportedAndHasImplicitSendability =
        nominal &&
        nominal->getParentModule() != fromContext.fromDC->getParentModule() &&
        !hasExplicitSendableConformance(nominal);

    if (emittedDiagnostics && nominalIsImportedAndHasImplicitSendability) {
      // This type was imported from another module; try to find the
      // corresponding import.
      std::optional<AttributedImport<swift::ImportedModule>> import =
          nominal->findImport(fromContext.fromDC);

      // If we found the import that makes this nominal type visible, remark
      // that it can be @preconcurrency import.
      // Only emit this remark once per source file, because it can happen a
      // lot.
      if (import && !import->options.contains(ImportFlags::Preconcurrency) &&
          import->importLoc.isValid() && sourceFile &&
          !sourceFile->hasImportUsedPreconcurrency(*import)) {
        SourceLoc importLoc = import->importLoc;
        ASTContext &ctx = nominal->getASTContext();

        ctx.Diags
            .diagnose(importLoc, diag::add_predates_concurrency_import,
                      ctx.LangOpts.isSwiftVersionAtLeast(6),
                      nominal->getParentModule()->getName())
            .fixItInsert(importLoc, "@preconcurrency ");

        sourceFile->setImportUsedPreconcurrency(*import);
      }
    }
  }

  return behavior == DiagnosticBehavior::Unspecified && !wasSuppressed;
}

void swift::diagnoseUnnecessaryPreconcurrencyImports(SourceFile &sf) {
  if (!shouldDiagnosePreconcurrencyImports(sf))
    return;

  ASTContext &ctx = sf.getASTContext();

  if (ctx.TypeCheckerOpts.SkipFunctionBodies != FunctionBodySkipping::None)
    return;

  for (const auto &import : sf.getImports()) {
    if (import.options.contains(ImportFlags::Preconcurrency) &&
        import.importLoc.isValid() &&
        !sf.hasImportUsedPreconcurrency(import)) {
      ctx.Diags.diagnose(
          import.importLoc, diag::remove_predates_concurrency_import,
          import.module.importedModule->getName())
        .fixItRemove(import.preconcurrencyRange);
    }
  }
}

/// Produce a diagnostic for a single instance of a non-Sendable type where
/// a Sendable type is required.
static bool diagnoseSingleNonSendableType(
    Type type, SendableCheckContext fromContext,
    Type inDerivedConformance, SourceLoc loc,
    llvm::function_ref<bool(Type, DiagnosticBehavior)> diagnose) {
  if (type->hasError())
    return false;

  auto module = fromContext.fromDC->getParentModule();
  auto nominal = type->getAnyNominal();
  auto &ctx = module->getASTContext();

  return diagnoseSendabilityErrorBasedOn(nominal, fromContext,
                                         [&](DiagnosticBehavior behavior) {
    bool wasSuppressed = diagnose(type, behavior);

    // Don't emit the following notes if we didn't have any diagnostics to
    // attach them to.
    if (wasSuppressed || behavior == DiagnosticBehavior::Ignore)
      return true;

    if (inDerivedConformance) {
      ctx.Diags.diagnose(loc, diag::in_derived_conformance,
                         inDerivedConformance);
    }

    if (type->is<FunctionType>()) {
      ctx.Diags.diagnose(loc, diag::nonsendable_function_type);
    } else if (nominal && nominal->getParentModule() == module) {
      // If the nominal type is in the current module, suggest adding
      // `Sendable` if it might make sense. Otherwise, just complain.
      if (isa<StructDecl>(nominal) || isa<EnumDecl>(nominal)) {
        auto note = nominal->diagnose(
            diag::add_nominal_sendable_conformance, nominal);
        addSendableFixIt(nominal, note, /*unchecked=*/false);
      } else {
        nominal->diagnose(diag::non_sendable_nominal, nominal);
      }
    } else if (nominal) {
      // Note which nominal type does not conform to `Sendable`.
      nominal->diagnose(diag::non_sendable_nominal, nominal);
    } else if (auto genericArchetype = type->getAs<ArchetypeType>()) {
      auto interfaceType = genericArchetype->getInterfaceType();
      if (auto genericParamType =
              interfaceType->getAs<GenericTypeParamType>()) {
        auto *genericParamTypeDecl = genericParamType->getDecl();
        if (genericParamTypeDecl &&
            genericParamTypeDecl->getModuleContext() == module) {
          auto diag = genericParamTypeDecl->diagnose(
              diag::add_generic_parameter_sendable_conformance, type);
          addSendableFixIt(genericParamTypeDecl, diag, /*unchecked=*/false);
        }
      }
    }

    return false;
  });
}

bool swift::diagnoseNonSendableTypes(
    Type type, SendableCheckContext fromContext,
    Type inDerivedConformance, SourceLoc loc,
    llvm::function_ref<bool(Type, DiagnosticBehavior)> diagnose) {
  auto module = fromContext.fromDC->getParentModule();

  // If the Sendable protocol is missing, do nothing.
  auto proto = module->getASTContext().getProtocol(KnownProtocolKind::Sendable);
  if (!proto)
    return false;

  // FIXME: More detail for unavailable conformances.
  auto conformance = module->lookupConformance(type, proto, /*allowMissing=*/true);
  if (conformance.isInvalid() || conformance.hasUnavailableConformance()) {
    return diagnoseSingleNonSendableType(
        type, fromContext, inDerivedConformance, loc, diagnose);
  }

  // Walk the conformance, diagnosing any missing Sendable conformances.
  bool anyMissing = false;
  conformance.forEachMissingConformance(
      [&](BuiltinProtocolConformance *missing) {
        if (diagnoseSingleNonSendableType(
                missing->getType(), fromContext,
                inDerivedConformance, loc, diagnose)) {
          anyMissing = true;
        }

        return false;
      });

  return anyMissing;
}

bool swift::diagnoseNonSendableTypesInReference(
    Expr *base, ConcreteDeclRef declRef, const DeclContext *fromDC,
    SourceLoc refLoc, SendableCheckReason refKind,
    std::optional<ActorIsolation> knownIsolation,
    FunctionCheckOptions funcCheckOptions, SourceLoc diagnoseLoc) {
  // Retrieve the actor isolation to use in diagnostics.
  auto getActorIsolation = [&] {
    if (knownIsolation)
      return *knownIsolation;

    return swift::getActorIsolation(declRef.getDecl());
  };

  // If the violation is in the implementation of a derived conformance,
  // point to the location of the parent type instead.
  Type derivedConformanceType;
  if (refLoc.isInvalid()) {
    auto *decl = fromDC->getAsDecl();
    if (decl && decl->isImplicit()) {
      if (auto *implements = decl->getAttrs().getAttribute<ImplementsAttr>()) {
        auto *parentDC = decl->getDeclContext();
        refLoc = parentDC->getAsDecl()->getLoc();
        derivedConformanceType =
            implements->getProtocol(parentDC)->getDeclaredInterfaceType();
      }
    }
  }

  // Check the 'self' argument.
  if (base) {
    if (diagnoseNonSendableTypes(
            base->getType(),
            fromDC, derivedConformanceType,
            base->getStartLoc(),
            diag::non_sendable_param_type,
            (unsigned)refKind, declRef.getDecl(),
            getActorIsolation()))
      return true;
  }

  // For functions, check the parameter and result types.
  SubstitutionMap subs = declRef.getSubstitutions();
  if (auto function = dyn_cast<AbstractFunctionDecl>(declRef.getDecl())) {
    if (funcCheckOptions.contains(FunctionCheckKind::Params)) {
      // only check params if funcCheckKind specifies so
      for (auto param : *function->getParameters()) {
        Type paramType = param->getInterfaceType().subst(subs);
        if (diagnoseNonSendableTypes(
                paramType, fromDC, derivedConformanceType,
                refLoc, diagnoseLoc.isInvalid() ? refLoc : diagnoseLoc,
                diag::non_sendable_param_type,
            (unsigned)refKind, function, getActorIsolation()))
          return true;
      }
    }

    // Check the result type of a function.
    if (auto func = dyn_cast<FuncDecl>(function)) {
      if (funcCheckOptions.contains(FunctionCheckKind::Results)) {
        // only check results if funcCheckKind specifies so
        Type resultType = func->getResultInterfaceType().subst(subs);
        if (diagnoseNonSendableTypes(
            resultType, fromDC, derivedConformanceType,
            refLoc, diagnoseLoc.isInvalid() ? refLoc : diagnoseLoc,
                diag::non_sendable_result_type,
            (unsigned)refKind, func, getActorIsolation()))
          return true;
      }
    }

    return false;
  }

  if (auto var = dyn_cast<VarDecl>(declRef.getDecl())) {
    Type propertyType = var->isLocalCapture()
        ? var->getTypeInContext()
        : var->getValueInterfaceType().subst(subs);
    if (diagnoseNonSendableTypes(
            propertyType, fromDC,
            derivedConformanceType, refLoc,
            diag::non_sendable_property_type,
            var,
            var->isLocalCapture(),
            (unsigned)refKind,
            getActorIsolation()))
      return true;
  }

  if (auto subscript = dyn_cast<SubscriptDecl>(declRef.getDecl())) {
    for (auto param : *subscript->getIndices()) {
      if (funcCheckOptions.contains(FunctionCheckKind::Params)) {
        // Check params of this subscript override for sendability
        Type paramType = param->getInterfaceType().subst(subs);
        if (diagnoseNonSendableTypes(
                paramType, fromDC, derivedConformanceType,
                refLoc, diagnoseLoc.isInvalid() ? refLoc : diagnoseLoc,
                diag::non_sendable_param_type,
                (unsigned)refKind, subscript, getActorIsolation()))
          return true;
      }
    }

    if (funcCheckOptions.contains(FunctionCheckKind::Results)) {
      // Check the element type of a subscript.
      Type resultType = subscript->getElementInterfaceType().subst(subs);
      if (diagnoseNonSendableTypes(
              resultType, fromDC, derivedConformanceType,
              refLoc, diagnoseLoc.isInvalid() ? refLoc : diagnoseLoc,
              diag::non_sendable_result_type,
          (unsigned)refKind, subscript, getActorIsolation()))
        return true;
    }

    return false;
  }

  return false;
}

void swift::diagnoseMissingSendableConformance(
    SourceLoc loc, Type type, const DeclContext *fromDC) {
  diagnoseNonSendableTypes(
      type, fromDC, /*inDerivedConformance*/Type(),
      loc, diag::non_sendable_type);
}

namespace {
/// Infer Sendable from the instance storage of the given nominal type.
/// \returns \c std::nullopt if there is no way to make the type \c Sendable,
/// \c true if \c Sendable needs to be @unchecked, \c false if it can be
/// \c Sendable without the @unchecked.
std::optional<bool>
inferSendableFromInstanceStorage(NominalTypeDecl *nominal,
                                 SmallVectorImpl<Requirement> &requirements) {
  // Raw storage is assumed not to be sendable.
  if (auto sd = dyn_cast<StructDecl>(nominal)) {
    if (sd->getAttrs().hasAttribute<RawLayoutAttr>()) {
      return true;
    }
  }

  class Visitor : public StorageVisitor {
  public:
    NominalTypeDecl *nominal;
    SmallVectorImpl<Requirement> &requirements;
    bool isUnchecked = false;
    ProtocolDecl *sendableProto = nullptr;

    Visitor(NominalTypeDecl *nominal,
            SmallVectorImpl<Requirement> &requirements)
        : StorageVisitor(), nominal(nominal), requirements(requirements) {
      ASTContext &ctx = nominal->getASTContext();
      sendableProto = ctx.getProtocol(KnownProtocolKind::Sendable);
    }

    bool operator()(VarDecl *var, Type propertyType) override {
      // If we have a class with mutable state, only an @unchecked
      // conformance will work.
      if (isa<ClassDecl>(nominal) && var->supportsMutation())
        isUnchecked = true;

      return checkType(propertyType);
    }

    bool operator()(EnumElementDecl *element, Type elementType) override {
      return checkType(elementType);
    }

    /// Check sendability of the given type, recording any requirements.
    bool checkType(Type type) {
      if (!sendableProto)
        return true;

      auto module = nominal->getParentModule();
      auto conformance = module->checkConformance(type, sendableProto);
      if (conformance.isInvalid())
        return true;

      // If there is an unavailable conformance here, fail.
      if (conformance.hasUnavailableConformance())
        return true;

      // Look for missing Sendable conformances.
      return conformance.forEachMissingConformance(
          [&](BuiltinProtocolConformance *missing) {
            // For anything other than Sendable, fail.
            if (missing->getProtocol() != sendableProto)
              return true;

            // If we have an archetype, capture the requirement
            // to make this type Sendable.
            if (missing->getType()->is<ArchetypeType>()) {
              requirements.push_back(
                  Requirement(RequirementKind::Conformance,
                              missing->getType()->mapTypeOutOfContext(),
                              sendableProto->getDeclaredType()));
              return false;
            }

            return true;
          });
    }
  } visitor(nominal, requirements);

  return visitor.visit(nominal, nominal);
}
}

static bool checkSendableInstanceStorage(
    NominalTypeDecl *nominal, DeclContext *dc, SendableCheck check);

void swift::diagnoseMissingExplicitSendable(NominalTypeDecl *nominal) {
  // Only diagnose when explicitly requested.
  ASTContext &ctx = nominal->getASTContext();
  if (!ctx.LangOpts.RequireExplicitSendable)
    return;

  if (nominal->getLoc().isInvalid())
    return;

  // Protocols aren't checked.
  if (isa<ProtocolDecl>(nominal))
    return;

  // Actors are always Sendable.
  if (auto classDecl = dyn_cast<ClassDecl>(nominal))
    if (classDecl->isActor())
      return;

  // Only public/open types have this check.
  if (!nominal->getFormalAccessScope(
        /*useDC=*/nullptr,
        /*treatUsableFromInlineAsPublic=*/true).isPublic())
    return;

  // If the conformance is explicitly stated, do nothing.
  if (hasExplicitSendableConformance(nominal, /*applyModuleDefault=*/false))
    return;

  // Diagnose it.
  nominal->diagnose(diag::public_decl_needs_sendable, nominal);

  // Note to add a Sendable conformance, possibly an unchecked one.
  {
    llvm::SmallVector<Requirement, 2> requirements;
    auto canMakeSendable = inferSendableFromInstanceStorage(
        nominal, requirements);

    // Non-final classes can only have @unchecked.
    bool isUnchecked = !canMakeSendable || *canMakeSendable;
    if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
      if (!classDecl->isFinal())
        isUnchecked = true;
    }

    auto note = nominal->diagnose(
        isUnchecked ? diag::explicit_unchecked_sendable
                    : diag::add_nominal_sendable_conformance,
        nominal);
    if (canMakeSendable && !requirements.empty()) {
      // Produce a Fix-It containing a conditional conformance to Sendable,
      // based on the requirements harvested from instance storage.

      // Form the where clause containing all of the requirements.
      std::string whereClause;
      {
        llvm::raw_string_ostream out(whereClause);
        llvm::interleaveComma(
            requirements, out,
            [&](const Requirement &req) {
              out << req.getFirstType().getString() << ": "
                  << req.getSecondType().getString();
            });
      }

      // Add a Fix-It containing the conditional extension text itself.
      auto insertionLoc = nominal->getBraces().End;
      note.fixItInsertAfter(
          insertionLoc,
          ("\n\nextension " + nominal->getName().str() + ": "
           + (isUnchecked? "@unchecked " : "") + "Sendable where " +
           whereClause + " { }\n").str());
    } else {
      addSendableFixIt(nominal, note, isUnchecked);
    }
  }

  // Note to disable the warning.
  {
    auto note = nominal->diagnose(diag::explicit_disable_sendable, nominal);
    auto insertionLoc = nominal->getBraces().End;
    note.fixItInsertAfter(
        insertionLoc,
        ("\n\n@available(*, unavailable)\nextension " + nominal->getName().str()
         + ": Sendable { }\n").str());
  }
}

void swift::tryDiagnoseExecutorConformance(ASTContext &C,
                                           const NominalTypeDecl *nominal,
                                           ProtocolDecl *proto) {
  assert(proto->isSpecificProtocol(KnownProtocolKind::Executor) ||
         proto->isSpecificProtocol(KnownProtocolKind::SerialExecutor) ||
         proto->isSpecificProtocol(KnownProtocolKind::TaskExecutor));

  auto &diags = C.Diags;
  auto module = nominal->getParentModule();
  Type nominalTy = nominal->getDeclaredInterfaceType();
  NominalTypeDecl *executorDecl = C.getExecutorDecl();

  // enqueue(_:)
  auto enqueueDeclName = DeclName(C, DeclBaseName(C.Id_enqueue), { Identifier() });

  FuncDecl *moveOnlyEnqueueRequirement = nullptr;
  FuncDecl *legacyMoveOnlyEnqueueRequirement = nullptr;
  FuncDecl *unownedEnqueueRequirement = nullptr;
  for (auto req: proto->getProtocolRequirements()) {
    auto *funcDecl = dyn_cast<FuncDecl>(req);
    if (!funcDecl)
      continue;

    if (funcDecl->getName() != enqueueDeclName)
      continue;

    // look for the first parameter being a Job or UnownedJob
    if (funcDecl->getParameters()->size() != 1)
      continue;

    if (auto param = funcDecl->getParameters()->front()) {
      StructDecl *executorJobDecl = C.getExecutorJobDecl();
      StructDecl *legacyJobDecl = C.getJobDecl();
      StructDecl *unownedJobDecl = C.getUnownedJobDecl();

      if (executorJobDecl && param->getInterfaceType()->isEqual(executorJobDecl->getDeclaredInterfaceType())) {
        assert(moveOnlyEnqueueRequirement == nullptr);
        moveOnlyEnqueueRequirement = funcDecl;
      } else if (legacyJobDecl && param->getInterfaceType()->isEqual(legacyJobDecl->getDeclaredInterfaceType())) {
        assert(legacyMoveOnlyEnqueueRequirement == nullptr);
        legacyMoveOnlyEnqueueRequirement = funcDecl;
      } else if (unownedJobDecl && param->getInterfaceType()->isEqual(unownedJobDecl->getDeclaredInterfaceType())) {
        assert(unownedEnqueueRequirement == nullptr);
        unownedEnqueueRequirement = funcDecl;
      }
    }

    // if we found all potential requirements, we're done here and break out of the loop
    if (unownedEnqueueRequirement &&
        moveOnlyEnqueueRequirement &&
        legacyMoveOnlyEnqueueRequirement)
      break; // we're done looking for the requirements
  }

  auto conformance = module->lookupConformance(nominalTy, proto);
  auto concreteConformance = conformance.getConcrete();
  assert(unownedEnqueueRequirement && "could not find the enqueue(UnownedJob) requirement, which should be always there");

  // try to find at least a single implementations of enqueue(_:)
  ValueDecl *unownedEnqueueWitnessDecl = concreteConformance->getWitnessDecl(unownedEnqueueRequirement);
  ValueDecl *moveOnlyEnqueueWitnessDecl = nullptr;
  ValueDecl *legacyMoveOnlyEnqueueWitnessDecl = nullptr;

  if (moveOnlyEnqueueRequirement) {
    moveOnlyEnqueueWitnessDecl = concreteConformance->getWitnessDecl(
        moveOnlyEnqueueRequirement);
  }
  if (legacyMoveOnlyEnqueueRequirement) {
    legacyMoveOnlyEnqueueWitnessDecl = concreteConformance->getWitnessDecl(
        legacyMoveOnlyEnqueueRequirement);
  }

  // --- Diagnose warnings and errors

  // true iff the nominal type's availability allows the legacy requirement
  // to be omitted in favor of moveOnlyEnqueueRequirement
  bool canRemoveOldDecls;
  if (!moveOnlyEnqueueRequirement) {
    // The move only enqueue does not exist in this lib version, we must keep relying on the UnownedJob version
    canRemoveOldDecls = false;
  } else if (C.LangOpts.DisableAvailabilityChecking) {
    // Assume we have all APIs available, and thus can use the ExecutorJob
    canRemoveOldDecls = true;
  } else {
    // Check if the availability of nominal is high enough to be using the ExecutorJob version
    AvailabilityContext requirementInfo
        = AvailabilityInference::availableRange(moveOnlyEnqueueRequirement, C);
    AvailabilityContext declInfo =
        TypeChecker::overApproximateAvailabilityAtLocation(nominal->getLoc(), dyn_cast<DeclContext>(nominal));
    canRemoveOldDecls = declInfo.isContainedIn(requirementInfo);
  }

  auto concurrencyModule = C.getLoadedModule(C.Id_Concurrency);
  auto isStdlibDefaultImplDecl = [executorDecl, concurrencyModule](ValueDecl *witness) -> bool {
    if (!witness)
      return false;

    if (auto declContext = witness->getDeclContext()) {
      if (auto *extension = dyn_cast<ExtensionDecl>(declContext)) {
        auto extensionModule = extension->getParentModule();
        if (extensionModule != concurrencyModule) {
          return false;
        }

        if (auto extendedNominal = extension->getExtendedNominal()) {
          return extendedNominal->getDeclaredInterfaceType()->isEqual(
              executorDecl->getDeclaredInterfaceType());
        }
      }
    }
    return false;
  };

  // If both old and new enqueue are implemented, but the old one cannot be removed,
  // emit a warning that the new enqueue is unused.
  if (!canRemoveOldDecls && unownedEnqueueWitnessDecl && moveOnlyEnqueueWitnessDecl) {
    if (!isStdlibDefaultImplDecl(moveOnlyEnqueueWitnessDecl) &&
        !isStdlibDefaultImplDecl(unownedEnqueueWitnessDecl)) {
      diags.diagnose(moveOnlyEnqueueWitnessDecl->getLoc(),
                     diag::executor_enqueue_unused_implementation);
      if (auto decl = unownedEnqueueWitnessDecl) {
         decl->diagnose(diag::decl_declared_here, decl);
      }
    }
  }

  // We specifically do allow the old UnownedJob implementation to be present.
  // In order to ease migration and compatibility for libraries which remain compatible with old Swift versions,
  // and would be getting this warning in situations they cannot address it.

  // Old Job based impl is present, warn about it suggesting the new protocol requirement.
  if (legacyMoveOnlyEnqueueWitnessDecl) {
    if (!isStdlibDefaultImplDecl(legacyMoveOnlyEnqueueWitnessDecl)) {
      diags.diagnose(legacyMoveOnlyEnqueueWitnessDecl->getLoc(),
                     diag::executor_enqueue_deprecated_owned_job_implementation,
                     nominalTy);
    }
  }

  bool unownedEnqueueWitnessIsDefaultImpl = isStdlibDefaultImplDecl(unownedEnqueueWitnessDecl);
  bool moveOnlyEnqueueWitnessIsDefaultImpl = isStdlibDefaultImplDecl(moveOnlyEnqueueWitnessDecl);
  bool legacyMoveOnlyEnqueueWitnessDeclIsDefaultImpl = isStdlibDefaultImplDecl(legacyMoveOnlyEnqueueWitnessDecl);

  auto missingWitness = !unownedEnqueueWitnessDecl &&
                        !moveOnlyEnqueueWitnessDecl &&
                        !legacyMoveOnlyEnqueueWitnessDecl;
  auto allWitnessesAreDefaultImpls = unownedEnqueueWitnessIsDefaultImpl &&
                                     moveOnlyEnqueueWitnessIsDefaultImpl &&
                                     legacyMoveOnlyEnqueueWitnessDeclIsDefaultImpl;
  if ((missingWitness) ||
      (!missingWitness && allWitnessesAreDefaultImpls)) {
    // Neither old nor new implementation have been found, but we provide default impls for them
    // that are mutually recursive, so we must error and suggest implementing the right requirement.
    //
    // If we're running against an SDK that does not have the ExecutorJob enqueue function,
    // try to diagnose using the next-best one available.
    auto missingRequirement = C.getExecutorDecl()->getExecutorOwnedEnqueueFunction();
    if (!missingRequirement)
      missingRequirement = C.getExecutorDecl()->getExecutorLegacyOwnedEnqueueFunction();
    if (!missingRequirement)
      missingRequirement = C.getExecutorDecl()->getExecutorLegacyUnownedEnqueueFunction();

    if (missingRequirement) {
      nominal->diagnose(diag::type_does_not_conform, nominalTy, proto->getDeclaredInterfaceType());
      missingRequirement->diagnose(diag::no_witnesses,
                                   getProtocolRequirementKind(missingRequirement),
                                   missingRequirement,
                                   missingRequirement->getParameters()->get(0)->getInterfaceType(),
                                   /*AddFixIt=*/true);
      return;
    }
  }
}

bool swift::shouldIgnoreDeprecationOfConcurrencyDecl(const Decl *decl,
                                                     DeclContext *declContext) {
  auto &ctx = decl->getASTContext();
  auto concurrencyModule = ctx.getLoadedModule(ctx.Id_Concurrency);

  // Only suppress these diagnostics in the implementation of _Concurrency.
  if (declContext->getParentModule() != concurrencyModule)
    return false;

  // Only suppress deprecation diagnostics for decls defined in _Concurrency.
  if (decl->getDeclContext()->getParentModule() != concurrencyModule)
    return false;

  auto *legacyJobDecl = ctx.getJobDecl();
  auto *unownedJobDecl = ctx.getUnownedJobDecl();

  if (decl == legacyJobDecl)
    return true;

  if (auto *funcDecl = dyn_cast<FuncDecl>(decl)) {
    auto enqueueDeclName =
        DeclName(ctx, DeclBaseName(ctx.Id_enqueue), {Identifier()});

    if (funcDecl->getName() == enqueueDeclName &&
        funcDecl->getParameters()->size() == 1) {
      auto paramTy = funcDecl->getParameters()->front()->getInterfaceType();
      if (paramTy->isEqual(legacyJobDecl->getDeclaredInterfaceType()) ||
          paramTy->isEqual(unownedJobDecl->getDeclaredInterfaceType()))
        return true;
    }
  }

  return false;
}

/// Determine whether this is the main actor type.
static bool isMainActor(Type type) {
  if (auto nominal = type->getAnyNominal())
    return nominal->isMainActor();

  return false;
}

/// If this DeclContext is an actor, or an extension on an actor, return the
/// NominalTypeDecl, otherwise return null.
static NominalTypeDecl *getSelfActorDecl(const DeclContext *dc) {
  auto nominal = dc->getSelfNominalTypeDecl();
  return nominal && nominal->isActor() ? nominal : nullptr;
}

ReferencedActor ReferencedActor::forGlobalActor(VarDecl *actor,
                                                bool isPotentiallyIsolated,
                                                Type globalActor) {
  Kind kind = isMainActor(globalActor) ? MainActor : GlobalActor;
  return ReferencedActor(actor, isPotentiallyIsolated, kind, globalActor);
}

static ActorIsolation getActorIsolationForReference(
    ValueDecl *decl, const DeclContext *fromDC);

bool ReferencedActor::isKnownToBeLocal() const {
  switch (kind) {
  case GlobalActor:
  case AsyncLet:
  case MainActor:
  case NonIsolatedAutoclosure:
  case NonIsolatedContext:
  case NonIsolatedParameter:
  case SendableFunction:
  case SendableClosure:
    if (isPotentiallyIsolated)
      return true;

    return actor && actor->isKnownToBeLocal();

  case Isolated:
    return true;
  }
}

const AbstractFunctionDecl *
swift::isActorInitOrDeInitContext(const DeclContext *dc) {
  while (true) {
    // Stop looking if we hit an isolation inference boundary.
    if (auto *closure = dyn_cast<AbstractClosureExpr>(dc)) {
      if (isIsolationInferenceBoundaryClosure(closure,
                                              false /*is for actor isolation*/))
        return nullptr;

      // Otherwise, look through our closure at the closure's parent decl
      // context.
      dc = dc->getParent();
      continue;
    }

    if (auto func = dyn_cast<AbstractFunctionDecl>(dc)) {
      // If this is an initializer or deinitializer of an actor, we're done.
      if ((isa<ConstructorDecl>(func) || isa<DestructorDecl>(func)) &&
          getSelfActorDecl(dc->getParent()))
        return func;

      // Non-Sendable local functions are considered part of the enclosing
      // context.
      if (func->getDeclContext()->isLocalContext()) {
        if (func->isSendable())
          return nullptr;

        dc = dc->getParent();
        continue;
      }
    }

    return nullptr;
  }
}

static bool isStoredProperty(ValueDecl const *member) {
  if (auto *var = dyn_cast<VarDecl>(member))
    if (var->hasStorage() && var->isInstanceMember())
      return true;
  return false;
}

static bool isNonInheritedStorage(ValueDecl const *member,
                                  DeclContext const *useDC) {
  auto *nominal = useDC->getParent()->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  return isStoredProperty(member) && member->getDeclContext() == nominal;
}

/// Based on the former escaping-use restriction, which was replaced by
/// flow-isolation. We need this to support backwards compatability in the
/// type-checker for programs prior to Swift 6.
/// \param fn either a constructor or destructor of an actor.
static bool wasLegacyEscapingUseRestriction(AbstractFunctionDecl *fn) {
  assert(fn->getDeclContext()->getSelfClassDecl()->isAnyActor());
  assert(isa<ConstructorDecl>(fn) || isa<DestructorDecl>(fn));

  // according to today's isolation, determine whether it use to have the
  // escaping-use restriction
  switch (getActorIsolation(fn).getKind()) {
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
    case ActorIsolation::GlobalActor:
      // convenience inits did not have the restriction.
      if (auto *ctor = dyn_cast<ConstructorDecl>(fn))
        if (ctor->isConvenienceInit())
          return false;

      break; // goto basic case

    case ActorIsolation::ActorInstance:
      // none of these had the restriction affect them.
      assert(fn->hasAsync());
      return false;

    case ActorIsolation::Erased:
      llvm_unreachable("function decl cannot have erased isolation");

    case ActorIsolation::Unspecified:
      // this is basically just objc-marked inits.
      break;
  };

  return !(fn->hasAsync()); // basic case: not async = had restriction.
}

/// Note that the given actor member is isolated.
static void noteIsolatedActorMember(ValueDecl const *decl,
                                    std::optional<VarRefUseEnv> useKind) {
  // detect if it is a distributed actor, to provide better isolation notes

  auto nominal = decl->getDeclContext()->getSelfNominalTypeDecl();
  bool isDistributedActor = false;
  if (nominal) isDistributedActor = nominal->isDistributedActor();

  // FIXME: Make this diagnostic more sensitive to the isolation context of
  // the declaration.
  if (isDistributedActor) {
    if (auto varDecl = dyn_cast<VarDecl>(decl)) {
      if (varDecl->isDistributed()) {
        // This is an attempt to access a `distributed var` synchronously, so offer a more detailed error
        decl->diagnose(diag::distributed_actor_synchronous_access_distributed_computed_property,
                       decl,
                       nominal->getName());
      } else {
        // Distributed actor properties are never accessible externally.
        decl->diagnose(diag::distributed_actor_isolated_property,
                       decl,
                       nominal->getName());
      }

    } else {
      // it's a function or subscript
      decl->diagnose(diag::note_distributed_actor_isolated_method, decl);
    }
  } else if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
    func->diagnose(diag::actor_isolated_sync_func, decl);

    // was it an attempt to mutate an actor instance's isolated state?
  } else if (useKind) {
    if (*useKind == VarRefUseEnv::Read)
      decl->diagnose(diag::kind_declared_here, decl->getDescriptiveKind());
    else
      decl->diagnose(diag::actor_mutable_state, decl->getDescriptiveKind());

  } else {
    decl->diagnose(diag::kind_declared_here, decl->getDescriptiveKind());
  }
}

/// An ad-hoc check specific to member isolation checking. assumed to be
/// queried when a self-member is being accessed in a context which is not
/// isolated to self. The "special permission" is part of a backwards
/// compatability with actor inits and deinits that maintains the
/// permissive nature of the escaping-use restriction, which was only
/// staged in as a warning. See implementation for more details.
///
/// \returns true if this access in the given context should be allowed
/// in Sema, with the side-effect of emitting a warning as needed.
/// If false is returned, then the "special permission" was not granted.
static bool memberAccessHasSpecialPermissionInSwift5(
    DeclContext const *refCxt, ReferencedActor &baseActor,
    ValueDecl const *member, SourceLoc memberLoc,
    std::optional<VarRefUseEnv> useKind) {
  // no need for this in Swift 6+
  if (refCxt->getASTContext().isSwiftVersionAtLeast(6))
    return false;

  // must be an access to an instance member.
  if (!member->isInstanceMember())
    return false;

  // In the history of actor initializers prior to Swift 6, self-isolated
  // members could be referenced from any init or deinit, even a synchronous
  // one, with no diagnostics at all.
  //
  // When the escaping-use restriction came into place for the release of
  // 5.5, it was implemented as a warning and only applied to initializers,
  // which stated that it would become an error in Swift 6.
  //
  // Once 5.6 was released, we also added restrictions in the deinits of
  // actors, at least for accessing members other than stored properties.
  //
  // Later on, for 5.7 we introduced flow-isolation as part of SE-327 for
  // both inits and deinits. This meant that stored property accesses now
  // are only sometimes going to be problematic. This change also brought
  // official changes in isolation for the inits and deinits to handle the
  // the non-stored-property members. Since those isolation changes are
  // currently in place, the purpose of the code below is to override the
  // isolation checking, so that the now-mismatched isolation on member
  // access is still permitted, but with a warning stating that it will
  // be rejected in Swift 6.
  //
  // In the checking below, we let stored-property accesses go ignored,
  // so that flow-isolation can warn about them only if needed. This helps
  // prevent needless warnings on property accesses that will actually be OK
  // with flow-isolation in the future.
  if (auto oldFn = isActorInitOrDeInitContext(refCxt)) {
    auto oldFnMut = const_cast<AbstractFunctionDecl*>(oldFn);

    // If function did not have the escaping-use restriction, then it gets
    // no special permissions here.
    if (!wasLegacyEscapingUseRestriction(oldFnMut))
      return false;

    // At this point, the special permission will be granted. But, we
    // need to warn now about this permission being taken away in Swift 6
    // for specific kinds of non-stored-property member accesses:

    // If the context in which we consider the access matches between the
    // old (escaping-use restriction) and new (flow-isolation) contexts,
    // and it is a stored or init accessor property, then permit it here
    // without any warning.
    // Later, flow-isolation pass will check and emit a warning if needed.
    if (refCxt == oldFn) {
      if (isStoredProperty(member))
        return true;

      if (auto *var = dyn_cast<VarDecl>(member)) {
        // Init accessor properties are permitted to access only stored
        // properties.
        if (var->hasInitAccessor())
          return true;
      }
    }

    // Otherwise, it's definitely going to be illegal, so warn and permit.
    auto &diags = refCxt->getASTContext().Diags;
    auto useKindInt = static_cast<unsigned>(
        useKind.value_or(VarRefUseEnv::Read));

    diags.diagnose(
        memberLoc, diag::actor_isolated_non_self_reference,
        member,
        useKindInt,
        baseActor.kind + 1,
        baseActor.globalActor,
        getActorIsolation(const_cast<ValueDecl *>(member)))
    .warnUntilSwiftVersion(6);

    noteIsolatedActorMember(member, useKind);
    return true;
  }

  return false;
}

/// To support flow-isolation, some member accesses in inits / deinits
/// must be permitted, despite the isolation of 'self' not being
/// correct in Sema.
///
/// \param refCxt the context in which the member reference happens.
/// \param baseActor the actor referenced in the base of the member access.
/// \param member the declaration corresponding to the accessed member.
/// \param memberLoc the source location of the reference to the member.
///
/// \returns true iff the member access is permitted in Sema because it will
/// be verified later by flow-isolation.
static bool checkedByFlowIsolation(DeclContext const *refCxt,
                                   ReferencedActor &baseActor,
                                   ValueDecl const *member, SourceLoc memberLoc,
                                   std::optional<VarRefUseEnv> useKind) {

  // base of member reference must be `self`
  if (!baseActor.isSelf())
    return false;

  // Must be directly in an init/deinit that uses flow-isolation,
  // or a defer within such a functions.
  //
  // NOTE: once flow-isolation can analyze calls to arbitrary local
  // functions, we should be using isActorInitOrDeInitContext instead
  // of this ugly loop.
  AbstractFunctionDecl const* fnDecl = nullptr;
  while (true) {
    fnDecl = dyn_cast_or_null<AbstractFunctionDecl>(refCxt->getAsDecl());
    if (!fnDecl)
      break;

    // go up one level if this context is a defer.
    if (auto *d = dyn_cast<FuncDecl>(fnDecl)) {
      if (d->isDeferBody()) {
        refCxt = refCxt->getParent();
        continue;
      }
    }
    break;
  }

  if (memberAccessHasSpecialPermissionInSwift5(refCxt, baseActor, member,
                                               memberLoc, useKind))
    return true; // then permit it now.

  if (!usesFlowSensitiveIsolation(fnDecl))
    return false;

  // Stored properties are definitely OK.
  if (isNonInheritedStorage(member, fnDecl))
    return true;

  return false;
}

/// Get the actor isolation of the innermost relevant context.
static ActorIsolation getInnermostIsolatedContext(
    const DeclContext *dc,
    llvm::function_ref<ActorIsolation(AbstractClosureExpr *)>
        getClosureActorIsolation) {
  // Retrieve the actor isolation of the context.
  auto mutableDC = const_cast<DeclContext *>(dc);
  switch (auto isolation =
              getActorIsolationOfContext(mutableDC, getClosureActorIsolation)) {
  case ActorIsolation::ActorInstance:
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe:
  case ActorIsolation::Unspecified:
    return isolation;

  case ActorIsolation::Erased:
    llvm_unreachable("closure cannot originally have dynamic isolation");

  case ActorIsolation::GlobalActor:
    return ActorIsolation::forGlobalActor(
        dc->mapTypeIntoContext(isolation.getGlobalActor()))
          .withPreconcurrency(isolation.preconcurrency());
  }
}

/// Determine whether this declaration is always accessed asynchronously.
bool swift::isAsyncDecl(ConcreteDeclRef declRef) {
  auto decl = declRef.getDecl();

  // An async function is asynchronously accessed.
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl))
    return func->hasAsync();

  // A computed property or subscript that has an 'async' getter
  // is asynchronously accessed.
  if (auto storageDecl = dyn_cast<AbstractStorageDecl>(decl)) {
    if (auto effectfulGetter = storageDecl->getEffectfulGetAccessor())
      return effectfulGetter->hasAsync();
  }

  return false;
}

/// Check if it is safe for the \c globalActor qualifier to be removed from
/// \c ty, when the function value of that type is isolated to that actor.
///
/// In general this is safe in a narrow but common case: a global actor
/// qualifier can be dropped from a function type while in a DeclContext
/// isolated to that same actor, as long as the value is not Sendable.
///
/// \param dc the innermost context in which the cast to remove the global actor
///           is happening.
/// \param globalActor global actor that was dropped from \c ty.
/// \param ty a function type where \c globalActor was removed from it.
/// \return true if it is safe to drop the global-actor qualifier.
static bool safeToDropGlobalActor(
    DeclContext *dc, Type globalActor, Type ty, ApplyExpr *call) {
  auto funcTy = ty->getAs<AnyFunctionType>();
  if (!funcTy)
    return false;

  auto otherIsolation = funcTy->getIsolation();

  // can't add a different global actor
  if (otherIsolation.isGlobalActor()) {
    assert(otherIsolation.getGlobalActorType()->getCanonicalType()
             != globalActor->getCanonicalType()
           && "not even dropping the actor?");
    return false;
  }

  // Converting to an isolation-erased function type is fine.
  if (otherIsolation.isErased())
    return true;

  // We currently allow unconditional dropping of global actors from
  // async function types, despite this confusing Sendable checking
  // in light of SE-338.
  if (funcTy->isAsync())
    return true;

  // If the argument is passed over an isolation boundary, it's not
  // safe to erase actor isolation, because the callee can call the
  // function synchronously from outside the isolation domain.
  if (call && call->getIsolationCrossing())
    return false;

  // fundamentally cannot be sendable if we want to drop isolation info
  if (funcTy->isSendable())
    return false;

  // finally, must be in a context with matching isolation.
  auto dcIsolation = getActorIsolationOfContext(dc);
  if (dcIsolation.isGlobalActor())
    if (dcIsolation.getGlobalActor()->getCanonicalType()
        == globalActor->getCanonicalType())
      return true;

  return false;
}

static FuncDecl *findAnnotatableFunction(DeclContext *dc) {
  auto fn = dyn_cast<FuncDecl>(dc);
  if (!fn) return nullptr;
  if (fn->isDeferBody())
    return findAnnotatableFunction(fn->getDeclContext());
  return fn;
}

static bool shouldCheckSendable(Expr *expr) {
  if (auto *declRef = dyn_cast<DeclRefExpr>(expr->findOriginalValue())) {
    auto isolation = getActorIsolation(declRef->getDecl());
    return isolation != ActorIsolation::NonisolatedUnsafe;
  }

  return true;
}

bool swift::diagnoseApplyArgSendability(ApplyExpr *apply, const DeclContext *declContext) {
  auto isolationCrossing = apply->getIsolationCrossing();
  if (!isolationCrossing.has_value())
    return false;

  auto fnExprType = apply->getFn()->getType();
  if (!fnExprType)
    return false;

  // Check the 'self' argument.
  if (auto *selfApply = dyn_cast<SelfApplyExpr>(apply->getFn())) {
    auto *base = selfApply->getBase();

    if (shouldCheckSendable(base) &&
        diagnoseNonSendableTypes(
            base->getType(),
            declContext, /*inDerivedConformance*/Type(),
            base->getStartLoc(),
            diag::non_sendable_call_argument,
            isolationCrossing.value().exitsIsolation(),
            isolationCrossing.value().getDiagnoseIsolation()))
      return true;
  }
  auto fnType = fnExprType->getAs<FunctionType>();
  if (!fnType)
    return false;

  auto params = fnType->getParams();
  for (unsigned paramIdx : indices(params)) {
    const auto &param = params[paramIdx];

    // Dig out the location of the argument.
    SourceLoc argLoc = apply->getLoc();
    Type argType;
    bool checkSendable = true;
    if (auto argList = apply->getArgs()) {
      auto arg = argList->get(paramIdx);
      if (arg.getStartLoc().isValid())
          argLoc = arg.getStartLoc();

      // Determine the type of the argument, ignoring any implicit
      // conversions that could have stripped sendability.
      if (Expr *argExpr = arg.getExpr()) {
        checkSendable = shouldCheckSendable(argExpr);
        argType = argExpr->findOriginalType();

        // If this is a default argument expression, don't check Sendability
        // if the argument is evaluated in the callee's isolation domain.
        if (auto *defaultExpr = dyn_cast<DefaultArgumentExpr>(argExpr)) {
          auto argIsolation = defaultExpr->getRequiredIsolation();
          auto calleeIsolation = isolationCrossing->getCalleeIsolation();
          if (argIsolation == calleeIsolation) {
            continue;
          }
        }
      }
    }

    if (checkSendable &&
        diagnoseNonSendableTypes(
            argType ? argType : param.getParameterType(),
            declContext, /*inDerivedConformance*/Type(),
            argLoc, diag::non_sendable_call_argument,
            isolationCrossing.value().exitsIsolation(),
            isolationCrossing.value().getDiagnoseIsolation()))
      return true;
  }
  return false;
}

namespace {
  /// Check for adherence to the actor isolation rules, emitting errors
  /// when actor-isolated declarations are used in an unsafe manner.
  class ActorIsolationChecker : public ASTWalker {
    ASTContext &ctx;
    SmallVector<const DeclContext *, 4> contextStack;
    SmallVector<llvm::PointerUnion<ApplyExpr *, LookupExpr *>, 4> applyStack;
    SmallVector<std::pair<OpaqueValueExpr *, Expr *>, 4> opaqueValues;
    SmallVector<const PatternBindingDecl *, 2> patternBindingStack;
    llvm::function_ref<Type(Expr *)> getType;
    llvm::function_ref<ActorIsolation(AbstractClosureExpr *)>
        getClosureActorIsolation;
    /// Whether to check if the closure captures an `isolated` parameter.
    /// This is needed as a workaround during code completion, which doesn't
    /// have types applied to the AST and thus doesn't have captures computed.
    bool checkIsolatedCapture;

    SourceLoc requiredIsolationLoc;

    /// Used under the mode to compute required actor isolation for
    /// an expression or function.
    llvm::SmallDenseMap<const DeclContext *, ActorIsolation> requiredIsolation;

    using ActorRefKindPair = std::pair<ReferencedActor::Kind, ActorIsolation>;

    using IsolationPair = std::pair<ActorIsolation, ActorIsolation>;

    using DiagnosticList = std::vector<IsolationError>;

    llvm::DenseMap<ActorRefKindPair, DiagnosticList> refErrors;

    llvm::DenseMap<IsolationPair, DiagnosticList> applyErrors;

    /// Keeps track of the capture context of variables that have been
    /// explicitly captured in closures.
    llvm::SmallDenseMap<VarDecl *, TinyPtrVector<const DeclContext *>>
      captureContexts;

    using MutableVarSource
        = llvm::PointerUnion<DeclRefExpr *, InOutExpr *, LookupExpr *>;

    using MutableVarParent
        = llvm::PointerUnion<InOutExpr *, LoadExpr *, AssignExpr *>;

    ApplyExpr *getImmediateApply() const {
      if (applyStack.empty())
        return nullptr;

      return applyStack.back().dyn_cast<ApplyExpr *>();
    }

    /// Note when the enclosing context could be put on a global actor.
    // FIXME: This should handle closures too.
    static bool missingGlobalActorOnContext(DeclContext *dc, Type globalActor,
                                            DiagnosticBehavior behavior) {
      // If we are in a synchronous function on the global actor,
      // suggest annotating with the global actor itself.
      if (auto fn = findAnnotatableFunction(dc)) {
        // Suppress this for accessors because you can't change the
        // actor isolation of an individual accessor.  Arguably we could
        // add this to the entire storage declaration, though.
        // Suppress this for async functions out of caution; but don't
        // suppress it if we looked through a defer.
        if (!isa<AccessorDecl>(fn) &&
            (!fn->isAsyncContext() || fn != dc)) {
          switch (getActorIsolation(fn)) {
          case ActorIsolation::ActorInstance:
          case ActorIsolation::GlobalActor:
          case ActorIsolation::Nonisolated:
          case ActorIsolation::NonisolatedUnsafe:
              return false;

          case ActorIsolation::Erased:
            llvm_unreachable("function cannot have erased isolation");

          case ActorIsolation::Unspecified:
            if (behavior != DiagnosticBehavior::Note) {
              fn->diagnose(diag::invalid_isolated_calls_in_body,
                           globalActor->getWithoutParens().getString(), fn)
                  .limitBehaviorUntilSwiftVersion(behavior, 6);
            }

            // Overrides cannot be isolated to a global actor; the isolation
            // must match the overridden decl.
            if (fn->getOverriddenDecl())
              return false;

            fn->diagnose(diag::add_globalactor_to_function,
                         globalActor->getWithoutParens().getString(),
                         fn, globalActor)
                .fixItInsert(fn->getAttributeInsertionLoc(false),
                             diag::insert_globalactor_attr, globalActor);
            return true;
          }
        }
      }
      return false;
    }

  public:
    bool diagnoseIsolationErrors() {
      bool diagnosedError = false;

      for (auto list : refErrors) {
        ActorRefKindPair key = list.getFirst();
        DiagnosticList errors = list.getSecond();
        ActorIsolation isolation = key.second;

        auto behavior = DiagnosticBehavior::Warning;
        // Upgrade behavior if @preconcurrency not detected
        if (llvm::any_of(errors, [&](IsolationError error) {
          return !error.preconcurrency;
        })) {
          behavior = DiagnosticBehavior::Error;
        }

        // Add Fix-it for missing @SomeActor annotation
        if (isolation.isGlobalActor()) {
          if (missingGlobalActorOnContext(
                  const_cast<DeclContext *>(getDeclContext()),
                  isolation.getGlobalActor(), behavior) &&
              errors.size() > 1) {
            behavior = DiagnosticBehavior::Note;
          }
        }

        for (IsolationError error : errors) {
          // Diagnose actor_isolated_non_self_reference as note
          // if there are multiple of these diagnostics
          ctx.Diags.diagnose(error.loc, error.diag)
            .limitBehaviorUntilSwiftVersion(behavior, 6);
        }
      }

      for (auto list : applyErrors) {
        IsolationPair key = list.getFirst();
        DiagnosticList errors = list.getSecond();
        ActorIsolation isolation = key.first;

        auto behavior = DiagnosticBehavior::Warning;
        // Upgrade behavior if @preconcurrency not detected
        if (llvm::any_of(errors, [&](IsolationError error) {
          return !error.preconcurrency;
        })) {
          behavior = DiagnosticBehavior::Error;
        }


        // Add Fix-it for missing @SomeActor annotation
        if (isolation.isGlobalActor()) {
          if (missingGlobalActorOnContext(
                  const_cast<DeclContext *>(getDeclContext()),
                  isolation.getGlobalActor(), behavior) &&
              errors.size() > 1) {
            behavior = DiagnosticBehavior::Note;
          }
        }

        for (IsolationError error : errors) {
          // Diagnose actor_isolated_call as note if
          // if there are multiple actor-isolated function calls
          // from outside the actor
          ctx.Diags.diagnose(error.loc, error.diag)
            .limitBehaviorUntilSwiftVersion(behavior, 6);
        }
      }

      return diagnosedError;
    }

  private:
    const PatternBindingDecl *getTopPatternBindingDecl() const {
      return patternBindingStack.empty() ? nullptr : patternBindingStack.back();
    }

    /// Mapping from mutable variable reference exprs, or inout expressions,
    /// to the parent expression, when that parent is either a load or
    /// an inout expr.
    llvm::SmallDenseMap<MutableVarSource, MutableVarParent, 4>
      mutableLocalVarParent;

    static bool isPropOrSubscript(ValueDecl const* decl) {
      return isa<VarDecl>(decl) || isa<SubscriptDecl>(decl);
    }

    /// In the given expression \c use that refers to the decl, this
    /// function finds the kind of environment tracked by
    /// \c mutableLocalVarParent that corresponds to that \c use.
    ///
    /// Note that an InoutExpr is not considered a use of the decl!
    ///
    /// @returns None if the context expression is either an InOutExpr,
    ///               not tracked, or if the decl is not a property or subscript
    std::optional<VarRefUseEnv> kindOfUsage(ValueDecl const *decl,
                                            Expr *use) const {
      // we need a use for lookup.
      if (!use)
        return std::nullopt;

      // must be a property or subscript
      if (!isPropOrSubscript(decl))
        return std::nullopt;

      if (auto lookup = dyn_cast<DeclRefExpr>(use))
        return usageEnv(lookup);
      else if (auto lookup = dyn_cast<LookupExpr>(use))
        return usageEnv(lookup);

      return std::nullopt;
    }

    /// @returns the kind of environment in which this expression appears, as
    ///          tracked by \c mutableLocalVarParent
    VarRefUseEnv usageEnv(MutableVarSource src) const {
      auto result = mutableLocalVarParent.find(src);
      if (result != mutableLocalVarParent.end()) {
        MutableVarParent parent = result->second;
        assert(!parent.isNull());
        if (parent.is<LoadExpr*>())
          return VarRefUseEnv::Read;
        else if (parent.is<AssignExpr*>())
          return VarRefUseEnv::Mutating;
        else if (auto inout = parent.dyn_cast<InOutExpr*>())
          return inout->isImplicit() ? VarRefUseEnv::Mutating
                                     : VarRefUseEnv::Inout;
        else
          llvm_unreachable("non-exhaustive case match");
      }
      return VarRefUseEnv::Read; // assume if it's not tracked, it's only read.
    }

    const DeclContext *getDeclContext() const {
      return contextStack.back();
    }

    ModuleDecl *getParentModule() const {
      return getDeclContext()->getParentModule();
    }

    /// Determine whether code in the given use context might execute
    /// concurrently with code in the definition context.
    bool mayExecuteConcurrentlyWith(
        const DeclContext *useContext, const DeclContext *defContext);

    /// If the subexpression is a reference to a mutable local variable from a
    /// different context, record its parent. We'll query this as part of
    /// capture semantics in concurrent functions.
    ///
    /// \returns true if we recorded anything, false otherwise.
    bool recordMutableVarParent(MutableVarParent parent, Expr *subExpr) {
      subExpr = subExpr->getValueProvidingExpr();

      if (auto declRef = dyn_cast<DeclRefExpr>(subExpr)) {
        auto var = dyn_cast_or_null<VarDecl>(declRef->getDecl());
        if (!var)
          return false;

        // Only mutable variables matter.
        if (!var->supportsMutation())
          return false;

        // Only mutable variables outside of the current context. This is an
        // optimization, because the parent map won't be queried in this case,
        // and it is the most common case for variables to be referenced in
        // their own context.
        if (var->getDeclContext() == getDeclContext())
          return false;

        assert(mutableLocalVarParent[declRef].isNull());
        mutableLocalVarParent[declRef] = parent;
        return true;
      }

      // For a member reference, try to record a parent for the base expression.
      if (auto memberRef = dyn_cast<MemberRefExpr>(subExpr)) {
        // Record the parent of this LookupExpr too.
        mutableLocalVarParent[memberRef] = parent;
        return recordMutableVarParent(parent, memberRef->getBase());
      }

      // For a subscript, try to record a parent for the base expression.
      if (auto subscript = dyn_cast<SubscriptExpr>(subExpr)) {
        // Record the parent of this LookupExpr too.
        mutableLocalVarParent[subscript] = parent;
        return recordMutableVarParent(parent, subscript->getBase());
      }

      // Look through postfix '!'.
      if (auto force = dyn_cast<ForceValueExpr>(subExpr)) {
        return recordMutableVarParent(parent, force->getSubExpr());
      }

      // Look through postfix '?'.
      if (auto bindOpt = dyn_cast<BindOptionalExpr>(subExpr)) {
        return recordMutableVarParent(parent, bindOpt->getSubExpr());
      }

      if (auto optEval = dyn_cast<OptionalEvaluationExpr>(subExpr)) {
        return recordMutableVarParent(parent, optEval->getSubExpr());
      }

      // & expressions can be embedded for references to mutable variables
      // or subscribes inside a struct/enum.
      if (auto inout = dyn_cast<InOutExpr>(subExpr)) {
        // Record the parent of the inout so we don't look at it again later.
        mutableLocalVarParent[inout] = parent;
        return recordMutableVarParent(parent, inout->getSubExpr());
      }

      // Look through an expression that opens an existential
      if (auto openExist = dyn_cast<OpenExistentialExpr>(subExpr)) {
        return recordMutableVarParent(parent, openExist->getSubExpr());
      }

      return false;
    }

    /// Some function conversions synthesized by the constraint solver may not
    /// be correct AND the solver doesn't know, so we must emit a diagnostic.
    void checkFunctionConversion(Expr *funcConv, Type fromType, Type toType) {
      if (auto fromFnType = fromType->getAs<FunctionType>()) {
        if (auto fromActor = fromFnType->getGlobalActor()) {
          if (auto toFnType = toType->getAs<FunctionType>()) {

            // ignore some kinds of casts, as they're diagnosed elsewhere.
            if (toFnType->hasGlobalActor() || toFnType->isAsync())
              return;

            auto dc = const_cast<DeclContext*>(getDeclContext());
            if (!safeToDropGlobalActor(dc, fromActor, toType,
                                       getImmediateApply())) {
              // otherwise, it's not a safe cast.
              dc->getASTContext()
                  .Diags
                  .diagnose(funcConv->getLoc(),
                            diag::converting_func_loses_global_actor, fromType,
                            toType, fromActor)
                  .warnUntilSwiftVersion(6);
            }
          }
        }
      }
    }

    bool refineRequiredIsolation(ActorIsolation refinedIsolation) {
      if (requiredIsolationLoc.isInvalid())
        return false;

      auto infersIsolationFromContext =
          [](const DeclContext *dc) -> bool {
            // Isolation for declarations is based solely on explicit 
            // annotations; only infer isolation for initializer expressions
            // and closures.
            if (dc->getAsDecl())
              return false;

            if (auto *closure = dyn_cast<AbstractClosureExpr>(dc)) {
              // We cannot infer a more specific actor isolation for a Sendable
              // closure. It is an error to cast away actor isolation from a
              // function type, but this is okay for non-Sendable closures
              // because they cannot leave the isolation domain they're created
              // in anyway.
              if (closure->isSendable())
                return false;

              if (closure->getActorIsolation().isActorIsolated())
                return false;
            }

            return true;
          };

      // For the call to require the given actor isolation, every DeclContext
      // in the current context stack must require the same isolation. If
      // along the way to the innermost context, we find a DeclContext that
      // has a different isolation (e.g. it's a local function that does not
      // receive isolation from its decl context), then the expression cannot
      // require a different isolation.
      for (auto *dc : contextStack) {
        if (!infersIsolationFromContext(dc)) {
          requiredIsolation.clear();
          return false;
        }

        // To refine the required isolation, the existing requirement
        // must either be 'nonisolated' or exactly the same as the
        // new refinement.
        auto isolation = requiredIsolation.find(dc);
        if (isolation == requiredIsolation.end() ||
            isolation->second == ActorIsolation::Nonisolated) {
          requiredIsolation[dc] = refinedIsolation;
        } else if (isolation->second != refinedIsolation) {
          dc->getASTContext().Diags.diagnose(
              requiredIsolationLoc,
              diag::conflicting_default_argument_isolation,
              isolation->second, refinedIsolation);
          requiredIsolation.clear();
          return true;
        }
      }

      return true;
    }

    void checkDefaultArgument(DefaultArgumentExpr *expr) {
      getCurrentContextIsolation(expr);

      // Check the context isolation against the required isolation for
      // evaluating the default argument synchronously. If the default
      // argument must be evaluated asynchronously, record that in the
      // expression node.
      auto requiredIsolation = expr->getRequiredIsolation();
      auto contextIsolation = getInnermostIsolatedContext(
          getDeclContext(), getClosureActorIsolation);

      if (requiredIsolation == contextIsolation)
        return;

      switch (requiredIsolation) {
      // Nonisolated is okay from any caller isolation because
      // default arguments cannot have any async calls.
      case ActorIsolation::Unspecified:
      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
        return;

      case ActorIsolation::Erased:
      case ActorIsolation::GlobalActor:
      case ActorIsolation::ActorInstance:
        break;
      }

      expr->setImplicitlyAsync();
    }

    /// Check closure captures for Sendable violations.
    void checkLocalCaptures(AnyFunctionRef localFunc) {
      for (const auto &capture : localFunc.getCaptureInfo().getCaptures()) {
        if (!capture.isLocalCapture())
          continue;
        if (capture.isDynamicSelfMetadata())
          continue;
        if (capture.isOpaqueValue())
          continue;

        // If the closure won't execute concurrently with the context in
        // which the declaration occurred, it's okay.
        auto decl = capture.getDecl();
        auto isolation = getActorIsolation(decl);

        // 'nonisolated' local variables are always okay to capture in
        // 'Sendable' closures because they can be accessed from anywhere.
        // Note that only 'nonisolated(unsafe)' can be applied to local
        // variables.
        if (isolation.isNonisolated())
          continue;

        auto *context = localFunc.getAsDeclContext();
        auto fnType = localFunc.getType()->getAs<AnyFunctionType>();
        if (!mayExecuteConcurrentlyWith(context, decl->getDeclContext()))
          continue;

        Type type = getDeclContext()
            ->mapTypeIntoContext(decl->getInterfaceType())
            ->getReferenceStorageReferent();

        if (type->hasError())
          continue;

        auto *closure = localFunc.getAbstractClosureExpr();
        if (closure && closure->isImplicit()) {
          auto *patternBindingDecl = getTopPatternBindingDecl();
          if (patternBindingDecl && patternBindingDecl->isAsyncLet()) {
            // Defer diagnosing checking of non-Sendable types that are passed
            // into async let to SIL level region based isolation if we have
            // region based isolation enabled.
            //
            // We purposely do not do this for SendingArgsAndResults since that
            // just triggers the actual ability to represent
            // 'sending'. RegionBasedIsolation is what determines if we get the
            // differing semantics.
            if (!ctx.LangOpts.hasFeature(Feature::RegionBasedIsolation)) {
              diagnoseNonSendableTypes(
                  type, getDeclContext(),
                  /*inDerivedConformance*/Type(), capture.getLoc(),
                  diag::implicit_async_let_non_sendable_capture,
                  decl->getName());
            }
          } else {
            // Fallback to a generic implicit capture missing sendable
            // conformance diagnostic.
            diagnoseNonSendableTypes(type, getDeclContext(),
                                     /*inDerivedConformance*/Type(),
                                     capture.getLoc(),
                                     diag::implicit_non_sendable_capture,
                                     decl->getName());
          }
        } else if (fnType->isSendable()) {
          diagnoseNonSendableTypes(type, getDeclContext(),
                                   /*inDerivedConformance*/Type(),
                                   capture.getLoc(),
                                   diag::non_sendable_capture,
                                   decl->getName(),
                                   /*closure=*/closure != nullptr);
        } else {
          diagnoseNonSendableTypes(type, getDeclContext(),
                                   /*inDerivedConformance*/Type(),
                                   capture.getLoc(),
                                   diag::non_sendable_isolated_capture,
                                   decl->getName(),
                                   /*closure=*/closure != nullptr);
        }
      }
    }

  public:
    ActorIsolationChecker(
        const DeclContext *dc,
        llvm::function_ref<Type(Expr *)> getType = __Expr_getType,
        llvm::function_ref<ActorIsolation(AbstractClosureExpr *)>
            getClosureActorIsolation = __AbstractClosureExpr_getActorIsolation,
        bool checkIsolatedCapture = true)
        : ctx(dc->getASTContext()), getType(getType),
          getClosureActorIsolation(getClosureActorIsolation),
          checkIsolatedCapture(checkIsolatedCapture) {
      contextStack.push_back(dc);
    }

    ActorIsolation computeRequiredIsolation(Expr *expr) {
      auto &ctx = getDeclContext()->getASTContext();

      if (ctx.LangOpts.hasFeature(Feature::IsolatedDefaultValues))
        requiredIsolationLoc = expr->getLoc();

      expr->walk(*this);
      requiredIsolationLoc = SourceLoc();
      return requiredIsolation[getDeclContext()];
    }

    /// Searches the applyStack from back to front for the inner-most CallExpr
    /// and marks that CallExpr as implicitly async.
    ///
    /// NOTE: Crashes if no CallExpr was found.
    ///
    /// For example, for global actor function `curryAdd`, if we have:
    ///     ((curryAdd 1) 2)
    /// then we want to mark the inner-most CallExpr, `(curryAdd 1)`.
    ///
    /// The same goes for calls to member functions, such as calc.add(1, 2),
    /// aka ((add calc) 1 2), looks like this:
    ///
    ///  (call_expr
    ///    (dot_syntax_call_expr
    ///      (declref_expr add)
    ///      (declref_expr calc))
    ///    (tuple_expr
    ///      ...))
    ///
    /// and we reach up to mark the CallExpr.
    void markNearestCallAsImplicitly(std::optional<ActorIsolation> setAsync,
                                     bool setThrows = false,
                                     bool setDistributedThunk = false) {
      assert(applyStack.size() > 0 && "not contained within an Apply?");

      const auto End = applyStack.rend();
      for (auto I = applyStack.rbegin(); I != End; ++I)
        if (auto call = dyn_cast<CallExpr>(I->dyn_cast<ApplyExpr *>())) {
          if (setAsync) {
            call->setImplicitlyAsync(*setAsync);
          }
          if (setThrows) {
            call->setImplicitlyThrows(true);
          }else {
            call->setImplicitlyThrows(false);
          }
          if (setDistributedThunk) {
            call->setShouldApplyDistributedThunk(true);
          }
          return;
        }
      llvm_unreachable("expected a CallExpr in applyStack!");
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *pattern) override {
      // Walking into patterns leads to nothing good because then we
      // end up visiting the AccessorDecls of a top-level
      // PatternBindingDecl twice.
      return Action::SkipNode(pattern);
    }

    PreWalkAction walkToDeclPre(Decl *decl) override {
      // Don't walk into local types because nothing in them can
      // change the outcome of our analysis, and we don't want to
      // assume things there have been type checked yet.
      if (isa<TypeDecl>(decl)) {
        return Action::SkipChildren();
      }

      if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
        if (func->getDeclContext()->isLocalContext()) {
          checkLocalCaptures(func);
        }

        contextStack.push_back(func);
      }

      if (auto *PBD = dyn_cast<PatternBindingDecl>(decl)) {
        patternBindingStack.push_back(PBD);
      }

      return Action::Continue();
    }

    PostWalkAction walkToDeclPost(Decl *decl) override {
      if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
        assert(contextStack.back() == func);
        contextStack.pop_back();
      }

      if (auto *PBD = dyn_cast<PatternBindingDecl>(decl)) {
        assert(patternBindingStack.back() == PBD);
        patternBindingStack.pop_back();
      }

      return Action::Continue();
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
      // Skip expressions that didn't make it to solution application
      // because the constraint system diagnosed an error.
      if (!expr->getType() || expr->getType()->hasError())
        return Action::SkipNode(expr);

      if (auto *openExistential = dyn_cast<OpenExistentialExpr>(expr)) {
        opaqueValues.push_back({
            openExistential->getOpaqueValue(),
            openExistential->getExistentialValue()});
        return Action::Continue(expr);
      }

      if (auto *closure = dyn_cast<AbstractClosureExpr>(expr)) {
        closure->setActorIsolation(determineClosureIsolation(closure));
        checkLocalCaptures(closure);
        contextStack.push_back(closure);
        return Action::Continue(expr);
      }

      if (auto inout = dyn_cast<InOutExpr>(expr)) {
        if (!applyStack.empty())
          diagnoseInOutArg(applyStack.back(), inout, false);

        if (mutableLocalVarParent.count(inout) == 0)
          recordMutableVarParent(inout, inout->getSubExpr());
      }

      if (auto assign = dyn_cast<AssignExpr>(expr)) {
        // mark vars in the destination expr as being part of the Assign.
        if (auto destExpr = assign->getDest())
          recordMutableVarParent(assign, destExpr);

        return Action::Continue(expr);
      }

      if (auto load = dyn_cast<LoadExpr>(expr))
        recordMutableVarParent(load, load->getSubExpr());

      if (auto lookup = dyn_cast<LookupExpr>(expr)) {
        applyStack.push_back(lookup);
        checkReference(lookup->getBase(), lookup->getMember(), lookup->getLoc(),
                       /*partialApply*/ std::nullopt, lookup);
        return Action::Continue(expr);
      }

      if (auto declRef = dyn_cast<DeclRefExpr>(expr)) {
        auto valueRef = declRef->getDeclRef();
        auto value = valueRef.getDecl();
        auto loc = declRef->getLoc();

        // FIXME: Should this be subsumed in reference checking?
        if (value->isLocalCapture())
          checkLocalCapture(valueRef, loc, declRef);
        else
          checkReference(nullptr, valueRef, loc, std::nullopt, declRef);
        return Action::Continue(expr);
      }

      if (auto apply = dyn_cast<ApplyExpr>(expr)) {
        // If this is a call to a partial apply thunk, decompose it to check it
        // like based on the original written syntax, e.g., "self.method".
        if (auto partialApply = decomposePartialApplyThunk(
                apply, Parent.getAsExpr())) {
          if (auto memberRef = findReference(partialApply->fn)) {
            // NOTE: partially-applied thunks are never annotated as
            // implicitly async, regardless of whether they are escaping.
            checkReference(
                partialApply->base, memberRef->first, memberRef->second,
                partialApply);

            partialApply->base->walk(*this);

            return Action::SkipNode(expr);
          }
        }

        applyStack.push_back(apply);  // record this encounter

        if (isa<SelfApplyExpr>(apply)) {
          // Self applications are checked as part of the outer call.
          // However, we look for inout issues here.
          if (applyStack.size() >= 2) {
            auto outerCall = applyStack[applyStack.size() - 2];
            if (isAsyncCall(outerCall)) {
              // This call is a partial application within an async call.
              // If the partial application take a value inout, it is bad.
              if (InOutExpr *inoutArg = dyn_cast<InOutExpr>(
                     apply->getArgs()->getExpr(0)->getSemanticsProvidingExpr()))
                diagnoseInOutArg(outerCall, inoutArg, true);
            }
          }
        } else {
          // Check the call itself.
          (void)checkApply(apply);
        }
      }

      if (auto keyPath = dyn_cast<KeyPathExpr>(expr))
        checkKeyPathExpr(keyPath);

      // The children of #selector expressions are not evaluated, so we do not
      // need to do isolation checking there. This is convenient because such
      // expressions tend to violate restrictions on the use of instance
      // methods.
      if (isa<ObjCSelectorExpr>(expr))
        return Action::SkipNode(expr);

      // Track the capture contexts for variables.
      if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
        auto *closure = captureList->getClosureBody();
        for (const auto &entry : captureList->getCaptureList()) {
          captureContexts[entry.getVar()].push_back(closure);
        }
      }

      // The constraint solver may not have chosen legal casts.
      if (auto funcConv = dyn_cast<FunctionConversionExpr>(expr)) {
        checkFunctionConversion(funcConv,
                                funcConv->getSubExpr()->getType(),
                                funcConv->getType());
      }

      if (auto *isolationErasure = dyn_cast<ActorIsolationErasureExpr>(expr)) {
        checkFunctionConversion(isolationErasure,
                                isolationErasure->getSubExpr()->getType(),
                                isolationErasure->getType());
      }

      if (auto *defaultArg = dyn_cast<DefaultArgumentExpr>(expr)) {
        checkDefaultArgument(defaultArg);
      }

      return Action::Continue(expr);
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *expr) override {
      if (auto *openExistential = dyn_cast<OpenExistentialExpr>(expr)) {
        assert(opaqueValues.back().first == openExistential->getOpaqueValue());
        opaqueValues.pop_back();
        return Action::Continue(expr);
      }

      if (auto *closure = dyn_cast<AbstractClosureExpr>(expr)) {
        assert(contextStack.back() == closure);
        contextStack.pop_back();
      }

      if (auto *apply = dyn_cast<ApplyExpr>(expr)) {
        assert(applyStack.back().get<ApplyExpr *>() == apply);
        applyStack.pop_back();
      }

      // Clear out the mutable local variable parent map on the way out.
      if (auto *declRefExpr = dyn_cast<DeclRefExpr>(expr)) {
        mutableLocalVarParent.erase(declRefExpr);
      } else if (auto *lookupExpr = dyn_cast<LookupExpr>(expr)) {
        mutableLocalVarParent.erase(lookupExpr);

        assert(applyStack.back().dyn_cast<LookupExpr *>() == lookupExpr);
        applyStack.pop_back();
      } else if (auto *inoutExpr = dyn_cast<InOutExpr>(expr)) {
        mutableLocalVarParent.erase(inoutExpr);
      }

      // Remove the tracked capture contexts.
      if (auto captureList = dyn_cast<CaptureListExpr>(expr)) {
        for (const auto &entry : captureList->getCaptureList()) {
          auto &contexts = captureContexts[entry.getVar()];
          assert(contexts.back() == captureList->getClosureBody());
          contexts.pop_back();
          if (contexts.empty())
            captureContexts.erase(entry.getVar());
        }
      }

      if (auto isolationExpr = dyn_cast<CurrentContextIsolationExpr>(expr))
        recordCurrentContextIsolation(isolationExpr);

      return Action::Continue(expr);
    }

  private:
    /// Find the directly-referenced parameter or capture of a parameter for
    /// for the given expression.
    VarDecl *getReferencedParamOrCapture(Expr *expr) {
      return ::getReferencedParamOrCapture(
          expr, [&](OpaqueValueExpr *opaqueValue) -> Expr * {
            for (const auto &known : opaqueValues) {
              if (known.first == opaqueValue) {
                return known.second;
              }
            }
            return nullptr;
          },
          [this]() -> VarDecl * {
            auto isolation = getActorIsolationOfContext(
                               const_cast<DeclContext *>(getDeclContext()),
                               getClosureActorIsolation);
            if (isolation == ActorIsolation::ActorInstance) {
              VarDecl *var = isolation.getActorInstance();
              if (!var) {
                auto dc = const_cast<DeclContext *>(getDeclContext());
                auto paramIdx = isolation.getActorInstanceParameter();
                if (paramIdx == 0) {
                  var = cast<AbstractFunctionDecl>(dc)->getImplicitSelfDecl();
                } else {
                  var = const_cast<ParamDecl *>(getParameterAt(dc, paramIdx - 1));
                }
              }
              return var;
            }
            return nullptr;
          });
    }

    /// Find the isolated actor instance to which the given expression refers.
    ReferencedActor getIsolatedActor(Expr *expr) {
      // Check whether this expression is an isolated parameter or a reference
      // to a capture thereof.
      auto var = getReferencedParamOrCapture(expr);
      bool isPotentiallyIsolated = isPotentiallyIsolatedActor(var);

      // helps aid in giving more informative diagnostics for autoclosure args.
      auto specificNonIsoClosureKind =
        [](DeclContext const* dc) -> ReferencedActor::Kind {
          if (auto autoClos = dyn_cast<AutoClosureExpr>(dc))
            if (autoClos->getThunkKind() == AutoClosureExpr::Kind::None)
              return ReferencedActor::NonIsolatedAutoclosure;

          return ReferencedActor::NonIsolatedContext;
      };

      // Walk the scopes between the variable reference and the variable
      // declaration to determine whether it is still isolated.
      auto dc = const_cast<DeclContext *>(getDeclContext());
      for (; dc; dc = dc->getParent()) {
        // If we hit the context in which the parameter is declared, we're done.
        if (var && dc == var->getDeclContext()) {
          if (isPotentiallyIsolated) {
            return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::Isolated);
          }
        }

        // If we've hit a module or type boundary, we're done.
        if (dc->isModuleScopeContext() || dc->isTypeContext())
          break;

        if (auto closure = dyn_cast<AbstractClosureExpr>(dc)) {
          auto isolation = getClosureActorIsolation(closure);
          switch (isolation) {
          case ActorIsolation::Unspecified:
          case ActorIsolation::Nonisolated:
          case ActorIsolation::NonisolatedUnsafe:
            if (isSendableClosure(closure, /*forActorIsolation=*/true)) {
              return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::SendableClosure);
            }

            return ReferencedActor(var, isPotentiallyIsolated, specificNonIsoClosureKind(dc));

          case ActorIsolation::ActorInstance:
            // If the closure is isolated to the same variable, we're all set.
            if (isPotentiallyIsolated &&
                (var == isolation.getActorInstance() ||
                 (var->isSelfParamCapture() &&
                  (isolation.getActorInstance()->isSelfParameter() ||
                   isolation.getActorInstance()->isSelfParamCapture())))) {
              return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::Isolated);
            }

            return ReferencedActor(var, isPotentiallyIsolated, specificNonIsoClosureKind(dc));

          case ActorIsolation::GlobalActor:
            return ReferencedActor::forGlobalActor(
                var, isPotentiallyIsolated, isolation.getGlobalActor());

          case ActorIsolation::Erased:
            llvm_unreachable("closure cannot have erased isolation");
          }
        }

        // Check for an 'async let' autoclosure.
        if (auto autoclosure = dyn_cast<AutoClosureExpr>(dc)) {
          switch (autoclosure->getThunkKind()) {
          case AutoClosureExpr::Kind::AsyncLet:
            return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::AsyncLet);

          case AutoClosureExpr::Kind::DoubleCurryThunk:
          case AutoClosureExpr::Kind::SingleCurryThunk:
          case AutoClosureExpr::Kind::None:
            break;
          }
        }

        // Look through defers.
        // FIXME: should this be covered automatically by the logic below?
        if (auto func = dyn_cast<FuncDecl>(dc))
          if (func->isDeferBody())
            continue;

        if (auto func = dyn_cast<AbstractFunctionDecl>(dc)) {
          // @Sendable functions are nonisolated.
          if (func->isSendable())
            return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::SendableFunction);
        }

        // Check isolation of the context itself. We do this separately
        // from the closure check because closures capture specific variables
        // while general isolation is declaration-based.
        switch (auto isolation =
                    getActorIsolationOfContext(dc, getClosureActorIsolation)) {
        case ActorIsolation::Nonisolated:
        case ActorIsolation::NonisolatedUnsafe:
        case ActorIsolation::Unspecified:
          // Local functions can capture an isolated parameter.
          // FIXME: This really should be modeled by getActorIsolationOfContext.
          if (isa<FuncDecl>(dc) && cast<FuncDecl>(dc)->isLocalCapture()) {
            // FIXME: Local functions could presumably capture an isolated
            // parameter that isn't 'self'.
            if (isPotentiallyIsolated &&
                (var->isSelfParameter() || var->isSelfParamCapture()))
              continue;
          }

          return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::NonIsolatedContext);

        case ActorIsolation::Erased:
          llvm_unreachable("context cannot have erased isolation");

        case ActorIsolation::GlobalActor:
          return ReferencedActor::forGlobalActor(
              var, isPotentiallyIsolated, isolation.getGlobalActor());

        case ActorIsolation::ActorInstance:
          break;
        }
      }

      if (isPotentiallyIsolated)
        return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::NonIsolatedContext);

      return ReferencedActor(var, isPotentiallyIsolated, ReferencedActor::NonIsolatedParameter);
    }

    /// Note that the given actor member is isolated.
    /// @param context is allowed to be null if no context is appropriate.
    void noteIsolatedActorMember(ValueDecl const* decl, Expr *context) {
      ::noteIsolatedActorMember(decl, kindOfUsage(decl, context));
    }

    // Retrieve the nearest enclosing actor context.
    static NominalTypeDecl *getNearestEnclosingActorContext(
        const DeclContext *dc) {
      while (!dc->isModuleScopeContext()) {
        if (dc->isTypeContext()) {
          // FIXME: Protocol extensions need specific handling here.
          if (auto nominal = dc->getSelfNominalTypeDecl()) {
            if (nominal->isActor())
              return nominal;
          }
        }

        dc = dc->getParent();
      }

      return nullptr;
    }

    /// Diagnose a reference to an unsafe entity.
    ///
    /// \returns true if we diagnosed the entity, \c false otherwise.
    bool diagnoseReferenceToUnsafeGlobal(ValueDecl *value, SourceLoc loc) {
      auto &ctx = value->getASTContext();
      switch (ctx.LangOpts.StrictConcurrencyLevel) {
      case StrictConcurrency::Minimal:
      case StrictConcurrency::Targeted:
        // Never diagnose.
        return false;

      case StrictConcurrency::Complete:
        break;
      }

      // Only diagnose direct references to mutable global state.
      auto var = dyn_cast<VarDecl>(value);
      if (!var || var->isLet())
        return false;

      if (!var->getDeclContext()->isModuleScopeContext() &&
          !(var->getDeclContext()->isTypeContext() && !var->isInstanceMember()))
        return false;

      if (!var->hasStorage())
        return false;

      // If it's actor-isolated, it's already been dealt with.
      const auto isolation = getActorIsolation(value);
      if (isolation.isActorIsolated())
        return false;

      if (auto attr = value->getAttrs().getAttribute<NonisolatedAttr>();
          attr && attr->isUnsafe()) {
        return false;
      }

      // If global variable checking is enabled and the global variable is
      // from the same module as the reference, we'll already have diagnosed
      // the global variable itself.
      if (ctx.LangOpts.hasFeature(Feature::GlobalConcurrency) &&
          var->getDeclContext()->getParentModule() ==
              getDeclContext()->getParentModule())
        return false;

      const auto import = var->findImport(getDeclContext());
      const bool isPreconcurrencyImport =
          import && import->options.contains(ImportFlags::Preconcurrency);
      const auto isPreconcurrencyUnspecifiedIsolation =
          isPreconcurrencyImport && isolation.isUnspecified();

      // If the global variable is preconcurrency without an explicit
      // isolation, ignore the warning. Otherwise, limit the behavior
      // to a warning until Swift 6.
      DiagnosticBehavior limit;
      if (isPreconcurrencyUnspecifiedIsolation) {
        limit = DiagnosticBehavior::Ignore;
      } else {
        limit = DiagnosticBehavior::Warning;
      }

      ctx.Diags.diagnose(loc, diag::shared_mutable_state_access, value)
          .limitBehaviorUntilSwiftVersion(limit, 6)
          // Preconcurrency global variables are warnings even in Swift 6
          .limitBehaviorIf(isPreconcurrencyImport, limit);
      value->diagnose(diag::kind_declared_here, value->getDescriptiveKind());
      if (const auto sourceFile = getDeclContext()->getParentSourceFile();
          sourceFile && isPreconcurrencyImport) {
        sourceFile->setImportUsedPreconcurrency(*import);
      }
      return true;
    }

    /// Diagnose an inout argument passed into an async call
    ///
    /// \returns true if we diagnosed the entity, \c false otherwise.
    bool diagnoseInOutArg(
        llvm::PointerUnion<ApplyExpr *, LookupExpr *> call,
        const InOutExpr *arg,
        bool isPartialApply) {
      // check that the call is actually async
      if (!isAsyncCall(call))
        return false;

      bool result = false;
      bool downgradeToWarning = false;
      auto diagnoseIsolatedInoutState = [&](
          ConcreteDeclRef declRef, SourceLoc argLoc) {
        auto decl = declRef.getDecl();
        auto isolation = getActorIsolationForReference(decl, getDeclContext());
        if (!isolation.isActorIsolated())
          return;

        if (isPartialApply) {
          auto *apply = call.get<ApplyExpr *>();
          // The partially applied InoutArg is a property of actor. This
          // can really only happen when the property is a struct with a
          // mutating async method.
          if (auto partialApply = dyn_cast<ApplyExpr>(apply->getFn())) {
            if (auto declRef = dyn_cast<DeclRefExpr>(partialApply->getFn())) {
              ValueDecl *fnDecl = declRef->getDecl();
              ctx.Diags.diagnose(apply->getLoc(),
                                 diag::actor_isolated_mutating_func,
                                 fnDecl->getName(), decl)
                  .warnUntilSwiftVersionIf(downgradeToWarning, 6);
              result = true;
              return;
            }
          }
        }

        bool isImplicitlyAsync;
        if (auto *apply = call.dyn_cast<ApplyExpr *>()) {
          isImplicitlyAsync = apply->isImplicitlyAsync().has_value();
        } else {
          auto *lookup = call.get<LookupExpr *>();
          isImplicitlyAsync = lookup->isImplicitlyAsync().has_value();
        }

        ctx.Diags.diagnose(argLoc, diag::actor_isolated_inout_state,
                           decl, isImplicitlyAsync);
        decl->diagnose(diag::kind_declared_here, decl->getDescriptiveKind());
        result = true;
        return;
      };

      auto findIsolatedState = [&](Expr *expr) -> Expr * {
        // This code used to not walk into InOutExpr, which allowed
        // some invalid code to slip by in compilers <=5.9.
        if (isa<InOutExpr>(expr))
          downgradeToWarning = true;

        if (LookupExpr *lookup = dyn_cast<LookupExpr>(expr)) {
          if (isa<DeclRefExpr>(lookup->getBase())) {
            diagnoseIsolatedInoutState(lookup->getMember().getDecl(),
                                       expr->getLoc());
            return nullptr; // Diagnosed. Don't keep walking
          }
        }
        if (DeclRefExpr *declRef = dyn_cast<DeclRefExpr>(expr)) {
          diagnoseIsolatedInoutState(declRef->getDecl(), expr->getLoc());
          return nullptr; // Diagnosed. Don't keep walking
        }
        return expr;
      };
      arg->getSubExpr()->forEachChildExpr(findIsolatedState);
      return result;
    }

    enum class AsyncMarkingResult {
      FoundAsync, // successfully marked an implicitly-async operation
      NotFound,  // fail: no valid implicitly-async operation was found
      SyncContext, // fail: a valid implicitly-async op, but in sync context
      NotDistributed, // fail: non-distributed declaration in distributed actor
    };

    /// Determine whether we can access the given declaration that is
    /// isolated to a distributed actor from a location that is potentially not
    /// local to this process.
    ///
    /// \returns the (setThrows, isDistributedThunk) bits to implicitly
    /// mark the access/call with on success, or emits an error and returns
    /// \c std::nullopt.
    std::optional<std::pair<bool, bool>>
    checkDistributedAccess(SourceLoc declLoc, ValueDecl *decl, Expr *context) {
      // If the actor itself is, we're not doing any distributed access.
      if (getIsolatedActor(context).isKnownToBeLocal()) {
        return std::make_pair(
            /*setThrows=*/false,
            /*isDistributedThunk=*/false);
      }

      // If there is no declaration, it can't possibly be distributed.
      if (!decl) {
        ctx.Diags.diagnose(declLoc, diag::distributed_actor_isolated_method);
        return std::nullopt;
      }

      // Check that we have a distributed function or computed property.
      if (auto afd = dyn_cast<AbstractFunctionDecl>(decl)) {
        if (!afd->isDistributed()) {
          ctx.Diags.diagnose(declLoc, diag::distributed_actor_isolated_method)
              .fixItInsert(decl->getAttributeInsertionLoc(true),
                           "distributed ");

          noteIsolatedActorMember(decl, context);
          return std::nullopt;
        }

        return std::make_pair(
            /*setThrows=*/!afd->hasThrows(),
            /*isDistributedThunk=*/true);
      }

      if (auto *var = dyn_cast<VarDecl>(decl)) {
        if (var->isDistributed()) {
          bool explicitlyThrowing = false;
          if (auto getter = var->getAccessor(swift::AccessorKind::Get)) {
            explicitlyThrowing = getter->hasThrows();
          }
          return std::make_pair(
              /*setThrows*/ !explicitlyThrowing,
              /*isDistributedThunk=*/true);
        }

        // In compiler versions <=5.10, the compiler did not diagnose cases
        // where a non-isolated distributed actor value was passed to a VarDecl
        // with a function type type that has an isolated distributed actor
        // parameter, e.g. `(isolated DA) -> Void`. Stage in the error as a
        // warning until Swift 6.
        if (var->getTypeInContext()->getAs<FunctionType>()) {
          ctx.Diags.diagnose(declLoc,
                             diag::distributed_actor_isolated_non_self_reference,
                             decl)
            .warnUntilSwiftVersion(6);
          noteIsolatedActorMember(decl, context);
          return std::nullopt;
        }
      }

      // FIXME: Subscript?

      // This is either non-distributed variable, subscript, or something else.
      ctx.Diags.diagnose(declLoc,
                         diag::distributed_actor_isolated_non_self_reference,
                         decl);
      noteIsolatedActorMember(decl, context);
      return std::nullopt;
    }

    /// Attempts to identify and mark a valid cross-actor use of a synchronous
    /// actor-isolated member (e.g., sync function application, property access)
    AsyncMarkingResult tryMarkImplicitlyAsync(SourceLoc declLoc,
                                              ConcreteDeclRef concDeclRef,
                                              Expr* context,
                                              ActorIsolation target,
                                              bool isDistributed) {
      ValueDecl *decl = concDeclRef.getDecl();
      AsyncMarkingResult result = AsyncMarkingResult::NotFound;

      // is it an access to a property?
      if (isPropOrSubscript(decl)) {
        // Cannot reference properties or subscripts of distributed actors.
        if (isDistributed) {
          bool setThrows = false;
          bool usesDistributedThunk = false;
          if (auto access = checkDistributedAccess(declLoc, decl, context)) {
            std::tie(setThrows, usesDistributedThunk) = *access;
          } else {
            return AsyncMarkingResult::NotDistributed;
          }

          // distributed computed property access, mark it throws + async
          if (auto lookupExpr = dyn_cast_or_null<LookupExpr>(context)) {
            if (auto memberRef = dyn_cast<MemberRefExpr>(lookupExpr)) {
              memberRef->setImplicitlyThrows(true);
              memberRef->setAccessViaDistributedThunk();
            } else {
              llvm_unreachable("expected distributed prop to be a MemberRef");
            }
          } else {
            llvm_unreachable("expected distributed prop to have LookupExpr");
          }
        }

        if (auto declRef = dyn_cast_or_null<DeclRefExpr>(context)) {
          if (usageEnv(declRef) == VarRefUseEnv::Read) {
            if (!getDeclContext()->isAsyncContext())
              return AsyncMarkingResult::SyncContext;

            declRef->setImplicitlyAsync(target);
            result = AsyncMarkingResult::FoundAsync;
          }
        } else if (auto lookupExpr = dyn_cast_or_null<LookupExpr>(context)) {
          if (usageEnv(lookupExpr) == VarRefUseEnv::Read) {

            if (!getDeclContext()->isAsyncContext())
              return AsyncMarkingResult::SyncContext;

            lookupExpr->setImplicitlyAsync(target);
            result = AsyncMarkingResult::FoundAsync;
          }
        }
      }

      return result;
    }

    /// Check actor isolation for a particular application.
    bool checkApply(ApplyExpr *apply) {
      auto fnExprType = getType(apply->getFn());
      if (!fnExprType)
        return false;

      auto fnType = fnExprType->getAs<FunctionType>();
      if (!fnType)
        return false;

      // The isolation of the context we're in.
      std::optional<ActorIsolation> contextIsolation;
      auto getContextIsolation = [&]() -> ActorIsolation {
        if (contextIsolation)
          return *contextIsolation;

        auto declContext = const_cast<DeclContext *>(getDeclContext());
        contextIsolation =
            getInnermostIsolatedContext(declContext, getClosureActorIsolation);
        return *contextIsolation;
      };

      // Default the call options to allow promotion to async, if it will be
      // warranted.
      ActorReferenceResult::Options callOptions;
      if (!fnType->getExtInfo().isAsync())
        callOptions |= ActorReferenceResult::Flags::AsyncPromotion;

      // Determine from the callee whether actor isolation is unsatisfied.
      std::optional<ActorIsolation> unsatisfiedIsolation;
      bool mayExitToNonisolated = true;
      Expr *argForIsolatedParam = nullptr;
      auto calleeDecl = apply->getCalledValue(/*skipFunctionConversions=*/true);

      auto fnTypeIsolation = fnType->getIsolation();
      if (fnTypeIsolation.isGlobalActor()) {
        // If the function type is global-actor-qualified, determine whether
        // we are within that global actor already.
        Type globalActor = fnTypeIsolation.getGlobalActorType();
        if (!(getContextIsolation().isGlobalActor() &&
            getContextIsolation().getGlobalActor()->isEqual(globalActor)))
          unsatisfiedIsolation = ActorIsolation::forGlobalActor(globalActor);
        mayExitToNonisolated = false;

      } else if (fnTypeIsolation.isErased()) {
        unsatisfiedIsolation = ActorIsolation::forErased();
        mayExitToNonisolated = false;

      } else if (auto *selfApplyFn = dyn_cast<SelfApplyExpr>(
                    apply->getFn()->getValueProvidingExpr())) {
        // If we're calling a member function, check whether the function
        // itself is isolated.
        auto memberFn = selfApplyFn->getFn()->getValueProvidingExpr();
        if (auto memberRef = findReference(memberFn)) {
          auto isolatedActor = getIsolatedActor(selfApplyFn->getBase());
          auto result = ActorReferenceResult::forReference(
              memberRef->first, selfApplyFn->getLoc(), getDeclContext(),
              kindOfUsage(memberRef->first.getDecl(), selfApplyFn),
              isolatedActor, std::nullopt, std::nullopt,
              getClosureActorIsolation);
          switch (result) {
          case ActorReferenceResult::SameConcurrencyDomain:
            break;

          case ActorReferenceResult::ExitsActorToNonisolated:
            unsatisfiedIsolation =
                ActorIsolation::forNonisolated(/*unsafe=*/false);
            break;

          case ActorReferenceResult::EntersActor:
            unsatisfiedIsolation = result.isolation;
            break;
          }

          callOptions = result.options;
          mayExitToNonisolated = false;
          calleeDecl = memberRef->first.getDecl();
          argForIsolatedParam = selfApplyFn->getBase();
        }
      } else if (calleeDecl &&
                 calleeDecl->getAttrs()
                     .hasAttribute<UnsafeInheritExecutorAttr>()) {
        return false;
      }

      // Check for isolated parameters.
      for (unsigned paramIdx : range(fnType->getNumParams())) {
        // We only care about isolated parameters.
        if (!fnType->getParams()[paramIdx].isIsolated())
          continue;

        auto *args = apply->getArgs();
        if (paramIdx >= args->size())
          continue;

        auto *arg = args->getExpr(paramIdx);

        // FIXME: CurrentContextIsolationExpr does not have its actor set
        // at this point.
        if (auto isolation = getCurrentContextIsolation(arg))
          arg = isolation;

        argForIsolatedParam = arg;
        unsatisfiedIsolation = std::nullopt;

        // Assume that a callee with an isolated parameter does not
        // cross an isolation boundary. We'll set this again below if
        // the given isolated argument doesn't match the isolation of the
        // caller.
        mayExitToNonisolated = false;

        // If the argument is an isolated parameter from the enclosing context,
        // or #isolation, then the call does not cross an isolation boundary.
        if (getIsolatedActor(arg) || isa<CurrentContextIsolationExpr>(arg))
          continue;

        auto calleeIsolation = ActorIsolation::forActorInstanceParameter(
            const_cast<Expr *>(arg->findOriginalValue()), paramIdx);

        if (getContextIsolation() != calleeIsolation) {
          if (calleeIsolation.isNonisolated()) {
            mayExitToNonisolated = true;
          } else {
            unsatisfiedIsolation = calleeIsolation;
          }
        }

        if (!fnType->getExtInfo().isAsync())
          callOptions |= ActorReferenceResult::Flags::AsyncPromotion;

        break;
      }

      // If we're calling an async function that's nonisolated, and we're in
      // an isolated context, then we're exiting the actor context.
      if (mayExitToNonisolated && fnType->isAsync() &&
          getContextIsolation().isActorIsolated())
        unsatisfiedIsolation = ActorIsolation::forNonisolated(/*unsafe=*/false);

      // If there was no unsatisfied actor isolation, we're done.
      if (!unsatisfiedIsolation)
        return false;

      bool onlyArgsCrossIsolation = callOptions.contains(
          ActorReferenceResult::Flags::OnlyArgsCrossIsolation);
      if (!onlyArgsCrossIsolation &&
          refineRequiredIsolation(*unsatisfiedIsolation))
        return false;

      // At this point, we know a jump is made to the callee that yields
      // an isolation requirement unsatisfied by the calling context, so
      // set the unsatisfiedIsolationJump fields of the ApplyExpr appropriately
      apply->setIsolationCrossing(getContextIsolation(), *unsatisfiedIsolation);

      bool requiresAsync =
          callOptions.contains(ActorReferenceResult::Flags::AsyncPromotion);

      // If we need to mark the call as implicitly asynchronous, make sure
      // we're in an asynchronous context.
      if (requiresAsync && !getDeclContext()->isAsyncContext()) {

        if (ctx.LangOpts.hasFeature(Feature::GroupActorErrors)) {

          IsolationError mismatch([calleeDecl, apply, unsatisfiedIsolation, getContextIsolation]() {
            if (calleeDecl) {
              auto preconcurrency = getContextIsolation().preconcurrency() || 
                getActorIsolation(calleeDecl).preconcurrency();

              return IsolationError(
                             apply->getLoc(),
                             preconcurrency,
                             Diagnostic(diag::actor_isolated_call_decl,
                                        *unsatisfiedIsolation,
                                        calleeDecl,
                                        getContextIsolation()));
            } else {
              return IsolationError(
                             apply->getLoc(),
                             getContextIsolation().preconcurrency(),
                             Diagnostic(diag::actor_isolated_call,
                                        *unsatisfiedIsolation,
                                        getContextIsolation()));
            }
          }());

          auto iter = applyErrors.find(std::make_pair(*unsatisfiedIsolation, getContextIsolation()));
          if (iter != applyErrors.end()){
            iter->second.push_back((mismatch));
          } else {
            DiagnosticList list;
            list.push_back((mismatch));
            auto keyPair = std::make_pair(*unsatisfiedIsolation, getContextIsolation());
            applyErrors.insert(std::make_pair(keyPair, list));
          }
        } else {
          if (calleeDecl) {
            auto preconcurrency = getContextIsolation().preconcurrency() ||
              getActorIsolation(calleeDecl).preconcurrency();

            ctx.Diags.diagnose(
                               apply->getLoc(), diag::actor_isolated_call_decl,
                               *unsatisfiedIsolation,
                               calleeDecl,
                               getContextIsolation())
            .warnUntilSwiftVersionIf(preconcurrency, 6);
            calleeDecl->diagnose(diag::actor_isolated_sync_func, calleeDecl);
          } else {
            ctx.Diags.diagnose(
                               apply->getLoc(), diag::actor_isolated_call, *unsatisfiedIsolation,
                               getContextIsolation())
            .warnUntilSwiftVersionIf(getContextIsolation().preconcurrency(), 6);
          }

          if (unsatisfiedIsolation->isGlobalActor()) {
            missingGlobalActorOnContext(
                                        const_cast<DeclContext *>(getDeclContext()),
                                        unsatisfiedIsolation->getGlobalActor(), DiagnosticBehavior::Note);
          }
        }

        return true;
      }

      // If the actor we're hopping to is distributed, we might also need
      // to mark the call as throwing and/or using the distributed thunk.
      // FIXME: ActorReferenceResult has this information, too.
      bool setThrows = false;
      bool usesDistributedThunk = false;
      if (unsatisfiedIsolation->isDistributedActor() &&
          !(calleeDecl && isa<ConstructorDecl>(calleeDecl))) {
        auto distributedAccess = checkDistributedAccess(
            apply->getFn()->getLoc(), calleeDecl, argForIsolatedParam);
        if (!distributedAccess)
          return true;

        std::tie(setThrows, usesDistributedThunk) = *distributedAccess;
      }

      // Mark as implicitly async/throws/distributed thunk as needed.
      if (requiresAsync || setThrows || usesDistributedThunk) {
        markNearestCallAsImplicitly(
            unsatisfiedIsolation, setThrows, usesDistributedThunk);
      }

      // Check if language features ask us to defer sendable diagnostics if so,
      // don't check for sendability of arguments here.
      if (!ctx.LangOpts.hasFeature(Feature::RegionBasedIsolation)) {
        diagnoseApplyArgSendability(apply, getDeclContext());
      }

      // Check for sendability of the result type if we do not have a
      // transferring result.
      if ((!ctx.LangOpts.hasFeature(Feature::RegionBasedIsolation) ||
           !fnType->hasSendingResult())) {
        assert(ctx.LangOpts.hasFeature(Feature::SendingArgsAndResults) &&
               "SendingArgsAndResults should be enabled if RegionIsolation is "
               "enabled");
        // See if we are a autoclosure that has a direct callee that has the
        // same non-transferred type value returned. If so, do not emit an
        // error... we are going to emit an error on the call expr and do not
        // want to emit the error twice.
        auto willDoubleError = [&]() -> bool {
          auto *autoclosure = dyn_cast<AutoClosureExpr>(apply->getFn());
          if (!autoclosure)
            return false;
          auto *await =
              dyn_cast<AwaitExpr>(autoclosure->getSingleExpressionBody());
          if (!await)
            return false;
          auto *subCallExpr = dyn_cast<CallExpr>(await->getSubExpr());
          if (!subCallExpr)
            return false;
          return subCallExpr->getType().getPointer() ==
                 fnType->getResult().getPointer();
        };

        if (!willDoubleError() &&
            diagnoseNonSendableTypes(fnType->getResult(), getDeclContext(),
                                     /*inDerivedConformance*/ Type(),
                                     apply->getLoc(),
                                     diag::non_sendable_call_result_type,
                                     apply->isImplicitlyAsync().has_value(),
                                     *unsatisfiedIsolation)) {
          return true;
        }
      }

      return false;
    }

    Expr *getCurrentContextIsolation(Expr *expr) {
      // Look through caller-side default arguments for #isolation.
      auto *defaultArg = dyn_cast<DefaultArgumentExpr>(expr);
      if (defaultArg && defaultArg->isCallerSide()) {
        expr = defaultArg->getCallerSideDefaultExpr();
      }

      if (auto *macro = dyn_cast<MacroExpansionExpr>(expr)) {
        expr = macro->getRewritten();
      }

      if (auto *isolation = dyn_cast<CurrentContextIsolationExpr>(expr)) {
        recordCurrentContextIsolation(isolation);
        return isolation->getActor();
      }

      return nullptr;
    }

    void recordCurrentContextIsolation(
        CurrentContextIsolationExpr *isolationExpr) {
      // If an actor has already been assigned, we're done.
      if (isolationExpr->getActor())
        return;

      auto loc = isolationExpr->getLoc();
      auto isolation = getActorIsolationOfContext(
          const_cast<DeclContext *>(getDeclContext()),
                                    getClosureActorIsolation);
      auto *dc = const_cast<DeclContext *>(getDeclContext());

      // Note that macro expansions are never implicit. They have
      // valid source locations in their macro expansion buffer, they
      // do not cause implicit 'self' capture diagnostics, etc.

      Expr *actorExpr = nullptr;
      Type optionalAnyActorType = isolationExpr->getType();
      switch (isolation) {
      case ActorIsolation::ActorInstance: {
        if (auto *instance = isolation.getActorInstanceExpr()) {
          actorExpr = instance;
          break;
        }

        const VarDecl *var = isolation.getActorInstance();
        if (!var) {
          auto dc = getDeclContext();
          auto paramIdx = isolation.getActorInstanceParameter();
          if (paramIdx == 0) {
            var = cast<AbstractFunctionDecl>(dc)->getImplicitSelfDecl();
          } else {
            var = getParameterAt(dc, paramIdx - 1);
          }
        }
        actorExpr = new (ctx) DeclRefExpr(
            const_cast<VarDecl *>(var), DeclNameLoc(loc),
            /*implicit=*/false);

        // For a distributed actor, we need to retrieve the local
        // actor.
        if (isolation.isDistributedActor()) {
          actorExpr = UnresolvedDotExpr::createImplicit(
              ctx, actorExpr, ctx.Id_asLocalActor);
        }
        break;
      }
      case ActorIsolation::GlobalActor: {
        // Form a <global actor type>.shared reference.
        Type globalActorType = getDeclContext()->mapTypeIntoContext(
            isolation.getGlobalActor());
        auto typeExpr = TypeExpr::createForDecl(
            DeclNameLoc(loc), globalActorType->getAnyNominal(), dc);
        actorExpr = new (ctx) UnresolvedDotExpr(
            typeExpr, loc, DeclNameRef(ctx.Id_shared), DeclNameLoc(loc),
            /*implicit=*/false);
        break;
      }

      case ActorIsolation::Erased:
        llvm_unreachable("context cannot have erased isolation");

      case ActorIsolation::Unspecified:
      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
        actorExpr = new (ctx) NilLiteralExpr(loc, /*implicit=*/false);
        break;
      }


      // Convert the actor argument to the appropriate type.
      (void)TypeChecker::typeCheckExpression(
          actorExpr, dc,
          constraints::ContextualTypeInfo(
            optionalAnyActorType, CTP_CallArgument));

      isolationExpr->setActor(actorExpr);
    }

    /// Find the innermost context in which this declaration was explicitly
    /// captured.
    const DeclContext *findCapturedDeclContext(ValueDecl *value) {
      assert(value->isLocalCapture());
      auto var = dyn_cast<VarDecl>(value);
      if (!var)
        return value->getDeclContext();

      auto knownContexts = captureContexts.find(var);
      if (knownContexts == captureContexts.end())
        return value->getDeclContext();

      return knownContexts->second.back();
    }

    /// Check a reference to a local capture.
    bool checkLocalCapture(
        ConcreteDeclRef valueRef, SourceLoc loc, DeclRefExpr *declRefExpr) {
      auto value = valueRef.getDecl();
      auto *dc = getDeclContext();

      // Check whether we are in a context that will not execute concurrently
      // with the context of 'self'. If not, it's safe.
      if (!mayExecuteConcurrentlyWith(dc, findCapturedDeclContext(value)))
        return false;

      SendableCheckContext sendableBehavior(dc);
      auto limit = sendableBehavior.defaultDiagnosticBehavior();

      // Check whether this is a local variable, in which case we can
      // determine whether it was safe to access concurrently.
      if (auto var = dyn_cast<VarDecl>(value)) {
        // Ignore interpolation variables.
        if (var->getBaseName() == ctx.Id_dollarInterpolation)
          return false;

        auto parent = mutableLocalVarParent[declRefExpr];

        // If the variable is immutable, it's fine so long as it involves
        // Sendable types.
        //
        // When flow-sensitive concurrent captures are enabled, we also
        // allow reads, depending on a SIL diagnostic pass to identify the
        // remaining race conditions.
        if (!var->supportsMutation() ||
            (ctx.LangOpts.hasFeature(
                 Feature::FlowSensitiveConcurrencyCaptures) &&
             parent.dyn_cast<LoadExpr *>())) {
          return false;
        }

        if (auto param =  dyn_cast<ParamDecl>(value)){
          if(param->isInOut()){
              ctx.Diags.diagnose(loc, diag::concurrent_access_of_inout_param, param->getName());
              return true;
          }
        }

        if (auto attr = var->getAttrs().getAttribute<NonisolatedAttr>();
            attr && attr->isUnsafe()) {
          return false;
        }

        // Otherwise, we have concurrent access. Complain.
        ctx.Diags.diagnose(
            loc, diag::concurrent_access_of_local_capture,
            parent.dyn_cast<LoadExpr *>(),
            var)
          .limitBehaviorUntilSwiftVersion(limit, 6);
        return true;
      }

      if (auto func = dyn_cast<FuncDecl>(value)) {
        if (func->isSendable())
          return false;

        func->diagnose(diag::local_function_executed_concurrently, func)
          .fixItInsert(func->getAttributeInsertionLoc(false), "@Sendable ")
          .limitBehaviorUntilSwiftVersion(limit, 6);

        // Add the @Sendable attribute implicitly, so we don't diagnose
        // again.
        const_cast<FuncDecl *>(func)->getAttrs().add(
            new (ctx) SendableAttr(true));
        return true;
      }

      // Concurrent access to some other local.
      ctx.Diags.diagnose(loc, diag::concurrent_access_local, value);
      value->diagnose(
          diag::kind_declared_here, value->getDescriptiveKind());
      return true;
    }

    ///
    /// \return true iff a diagnostic was emitted
    bool checkKeyPathExpr(KeyPathExpr *keyPath) {
      bool diagnosed = false;

      // check the components of the keypath.
      for (const auto &component : keyPath->getComponents()) {
        // The decl referred to by the path component cannot be within an actor.
        if (component.hasDeclRef()) {
          auto declRef = component.getDeclRef();
          auto decl = declRef.getDecl();
          auto isolation = getActorIsolationForReference(
              decl, getDeclContext());
          switch (isolation) {
          case ActorIsolation::Nonisolated:
          case ActorIsolation::NonisolatedUnsafe:
          case ActorIsolation::Unspecified:
            break;

          case ActorIsolation::Erased:
            llvm_unreachable("component cannot have erased isolation");

          case ActorIsolation::GlobalActor: {
            auto result = ActorReferenceResult::forReference(
                declRef, component.getLoc(), getDeclContext(),
                kindOfUsage(decl, keyPath));

            if (result == ActorReferenceResult::SameConcurrencyDomain)
              break;

            LLVM_FALLTHROUGH;
          }

          case ActorIsolation::ActorInstance: {
            ActorReferenceResult::Options options = std::nullopt;
            if (isAccessibleAcrossActors(decl, isolation, getDeclContext(),
                                         options)) {
              break;
            }

            bool downgrade = isolation.isGlobalActor() ||
              options.contains(
                  ActorReferenceResult::Flags::Preconcurrency);

            ctx.Diags.diagnose(
                component.getLoc(), diag::actor_isolated_keypath_component,
                isolation, decl)
              .warnUntilSwiftVersionIf(downgrade, 6);

            diagnosed = !downgrade;
            break;
          }
          }
        }

        // With `InferSendableFromCaptures` feature enabled the solver is
        // responsible for inferring `& Sendable` for sendable key paths.
        if (!ctx.LangOpts.hasFeature(Feature::InferSendableFromCaptures)) {
          // Captured values in a path component must conform to Sendable.
          // These captured values appear in Subscript, such as \Type.dict[k]
          // where k is a captured dictionary key.
          if (auto *args = component.getSubscriptArgs()) {
            for (auto arg : *args) {
              auto type = getType(arg.getExpr());
              if (type && shouldDiagnoseExistingDataRaces(getDeclContext()) &&
                  diagnoseNonSendableTypes(type, getDeclContext(),
                                           /*inDerivedConformance*/Type(),
                                           component.getLoc(),
                                           diag::non_sendable_keypath_capture))
                diagnosed = true;
            }
          }
        }
      }

      return diagnosed;
    }

    /// Check a reference to the given declaration.
    ///
    /// \param base For a reference to a member, the base expression. May be
    /// nullptr for non-member referenced.
    ///
    /// \returns true if the reference is invalid, in which case a diagnostic
    /// has already been emitted.
    bool checkReference(
        Expr *base, ConcreteDeclRef declRef, SourceLoc loc,
        std::optional<PartialApplyThunkInfo> partialApply = std::nullopt,
        Expr *context = nullptr) {
      if (!declRef)
        return false;

      auto decl = declRef.getDecl();

      // If this declaration is a callee from the enclosing application,
      // it's already been checked via the call.
      if (auto *apply = getImmediateApply()) {
        auto immediateCallee =
            apply->getCalledValue(/*skipFunctionConversions=*/true);
        if (decl == immediateCallee)
          return false;
      }

      std::optional<ReferencedActor> isolatedActor;
      if (base)
        isolatedActor.emplace(getIsolatedActor(base));
      auto result = ActorReferenceResult::forReference(
          declRef, loc, getDeclContext(), kindOfUsage(decl, context),
          isolatedActor, std::nullopt, std::nullopt, getClosureActorIsolation);
      switch (result) {
      case ActorReferenceResult::SameConcurrencyDomain:
        return diagnoseReferenceToUnsafeGlobal(decl, loc);

      case ActorReferenceResult::ExitsActorToNonisolated:
        if (diagnoseReferenceToUnsafeGlobal(decl, loc))
          return true;

        return diagnoseNonSendableTypesInReference(
                   base, declRef, getDeclContext(), loc,
                   SendableCheckReason::ExitingActor,
                   result.isolation,
                   // Function reference sendability can only cross isolation
                   // boundaries when they're passed as an argument or called,
                   // and their Sendability depends only on captures; do not
                   // check the parameter or result types here.
                   FunctionCheckOptions());

      case ActorReferenceResult::EntersActor:
        // Handle all of the checking below.
        break;
      }

      // A partial application of a global-actor-isolated member is always
      // okay, because the global actor is part of the resulting function
      // type.
      if (partialApply && result.isolation.isGlobalActor())
        return false;

      // A call to a global-actor-isolated function, or a function with an
      // isolated parameter, is diagnosed elsewhere.
      if (!partialApply &&
          (result.isolation.isGlobalActor() ||
           (result.isolation == ActorIsolation::ActorInstance &&
            result.isolation.getActorInstanceParameter() > 0)) &&
          isa<AbstractFunctionDecl>(decl))
        return false;

      // An escaping partial application of something that is part of
      // the actor's isolated state is never permitted.
      if (partialApply && partialApply->isEscaping && !isAsyncDecl(declRef)) {
        ctx.Diags.diagnose(loc, diag::actor_isolated_partial_apply, decl);
        return true;
      }

      // If we do not need any async/throws/distributed checks, just perform
      // Sendable checking and we're done.
      if (!result.options) {
        return diagnoseNonSendableTypesInReference(
                   base, declRef, getDeclContext(), loc,
                   SendableCheckReason::CrossActor);
      }

      // Some combination of implicit async/throws/distributed is required.
      bool isDistributed = result.options.contains(
          ActorReferenceResult::Flags::Distributed);

      // Determine the actor hop.
      auto implicitAsyncResult = tryMarkImplicitlyAsync(
          loc, declRef, context, result.isolation, isDistributed);
      switch (implicitAsyncResult) {
      case AsyncMarkingResult::FoundAsync:
        return diagnoseNonSendableTypesInReference(
            base, declRef, getDeclContext(), loc,
            SendableCheckReason::SynchronousAsAsync);

      case AsyncMarkingResult::NotDistributed:
        // Failed, but diagnostics have already been emitted.
        return true;

      case AsyncMarkingResult::SyncContext:
      case AsyncMarkingResult::NotFound:
        // If we found an implicitly async reference in a sync
        // context and we're computing the required isolation for
        // an expression, the calling context requires the isolation
        // of the reference.
        if (refineRequiredIsolation(result.isolation)) {
          return false;
        }

        // Complain about access outside of the isolation domain.
        auto useKind = static_cast<unsigned>(
            kindOfUsage(decl, context).value_or(VarRefUseEnv::Read));

        ReferencedActor::Kind refKind;
        Type refGlobalActor;
        if (isolatedActor) {
          refKind = isolatedActor->kind;
          refGlobalActor = isolatedActor->globalActor;
        } else {
          auto contextIsolation = getInnermostIsolatedContext(
              getDeclContext(), getClosureActorIsolation);
          switch (contextIsolation) {
          case ActorIsolation::ActorInstance:
            refKind = ReferencedActor::Isolated;
            break;

          case ActorIsolation::Erased:
            llvm_unreachable("context cannot have erased isolation");

          case ActorIsolation::GlobalActor:
            refGlobalActor = contextIsolation.getGlobalActor();
            refKind = isMainActor(refGlobalActor)
                ? ReferencedActor::MainActor
                : ReferencedActor::GlobalActor;
            break;

          case ActorIsolation::Unspecified:
          case ActorIsolation::Nonisolated:
          case ActorIsolation::NonisolatedUnsafe:
            refKind = ReferencedActor::NonIsolatedContext;
            break;
          }
        }

        // Does the reference originate from a @preconcurrency context?
        bool preconcurrencyContext =
          result.options.contains(ActorReferenceResult::Flags::Preconcurrency);

        Type derivedConformanceType;
        DeclName requirementName;
        if (loc.isInvalid()) {
          auto *decl = getDeclContext()->getAsDecl();
          if (decl && decl->isImplicit()) {
            auto *parentDC = decl->getDeclContext();
            loc = parentDC->getAsDecl()->getLoc();

            if (auto *implements = decl->getAttrs().getAttribute<ImplementsAttr>()) {
              derivedConformanceType =
                  implements->getProtocol(parentDC)->getDeclaredInterfaceType();
              requirementName = implements->getMemberName();
            }
          }
        }

        if (ctx.LangOpts.hasFeature(Feature::GroupActorErrors)) {
          IsolationError mismatch = IsolationError(loc,
              preconcurrencyContext,
              Diagnostic(diag::actor_isolated_non_self_reference,
                  decl, useKind, refKind + 1, refGlobalActor,
                  result.isolation));

          auto iter = refErrors.find(std::make_pair(refKind,result.isolation));
          if (iter != refErrors.end()) {
            iter->second.push_back(mismatch);
          } else {
            DiagnosticList list;
            list.push_back(mismatch);
            auto keyPair = std::make_pair(refKind,result.isolation);
            refErrors.insert(std::make_pair(keyPair, list));
          }
        } else {
          ctx.Diags.diagnose(
              loc, diag::actor_isolated_non_self_reference,
              decl, useKind,
              refKind + 1, refGlobalActor,
              result.isolation)
          .warnUntilSwiftVersionIf(preconcurrencyContext, 6);

          if (derivedConformanceType) {
            auto *decl = dyn_cast<ValueDecl>(getDeclContext()->getAsDecl());
            ctx.Diags.diagnose(loc, diag::in_derived_witness,
                               decl->getDescriptiveKind(),
                               requirementName,
                               derivedConformanceType);
          }

          noteIsolatedActorMember(decl, context);
          if (result.isolation.isGlobalActor()) {
            missingGlobalActorOnContext(
                const_cast<DeclContext *>(getDeclContext()),
                result.isolation.getGlobalActor(), DiagnosticBehavior::Note);
          }
        }

        return true;
      }
    }

    // Attempt to resolve the global actor type of a closure.
    Type resolveGlobalActorType(ClosureExpr *closure) {
      // Check whether the closure's type has a global actor already.
      if (Type closureType = getType(closure)) {
        if (auto closureFnType = closureType->getAs<FunctionType>()) {
          if (Type globalActor = closureFnType->getGlobalActor())
            return globalActor;
        }
      }

      // Look for an explicit attribute.
      return getExplicitGlobalActor(closure);
    }

  public:
    /// Determine the isolation of a particular closure.
    ///
    /// This function assumes that enclosing closures have already had their
    /// isolation checked.
    ActorIsolation determineClosureIsolation(
        AbstractClosureExpr *closure) {
      bool preconcurrency = false;

      if (auto explicitClosure = dyn_cast<ClosureExpr>(closure)) {
        preconcurrency = explicitClosure->isIsolatedByPreconcurrency();

        // If the closure specifies a global actor, use it.
        if (Type globalActor = resolveGlobalActorType(explicitClosure))
          return ActorIsolation::forGlobalActor(globalActor)
              .withPreconcurrency(preconcurrency);

        if (auto *attr =
                explicitClosure->getAttrs().getAttribute<NonisolatedAttr>();
            attr && ctx.LangOpts.hasFeature(Feature::ClosureIsolation)) {
          return ActorIsolation::forNonisolated(attr->isUnsafe())
              .withPreconcurrency(preconcurrency);
        }
      }

      // If a closure has an isolated parameter, it is isolated to that
      // parameter.
      for (auto param : *closure->getParameters()) {
        if (param->isIsolated())
          return ActorIsolation::forActorInstanceCapture(param)
              .withPreconcurrency(preconcurrency);
      }

      // If we have a closure that acts as an isolation inference boundary, then
      // we return that it is non-isolated.
      //
      // NOTE: Since we already checked for global actor isolated things, we
      // know that all Sendable closures must be nonisolated. That is why it is
      // safe to rely on this path to handle Sendable closures.
      if (isIsolationInferenceBoundaryClosure(
              closure, true /*is for closure isolation*/))
        return ActorIsolation::forNonisolated(/*unsafe=*/false)
            .withPreconcurrency(preconcurrency);

      // A non-Sendable closure gets its isolation from its context.
      auto parentIsolation = getActorIsolationOfContext(
          closure->getParent(), getClosureActorIsolation);
      preconcurrency |= parentIsolation.preconcurrency();

      // We must have parent isolation determined to get here.
      switch (parentIsolation) {
      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
      case ActorIsolation::Unspecified:
        return ActorIsolation::forNonisolated(parentIsolation ==
                                              ActorIsolation::NonisolatedUnsafe)
            .withPreconcurrency(preconcurrency);

      case ActorIsolation::Erased:
        llvm_unreachable("context cannot have erased isolation");

      case ActorIsolation::GlobalActor: {
        Type globalActor = closure->mapTypeIntoContext(
            parentIsolation.getGlobalActor()->mapTypeOutOfContext());
        return ActorIsolation::forGlobalActor(globalActor)
            .withPreconcurrency(preconcurrency);
      }

      case ActorIsolation::ActorInstance: {
        if (checkIsolatedCapture) {
          if (auto param = closure->getCaptureInfo().getIsolatedParamCapture())
            return ActorIsolation::forActorInstanceCapture(param)
                .withPreconcurrency(preconcurrency);
        } else {
          // If we don't have capture information during code completion, assume
          // that the closure captures the `isolated` parameter from the parent
          // context.
          return parentIsolation;
        }

        return ActorIsolation::forNonisolated(/*unsafe=*/false)
            .withPreconcurrency(preconcurrency);
      }
    }
    }

  };
}

bool ActorIsolationChecker::mayExecuteConcurrentlyWith(
    const DeclContext *useContext, const DeclContext *defContext) {
  // Fast path for when the use and definition contexts are the same.
  if (useContext == defContext)
    return false;

  bool isolatedStateMayEscape = false;

  auto useIsolation = getActorIsolationOfContext(
      const_cast<DeclContext *>(useContext), getClosureActorIsolation);
  if (useIsolation.isActorIsolated()) {
    auto defIsolation = getActorIsolationOfContext(
        const_cast<DeclContext *>(defContext), getClosureActorIsolation);
    // If both contexts are isolated to the same actor, then they will not
    // execute concurrently.
    if (useIsolation == defIsolation)
      return false;

    auto &ctx = useContext->getASTContext();
    bool regionIsolationEnabled =
        ctx.LangOpts.hasFeature(Feature::RegionBasedIsolation);
    
    // Globally-isolated closures may never be executed concurrently.
    if (ctx.LangOpts.hasFeature(Feature::GlobalActorIsolatedTypesUsability) &&
        regionIsolationEnabled && useIsolation.isGlobalActor())
      return false;

    // If the local function is not Sendable, its isolation differs
    // from that of the context, and both contexts are actor isolated,
    // then capturing non-Sendable values allows the closure to stash
    // those values into actor isolated state. The original context
    // may also stash those values into isolated state, enabling concurrent
    // access later on.
    isolatedStateMayEscape =
        (!regionIsolationEnabled &&
        useIsolation.isActorIsolated() && defIsolation.isActorIsolated());
  }

  // Walk the context chain from the use to the definition.
  while (useContext != defContext) {
    // If we find a concurrent closure... it can be run concurrently.
    if (auto closure = dyn_cast<AbstractClosureExpr>(useContext)) {
      if (isSendableClosure(closure, /*forActorIsolation=*/false))
        return true;

      if (isolatedStateMayEscape)
        return true;
    }

    if (auto func = dyn_cast<FuncDecl>(useContext)) {
      if (func->isLocalCapture()) {
        // If the function is @Sendable... it can be run concurrently.
        if (func->isSendable())
          return true;

        if (isolatedStateMayEscape)
          return true;
      }
    }

    // If we hit a module-scope or type context context, it's not
    // concurrent.
    useContext = useContext->getParent();
    if (useContext->isModuleScopeContext() || useContext->isTypeContext())
      return false;
  }

  // We hit the same context, so it won't execute concurrently.
  return false;
}

void swift::checkTopLevelActorIsolation(TopLevelCodeDecl *decl) {
  ActorIsolationChecker checker(decl);
  if (auto *body = decl->getBody())
    body->walk(checker);
}

void swift::checkFunctionActorIsolation(AbstractFunctionDecl *decl) {
  // Disable this check for @LLDBDebuggerFunction functions.
  if (decl->getAttrs().hasAttribute<LLDBDebuggerFunctionAttr>())
    return;

  auto &ctx = decl->getASTContext();
  ActorIsolationChecker checker(decl);
  if (auto body = decl->getBody()) {
    body->walk(checker);
    if (ctx.LangOpts.hasFeature(Feature::GroupActorErrors)) {
      checker.diagnoseIsolationErrors();
    }
  }
  if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
    if (auto superInit = ctor->getSuperInitCall())
      superInit->walk(checker);
  }

  if (decl->getAttrs().hasAttribute<DistributedActorAttr>()) {
    if (auto func = dyn_cast<FuncDecl>(decl)) {
      checkDistributedFunction(func);
    }
  }
}

void swift::checkEnumElementActorIsolation(
    EnumElementDecl *element, Expr *expr) {
  ActorIsolationChecker checker(element);
  expr->walk(checker);
}

void swift::checkPropertyWrapperActorIsolation(
    VarDecl *wrappedVar, Expr *expr) {
  ActorIsolationChecker checker(wrappedVar->getDeclContext());
  expr->walk(checker);
}

ActorIsolation swift::determineClosureActorIsolation(
    AbstractClosureExpr *closure, llvm::function_ref<Type(Expr *)> getType,
    llvm::function_ref<ActorIsolation(AbstractClosureExpr *)>
        getClosureActorIsolation) {
  ActorIsolationChecker checker(closure->getParent(), getType,
                                getClosureActorIsolation,
                                /*checkIsolatedCapture=*/false);
  return checker.determineClosureIsolation(closure);
}

/// Determine whethere there is an explicit isolation attribute
/// of any kind.
static bool hasExplicitIsolationAttribute(const Decl *decl) {
  if (auto nonisolatedAttr =
      decl->getAttrs().getAttribute<NonisolatedAttr>()) {
    if (!nonisolatedAttr->isImplicit())
      return true;
  }

  if (auto globalActorAttr = decl->getGlobalActorAttr()) {
    if (!globalActorAttr->first->isImplicit())
      return true;
  }

  return false;
}

/// Determine actor isolation solely from attributes.
///
/// \returns the actor isolation determined from attributes alone (with no
/// inference rules). Returns \c None if there were no attributes on this
/// declaration.
static std::optional<ActorIsolation>
getIsolationFromAttributes(const Decl *decl, bool shouldDiagnose = true,
                           bool onlyExplicit = false) {
  // Look up attributes on the declaration that can affect its actor isolation.
  // If any of them are present, use that attribute.
  auto nonisolatedAttr = decl->getAttrs().getAttribute<NonisolatedAttr>();
  auto globalActorAttr = decl->getGlobalActorAttr();

  // Remove implicit attributes if we only care about explicit ones.
  if (onlyExplicit) {
    if (nonisolatedAttr && nonisolatedAttr->isImplicit())
      nonisolatedAttr = nullptr;
    if (globalActorAttr && globalActorAttr->first->isImplicit())
      globalActorAttr = std::nullopt;
  }

  unsigned numIsolationAttrs =
    (nonisolatedAttr ? 1 : 0) + (globalActorAttr ? 1 : 0);
  if (numIsolationAttrs == 0)
    return std::nullopt;

  // Only one such attribute is valid, but we only actually care of one of
  // them is a global actor.
  if (numIsolationAttrs > 1 && globalActorAttr && shouldDiagnose) {
    decl->diagnose(diag::actor_isolation_multiple_attr, decl,
                   nonisolatedAttr->getAttrName(),
                   globalActorAttr->second->getName().str())
      .highlight(nonisolatedAttr->getRangeWithAt())
      .highlight(globalActorAttr->first->getRangeWithAt());
  }

  // If the declaration is explicitly marked 'nonisolated', report it as
  // independent.
  if (nonisolatedAttr) {
    return ActorIsolation::forNonisolated(nonisolatedAttr->isUnsafe());
  }

  // If the declaration is marked with a global actor, report it as being
  // part of that global actor.
  if (globalActorAttr) {
    ASTContext &ctx = decl->getASTContext();
    auto dc = decl->getInnermostDeclContext();
    Type globalActorType = evaluateOrDefault(
        ctx.evaluator,
        CustomAttrTypeRequest{
          globalActorAttr->first, dc, CustomAttrTypeKind::GlobalActor},
        Type());
    if (!globalActorType || globalActorType->hasError())
      return ActorIsolation::forUnspecified();

    // Handle @<global attribute type>(unsafe).
    auto *attr = globalActorAttr->first;
    bool isUnsafe = attr->isArgUnsafe();
    if (attr->hasArgs()) {
      if (isUnsafe) {
        SourceFile *file = decl->getDeclContext()->getParentSourceFile();
        bool inSwiftinterface =
            file && file->Kind == SourceFileKind::Interface;
        ctx.Diags.diagnose(
            attr->getLocation(),
            diag::unsafe_global_actor)
          .fixItRemove(attr->getArgs()->getSourceRange())
          .fixItInsert(attr->getLocation(), "@preconcurrency ")
          .warnUntilSwiftVersion(6)
          .limitBehaviorIf(inSwiftinterface, DiagnosticBehavior::Ignore);
      } else {
        ctx.Diags.diagnose(
            attr->getLocation(),
            diag::global_actor_arg, globalActorType)
          .fixItRemove(attr->getArgs()->getSourceRange());
      }
    }

    return ActorIsolation::forGlobalActor(
        globalActorType->mapTypeOutOfContext())
        .withPreconcurrency(decl->preconcurrency() || isUnsafe);
  }

  llvm_unreachable("Forgot about an attribute?");
}

/// Infer isolation from witnessed protocol requirements.
static std::optional<ActorIsolation>
getIsolationFromWitnessedRequirements(ValueDecl *value) {
  // Associated types cannot have isolation, so there's no such inference for
  // type witnesses.
  if (isa<TypeDecl>(value))
    return std::nullopt;

  auto dc = value->getDeclContext();
  auto idc = dyn_cast_or_null<IterableDeclContext>(dc->getAsDecl());
  if (!idc)
    return std::nullopt;

  if (dc->getSelfProtocolDecl())
    return std::nullopt;

  // Walk through each of the conformances in this context, collecting any
  // requirements that have actor isolation.
  auto conformances = idc->getLocalConformances( // note this
      ConformanceLookupKind::NonStructural);
  using IsolatedRequirement =
      std::tuple<ProtocolConformance *, ActorIsolation, ValueDecl *>;
  SmallVector<IsolatedRequirement, 2> isolatedRequirements;
  for (auto conformance : conformances) {
    auto protocol = conformance->getProtocol();
    for (auto found : protocol->lookupDirect(value->getName())) {
      if (!isa<ProtocolDecl>(found->getDeclContext()))
        continue;

      auto requirement = dyn_cast<ValueDecl>(found);
      if (!requirement || isa<TypeDecl>(requirement))
        continue;

      auto requirementIsolation = getActorIsolation(requirement);
      switch (requirementIsolation) {
      case ActorIsolation::ActorInstance:
      case ActorIsolation::Unspecified:
        continue;

      case ActorIsolation::Erased:
        llvm_unreachable("requirement cannot have erased isolation");

      case ActorIsolation::GlobalActor:
      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
        break;
      }

      auto witness = conformance->getWitnessDecl(requirement);
      if (witness != value)
        continue;

      isolatedRequirements.push_back(
          IsolatedRequirement{conformance, requirementIsolation, requirement});
    }
  }

  // Filter out duplicate actors.
  SmallPtrSet<CanType, 2> globalActorTypes;
  bool sawActorIndependent = false;
  isolatedRequirements.erase(
      std::remove_if(isolatedRequirements.begin(), isolatedRequirements.end(),
                     [&](IsolatedRequirement &isolated) {
    auto isolation = std::get<1>(isolated);
    switch (isolation) {
      case ActorIsolation::ActorInstance:
        llvm_unreachable("protocol requirements cannot be actor instances");

      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
        // We only need one nonisolated.
        if (sawActorIndependent)
          return true;

        sawActorIndependent = true;
        return false;

      case ActorIsolation::Erased:
        llvm_unreachable("requirements cannot have erased isolation");

      case ActorIsolation::Unspecified:
        return true;

      case ActorIsolation::GlobalActor: {
        // Substitute into the global actor type.
        auto conformance = std::get<0>(isolated);
        auto requirementSubs = SubstitutionMap::getProtocolSubstitutions(
            conformance->getProtocol(), dc->getSelfTypeInContext(),
            ProtocolConformanceRef(conformance));
        Type globalActor = isolation.getGlobalActor().subst(requirementSubs);
        if (!globalActorTypes.insert(globalActor->getCanonicalType()).second)
          return true;

        // Update the global actor type, now that we've done this substitution.
        std::get<1>(isolated) = ActorIsolation::forGlobalActor(globalActor)
            .withPreconcurrency(isolation.preconcurrency());
        return false;
      }
      }
      }),
      isolatedRequirements.end());

  if (isolatedRequirements.size() != 1)
    return std::nullopt;

  return std::get<1>(isolatedRequirements.front());
}

/// Compute the isolation of a nominal type from the conformances that
/// are directly specified on the type.
static std::optional<ActorIsolation>
getIsolationFromConformances(NominalTypeDecl *nominal) {
  if (isa<ProtocolDecl>(nominal))
    return std::nullopt;

  std::optional<ActorIsolation> foundIsolation;
  for (auto proto :
       nominal->getLocalProtocols(ConformanceLookupKind::NonStructural)) {
    switch (auto protoIsolation = getActorIsolation(proto)) {
    case ActorIsolation::ActorInstance:
    case ActorIsolation::Unspecified:
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
      break;

    case ActorIsolation::Erased:
      llvm_unreachable("protocol cannot have erased isolation");

    case ActorIsolation::GlobalActor:
      if (!foundIsolation) {
        foundIsolation = protoIsolation;
        continue;
      }

      if (*foundIsolation != protoIsolation)
        return std::nullopt;

      break;
    }
  }

  return foundIsolation;
}

/// Compute the isolation of a protocol
static std::optional<ActorIsolation>
getIsolationFromInheritedProtocols(ProtocolDecl *protocol) {
  std::optional<ActorIsolation> foundIsolation;
  bool conflict = false;

  auto inferIsolation = [&](ValueDecl *decl) {
    switch (auto protoIsolation = getActorIsolation(decl)) {
    case ActorIsolation::ActorInstance:
    case ActorIsolation::Unspecified:
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
      return;

    case ActorIsolation::Erased:
      llvm_unreachable("protocol cannot have erased isolation");

    case ActorIsolation::GlobalActor:
      if (!foundIsolation) {
        foundIsolation = protoIsolation;
        return;
      }

      if (*foundIsolation != protoIsolation)
        conflict = true;

      return;
    }
  };

  for (auto inherited : protocol->getInheritedProtocols()) {
    inferIsolation(inherited);
  }

  if (auto *superclass = protocol->getSuperclassDecl()) {
    inferIsolation(superclass);
  }

  if (conflict)
    return std::nullopt;

  return foundIsolation;
}

/// Compute the isolation of a nominal type from the property wrappers on
/// any stored properties.
static std::optional<ActorIsolation>
getIsolationFromWrappers(NominalTypeDecl *nominal) {
  if (!isa<StructDecl>(nominal) && !isa<ClassDecl>(nominal))
    return std::nullopt;

  if (!nominal->getParentSourceFile())
    return std::nullopt;

  ASTContext &ctx = nominal->getASTContext();
  if (ctx.LangOpts.hasFeature(Feature::DisableOutwardActorInference)) {
    // In Swift 6, we no longer infer isolation of a nominal type
    // based on the property wrappers used in its stored properties
    return std::nullopt;
  }

  std::optional<ActorIsolation> foundIsolation;
  for (auto member : nominal->getMembers()) {
    auto var = dyn_cast<VarDecl>(member);
    if (!var || !var->isInstanceMember())
      continue;

    auto info = var->getAttachedPropertyWrapperTypeInfo(0);
    if (!info)
      continue;

    auto isolation = getActorIsolation(info.valueVar);

    // Inconsistent wrappedValue/projectedValue isolation disables inference.
    if (info.projectedValueVar &&
        getActorIsolation(info.projectedValueVar) != isolation)
      continue;

    switch (isolation) {
    case ActorIsolation::ActorInstance:
    case ActorIsolation::Unspecified:
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
      break;

    case ActorIsolation::Erased:
      llvm_unreachable("variable cannot have erased isolation");

    case ActorIsolation::GlobalActor:
      if (!foundIsolation) {
        foundIsolation = isolation;
        continue;
      }

      if (*foundIsolation != isolation)
        return std::nullopt;

      break;
    }
  }

  return foundIsolation;
}

namespace {

/// Describes how actor isolation is propagated to a member, if at all.
enum class MemberIsolationPropagation {
  GlobalActor,
  AnyIsolation
};

}
/// Determine how the given member can receive its isolation from its type
/// context.
static std::optional<MemberIsolationPropagation>
getMemberIsolationPropagation(const ValueDecl *value) {
  if (!value->getDeclContext()->isTypeContext())
    return std::nullopt;

  switch (value->getKind()) {
  case DeclKind::Import:
  case DeclKind::Extension:
  case DeclKind::TopLevelCode:
  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::PrecedenceGroup:
  case DeclKind::Missing:
  case DeclKind::MissingMember:
  case DeclKind::Class:
  case DeclKind::Enum:
  case DeclKind::Protocol:
  case DeclKind::Struct:
  case DeclKind::TypeAlias:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::OpaqueType:
  case DeclKind::Param:
  case DeclKind::Module:
  case DeclKind::Destructor:
  case DeclKind::EnumCase:
  case DeclKind::EnumElement:
  case DeclKind::Macro:
  case DeclKind::MacroExpansion:
    return std::nullopt;

  case DeclKind::PatternBinding:
    return MemberIsolationPropagation::GlobalActor;

  case DeclKind::Constructor:
    return MemberIsolationPropagation::AnyIsolation;

  case DeclKind::Func:
  case DeclKind::Accessor:
  case DeclKind::Subscript:
  case DeclKind::Var:
    return value->isInstanceMember() ? MemberIsolationPropagation::AnyIsolation
                                     : MemberIsolationPropagation::GlobalActor;

  case DeclKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleDecl should not show up here");
  }
}

/// Given a property, determine the isolation when it part of a wrapped
/// property.
static ActorIsolation getActorIsolationFromWrappedProperty(VarDecl *var) {
  // If this is a variable with a property wrapper, infer from the property
  // wrapper's wrappedValue.
  if (auto wrapperInfo = var->getAttachedPropertyWrapperTypeInfo(0)) {
    if (auto wrappedValue = wrapperInfo.valueVar) {
      if (auto isolation = getActorIsolation(wrappedValue))
        return isolation;
    }
  }

  // If this is the backing storage for a property wrapper, infer from the
  // type of the outermost property wrapper.
  if (auto originalVar = var->getOriginalWrappedProperty(
          PropertyWrapperSynthesizedPropertyKind::Backing)) {
    if (auto backingType =
            originalVar->getPropertyWrapperBackingPropertyType()) {
      if (auto backingNominal = backingType->getAnyNominal()) {
        if (!isa<ClassDecl>(backingNominal) ||
            !cast<ClassDecl>(backingNominal)->isActor()) {
          if (auto isolation = getActorIsolation(backingNominal))
            return isolation;
        }
      }
    }
  }

  // If this is the projected property for a property wrapper, infer from
  // the property wrapper's projectedValue.
  if (auto originalVar = var->getOriginalWrappedProperty(
          PropertyWrapperSynthesizedPropertyKind::Projection)) {
    if (auto wrapperInfo =
            originalVar->getAttachedPropertyWrapperTypeInfo(0)) {
      if (auto projectedValue = wrapperInfo.projectedValueVar) {
        if (auto isolation = getActorIsolation(projectedValue))
          return isolation;
      }
    }
  }

  return ActorIsolation::forUnspecified();
}

static std::optional<ActorIsolation>
getActorIsolationForMainFuncDecl(FuncDecl *fnDecl) {
  // Ensure that the base type that this function is declared in has @main
  // attribute
  NominalTypeDecl *declContext =
      dyn_cast<NominalTypeDecl>(fnDecl->getDeclContext());
  if (ExtensionDecl *exDecl =
          dyn_cast<ExtensionDecl>(fnDecl->getDeclContext())) {
    declContext = exDecl->getExtendedNominal();
  }

  // We're not even in a nominal decl type, this can't be the main function decl
  if (!declContext)
    return {};
  const bool isMainDeclContext =
      declContext->getAttrs().hasAttribute<MainTypeAttr>(
          /*allow invalid*/ true);

  ASTContext &ctx = fnDecl->getASTContext();

  const bool isMainMain = fnDecl->isMainTypeMainMethod();
  const bool isMainInternalMain =
      fnDecl->getBaseIdentifier() == ctx.getIdentifier("$main") &&
      !fnDecl->isInstanceMember() &&
      fnDecl->getResultInterfaceType()->isVoid() &&
      fnDecl->getParameters()->size() == 0;
  const bool isMainFunction =
      isMainDeclContext && (isMainMain || isMainInternalMain);
  const bool hasMainActor = !ctx.getMainActorType().isNull();

  return isMainFunction && hasMainActor
             ? ActorIsolation::forGlobalActor(
                   ctx.getMainActorType()->mapTypeOutOfContext())
             : std::optional<ActorIsolation>();
}

/// Check rules related to global actor attributes on a class declaration.
///
/// \returns true if an error occurred.
static bool checkClassGlobalActorIsolation(
    ClassDecl *classDecl, ActorIsolation isolation) {
  assert(isolation.isGlobalActor());

  // A class can only be annotated with a global actor if it has no
  // superclass, the superclass is annotated with the same global actor, or
  // the superclass is NSObject. A subclass of a global-actor-annotated class
  // must be isolated to the same global actor.
  auto superclassDecl = classDecl->getSuperclassDecl();
  if (!superclassDecl)
    return false;

  if (superclassDecl->isNSObject())
    return false;

  // Ignore actors outright. They'll be diagnosed later.
  if (classDecl->isActor() || superclassDecl->isActor())
    return false;

  // Check the superclass's isolation.
  bool downgradeToWarning = false;
  auto superIsolation = getActorIsolation(superclassDecl);
  switch (superIsolation) {
  case ActorIsolation::Unspecified:
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe: {
    return false;
  }

  case ActorIsolation::Erased:
    llvm_unreachable("class cannot have erased isolation");

  case ActorIsolation::ActorInstance:
    // This is an error that will be diagnosed later. Ignore it here.
    return false;

  case ActorIsolation::GlobalActor: {
    // If the global actors match, we're fine.
    Type superclassGlobalActor = superIsolation.getGlobalActor();
    auto module = classDecl->getParentModule();
    SubstitutionMap subsMap = classDecl->getDeclaredInterfaceType()
      ->getSuperclassForDecl(superclassDecl)
      ->getContextSubstitutionMap(module, superclassDecl);
    Type superclassGlobalActorInSub = superclassGlobalActor.subst(subsMap);
    if (isolation.getGlobalActor()->isEqual(superclassGlobalActorInSub))
      return false;

    break;
  }
  }

  // Complain about the mismatch.
  classDecl->diagnose(diag::actor_isolation_superclass_mismatch, isolation,
                      classDecl, superIsolation, superclassDecl)
      .warnUntilSwiftVersionIf(downgradeToWarning, 6);
  return true;
}

namespace {
  /// Describes the result of checking override isolation.
  enum class OverrideIsolationResult {
    /// The override is permitted.
    Allowed,
    /// The override is permitted, but requires a Sendable check.
    Sendable,
    /// The override is not permitted.
    Disallowed,
  };
}

/// Return the isolation of the declaration overridden by this declaration,
/// in the context of the
static ActorIsolation getOverriddenIsolationFor(ValueDecl *value) {
  auto overridden = value->getOverriddenDecl();
  assert(overridden && "Doesn't have an overridden declaration");

  auto isolation = getActorIsolation(overridden);
  if (!isolation.requiresSubstitution())
    return isolation;

  SubstitutionMap subs;
  if (Type selfType = value->getDeclContext()->getSelfInterfaceType()) {
    subs = selfType->getMemberSubstitutionMap(
        value->getModuleContext(), overridden);
  }
  return isolation.subst(subs);
}

ConcreteDeclRef swift::getDeclRefInContext(ValueDecl *value) {
  auto declContext = value->getInnermostDeclContext();
  if (auto genericEnv = declContext->getGenericEnvironmentOfContext()) {
    return ConcreteDeclRef(
        value, genericEnv->getForwardingSubstitutionMap());
  }

  return ConcreteDeclRef(value);
}

/// Generally speaking, the isolation of the decl that overrides
/// must match the overridden decl. But there are a number of exceptions,
/// e.g., the decl that overrides can be nonisolated.
/// \param isolation the isolation of the overriding declaration.
static OverrideIsolationResult validOverrideIsolation(
    ValueDecl *value, ActorIsolation isolation,
    ValueDecl *overridden, ActorIsolation overriddenIsolation) {
  ConcreteDeclRef valueRef = getDeclRefInContext(value);
  auto declContext = value->getInnermostDeclContext();
  auto &ctx = declContext->getASTContext();

  auto refResult = ActorReferenceResult::forReference(
      valueRef, SourceLoc(), declContext, std::nullopt, std::nullopt, isolation,
      overriddenIsolation);
  switch (refResult) {
  case ActorReferenceResult::SameConcurrencyDomain:
    return OverrideIsolationResult::Allowed;

  case ActorReferenceResult::ExitsActorToNonisolated:
    return OverrideIsolationResult::Sendable;

  case ActorReferenceResult::EntersActor:
    // It's okay to enter the actor when the overridden declaration is
    // asynchronous (because it will do the switch) or is accessible from
    // anywhere.
    if (isAsyncDecl(overridden) ||
        isAccessibleAcrossActors(
            overridden, refResult.isolation, declContext)) {
      return OverrideIsolationResult::Sendable;
    }

    // If the overridden declaration is from Objective-C with no actor
    // annotation, allow it.
    if (ctx.LangOpts.StrictConcurrencyLevel != StrictConcurrency::Complete &&
        overridden->hasClangNode() && !overriddenIsolation) {
      return OverrideIsolationResult::Allowed;
    }

    return OverrideIsolationResult::Disallowed;
  }
}

/// Retrieve the index of the first isolated parameter of the given
/// declaration, if there is one.
static std::optional<unsigned> getIsolatedParamIndex(ValueDecl *value) {
  auto params = getParameterList(value);
  if (!params)
    return std::nullopt;

  for (unsigned paramIdx : range(params->size())) {
    auto param = params->get(paramIdx);
    if (param->isIsolated())
      return paramIdx;
  }

  return std::nullopt;
}

/// Verifies rules about `isolated` parameters for the given decl. There is more
/// checking about these in TypeChecker::checkParameterList.
///
/// This function is focused on rules that apply when it's a declaration with
/// an isolated parameter, rather than some generic parameter list in a
/// DeclContext.
///
/// This function assumes the value already contains an isolated parameter.
static void checkDeclWithIsolatedParameter(ValueDecl *value) {
  // assume there is an isolated parameter.
  assert(getIsolatedParamIndex(value));

  // Suggest removing global-actor attributes written on it, as its ignored.
  if (auto attr = value->getGlobalActorAttr()) {
    if (!attr->first->isImplicit()) {
      value->diagnose(diag::isolated_parameter_combined_global_actor_attr,
                      value->getDescriptiveKind())
          .fixItRemove(attr->first->getRangeWithAt())
          .warnUntilSwiftVersion(6);
    }
  }

  // Suggest removing `nonisolated` as it is also ignored
  if (auto attr = value->getAttrs().getAttribute<NonisolatedAttr>()) {
    if (!attr->isImplicit()) {
      value->diagnose(diag::isolated_parameter_combined_nonisolated,
                      value->getDescriptiveKind())
          .fixItRemove(attr->getRangeWithAt())
          .warnUntilSwiftVersion(6);
    }
  }
}

ActorIsolation ActorIsolationRequest::evaluate(
    Evaluator &evaluator, ValueDecl *value) const {
  auto &ctx = value->getASTContext();

  // If this declaration has actor-isolated "self", it's isolated to that
  // actor.
  if (evaluateOrDefault(evaluator, HasIsolatedSelfRequest{value}, false)) {
    auto actor = value->getDeclContext()->getSelfNominalTypeDecl();
    assert(actor && "could not find the actor that 'self' is isolated to");
    return ActorIsolation::forActorInstanceSelf(value);
  }

  // If this declaration has an isolated parameter, it's isolated to that
  // parameter.
  if (auto paramIdx = getIsolatedParamIndex(value)) {
    checkDeclWithIsolatedParameter(value);

    ParamDecl *param = getParameterList(value)->get(*paramIdx);
    Type paramType = param->getDeclContext()->mapTypeIntoContext(
        param->getInterfaceType());
    Type actorType;
    if (auto wrapped = paramType->getOptionalObjectType()) {
      actorType = wrapped;
    } else {
      actorType = paramType;
    }
    if (auto actor = actorType->getAnyActor())
      return ActorIsolation::forActorInstanceParameter(param, *paramIdx);
  }

  // Diagnose global state that is not either immutable plus Sendable or
  // isolated to a global actor.
  auto checkGlobalIsolation = [var = dyn_cast<VarDecl>(value)](
                                  ActorIsolation isolation) {
    // Diagnose only declarations in the same module.
    //
    // TODO: This should be factored out from ActorIsolationRequest into
    // either ActorIsolationChecker or DeclChecker.
    if (var && var->getLoc(/*SerializedOK*/false) &&
        var->getASTContext().LangOpts.hasFeature(Feature::GlobalConcurrency) &&
        !isolation.isGlobalActor() &&
        (isolation != ActorIsolation::NonisolatedUnsafe)) {
      auto *classDecl = var->getDeclContext()->getSelfClassDecl();
      const bool isActorType = classDecl && classDecl->isAnyActor();
      if (var->isGlobalStorage() && !isActorType) {
        auto *diagVar = var;
        if (auto *originalVar = var->getOriginalWrappedProperty()) {
          diagVar = originalVar;
        }

        bool diagnosed = false;
        if (var->isLet()) {
          auto type = var->getInterfaceType();
          diagnosed = diagnoseIfAnyNonSendableTypes(
              type, SendableCheckContext(var->getDeclContext()),
              /*inDerivedConformance=*/Type(), /*typeLoc=*/SourceLoc(),
              /*diagnoseLoc=*/var->getLoc(),
              diag::shared_immutable_state_decl, diagVar);
        } else {
          diagVar->diagnose(diag::shared_mutable_state_decl, diagVar)
              .warnUntilSwiftVersion(6);
          diagnosed = true;
        }

        // If we diagnosed this global, tack on notes to suggest potential
        // courses of action.
        if (diagnosed) {
          if (!var->isLet()) {
            auto diag = diagVar->diagnose(diag::shared_state_make_immutable,
                                          diagVar);
            SourceLoc fixItLoc = getFixItLocForVarToLet(diagVar);
            if (fixItLoc.isValid()) {
              diag.fixItReplace(fixItLoc, "let");
            }
          }

          diagVar->diagnose(diag::shared_state_main_actor_node,
                            diagVar)
              .fixItInsert(diagVar->getAttributeInsertionLoc(false),
                           "@MainActor ");
          diagVar->diagnose(diag::shared_state_nonisolated_unsafe,
                            diagVar)
              .fixItInsert(diagVar->getAttributeInsertionLoc(true),
                           "nonisolated(unsafe) ");
        }
      }
    }
    return isolation;
  };

  auto isolationFromAttr = getIsolationFromAttributes(value);
  if (isolationFromAttr && isolationFromAttr->preconcurrency() &&
      !value->getAttrs().hasAttribute<PreconcurrencyAttr>()) {
    auto preconcurrency =
        new (ctx) PreconcurrencyAttr(/*isImplicit*/true);
    value->getAttrs().add(preconcurrency);
  }

  if (FuncDecl *fd = dyn_cast<FuncDecl>(value)) {
    // Main.main() and Main.$main are implicitly MainActor-protected.
    // Any other isolation is an error.
    std::optional<ActorIsolation> mainIsolation =
        getActorIsolationForMainFuncDecl(fd);
    if (mainIsolation) {
      if (isolationFromAttr && isolationFromAttr->isGlobalActor()) {
        if (!areTypesEqual(isolationFromAttr->getGlobalActor(),
                           mainIsolation->getGlobalActor())) {
          fd->getASTContext().Diags.diagnose(
              fd->getLoc(), diag::main_function_must_be_mainActor);
        }
      }
      return *mainIsolation;
    }
  }
  // If this declaration has one of the actor isolation attributes, report
  // that.
  if (isolationFromAttr) {
    // Classes with global actors have additional rules regarding inheritance.
    if (isolationFromAttr->isGlobalActor()) {
      if (auto classDecl = dyn_cast<ClassDecl>(value))
        checkClassGlobalActorIsolation(classDecl, *isolationFromAttr);
    }

    return checkGlobalIsolation(*isolationFromAttr);
  }

  // Determine the default isolation for this declaration, which may still be
  // overridden by other inference rules.
  ActorIsolation defaultIsolation = ActorIsolation::forUnspecified();

  if (auto func = dyn_cast<AbstractFunctionDecl>(value)) {
    // A @Sendable function is assumed to be actor-independent.
    if (func->isSendable()) {
      defaultIsolation = ActorIsolation::forNonisolated(/*unsafe=*/false);
    }
  }

  // When no other isolation applies, an actor's non-async init is independent
  if (auto nominal = value->getDeclContext()->getSelfNominalTypeDecl())
    if (nominal->isAnyActor())
      if (auto ctor = dyn_cast<ConstructorDecl>(value))
        if (!ctor->hasAsync())
          defaultIsolation = ActorIsolation::forNonisolated(/*unsafe=*/false);

  // Look for and remember the overridden declaration's isolation.
  std::optional<ActorIsolation> overriddenIso;
  ValueDecl *overriddenValue = value->getOverriddenDecl();
  if (overriddenValue) {
    // use the overridden decl's iso as the default isolation for this decl.
    defaultIsolation = getOverriddenIsolationFor(value);
    overriddenIso = defaultIsolation;
  }

  // Function used when returning an inferred isolation.
  auto inferredIsolation = [&](ActorIsolation inferred,
                               bool onlyGlobal = false) {
    // Invoke the body within checkGlobalIsolation to check the result.
    return checkGlobalIsolation([&] {
      // check if the inferred isolation is valid in the context of
      // its overridden isolation.
      if (overriddenValue) {
        // if the inferred isolation is not valid, then carry-over the
        // overridden declaration's isolation as this decl's inferred isolation.
        switch (validOverrideIsolation(value, inferred, overriddenValue,
                                       *overriddenIso)) {
        case OverrideIsolationResult::Allowed:
        case OverrideIsolationResult::Sendable:
          break;

        case OverrideIsolationResult::Disallowed:
          inferred = *overriddenIso;
          break;
        }
      }

      // Add an implicit attribute to capture the actor isolation that was
      // inferred, so that (e.g.) it will be printed and serialized.
      switch (inferred) {
      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
        // Stored properties cannot be non-isolated, so don't infer it.
        if (auto var = dyn_cast<VarDecl>(value)) {
          if (!var->isStatic() && var->hasStorage())
            return ActorIsolation::forUnspecified().withPreconcurrency(
                inferred.preconcurrency());
        }

        if (onlyGlobal) {
          return ActorIsolation::forUnspecified().withPreconcurrency(
              inferred.preconcurrency());
        }

        value->getAttrs().add(new (ctx) NonisolatedAttr(
            inferred == ActorIsolation::NonisolatedUnsafe, /*implicit=*/true));
        break;

      case ActorIsolation::Erased:
        llvm_unreachable("cannot infer erased isolation");

      case ActorIsolation::GlobalActor: {
        auto typeExpr =
            TypeExpr::createImplicit(inferred.getGlobalActor(), ctx);
        auto attr =
            CustomAttr::create(ctx, SourceLoc(), typeExpr, /*implicit=*/true);
        value->getAttrs().add(attr);

        if (inferred.preconcurrency() &&
            !value->getAttrs().hasAttribute<PreconcurrencyAttr>()) {
          auto preconcurrency =
              new (ctx) PreconcurrencyAttr(/*isImplicit*/true);
          value->getAttrs().add(preconcurrency);
        }

        break;
      }

      case ActorIsolation::ActorInstance:
      case ActorIsolation::Unspecified:
        if (onlyGlobal)
          return ActorIsolation::forUnspecified().withPreconcurrency(
              inferred.preconcurrency());

        // Nothing to do.
        break;
      }

      return inferred;
    }());
  };

  // If this is a local function, inherit the actor isolation from its
  // context if it global or was captured.
  if (auto func = dyn_cast<FuncDecl>(value)) {
    if (func->isLocalCapture() && !func->isSendable()) {
      switch (auto enclosingIsolation =
                  getActorIsolationOfContext(func->getDeclContext())) {
      case ActorIsolation::Nonisolated:
      case ActorIsolation::NonisolatedUnsafe:
      case ActorIsolation::Unspecified:
        // Do nothing.
        break;

      case ActorIsolation::Erased:
        llvm_unreachable("context cannot have erased isolation");

      case ActorIsolation::ActorInstance:
        return inferredIsolation(enclosingIsolation);

      case ActorIsolation::GlobalActor:
        return inferredIsolation(enclosingIsolation);
      }
    }
  }

  // If this is an accessor, use the actor isolation of its storage
  // declaration.
  if (auto accessor = dyn_cast<AccessorDecl>(value)) {
    return getActorIsolation(accessor->getStorage());
  }

  if (auto var = dyn_cast<VarDecl>(value)) {
    auto &ctx = var->getASTContext();
    if (!ctx.LangOpts.isConcurrencyModelTaskToThread() &&
        var->isTopLevelGlobal() &&
        (ctx.LangOpts.StrictConcurrencyLevel >=
             StrictConcurrency::Complete ||
         var->getDeclContext()->isAsyncContext())) {
      if (Type mainActor = var->getASTContext().getMainActorType())
        return inferredIsolation(
            ActorIsolation::forGlobalActor(mainActor))
                .withPreconcurrency(var->preconcurrency());
    }
    if (auto isolation = getActorIsolationFromWrappedProperty(var))
      return inferredIsolation(isolation);
  }

  // If this is a dynamic replacement for another function, use the
  // actor isolation of the function it replaces.
  if (auto replacedDecl = value->getDynamicallyReplacedDecl()) {
    if (auto isolation = getActorIsolation(replacedDecl))
      return inferredIsolation(isolation);
  }

  if (shouldInferAttributeInContext(value->getDeclContext())) {
    // If the declaration witnesses a protocol requirement that is isolated,
    // use that.
    if (auto witnessedIsolation = getIsolationFromWitnessedRequirements(value)) {
      if (auto inferred = inferredIsolation(*witnessedIsolation))
        return inferred;
    }

    // If the declaration is a class with a superclass that has specified
    // isolation, use that.
    if (auto classDecl = dyn_cast<ClassDecl>(value)) {
      if (auto superclassDecl = classDecl->getSuperclassDecl()) {
        auto superclassIsolation = getActorIsolation(superclassDecl);
        if (!superclassIsolation.isUnspecified()) {
          if (superclassIsolation.requiresSubstitution()) {
            Type superclassType = classDecl->getSuperclass();
            if (!superclassType)
              return ActorIsolation::forUnspecified();

            SubstitutionMap subs = superclassType->getMemberSubstitutionMap(
                classDecl->getModuleContext(), classDecl);
            superclassIsolation = superclassIsolation.subst(subs);
          }

          if (auto inferred = inferredIsolation(superclassIsolation))
            return inferred;
        }
      }
    }

    if (auto nominal = dyn_cast<NominalTypeDecl>(value)) {
      // If the declaration is a nominal type and any of the protocols to which
      // it directly conforms is isolated to a global actor, use that.
      if (auto conformanceIsolation = getIsolationFromConformances(nominal))
        if (auto inferred = inferredIsolation(*conformanceIsolation))
          return inferred;

      // For a protocol, inherit isolation from the directly-inherited
      // protocols.
      if (ctx.LangOpts.hasFeature(Feature::GlobalActorIsolatedTypesUsability)) {
        if (auto proto = dyn_cast<ProtocolDecl>(nominal)) {
          if (auto protoIsolation = getIsolationFromInheritedProtocols(proto)) {
            if (auto inferred = inferredIsolation(*protoIsolation))
              return inferred;
          }
        }
      }

      // Before Swift 6: If the declaration is a nominal type and any property
      // wrappers on its stored properties require isolation, use that.
      if (auto wrapperIsolation = getIsolationFromWrappers(nominal)) {
        if (auto inferred = inferredIsolation(*wrapperIsolation))
          return inferred;
      }
    }
  }

  // Infer isolation for a member.
  if (auto memberPropagation = getMemberIsolationPropagation(value)) {
    // If were only allowed to propagate global actors, do so.
    bool onlyGlobal =
        *memberPropagation == MemberIsolationPropagation::GlobalActor;

    // If the declaration is in an extension that has one of the isolation
    // attributes, use that.
    if (auto ext = dyn_cast<ExtensionDecl>(value->getDeclContext())) {
      if (auto isolationFromAttr = getIsolationFromAttributes(ext)) {
        return inferredIsolation(*isolationFromAttr, onlyGlobal);
      }
    }

    // If the declaration is in a nominal type (or extension thereof) that
    // has isolation, use that.
    if (auto selfTypeDecl = value->getDeclContext()->getSelfNominalTypeDecl()) {
      if (auto selfTypeIsolation = getActorIsolation(selfTypeDecl))
        return inferredIsolation(selfTypeIsolation, onlyGlobal);
    }
  }

  // @IBAction implies @MainActor(unsafe).
  if (value->getAttrs().hasAttribute<IBActionAttr>()) {
    ASTContext &ctx = value->getASTContext();
    if (Type mainActor = ctx.getMainActorType()) {
      return inferredIsolation(
          ActorIsolation::forGlobalActor(mainActor)
              .withPreconcurrency(true));
    }
  }

  // Default isolation for this member.
  return checkGlobalIsolation(defaultIsolation);
}

bool HasIsolatedSelfRequest::evaluate(
    Evaluator &evaluator, ValueDecl *value) const {
  // Only ever applies to members of actors.
  auto dc = value->getDeclContext();
  auto selfTypeDecl = dc->getSelfNominalTypeDecl();
  if (!selfTypeDecl || !selfTypeDecl->isAnyActor())
    return false;

  // For accessors, consider the storage declaration.
  if (auto accessor = dyn_cast<AccessorDecl>(value)) {
    // distributed thunks are nonisolated, although the attached to storage
    // will be 'distributed var' and therefore isolated to the distributed
    // actor. Therefore, if we're a thunk, don't look at the storage for
    // deciding about isolation of this function.
    if (accessor->isDistributedThunk()) {
      return false;
    }

    value = accessor->getStorage();
  }

  // If there is an isolated parameter, then "self" is not isolated.
  if (getIsolatedParamIndex(value))
    return false;

  // Check whether this member can be isolated to an actor at all.
  auto memberIsolation = getMemberIsolationPropagation(value);
  if (!memberIsolation)
    return false;

  switch (*memberIsolation) {
  case MemberIsolationPropagation::GlobalActor:
    return false;

  case MemberIsolationPropagation::AnyIsolation:
    break;
  }

  // Check whether the default isolation was overridden by any attributes on
  // this declaration.
  if (getIsolationFromAttributes(value))
    return false;

  // ... or its extension context.
  if (auto ext = dyn_cast<ExtensionDecl>(dc)) {
    if (getIsolationFromAttributes(ext))
      return false;
  }

  // If this is a variable, check for a property wrapper that alters its
  // isolation.
  if (auto var = dyn_cast<VarDecl>(value)) {
    switch (auto isolation = getActorIsolationFromWrappedProperty(var)) {
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
    case ActorIsolation::Unspecified:
      break;

    case ActorIsolation::GlobalActor:
      return false;

    case ActorIsolation::Erased:
      llvm_unreachable("property cannot have erased isolation");

    case ActorIsolation::ActorInstance:
      if (isolation.getActor() != selfTypeDecl)
        return false;
      break;
    }
  }

  if (auto ctor = dyn_cast<ConstructorDecl>(value)) {
    // When no other isolation applies to an actor's constructor,
    // then it is isolated only if it is async.
    if (!ctor->hasAsync())
      return false;
  }

  return true;
}

ActorIsolation
DefaultInitializerIsolation::evaluate(Evaluator &evaluator,
                                      VarDecl *var) const {
  if (var->isInvalid())
    return ActorIsolation::forUnspecified();

  Initializer *dc = nullptr;
  Expr *initExpr = nullptr;
  ActorIsolation enclosingIsolation;

  if (auto *pbd = var->getParentPatternBinding()) {
    if (!var->isParentInitialized())
      return ActorIsolation::forUnspecified();

    auto i = pbd->getPatternEntryIndexForVarDecl(var);

    dc = cast<Initializer>(pbd->getInitContext(i));
    initExpr = pbd->getCheckedAndContextualizedInit(i);
    enclosingIsolation = getActorIsolation(var);
  } else if (auto *param = dyn_cast<ParamDecl>(var)) {
    // If this parameter corresponds to a stored property for a
    // memberwise initializer, the default argument is the default
    // initializer expression.
    if (auto *property = param->getStoredProperty()) {
      // FIXME: Force computation of property wrapper initializers.
      if (auto *wrapped = property->getOriginalWrappedProperty())
        (void)property->getPropertyWrapperInitializerInfo();

      return property->getInitializerIsolation();
    }

    if (!param->hasDefaultExpr())
      return ActorIsolation::forUnspecified();

    dc = param->getDefaultArgumentInitContext();
    initExpr = param->getTypeCheckedDefaultExpr();
    enclosingIsolation =
        getActorIsolationOfContext(param->getDeclContext());
  }

  if (!dc || !initExpr)
    return ActorIsolation::forUnspecified();

  // If the default argument has isolation, it must match the
  // isolation of the decl context.
  ActorIsolationChecker checker(dc);
  auto requiredIsolation = checker.computeRequiredIsolation(initExpr);
  if (requiredIsolation.isActorIsolated()) {
    if (enclosingIsolation != requiredIsolation) {
      var->diagnose(
          diag::isolated_default_argument_context,
          requiredIsolation, enclosingIsolation)
        .warnUntilSwiftVersionIf(!isa<ParamDecl>(var), 6);
      return ActorIsolation::forUnspecified();
    }
  }

  return requiredIsolation;
}

void swift::checkOverrideActorIsolation(ValueDecl *value) {
  if (isa<TypeDecl>(value))
    return;

  auto overridden = value->getOverriddenDecl();
  if (!overridden)
    return;

  // Determine the actor isolation of the overriding function.
  auto isolation = getActorIsolation(value);
  
  // Determine the actor isolation of the overridden function.
  auto overriddenIsolation = getOverriddenIsolationFor(value);
  switch (validOverrideIsolation(
              value, isolation, overridden, overriddenIsolation)) {
  case OverrideIsolationResult::Allowed:
    return;

  case OverrideIsolationResult::Sendable:
    // Check that the results of the overriding method are sendable
    diagnoseNonSendableTypesInReference(
        /*base=*/nullptr,
        getDeclRefInContext(value), value->getInnermostDeclContext(),
        value->getLoc(), SendableCheckReason::Override,
        getActorIsolation(value), FunctionCheckKind::Results);

    // Check that the parameters of the overridden method are sendable
    diagnoseNonSendableTypesInReference(
        /*base=*/nullptr,
        getDeclRefInContext(overridden), overridden->getInnermostDeclContext(),
        overridden->getLoc(), SendableCheckReason::Override,
        getActorIsolation(value), FunctionCheckKind::Params,
        value->getLoc());
    return;

  case OverrideIsolationResult::Disallowed:
    // Diagnose below.
    break;
  }

  // Isolation mismatch. Diagnose it.
  DiagnosticBehavior behavior = DiagnosticBehavior::Unspecified;
  if (overridden->hasClangNode() && !overriddenIsolation) {
    behavior = SendableCheckContext(value->getInnermostDeclContext())
        .defaultDiagnosticBehavior();
  }

  value->diagnose(
      diag::actor_isolation_override_mismatch, isolation,
      value, overriddenIsolation)
    .limitBehaviorUntilSwiftVersion(behavior, 6);
  overridden->diagnose(diag::overridden_here);
}

bool swift::contextRequiresStrictConcurrencyChecking(
    const DeclContext *dc,
    llvm::function_ref<Type(const AbstractClosureExpr *)> getType,
    llvm::function_ref<bool(const ClosureExpr *)> isolatedByPreconcurrency) {
  switch (dc->getASTContext().LangOpts.StrictConcurrencyLevel) {
  case StrictConcurrency::Complete:
    return true;

  case StrictConcurrency::Targeted:
  case StrictConcurrency::Minimal:
    // Check below to see if the context has adopted concurrency features.
    break;
  }

  while (!dc->isModuleScopeContext()) {
    if (auto closure = dyn_cast<AbstractClosureExpr>(dc)) {
      // A closure with an explicit global actor, async, or Sendable
      // uses concurrency features.
      if (auto explicitClosure = dyn_cast<ClosureExpr>(closure)) {
        if (getExplicitGlobalActor(const_cast<ClosureExpr *>(explicitClosure)))
          return true;

        // Don't take any more cues if this only got its type information by
        // being provided to a `@preconcurrency` operation.
        if (isolatedByPreconcurrency(explicitClosure)) {
          dc = dc->getParent();
          continue;
        }

        if (auto type = getType(closure)) {
          if (auto fnType = type->getAs<AnyFunctionType>())
            if (fnType->isAsync() || fnType->isSendable())
              return true;
        }
      }

      // Async and @Sendable closures use concurrency features.
      if (closure->isBodyAsync() || closure->isSendable())
        return true;
    } else if (auto decl = dc->getAsDecl()) {
      // If any isolation attributes are present, we're using concurrency
      // features.
      if (hasExplicitIsolationAttribute(decl))
        return true;

      // Extensions of explicitly isolated types are using concurrency
      // features.
      if (auto *extension = dyn_cast<ExtensionDecl>(decl)) {
        auto *nominal = extension->getExtendedNominal();
        if (nominal && hasExplicitIsolationAttribute(nominal) &&
            !getActorIsolation(nominal).preconcurrency())
          return true;
      }

      if (auto func = dyn_cast<AbstractFunctionDecl>(decl)) {
        // Async and concurrent functions use concurrency features.
        if (func->hasAsync() || func->isSendable())
          return true;

        // If we're in an accessor declaration, also check the storage
        // declaration.
        if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
          if (hasExplicitIsolationAttribute(accessor->getStorage()))
            return true;
        }
      }
    }

    // If we're in an actor, we're using concurrency features.
    if (auto nominal = dc->getSelfNominalTypeDecl()) {
      if (nominal->isActor())
        return true;
    }

    // Keep looking.
    dc = dc->getParent();
  }

  return false;
}

/// Check the instance storage of the given nominal type to verify whether
/// it is comprised only of Sendable instance storage.
static bool checkSendableInstanceStorage(
    NominalTypeDecl *nominal, DeclContext *dc, SendableCheck check) {
  // Raw storage is assumed not to be sendable.
  if (auto sd = dyn_cast<StructDecl>(nominal)) {
    if (auto rawLayout = sd->getAttrs().getAttribute<RawLayoutAttr>()) {
      auto behavior = SendableCheckContext(
            dc, check).defaultDiagnosticBehavior();
      if (!isImplicitSendableCheck(check)
          && SendableCheckContext(dc, check)
               .defaultDiagnosticBehavior() != DiagnosticBehavior::Ignore) {
        sd->diagnose(diag::sendable_raw_storage, sd->getName())
          .limitBehaviorUntilSwiftVersion(behavior, 6);
      }
      return true;
    }
  }

  // Stored properties of structs and classes must have
  // Sendable-conforming types.
  class Visitor: public StorageVisitor {
  public:
    bool invalid = false;
    NominalTypeDecl *nominal;
    DeclContext *dc;
    SendableCheck check;
    const LangOptions &langOpts;

    Visitor(NominalTypeDecl *nominal, DeclContext *dc, SendableCheck check)
      : StorageVisitor(), nominal(nominal), dc(dc), check(check),
        langOpts(nominal->getASTContext().LangOpts) { }

    /// Handle a stored property.
    bool operator()(VarDecl *property, Type propertyType) override {
      ActorIsolation isolation = getActorIsolation(property);

      // 'nonisolated' properties are always okay in 'Sendable' types because
      // they can be accessed from anywhere. Note that 'nonisolated' without
      // '(unsafe)' can only be applied to immutable, 'Sendable' properties.
      if (isolation.isNonisolated())
        return false;

      // Classes with mutable properties are Sendable if property is
      // actor-isolated
      if (isa<ClassDecl>(nominal)) {
        if (property->supportsMutation() && isolation.isUnspecified()) {
          auto behavior =
              SendableCheckContext(dc, check).defaultDiagnosticBehavior();
          if (behavior != DiagnosticBehavior::Ignore) {
            property
                ->diagnose(diag::concurrent_value_class_mutable_property,
                           property->getName(), nominal)
                .limitBehaviorUntilSwiftVersion(behavior, 6);
          }
          invalid = invalid || (behavior == DiagnosticBehavior::Unspecified);
          return true;
        }

        if (!(isolation.isNonisolated() || isolation.isUnspecified())) {
          return false; // skip sendable check on actor-isolated properties
        }
      }

      // Check that the property type is Sendable.
      SendableCheckContext context(dc, check);
      diagnoseNonSendableTypes(
          propertyType, context,
          /*inDerivedConformance*/Type(), property->getLoc(),
          [&](Type type, DiagnosticBehavior behavior) {
            auto preconcurrency =
                context.preconcurrencyBehavior(type->getAnyNominal());
            if (isImplicitSendableCheck(check)) {
              // If this is for an externally-visible conformance, fail.
              if (check == SendableCheck::ImplicitForExternallyVisible) {
                invalid = true;
                return true;
              }

              // If we are to ignore this diagnostic, just continue.
              if (behavior == DiagnosticBehavior::Ignore ||
                  preconcurrency == DiagnosticBehavior::Ignore)
                return true;

              invalid = true;
              return true;
            }

            property->diagnose(diag::non_concurrent_type_member,
                               propertyType, false, property->getName(),
                               nominal)
                .limitBehaviorUntilSwiftVersion(behavior, 6)
                .limitBehaviorIf(preconcurrency);
            return false;
          });

      if (invalid) {
        // For implicit checks, bail out early if anything failed.
        if (isImplicitSendableCheck(check))
          return true;
      }

      return false;
    }

    /// Handle an enum associated value.
    bool operator()(EnumElementDecl *element, Type elementType) override {
      SendableCheckContext context (dc, check);
      diagnoseNonSendableTypes(
          elementType, context,
          /*inDerivedConformance*/Type(), element->getLoc(),
          [&](Type type, DiagnosticBehavior behavior) {
            auto preconcurrency =
                context.preconcurrencyBehavior(type->getAnyNominal());
            if (isImplicitSendableCheck(check)) {
              // If this is for an externally-visible conformance, fail.
              if (check == SendableCheck::ImplicitForExternallyVisible) {
                invalid = true;
                return true;
              }

              // If we are to ignore this diagnostic, just continue.
              if (behavior == DiagnosticBehavior::Ignore ||
                  preconcurrency == DiagnosticBehavior::Ignore)
                return true;

              invalid = true;
              return true;
            }

            element->diagnose(diag::non_concurrent_type_member, type,
                              true, element->getName(), nominal)
                .limitBehaviorUntilSwiftVersion(behavior, 6)
                .limitBehaviorIf(preconcurrency);
            return false;
          });

      if (invalid) {
        // For implicit checks, bail out early if anything failed.
        if (isImplicitSendableCheck(check))
          return true;
      }

      return false;
    }
  } visitor(nominal, dc, check);

  return visitor.visit(nominal, dc) || visitor.invalid;
}

bool swift::checkSendableConformance(
    ProtocolConformance *conformance, SendableCheck check) {
  auto conformanceDC = conformance->getDeclContext();
  auto nominal = conformance->getType()->getAnyNominal();
  if (!nominal)
    return false;

  // If this is an always-unavailable conformance, there's nothing to check.
  if (auto ext = dyn_cast<ExtensionDecl>(conformanceDC)) {
    if (AvailableAttr::isUnavailable(ext))
      return false;
  }

  bool isUnchecked = false;
  if (auto *normal = conformance->getRootNormalConformance())
    isUnchecked = normal->isUnchecked();

  if (isUnchecked) {
    // Warn if inferred or inherited '@unchecked Sendable' is not restated.
    // Beyond that, '@unchecked Sendable' requires no further checking.

    if (!isa<InheritedProtocolConformance>(conformance))
      return false;

    auto statesUnchecked =
      [](InheritedTypes inheritedTypes, DeclContext *dc) -> bool {
        for (auto i : inheritedTypes.getIndices()) {
          auto inheritedType =
              TypeResolution::forInterface(
                  dc,
                  TypeResolverContext::Inherited,
                  /*unboundTyOpener*/ nullptr,
                  /*placeholderHandler*/ nullptr,
                  /*packElementOpener*/ nullptr)
              .resolveType(inheritedTypes.getTypeRepr(i));

          if (!inheritedType || inheritedType->hasError())
            continue;

          if (inheritedType->getKnownProtocol() != KnownProtocolKind::Sendable)
            continue;

          if (inheritedTypes.getEntry(i).isUnchecked())
            return true;
        }

        return false;
      };

    if (statesUnchecked(nominal->getInherited(), nominal))
      return false;

    for (auto *extension : nominal->getExtensions()) {
      if (statesUnchecked(extension->getInherited(), extension))
        return false;
    }

    nominal->diagnose(diag::restate_unchecked_sendable,
                      nominal->getName());
    return false;
  }

  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if (classDecl) {
    // Actors implicitly conform to Sendable and protect their state.
    if (classDecl->isActor())
      return false;
  }

  // Global-actor-isolated types can be Sendable. We do not check the
  // instance data because it's all isolated to the global actor.
  switch (getActorIsolation(nominal)) {
  case ActorIsolation::Unspecified:
  case ActorIsolation::ActorInstance:
  case ActorIsolation::Nonisolated:
  case ActorIsolation::NonisolatedUnsafe:
    break;

  case ActorIsolation::Erased:
    llvm_unreachable("type cannot have erased isolation");

  case ActorIsolation::GlobalActor:
    return false;
  }

  // Sendable can only be used in the same source file.
  auto conformanceDecl = conformanceDC->getAsDecl();
  SendableCheckContext checkContext(conformanceDC, check);
  DiagnosticBehavior behavior = checkContext.defaultDiagnosticBehavior();
  if (conformance->getSourceKind() == ConformanceEntryKind::Implied &&
      conformance->getProtocol()->isSpecificProtocol(
          KnownProtocolKind::Sendable)) {
    if (auto optBehavior = checkContext.preconcurrencyBehavior(
            nominal, /*ignoreExplicitConformance=*/true))
      behavior = *optBehavior;
  }

  if (conformanceDC->getOutermostParentSourceFile() &&
      conformanceDC->getOutermostParentSourceFile() !=
      nominal->getOutermostParentSourceFile()) {
    conformanceDecl->diagnose(diag::concurrent_value_outside_source_file,
                              nominal)
      .limitBehaviorUntilSwiftVersion(behavior, 6);

    if (behavior == DiagnosticBehavior::Unspecified)
      return true;
  }

  if (classDecl && classDecl->getParentSourceFile()) {
    bool isInherited = isa<InheritedProtocolConformance>(conformance);

    // An non-final class cannot conform to `Sendable`.
    if (!classDecl->isSemanticallyFinal()) {
      classDecl->diagnose(diag::concurrent_value_nonfinal_class,
                          classDecl->getName())
        .limitBehaviorUntilSwiftVersion(behavior, 6);

      if (behavior == DiagnosticBehavior::Unspecified)
        return true;
    }

    if (!isInherited) {
      // A 'Sendable' class cannot inherit from another class, although
      // we allow `NSObject` for Objective-C interoperability.
      if (auto superclassDecl = classDecl->getSuperclassDecl()) {
        if (!superclassDecl->isNSObject()) {
          classDecl
              ->diagnose(diag::concurrent_value_inherit,
                         nominal->getASTContext().LangOpts.EnableObjCInterop,
                         classDecl->getName())
              .limitBehaviorUntilSwiftVersion(behavior, 6);

          if (behavior == DiagnosticBehavior::Unspecified)
            return true;
        }
      }
    }
  }

  return checkSendableInstanceStorage(nominal, conformanceDC, check);
}

/// Add "unavailable" attributes to the given extension.
static void addUnavailableAttrs(ExtensionDecl *ext, NominalTypeDecl *nominal) {
  ASTContext &ctx = nominal->getASTContext();
  llvm::VersionTuple noVersion;

  // Add platform-version-specific @available attributes. Search from nominal
  // type declaration through its enclosing declarations to find the first one
  // with platform-specific attributes.
  for (Decl *enclosing = nominal;
       enclosing;
       enclosing = enclosing->getDeclContext()
           ? enclosing->getDeclContext()->getAsDecl()
           : nullptr) {
    bool anyPlatformSpecificAttrs = false;
    for (auto available: enclosing->getAttrs().getAttributes<AvailableAttr>()) {
      if (available->Platform == PlatformKind::none)
        continue;

      auto attr = new (ctx) AvailableAttr(
          SourceLoc(), SourceRange(),
          available->Platform,
          available->Message,
          "", nullptr,
          available->Introduced.value_or(noVersion), SourceRange(),
          available->Deprecated.value_or(noVersion), SourceRange(),
          available->Obsoleted.value_or(noVersion), SourceRange(),
          PlatformAgnosticAvailabilityKind::Unavailable,
          /*implicit=*/true,
          available->IsSPI);
      ext->getAttrs().add(attr);
      anyPlatformSpecificAttrs = true;
    }

    // If we found any platform-specific availability attributes, we're done.
    if (anyPlatformSpecificAttrs)
      break;
  }

  // Add the blanket "unavailable".

  auto attr = new (ctx) AvailableAttr(SourceLoc(), SourceRange(),
                                      PlatformKind::none, "", "", nullptr,
                                      noVersion, SourceRange(),
                                      noVersion, SourceRange(),
                                      noVersion, SourceRange(),
                                      PlatformAgnosticAvailabilityKind::Unavailable,
                                      false,
                                      false);
  ext->getAttrs().add(attr);
}

ProtocolConformance *swift::deriveImplicitSendableConformance(
    Evaluator &evaluator, NominalTypeDecl *nominal) {
  // Protocols never get implicit Sendable conformances.
  if (isa<ProtocolDecl>(nominal))
    return nullptr;

  // Actor types are always Sendable; they don't get it via this path.
  auto classDecl = dyn_cast<ClassDecl>(nominal);
  if (classDecl && classDecl->isActor())
    return nullptr;

  // Check whether we can infer conformance at all.
  if (auto *file = dyn_cast<FileUnit>(nominal->getModuleScopeContext())) {
    switch (file->getKind()) {
    case FileUnitKind::Source:
      // Check what kind of source file we have.
      if (auto sourceFile = nominal->getParentSourceFile()) {
        switch (sourceFile->Kind) {
        case SourceFileKind::Interface:
          // Interfaces have explicitly called-out Sendable conformances.
          return nullptr;

        case SourceFileKind::DefaultArgument:
        case SourceFileKind::Library:
        case SourceFileKind::MacroExpansion:
        case SourceFileKind::Main:
        case SourceFileKind::SIL:
          break;
        }
      }
      break;

    case FileUnitKind::Builtin:
    case FileUnitKind::SerializedAST:
    case FileUnitKind::Synthesized:
      // Explicitly-handled modules don't infer Sendable conformances.
      return nullptr;

    case FileUnitKind::ClangModule:
    case FileUnitKind::DWARFModule:
      // Infer conformances for imported modules.
      break;
    }
  } else {
    return nullptr;
  }

  ASTContext &ctx = nominal->getASTContext();
  auto proto = ctx.getProtocol(KnownProtocolKind::Sendable);
  if (!proto)
    return nullptr;

  // Local function to form the implicit conformance.
  auto formConformance = [&](const DeclAttribute *attrMakingUnavailable)
        -> NormalProtocolConformance * {
    DeclContext *conformanceDC = nominal;
    if (attrMakingUnavailable) {
      // Conformance availability is currently tied to the declaring extension.
      // FIXME: This is a hack--we should give conformances real availability.
      auto inherits = ctx.AllocateCopy(llvm::ArrayRef(
          InheritedEntry(TypeLoc::withoutLoc(proto->getDeclaredInterfaceType()),
                         /*isUnchecked*/ true, /*isRetroactive=*/false,
                         /*isPreconcurrency=*/false)));
      // If you change the use of AtLoc in the ExtensionDecl, make sure you
      // update isNonSendableExtension() in ASTPrinter.
      auto extension = ExtensionDecl::create(ctx, attrMakingUnavailable->AtLoc,
                                             nullptr, inherits,
                                             nominal->getModuleScopeContext(),
                                             nullptr);
      extension->setImplicit();
      addUnavailableAttrs(extension, nominal);

      ctx.evaluator.cacheOutput(ExtendedTypeRequest{extension},
                                nominal->getDeclaredType());
      ctx.evaluator.cacheOutput(ExtendedNominalRequest{extension},
                                std::move(nominal));
      nominal->addExtension(extension);

      // Make it accessible to getTopLevelDecls()
      if (auto file = dyn_cast<FileUnit>(nominal->getModuleScopeContext()))
        file->getOrCreateSynthesizedFile().addTopLevelDecl(extension);

      conformanceDC = extension;
    }

    auto conformance = ctx.getNormalConformance(
        nominal->getDeclaredInterfaceType(), proto, nominal->getLoc(),
        conformanceDC, ProtocolConformanceState::Complete,
        /*isUnchecked=*/attrMakingUnavailable != nullptr,
        /*isPreconcurrency=*/false);
    conformance->setSourceKindAndImplyingConformance(
        ConformanceEntryKind::Synthesized, nullptr);

    nominal->registerProtocolConformance(conformance, /*synthesized=*/true);
    return conformance;
  };

  // If this is a class, check the superclass. If it's already Sendable,
  // form an inherited conformance.
  if (classDecl) {
    if (Type superclass = classDecl->getSuperclass()) {
      auto classModule = classDecl->getParentModule();
      auto inheritedConformance = classModule->checkConformance(
          classDecl->mapTypeIntoContext(superclass),
          proto, /*allowMissing=*/false);
      if (inheritedConformance.hasUnavailableConformance())
        inheritedConformance = ProtocolConformanceRef::forInvalid();

      if (inheritedConformance) {
        inheritedConformance = inheritedConformance
            .mapConformanceOutOfContext();
        if (inheritedConformance.isConcrete()) {
          return ctx.getInheritedConformance(
              nominal->getDeclaredInterfaceType(),
              inheritedConformance.getConcrete());
        }
      }

      // Classes that add global actor isolation to non-Sendable
      // superclasses cannot be 'Sendable'.
      auto superclassDecl = classDecl->getSuperclassDecl();
      if (nominal->getGlobalActorAttr() && !superclassDecl->isNSObject()) {
        return nullptr;
      }
    }
  }

  // A non-protocol type with a global actor is implicitly Sendable.
  if (getActorIsolation(nominal).isGlobalActor()) {
    // Form the implicit conformance to Sendable.
    return formConformance(nullptr);
  }

  if (auto attr = nominal->getAttrs().getEffectiveSendableAttr()) {
    assert(!isa<SendableAttr>(attr) &&
           "Conformance should have been added by SynthesizedProtocolAttr!");
    return formConformance(cast<NonSendableAttr>(attr));
  }

  // Only structs and enums can get implicit Sendable conformances by
  // considering their instance data.
  if (!isa<StructDecl>(nominal) && !isa<EnumDecl>(nominal))
    return nullptr;

  SendableCheck check;

  // Okay to infer Sendable conformance for non-public types or when
  // specifically requested.
  if (nominal->getASTContext().LangOpts.EnableInferPublicSendable ||
      !nominal->getFormalAccessScope(
          /*useDC=*/nullptr, /*treatUsableFromInlineAsPublic=*/true)
            .isPublic()) {
    check = SendableCheck::Implicit;
  } else if (nominal->hasClangNode() ||
             nominal->getAttrs().hasAttribute<FixedLayoutAttr>() ||
             nominal->getAttrs().hasAttribute<FrozenAttr>()) {
    // @_frozen public types can also infer Sendable, but be more careful here.
    check = SendableCheck::ImplicitForExternallyVisible;
  } else {
    // No inference.
    return nullptr;
  }

  // Check the instance storage for Sendable conformance.
  if (checkSendableInstanceStorage(nominal, nominal, check))
    return nullptr;

  return formConformance(nullptr);
}

/// Apply @Sendable and/or @MainActor to the given parameter type.
static Type applyUnsafeConcurrencyToParameterType(
    Type type, bool sendable, bool mainActor) {
  if (Type objectType = type->getOptionalObjectType()) {
    return OptionalType::get(
        applyUnsafeConcurrencyToParameterType(objectType, sendable, mainActor));
  }

  auto fnType = type->getAs<FunctionType>();
  if (!fnType)
    return type;

  auto isolation = fnType->getIsolation();
  if (mainActor)
    isolation = FunctionTypeIsolation::forGlobalActor(
                  type->getASTContext().getMainActorType());

  return fnType->withExtInfo(fnType->getExtInfo()
                               .withSendable(sendable)
                               .withIsolation(isolation));
}

/// Determine whether the given name is that of a DispatchQueue operation that
/// takes a closure to be executed on the queue.
std::optional<DispatchQueueOperation>
swift::isDispatchQueueOperationName(StringRef name) {
  return llvm::StringSwitch<std::optional<DispatchQueueOperation>>(name)
      .Case("sync", DispatchQueueOperation::Normal)
      .Case("async", DispatchQueueOperation::Sendable)
      .Case("asyncAndWait", DispatchQueueOperation::Normal)
      .Case("asyncUnsafe", DispatchQueueOperation::Normal)
      .Case("asyncAfter", DispatchQueueOperation::Sendable)
      .Case("concurrentPerform", DispatchQueueOperation::Sendable)
      .Default(std::nullopt);
}

/// Determine whether this function is implicitly known to have its
/// parameters of function type be @_unsafeSendable.
///
/// This hard-codes knowledge of a number of functions that will
/// eventually have @_unsafeSendable and, eventually, @Sendable,
/// on their parameters of function type.
static bool hasKnownUnsafeSendableFunctionParams(AbstractFunctionDecl *func) {
  auto nominal = func->getDeclContext()->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  // DispatchQueue operations.
  auto nominalName = nominal->getName().str();
  if (nominalName == "DispatchQueue") {
    auto name = func->getBaseName().userFacingName();
    auto operation = isDispatchQueueOperationName(name);
    if (!operation)
      return false;

    switch (*operation) {
    case DispatchQueueOperation::Normal:
      return false;

    case DispatchQueueOperation::Sendable:
      return true;
    }
  }

  return false;
}

Type swift::adjustVarTypeForConcurrency(
    Type type, VarDecl *var, DeclContext *dc,
    llvm::function_ref<Type(const AbstractClosureExpr *)> getType,
    llvm::function_ref<bool(const ClosureExpr *)> isolatedByPreconcurrency) {
  if (!var->preconcurrency())
    return type;

  if (contextRequiresStrictConcurrencyChecking(
          dc, getType, isolatedByPreconcurrency))
    return type;

  bool isLValue = false;
  if (auto *lvalueType = type->getAs<LValueType>()) {
    type = lvalueType->getObjectType();
    isLValue = true;
  }

  type = type->stripConcurrency(/*recurse=*/false, /*dropGlobalActor=*/true);

  if (isLValue)
    type = LValueType::get(type);

  return type;
}

/// Adjust a function type for @_unsafeSendable, @_unsafeMainActor, and
/// @preconcurrency.
static AnyFunctionType *applyUnsafeConcurrencyToFunctionType(
    AnyFunctionType *fnType, ValueDecl *decl,
    bool inConcurrencyContext, unsigned numApplies, bool isMainDispatchQueue) {
  // Functions/subscripts/enum elements have function types to adjust.
  auto func = dyn_cast_or_null<AbstractFunctionDecl>(decl);
  auto subscript = dyn_cast_or_null<SubscriptDecl>(decl);

  if (!func && !subscript)
    return fnType;

  AnyFunctionType *outerFnType = nullptr;
  if ((subscript && numApplies > 1) || (func && func->hasImplicitSelfDecl())) {
    outerFnType = fnType;
    fnType = outerFnType->getResult()->castTo<AnyFunctionType>();

    if (numApplies > 0)
      --numApplies;
  }

  SmallVector<AnyFunctionType::Param, 4> newTypeParams;
  auto typeParams = fnType->getParams();
  auto paramDecls = func ? func->getParameters() : subscript->getIndices();
  assert(typeParams.size() == paramDecls->size());
  bool knownUnsafeParams = func && hasKnownUnsafeSendableFunctionParams(func);
  bool stripConcurrency =
      decl->preconcurrency() && !inConcurrencyContext;
  for (unsigned index : indices(typeParams)) {
    auto param = typeParams[index];

    // Determine whether the resulting parameter should be @Sendable or
    // @MainActor. @Sendable occurs only in concurrency contents, while
    // @MainActor occurs in concurrency contexts or those where we have an
    // application.
    bool addSendable = knownUnsafeParams && inConcurrencyContext;
    bool addMainActor = isMainDispatchQueue &&
        (inConcurrencyContext || numApplies >= 1);
    Type newParamType = param.getPlainType();
    if (addSendable || addMainActor) {
      newParamType = applyUnsafeConcurrencyToParameterType(
        param.getPlainType(), addSendable, addMainActor);
    } else if (stripConcurrency && numApplies == 0) {
      newParamType = param.getPlainType()->stripConcurrency(
          /*recurse=*/false, /*dropGlobalActor=*/numApplies == 0);
    }

    if (!newParamType || newParamType->isEqual(param.getPlainType())) {
      // If any prior parameter has changed, record this one.
      if (!newTypeParams.empty())
        newTypeParams.push_back(param);

      continue;
    }

    // If this is the first parameter to have changed, copy all of the others
    // over.
    if (newTypeParams.empty()) {
      newTypeParams.append(typeParams.begin(), typeParams.begin() + index);
    }

    // Transform the parameter type.
    newTypeParams.push_back(param.withType(newParamType));
  }

  // Compute the new result type.
  Type newResultType = fnType->getResult();
  if (stripConcurrency) {
    newResultType = newResultType->stripConcurrency(
        /*recurse=*/false, /*dropGlobalActor=*/true);

    if (!newResultType->isEqual(fnType->getResult()) && newTypeParams.empty()) {
      newTypeParams.append(typeParams.begin(), typeParams.end());
    }
  }

  // If we didn't change any parameters, we're done.
  if (newTypeParams.empty() && newResultType->isEqual(fnType->getResult())) {
    return outerFnType ? outerFnType : fnType;
  }

  // Rebuild the (inner) function type.
  fnType = FunctionType::get(
      newTypeParams, newResultType, fnType->getExtInfo());

  if (!outerFnType)
    return fnType;

  // Rebuild the outer function type.
  if (auto genericFnType = dyn_cast<GenericFunctionType>(outerFnType)) {
    return GenericFunctionType::get(
        genericFnType->getGenericSignature(), outerFnType->getParams(),
        Type(fnType), outerFnType->getExtInfo());
  }

  return FunctionType::get(
      outerFnType->getParams(), Type(fnType), outerFnType->getExtInfo());
}

AnyFunctionType *swift::adjustFunctionTypeForConcurrency(
    AnyFunctionType *fnType, ValueDecl *decl, DeclContext *dc,
    unsigned numApplies, bool isMainDispatchQueue,
    llvm::function_ref<Type(const AbstractClosureExpr *)> getType,
    llvm::function_ref<bool(const ClosureExpr *)> isolatedByPreconcurrency,
    llvm::function_ref<Type(Type)> openType) {
  // Apply unsafe concurrency features to the given function type.
  bool strictChecking = contextRequiresStrictConcurrencyChecking(
      dc, getType, isolatedByPreconcurrency);

  fnType = applyUnsafeConcurrencyToFunctionType(
      fnType, decl, strictChecking, numApplies, isMainDispatchQueue);
  Type globalActorType;
  if (decl) {
    switch (auto isolation = getActorIsolation(decl)) {
    case ActorIsolation::ActorInstance:
      // The function type may or may not have parameter isolation.
      return fnType;

    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
    case ActorIsolation::Unspecified:
      assert(fnType->getIsolation().isNonIsolated());
      return fnType;

    case ActorIsolation::Erased:
      llvm_unreachable("declaration cannot have erased isolation");

    case ActorIsolation::GlobalActor:
      // For preconcurrency, only treat as global-actor-qualified
      // within code that has adopted Swift Concurrency features.
      if (!strictChecking && isolation.preconcurrency())
        return fnType;

      globalActorType = openType(isolation.getGlobalActor());
      break;
    }
  }

  auto isolation = FunctionTypeIsolation::forGlobalActor(globalActorType);

  // If there's no implicit "self" declaration, apply the isolation to
  // the outermost function type.
  bool hasImplicitSelfDecl = decl && (isa<EnumElementDecl>(decl) ||
      (isa<AbstractFunctionDecl>(decl) &&
       cast<AbstractFunctionDecl>(decl)->hasImplicitSelfDecl()));
  if (!hasImplicitSelfDecl) {
    return fnType->withExtInfo(fnType->getExtInfo().withIsolation(isolation));
  }

  // Dig out the inner function type.
  auto innerFnType = fnType->getResult()->getAs<AnyFunctionType>();
  if (!innerFnType)
    return fnType;

  // Update the inner function type with the isolation.
  innerFnType = innerFnType->withExtInfo(
      innerFnType->getExtInfo().withIsolation(isolation));

  // Rebuild the outer function type around it.
  if (auto genericFnType = dyn_cast<GenericFunctionType>(fnType)) {
    return GenericFunctionType::get(
        genericFnType->getGenericSignature(), fnType->getParams(),
        Type(innerFnType), fnType->getExtInfo());
  }

  return FunctionType::get(
      fnType->getParams(), Type(innerFnType), fnType->getExtInfo());
}

bool swift::completionContextUsesConcurrencyFeatures(const DeclContext *dc) {
  return contextRequiresStrictConcurrencyChecking(
      dc, [](const AbstractClosureExpr *) {
        return Type();
      },
    [](const ClosureExpr *closure) {
      return closure->isIsolatedByPreconcurrency();
    });
}

/// Find the directly-referenced parameter or capture of a parameter for
/// for the given expression.
VarDecl *swift::getReferencedParamOrCapture(
    Expr *expr,
    llvm::function_ref<Expr *(OpaqueValueExpr *)> getExistentialValue,
    llvm::function_ref<VarDecl *()> getCurrentIsolatedVar) {
  // Look through identity expressions and implicit conversions.
  Expr *prior;

  do {
    prior = expr;

    expr = expr->getSemanticsProvidingExpr();

    if (auto conversion = dyn_cast<ImplicitConversionExpr>(expr))
      expr = conversion->getSubExpr();

    // Map opaque values.
    if (auto opaqueValue = dyn_cast<OpaqueValueExpr>(expr)) {
      if (auto *value = getExistentialValue(opaqueValue))
        expr = value;
    }
  } while (prior != expr);

  // 'super' references always act on a 'self' variable.
  if (auto super = dyn_cast<SuperRefExpr>(expr))
    return super->getSelf();

  // Declaration references to a variable.
  if (auto declRef = dyn_cast<DeclRefExpr>(expr))
    return dyn_cast<VarDecl>(declRef->getDecl());

  // The current context isolation expression (#isolation) always
  // corresponds to the isolation of the given code.
  if (isa<CurrentContextIsolationExpr>(expr))
    return getCurrentIsolatedVar();

  return nullptr;
}

bool swift::isPotentiallyIsolatedActor(
    VarDecl *var, llvm::function_ref<bool(ParamDecl *)> isIsolated) {
  if (!var)
    return false;

  if (var->getName().str().equals("__secretlyKnownToBeLocal")) {
    // FIXME(distributed): we did a dynamic check and know that this actor is
    //   local, but we can't express that to the type system; the real
    //   implementation will have to mark 'self' as "known to be local" after
    //   an is-local check.
    return true;
  }

  if (auto param = dyn_cast<ParamDecl>(var))
    return isIsolated(param);

  // If this is a captured 'self', check whether the original 'self' is
  // isolated.
  if (var->isSelfParamCapture())
    return var->isSelfParamCaptureIsolated();

  return false;
}

/// Determine the actor isolation used when we are referencing the given
/// declaration.
static ActorIsolation getActorIsolationForReference(ValueDecl *decl,
                                                    const DeclContext *fromDC) {
  auto declIsolation = getActorIsolation(decl);

  // If the isolation is preconcurrency global actor, adjust it based on
  // context itself. For contexts that require strict checking, treat it as
  // global actor isolation. Otherwise, treat it as unspecified isolation.
  if (declIsolation == ActorIsolation::GlobalActor &&
      declIsolation.preconcurrency()) {
    if (!contextRequiresStrictConcurrencyChecking(
            fromDC,
            [](const AbstractClosureExpr *closure) {
              return closure->getType();
            },
            [](const ClosureExpr *closure) {
              return closure->isIsolatedByPreconcurrency();
            })) {
      declIsolation = ActorIsolation::forUnspecified();
    }
  }

  // A constructor that is not explicitly 'nonisolated' is treated as
  // isolated from the perspective of the referencer.
  //
  // FIXME: The current state is that even `nonisolated` initializers are
  // externally treated as being on the actor, even though this model isn't
  // consistent. We'll fix it later.
  if (auto ctor = dyn_cast<ConstructorDecl>(decl)) {
    // If the constructor is part of an actor, references to it are treated
    // as needing to enter the actor.
    if (auto nominal = ctor->getDeclContext()->getSelfNominalTypeDecl()) {
      if (nominal->isAnyActor())
        return ActorIsolation::forActorInstanceSelf(decl);
    }

    // Fall through to treat initializers like any other declaration.
  }

  // A 'nonisolated let' within an actor is treated as isolated if
  // the access is outside the module or if the property type is not
  // 'Sendable'.
  //
  // FIXME: getActorIsolation(decl) should treat these as isolated.
  // FIXME: Expand this out to local variables?
  if (auto var = dyn_cast<VarDecl>(decl)) {
    auto *fromModule = fromDC->getParentModule();
    ActorReferenceResult::Options options = std::nullopt;
    if (varIsSafeAcrossActors(fromModule, var, declIsolation, options) &&
        var->getTypeInContext()->isSendableType())
      return ActorIsolation::forNonisolated(/*unsafe*/false);

    if (var->isLet() && isStoredProperty(var) &&
        declIsolation.isNonisolated()) {
      if (auto nominal = var->getDeclContext()->getSelfNominalTypeDecl()) {
        if (nominal->isAnyActor())
          return ActorIsolation::forActorInstanceSelf(decl);

        auto nominalIsolation = getActorIsolation(nominal);
        if (nominalIsolation.isGlobalActor())
          return getActorIsolationForReference(nominal, fromDC);
      }
    }
  }

  return declIsolation;
}

/// Determine whether this declaration always throws.
bool swift::isThrowsDecl(ConcreteDeclRef declRef) {
  auto decl = declRef.getDecl();

  // An async function is asynchronously accessed.
  if (auto func = dyn_cast<AbstractFunctionDecl>(decl))
    return func->hasThrows();

  // A computed property or subscript that has an 'async' getter
  // is asynchronously accessed.
  if (auto storageDecl = dyn_cast<AbstractStorageDecl>(decl)) {
    if (auto effectfulGetter = storageDecl->getEffectfulGetAccessor())
      return effectfulGetter->hasThrows();
  }

  return false;
}

/// Determine whether a reference to this value isn't actually a value.
static bool isNonValueReference(const ValueDecl *value) {
  switch (value->getKind()) {
  case DeclKind::AssociatedType:
  case DeclKind::Class:
  case DeclKind::Enum:
  case DeclKind::Extension:
  case DeclKind::GenericTypeParam:
  case DeclKind::OpaqueType:
  case DeclKind::Protocol:
  case DeclKind::Struct:
  case DeclKind::TypeAlias:
  case DeclKind::EnumCase:
  case DeclKind::IfConfig:
  case DeclKind::Import:
  case DeclKind::InfixOperator:
  case DeclKind::Missing:
  case DeclKind::MissingMember:
  case DeclKind::Module:
  case DeclKind::PatternBinding:
  case DeclKind::PostfixOperator:
  case DeclKind::PoundDiagnostic:
  case DeclKind::PrecedenceGroup:
  case DeclKind::PrefixOperator:
  case DeclKind::TopLevelCode:
  case DeclKind::Destructor:
  case DeclKind::MacroExpansion:
    return true;

  case DeclKind::EnumElement:
  case DeclKind::Constructor:
  case DeclKind::Param:
  case DeclKind::Var:
  case DeclKind::Accessor:
  case DeclKind::Func:
  case DeclKind::Subscript:
  case DeclKind::Macro:
    return false;

  case DeclKind::BuiltinTuple:
    llvm_unreachable("BuiltinTupleDecl should not show up here");
  }
}

bool swift::isAccessibleAcrossActors(
    ValueDecl *value, const ActorIsolation &isolation,
    const DeclContext *fromDC, ActorReferenceResult::Options &options,
    std::optional<ReferencedActor> actorInstance) {
  // Initializers and enum elements are accessible across actors unless they
  // are global-actor qualified.
  if (isa<ConstructorDecl>(value) || isa<EnumElementDecl>(value)) {
    switch (isolation) {
    case ActorIsolation::ActorInstance:
    case ActorIsolation::Nonisolated:
    case ActorIsolation::NonisolatedUnsafe:
    case ActorIsolation::Unspecified:
      return true;

    case ActorIsolation::Erased:
      llvm_unreachable("declaration cannot have erased isolation");

    case ActorIsolation::GlobalActor:
      return false;
    }
  }

  // 'let' declarations are immutable, so some of them can be accessed across
  // actors.
  if (auto var = dyn_cast<VarDecl>(value)) {
    return varIsSafeAcrossActors(
        fromDC->getParentModule(), var, isolation, options);
  }

  return false;
}

bool swift::isAccessibleAcrossActors(
    ValueDecl *value, const ActorIsolation &isolation,
    const DeclContext *fromDC, std::optional<ReferencedActor> actorInstance) {
  ActorReferenceResult::Options options = std::nullopt;
  return isAccessibleAcrossActors(
      value, isolation, fromDC, options, actorInstance);
}

ActorReferenceResult ActorReferenceResult::forSameConcurrencyDomain(
    ActorIsolation isolation, Options options) {
  return ActorReferenceResult{SameConcurrencyDomain, options, isolation};
}

ActorReferenceResult ActorReferenceResult::forEntersActor(
    ActorIsolation isolation, Options options) {
  return ActorReferenceResult{EntersActor, options, isolation};
}

ActorReferenceResult ActorReferenceResult::forExitsActorToNonisolated(
    ActorIsolation isolation, Options options) {
  return ActorReferenceResult{ExitsActorToNonisolated, options, isolation};
}

// Determine if two actor isolation contexts are considered to be equivalent.
static bool equivalentIsolationContexts(
    const ActorIsolation &lhs, const ActorIsolation &rhs) {
  if (lhs == rhs)
    return true;

  if (lhs == ActorIsolation::ActorInstance &&
      rhs == ActorIsolation::ActorInstance &&
      lhs.isDistributedActor() == rhs.isDistributedActor())
    return true;

  return false;
}

ActorReferenceResult ActorReferenceResult::forReference(
    ConcreteDeclRef declRef, SourceLoc declRefLoc, const DeclContext *fromDC,
    std::optional<VarRefUseEnv> useKind,
    std::optional<ReferencedActor> actorInstance,
    std::optional<ActorIsolation> knownDeclIsolation,
    std::optional<ActorIsolation> knownContextIsolation,
    llvm::function_ref<ActorIsolation(AbstractClosureExpr *)>
        getClosureActorIsolation) {
  // If not provided, compute the isolation of the declaration, adjusted
  // for references.
  ActorIsolation declIsolation = ActorIsolation::forUnspecified();
  if (knownDeclIsolation) {
    declIsolation = *knownDeclIsolation;
  } else {
    declIsolation = getActorIsolationForReference(declRef.getDecl(), fromDC);
    if (declIsolation.requiresSubstitution())
      declIsolation = declIsolation.subst(declRef.getSubstitutions());
  }

  // Determine what adjustments we need to perform for cross-actor
  // references.
  Options options = std::nullopt;

  // FIXME: Actor constructors are modeled as isolated to the actor
  // so that Sendable checking is applied to their arguments, but the
  // call itself does not hop to another executor.
  if (auto ctor = dyn_cast<ConstructorDecl>(declRef.getDecl())) {
    if (auto nominal = ctor->getDeclContext()->getSelfNominalTypeDecl()) {
      if (nominal->isAnyActor())
        options |= Flags::OnlyArgsCrossIsolation;
    }
  }

  // If the entity we are referencing is not a value, we're in the same
  // concurrency domain.
  if (isNonValueReference(declRef.getDecl()))
    return forSameConcurrencyDomain(declIsolation, options);

  // Compute the isolation of the context, if not provided.
  ActorIsolation contextIsolation = ActorIsolation::forUnspecified();
  if (knownContextIsolation) {
    contextIsolation = *knownContextIsolation;
  } else {
    contextIsolation =
        getInnermostIsolatedContext(fromDC, getClosureActorIsolation);
  }

  // When the declaration is not actor-isolated, it can always be accessed
  // directly.
  if (!declIsolation.isActorIsolated()) {
    // If the declaration is asynchronous and we are in an actor-isolated
    // context (of any kind), then we exit the actor to the nonisolated context.
    if (isAsyncDecl(declRef) && contextIsolation.isActorIsolated() &&
        !declRef.getDecl()->getAttrs()
            .hasAttribute<UnsafeInheritExecutorAttr>())
      return forExitsActorToNonisolated(contextIsolation, options);

    // Otherwise, we stay in the same concurrency domain, whether on an actor
    // or in a task.
    return forSameConcurrencyDomain(declIsolation, options);
  }

  // The declaration we are accessing is actor-isolated. First, check whether
  // we are on the same actor already.
  if (actorInstance && declIsolation == ActorIsolation::ActorInstance &&
      declIsolation.getActorInstanceParameter() == 0) {
    // If this instance is isolated, we're in the same concurrency domain.
    if (actorInstance->isIsolated())
      return forSameConcurrencyDomain(declIsolation, options);
  } else if (equivalentIsolationContexts(declIsolation, contextIsolation)) {
    // The context isolation matches, so we are in the same concurrency
    // domain.
    return forSameConcurrencyDomain(declIsolation, options);
  }

  // Initializing an actor isolated stored property with a value effectively
  // passes that value from the init context into the actor isolated context.
  // It's only okay for the value to cross isolation boundaries if the property
  // type is Sendable. Note that if the init is a nonisolated actor init,
  // Sendable checking is already performed on arguments at the call-site.
  if ((declIsolation.isActorIsolated() && contextIsolation.isGlobalActor()) ||
      declIsolation.isGlobalActor()) {
    auto *init = dyn_cast<ConstructorDecl>(fromDC);
    auto *decl = declRef.getDecl();
    if (init && init->isDesignatedInit() && isStoredProperty(decl) &&
        (!actorInstance || actorInstance->isSelf())) {
      auto type =
          fromDC->mapTypeIntoContext(declRef.getDecl()->getInterfaceType());
      if (!type->isSendableType()) {
        // Treat the decl isolation as 'preconcurrency' to downgrade violations
        // to warnings, because violating Sendable here is accepted by the
        // Swift 5.9 compiler.
        options |= Flags::Preconcurrency;
        return forEntersActor(declIsolation, options);
      }
    }
  }

  // If there is an instance and it is checked by flow isolation, treat it
  // as being in the same concurrency domain.
  if (actorInstance &&
      checkedByFlowIsolation(
          fromDC, *actorInstance, declRef.getDecl(), declRefLoc, useKind))
    return forSameConcurrencyDomain(declIsolation, options);

  // If we are delegating to another initializer, treat them as being in the
  // same concurrency domain.
  // FIXME: This has a lot of overlap with both the stored-property checks
  // below and the flow-isolation checks above.
  if (actorInstance && actorInstance->isSelf() &&
      isa<ConstructorDecl>(declRef.getDecl()) &&
      isa<ConstructorDecl>(fromDC))
    return forSameConcurrencyDomain(declIsolation, options);

  // If there is an instance that corresponds to 'self',
  // we are in a constructor or destructor, and we have a stored property of
  // global-actor-qualified type, then we have problems if the stored property
  // type is non-Sendable. Note that if we get here, the type must be Sendable.
  if (actorInstance && actorInstance->isSelf() &&
      isNonInheritedStorage(declRef.getDecl(), fromDC) &&
      declIsolation.isGlobalActor() &&
      (isa<ConstructorDecl>(fromDC) || isa<DestructorDecl>(fromDC)))
    return forSameConcurrencyDomain(declIsolation, options);

  // At this point, we are accessing the target from outside the actor.
  // First, check whether it is something that can be accessed directly,
  // without any kind of promotion.
  if (isAccessibleAcrossActors(
          declRef.getDecl(), declIsolation, fromDC, options, actorInstance))
    return forEntersActor(declIsolation, options);

  // This is a cross-actor reference.

  // Note if the reference originates from a @preconcurrency-isolated context.
  if (contextIsolation.preconcurrency() || declIsolation.preconcurrency())
    options |= Flags::Preconcurrency;

  // If the declaration isn't asynchronous, promote to async.
  if (!isAsyncDecl(declRef))
    options |= Flags::AsyncPromotion;

  // If the declaration is isolated to a distributed actor and we are not
  // guaranteed to be on the same node, make adjustments distributed
  // access.
  if (declIsolation.isDistributedActor()) {
    bool needsDistributed;
    if (actorInstance)
      needsDistributed = !actorInstance->isKnownToBeLocal();
    else
      needsDistributed = !contextIsolation.isDistributedActor();

    if (needsDistributed) {
      options |= Flags::Distributed;

      if (!isThrowsDecl(declRef))
        options |= Flags::ThrowsPromotion;
    }
  }

  return forEntersActor(declIsolation, options);
}

bool swift::diagnoseNonSendableFromDeinit(
    SourceLoc refLoc, VarDecl *var, DeclContext *dc) {
  return diagnoseIfAnyNonSendableTypes(var->getTypeInContext(),
                                SendableCheckContext(dc),
                                Type(),
                                SourceLoc(),
                                refLoc,
                                diag::non_sendable_from_deinit,
                                var->getDescriptiveKind(), var->getName());
}
