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
#include "swift/AST/Type.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/SILFunctionConventions.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace swift::Lowering;

SILType SILType::getExceptionType(const ASTContext &C) {
  return SILType::getPrimitiveObjectType(C.getExceptionType());
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
  auto &ctx = type.getASTContext();
  auto optType = BoundGenericEnumType::get(ctx.getOptionalDecl(), Type(),
                                           { type.getASTType() });
  return getPrimitiveType(CanType(optType), type.getCategory());
}

SILType SILType::getSILTokenType(const ASTContext &C) {
  return getPrimitiveObjectType(C.TheSILTokenType);
}

bool SILType::isTrivial(const SILFunction &F) const {
  auto contextType = hasTypeParameter() ? F.mapTypeIntoContext(*this) : *this;
  
  return F.getTypeLowering(contextType).isTrivial();
}

bool SILType::isReferenceCounted(SILModule &M) const {
  return M.Types.getTypeLowering(*this,
                                 TypeExpansionContext::minimal())
    .isReferenceCounted();
}

bool SILType::isNoReturnFunction(SILModule &M,
                                 TypeExpansionContext context) const {
  if (auto funcTy = dyn_cast<SILFunctionType>(getASTType()))
    return funcTy->isNoReturnFunction(M, context);

  return false;
}

std::string SILType::getMangledName() const {
  Mangle::ASTMangler mangler(false/*use dwarf mangling*/);
  return mangler.mangleTypeWithoutPrefix(getASTType());
}

std::string SILType::getAsString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  print(OS);
  return OS.str();
}

bool SILType::isPointerSizeAndAligned() {
  auto &C = getASTContext();
  if (isHeapObjectReferenceType()
      || getASTType()->isEqual(C.TheRawPointerType)) {
    return true;
  }
  if (auto intTy = dyn_cast<BuiltinIntegerType>(getASTType()))
    return intTy->getWidth().isPointerWidth();

  return false;
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

SILType SILType::getFieldType(VarDecl *field, TypeConverter &TC,
                              TypeExpansionContext context) const {
  AbstractionPattern origFieldTy = TC.getAbstractionPattern(field);
  CanType substFieldTy;
  if (field->hasClangNode()) {
    substFieldTy = origFieldTy.getType();
  } else {
    substFieldTy =
      getASTType()->getTypeOfMember(&TC.M, field, nullptr)->getCanonicalType();
  }

  auto loweredTy =
      TC.getLoweredRValueType(context, origFieldTy, substFieldTy);
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

SILType SILType::getEnumElementType(EnumElementDecl *elt, TypeConverter &TC,
                                    TypeExpansionContext context) const {
  assert(elt->getDeclContext() == getEnumOrBoundGenericEnum());
  assert(elt->hasAssociatedValues());

  if (auto objectType = getASTType().getOptionalObjectType()) {
    assert(elt == TC.Context.getOptionalSomeDecl());
    return SILType(objectType, getCategory());
  }

  // If the case is indirect, then the payload is boxed.
  if (elt->isIndirect() || elt->getParentEnum()->isIndirect()) {
    auto box = TC.getBoxTypeForEnumElement(context, *this, elt);
    return SILType(SILType::getPrimitiveObjectType(box).getASTType(),
                   getCategory());
  }

  auto substEltTy =
    getASTType()->getTypeOfMember(&TC.M, elt,
                                  elt->getArgumentInterfaceType());
  auto loweredTy = TC.getLoweredRValueType(
      context, TC.getAbstractionPattern(elt), substEltTy);

  return SILType(loweredTy, getCategory());
}

SILType SILType::getEnumElementType(EnumElementDecl *elt, SILModule &M,
                                    TypeExpansionContext context) const {
  return getEnumElementType(elt, M.Types, context);
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
  return false;
}

SILType SILType::getOptionalObjectType() const {
  if (auto objectTy = getASTType().getOptionalObjectType()) {
    return SILType(objectTy, getCategory());
  }

  return SILType();
}

SILType SILType::unwrapOptionalType() const {
  if (auto objectTy = getOptionalObjectType()) {
    return objectTy;
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
  
  return fieldTy.getASTType();
}

ValueOwnershipKind
SILResultInfo::getOwnershipKind(SILFunction &F,
                                CanSILFunctionType FTy) const {
  auto &M = F.getModule();

  bool IsTrivial =
      getSILStorageType(M, FTy, TypeExpansionContext::minimal()).isTrivial(F);
  switch (getConvention()) {
  case ResultConvention::Indirect:
    return SILModuleConventions(M).isSILIndirect(*this)
               ? ValueOwnershipKind::None
               : ValueOwnershipKind::Owned;
  case ResultConvention::Autoreleased:
  case ResultConvention::Owned:
    return ValueOwnershipKind::Owned;
  case ResultConvention::Unowned:
  case ResultConvention::UnownedInnerPointer:
    if (IsTrivial)
      return ValueOwnershipKind::None;
    return ValueOwnershipKind::Unowned;
  }

  llvm_unreachable("Unhandled ResultConvention in switch.");
}

SILModuleConventions::SILModuleConventions(SILModule &M)
    : M(&M),
      loweredAddresses(!M.getASTContext().LangOpts.EnableSILOpaqueValues
                       || M.getStage() == SILStage::Lowered)
{}

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

  // Dynamic self has the same lowering as its contained type.
  if (auto dynamicSelf = dyn_cast<DynamicSelfType>(formalType))
    formalType = dynamicSelf.getSelfType();

  // Other types are preserved through lowering.
  return loweredType.getASTType() == formalType;
}

bool SILType::isDifferentiable(SILModule &M) const {
  return getASTType()
      ->getAutoDiffTangentSpace(LookUpConformanceInModule(M.getSwiftModule()))
      .hasValue();
}

Type
TypeBase::replaceSubstitutedSILFunctionTypesWithUnsubstituted(SILModule &M) const {
  return Type(const_cast<TypeBase*>(this)).transform([&](Type t) -> Type {
    if (auto f = t->getAs<SILFunctionType>()) {
      auto sft = f->getUnsubstitutedType(M);
      
      // Also eliminate substituted function types in the arguments, yields,
      // and returns of the function type.
      bool didChange = false;
      SmallVector<SILParameterInfo, 4> newParams;
      SmallVector<SILYieldInfo, 4> newYields;
      SmallVector<SILResultInfo, 4> newResults;
      Optional<SILResultInfo> newErrorResult;
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
