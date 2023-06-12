//===--- Type.cpp - Swift Language Type ASTs ------------------------------===//
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
//  This file implements the Type class and subclasses.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "ast-types"

#include "swift/AST/Types.h"
#include "ForeignRepresentationInfo.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ReferenceCounting.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeVisitor.h"
#include "swift/AST/TypeWalker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/TypeRepr.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <iterator>
using namespace swift;

#define TYPE(Id, _) \
  static_assert(IsTriviallyDestructible<Id##Type>::value, \
                "Types are BumpPtrAllocated; the destructor is never called");
#include "swift/AST/TypeNodes.def"

void TypeLoc::setType(Type Ty) {
  assert(!Ty || !Ty->hasTypeVariable() || !Ty->hasPlaceholder());
  this->Ty = Ty;
}

bool TypeLoc::isError() const {
  assert(wasValidated() && "Type not yet validated");
  return getType()->hasError();
}

SourceRange TypeLoc::getSourceRange() const {
  if (TyR)
    return TyR->getSourceRange();
  return SourceRange();
}

SourceLoc TypeLoc::getLoc() const {
  if (TyR) return TyR->getLoc();
  return SourceLoc();
}

NominalTypeDecl *CanType::getAnyNominal() const {
  return dyn_cast_or_null<NominalTypeDecl>(getAnyGeneric());
}

GenericTypeDecl *CanType::getAnyGeneric() const {
  if (auto existential = dyn_cast<ExistentialType>(*this))
    return existential->getConstraintType()->getAnyGeneric();
  if (auto ppt = dyn_cast<ParameterizedProtocolType>(*this))
    return ppt->getBaseType()->getDecl();
  if (auto Ty = dyn_cast<AnyGenericType>(*this))
    return Ty->getDecl();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Various Type Methods.
//===----------------------------------------------------------------------===//

/// isEqual - Return true if these two types are equal, ignoring sugar.
bool TypeBase::isEqual(Type Other) const {
  return getCanonicalType() == Other.getPointer()->getCanonicalType();
}

/// hasReferenceSemantics - Does this type have reference semantics?
bool TypeBase::hasReferenceSemantics() {
  return getCanonicalType().hasReferenceSemantics();
}

bool TypeBase::isUninhabited() {
  // Empty enum declarations are uninhabited
  if (auto nominalDecl = getAnyNominal())
    if (auto enumDecl = dyn_cast<EnumDecl>(nominalDecl))
      // Objective-C enums may be allowed to hold any value representable by
      // the underlying type, but only if they come from clang.
      if (enumDecl->getAllElements().empty() &&
          !(enumDecl->isObjC() && enumDecl->hasClangNode()))
        return true;
  return false;
}

bool TypeBase::isStructurallyUninhabited() {
  if (isUninhabited()) return true;
  
  // Tuples of uninhabited types are uninhabited
  if (auto *TTy = getAs<TupleType>())
    for (auto eltTy : TTy->getElementTypes())
      if (eltTy->isStructurallyUninhabited())
        return true;
  return false;
}

bool TypeBase::isAny() {
  Type constraint = this;
  if (auto existential = constraint->getAs<ExistentialType>())
    constraint = existential->getConstraintType();
  return constraint->isEqual(getASTContext().TheAnyType);
}

bool TypeBase::isMarkerExistential() {
  Type constraint = this;
  if (auto existential = constraint->getAs<ExistentialType>())
    constraint = existential->getConstraintType();

  if (!constraint->isConstraintType())
    return false;

  auto layout = constraint->getExistentialLayout();
  if (layout.hasExplicitAnyObject ||
      layout.explicitSuperclass) {
    return false;
  }

  for (auto *proto : layout.getProtocols()) {
    if (!proto->isMarkerProtocol())
      return false;
  }

  return true;
}

bool TypeBase::isPureMoveOnly() {
  if (auto *nom = getAnyNominal())
    return nom->isMoveOnly();

  // if any components of the tuple are move-only, then the tuple is move-only.
  if (auto *tupl = getCanonicalType()->getAs<TupleType>()) {
    for (auto eltTy : tupl->getElementTypes())
      if (eltTy->isPureMoveOnly())
        return true;
  }

  return false;
}

bool TypeBase::isPlaceholder() {
  return is<PlaceholderType>();
}

bool TypeBase::isAnyClassReferenceType() {
  return getCanonicalType().isAnyClassReferenceType();
}

bool CanType::isReferenceTypeImpl(CanType type, const GenericSignatureImpl *sig,
                                  bool functionsCount) {
  switch (type->getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
    llvm_unreachable("sugared canonical type?");

  // These types are always class references.
  case TypeKind::BuiltinNativeObject:
  case TypeKind::BuiltinBridgeObject:
  case TypeKind::Class:
  case TypeKind::BoundGenericClass:
  case TypeKind::SILBox:
    return true;

  // For Self types, recur on the underlying type.
  case TypeKind::DynamicSelf:
    return isReferenceTypeImpl(cast<DynamicSelfType>(type).getSelfType(),
                               sig, functionsCount);
  case TypeKind::SILMoveOnlyWrapped:
    return isReferenceTypeImpl(
        cast<SILMoveOnlyWrappedType>(type)->getInnerType(), sig,
        functionsCount);

  // Archetypes and existentials are only class references if class-bounded.
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype:
    return cast<ArchetypeType>(type)->requiresClass();
  case TypeKind::Protocol:
    return cast<ProtocolType>(type)->requiresClass();
  case TypeKind::ProtocolComposition:
    return cast<ProtocolCompositionType>(type)->requiresClass();
  case TypeKind::ParameterizedProtocol:
    return cast<ParameterizedProtocolType>(type)->getBaseType()->requiresClass();
  case TypeKind::Existential:
    return isReferenceTypeImpl(cast<ExistentialType>(type).getConstraintType(),
                               sig, functionsCount);

  case TypeKind::UnboundGeneric:
    return isa<ClassDecl>(cast<UnboundGenericType>(type)->getDecl());

  // Functions have reference semantics, but are not class references.
  case TypeKind::Function:
  case TypeKind::GenericFunction:
  case TypeKind::SILFunction:
    return functionsCount;

  // Nothing else is statically just a class reference.
  case TypeKind::SILBlockStorage:
  case TypeKind::Error:
  case TypeKind::Unresolved:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinIntegerLiteral:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinRawUnsafeContinuation:
  case TypeKind::BuiltinJob:
  case TypeKind::BuiltinExecutor:
  case TypeKind::BuiltinDefaultActorStorage:
  case TypeKind::BuiltinNonDefaultDistributedActorStorage:
  case TypeKind::BuiltinPackIndex:
  case TypeKind::BuiltinUnsafeValueBuffer:
  case TypeKind::BuiltinVector:
  case TypeKind::Tuple:
  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Metatype:
  case TypeKind::ExistentialMetatype:
  case TypeKind::Module:
  case TypeKind::LValue:
  case TypeKind::InOut:
  case TypeKind::TypeVariable:
  case TypeKind::Placeholder:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
  case TypeKind::SILToken:
  case TypeKind::Pack:
  case TypeKind::PackExpansion:
  case TypeKind::PackElement:
  case TypeKind::SILPack:
#define REF_STORAGE(Name, ...) \
  case TypeKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
    return false;

  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    assert(sig && "dependent types can't answer reference semantics query");
    return sig->requiresClass(type);
  case TypeKind::BuiltinTuple:
    llvm_unreachable("Should not get a BuiltinTupleType here");
  }

  llvm_unreachable("Unhandled type kind!");
}

/// Are variables of this type permitted to have
/// ownership attributes?
///
/// This includes:
///   - class types, generic or not
///   - archetypes with class or class protocol bounds
///   - existentials with class or class protocol bounds
/// But not:
///   - function types
bool TypeBase::allowsOwnership(const GenericSignatureImpl *sig) {
  return getCanonicalType().allowsOwnership(sig);
}

ExistentialLayout::ExistentialLayout(CanProtocolType type) {
  auto *protoDecl = type->getDecl();

  hasExplicitAnyObject = false;
  containsNonObjCProtocol = !protoDecl->isObjC();
  containsParameterized = false;

  protocols.push_back(protoDecl);
}

ExistentialLayout::ExistentialLayout(CanProtocolCompositionType type) {
  hasExplicitAnyObject = type->hasExplicitAnyObject();
  containsNonObjCProtocol = false;
  containsParameterized = false;

  auto members = type.getMembers();
  if (!members.empty() &&
      (members[0].getClassOrBoundGenericClass() ||
       isa<UnboundGenericType>(members[0]))) {
    explicitSuperclass = members[0];
    members = members.slice(1);
  }

  for (auto member : members) {
    ProtocolDecl *protoDecl;
    if (auto protocolType = dyn_cast<ProtocolType>(member)) {
      protoDecl = protocolType->getDecl();
    } else {
      auto parameterized = cast<ParameterizedProtocolType>(member);
      protoDecl = parameterized->getProtocol();
      containsParameterized = true;
    }
    containsNonObjCProtocol |=
        !protoDecl->isObjC() &&
        !protoDecl->isSpecificProtocol(KnownProtocolKind::Sendable);
    protocols.push_back(protoDecl);
  }
}

ExistentialLayout::ExistentialLayout(CanParameterizedProtocolType type)
    : ExistentialLayout(type.getBaseType()) {
  sameTypeRequirements = type->getArgs();
  containsParameterized = true;
}

ExistentialLayout TypeBase::getExistentialLayout() {
  return getCanonicalType().getExistentialLayout();
}

ExistentialLayout CanType::getExistentialLayout() {
  CanType ty = *this;

  // Always remove one layer of move only ness.
  if (auto mv = dyn_cast<SILMoveOnlyWrappedType>(ty))
    ty = mv->getInnerType();

  if (auto existential = dyn_cast<ExistentialType>(ty))
    return existential->getConstraintType()->getExistentialLayout();

  if (auto metatype = dyn_cast<ExistentialMetatypeType>(ty))
    return metatype->getInstanceType()->getExistentialLayout();

  if (auto proto = dyn_cast<ProtocolType>(ty))
    return ExistentialLayout(proto);

  if (auto param = dyn_cast<ParameterizedProtocolType>(ty))
    return ExistentialLayout(param);

  auto comp = cast<ProtocolCompositionType>(ty);
  return ExistentialLayout(comp);
}

bool ExistentialLayout::requiresClass() const {
  if (hasExplicitAnyObject || explicitSuperclass)
    return true;

  for (auto proto : getProtocols()) {
    if (proto->requiresClass())
      return true;
  }

  return false;
}

Type ExistentialLayout::getSuperclass() const {
  if (explicitSuperclass)
    return explicitSuperclass;

  for (auto protoDecl : getProtocols()) {
    // If we have a generic signature, check there, because it
    // will pick up superclass constraints from protocols that we
    // refine as well.
    if (auto genericSig = protoDecl->getGenericSignature()) {
      if (auto superclass = genericSig->getSuperclassBound(
            protoDecl->getSelfInterfaceType()))
        return superclass;
    } else if (auto superclass = protoDecl->getSuperclass())
      return superclass;
  }

  return Type();
}

bool ExistentialLayout::isAnyObject() const {
  return (hasExplicitAnyObject && !explicitSuperclass && getProtocols().empty());
}

bool TypeBase::isObjCExistentialType() {
  return getCanonicalType().isObjCExistentialType();
}

bool TypeBase::isTypeErasedGenericClassType() {
  return getCanonicalType().isTypeErasedGenericClassType();
}

bool CanType::isObjCExistentialTypeImpl(CanType type) {
  if (!type.isExistentialType())
    return false;

  return type.getExistentialLayout().isObjC();
}

bool CanType::isTypeErasedGenericClassTypeImpl(CanType type) {
  if (auto nom = type->getAnyNominal())
    return nom->isTypeErasedGenericClass();
  return false;
}

NominalTypeDecl *TypeBase::getAnyActor() {
  // Nominal types: check whether the declaration is an actor.
  if (auto nominal = getAnyNominal()) {
    if (nominal->isAnyActor())
      return nominal;
  }

  // Archetypes check for conformance to Actor.
  if (auto archetype = getAs<ArchetypeType>()) {
    for (auto proto : archetype->getConformsTo()) {
      if (proto->isAnyActor())
        return proto;
    }

    return nullptr;
  }

  // Existential types: check for Actor protocol.
  if (isExistentialType()) {
    auto layout = getExistentialLayout();
    if (auto superclass = layout.getSuperclass()) {
      if (auto actor = superclass->getAnyActor())
        return actor;
    }

    for (auto proto : layout.getProtocols()) {
      if (proto->isAnyActor())
        return proto;
    }

    return nullptr;
  }

  return nullptr;
}

bool TypeBase::isActorType() {
  if (auto actor = getAnyActor())
    return actor->isActor();
  return false;
}

bool TypeBase::isDistributedActor() {
  if (auto actor = getAnyActor())
    return actor->isDistributedActor();
  return false;
}

bool TypeBase::isSpecialized() {
  Type t = getCanonicalType();

  for (;;) {
    if (!t || !t->getAnyNominal())
      return false;
    if (t->is<BoundGenericType>())
      return true;
    t = t->getNominalParent();
  }

  return false;
}

bool TypeBase::hasOpenedExistentialWithRoot(
    const OpenedArchetypeType *root) const {
  assert(root->isRoot() && "Expected a root archetype");

  if (!hasOpenedExistential())
    return false;

  return getCanonicalType().findIf([&](Type type) -> bool {
    auto *opened = dyn_cast<OpenedArchetypeType>(type.getPointer());
    if (!opened)
      return false;

    return opened->getRoot() == root;
  });
}

void TypeBase::getRootOpenedExistentials(
    SmallVectorImpl<OpenedArchetypeType *> &rootOpenedArchetypes) const {
  if (!hasOpenedExistential())
    return;

  SmallPtrSet<OpenedArchetypeType *, 4> known;
  getCanonicalType().findIf([&](Type type) -> bool {
    auto *archetype = dyn_cast<OpenedArchetypeType>(type.getPointer());
    if (!archetype)
      return false;

    auto *root = archetype->getRoot();
    if (known.insert(root).second)
      rootOpenedArchetypes.push_back(root);

    return false;
  });
}

Type TypeBase::typeEraseOpenedArchetypesWithRoot(
    const OpenedArchetypeType *root, const DeclContext *useDC) const {
  assert(root->isRoot() && "Expected a root archetype");

  Type type = Type(const_cast<TypeBase *>(this));
  if (!hasOpenedExistential())
    return type;

  const auto sig = root->getASTContext().getOpenedExistentialSignature(
      root->getExistentialType(), useDC->getGenericSignatureOfContext());

  unsigned metatypeDepth = 0;

  std::function<Type(Type)> transformFn;
  transformFn = [&](Type type) -> Type {
    return type.transformRec([&](TypeBase *ty) -> Optional<Type> {
      // Don't recurse into children unless we have to.
      if (!ty->hasOpenedExistential())
        return Type(ty);

      if (isa<MetatypeType>(ty)) {
        const auto instanceTy = ty->getMetatypeInstanceType();
        ++metatypeDepth;
        const auto erasedTy = transformFn(instanceTy);
        --metatypeDepth;

        if (instanceTy.getPointer() == erasedTy.getPointer()) {
          return Type(ty);
        }

        return Type(ExistentialMetatypeType::get(erasedTy));
      }

      // Opaque types whose substitutions involve this type parameter are
      // erased to their upper bound.
      if (auto opaque = dyn_cast<OpaqueTypeArchetypeType>(ty)) {
        for (auto replacementType :
                 opaque->getSubstitutions().getReplacementTypes()) {
          if (replacementType->hasOpenedExistentialWithRoot(root)) {
            Type interfaceType = opaque->getInterfaceType();
            auto genericSig =
                opaque->getDecl()->getOpaqueInterfaceGenericSignature();
            return genericSig->getNonDependentUpperBounds(interfaceType);
          }
        }
      }

      auto *const archetype = dyn_cast<OpenedArchetypeType>(ty);
      if (!archetype) {
        // Recurse.
        return None;
      }

      if (!root->isEqual(archetype->getRoot())) {
        return Type(ty);
      }

      Type erasedTy;
      if (root->isEqual(archetype)) {
        erasedTy = root->getExistentialType();
      } else {
        erasedTy =
            sig->getNonDependentUpperBounds(archetype->getInterfaceType());
      }

      if (metatypeDepth) {
        if (const auto existential = erasedTy->getAs<ExistentialType>())
          return existential->getConstraintType();
      }

      return erasedTy;
    });
  };

  return transformFn(type);
}

Type TypeBase::addCurriedSelfType(const DeclContext *dc) {
  if (!dc->isTypeContext())
    return this;

  auto *type = this;

  GenericSignature sig = dc->getGenericSignatureOfContext();
  if (auto *genericFn = type->getAs<GenericFunctionType>()) {
    sig = genericFn->getGenericSignature();
    type = FunctionType::get(genericFn->getParams(),
                             genericFn->getResult(),
                             genericFn->getExtInfo());
  }

  auto selfTy = dc->getSelfInterfaceType();
  auto selfParam = AnyFunctionType::Param(selfTy);
  // FIXME: Verify ExtInfo state is correct, not working by accident.
  if (sig) {
    GenericFunctionType::ExtInfo info;
    return GenericFunctionType::get(sig, {selfParam}, type, info);
  }
  FunctionType::ExtInfo info;
  return FunctionType::get({selfParam}, type, info);
}

void TypeBase::getTypeVariables(
    SmallPtrSetImpl<TypeVariableType *> &typeVariables) {
  // If we know we don't have any type variables, we're done.
  if (hasTypeVariable()) {
    auto addTypeVariables = [&](Type type) -> bool {
      if (auto tv = dyn_cast<TypeVariableType>(type.getPointer())) {
        typeVariables.insert(tv);
      }

      return false;
    };

    // Use Type::findIf() to walk the types, finding type variables along the
    // way.
    getCanonicalType().findIf(addTypeVariables);
    Type(this).findIf(addTypeVariables);
    assert((!typeVariables.empty() || hasError()) &&
           "Did not find type variables!");
  }
}

static bool isLegalSILType(CanType type);

static bool isLegalSILTypeOrPackExpansion(CanType type) {
  // Pack expansions aren't legal in arbitrary positions in SIL;
  // for example, we should never see a function parameter or result
  // of pack-expansion type.  But they're allowed within SILPackTypes
  // and SIL TupleTypes as long as their pattern types are legal.
  // The count type should always be an AST type.
  if (auto packExpansionType = dyn_cast<PackExpansionType>(type)) {
    return isLegalSILType(packExpansionType.getPatternType());
  }

  return isLegalSILType(type);
}

static bool isLegalSILType(CanType type) {
  // L-values and inouts are not legal.
  if (!type->isMaterializable()) return false;

  // Function types must be lowered.
  if (isa<AnyFunctionType>(type)) return false;

  // Metatypes must have a representation.
  if (auto meta = dyn_cast<AnyMetatypeType>(type))
    return meta->hasRepresentation();

  // Tuples are legal if all their elements are legal.
  if (auto tupleType = dyn_cast<TupleType>(type)) {
    for (auto eltType : tupleType.getElementTypes()) {
      if (!isLegalSILTypeOrPackExpansion(eltType)) return false;
    }
    return true;
  }

  // Packs must be lowered.
  if (isa<PackType>(type)) return false;

  // Optionals are legal if their object type is legal.
  if (auto objectType = type.getOptionalObjectType()) {
    return isLegalSILType(objectType);
  }

  // Reference storage types are legal if their object type is legal.
  if (auto refType = dyn_cast<ReferenceStorageType>(type))
    return isLegalSILType(refType.getReferentType());

  return true;
}

bool TypeBase::isLegalSILType() {
  return ::isLegalSILType(getCanonicalType());
}

static bool isLegalFormalType(CanType type) {
  // L-values and inouts are not formal types.
  if (!type->isMaterializable()) return false;

  // Function types must not be lowered.
  if (isa<SILFunctionType>(type)) return false;

  // Pack types must not be lowered.
  if (isa<SILPackType>(type)) return false;

  // Reference storage types are not formal types.
  if (isa<ReferenceStorageType>(type)) return false;

  // Metatypes must not have a representation.
  if (auto meta = dyn_cast<AnyMetatypeType>(type))
    return !meta->hasRepresentation();

  // Tuples are legal if all their elements are legal.
  if (auto tupleType = dyn_cast<TupleType>(type)) {
    for (auto eltType : tupleType.getElementTypes()) {
      if (!isLegalFormalType(eltType)) return false;
    }
    return true;
  }

  // Optionals are legal if their object type is legal.
  if (auto objectType = type.getOptionalObjectType()) {
    return isLegalFormalType(objectType);
  }

  // Expansions are legal if their pattern type is legal.
  if (auto expansionType = dyn_cast<PackExpansionType>(type)) {
    return isLegalFormalType(expansionType.getPatternType());
  }

  return true;
}

bool TypeBase::isLegalFormalType() {
  return ::isLegalFormalType(getCanonicalType());
}

bool TypeBase::hasTypeRepr() const {
  // A type has a source-printable representation if none of its sub-pieces do
  // /not/ have a source-printable representation.
  return !Type(const_cast<TypeBase *>(this)).findIf([](Type subTy) -> bool {
    switch (subTy->getKind()) {
    case TypeKind::Error:
    case TypeKind::Unresolved:
    case TypeKind::TypeVariable:
      return true;

    case TypeKind::OpenedArchetype:
    case TypeKind::OpaqueTypeArchetype:
    case TypeKind::GenericFunction:
    case TypeKind::LValue:
      return true;

#define REF_STORAGE(Name, ...) \
    case TypeKind::Name##Storage: return true;
#include "swift/AST/ReferenceStorage.def"

    case TypeKind::SILFunction:
    case TypeKind::SILBlockStorage:
    case TypeKind::SILBox:
    case TypeKind::SILToken:
      return true;

    default:
      return false;
    }
  });
}

bool TypeBase::isVoid() {
  if (auto TT = getAs<TupleType>())
    return TT->getNumElements() == 0;
  return false;
}

#define KNOWN_STDLIB_TYPE_DECL(NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
/** Check if this type is equal to Swift.NAME. */ \
bool TypeBase::is##NAME() { \
  if (auto generic = getAnyGeneric()) { \
    if (isa<DECL_CLASS>(generic)) { \
      return getASTContext().get##NAME##Decl() == generic; \
    } \
  } \
  return false; \
}
#include "swift/AST/KnownStdlibTypes.def"

Type TypeBase::getRValueType() {
  // If the type is not an lvalue, this is a no-op.
  if (!hasLValueType())
    return this;

  return Type(this).transform([](Type t) -> Type {
      if (auto *lvalueTy = dyn_cast<LValueType>(t.getPointer()))
        return lvalueTy->getObjectType();
      return t;
    });
}

Type TypeBase::getOptionalObjectType() {
  if (auto boundTy = getAs<BoundGenericEnumType>())
    if (boundTy->getDecl()->isOptionalDecl())
      return boundTy->getGenericArgs()[0];
  return Type();
}

CanType CanType::getOptionalObjectTypeImpl(CanType type) {
  if (auto boundTy = dyn_cast<BoundGenericEnumType>(type))
    if (boundTy->getDecl()->isOptionalDecl())
      return boundTy.getGenericArgs()[0];
  return CanType();
}

Type TypeBase::wrapInOptionalType() const {
  auto &ctx = getASTContext();
  auto *result = BoundGenericEnumType::get(ctx.getOptionalDecl(), Type(),
                                           { getCanonicalType() });
  return result;
}

CanType CanType::wrapInOptionalTypeImpl(CanType type) {
  return type->wrapInOptionalType()->getCanonicalType();
}

Type TypeBase::isArrayType() {
  if (auto boundStruct = getAs<BoundGenericStructType>()) {
    if (boundStruct->getDecl() == getASTContext().getArrayDecl())
      return boundStruct->getGenericArgs()[0];
  }
  return Type();
}

Type TypeBase::getAnyPointerElementType(PointerTypeKind &PTK) {
  auto &C = getASTContext();
  if (isUnsafeMutableRawPointer()) {
    PTK = PTK_UnsafeMutableRawPointer;
    return C.TheEmptyTupleType;
  }
  if (isUnsafeRawPointer()) {
    PTK = PTK_UnsafeRawPointer;
    return C.TheEmptyTupleType;
  }
  if (auto boundTy = getAs<BoundGenericType>()) {
    if (boundTy->isUnsafeMutablePointer()) {
      PTK = PTK_UnsafeMutablePointer;
    } else if (boundTy->isUnsafePointer()) {
      PTK = PTK_UnsafePointer;
    } else if (boundTy->isAutoreleasingUnsafeMutablePointer()) {
      PTK = PTK_AutoreleasingUnsafeMutablePointer;
    } else {
      return Type();
    }
    return boundTy->getGenericArgs()[0];
  }
  return Type();
}

Type TypeBase::wrapInPointer(PointerTypeKind kind) {
  ASTContext &ctx = getASTContext();
  NominalTypeDecl *pointerDecl = ([&ctx, kind] {
    switch (kind) {
    case PTK_UnsafeMutableRawPointer:
    case PTK_UnsafeRawPointer:
      // these pointer types don't take arguments.
      return (NominalTypeDecl*)nullptr;
    case PTK_UnsafePointer:
      return ctx.getUnsafePointerDecl();
    case PTK_UnsafeMutablePointer:
      return ctx.getUnsafeMutablePointerDecl();
    case PTK_AutoreleasingUnsafeMutablePointer:
      return ctx.getAutoreleasingUnsafeMutablePointerDecl();
    }
    llvm_unreachable("bad kind");
  }());

  assert(pointerDecl);
  // Don't fail hard on null pointerDecl.
  if (!pointerDecl) {
    return Type();
  }
  return BoundGenericType::get(pointerDecl, /*parent*/nullptr, Type(this));
}

Type TypeBase::getAnyBufferPointerElementType(BufferPointerTypeKind &BPTK) {
  auto &C = getASTContext();
  if (isUnsafeMutableRawBufferPointer()) {
    BPTK = BPTK_UnsafeMutableRawBufferPointer;
    return C.TheEmptyTupleType;
  }
  if (isUnsafeRawBufferPointer()) {
    BPTK = BPTK_UnsafeRawBufferPointer;
    return C.TheEmptyTupleType;
  }
  if (auto boundTy = getAs<BoundGenericType>()) {
    if (boundTy->isUnsafeMutableBufferPointer()) {
      BPTK = BPTK_UnsafeMutableBufferPointer;
    } else if (boundTy->isUnsafeBufferPointer()) {
      BPTK = BPTK_UnsafeBufferPointer;
    } else {
      return Type();
    }
    return boundTy->getGenericArgs()[0];
  }
  return Type();
}

Type TypeBase::lookThroughSingleOptionalType() {
  Type type(this);
  if (auto objType = type->getOptionalObjectType())
    type = objType;
  return type;
}

Type TypeBase::lookThroughAllOptionalTypes() {
  Type type(this);
  while (auto objType = type->getOptionalObjectType())
    type = objType;

  return type;
}

Type TypeBase::lookThroughAllOptionalTypes(SmallVectorImpl<Type> &optionals){
  Type type(this);
  while (auto objType = type->getOptionalObjectType()) {
    optionals.push_back(type);
    type = objType;
  }

  return type;
}

unsigned int TypeBase::getOptionalityDepth() {
  Type type(this);
  unsigned int depth = 0;
  while (auto objType = type->getOptionalObjectType()) {
    type = objType;
    ++depth;
  }
  return depth;
}

Type TypeBase::stripConcurrency(bool recurse, bool dropGlobalActor) {
  // Look through optionals.
  if (Type optionalObject = getOptionalObjectType()) {
    Type newOptionalObject =
        optionalObject->stripConcurrency(recurse, dropGlobalActor);
    if (optionalObject->isEqual(newOptionalObject))
      return Type(this);

    return OptionalType::get(newOptionalObject);
  }

  // Function types.
  if (auto fnType = getAs<AnyFunctionType>()) {
    // Strip off Sendable and (possibly) the global actor.
    ASTExtInfo extInfo =
        fnType->hasExtInfo() ? fnType->getExtInfo() : ASTExtInfo();
    extInfo = extInfo.withConcurrent(false);
    if (dropGlobalActor)
      extInfo = extInfo.withGlobalActor(Type());

    ArrayRef<AnyFunctionType::Param> params = fnType->getParams();
    Type resultType = fnType->getResult();

    SmallVector<AnyFunctionType::Param, 4> newParams;
    if (recurse) {
      for (unsigned paramIdx : indices(params)) {
        const auto &param = params[paramIdx];
        Type newParamType = param.getPlainType()->stripConcurrency(
            recurse, dropGlobalActor);

        if (!newParams.empty()) {
          newParams.push_back(param.withType(newParamType));
          continue;
        }

        if (newParamType->isEqual(param.getPlainType()))
          continue;

        newParams.append(params.begin(), params.begin() + paramIdx);
        newParams.push_back(param.withType(newParamType));
      }

      if (!newParams.empty())
        params = newParams;

      resultType = resultType->stripConcurrency(recurse, dropGlobalActor);
    }

    // Drop Sendable requirements.
    GenericSignature genericSig;
    if (auto genericFnType = dyn_cast<GenericFunctionType>(fnType)) {
      auto requirements = genericFnType->getRequirements();
      SmallVector<Requirement, 4> newRequirements;
      for (unsigned reqIdx : indices(requirements)) {
        // If it's a Sendable requirement, skip it.
        const auto &req = requirements[reqIdx];
        if (req.getKind() == RequirementKind::Conformance &&
            req.getSecondType()->castTo<ProtocolType>()->getDecl()
              ->isSpecificProtocol(KnownProtocolKind::Sendable))
          continue;

        newRequirements.push_back(req);
      }

      if (newRequirements.size() == requirements.size()) {
        genericSig = genericFnType->getGenericSignature();
      } else {
        genericSig = GenericSignature::get(
            genericFnType->getGenericParams(), newRequirements);
      }
    }

    Type newFnType;
    if (genericSig) {
      newFnType = GenericFunctionType::get(
          genericSig, params, resultType, extInfo);
    } else {
      newFnType = FunctionType::get(params, resultType, extInfo);
    }
    if (newFnType->isEqual(this))
      return Type(this);

    return newFnType;
  }

  if (auto existentialType = getAs<ExistentialType>()) {
    auto newConstraintType = existentialType->getConstraintType()
        ->stripConcurrency(recurse, dropGlobalActor);
    if (newConstraintType.getPointer() ==
            existentialType->getConstraintType().getPointer())
      return Type(this);

    return ExistentialType::get(newConstraintType);
  }

  if (auto protocolType = getAs<ProtocolType>()) {
    if (protocolType->getDecl()->isSpecificProtocol(
            KnownProtocolKind::Sendable))
      return ProtocolCompositionType::get(getASTContext(), { }, false);

    return Type(this);
  }

  if (auto protocolCompositionType = getAs<ProtocolCompositionType>()) {
    SmallVector<Type, 4> newMembers;
    auto members = protocolCompositionType->getMembers();
    for (unsigned i : indices(members)) {
      auto memberType = members[i];
      auto newMemberType =
          memberType->stripConcurrency(recurse, dropGlobalActor);
      if (!newMembers.empty()) {
        newMembers.push_back(newMemberType);
        continue;
      }

      if (memberType.getPointer() != newMemberType.getPointer()) {
        newMembers.append(members.begin(), members.begin() + i);
        newMembers.push_back(newMemberType);
        continue;
      }
    }

    if (!newMembers.empty()) {
      return ProtocolCompositionType::get(
          getASTContext(), newMembers,
          protocolCompositionType->hasExplicitAnyObject());
    }

    return Type(this);
  }

  if (auto existentialMetatype = getAs<ExistentialMetatypeType>()) {
    auto instanceType = existentialMetatype->getExistentialInstanceType();
    auto newInstanceType =
        instanceType->stripConcurrency(recurse, dropGlobalActor);
    if (instanceType.getPointer() != newInstanceType.getPointer()) {
      Optional<MetatypeRepresentation> repr;
      if (existentialMetatype->hasRepresentation())
        repr = existentialMetatype->getRepresentation();
      return ExistentialMetatypeType::get(
          newInstanceType, repr, getASTContext());
    }

    return Type(this);
  }

  return Type(this);
}

bool TypeBase::isAnyObject() {
  auto canTy = getCanonicalType();

  if (!canTy.isExistentialType() || canTy.isForeignReferenceType())
    return false;

  return canTy.getExistentialLayout().isAnyObject();
}

// Distinguish between class-bound types that might be AnyObject vs other
// class-bound types. Only types that are potentially AnyObject might have a
// transparent runtime type wrapper like __SwiftValue. This must look through
// all optional types because dynamic casting sees through them.
bool TypeBase::isPotentiallyAnyObject() {
  Type unwrappedTy = lookThroughAllOptionalTypes();
  if (auto archetype = unwrappedTy->getAs<ArchetypeType>()) {
    // Does archetype have any requirements that contradict AnyObject?
    // 'T : AnyObject' requires a layout constraint, not a conformance.
    return archetype->getConformsTo().empty() && !archetype->getSuperclass();
  }
  return unwrappedTy->isAnyObject();
}

bool ExistentialLayout::isErrorExistential() const {
  auto protocols = getProtocols();
  return (!hasExplicitAnyObject &&
          !explicitSuperclass &&
          protocols.size() == 1 &&
          protocols[0]->isSpecificProtocol(KnownProtocolKind::Error));
}

bool ExistentialLayout::isExistentialWithError(ASTContext &ctx) const {
  auto errorProto = ctx.getProtocol(KnownProtocolKind::Error);
  if (!errorProto) return false;

  for (auto protoDecl : getProtocols()) {
    if (protoDecl == errorProto || protoDecl->inheritsFrom(errorProto))
      return true;
  }

  return false;
}

LayoutConstraint ExistentialLayout::getLayoutConstraint() const {
  if (hasExplicitAnyObject) {
    return LayoutConstraint::getLayoutConstraint(
      LayoutConstraintKind::Class);
  }

  return LayoutConstraint();
}

bool TypeBase::isExistentialWithError() {
  auto canTy = getCanonicalType();

  if (!canTy.isExistentialType()) return false;

  // FIXME: Compute this as a bit in TypeBase so this operation isn't
  // overly expensive.
  auto layout = canTy.getExistentialLayout();
  return layout.isExistentialWithError(getASTContext());
}

bool TypeBase::isOpenedExistentialWithError() {
  if (auto archetype = getAs<OpenedArchetypeType>()) {
    auto errorProto = getASTContext().getErrorDecl();
    if (!errorProto) return false;

    for (auto protoDecl : archetype->getConformsTo()) {
      if (protoDecl == errorProto || protoDecl->inheritsFrom(errorProto))
        return true;
    }
  }
  return false;
}

bool TypeBase::isStdlibType() {
  if (auto *NTD = getAnyNominal()) {
    auto *DC = NTD->getDeclContext();
    return DC->isModuleScopeContext() &&
           DC->getParentModule()->isStdlibModule();
  }
  return false;
}

bool TypeBase::isCGFloat() {
  auto *NTD = getAnyNominal();
  if (!NTD)
    return false;

  auto *DC = NTD->getDeclContext();
  if (!DC->isModuleScopeContext())
    return false;

  auto *module = DC->getParentModule();
  // On macOS `CGFloat` is part of a `CoreGraphics` module,
  // but on Linux it could be found in `Foundation`.
  return (module->getName().is("CoreGraphics") ||
          module->getName().is("Foundation")   ||
          module->getName().is("CoreFoundation")) &&
         NTD->getName().is("CGFloat");
}

bool TypeBase::isKnownStdlibCollectionType() {
  if (isArray() || isDictionary() || isSet()) {
    return true;
  }

  return false;
}

/// Remove argument labels from the function type.
Type TypeBase::removeArgumentLabels(unsigned numArgumentLabels) {
  // If there is nothing to remove, don't.
  if (numArgumentLabels == 0) return Type(this);

  auto fnType = castTo<AnyFunctionType>();

  // Drop argument labels from the input type.
  llvm::SmallVector<AnyFunctionType::Param, 8> unlabeledParams;
  unlabeledParams.reserve(fnType->getNumParams());
  for (const auto &param : fnType->getParams())
    unlabeledParams.push_back(param.getWithoutLabels());

  auto result = fnType->getResult()
                      ->removeArgumentLabels(numArgumentLabels - 1);

  if (auto *genericFnType = dyn_cast<GenericFunctionType>(fnType)) {
    return GenericFunctionType::get(genericFnType->getGenericSignature(),
                                    unlabeledParams, result,
                                    fnType->getExtInfo());
  }

  return FunctionType::get(unlabeledParams, result, fnType->getExtInfo());
}


Type TypeBase::getWithoutParens() {
  Type Ty = this;
  while (auto ParenTy = dyn_cast<ParenType>(Ty.getPointer()))
    Ty = ParenTy->getUnderlyingType();
  return Ty;
}

Type TypeBase::replaceCovariantResultType(Type newResultType,
                                          unsigned uncurryLevel) {
  if (uncurryLevel == 0) {
    bool isLValue = is<LValueType>();

    auto loadedTy = getWithoutSpecifierType();
    if (auto objectType = loadedTy->getOptionalObjectType()) {
      newResultType = OptionalType::get(
          objectType->replaceCovariantResultType(newResultType, uncurryLevel));
    }

    return isLValue ? LValueType::get(newResultType) : newResultType;
  }

  // Determine the input and result types of this function.
  auto fnType = this->castTo<AnyFunctionType>();
  auto inputType = fnType->getParams();
  Type resultType =
    fnType->getResult()->replaceCovariantResultType(newResultType,
                                                    uncurryLevel - 1);

  // Produce the resulting function type.
  if (auto genericFn = dyn_cast<GenericFunctionType>(fnType)) {
    return GenericFunctionType::get(genericFn->getGenericSignature(),
                                    inputType, resultType,
                                    fnType->getExtInfo());
  }
  
  return FunctionType::get(inputType, resultType, fnType->getExtInfo());
}

/// Whether this parameter accepts an unlabeled trailing closure argument
/// using the more-restrictive forward-scan rule.
static bool allowsUnlabeledTrailingClosureParameter(const ParamDecl *param) {
  // inout parameters never allow an unlabeled trailing closure.
  if (param->isInOut())
    return false;

  Type paramType = param->isVariadic() ? param->getVarargBaseTy()
                                       : param->getInterfaceType();
  paramType = paramType->getRValueType()->lookThroughAllOptionalTypes();

  // For autoclosure parameters, look through the autoclosure result type
  // to get the actual argument type.
  if (param->isAutoClosure()) {
    auto fnType = paramType->getAs<AnyFunctionType>();
    if (!fnType)
      return false;

    paramType = fnType->getResult()->lookThroughAllOptionalTypes();
  }

  // After lookup through all optional types, this parameter allows an
  // unlabeled trailing closure if it is (structurally) a function type.
  return paramType->is<AnyFunctionType>();
}

ParameterListInfo::ParameterListInfo(
    ArrayRef<AnyFunctionType::Param> params,
    const ValueDecl *paramOwner,
    bool skipCurriedSelf) {
  defaultArguments.resize(params.size());
  propertyWrappers.resize(params.size());
  implicitSelfCapture.resize(params.size());
  inheritActorContext.resize(params.size());
  variadicGenerics.resize(params.size());

  // No parameter owner means no parameter list means no default arguments
  // - hand back the zeroed bitvector.
  //
  // FIXME: We ought to not request parameter list info in this case.
  if (!paramOwner)
    return;

  // If the decl has a curried self, but we're not allowed to skip it, return.
  if (paramOwner->hasCurriedSelf() && !skipCurriedSelf)
    return;

  // Find the corresponding parameter list.
  const ParameterList *paramList =
      getParameterList(const_cast<ValueDecl *>(paramOwner));

  // No parameter list means no default arguments - hand back the zeroed
  // bitvector.
  if (!paramList) {
    assert(!paramOwner->hasParameterList());
    return;
  }

  if (params.empty())
    return;

  // Arguments and parameters are not guaranteed to always line-up
  // perfectly, e.g. failure diagnostics tries to match argument type
  // to different "candidate" parameters.
  if (params.size() != paramList->size())
    return;

  // Now we have enough information to determine which parameters accept
  // unlabeled trailing closures.
  acceptsUnlabeledTrailingClosures.resize(params.size());

  // Note which parameters have default arguments and/or accept unlabeled
  // trailing closure arguments with the forward-scan rule.
  for (auto i : range(0, params.size())) {
    auto param = paramList->get(i);
    if (param->isDefaultArgument()) {
      defaultArguments.set(i);
    }

    if (allowsUnlabeledTrailingClosureParameter(param)) {
      acceptsUnlabeledTrailingClosures.set(i);
    }

    if (param->hasExternalPropertyWrapper()) {
      propertyWrappers.set(i);
    }

    if (param->getAttrs().hasAttribute<ImplicitSelfCaptureAttr>()) {
      implicitSelfCapture.set(i);
    }

    if (param->getAttrs().hasAttribute<InheritActorContextAttr>()) {
      inheritActorContext.set(i);
    }

    if (param->getInterfaceType()->is<PackExpansionType>()) {
      variadicGenerics.set(i);
    }
  }
}

bool ParameterListInfo::hasDefaultArgument(unsigned paramIdx) const {
  return paramIdx < defaultArguments.size() ? defaultArguments[paramIdx]
      : false;
}

bool ParameterListInfo::acceptsUnlabeledTrailingClosureArgument(
    unsigned paramIdx) const {
  return paramIdx >= acceptsUnlabeledTrailingClosures.size() ||
      acceptsUnlabeledTrailingClosures[paramIdx];
}

bool ParameterListInfo::hasExternalPropertyWrapper(unsigned paramIdx) const {
  return paramIdx < propertyWrappers.size() ? propertyWrappers[paramIdx] : false;
}

bool ParameterListInfo::isImplicitSelfCapture(unsigned paramIdx) const {
  return paramIdx < implicitSelfCapture.size()
      ? implicitSelfCapture[paramIdx]
      : false;
}

bool ParameterListInfo::inheritsActorContext(unsigned paramIdx) const {
  return paramIdx < inheritActorContext.size()
      ? inheritActorContext[paramIdx]
      : false;
}

bool ParameterListInfo::anyContextualInfo() const {
  return implicitSelfCapture.any() || inheritActorContext.any();
}

bool ParameterListInfo::isVariadicGenericParameter(unsigned paramIdx) const {
  return paramIdx < variadicGenerics.size()
      ? variadicGenerics[paramIdx]
      : false;
}


/// Turn a param list into a symbolic and printable representation that does not
/// include the types, something like (_:, b:, c:)
std::string swift::getParamListAsString(ArrayRef<AnyFunctionType::Param> params) {
  std::string result = "(";

  interleave(params,
             [&](const AnyFunctionType::Param &param) {
               if (!param.getLabel().empty())
                 result += param.getLabel().str();
               else
                 result += "_";
               result += ":";
             },
             [&] { result += ", "; });

  result += ')';
  return result;
}

/// Rebuilds the given 'self' type using the given object type as the
/// replacement for the object type of self.
static AnyFunctionType::Param
rebuildSelfTypeWithObjectType(AnyFunctionType::Param selfParam,
                              Type objectTy) {
  if (selfParam.getPlainType()->getAs<MetatypeType>())
    objectTy = MetatypeType::get(objectTy);

  return selfParam.withType(objectTy);
}

/// Returns a new function type exactly like this one but with the self
/// parameter replaced. Only makes sense for members of classes.
Type TypeBase::replaceSelfParameterType(Type newSelf) {
  auto fnTy = castTo<AnyFunctionType>();

  auto params = fnTy->getParams();
  assert(params.size() == 1);
  auto selfParam = rebuildSelfTypeWithObjectType(params[0], newSelf);

  if (auto genericFnTy = getAs<GenericFunctionType>()) {
    return GenericFunctionType::get(genericFnTy->getGenericSignature(),
                                    {selfParam},
                                    fnTy->getResult(),
                                    fnTy->getExtInfo());
  }

  return FunctionType::get({selfParam},
                           fnTy->getResult(),
                           fnTy->getExtInfo());
}

/// Look through a metatype, or just return the original type if it is
/// not a metatype.
Type TypeBase::getMetatypeInstanceType() {
  if (auto existentialMetaType = getAs<ExistentialMetatypeType>())
    return existentialMetaType->getExistentialInstanceType();

  if (auto metaTy = getAs<AnyMetatypeType>())
    return metaTy->getInstanceType();

  return this;
}

using ParameterizedProtocolMap =
  llvm::DenseMap<ProtocolDecl *, ParameterizedProtocolType *>;

/// Collect the protocols in the existential type T into the given
/// vector.
static void addProtocols(Type T,
                         SmallVectorImpl<ProtocolDecl *> &Protocols,
                         ParameterizedProtocolMap &Parameterized,
                         Type &Superclass,
                         bool &HasExplicitAnyObject) {
  if (auto Proto = T->getAs<ProtocolType>()) {
    Protocols.push_back(Proto->getDecl());
    return;
  }

  if (auto PC = T->getAs<ProtocolCompositionType>()) {
    if (PC->hasExplicitAnyObject())
      HasExplicitAnyObject = true;
    for (auto P : PC->getMembers())
      addProtocols(P, Protocols, Parameterized, Superclass,
                   HasExplicitAnyObject);
    return;
  }

  if (auto PP = T->getAs<ParameterizedProtocolType>()) {
    Parameterized.insert({PP->getProtocol(), PP});
    Protocols.push_back(PP->getProtocol());
    return;
  }

  assert(isa<ClassDecl>(T->getAnyNominal()) && "Non-class, non-protocol "
         "member in protocol composition");
  assert((!Superclass || Superclass->isEqual(T)) &&
         "Should have diagnosed multiple superclasses by now");
  Superclass = T;
}

bool ProtocolType::visitAllProtocols(
                                 ArrayRef<ProtocolDecl *> protocols,
                                 llvm::function_ref<bool(ProtocolDecl *)> fn) {
  SmallVector<ProtocolDecl *, 4> stack;
  SmallPtrSet<ProtocolDecl *, 4> knownProtocols;

  // Prepopulate the stack.
  for (auto proto : protocols) {
    if (knownProtocols.insert(proto).second)
      stack.push_back(proto);
  }
  std::reverse(stack.begin(), stack.end());

  while (!stack.empty()) {
    auto proto = stack.back();
    stack.pop_back();

    // Visit this protocol.
    if (fn(proto))
      return true;

    // Add inherited protocols that we haven't seen already.
    for (auto inherited : proto->getInheritedProtocols()) {
      if (knownProtocols.insert(inherited).second)
        stack.push_back(inherited);
    }
  }

  return false;
}

static void canonicalizeProtocols(SmallVectorImpl<ProtocolDecl *> &protocols,
                                  ParameterizedProtocolMap *parameterized) {
  llvm::SmallDenseMap<ProtocolDecl *, unsigned> known;
  bool zappedAny = false;

  // Seed the stack with the protocol declarations in the original list.
  // Zap any obvious duplicates along the way.
  for (unsigned i : indices(protocols)) {
    // If we have not seen this protocol before, record its index.
    if (known.count(protocols[i]) == 0) {
      known[protocols[i]] = i;
      continue;
    }

    // We have seen this protocol before; zap this occurrence.
    protocols[i] = nullptr;
    zappedAny = true;
  }
  
  // Walk the inheritance hierarchies of all of the protocols. If we run into
  // one of the known protocols, zap it from the original list.
  for (unsigned i : indices(protocols)) {
    auto *proto = protocols[i];
    if (proto == nullptr)
      continue;

    // Add the protocols we inherited.
    proto->walkInheritedProtocols([&](ProtocolDecl *inherited) {
      if (inherited == proto)
        return TypeWalker::Action::Continue;

      auto found = known.find(inherited);
      if (found != known.end()) {
        // Don't zap protocols associated with parameterized types.
        if (parameterized && parameterized->count(inherited))
          return TypeWalker::Action::Continue;

        protocols[found->second] = nullptr;
        zappedAny = true;
      }

      return TypeWalker::Action::Continue;
    });
  }
  
  if (zappedAny) {
    protocols.erase(std::remove(protocols.begin(), protocols.end(), nullptr),
                    protocols.end());
  }

  // Sort the set of protocols by module + name, to give a stable
  // ordering.
  llvm::array_pod_sort(protocols.begin(), protocols.end(), TypeDecl::compare);
}

void ProtocolType::canonicalizeProtocols(
       SmallVectorImpl<ProtocolDecl *> &protocols) {
  return ::canonicalizeProtocols(protocols, nullptr);
}

static void
getCanonicalParams(AnyFunctionType *funcType,
                   CanGenericSignature genericSig,
                   SmallVectorImpl<AnyFunctionType::Param> &canParams) {
  auto origParams = funcType->getParams();
  for (auto param : origParams) {
    canParams.emplace_back(param.getCanonical(genericSig));
  }
}

AnyFunctionType::Param
AnyFunctionType::Param::getCanonical(CanGenericSignature genericSig) const {
  // Canonicalize the type and drop the internal label to canonicalize the
  // Param.
  return Param(getPlainType()->getReducedType(genericSig),
               getLabel(), getParameterFlags(),
               /*InternalLabel=*/Identifier());
}

CanType TypeBase::computeCanonicalType() {
  assert(!hasCanonicalTypeComputed() && "called unnecessarily");

  TypeBase *Result = nullptr;
  switch (getKind()) {
#define ALWAYS_CANONICAL_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Error:
  case TypeKind::Unresolved:
  case TypeKind::TypeVariable:
  case TypeKind::Placeholder:
  case TypeKind::BuiltinTuple:
    llvm_unreachable("these types are always canonical");

#define SUGARED_TYPE(id, parent) \
  case TypeKind::id: \
    Result = cast<id##Type>(this)-> \
             getSinglyDesugaredType()->getCanonicalType().getPointer(); \
    break;
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"

  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::Protocol: {
    auto nominalTy = cast<NominalType>(this);
    auto parentTy = nominalTy->getParent()->getCanonicalType();
    Result = NominalType::get(nominalTy->getDecl(), parentTy,
                              parentTy->getASTContext());
    break;
  }

  case TypeKind::Pack: {
    PackType *PT = cast<PackType>(this);
    assert(PT->getNumElements() != 0 && "Empty packs are always canonical");

    SmallVector<Type, 8> CanTys;
    CanTys.reserve(PT->getNumElements());
    for (Type field : PT->getElementTypes()) {
      CanTys.push_back(field->getCanonicalType());
    }

    const ASTContext &C = CanTys[0]->getASTContext();
    Result = PackType::get(C, CanTys)->castTo<PackType>();
    break;
  }

  case TypeKind::PackExpansion: {
    auto *expansion = cast<PackExpansionType>(this);
    auto patternType = expansion->getPatternType()->getCanonicalType();
    auto countType = expansion->getCountType()->getCanonicalType();
    if (auto packArchetype = dyn_cast<PackArchetypeType>(countType))
      countType = packArchetype->getReducedShape();
    Result = PackExpansionType::get(patternType, countType);
    break;
  }

  case TypeKind::PackElement: {
    auto *element = cast<PackElementType>(this);
    auto packType = element->getPackType()->getCanonicalType();
    Result = PackElementType::get(packType, element->getLevel());
    break;
  }

  case TypeKind::Tuple: {
    TupleType *TT = cast<TupleType>(this);
    assert(TT->getNumElements() != 0 && "Empty tuples are always canonical");

    SmallVector<TupleTypeElt, 8> CanElts;
    CanElts.reserve(TT->getNumElements());
    for (const TupleTypeElt &field : TT->getElements()) {
      assert(!field.getType().isNull() &&
             "Cannot get canonical type of un-typechecked TupleType!");
      CanElts.push_back(field.getWithType(field.getType()->getCanonicalType()));
    }

    const ASTContext &C = CanElts[0].getType()->getASTContext();
    Result = TupleType::get(CanElts, C)->castTo<TupleType>();
    break;
  }

  case TypeKind::GenericTypeParam: {
    GenericTypeParamType *gp = cast<GenericTypeParamType>(this);
    auto gpDecl = gp->getDecl();

    // If we haven't set a depth for this generic parameter, try to do so.
    // FIXME: This is a dreadful hack.
    if (gpDecl->getDepth() == GenericTypeParamDecl::InvalidDepth) {
      auto *dc = gpDecl->getDeclContext();
      auto *gpList = dc->getAsDecl()->getAsGenericContext()->getGenericParams();
      gpList->setDepth(dc->getGenericContextDepth());
    }

    assert(gpDecl->getDepth() != GenericTypeParamDecl::InvalidDepth &&
           "parameter hasn't been validated");
    Result =
        GenericTypeParamType::get(gpDecl->isParameterPack(), gpDecl->getDepth(),
                                  gpDecl->getIndex(), gpDecl->getASTContext());
    break;
  }

  case TypeKind::DependentMember: {
    auto dependent = cast<DependentMemberType>(this);
    auto base = dependent->getBase()->getCanonicalType();
    if (auto assocType = dependent->getAssocType())
      Result = DependentMemberType::get(base, assocType);
    else
      Result = DependentMemberType::get(base, dependent->getName());
    break;
  }

#define REF_STORAGE(Name, ...) \
  case TypeKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
  {
    auto ref = cast<ReferenceStorageType>(this);
    Type referentType = ref->getReferentType()->getCanonicalType();
    Result = ReferenceStorageType::get(referentType, ref->getOwnership(),
                                       referentType->getASTContext());
    break;
  }
  case TypeKind::LValue:
    Result = LValueType::get(cast<LValueType>(this)->getObjectType()
                               ->getCanonicalType());
    break;
  case TypeKind::InOut:
    Result = InOutType::get(getInOutObjectType()->getCanonicalType());
    break;
  case TypeKind::Function:
  case TypeKind::GenericFunction: {
    AnyFunctionType *funcTy = cast<AnyFunctionType>(this);

    CanGenericSignature genericSig;
    if (auto *genericFnTy = dyn_cast<GenericFunctionType>(this))
      genericSig = genericFnTy->getGenericSignature().getCanonicalSignature();

    // Transform the parameter and result types.
    SmallVector<AnyFunctionType::Param, 8> canParams;
    getCanonicalParams(funcTy, genericSig, canParams);
    auto resultTy = funcTy->getResult()->getReducedType(genericSig);

    Optional<ASTExtInfo> extInfo = None;
    if (funcTy->hasExtInfo())
      extInfo = funcTy->getCanonicalExtInfo(useClangTypes(resultTy));
    if (genericSig) {
      Result = GenericFunctionType::get(genericSig, canParams, resultTy,
                                        extInfo);
    } else {
      Result = FunctionType::get(canParams, resultTy, extInfo);
    }
    assert(Result->isCanonical());
    break;
  }

  case TypeKind::SILBlockStorage:
  case TypeKind::SILBox:
  case TypeKind::SILFunction:
  case TypeKind::SILToken:
  case TypeKind::SILMoveOnlyWrapped:
    llvm_unreachable("SIL-only types are always canonical!");

  case TypeKind::ProtocolComposition: {
    auto *PCT = cast<ProtocolCompositionType>(this);
    SmallVector<Type, 4> CanProtos;
    for (Type t : PCT->getMembers())
      CanProtos.push_back(t->getCanonicalType());
    assert(!CanProtos.empty() && "Non-canonical empty composition?");
    const ASTContext &C = CanProtos[0]->getASTContext();
    Type Composition = ProtocolCompositionType::get(C, CanProtos,
                                                    PCT->hasExplicitAnyObject());
    Result = Composition.getPointer();
    break;
  }
  case TypeKind::ParameterizedProtocol: {
    auto *PPT = cast<ParameterizedProtocolType>(this);
    auto Base = cast<ProtocolType>(PPT->getBaseType()->getCanonicalType());
    SmallVector<Type, 1> CanArgs;
    for (Type t : PPT->getArgs())
      CanArgs.push_back(t->getCanonicalType());
    auto &C = Base->getASTContext();
    Result = ParameterizedProtocolType::get(C, Base, CanArgs);
    break;
  }
  case TypeKind::Existential: {
    auto *existential = cast<ExistentialType>(this);
    auto constraint = existential->getConstraintType()->getCanonicalType();
    Result = ExistentialType::get(constraint).getPointer();
    break;
  }
  case TypeKind::ExistentialMetatype: {
    auto metatype = cast<ExistentialMetatypeType>(this);
    auto instanceType = metatype->getInstanceType()->getCanonicalType();
    if (metatype->hasRepresentation())
      Result = ExistentialMetatypeType::get(instanceType,
                                            metatype->getRepresentation());
    else
      Result = ExistentialMetatypeType::get(instanceType);
    break;
  }
  case TypeKind::Metatype: {
    MetatypeType *MT = cast<MetatypeType>(this);
    Type InstanceTy = MT->getInstanceType()->getCanonicalType();
    if (MT->hasRepresentation())
      Result = MetatypeType::get(InstanceTy, MT->getRepresentation());
    else
      Result = MetatypeType::get(InstanceTy);
    break;
  }
  case TypeKind::DynamicSelf: {
    DynamicSelfType *DST = cast<DynamicSelfType>(this);
    Type SelfTy = DST->getSelfType()->getCanonicalType();
    Result = DynamicSelfType::get(SelfTy, SelfTy->getASTContext());
    break;
  }
  case TypeKind::UnboundGeneric: {
    auto unbound = cast<UnboundGenericType>(this);
    Type parentTy = unbound->getParent()->getCanonicalType();
    Result = UnboundGenericType::get(unbound->getDecl(), parentTy,
                                     parentTy->getASTContext());
    break;
  }
  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct: {
    BoundGenericType *BGT = cast<BoundGenericType>(this);
    Type parentTy;
    if (BGT->getParent())
      parentTy = BGT->getParent()->getCanonicalType();
    SmallVector<Type, 4> CanGenericArgs;
    for (Type Arg : BGT->getGenericArgs())
      CanGenericArgs.push_back(Arg->getCanonicalType());
    Result = BoundGenericType::get(BGT->getDecl(), parentTy, CanGenericArgs);
    break;
  }
  }

  // Cache the canonical type for future queries.
  assert(Result && "Case not implemented!");
  return CanonicalType = CanType(Result);
}

CanType TypeBase::getReducedType(GenericSignature sig) {
  return sig.getReducedType(this);
}

CanType TypeBase::getMinimalCanonicalType(const DeclContext *useDC) const {
  const auto MinimalTy = getCanonicalType().transform([useDC](Type Ty) -> Type {
    const CanType CanTy = CanType(Ty);

    if (const auto ET = dyn_cast<ExistentialType>(CanTy)) {
      const auto PCT =
          dyn_cast<ProtocolCompositionType>(ET.getConstraintType());
      if (!PCT) {
        return CanTy;
      }

      const auto MinimalTy = PCT->getMinimalCanonicalType(useDC);
      if (MinimalTy->getClassOrBoundGenericClass()) {
        return MinimalTy;
      }

      return ExistentialType::get(MinimalTy);
    }

    if (const auto EM = dyn_cast<ExistentialMetatypeType>(CanTy)) {
      const auto PCT = dyn_cast<ProtocolCompositionType>(EM.getInstanceType());
      if (!PCT) {
        return CanTy;
      }

      const auto MinimalTy = PCT->getMinimalCanonicalType(useDC);
      if (MinimalTy->getClassOrBoundGenericClass()) {
        return MetatypeType::get(MinimalTy);
      }

      return ExistentialMetatypeType::get(MinimalTy);
    }

    if (const auto Composition = dyn_cast<ProtocolCompositionType>(CanTy)) {
      return Composition->getMinimalCanonicalType(useDC);
    }

    return CanTy;
  });

  return CanType(MinimalTy);
}

TypeBase *TypeBase::reconstituteSugar(bool Recursive) {
  auto Func = [Recursive](Type Ty) -> Type {
    if (auto boundGeneric = dyn_cast<BoundGenericType>(Ty.getPointer())) {

      auto getGenericArg = [&](unsigned i) -> Type {
        auto arg = boundGeneric->getGenericArgs()[i];
        if (Recursive)
          arg = arg->reconstituteSugar(Recursive);
        return arg;
      };

      if (boundGeneric->isArray())
        return ArraySliceType::get(getGenericArg(0));
      if (boundGeneric->isDictionary())
        return DictionaryType::get(getGenericArg(0), getGenericArg(1));
      if (boundGeneric->isOptional())
        return OptionalType::get(getGenericArg(0));
    }
    return Ty;
  };
  if (Recursive)
    return Type(this).transform(Func).getPointer();
  else
    return Func(this).getPointer();
}

TypeBase *TypeBase::getWithoutSyntaxSugar() {
  auto Func = [](Type Ty) -> Type {
    if (auto *syntaxSugarType = dyn_cast<SyntaxSugarType>(Ty.getPointer()))
      return syntaxSugarType->getSinglyDesugaredType()->getWithoutSyntaxSugar();
    return Ty;
  };
  return Type(this).transform(Func).getPointer();
}

#define TYPE(Id, Parent)
#define SUGARED_TYPE(Id, Parent) \
  static_assert(std::is_base_of<SugarType, Id##Type>::value, "Sugar mismatch");
#include "swift/AST/TypeNodes.def"

ParenType::ParenType(Type baseType, RecursiveTypeProperties properties)
    : SugarType(TypeKind::Paren, baseType, properties) {
  // In some situations (rdar://75740683) we appear to end up with ParenTypes
  // that contain a nullptr baseType. Once this is eliminated, we can remove
  // the checks for `type.isNull()` in the `DiagnosticArgumentKind::Type` case
  // of `formatDiagnosticArgument`.
  assert(baseType && "A ParenType should always wrap a non-null type");
}

Type SugarType::getSinglyDesugaredTypeSlow() {
  // Find the generic type that implements this syntactic sugar type.
  NominalTypeDecl *implDecl;

  // XXX -- If the Decl and Type class hierarchies agreed on spelling, then
  // we could handle the entire switch statement via macros.
  switch (getKind()) {
#define TYPE(Id, Parent) \
  case TypeKind::Id: llvm_unreachable("non-sugared type?");
#define SUGARED_TYPE(Id, Parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Paren:
    llvm_unreachable("parenthesis are sugar, but not syntax sugar");
  case TypeKind::TypeAlias:
    llvm_unreachable("bound type alias types always have an underlying type");
  case TypeKind::ArraySlice:
  case TypeKind::VariadicSequence:
    implDecl = Context->getArrayDecl();
    break;
  case TypeKind::Optional:
    implDecl = Context->getOptionalDecl();
    break;
  case TypeKind::Dictionary:
    implDecl = Context->getDictionaryDecl();
    break;
  }
  if (!implDecl) {
    return ErrorType::get(*Context);
  }

  Bits.SugarType.HasCachedType = true;
  if (auto Ty = dyn_cast<UnarySyntaxSugarType>(this)) {
    UnderlyingType = BoundGenericType::get(implDecl, Type(), Ty->getBaseType());
  } else if (auto Ty = dyn_cast<DictionaryType>(this)) {
    UnderlyingType = BoundGenericType::get(implDecl, Type(),
                                      { Ty->getKeyType(), Ty->getValueType() });
  } else {
    llvm_unreachable("Not UnarySyntaxSugarType or DictionaryType?");
  }

  // Record the implementation type.
  return UnderlyingType;
}

ArrayRef<Type> TypeAliasType::getDirectGenericArgs() const {
  if (!typealias->isGeneric()) return { };

  // Otherwise, the innermost replacement types are the direct
  // generic arguments.
  return getSubstitutionMap().getInnermostReplacementTypes();
}

unsigned GenericTypeParamType::getDepth() const {
  if (auto param = getDecl()) {
    return param->getDepth();
  }

  auto fixedNum = ParamOrDepthIndex.get<DepthIndexTy>();
  return (fixedNum & ~GenericTypeParamType::TYPE_SEQUENCE_BIT) >> 16;
}

unsigned GenericTypeParamType::getIndex() const {
  if (auto param = getDecl()) {
    return param->getIndex();
  }

  auto fixedNum = ParamOrDepthIndex.get<DepthIndexTy>();
  return fixedNum & 0xFFFF;
}

Identifier GenericTypeParamType::getName() const {
  // Use the declaration name if we still have that sugar.
  if (auto decl = getDecl())
    return decl->getName();
  
  // Otherwise, we're canonical. Produce an anonymous '<tau>_n_n' name.
  assert(isCanonical());
  // getASTContext() doesn't actually mutate an already-canonical type.
  auto &C = const_cast<GenericTypeParamType*>(this)->getASTContext();
  auto &names = C.CanonicalGenericTypeParamTypeNames;
  unsigned depthIndex = ParamOrDepthIndex.get<DepthIndexTy>();
  auto cached = names.find(depthIndex);
  if (cached != names.end())
    return cached->second;
  
  llvm::SmallString<10> nameBuf;
  llvm::raw_svector_ostream os(nameBuf);

  static const char *tau = u8"\u03C4_";
  
  os << tau << getDepth() << '_' << getIndex();
  Identifier name = C.getIdentifier(os.str());
  names.insert({depthIndex, name});
  return name;
}

const llvm::fltSemantics &BuiltinFloatType::getAPFloatSemantics() const {
  switch (getFPKind()) {
  case BuiltinFloatType::IEEE16:  return APFloat::IEEEhalf();
  case BuiltinFloatType::IEEE32:  return APFloat::IEEEsingle();
  case BuiltinFloatType::IEEE64:  return APFloat::IEEEdouble();
  case BuiltinFloatType::IEEE80:  return APFloat::x87DoubleExtended();
  case BuiltinFloatType::IEEE128: return APFloat::IEEEquad();
  case BuiltinFloatType::PPC128:  return APFloat::PPCDoubleDouble();
  }
  llvm::report_fatal_error("Unknown FP semantics");
}

bool TypeBase::mayBeCallable(DeclContext *dc) {
  if (is<AnyFunctionType>())
    return true;

  // Callable for construction.
  if (is<AnyMetatypeType>())
    return true;

  // Unresolved types that could potentially be callable.
  if (isPlaceholder() || is<UnresolvedType>() ||
      isTypeParameter() || isTypeVariableOrMember()) {
    return true;
  }
  // Callable nominal types.
  if (isCallAsFunctionType(dc) || hasDynamicCallableAttribute())
    return true;

  return false;
}

bool TypeBase::mayHaveSuperclass() {
  if (getClassOrBoundGenericClass())
    return true;

  if (auto archetype = getAs<ArchetypeType>())
    return (bool)archetype->requiresClass();

  return is<DynamicSelfType>();
}

bool TypeBase::satisfiesClassConstraint() {
  return mayHaveSuperclass() || isObjCExistentialType();
}

Type TypeBase::getSuperclass(bool useArchetypes) {
  auto *nominalDecl = getAnyNominal();
  auto *classDecl = dyn_cast_or_null<ClassDecl>(nominalDecl);

  // Handle some special non-class types here.
  if (!classDecl) {
    if (auto archetype = getAs<ArchetypeType>())
      return archetype->getSuperclass();

    if (auto dynamicSelfTy = getAs<DynamicSelfType>())
      return dynamicSelfTy->getSelfType();

    if (isExistentialType())
      return getExistentialLayout().getSuperclass();

    // No other types have superclasses.
    return Type();
  }

  // We have a class, so get the superclass type.
  //
  // If the derived class is generic, the superclass type may contain
  // generic type parameters from the signature of the derived class.
  Type superclassTy = classDecl->getSuperclass();

  // If there's no superclass, or it is fully concrete, we're done.
  if (!superclassTy || !superclassTy->hasTypeParameter() ||
      hasUnboundGenericType())
    return superclassTy;

  // Gather substitutions from the self type, and apply them to the original
  // superclass type to form the substituted superclass type.
  ModuleDecl *module = classDecl->getModuleContext();
  auto subMap = getContextSubstitutionMap(module,
                                          classDecl,
                                          (useArchetypes
                                           ? classDecl->getGenericEnvironment()
                                           : nullptr));
  return superclassTy.subst(subMap);
}

Type TypeBase::getRootClass(bool useArchetypes) {
  Type iterator = this;
  assert(iterator);

  while (auto superclass = iterator->getSuperclass(useArchetypes)) {
    iterator = superclass;
  }

  return iterator;
}

bool TypeBase::isExactSuperclassOf(Type ty) {
  // For there to be a superclass relationship, we must be a class, and
  // the potential subtype must be a class, superclass-bounded archetype,
  // or subclass existential involving an imported class and @objc
  // protocol.
  if (!getClassOrBoundGenericClass() ||
      !(ty->mayHaveSuperclass() ||
        (ty->isObjCExistentialType() &&
         ty->getSuperclass() &&
         ty->getSuperclass()->getAnyNominal()->hasClangNode())))
    return false;

  SmallPtrSet<ClassDecl *, 8> seen;
  do {
    if (auto *classDecl = ty->getClassOrBoundGenericClass())
      if (!seen.insert(classDecl).second)
        return false;
    if (ty->isEqual(this))
      return true;
  } while ((ty = ty->getSuperclass()));
  return false;
}

namespace {
class IsBindableVisitor
  : public TypeVisitor<IsBindableVisitor, CanType,
                     CanType, ArchetypeType *, ArrayRef<ProtocolConformanceRef>>
{
public:
  using VisitBindingCallback =
    llvm::function_ref<CanType (ArchetypeType *, CanType,
                            ArchetypeType *, ArrayRef<ProtocolConformanceRef>)>;
    
  VisitBindingCallback VisitBinding;
  
  IsBindableVisitor(VisitBindingCallback visit)
    : VisitBinding(visit)
  {}

  CanType visitArchetypeType(ArchetypeType *orig,
                             CanType subst,
                             ArchetypeType *upperBound,
                             ArrayRef<ProtocolConformanceRef> substConformances) {
    if (!subst->isTypeParameter()) {
      // Check that the archetype isn't constrained in a way that makes the
      // binding impossible.
      // For instance, if the archetype is class-constrained, and the binding
      // is not a class, it can never be bound.
      if (orig->requiresClass()
          && !subst->satisfiesClassConstraint())
        return CanType();

      if (auto superclass = orig->getSuperclass())
        if (!superclass->isBindableToSuperclassOf(subst))
          return CanType();

      // TODO: If the archetype has a superclass constraint, check that the
      // substitution is a subclass.
      
      // TODO: For private types or protocols, we might be able to definitively
      // deny bindings.
      
      // Otherwise, there may be an external retroactive conformance that
      // allows the binding.
    }
    // Let the binding succeed.
    return VisitBinding(orig, subst, upperBound, substConformances);
  }
  
  CanType visitType(TypeBase *orig, CanType subst,
                    ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    if (CanType(orig) == subst)
      return subst;
    
    return CanType();
  }
  
  CanType visitDynamicSelfType(DynamicSelfType *orig, CanType subst,
                           ArchetypeType *upperBound,
                           ArrayRef<ProtocolConformanceRef> substConformances) {
    // A "dynamic self" type can be bound to another dynamic self type, or the
    // non-dynamic base class type.
    if (auto dynSubst = dyn_cast<DynamicSelfType>(subst)) {
      if (auto newBase = visit(orig->getSelfType(), dynSubst.getSelfType(),
                               upperBound, substConformances)) {
        return CanDynamicSelfType::get(newBase, orig->getASTContext())
                                 ->getCanonicalType();
      }
      return CanType();
    }
    
    if (auto newNonDynBase = visit(orig->getSelfType(), subst,
                                   upperBound, substConformances)) {
      return newNonDynBase;
    }
    return CanType();
  }
  
  /// Handle a nominal type with generic parameters anywhere in its context.
  /// \c origType and \c substType must already have been established to be
  /// instantiations of the same \c NominalTypeDecl.
  CanType handleGenericNominalType(NominalTypeDecl *decl,
                           CanType origType,
                           CanType substType,
                           ArchetypeType *upperBound,
                           ArrayRef<ProtocolConformanceRef> substConformances) {
    assert(origType->getAnyNominal() == decl
           && substType->getAnyNominal() == decl);
    
    LLVM_DEBUG(llvm::dbgs() << "\n---\nTesting bindability of:\n";
               origType->print(llvm::dbgs());
               llvm::dbgs() << "\nto subst type:\n";
               substType->print(llvm::dbgs());
               if (upperBound) {
                 llvm::dbgs() << "\nwith upper bound archetype:\n";
                 upperBound->print(llvm::dbgs());
               });
    
    auto *moduleDecl = decl->getParentModule();
    auto origSubMap = origType->getContextSubstitutionMap(
        moduleDecl, decl, decl->getGenericEnvironment());
    auto substSubMap = substType->getContextSubstitutionMap(
        moduleDecl, decl, decl->getGenericEnvironment());

    auto genericSig = decl->getGenericSignature();
    
    SmallVector<Type, 4> newParams;
    llvm::DenseMap<SubstitutableType *, Type> newParamsMap;
    bool didChange = false;
    
    // The upper bounds for the nominal type's arguments may depend on the
    // upper bounds imposed on the nominal type itself, if conditional
    // conformances are involved. For instance, if we're looking at:
    //
    // protocol P {}
    // struct A<T: P> {}
    // struct B<U> {}
    // extension B: P where U: P {}
    //
    // and visiting `A<B<V>>`, then `B<V>` has an upper bound of `_: P` because
    // of A's generic type constraint. In order to stay within this upper bound,
    // `V` must also have `_: P` as its upper bound, in order to satisfy the
    // constraint on `B<V>`. To handle this correctly, ingest requirements
    // from any extension declarations providing conformances required by the
    // upper bound.
    auto upperBoundGenericSig = genericSig;
    auto upperBoundSubstMap = substSubMap;
    
    LLVM_DEBUG(llvm::dbgs() << "\nNominal type generic signature:\n";
               upperBoundGenericSig.print(llvm::dbgs());
               upperBoundSubstMap.dump(llvm::dbgs()));
    
    if (upperBound && !upperBound->getConformsTo().empty()) {
      // Start with the set of requirements from the nominal type.
      SmallVector<Requirement, 4> addedRequirements;
      
      llvm::DenseMap<std::pair<CanType, ProtocolDecl*>, ProtocolConformanceRef> addedConformances;
      
      for (auto proto : upperBound->getConformsTo()) {
        // Find the DeclContext providing the conformance for the type.
        auto nomConformance = moduleDecl->lookupConformance(
            substType, proto, /*allowMissing=*/true);
        if (!nomConformance)
          return CanType();
        if (nomConformance.isAbstract())
          continue;
        auto conformanceContext = nomConformance.getConcrete()->getDeclContext();
        
        LLVM_DEBUG(llvm::dbgs() << "\nFound extension conformance for "
                                << proto->getName()
                                << " in context:\n";
                   conformanceContext->printContext(llvm::dbgs()));
        
        auto conformanceSig = conformanceContext->getGenericSignatureOfContext();
        // TODO: Conformance on generalized generic extensions could conceivably
        // have totally different generic signatures.
        assert(conformanceSig.getGenericParams().size()
                 == genericSig.getGenericParams().size()
               && "generalized generic extension not handled properly");
        
        // Collect requirements from the conformance not satisfied by the
        // original declaration.
        for (auto reqt : conformanceSig.requirementsNotSatisfiedBy(genericSig)) {
          LLVM_DEBUG(llvm::dbgs() << "\n- adds requirement\n";
                     reqt.dump(llvm::dbgs()));
          
          addedRequirements.push_back(reqt);
          
          // Collect the matching conformance for the substituted type.
          // TODO: Look this up using the upperBoundSubstConformances
          if (reqt.getKind() == RequirementKind::Conformance) {
            auto proto = reqt.getSecondType()->castTo<ProtocolType>()->getDecl();
            auto substTy = reqt.getFirstType().subst(substSubMap);
            ProtocolConformanceRef substConformance;
            if (substTy->isTypeParameter()) {
              substConformance = ProtocolConformanceRef(proto);
            } else {
              substConformance = moduleDecl->lookupConformance(
                  substTy, proto, /*allowMissing=*/true);
            }
            
            LLVM_DEBUG(llvm::dbgs() << "\n` adds conformance for subst type\n";
                       substTy->print(llvm::dbgs());
                       substConformance.dump(llvm::dbgs()));
            
            auto key = std::make_pair(reqt.getFirstType()->getCanonicalType(),
                                      proto);
            
            addedConformances.insert({key, substConformance});
          }
        }
      }
      
      // Build the generic signature with the additional collected requirements.
      if (!addedRequirements.empty()) {
        upperBoundGenericSig = buildGenericSignature(decl->getASTContext(),
                                                     upperBoundGenericSig,
                                                     /*genericParams=*/{ },
                                                     std::move(addedRequirements));

        upperBoundSubstMap = SubstitutionMap::get(upperBoundGenericSig,
          [&](SubstitutableType *t) -> Type {
            // Type substitutions remain the same as the original substitution
            // map.
            return Type(t).subst(substSubMap);
          },
          [&](CanType dependentType,
              Type conformingReplacementType,
              ProtocolDecl *conformedProtocol) -> ProtocolConformanceRef {
            // Check whether we added this conformance.
            auto added = addedConformances.find({dependentType,
                                                 conformedProtocol});
            if (added != addedConformances.end()) {
              return added->second;
            }
            // Otherwise, use the conformance from the original map.
            
            return substSubMap.lookupConformance(dependentType, conformedProtocol);
          });
        
        LLVM_DEBUG(llvm::dbgs() << "\nGeneric signature with conditional reqts:\n";
                   upperBoundGenericSig.print(llvm::dbgs());
                   upperBoundSubstMap.dump(llvm::dbgs()));
      }
    }
    
    auto upperBoundGenericEnv = upperBoundGenericSig.getGenericEnvironment();
    
    for (auto gpTy : upperBoundGenericSig.getGenericParams()) {
      auto gp = gpTy->getCanonicalType();
      
      auto orig = gp.subst(origSubMap)->getCanonicalType();
      auto subst = gp.subst(substSubMap)->getCanonicalType();
      
      // The new type is upper-bounded by the constraints the nominal type
      // requires. The substitution operation may be interested in transforming
      // the substituted type's conformances to these protocols.
      //
      // FIXME: The upperBound on the nominal type itself may impose additional
      // requirements on the type parameters due to conditional conformances.
      // These are currently not considered, leading to invalid generic signatures
      // built during SILGen.
      auto paramUpperBound =
        GenericEnvironment::mapTypeIntoContext(upperBoundGenericEnv, gp)
          ->getAs<ArchetypeType>();
      SmallVector<ProtocolConformanceRef, 4> paramSubstConformances;
      if (paramUpperBound) {
        for (auto proto : paramUpperBound->getConformsTo()) {
          auto conformance = upperBoundSubstMap.lookupConformance(gp, proto);
          if (!conformance)
            return CanType();
          paramSubstConformances.push_back(conformance);
        }
      }
      
      auto newParam = visit(orig, subst, paramUpperBound,
                            paramSubstConformances);
      if (!newParam)
        return CanType();
      
      newParams.push_back(newParam);
      newParamsMap.insert({gp->castTo<GenericTypeParamType>(), newParam});
      didChange |= (newParam != subst);
    }
    
    SmallVector<ProtocolConformanceRef, 4> newConformances;

    // Collect conformances for the new substitutions, and verify that they don't
    // invalidate the binding to the original type.
    for (const auto &req : genericSig.getRequirements()) {
      if (req.getKind() != RequirementKind::Conformance) continue;

      auto canTy = req.getFirstType()->getCanonicalType();
      
      // Verify the generic requirements, if the subst type is bound to
      // concrete type.
      auto *proto = req.getProtocolDecl();
      if (!canTy.subst(substSubMap)->isTypeParameter()) {
        auto origConf = origSubMap.lookupConformance(canTy, proto);
        auto substConf = substSubMap.lookupConformance(canTy, proto);

        if (origConf.isConcrete()) {
          if (!substConf.isConcrete())
            return CanType();
          if (origConf.getConcrete()->getRootConformance()
                != substConf.getConcrete()->getRootConformance())
            return CanType();
        }
      }
      
      // Gather the conformances for the new binding type, if the type changed.
      if (didChange) {
        auto newSubstTy = req.getFirstType().subst(
          QueryTypeSubstitutionMap{newParamsMap},
          LookUpConformanceInModule(moduleDecl));
        
        if (newSubstTy->isTypeParameter()) {
          newConformances.push_back(ProtocolConformanceRef(proto));
        } else {
          auto newConformance
            = moduleDecl->lookupConformance(
                  newSubstTy, proto, /*allowMissing=*/true);
          if (!newConformance)
            return CanType();
          newConformances.push_back(newConformance);
        }
      }
    }

    if (!didChange)
      return substType;
    
    // Build the new substituted generic type.
    auto newSubMap = SubstitutionMap::get(genericSig,
                                          newParams,
                                          newConformances);
    return decl->getDeclaredInterfaceType().subst(newSubMap)
               ->getCanonicalType();
  }
  
  CanType visitNominalType(NominalType *nom, CanType subst,
                           ArchetypeType* upperBound,
                           ArrayRef<ProtocolConformanceRef> substConformances) {
    if (auto substNom = dyn_cast<NominalType>(subst)) {
      auto nomDecl = nom->getDecl();
      if (nom->getDecl() != substNom->getDecl())
        return CanType();
      
      // If the type is generic (because it's a nested type in a generic context),
      // process the generic type bindings.
      if (!isa<ProtocolDecl>(nomDecl) && nomDecl->isGenericContext()) {
        return handleGenericNominalType(nomDecl, CanType(nom), subst,
                                        upperBound, substConformances);
      }
      // Otherwise, the nongeneric nominal types trivially match.
      return subst;
    }
    return CanType();
  }
  
  CanType visitAnyMetatypeType(AnyMetatypeType *meta, CanType subst,
                             ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    if (auto substMeta = dyn_cast<AnyMetatypeType>(subst)) {
      if (substMeta->getKind() != meta->getKind())
        return CanType();
      
      auto substInstance = visit(meta->getInstanceType()->getCanonicalType(),
                               substMeta->getInstanceType()->getCanonicalType(),
                               nullptr, {});
      if (!substInstance)
        return CanType();
      
      if (substInstance == substMeta.getInstanceType())
        return subst;
      
      return isa<ExistentialMetatypeType>(substMeta)
        ? CanType(CanExistentialMetatypeType::get(substInstance))
        : CanType(CanMetatypeType::get(substInstance));
    }
    return CanType();
  }
  
  CanType visitTupleType(TupleType *tuple, CanType subst,
                         ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    if (auto substTuple = dyn_cast<TupleType>(subst)) {
      // Tuple elements must match.
      if (tuple->getNumElements() != substTuple->getNumElements())
        return CanType();
      // TODO: Label reordering?
      SmallVector<TupleTypeElt, 4> newElements;
      bool didChange = false;
      for (unsigned i : indices(tuple->getElements())) {
        auto elt = tuple->getElements()[i],
             substElt = substTuple->getElements()[i];
        if (elt.getName() != substElt.getName())
          return CanType();
        auto newElt = visit(elt.getType(),
                            substElt.getType()->getCanonicalType(),
                            nullptr, {});
        if (!newElt)
          return CanType();
        newElements.push_back(substElt.getWithType(newElt));
        didChange = didChange | (newElt != CanType(substElt.getType()));
      }
      if (!didChange)
        return subst;
      return TupleType::get(newElements, subst->getASTContext())
        ->getCanonicalType();
    }
    return CanType();
  }
  
  CanType visitDependentMemberType(DependentMemberType *dt, CanType subst,
                             ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    return subst;
  }
  CanType visitGenericTypeParamType(GenericTypeParamType *dt, CanType subst,
                             ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    return subst;
  }
  
  CanType visitFunctionType(FunctionType *func, CanType subst,
                            ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    if (auto substFunc = dyn_cast<FunctionType>(subst)) {
      if (!func->hasSameExtInfoAs(substFunc))
        return CanType();
      
      if (func->getParams().size() != substFunc->getParams().size())
        return CanType();

      SmallVector<AnyFunctionType::Param, 4> newParams;
      bool didChange = false;
      for (unsigned i : indices(func->getParams())) {
        auto param = func->getParams()[i];
        auto substParam = substFunc.getParams()[i];
        if (param.getParameterFlags() != substParam.getParameterFlags())
          return CanType();
        
        auto newParamTy =
          visit(param.getPlainType(), substParam.getPlainType(), nullptr, {});
        if (!newParamTy)
          return CanType();
        
        newParams.push_back(substParam.withType(newParamTy));
        didChange = didChange | (newParamTy != substParam.getPlainType());
      }
      
      auto newReturn = visit(func->getResult()->getCanonicalType(),
                             substFunc->getResult()->getCanonicalType(),
                             nullptr, {});
      if (!newReturn)
        return CanType();
      if (!didChange && newReturn == substFunc.getResult())
        return subst;
      return FunctionType::get(newParams, newReturn, func->getExtInfo())
        ->getCanonicalType();
    }
    return CanType();
  }
  
  CanType visitSILFunctionType(SILFunctionType *func, CanType subst,
                             ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    if (auto substFunc = dyn_cast<SILFunctionType>(subst)) {
      if (!func->hasSameExtInfoAs(substFunc))
        return CanType();

      if (func->getInvocationGenericSignature()
          || substFunc->getInvocationGenericSignature()) {
        auto sig = func->getInvocationGenericSignature();
        if (sig != substFunc->getInvocationGenericSignature())
          return CanType();

        auto origSubs = func->getPatternSubstitutions();
        auto substSubs = substFunc->getPatternSubstitutions();

        if ((bool) origSubs != (bool) substSubs)
          return CanType();

        for (unsigned i : indices(origSubs.getReplacementTypes())) {
          auto origType =
            origSubs.getReplacementTypes()[i]->getReducedType(sig);
          auto substType =
            substSubs.getReplacementTypes()[i]->getReducedType(sig);

          auto newType = visit(origType, substType, nullptr, {});

          if (!newType)
            return CanType();

          // We can test SILFunctionTypes for bindability, but we can't
          // transform them.
          assert(newType == substType
                 && "cannot transform SILFunctionTypes");
        }
      }

      // Compare substituted function types.
      if (func->getPatternGenericSignature()
          || substFunc->getPatternGenericSignature()) {
        if (func->getPatternGenericSignature()
              != substFunc->getPatternGenericSignature())
          return CanType();
        
        auto sig = func->getPatternGenericSignature();
        
        auto origSubs = func->getPatternSubstitutions();
        auto substSubs = substFunc->getPatternSubstitutions();
        
        for (unsigned i : indices(origSubs.getReplacementTypes())) {
          auto origType =
            origSubs.getReplacementTypes()[i]->getReducedType(sig);
          auto substType =
            substSubs.getReplacementTypes()[i]->getReducedType(sig);
          
          auto newType = visit(origType, substType, nullptr, {});
          
          if (!newType)
            return CanType();
          
          // We can test SILFunctionTypes for bindability, but we can't
          // transform them.
          assert(newType == substType
                 && "cannot transform SILFunctionTypes");
        }
        
        return subst;
      }
      
      if (func->getParameters().size() != substFunc->getParameters().size())
        return CanType();
      if (func->getResults().size() != substFunc->getResults().size())
        return CanType();
      
      for (unsigned i : indices(func->getParameters())) {
        if (func->getParameters()[i].getConvention()
              != substFunc->getParameters()[i].getConvention())
          return CanType();
        
        auto origParam = func->getParameters()[i].getInterfaceType();
        auto substParam = substFunc->getParameters()[i].getInterfaceType();
        auto newParam = visit(origParam, substParam, nullptr, {});
        if (!newParam)
          return CanType();
        
        // We can test SILFunctionTypes for bindability, but we can't
        // transform them.
        assert(newParam == substParam
               && "cannot transform SILFunctionTypes");
      }

      for (unsigned i : indices(func->getResults())) {
        if (func->getResults()[i].getConvention()
            != substFunc->getResults()[i].getConvention())
          return CanType();

        auto origResult = func->getResults()[i].getInterfaceType();
        auto substResult = substFunc->getResults()[i].getInterfaceType();
        auto newResult = visit(origResult, substResult, nullptr, {});
        if (!newResult)
          return CanType();

        // We can test SILFunctionTypes for bindability, but we can't
        // transform them.
        assert(newResult == substResult
               && "cannot transform SILFunctionTypes");
      }
      
      return subst;
    }
    
    return CanType();
  }
  
  CanType visitBoundGenericType(BoundGenericType *bgt, CanType subst,
                          ArchetypeType *upperBound,
                          ArrayRef<ProtocolConformanceRef> substConformances) {
    auto substBGT = dyn_cast<BoundGenericType>(subst);
    if (!substBGT)
      return CanType();
    
    if (bgt->getDecl() != substBGT->getDecl())
      return CanType();

    auto *decl = bgt->getDecl();

    return handleGenericNominalType(decl, CanType(bgt), subst,
                                    upperBound, substConformances);
  }
};
}

CanType TypeBase::substituteBindingsTo(Type ty,
                             IsBindableVisitor::VisitBindingCallback substFn) {
  return IsBindableVisitor(substFn)
    .visit(getCanonicalType(), ty->getCanonicalType(), nullptr, {});
}

bool TypeBase::isBindableTo(Type ty) {
  // Keep a mapping of archetype bindings, so we reject types that try to bind
  // different types to the same type parameter, e.g.
  // `Foo<T,T>`.isBindableTo(`Foo<Int, String>`).
  llvm::DenseMap<ArchetypeType *, CanType> Bindings;
  
  return !substituteBindingsTo(ty,
    [&](ArchetypeType *archetype, CanType binding,
        ArchetypeType*, ArrayRef<ProtocolConformanceRef>) -> CanType {
      // If we already bound this archetype, make sure the new binding candidate
      // is the same type.
      auto bound = Bindings.find(archetype);
      if (bound != Bindings.end()) {
        if (!bound->second->isEqual(binding))
          return CanType();
      } else {
        Bindings.insert({archetype, binding});
      }
      return binding;
    })
    .isNull();
}

bool TypeBase::isBindableToSuperclassOf(Type ty) {
  // Do an exact match if no archetypes are involved.
  if (!hasArchetype())
    return isExactSuperclassOf(ty);
  
  // For there to be a superclass relationship,
  // the potential subtype must be a class or superclass-bounded archetype.
  if (!ty->mayHaveSuperclass())
    return false;

  SmallPtrSet<ClassDecl *, 8> seen;
  do {
    if (auto *classDecl = ty->getClassOrBoundGenericClass())
      if (!seen.insert(classDecl).second)
        return false;
    if (isBindableTo(ty))
      return true;
  } while ((ty = ty->getSuperclass()));
  return false;
}

static bool isBridgeableObjectType(CanType type) {
  // Metatypes aren't always trivially bridgeable unless they've been
  // SIL-lowered to have an @objc representation.
  if (auto metaTy = dyn_cast<AnyMetatypeType>(type)) {
    if (!metaTy->hasRepresentation())
      return false;

    if (metaTy->getRepresentation() != MetatypeRepresentation::ObjC)
      return false;

    if (auto metatype = dyn_cast<MetatypeType>(type)) {
      CanType instanceType = metatype.getInstanceType();
      return instanceType->mayHaveSuperclass();
    }

    // @objc protocol metatypes.
    if (auto metatype = dyn_cast<ExistentialMetatypeType>(type)) {
      return metatype.getInstanceType()->isObjCExistentialType();
    }
  }

  // Classes and class-constrained archetypes.
  if (type->mayHaveSuperclass())
    return true;

  // Pure-ObjC existential types.
  if (type.isObjCExistentialType()) {
    return true;
  }

  // Blocks.
  if (auto fnType = dyn_cast<AnyFunctionType>(type)) {
    return fnType->getRepresentation()
      == AnyFunctionType::Representation::Block;
  } else if (auto fnType = dyn_cast<SILFunctionType>(type)) {
    return fnType->getRepresentation()
      == SILFunctionType::Representation::Block;
  }

  return false;
}

static bool hasRetainablePointerRepresentation(CanType type) {
  // Look through one level of Optional<> or ImplicitlyUnwrappedOptional<>.
  if (auto objType = type.getOptionalObjectType()) {
    type = objType;
  }

  return isBridgeableObjectType(type);
}

bool TypeBase::hasRetainablePointerRepresentation() {
  return ::hasRetainablePointerRepresentation(getCanonicalType());
}

bool TypeBase::isBridgeableObjectType() {
  return ::isBridgeableObjectType(getCanonicalType());
}

bool TypeBase::isPotentiallyBridgedValueType() {
  // struct and enum types
  if (auto nominal = getAnyNominal()) {
    if (isa<StructDecl>(nominal) || isa<EnumDecl>(nominal))
      return true;
  }

  // Error existentials.
  if (isExistentialWithError()) return true;

  // Archetypes that aren't class-constrained.
  if (auto archetype = getAs<ArchetypeType>())
    return !archetype->requiresClass();

  return false;
}

/// Determine whether this is a representable Objective-C object type.
static ForeignRepresentableKind
getObjCObjectRepresentable(Type type, const DeclContext *dc) {
  // @objc metatypes are representable when their instance type is.
  if (auto metatype = type->getAs<AnyMetatypeType>()) {
    auto instanceType = metatype->getInstanceType();

    // Consider metatype of any existential type as not Objective-C
    // representable.
    if (metatype->is<MetatypeType>() && instanceType->isAnyExistentialType())
      return ForeignRepresentableKind::None;

    // If the instance type is not representable verbatim, the metatype is not
    // representable.
    if (getObjCObjectRepresentable(instanceType, dc)
          != ForeignRepresentableKind::Object)
      return ForeignRepresentableKind::None;

    // Objective-C metatypes are trivially representable.
    if (metatype->hasRepresentation() &&
        metatype->getRepresentation() == MetatypeRepresentation::ObjC)
      return ForeignRepresentableKind::Object;

    // All other metatypes are bridged.
    return ForeignRepresentableKind::Bridged;
  }

  // DynamicSelfType is always representable in Objective-C, even if
  // the class is not @objc, allowing Self-returning methods to witness
  // @objc protocol requirements.
  if (type->is<DynamicSelfType>())
    return ForeignRepresentableKind::Object;

  // @objc classes.
  if (auto classDecl = type->getClassOrBoundGenericClass()) {
    if (classDecl->isObjC())
      return ForeignRepresentableKind::Object;
  }

  // Objective-C existential types are trivially representable if
  // they don't have a superclass constraint, or if the superclass
  // constraint is an @objc class.
  if (type->isExistentialType()) {
    auto layout = type->getExistentialLayout();
    if (layout.isObjC() &&
        (!layout.explicitSuperclass ||
         getObjCObjectRepresentable(layout.explicitSuperclass, dc) ==
           ForeignRepresentableKind::Object))
      return ForeignRepresentableKind::Object;
  }

  // Existentials consisting of only marker protocols can be bridged to id.
  if (type->isMarkerExistential()) {
    return ForeignRepresentableKind::Bridged;
  }

  if (auto tyContext = dc->getInnermostTypeContext()) {
    // Class-constrained generic parameters, from ObjC generic classes.
    if (auto *classDecl = tyContext->getSelfClassDecl())
      if (classDecl->hasClangNode())
        if (auto archetype = type->getAs<ArchetypeType>())
          if (archetype->requiresClass())
            return ForeignRepresentableKind::Object;

    // The 'Self' parameter in a protocol is representable in Objective-C.
    if (auto *protoDecl = dyn_cast<ProtocolDecl>(tyContext))
      if (type->isEqual(dc->mapTypeIntoContext(protoDecl->getSelfInterfaceType())))
        return ForeignRepresentableKind::Object;
  }

  return ForeignRepresentableKind::None;
}

/// Determine the foreign representation of this type.
///
/// This function determines when and how a particular type is mapped
/// into a foreign language. Any changes to the logic here also need
/// to be reflected in PrintAsClang, so that the Swift type will be
/// properly printed for (Objective-)C and in SIL's bridging logic.
static std::pair<ForeignRepresentableKind, ProtocolConformance *>
getForeignRepresentable(Type type, ForeignLanguage language,
                        const DeclContext *dc) {
  // Local function that simply produces a failing result.
  auto failure = []() -> std::pair<ForeignRepresentableKind,
                                   ProtocolConformance *> {
    return { ForeignRepresentableKind::None, nullptr };
  };
  
  // If type has an error let's fail early.
  if (type->hasError())
    return failure();

  // Look through one level of optional type, but remember that we did.
  bool wasOptional = false;
  if (auto valueType = type->getOptionalObjectType()) {
    type = valueType;
    wasOptional = true;
  }

  if (auto existential = type->getAs<ExistentialType>())
    type = existential->getConstraintType();

  // Objective-C object types, including metatypes.
  if (language == ForeignLanguage::ObjectiveC) {
    auto representable = getObjCObjectRepresentable(type, dc);
    if (representable != ForeignRepresentableKind::None)
      return { representable, nullptr };
  }

  // Function types.
  if (auto functionType = type->getAs<FunctionType>()) {
    // Cannot handle async or throwing functions.
    if (functionType->getExtInfo().isAsync() ||
        functionType->getExtInfo().isThrowing())
      return failure();

    // Whether we have found any types that are bridged.
    bool anyBridged = false;
    bool anyStaticBridged = false;

    // Local function to combine the result of a recursive invocation.
    //
    // Returns true on failure.
    auto recurse = [&](Type componentType) -> bool {
      switch (componentType->getForeignRepresentableIn(language, dc).first) {
      case ForeignRepresentableKind::None:
        return true;

      case ForeignRepresentableKind::Trivial:
      case ForeignRepresentableKind::Object:
        return false;

      case ForeignRepresentableKind::Bridged:
      case ForeignRepresentableKind::BridgedError:
        anyBridged = true;
        return false;

      case ForeignRepresentableKind::StaticBridged:
        anyStaticBridged = true;
        return false;
      }

      llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
    };

    // Check the representation of the function type.
    bool isBlock = false;
    switch (functionType->getRepresentation()) {
    case AnyFunctionType::Representation::Thin:
      return failure();

    case AnyFunctionType::Representation::Swift:
      anyStaticBridged = true;
      break;

    case AnyFunctionType::Representation::Block:
      isBlock = true;
      break;

    case AnyFunctionType::Representation::CFunctionPointer:
      break;
    }

    auto success = [](bool anyStaticBridged,
                      bool anyBridged,
                      bool isBlock) -> std::pair<ForeignRepresentableKind,
                                                 ProtocolConformance *> {
      // We have something representable; check how it is representable.
      return { anyStaticBridged ? ForeignRepresentableKind::StaticBridged
                   : anyBridged ? ForeignRepresentableKind::Bridged
                   : isBlock    ? ForeignRepresentableKind::Object
                   : ForeignRepresentableKind::Trivial,
                   nullptr };
    };

    // Look at the result type.
    Type resultType = functionType->getResult();
    if (!resultType->isVoid() && recurse(resultType))
      return failure();

    // Look at the input params.
    for (const auto &param : functionType->getParams()) {
      if (param.isVariadic())
        return failure();
      if (recurse(param.getOldType()))
        return failure();
    }

    return success(anyStaticBridged, anyBridged, isBlock);
  }

  // Give special dispensation to builtin types for testing purposes.
  if (type->is<BuiltinType>())
    return { ForeignRepresentableKind::Trivial, nullptr };

  auto nominal = type->getAnyNominal();
  if (!nominal) return failure();

  ASTContext &ctx = nominal->getASTContext();

  // Unmanaged<T> can be trivially represented in Objective-C if T
  // is trivially represented in Objective-C.
  if (language == ForeignLanguage::ObjectiveC && type->isUnmanaged()) {
    auto boundGenericType = type->getAs<BoundGenericType>();

    // Note: works around a broken Unmanaged<> definition.
    if (!boundGenericType || boundGenericType->getGenericArgs().size() != 1)
      return failure();
    
    auto typeArgument = boundGenericType->getGenericArgs()[0];
    if (typeArgument->isTriviallyRepresentableIn(language, dc))
      return { ForeignRepresentableKind::Trivial, nullptr };

    return failure();
  }

  // If the type was imported from Clang, check whether it is
  // representable in the requested language.
  if (nominal->hasClangNode() || nominal->isObjC()) {
    switch (language) {
    case ForeignLanguage::C:
      // Imported classes and protocols are not representable in C.
      if (isa<ClassDecl>(nominal) || isa<ProtocolDecl>(nominal))
        return failure();
      LLVM_FALLTHROUGH;

    case ForeignLanguage::ObjectiveC:
      if (isa<StructDecl>(nominal) || isa<EnumDecl>(nominal)) {
        // Optional structs are not representable in (Objective-)C if they
        // originally came from C, whether or not they are bridged, unless they
        // came from swift_newtype. If they are defined in Swift, they are only
        // representable if they are bridged (checked below).
        if (wasOptional) {
          if (nominal->hasClangNode()) {
            Type underlyingType =
                nominal->getDeclaredInterfaceType()
                       ->getSwiftNewtypeUnderlyingType();
            if (underlyingType) {
              return getForeignRepresentable(OptionalType::get(underlyingType),
                                             language, dc);
            }
            return failure();
          }
          break;
        }
      }

      return { ForeignRepresentableKind::Trivial, nullptr };
    }
  }

  // Pointers may be representable in ObjC.
  PointerTypeKind pointerKind;
  if (auto pointerElt = type->getAnyPointerElementType(pointerKind)) {
    switch (pointerKind) {
    case PTK_UnsafeMutableRawPointer:
    case PTK_UnsafeRawPointer:
    case PTK_UnsafeMutablePointer:
    case PTK_UnsafePointer:
      // An UnsafeMutablePointer<T> or UnsafePointer<T> is
      // representable if T is trivially representable or Void.
      if (pointerElt->isVoid() ||
          pointerElt->isTriviallyRepresentableIn(language, dc))
        return { ForeignRepresentableKind::Trivial, nullptr };

      return failure();

    case PTK_AutoreleasingUnsafeMutablePointer:
      // An AutoreleasingUnsafeMutablePointer<T> is representable in
      // Objective-C if T is a representable object type in
      // Objective-C.

      // Allow one level of optionality.
      if (auto objectType = pointerElt->getOptionalObjectType())
        pointerElt = objectType;

      if (language == ForeignLanguage::ObjectiveC &&
          getObjCObjectRepresentable(pointerElt, dc)
            != ForeignRepresentableKind::None)
        return { ForeignRepresentableKind::Trivial, nullptr };

      return failure();
    }
  }

  // Determine whether this nominal type is known to be representable
  // in this foreign language.
  auto result = ctx.getForeignRepresentationInfo(nominal, language, dc);
  if (result.getKind() == ForeignRepresentableKind::None) return failure();

  if (wasOptional && !result.isRepresentableAsOptional())
    return failure();
  
  // If our nominal type has type arguments, make sure they are
  // representable as well. Because type arguments are not actually
  // translated separately, whether they are trivially representable
  // or bridged representable doesn't impact our final result.
  if (auto boundGenericType = type->getAs<BoundGenericType>()) {
    for (auto typeArg : boundGenericType->getGenericArgs()) {
      // Type arguments cannot be optional.
      if (typeArg->getOptionalObjectType())
        return failure();

      // A type parameter can appear here when we're looking at an
      // extension of an @objc imported class.
      //
      // FIXME: Make this more principled.
      if (typeArg->isTypeParameter())
        continue;

      // And must be representable either an object or bridged.
      switch (typeArg->getForeignRepresentableIn(language, dc).first) {
      case ForeignRepresentableKind::None:
      case ForeignRepresentableKind::StaticBridged:
        return failure();

      case ForeignRepresentableKind::Trivial:
        // FIXME: We allow trivially-representable cases that also
        // conform to _ObjectiveCBridgeable. This may not be desirable
        // and should be re-evaluated.
        if (auto nominal = typeArg->getAnyNominal()) {
          if (auto objcBridgeable
                = ctx.getProtocol(KnownProtocolKind::ObjectiveCBridgeable)) {
            SmallVector<ProtocolConformance *, 1> conformances;
            if (nominal->lookupConformance(objcBridgeable, conformances))
              break;
          }
        }
        
        return failure();
        
      case ForeignRepresentableKind::Object:
      case ForeignRepresentableKind::Bridged:
      case ForeignRepresentableKind::BridgedError:
        break;
      }
    }
    
    // Specialize the conformance we were given for the type we're testing.
    if (result.getKind() == ForeignRepresentableKind::Bridged
        && !result.getConformance()->getType()->isEqual(type)) {
      auto specialized = type->getASTContext()
        .getSpecializedConformance(type,
             cast<RootProtocolConformance>(result.getConformance()),
             boundGenericType->getContextSubstitutionMap(dc->getParentModule(),
                                                 boundGenericType->getDecl()));
      result = ForeignRepresentationInfo::forBridged(specialized);
    }
  }

  return { result.getKind(), result.getConformance() };
}

std::pair<ForeignRepresentableKind, ProtocolConformance *>
TypeBase::getForeignRepresentableIn(ForeignLanguage language,
                                    const DeclContext *dc) {
  return getForeignRepresentable(Type(this), language, dc);
}

bool TypeBase::isRepresentableIn(ForeignLanguage language,
                                 const DeclContext *dc) {
  switch (getForeignRepresentableIn(language, dc).first) {
  case ForeignRepresentableKind::None:
    return false;

  case ForeignRepresentableKind::Trivial:
  case ForeignRepresentableKind::Object:
  case ForeignRepresentableKind::Bridged:
  case ForeignRepresentableKind::BridgedError:
  case ForeignRepresentableKind::StaticBridged:
    return true;
  }

  llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
}

bool TypeBase::isTriviallyRepresentableIn(ForeignLanguage language,
                                          const DeclContext *dc) {
  switch (getForeignRepresentableIn(language, dc).first) {
  case ForeignRepresentableKind::None:
  case ForeignRepresentableKind::Bridged:
  case ForeignRepresentableKind::BridgedError:
  case ForeignRepresentableKind::StaticBridged:
    return false;

  case ForeignRepresentableKind::Trivial:
  case ForeignRepresentableKind::Object:
    return true;
  }

  llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
}

static bool isABICompatibleEvenAddingOptional(CanType t1, CanType t2) {
  // Classes, class-constrained archetypes, and pure-ObjC existential
  // types all have single retainable pointer representation; optionality
  // change is allowed.
  // NOTE: This doesn't use isAnyClassReferenceType because we want it to
  // return a conservative answer for dependent types. There's probably
  // a better answer here, though.
  if ((t1->mayHaveSuperclass() || t1->isObjCExistentialType()) &&
      (t2->mayHaveSuperclass() || t2->isObjCExistentialType())) {
    return true;
  }

  // Class metatypes are ABI-compatible even under optionality change.
  if (auto metaTy1 = dyn_cast<MetatypeType>(t1)) {
    if (auto metaTy2 = dyn_cast<MetatypeType>(t2)) {
      if (metaTy1.getInstanceType().getClassOrBoundGenericClass() &&
          metaTy2.getInstanceType().getClassOrBoundGenericClass()) {
        return true;
      }
    }
  }

  return false;
}

namespace {
  enum class ParameterPosition {
    NotParameter,
    Parameter,
    ParameterTupleElement
  };

  enum class OptionalUnwrapping {
    None,
    OptionalToOptional,
    ValueToOptional,
    OptionalToValue
  };
} // end anonymous namespace

static bool matchesFunctionType(CanAnyFunctionType fn1, CanAnyFunctionType fn2,
                                TypeMatchOptions matchMode,
                                OptionalUnwrapping insideOptional,
                                llvm::function_ref<bool()> paramsAndResultMatch) {
  // FIXME: Handle generic functions in non-ABI matches.
  if (!matchMode.contains(TypeMatchFlags::AllowABICompatible)) {
    if (!isa<FunctionType>(fn1) || !isa<FunctionType>(fn2))
      return false;
  }

  // When checking overrides, allow the base type to be throwing even if the
  // overriding type isn't.
  auto ext1 = fn1->getExtInfo();
  auto ext2 = fn2->getExtInfo();
  if (matchMode.contains(TypeMatchFlags::AllowOverride)) {
    // Removing 'throwing' is ABI-compatible for synchronous functions, but
    // not for async ones.
    if (ext2.isThrowing() &&
        !(ext2.isAsync() &&
          matchMode.contains(TypeMatchFlags::AllowABICompatible))) {
      ext1 = ext1.withThrows(true);
    }

    // Removing '@Sendable' is ABI-compatible because there's nothing wrong with
    // a function being sendable when it doesn't need to be.
    if (!ext2.isSendable())
      ext1 = ext1.withConcurrent(false);
  }

  if (matchMode.contains(TypeMatchFlags::IgnoreFunctionSendability)) {
    ext1 = ext1.withConcurrent(false);
    ext2 = ext2.withConcurrent(false);
  }

  // If specified, allow an escaping function parameter to override a
  // non-escaping function parameter when the parameter is optional.
  // Note that this is checking 'ext2' rather than 'ext1' because parameters
  // must be contravariant for the containing function to be covariant.
  if (matchMode.contains(
          TypeMatchFlags::IgnoreNonEscapingForOptionalFunctionParam) &&
      insideOptional == OptionalUnwrapping::OptionalToOptional) {
    if (!ext2.isNoEscape())
      ext1 = ext1.withNoEscape(false);
  }
  if (!ext1.isEqualTo(ext2, useClangTypes(fn1)))
    return false;

  return paramsAndResultMatch();
}

static bool matches(CanType t1, CanType t2, TypeMatchOptions matchMode,
                    ParameterPosition paramPosition,
                    OptionalUnwrapping insideOptional) {
  if (t1 == t2) return true;

  // First try unwrapping optionals.
  // Make sure we only unwrap at most one layer of optional.
  if (insideOptional == OptionalUnwrapping::None) {
    // Value-to-optional and optional-to-optional.
    if (auto obj2 = t2.getOptionalObjectType()) {
      // Optional-to-optional.
      if (auto obj1 = t1.getOptionalObjectType()) {
        // Allow T? and T! to freely match one another.
        return matches(obj1, obj2, matchMode, ParameterPosition::NotParameter,
                       OptionalUnwrapping::OptionalToOptional);
      }

      // Value-to-optional.
      if (matchMode.contains(TypeMatchFlags::AllowABICompatible)) {
        if (isABICompatibleEvenAddingOptional(t1, obj2))
          return true;
      }
      if (matchMode.contains(TypeMatchFlags::AllowOverride) ||
          matchMode.contains(TypeMatchFlags::AllowTopLevelOptionalMismatch)) {
        return matches(t1, obj2, matchMode, ParameterPosition::NotParameter,
                       OptionalUnwrapping::ValueToOptional);
      }

    } else if (matchMode.contains(
                 TypeMatchFlags::AllowTopLevelOptionalMismatch)) {
      // Optional-to-value, normally disallowed.
      if (auto obj1 = t1.getOptionalObjectType()) {
        return matches(obj1, t2, matchMode, ParameterPosition::NotParameter,
                       OptionalUnwrapping::OptionalToValue);
      }
    }
  }

  // Scalar-to-tuple and tuple-to-tuple.
  if (auto tuple2 = dyn_cast<TupleType>(t2)) {
    // We only ever look into singleton tuples on the RHS if we're
    // certain that the LHS isn't also a singleton tuple.
    ParameterPosition elementPosition;
    switch (paramPosition) {
    case ParameterPosition::NotParameter:
    case ParameterPosition::ParameterTupleElement:
      elementPosition = ParameterPosition::NotParameter;
      break;
    case ParameterPosition::Parameter:
      elementPosition = ParameterPosition::ParameterTupleElement;
      break;
    }

    auto tuple1 = dyn_cast<TupleType>(t1);
    if (!tuple1 || tuple1->getNumElements() != tuple2->getNumElements()) {
      if (tuple2->getNumElements() == 1) {
        return matches(t1, tuple2.getElementType(0), matchMode, elementPosition,
                       OptionalUnwrapping::None);
      }
      return false;
    }

    for (auto i : indices(tuple1.getElementTypes())) {
      if (!matches(tuple1.getElementType(i), tuple2.getElementType(i),
                   matchMode, elementPosition, OptionalUnwrapping::None)) {
        return false;
      }
    }
    return true;
  }

  // Function-to-function.
  if (auto fn2 = dyn_cast<AnyFunctionType>(t2)) {
    auto fn1 = dyn_cast<AnyFunctionType>(t1);
    if (!fn1)
      return false;

    auto paramsAndResultMatch = [&]() {
      auto fn2Params = fn2.getParams();
      auto fn1Params = fn1.getParams();
      if (fn2Params.size() != fn1Params.size()) {
        return false;
      }

      // Inputs are contravariant.
      for (auto i : indices(fn2.getParams())) {
        if (!matches(fn2Params[i].getOldType(), fn1Params[i].getOldType(),
                     matchMode, ParameterPosition::ParameterTupleElement,
                     OptionalUnwrapping::None)) {
          return false;
        }
      }

      // Results are covariant.
      return (matches(fn1.getResult(), fn2.getResult(), matchMode,
                      ParameterPosition::NotParameter,
                      OptionalUnwrapping::None));
    };

    return matchesFunctionType(fn1, fn2, matchMode, insideOptional,
                               paramsAndResultMatch);
  }

  // Class-to-class.
  if (matchMode.contains(TypeMatchFlags::AllowOverride))
    if (t2->isExactSuperclassOf(t1))
      return true;

  if (matchMode.contains(TypeMatchFlags::AllowABICompatible))
    if (isABICompatibleEvenAddingOptional(t1, t2))
      return true;

  if (matchMode.contains(TypeMatchFlags::AllowCompatibleOpaqueTypeArchetypes))
    if (auto opaque1 = t1->getAs<OpaqueTypeArchetypeType>())
      if (auto opaque2 = t2->getAs<OpaqueTypeArchetypeType>())
        return opaque1->getDecl()->getOpaqueInterfaceGenericSignature()
                 .getCanonicalSignature() ==
               opaque2->getDecl()->getOpaqueInterfaceGenericSignature()
                  .getCanonicalSignature() &&
               opaque1->getSubstitutions().getCanonical() ==
                 opaque2->getSubstitutions().getCanonical() &&
               opaque1->getInterfaceType()->getCanonicalType()->matches(
                   opaque2->getInterfaceType()->getCanonicalType(), matchMode);

  return false;
}

bool TypeBase::matches(Type other, TypeMatchOptions matchMode) {
  return ::matches(getCanonicalType(), other->getCanonicalType(), matchMode,
                   ParameterPosition::NotParameter, OptionalUnwrapping::None);
}

bool TypeBase::matchesParameter(Type other, TypeMatchOptions matchMode) {
  return ::matches(getCanonicalType(), other->getCanonicalType(), matchMode,
                   ParameterPosition::Parameter, OptionalUnwrapping::None);
}

bool TypeBase::matchesFunctionType(Type other, TypeMatchOptions matchMode,
                                   llvm::function_ref<bool()> paramsAndResultMatch) {
  auto thisFnTy = dyn_cast<AnyFunctionType>(getCanonicalType());
  auto otherFnTy = dyn_cast<AnyFunctionType>(other->getCanonicalType());

  assert(thisFnTy && otherFnTy);
  return ::matchesFunctionType(thisFnTy, otherFnTy, matchMode,
                               OptionalUnwrapping::None, paramsAndResultMatch);
}

/// getNamedElementId - If this tuple has a field with the specified name,
/// return the field index, otherwise return -1.
int TupleType::getNamedElementId(Identifier I) const {
  for (unsigned i = 0, e = Bits.TupleType.Count; i != e; ++i) {
    if (getTrailingObjects<TupleTypeElt>()[i].getName() == I)
      return i;
  }

  // Otherwise, name not found.
  return -1;
}

ArchetypeType::ArchetypeType(TypeKind Kind,
                             const ASTContext &Ctx,
                             RecursiveTypeProperties properties,
                             Type InterfaceType,
                             ArrayRef<ProtocolDecl *> ConformsTo,
                             Type Superclass, LayoutConstraint Layout,
                             GenericEnvironment *Environment)
  : SubstitutableType(Kind, &Ctx, properties),
    InterfaceType(InterfaceType),
    Environment(Environment)
{
  // Set up the bits we need for trailing objects to work.
  Bits.ArchetypeType.HasSuperclass = static_cast<bool>(Superclass);
  Bits.ArchetypeType.HasLayoutConstraint = static_cast<bool>(Layout);
  Bits.ArchetypeType.NumProtocols = ConformsTo.size();

  // Record the superclass.
  if (Superclass)
    *getSubclassTrailingObjects<Type>() = Superclass;

  // Record the layout constraint.
  if (Layout)
    *getSubclassTrailingObjects<LayoutConstraint>() = Layout;

  // Copy the protocols.
  std::uninitialized_copy(ConformsTo.begin(), ConformsTo.end(),
                          getSubclassTrailingObjects<ProtocolDecl *>());
}

ArchetypeType *ArchetypeType::getParent() const {
  if (auto depMemTy = getInterfaceType()->getAs<DependentMemberType>()) {
    return getGenericEnvironment()->mapTypeIntoContext(depMemTy->getBase())
        ->castTo<ArchetypeType>();
  }

  return nullptr;
}

ArchetypeType *ArchetypeType::getRoot() const {
  if (isRoot()) {
    return const_cast<ArchetypeType *>(this);
  }

  auto gp = InterfaceType->getRootGenericParam();
  assert(gp && "Missing root generic parameter?");
  return getGenericEnvironment()->mapTypeIntoContext(
      Type(gp))->castTo<ArchetypeType>();
}

bool ArchetypeType::isRoot() const {
  return getInterfaceType()->is<GenericTypeParamType>();
}

Type ArchetypeType::getExistentialType() const {
  auto *genericEnv = getGenericEnvironment();

  // Opened types hold this directly.
  if (auto *opened = dyn_cast<OpenedArchetypeType>(this)) {
    if (opened->isRoot()) {
      return genericEnv->getOpenedExistentialType();
    }
  }

  // Otherwise we compute it via a generic signature query.
  auto interfaceType = getInterfaceType();
  auto genericSig = genericEnv->getGenericSignature();

  auto upperBound = genericSig->getDependentUpperBounds(interfaceType);

  return genericEnv->mapTypeIntoContext(upperBound);
}

bool ArchetypeType::requiresClass() const {
  if (auto layout = getLayoutConstraint())
    return layout->isClass();
  return false;
}

Type ArchetypeType::getNestedType(AssociatedTypeDecl *assocType) {
  Type interfaceType = getInterfaceType();
  Type memberInterfaceType =
      DependentMemberType::get(interfaceType, assocType);
  auto genericSig = getGenericEnvironment()->getGenericSignature();
  if (genericSig->isValidTypeParameter(memberInterfaceType)) {
    return getGenericEnvironment()->getOrCreateArchetypeFromInterfaceType(
        memberInterfaceType);
  }

  return Type();
}

Type ArchetypeType::getNestedTypeByName(Identifier name) {
  Type interfaceType = getInterfaceType();
  Type memberInterfaceType = DependentMemberType::get(interfaceType, name);
  auto genericSig = getGenericEnvironment()->getGenericSignature();
  if (genericSig->isValidTypeParameter(memberInterfaceType)) {
    return getGenericEnvironment()->getOrCreateArchetypeFromInterfaceType(
        memberInterfaceType);
  }

  return Type();
}

Identifier ArchetypeType::getName() const {
  assert(InterfaceType);
  if (auto depMemTy = InterfaceType->getAs<DependentMemberType>())
    return depMemTy->getName();
  return InterfaceType->castTo<GenericTypeParamType>()->getName();
}

std::string ArchetypeType::getFullName() const {
  return InterfaceType.getString();
}

PrimaryArchetypeType::PrimaryArchetypeType(const ASTContext &Ctx,
                                     GenericEnvironment *GenericEnv,
                                     Type InterfaceType,
                                     ArrayRef<ProtocolDecl *> ConformsTo,
                                     Type Superclass, LayoutConstraint Layout)
  : ArchetypeType(TypeKind::PrimaryArchetype, Ctx,
                  RecursiveTypeProperties::HasArchetype,
                  InterfaceType, ConformsTo, Superclass, Layout, GenericEnv)
{
  assert(!InterfaceType->isParameterPack());
}

CanPrimaryArchetypeType
PrimaryArchetypeType::getNew(const ASTContext &Ctx,
                      GenericEnvironment *GenericEnv,
                      Type InterfaceType,
                      SmallVectorImpl<ProtocolDecl *> &ConformsTo,
                      Type Superclass,
                      LayoutConstraint Layout) {
  assert(!Superclass || Superclass->getClassOrBoundGenericClass());
  assert(GenericEnv && "missing generic environment for archetype");

  // Gather the set of protocol declarations to which this archetype conforms.
  ProtocolType::canonicalizeProtocols(ConformsTo);

  auto arena = AllocationArena::Permanent;
  void *mem = Ctx.Allocate(
    PrimaryArchetypeType::totalSizeToAlloc<ProtocolDecl *, Type, LayoutConstraint>(
          ConformsTo.size(), Superclass ? 1 : 0, Layout ? 1 : 0),
      alignof(PrimaryArchetypeType), arena);

  return CanPrimaryArchetypeType(::new (mem) PrimaryArchetypeType(
      Ctx, GenericEnv, InterfaceType, ConformsTo, Superclass, Layout));
}

OpaqueTypeArchetypeType::OpaqueTypeArchetypeType(
    GenericEnvironment *environment,
    RecursiveTypeProperties properties,
    Type interfaceType,
    ArrayRef<ProtocolDecl*> conformsTo,
    Type superclass, LayoutConstraint layout)
  : ArchetypeType(TypeKind::OpaqueTypeArchetype, interfaceType->getASTContext(),
                  properties, interfaceType, conformsTo, superclass, layout,
                  environment)
{
  assert(!interfaceType->isParameterPack());
}

CanType OpaqueTypeArchetypeType::getCanonicalInterfaceType(Type interfaceType) {
  auto sig = Environment->getOpaqueTypeDecl()
      ->getOpaqueInterfaceGenericSignature();
  CanType canonicalType = interfaceType->getReducedType(sig);
  return Environment->maybeApplyOuterContextSubstitutions(canonicalType)
      ->getCanonicalType();
}

OpaqueTypeDecl *OpaqueTypeArchetypeType::getDecl() const {
  return Environment->getOpaqueTypeDecl();
}

SubstitutionMap OpaqueTypeArchetypeType::getSubstitutions() const {
  return Environment->getOpaqueSubstitutions();
}

OpenedArchetypeType::OpenedArchetypeType(
    GenericEnvironment *environment, Type interfaceType,
    ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
    LayoutConstraint layout)
  : LocalArchetypeType(TypeKind::OpenedArchetype,
                       interfaceType->getASTContext(),
                       RecursiveTypeProperties::HasArchetype
                         | RecursiveTypeProperties::HasOpenedExistential,
                       interfaceType, conformsTo, superclass, layout,
                       environment)
{
  assert(!interfaceType->isParameterPack());
}

UUID OpenedArchetypeType::getOpenedExistentialID() const {
  return getGenericEnvironment()->getOpenedExistentialUUID();
}

PackArchetypeType::PackArchetypeType(
    const ASTContext &Ctx, GenericEnvironment *GenericEnv, Type InterfaceType,
    ArrayRef<ProtocolDecl *> ConformsTo, Type Superclass,
    LayoutConstraint Layout, PackShape Shape)
    : ArchetypeType(TypeKind::PackArchetype, Ctx,
                    RecursiveTypeProperties::HasArchetype, InterfaceType,
                    ConformsTo, Superclass, Layout, GenericEnv) {
  assert(InterfaceType->isParameterPack());
  *getTrailingObjects<PackShape>() = Shape;
}

CanPackArchetypeType
PackArchetypeType::get(const ASTContext &Ctx,
                       GenericEnvironment *GenericEnv,
                       Type InterfaceType, Type ShapeType,
                       SmallVectorImpl<ProtocolDecl *> &ConformsTo,
                       Type Superclass, LayoutConstraint Layout) {
  assert(!Superclass || Superclass->getClassOrBoundGenericClass());
  assert(GenericEnv && "missing generic environment for archetype");

  // Gather the set of protocol declarations to which this archetype conforms.
  ProtocolType::canonicalizeProtocols(ConformsTo);

  auto arena = AllocationArena::Permanent;
  void *mem =
      Ctx.Allocate(PackArchetypeType::totalSizeToAlloc<ProtocolDecl *, Type,
                                                       LayoutConstraint, PackShape>(
                       ConformsTo.size(), Superclass ? 1 : 0, Layout ? 1 : 0,
                       /*shapeSize*/1),
                   alignof(PackArchetypeType), arena);

  return CanPackArchetypeType(::new (mem) PackArchetypeType(
      Ctx, GenericEnv, InterfaceType, ConformsTo, Superclass, Layout,
      {ShapeType}));
}

CanType PackArchetypeType::getReducedShape() {
  // mapTypeIntoContext() also calls getReducedShape() via
  // PackExpansionType::get(), so avoid that by short-circuiting
  // the case where the pack archetype represents its own
  // shape class.
  auto shapeType = getTrailingObjects<PackShape>()->shapeType;
  if (shapeType->isEqual(getInterfaceType()))
    return CanType(this);

  return getGenericEnvironment()
      ->mapTypeIntoContext(shapeType)
      ->getCanonicalType();
}

ElementArchetypeType::ElementArchetypeType(
    const ASTContext &Ctx, GenericEnvironment *GenericEnv, Type InterfaceType,
    ArrayRef<ProtocolDecl *> ConformsTo, Type Superclass,
    LayoutConstraint Layout)
    : LocalArchetypeType(TypeKind::ElementArchetype, Ctx,
                         RecursiveTypeProperties::HasArchetype |
                         RecursiveTypeProperties::HasElementArchetype,
                         InterfaceType,
                         ConformsTo, Superclass, Layout, GenericEnv) {
}

CanTypeWrapper<ElementArchetypeType> ElementArchetypeType::getNew(
    GenericEnvironment *environment, Type interfaceType,
    ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
    LayoutConstraint layout) {
  assert(!interfaceType->isParameterPack());
  assert((!superclass || !superclass->hasArchetype())
         && "superclass must be interface type");
  auto arena = AllocationArena::Permanent;
  ASTContext &ctx = interfaceType->getASTContext();
  void *mem = ctx.Allocate(
    ElementArchetypeType::totalSizeToAlloc<ProtocolDecl *,Type,LayoutConstraint>(
      conformsTo.size(),
      superclass ? 1 : 0,
      layout ? 1 : 0),
      alignof(ElementArchetypeType), arena);

  return CanElementArchetypeType(::new (mem) ElementArchetypeType(
      ctx, environment, interfaceType, conformsTo, superclass, layout));
}

UUID ElementArchetypeType::getOpenedElementID() const {
  return getGenericEnvironment()->getOpenedElementUUID();
}

CanExistentialType CanExistentialType::get(CanType constraint) {
  assert(!(constraint->isAny() || constraint->isAnyObject()) &&
         "Any(Object) may not apppear as canonical constraint type");
  assert(!constraint->is<ExistentialMetatypeType>() &&
         "Existential metatype may not apppear as canonical constraint type");
  return CanExistentialType(
      ExistentialType::get(constraint)->castTo<ExistentialType>());
}

void ProtocolCompositionType::Profile(llvm::FoldingSetNodeID &ID,
                                      ArrayRef<Type> Members,
                                      bool HasExplicitAnyObject) {
  ID.AddInteger(HasExplicitAnyObject);
  for (auto T : Members)
    ID.AddPointer(T.getPointer());
}

ParameterizedProtocolType::ParameterizedProtocolType(
    const ASTContext *ctx,
    ProtocolType *base, ArrayRef<Type> args,
    RecursiveTypeProperties properties)
  : TypeBase(TypeKind::ParameterizedProtocol, /*Context=*/ctx, properties),
    Base(base) {
  assert(args.size() > 0);
  Bits.ParameterizedProtocolType.ArgCount = args.size();
  for (unsigned i : indices(args))
    getTrailingObjects<Type>()[i] = args[i];
}

void ParameterizedProtocolType::Profile(llvm::FoldingSetNodeID &ID,
                                        ProtocolType *baseTy,
                                        ArrayRef<Type> args) {
  ID.AddPointer(baseTy);
  for (auto arg : args)
    ID.AddPointer(arg.getPointer());
}

void ParameterizedProtocolType::getRequirements(
    Type baseType, SmallVectorImpl<Requirement> &reqs) const {
  auto *protoDecl = getProtocol();

  auto assocTypes = protoDecl->getPrimaryAssociatedTypes();
  auto argTypes = getArgs();
  assert(argTypes.size() <= assocTypes.size());

  for (unsigned i : indices(argTypes)) {
    auto argType = argTypes[i];
    auto *assocType = assocTypes[i];
    auto subjectType = assocType->getDeclaredInterfaceType()
        ->castTo<DependentMemberType>()
        ->substBaseType(protoDecl->getParentModule(), baseType);
    reqs.emplace_back(RequirementKind::SameType, subjectType, argType);
  }
}

bool ProtocolType::requiresClass() {
  return getDecl()->requiresClass();
}

bool ProtocolCompositionType::requiresClass() {
  return getExistentialLayout().requiresClass();
}

Type ProtocolCompositionType::get(const ASTContext &C,
                                  ArrayRef<Type> Members,
                                  bool HasExplicitAnyObject) {
  // Fast path for 'AnyObject' and 'Any'.
  if (Members.empty()) {
    return build(C, Members, HasExplicitAnyObject);
  }

  // If there's a single member and no layout constraint, return that type.
  if (Members.size() == 1 && !HasExplicitAnyObject) {
    return Members.front();
  }

  for (Type t : Members) {
    if (!t->isCanonical())
      return build(C, Members, HasExplicitAnyObject);
  }
    
  Type Superclass;
  SmallVector<ProtocolDecl *, 4> Protocols;
  ParameterizedProtocolMap Parameterized;
  for (Type t : Members) {
    addProtocols(t, Protocols, Parameterized, Superclass, HasExplicitAnyObject);
  }

  // The presence of a superclass constraint makes AnyObject redundant.
  if (Superclass)
    HasExplicitAnyObject = false;

  // If there are any parameterized protocols, the canonicalization
  // algorithm gets more complex.

  // Form the set of canonical component types.
  SmallVector<Type, 4> CanTypes;
  if (Superclass)
    CanTypes.push_back(Superclass->getCanonicalType());

  canonicalizeProtocols(Protocols, &Parameterized);

  for (auto proto: Protocols) {
    // If we have a parameterized type for this protocol, use the
    // canonical type of that.  Sema should prevent us from building
    // compositions with the same protocol and conflicting constraints.
    if (!Parameterized.empty()) {
      auto it = Parameterized.find(proto);
      if (it != Parameterized.end()) {
        CanTypes.push_back(it->second->getCanonicalType());
        continue;
      }
    }

    CanTypes.push_back(proto->getDeclaredInterfaceType());
  }

  // If one member remains and no layout constraint, return that type.
  if (CanTypes.size() == 1 && !HasExplicitAnyObject)
    return CanTypes.front();

  return build(C, CanTypes, HasExplicitAnyObject);
}

CanType ProtocolCompositionType::getMinimalCanonicalType(
    const DeclContext *useDC) const {
  const CanType CanTy = getCanonicalType();

  // If the canonical type is not a composition, it's minimal.
  const auto Composition = dyn_cast<ProtocolCompositionType>(CanTy);
  if (!Composition) {
    return CanTy;
  }

  // Nothing to minimize.
  if (Composition->getMembers().empty()) {
    return CanTy;
  }

  // The only cases we're missing out on proper minimization is when a
  // composition has an explicit superclass or AnyObject constraint.
  if (!Composition->hasExplicitAnyObject() &&
      !Composition->getMembers().front()->getClassOrBoundGenericClass()) {
    // Already minimal.
    return CanTy;
  }

  auto &Ctx = CanTy->getASTContext();

  // Use generic signature minimization: the requirements of the signature will
  // represent the minimal composition.
  auto sig = useDC->getGenericSignatureOfContext();
  const auto Sig = Ctx.getOpenedExistentialSignature(CanTy, sig);
  const auto &Reqs = Sig.getRequirements();
  if (Reqs.size() == 1) {
    return Reqs.front().getSecondType()->getCanonicalType();
  }

  llvm::SmallVector<Type, 2> MinimalMembers;
  bool MinimalHasExplicitAnyObject = false;
  auto ifaceTy = Sig.getGenericParams().back();
  for (const auto &Req : Reqs) {
    if (!Req.getFirstType()->isEqual(ifaceTy)) {
      continue;
    }

    switch (Req.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement not supported here");
    case RequirementKind::Superclass:
    case RequirementKind::Conformance:
      MinimalMembers.push_back(Req.getSecondType());
      break;
    case RequirementKind::Layout:
      MinimalHasExplicitAnyObject = true;
      break;
    case RequirementKind::SameType:
      llvm_unreachable("");
    }
  }

  // A superclass constraint is always retained and must appear first in the
  // members list.
  assert(Composition->getMembers().front()->getClassOrBoundGenericClass() ==
         MinimalMembers.front()->getClassOrBoundGenericClass());

  // If we are left with a single member and no layout constraint, the member
  // is the minimal type. Also, note that a protocol composition cannot be
  // constructed with a single member unless there is a layout constraint.
  if (MinimalMembers.size() == 1 && !MinimalHasExplicitAnyObject)
    return CanType(MinimalMembers.front());

  // The resulting composition is necessarily canonical.
  return CanType(build(Ctx, MinimalMembers, MinimalHasExplicitAnyObject));
}

ClangTypeInfo AnyFunctionType::getClangTypeInfo() const {
  switch (getKind()) {
  case TypeKind::Function:
    return cast<FunctionType>(this)->getClangTypeInfo();
  case TypeKind::GenericFunction:
    // Generic functions do not have C types.
    return ClangTypeInfo();
  default:
    llvm_unreachable("Illegal type kind for AnyFunctionType.");
  }
}

Type AnyFunctionType::getGlobalActor() const {
  switch (getKind()) {
  case TypeKind::Function:
    return cast<FunctionType>(this)->getGlobalActor();
  case TypeKind::GenericFunction:
    return cast<GenericFunctionType>(this)->getGlobalActor();
  default:
    llvm_unreachable("Illegal type kind for AnyFunctionType.");
  }
}

ClangTypeInfo AnyFunctionType::getCanonicalClangTypeInfo() const {
  return getClangTypeInfo().getCanonical();
}

bool AnyFunctionType::hasNonDerivableClangType() {
  auto clangTypeInfo = getClangTypeInfo();
  if (clangTypeInfo.empty())
    return false;
  auto computedClangType = getASTContext().getClangFunctionType(
      getParams(), getResult(), getRepresentation());
  assert(computedClangType && "Failed to compute Clang type.");
  return clangTypeInfo != ClangTypeInfo(computedClangType);
}

bool AnyFunctionType::hasSameExtInfoAs(const AnyFunctionType *otherFn) {
  return getExtInfo().isEqualTo(otherFn->getExtInfo(), useClangTypes(this));
}

ClangTypeInfo SILFunctionType::getClangTypeInfo() const {
  if (!Bits.SILFunctionType.HasClangTypeInfo)
    return ClangTypeInfo();
  auto *info = getTrailingObjects<ClangTypeInfo>();
  assert(!info->empty() &&
         "If the ClangTypeInfo was empty, we shouldn't have stored it.");
  return *info;
}

bool SILFunctionType::hasNonDerivableClangType() {
  auto clangTypeInfo = getClangTypeInfo();
  if (clangTypeInfo.empty())
    return false;
  auto results = getResults();
  auto computedClangType =
      getASTContext().getCanonicalClangFunctionType(
          getParameters(),
          results.empty() ? None : Optional<SILResultInfo>(results[0]),
          getRepresentation());
  assert(computedClangType && "Failed to compute Clang type.");
  return clangTypeInfo != ClangTypeInfo(computedClangType);
}

bool SILFunctionType::hasSameExtInfoAs(const SILFunctionType *otherFn) {
  return getExtInfo().isEqualTo(otherFn->getExtInfo(), useClangTypes(this));
}

DependentMemberType *TypeBase::findUnresolvedDependentMemberType() {
  if (!hasTypeParameter()) return nullptr;

  DependentMemberType *unresolvedDepMemTy = nullptr;
  Type(this).findIf([&](Type type) -> bool {
    if (auto depMemTy = type->getAs<DependentMemberType>()) {
      if (depMemTy->getAssocType() == nullptr) {
        unresolvedDepMemTy = depMemTy;
        return true;
      }
    }
    return false;
  });

  return unresolvedDepMemTy;
}

bool TypeBase::isNoEscape() const {
  auto type = getCanonicalType();

  if (auto silFuncTy = dyn_cast<SILFunctionType>(type))
    return silFuncTy->isNoEscape();

  if (auto funcTy = dyn_cast<FunctionType>(type))
    return funcTy->isNoEscape();

  if (auto tupleTy = dyn_cast<TupleType>(type)) {
    for (auto eltTy : tupleTy.getElementTypes())
      if (eltTy->isNoEscape())
        return true;
  }

  return false;
}

Identifier DependentMemberType::getName() const {
  if (NameOrAssocType.is<Identifier>())
    return NameOrAssocType.get<Identifier>();

  return NameOrAssocType.get<AssociatedTypeDecl *>()->getName();
}

/// \param pos The variance position of the result type.
static bool transformSILResult(
    TypePosition pos, SILResultInfo &result, bool &changed,
    llvm::function_ref<Optional<Type>(TypeBase *, TypePosition)> fn) {
  Type transType = result.getInterfaceType().transformWithPosition(pos, fn);
  if (!transType) return true;

  CanType canTransType = transType->getCanonicalType();
  if (canTransType != result.getInterfaceType()) {
    changed = true;
    result = result.getWithInterfaceType(canTransType);
  }
  return false;
}

/// \param pos The variance position of the yield type.
static bool transformSILYield(
    TypePosition pos, SILYieldInfo &yield, bool &changed,
    llvm::function_ref<Optional<Type>(TypeBase *, TypePosition)> fn) {
  Type transType = yield.getInterfaceType().transformWithPosition(pos, fn);
  if (!transType) return true;

  CanType canTransType = transType->getCanonicalType();
  if (canTransType != yield.getInterfaceType()) {
    changed = true;
    yield = yield.getWithInterfaceType(canTransType);
  }
  return false;
}

/// \param pos The variance position of the parameter type.
static bool transformSILParameter(
    TypePosition pos, SILParameterInfo &param, bool &changed,
    llvm::function_ref<Optional<Type>(TypeBase *, TypePosition)> fn) {
  Type transType = param.getInterfaceType().transformWithPosition(pos, fn);
  if (!transType) return true;

  CanType canTransType = transType->getCanonicalType();
  if (canTransType != param.getInterfaceType()) {
    changed = true;
    param = param.getWithInterfaceType(canTransType);
  }
  return false;
}

Type Type::transform(llvm::function_ref<Type(Type)> fn) const {
  return transformWithPosition(TypePosition::Invariant,
                               [fn](TypeBase *type, auto) -> Optional<Type> {
    Type transformed = fn(Type(type));
    if (!transformed)
      return Type();

    // If the function didn't change the type at
    // all, let transformRec() recurse.
    if (transformed.getPointer() == type)
      return None;

    return transformed;
  });
}

static PackType *getTransformedPack(Type substType) {
  if (auto pack = substType->getAs<PackType>()) {
    return pack;
  }

  // The pack matchers like to make expansions out of packs, and
  // these types then propagate out into transforms.  Make sure we
  // flatten them exactly if they were the underlying pack.
  // FIXME: stop doing this and make PackExpansionType::get assert
  // that we never construct these types
  if (auto expansion = substType->getAs<PackExpansionType>()) {
    return expansion->getPatternType()->getAs<PackType>();
  }

  return nullptr;
}

Type Type::transformRec(
    llvm::function_ref<Optional<Type>(TypeBase *)> fn) const {
  return transformWithPosition(TypePosition::Invariant,
                               [fn](TypeBase *type, auto) { return fn(type); });
}

Type Type::transformWithPosition(
    TypePosition pos,
    llvm::function_ref<Optional<Type>(TypeBase *, TypePosition)> fn) const {
  if (!isa<ParenType>(getPointer())) {
    // Transform this type node.
    if (Optional<Type> transformed = fn(getPointer(), pos))
      return *transformed;

    // Recur.
  }

  // Recur into children of this type.
  TypeBase *const base = getPointer();
  switch (base->getKind()) {
#define BUILTIN_TYPE(Id, Parent) \
case TypeKind::Id:
#define TYPE(Id, Parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype:
  case TypeKind::Error:
  case TypeKind::Unresolved:
  case TypeKind::TypeVariable:
  case TypeKind::Placeholder:
  case TypeKind::GenericTypeParam:
  case TypeKind::SILToken:
  case TypeKind::Module:
  case TypeKind::BuiltinTuple:
    return *this;

  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::Protocol: {
    auto nominalTy = cast<NominalType>(base);
    if (auto parentTy = nominalTy->getParent()) {
      parentTy = parentTy.transformWithPosition(pos, fn);
      if (!parentTy)
        return Type();

      if (parentTy.getPointer() == nominalTy->getParent().getPointer())
        return *this;

      return NominalType::get(nominalTy->getDecl(), parentTy,
                              Ptr->getASTContext());
    }

    return *this;
  }
      
  case TypeKind::SILBlockStorage: {
    auto storageTy = cast<SILBlockStorageType>(base);
    Type transCap = storageTy->getCaptureType().transformWithPosition(
        TypePosition::Invariant, fn);
    if (!transCap)
      return Type();
    CanType canTransCap = transCap->getCanonicalType();
    if (canTransCap != storageTy->getCaptureType())
      return SILBlockStorageType::get(canTransCap);
    return storageTy;
  }

  case TypeKind::SILMoveOnlyWrapped: {
    auto *storageTy = cast<SILMoveOnlyWrappedType>(base);
    Type transCap = storageTy->getInnerType().transformWithPosition(
        TypePosition::Invariant, fn);
    if (!transCap)
      return Type();
    CanType canTransCap = transCap->getCanonicalType();
    if (canTransCap != storageTy->getInnerType())
      return SILMoveOnlyWrappedType::get(canTransCap);
    return storageTy;
  }

  case TypeKind::SILBox: {
    bool changed = false;
    auto boxTy = cast<SILBoxType>(base);
#ifndef NDEBUG
    // This interface isn't suitable for updating the substitution map in a
    // generic SILBox.
    for (Type type : boxTy->getSubstitutions().getReplacementTypes()) {
      assert(type->isEqual(
                 type.transformWithPosition(TypePosition::Invariant, fn)) &&
             "SILBoxType substitutions can't be transformed");
    }
#endif
    SmallVector<SILField, 4> newFields;
    auto *l = boxTy->getLayout();
    for (auto f : l->getFields()) {
      auto fieldTy = f.getLoweredType();
      auto transformed =
          fieldTy.transformWithPosition(TypePosition::Invariant, fn)
              ->getCanonicalType();
      changed |= fieldTy != transformed;
      newFields.push_back(SILField(transformed, f.isMutable()));
    }
    if (!changed)
      return *this;
    boxTy = SILBoxType::get(Ptr->getASTContext(),
                            SILLayout::get(Ptr->getASTContext(),
                                           l->getGenericSignature(),
                                           newFields,
                                           l->capturesGenericEnvironment()),
                            boxTy->getSubstitutions());
    return boxTy;
  }
  
  case TypeKind::SILFunction: {
    auto fnTy = cast<SILFunctionType>(base);
    bool changed = false;
    auto updateSubs = [&](SubstitutionMap &subs) -> bool {
      // This interface isn't suitable for doing most transformations on
      // a substituted SILFunctionType, but it's too hard to come up with
      // an assertion that meaningfully captures what restrictions are in
      // place.  Generally the restriction that you can't naively substitute
      // a SILFunctionType using AST mechanisms will have to be good enough.
      SmallVector<Type, 4> newReplacements;
      for (Type type : subs.getReplacementTypes()) {
        auto transformed =
            type.transformWithPosition(TypePosition::Invariant, fn);
        newReplacements.push_back(transformed->getCanonicalType());
        if (!type->isEqual(transformed))
          changed = true;
      }

      if (changed) {
        subs = SubstitutionMap::get(subs.getGenericSignature(),
                                    newReplacements,
                                    subs.getConformances());
      }

      return changed;
    };

    if (fnTy->isPolymorphic())
      return fnTy;

    if (auto subs = fnTy->getInvocationSubstitutions()) {
      if (updateSubs(subs)) {
        return fnTy->withInvocationSubstitutions(subs);
      }
      return fnTy;
    }

    if (auto subs = fnTy->getPatternSubstitutions()) {
      if (updateSubs(subs)) {
        return fnTy->withPatternSubstitutions(subs);
      }
      return fnTy;
    }

    SmallVector<SILParameterInfo, 8> transInterfaceParams;
    for (SILParameterInfo param : fnTy->getParameters()) {
      if (transformSILParameter(pos.flipped(), param, changed, fn))
        return Type();
      transInterfaceParams.push_back(param);
    }

    SmallVector<SILYieldInfo, 8> transInterfaceYields;
    for (SILYieldInfo yield : fnTy->getYields()) {
      if (transformSILYield(pos, yield, changed, fn)) return Type();
      transInterfaceYields.push_back(yield);
    }

    SmallVector<SILResultInfo, 8> transInterfaceResults;
    for (SILResultInfo result : fnTy->getResults()) {
      if (transformSILResult(pos, result, changed, fn)) return Type();
      transInterfaceResults.push_back(result);
    }

    Optional<SILResultInfo> transErrorResult;
    if (fnTy->hasErrorResult()) {
      SILResultInfo result = fnTy->getErrorResult();
      if (transformSILResult(pos, result, changed, fn)) return Type();
      transErrorResult = result;
    }

    if (!changed) return *this;

    return SILFunctionType::get(
        fnTy->getInvocationGenericSignature(),
        fnTy->getExtInfo(),
        fnTy->getCoroutineKind(),
        fnTy->getCalleeConvention(),
        transInterfaceParams,
        transInterfaceYields,
        transInterfaceResults,
        transErrorResult,
        SubstitutionMap(),
        SubstitutionMap(),
        Ptr->getASTContext(),
        fnTy->getWitnessMethodConformanceOrInvalid());
  }

#define REF_STORAGE(Name, ...) \
  case TypeKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
  {
    auto storageTy = cast<ReferenceStorageType>(base);
    Type refTy = storageTy->getReferentType();
    Type substRefTy = refTy.transformWithPosition(pos, fn);
    if (!substRefTy)
      return Type();

    if (substRefTy.getPointer() == refTy.getPointer())
      return *this;

    return ReferenceStorageType::get(substRefTy, storageTy->getOwnership(),
                                     Ptr->getASTContext());
  }

  case TypeKind::UnboundGeneric: {
    auto unbound = cast<UnboundGenericType>(base);
    Type substParentTy;
    if (auto parentTy = unbound->getParent()) {
      substParentTy = parentTy.transformWithPosition(pos, fn);
      if (!substParentTy)
        return Type();

      if (substParentTy.getPointer() == parentTy.getPointer())
        return *this;

      return UnboundGenericType::get(unbound->getDecl(), substParentTy,
                                     Ptr->getASTContext());
    }

    return *this;
  }

  case TypeKind::BoundGenericClass:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct: {
    auto bound = cast<BoundGenericType>(base);
    SmallVector<Type, 4> substArgs;
    bool anyChanged = false;
    Type substParentTy;
    if (auto parentTy = bound->getParent()) {
      substParentTy = parentTy.transformWithPosition(pos, fn);
      if (!substParentTy)
        return Type();

      if (substParentTy.getPointer() != parentTy.getPointer())
        anyChanged = true;
    }

    const auto transformGenArg = [&](Type arg, TypePosition p) -> bool {
      Type substArg = arg.transformWithPosition(p, fn);
      if (!substArg)
        return true;
      substArgs.push_back(substArg);
      if (substArg.getPointer() != arg.getPointer())
        anyChanged = true;

      return false;
    };

    if (bound->isArray() || bound->isOptional()) {
      // Swift.Array preserves variance in its 'Value' type.
      // Swift.Optional preserves variance in its 'Wrapped' type.
      if (transformGenArg(bound->getGenericArgs().front(), pos))
        return Type();
    } else if (bound->isDictionary()) {
      // Swift.Dictionary preserves variance in its 'Element' type.
      if (transformGenArg(bound->getGenericArgs().front(),
                          TypePosition::Invariant) ||
          transformGenArg(bound->getGenericArgs().back(), pos))
        return Type();
    } else {
      for (auto arg : bound->getGenericArgs()) {
        if (transformGenArg(arg, TypePosition::Invariant))
          return Type();
      }
    }

    if (!anyChanged)
      return *this;

    return BoundGenericType::get(bound->getDecl(), substParentTy, substArgs);
  }
      
  case TypeKind::OpaqueTypeArchetype: {
    auto opaque = cast<OpaqueTypeArchetypeType>(base);
    if (opaque->getSubstitutions().empty())
      return *this;
    
    SmallVector<Type, 4> newSubs;
    bool anyChanged = false;
    for (auto replacement : opaque->getSubstitutions().getReplacementTypes()) {
      Type newReplacement =
          replacement.transformWithPosition(TypePosition::Invariant, fn);
      if (!newReplacement)
        return Type();
      newSubs.push_back(newReplacement);
      if (replacement.getPointer() != newReplacement.getPointer())
        anyChanged = true;
    }
    
    if (!anyChanged)
      return *this;
    
    // FIXME: This re-looks-up conformances instead of transforming them in
    // a systematic way.
    auto sig = opaque->getDecl()->getGenericSignature();
    auto newSubMap =
      SubstitutionMap::get(sig,
       [&](SubstitutableType *t) -> Type {
         auto index = sig->getGenericParamOrdinal(cast<GenericTypeParamType>(t));
         return newSubs[index];
       },
       LookUpConformanceInModule(opaque->getDecl()->getModuleContext()));
    return OpaqueTypeArchetypeType::get(opaque->getDecl(),
                                        opaque->getInterfaceType(),
                                        newSubMap);
  }

  case TypeKind::ExistentialMetatype: {
    auto meta = cast<ExistentialMetatypeType>(base);
    auto instanceTy = meta->getInstanceType().transformWithPosition(pos, fn);
    if (!instanceTy)
      return Type();

    if (instanceTy.getPointer() == meta->getInstanceType().getPointer())
      return *this;

    if (meta->hasRepresentation())
      return ExistentialMetatypeType::get(instanceTy,
                                          meta->getRepresentation());
    return ExistentialMetatypeType::get(instanceTy);
  }

  case TypeKind::Metatype: {
    auto meta = cast<MetatypeType>(base);
    auto instanceTy = meta->getInstanceType().transformWithPosition(pos, fn);
    if (!instanceTy)
      return Type();

    if (instanceTy.getPointer() == meta->getInstanceType().getPointer())
      return *this;

    if (meta->hasRepresentation())
      return MetatypeType::get(instanceTy, meta->getRepresentation());
    return MetatypeType::get(instanceTy);
  }

  case TypeKind::DynamicSelf: {
    auto dynamicSelf = cast<DynamicSelfType>(base);
    auto selfTy = dynamicSelf->getSelfType().transformWithPosition(pos, fn);
    if (!selfTy)
      return Type();

    if (selfTy.getPointer() == dynamicSelf->getSelfType().getPointer())
      return *this;

    return DynamicSelfType::get(selfTy, selfTy->getASTContext());
  }

  case TypeKind::TypeAlias: {
    auto alias = cast<TypeAliasType>(base);
    Type oldUnderlyingTy = Type(alias->getSinglyDesugaredType());
    Type newUnderlyingTy = oldUnderlyingTy.transformWithPosition(pos, fn);
    if (!newUnderlyingTy) return Type();

    Type oldParentType = alias->getParent();
    Type newParentType;
    if (oldParentType) {
      newParentType = oldParentType.transformWithPosition(pos, fn);
      if (!newParentType) return newUnderlyingTy;
    }

    auto subMap = alias->getSubstitutionMap();
    for (Type oldReplacementType : subMap.getReplacementTypes()) {
      Type newReplacementType =
          oldReplacementType.transformWithPosition(TypePosition::Invariant, fn);
      if (!newReplacementType)
        return newUnderlyingTy;

      // If anything changed with the replacement type, we lose the sugar.
      // FIXME: This is really unfortunate.
      if (newReplacementType.getPointer() != oldReplacementType.getPointer())
        return newUnderlyingTy;
    }

    if (oldParentType.getPointer() == newParentType.getPointer() &&
        oldUnderlyingTy.getPointer() == newUnderlyingTy.getPointer())
      return *this;

    return TypeAliasType::get(alias->getDecl(), newParentType, subMap,
                              newUnderlyingTy);
  }

  case TypeKind::Paren: {
    auto paren = cast<ParenType>(base);
    Type underlying = paren->getUnderlyingType().transformWithPosition(pos, fn);
    if (!underlying)
      return Type();

    if (underlying.getPointer() == paren->getUnderlyingType().getPointer())
      return *this;

    return ParenType::get(Ptr->getASTContext(), underlying);
  }

  case TypeKind::Pack: {
    auto pack = cast<PackType>(base);
    bool anyChanged = false;
    SmallVector<Type, 4> elements;
    unsigned Index = 0;
    for (Type eltTy : pack->getElementTypes()) {
      Type transformedEltTy =
          eltTy.transformWithPosition(TypePosition::Invariant, fn);
      if (!transformedEltTy)
        return Type();

      // If nothing has changed, just keep going.
      if (!anyChanged &&
          transformedEltTy.getPointer() == eltTy.getPointer()) {
        ++Index;
        continue;
      }

      // If this is the first change we've seen, copy all of the previous
      // elements.
      if (!anyChanged) {
        // Copy all of the previous elements.
        elements.append(pack->getElementTypes().begin(),
                        pack->getElementTypes().begin() + Index);
        anyChanged = true;
      }

      // If the transformed type is a pack, immediately expand it.
      if (auto eltPack = getTransformedPack(transformedEltTy)) {
        auto eltElements = eltPack->getElementTypes();
        elements.append(eltElements.begin(), eltElements.end());
      } else {
        elements.push_back(transformedEltTy);
      }
    }

    if (!anyChanged)
      return *this;

    return PackType::get(Ptr->getASTContext(), elements);
  }

  case TypeKind::SILPack: {
    auto pack = cast<SILPackType>(base);
    bool anyChanged = false;
    SmallVector<CanType, 4> elements;
    unsigned Index = 0;
    for (Type eltTy : pack->getElementTypes()) {
      Type transformedEltTy =
          eltTy.transformWithPosition(TypePosition::Invariant, fn);
      if (!transformedEltTy)
        return Type();

      // If nothing has changed, just keep going.
      if (!anyChanged &&
          transformedEltTy.getPointer() == eltTy.getPointer()) {
        ++Index;
        continue;
      }

      // If this is the first change we've seen, copy all of the previous
      // elements.
      if (!anyChanged) {
        // Copy all of the previous elements.
        elements.append(pack->getElementTypes().begin(),
                        pack->getElementTypes().begin() + Index);
        anyChanged = true;
      }

      auto transformedEltCanTy = transformedEltTy->getCanonicalType();

      // Flatten immediately.
      if (auto transformedEltPack =
            dyn_cast<SILPackType>(transformedEltCanTy)) {
        auto elementElements = transformedEltPack->getElementTypes();
        elements.append(elementElements.begin(), elementElements.end());
      } else {
        assert(!isa<PackType>(transformedEltCanTy));
        elements.push_back(transformedEltCanTy);
      }
    }

    if (!anyChanged)
      return *this;

    return SILPackType::get(Ptr->getASTContext(), pack->getExtInfo(), elements);
  }

  case TypeKind::PackExpansion: {
    auto expand = cast<PackExpansionType>(base);

    // Substitution completely replaces this.

    Type transformedPat =
        expand->getPatternType().transformWithPosition(pos, fn);
    if (!transformedPat)
      return Type();

    Type transformedCount =
        expand->getCountType().transformWithPosition(pos, fn);
    if (!transformedCount)
      return Type();

    if (transformedPat.getPointer() == expand->getPatternType().getPointer() &&
        transformedCount.getPointer() == expand->getCountType().getPointer())
      return *this;

    // // If we transform the count to a pack type, expand the pattern.
    // // This is necessary because of how we piece together types in
    // // the constraint system.
    // if (auto countPack = transformedCount->getAs<PackType>()) {
    //   return PackExpansionType::expand(transformedPat, countPack);
    // }

    return PackExpansionType::get(transformedPat, transformedCount);
  }

  case TypeKind::PackElement: {
    auto element = cast<PackElementType>(base);

    Type transformedPack =
        element->getPackType().transformWithPosition(pos, fn);
    if (!transformedPack)
      return Type();

    if (transformedPack.getPointer() == element->getPackType().getPointer())
      return *this;

    return PackElementType::get(transformedPack, element->getLevel());
  }

  case TypeKind::Tuple: {
    auto tuple = cast<TupleType>(base);
    bool anyChanged = false;
    SmallVector<TupleTypeElt, 4> elements;
    unsigned Index = 0;
    for (const auto &elt : tuple->getElements()) {
      Type eltTy = elt.getType();
      Type transformedEltTy = eltTy.transformWithPosition(pos, fn);
      if (!transformedEltTy)
        return Type();

      // If nothing has changed, just keep going.
      if (!anyChanged &&
          transformedEltTy.getPointer() == elt.getType().getPointer()) {
        ++Index;
        continue;
      }

      // If this is the first change we've seen, copy all of the previous
      // elements.
      if (!anyChanged) {
        // Copy all of the previous elements.
        elements.append(tuple->getElements().begin(),
                        tuple->getElements().begin() + Index);
        anyChanged = true;
      }

      // Add the new tuple element, with the transformed type.
      // Expand packs immediately.
      if (auto eltPack = getTransformedPack(transformedEltTy)) {
        bool first = true;
        for (auto eltElement : eltPack->getElementTypes()) {
          if (first) {
            elements.push_back(elt.getWithType(eltElement));
            first = false;
          } else {
            elements.push_back(TupleTypeElt(eltElement));
          }
        }
      } else {
        elements.push_back(elt.getWithType(transformedEltTy));
      }
    }

    if (!anyChanged)
      return *this;

    // If the transform would yield a singleton tuple, and we didn't
    // start with one, flatten to produce the element type.
    if (elements.size() == 1 &&
        !elements[0].getType()->is<PackExpansionType>() &&
        !(tuple->getNumElements() == 1 &&
          !tuple->getElementType(0)->is<PackExpansionType>())) {
      return elements[0].getType();
    }

    return TupleType::get(elements, Ptr->getASTContext());
  }


  case TypeKind::DependentMember: {
    auto dependent = cast<DependentMemberType>(base);
    auto dependentBase = dependent->getBase().transformWithPosition(pos, fn);
    if (!dependentBase)
      return Type();

    if (dependentBase.getPointer() == dependent->getBase().getPointer())
      return *this;

    if (auto assocType = dependent->getAssocType())
      return DependentMemberType::get(dependentBase, assocType);

    return DependentMemberType::get(dependentBase, dependent->getName());
  }

  case TypeKind::GenericFunction:
  case TypeKind::Function: {
    auto function = cast<AnyFunctionType>(base);

    bool isUnchanged = true;

    // Transform function parameter types.
    SmallVector<AnyFunctionType::Param, 8> substParams;
    for (auto param : function->getParams()) {
      auto type = param.getPlainType();
      auto label = param.getLabel();
      auto flags = param.getParameterFlags();
      auto internalLabel = param.getInternalLabel();

      TypePosition paramPos = pos.flipped();
      if (param.isInOut())
        paramPos = TypePosition::Invariant;

      auto substType = type.transformWithPosition(paramPos, fn);
      if (!substType)
        return Type();

      if (type.getPointer() != substType.getPointer())
        isUnchanged = false;

      // FIXME: Remove this once we get rid of TVO_CanBindToInOut;
      // the only time we end up here is when the constraint solver
      // simplifies a type containing a type variable fixed to an
      // InOutType.
      if (substType->is<InOutType>()) {
        assert(flags.getValueOwnership() == ValueOwnership::Default);
        substType = substType->getInOutObjectType();
        flags = flags.withInOut(true);
      }

      if (auto substPack = getTransformedPack(substType)) {
        bool first = true;
        for (auto substEltType : substPack->getElementTypes()) {
          if (first) {
            substParams.emplace_back(substEltType, label, flags,
                                     internalLabel);
            first = false;
          } else {
            substParams.emplace_back(substEltType, Identifier(), flags,
                                     Identifier());
          }
        }
      } else {
        substParams.emplace_back(substType, label, flags, internalLabel);
      }
    }

    // Transform result type.
    auto resultTy = function->getResult().transformWithPosition(pos, fn);
    if (!resultTy)
      return Type();

    if (resultTy.getPointer() != function->getResult().getPointer())
      isUnchanged = false;

    // Transform the global actor.
    Type globalActorType;
    if (Type origGlobalActorType = function->getGlobalActor()) {
      globalActorType = origGlobalActorType.transformWithPosition(
          TypePosition::Invariant, fn);
      if (!globalActorType)
        return Type();

      if (globalActorType.getPointer() != origGlobalActorType.getPointer())
        isUnchanged = false;
    }

    if (auto genericFnType = dyn_cast<GenericFunctionType>(base)) {
#ifndef NDEBUG
      // Check that generic parameters won't be transformed.
      // Transform generic parameters.
      for (auto param : genericFnType->getGenericParams()) {
        assert(Type(param)
                   .transformWithPosition(TypePosition::Invariant, fn)
                   ->isEqual(param) &&
               "GenericFunctionType transform() changes type parameter");
      }
#endif

      if (isUnchanged) return *this;

      auto genericSig = genericFnType->getGenericSignature();
      if (!function->hasExtInfo())
        return GenericFunctionType::get(genericSig, substParams, resultTy);
      return GenericFunctionType::get(genericSig, substParams, resultTy,
                                      function->getExtInfo()
                                          .withGlobalActor(globalActorType));
    }

    if (isUnchanged) return *this;

    if (!function->hasExtInfo())
      return FunctionType::get(substParams, resultTy);
    return FunctionType::get(substParams, resultTy,
                             function->getExtInfo()
                                 .withGlobalActor(globalActorType));
  }

  case TypeKind::ArraySlice: {
    auto slice = cast<ArraySliceType>(base);
    auto baseTy = slice->getBaseType().transformWithPosition(pos, fn);
    if (!baseTy)
      return Type();

    if (baseTy.getPointer() == slice->getBaseType().getPointer())
      return *this;

    return ArraySliceType::get(baseTy);
  }

  case TypeKind::Optional: {
    auto optional = cast<OptionalType>(base);
    auto baseTy = optional->getBaseType().transformWithPosition(pos, fn);
    if (!baseTy)
      return Type();

    if (baseTy.getPointer() == optional->getBaseType().getPointer())
      return *this;

    return OptionalType::get(baseTy);
  }

  case TypeKind::VariadicSequence: {
    auto seq = cast<VariadicSequenceType>(base);
    auto baseTy = seq->getBaseType().transformWithPosition(pos, fn);
    if (!baseTy)
      return Type();

    if (baseTy.getPointer() == seq->getBaseType().getPointer())
      return *this;

    return VariadicSequenceType::get(baseTy);
  }

  case TypeKind::Dictionary: {
    auto dict = cast<DictionaryType>(base);
    auto keyTy =
        dict->getKeyType().transformWithPosition(TypePosition::Invariant, fn);
    if (!keyTy)
      return Type();

    auto valueTy = dict->getValueType().transformWithPosition(pos, fn);
    if (!valueTy)
      return Type();

    if (keyTy.getPointer() == dict->getKeyType().getPointer() &&
        valueTy.getPointer() == dict->getValueType().getPointer())
      return *this;

    return DictionaryType::get(keyTy, valueTy);
  }

  case TypeKind::LValue: {
    auto lvalue = cast<LValueType>(base);
    auto objectTy = lvalue->getObjectType().transformWithPosition(
        TypePosition::Invariant, fn);
    if (!objectTy || objectTy->hasError())
      return objectTy;

    return objectTy.getPointer() == lvalue->getObjectType().getPointer() ?
      *this : LValueType::get(objectTy);
  }

  case TypeKind::InOut: {
    auto inout = cast<InOutType>(base);
    auto objectTy = inout->getObjectType().transformWithPosition(
        TypePosition::Invariant, fn);
    if (!objectTy || objectTy->hasError())
      return objectTy;
    
    return objectTy.getPointer() == inout->getObjectType().getPointer() ?
      *this : InOutType::get(objectTy);
  }

  case TypeKind::Existential: {
    auto *existential = cast<ExistentialType>(base);
    auto constraint =
        existential->getConstraintType().transformWithPosition(pos, fn);
    if (!constraint || constraint->hasError())
      return constraint;

    if (constraint.getPointer() ==
        existential->getConstraintType().getPointer())
      return *this;

    return ExistentialType::get(constraint);
  }

  case TypeKind::ProtocolComposition: {
    auto pc = cast<ProtocolCompositionType>(base);
    SmallVector<Type, 4> substMembers;
    auto members = pc->getMembers();
    bool anyChanged = false;
    for (auto member : members) {
      auto substMember = member.transformWithPosition(pos, fn);
      if (!substMember)
        return Type();

      substMembers.push_back(substMember);

      if (substMember.getPointer() != member.getPointer())
        anyChanged = true;
    }
    
    if (!anyChanged)
      return *this;
    
    return ProtocolCompositionType::get(Ptr->getASTContext(),
                                        substMembers,
                                        pc->hasExplicitAnyObject());
  }

  case TypeKind::ParameterizedProtocol: {
    auto *ppt = cast<ParameterizedProtocolType>(base);
    Type base = ppt->getBaseType();

    bool anyChanged = false;

    auto substBase = base.transformWithPosition(pos, fn);
    if (!substBase)
      return Type();

    if (substBase.getPointer() != base.getPointer())
      anyChanged = true;

    SmallVector<Type, 2> substArgs;
    for (auto arg : ppt->getArgs()) {
      auto substArg = arg.transformWithPosition(TypePosition::Invariant, fn);
      if (!substArg)
        return Type();

      substArgs.push_back(substArg);

      if (substArg.getPointer() != arg.getPointer())
        anyChanged = true;
    }

    if (!anyChanged)
      return *this;

    return ParameterizedProtocolType::get(
        Ptr->getASTContext(),
        substBase->castTo<ProtocolType>(),
        substArgs);
  }
  }
  
  llvm_unreachable("Unhandled type in transformation");
}

bool Type::findIf(llvm::function_ref<bool(Type)> pred) const {
  class Walker : public TypeWalker {
    llvm::function_ref<bool(Type)> Pred;
  public:
    explicit Walker(llvm::function_ref<bool(Type)> pred) : Pred(pred) {}

    Action walkToTypePre(Type ty) override {
      if (Pred(ty))
        return Action::Stop;
      return Action::Continue;
    }
  };

  return walk(Walker(pred));
}

TypeTraitResult TypeBase::canBeClass() {
  // Any bridgeable object type can be a class.
  if (isBridgeableObjectType())
    return TypeTraitResult::Is;

  CanType self = getCanonicalType();

  // Archetypes with a trivial layout constraint can never
  // represent a class.
  if (auto Archetype = dyn_cast<ArchetypeType>(self)) {
    if (auto Layout = Archetype->getLayoutConstraint()) {
      if (Layout->isTrivial())
        return TypeTraitResult::IsNot;
      if (Layout->isClass())
        return TypeTraitResult::Is;
    }
  }

  // Dependent types might be bound to classes.
  if (isa<SubstitutableType>(self))
    return TypeTraitResult::CanBe;
  if (isa<DependentMemberType>(self))
    return TypeTraitResult::CanBe;
  
  return TypeTraitResult::IsNot;
}

bool Type::isPrivateStdlibType(bool treatNonBuiltinProtocolsAsPublic) const {
  Type Ty = *this;
  if (!Ty)
    return false;

  if (auto existential = dyn_cast<ExistentialType>(Ty.getPointer()))
    return existential->getConstraintType()
        .isPrivateStdlibType(treatNonBuiltinProtocolsAsPublic);

  // A 'public' typealias can have an 'internal' type.
  if (auto *NAT = dyn_cast<TypeAliasType>(Ty.getPointer())) {
    auto *AliasDecl = NAT->getDecl();
    if (auto parent = NAT->getParent()) {
      if (parent.isPrivateStdlibType(treatNonBuiltinProtocolsAsPublic))
        return true;
    }

    if (AliasDecl->isPrivateStdlibDecl(treatNonBuiltinProtocolsAsPublic))
      return true;

    return Type(NAT->getSinglyDesugaredType()).isPrivateStdlibType(
                                            treatNonBuiltinProtocolsAsPublic);
  }

  if (auto Paren = dyn_cast<ParenType>(Ty.getPointer())) {
    Type Underlying = Paren->getUnderlyingType();
    return Underlying.isPrivateStdlibType(treatNonBuiltinProtocolsAsPublic);
  }

  if (Type Unwrapped = Ty->getOptionalObjectType())
    return Unwrapped.isPrivateStdlibType(treatNonBuiltinProtocolsAsPublic);

  if (auto TyD = Ty->getAnyNominal())
    if (TyD->isPrivateStdlibDecl(treatNonBuiltinProtocolsAsPublic))
      return true;

  return false;
}

// NOTE: If you add a new SOMETIMES_LOADABLE_CHECKED_REF_STORAGE type, then you
// may or may not want to emulate what 'unowned' does.
bool UnownedStorageType::isLoadable(ResilienceExpansion resilience) const {
  auto ty = getReferentType();
  if (auto underlyingTy = ty->getOptionalObjectType())
    ty = underlyingTy;
  return ty->getReferenceCounting() == ReferenceCounting::Native;
}

ReferenceCounting TypeBase::getReferenceCounting() {
  CanType type = getCanonicalType();
  ASTContext &ctx = type->getASTContext();

  if (isForeignReferenceType())
    return lookThroughAllOptionalTypes()
                   ->getClassOrBoundGenericClass()
                   ->hasRefCountingAnnotations()
               ? ReferenceCounting::Custom
               : ReferenceCounting::None;

  // In the absence of Objective-C interoperability, everything uses native
  // reference counting or is the builtin BridgeObject.
  if (!ctx.LangOpts.EnableObjCInterop) {
    return type->getKind() == TypeKind::BuiltinBridgeObject
             ? ReferenceCounting::Bridge
             : ReferenceCounting::Native;
  }

  switch (type->getKind()) {
#define SUGARED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
    llvm_unreachable("sugared canonical type?");

  case TypeKind::BuiltinNativeObject:
  case TypeKind::SILBox:
    return ReferenceCounting::Native;

  case TypeKind::BuiltinBridgeObject:
    return ReferenceCounting::Bridge;

  case TypeKind::Class:
    return cast<ClassType>(type)->getDecl()->getObjectModel();
  case TypeKind::BoundGenericClass:
    return cast<BoundGenericClassType>(type)->getDecl()->getObjectModel();
  case TypeKind::UnboundGeneric:
    return cast<ClassDecl>(cast<UnboundGenericType>(type)->getDecl())
        ->getObjectModel();

  case TypeKind::DynamicSelf:
    return cast<DynamicSelfType>(type).getSelfType()
        ->getReferenceCounting();
  case TypeKind::SILMoveOnlyWrapped:
    return cast<SILMoveOnlyWrappedType>(type)
        ->getInnerType()
        ->getReferenceCounting();

  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::PackArchetype:
  case TypeKind::ElementArchetype: {
    auto archetype = cast<ArchetypeType>(type);
    auto layout = archetype->getLayoutConstraint();
    (void)layout;
    assert(layout && layout->isRefCounted());
    if (auto supertype = archetype->getSuperclass())
      return supertype->getReferenceCounting();
    return ReferenceCounting::Unknown;
  }

  case TypeKind::Protocol:
  case TypeKind::ProtocolComposition: {
    auto layout = type->getExistentialLayout();
    assert(layout.requiresClass() && "Opaque existentials don't use refcounting");
    if (auto superclass = layout.getSuperclass())
      return superclass->getReferenceCounting();
    return ReferenceCounting::Unknown;
  }

  case TypeKind::ParameterizedProtocol: {
    return cast<ParameterizedProtocolType>(this)
      ->getBaseType()
      ->getReferenceCounting();
  }

  case TypeKind::Existential:
    return cast<ExistentialType>(type)->getConstraintType()
        ->getReferenceCounting();

  case TypeKind::Function:
  case TypeKind::GenericFunction:
  case TypeKind::SILFunction:
  case TypeKind::SILBlockStorage:
  case TypeKind::Error:
  case TypeKind::Unresolved:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinIntegerLiteral:
  case TypeKind::BuiltinFloat:
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinRawUnsafeContinuation:
  case TypeKind::BuiltinJob:
  case TypeKind::BuiltinExecutor:
  case TypeKind::BuiltinDefaultActorStorage:
  case TypeKind::BuiltinNonDefaultDistributedActorStorage:
  case TypeKind::BuiltinPackIndex:
  case TypeKind::BuiltinUnsafeValueBuffer:
  case TypeKind::BuiltinVector:
  case TypeKind::Tuple:
  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Metatype:
  case TypeKind::ExistentialMetatype:
  case TypeKind::Module:
  case TypeKind::LValue:
  case TypeKind::InOut:
  case TypeKind::TypeVariable:
  case TypeKind::Placeholder:
  case TypeKind::BoundGenericEnum:
  case TypeKind::BoundGenericStruct:
  case TypeKind::SILToken:
  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
  case TypeKind::Pack:
  case TypeKind::PackExpansion:
  case TypeKind::PackElement:
  case TypeKind::SILPack:
  case TypeKind::BuiltinTuple:
#define REF_STORAGE(Name, ...) \
  case TypeKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
    llvm_unreachable("type is not a class reference");
  }

  llvm_unreachable("Unhandled type kind!");
}

//
// SILBoxType implementation
//

void SILBoxType::Profile(llvm::FoldingSetNodeID &id, SILLayout *Layout,
                         SubstitutionMap Substitutions) {
  id.AddPointer(Layout);
  Substitutions.profile(id);
}

static RecursiveTypeProperties getBoxRecursiveProperties(
    SILLayout *Layout, SubstitutionMap subMap) {
  RecursiveTypeProperties props;
  for (auto &field : Layout->getFields()) {
    auto fieldProps = field.getLoweredType()->getRecursiveProperties();
    fieldProps.removeHasTypeParameter();
    fieldProps.removeHasDependentMember();
    props |= fieldProps;
  }
  for (auto replacementType : subMap.getReplacementTypes()) {
    if (replacementType) props |= replacementType->getRecursiveProperties();
  }
  return props;
}

SILBoxType::SILBoxType(ASTContext &C,
                       SILLayout *Layout, SubstitutionMap Substitutions)
  : TypeBase(TypeKind::SILBox, &C,
             getBoxRecursiveProperties(Layout, Substitutions)),
    Layout(Layout), Substitutions(Substitutions) {
  assert(Substitutions.isCanonical());
}

Type TypeBase::openAnyExistentialType(OpenedArchetypeType *&opened,
                                      GenericSignature parentSig) {
  assert(isAnyExistentialType());
  if (auto metaty = getAs<ExistentialMetatypeType>()) {
    opened = OpenedArchetypeType::get(
        metaty->getExistentialInstanceType()->getCanonicalType(),
        parentSig.getCanonicalSignature());
    if (metaty->hasRepresentation())
      return MetatypeType::get(opened, metaty->getRepresentation());
    else
      return MetatypeType::get(opened);
  }
  opened = OpenedArchetypeType::get(getCanonicalType(),
                                    parentSig.getCanonicalSignature());
  return opened;
}

CanType swift::substOpaqueTypesWithUnderlyingTypes(CanType ty,
                                                   TypeExpansionContext context,
                                                   bool allowLoweredTypes) {
  if (!context.shouldLookThroughOpaqueTypeArchetypes() ||
      !ty->hasOpaqueArchetype())
    return ty;

  ReplaceOpaqueTypesWithUnderlyingTypes replacer(
      context.getContext(), context.getResilienceExpansion(),
      context.isWholeModuleContext());
  SubstOptions flags = SubstFlags::SubstituteOpaqueArchetypes;
  if (allowLoweredTypes)
    flags =
        SubstFlags::SubstituteOpaqueArchetypes | SubstFlags::AllowLoweredTypes;
  return ty.subst(replacer, replacer, flags)->getCanonicalType();
}

AnyFunctionType *AnyFunctionType::getWithoutDifferentiability() const {
  SmallVector<Param, 8> newParams;
  for (auto &param : getParams()) {
    Param newParam(param.getPlainType(), param.getLabel(),
                   param.getParameterFlags().withNoDerivative(false));
    newParams.push_back(newParam);
  }
  auto nonDiffExtInfo =
      getExtInfo()
          .intoBuilder()
          .withDifferentiabilityKind(DifferentiabilityKind::NonDifferentiable)
          .build();
  if (isa<FunctionType>(this))
    return FunctionType::get(newParams, getResult(), nonDiffExtInfo);
  assert(isa<GenericFunctionType>(this));
  return GenericFunctionType::get(getOptGenericSignature(), newParams,
                                  getResult(), nonDiffExtInfo);
}

AnyFunctionType *AnyFunctionType::getWithoutThrowing() const {
  auto info = getExtInfo().intoBuilder().withThrows(false).build();
  return withExtInfo(info);
}

Optional<TangentSpace>
TypeBase::getAutoDiffTangentSpace(LookupConformanceFn lookupConformance) {
  assert(lookupConformance);
  auto &ctx = getASTContext();

  Type cacheKey = this;
  auto lookup = ctx.AutoDiffTangentSpaces.find(cacheKey);
  if (lookup != ctx.AutoDiffTangentSpaces.end())
    return lookup->getSecond();
  auto cache = [&](Optional<TangentSpace> tangentSpace) {
    ctx.AutoDiffTangentSpaces.insert({cacheKey, tangentSpace});
    return tangentSpace;
  };

  // For tuple types: the tangent space is a tuple of the elements'  tangent
  // space types, for the elements that have a tangent space.
  if (auto *tupleTy = getAs<TupleType>()) {
    SmallVector<TupleTypeElt, 8> newElts;
    for (auto elt : tupleTy->getElements()) {
      auto eltSpace = elt.getType()->getAutoDiffTangentSpace(lookupConformance);
      if (!eltSpace)
        continue;
      newElts.push_back(elt.getWithType(eltSpace->getType()));
    }
    if (newElts.empty())
      return cache(
          TangentSpace::getTuple(ctx.TheEmptyTupleType->castTo<TupleType>()));
    if (newElts.size() == 1)
      return cache(TangentSpace::getTangentVector(newElts.front().getType()));
    auto *tupleType = TupleType::get(newElts, ctx);
    return cache(TangentSpace::getTuple(tupleType));
  }

  // For `Differentiable`-conforming types: the tangent space is the
  // `TangentVector` associated type.
  auto *differentiableProtocol =
      ctx.getProtocol(KnownProtocolKind::Differentiable);
  if (!differentiableProtocol)
    return cache(None);
  auto associatedTypeLookup =
      differentiableProtocol->lookupDirect(ctx.Id_TangentVector);
  assert(associatedTypeLookup.size() == 1);
  auto *dependentType = DependentMemberType::get(
      differentiableProtocol->getDeclaredInterfaceType(),
      cast<AssociatedTypeDecl>(associatedTypeLookup[0]));

  // Try to get the `TangentVector` associated type of `base`.
  // Return the associated type if it is valid.
  auto assocTy = dependentType->substBaseType(this, lookupConformance);
  if (!assocTy->hasError())
    return cache(TangentSpace::getTangentVector(assocTy));

  // Otherwise, there is no associated tangent space. Return `None`.
  return cache(None);
}

bool TypeBase::isForeignReferenceType() {
  if (auto *classDecl = lookThroughAllOptionalTypes()->getClassOrBoundGenericClass())
    return classDecl->isForeignReferenceType();
  return false;
}

bool TypeBase::hasSimpleTypeRepr() const {
  // NOTE: Please keep this logic in sync with TypeRepr::isSimple().
  switch (getKind()) {
  case TypeKind::Function:
  case TypeKind::GenericFunction:
    return false;

  case TypeKind::Metatype:
    return !cast<const AnyMetatypeType>(this)->hasRepresentation();

  case TypeKind::ExistentialMetatype:
  case TypeKind::Existential:
    return false;

  case TypeKind::OpaqueTypeArchetype:
  case TypeKind::OpenedArchetype:
    return false;

  case TypeKind::PrimaryArchetype: {
    auto archetype = cast<const PrimaryArchetypeType>(this);
    auto interface = archetype->getInterfaceType();
    return interface->hasSimpleTypeRepr();
  }

  case TypeKind::ProtocolComposition: {
    // 'Any', 'AnyObject' and single protocol compositions are simple
    auto composition = cast<const ProtocolCompositionType>(this);
    auto memberCount = composition->getMembers().size();
    if (composition->hasExplicitAnyObject())
      return memberCount == 0;
    return memberCount <= 1;
  }

  case TypeKind::GenericTypeParam: {
    if (auto *decl = cast<const GenericTypeParamType>(this)->getDecl()) {
      return !decl->isOpaqueType();
    }

    return true;
  }

  default:
    return true;
  }
}

bool CanType::isForeignReferenceType() {
  if (auto *classDecl = getPointer()->lookThroughAllOptionalTypes()->getClassOrBoundGenericClass())
    return classDecl->isForeignReferenceType();
  return false;
}

// Creates an `AnyFunctionType` from the given parameters, result type,
// generic signature, and `ExtInfo`.
static AnyFunctionType *
makeFunctionType(ArrayRef<AnyFunctionType::Param> parameters, Type resultType,
                 GenericSignature genericSignature,
                 AnyFunctionType::ExtInfo extInfo) {
  if (genericSignature)
    return GenericFunctionType::get(genericSignature, parameters, resultType,
                                    extInfo);
  return FunctionType::get(parameters, resultType, extInfo);
}

AnyFunctionType *AnyFunctionType::getAutoDiffDerivativeFunctionType(
    IndexSubset *parameterIndices, AutoDiffDerivativeFunctionKind kind,
    LookupConformanceFn lookupConformance, GenericSignature derivativeGenSig,
    bool makeSelfParamFirst) {
  assert(!parameterIndices->isEmpty() &&
         "Expected at least one differentiability parameter");
  auto &ctx = getASTContext();

  // If `derivativeGenSig` is not defined, use the current function's type
  // generic signature.
  if (!derivativeGenSig)
    derivativeGenSig = getOptGenericSignature();

  // Unwrap curry levels. At most, two parameter lists are necessary, for
  // curried method types with a `(Self)` parameter list.
  // TODO(TF-874): Simplify curry level logic.
  SmallVector<AnyFunctionType *, 2> curryLevels;
  auto *currentLevel = castTo<AnyFunctionType>();
  for (unsigned i : range(2)) {
    (void)i;
    if (currentLevel == nullptr)
      break;
    curryLevels.push_back(currentLevel);
    currentLevel = currentLevel->getResult()->getAs<AnyFunctionType>();
  }

  auto originalResult = curryLevels.back()->getResult();

  auto linearMapTypeExpected = getAutoDiffDerivativeFunctionLinearMapType(
      parameterIndices, kind.getLinearMapKind(), lookupConformance,
      makeSelfParamFirst);
  assert(linearMapTypeExpected && "Linear map type is invalid");
  Type linearMapType = linearMapTypeExpected.get();

  // Build the full derivative function type: `(T...) -> (R, LinearMapType)`.
  SmallVector<TupleTypeElt, 2> retElts;
  retElts.push_back(originalResult);
  retElts.push_back(linearMapType);
  auto retTy = TupleType::get(retElts, ctx);
  auto *derivativeFunctionType =
      makeFunctionType(curryLevels.back()->getParams(), retTy,
                       curryLevels.size() == 1 ? derivativeGenSig : nullptr,
                       curryLevels.back()->getExtInfo());

  // Wrap the derivative function type in additional curry levels.
  auto curryLevelsWithoutLast =
      ArrayRef<AnyFunctionType *>(curryLevels).drop_back(1);
  for (auto pair : enumerate(llvm::reverse(curryLevelsWithoutLast))) {
    unsigned i = pair.index();
    auto *curryLevel = pair.value();
    derivativeFunctionType = makeFunctionType(
        curryLevel->getParams(), derivativeFunctionType,
        i == curryLevelsWithoutLast.size() - 1 ? derivativeGenSig : nullptr,
        curryLevel->getExtInfo());
  }

  return derivativeFunctionType;
}

llvm::Expected<AnyFunctionType *>
AnyFunctionType::getAutoDiffDerivativeFunctionLinearMapType(
    IndexSubset *parameterIndices, AutoDiffLinearMapKind kind,
    LookupConformanceFn lookupConformance, bool makeSelfParamFirst) {
  auto &ctx = getASTContext();
  // Error if differentiability parameter indices are empty.
  if (parameterIndices->isEmpty())
    return llvm::make_error<DerivativeFunctionTypeError>(
        this, DerivativeFunctionTypeError::Kind::NoDifferentiabilityParameters);

  // Get differentiability parameters.
  SmallVector<AnyFunctionType::Param, 8> diffParams;
  getSubsetParameters(parameterIndices, diffParams,
                      /*reverseCurryLevels*/ !makeSelfParamFirst);

  // Get the original semantic result type.
  SmallVector<AutoDiffSemanticFunctionResultType, 1> originalResults;
  autodiff::getFunctionSemanticResultTypes(this, originalResults);
  // Error if no original semantic results.
  if (originalResults.empty())
    return llvm::make_error<DerivativeFunctionTypeError>(
        this, DerivativeFunctionTypeError::Kind::NoSemanticResults);
  // Error if multiple original semantic results.
  // TODO(TF-1250): Support functions with multiple semantic results.
  if (originalResults.size() > 1)
    return llvm::make_error<DerivativeFunctionTypeError>(
        this, DerivativeFunctionTypeError::Kind::MultipleSemanticResults);
  auto originalResult = originalResults.front();
  auto originalResultType = originalResult.type;

  // Get the original semantic result type's `TangentVector` associated type.
  auto resultTan =
      originalResultType->getAutoDiffTangentSpace(lookupConformance);
  // Error if original semantic result has no tangent space.
  if (!resultTan) {
    return llvm::make_error<DerivativeFunctionTypeError>(
        this, DerivativeFunctionTypeError::Kind::NonDifferentiableResult,
        std::make_pair(originalResultType, /*index*/ 0));
  }
  auto resultTanType = resultTan->getType();

  // Compute the result linear map function type.
  FunctionType *linearMapType;
  switch (kind) {
  case AutoDiffLinearMapKind::Differential: {
    // Compute the differential type, returned by JVP functions.
    //
    // Case 1: original function has no `inout` parameters.
    // - Original:     `(T0, T1, ...) -> R`
    // - Differential: `(T0.Tan, T1.Tan, ...) -> R.Tan`
    //
    // Case 2: original function has a non-wrt `inout` parameter.
    // - Original:      `(T0, inout T1, ...) -> Void`
    // - Differential: `(T0.Tan, ...) -> T1.Tan`
    //
    // Case 3: original function has a wrt `inout` parameter.
    // - Original:     `(T0, inout T1, ...) -> Void`
    // - Differential: `(T0.Tan, inout T1.Tan, ...) -> Void`
    SmallVector<AnyFunctionType::Param, 4> differentialParams;
    bool hasInoutDiffParameter = false;
    for (auto i : range(diffParams.size())) {
      auto diffParam = diffParams[i];
      auto paramType = diffParam.getPlainType();
      auto paramTan = paramType->getAutoDiffTangentSpace(lookupConformance);
      // Error if parameter has no tangent space.
      if (!paramTan) {
        return llvm::make_error<DerivativeFunctionTypeError>(
            this,
            DerivativeFunctionTypeError::Kind::
                NonDifferentiableDifferentiabilityParameter,
            std::make_pair(paramType, i));
      }
      differentialParams.push_back(AnyFunctionType::Param(
          paramTan->getType(), Identifier(), diffParam.getParameterFlags()));
      if (diffParam.isInOut())
        hasInoutDiffParameter = true;
    }
    auto differentialResult =
        hasInoutDiffParameter ? Type(ctx.TheEmptyTupleType) : resultTanType;
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo info;
    linearMapType =
        FunctionType::get(differentialParams, differentialResult, info);
    break;
  }
  case AutoDiffLinearMapKind::Pullback: {
    // Compute the pullback type, returned by VJP functions.
    //
    // Case 1: original function has no `inout` parameters.
    // - Original: `(T0, T1, ...) -> R`
    // - Pullback: `R.Tan -> (T0.Tan, T1.Tan, ...)`
    //
    // Case 2: original function has a non-wrt `inout` parameter.
    // - Original: `(T0, inout T1, ...) -> Void`
    // - Pullback: `(T1.Tan) -> (T0.Tan, ...)`
    //
    // Case 3: original function has a wrt `inout` parameter.
    // - Original: `(T0, inout T1, ...) -> Void`
    // - Pullback: `(inout T1.Tan) -> (T0.Tan, ...)`
    SmallVector<TupleTypeElt, 4> pullbackResults;
    bool hasInoutDiffParameter = false;
    for (auto i : range(diffParams.size())) {
      auto diffParam = diffParams[i];
      auto paramType = diffParam.getPlainType();
      auto paramTan = paramType->getAutoDiffTangentSpace(lookupConformance);
      // Error if parameter has no tangent space.
      if (!paramTan) {
        return llvm::make_error<DerivativeFunctionTypeError>(
            this,
            DerivativeFunctionTypeError::Kind::
                NonDifferentiableDifferentiabilityParameter,
            std::make_pair(paramType, i));
      }
      if (diffParam.isInOut()) {
        hasInoutDiffParameter = true;
        continue;
      }
      pullbackResults.emplace_back(paramTan->getType());
    }
    Type pullbackResult;
    if (pullbackResults.empty()) {
      pullbackResult = ctx.TheEmptyTupleType;
    } else if (pullbackResults.size() == 1) {
      pullbackResult = pullbackResults.front().getType();
    } else {
      pullbackResult = TupleType::get(pullbackResults, ctx);
    }
    auto flags = ParameterTypeFlags().withInOut(hasInoutDiffParameter);
    auto pullbackParam =
        AnyFunctionType::Param(resultTanType, Identifier(), flags);
    // FIXME: Verify ExtInfo state is correct, not working by accident.
    FunctionType::ExtInfo info;
    linearMapType = FunctionType::get({pullbackParam}, pullbackResult, info);
    break;
  }
  }
  assert(linearMapType && "Expected linear map type");
  return linearMapType;
}

CanSILFunctionType
SILFunctionType::withInvocationSubstitutions(SubstitutionMap subs) const {
  subs = subs.getCanonical();
  if (subs == getInvocationSubstitutions())
    return CanSILFunctionType(const_cast<SILFunctionType*>(this));

  assert(!subs || CanGenericSignature(subs.getGenericSignature())
                    == getInvocationGenericSignature());
  return SILFunctionType::get(getInvocationGenericSignature(),
                          getExtInfo(), getCoroutineKind(),
                          getCalleeConvention(),
                          getParameters(), getYields(), getResults(),
                          getOptionalErrorResult(),
                          getPatternSubstitutions(), subs,
                          const_cast<SILFunctionType*>(this)->getASTContext(),
                          getWitnessMethodConformanceOrInvalid());
}

CanSILFunctionType
SILFunctionType::withPatternSubstitutions(SubstitutionMap subs) const {
  subs = subs.getCanonical();
  if (subs == getPatternSubstitutions())
    return CanSILFunctionType(const_cast<SILFunctionType*>(this));

  assert(!subs || CanGenericSignature(subs.getGenericSignature())
                    == getPatternGenericSignature());
  return SILFunctionType::get(getInvocationGenericSignature(),
                          getExtInfo(), getCoroutineKind(),
                          getCalleeConvention(),
                          getParameters(), getYields(), getResults(),
                          getOptionalErrorResult(),
                          subs, getInvocationSubstitutions(),
                          const_cast<SILFunctionType*>(this)->getASTContext(),
                          getWitnessMethodConformanceOrInvalid());
}

CanSILFunctionType
SILFunctionType::withPatternSpecialization(CanGenericSignature sig,
                                           SubstitutionMap subs,
                                           ProtocolConformanceRef
                                             witnessConformance) const {
  assert(!hasInvocationSubstitutions());
  subs = subs.getCanonical();
  assert(!subs || CanGenericSignature(subs.getGenericSignature())
                    == getSubstGenericSignature());
  return SILFunctionType::get(sig,
                          getExtInfo(), getCoroutineKind(),
                          getCalleeConvention(),
                          getParameters(), getYields(), getResults(),
                          getOptionalErrorResult(),
                          subs, SubstitutionMap(),
                          const_cast<SILFunctionType*>(this)->getASTContext(),
                          witnessConformance);
}

SourceLoc swift::extractNearestSourceLoc(Type ty) {
  if (auto nominal = ty->getAnyNominal())
    return extractNearestSourceLoc(nominal);

  return SourceLoc();
}
