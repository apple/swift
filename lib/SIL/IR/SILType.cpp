//===--- SILType.cpp - Defines SILType ------------------------------------===//
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

#include "swift/SIL/SILType.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/Type.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/SILFunctionConventions.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include <tuple>

using namespace swift;
using namespace swift::Lowering;

/// Find a local archetype represented by this type.
/// It is assumed by this method that the type contains
/// at most one opened archetype.
/// Typically, it would be called from a type visitor.
/// It checks only the type itself, but does not try to
/// recursively check any children of this type, because
/// this is the task of the type visitor invoking it.
/// \returns The found archetype or empty type otherwise.
CanOpenedArchetypeType swift::getOpenedArchetypeOf(CanType Ty) {
  return dyn_cast_or_null<OpenedArchetypeType>(getLocalArchetypeOf(Ty));
}
CanLocalArchetypeType swift::getLocalArchetypeOf(CanType Ty) {
  if (!Ty)
    return CanLocalArchetypeType();
  while (auto MetaTy = dyn_cast<AnyMetatypeType>(Ty))
    Ty = MetaTy.getInstanceType();
  return dyn_cast<LocalArchetypeType>(Ty);
}

SILType SILType::getExceptionType(const ASTContext &C) {
  return SILType::getPrimitiveObjectType(C.getErrorExistentialType());
}

SILType SILType::getNativeObjectType(const ASTContext &C) {
  return SILType(C.TheNativeObjectType, SILValueCategory::Object);
}

SILType SILType::getBridgeObjectType(const ASTContext &C) {
  return SILType(C.TheBridgeObjectType, SILValueCategory::Object);
}

SILType SILType::getRawPointerType(const ASTContext &C) {
  return getPrimitiveObjectType(C.TheRawPointerType);
}

SILType SILType::getBuiltinIntegerLiteralType(const ASTContext &C) {
  return getPrimitiveObjectType(C.TheIntegerLiteralType);
}

SILType SILType::getBuiltinIntegerType(unsigned bitWidth,
                                       const ASTContext &C) {
  return getPrimitiveObjectType(CanType(BuiltinIntegerType::get(bitWidth, C)));
}

SILType SILType::getBuiltinFloatType(BuiltinFloatType::FPKind Kind,
                                     const ASTContext &C) {
  CanType ty;
  switch (Kind) {
  case BuiltinFloatType::IEEE16:  ty = C.TheIEEE16Type; break;
  case BuiltinFloatType::IEEE32:  ty = C.TheIEEE32Type; break;
  case BuiltinFloatType::IEEE64:  ty = C.TheIEEE64Type; break;
  case BuiltinFloatType::IEEE80:  ty = C.TheIEEE80Type; break;
  case BuiltinFloatType::IEEE128: ty = C.TheIEEE128Type; break;
  case BuiltinFloatType::PPC128:  ty = C.ThePPC128Type; break;
  }
  return getPrimitiveObjectType(ty);
}

SILType SILType::getBuiltinWordType(const ASTContext &C) {
  return getPrimitiveObjectType(CanType(BuiltinIntegerType::getWordType(C)));
}

SILType SILType::getOptionalType(SILType type) {
  return getPrimitiveType(type.getASTType().wrapInOptionalType(),
                          type.getCategory())
      .copyingMoveOnlyWrapper(type);
}

SILType SILType::getEmptyTupleType(const ASTContext &C) {
  return getPrimitiveObjectType(C.TheEmptyTupleType);
}

SILType SILType::getSILTokenType(const ASTContext &C) {
  return getPrimitiveObjectType(C.TheSILTokenType);
}

SILType SILType::getPackIndexType(const ASTContext &C) {
  return getPrimitiveObjectType(C.ThePackIndexType);
}

bool SILType::isTrivial(const SILFunction &F) const {
  auto contextType = hasTypeParameter() ? F.mapTypeIntoContext(*this) : *this;
  
  return F.getTypeLowering(contextType).isTrivial();
}

bool SILType::isOrContainsRawPointer(const SILFunction &F) const {
  auto contextType = hasTypeParameter() ? F.mapTypeIntoContext(*this) : *this;
  return F.getTypeLowering(contextType).isOrContainsRawPointer();
}

bool SILType::isNonTrivialOrContainsRawPointer(const SILFunction *f) const {
  auto contextType = hasTypeParameter() ? f->mapTypeIntoContext(*this) : *this;
  const TypeLowering &tyLowering = f->getTypeLowering(contextType);
  bool result = !tyLowering.isTrivial() || tyLowering.isOrContainsRawPointer();
  assert((result || !isFunctionTypeWithContext()) &&
         "a function type with context must either be non trivial or marked as containing a pointer");
  return result;
}

bool SILType::isEmpty(const SILFunction &F) const {
  // Infinite types are never empty.
  if (F.getTypeLowering(*this).getRecursiveProperties().isInfinite()) {
    return false;
  }
  
  if (auto tupleTy = getAs<TupleType>()) {
    // A tuple is empty if it either has no elements or if all elements are
    // empty.
    for (unsigned idx = 0, num = tupleTy->getNumElements(); idx < num; ++idx) {
      if (!getTupleElementType(idx).isEmpty(F))
        return false;
    }
    return true;
  }
  if (StructDecl *structDecl = getStructOrBoundGenericStruct()) {
    // Also, a struct is empty if it either has no fields or if all fields are
    // empty.
    SILModule &module = F.getModule();
    TypeExpansionContext typeEx = F.getTypeExpansionContext();
    for (VarDecl *field : structDecl->getStoredProperties()) {
      if (!getFieldType(field, module, typeEx).isEmpty(F))
        return false;
    }
    return true;
  }
  return false;
}

bool SILType::isReferenceCounted(SILModule &M) const {
  return M.Types.getTypeLowering(*this,
                                 TypeExpansionContext::minimal())
    .isReferenceCounted();
}

bool SILType::isReferenceCounted(SILFunction *f) const {
  return isReferenceCounted(f->getModule());
}

bool SILType::isNoReturnFunction(SILModule &M,
                                 TypeExpansionContext context) const {
  if (auto funcTy = dyn_cast<SILFunctionType>(getASTType()))
    return funcTy->isNoReturnFunction(M, context);

  return false;
}

Lifetime SILType::getLifetime(const SILFunction &F) const {
  auto contextType = hasTypeParameter() ? F.mapTypeIntoContext(*this) : *this;
  const auto &lowering = F.getTypeLowering(contextType);
  auto properties = lowering.getRecursiveProperties();
  if (properties.isTrivial())
    return Lifetime::None;
  return properties.isLexical() ? Lifetime::Lexical : Lifetime::EagerMove;
}

std::string SILType::getMangledName() const {
  Mangle::ASTMangler mangler;
  return mangler.mangleTypeWithoutPrefix(getRawASTType());
}

std::string SILType::getAsString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  print(OS);
  return OS.str();
}

bool SILType::isPointerSizeAndAligned(SILModule &M,
                                      ResilienceExpansion expansion) const {
  auto &C = getASTContext();
  if (isHeapObjectReferenceType()
      || getASTType()->isEqual(C.TheRawPointerType)) {
    return true;
  }
  if (auto intTy = dyn_cast<BuiltinIntegerType>(getASTType()))
    return intTy->getWidth().isPointerWidth();

  if (auto underlyingField = getSingletonAggregateFieldType(M, expansion)) {
    return underlyingField.isPointerSizeAndAligned(M, expansion);
  }
  
  return false;
}

static bool isSingleSwiftRefcounted(SILModule &M,
                                    SILType SILTy,
                                    ResilienceExpansion expansion,
                                    bool didUnwrapOptional) {
  auto &C = M.getASTContext();
  
  // Unwrap one layer of optionality.
  // TODO: Or more generally, any fragile enum with a single payload and single
  // no-payload case.
  if (!didUnwrapOptional) {
    if (auto objectTy = SILTy.getOptionalObjectType()) {
      return ::isSingleSwiftRefcounted(M, objectTy, expansion, true);
    }
  }

  // Unwrap singleton aggregates.
  if (auto underlyingField = SILTy.getSingletonAggregateFieldType(M, expansion)) {
    return ::isSingleSwiftRefcounted(M, underlyingField, expansion,
                                     didUnwrapOptional);
  }
  
  auto Ty = SILTy.getASTType();
  
  // Easy cases: Builtin.NativeObject and boxes are always Swift-refcounted.
  if (Ty == C.TheNativeObjectType)
    return true;
  if (isa<SILBoxType>(Ty))
    return true;
  
  // Is the type a Swift-refcounted class?
  // For a generic type, consider its superclass constraint, if any.
  auto ClassTy = Ty;
  if (auto archety = dyn_cast<ArchetypeType>(Ty)) {
    if (auto superclass = Ty->getSuperclass()) {
      ClassTy = superclass->getCanonicalType();
    }
  }
  // For an existential type, consider its superclass constraint, if it carries
  // no witness tables.
  if (Ty->isAnyExistentialType()) {
    auto layout = Ty->getExistentialLayout();
    // Must be no protocol constraints that aren't @objc or @_marker.
    if (layout.containsNonObjCProtocol) {
      for (auto proto : layout.getProtocols()) {
        if (!proto->isObjC() && !proto->isMarkerProtocol()) {
          return false;
        }
      }
    }
    
    // The Error existential has its own special layout.
    if (layout.isErrorExistential()) {
      return false;
    }
    
    // We can look at the superclass constraint, if any, to see if it's
    // Swift-refcounted.
    if (!layout.getSuperclass()) {
      return false;
    }
    ClassTy = layout.getSuperclass()->getCanonicalType();
  }
  
  // TODO: Does the base class we found have fully native Swift ancestry,
  // so we can use Swift native refcounting on it?
  return false;
}

bool SILType::isSingleSwiftRefcounted(SILModule &M,
                                      ResilienceExpansion expansion) const {
  return ::isSingleSwiftRefcounted(M, *this, expansion, false);
}

// Reference cast from representations with single pointer low bits.
// Only reference cast to simple single pointer representations.
//
// TODO: handle casting to a loadable existential by generating
// init_existential_ref. Until then, only promote to a heap object dest.
//
// This cannot allow trivial-to-reference casts, as required by
// isRCIdentityPreservingCast.
bool SILType::canRefCast(SILType operTy, SILType resultTy, SILModule &M) {
  auto fromTy = operTy.unwrapOptionalType();
  auto toTy = resultTy.unwrapOptionalType();
  return (fromTy.isHeapObjectReferenceType() || fromTy.isClassExistentialType())
    && toTy.isHeapObjectReferenceType();
}

static bool needsFieldSubstitutions(const AbstractionPattern &origType) {
  if (origType.isTypeParameter()) return false;
  auto type = origType.getType();
  if (!type->hasTypeParameter()) return false;
  return type.findIf([](CanType type) {
    return isa<PackExpansionType>(type);
  });
}

static void addFieldSubstitutionsIfNeeded(TypeConverter &TC, SILType ty,
                                          ValueDecl *field,
                                          AbstractionPattern &origType) {
  if (needsFieldSubstitutions(origType)) {
    auto subMap = ty.getASTType()->getContextSubstitutionMap(
                    &TC.M, field->getDeclContext());
    origType = origType.withSubstitutions(subMap);
  }
}

SILType SILType::getFieldType(VarDecl *field, TypeConverter &TC,
                              TypeExpansionContext context) const {
  AbstractionPattern origFieldTy = TC.getAbstractionPattern(field);
  addFieldSubstitutionsIfNeeded(TC, *this, field, origFieldTy);

  CanType substFieldTy;
  if (field->hasClangNode()) {
    substFieldTy = origFieldTy.getType();
  } else {
    // We want to specifically use getASTType() here instead of getRawASTType()
    // to ensure that we can correctly get our substituted field type. If we
    // need to rewrap the type layer, we do it below.
    substFieldTy =
        getASTType()->getTypeOfMember(&TC.M, field)->getCanonicalType();
  }

  auto loweredTy =
      TC.getLoweredRValueType(context, origFieldTy, substFieldTy);

  // If this type is not a class type, then we propagate "move only"-ness to the
  // field. Example:
  if (!getClassOrBoundGenericClass() && isMoveOnlyWrapped())
    loweredTy = SILMoveOnlyWrappedType::get(loweredTy);

  if (isAddress() || getClassOrBoundGenericClass() != nullptr) {
    return SILType::getPrimitiveAddressType(loweredTy);
  } else {
    return SILType::getPrimitiveObjectType(loweredTy);
  }
}

SILType SILType::getFieldType(VarDecl *field, SILModule &M,
                              TypeExpansionContext context) const {
  return getFieldType(field, M.Types, context);
}

SILType SILType::getFieldType(VarDecl *field, SILFunction *fn) const {
  return getFieldType(field, fn->getModule(), fn->getTypeExpansionContext());
}

SILType SILType::getFieldType(intptr_t fieldIndex, SILFunction *function) const {
  NominalTypeDecl *decl = getNominalOrBoundGenericNominal();
  assert(decl && "expected nominal type");
  VarDecl *field = getIndexedField(decl, fieldIndex);
  return getFieldType(field, function->getModule(), function->getTypeExpansionContext());
}

StringRef SILType::getFieldName(intptr_t fieldIndex) const {
  NominalTypeDecl *decl = getNominalOrBoundGenericNominal();
  VarDecl *field = getIndexedField(decl, fieldIndex);
  return field->getName().str();
}

unsigned SILType::getNumNominalFields() const {
  auto *nominal = getNominalOrBoundGenericNominal();
  assert(nominal && "expected nominal type");
  return getNumFieldsInNominal(nominal);
}

SILType SILType::getEnumElementType(EnumElementDecl *elt, TypeConverter &TC,
                                    TypeExpansionContext context) const {
  assert(elt->getDeclContext() == getEnumOrBoundGenericEnum());
  assert(elt->hasAssociatedValues());

  if (auto objectType = getASTType().getOptionalObjectType()) {
    assert(elt == TC.Context.getOptionalSomeDecl());
    return SILType(objectType, getCategory()).copyingMoveOnlyWrapper(*this);
  }

  // If the case is indirect, then the payload is boxed.
  if (elt->isIndirect() || elt->getParentEnum()->isIndirect()) {
    auto box = TC.getBoxTypeForEnumElement(context, *this, elt);
    return SILType(SILType::getPrimitiveObjectType(box).getASTType(),
                   getCategory());
  }

  auto origEltType = TC.getAbstractionPattern(elt);
  addFieldSubstitutionsIfNeeded(TC, *this, elt, origEltType);

  auto substEltTy = getASTType()->getTypeOfMember(
      &TC.M, elt, elt->getArgumentInterfaceType());
  auto loweredTy = TC.getLoweredRValueType(
      context, TC.getAbstractionPattern(elt), substEltTy);

  return SILType(loweredTy, getCategory()).copyingMoveOnlyWrapper(*this);
}

SILType SILType::getEnumElementType(EnumElementDecl *elt, SILModule &M,
                                    TypeExpansionContext context) const {
  return getEnumElementType(elt, M.Types, context);
}

SILType SILType::getEnumElementType(EnumElementDecl *elt,
                                    SILFunction *fn) const {
  return getEnumElementType(elt, fn->getModule(),
                            fn->getTypeExpansionContext());
}

EnumElementDecl *SILType::getEnumElement(int caseIndex) const {
  EnumDecl *enumDecl = getEnumOrBoundGenericEnum();
  for (auto elemWithIndex : llvm::enumerate(enumDecl->getAllElements())) {
    if ((int)elemWithIndex.index() == caseIndex)
      return elemWithIndex.value();
  }
  llvm_unreachable("invalid enum case index");
}

bool SILType::isLoadableOrOpaque(const SILFunction &F) const {
  SILModule &M = F.getModule();
  return isLoadable(F) || !SILModuleConventions(M).useLoweredAddresses();
}

bool SILType::isAddressOnly(const SILFunction &F) const {
  auto contextType = hasTypeParameter() ? F.mapTypeIntoContext(*this) : *this;
    
  return F.getTypeLowering(contextType).isAddressOnly();
}

SILType SILType::substGenericArgs(SILModule &M, SubstitutionMap SubMap,
                                  TypeExpansionContext context) const {
  auto fnTy = castTo<SILFunctionType>();
  auto canFnTy = CanSILFunctionType(fnTy->substGenericArgs(M, SubMap, context));
  return SILType::getPrimitiveObjectType(canFnTy);
}

bool SILType::isHeapObjectReferenceType() const {
  auto &C = getASTContext();
  auto Ty = getASTType();
  if (Ty->isBridgeableObjectType())
    return true;
  if (Ty->isEqual(C.TheNativeObjectType))
    return true;
  if (Ty->isEqual(C.TheBridgeObjectType))
    return true;
  if (is<SILBoxType>())
    return true;
  return false;
}

bool SILType::aggregateContainsRecord(SILType Record, SILModule &Mod,
                                      TypeExpansionContext context) const {
  assert(!hasArchetype() && "Agg should be proven to not be generic "
                             "before passed to this function.");
  assert(!Record.hasArchetype() && "Record should be proven to not be generic "
                                    "before passed to this function.");

  llvm::SmallVector<SILType, 8> Worklist;
  Worklist.push_back(*this);

  // For each "subrecord" of agg in the worklist...
  while (!Worklist.empty()) {
    SILType Ty = Worklist.pop_back_val();

    // If it is record, we succeeded. Return true.
    if (Ty == Record)
      return true;

    // Otherwise, we gather up sub-records that need to be checked for
    // checking... First handle the tuple case.
    if (CanTupleType TT = Ty.getAs<TupleType>()) {
      for (unsigned i = 0, e = TT->getNumElements(); i != e; ++i)
        Worklist.push_back(Ty.getTupleElementType(i));
      continue;
    }

    // Then if we have an enum...
    if (EnumDecl *E = Ty.getEnumOrBoundGenericEnum()) {
      for (auto Elt : E->getAllElements())
        if (Elt->hasAssociatedValues())
          Worklist.push_back(Ty.getEnumElementType(Elt, Mod, context));
      continue;
    }

    // Then if we have a struct address...
    if (StructDecl *S = Ty.getStructOrBoundGenericStruct())
      for (VarDecl *Var : S->getStoredProperties())
        Worklist.push_back(Ty.getFieldType(Var, Mod, context));

    // If we have a class address, it is a pointer so it cannot contain other
    // types.

    // If we reached this point, then this type has no subrecords. Since it does
    // not equal our record, we can skip it.
  }

  // Could not find the record in the aggregate.
  return false;
}

bool SILType::aggregateHasUnreferenceableStorage() const {
  if (auto s = getStructOrBoundGenericStruct()) {
    return s->hasUnreferenceableStorage();
  }
  // Tuples with pack expansions don't *actually* have unreferenceable
  // storage, but the optimizer needs to be taught how to handle them,
  // and it won't do that correctly in the short term.
  if (auto t = getAs<TupleType>()) {
    return t.containsPackExpansionType();
  }
  return false;
}

SILType SILType::getOptionalObjectType() const {
  if (auto objectTy = getASTType().getOptionalObjectType()) {
    return SILType(objectTy, getCategory()).copyingMoveOnlyWrapper(*this);
  }

  return SILType();
}

SILType SILType::unwrapOptionalType() const {
  if (auto objectTy = removingMoveOnlyWrapper().getOptionalObjectType()) {
    return objectTy.copyingMoveOnlyWrapper(*this);
  }

  return *this;
}

/// True if the given type value is nonnull, and the represented type is NSError
/// or CFError, the error classes for which we support "toll-free" bridging to
/// Error existentials.
static bool isBridgedErrorClass(ASTContext &ctx, Type t) {
  // There's no bridging if ObjC interop is disabled.
  if (!ctx.LangOpts.EnableObjCInterop)
    return false;

  if (!t)
    return false;

  if (auto archetypeType = t->getAs<ArchetypeType>())
    t = archetypeType->getSuperclass();

  // NSError (TODO: and CFError) can be bridged.
  auto nsErrorType = ctx.getNSErrorType();
  if (t && nsErrorType && nsErrorType->isExactSuperclassOf(t))
    return true;

  return false;
}

ExistentialRepresentation
SILType::getPreferredExistentialRepresentation(Type containedType) const {
  // Existential metatypes always use metatype representation.
  if (is<ExistentialMetatypeType>())
    return ExistentialRepresentation::Metatype;
  
  // If the type isn't existential, then there is no representation.
  if (!isExistentialType())
    return ExistentialRepresentation::None;

  auto layout = getASTType().getExistentialLayout();

  if (layout.isErrorExistential()) {
    // NSError or CFError references can be adopted directly as Error
    // existentials.
    if (isBridgedErrorClass(getASTContext(), containedType)) {
      return ExistentialRepresentation::Class;
    } else {
      return ExistentialRepresentation::Boxed;
    }
  }

  // A class-constrained protocol composition can adopt the conforming
  // class reference directly.
  if (layout.requiresClass())
    return ExistentialRepresentation::Class;
  
  // Otherwise, we need to use a fixed-sized buffer.
  return ExistentialRepresentation::Opaque;
}

bool
SILType::canUseExistentialRepresentation(ExistentialRepresentation repr,
                                         Type containedType) const {
  switch (repr) {
  case ExistentialRepresentation::None:
    return !isAnyExistentialType();
  case ExistentialRepresentation::Opaque:
  case ExistentialRepresentation::Class:
  case ExistentialRepresentation::Boxed: {
    // Look at the protocols to see what representation is appropriate.
    if (!isExistentialType())
      return false;

    auto layout = getASTType().getExistentialLayout();

    switch (layout.getKind()) {
    // A class-constrained composition uses ClassReference representation;
    // otherwise, we use a fixed-sized buffer.
    case ExistentialLayout::Kind::Class:
      return repr == ExistentialRepresentation::Class;
    // The (uncomposed) Error existential uses a special boxed
    // representation. It can also adopt class references of bridged
    // error types directly.
    case ExistentialLayout::Kind::Error:
      return repr == ExistentialRepresentation::Boxed
        || (repr == ExistentialRepresentation::Class
            && isBridgedErrorClass(getASTContext(), containedType));
    case ExistentialLayout::Kind::Opaque:
      return repr == ExistentialRepresentation::Opaque;
    }
    llvm_unreachable("unknown existential kind!");
  }
  case ExistentialRepresentation::Metatype:
    return is<ExistentialMetatypeType>();
  }

  llvm_unreachable("Unhandled ExistentialRepresentation in switch.");
}

SILType SILType::mapTypeOutOfContext() const {
  return SILType::getPrimitiveType(mapTypeOutOfContext(getASTType()),
                                   getCategory());
}

CanType SILType::mapTypeOutOfContext(CanType type) {
  return type->mapTypeOutOfContext()->getCanonicalType();
}

CanType swift::getSILBoxFieldLoweredType(TypeExpansionContext context,
                                         SILBoxType *type, TypeConverter &TC,
                                         unsigned index) {
  auto fieldTy = SILType::getPrimitiveObjectType(
    type->getLayout()->getFields()[index].getLoweredType());
  
  // Map the type into the new expansion context, which might substitute opaque
  // types.
  auto sig = type->getLayout()->getGenericSignature();
  fieldTy = TC.getTypeLowering(fieldTy, context, sig)
              .getLoweredType();
  
  // Apply generic arguments if the layout is generic.
  if (auto subMap = type->getSubstitutions()) {
    fieldTy = fieldTy.subst(TC,
                            QuerySubstitutionMap{subMap},
                            LookUpConformanceInSubstitutionMap(subMap),
                            sig);
  }

  return fieldTy.getRawASTType();
}

ValueOwnershipKind
SILResultInfo::getOwnershipKind(SILFunction &F,
                                CanSILFunctionType FTy) const {
  auto &M = F.getModule();

  bool IsTrivial =
      getSILStorageType(M, FTy, TypeExpansionContext::minimal()).isTrivial(F);
  switch (getConvention()) {
  case ResultConvention::Indirect:
  case ResultConvention::Pack:
    return SILModuleConventions(M).isSILIndirect(*this) ? OwnershipKind::None
                                                        : OwnershipKind::Owned;
  case ResultConvention::Autoreleased:
  case ResultConvention::Owned:
    return OwnershipKind::Owned;
  case ResultConvention::Unowned:
  case ResultConvention::UnownedInnerPointer:
    if (IsTrivial)
      return OwnershipKind::None;
    return OwnershipKind::Unowned;
  }

  llvm_unreachable("Unhandled ResultConvention in switch.");
}

SILModuleConventions::SILModuleConventions(SILModule &M)
    : M(&M), loweredAddresses(M.useLoweredAddresses()) {}

bool SILModuleConventions::isReturnedIndirectlyInSIL(SILType type,
                                                     SILModule &M) {
  if (SILModuleConventions(M).loweredAddresses) {
    return M.Types.getTypeLowering(type, TypeExpansionContext::minimal())
        .isAddressOnly();
  }

  return false;
}

bool SILModuleConventions::isPassedIndirectlyInSIL(SILType type, SILModule &M) {
  if (SILModuleConventions(M).loweredAddresses) {
    return M.Types.getTypeLowering(type, TypeExpansionContext::minimal())
        .isAddressOnly();
  }

  return false;
}

bool SILFunctionType::isNoReturnFunction(SILModule &M,
                                         TypeExpansionContext context) const {
  for (unsigned i = 0, e = getNumResults(); i < e; ++i) {
    if (getResults()[i].getReturnValueType(M, this, context)->isUninhabited())
      return true;
  }

  return false;
}

#ifndef NDEBUG
static bool areOnlyAbstractionDifferent(CanType type1, CanType type2) {
  assert(type1->isLegalSILType());
  assert(type2->isLegalSILType());

  // Exact equality is fine.
  if (type1 == type2)
    return true;

  // Either both types should be optional or neither should be.
  if (auto object1 = type1.getOptionalObjectType()) {
    auto object2 = type2.getOptionalObjectType();
    if (!object2)
      return false;
    return areOnlyAbstractionDifferent(object1, object2);
  }
  if (type2.getOptionalObjectType())
    return false;

  // Either both types should be tuples or neither should be.
  if (auto tuple1 = dyn_cast<TupleType>(type1)) {
    auto tuple2 = dyn_cast<TupleType>(type2);
    if (!tuple2)
      return false;
    if (tuple1->getNumElements() != tuple2->getNumElements())
      return false;
    for (auto i : indices(tuple2->getElementTypes()))
      if (!areOnlyAbstractionDifferent(tuple1.getElementType(i),
                                       tuple2.getElementType(i)))
        return false;
    return true;
  }
  if (isa<TupleType>(type2))
    return false;

  // Either both types should be metatypes or neither should be.
  if (auto meta1 = dyn_cast<AnyMetatypeType>(type1)) {
    auto meta2 = dyn_cast<AnyMetatypeType>(type2);
    if (!meta2)
      return false;
    if (meta1.getInstanceType() != meta2.getInstanceType())
      return false;
    return true;
  }

  // Either both types should be functions or neither should be.
  if (auto fn1 = dyn_cast<SILFunctionType>(type1)) {
    auto fn2 = dyn_cast<SILFunctionType>(type2);
    if (!fn2)
      return false;
    // TODO: maybe there are checks we can do here?
    (void)fn1;
    (void)fn2;
    return true;
  }
  if (isa<SILFunctionType>(type2))
    return false;

  llvm_unreachable("no other types should differ by abstraction");
}
#endif

/// Given two SIL types which are representations of the same type,
/// check whether they have an abstraction difference.
bool SILType::hasAbstractionDifference(SILFunctionTypeRepresentation rep,
                                       SILType type2) {
  CanType ct1 = getASTType();
  CanType ct2 = type2.getASTType();
  assert(getSILFunctionLanguage(rep) == SILFunctionLanguage::C ||
         areOnlyAbstractionDifferent(ct1, ct2));
  (void)ct1;
  (void)ct2;

  // Assuming that we've applied the same substitutions to both types,
  // abstraction equality should equal type equality.
  return (*this != type2);
}

bool SILType::isLoweringOf(TypeExpansionContext context, SILModule &Mod,
                           CanType formalType) {
  SILType loweredType = *this;
  if (formalType->hasOpaqueArchetype() &&
      context.shouldLookThroughOpaqueTypeArchetypes() &&
      loweredType.getASTType() ==
          Mod.Types.getLoweredRValueType(context, formalType))
    return true;

  // Optional lowers its contained type.
  SILType loweredObjectType = loweredType.getOptionalObjectType();
  CanType formalObjectType = formalType.getOptionalObjectType();

  if (loweredObjectType) {
    return formalObjectType &&
           loweredObjectType.isLoweringOf(context, Mod, formalObjectType);
  }

  // Metatypes preserve their instance type through lowering.
  if (auto loweredMT = loweredType.getAs<MetatypeType>()) {
    if (auto formalMT = dyn_cast<MetatypeType>(formalType)) {
      return loweredMT.getInstanceType() == formalMT.getInstanceType();
    }
  }

  if (auto loweredEMT = loweredType.getAs<ExistentialMetatypeType>()) {
    if (auto formalEMT = dyn_cast<ExistentialMetatypeType>(formalType)) {
      return loweredEMT.getInstanceType() == formalEMT.getInstanceType();
    }
  }

  // TODO: Function types go through a more elaborate lowering.
  // For now, just check that a SIL function type came from some AST function
  // type.
  if (loweredType.is<SILFunctionType>())
    return isa<AnyFunctionType>(formalType);

  // Tuples are lowered elementwise.
  // TODO: Will this always be the case?
  if (auto loweredTT = loweredType.getAs<TupleType>()) {
    if (auto formalTT = dyn_cast<TupleType>(formalType)) {
      if (loweredTT->getNumElements() != formalTT->getNumElements())
        return false;
      for (unsigned i = 0, e = loweredTT->getNumElements(); i < e; ++i) {
        auto loweredTTEltType =
            SILType::getPrimitiveAddressType(loweredTT.getElementType(i));
        if (!loweredTTEltType.isLoweringOf(context, Mod,
                                           formalTT.getElementType(i)))
          return false;
      }
      return true;
    }
  }

  // The pattern of a pack expansion is lowered.
  if (auto formalExpansion = dyn_cast<PackExpansionType>(formalType)) {
    if (auto loweredExpansion = loweredType.getAs<PackExpansionType>()) {
      return loweredExpansion.getCountType() == formalExpansion.getCountType()
         && SILType::getPrimitiveAddressType(loweredExpansion.getPatternType())
               .isLoweringOf(context, Mod, formalExpansion.getPatternType());
    }
    return false;
  }

  // Dynamic self has the same lowering as its contained type.
  if (auto dynamicSelf = dyn_cast<DynamicSelfType>(formalType))
    formalType = dynamicSelf.getSelfType();

  // Other types are preserved through lowering.
  return loweredType.getASTType() == formalType;
}

bool SILType::isDifferentiable(SILModule &M) const {
  return getASTType()
      ->getAutoDiffTangentSpace(LookUpConformanceInModule(M.getSwiftModule()))
      .has_value();
}

Type
TypeBase::replaceSubstitutedSILFunctionTypesWithUnsubstituted(SILModule &M) const {
  return Type(const_cast<TypeBase *>(this)).transform([&](Type t) -> Type {
    if (auto *f = t->getAs<SILFunctionType>()) {
      auto sft = f->getUnsubstitutedType(M);
      
      // Also eliminate substituted function types in the arguments, yields,
      // and returns of the function type.
      bool didChange = false;
      SmallVector<SILParameterInfo, 4> newParams;
      SmallVector<SILYieldInfo, 4> newYields;
      SmallVector<SILResultInfo, 4> newResults;
      llvm::Optional<SILResultInfo> newErrorResult;
      for (auto param : sft->getParameters()) {
        auto newParamTy = param.getInterfaceType()
          ->replaceSubstitutedSILFunctionTypesWithUnsubstituted(M)
          ->getCanonicalType();
        didChange |= param.getInterfaceType() != newParamTy;
        newParams.push_back(SILParameterInfo(newParamTy, param.getConvention()));
      }
      for (auto yield : sft->getYields()) {
        auto newYieldTy = yield.getInterfaceType()
          ->replaceSubstitutedSILFunctionTypesWithUnsubstituted(M)
          ->getCanonicalType();
        didChange |= yield.getInterfaceType() != newYieldTy;
        newYields.push_back(SILYieldInfo(newYieldTy, yield.getConvention()));
      }
      for (auto result : sft->getResults()) {
        auto newResultTy = result.getInterfaceType()
          ->replaceSubstitutedSILFunctionTypesWithUnsubstituted(M)
          ->getCanonicalType();
        didChange |= result.getInterfaceType() != newResultTy;
        newResults.push_back(SILResultInfo(newResultTy, result.getConvention()));
      }
      if (auto error = sft->getOptionalErrorResult()) {
        auto newErrorTy = error->getInterfaceType()
          ->replaceSubstitutedSILFunctionTypesWithUnsubstituted(M)
          ->getCanonicalType();
        didChange |= error->getInterfaceType() != newErrorTy;
        newErrorResult = SILResultInfo(newErrorTy, error->getConvention());
      }
      
      if (!didChange)
        return sft;
      
      return SILFunctionType::get(sft->getInvocationGenericSignature(),
                                  sft->getExtInfo(), sft->getCoroutineKind(),
                                  sft->getCalleeConvention(),
                                  newParams, newYields, newResults,
                                  newErrorResult,
                                  SubstitutionMap(),
                                  SubstitutionMap(),
                                  M.getASTContext());
    }
    return t;
  });
}

bool SILType::isEffectivelyExhaustiveEnumType(SILFunction *f) {
  EnumDecl *decl = getEnumOrBoundGenericEnum();
  assert(decl && "Called for a non enum type");
  return decl->isEffectivelyExhaustive(f->getModule().getSwiftModule(),
                                       f->getResilienceExpansion());
}

SILType SILType::getSILBoxFieldType(const SILFunction *f, unsigned field) const {
  auto *boxTy = getASTType()->getAs<SILBoxType>();
  if (!boxTy)
    return SILType();
  return ::getSILBoxFieldType(f->getTypeExpansionContext(), boxTy,
                              f->getModule().Types, field);
}

SILType
SILType::getSingletonAggregateFieldType(SILModule &M,
                                        ResilienceExpansion expansion) const {
  if (auto tuple = getAs<TupleType>()) {
    if (tuple->getNumElements() == 1) {
      return getTupleElementType(0);
    }
  }

  if (auto structDecl = getStructOrBoundGenericStruct()) {
    // If the struct has to be accessed resiliently from this resilience domain,
    // we can't assume anything about its layout.
    if (structDecl->isResilient(M.getSwiftModule(), expansion)) {
      return SILType();
    }

    // C ABI wackiness may cause a single-field struct to have different layout
    // from its field.
    if (structDecl->hasUnreferenceableStorage()
        || structDecl->hasClangNode()) {
      return SILType();
    }

    // A single-field struct with custom alignment has different layout from its
    // field.
    if (structDecl->getAttrs().hasAttribute<AlignmentAttr>()) {
      return SILType();
    }

    // If there's only one stored property, we have the layout of its field.
    auto allFields = structDecl->getStoredProperties();
    
    if (allFields.size() == 1) {
      auto fieldTy = getFieldType(
          allFields[0], M,
          TypeExpansionContext(expansion, M.getSwiftModule(),
                               M.isWholeModule()));
      if (!M.isTypeABIAccessible(fieldTy,
                       TypeExpansionContext::maximalResilienceExpansionOnly())){
        return SILType();
      }
      return fieldTy;
    }

    return SILType();
  }

  if (auto enumDecl = getEnumOrBoundGenericEnum()) {
    // If the enum has to be accessed resiliently from this resilience domain,
    // we can't assume anything about its layout.
    if (enumDecl->isResilient(M.getSwiftModule(), expansion)) {
      return SILType();
    }

    auto allCases = enumDecl->getAllElements();
    
    auto theCase = allCases.begin();
    if (!allCases.empty() && std::next(theCase) == allCases.end()
        && (*theCase)->hasAssociatedValues()) {
      auto enumEltTy = getEnumElementType(
          *theCase, M,
          TypeExpansionContext(expansion, M.getSwiftModule(),
                               M.isWholeModule()));
      if (!M.isTypeABIAccessible(enumEltTy,
                       TypeExpansionContext::maximalResilienceExpansionOnly())){
        return SILType();
      }
      return enumEltTy;
    }

    return SILType();
  }

  return SILType();
}

bool SILType::isMoveOnly() const {
  // Nominal types are move-only if declared as such.
  if (isMoveOnlyNominalType())
    return true;


  // TODO: Nonescaping closures ought to be treated as move-only in SIL.
  // They aren't marked move-only now, because the necessary move-only passes
  // haven't yet been enabled. We can get away without this because we don't
  // ever copy them after SILGen and any incidental copies we emit are always
  // optimized out, but we ought to enforce this once move-only type support
  // is robust.
  /*
  if (auto fnTy = getAs<SILFunctionType>()) {
    return fnTy->isTrivialNoEscape();
  }
   */
  return isMoveOnlyWrapped();
}

bool SILType::isMoveOnlyNominalType() const {
  if (auto *nom = getNominalOrBoundGenericNominal())
    if (nom->isMoveOnly())
      return true;
  return false;
}

bool SILType::isPureMoveOnly() const {
  if (auto *nom = getNominalOrBoundGenericNominal())
    if (nom->isMoveOnly())
      return true;
  return false;
}

bool SILType::isValueTypeWithDeinit() const {
  // Do not look inside an aggregate type that has a user-deinit, for which
  // memberwise-destruction is not equivalent to aggregate destruction.
  if (auto *nominal = getNominalOrBoundGenericNominal()) {
    return nominal->getValueTypeDestructor() != nullptr;
  }
  return false;
}

SILType SILType::getInstanceTypeOfMetatype(SILFunction *function) const {
  auto metaType = castTo<MetatypeType>();
  CanType instanceTy = metaType.getInstanceType();
  auto &tl = function->getModule().Types.getTypeLowering(instanceTy, TypeExpansionContext(*function));
  return tl.getLoweredType();
}

bool SILType::isOrContainsObjectiveCClass() const {
  return getASTType().findIf([](Type ty) {
    if (ClassDecl *cd = ty->getClassOrBoundGenericClass()) {
      if (cd->isForeign() || cd->getObjectModel() == ReferenceCounting::ObjC)
        return true;
    }
    if (ty->is<ProtocolCompositionType>())
      return true;
    return false;
  });
}

static bool hasImmortalAttr(NominalTypeDecl *nominal) {
  if (auto *semAttr = nominal->getAttrs().getAttribute<SemanticsAttr>()) {
    if (semAttr->Value == semantics::ARC_IMMORTAL) {
      return true;
    }
  }
  return false;
}

static bool nominalIsMarkedAsImmortal(NominalTypeDecl *nominal) {
  if (hasImmortalAttr(nominal))
    return true;

  if (!isa<ProtocolDecl>(nominal)) {
    for (ProtocolDecl *p : nominal->getAllProtocols()) {
      if (hasImmortalAttr(p))
        return true;
    }
  }
  return false;
}

bool SILType::isMarkedAsImmortal() const {
  NominalTypeDecl *nominal = getNominalOrBoundGenericNominal();
  if (!nominal)
    return false;

  if (nominalIsMarkedAsImmortal(nominal))
    return true;

  if (ClassDecl *cl = dyn_cast<ClassDecl>(nominal)) {
    cl = cl->getSuperclassDecl();
    while (cl) {
      if (nominalIsMarkedAsImmortal(cl))
        return true;
      cl = cl->getSuperclassDecl();
    }
  }
  return false;
}

intptr_t SILType::getFieldIdxOfNominalType(StringRef fieldName) const {
  auto *nominal = getNominalOrBoundGenericNominal();
  if (!nominal)
    return -1;

  SmallVector<NominalTypeDecl *, 5> decls;
  decls.push_back(nominal);
  if (auto *cd = dyn_cast<ClassDecl>(nominal)) {
    while ((cd = cd->getSuperclassDecl()) != nullptr) {
      decls.push_back(cd);
    }
  }
  std::reverse(decls.begin(), decls.end());

  intptr_t idx = 0;
  for (auto *decl : decls) {
    for (VarDecl *field : decl->getStoredProperties()) {
      if (field->getName().str() == fieldName)
        return idx;
      idx++;
    }
  }
  return -1;
}

intptr_t SILType::getCaseIdxOfEnumType(StringRef caseName) const {
  auto *enumDecl = getEnumOrBoundGenericEnum();
  if (!enumDecl)
    return -1;

  intptr_t idx = 0;
  for (EnumElementDecl *elem : enumDecl->getAllElements()) {
    if (elem->getNameStr() == caseName)
      return idx;
    idx++;
  }
  return -1;
}

std::string SILType::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  print(os);
  return str;
}

SILType SILType::addingMoveOnlyWrapperToBoxedType(const SILFunction *fn) {
  auto boxTy = castTo<SILBoxType>();
  auto *oldLayout = boxTy->getLayout();
  auto oldField = oldLayout->getFields()[0];
  if (oldField.getLoweredType()->is<SILMoveOnlyWrappedType>())
    return *this;
  assert(!oldField.getLoweredType()->isPureMoveOnly() &&
         "Cannot moveonlywrapped in a moveonly type");
  auto newField =
      SILField(SILMoveOnlyWrappedType::get(oldField.getLoweredType()),
               oldField.isMutable());
  auto *newLayout =
      SILLayout::get(fn->getASTContext(), oldLayout->getGenericSignature(),
                     {newField}, oldLayout->capturesGenericEnvironment());
  auto newBoxType = SILBoxType::get(fn->getASTContext(), newLayout,
                                    boxTy->getSubstitutions());
  return SILType::getPrimitiveObjectType(newBoxType);
}

SILType SILType::removingMoveOnlyWrapperToBoxedType(const SILFunction *fn) {
  auto boxTy = castTo<SILBoxType>();
  auto *oldLayout = boxTy->getLayout();
  auto oldField = oldLayout->getFields()[0];
  auto *moveOnlyWrapped =
      oldField.getLoweredType()->getAs<SILMoveOnlyWrappedType>();
  if (!moveOnlyWrapped)
    return *this;
  auto newField =
      SILField(moveOnlyWrapped->getInnerType(), oldField.isMutable());
  auto *newLayout =
      SILLayout::get(fn->getASTContext(), oldLayout->getGenericSignature(),
                     {newField}, oldLayout->capturesGenericEnvironment());
  auto newBoxType = SILBoxType::get(fn->getASTContext(), newLayout,
                                    boxTy->getSubstitutions());
  return SILType::getPrimitiveObjectType(newBoxType);
}
