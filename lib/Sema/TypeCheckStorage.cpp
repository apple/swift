//===--- TypeCheckStorage.cpp - Checking Properties and Subscripts -------===//
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
// This file implements semantic analysis for properties, subscripts as well
// as their accessors.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "TypeChecker.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckConcurrency.h"
#include "TypeCheckDecl.h"
#include "TypeCheckMacros.h"
#include "TypeCheckType.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
using namespace swift;

/// Set each bound variable in the pattern to have an error type.
void swift::setBoundVarsTypeError(Pattern *pattern, ASTContext &ctx) {
  pattern->forEachVariable([&](VarDecl *var) {
    // Don't change the type of a variable that we've been able to
    // compute a type for.
    if (var->hasInterfaceType())
      return;

    var->setInvalid();
  });
}

/// Build a default initializer for the given type.
Expr *TypeChecker::buildDefaultInitializer(Type type) {
  auto &Context = type->getASTContext();
  // Default-initialize optional types and weak values to 'nil'.
  if (type->getReferenceStorageReferent()->getOptionalObjectType())
    return new (Context) NilLiteralExpr(SourceLoc(), /*Implicit=*/true);

  // Build tuple literals for tuple types.
  if (auto tupleType = type->getAs<TupleType>()) {
    SmallVector<Expr *, 2> inits;
    for (const auto &elt : tupleType->getElements()) {
      auto eltInit = TypeChecker::buildDefaultInitializer(elt.getType());
      if (!eltInit)
        return nullptr;

      inits.push_back(eltInit);
    }

    return TupleExpr::createImplicit(Context, inits, { });
  }

  // We don't default-initialize anything else.
  return nullptr;
}

/// Does the context allow pattern bindings that don't bind any variables?
static bool contextAllowsPatternBindingWithoutVariables(DeclContext *dc) {
  
  // Property decls in type context must bind variables.
  if (dc->isTypeContext())
    return false;
  
  // Global variable decls must bind variables, except in scripts.
  if (dc->isModuleScopeContext()) {
    if (dc->getParentSourceFile()
        && dc->getParentSourceFile()->isScriptMode())
      return true;
    
    return false;
  }
  
  return true;
}

static bool hasStoredProperties(NominalTypeDecl *decl,
                                IterableDeclContext *implDecl) {
  bool isForeignReferenceTy =
      isa<ClassDecl>(decl) && cast<ClassDecl>(decl)->isForeignReferenceType();

  return (isa<StructDecl>(decl) ||
          (isa<ClassDecl>(decl) &&
           (!decl->hasClangNode() || isForeignReferenceTy
                || (decl != implDecl))));
}

namespace {
  enum class LoweredPropertiesReason {
    Stored,
    Memberwise
  };
}

static void computeLoweredProperties(NominalTypeDecl *decl,
                                     IterableDeclContext *implDecl,
                                     LoweredPropertiesReason reason) {
  // Expand synthesized member macros.
  auto &ctx = decl->getASTContext();
  (void)evaluateOrDefault(ctx.evaluator,
                          ExpandSynthesizedMemberMacroRequest{decl},
                          false);

  // Just walk over the members of the type, forcing backing storage
  // for lazy properties and property wrappers to be synthesized.
  for (auto *member : implDecl->getMembers()) {
    // Expand peer macros.
    (void)evaluateOrDefault(
        ctx.evaluator,
        ExpandPeerMacroRequest{member},
        {});

    auto *var = dyn_cast<VarDecl>(member);
    if (!var || var->isStatic())
      continue;

    if (reason == LoweredPropertiesReason::Stored) {
      if (var->getAttrs().hasAttribute<LazyAttr>())
        (void) var->getLazyStorageProperty();

      if (var->hasAttachedPropertyWrapper()) {
        (void) var->getPropertyWrapperAuxiliaryVariables();
        (void) var->getPropertyWrapperInitializerInfo();
      }
    }
  }

  if (reason != LoweredPropertiesReason::Stored)
    return;

  // If this is an actor, check conformance to the Actor protocol to
  // ensure that the actor storage will get created (if needed).
  if (auto classDecl = dyn_cast<ClassDecl>(decl)) {
    // If this is an actor class, check conformance to the Actor protocol to
    // ensure that the actor storage will get created (if needed).
    if (classDecl->isActor()) {
      ASTContext &ctx = decl->getASTContext();

      if (auto actorProto = ctx.getProtocol(KnownProtocolKind::Actor)) {
        SmallVector<ProtocolConformance *, 1> conformances;
        classDecl->lookupConformance(actorProto, conformances);
        for (auto conformance : conformances)
          TypeChecker::checkConformance(conformance->getRootNormalConformance());
      }

      // If this is a distributed actor, synthesize its special stored properties.
      if (classDecl->isDistributedActor()) {
        if (auto actorProto = ctx.getProtocol(KnownProtocolKind::DistributedActor)) {
          SmallVector<ProtocolConformance *, 1> conformances;
          classDecl->lookupConformance(actorProto, conformances);
          for (auto conformance : conformances)
            TypeChecker::checkConformance(conformance->getRootNormalConformance());
        }
      }
    }
  }
}

static void computeLoweredStoredProperties(NominalTypeDecl *decl,
                                           IterableDeclContext *implDecl) {
  computeLoweredProperties(decl, implDecl, LoweredPropertiesReason::Stored);
}

/// Enumerate both the stored properties and missing members,
/// in a deterministic order.
static void enumerateStoredPropertiesAndMissing(
    NominalTypeDecl *decl,
    IterableDeclContext *implDecl,
    llvm::function_ref<void(VarDecl *)> _addStoredProperty,
    llvm::function_ref<void(MissingMemberDecl *)> addMissing) {
  // Add a variable as a stored properties.
  llvm::SmallSet<VarDecl *, 8> knownStoredProperties;
  auto addStoredProperty = [&](VarDecl *var) {
    if (!var->isStatic() && var->hasStorage()) {
      if (knownStoredProperties.insert(var).second)
        _addStoredProperty(var);
    }
  };

  // If we have a distributed actor, find the id and actorSystem
  // properties. We always want them first, and in a specific
  // order.
  if (decl->isDistributedActor()) {
    VarDecl *distributedActorId = nullptr;
    VarDecl *distributedActorSystem = nullptr;
    ASTContext &ctx = decl->getASTContext();
    for (auto *member : implDecl->getMembers()) {
      if (auto *var = dyn_cast<VarDecl>(member)) {
        if (!var->isStatic() && var->hasStorage()) {
          if (var->getName() == ctx.Id_id) {
            distributedActorId = var;
          } else if (var->getName() == ctx.Id_actorSystem) {
            distributedActorSystem = var;
          }
        }

        if (distributedActorId && distributedActorSystem)
          break;
      }
    }

    if (distributedActorId)
      addStoredProperty(distributedActorId);
    if (distributedActorSystem)
      addStoredProperty(distributedActorSystem);
  }

  for (auto *member : implDecl->getMembers()) {
    if (auto *var = dyn_cast<VarDecl>(member)) {
      addStoredProperty(var);
    }

    member->visitAuxiliaryDecls([&](Decl *auxDecl) {
      if (auto auxVar = dyn_cast<VarDecl>(auxDecl))
        addStoredProperty(auxVar);
    });

    if (auto missing = dyn_cast<MissingMemberDecl>(member))
      if (missing->getNumberOfFieldOffsetVectorEntries() > 0)
        addMissing(missing);
  }
}

static bool isInSourceFile(IterableDeclContext *idc) {
  const DeclContext *dc = idc->getAsGenericContext();
  return isa<SourceFile>(dc->getModuleScopeContext());
}

ArrayRef<VarDecl *>
StoredPropertiesRequest::evaluate(Evaluator &evaluator,
                                  NominalTypeDecl *decl) const {
  // If this is an imported class with an @_objcImplementation extension, get
  // members from the extension instead.
  IterableDeclContext *implDecl = decl->getImplementationContext();

  if (!hasStoredProperties(decl, implDecl))
    return ArrayRef<VarDecl *>();

  SmallVector<VarDecl *, 4> results;

  // Unless we're in a source file we don't have to do anything
  // special to lower lazy properties and property wrappers.
  if (isInSourceFile(implDecl))
    computeLoweredStoredProperties(decl, implDecl);

  enumerateStoredPropertiesAndMissing(decl, implDecl,
    [&](VarDecl *var) {
      results.push_back(var);
    },
    [](MissingMemberDecl *missing) { });

  return decl->getASTContext().AllocateCopy(results);
}

ArrayRef<Decl *>
StoredPropertiesAndMissingMembersRequest::evaluate(Evaluator &evaluator,
                                                   NominalTypeDecl *decl) const {
  // If this is an imported class with an @_objcImplementation extension, get
  // members from the extension instead.
  IterableDeclContext *implDecl = decl->getImplementationContext();

  if (!hasStoredProperties(decl, implDecl))
    return ArrayRef<Decl *>();

  SmallVector<Decl *, 4> results;

  // Unless we're in a source file we don't have to do anything
  // special to lower lazy properties and property wrappers.
  if (isInSourceFile(implDecl))
    computeLoweredStoredProperties(decl, implDecl);

  enumerateStoredPropertiesAndMissing(decl, implDecl,
    [&](VarDecl *var) {
      results.push_back(var);
    },
    [&](MissingMemberDecl *missing) {
      results.push_back(missing);
    });

  return decl->getASTContext().AllocateCopy(results);
}

bool HasInitAccessorRequest::evaluate(Evaluator &evaluator,
                                      AbstractStorageDecl *decl) const {
  auto *var = dyn_cast<VarDecl>(decl);
  if (!var)
    return false;

  if (var->getAccessor(AccessorKind::Init))
    return true;

  // Look to see whether it is possible that there is an init accessor.
  bool hasInitAccessor = false;
  namelookup::forEachPotentialAttachedMacro(
      var, MacroRole::Accessor,
      [&](MacroDecl *macro, const MacroRoleAttr *attr) {
        if (accessorMacroIntroducesInitAccessor(macro, attr))
          hasInitAccessor = true;
      });

  // There is no chance for an init accessor, so we're done.
  if (!hasInitAccessor)
    return false;

  // We might get an init accessor by expanding accessor macros; do so now.
  (void)evaluateOrDefault(
       var->getASTContext().evaluator, ExpandAccessorMacros{var}, { });

  return var->getAccessor(AccessorKind::Init);
}

ArrayRef<VarDecl *>
InitAccessorPropertiesRequest::evaluate(Evaluator &evaluator,
                                        NominalTypeDecl *decl) const {
  SmallVector<VarDecl *, 4> results;
  for (auto var : decl->getMemberwiseInitProperties()) {
    if (var->hasInitAccessor())
      results.push_back(var);
  }

  return decl->getASTContext().AllocateCopy(results);
}

ArrayRef<VarDecl *>
MemberwiseInitPropertiesRequest::evaluate(Evaluator &evaluator,
                                        NominalTypeDecl *decl) const {
  IterableDeclContext *implDecl = decl->getImplementationContext();

  if (!hasStoredProperties(decl, implDecl))
    return ArrayRef<VarDecl *>();

  // Make sure we expand what we need to to get all of the properties.
  computeLoweredProperties(decl, implDecl, LoweredPropertiesReason::Memberwise);

  SmallVector<VarDecl *, 4> results;
  SmallPtrSet<VarDecl *, 4> subsumedViaInitAccessor;

  auto maybeAddProperty = [&](VarDecl *var) {
    // We only care about properties that are memberwise initialized.
    if (!var->isMemberwiseInitialized(/*preferDeclaredProperties=*/true))
      return;

    // Add this property.
    results.push_back(var);

    // If this property has an init accessor, it subsumes all of the stored properties
    // that the accessor initializes. Mark those stored properties as being subsumed; we'll
    // get back to them later.
    if (auto initAccessor = var->getAccessor(AccessorKind::Init)) {
      if (auto initAttr = initAccessor->getAttrs().getAttribute<InitializesAttr>()) {
        for (auto subsumed : initAttr->getPropertyDecls(initAccessor))
          subsumedViaInitAccessor.insert(subsumed);
      }
    }
  };

  for (auto *member : decl->getCurrentMembers()) {
    if (auto *var = dyn_cast<VarDecl>(member))
      maybeAddProperty(var);

    member->visitAuxiliaryDecls([&](Decl *auxDecl) {
      if (auto auxVar = dyn_cast<VarDecl>(auxDecl))
        maybeAddProperty(auxVar);
    });
  }

  // If any properties were subsumed via init accessors, drop them from the list.
  if (!subsumedViaInitAccessor.empty()) {
    results.erase(std::remove_if(results.begin(), results.end(), [&](VarDecl *var) {
                    return subsumedViaInitAccessor.contains(var);
                  }),
                  results.end());
  }

  return decl->getASTContext().AllocateCopy(results);
}

/// Validate the \c entryNumber'th entry in \c binding.
const PatternBindingEntry *PatternBindingEntryRequest::evaluate(
    Evaluator &eval, PatternBindingDecl *binding, unsigned entryNumber,
    bool LeaveClosureBodiesUnchecked) const {
  const auto &pbe = binding->getPatternList()[entryNumber];
  auto &Context = binding->getASTContext();

  // Resolve the pattern.
  auto *pattern = TypeChecker::resolvePattern(binding->getPattern(entryNumber),
                                              binding->getDeclContext(),
                                              /*isStmtCondition*/ true);
  if (!pattern) {
    binding->setInvalid();
    binding->getPattern(entryNumber)->setType(ErrorType::get(Context));
    return &pbe;
  }

  binding->setPattern(entryNumber, pattern,
                      binding->getInitContext(entryNumber));

  // Validate 'static'/'class' on properties in nominal type decls.
  auto StaticSpelling = binding->getStaticSpelling();
  if (StaticSpelling != StaticSpellingKind::None &&
      isa<ExtensionDecl>(binding->getDeclContext())) {
    if (auto *NTD = binding->getDeclContext()->getSelfNominalTypeDecl()) {
      if (!isa<ClassDecl>(NTD)) {
        if (StaticSpelling == StaticSpellingKind::KeywordClass) {
          binding->diagnose(diag::class_var_not_in_class, false)
              .fixItReplace(binding->getStaticLoc(), "static");
          NTD->diagnose(diag::extended_type_declared_here);
        }
      }
    }
  }

  // Reject "class" methods on actors.
  if (StaticSpelling == StaticSpellingKind::KeywordClass &&
      binding->getDeclContext()->getSelfClassDecl() &&
      binding->getDeclContext()->getSelfClassDecl()->isActor()) {
    binding->diagnose(diag::class_var_not_in_class, false)
        .fixItReplace(binding->getStaticLoc(), "static");
  }

  // Check the pattern.
  auto contextualPattern =
      ContextualPattern::forPatternBindingDecl(binding, entryNumber);
  Type patternType = TypeChecker::typeCheckPattern(contextualPattern);
  if (patternType->hasError()) {
    swift::setBoundVarsTypeError(pattern, Context);
    binding->setInvalid();
    pattern->setType(ErrorType::get(Context));
    return &pbe;
  }

  llvm::SmallVector<VarDecl *, 2> vars;
  binding->getPattern(entryNumber)->collectVariables(vars);
  bool isReq = false;
  bool shouldRequireStatic = false;
  if (auto *d = binding->getDeclContext()->getAsDecl()) {
    isReq = isa<ProtocolDecl>(d);
    shouldRequireStatic = isa<NominalTypeDecl>(d);
  }
  for (auto *sv: vars) {
    bool hasConst = sv->getAttrs().getAttribute<CompileTimeConstAttr>();
    if (!hasConst)
      continue;
    bool hasStatic = StaticSpelling != StaticSpellingKind::None;
    // only static _const let/var is supported
    if (shouldRequireStatic && !hasStatic) {
      binding->diagnose(diag::require_static_for_const);
      continue;
    }
    if (isReq) {
      continue;
    }
    auto varSourceFile = binding->getDeclContext()->getParentSourceFile();
    auto isVarInInterfaceFile =
        varSourceFile && varSourceFile->Kind == SourceFileKind::Interface;
    // Don't diagnose too strictly for textual interfaces.
    if (isVarInInterfaceFile) {
      continue;
    }
    // var is only allowed in a protocol.
    if (!sv->isLet()) {
      binding->diagnose(diag::require_let_for_const);
    }
    // Diagnose when an init isn't given and it's not a compile-time constant
    if (auto *init = binding->getInit(entryNumber)) {
      if (!init->isSemanticallyConstExpr()) {
        binding->diagnose(diag::require_const_initializer_for_const);
      }
    } else {
      binding->diagnose(diag::require_const_initializer_for_const);
    }
  }

  // If we have a type but no initializer, check whether the type is
  // default-initializable. If so, do it.
  if (!pbe.isInitialized() &&
      binding->isDefaultInitializable(entryNumber) &&
      pattern->hasStorage()) {
    if (auto defaultInit = TypeChecker::buildDefaultInitializer(patternType)) {
      // If we got a default initializer, install it and re-type-check it
      // to make sure it is properly coerced to the pattern type.
      binding->setInit(entryNumber, defaultInit);
    }
  }

  // If the pattern contains some form of unresolved type, we'll need to
  // check the initializer.
  if (patternType->hasUnresolvedType() ||
      patternType->hasPlaceholder() ||
      patternType->hasUnboundGenericType()) {
    TypeCheckExprOptions options;
    if (LeaveClosureBodiesUnchecked) {
      options |= TypeCheckExprFlags::LeaveClosureBodyUnchecked;
    }
    if (TypeChecker::typeCheckPatternBinding(binding, entryNumber, patternType,
                                             options)) {
      binding->setInvalid();
      return &pbe;
    }

    // Local variable packs are not allowed.
    if (binding->getDeclContext()->isLocalContext() &&
        binding->getInit(entryNumber)->getType()->is<PackExpansionType>()) {
      binding->diagnose(diag::expansion_not_allowed,
                        binding->getInit(entryNumber)->getType());
    }

    // A pattern binding at top level is not allowed to pick up another decl's
    // opaque result type as its type by type inference.
    if (!binding->getDeclContext()->isLocalContext() &&
        binding->getInit(entryNumber)->getType()->hasOpaqueArchetype()) {
      // TODO: Check whether the type is the pattern binding's own opaque type.
      binding->diagnose(diag::inferred_opaque_type,
                        binding->getInit(entryNumber)->getType());
    }
  } else {
    // Coerce the pattern to the computed type.
    if (auto newPattern = TypeChecker::coercePatternToType(
            contextualPattern, patternType,
            TypeResolverContext::PatternBindingDecl)) {
      pattern = newPattern;
    } else {
      binding->setInvalid();
      pattern->setType(ErrorType::get(Context));
      return &pbe;
    }
  }

  // If the pattern binding appears in a type or library file context, then
  // it must bind at least one variable.
  if (!contextAllowsPatternBindingWithoutVariables(binding->getDeclContext())) {
    if (vars.empty()) {
      // Selector for error message.
      enum : unsigned {
        Property,
        GlobalVariable,
      };
      Context.Diags.diagnose(binding->getPattern(entryNumber)->getLoc(),
                             diag::pattern_binds_no_variables,
                             binding->getDeclContext()->isTypeContext()
                                 ? Property
                                 : GlobalVariable);
    }
  }
  return &pbe;
}

bool
IsGetterMutatingRequest::evaluate(Evaluator &evaluator,
                                  AbstractStorageDecl *storage) const {
  auto storageDC = storage->getDeclContext();
  bool result = (!storage->isStatic() && storageDC->isTypeContext() &&
                 storageDC->hasValueSemantics());

  // 'lazy' overrides the normal accessor-based rules and heavily
  // restricts what accessors can be used.  The getter is considered
  // mutating if this is instance storage on a value type.
  if (storage->getAttrs().hasAttribute<LazyAttr>()) {
    return result;
  }

  // If we have an attached property wrapper, the getter's mutating-ness
  // depends on the composition of the wrappers.
  if (auto var = dyn_cast<VarDecl>(storage)) {
    if (auto mut = var->getPropertyWrapperMutability()) {
      return mut->Getter == PropertyWrapperMutability::Mutating
        && result;
    }
  }

  auto checkMutability = [&](AccessorKind kind) -> bool {
    auto *accessor = storage->getParsedAccessor(kind);
    if (!accessor)
      return false;

    return accessor->isMutating();
  };

  // Protocol requirements are always written as '{ get }' or '{ get set }';
  // the @_borrowed attribute determines if getReadImpl() becomes Get or Read.
  if (isa<ProtocolDecl>(storageDC))
    return checkMutability(AccessorKind::Get);

  switch (storage->getReadImpl()) {
  case ReadImplKind::Stored:
  case ReadImplKind::Inherited:
    return false;

  case ReadImplKind::Get:
    return checkMutability(AccessorKind::Get);

  case ReadImplKind::Address:
    return checkMutability(AccessorKind::Address);

  case ReadImplKind::Read:
    return checkMutability(AccessorKind::Read);
  }

  llvm_unreachable("bad impl kind");
}

bool
IsSetterMutatingRequest::evaluate(Evaluator &evaluator,
                                  AbstractStorageDecl *storage) const {
  // By default, the setter is mutating if we have an instance member of a
  // value type, but this can be overridden below.
  auto storageDC = storage->getDeclContext();
  bool result = (!storage->isStatic() && storageDC->isTypeContext() &&
                 storageDC->hasValueSemantics());

  // If we have an attached property wrapper, the setter is mutating
  // or not based on the composition of the wrappers.
  if (auto var = dyn_cast<VarDecl>(storage)) {
    if (auto mut = var->getPropertyWrapperMutability()) {
      bool isMutating = mut->Setter == PropertyWrapperMutability::Mutating;
      if (auto *didSet = var->getParsedAccessor(AccessorKind::DidSet)) {
        // If there's a didSet, we call the getter for the 'oldValue', and so
        // should consider the getter's mutatingness as well
        isMutating |= (mut->Getter == PropertyWrapperMutability::Mutating);
        isMutating |= didSet->getAttrs().hasAttribute<MutatingAttr>();
      }
      if (auto *willSet = var->getParsedAccessor(AccessorKind::WillSet))
        isMutating |= willSet->getAttrs().hasAttribute<MutatingAttr>();
      return isMutating && result;
    }
  }

  auto impl = storage->getImplInfo();
  switch (impl.getWriteImpl()) {
  case WriteImplKind::Immutable:
  case WriteImplKind::Stored:
    // Instance member setters are mutating; static property setters and
    // top-level setters are not.
    // It's important that we use this logic for "immutable" storage
    // in order to handle initialization of let-properties.
    return result;

  case WriteImplKind::StoredWithObservers:
  case WriteImplKind::InheritedWithObservers:
  case WriteImplKind::Set: {
    auto *setter = storage->getParsedAccessor(AccessorKind::Set);

    if (setter)
      result = setter->isMutating();


    // As a special extra check, if the user also gave us a modify
    // coroutine, check that it has the same mutatingness as the setter.
    // TODO: arguably this should require the spelling to match even when
    // it's the implied value.
    auto modifyAccessor = storage->getParsedAccessor(AccessorKind::Modify);

    if (impl.getReadWriteImpl() == ReadWriteImplKind::Modify &&
        modifyAccessor != nullptr) {
      auto modifyResult = modifyAccessor->isMutating();
      if ((result || storage->isGetterMutating()) != modifyResult) {
        modifyAccessor->diagnose(
            diag::modify_mutatingness_differs_from_setter,
            modifyResult ? SelfAccessKind::Mutating
                         : SelfAccessKind::NonMutating,
            modifyResult ? SelfAccessKind::NonMutating
                         : SelfAccessKind::Mutating);
        if (setter)
          setter->diagnose(diag::previous_accessor, "setter", 0);
        modifyAccessor->setInvalid();
      }
    }

    return result;
  }

  case WriteImplKind::MutableAddress:
    return storage->getParsedAccessor(AccessorKind::MutableAddress)
      ->isMutating();

  case WriteImplKind::Modify:
    return storage->getParsedAccessor(AccessorKind::Modify)
      ->isMutating();
  }
  llvm_unreachable("bad storage kind");
}

OpaqueReadOwnership
OpaqueReadOwnershipRequest::evaluate(Evaluator &evaluator,
                                     AbstractStorageDecl *storage) const {
  enum class DiagKind {
    BorrowedAttr,
    NoncopyableType
  };

  auto usesBorrowed = [&](DiagKind kind) -> OpaqueReadOwnership {
    // Check for effects on the getter.
    if (auto *getter = storage->getEffectfulGetAccessor()) {
      switch (kind) {
      case DiagKind::NoncopyableType:
        getter->diagnose(diag::noncopyable_effectful_getter,
                         getter->getDescriptiveKind());
        break;
      case DiagKind::BorrowedAttr:
        getter->diagnose(diag::borrowed_with_effect,
                         getter->getDescriptiveKind());
        break;
      }
    }
    return OpaqueReadOwnership::Borrowed;
  };

  if (storage->getAttrs().hasAttribute<BorrowedAttr>())
    return usesBorrowed(DiagKind::BorrowedAttr);

  if (storage->getValueInterfaceType()->isPureMoveOnly())
    return usesBorrowed(DiagKind::NoncopyableType);

  return OpaqueReadOwnership::Owned;
}

/// Insert the specified decl into the DeclContext's member list.  If the hint
/// decl is specified, the new decl is inserted next to the hint.
static void addMemberToContextIfNeeded(Decl *D, DeclContext *DC,
                                       Decl *Hint = nullptr) {
  if (auto *ntd = dyn_cast<NominalTypeDecl>(DC)) {
    ntd->addMember(D, Hint);
  } else if (auto *ed = dyn_cast<ExtensionDecl>(DC)) {
    ed->addMember(D, Hint);
  } else {
    assert((DC->isLocalContext() || isa<FileUnit>(DC)) &&
           "Unknown declcontext");
  }
}

/// Build a parameter list which can forward the formal index parameters of a
/// declaration.
///
/// \param prefix optional arguments to be prefixed onto the index
///   forwarding pattern.
static ParameterList *
buildIndexForwardingParamList(AbstractStorageDecl *storage,
                              ArrayRef<ParamDecl*> prefix,
                              ASTContext &context) {
  auto subscript = dyn_cast<SubscriptDecl>(storage);

  // Fast path: if this isn't a subscript, just use whatever we have.
  if (!subscript)
    return ParameterList::create(context, prefix);

  // Clone the parameter list over for a new decl, so we get new ParamDecls.
  auto indices = subscript->getIndices()->clone(context,
                                                ParameterList::Implicit);

  // Give all of the parameters meaningless names so that we can forward
  // them properly.  If it's declared anonymously, SILGen will think
  // it's unused.
  // TODO: use some special DeclBaseName for this?
  for (auto param : indices->getArray()) {
    if (!param->hasName())
      param->setName(context.getIdentifier("anonymous"));
    assert(param->hasName());
  }

  if (prefix.empty())
    return indices;
  
  
  // Otherwise, we need to build up a new parameter list.
  SmallVector<ParamDecl*, 4> elements;

  // Start with the fields we were given, if there are any.
  elements.append(prefix.begin(), prefix.end());
  elements.append(indices->begin(), indices->end());
  return ParameterList::create(context, elements);
}

static bool doesAccessorHaveBody(AccessorDecl *accessor) {
  // Protocol requirements don't have bodies.
  //
  // FIXME: Revisit this if we ever get 'real' default implementations.
  if (isa<ProtocolDecl>(accessor->getDeclContext()))
    return false;

  auto *storage = accessor->getStorage();

  // NSManaged getters and setters don't have bodies.
  if (storage->getAttrs().hasAttribute<NSManagedAttr>())
    if (accessor->isGetterOrSetter())
      return false;

  return true;
}

/// Build an argument list referencing the subscript parameters for this
/// subscript accessor.
static ArgumentList *buildSubscriptArgumentList(ASTContext &ctx,
                                                AccessorDecl *accessor) {
  // Pull out the body parameters, which we should have cloned
  // previously to be forwardable.  Drop the initial buffer/value
  // parameter in accessors that have one.
  auto params = accessor->getParameters()->getArray();
  auto accessorKind = accessor->getAccessorKind();

  // Ignore the value parameter of a setter.
  if (accessorKind == AccessorKind::Set) {
    params = params.slice(1);
  }

  // Okay, everything else should be forwarded, build the argument list.
  return buildForwardingArgumentList(params, ctx);
}

namespace {
  enum class TargetImpl {
    /// We're doing an ordinary storage reference.
    Ordinary,
    /// We're referencing the physical storage created for the storage.
    Storage,
    /// We're referencing this specific implementation of the storage, not
    /// an override of it.
    Implementation,
    /// We're referencing the superclass's implementation of the storage.
    Super,
    /// We're referencing the backing property for a property with a wrapper
    /// through the 'value' property.
    Wrapper,
    /// We're referencing the backing property for a property with a wrapper
    /// through the 'projectedValue' property.
    WrapperStorage,
  };
} // end anonymous namespace

namespace  {
  /// Describes the information needed to perform property wrapper access via
  /// the enclosing self.
  struct EnclosingSelfPropertyWrapperAccess {
    /// The (generic) subscript that will be used to perform the access.
    SubscriptDecl *subscript;

    /// The property being accessed.
    VarDecl *accessedProperty;
  };
}

/// Determine whether the given property should be accessed via the enclosing-self access pattern.
static llvm::Optional<EnclosingSelfPropertyWrapperAccess>
getEnclosingSelfPropertyWrapperAccess(VarDecl *property, bool forProjected) {
  // The enclosing-self pattern only applies to instance properties of
  // classes.
  if (!property->isInstanceMember())
    return llvm::None;
  auto classDecl = property->getDeclContext()->getSelfClassDecl();
  if (!classDecl)
    return llvm::None;

  // The pattern currently only works with the outermost property wrapper.
  Type outermostWrapperType = property->getPropertyWrapperBackingPropertyType();
  if (!outermostWrapperType)
    return llvm::None;
  NominalTypeDecl *wrapperTypeDecl = outermostWrapperType->getAnyNominal();
  if (!wrapperTypeDecl)
    return llvm::None;

  // Look for a generic subscript that fits the general form we need.
  auto wrapperInfo = wrapperTypeDecl->getPropertyWrapperTypeInfo();
  auto subscript =
      forProjected ? wrapperInfo.enclosingInstanceProjectedSubscript
                   : wrapperInfo.enclosingInstanceWrappedSubscript;
  if (!subscript)
    return llvm::None;

  EnclosingSelfPropertyWrapperAccess result;
  result.subscript = subscript;

  if (forProjected) {
    result.accessedProperty =
        property->getPropertyWrapperAuxiliaryVariables().projectionVar;
  } else {
    result.accessedProperty = property;
  }
  return result;
}

static llvm::Optional<PropertyWrapperLValueness>
getPropertyWrapperLValueness(VarDecl *var) {
  auto &ctx = var->getASTContext();
  return evaluateOrDefault(ctx.evaluator, PropertyWrapperLValuenessRequest{var},
                           llvm::None);
}

/// Build a reference to the storage of a declaration. Returns nullptr if there
/// was an error. This should only occur if an invalid declaration was type
/// checked; another diagnostic should have been emitted already.
///
/// The resulting reference is used in synthesized property accessors and is of
/// one of the following forms:
///   1. Without property wrappers:
///     - Stored: \c self.member
///   2. With property wrappers:
///     - Wrapped: \c self._member.wrappedValue
///     - Composition: \c self._member.wrappedValue.wrappedValue….wrappedValue
///     - Projected: \c self._member.projectedValue
///     - Enclosed instance: \c Wrapper[_enclosedInstance: self, …]
static Expr *buildStorageReference(AccessorDecl *accessor,
                                   AbstractStorageDecl *storage,
                                   TargetImpl target,
                                   bool isUsedForGetAccess,
                                   bool isUsedForSetAccess,
                                   ASTContext &ctx) {
  // Whether the last component of the expression should be an l-value
  bool isLValue = isUsedForSetAccess;
  // Local function to "finish" the expression, creating a member reference
  // to the given sequence of underlying variables.
  llvm::Optional<EnclosingSelfPropertyWrapperAccess> enclosingSelfAccess;
  // Contains the underlying wrappedValue declaration in a property wrapper
  // along with whether or not the reference to this field needs to be an lvalue
  llvm::SmallVector<std::pair<VarDecl *, bool>, 1> underlyingVars;
  auto finish = [&](Expr *result) -> Expr * {
    for (auto underlyingVarPair : underlyingVars) {
      auto underlyingVar = underlyingVarPair.first;
      auto isWrapperRefLValue = underlyingVarPair.second;
      auto subs = result->getType()
          ->getWithoutSpecifierType()
          ->getContextSubstitutionMap(
            accessor->getParentModule(),
            underlyingVar->getDeclContext());

      ConcreteDeclRef memberRef(underlyingVar, subs);
      auto *memberRefExpr = new (ctx) MemberRefExpr(
          result, SourceLoc(), memberRef, DeclNameLoc(), /*Implicit=*/true);
      auto type = underlyingVar->getValueInterfaceType().subst(subs);
      if (isWrapperRefLValue)
        type = LValueType::get(type);
      memberRefExpr->setType(type);
      
      result = memberRefExpr;
    }
    
    return result;
  };

  VarDecl *selfDecl = accessor->getImplicitSelfDecl();

  AccessSemantics semantics;
  SelfAccessorKind selfAccessKind;
  Type selfTypeForAccess = (selfDecl ? selfDecl->getType() : Type());

  bool isMemberLValue = isLValue;

  auto *genericEnv = accessor->getGenericEnvironment();
  SubstitutionMap subs;
  if (genericEnv)
    subs = genericEnv->getForwardingSubstitutionMap();

  switch (target) {
  case TargetImpl::Ordinary:
    semantics = AccessSemantics::Ordinary;
    selfAccessKind = SelfAccessorKind::Peer;
    break;

  case TargetImpl::Storage:
    semantics = AccessSemantics::DirectToStorage;
    selfAccessKind = SelfAccessorKind::Peer;
    break;

  case TargetImpl::Implementation:
    semantics = AccessSemantics::DirectToImplementation;
    selfAccessKind = SelfAccessorKind::Peer;
    break;

  case TargetImpl::Super:
    // If this really is an override, use a super-access.
    if (auto override = storage->getOverriddenDecl()) {
      semantics = AccessSemantics::Ordinary;
      selfAccessKind = SelfAccessorKind::Super;

      auto isMetatype = false;
      if (auto *metaTy = selfTypeForAccess->getAs<MetatypeType>()) {
        isMetatype = true;
        selfTypeForAccess = metaTy->getInstanceType();
      }

      // Adjust the self type of the access to refer to the relevant superclass.
      auto *baseClass = override->getDeclContext()->getSelfClassDecl();
      selfTypeForAccess = selfTypeForAccess->getSuperclassForDecl(baseClass);

      // Error recovery path. We get an ErrorType here if getSuperclassForDecl()
      // fails (because, for instance, a generic parameter of a generic nominal
      // type cannot be resolved).
      if (!selfTypeForAccess->is<ErrorType>()) {
        subs =
          selfTypeForAccess->getContextSubstitutionMap(
            accessor->getParentModule(),
            baseClass);
      }

      storage = override;

      if (isMetatype)
        selfTypeForAccess = MetatypeType::get(selfTypeForAccess);

    // Otherwise do a self-reference, which is dynamically bogus but
    // should be statically valid.  This should only happen in invalid cases.    
    } else {
      semantics = AccessSemantics::Ordinary;
      selfAccessKind = SelfAccessorKind::Peer;
    }
    break;

  case TargetImpl::Wrapper: {
    auto var = cast<VarDecl>(accessor->getStorage());
    auto *backing = var->getPropertyWrapperBackingProperty();

    // Error recovery.
    if (!backing || backing->isInvalid())
      return nullptr;

    storage = backing;

    // If the outermost property wrapper uses the enclosing self pattern,
    // record that.
    unsigned lastWrapperIdx = var->getAttachedPropertyWrappers().size();
    unsigned firstWrapperIdx = 0;
    enclosingSelfAccess =
        getEnclosingSelfPropertyWrapperAccess(var, /*forProjected=*/false);
    if (enclosingSelfAccess)
      firstWrapperIdx = 1;

    // Perform accesses to the wrappedValues along the composition chain.
    if (firstWrapperIdx < lastWrapperIdx) {
      auto lvalueness = *getPropertyWrapperLValueness(var);

      // Figure out if the outermost wrapper instance should be an l-value
      bool isLValueForGet = lvalueness.isLValueForGetAccess[firstWrapperIdx];
      bool isLValueForSet = lvalueness.isLValueForSetAccess[firstWrapperIdx];
      isMemberLValue = (isLValueForGet && isUsedForGetAccess) ||
                       (isLValueForSet && isUsedForSetAccess);

      for (unsigned i : range(firstWrapperIdx, lastWrapperIdx)) {
        auto wrapperInfo = var->getAttachedPropertyWrapperTypeInfo(i);
        auto wrappedValue = wrapperInfo.valueVar;

        // Figure out if the wrappedValue accesses should be l-values
        bool isWrapperRefLValue = isLValue;
        if (i < lastWrapperIdx - 1) {
          bool isLValueForGet = lvalueness.isLValueForGetAccess[i+1];
          bool isLValueForSet = lvalueness.isLValueForSetAccess[i+1];
          isWrapperRefLValue = (isLValueForGet && isUsedForGetAccess) ||
                               (isLValueForSet && isUsedForSetAccess);
        }

        // Check for availability of wrappedValue.
        if (accessor->getAccessorKind() == AccessorKind::Get ||
            accessor->getAccessorKind() == AccessorKind::Read) {
          if (wrappedValue->getAttrs().getUnavailable(ctx)) {
            ExportContext where = ExportContext::forDeclSignature(var);
            diagnoseExplicitUnavailability(
                wrappedValue,
                var->getAttachedPropertyWrappers()[i]->getRangeWithAt(),
                where, nullptr);
          }
        }

        underlyingVars.push_back({ wrappedValue, isWrapperRefLValue });
      }
    }
    semantics = AccessSemantics::DirectToStorage;
    selfAccessKind = SelfAccessorKind::Peer;
    break;
  }

  case TargetImpl::WrapperStorage: {
    auto var =
        cast<VarDecl>(accessor->getStorage())->getOriginalWrappedProperty();
    auto *backing = var->getPropertyWrapperBackingProperty();

    // Error recovery.
    if (!backing || backing->isInvalid())
      return nullptr;

    storage = backing;

    enclosingSelfAccess =
        getEnclosingSelfPropertyWrapperAccess(var, /*forProjected=*/true);
    if (!enclosingSelfAccess) {
      auto projectionVar = cast<VarDecl>(accessor->getStorage());
      if (auto lvalueness = getPropertyWrapperLValueness(projectionVar)) {
        isMemberLValue =
          (lvalueness->isLValueForGetAccess[0] && isUsedForGetAccess) ||
          (lvalueness->isLValueForSetAccess[0] && isUsedForSetAccess);
      }
      underlyingVars.push_back(
        { var->getAttachedPropertyWrapperTypeInfo(0).projectedValueVar,
          isLValue });
    }
    semantics = AccessSemantics::DirectToStorage;
    selfAccessKind = SelfAccessorKind::Peer;
    break;
  }
  }

  // If the base is not 'self', default get access to nonmutating and set access to mutating.
  bool getterMutatesBase = selfDecl && storage->isGetterMutating();
  bool setterMutatesBase = !selfDecl || storage->isSetterMutating();
  // If we're not accessing via a property wrapper, we don't need to adjust
  // the mutability.
  if (target == TargetImpl::Wrapper || target == TargetImpl::WrapperStorage) {
    auto var = cast<VarDecl>(accessor->getStorage());
    auto mutability = var->getPropertyWrapperMutability();
    // Only adjust mutability if it's possible to mutate the base.
    if (mutability && !var->isStatic() &&
        !(selfDecl && selfTypeForAccess->hasReferenceSemantics())) {
      getterMutatesBase = (mutability->Getter == PropertyWrapperMutability::Mutating);
      setterMutatesBase = (mutability->Setter == PropertyWrapperMutability::Mutating);
    }
  }

  // If the accessor is mutating, then the base should be referred as an l-value
  bool isBaseLValue = (getterMutatesBase && isUsedForGetAccess) ||
                      (setterMutatesBase && isUsedForSetAccess);

  if (!selfDecl) {
    assert(target != TargetImpl::Super);
    auto *storageDRE = new (ctx) DeclRefExpr(storage, DeclNameLoc(),
                                             /*IsImplicit=*/true, semantics);
    auto type = storage->getValueInterfaceType().subst(subs);
    if (isBaseLValue)
      type = LValueType::get(type);
    storageDRE->setType(type);

    return finish(storageDRE);
  }

  // Build self
  Expr *selfDRE = buildSelfReference(selfDecl, selfAccessKind, isBaseLValue,
                                     /*convertTy*/ selfTypeForAccess);

  // Build self.member or equivalent

  Expr *lookupExpr;
  ConcreteDeclRef memberRef(storage, subs);
  auto type = storage->getValueInterfaceType().subst(subs);
  if (isMemberLValue)
    type = LValueType::get(type);

  // When we are performing access via a property wrapper's static subscript
  // that accepts the enclosing self along with key paths, form that subscript
  // operation now.
  if (enclosingSelfAccess) {
    Type storageType = storage->getValueInterfaceType().subst(subs);
    // Metatype instance for the wrapper type itself.
    TypeExpr *wrapperMetatype = TypeExpr::createImplicit(storageType, ctx);

    // Key path referring to the property being accessed.
    Expr *propertyKeyPath = new (ctx) KeyPathDotExpr(SourceLoc());
    propertyKeyPath = UnresolvedDotExpr::createImplicit(ctx, propertyKeyPath,
        enclosingSelfAccess->accessedProperty->getName());
    propertyKeyPath = KeyPathExpr::createImplicit(
        ctx, /*backslashLoc*/ SourceLoc(), /*parsedRoot*/ nullptr,
        propertyKeyPath, /*hasLeadingDot*/ true);

    // Key path referring to the backing storage property.
    Expr *storageKeyPath = new (ctx) KeyPathDotExpr(SourceLoc());
    storageKeyPath = UnresolvedDotExpr::createImplicit(ctx, storageKeyPath,
                                                       storage->getName());
    storageKeyPath = KeyPathExpr::createImplicit(
        ctx, /*backslashLoc*/ SourceLoc(), /*parsedRoot*/ nullptr,
        storageKeyPath, /*hasLeadingDot*/ true);
    Expr *args[3] = {selfDRE, propertyKeyPath, storageKeyPath};

    auto *subscriptDecl = enclosingSelfAccess->subscript;
    auto argList =
        ArgumentList::forImplicitCallTo(subscriptDecl->getIndices(), args, ctx);
    lookupExpr = SubscriptExpr::create(ctx, wrapperMetatype, argList,
                                       subscriptDecl, /*Implicit=*/true);

    // FIXME: Since we're not resolving overloads or anything, we should be
    // building fully type-checked AST above; we already have all the
    // information that we need.
    if (!TypeChecker::typeCheckExpression(lookupExpr, accessor))
      return nullptr;

    // Make sure we produce an lvalue only when desired.
    if (isMemberLValue != lookupExpr->getType()->is<LValueType>()) {
      if (isMemberLValue) {
        // Strip off an extraneous load.
        if (auto load = dyn_cast<LoadExpr>(lookupExpr))
          lookupExpr = load->getSubExpr();
      } else {
        lookupExpr = new (ctx) LoadExpr(
            lookupExpr, lookupExpr->getType()->getRValueType());
      }
    }
  } else if (isa<SubscriptDecl>(storage)) {
    auto *argList = buildSubscriptArgumentList(ctx, accessor);
    lookupExpr = SubscriptExpr::create(ctx, selfDRE, argList, memberRef,
                                       /*IsImplicit=*/true, semantics);

    if (selfAccessKind == SelfAccessorKind::Super)
      cast<LookupExpr>(lookupExpr)->setIsSuper(true);

    lookupExpr->setType(type);

  } else {
    lookupExpr = new (ctx) MemberRefExpr(selfDRE, SourceLoc(), memberRef,
                                         DeclNameLoc(), /*IsImplicit=*/true,
                                         semantics);

    if (selfAccessKind == SelfAccessorKind::Super)
      cast<LookupExpr>(lookupExpr)->setIsSuper(true);

    lookupExpr->setType(type);
  }

  // Build self.member.wrappedValue if applicable

  return finish(lookupExpr);
}

/// Load the value of VD.  If VD is an @override of another value, we call the
/// superclass getter.  Otherwise, we do a direct load of the value.
static Expr *
createPropertyLoadOrCallSuperclassGetter(AccessorDecl *accessor,
                                         AbstractStorageDecl *storage,
                                         TargetImpl target,
                                         ASTContext &ctx) {
  return buildStorageReference(accessor, storage, target,
                               /*isUsedForGetAccess=*/true,
                               /*isUsedForSetAccess=*/false,
                               ctx);
}

static ProtocolConformanceRef checkConformanceToNSCopying(VarDecl *var,
                                                          Type type) {
  auto dc = var->getDeclContext();
  auto &ctx = dc->getASTContext();
  auto proto = ctx.getNSCopyingDecl();

  if (proto) {
    if (auto result = TypeChecker::conformsToProtocol(type, proto,
                                                      dc->getParentModule()))
      return result;
  }

  ctx.Diags.diagnose(var->getLoc(), diag::nscopying_doesnt_conform);
  return ProtocolConformanceRef::forInvalid();
}

static std::pair<Type, bool> getUnderlyingTypeOfVariable(VarDecl *var) {
  Type type = var->getType()->getReferenceStorageReferent();

  if (Type objectType = type->getOptionalObjectType()) {
    return {objectType, true};
  } else {
    return {type, false};
  }
}

ProtocolConformanceRef TypeChecker::checkConformanceToNSCopying(VarDecl *var) {
  Type type = getUnderlyingTypeOfVariable(var).first;
  return ::checkConformanceToNSCopying(var, type);
}

/// Synthesize the code to store 'Val' to 'VD', given that VD has an @NSCopying
/// attribute on it.  We know that VD is a stored property in a class, so we
/// just need to generate something like "self.property = val.copy(zone: nil)"
/// here.  This does some type checking to validate that the call will succeed.
static Expr *synthesizeCopyWithZoneCall(Expr *Val, VarDecl *VD,
                                        ASTContext &Ctx) {
  // We support @NSCopying on class types (which conform to NSCopying),
  // protocols which conform, and option types thereof.
  auto underlyingTypeAndIsOptional = getUnderlyingTypeOfVariable(VD);
  auto underlyingType = underlyingTypeAndIsOptional.first;
  auto isOptional = underlyingTypeAndIsOptional.second;

  // The element type must conform to NSCopying.  If not, emit an error and just
  // recovery by synthesizing without the copy call.
  auto conformance = checkConformanceToNSCopying(VD, underlyingType);
  if (!conformance)
    return Val;

  //- (id)copyWithZone:(NSZone *)zone;
  DeclName copyWithZoneName(Ctx, Ctx.getIdentifier("copy"), { Ctx.Id_with });
  FuncDecl *copyMethod = nullptr;
  for (auto member : conformance.getRequirement()->getMembers()) {
    if (auto func = dyn_cast<FuncDecl>(member)) {
      if (func->getName() == copyWithZoneName) {
        copyMethod = func;
        break;
      }
    }
  }
  assert(copyMethod != nullptr);

  // If we have an optional type, we have to "?" the incoming value to only
  // evaluate the subexpression if the incoming value is non-null.
  if (isOptional) {
    Val = new (Ctx) BindOptionalExpr(Val, SourceLoc(), 0);
    Val->setType(underlyingType);
  }

  SubstitutionMap subs =
      SubstitutionMap::get(copyMethod->getGenericSignature(), {underlyingType},
                           ArrayRef<ProtocolConformanceRef>(conformance));
  ConcreteDeclRef copyMethodRef(copyMethod, subs);
  auto copyMethodType = copyMethod->getInterfaceType()
                           ->castTo<GenericFunctionType>()
                           ->substGenericArgs(subs);
  auto DRE = new (Ctx) DeclRefExpr(copyMethodRef, DeclNameLoc(),
                                   /*IsImplicit=*/true);
  DRE->setType(copyMethodType);

  // Drop the self type
  copyMethodType = copyMethodType->getResult()->castTo<FunctionType>();

  auto DSCE = DotSyntaxCallExpr::create(Ctx, DRE, SourceLoc(),
                                        Argument::unlabeled(Val));
  DSCE->setImplicit();
  DSCE->setType(copyMethodType);
  DSCE->setThrows(false);

  Expr *Nil = new (Ctx) NilLiteralExpr(SourceLoc(), /*implicit*/true);
  Nil->setType(copyMethodType->getParams()[0].getParameterType());

  auto *argList =
      ArgumentList::forImplicitCallTo(copyMethod->getParameters(), {Nil}, Ctx);
  auto *Call = CallExpr::createImplicit(Ctx, DSCE, argList);
  Call->setType(copyMethodType->getResult());
  Call->setThrows(false);

  // If we're working with non-optional types, we're forcing the cast.
  if (!isOptional) {
    auto *const Cast =
        ForcedCheckedCastExpr::createImplicit(Ctx, Call, underlyingType);
    Cast->setCastKind(CheckedCastKind::ValueCast);

    return Cast;
  }

  // We're working with optional types, so perform a conditional checked
  // downcast.
  auto *const Cast =
      ConditionalCheckedCastExpr::createImplicit(Ctx, Call, underlyingType);
  Cast->setCastKind(CheckedCastKind::ValueCast);

  // Use OptionalEvaluationExpr to evaluate the "?".
  auto *Result = new (Ctx) OptionalEvaluationExpr(Cast);
  Result->setType(OptionalType::get(underlyingType));

  return Result;
}

/// In a synthesized accessor body, store 'value' to the appropriate element.
///
/// If the property is an override, we call the superclass setter.
/// Otherwise, we do a direct store of the value.
static
void createPropertyStoreOrCallSuperclassSetter(AccessorDecl *accessor,
                                               Expr *value,
                                               AbstractStorageDecl *storage,
                                               TargetImpl target,
                                               SmallVectorImpl<ASTNode> &body,
                                               ASTContext &ctx) {
  // If the storage is an @NSCopying property, then we store the
  // result of a copyWithZone call on the value, not the value itself.
  if (auto property = dyn_cast<VarDecl>(storage)) {
    if (property->getAttrs().hasAttribute<NSCopyingAttr>())
      value = synthesizeCopyWithZoneCall(value, property, ctx);
  }

  // Error recovery.
  if (value->getType()->hasError())
    return;

  Expr *dest = buildStorageReference(accessor, storage, target,
                                     /*isUsedForGetAccess=*/false,
                                     /*isUsedForSetAccess=*/true,
                                     ctx);

  // Error recovery.
  if (dest == nullptr)
    return;

  // A lazy property setter will store a value of type T into underlying storage
  // of type T?.
  auto destType = dest->getType()->getWithoutSpecifierType();

  // Error recovery.
  if (destType->hasError())
    return;

  if (!destType->isEqual(value->getType())) {
    assert(destType->getOptionalObjectType());
    assert(destType->getOptionalObjectType()->isEqual(value->getType()));
    value = new (ctx) InjectIntoOptionalExpr(value, destType);
  }

  auto *assign = new (ctx) AssignExpr(dest, SourceLoc(), value,
                                      /*IsImplicit=*/true);
  assign->setType(ctx.TheEmptyTupleType);

  body.push_back(assign);
}

LLVM_ATTRIBUTE_UNUSED
static bool isSynthesizedComputedProperty(AbstractStorageDecl *storage) {
  return (storage->getAttrs().hasAttribute<LazyAttr>() ||
          storage->getAttrs().hasAttribute<NSManagedAttr>() ||
          (isa<VarDecl>(storage) &&
           cast<VarDecl>(storage)->hasAttachedPropertyWrapper()));
}

/// Synthesize the body of a trivial getter.  For a non-member vardecl or one
/// which is not an override of a base class property, it performs a direct
/// storage load.  For an override of a base member property, it chains up to
/// super.
static std::pair<BraceStmt *, bool>
synthesizeTrivialGetterBody(AccessorDecl *getter, TargetImpl target,
                            ASTContext &ctx) {
  auto storage = getter->getStorage();
  assert(!isSynthesizedComputedProperty(storage) ||
         target == TargetImpl::Wrapper ||
         target == TargetImpl::WrapperStorage);

  SourceLoc loc = storage->getLoc();

  Expr *result =
    createPropertyLoadOrCallSuperclassGetter(getter, storage, target, ctx);

  SmallVector<ASTNode, 2> body;
  if (result != nullptr) {
    ASTNode returnStmt = new (ctx) ReturnStmt(SourceLoc(), result,
                                              /*IsImplicit=*/true);
    body.push_back(returnStmt);
  }

  return { BraceStmt::create(ctx, loc, body, loc, true),
           /*isTypeChecked=*/true };
}

/// Synthesize the body of a getter which just directly accesses the
/// underlying storage.
static std::pair<BraceStmt *, bool>
synthesizeTrivialGetterBody(AccessorDecl *getter, ASTContext &ctx) {
  assert(getter->getStorage()->getImplInfo().hasStorage());
  return synthesizeTrivialGetterBody(getter, TargetImpl::Storage, ctx);
}

/// Synthesize the body of a getter which just delegates to its superclass
/// implementation.
static std::pair<BraceStmt *, bool>
synthesizeInheritedGetterBody(AccessorDecl *getter, ASTContext &ctx) {
  // This should call the superclass getter.
  return synthesizeTrivialGetterBody(getter, TargetImpl::Super, ctx);
}

/// Synthesize the body of a getter which just delegates to an addressor.
static std::pair<BraceStmt *, bool>
synthesizeAddressedGetterBody(AccessorDecl *getter, ASTContext &ctx) {
  assert(getter->getStorage()->getParsedAccessor(AccessorKind::Address));

  // This should call the addressor.
  return synthesizeTrivialGetterBody(getter, TargetImpl::Implementation, ctx);
}

/// Synthesize the body of a getter which just delegates to a read
/// coroutine accessor.
static std::pair<BraceStmt *, bool>
synthesizeReadCoroutineGetterBody(AccessorDecl *getter, ASTContext &ctx) {
  assert(getter->getStorage()->getParsedAccessor(AccessorKind::Read));

  // This should call the read coroutine.
  return synthesizeTrivialGetterBody(getter, TargetImpl::Implementation, ctx);
}

namespace {
  /// This ASTWalker explores an expression tree looking for expressions (which
  /// are DeclContext's) and changes their parent DeclContext to NewDC.
  class RecontextualizeClosures : public ASTWalker {
    DeclContext *NewDC;
  public:
    RecontextualizeClosures(DeclContext *NewDC) : NewDC(NewDC) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      // If we find a closure, update its declcontext and do *not* walk into it.
      if (auto CE = dyn_cast<AbstractClosureExpr>(E)) {
        CE->setParent(NewDC);
        return Action::SkipChildren(E);
      }
      
      if (auto CLE = dyn_cast<CaptureListExpr>(E)) {
        // Make sure to recontextualize any decls in the capture list as well.
        for (auto &CLE : CLE->getCaptureList()) {
          CLE.getVar()->setDeclContext(NewDC);
          CLE.PBD->setDeclContext(NewDC);
        }
      }
      
      // Unlike a closure, a TapExpr is not a DeclContext, so we need to
      // recontextualize its variable and then anything else in its body.
      // FIXME: Might be better to change walkToDeclPre() and walkToStmtPre()
      // below, but I don't know what other effects that might have.
      if (auto TE = dyn_cast<TapExpr>(E)) {
        TE->getVar()->setDeclContext(NewDC);
        for (auto node : TE->getBody()->getElements())
          node.walk(RecontextualizeClosures(NewDC));
      }

      return Action::Continue(E);
    }

    /// We don't want to recurse into declarations or statements.
    PreWalkAction walkToDeclPre(Decl *) override {
      return Action::SkipChildren();
    }
    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      return Action::SkipChildren(S);
    }
  };
} // end anonymous namespace

/// Synthesize the getter for a lazy property with the specified storage
/// vardecl.
static std::pair<BraceStmt *, bool>
synthesizeLazyGetterBody(AccessorDecl *Get, VarDecl *VD, VarDecl *Storage,
                         ASTContext &Ctx) {
  // The getter checks the optional, storing the initial value in if nil.  The
  // specific pattern we generate is:
  //   get {
  //     if let tmp1 = storage {
  //       return tmp1
  //     }
  //     let tmp2 : Ty = <<initializer expression>>
  //     storage = tmp2
  //     return tmp2
  //   }
  SmallVector<ASTNode, 6> Body;

  // Load the existing storage and store it into the 'tmp1' temporary.
  auto *Tmp1VD = new (Ctx) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
                                   SourceLoc(), Ctx.getIdentifier("tmp1"), Get);
  Tmp1VD->setInterfaceType(VD->getValueInterfaceType());
  Tmp1VD->setImplicit();

  auto *Named = NamedPattern::createImplicit(Ctx, Tmp1VD, Tmp1VD->getType());
  auto *Let =
      BindingPattern::createImplicit(Ctx, VarDecl::Introducer::Let, Named);
  Let->setType(Named->getType());
  auto *Some = OptionalSomePattern::createImplicit(Ctx, Let);
  Some->setType(OptionalType::get(Let->getType()));

  auto *StoredValueExpr =
    createPropertyLoadOrCallSuperclassGetter(Get, Storage,
                                             TargetImpl::Storage, Ctx);
  SmallVector<StmtConditionElement, 1> Cond;
  Cond.emplace_back(SourceLoc(), Some, StoredValueExpr);

  // Build the early return inside the if.
  auto *Tmp1DRE = new (Ctx) DeclRefExpr(Tmp1VD, DeclNameLoc(), /*Implicit*/true,
                                        AccessSemantics::Ordinary);
  Tmp1DRE->setType(Tmp1VD->getType());
  auto *Return = new (Ctx) ReturnStmt(SourceLoc(), Tmp1DRE,
                                      /*implicit*/true);


  // Build the "if" around the early return.
  Body.push_back(new (Ctx) IfStmt(LabeledStmtInfo(),
                                  SourceLoc(), Ctx.AllocateCopy(Cond), Return,
                                  /*elseloc*/SourceLoc(), /*else*/nullptr,
                                  /*implicit*/ true));


  auto *Tmp2VD = new (Ctx) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
                                   SourceLoc(), Ctx.getIdentifier("tmp2"),
                                   Get);
  Tmp2VD->setInterfaceType(VD->getValueInterfaceType());
  Tmp2VD->setImplicit();


  // Take the initializer from the PatternBindingDecl for VD.
  // TODO: This doesn't work with complicated patterns like:
  //   lazy var (a,b) = foo()
  auto PBD = VD->getParentPatternBinding();
  unsigned entryIndex = PBD->getPatternEntryIndexForVarDecl(VD);

  Expr *InitValue;
  if (PBD->getInit(entryIndex)) {
    PBD->setInitializerSubsumed(entryIndex);

    if (!PBD->isInitializerChecked(entryIndex))
      TypeChecker::typeCheckPatternBinding(PBD, entryIndex);

    InitValue = PBD->getInit(entryIndex);
  } else {
    InitValue = new (Ctx) ErrorExpr(SourceRange(), Tmp2VD->getType());
  }

  // Recontextualize any closure declcontexts nested in the initializer to
  // realize that they are in the getter function.
  if (Get->hasImplicitSelfDecl())
    Get->getImplicitSelfDecl()->setDeclContext(Get);

  InitValue->walk(RecontextualizeClosures(Get));

  // Wrap the initializer in a LazyInitializerExpr to avoid walking it twice.
  auto initType = InitValue->getType();
  InitValue = new (Ctx) LazyInitializerExpr(InitValue);
  InitValue->setType(initType);

  Pattern *Tmp2PBDPattern =
      NamedPattern::createImplicit(Ctx, Tmp2VD, Tmp2VD->getType());
  Tmp2PBDPattern =
    TypedPattern::createImplicit(Ctx, Tmp2PBDPattern, Tmp2VD->getType());

  auto *Tmp2PBD = PatternBindingDecl::createImplicit(
      Ctx, StaticSpellingKind::None, Tmp2PBDPattern, InitValue, Get,
      /*VarLoc*/ InitValue->getStartLoc());
  Body.push_back(Tmp2PBD);
  Body.push_back(Tmp2VD);

  // Assign tmp2 into storage.
  auto Tmp2DRE = new (Ctx) DeclRefExpr(Tmp2VD, DeclNameLoc(), /*Implicit*/true,
                                       AccessSemantics::DirectToStorage);
  Tmp2DRE->setType(Tmp2VD->getType());
  createPropertyStoreOrCallSuperclassSetter(Get, Tmp2DRE, Storage,
                                            TargetImpl::Storage, Body, Ctx);

  // Return tmp2.
  Tmp2DRE = new (Ctx) DeclRefExpr(Tmp2VD, DeclNameLoc(), /*Implicit*/true,
                                  AccessSemantics::DirectToStorage);
  Tmp2DRE->setType(Tmp2VD->getType());

  Body.push_back(new (Ctx) ReturnStmt(SourceLoc(), Tmp2DRE, /*implicit*/true));

  auto Range = InitValue->getSourceRange();
  return { BraceStmt::create(Ctx, Range.Start, Body, Range.End,
                             /*implicit*/true),
           /*isTypeChecked=*/true };
}

/// Synthesize the body of a getter for a property wrapper, which
/// delegates to the wrapper's "value" property.
static std::pair<BraceStmt *, bool>
synthesizePropertyWrapperGetterBody(AccessorDecl *getter, ASTContext &ctx) {
  return synthesizeTrivialGetterBody(getter, TargetImpl::Wrapper, ctx);
}

static std::pair<BraceStmt *, bool>
synthesizeInvalidAccessor(AccessorDecl *accessor, ASTContext &ctx) {
  auto loc = accessor->getLoc();
  return { BraceStmt::create(ctx, loc, ArrayRef<ASTNode>(), loc, true), true };
}

static std::pair<BraceStmt *, bool>
synthesizeGetterBody(AccessorDecl *getter, ASTContext &ctx) {
  auto storage = getter->getStorage();

  // Synthesize the getter for a lazy property or property wrapper.
  if (auto var = dyn_cast<VarDecl>(storage)) {
    if (var->getAttrs().hasAttribute<LazyAttr>()) {
      auto *storage = var->getLazyStorageProperty();
      return synthesizeLazyGetterBody(getter, var, storage, ctx);
    }

    if (var->hasAttachedPropertyWrapper()) {
      return synthesizePropertyWrapperGetterBody(getter, ctx);
    }

    if (var->getOriginalWrappedProperty(
            PropertyWrapperSynthesizedPropertyKind::Projection)) {
      return synthesizeTrivialGetterBody(getter, TargetImpl::WrapperStorage,
                                         ctx);
    }
  }

  if (getter->hasForcedStaticDispatch()) {
    return synthesizeTrivialGetterBody(getter, TargetImpl::Ordinary, ctx);
  }

  switch (getter->getStorage()->getReadImpl()) {
  case ReadImplKind::Stored:
    return synthesizeTrivialGetterBody(getter, ctx);

  case ReadImplKind::Get:
    return synthesizeInvalidAccessor(getter, ctx);

  case ReadImplKind::Inherited:
    return synthesizeInheritedGetterBody(getter, ctx);

  case ReadImplKind::Address:
    return synthesizeAddressedGetterBody(getter, ctx);

  case ReadImplKind::Read:
    return synthesizeReadCoroutineGetterBody(getter, ctx);
  }
  llvm_unreachable("bad ReadImplKind");
}

/// Synthesize the body of a setter which just stores to the given storage
/// declaration (which doesn't have to be the storage for the setter).
static std::pair<BraceStmt *, bool>
synthesizeTrivialSetterBodyWithStorage(AccessorDecl *setter,
                                       TargetImpl target,
                                       AbstractStorageDecl *storageToUse,
                                       ASTContext &ctx) {
  SourceLoc loc = setter->getStorage()->getLoc();

  VarDecl *valueParamDecl = setter->getParameters()->get(0);

  auto *valueDRE =
    new (ctx) DeclRefExpr(valueParamDecl, DeclNameLoc(), /*IsImplicit=*/true);
  valueDRE->setType(valueParamDecl->getType());

  SmallVector<ASTNode, 1> setterBody;

  createPropertyStoreOrCallSuperclassSetter(setter, valueDRE, storageToUse,
                                            target, setterBody, ctx);
  return { BraceStmt::create(ctx, loc, setterBody, loc, true),
           /*isTypeChecked=*/true };
}

static std::pair<BraceStmt *, bool>
synthesizeTrivialSetterBody(AccessorDecl *setter, ASTContext &ctx) {
  auto storage = setter->getStorage();
  assert(!isSynthesizedComputedProperty(storage));

  return synthesizeTrivialSetterBodyWithStorage(setter, TargetImpl::Storage,
                                                storage, ctx);
}

/// Synthesize the body of a setter for a property wrapper, which
/// delegates to the wrapper's "value" property.
static std::pair<BraceStmt *, bool>
synthesizePropertyWrapperSetterBody(AccessorDecl *setter, ASTContext &ctx) {
  return synthesizeTrivialSetterBodyWithStorage(setter, TargetImpl::Wrapper,
                                                setter->getStorage(), ctx);
}

/// Synthesize the body of a setter which just delegates to a mutable
/// addressor.
static std::pair<BraceStmt *, bool>
synthesizeMutableAddressSetterBody(AccessorDecl *setter, ASTContext &ctx) {
  // This should call the mutable addressor.
  return synthesizeTrivialSetterBodyWithStorage(setter,
                                                TargetImpl::Implementation,
                                                setter->getStorage(), ctx);
}

/// Synthesize the body of a setter which just delegates to a modify
/// coroutine accessor.
static std::pair<BraceStmt *, bool>
synthesizeModifyCoroutineSetterBody(AccessorDecl *setter, ASTContext &ctx) {
  // This should call the modify coroutine.
  return synthesizeTrivialSetterBodyWithStorage(setter,
                                                TargetImpl::Implementation,
                                                setter->getStorage(), ctx);
}

static Expr *maybeWrapInOutExpr(Expr *expr, ASTContext &ctx) {
  if (auto lvalueType = expr->getType()->getAs<LValueType>()) {
    auto type = lvalueType->getObjectType();
    return new (ctx) InOutExpr(SourceLoc(), expr, type, true);
  }

  return expr;
}

/// Given a VarDecl with a willSet: and/or didSet: specifier, synthesize the
/// setter which calls them.
static std::pair<BraceStmt *, bool>
synthesizeObservedSetterBody(AccessorDecl *Set, TargetImpl target,
                             ASTContext &Ctx, bool isLazy = false) {
  auto VD = cast<VarDecl>(Set->getStorage());

  SourceLoc Loc = VD->getLoc();

  // Start by finding the decls for 'self' and 'value'.
  auto *SelfDecl = Set->getImplicitSelfDecl();
  VarDecl *ValueDecl = Set->getParameters()->get(0);

  bool IsSelfLValue = VD->isSetterMutating();

  SubstitutionMap subs;
  if (auto *genericEnv = Set->getGenericEnvironment())
    subs = genericEnv->getForwardingSubstitutionMap();

  // The setter loads the oldValue, invokes willSet with the incoming value,
  // does a direct store, then invokes didSet with the oldValue.
  SmallVector<ASTNode, 6> SetterBody;

  auto callObserver = [&](AccessorDecl *observer, VarDecl *arg) {
    ConcreteDeclRef ref(observer, subs);
    auto type = observer->getInterfaceType().subst(subs);
    Expr *Callee = new (Ctx) DeclRefExpr(ref, DeclNameLoc(), /*imp*/true);
    Callee->setType(type);

    DeclRefExpr *ValueDRE = nullptr;
    if (arg) {
      ValueDRE = new (Ctx) DeclRefExpr(arg, DeclNameLoc(), /*imp*/ true);
      ValueDRE->setType(arg->getType());
    }

    if (SelfDecl) {
      auto SelfArg = buildSelfArgument(SelfDecl, SelfAccessorKind::Peer,
                                       /*isMutable*/ IsSelfLValue);
      auto *DSCE = DotSyntaxCallExpr::create(Ctx, Callee, SourceLoc(), SelfArg);

      if (auto funcType = type->getAs<FunctionType>())
        type = funcType->getResult();
      DSCE->setType(type);
      DSCE->setThrows(false);
      Callee = DSCE;
    }

    CallExpr *Call = nullptr;
    if (arg) {
      auto *argList = ArgumentList::forImplicitUnlabeled(Ctx, {ValueDRE});
      Call = CallExpr::createImplicit(Ctx, Callee, argList);
    } else {
      Call = CallExpr::createImplicitEmpty(Ctx, Callee);
    }

    if (auto funcType = type->getAs<FunctionType>())
      type = funcType->getResult();
    Call->setType(type);
    Call->setThrows(false);

    SetterBody.push_back(Call);
  };

  // If there is a didSet, it will take the old value.  Load it into a temporary
  // 'let' so we have it for later.
  VarDecl *OldValue = nullptr;
  if (auto didSet = VD->getParsedAccessor(AccessorKind::DidSet)) {
    // Only do the load if the didSet body references the implicit oldValue
    // parameter or it's provided explicitly in the parameter list.
    if (!didSet->isSimpleDidSet()) {
      Expr *OldValueExpr =
          buildStorageReference(Set, VD, isLazy ? TargetImpl::Ordinary : target,
                                /*isUsedForGetAccess=*/true,
                                /*isUsedForSetAccess=*/true, Ctx);

      // Error recovery.
      if (OldValueExpr == nullptr) {
        OldValueExpr = new (Ctx) ErrorExpr(SourceRange(), VD->getType());
      } else {
        OldValueExpr = new (Ctx) LoadExpr(OldValueExpr, VD->getType());
      }

      OldValue = new (Ctx) VarDecl(/*IsStatic*/ false, VarDecl::Introducer::Let,
                                   SourceLoc(), Ctx.getIdentifier("tmp"), Set);
      OldValue->setImplicit();
      OldValue->setInterfaceType(VD->getValueInterfaceType());
      auto *tmpPattern =
          NamedPattern::createImplicit(Ctx, OldValue, OldValue->getType());
      auto *tmpPBD = PatternBindingDecl::createImplicit(
          Ctx, StaticSpellingKind::None, tmpPattern, OldValueExpr, Set);
      SetterBody.push_back(tmpPBD);
      SetterBody.push_back(OldValue);
    }
  }

  if (auto willSet = VD->getParsedAccessor(AccessorKind::WillSet))
    callObserver(willSet, ValueDecl);
  
  // Create an assignment into the storage or call to superclass setter.
  auto *ValueDRE = new (Ctx) DeclRefExpr(ValueDecl, DeclNameLoc(), true);
  ValueDRE->setType(ValueDecl->getType());
  createPropertyStoreOrCallSuperclassSetter(
      Set, ValueDRE, isLazy ? VD->getLazyStorageProperty() : VD, target,
      SetterBody, Ctx);

  if (auto didSet = VD->getParsedAccessor(AccessorKind::DidSet))
    callObserver(didSet, OldValue);

  return { BraceStmt::create(Ctx, Loc, SetterBody, Loc, true),
           /*isTypeChecked=*/true };
}

static std::pair<BraceStmt *, bool>
synthesizeStoredWithObserversSetterBody(AccessorDecl *setter, ASTContext &ctx) {
  return synthesizeObservedSetterBody(setter, TargetImpl::Storage, ctx);
}

static std::pair<BraceStmt *, bool>
synthesizeInheritedWithObserversSetterBody(AccessorDecl *setter,
                                           ASTContext &ctx) {
  return synthesizeObservedSetterBody(setter, TargetImpl::Super, ctx);
}

static std::pair<BraceStmt *, bool>
synthesizeSetterBody(AccessorDecl *setter, ASTContext &ctx) {
  auto storage = setter->getStorage();

  // Synthesize the setter for a lazy property or property wrapper.
  if (auto var = dyn_cast<VarDecl>(storage)) {
    if (var->getAttrs().hasAttribute<LazyAttr>()) {
      // Lazy property setters write to the underlying storage.
      if (var->hasObservers()) {
        return synthesizeObservedSetterBody(setter, TargetImpl::Storage, ctx,
                                            /*isLazy=*/true);
      }
      auto *storage = var->getLazyStorageProperty();
      return synthesizeTrivialSetterBodyWithStorage(setter, TargetImpl::Storage,
                                                    storage, ctx);
    }

    if (var->hasAttachedPropertyWrapper()) {
      if (var->hasObservers()) {
        return synthesizeObservedSetterBody(setter, TargetImpl::Wrapper, ctx);
      }

      return synthesizePropertyWrapperSetterBody(setter, ctx);
    }

    // Synthesize a setter for the storage wrapper property of a property
    // with an attached wrapper.
    if (auto original = var->getOriginalWrappedProperty(
            PropertyWrapperSynthesizedPropertyKind::Projection)) {
      auto backingVar = original->getPropertyWrapperBackingProperty();
      return synthesizeTrivialSetterBodyWithStorage(setter,
                                                    TargetImpl::WrapperStorage,
                                                    backingVar, ctx);
    }
  }

  switch (storage->getWriteImpl()) {
  case WriteImplKind::Immutable:
    llvm_unreachable("synthesizing setter from immutable storage");

  case WriteImplKind::Stored:
    return synthesizeTrivialSetterBody(setter, ctx);

  case WriteImplKind::StoredWithObservers:
    return synthesizeStoredWithObserversSetterBody(setter, ctx);

  case WriteImplKind::InheritedWithObservers:
    return synthesizeInheritedWithObserversSetterBody(setter, ctx);

  case WriteImplKind::Set:
    return synthesizeInvalidAccessor(setter, ctx);

  case WriteImplKind::MutableAddress:
    return synthesizeMutableAddressSetterBody(setter, ctx);

  case WriteImplKind::Modify:
    return synthesizeModifyCoroutineSetterBody(setter, ctx);
  }
  llvm_unreachable("bad WriteImplKind");
}

static std::pair<BraceStmt *, bool>
synthesizeModifyCoroutineBodyWithSimpleDidSet(AccessorDecl *accessor,
                                              ASTContext &ctx) {
  auto storage = accessor->getStorage();
  SourceLoc loc = storage->getLoc();
  auto isOverride = storage->getOverriddenDecl();
  auto target = isOverride ? TargetImpl::Super : TargetImpl::Storage;

  SmallVector<ASTNode, 1> body;

  Expr *ref = buildStorageReference(accessor, storage, target,
                                    /*isUsedForGetAccess=*/true,
                                    /*isUsedForSetAccess=*/true,
                                    ctx);
  ref = maybeWrapInOutExpr(ref, ctx);

  YieldStmt *yield = YieldStmt::create(ctx, loc, loc, ref, loc, true);
  body.push_back(yield);

  auto Set = storage->getAccessor(AccessorKind::Set);
  auto DidSet = storage->getAccessor(AccessorKind::DidSet);
  auto *SelfDecl = accessor->getImplicitSelfDecl();

  SubstitutionMap subs;
  if (auto *genericEnv = Set->getGenericEnvironment())
    subs = genericEnv->getForwardingSubstitutionMap();

  auto callDidSet = [&]() {
    ConcreteDeclRef ref(DidSet, subs);
    auto type = DidSet->getInterfaceType().subst(subs);
    Expr *Callee = new (ctx) DeclRefExpr(ref, DeclNameLoc(), /*imp*/ true);
    Callee->setType(type);

    if (SelfDecl) {
      auto SelfArg =
          buildSelfArgument(SelfDecl, SelfAccessorKind::Peer,
                            /*isMutable*/ storage->isSetterMutating());
      auto *DSCE = DotSyntaxCallExpr::create(ctx, Callee, SourceLoc(), SelfArg);

      if (auto funcType = type->getAs<FunctionType>())
        type = funcType->getResult();
      DSCE->setType(type);
      DSCE->setThrows(false);
      Callee = DSCE;
    }

    auto *Call = CallExpr::createImplicitEmpty(ctx, Callee);
    if (auto funcType = type->getAs<FunctionType>())
      type = funcType->getResult();
    Call->setType(type);
    Call->setThrows(false);

    body.push_back(Call);
  };

  callDidSet();

  return {BraceStmt::create(ctx, loc, body, loc, true),
          /*isTypeChecked=*/true};
}

static std::pair<BraceStmt *, bool>
synthesizeCoroutineAccessorBody(AccessorDecl *accessor, ASTContext &ctx) {
  assert(accessor->isCoroutine());

  auto storage = accessor->getStorage();
  auto storageReadWriteImpl = storage->getReadWriteImpl();
  auto target = (accessor->hasForcedStaticDispatch()
                   ? TargetImpl::Ordinary
                   : TargetImpl::Implementation);

  // If this is a variable with an attached property wrapper, then
  // the accessors need to yield the wrappedValue or projectedValue.
  if (accessor->getAccessorKind() == AccessorKind::Read ||
      storageReadWriteImpl == ReadWriteImplKind::Modify) {
    if (auto var = dyn_cast<VarDecl>(storage)) {
      if (var->hasAttachedPropertyWrapper()) {
        target = TargetImpl::Wrapper;
      }

      if (var->getOriginalWrappedProperty(
              PropertyWrapperSynthesizedPropertyKind::Projection)) {
        target = TargetImpl::WrapperStorage;
      }
    }
  }

  SourceLoc loc = storage->getLoc();
  SmallVector<ASTNode, 1> body;

  bool isModify = accessor->getAccessorKind() == AccessorKind::Modify;

  // Special-case for a modify coroutine of a simple stored property with
  // observers. We can yield a borrowed copy of the underlying storage
  // in this case. However, if the accessor was synthesized on-demand,
  // we do the more general thing, because on-demand accessors might be
  // serialized, which prevents them from being able to directly reference
  // didSet/willSet accessors, which are private.
  if (isModify &&
      !accessor->hasForcedStaticDispatch() &&
      (storageReadWriteImpl == ReadWriteImplKind::StoredWithDidSet ||
       storageReadWriteImpl == ReadWriteImplKind::InheritedWithDidSet) &&
      storage->getParsedAccessor(AccessorKind::DidSet)->isSimpleDidSet()) {
    return synthesizeModifyCoroutineBodyWithSimpleDidSet(accessor, ctx);
  }

  // Build a reference to the storage.
  Expr *ref = buildStorageReference(accessor, storage, target,
                                    /*isUsedForGetAccess=*/true,
                                    /*isUsedForSetAccess=*/isModify,
                                    ctx);
  if (ref != nullptr) {
    // Wrap it with an `&` marker if this is a modify.
    ref = maybeWrapInOutExpr(ref, ctx);

    // Yield it.
    YieldStmt *yield = YieldStmt::create(ctx, loc, loc, ref, loc, true);
    body.push_back(yield);
  }

  return { BraceStmt::create(ctx, loc, body, loc, true),
           /*isTypeChecked=*/true };
}

/// Synthesize the body of a read coroutine.
static std::pair<BraceStmt *, bool>
synthesizeReadCoroutineBody(AccessorDecl *read, ASTContext &ctx) {
  assert(read->getStorage()->getReadImpl() != ReadImplKind::Read);
  return synthesizeCoroutineAccessorBody(read, ctx);
}

/// Synthesize the body of a modify coroutine.
static std::pair<BraceStmt *, bool>
synthesizeModifyCoroutineBody(AccessorDecl *modify, ASTContext &ctx) {
#ifndef NDEBUG
  auto storage = modify->getStorage();
  auto impl = storage->getReadWriteImpl();
  auto hasWrapper = isa<VarDecl>(storage) &&
                    cast<VarDecl>(storage)->hasAttachedPropertyWrapper();
  assert((hasWrapper || impl != ReadWriteImplKind::Modify) &&
         impl != ReadWriteImplKind::Immutable);
#endif
  return synthesizeCoroutineAccessorBody(modify, ctx);
}

static std::pair<BraceStmt *, bool>
synthesizeAccessorBody(AbstractFunctionDecl *fn, void *) {
  auto *accessor = cast<AccessorDecl>(fn);
  auto &ctx = accessor->getASTContext();

  if (ctx.Stats)
    ++ctx.Stats->getFrontendCounters().NumAccessorBodiesSynthesized;

  switch (accessor->getAccessorKind()) {
  case AccessorKind::Get:
    return synthesizeGetterBody(accessor, ctx);

  case AccessorKind::Set:
    return synthesizeSetterBody(accessor, ctx);

  case AccessorKind::Read:
    return synthesizeReadCoroutineBody(accessor, ctx);

  case AccessorKind::Modify:
    return synthesizeModifyCoroutineBody(accessor, ctx);

  case AccessorKind::WillSet:
  case AccessorKind::DidSet:
  case AccessorKind::Address:
  case AccessorKind::MutableAddress:
    break;

  case AccessorKind::Init:
    llvm_unreachable("init accessor not yet implemented");
  }
  llvm_unreachable("bad synthesized function kind");
}

static void finishImplicitAccessor(AccessorDecl *accessor,
                                   ASTContext &ctx) {
  accessor->setImplicit();

  if (ctx.Stats)
    ++ctx.Stats->getFrontendCounters().NumAccessorsSynthesized;

  if (doesAccessorHaveBody(accessor))
    accessor->setBodySynthesizer(&synthesizeAccessorBody);
}

static AccessorDecl *createGetterPrototype(AbstractStorageDecl *storage,
                                           ASTContext &ctx) {
  SourceLoc loc = storage->getLoc();

  ParamDecl *selfDecl = nullptr;
  if (storage->getDeclContext()->isTypeContext()) {
    if (storage->getAttrs().hasAttribute<LazyAttr>()) {
      // For lazy properties, steal the 'self' from the initializer context.
      auto *varDecl = cast<VarDecl>(storage);
      auto *bindingDecl = varDecl->getParentPatternBinding();
      const auto i = bindingDecl->getPatternEntryIndexForVarDecl(varDecl);
      auto *bindingInit = cast<PatternBindingInitializer>(
        bindingDecl->getInitContext(i));

      selfDecl = bindingInit->getImplicitSelfDecl();
    }
  }

  // Add an index-forwarding clause.
  auto *getterParams = buildIndexForwardingParamList(storage, {}, ctx);

  SourceLoc staticLoc;
  if (storage->isStatic())
    staticLoc = storage->getLoc();

  auto getter = AccessorDecl::create(
      ctx, loc, /*AccessorKeywordLoc*/ loc, AccessorKind::Get, storage,
      staticLoc, StaticSpellingKind::None,
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(), getterParams, Type(),
      storage->getDeclContext());
  getter->setSynthesized();

  // If we're stealing the 'self' from a lazy initializer, set it now.
  // Note that we don't re-parent the 'self' declaration to be part of
  // the getter until we synthesize the body of the getter later.
  if (selfDecl)
    *getter->getImplicitSelfDeclStorage() = selfDecl;

  if (storage->isGetterMutating())
    getter->setSelfAccessKind(SelfAccessKind::Mutating);
  else
    getter->setSelfAccessKind(SelfAccessKind::NonMutating);

  if (!storage->requiresOpaqueAccessor(AccessorKind::Get))
    getter->setForcedStaticDispatch(true);

  finishImplicitAccessor(getter, ctx);

  return getter;
}

static void addPropertyWrapperAccessorAvailability(VarDecl *var, AccessorKind accessorKind,
                                                   SmallVectorImpl<const Decl *> &asAvailableAs) {
  AccessorDecl *synthesizedFrom = nullptr;
  if (var->hasAttachedPropertyWrapper()) {
    AbstractStorageDecl *wrappedValueImpl;
    if (auto access = getEnclosingSelfPropertyWrapperAccess(var, /*forProjected=*/false)) {
      wrappedValueImpl = access->subscript;
    } else {
      wrappedValueImpl = var->getAttachedPropertyWrapperTypeInfo(0).valueVar;
    }

    // The property wrapper info may not actually link back to a wrapper
    // implementation, if there was a semantic error checking the wrapper.
    if (wrappedValueImpl) {
      synthesizedFrom = wrappedValueImpl->getOpaqueAccessor(accessorKind);
    }
  } else if (auto wrapperSynthesizedKind
               = var->getPropertyWrapperSynthesizedPropertyKind()) {
    switch (*wrapperSynthesizedKind) {
    case PropertyWrapperSynthesizedPropertyKind::Backing:
      break;

    case PropertyWrapperSynthesizedPropertyKind::Projection: {
      if (auto origVar = var->getOriginalWrappedProperty(wrapperSynthesizedKind)) {
        AbstractStorageDecl *projectedValueImpl;
        if (auto access = getEnclosingSelfPropertyWrapperAccess(origVar, /*forProjected=*/true)) {
          projectedValueImpl = access->subscript;
        } else {
          projectedValueImpl = origVar->getAttachedPropertyWrapperTypeInfo(0).projectedValueVar;
        }

        // The property wrapper info may not actually link back to a wrapper
        // implementation, if there was a semantic error checking the wrapper.
        if (projectedValueImpl) {
          synthesizedFrom = projectedValueImpl->getOpaqueAccessor(accessorKind);
        }
      }
      break;
    }
    }
  }

  // Infer availability from the accessor used for synthesis, and intersect it
  // with the availability of the enclosing scope.
  if (synthesizedFrom) {
    asAvailableAs.push_back(synthesizedFrom);
    if (auto *enclosingDecl = var->getInnermostDeclWithAvailability())
      asAvailableAs.push_back(enclosingDecl);
  }
}

static AccessorDecl *createSetterPrototype(AbstractStorageDecl *storage,
                                           ASTContext &ctx,
                                           AccessorDecl *getter = nullptr) {
  assert(storage->supportsMutation());

  SourceLoc loc = storage->getLoc();

  bool isMutating = storage->isSetterMutating();

  // Add a "(value : T, indices...)" argument list.
  auto *param = new (ctx) ParamDecl(SourceLoc(), SourceLoc(),
                                    Identifier(), loc,
                                    ctx.getIdentifier("value"),
                                    storage->getDeclContext());
  param->setSpecifier(ParamSpecifier::Default);
  param->setImplicit();

  auto *params = buildIndexForwardingParamList(storage, param, ctx);

  auto setter = AccessorDecl::create(
      ctx, loc, /*AccessorKeywordLoc*/ SourceLoc(), AccessorKind::Set, storage,
      /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(), params, Type(),
      storage->getDeclContext());
  setter->setSynthesized();

  if (isMutating)
    setter->setSelfAccessKind(SelfAccessKind::Mutating);
  else
    setter->setSelfAccessKind(SelfAccessKind::NonMutating);

  // All mutable storage requires a setter.
  assert(storage->requiresOpaqueAccessor(AccessorKind::Set));
  
  // Copy availability from the accessor we'll synthesize the setter from.
  SmallVector<const Decl *, 2> asAvailableAs;

  // That could be a property wrapper...
  if (auto var = dyn_cast<VarDecl>(storage)) {
    addPropertyWrapperAccessorAvailability(var, AccessorKind::Set, asAvailableAs);
  }


  // ...or another accessor.
  switch (storage->getWriteImpl()) {
  case WriteImplKind::Immutable:
    llvm_unreachable("synthesizing setter from immutable storage");
  case WriteImplKind::Stored:
  case WriteImplKind::StoredWithObservers:
  case WriteImplKind::InheritedWithObservers:
  case WriteImplKind::Set:
    // Setter's availability shouldn't be externally influenced in these
    // cases.
    break;
      
  case WriteImplKind::MutableAddress:
    if (auto addr = storage->getOpaqueAccessor(AccessorKind::MutableAddress)) {
      asAvailableAs.push_back(addr);
    }
    break;
  case WriteImplKind::Modify:
    if (auto mod = storage->getOpaqueAccessor(AccessorKind::Modify)) {
      asAvailableAs.push_back(mod);
    }
    break;
  }
  
  if (!asAvailableAs.empty()) {
    AvailabilityInference::applyInferredAvailableAttrs(
        setter, asAvailableAs, ctx);
  }
  
  finishImplicitAccessor(setter, ctx);

  return setter;
}

static AccessorDecl *
createCoroutineAccessorPrototype(AbstractStorageDecl *storage,
                                 AccessorKind kind,
                                ASTContext &ctx) {
  assert(kind == AccessorKind::Read || kind == AccessorKind::Modify);

  SourceLoc loc = storage->getLoc();

  bool isMutating = storage->isGetterMutating();
  if (kind == AccessorKind::Modify)
    isMutating |= storage->isSetterMutating();

  auto dc = storage->getDeclContext();

  // The forwarding index parameters.
  auto *params = buildIndexForwardingParamList(storage, {}, ctx);

  // Coroutine accessors always return ().
  const Type retTy = TupleType::getEmpty(ctx);

  auto *accessor = AccessorDecl::create(
      ctx, loc, /*AccessorKeywordLoc=*/SourceLoc(), kind, storage,
      /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
      /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
      /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(), params, retTy, dc);
  accessor->setSynthesized();

  if (isMutating)
    accessor->setSelfAccessKind(SelfAccessKind::Mutating);
  else
    accessor->setSelfAccessKind(SelfAccessKind::NonMutating);

  // If the storage does not provide this accessor as an opaque accessor,
  // we can't add a dynamically-dispatched method entry for the accessor,
  // so force it to be statically dispatched. ("final" would be inappropriate
  // because the property can still be overridden.)
  if (!storage->requiresOpaqueAccessor(kind))
    accessor->setForcedStaticDispatch(true);

  // Make sure the coroutine is available enough to access
  // the storage (and its getters/setters if it has them).
  SmallVector<const Decl *, 2> asAvailableAs;
  asAvailableAs.push_back(storage);
  if (FuncDecl *getter = storage->getParsedAccessor(AccessorKind::Get)) {
    asAvailableAs.push_back(getter);
  }
  if (kind == AccessorKind::Modify) {
    if (FuncDecl *setter = storage->getParsedAccessor(AccessorKind::Set)) {
      asAvailableAs.push_back(setter);
    }
  }

  if (auto var = dyn_cast<VarDecl>(storage)) {
    addPropertyWrapperAccessorAvailability(var, kind, asAvailableAs);
  }

  AvailabilityInference::applyInferredAvailableAttrs(accessor,
                                                     asAvailableAs, ctx);

  // A modify coroutine should have the same SPI visibility as the setter.
  if (kind == AccessorKind::Modify) {
    if (FuncDecl *setter = storage->getParsedAccessor(AccessorKind::Set))
      applyInferredSPIAccessControlAttr(accessor, setter, ctx);
  }

  finishImplicitAccessor(accessor, ctx);

  return accessor;
}

static AccessorDecl *
createReadCoroutinePrototype(AbstractStorageDecl *storage,
                             ASTContext &ctx) {
  return createCoroutineAccessorPrototype(storage, AccessorKind::Read, ctx);
}

static AccessorDecl *
createModifyCoroutinePrototype(AbstractStorageDecl *storage,
                               ASTContext &ctx) {
  return createCoroutineAccessorPrototype(storage, AccessorKind::Modify, ctx);
}

AccessorDecl *
SynthesizeAccessorRequest::evaluate(Evaluator &evaluator,
                                    AbstractStorageDecl *storage,
                                    AccessorKind kind) const {
  auto &ctx = storage->getASTContext();

  switch (kind) {
  case AccessorKind::Get:
    return createGetterPrototype(storage, ctx);

  case AccessorKind::Set:
    return createSetterPrototype(storage, ctx);

  case AccessorKind::Read:
    return createReadCoroutinePrototype(storage, ctx);

  case AccessorKind::Modify:
    return createModifyCoroutinePrototype(storage, ctx);

#define OPAQUE_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
  case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
    llvm_unreachable("not an opaque accessor");
  }
  llvm_unreachable("Unhandled AccessorKind in switch");
}

bool
RequiresOpaqueAccessorsRequest::evaluate(Evaluator &evaluator,
                                         VarDecl *var) const {
  // Nameless vars from interface files should not have any accessors.
  // TODO: Replace this check with a broader check that all storage decls
  //       from interface files have all their accessors up front.
  if (var->getBaseName().empty())
    return false;

  // Computed properties always require opaque accessors.
  if (!var->getImplInfo().isSimpleStored())
    return true;

  // The backing storage for a lazy property does require opaque accessors.
  if (var->isLazyStorageProperty())
    return false;

  auto *dc = var->getDeclContext();

  // Local stored variables don't require opaque accessors.
  if (dc->isLocalContext()) {
    return false;

  } else if (dc->isModuleScopeContext()) {
    // Fixed-layout global variables don't require opaque accessors.
    if (!var->isResilient() && !var->shouldUseNativeDynamicDispatch())
      return false;

  // Stored properties imported from Clang don't require opaque accessors.
  } else if (auto *structDecl = dyn_cast<StructDecl>(dc)) {
    if (structDecl->hasClangNode())
      return false;
  } else if (isa<ClassDecl>(dc) &&
             cast<ClassDecl>(dc)->isForeignReferenceType()) {
    return false;
  }

  // Stored properties in SIL mode don't get accessors.
  // But we might need to create opaque accessors for them.
  if (auto sourceFile = dc->getParentSourceFile()) {
    if (sourceFile->Kind == SourceFileKind::SIL) {
      if (!var->getParsedAccessor(AccessorKind::Get))
        return false;
    }
  }

  // Everything else requires opaque accessors.
  return true;
}

bool
RequiresOpaqueModifyCoroutineRequest::evaluate(Evaluator &evaluator,
                                               AbstractStorageDecl *storage) const {
  // Only for mutable storage.
  if (!storage->supportsMutation())
    return false;

  auto *dc = storage->getDeclContext();

  // Local properties don't have an opaque modify coroutine.
  if (dc->isLocalContext())
    return false;

  // Fixed-layout global properties don't have an opaque modify coroutine.
  if (dc->isModuleScopeContext() && !storage->isResilient())
    return false;

  // Imported storage declarations don't have an opaque modify coroutine.
  if (storage->hasClangNode())
    return false;

  // Dynamic storage does not have an opaque modify coroutine.
  if (dc->getSelfClassDecl())
    if (storage->shouldUseObjCDispatch())
      return false;

  // Requirements of ObjC protocols don't have an opaque modify coroutine.
  if (auto protoDecl = dyn_cast<ProtocolDecl>(dc))
    if (protoDecl->isObjC())
      return false;

  return true;
}

/// Mark the accessor as transparent if we can.
///
/// If the storage is inside a fixed-layout nominal type, we can mark the
/// accessor as transparent, since in this case we just want it for abstraction
/// purposes (i.e., to make access to the variable uniform and to be able to
/// put the getter in a vtable).
///
/// If the storage is for a global stored property or a stored property of a
/// resilient type, we are synthesizing accessors to present a resilient
/// interface to the storage and they should not be transparent.
bool
IsAccessorTransparentRequest::evaluate(Evaluator &evaluator,
                                       AccessorDecl *accessor) const {
  auto *storage = accessor->getStorage();
  if (storage->isTransparent())
    return true;

  if (accessor->getAttrs().hasAttribute<TransparentAttr>())
    return true;

  if (!accessor->isImplicit())
    return false;

  if (!doesAccessorHaveBody(accessor))
    return false;

  auto *DC = accessor->getDeclContext();
  auto *nominalDecl = DC->getSelfNominalTypeDecl();

  // Global variable accessors are not @_transparent.
  if (!nominalDecl)
    return false;

  // Accessors for resilient properties are not @_transparent.
  if (storage->isResilient())
    return false;

  // Accessors for classes with @objc ancestry are not @_transparent,
  // since they use a field offset variable which is not exported.
  if (auto *classDecl = dyn_cast<ClassDecl>(nominalDecl))
    if (classDecl->checkAncestry(AncestryFlags::ObjC))
      return false;

  // Accessors synthesized on-demand are never transparent.
  if (accessor->hasForcedStaticDispatch())
    return false;

  if (accessor->getAccessorKind() == AccessorKind::Get ||
      accessor->getAccessorKind() == AccessorKind::Set) {
    // Getters and setters for lazy properties are not @_transparent.
    if (storage->getAttrs().hasAttribute<LazyAttr>())
      return false;
  }

  // Accessors for a property with a wrapper are not @_transparent if
  // the backing variable has more-restrictive access than the original
  // property. The same goes for its storage wrapper.
  if (auto var = dyn_cast<VarDecl>(storage)) {
    if (auto backingVar = var->getPropertyWrapperBackingProperty()) {
      if (backingVar->getFormalAccess() < var->getFormalAccess())
        return false;
    }

    if (auto original = var->getOriginalWrappedProperty(
            PropertyWrapperSynthesizedPropertyKind::Projection)) {
      auto backingVar = original->getPropertyWrapperBackingProperty();
      if (backingVar->getFormalAccess() < var->getFormalAccess())
        return false;
    }
  }

  switch (accessor->getAccessorKind()) {
  case AccessorKind::Get:
    break;

  case AccessorKind::Set:

    switch (storage->getWriteImpl()) {
    case WriteImplKind::Set:
      // Setters for property wrapper are OK, unless there are observers.
      // FIXME: This should be folded into the WriteImplKind below.
      if (auto var = dyn_cast<VarDecl>(storage)) {
        if (var->hasAttachedPropertyWrapper()) {
          if (var->hasObservers())
            return false;

          break;
        } else if (var->getOriginalWrappedProperty(
                     PropertyWrapperSynthesizedPropertyKind::Projection)) {
          break;
        }
      }

      if (auto subscript = dyn_cast<SubscriptDecl>(storage)) {
        break;
      }

      // Anything else should not have a synthesized setter.
      LLVM_FALLTHROUGH;
    case WriteImplKind::Immutable:
      if (accessor->getASTContext().LangOpts.AllowModuleWithCompilerErrors)
        return false;
      llvm_unreachable("should not be synthesizing accessor in this case");

    case WriteImplKind::StoredWithObservers:
    case WriteImplKind::InheritedWithObservers:
      // Setters for observed properties are not @_transparent (because the
      // observers are private) and cannot be referenced from a transparent
      // method).
      return false;

    case WriteImplKind::Stored:
    case WriteImplKind::MutableAddress:
    case WriteImplKind::Modify:
      break;
    }
    break;

  case AccessorKind::Read:
  case AccessorKind::Modify:
  case AccessorKind::Init:
    break;

  case AccessorKind::WillSet:
  case AccessorKind::DidSet:
  case AccessorKind::Address:
  case AccessorKind::MutableAddress:
    llvm_unreachable("bad synthesized function kind");
  }

  switch (storage->getReadWriteImpl()) {
  case ReadWriteImplKind::StoredWithDidSet:
  case ReadWriteImplKind::InheritedWithDidSet:
    if (storage->getAccessor(AccessorKind::DidSet)->isSimpleDidSet())
      return false;
    break;
  default:
    break;
  }

  return true;
}

VarDecl *
LazyStoragePropertyRequest::evaluate(Evaluator &evaluator,
                                     VarDecl *VD) const {
  assert(isa<SourceFile>(VD->getDeclContext()->getModuleScopeContext()));
  assert(VD->getAttrs().hasAttribute<LazyAttr>());
  auto &Context = VD->getASTContext();

  // Create the storage property as an optional of VD's type.
  SmallString<64> NameBuf;
  NameBuf += "$__lazy_storage_$_";
  NameBuf += VD->getName().str();
  auto StorageName = Context.getIdentifier(NameBuf);
  auto StorageInterfaceTy = OptionalType::get(VD->getInterfaceType());
  auto StorageTy = OptionalType::get(VD->getType());

  auto *Storage = new (Context) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Var,
                                        VD->getLoc(), StorageName,
                                        VD->getDeclContext());
  Storage->setInterfaceType(StorageInterfaceTy);
  Storage->setLazyStorageProperty(true);
  Storage->setUserAccessible(false);

  // The storage is implicit and private.
  Storage->setImplicit();
  Storage->overwriteAccess(AccessLevel::Private);
  Storage->overwriteSetterAccess(AccessLevel::Private);

  addMemberToContextIfNeeded(Storage, VD->getDeclContext(), VD);

  // Create the pattern binding decl for the storage decl.  This will get
  // default initialized to nil.
  Pattern *PBDPattern =
      NamedPattern::createImplicit(Context, Storage, StorageTy);
  PBDPattern = TypedPattern::createImplicit(Context, PBDPattern, StorageTy);
  auto *InitExpr = new (Context) NilLiteralExpr(SourceLoc(), /*Implicit=*/true);
  InitExpr->setType(Storage->getType());

  auto *PBD = PatternBindingDecl::createImplicit(
      Context, StaticSpellingKind::None, PBDPattern, InitExpr,
      VD->getDeclContext(), /*VarLoc*/ VD->getLoc());
  PBD->setInitializerChecked(0);

  addMemberToContextIfNeeded(PBD, VD->getDeclContext(), Storage);

  return Storage;
}

/// Synthesize a computed property representing the wrapped value for a
/// parameter with an attached property wrapper.
static VarDecl *synthesizeLocalWrappedValueVar(VarDecl *var) {
  if (!var->hasAttachedPropertyWrapper() || !isa<ParamDecl>(var))
    return nullptr;

  auto dc = var->getDeclContext();
  auto &ctx = var->getASTContext();

  SmallString<64> nameBuf;
  if (var->getName().hasDollarPrefix()) {
    nameBuf = var->getName().str().drop_front();
  } else {
    nameBuf = var->getName().str();
  }
  Identifier name = ctx.getIdentifier(nameBuf);

  VarDecl *localVar = new (ctx) VarDecl(/*IsStatic=*/false,
                                        VarDecl::Introducer::Var,
                                        var->getLoc(), name, dc);
  localVar->setImplicit();
  localVar->getAttrs() = var->getAttrs();
  localVar->overwriteAccess(var->getFormalAccess());

  if (var->hasImplicitPropertyWrapper()) {
    // FIXME: This can have a setter, but we need a resolved wrapper type
    // to figure it out.
    localVar->setImplInfo(StorageImplInfo::getImmutableComputed());
  } else {
    auto mutability = *var->getPropertyWrapperMutability();
    if (mutability.Getter == PropertyWrapperMutability::Mutating) {
      ctx.Diags.diagnose(var->getLoc(), diag::property_wrapper_param_mutating);
      return nullptr;
    }

    if (mutability.Setter == PropertyWrapperMutability::Nonmutating) {
      localVar->setImplInfo(StorageImplInfo::getMutableComputed());
    } else {
      localVar->setImplInfo(StorageImplInfo::getImmutableComputed());
    }
  }

  return localVar;
}

/// Synthesize a computed property `$foo` for a property with an attached
/// wrapper that has a `projectedValue` property.
static VarDecl *synthesizePropertyWrapperProjectionVar(
    ASTContext &ctx, VarDecl *var, VarDecl *wrapperVar) {
  // If the original property has a @_projectedValueProperty attribute, use
  // that to find the storage wrapper property.
  if (auto attr = var->getAttrs().getAttribute<ProjectedValuePropertyAttr>()){
    SmallVector<ValueDecl *, 2> declsFound;
    DeclNameRef projectionName(attr->ProjectionPropertyName);
    auto dc = var->getDeclContext();
    if (dc->isTypeContext()) {
      dc->lookupQualified(dc->getSelfNominalTypeDecl(), projectionName,
                          var->getLoc(), NL_QualifiedDefault, declsFound);
    } else if (dc->isModuleScopeContext()) {
      dc->lookupQualified(dc->getParentModule(), projectionName,
                          var->getLoc(), NL_QualifiedDefault, declsFound);
    } else {
      llvm_unreachable("Property wrappers don't work in local contexts");
    }

    if (declsFound.size() == 1 && isa<VarDecl>(declsFound.front())) {
      auto property = cast<VarDecl>(declsFound.front());
      property->setOriginalWrappedProperty(var);
      return property;
    }

    ctx.Diags.diagnose(attr->getLocation(),
                       diag::property_wrapper_projection_value_missing,
                       projectionName);
    attr->setInvalid();
  }

  // Compute the name of the storage type.
  SmallString<64> nameBuf;
  if (var->getName().hasDollarPrefix()) {
    nameBuf = var->getName().str();
  } else {
    nameBuf = "$";
    nameBuf += var->getName().str();
  }
  Identifier name = ctx.getIdentifier(nameBuf);

  // Form the property.
  auto dc = var->getDeclContext();
  VarDecl *property = new (ctx) VarDecl(/*IsStatic=*/var->isStatic(),
                                        VarDecl::Introducer::Var,
                                        var->getLoc(),
                                        name, dc);
  property->setImplicit();
  property->setOriginalWrappedProperty(var);
  addMemberToContextIfNeeded(property, dc, var);

  // Determine the access level for the property.
  property->overwriteAccess(var->getFormalAccess());

  // Determine setter access.
  property->overwriteSetterAccess(var->getSetterFormalAccess());

  // Add the accessors we need.
  if (var->hasImplicitPropertyWrapper()) {
    // FIXME: This can have a setter, but we need a resolved type first
    // to figure it out.
    property->setImplInfo(StorageImplInfo::getImmutableComputed());
  } else {
    bool hasSetter = wrapperVar->isSettable(nullptr) &&
    wrapperVar->isSetterAccessibleFrom(var->getInnermostDeclContext());
    if (hasSetter)
      property->setImplInfo(StorageImplInfo::getMutableComputed());
    else
      property->setImplInfo(StorageImplInfo::getImmutableComputed());
  }

  if (!isa<ParamDecl>(var))
    var->getAttrs().add(
        new (ctx) ProjectedValuePropertyAttr(name, SourceLoc(), SourceRange(),
                                             /*Implicit=*/true));
  return property;
}

static void typeCheckSynthesizedWrapperInitializer(VarDecl *wrappedVar,
                                                   Expr *&initializer,
                                                   bool contextualize) {
  auto *dc = wrappedVar->getInnermostDeclContext();
  auto &ctx = wrappedVar->getASTContext();
  auto *initContext = new (ctx) PropertyWrapperInitializer(
      dc, wrappedVar, PropertyWrapperInitializer::Kind::WrappedValue);

  // Type-check the initialization.
  using namespace constraints;
  auto target = SyntacticElementTarget::forPropertyWrapperInitializer(
      wrappedVar, initContext, initializer);
  auto result = TypeChecker::typeCheckExpression(target);
  if (!result)
    return;

  initializer = result->getAsExpr();

  // Contextualize the initializer which is a local variable with defaultInit or
  // gets an independent initializer. The rest of initializer contextualizing
  // will be done in visitPatternBindingDecl.
  if (!contextualize)
    return;
  TypeChecker::contextualizeInitializer(initContext, initializer);
  checkPropertyWrapperActorIsolation(wrappedVar, initializer);
  TypeChecker::checkInitializerEffects(initContext, initializer);
}

static PropertyWrapperMutability::Value
getGetterMutatingness(VarDecl *var) {
  return var->isGetterMutating()
    ? PropertyWrapperMutability::Mutating
    : PropertyWrapperMutability::Nonmutating;
}

static PropertyWrapperMutability::Value
getSetterMutatingness(VarDecl *var, DeclContext *dc) {
  if (!var->isSettable(nullptr) ||
      !var->isSetterAccessibleFrom(dc))
    return PropertyWrapperMutability::DoesntExist;
  
  return var->isSetterMutating()
    ? PropertyWrapperMutability::Mutating
    : PropertyWrapperMutability::Nonmutating;
}

llvm::Optional<PropertyWrapperMutability>
PropertyWrapperMutabilityRequest::evaluate(Evaluator &, VarDecl *var) const {
  VarDecl *originalVar = var;
  unsigned numWrappers = originalVar->getAttachedPropertyWrappers().size();
  bool isProjectedValue = false;
  if (numWrappers < 1) {
    originalVar = var->getOriginalWrappedProperty(
        PropertyWrapperSynthesizedPropertyKind::Projection);
    if (!originalVar)
      return llvm::None;

    numWrappers = originalVar->getAttachedPropertyWrappers().size();
    isProjectedValue = true;
  }

  // Make sure we don't ignore .swiftinterface files, because those will
  // have the accessors printed
  auto varSourceFile = var->getDeclContext()->getParentSourceFile();
  auto isVarNotInInterfaceFile =
      varSourceFile && varSourceFile->Kind != SourceFileKind::Interface;

  if (var->getParsedAccessor(AccessorKind::Get) && isVarNotInInterfaceFile)
    return llvm::None;
  if (var->getParsedAccessor(AccessorKind::Set) && isVarNotInInterfaceFile)
    return llvm::None;

  // Figure out which member we're looking through.
  auto varMember = isProjectedValue
    ? &PropertyWrapperTypeInfo::projectedValueVar
    : &PropertyWrapperTypeInfo::valueVar;

  // Start with the traits from the outermost wrapper.
  auto firstWrapper = originalVar->getAttachedPropertyWrapperTypeInfo(0);
  if (firstWrapper.*varMember == nullptr)
    return llvm::None;

  PropertyWrapperMutability result;
  
  result.Getter = getGetterMutatingness(firstWrapper.*varMember);
  result.Setter = getSetterMutatingness(firstWrapper.*varMember,
                                        var->getInnermostDeclContext());

  auto getCustomAttrTypeLoc = [](const CustomAttr *CA) -> TypeLoc {
    return { CA->getTypeRepr(), CA->getType() };
  };

  // Compose the traits of the following wrappers.
  for (unsigned i = 1; i < numWrappers && !isProjectedValue; ++i) {
    assert(var == originalVar);
    auto wrapper = var->getAttachedPropertyWrapperTypeInfo(i);
    if (!wrapper.valueVar)
      return llvm::None;

    PropertyWrapperMutability nextResult;
    nextResult.Getter =
                    result.composeWith(getGetterMutatingness(wrapper.valueVar));
    // A property must have a getter, so we can't compose a wrapper that
    // exposes a mutating getter wrapped inside a get-only wrapper.
    if (nextResult.Getter == PropertyWrapperMutability::DoesntExist) {
      auto &ctx = var->getASTContext();
      ctx.Diags.diagnose(var->getAttachedPropertyWrappers()[i]->getLocation(),
               diag::property_wrapper_mutating_get_composed_to_get_only,
               getCustomAttrTypeLoc(var->getAttachedPropertyWrappers()[i]),
               getCustomAttrTypeLoc(var->getAttachedPropertyWrappers()[i-1]));

      return llvm::None;
    }
    nextResult.Setter =
              result.composeWith(getSetterMutatingness(wrapper.valueVar,
                                               var->getInnermostDeclContext()));
    result = nextResult;
  }
  assert(result.Getter != PropertyWrapperMutability::DoesntExist
         && "getter must exist");
  return result;
}

llvm::Optional<PropertyWrapperLValueness>
PropertyWrapperLValuenessRequest::evaluate(Evaluator &, VarDecl *var) const {
  VarDecl *VD = var;
  unsigned numWrappers = var->getAttachedPropertyWrappers().size();
  bool isProjectedValue = false;
  if (numWrappers < 1) {
    VD = var->getOriginalWrappedProperty(
        PropertyWrapperSynthesizedPropertyKind::Projection);
    numWrappers = 1; // Can't compose projected values
    isProjectedValue = true;
  }

  if (!VD)
    return llvm::None;

  auto varMember = isProjectedValue
      ? &PropertyWrapperTypeInfo::projectedValueVar
      : &PropertyWrapperTypeInfo::valueVar;

  auto accessorMutability = [&](unsigned wrapperIndex) -> PropertyWrapperMutability {
    PropertyWrapperMutability mutability;
    auto wrapperInfo = VD->getAttachedPropertyWrapperTypeInfo(wrapperIndex);
    mutability.Getter = getGetterMutatingness(wrapperInfo.*varMember);
    mutability.Setter = getSetterMutatingness(wrapperInfo.*varMember,
                                              var->getInnermostDeclContext());
    return mutability;
  };

  // Calling the getter (or setter) on the nth property wrapper in the chain
  // is done as follows:
  //  1. call the getter on the (n-1)th property wrapper instance to get the
  //     nth property wrapper instance
  //  2. call the getter (or setter) on the nth property wrapper instance
  //  3. if (2) is a mutating access, call the setter on the (n-1)th property
  //     wrapper instance to write back the mutated value

  // Below, we determine which of these property wrapper instances need to be
  // accessed mutating-ly, and therefore should be l-values.

  unsigned innermostWrapperIdx = numWrappers - 1;
  auto lastAccess = accessorMutability(innermostWrapperIdx);

  PropertyWrapperLValueness lvalueness(numWrappers);
  lvalueness.isLValueForGetAccess[innermostWrapperIdx] =
      lastAccess.Getter == PropertyWrapperMutability::Mutating;
  lvalueness.isLValueForSetAccess[innermostWrapperIdx] =
      lastAccess.Setter == PropertyWrapperMutability::Mutating;

  auto lastAccessForGet = lastAccess.Getter;
  auto lastAccessForSet = lastAccess.Setter;
  for (int i = innermostWrapperIdx - 1; i >= 0; --i) {
    auto access = accessorMutability(i);

    lastAccessForGet = access.composeWith(lastAccessForGet);
    lastAccessForSet = access.composeWith(lastAccessForSet);

    lvalueness.isLValueForGetAccess[i] =
        lastAccessForGet == PropertyWrapperMutability::Mutating;
    lvalueness.isLValueForSetAccess[i] =
        lastAccessForSet == PropertyWrapperMutability::Mutating;
  }

  return lvalueness;
}

PropertyWrapperAuxiliaryVariables
PropertyWrapperAuxiliaryVariablesRequest::evaluate(Evaluator &evaluator,
                                                   VarDecl *var) const {
  if (!var->hasAttachedPropertyWrapper())
    return PropertyWrapperAuxiliaryVariables();

  auto wrapperInfo = var->getAttachedPropertyWrapperTypeInfo(0);

  // Compute the name of the storage type.
  ASTContext &ctx = var->getASTContext();
  SmallString<64> nameBuf;
  nameBuf = "_";
  if (var->getName().hasDollarPrefix())
    nameBuf += var->getName().str().drop_front();
  else
    nameBuf += var->getName().str();
  Identifier name = ctx.getIdentifier(nameBuf);

  auto dc = var->getDeclContext();
  VarDecl *backingVar = nullptr;
  VarDecl *projectionVar = nullptr;
  VarDecl *wrappedValueVar = nullptr;

  // Create the backing storage property.
  if (var->hasExternalPropertyWrapper()) {
    auto *param = cast<ParamDecl>(var);
    backingVar = ParamDecl::cloneWithoutType(ctx, param);
    backingVar->setName(name);
  } else {
    auto introducer = isa<ParamDecl>(var) ? VarDecl::Introducer::Let : VarDecl::Introducer::Var;
    backingVar = new (ctx) VarDecl(/*IsStatic=*/var->isStatic(),
                                   introducer,
                                   var->getLoc(),
                                   name, dc);
    backingVar->setImplicit();
    backingVar->setOriginalWrappedProperty(var);

    // The backing storage is 'private'.
    backingVar->overwriteAccess(AccessLevel::Private);
    backingVar->overwriteSetterAccess(AccessLevel::Private);

    addMemberToContextIfNeeded(backingVar, dc, var);
  }

  if (wrapperInfo.projectedValueVar || var->getName().hasDollarPrefix()) {
    projectionVar = synthesizePropertyWrapperProjectionVar(
        ctx, var, wrapperInfo.projectedValueVar);
  }

  if ((wrappedValueVar = synthesizeLocalWrappedValueVar(var))) {
    // Record the backing storage for the local wrapped value var, which
    // is needed for synthesizing its accessors.
    evaluator.cacheOutput(PropertyWrapperAuxiliaryVariablesRequest{wrappedValueVar},
                          PropertyWrapperAuxiliaryVariables(backingVar, projectionVar));
  }

  return PropertyWrapperAuxiliaryVariables(backingVar, projectionVar, wrappedValueVar);
}

PropertyWrapperInitializerInfo
PropertyWrapperInitializerInfoRequest::evaluate(Evaluator &evaluator,
                                                VarDecl *var) const {
  if (!var->hasAttachedPropertyWrapper() ||
      (var->isImplicit() && !isa<ParamDecl>(var)))
    return PropertyWrapperInitializerInfo();

  auto wrapperInfo = var->getAttachedPropertyWrapperTypeInfo(0);
  if (!wrapperInfo)
    return PropertyWrapperInitializerInfo();

  ASTContext &ctx = var->getASTContext();
  auto dc = var->getDeclContext();

  // Determine the type of the storage.
  auto wrapperType = var->getPropertyWrapperBackingPropertyType();
  if (!wrapperType || wrapperType->hasError())
    return PropertyWrapperInitializerInfo();

  Type storageType = dc->mapTypeIntoContext(wrapperType);
  Expr *initializer = nullptr;
  PropertyWrapperValuePlaceholderExpr *wrappedValue = nullptr;

  auto createPBD = [&](VarDecl *singleVar) -> PatternBindingDecl * {
    Pattern *pattern =
        NamedPattern::createImplicit(ctx, singleVar, singleVar->getType());
    pattern = TypedPattern::createImplicit(ctx, pattern, singleVar->getType());
    PatternBindingDecl *pbd = PatternBindingDecl::createImplicit(
        ctx, var->getCorrectStaticSpelling(), pattern, /*init*/nullptr,
        dc, SourceLoc());
    addMemberToContextIfNeeded(pbd, dc, var);
    pbd->setStatic(var->isStatic());
    return pbd;
  };

  // Take the initializer from the original property.
  if (!isa<ParamDecl>(var)) {
    auto parentPBD = var->getParentPatternBinding();
    unsigned patternNumber = parentPBD->getPatternEntryIndexForVarDecl(var);
    auto *backingVar = var->getPropertyWrapperBackingProperty();
    auto *pbd = createPBD(backingVar);

    // Force the default initializer to come into existence, if there is one,
    // and the wrapper doesn't provide its own.
    if (!parentPBD->isInitialized(patternNumber)
        && parentPBD->isDefaultInitializable(patternNumber)
        && !wrapperInfo.defaultInit) {
      auto ty = parentPBD->getPattern(patternNumber)->getType();
      if (auto defaultInit = TypeChecker::buildDefaultInitializer(ty)) {
        typeCheckSynthesizedWrapperInitializer(var, defaultInit,
                                               /*contextualize=*/false);
        parentPBD->setInit(0, defaultInit);
        parentPBD->setInitializerChecked(0);
      }
    }

    if ((initializer = parentPBD->getInit(patternNumber))) {
      assert(parentPBD->isInitializerChecked(0) &&
             "Initializer should to be type-checked");

      pbd->setInit(0, initializer);
      pbd->setInitializerChecked(0);
      wrappedValue = findWrappedValuePlaceholder(initializer);
    } else {
      if (!parentPBD->isInitialized(patternNumber) && wrapperInfo.defaultInit) {
        // FIXME: Record this expression somewhere so that DI can perform the
        // initialization itself.
        Expr *defaultInit = nullptr;
        // Only contextualize local wrapped property, the rest of wrapped
        // property will be contextualized in visitPatternBindingDecl.
        typeCheckSynthesizedWrapperInitializer(var, defaultInit, dc->isLocalContext());
        pbd->setInit(0, defaultInit);
        pbd->setInitializerChecked(0);

        // If a static, global, or local wrapped property has a default
        // initializer, this is the only initializer that will be used.
        if (var->isStatic() || !dc->isTypeContext()) {
          initializer = defaultInit;
        }
      } else if (var->hasObservers() && !dc->isTypeContext()) {
        var->diagnose(diag::observingprop_requires_initializer);
      }

      if (var->getOpaqueResultTypeDecl()) {
        var->diagnose(diag::opaque_type_var_no_underlying_type);
      }
    }
  } else if (!var->hasExternalPropertyWrapper()) {
    auto *param = cast<ParamDecl>(var);
    auto *backingVar = var->getPropertyWrapperBackingProperty();
    auto *pbd = createPBD(backingVar);

    auto *paramRef = new (ctx) DeclRefExpr(var, DeclNameLoc(), /*implicit=*/true);
    initializer = buildPropertyWrapperInitCall(
        var, storageType, paramRef, PropertyWrapperInitKind::WrappedValue);
    TypeChecker::typeCheckExpression(initializer, dc);

    // Check initializer effects.
    auto *initContext = new (ctx) PropertyWrapperInitializer(
        dc, param, PropertyWrapperInitializer::Kind::ProjectedValue);

    TypeChecker::checkInitializerEffects(initContext, initializer);

    pbd->setInit(0, initializer);
    pbd->setInitializerChecked(0);
  }

  // If there is a projection property (projectedValue) in the wrapper,
  // synthesize a computed property for '$foo'.
  Expr *projectedValueInit = nullptr;
  if (auto *projection = var->getPropertyWrapperProjectionVar()) {
    createPBD(projection);

    if (var->hasExternalPropertyWrapper()) {
      // Projected-value initialization is currently only supported for parameters.
      auto *param = dyn_cast<ParamDecl>(var);
      auto *placeholder = PropertyWrapperValuePlaceholderExpr::create(
          ctx, var->getSourceRange(), projection->getType(), /*projectedValue=*/nullptr);
      projectedValueInit = buildPropertyWrapperInitCall(
          var, storageType, placeholder, PropertyWrapperInitKind::ProjectedValue);
      TypeChecker::typeCheckExpression(projectedValueInit, dc);

      // Check initializer effects.
      auto *initContext = new (ctx) PropertyWrapperInitializer(
          dc, param, PropertyWrapperInitializer::Kind::ProjectedValue);
      checkInitializerActorIsolation(initContext, projectedValueInit);
      TypeChecker::checkInitializerEffects(initContext, projectedValueInit);
    }
  }

  // Form the initialization of the backing property from a value of the
  // original property's type.
  Expr *wrappedValueInit = nullptr;
  if (wrappedValue) {
    wrappedValueInit = initializer;
  } else if (!initializer &&
             var->allAttachedPropertyWrappersHaveWrappedValueInit() &&
             !var->getName().hasDollarPrefix()) {
    wrappedValueInit = PropertyWrapperValuePlaceholderExpr::create(
        ctx, var->getSourceRange(), var->getType(), /*wrappedValue=*/nullptr);
    typeCheckSynthesizedWrapperInitializer(var, wrappedValueInit,
                                           /*contextualize=*/true);
  }

  return PropertyWrapperInitializerInfo(wrappedValueInit, projectedValueInit);
}

/// Given a storage declaration in a protocol, set it up with the right
/// StorageImpl and add the right set of opaque accessors.
static void finishProtocolStorageImplInfo(AbstractStorageDecl *storage,
                                          StorageImplInfo &info) {
  if (auto *var = dyn_cast<VarDecl>(storage)) {
    SourceLoc typeLoc;
    if (auto *repr = var->getTypeReprOrParentPatternTypeRepr())
      typeLoc = repr->getEndLoc();
    
    if (info.hasStorage()) {
      // Protocols cannot have stored properties.
      if (var->isLet()) {
        var->diagnose(diag::protocol_property_must_be_computed_var)
            .fixItReplace(var->getParentPatternBinding()->getLoc(), "var")
            .fixItInsertAfter(typeLoc, " { get }");
      } else {
        auto diag = var->diagnose(diag::protocol_property_must_be_computed);
        auto braces = var->getBracesRange();

        if (braces.isValid())
          diag.fixItReplace(braces, "{ get <#set#> }");
        else
          diag.fixItInsertAfter(typeLoc, " { get <#set#> }");
      }
    }
  }

  auto protocol = cast<ProtocolDecl>(storage->getDeclContext());
  if (protocol->isObjC()) {
    info = StorageImplInfo::getComputed(info.supportsMutation());
  } else {
    info = StorageImplInfo::getOpaque(info.supportsMutation(),
                                      storage->getOpaqueReadOwnership());
  }
}

static void finishLazyVariableImplInfo(VarDecl *var,
                                       StorageImplInfo &info) {
  auto *attr = var->getAttrs().getAttribute<LazyAttr>();

  // It cannot currently be used on let's since we don't have a mutability model
  // that supports it.
  if (var->isLet())
    diagnoseAttrWithRemovalFixIt(var, attr, diag::lazy_not_on_let);

  // lazy must have an initializer.
  if (!var->getParentInitializer())
    diagnoseAttrWithRemovalFixIt(var, attr, diag::lazy_requires_initializer);

  bool invalid = false;

  if (isa<ProtocolDecl>(var->getDeclContext())) {
    diagnoseAttrWithRemovalFixIt(var, attr, diag::lazy_not_in_protocol);
    invalid = true;
  }

  // Lazy properties must be written as stored properties in the source.
  if (info.getReadImpl() != ReadImplKind::Stored &&
      (info.getWriteImpl() != WriteImplKind::Stored &&
       info.getWriteImpl() != WriteImplKind::StoredWithObservers)) {
    diagnoseAttrWithRemovalFixIt(var, attr, diag::lazy_not_on_computed);
    invalid = true;
  }

  // The pattern binding must only bind a single variable.
  if (!var->getParentPatternBinding()->getSingleVar())
    diagnoseAttrWithRemovalFixIt(var, attr, diag::lazy_requires_single_var);

  if (!invalid)
    info = StorageImplInfo::getMutableComputed();
}

static void finishPropertyWrapperImplInfo(VarDecl *var,
                                          StorageImplInfo &info) {
  auto parentSF = var->getDeclContext()->getParentSourceFile();
  if (!parentSF)
    return;

  // Properties with wrappers must not declare a getter or setter.
  if (!info.hasStorage() && parentSF->Kind != SourceFileKind::Interface) {
    auto &ctx = parentSF->getASTContext();
    for (auto attr : var->getAttrs().getAttributes<CustomAttr>())
      ctx.Diags.diagnose(attr->getLocation(), diag::property_wrapper_computed);

    return;
  }

  bool wrapperSetterIsUsable = false;
  if (var->getParsedAccessor(AccessorKind::Set)) {
    wrapperSetterIsUsable = true;
  } else if (parentSF && parentSF->Kind != SourceFileKind::Interface
             && !var->isLet()) {
    if (auto comp = var->getPropertyWrapperMutability()) {
      wrapperSetterIsUsable =
        comp->Setter != PropertyWrapperMutability::DoesntExist;
    } else {
      wrapperSetterIsUsable = true;
    }
  }

  if (!wrapperSetterIsUsable) {
    info = StorageImplInfo::getImmutableComputed();
    return;
  }

  if (var->hasObservers() || var->getDeclContext()->isLocalContext()) {
    info = StorageImplInfo::getMutableComputed();
  } else {
    info = StorageImplInfo(ReadImplKind::Get, WriteImplKind::Set,
                           ReadWriteImplKind::Modify);
  }
}

static void finishNSManagedImplInfo(VarDecl *var,
                                    StorageImplInfo &info) {
  auto *attr = var->getAttrs().getAttribute<NSManagedAttr>();

  if (var->isLet())
    diagnoseAttrWithRemovalFixIt(var, attr, diag::attr_NSManaged_let_property);

  SourceFile *parentFile = var->getDeclContext()->getParentSourceFile();

  auto diagnoseNotStored = [&](unsigned kind) {
    // Skip diagnosing @NSManaged declarations in module interfaces. They are
    // properties that are stored, but have specially synthesized observers
    // and we should allow them to have getters and setters in a module
    // interface.
    if (parentFile && parentFile->Kind == SourceFileKind::Interface)
      return;

    diagnoseAttrWithRemovalFixIt(var, attr, diag::attr_NSManaged_not_stored, kind);
  };

  // @NSManaged properties must be written as stored.
  if (info.isSimpleStored()) {
    // @NSManaged properties end up being computed; complain if there is
    // an initializer.
    if (var->getParentExecutableInitializer()) {
      auto &Diags = var->getASTContext().Diags;
      Diags.diagnose(attr->getLocation(), diag::attr_NSManaged_initial_value)
           .highlight(var->getParentExecutableInitializer()->getSourceRange());
    }

    // Otherwise, ok.
    info = StorageImplInfo::getMutableComputed();

  } else if (info.getReadImpl() == ReadImplKind::Address ||
             info.getWriteImpl() == WriteImplKind::MutableAddress) {
    diagnoseNotStored(/*addressed*/ 2);
  } else if (info.getWriteImpl() == WriteImplKind::StoredWithObservers ||
             info.getWriteImpl() == WriteImplKind::InheritedWithObservers) {
    diagnoseNotStored(/*observing*/ 1);
  } else {
    diagnoseNotStored(/*computed*/ 0);
  }
}

static void finishStorageImplInfo(AbstractStorageDecl *storage,
                                  StorageImplInfo &info) {
  auto dc = storage->getDeclContext();

  if (auto var = dyn_cast<VarDecl>(storage)) {
    if (!info.hasStorage() && !var->hasInitAccessor()) {
      if (auto *init = var->getParentExecutableInitializer()) {
        auto &Diags = var->getASTContext().Diags;
        Diags.diagnose(init->getLoc(), diag::getset_init)
             .highlight(init->getSourceRange());
      }
    }

    if (var->getAttrs().hasAttribute<LazyAttr>()) {
      finishLazyVariableImplInfo(var, info);
    } else if (var->getAttrs().hasAttribute<NSManagedAttr>()) {
      finishNSManagedImplInfo(var, info);
    } else if (var->hasAttachedPropertyWrapper()) {
      finishPropertyWrapperImplInfo(var, info);
    }
  }

  if (isa<ProtocolDecl>(dc))
    finishProtocolStorageImplInfo(storage, info);

  // If we have a stored property in an unsupported context, diagnose
  // and change it to computed to avoid confusing SILGen.

  // Note: Stored properties in protocols are diagnosed in
  // finishProtocolStorageImplInfo().

  if (info.hasStorage() && !storage->isStatic()) {
    if (isa<EnumDecl>(dc)) {
      storage->diagnose(diag::enum_stored_property);
      info = StorageImplInfo::getMutableComputed();
    } else if (auto ext = dyn_cast<ExtensionDecl>(dc)) {
      // Extensions can dynamically replace a stored property.
      if (storage->getAttrs().getAttribute<DynamicReplacementAttr>())
        return;

      // @_objcImplementation extensions on a non-category can declare stored
      // properties; StoredPropertiesRequest knows to look for them there.
      if (ext->isObjCImplementation() &&
          ext->getCategoryNameForObjCImplementation() == Identifier())
        return;

      storage->diagnose(diag::extension_stored_property);

      info = (info.supportsMutation()
              ? StorageImplInfo::getMutableComputed()
              : StorageImplInfo::getImmutableComputed());
    }
  }
}

/// Gets the storage info of the provided storage decl if it has the
/// @_hasStorage attribute and it's not in SIL mode.
///
/// In this case, we say the decl is:
///
/// Read:
///   - Stored, always
/// Write:
///   - Stored, if the decl is a 'var'.
///   - StoredWithObservers, if the decl has a setter
///     - This indicates that the original decl had a 'didSet' and/or 'willSet'
///   - InheritedWithObservers, if the decl has a setter and is an override.
///   - Immutable, if the decl is a 'let' or it does not have a setter.
/// ReadWrite:
///   - Stored, if the decl has no accessors listed.
///   - Immutable, if the decl is a 'let' or it does not have a setter.
///   - MaterializeToTemporary, if the decl has a setter.
static StorageImplInfo classifyWithHasStorageAttr(VarDecl *var) {
  WriteImplKind writeImpl;
  ReadWriteImplKind readWriteImpl;

  if (var->getParsedAccessor(AccessorKind::Get) &&
      var->getParsedAccessor(AccessorKind::Set)) {
    // If we see `@_hasStorage var x: T { get set }`, then our property has
    // willSet/didSet observers.
    writeImpl = var->getAttrs().hasAttribute<OverrideAttr>() ?
      WriteImplKind::InheritedWithObservers :
      WriteImplKind::StoredWithObservers;
    readWriteImpl = ReadWriteImplKind::MaterializeToTemporary;
  } else if (var->isLet()) {
    writeImpl = WriteImplKind::Immutable;
    readWriteImpl = ReadWriteImplKind::Immutable;
  } else {
    // Default to stored writes.
    writeImpl = WriteImplKind::Stored;
    readWriteImpl = ReadWriteImplKind::Stored;
  }

  // Always force Stored reads if @_hasStorage is present.
  return StorageImplInfo(ReadImplKind::Stored, writeImpl, readWriteImpl);
}

bool HasStorageRequest::evaluate(Evaluator &evaluator,
                                 AbstractStorageDecl *storage) const {
  // Parameters are always stored.
  if (isa<ParamDecl>(storage))
    return true;

  // Only variables can be stored.
  auto *var = dyn_cast<VarDecl>(storage);
  if (!var)
    return false;

  // @_hasStorage implies that it... has storage.
  if (var->getAttrs().hasAttribute<HasStorageAttr>())
    return true;

  // Protocol requirements never have storage.
  if (isa<ProtocolDecl>(storage->getDeclContext()))
    return false;

  // lazy declarations do not have storage.
  if (storage->getAttrs().hasAttribute<LazyAttr>())
    return false;

  // @NSManaged attributes don't have storage
  if (storage->getAttrs().hasAttribute<NSManagedAttr>())
    return false;

  // Any accessors that read or write imply that there is no storage.
  if (storage->getParsedAccessor(AccessorKind::Get) ||
      storage->getParsedAccessor(AccessorKind::Read) ||
      storage->getParsedAccessor(AccessorKind::Address) ||
      storage->getParsedAccessor(AccessorKind::Set) ||
      storage->getParsedAccessor(AccessorKind::Modify) ||
      storage->getParsedAccessor(AccessorKind::MutableAddress))
    return false;

  // willSet or didSet in an overriding property imply that there is no storage.
  if ((storage->getParsedAccessor(AccessorKind::WillSet) ||
       storage->getParsedAccessor(AccessorKind::DidSet)) &&
      storage->getAttrs().hasAttribute<OverrideAttr>())
    return false;

  // The presence of a property wrapper implies that there is no storage.
  if (var->hasAttachedPropertyWrapper())
    return false;

  // Look for any accessor macros that might make this property computed.
  bool hasStorage = true;
  namelookup::forEachPotentialAttachedMacro(
      var, MacroRole::Accessor,
      [&](MacroDecl *macro, const MacroRoleAttr *attr) {
        // Will this macro introduce observers?
        bool foundObserver = accessorMacroOnlyIntroducesObservers(macro, attr);

        // If it's not (just) introducing observers, it's making the property
        // computed.
        if (!foundObserver)
          hasStorage = false;

        // If it will introduce observers, and there is an "override",
        // the property doesn't have storage.
        if (foundObserver && storage->getAttrs().hasAttribute<OverrideAttr>())
          hasStorage = false;
      });
  return hasStorage;
}

StorageImplInfo
StorageImplInfoRequest::evaluate(Evaluator &evaluator,
                                 AbstractStorageDecl *storage) const {
  if (auto *param = dyn_cast<ParamDecl>(storage)) {
    return StorageImplInfo::getSimpleStored(
      param->isImmutableInFunctionBody()
        ? StorageIsNotMutable
        : StorageIsMutable);
  }

  if (auto *var = dyn_cast<VarDecl>(storage)) {
    // Allow the @_hasStorage attribute to override all the accessors we parsed
    // when making the final classification.
    if (var->getAttrs().hasAttribute<HasStorageAttr>()) {
      // The SIL rules for @_hasStorage are slightly different from the non-SIL
      // rules. In SIL mode, @_hasStorage marks that the type is simply stored,
      // and the only thing that determines mutability is the existence of the
      // setter.
      //
      // FIXME: SIL should not be special cased here. The behavior should be
      //        consistent between SIL and non-SIL.
      //        The strategy here should be to keep track of all opaque accessors
      //        along with enough information to access the storage trivially
      //        if allowed. This could be a representational change to
      //        StorageImplInfo such that it keeps a bitset of listed accessors
      //        and dynamically determines the access strategy from that.
      auto *SF = storage->getDeclContext()->getParentSourceFile();
      if (SF && SF->Kind == SourceFileKind::SIL)
        return StorageImplInfo::getSimpleStored(
          var->getParsedAccessor(AccessorKind::Set)
          ? StorageIsMutable
          : StorageIsNotMutable);

      return classifyWithHasStorageAttr(var);
    }
  }

  // Handle protocol requirements specially.
  if (isa<ProtocolDecl>(storage->getDeclContext())) {
    ReadImplKind readImpl = ReadImplKind::Stored;
    // By default, treat the requirement as not having a setter.
    WriteImplKind writeImpl = WriteImplKind::Immutable;
    ReadWriteImplKind readWriteImpl = ReadWriteImplKind::Immutable;

    if (storage->getParsedAccessor(AccessorKind::Set)) {
      readImpl = ReadImplKind::Get;
      writeImpl = WriteImplKind::Set;
      readWriteImpl = ReadWriteImplKind::MaterializeToTemporary;
    } else if (storage->getParsedAccessor(AccessorKind::Get)) {
      readImpl = ReadImplKind::Get;
    }

    StorageImplInfo info(readImpl, writeImpl, readWriteImpl);
    finishStorageImplInfo(storage, info);

    return info;
  }

  // Expand any attached accessor macros.
  (void)evaluateOrDefault(evaluator, ExpandAccessorMacros{storage}, { });

  bool hasWillSet = storage->getParsedAccessor(AccessorKind::WillSet);
  bool hasDidSet = storage->getParsedAccessor(AccessorKind::DidSet);
  if ((hasWillSet || hasDidSet) && !isa<SubscriptDecl>(storage)) {
    // Observers conflict with non-observers.
    AccessorDecl *firstNonObserver = nullptr;
    for (auto accessor : storage->getAllAccessors()) {
      if (!accessor->isImplicit() && !accessor->isObservingAccessor()) {
        firstNonObserver = accessor;
        break;
      }
    }

    if (firstNonObserver) {
      if (auto willSet = storage->getParsedAccessor(AccessorKind::WillSet)) {
        willSet->diagnose(
            diag::observing_accessor_conflicts_with_accessor, 0,
            getAccessorNameForDiagnostic(
                firstNonObserver->getAccessorKind(), /*article=*/ true));
        willSet->setInvalid();
        hasWillSet = false;
      }

      if (auto didSet = storage->getParsedAccessor(AccessorKind::DidSet)) {
        didSet->diagnose(
            diag::observing_accessor_conflicts_with_accessor, 1,
            getAccessorNameForDiagnostic(
              firstNonObserver->getAccessorKind(), /*article=*/ true));
        didSet->setInvalid();
        hasDidSet = false;
      }
    }
  }

  bool hasSetter = storage->getParsedAccessor(AccessorKind::Set);
  bool hasModify = storage->getParsedAccessor(AccessorKind::Modify);
  bool hasMutableAddress = storage->getParsedAccessor(AccessorKind::MutableAddress);
  bool hasInit = storage->getParsedAccessor(AccessorKind::Init);

  auto *DC = storage->getDeclContext();
  // 'get', 'read', and a non-mutable addressor are all exclusive.
  ReadImplKind readImpl;
  if (storage->getParsedAccessor(AccessorKind::Get)) {
    readImpl = ReadImplKind::Get;
  } else if (storage->getParsedAccessor(AccessorKind::Read)) {
    readImpl = ReadImplKind::Read;
  } else if (storage->getParsedAccessor(AccessorKind::Address)) {
    readImpl = ReadImplKind::Address;

  // If there's a writing accessor of any sort, there must also be a
  // reading accessor.
  } else if (hasInit || hasSetter || hasModify || hasMutableAddress) {
    readImpl = ReadImplKind::Get;

  // Subscripts always have to have some sort of accessor; they can't be
  // purely stored.
  } else if (isa<SubscriptDecl>(storage)) {
    readImpl = ReadImplKind::Get;

  // Check if we have observers.
  } else if (hasWillSet || hasDidSet) {
    if (storage->getAttrs().hasAttribute<OverrideAttr>() &&
        storage->getDeclContext()->isTypeContext()) {
      readImpl = ReadImplKind::Inherited;
    } else {
      readImpl = ReadImplKind::Stored;
    }

  // Extensions and enums can't have stored properties. If there are braces,
  // assume this is an incomplete computed property. This avoids an
  // "extensions|enums must not contain stored properties" error later on.
  } else if ((isa<ExtensionDecl>(DC) || isa<EnumDecl>(DC)) &&
             storage->getBracesRange().isValid()) {
    readImpl = ReadImplKind::Get;

  // Otherwise, it's stored.
  } else {
    readImpl = ReadImplKind::Stored;
  }

  // Prefer using 'set' and 'modify' over a mutable addressor.
  WriteImplKind writeImpl;
  ReadWriteImplKind readWriteImpl;
  if (hasSetter) {
    writeImpl = WriteImplKind::Set;
    if (hasModify) {
      readWriteImpl = ReadWriteImplKind::Modify;
    } else {
      readWriteImpl = ReadWriteImplKind::MaterializeToTemporary;
    }
  } else if (hasModify) {
    writeImpl = WriteImplKind::Modify;
    readWriteImpl = ReadWriteImplKind::Modify;
  } else if (hasMutableAddress) {
    writeImpl = WriteImplKind::MutableAddress;
    readWriteImpl = ReadWriteImplKind::MutableAddress;

  // Check if we have observers.
  } else if (readImpl == ReadImplKind::Inherited) {
    writeImpl = WriteImplKind::InheritedWithObservers;

    if (hasWillSet)
      readWriteImpl = ReadWriteImplKind::MaterializeToTemporary;
    else
      readWriteImpl = ReadWriteImplKind::InheritedWithDidSet;

  // Otherwise, it's stored.
  } else if (readImpl == ReadImplKind::Stored &&
             !cast<VarDecl>(storage)->isLet()) {
    if (hasWillSet || hasDidSet) {
      writeImpl = WriteImplKind::StoredWithObservers;

      if (hasWillSet)
        readWriteImpl = ReadWriteImplKind::MaterializeToTemporary;
      else
        readWriteImpl = ReadWriteImplKind::StoredWithDidSet;
    } else {
      writeImpl = WriteImplKind::Stored;
      readWriteImpl = ReadWriteImplKind::Stored;
    }

  // Otherwise, it's immutable.
  } else {
    writeImpl = WriteImplKind::Immutable;
    readWriteImpl = ReadWriteImplKind::Immutable;
  }

  StorageImplInfo info(readImpl, writeImpl, readWriteImpl);
  finishStorageImplInfo(storage, info);

  assert(info.hasStorage() == storage->hasStorage() ||
         storage->getASTContext().Diags.hadAnyError());
  return info;
}

bool SimpleDidSetRequest::evaluate(Evaluator &evaluator,
                                   AccessorDecl *decl) const {

  class OldValueFinder : public ASTWalker {
    const ParamDecl *OldValueParam;
    bool foundOldValueRef = false;

  public:
    OldValueFinder(const ParamDecl *param) : OldValueParam(param) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::ArgumentsAndExpansion;
    }


    virtual PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E)
        return Action::Continue(E);
      if (auto DRE = dyn_cast<DeclRefExpr>(E)) {
        if (auto decl = DRE->getDecl()) {
          if (decl == OldValueParam) {
            foundOldValueRef = true;
            return Action::Stop();
          }
        }
      }

      return Action::Continue(E);
    }

    bool didFindOldValueRef() { return foundOldValueRef; }
  };

  // If this is not a didSet accessor, bail out.
  if (decl->getAccessorKind() != AccessorKind::DidSet) {
    return false;
  }

  // Always assume non-simple 'didSet' in code completion mode.
  if (decl->getASTContext().SourceMgr.hasIDEInspectionTargetBuffer())
    return false;

  // didSet must have a single parameter.
  if (decl->getParameters()->size() != 1) {
    return false;
  }

  auto param = decl->getParameters()->get(0);
  // If this parameter is not implicit, then it means it has been explicitly
  // provided by the user (i.e. 'didSet(oldValue)'). This means we cannot
  // consider this a "simple" didSet because we have to fetch the oldValue
  // regardless of whether it's referenced in the body or not.
  if (!param->isImplicit()) {
    return false;
  }

  // If we find a reference to the implicit 'oldValue' parameter, then it is
  // not a "simple" didSet because we need to fetch it.
  auto walker = OldValueFinder(param);
  if (auto *body = decl->getTypecheckedBody())
    body->walk(walker);
  auto hasOldValueRef = walker.didFindOldValueRef();
  if (!hasOldValueRef) {
    // If the body does not refer to implicit 'oldValue', it means we can
    // consider this as a "simple" didSet. Let's also erase the implicit
    // oldValue as it is never used.
    auto &ctx = decl->getASTContext();
    decl->setParameters(ParameterList::createEmpty(ctx));
    return true;
  }
  return false;
}

