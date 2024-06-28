//===--- CodeCompletionResultType.cpp -------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/CodeCompletionResultType.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Assertions.h"
#include "swift/Sema/IDETypeChecking.h"

using namespace swift;
using namespace ide;
using TypeRelation = CodeCompletionResultTypeRelation;

#define DEBUG_TYPE "CodeCompletionResultType"
#include "llvm/Support/Debug.h"

// MARK: - Utilities

/// Returns the kind of attributes \c Ty can be used as.
static OptionSet<CustomAttributeKind> getCustomAttributeKinds(Type Ty) {
  OptionSet<CustomAttributeKind> Result;
  if (auto NominalTy = Ty->getAs<NominalOrBoundGenericNominalType>()) {
    auto NominalDecl = NominalTy->getDecl();
    if (NominalDecl->getAttrs().hasAttribute<PropertyWrapperAttr>()) {
      Result |= CustomAttributeKind::PropertyWrapper;
    }
    if (NominalDecl->getAttrs().hasAttribute<ResultBuilderAttr>()) {
      Result |= CustomAttributeKind::ResultBuilder;
    }
    if (NominalDecl->isGlobalActor()) {
      Result |= CustomAttributeKind::GlobalActor;
    }
  }
  return Result;
}

// MARK: - USRBasedTypeContext

USRBasedTypeContext::USRBasedTypeContext(const ExpectedTypeContext *TypeContext,
                                         USRBasedTypeArena &Arena)
    : Arena(Arena), ExpectedCustomAttributeKinds(
                        TypeContext->getExpectedCustomAttributeKinds()) {

  for (auto possibleTy : TypeContext->getPossibleTypes()) {
    ContextualTypes.emplace_back(USRBasedType::fromType(possibleTy, Arena));

    // Add the unwrapped optional types as 'convertible' contextual types.
    auto UnwrappedOptionalType = possibleTy->getOptionalObjectType();
    while (UnwrappedOptionalType) {
      ContextualTypes.emplace_back(
          USRBasedType::fromType(UnwrappedOptionalType, Arena));
      UnwrappedOptionalType = UnwrappedOptionalType->getOptionalObjectType();
    }

    // If the contextual type is an opaque return type, make the protocol a
    // contextual type. E.g. if we have
    //   func foo() -> some View { #^COMPLETE^# }
    // we should show items conforming to `View` as convertible.
    if (auto OpaqueType = possibleTy->getAs<OpaqueTypeArchetypeType>()) {
      llvm::SmallVector<const USRBasedType *, 1> USRTypes;
      if (auto Superclass = OpaqueType->getSuperclass()) {
        USRTypes.push_back(USRBasedType::fromType(Superclass, Arena));
      }
      for (auto Proto : OpaqueType->getConformsTo()) {
        USRTypes.push_back(
            USRBasedType::fromType(Proto->getDeclaredInterfaceType(), Arena));
      }
      // Archetypes are also be used to model generic return types, in which
      // case they don't have any conformsTo entries. We simply ignore those.
      if (!USRTypes.empty()) {
        ContextualTypes.emplace_back(USRTypes);
      }
    }
  }
}

TypeRelation
USRBasedTypeContext::typeRelation(const USRBasedType *ResultType) const {
  if (ExpectedCustomAttributeKinds) {
    return ResultType->getCustomAttributeKinds() & ExpectedCustomAttributeKinds
               ? TypeRelation::Convertible
               : TypeRelation::Unrelated;
  }
  const USRBasedType *VoidType = Arena.getVoidType();
  if (ResultType == VoidType) {
    // Void is not convertible to anything and we don't report Void <-> Void
    // identical matches (see USRBasedType::typeRelation). So we don't have to
    // check anything if the result returns Void.
    return TypeRelation::Unknown;
  }

  TypeRelation Res = TypeRelation::Unknown;
  for (auto &ContextualType : ContextualTypes) {
    Res = std::max(Res, ContextualType.typeRelation(ResultType, VoidType));
    if (Res == TypeRelation::MAX_VALUE) {
      return Res; // We can't improve further
    }
  }
  return Res;
}

// MARK: - USRBasedTypeArena

USRBasedTypeArena::USRBasedTypeArena() {
  // '$sytD' is the USR of the Void type.
  VoidType = USRBasedType::fromUSR("$sytD", {}, {}, *this);
}

const USRBasedType *USRBasedTypeArena::getVoidType() const { return VoidType; }

// MARK: - USRBasedType

TypeRelation USRBasedType::typeRelationImpl(
    const USRBasedType *ResultType, const USRBasedType *VoidType,
    SmallPtrSetImpl<const USRBasedType *> &VisitedTypes) const {

  // `this` is the contextual type.
  if (this == VoidType) {
    // We don't report Void <-> Void matches because that would boost
    // methods returning Void in e.g.
    // func foo() { #^COMPLETE^# }
    // because #^COMPLETE^# is implicitly returned. But that's not very
    // helpful.
    return TypeRelation::Unknown;
  }
  if (ResultType == this) {
    return TypeRelation::Convertible;
  }
  for (const USRBasedType *Supertype : ResultType->getSupertypes()) {
    if (!VisitedTypes.insert(Supertype).second) {
      // Already visited this type.
      continue;
    }
    if (this->typeRelation(Supertype, VoidType) >= TypeRelation::Convertible) {
      return TypeRelation::Convertible;
    }
  }
  // TypeRelation computation based on USRs is an under-approximation because we
  // don't take into account generic conversions or retroactive conformance of
  // library types. Hence, we can't know for sure that ResultType is not
  // convertible to `this` type and thus can't return Unrelated or Invalid here.
  return TypeRelation::Unknown;
}

const USRBasedType *USRBasedType::null(USRBasedTypeArena &Arena) {
  return USRBasedType::fromUSR(/*USR=*/"", /*Supertypes=*/{}, {}, Arena);
}

const USRBasedType *
USRBasedType::fromUSR(StringRef USR, ArrayRef<const USRBasedType *> Supertypes,
                      OptionSet<CustomAttributeKind> CustomAttributeKinds,
                      USRBasedTypeArena &Arena) {
  auto ExistingTypeIt = Arena.CanonicalTypes.find(USR);
  if (ExistingTypeIt != Arena.CanonicalTypes.end()) {
    return ExistingTypeIt->second;
  }
  // USR and Supertypes need to be allocated in the arena to be passed into the
  // USRBasedType constructor. The elements of Supertypes are already allocated
  // in the arena.
  USR = USR.copy(Arena.Allocator);
  Supertypes = Supertypes.copy(Arena.Allocator);

  const USRBasedType *Result =
      new (Arena.Allocator) USRBasedType(USR, Supertypes, CustomAttributeKinds);
  Arena.CanonicalTypes[USR] = Result;
  return Result;
}

const USRBasedType *USRBasedType::fromType(Type Ty, USRBasedTypeArena &Arena) {
  if (!Ty) {
    return USRBasedType::null(Arena);
  }

  // USRBasedTypes are backed by canonical types so that equivalent types have
  // the same USR.
  Ty = Ty->getCanonicalType();

  // For opaque types like 'some View', consider them equivalent to 'View'.
  if (auto OpaqueType = Ty->getAs<ArchetypeType>()) {
    if (auto Existential = OpaqueType->getExistentialType()) {
      Ty = Existential;
    }
  }
  // We can't represent more complicated archetypes like 'some View & MyProto'
  // in USRBasedType yet. Simply map them to null types for now.
  if (Ty->hasArchetype()) {
    return USRBasedType::null(Arena);
  }

  // ParameterizedProtocolType should always be wrapped in ExistentialType and
  // cannot be mangled on its own.
  // But ParameterizedProtocolType can currently occur in 'typealias'
  // declarations. rdar://99176683
  // To avoid crashing in USR generation, simply return a null type until the
  // underlying issue has been fixed.
  if (Ty->is<ParameterizedProtocolType>()) {
    return USRBasedType::null(Arena);
  }

  SmallString<32> USR;
  llvm::raw_svector_ostream OS(USR);
  printTypeUSR(Ty, OS);

  // Check the USRBasedType cache in the arena as quickly as possible to avoid
  // converting the entire supertype hierarchy from AST-based types to
  // USRBasedTypes.
  auto ExistingTypeIt = Arena.CanonicalTypes.find(USR);
  if (ExistingTypeIt != Arena.CanonicalTypes.end()) {
    return ExistingTypeIt->second;
  }

  LLVM_DEBUG(llvm::dbgs() << "enter USRBasedType(" << Ty << ", USR = "
                          << USR << ")\n";
             Ty->dump(llvm::dbgs()););

  SmallVector<const USRBasedType *, 2> Supertypes;
  ;
  if (auto Nominal = Ty->getAnyNominal()) {
    if (auto *Proto = dyn_cast<ProtocolDecl>(Nominal)) {
      for (auto *inherited : Proto->getAllInheritedProtocols()) {
        if (!inherited->isSpecificProtocol(KnownProtocolKind::Sendable) &&
            !inherited->getInvertibleProtocolKind()) {
          LLVM_DEBUG(llvm::dbgs() << "Adding inherited protocol "
                                  << inherited->getName()
                                  << "\n";);
          Supertypes.push_back(USRBasedType::fromType(
            inherited->getDeclaredInterfaceType(), Arena));
        }
      }
    } else {
      auto Conformances = Nominal->getAllConformances();
      Supertypes.reserve(Conformances.size());
      for (auto Conformance : Conformances) {
        if (isa<InheritedProtocolConformance>(Conformance)) {
          // Skip inherited conformances; we'll collect them when we visit the
          // superclass.
          continue;
        }
        if (Conformance->getDeclContext()->getParentModule() !=
            Nominal->getModuleContext()) {
          // Only include conformances that are declared within the module of the
          // type to avoid caching retroactive conformances which might not
          // exist when using the code completion cache from a different module.
          continue;
        }
        if (Conformance->getProtocol()->isSpecificProtocol(KnownProtocolKind::Sendable) ||
            Conformance->getProtocol()->getInvertibleProtocolKind()) {
          // FIXME: Sendable conformances are lazily synthesized as they are
          // needed by the compiler. Depending on whether we checked whether a
          // type conforms to Sendable before constructing the USRBasedType, we
          // get different results for its conformance. For now, always drop the
          // Sendable conformance.
          //
          // FIXME: Copyable and Escapable conformances are skipped because the
          // USR mangling produces the type 'Any' for the protocol type.
          continue;
        }
        LLVM_DEBUG(llvm::dbgs() << "Adding conformed protocol "
                                << Conformance->getProtocol()->getName()
                                << "\n";);
        Supertypes.push_back(USRBasedType::fromType(
            Conformance->getProtocol()->getDeclaredInterfaceType(), Arena));
      }
    }
  }

  // You would think that superclass + conformances form a DAG. You are wrong!
  // We can achieve a circular supertype hierarchy with
  //
  // protocol Proto : Class {}
  // class Class : Proto {}
  //
  // USRBasedType is not set up for this. Serialization of code completion
  // results from global modules can't handle cycles in the supertype hierarchy
  // because it writes the DAG leaf to root(s) and needs to know the type
  // offsets. To get consistent results independent of where we start
  // constructing USRBasedTypes, ignore superclasses of protocols. If we kept
  // track of already visited types, we would get different results depending on
  // whether we start constructing the USRBasedType hierarchy from Proto or
  // Class.
  // Ignoring superclasses of protocols is safe to do because USRBasedType is an
  // under-approximation anyway.

  /// If `Ty` is a class type and has a superclass, return that. In all other
  /// cases, return null.
  auto getSuperclass = [](Type Ty) -> Type {
    if (isa_and_nonnull<ClassDecl>(Ty->getAnyNominal())) {
      return Ty->getSuperclass();
    } else {
      return Type();
    }
  };

  Type Superclass = getSuperclass(Ty);
  while (Superclass) {
    LLVM_DEBUG(llvm::dbgs() << "Adding superclass "
                            << Superclass
                            << "\n";);
    Supertypes.push_back(USRBasedType::fromType(Superclass, Arena));
    Superclass = getSuperclass(Superclass);
  }

  assert(llvm::all_of(Supertypes, [&USR](const USRBasedType *Ty) {
    if (Ty->getUSR() == USR) {
      LLVM_DEBUG(llvm::dbgs() << "Duplicate USR: " << USR << "\n";);
    }
    return Ty->getUSR() != USR;
  }) && "Circular supertypes?");

  llvm::SmallPtrSet<const USRBasedType *, 2> ImpliedSupertypes;
  for (auto Supertype : Supertypes) {
    ImpliedSupertypes.insert(Supertype->getSupertypes().begin(),
                             Supertype->getSupertypes().end());
  }
  llvm::erase_if(Supertypes, [&ImpliedSupertypes](const USRBasedType *Ty) {
    return ImpliedSupertypes.contains(Ty);
  });

  LLVM_DEBUG(llvm::dbgs() << "leave USRBasedType(" << Ty << ")\n";
             Ty->dump(llvm::dbgs()););

  return USRBasedType::fromUSR(USR, Supertypes, ::getCustomAttributeKinds(Ty),
                               Arena);
}

TypeRelation USRBasedType::typeRelation(const USRBasedType *ResultType,
                                        const USRBasedType *VoidType) const {
  SmallPtrSet<const USRBasedType *, 4> VisitedTypes;
  return this->typeRelationImpl(ResultType, VoidType, VisitedTypes);
}

// MARK: - USRBasedTypeContext

TypeRelation USRBasedTypeContext::ContextualType::typeRelation(
    const USRBasedType *ResultType, const USRBasedType *VoidType) const {
  assert(!Types.empty() && "A contextual type should have at least one type");

  /// Types is a conjunction, not a disjunction (see documentation on Types),
  /// so we need to compute the minimum type relation here.
  TypeRelation Result = TypeRelation::Convertible;
  for (auto ContextType : Types) {
    Result = std::min(Result, ContextType->typeRelation(ResultType, VoidType));
  }
  return Result;
}

// MARK: - CodeCompletionResultType

/// Returns \c true if \p Ty is the 'Any' type or some type that is sufficiently
/// similar to Any, like the 'Any' metatype or an optional type wrapping 'Any'.
static bool isEssentiallyAnyType(Type Ty) {
  while (true) {
    if (auto MT = Ty->getAs<AnyMetatypeType>()) {
      Ty = MT->getInstanceType();
    } else if (auto OT = Ty->getOptionalObjectType()) {
      Ty = OT;
    } else {
      break;
    }
  }
  return Ty->isAny();
}

static TypeRelation calculateTypeRelation(Type Ty, Type ExpectedTy,
                                          const DeclContext &DC) {
  if (Ty.isNull() || ExpectedTy.isNull() || Ty->is<ErrorType>() ||
      ExpectedTy->is<ErrorType>())
    return TypeRelation::Unrelated;

  /// Computing type relations to 'Any' is not very enlightning because
  /// everything would be convertible to it. If the contextual type is 'Any',
  /// just report all type relations as 'Unknown'.
  if (isEssentiallyAnyType(ExpectedTy)) {
    return TypeRelation::Unknown;
  }

  // Equality/Conversion of GenericTypeParameterType won't account for
  // requirements – ignore them
  if (!Ty->hasTypeParameter() && !ExpectedTy->hasTypeParameter()) {
    if (Ty->isEqual(ExpectedTy))
      return TypeRelation::Convertible;
    bool isAny = false;
    isAny |= ExpectedTy->isAny();
    isAny |= ExpectedTy->is<ArchetypeType>() &&
             ExpectedTy->castTo<ArchetypeType>()->getExistentialType()->isAny();

    if (!isAny && isConvertibleTo(Ty, ExpectedTy, /*openArchetypes=*/true,
                                  const_cast<DeclContext &>(DC)))
      return TypeRelation::Convertible;
  }
  if (auto FT = Ty->getAs<AnyFunctionType>()) {
    if (FT->getResult()->isVoid())
      return TypeRelation::Invalid;
  }
  return TypeRelation::Unrelated;
}

static TypeRelation
calculateMaxTypeRelation(Type Ty, const ExpectedTypeContext &typeContext,
                         const DeclContext &DC) {
  if (Ty->isVoid() && typeContext.requiresNonVoid())
    return TypeRelation::Invalid;
  if (typeContext.getExpectedCustomAttributeKinds()) {
    return (getCustomAttributeKinds(Ty) &
            typeContext.getExpectedCustomAttributeKinds())
               ? TypeRelation::Convertible
               : TypeRelation::Unrelated;
  }
  if (typeContext.empty())
    return TypeRelation::Unknown;

  if (auto funcTy = Ty->getAs<AnyFunctionType>())
    Ty = funcTy->removeArgumentLabels(1);

  auto Result = TypeRelation::Unrelated;
  for (auto expectedTy : typeContext.getPossibleTypes()) {
    // Do not use Void type context for an implied result such as a
    // single-expression closure body, since the implicit return does not
    // constrain the expression.
    //
    //     { ... -> ()  in x } // x can be anything
    //
    // This behaves differently from explicit return, and from non-Void:
    //
    //     { ... -> Int in x }        // x must be Int
    //     { ... -> ()  in return x } // x must be Void
    if (typeContext.isImpliedResult() && expectedTy->isVoid())
      continue;

    Result = std::max(Result, calculateTypeRelation(Ty, expectedTy, DC));
  }

  // Map invalid -> unrelated for an implied result, since the input may be
  // incomplete.
  if (typeContext.isImpliedResult() && Result == TypeRelation::Invalid)
    Result = TypeRelation::Unrelated;

  return Result;
}

bool CodeCompletionResultType::isBackedByUSRs() const {
  return llvm::all_of(
      getResultTypes(),
      [](const PointerUnion<Type, const USRBasedType *> &ResultType) {
        return ResultType.is<const USRBasedType *>();
      });
}

llvm::SmallVector<const USRBasedType *, 1>
CodeCompletionResultType::getUSRBasedResultTypes(
    USRBasedTypeArena &Arena) const {
  llvm::SmallVector<const USRBasedType *, 1> USRBasedTypes;
  auto ResultTypes = getResultTypes();
  USRBasedTypes.reserve(ResultTypes.size());
  for (auto ResultType : ResultTypes) {
    if (auto USRType = ResultType.dyn_cast<const USRBasedType *>()) {
      USRBasedTypes.push_back(USRType);
    } else {
      USRBasedTypes.push_back(
          USRBasedType::fromType(ResultType.get<Type>(), Arena));
    }
  }
  return USRBasedTypes;
}

CodeCompletionResultType
CodeCompletionResultType::usrBasedType(USRBasedTypeArena &Arena) const {
  return CodeCompletionResultType(this->getUSRBasedResultTypes(Arena));
}

TypeRelation CodeCompletionResultType::calculateTypeRelation(
    const ExpectedTypeContext *TypeContext, const DeclContext *DC,
    const USRBasedTypeContext *USRTypeContext) const {
  if (isNotApplicable()) {
    return TypeRelation::NotApplicable;
  }

  if (!TypeContext || !DC) {
    return TypeRelation::Unknown;
  }

  TypeRelation Res = TypeRelation::Unknown;
  for (auto Ty : getResultTypes()) {
    if (auto USRType = Ty.dyn_cast<const USRBasedType *>()) {
      if (!USRTypeContext) {
        assert(false && "calculateTypeRelation must have a USRBasedTypeContext "
                        "passed if it contains a USR-based result type");
        continue;
      }
      Res = std::max(Res, USRTypeContext->typeRelation(USRType));
    } else {
      Res = std::max(
          Res, calculateMaxTypeRelation(Ty.get<Type>(), *TypeContext, *DC));
    }
  }
  return Res;
}
