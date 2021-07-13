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
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <iterator>
using namespace swift;

#define TYPE(Id, _) \
  static_assert(IsTriviallyDestructible<Id##Type>::value, \
                "Types are BumpPtrAllocated; the destructor is never called");
#include "swift/AST/TypeNodes.def"

Type QueryTypeSubstitutionMap::operator()(SubstitutableType *type) const {
  auto key = type->getCanonicalType()->castTo<SubstitutableType>();
  auto known = substitutions.find(key);
  if (known != substitutions.end() && known->second)
    return known->second;

  // Not known.
  return Type();
}

Type
QueryTypeSubstitutionMapOrIdentity::operator()(SubstitutableType *type) const {
  auto key = type->getCanonicalType()->castTo<SubstitutableType>();
  auto known = substitutions.find(key);
  if (known != substitutions.end() && known->second)
    return known->second;
  
  return type;
}

Type QuerySubstitutionMap::operator()(SubstitutableType *type) const {
  auto key = cast<SubstitutableType>(type->getCanonicalType());
  return subMap.lookupSubstitution(key);
}

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

// Only allow allocation of Types using the allocator in ASTContext.
void *TypeBase::operator new(size_t bytes, const ASTContext &ctx,
                             AllocationArena arena, unsigned alignment) {
  return ctx.Allocate(bytes, alignment, arena);
}

NominalTypeDecl *CanType::getAnyNominal() const {
  return dyn_cast_or_null<NominalTypeDecl>(getAnyGeneric());
}

GenericTypeDecl *CanType::getAnyGeneric() const {
  if (auto Ty = dyn_cast<AnyGenericType>(*this))
    return Ty->getDecl();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Various Type Methods.
//===----------------------------------------------------------------------===//

/// isEqual - Return true if these two types are equal, ignoring sugar.
bool TypeBase::isEqual(Type Other) {
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
  return isEqual(getASTContext().TheAnyType);
}

bool TypeBase::isPlaceholder() {
  return getCanonicalType()->is<PlaceholderType>();
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

  // Archetypes and existentials are only class references if class-bounded.
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::NestedArchetype:
  case TypeKind::OpaqueTypeArchetype:
    return cast<ArchetypeType>(type)->requiresClass();
  case TypeKind::Protocol:
    return cast<ProtocolType>(type)->requiresClass();
  case TypeKind::ProtocolComposition:
    return cast<ProtocolCompositionType>(type)->requiresClass();

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
#define REF_STORAGE(Name, ...) \
  case TypeKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
    return false;

  case TypeKind::GenericTypeParam:
  case TypeKind::DependentMember:
    assert(sig && "dependent types can't answer reference semantics query");
    return sig->requiresClass(type);
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

ExistentialLayout::ExistentialLayout(ProtocolType *type) {
  assert(type->isCanonical());

  auto *protoDecl = type->getDecl();

  hasExplicitAnyObject = false;
  containsNonObjCProtocol = !protoDecl->isObjC();

  singleProtocol = type;
}

ExistentialLayout::ExistentialLayout(ProtocolCompositionType *type) {
  assert(type->isCanonical());

  hasExplicitAnyObject = type->hasExplicitAnyObject();
  containsNonObjCProtocol = false;

  auto members = type->getMembers();
  if (!members.empty() &&
      isa<ClassDecl>(members[0]->getAnyNominal())) {
    explicitSuperclass = members[0];
    members = members.slice(1);
  }

  for (auto member : members) {
    auto *protoDecl = member->castTo<ProtocolType>()->getDecl();
    containsNonObjCProtocol |= !protoDecl->isObjC();
  }

  singleProtocol = nullptr;
  protocols = { members.data(), members.size() };
}


ExistentialLayout TypeBase::getExistentialLayout() {
  return getCanonicalType().getExistentialLayout();
}

ExistentialLayout CanType::getExistentialLayout() {
  if (auto proto = dyn_cast<ProtocolType>(*this))
    return ExistentialLayout(proto);

  auto comp = cast<ProtocolCompositionType>(*this);
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

  for (auto proto : getProtocols()) {
    // If we have a generic signature, check there, because it
    // will pick up superclass constraints from protocols that we
    // refine as well.
    auto *protoDecl = proto->getDecl();
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

bool TypeBase::isActorType() {
  // Nominal types: check whether the declaration is an actor.
  if (auto nominal = getAnyNominal())
    return nominal->isActor();

  // Archetypes check for conformance to Actor.
  if (auto archetype = getAs<ArchetypeType>()) {
    auto actorProto = getASTContext().getProtocol(KnownProtocolKind::Actor);
    if (!actorProto)
      return false;

    auto interfaceType = archetype->getInterfaceType();
    auto genericEnv = archetype->getGenericEnvironment();
    return genericEnv->getGenericSignature()->requiresProtocol(
        interfaceType, actorProto);
  }

  // Existential types: check for Actor protocol.
  if (isExistentialType()) {
    auto actorProto = getASTContext().getProtocol(KnownProtocolKind::Actor);
    if (!actorProto)
      return false;

    auto layout = getExistentialLayout();
    if (auto superclass = layout.getSuperclass()) {
      if (superclass->isActorType())
        return true;
    }

    for (auto proto : layout.getProtocols()) {
      if (proto->isActorType())
        return true;
    }

    return false;
  }

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

bool TypeBase::hasOpenedExistential(OpenedArchetypeType *opened) {
  if (!hasOpenedExistential())
    return false;

  return getCanonicalType().findIf([&](Type type) -> bool {
    return opened == dyn_cast<OpenedArchetypeType>(type.getPointer());
  });
}

void TypeBase::getOpenedExistentials(
       SmallVectorImpl<OpenedArchetypeType *> &opened) {
  if (!hasOpenedExistential())
    return;

  SmallPtrSet<ArchetypeType *, 4> known;
  getCanonicalType().findIf([&](Type type) -> bool {
    auto archetype = dyn_cast<OpenedArchetypeType>(type.getPointer());
    if (!archetype)
      return false;

    if (known.insert(archetype).second)
      opened.push_back(archetype);

    return false;
  });
}

Type TypeBase::eraseOpenedExistential(OpenedArchetypeType *opened) {
  if (!hasOpenedExistential())
    return Type(this);

  auto existentialType = opened->getOpenedExistentialType();

  return Type(this).transform([&](Type t) -> Type {
    // A metatype with an opened existential type becomes an
    // existential metatype.
    if (auto *metatypeType = dyn_cast<MetatypeType>(t.getPointer())) {
      auto instanceType = metatypeType->getInstanceType();
      if (instanceType->hasOpenedExistential()) {
        instanceType = instanceType->eraseOpenedExistential(opened);
        return ExistentialMetatypeType::get(instanceType);
      }
    }

    // @opened P => P
    if (auto *archetypeType = dyn_cast<ArchetypeType>(t.getPointer())) {
      if (archetypeType == opened)
        return existentialType;
    }

    return t;
  });
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
      if (!isLegalSILType(eltType)) return false;
    }
    return true;
  }

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

bool TypeBase::isAnyObject() {
  auto canTy = getCanonicalType();

  if (!canTy.isExistentialType())
    return false;

  return canTy.getExistentialLayout().isAnyObject();
}

bool ExistentialLayout::isErrorExistential() const {
  auto protocols = getProtocols();
  return (!hasExplicitAnyObject &&
          !explicitSuperclass &&
          protocols.size() == 1 &&
          protocols[0]->getDecl()->isSpecificProtocol(KnownProtocolKind::Error));
}

bool ExistentialLayout::isExistentialWithError(ASTContext &ctx) const {
  auto errorProto = ctx.getProtocol(KnownProtocolKind::Error);
  if (!errorProto) return false;

  for (auto proto : getProtocols()) {
    auto *protoDecl = proto->getDecl();
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

bool TypeBase::isStdlibType() {
  if (auto *NTD = getAnyNominal()) {
    auto *DC = NTD->getDeclContext();
    return DC->isModuleScopeContext() &&
           DC->getParentModule()->isStdlibModule();
  }
  return false;
}

bool TypeBase::isCGFloatType() {
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
          module->getName().is("Foundation")) &&
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

/// Determine whether the parameter is contextually Sendable.
static bool isParamUnsafeSendable(const ParamDecl *param) {
  // Check for @_unsafeSendable.
  if (param->getAttrs().hasAttribute<UnsafeSendableAttr>())
    return true;

  // Check that the parameter is of function type.
  Type paramType = param->isVariadic() ? param->getVarargBaseTy()
                                       : param->getInterfaceType();
  paramType = paramType->getRValueType()->lookThroughAllOptionalTypes();
  if (!paramType->is<FunctionType>())
    return false;

  // Check whether this function is known to have @_unsafeSendable function
  // parameters.
  auto func = dyn_cast<AbstractFunctionDecl>(param->getDeclContext());
  if (!func)
    return false;

  return func->hasKnownUnsafeSendableFunctionParams();
}

ParameterListInfo::ParameterListInfo(
    ArrayRef<AnyFunctionType::Param> params,
    const ValueDecl *paramOwner,
    bool skipCurriedSelf) {
  defaultArguments.resize(params.size());
  propertyWrappers.resize(params.size());
  unsafeSendable.resize(params.size());
  unsafeMainActor.resize(params.size());
  implicitSelfCapture.resize(params.size());
  inheritActorContext.resize(params.size());

  // No parameter owner means no parameter list means no default arguments
  // - hand back the zeroed bitvector.
  //
  // FIXME: We ought to not request paramer list info in this case.
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
  // unlabled trailing closures.
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

    if (isParamUnsafeSendable(param)) {
      unsafeSendable.set(i);
    }

    if (param->getAttrs().hasAttribute<UnsafeMainActorAttr>()) {
      unsafeMainActor.set(i);
    }

    if (param->getAttrs().hasAttribute<ImplicitSelfCaptureAttr>()) {
      implicitSelfCapture.set(i);
    }

    if (param->getAttrs().hasAttribute<InheritActorContextAttr>()) {
      inheritActorContext.set(i);
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

bool ParameterListInfo::isUnsafeSendable(unsigned paramIdx) const {
  return paramIdx < unsafeSendable.size()
      ? unsafeSendable[paramIdx]
      : false;
}

bool ParameterListInfo::isUnsafeMainActor(unsigned paramIdx) const {
  return paramIdx < unsafeMainActor.size()
      ? unsafeMainActor[paramIdx]
      : false;
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
  return unsafeSendable.any() || unsafeMainActor.any() ||
      implicitSelfCapture.any() || inheritActorContext.any();
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
  if (auto metaTy = getAs<AnyMetatypeType>())
    return metaTy->getInstanceType();

  return this;
}

/// Collect the protocols in the existential type T into the given
/// vector.
static void addProtocols(Type T,
                         SmallVectorImpl<ProtocolDecl *> &Protocols,
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
      addProtocols(P, Protocols, Superclass, HasExplicitAnyObject);
    return;
  }

  assert(isa<ClassDecl>(T->getAnyNominal()) && "Non-class, non-protocol "
         "member in protocol composition");
  assert((!Superclass || Superclass->isEqual(T)) &&
         "Should have diagnosed multiple superclasses by now");
  Superclass = T;
}

/// Add the protocol (or protocols) in the type T to the stack of
/// protocols, checking whether any of the protocols had already been seen and
/// zapping those in the original list that we find again.
static void
addMinimumProtocols(Type T, SmallVectorImpl<ProtocolDecl *> &Protocols,
                    llvm::SmallDenseMap<ProtocolDecl *, unsigned> &Known,
                    llvm::SmallPtrSetImpl<ProtocolDecl *> &Visited,
                    SmallVector<ProtocolDecl *, 16> &Stack, bool &ZappedAny) {
  if (auto Proto = T->getAs<ProtocolType>()) {
    auto KnownPos = Known.find(Proto->getDecl());
    if (KnownPos != Known.end()) {
      // We've come across a protocol that is in our original list. Zap it.
      Protocols[KnownPos->second] = nullptr;
      ZappedAny = true;
    }

    if (Visited.insert(Proto->getDecl()).second) {
      Stack.push_back(Proto->getDecl());
      for (auto Inherited : Proto->getDecl()->getInheritedProtocols())
        addMinimumProtocols(Inherited->getDeclaredInterfaceType(),
                            Protocols, Known, Visited, Stack, ZappedAny);
    }
    return;
  }
  
  if (auto PC = T->getAs<ProtocolCompositionType>()) {
    for (auto C : PC->getMembers()) {
      addMinimumProtocols(C, Protocols, Known, Visited, Stack, ZappedAny);
    }
  }
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

void ProtocolType::canonicalizeProtocols(
       SmallVectorImpl<ProtocolDecl *> &protocols) {
  llvm::SmallDenseMap<ProtocolDecl *, unsigned> known;
  llvm::SmallPtrSet<ProtocolDecl *, 16> visited;
  SmallVector<ProtocolDecl *, 16> stack;
  bool zappedAny = false;

  // Seed the stack with the protocol declarations in the original list.
  // Zap any obvious duplicates along the way.
  for (unsigned I = 0, N = protocols.size(); I != N; ++I) {
    // Check whether we've seen this protocol before.
    auto knownPos = known.find(protocols[I]);
    
    // If we have not seen this protocol before, record its index.
    if (knownPos == known.end()) {
      known[protocols[I]] = I;
      stack.push_back(protocols[I]);
      continue;
    }
    
    // We have seen this protocol before; zap this occurrence.
    protocols[I] = nullptr;
    zappedAny = true;
  }
  
  // Walk the inheritance hierarchies of all of the protocols. If we run into
  // one of the known protocols, zap it from the original list.
  while (!stack.empty()) {
    ProtocolDecl *Current = stack.back();
    stack.pop_back();
    
    // Add the protocols we inherited.
    for (auto Inherited : Current->getInheritedProtocols()) {
      addMinimumProtocols(Inherited->getDeclaredInterfaceType(),
                          protocols, known, visited, stack, zappedAny);
    }
  }
  
  if (zappedAny)
    protocols.erase(std::remove(protocols.begin(), protocols.end(), nullptr),
                    protocols.end());

  // Sort the set of protocols by module + name, to give a stable
  // ordering.
  llvm::array_pod_sort(protocols.begin(), protocols.end(), TypeDecl::compare);
}

static void
getCanonicalParams(AnyFunctionType *funcType,
                   CanGenericSignature genericSig,
                   SmallVectorImpl<AnyFunctionType::Param> &canParams) {
  auto origParams = funcType->getParams();
  for (auto param : origParams) {
    // Canonicalize the type and drop the internal label to canonicalize the
    // Param.
    canParams.emplace_back(param.getPlainType()->getCanonicalType(genericSig),
                           param.getLabel(), param.getParameterFlags(),
                           /*InternalLabel=*/Identifier());
  }
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
      if (auto decl =
            gpDecl->getDeclContext()->getInnermostDeclarationDeclContext())
        if (auto valueDecl = decl->getAsGenericContext())
          (void)valueDecl->getGenericSignature();
    }

    assert(gpDecl->getDepth() != GenericTypeParamDecl::InvalidDepth &&
           "parameter hasn't been validated");
    Result = GenericTypeParamType::get(gpDecl->getDepth(), gpDecl->getIndex(),
                                       gpDecl->getASTContext());
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
    auto resultTy = funcTy->getResult()->getCanonicalType(genericSig);

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

CanType TypeBase::getCanonicalType(GenericSignature sig) {
  if (!sig)
    return getCanonicalType();

  return sig->getCanonicalTypeInContext(this);
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

ParenType::ParenType(Type baseType, RecursiveTypeProperties properties,
                     ParameterTypeFlags flags)
  : SugarType(TypeKind::Paren,
              flags.isInOut() ? InOutType::get(baseType) : baseType,
              properties) {
  // In some situations (rdar://75740683) we appear to end up with ParenTypes
  // that contain a nullptr baseType. Once this is eliminated, we can remove
  // the checks for `type.isNull()` in the `DiagnosticArgumentKind::Type` case
  // of `formatDiagnosticArgument`.
  assert(baseType && "A ParenType should always wrap a non-null type");
  Bits.ParenType.Flags = flags.toRaw();
  if (flags.isInOut())
    assert(!baseType->is<InOutType>() && "caller did not pass a base type");
  if (baseType->is<InOutType>())
    assert(flags.isInOut() && "caller did not set flags correctly");
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
  return fixedNum >> 16;
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

bool TypeBase::isCallableNominalType(DeclContext *dc) {
  // Don't allow callAsFunction to be used with dynamic lookup.
  if (isAnyObject())
    return false;

  // If the type cannot have members, we're done.
  if (!mayHaveMembers())
    return false;

  auto canTy = getCanonicalType();
  auto &ctx = canTy->getASTContext();
  return evaluateOrDefault(ctx.evaluator,
                           IsCallableNominalTypeRequest{canTy, dc}, false);
}

bool TypeBase::hasDynamicMemberLookupAttribute() {
  if (!mayHaveMembers())
    return false;

  auto canTy = getCanonicalType();
  auto &ctx = canTy->getASTContext();
  return evaluateOrDefault(
      ctx.evaluator, HasDynamicMemberLookupAttributeRequest{canTy}, false);
}

bool TypeBase::hasDynamicCallableAttribute() {
  if (!mayHaveMembers())
    return false;

  auto canTy = getCanonicalType();
  auto &ctx = canTy->getASTContext();
  return evaluateOrDefault(
      ctx.evaluator, HasDynamicCallableAttributeRequest{canTy}, false);
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
                               ArchetypeType *, ArrayRef<ProtocolConformanceRef>) {
    // A "dynamic self" type can be bound to another dynamic self type, or the
    // non-dynamic base class type.
    if (auto dynSubst = dyn_cast<DynamicSelfType>(subst)) {
      if (auto newBase = visit(orig->getSelfType(), dynSubst.getSelfType(),
                               nullptr, {})) {
        return CanDynamicSelfType::get(newBase, orig->getASTContext())
                                 ->getCanonicalType();
      }
      return CanType();
    }
    
    if (auto newNonDynBase = visit(orig->getSelfType(), subst,
                                   nullptr, {})) {
      return newNonDynBase;
    }
    return CanType();
  }
  
  CanType visitNominalType(NominalType *nom, CanType subst,
                           ArchetypeType*, ArrayRef<ProtocolConformanceRef>) {
    if (auto substNom = dyn_cast<NominalType>(subst)) {
      if (nom->getDecl() != substNom->getDecl())
        return CanType();
      
      // Same decl should always either have or not have a parent.
      assert((bool)nom->getParent() == (bool)substNom->getParent());
      
      if (nom->getParent()) {
        auto substParent = visit(nom->getParent()->getCanonicalType(),
                                 substNom->getParent()->getCanonicalType(),
                                 nullptr, {});
        if (substParent == substNom.getParent())
          return subst;
        return NominalType::get(nom->getDecl(), substParent,
                                nom->getASTContext())
          ->getCanonicalType();
      }
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
            origSubs.getReplacementTypes()[i]->getCanonicalType(sig);
          auto substType =
            substSubs.getReplacementTypes()[i]->getCanonicalType(sig);

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
            origSubs.getReplacementTypes()[i]->getCanonicalType(sig);
          auto substType =
            substSubs.getReplacementTypes()[i]->getCanonicalType(sig);
          
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
                            ArchetypeType *, ArrayRef<ProtocolConformanceRef>) {
    auto substBGT = dyn_cast<BoundGenericType>(subst);
    if (!substBGT)
      return CanType();
    
    if (bgt->getDecl() != substBGT->getDecl())
      return CanType();

    auto *decl = bgt->getDecl();

    auto *moduleDecl = decl->getParentModule();
    auto origSubMap = bgt->getContextSubstitutionMap(
        moduleDecl, decl, decl->getGenericEnvironment());
    auto substSubMap = substBGT->getContextSubstitutionMap(
        moduleDecl, decl, decl->getGenericEnvironment());

    auto genericSig = decl->getGenericSignature();
    
    // Same decl should always either have or not have a parent.
    assert((bool)bgt->getParent() == (bool)substBGT->getParent());
    CanType newParent;
    if (bgt->getParent()) {
      newParent = visit(bgt->getParent()->getCanonicalType(),
                        substBGT.getParent(),
                        nullptr, {});
      if (!newParent)
        return CanType();
    }
    
    SmallVector<Type, 4> newParams;
    bool didChange = newParent != substBGT.getParent();
    
    auto depthStart =
      genericSig->getGenericParams().size() - bgt->getGenericArgs().size();
    for (auto i : indices(bgt->getGenericArgs())) {
      auto orig = bgt->getGenericArgs()[i]->getCanonicalType();
      auto subst = substBGT.getGenericArgs()[i];
      auto gp = genericSig->getGenericParams()[depthStart + i];
      
      // The new type is upper-bounded by the constraints the nominal type
      // requires. The substitution operation may be interested in transforming
      // the substituted type's conformances to these protocols.
      auto upperBoundArchetype = decl->mapTypeIntoContext(gp)
                                     ->getAs<ArchetypeType>();
      SmallVector<ProtocolConformanceRef, 4> substConformances;
      if (upperBoundArchetype) {
        for (auto proto : upperBoundArchetype->getConformsTo()) {
          auto conformance = substSubMap.lookupConformance(gp->getCanonicalType(),
                                                           proto);
          if (!conformance)
            return CanType();
          substConformances.push_back(conformance);
        }
      }
      
      auto newParam = visit(orig, subst, upperBoundArchetype,
                            substConformances);
      if (!newParam)
        return CanType();
      
      newParams.push_back(newParam);
      didChange |= (newParam != subst);
    }

    for (const auto &req : genericSig->getRequirements()) {
      if (req.getKind() != RequirementKind::Conformance) continue;

      auto canTy = req.getFirstType()->getCanonicalType();

      // If the substituted type is an interface type, we can't verify the
      // generic requirements.
      if (canTy.subst(substSubMap)->isTypeParameter())
        continue;

      auto *proto = req.getProtocolDecl();
      auto origConf = origSubMap.lookupConformance(canTy, proto);
      auto substConf = substSubMap.lookupConformance(canTy, proto);

      if (origConf.isConcrete()) {
        // A generic argument may inherit a concrete conformance from a class
        // constraint, which could still be bound to a type parameter we don't
        // know more about.
        if (origConf.getConcrete()->getType()->is<ArchetypeType>())
          continue;
        
        if (!substConf.isConcrete())
          return CanType();
        if (origConf.getConcrete()->getRootConformance()
              != substConf.getConcrete()->getRootConformance())
          return CanType();
      }
    }

    if (!didChange)
      return subst;
    
    return BoundGenericType::get(substBGT->getDecl(),
                                 newParent, newParams)
      ->getCanonicalType();
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

  // Any can be bridged to id.
  if (type->isAny()) {
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
/// to be reflected in PrintAsObjC, so that the Swift type will be
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
            if (nominal->lookupConformance(dc->getParentModule(),
                                           objcBridgeable,
                                           conformances))
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
        .getSpecializedConformance(type, result.getConformance(),
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
    if (ext2.isThrowing()) {
      ext1 = ext1.withThrows(true);
    }
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
        return opaque1->getBoundSignature().getCanonicalSignature() ==
                   opaque2->getBoundSignature().getCanonicalSignature() &&
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
                             Type Superclass, LayoutConstraint Layout)
  : SubstitutableType(Kind, &Ctx, properties),
    InterfaceType(InterfaceType)
{
  // Set up the bits we need for trailing objects to work.
  Bits.ArchetypeType.ExpandedNestedTypes = false;
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

GenericEnvironment *ArchetypeType::getGenericEnvironment() const {
  auto root = getRoot();
  if (auto primary = dyn_cast<PrimaryArchetypeType>(root)) {
    return primary->getGenericEnvironment();
  }
  if (auto opened = dyn_cast<OpenedArchetypeType>(root)) {
    return opened->getGenericEnvironment();
  }
  if (auto opaque = dyn_cast<OpaqueTypeArchetypeType>(root)) {
    return opaque->getGenericEnvironment();
  }
  llvm_unreachable("unhandled root archetype kind?!");
}

ArchetypeType *ArchetypeType::getRoot() const {
  auto parent = this;
  while (auto nested = dyn_cast<NestedArchetypeType>(parent)) {
    parent = nested->getParent();
  }
  return const_cast<ArchetypeType*>(parent);
}

Type ArchetypeType::getExistentialType() const {
  // Opened types hold this directly.
  if (auto opened = dyn_cast<OpenedArchetypeType>(this))
    return opened->getOpenedExistentialType();
  
  // Otherwise, compute it from scratch.
  SmallVector<Type, 4> constraintTypes;
  
  if (auto super = getSuperclass()) {
    constraintTypes.push_back(super);
  }
  for (auto proto : getConformsTo()) {
    constraintTypes.push_back(proto->getDeclaredInterfaceType());
  }
  return ProtocolCompositionType::get(
     const_cast<ArchetypeType*>(this)->getASTContext(), constraintTypes, false);
}

PrimaryArchetypeType::PrimaryArchetypeType(const ASTContext &Ctx,
                                     GenericEnvironment *GenericEnv,
                                     Type InterfaceType,
                                     ArrayRef<ProtocolDecl *> ConformsTo,
                                     Type Superclass, LayoutConstraint Layout)
  : ArchetypeType(TypeKind::PrimaryArchetype, Ctx,
                  RecursiveTypeProperties::HasArchetype,
                  InterfaceType, ConformsTo, Superclass, Layout),
    Environment(GenericEnv)
{
}

OpenedArchetypeType::OpenedArchetypeType(const ASTContext &Ctx,
                                         Type Existential,
                                         ArrayRef<ProtocolDecl *> ConformsTo,
                                         Type Superclass,
                                         LayoutConstraint Layout, UUID uuid)
  : ArchetypeType(TypeKind::OpenedArchetype, Ctx,
                  RecursiveTypeProperties::HasArchetype
                    | RecursiveTypeProperties::HasOpenedExistential,
                  Type(), ConformsTo, Superclass, Layout),
    Opened(Existential.getPointer()),
    ID(uuid)
{
}

NestedArchetypeType::NestedArchetypeType(const ASTContext &Ctx,
                                       ArchetypeType *Parent,
                                       Type InterfaceType,
                                       ArrayRef<ProtocolDecl *> ConformsTo,
                                       Type Superclass,
                                       LayoutConstraint Layout)
  : ArchetypeType(TypeKind::NestedArchetype, Ctx,
                  Parent->getRecursiveProperties(),
                  InterfaceType, ConformsTo, Superclass, Layout),
    Parent(Parent)
{
}

OpaqueTypeArchetypeType::OpaqueTypeArchetypeType(OpaqueTypeDecl *OpaqueDecl,
                                   SubstitutionMap Substitutions,
                                   RecursiveTypeProperties Props,
                                   Type InterfaceType,
                                   ArrayRef<ProtocolDecl*> ConformsTo,
                                   Type Superclass, LayoutConstraint Layout)
  : ArchetypeType(TypeKind::OpaqueTypeArchetype, OpaqueDecl->getASTContext(),
                  Props,
                  InterfaceType, ConformsTo, Superclass, Layout),
    OpaqueDecl(OpaqueDecl),
    Substitutions(Substitutions)
{
}

GenericSignature OpaqueTypeArchetypeType::getBoundSignature() const {
  return Environment->getGenericSignature();
}

static Optional<std::pair<ArchetypeType *, OpaqueTypeArchetypeType*>>
getArchetypeAndRootOpaqueArchetype(Type maybeOpaqueType) {
  auto archetype = dyn_cast<ArchetypeType>(maybeOpaqueType.getPointer());
  if (!archetype)
    return None;
  auto opaqueRoot = dyn_cast<OpaqueTypeArchetypeType>(archetype->getRoot());
  if (!opaqueRoot)
    return None;

  return std::make_pair(archetype, opaqueRoot);
}

OpaqueSubstitutionKind
ReplaceOpaqueTypesWithUnderlyingTypes::shouldPerformSubstitution(
    OpaqueTypeDecl *opaque) const {
  return shouldPerformSubstitution(opaque, inContext->getParentModule(),
                                   contextExpansion);
}
OpaqueSubstitutionKind
ReplaceOpaqueTypesWithUnderlyingTypes::shouldPerformSubstitution(
    OpaqueTypeDecl *opaque, ModuleDecl *contextModule,
    ResilienceExpansion contextExpansion) {
  auto namingDecl = opaque->getNamingDecl();
  
  // Don't allow replacement if the naming decl is dynamically replaceable.
  if (namingDecl && namingDecl->isDynamic())
    return OpaqueSubstitutionKind::DontSubstitute;

  // Allow replacement of opaque result types of inlineable function regardless
  // of resilience and in which context.
  if (auto *afd = dyn_cast<AbstractFunctionDecl>(namingDecl)) {
    if (afd->getResilienceExpansion() == ResilienceExpansion::Minimal) {
      return OpaqueSubstitutionKind::AlwaysSubstitute;
    }
  } else if (auto *asd = dyn_cast<AbstractStorageDecl>(namingDecl)) {
    auto *getter = asd->getOpaqueAccessor(AccessorKind::Get);
    if (getter &&
        getter->getResilienceExpansion() == ResilienceExpansion::Minimal) {
      return OpaqueSubstitutionKind::AlwaysSubstitute;
    }
  }

  // Allow replacement of opaque result types in the context of maximal
  // resilient expansion if the context's and the opaque type's module are the
  // same.
  auto module = namingDecl->getModuleContext();
  if (contextExpansion == ResilienceExpansion::Maximal &&
      module == contextModule)
    return OpaqueSubstitutionKind::SubstituteSameModuleMaximalResilience;

  // Allow general replacement from non resilient modules. Otherwise, disallow.
  if (module->isResilient())
    return OpaqueSubstitutionKind::DontSubstitute;

  return OpaqueSubstitutionKind::SubstituteNonResilientModule;
}

static Type
substOpaqueTypesWithUnderlyingTypes(Type ty, const DeclContext *inContext,
                                    ResilienceExpansion contextExpansion,
                                    bool isWholeModuleContext) {
  ReplaceOpaqueTypesWithUnderlyingTypes replacer(inContext, contextExpansion,
                                                 isWholeModuleContext);
  return ty.subst(replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes);
}

/// Checks that \p dc has access to \p ty for the purposes of an opaque
/// substitution described by \p kind.
///
/// This is purely an implementation detail check about whether type metadata
/// will be accessible. It's not intended to enforce any rules about what
/// opaque substitutions are or are not allowed.
static bool canSubstituteTypeInto(Type ty, const DeclContext *dc,
                                  OpaqueSubstitutionKind kind,
                                  bool isContextWholeModule) {
  TypeDecl *typeDecl = ty->getAnyNominal();
  if (!typeDecl) {
    // We also need to check that the opaque type descriptor is accessible.
    if (auto opaqueTy = ty->getAs<OpaqueTypeArchetypeType>())
      typeDecl = opaqueTy->getDecl();
  }
  if (!typeDecl) {
    return true;
  }

  switch (kind) {
  case OpaqueSubstitutionKind::DontSubstitute:
    return false;

  case OpaqueSubstitutionKind::AlwaysSubstitute:
    return true;

  case OpaqueSubstitutionKind::SubstituteSameModuleMaximalResilience:
    // In whole module compilation private types are okay.
    if (isContextWholeModule)
      return true;

    // In the same file any visibility is okay.
    if (!dc->isModuleContext() &&
        typeDecl->getDeclContext()->getParentSourceFile() ==
        dc->getParentSourceFile())
      return true;

    return typeDecl->getEffectiveAccess() > AccessLevel::FilePrivate;

  case OpaqueSubstitutionKind::SubstituteNonResilientModule:
    // Can't access types that are not public from a different module.
    if (dc->getParentModule() == typeDecl->getDeclContext()->getParentModule())
      return typeDecl->getEffectiveAccess() > AccessLevel::FilePrivate;

    return typeDecl->getEffectiveAccess() > AccessLevel::Internal;
  }
  llvm_unreachable("invalid subsitution kind");
}

Type ReplaceOpaqueTypesWithUnderlyingTypes::
operator()(SubstitutableType *maybeOpaqueType) const {
  auto archetypeAndRoot = getArchetypeAndRootOpaqueArchetype(maybeOpaqueType);
  if (!archetypeAndRoot)
    return maybeOpaqueType;

  auto archetype = archetypeAndRoot->first;
  auto opaqueRoot = archetypeAndRoot->second;

  auto substitutionKind = shouldPerformSubstitution(opaqueRoot->getDecl());
  if (substitutionKind == OpaqueSubstitutionKind::DontSubstitute) {
    return maybeOpaqueType;
  }

  auto subs = opaqueRoot->getDecl()->getUnderlyingTypeSubstitutions();
  // If the body of the opaque decl providing decl has not been type checked we
  // don't have a underlying subsitution.
  if (!subs.hasValue())
    return maybeOpaqueType;

  // Apply the underlying type substitutions to the interface type of the
  // archetype in question. This will map the inner generic signature of the
  // opaque type to its outer signature.
  auto partialSubstTy = archetype->getInterfaceType().subst(*subs);

  // Check that we are allowed to substitute the underlying type into the
  // context.
  auto inContext = this->inContext;
  auto isContextWholeModule = this->isContextWholeModule;
  if (partialSubstTy.findIf(
          [inContext, substitutionKind, isContextWholeModule](Type t) -> bool {
            if (!canSubstituteTypeInto(t, inContext, substitutionKind,
                                       isContextWholeModule))
              return true;
            return false;
          }))
    return maybeOpaqueType;

  // Then apply the substitutions from the root opaque archetype, to specialize
  // for its type arguments. We perform this substitution after checking for
  // visibility, since we do not want the result of the visibility check to
  // depend on the substitutions previously applied.
  auto substTy = partialSubstTy.subst(opaqueRoot->getSubstitutions());

  // If the type changed, but still contains opaque types, recur.
  if (!substTy->isEqual(maybeOpaqueType) && substTy->hasOpaqueArchetype()) {
    return ::substOpaqueTypesWithUnderlyingTypes(
        substTy, inContext, contextExpansion, isContextWholeModule);
  }

  return substTy;
}

static ProtocolConformanceRef substOpaqueTypesWithUnderlyingTypes(
    ProtocolConformanceRef ref, Type origType, const DeclContext *inContext,
    ResilienceExpansion contextExpansion, bool isWholeModuleContext) {
  ReplaceOpaqueTypesWithUnderlyingTypes replacer(inContext, contextExpansion,
                                                 isWholeModuleContext);
  return ref.subst(origType, replacer, replacer,
                   SubstFlags::SubstituteOpaqueArchetypes);
}

ProtocolConformanceRef swift::substOpaqueTypesWithUnderlyingTypes(
    ProtocolConformanceRef ref, Type origType, TypeExpansionContext context) {
  ReplaceOpaqueTypesWithUnderlyingTypes replacer(
      context.getContext(), context.getResilienceExpansion(),
      context.isWholeModuleContext());
  return ref.subst(origType, replacer, replacer,
                   SubstFlags::SubstituteOpaqueArchetypes);
}

ProtocolConformanceRef ReplaceOpaqueTypesWithUnderlyingTypes::
operator()(CanType maybeOpaqueType, Type replacementType,
           ProtocolDecl *protocol) const {
  auto abstractRef = ProtocolConformanceRef(protocol);
  
  auto archetypeAndRoot = getArchetypeAndRootOpaqueArchetype(maybeOpaqueType);
  if (!archetypeAndRoot) {
    if (maybeOpaqueType->isTypeParameter() ||
        maybeOpaqueType->is<ArchetypeType>())
      return abstractRef;
    
    // SIL type lowering may have already substituted away the opaque type, in
    // which case we'll end up "substituting" the same type.
    if (maybeOpaqueType->isEqual(replacementType)) {
      return inContext->getParentModule()
                      ->lookupConformance(replacementType, protocol);
    }
    
    llvm_unreachable("origType should have been an opaque type or type parameter");
  }

  auto archetype = archetypeAndRoot->first;
  auto opaqueRoot = archetypeAndRoot->second;

  auto substitutionKind = shouldPerformSubstitution(opaqueRoot->getDecl());
  if (substitutionKind == OpaqueSubstitutionKind::DontSubstitute) {
    return abstractRef;
  }

  auto subs = opaqueRoot->getDecl()->getUnderlyingTypeSubstitutions();
  // If the body of the opaque decl providing decl has not been type checked we
  // don't have a underlying subsitution.
  if (!subs.hasValue())
    return abstractRef;

  // Apply the underlying type substitutions to the interface type of the
  // archetype in question. This will map the inner generic signature of the
  // opaque type to its outer signature.
  auto partialSubstTy = archetype->getInterfaceType().subst(*subs);

  // Check that we are allowed to substitute the underlying type into the
  // context.
  auto inContext = this->inContext;
  auto isContextWholeModule = this->isContextWholeModule;
  if (partialSubstTy.findIf(
          [inContext, substitutionKind, isContextWholeModule](Type t) -> bool {
            if (!canSubstituteTypeInto(t, inContext, substitutionKind,
                                       isContextWholeModule))
              return true;
            return false;
          }))
    return abstractRef;

  // Then apply the substitutions from the root opaque archetype, to specialize
  // for its type arguments. We perform this substitution after checking for
  // visibility, since we do not want the result of the visibility check to
  // depend on the substitutions previously applied.
  auto substTy = partialSubstTy.subst(opaqueRoot->getSubstitutions());

  auto partialSubstRef =
      abstractRef.subst(archetype->getInterfaceType(), *subs);
  auto substRef =
      partialSubstRef.subst(partialSubstTy, opaqueRoot->getSubstitutions());

  // If the type still contains opaque types, recur.
  if (substTy->hasOpaqueArchetype()) {
    return ::substOpaqueTypesWithUnderlyingTypes(
        substRef, substTy, inContext, contextExpansion, isContextWholeModule);
  }
  return substRef;
}

CanNestedArchetypeType NestedArchetypeType::getNew(
                                   const ASTContext &Ctx,
                                   ArchetypeType *Parent,
                                   DependentMemberType *InterfaceType,
                                   SmallVectorImpl<ProtocolDecl *> &ConformsTo,
                                   Type Superclass,
                                   LayoutConstraint Layout) {
  assert(!Superclass || Superclass->getClassOrBoundGenericClass());

  // Gather the set of protocol declarations to which this archetype conforms.
  ProtocolType::canonicalizeProtocols(ConformsTo);

  auto arena = AllocationArena::Permanent;
  void *mem = Ctx.Allocate(
    NestedArchetypeType::totalSizeToAlloc<ProtocolDecl *, Type, LayoutConstraint>(
          ConformsTo.size(), Superclass ? 1 : 0, Layout ? 1 : 0),
      alignof(NestedArchetypeType), arena);

  return CanNestedArchetypeType(::new (mem) NestedArchetypeType(
      Ctx, Parent, InterfaceType, ConformsTo, Superclass, Layout));
}

CanPrimaryArchetypeType
PrimaryArchetypeType::getNew(const ASTContext &Ctx,
                      GenericEnvironment *GenericEnv,
                      GenericTypeParamType *InterfaceType,
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

bool ArchetypeType::requiresClass() const {
  if (auto layout = getLayoutConstraint())
    return layout->isClass();
  return false;
}

namespace {
  /// Function object that orders archetypes by name.
  struct OrderArchetypeByName {
    bool operator()(std::pair<Identifier, Type> X,
                    std::pair<Identifier, Type> Y) const {
      return X.first.str() < Y.first.str();
    }

    bool operator()(std::pair<Identifier, Type> X,
                    Identifier Y) const {
      return X.first.str() < Y.str();
    }

    bool operator()(Identifier X,
                    std::pair<Identifier, Type> Y) const {
      return X.str() < Y.first.str();
    }

    bool operator()(Identifier X, Identifier Y) const {
      return X.str() < Y.str();
    }
  };
} // end anonymous namespace

void ArchetypeType::populateNestedTypes() const {
  if (Bits.ArchetypeType.ExpandedNestedTypes) return;

  // Collect the set of nested types of this archetype.
  SmallVector<std::pair<Identifier, Type>, 4> nestedTypes;
  llvm::SmallPtrSet<Identifier, 4> knownNestedTypes;
  ProtocolType::visitAllProtocols(getConformsTo(),
                                  [&](ProtocolDecl *proto) -> bool {
    for (auto assocType : proto->getAssociatedTypeMembers()) {
      if (knownNestedTypes.insert(assocType->getName()).second)
        nestedTypes.push_back({ assocType->getName(), Type() });
    }

    return false;
  });

  // Record the nested types.
  auto mutableThis = const_cast<ArchetypeType *>(this);

  std::sort(nestedTypes.begin(), nestedTypes.end(), OrderArchetypeByName());
  auto &Ctx = mutableThis->getASTContext();
  mutableThis->NestedTypes = Ctx.AllocateCopy(nestedTypes);
  mutableThis->Bits.ArchetypeType.ExpandedNestedTypes = true;
}

Type ArchetypeType::getNestedType(Identifier Name) const {
  populateNestedTypes();

  auto Pos = std::lower_bound(NestedTypes.begin(), NestedTypes.end(), Name,
                              OrderArchetypeByName());
  if (Pos == NestedTypes.end() || Pos->first != Name) {
    return ErrorType::get(const_cast<ArchetypeType *>(this)->getASTContext());
  }

  // If the type is null, lazily resolve it. 
  if (!Pos->second) {
    resolveNestedType(*Pos);
  }

  return Pos->second;
}

Optional<Type> ArchetypeType::getNestedTypeIfKnown(Identifier Name) const {
  populateNestedTypes();

  auto Pos = std::lower_bound(NestedTypes.begin(), NestedTypes.end(), Name,
                              OrderArchetypeByName());
  if (Pos == NestedTypes.end() || Pos->first != Name || !Pos->second)
    return None;

  return Pos->second;
}

bool ArchetypeType::hasNestedType(Identifier Name) const {
  populateNestedTypes();

  auto Pos = std::lower_bound(NestedTypes.begin(), NestedTypes.end(), Name,
                              OrderArchetypeByName());
  return Pos != NestedTypes.end() && Pos->first == Name;
}

ArrayRef<std::pair<Identifier, Type>>
ArchetypeType::getKnownNestedTypes() const {
  populateNestedTypes();
  return NestedTypes;
}

void ArchetypeType::registerNestedType(Identifier name, Type nested) {
  populateNestedTypes();

  auto found = std::lower_bound(NestedTypes.begin(), NestedTypes.end(), name,
                                OrderArchetypeByName());
  assert(found != NestedTypes.end() && found->first == name &&
         "Unable to find nested type?");
  assert(!found->second ||
         found->second->isEqual(nested) ||
         (found->second->hasError() && nested->hasError()));
  found->second = nested;
}

static void collectFullName(const ArchetypeType *Archetype,
                            SmallVectorImpl<char> &Result) {
  if (auto nested = dyn_cast<NestedArchetypeType>(Archetype)) {
    collectFullName(nested->getParent(), Result);
    Result.push_back('.');
  }
  Result.append(Archetype->getName().str().begin(),
                Archetype->getName().str().end());
}

AssociatedTypeDecl *NestedArchetypeType::getAssocType() const {
  return InterfaceType->castTo<DependentMemberType>()->getAssocType();
}

Identifier ArchetypeType::getName() const {
  if (auto nested = dyn_cast<NestedArchetypeType>(this))
    return nested->getAssocType()->getName();
  assert(InterfaceType);
  return InterfaceType->castTo<GenericTypeParamType>()->getName();
}

std::string ArchetypeType::getFullName() const {
  llvm::SmallString<64> Result;
  collectFullName(this, Result);
  return Result.str().str();
}

void
OpaqueTypeArchetypeType::Profile(llvm::FoldingSetNodeID &id,
                                 OpaqueTypeDecl *decl,
                                 SubstitutionMap subs) {
  id.AddPointer(decl);
  subs.profile(id);
}

void ProtocolCompositionType::Profile(llvm::FoldingSetNodeID &ID,
                                      ArrayRef<Type> Members,
                                      bool HasExplicitAnyObject) {
  ID.AddInteger(HasExplicitAnyObject);
  for (auto T : Members)
    ID.AddPointer(T.getPointer());
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
  for (Type t : Members) {
    if (!t->isCanonical())
      return build(C, Members, HasExplicitAnyObject);
  }
    
  Type Superclass;
  SmallVector<ProtocolDecl *, 4> Protocols;
  for (Type t : Members) {
    addProtocols(t, Protocols, Superclass, HasExplicitAnyObject);
  }
  
  // Minimize the set of protocols composed together.
  ProtocolType::canonicalizeProtocols(Protocols);

  // The presence of a superclass constraint makes AnyObject redundant.
  if (Superclass)
    HasExplicitAnyObject = false;

  // If one protocol remains with no further constraints, its nominal
  // type is the canonical type.
  if (Protocols.size() == 1 && !Superclass && !HasExplicitAnyObject)
    return Protocols.front()->getDeclaredInterfaceType();

  // Form the set of canonical protocol types from the protocol
  // declarations, and use that to build the canonical composition type.
  SmallVector<Type, 4> CanTypes;
  if (Superclass)
    CanTypes.push_back(Superclass->getCanonicalType());
  llvm::transform(
      Protocols, std::back_inserter(CanTypes),
      [](ProtocolDecl *Proto) { return Proto->getDeclaredInterfaceType(); });

  // TODO: Canonicalize away HasExplicitAnyObject if it is implied
  // by one of our member protocols.
  return build(C, CanTypes, HasExplicitAnyObject);
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

FunctionType *
GenericFunctionType::substGenericArgs(SubstitutionMap subs) {
  return substGenericArgs(
    [=](Type t) { return t.subst(subs); });
}

FunctionType *GenericFunctionType::substGenericArgs(
    llvm::function_ref<Type(Type)> substFn) const {
  llvm::SmallVector<AnyFunctionType::Param, 4> params;
  params.reserve(getNumParams());

  llvm::transform(getParams(), std::back_inserter(params),
                  [&](const AnyFunctionType::Param &param) {
                    return param.withType(substFn(param.getPlainType()));
                  });

  auto resultTy = substFn(getResult());

  // Build the resulting (non-generic) function type.
  return FunctionType::get(params, resultTy, getExtInfo());
}

CanFunctionType
CanGenericFunctionType::substGenericArgs(SubstitutionMap subs) const {
  return cast<FunctionType>(
           getPointer()->substGenericArgs(subs)->getCanonicalType());
}

static Type getMemberForBaseType(LookupConformanceFn lookupConformances,
                                 Type origBase,
                                 Type substBase,
                                 AssociatedTypeDecl *assocType,
                                 Identifier name,
                                 SubstOptions options) {
  // Produce a dependent member type for the given base type.
  auto getDependentMemberType = [&](Type baseType) {
    if (assocType)
      return DependentMemberType::get(baseType, assocType);

    return DependentMemberType::get(baseType, name);
  };

  // Produce a failed result.
  auto failed = [&]() -> Type {
    Type baseType = ErrorType::get(substBase ? substBase : origBase);
    if (assocType)
      return DependentMemberType::get(baseType, assocType);

    return DependentMemberType::get(baseType, name);
  };

  // If we don't have a substituted base type, fail.
  if (!substBase) return failed();

  if (auto *selfType = substBase->getAs<DynamicSelfType>())
    substBase = selfType->getSelfType();

  // Error recovery path.
  // FIXME: Generalized existentials will look here.
  if (substBase->isOpenedExistential())
    return failed();

  // If the parent is an archetype, extract the child archetype with the
  // given name.
  if (auto archetypeParent = substBase->getAs<ArchetypeType>()) {
    if (archetypeParent->hasNestedType(name))
      return archetypeParent->getNestedType(name);

    // If looking for an associated type and the archetype is constrained to a
    // class, continue to the default associated type lookup
    if (!assocType || !archetypeParent->getSuperclass())
      return failed();
  }

  // If the parent is a type variable or a member rooted in a type variable,
  // or if the parent is a type parameter, we're done. Also handle
  // UnresolvedType here, which can come up in diagnostics.
  if (substBase->isTypeVariableOrMember() ||
      substBase->isTypeParameter() ||
      substBase->is<UnresolvedType>())
    return getDependentMemberType(substBase);

  // Retrieve the member type with the given name.

  // Tuples don't have member types.
  if (substBase->is<TupleType>())
    return failed();

  // If we know the associated type, look in the witness table.
  if (assocType) {
    auto proto = assocType->getProtocol();
    ProtocolConformanceRef conformance =
        lookupConformances(origBase->getCanonicalType(), substBase, proto);

    if (conformance.isInvalid())
      return failed();
    if (!conformance.isConcrete())
      return failed();

    // Retrieve the type witness.
    auto witness =
        conformance.getConcrete()->getTypeWitnessAndDecl(assocType, options);

    auto witnessTy = witness.getWitnessType();
    if (!witnessTy || witnessTy->hasError())
      return failed();

    // This is a hacky feature allowing code completion to migrate to
    // using Type::subst() without changing output.
    if (options & SubstFlags::DesugarMemberTypes) {
      if (auto *aliasType = dyn_cast<TypeAliasType>(witnessTy.getPointer()))
        witnessTy = aliasType->getSinglyDesugaredType();

      // Another hack. If the type witness is a opaque result type. They can
      // only be referred using the name of the associated type.
      if (witnessTy->is<OpaqueTypeArchetypeType>())
        witnessTy = witness.getWitnessDecl()->getDeclaredInterfaceType();
    }

    if (witnessTy->is<ErrorType>())
      return failed();

    return witnessTy;
  }

  return failed();
}

ProtocolConformanceRef LookUpConformanceInModule::
operator()(CanType dependentType, Type conformingReplacementType,
           ProtocolDecl *conformedProtocol) const {
  if (conformingReplacementType->isTypeParameter())
    return ProtocolConformanceRef(conformedProtocol);

  return M->lookupConformance(conformingReplacementType,
                              conformedProtocol);
}

ProtocolConformanceRef LookUpConformanceInSubstitutionMap::
operator()(CanType dependentType, Type conformingReplacementType,
           ProtocolDecl *conformedProtocol) const {
  // Lookup conformances for opened existential.
  if (conformingReplacementType->isOpenedExistential()) {
    return conformedProtocol->getModuleContext()->lookupConformance(
        conformingReplacementType, conformedProtocol);
  }
  return Subs.lookupConformance(dependentType, conformedProtocol);
}

ProtocolConformanceRef MakeAbstractConformanceForGenericType::
operator()(CanType dependentType, Type conformingReplacementType,
           ProtocolDecl *conformedProtocol) const {
  assert((conformingReplacementType->is<ErrorType>()
          || conformingReplacementType->is<SubstitutableType>()
          || conformingReplacementType->is<DependentMemberType>()
          || conformingReplacementType->is<TypeVariableType>())
         && "replacement requires looking up a concrete conformance");
  // Lookup conformances for opened existential.
  if (conformingReplacementType->isOpenedExistential()) {
    return conformedProtocol->getModuleContext()->lookupConformance(
        conformingReplacementType, conformedProtocol);
  }
  return ProtocolConformanceRef(conformedProtocol);
}

ProtocolConformanceRef LookUpConformanceInSignature::
operator()(CanType dependentType, Type conformingReplacementType,
           ProtocolDecl *conformedProtocol) const {
  // Lookup conformances for opened existential.
  if (conformingReplacementType->isOpenedExistential()) {
    return conformedProtocol->getModuleContext()->lookupConformance(
        conformingReplacementType, conformedProtocol);
  }

  // FIXME: Should pass dependentType instead, once
  // GenericSignature::lookupConformance() does the right thing
  return Sig->lookupConformance(conformingReplacementType->getCanonicalType(),
                                conformedProtocol);
}

Type DependentMemberType::substBaseType(ModuleDecl *module, Type substBase) {
  return substBaseType(substBase, LookUpConformanceInModule(module));
}

Type DependentMemberType::substBaseType(Type substBase,
                                        LookupConformanceFn lookupConformance) {
  if (substBase.getPointer() == getBase().getPointer() &&
      substBase->hasTypeParameter())
    return this;

  return getMemberForBaseType(lookupConformance, getBase(), substBase,
                              getAssocType(), getName(), None);
}

Type DependentMemberType::substRootParam(Type newRoot,
                                         LookupConformanceFn lookupConformance){
  auto base = getBase();
  if (base->is<GenericTypeParamType>()) {
    return substBaseType(newRoot, lookupConformance);
  }
  if (auto depMem = base->getAs<DependentMemberType>()) {
    return substBaseType(depMem->substRootParam(newRoot, lookupConformance),
                         lookupConformance);
  }
  return Type();
}

static Type substGenericFunctionType(GenericFunctionType *genericFnType,
                                     TypeSubstitutionFn substitutions,
                                     LookupConformanceFn lookupConformances,
                                     SubstOptions options) {
  // Substitute into the function type (without generic signature).
  auto *bareFnType = FunctionType::get(genericFnType->getParams(),
                                       genericFnType->getResult(),
                                       genericFnType->getExtInfo());
  Type result =
    Type(bareFnType).subst(substitutions, lookupConformances, options);
  if (!result || result->is<ErrorType>()) return result;

  auto *fnType = result->castTo<FunctionType>();
  // Substitute generic parameters.
  bool anySemanticChanges = false;
  SmallVector<GenericTypeParamType *, 2> genericParams;
  for (auto param : genericFnType->getGenericParams()) {
    Type paramTy =
      Type(param).subst(substitutions, lookupConformances, options);
    if (!paramTy)
      return Type();

    if (auto newParam = paramTy->getAs<GenericTypeParamType>()) {
      if (!newParam->isEqual(param))
        anySemanticChanges = true;

      genericParams.push_back(newParam);
    } else {
      anySemanticChanges = true;
    }
  }

  // If no generic parameters remain, this is a non-generic function type.
  if (genericParams.empty())
    return result;

  // Transform requirements.
  SmallVector<Requirement, 2> requirements;
  for (const auto &req : genericFnType->getRequirements()) {
    // Substitute into the requirement.
    auto substReqt = req.subst(substitutions, lookupConformances, options);
    if (!substReqt) {
      anySemanticChanges = true;
      continue;
    }

    // Did anything change?
    if (!anySemanticChanges &&
        (!req.getFirstType()->isEqual(substReqt->getFirstType()) ||
         (req.getKind() != RequirementKind::Layout &&
          !req.getSecondType()->isEqual(substReqt->getSecondType())))) {
      anySemanticChanges = true;
    }

    // Skip any erroneous requirements.
    if (substReqt->getFirstType()->hasError() ||
        (substReqt->getKind() != RequirementKind::Layout &&
         substReqt->getSecondType()->hasError()))
      continue;

    requirements.push_back(*substReqt);
  }

  GenericSignature genericSig;
  if (anySemanticChanges) {
    // If there were semantic changes, we need to build a new generic
    // signature.
    ASTContext &ctx = genericFnType->getASTContext();
    genericSig = evaluateOrDefault(
        ctx.evaluator,
        AbstractGenericSignatureRequest{nullptr, genericParams, requirements},
        GenericSignature());
  } else {
    // Use the mapped generic signature.
    genericSig = GenericSignature::get(genericParams, requirements);
  }

  // Produce the new generic function type.
  return GenericFunctionType::get(genericSig, fnType->getParams(),
                                  fnType->getResult(), fnType->getExtInfo());
}

static Type substType(Type derivedType,
                      TypeSubstitutionFn substitutions,
                      LookupConformanceFn lookupConformances,
                      SubstOptions options) {
  // Handle substitutions into generic function types.
  if (auto genericFnType = derivedType->getAs<GenericFunctionType>()) {
    return substGenericFunctionType(genericFnType, substitutions,
                                    lookupConformances, options);
  }

  // FIXME: Change getTypeOfMember() to not pass GenericFunctionType here
  if (!derivedType->hasArchetype()
      && !derivedType->hasTypeParameter()
      && (!options.contains(SubstFlags::SubstituteOpaqueArchetypes)
          || !derivedType->hasOpaqueArchetype()))
    return derivedType;

  return derivedType.transformRec([&](TypeBase *type) -> Optional<Type> {
    // FIXME: Add SIL versions of mapTypeIntoContext() and
    // mapTypeOutOfContext() and use them appropriately
    assert((options.contains(SubstFlags::AllowLoweredTypes) ||
            !isa<SILFunctionType>(type)) &&
           "should not be doing AST type-substitution on a lowered SIL type;"
           "use SILType::subst");

    // Special-case handle SILBoxTypes and substituted SILFunctionTypes;
    // we want to structurally substitute the substitutions.
    if (auto boxTy = dyn_cast<SILBoxType>(type)) {
      auto subMap = boxTy->getSubstitutions();
      auto newSubMap = subMap.subst(substitutions, lookupConformances, options);

      return SILBoxType::get(boxTy->getASTContext(),
                             boxTy->getLayout(),
                             newSubMap);
    }
    
    if (auto silFnTy = dyn_cast<SILFunctionType>(type)) {
      if (silFnTy->isPolymorphic())
        return None;
      if (auto subs = silFnTy->getInvocationSubstitutions()) {
        auto newSubs = subs.subst(substitutions, lookupConformances, options);
        return silFnTy->withInvocationSubstitutions(newSubs);
      }
      if (auto subs = silFnTy->getPatternSubstitutions()) {
        auto newSubs = subs.subst(substitutions, lookupConformances, options);
        return silFnTy->withPatternSubstitutions(newSubs);
      }
      return None;
    }

    // Special-case TypeAliasType; we need to substitute conformances.
    if (auto aliasTy = dyn_cast<TypeAliasType>(type)) {
      Type parentTy;
      if (auto origParentTy = aliasTy->getParent())
        parentTy = substType(origParentTy,
                             substitutions, lookupConformances, options);
      auto underlyingTy = substType(aliasTy->getSinglyDesugaredType(),
                                    substitutions, lookupConformances, options);
      if (parentTy && parentTy->isExistentialType())
        return underlyingTy;
      auto subMap = aliasTy->getSubstitutionMap()
          .subst(substitutions, lookupConformances, options);
      return Type(TypeAliasType::get(aliasTy->getDecl(), parentTy,
                                     subMap, underlyingTy));
    }

    // We only substitute for substitutable types and dependent member types.
    
    // For dependent member types, we may need to look up the member if the
    // base is resolved to a non-dependent type.
    if (auto depMemTy = dyn_cast<DependentMemberType>(type)) {
      auto newBase = substType(depMemTy->getBase(),
                               substitutions, lookupConformances, options);
      if (!newBase)
        return Type();
      
      return getMemberForBaseType(lookupConformances,
                                  depMemTy->getBase(), newBase,
                                  depMemTy->getAssocType(),
                                  depMemTy->getName(), options);
    }
    
    auto substOrig = dyn_cast<SubstitutableType>(type);
    if (!substOrig)
      return None;
    // Opaque types can't normally be directly substituted unless we
    // specifically were asked to substitute them.
    if (!options.contains(SubstFlags::SubstituteOpaqueArchetypes)
        && isa<OpaqueTypeArchetypeType>(substOrig))
      return None;

    // If we have a substitution for this type, use it.
    if (auto known = substitutions(substOrig)) {
      if (options.contains(SubstFlags::SubstituteOpaqueArchetypes) &&
          isa<OpaqueTypeArchetypeType>(substOrig) &&
          known->getCanonicalType() == substOrig->getCanonicalType())
        return None; // Recursively process the substitutions of the opaque type
                     // archetype.
      return known;
    }

    // If we failed to substitute a generic type parameter, give up.
    if (isa<GenericTypeParamType>(substOrig))
      return ErrorType::get(type);

    if (isa<PrimaryArchetypeType>(substOrig))
      return ErrorType::get(type);

    // Opened existentials cannot be substituted in this manner,
    // but if they appear in the original type this is not an
    // error.
    if (isa<OpenedArchetypeType>(substOrig))
      return Type(type);

    // For nested archetypes, we can substitute the parent.
    auto nestedArchetype = cast<NestedArchetypeType>(substOrig);
    auto parent = nestedArchetype->getParent();

    // Substitute into the parent type.
    Type substParent = substType(parent, substitutions,
                                 lookupConformances, options);

    // If the parent didn't change, we won't change.
    if (substParent.getPointer() == parent)
      return Type(type);

    // Get the associated type reference from a child archetype.
    AssociatedTypeDecl *assocType = nestedArchetype->getAssocType();

    return getMemberForBaseType(lookupConformances, parent, substParent,
                                assocType, nestedArchetype->getName(),
                                options);
  });
}

Type Type::subst(SubstitutionMap substitutions,
                 SubstOptions options) const {
  return substType(*this,
                   QuerySubstitutionMap{substitutions},
                   LookUpConformanceInSubstitutionMap(substitutions),
                   options);
}

Type Type::subst(TypeSubstitutionFn substitutions,
                 LookupConformanceFn conformances,
                 SubstOptions options) const {
  return substType(*this, substitutions, conformances, options);
}

Type Type::substDependentTypesWithErrorTypes() const {
  return substType(*this,
                   [](SubstitutableType *t) -> Type { return Type(); },
                   MakeAbstractConformanceForGenericType(),
                   SubstFlags::AllowLoweredTypes);
}

const DependentMemberType *TypeBase::findUnresolvedDependentMemberType() {
  if (!hasTypeParameter()) return nullptr;

  const DependentMemberType *unresolvedDepMemTy = nullptr;
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

static Type getConcreteTypeForSuperclassTraversing(Type t) {
  if (t->isExistentialType()) {
    return t->getExistentialLayout().getSuperclass();
  } if (auto archetype = t->getAs<ArchetypeType>()) {
    return archetype->getSuperclass();
  } else if (auto dynamicSelfTy = t->getAs<DynamicSelfType>()) {
    return dynamicSelfTy->getSelfType();
  }
  return t;
}

Type TypeBase::getSuperclassForDecl(const ClassDecl *baseClass,
                                    bool useArchetypes) {
  Type t = getConcreteTypeForSuperclassTraversing(this);

  while (t) {
    // If we have a class-constrained archetype or class-constrained
    // existential, get the underlying superclass constraint.
    auto *nominalDecl = t->getAnyNominal();
    assert(nominalDecl && "expected nominal type here");
    assert(isa<ClassDecl>(nominalDecl) && "expected a class here");

    if (nominalDecl == baseClass)
      return t;

    t = t->getSuperclass(useArchetypes);
  }

#ifndef NDEBUG
  auto *currentClass = getConcreteTypeForSuperclassTraversing(this)
      ->getClassOrBoundGenericClass();
  assert(baseClass->isSuperclassOf(currentClass) &&
         "no inheritance relationship between given classes");
#endif

  return ErrorType::get(this);
}

TypeSubstitutionMap
TypeBase::getContextSubstitutions(const DeclContext *dc,
                                  GenericEnvironment *genericEnv) {
  assert(dc->isTypeContext());
  Type baseTy(this);

  assert(!baseTy->hasLValueType() &&
         !baseTy->is<AnyMetatypeType>() &&
         !baseTy->is<ErrorType>());

  // The resulting set of substitutions. Always use this to ensure we
  // don't miss out on NRVO anywhere.
  TypeSubstitutionMap substitutions;

  // If the member is part of a protocol or extension thereof, we need
  // to substitute in the type of Self.
  if (dc->getSelfProtocolDecl()) {
    // FIXME: This feels painfully inefficient. We're creating a dense map
    // for a single substitution.
    substitutions[dc->getSelfInterfaceType()
                    ->getCanonicalType()->castTo<GenericTypeParamType>()]
      = baseTy;
    return substitutions;
  }

  const auto genericSig = dc->getGenericSignatureOfContext();
  if (!genericSig)
    return substitutions;

  // Find the superclass type with the context matching that of the member.
  auto *ownerNominal = dc->getSelfNominalTypeDecl();
  if (auto *ownerClass = dyn_cast<ClassDecl>(ownerNominal))
    baseTy = baseTy->getSuperclassForDecl(ownerClass);

  // Gather all of the substitutions for all levels of generic arguments.
  auto params = genericSig->getGenericParams();
  unsigned n = params.size();

  while (baseTy && n > 0) {
    if (baseTy->is<ErrorType>())
      break;

    // For a bound generic type, gather the generic parameter -> generic
    // argument substitutions.
    if (auto boundGeneric = baseTy->getAs<BoundGenericType>()) {
      auto args = boundGeneric->getGenericArgs();
      for (unsigned i = 0, e = args.size(); i < e; ++i) {
        substitutions[params[n - e + i]->getCanonicalType()
                        ->castTo<GenericTypeParamType>()] = args[i];
      }

      // Continue looking into the parent.
      baseTy = boundGeneric->getParent();
      n -= args.size();
      continue;
    }

    // Continue looking into the parent.
    if (auto protocolTy = baseTy->getAs<ProtocolType>()) {
      baseTy = protocolTy->getParent();
      --n;
      continue;
    }

    // Continue looking into the parent.
    if (auto nominalTy = baseTy->getAs<NominalType>()) {
      baseTy = nominalTy->getParent();
      continue;
    }

    // There are no subtitutions to apply if the type is still unbound,
    // continue looking into the parent.
    if (auto unboundGeneric = baseTy->getAs<UnboundGenericType>()) {
      baseTy = unboundGeneric->getParent();
      continue;
    }

    // Assert and break to avoid hanging if we get an unexpected baseTy.
    assert(0 && "Bad base type");
    break;
  }

  while (n > 0) {
    auto *gp = params[--n];
    auto substTy = (genericEnv
                    ? genericEnv->mapTypeIntoContext(gp)
                    : gp);
    auto result = substitutions.insert(
      {gp->getCanonicalType()->castTo<GenericTypeParamType>(),
       substTy});
    assert(result.second);
    (void) result;
  }

  return substitutions;
}

SubstitutionMap TypeBase::getContextSubstitutionMap(
    ModuleDecl *module, const DeclContext *dc,
    GenericEnvironment *genericEnv) {
  auto genericSig = dc->getGenericSignatureOfContext();
  if (genericSig.isNull())
    return SubstitutionMap();
  return SubstitutionMap::get(
    genericSig,
    QueryTypeSubstitutionMap{getContextSubstitutions(dc, genericEnv)},
    LookUpConformanceInModule(module));
}

TypeSubstitutionMap TypeBase::getMemberSubstitutions(
    const ValueDecl *member,
    GenericEnvironment *genericEnv) {
  auto *memberDC = member->getDeclContext();

  TypeSubstitutionMap substitutions;

  // Compute the set of member substitutions to apply.
  if (memberDC->isTypeContext())
    substitutions = getContextSubstitutions(memberDC, genericEnv);

  // If the member itself is generic, preserve its generic parameters.
  // We need this since code completion and diagnostics want to be able
  // to call getTypeOfMember() with functions and nested types.
  if (isa<AbstractFunctionDecl>(member) ||
      isa<GenericTypeDecl>(member) ||
      isa<SubscriptDecl>(member)) {
    auto *innerDC = member->getInnermostDeclContext();
    if (innerDC->isInnermostContextGeneric()) {
      if (auto sig = innerDC->getGenericSignatureOfContext()) {
        for (auto param : sig->getInnermostGenericParams()) {
          auto *genericParam = param->getCanonicalType()
              ->castTo<GenericTypeParamType>();
          substitutions[genericParam] =
            (genericEnv
             ? genericEnv->mapTypeIntoContext(param)
             : param);
        }
      }
    }
  }

  return substitutions;
}

SubstitutionMap TypeBase::getMemberSubstitutionMap(
    ModuleDecl *module, const ValueDecl *member,
    GenericEnvironment *genericEnv) {
  auto genericSig = member->getInnermostDeclContext()
      ->getGenericSignatureOfContext();
  if (genericSig.isNull())
    return SubstitutionMap();
  auto subs = getMemberSubstitutions(member, genericEnv);
  return SubstitutionMap::get(
      genericSig,
      QueryTypeSubstitutionMap{subs},
      LookUpConformanceInModule(module));
}

Type TypeBase::getTypeOfMember(ModuleDecl *module, const ValueDecl *member,
                               Type memberType) {
  if (is<ErrorType>())
    return ErrorType::get(getASTContext());

  if (auto *lvalue = getAs<LValueType>()) {
    auto objectTy = lvalue->getObjectType();
    return objectTy->getTypeOfMember(module, member, memberType);
  }

  // If no member type was provided, use the member's type.
  if (!memberType)
    memberType = member->getInterfaceType();

  assert(memberType);

  // Perform the substitution.
  auto substitutions = getMemberSubstitutionMap(module, member);
  return memberType.subst(substitutions);
}

Type TypeBase::adjustSuperclassMemberDeclType(const ValueDecl *baseDecl,
                                              const ValueDecl *derivedDecl,
                                              Type memberType) {
  auto subs = SubstitutionMap::getOverrideSubstitutions(
      baseDecl, derivedDecl, /*derivedSubs=*/None);

  if (auto *genericMemberType = memberType->getAs<GenericFunctionType>()) {
    memberType = FunctionType::get(genericMemberType->getParams(),
                                   genericMemberType->getResult(),
                                   genericMemberType->getExtInfo());
  }

  auto type = memberType.subst(subs);
  if (baseDecl->getDeclContext()->getSelfProtocolDecl())
    return type;

  if (auto *afd = dyn_cast<AbstractFunctionDecl>(baseDecl)) {
    type = type->replaceSelfParameterType(this);
    if (afd->hasDynamicSelfResult())
      type = type->replaceCovariantResultType(this, /*uncurryLevel=*/2);
  } else if (auto *sd = dyn_cast<SubscriptDecl>(baseDecl)) {
    if (sd->getElementInterfaceType()->hasDynamicSelfType())
      type = type->replaceCovariantResultType(this, /*uncurryLevel=*/1);
  } else if (auto *vd = dyn_cast<VarDecl>(baseDecl)) {
    if (vd->getValueInterfaceType()->hasDynamicSelfType())
      type = type->replaceCovariantResultType(this, /*uncurryLevel=*/0);
  }

  return type;
}

Identifier DependentMemberType::getName() const {
  if (NameOrAssocType.is<Identifier>())
    return NameOrAssocType.get<Identifier>();

  return NameOrAssocType.get<AssociatedTypeDecl *>()->getName();
}

static bool transformSILResult(
                           SILResultInfo &result, bool &changed,
                           llvm::function_ref<Optional<Type>(TypeBase *)> fn) {
  Type transType = result.getInterfaceType().transformRec(fn);
  if (!transType) return true;

  CanType canTransType = transType->getCanonicalType();
  if (canTransType != result.getInterfaceType()) {
    changed = true;
    result = result.getWithInterfaceType(canTransType);
  }
  return false;
}

static bool transformSILYield(
                            SILYieldInfo &yield, bool &changed,
                            llvm::function_ref<Optional<Type>(TypeBase *)> fn) {
  Type transType = yield.getInterfaceType().transformRec(fn);
  if (!transType) return true;

  CanType canTransType = transType->getCanonicalType();
  if (canTransType != yield.getInterfaceType()) {
    changed = true;
    yield = yield.getWithInterfaceType(canTransType);
  }
  return false;
}

static bool transformSILParameter(
                            SILParameterInfo &param, bool &changed,
                            llvm::function_ref<Optional<Type>(TypeBase *)> fn) {
  Type transType = param.getInterfaceType().transformRec(fn);
  if (!transType) return true;

  CanType canTransType = transType->getCanonicalType();
  if (canTransType != param.getInterfaceType()) {
    changed = true;
    param = param.getWithInterfaceType(canTransType);
  }
  return false;
}

Type Type::transform(llvm::function_ref<Type(Type)> fn) const {
  return transformRec([&fn](TypeBase *type) -> Optional<Type> {
    Type transformed = fn(Type(type));
    if (!transformed)
      return Type();

    // If the function didn't change the type at all, let transformRec()
    // recurse.
    if (transformed.getPointer() == type)
      return None;

    return transformed;
  });
}

Type Type::transformRec(
                    llvm::function_ref<Optional<Type>(TypeBase *)> fn) const {
  if (!isa<ParenType>(getPointer())) {
    // Transform this type node.
    if (Optional<Type> transformed = fn(getPointer()))
      return *transformed;

    // Recur.
  }

  // Recur into children of this type.
  TypeBase *base = getPointer();
  switch (base->getKind()) {
#define BUILTIN_TYPE(Id, Parent) \
case TypeKind::Id:
#define TYPE(Id, Parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::Error:
  case TypeKind::Unresolved:
  case TypeKind::TypeVariable:
  case TypeKind::Placeholder:
  case TypeKind::GenericTypeParam:
  case TypeKind::SILToken:
  case TypeKind::Module:
    return *this;

  case TypeKind::Enum:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::Protocol: {
    auto nominalTy = cast<NominalType>(base);
    if (auto parentTy = nominalTy->getParent()) {
      parentTy = parentTy.transformRec(fn);
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
    Type transCap = storageTy->getCaptureType().transformRec(fn);
    if (!transCap)
      return Type();
    CanType canTransCap = transCap->getCanonicalType();
    if (canTransCap != storageTy->getCaptureType())
      return SILBlockStorageType::get(canTransCap);
    return storageTy;
  }

  case TypeKind::SILBox: {
    bool changed = false;
    auto boxTy = cast<SILBoxType>(base);
#ifndef NDEBUG
    // This interface isn't suitable for updating the substitution map in a
    // generic SILBox.
    for (Type type : boxTy->getSubstitutions().getReplacementTypes()) {
      assert(type->isEqual(type.transformRec(fn))
             && "SILBoxType substitutions can't be transformed");
    }
#endif
    SmallVector<SILField, 4> newFields;
    auto *l = boxTy->getLayout();
    for (auto f : l->getFields()) {
      auto fieldTy = f.getLoweredType();
      auto transformed = fieldTy.transformRec(fn)->getCanonicalType();
      changed |= fieldTy != transformed;
      newFields.push_back(SILField(transformed, f.isMutable()));
    }
    boxTy = SILBoxType::get(Ptr->getASTContext(),
                            SILLayout::get(Ptr->getASTContext(),
                                           l->getGenericSignature(), newFields),
                            boxTy->getSubstitutions());
    return boxTy;
  }
  
  case TypeKind::SILFunction: {
    auto fnTy = cast<SILFunctionType>(base);
    bool changed = false;
    auto hasTypeErasedGenericClassType = [](Type ty) -> bool {
      return ty.findIf([](Type subType) -> bool {
        if (subType->isTypeErasedGenericClassType())
          return true;
        else
          return false;
      });
    };
    auto updateSubs = [&](SubstitutionMap &subs) -> bool {
      // This interface isn't suitable for updating the substitution map in a
      // substituted SILFunctionType.
      // TODO(SILFunctionType): Is it suitable for any SILFunctionType??
      SmallVector<Type, 4> newReplacements;
      for (Type type : subs.getReplacementTypes()) {
        auto transformed = type.transformRec(fn);
        assert((type->isEqual(transformed) ||
                (type->hasTypeParameter() && transformed->hasTypeParameter()) ||
                (hasTypeErasedGenericClassType(type) &&
                 hasTypeErasedGenericClassType(transformed))) &&
               "Substituted SILFunctionType can't be transformed into a "
               "concrete type");
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
      if (transformSILParameter(param, changed, fn)) return Type();
      transInterfaceParams.push_back(param);
    }

    SmallVector<SILYieldInfo, 8> transInterfaceYields;
    for (SILYieldInfo yield : fnTy->getYields()) {
      if (transformSILYield(yield, changed, fn)) return Type();
      transInterfaceYields.push_back(yield);
    }

    SmallVector<SILResultInfo, 8> transInterfaceResults;
    for (SILResultInfo result : fnTy->getResults()) {
      if (transformSILResult(result, changed, fn)) return Type();
      transInterfaceResults.push_back(result);
    }

    Optional<SILResultInfo> transErrorResult;
    if (fnTy->hasErrorResult()) {
      SILResultInfo result = fnTy->getErrorResult();
      if (transformSILResult(result, changed, fn)) return Type();
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
    Type substRefTy = refTy.transformRec(fn);
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
      substParentTy = parentTy.transformRec(fn);
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
      substParentTy = parentTy.transformRec(fn);
      if (!substParentTy)
        return Type();

      if (substParentTy.getPointer() != parentTy.getPointer())
        anyChanged = true;
    }

    for (auto arg : bound->getGenericArgs()) {
      Type substArg = arg.transformRec(fn);
      if (!substArg)
        return Type();
      substArgs.push_back(substArg);
      if (substArg.getPointer() != arg.getPointer())
        anyChanged = true;
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
      Type newReplacement = replacement.transformRec(fn);
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
                                        newSubMap);
  }
  case TypeKind::NestedArchetype: {
    // Transform the root type of a nested opaque archetype.
    auto nestedType = cast<NestedArchetypeType>(base);
    auto root = dyn_cast<OpaqueTypeArchetypeType>(nestedType->getRoot());
    if (!root)
      return *this;
    
    auto substRoot = Type(root).transformRec(fn);
    if (substRoot.getPointer() == root) {
      return *this;
    }
    
    // Substitute the new root into the root of the interface type.
    return nestedType->getInterfaceType()->substRootParam(substRoot,
        LookUpConformanceInModule(root->getDecl()->getModuleContext()));
  }

  case TypeKind::ExistentialMetatype: {
    auto meta = cast<ExistentialMetatypeType>(base);
    auto instanceTy = meta->getInstanceType().transformRec(fn);
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
    auto instanceTy = meta->getInstanceType().transformRec(fn);
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
    auto selfTy = dynamicSelf->getSelfType().transformRec(fn);
    if (!selfTy)
      return Type();

    if (selfTy.getPointer() == dynamicSelf->getSelfType().getPointer())
      return *this;

    return DynamicSelfType::get(selfTy, selfTy->getASTContext());
  }

  case TypeKind::TypeAlias: {
    auto alias = cast<TypeAliasType>(base);
    Type oldUnderlyingType = Type(alias->getSinglyDesugaredType());
    Type newUnderlyingType = oldUnderlyingType.transformRec(fn);
    if (!newUnderlyingType) return Type();

    Type oldParentType = alias->getParent();
    Type newParentType;
    if (oldParentType) {
      newParentType = oldParentType.transformRec(fn);
      if (!newParentType) return newUnderlyingType;
    }

    auto subMap = alias->getSubstitutionMap();
    for (Type oldReplacementType : subMap.getReplacementTypes()) {
      Type newReplacementType = oldReplacementType.transformRec(fn);
      if (!newReplacementType)
        return newUnderlyingType;

      // If anything changed with the replacement type, we lose the sugar.
      // FIXME: This is really unfortunate.
      if (newReplacementType.getPointer() != oldReplacementType.getPointer())
        return newUnderlyingType;
    }

    if (oldParentType.getPointer() == newParentType.getPointer() &&
        oldUnderlyingType.getPointer() == newUnderlyingType.getPointer())
      return *this;

    return TypeAliasType::get(alias->getDecl(), newParentType, subMap,
                              newUnderlyingType);
  }

  case TypeKind::Paren: {
    auto paren = cast<ParenType>(base);
    Type underlying = paren->getUnderlyingType().transformRec(fn);
    if (!underlying)
      return Type();

    if (underlying.getPointer() == paren->getUnderlyingType().getPointer())
      return *this;

    auto otherFlags = paren->getParameterFlags().withInOut(underlying->is<InOutType>());
    return ParenType::get(Ptr->getASTContext(), underlying->getInOutObjectType(), otherFlags);
  }

  case TypeKind::Tuple: {
    auto tuple = cast<TupleType>(base);
    bool anyChanged = false;
    SmallVector<TupleTypeElt, 4> elements;
    unsigned Index = 0;
    for (const auto &elt : tuple->getElements()) {
      Type eltTy = elt.getType().transformRec(fn);
      if (!eltTy)
        return Type();

      // If nothing has changed, just keep going.
      if (!anyChanged && eltTy.getPointer() == elt.getType().getPointer()) {
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

      // Add the new tuple element, with the new type, no initializer,
      elements.push_back(elt.getWithType(eltTy));
      ++Index;
    }

    if (!anyChanged)
      return *this;

    return TupleType::get(elements, Ptr->getASTContext());
  }


  case TypeKind::DependentMember: {
    auto dependent = cast<DependentMemberType>(base);
    auto dependentBase = dependent->getBase().transformRec(fn);
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

      auto substType = type.transformRec(fn);
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

      substParams.emplace_back(substType, label, flags, internalLabel);
    }

    // Transform result type.
    auto resultTy = function->getResult().transformRec(fn);
    if (!resultTy)
      return Type();

    if (resultTy.getPointer() != function->getResult().getPointer())
      isUnchanged = false;

    // Transform the global actor.
    Type globalActorType;
    if (Type origGlobalActorType = function->getGlobalActor()) {
      globalActorType = origGlobalActorType.transformRec(fn);
      if (!globalActorType)
        return Type();

      if (globalActorType.getPointer() != origGlobalActorType.getPointer())
        isUnchanged = false;
    }

    if (auto genericFnType = dyn_cast<GenericFunctionType>(base)) {
#ifndef NDEBUG
      // Check that generic parameters won't be trasnformed.
      // Transform generic parameters.
      for (auto param : genericFnType->getGenericParams()) {
        assert(Type(param).transformRec(fn).getPointer() == param &&
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
    auto baseTy = slice->getBaseType().transformRec(fn);
    if (!baseTy)
      return Type();

    if (baseTy.getPointer() == slice->getBaseType().getPointer())
      return *this;

    return ArraySliceType::get(baseTy);
  }

  case TypeKind::Optional: {
    auto optional = cast<OptionalType>(base);
    auto baseTy = optional->getBaseType().transformRec(fn);
    if (!baseTy)
      return Type();

    if (baseTy.getPointer() == optional->getBaseType().getPointer())
      return *this;

    return OptionalType::get(baseTy);
  }

  case TypeKind::Dictionary: {
    auto dict = cast<DictionaryType>(base);
    auto keyTy = dict->getKeyType().transformRec(fn);
    if (!keyTy)
      return Type();

    auto valueTy = dict->getValueType().transformRec(fn);
    if (!valueTy)
      return Type();

    if (keyTy.getPointer() == dict->getKeyType().getPointer() &&
        valueTy.getPointer() == dict->getValueType().getPointer())
      return *this;

    return DictionaryType::get(keyTy, valueTy);
  }

  case TypeKind::LValue: {
    auto lvalue = cast<LValueType>(base);
    auto objectTy = lvalue->getObjectType().transformRec(fn);
    if (!objectTy || objectTy->hasError())
      return objectTy;

    return objectTy.getPointer() == lvalue->getObjectType().getPointer() ?
      *this : LValueType::get(objectTy);
  }

  case TypeKind::InOut: {
    auto inout = cast<InOutType>(base);
    auto objectTy = inout->getObjectType().transformRec(fn);
    if (!objectTy || objectTy->hasError())
      return objectTy;
    
    return objectTy.getPointer() == inout->getObjectType().getPointer() ?
      *this : InOutType::get(objectTy);
  }

  case TypeKind::ProtocolComposition: {
    auto pc = cast<ProtocolCompositionType>(base);
    SmallVector<Type, 4> substMembers;
    auto members = pc->getMembers();
    bool anyChanged = false;
    unsigned index = 0;
    for (auto member : members) {
      auto substMember = member.transformRec(fn);
      if (!substMember)
        return Type();
      
      if (anyChanged) {
        substMembers.push_back(substMember);
        ++index;
        continue;
      }
      
      if (substMember.getPointer() != member.getPointer()) {
        anyChanged = true;
        substMembers.append(members.begin(), members.begin() + index);
        substMembers.push_back(substMember);
      }
      
      ++index;
    }
    
    if (!anyChanged)
      return *this;
    
    return ProtocolCompositionType::get(Ptr->getASTContext(),
                                        substMembers,
                                        pc->hasExplicitAnyObject());
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

static ReferenceCounting getClassReferenceCounting(ClassDecl *theClass) {
  return (theClass->usesObjCObjectModel()
          ? ReferenceCounting::ObjC
          : ReferenceCounting::Native);
}

ReferenceCounting TypeBase::getReferenceCounting() {
  CanType type = getCanonicalType();
  ASTContext &ctx = type->getASTContext();

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
    return getClassReferenceCounting(cast<ClassType>(type)->getDecl());
  case TypeKind::BoundGenericClass:
    return getClassReferenceCounting(
                                  cast<BoundGenericClassType>(type)->getDecl());
  case TypeKind::UnboundGeneric:
    return getClassReferenceCounting(
                    cast<ClassDecl>(cast<UnboundGenericType>(type)->getDecl()));

  case TypeKind::DynamicSelf:
    return cast<DynamicSelfType>(type).getSelfType()
        ->getReferenceCounting();

  case TypeKind::PrimaryArchetype:
  case TypeKind::OpenedArchetype:
  case TypeKind::NestedArchetype:
  case TypeKind::OpaqueTypeArchetype: {
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

Type TypeBase::openAnyExistentialType(OpenedArchetypeType *&opened) {
  assert(isAnyExistentialType());
  if (auto metaty = getAs<ExistentialMetatypeType>()) {
    opened = OpenedArchetypeType::get(metaty->getInstanceType());
    if (metaty->hasRepresentation())
      return MetatypeType::get(opened, metaty->getRepresentation());
    else
      return MetatypeType::get(opened);
  }
  opened = OpenedArchetypeType::get(this);
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
    auto *tupleType = TupleType::get(newElts, ctx)->castTo<TupleType>();
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
        DerivativeFunctionTypeError::TypeAndIndex(
            originalResultType, /*index*/ 0));
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
      // Error if paraneter has no tangent space.
      if (!paramTan) {
        return llvm::make_error<DerivativeFunctionTypeError>(
            this,
            DerivativeFunctionTypeError::Kind::
                NonDifferentiableDifferentiabilityParameter,
            DerivativeFunctionTypeError::TypeAndIndex(
                paramType, 0));
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
      // Error if paraneter has no tangent space.
      if (!paramTan) {
        return llvm::make_error<DerivativeFunctionTypeError>(
            this,
            DerivativeFunctionTypeError::Kind::
                NonDifferentiableDifferentiabilityParameter,
            DerivativeFunctionTypeError::TypeAndIndex(paramType, i));
      }
      if (diffParam.isInOut()) {
        hasInoutDiffParameter = true;
        continue;
      }
      pullbackResults.push_back(TupleTypeElt(paramTan->getType(), Identifier(),
                                             diffParam.getParameterFlags()));
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
