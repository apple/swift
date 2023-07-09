//===--- SILType.h - Defines the SILType type -------------------*- C++ -*-===//
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
// This file defines the SILType class, which is used to refer to SIL
// representation types.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILTYPE_H
#define SWIFT_SIL_SILTYPE_H

#include "swift/AST/SILLayout.h"
#include "swift/AST/Types.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/Lifetime.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {

class ASTContext;
class VarDecl;
class SILFunction;

namespace Lowering {
  class AbstractionPattern;
  class TypeConverter;
}

} // end namespace swift
  
namespace swift {

/// Find an opened archetype represented by this type.
/// It is assumed by this method that the type contains
/// at most one opened archetype.
/// Typically, it would be called from a type visitor.
/// It checks only the type itself, but does not try to
/// recursively check any children of this type, because
/// this is the task of the type visitor invoking it.
/// \returns The found opened archetype or empty type otherwise.
CanOpenedArchetypeType getOpenedArchetypeOf(CanType Ty);
CanLocalArchetypeType getLocalArchetypeOf(CanType Ty);

/// How an existential type container is represented.
enum class ExistentialRepresentation {
  /// The type is not existential.
  None,
  /// The container uses an opaque existential container, with a fixed-sized
  /// buffer. The type is address-only and is manipulated using the
  /// {init,open,deinit}_existential_addr family of instructions.
  Opaque,
  /// The container uses a class existential container, which holds a reference
  /// to the class instance that conforms to the protocol. The type is
  /// reference-counted and is manipulated using the
  /// {init,open}_existential_ref family of instructions.
  Class,
  /// The container uses a metatype existential container, which holds a
  /// reference to the type metadata for a type that
  /// conforms to the protocol. The type is trivial, and is manipulated using
  /// the {init,open}_existential_metatype family of instructions.
  Metatype,
  /// The container uses a boxed existential container, which is a
  /// reference-counted buffer that indirectly contains the
  /// conforming value. The type is manipulated
  /// using the {alloc,open,dealloc}_existential_box family of instructions.
  /// The container may be able to directly adopt a class reference using
  /// init_existential_ref for some class types.
  Boxed,
};

/// The value category.
enum class SILValueCategory : uint8_t {
  /// An object is a value of the type.
  Object,

  /// An address is a pointer to an allocated variable of the type
  /// (possibly uninitialized).
  Address,
};

class SILPrinter;
class SILParser;

/// SILType - A Swift type that has been lowered to a SIL representation type.
/// In addition to the Swift type system, SIL adds "address" types that can
/// reference any Swift type (but cannot take the address of an address). *T
/// is the type of an address pointing at T.
class SILType {
public:
  /// The unsigned is a SILValueCategory.
  using ValueType = llvm::PointerIntPair<TypeBase *, 2, unsigned>;

private:
  ValueType value;

  /// Private constructor. SILTypes are normally vended by
  /// TypeConverter::getLoweredType().
  SILType(CanType ty, SILValueCategory category)
      : value(ty.getPointer(), unsigned(category)) {
    if (!ty) return;
    assert(ty->isLegalSILType() &&
           "constructing SILType with type that should have been "
           "eliminated by SIL lowering");
  }
  
  SILType(ValueType value) : value(value) {
  }

  friend class Lowering::TypeConverter;
  friend struct llvm::DenseMapInfo<SILType>;
  friend class SILPrinter;
  friend class SILParser;

public:
  SILType() = default;

  /// getPrimitiveType - Form a SILType for a primitive type that does not
  /// require any special handling (i.e., not a function or aggregate type).
  static SILType getPrimitiveType(CanType T, SILValueCategory category) {
    return SILType(T, category);
  }

  /// Form the type of an r-value, given a Swift type that either does
  /// not require any special handling or has already been
  /// appropriately lowered.
  static SILType getPrimitiveObjectType(CanType T) {
    return SILType(T, SILValueCategory::Object);
  }

  /// Form the type for the address of an object, given a Swift type
  /// that either does not require any special handling or has already
  /// been appropriately lowered.
  static SILType getPrimitiveAddressType(CanType T) {
    return SILType(T, SILValueCategory::Address);
  }

  bool isNull() const { return value.getPointer() == nullptr; }
  explicit operator bool() const { return bool(value.getPointer()); }

  SILValueCategory getCategory() const {
    return SILValueCategory(value.getInt());
  }

  /// Returns the \p Category variant of this type.
  SILType getCategoryType(SILValueCategory Category) const {
    return SILType(getRawASTType(), Category);
  }

  /// Returns the variant of this type that matches \p Ty.getCategory()
  SILType copyCategory(SILType Ty) const {
    return getCategoryType(Ty.getCategory());
  }

  /// Returns the address variant of this type.  Instructions which
  /// manipulate memory will generally work with object addresses.
  SILType getAddressType() const {
    return SILType(getRawASTType(), SILValueCategory::Address);
  }

  /// Returns the object variant of this type.  Note that address-only
  /// types are not legal to manipulate directly as objects in SIL.
  SILType getObjectType() const {
    return SILType(getRawASTType(), SILValueCategory::Object);
  }

  /// Returns the canonical AST type referenced by this SIL type.
  ///
  /// NOTE:
  /// 1. The returned AST type may not be a proper formal type.
  ///    For example, it may contain a SILFunctionType instead of a
  ///    FunctionType.
  /// 2. The returned type may not be the same as the original
  ///    unlowered type that produced this SILType (even after
  ///    canonicalization). If you need it, you must pass it separately.
  ///    For example, `AnyObject.Type` may get lowered to
  ///    `$@thick AnyObject.Type`, for which the AST type will be
  ///    `@thick AnyObject.Type`.
  ///    More generally, you cannot recover a formal type from
  ///    a lowered type. See docs/SIL.rst for more details.
  /// 3. If the underlying type is move only, the returned CanType will not be
  ///    pointer equal to the RawASTType since we return the unwrapped inner
  ///    type. This is done under the assumption that in all cases where we are
  ///    performing these AST queries on SILType, we are not interested in the
  ///    move only-ness of the value (which we can query separately anyways).
  CanType getASTType() const {
    return removingMoveOnlyWrapper().getRawASTType();
  }

  /// Returns the canonical AST type references by this SIL type without looking
  /// through move only. Should only be used by internal utilities of SILType.
  CanType getRawASTType() const { return CanType(value.getPointer()); }

public:
  // FIXME -- Temporary until LLDB adopts getASTType()
  [[deprecated("Please use getASTType()")]] CanType getSwiftRValueType() const {
    return getASTType();
  }

  /// Returns the AbstractCC of a function type.
  /// The SILType must refer to a function type.
  SILFunctionTypeRepresentation getFunctionRepresentation() const {
    return castTo<SILFunctionType>()->getRepresentation();
  }

  /// Cast the Swift type referenced by this SIL type, or return null if the
  /// cast fails.
  template<typename TYPE>
  typename CanTypeWrapperTraits<TYPE>::type
  getAs() const { return dyn_cast<TYPE>(getASTType()); }

  /// Cast the Swift type referenced by this SIL type, which must be of the
  /// specified subtype.
  template<typename TYPE>
  typename CanTypeWrapperTraits<TYPE>::type
  castTo() const { return cast<TYPE>(getASTType()); }

  /// Returns true if the Swift type referenced by this SIL type is of the
  /// specified subtype.
  template<typename TYPE>
  bool is() const { return isa<TYPE>(getASTType()); }

  bool isVoid() const {
    return value.getPointer()->isVoid();
  }

  /// Whether the type is an enum, struct, or tuple.
  bool isAggregate() {
    return is<TupleType>() || is<StructType>() ||
           is<BoundGenericStructType>() || is<EnumType>() ||
           is<BoundGenericEnumType>();
  }

  /// Retrieve the ClassDecl for a type that maps to a Swift class or
  /// bound generic class type.
  SWIFT_IMPORT_UNSAFE
  ClassDecl *getClassOrBoundGenericClass() const {
    return getASTType().getClassOrBoundGenericClass();
  }
  /// Retrieve the StructDecl for a type that maps to a Swift struct or
  /// bound generic struct type.
  SWIFT_IMPORT_UNSAFE
  StructDecl *getStructOrBoundGenericStruct() const {
    return getASTType().getStructOrBoundGenericStruct();
  }
  /// Retrieve the EnumDecl for a type that maps to a Swift enum or
  /// bound generic enum type.
  SWIFT_IMPORT_UNSAFE
  EnumDecl *getEnumOrBoundGenericEnum() const {
    return getASTType().getEnumOrBoundGenericEnum();
  }
  
  /// Returns true if this type is an enum or contains an enum.
  bool isOrHasEnum() const {
    return getASTType().findIf([](Type ty) {
      return ty->getEnumOrBoundGenericEnum() != nullptr;
    });
  }

  bool isBuiltinInteger() const {
    return is<BuiltinIntegerType>();
  }

  bool isBuiltinFixedWidthInteger(unsigned width) const {
    BuiltinIntegerType *bi = getAs<BuiltinIntegerType>();
    return bi && bi->isFixedWidth(width);
  }

  bool isBuiltinFloat() const {
    return is<BuiltinFloatType>();
  }

  bool isBuiltinVector() const {
    return is<BuiltinVectorType>();
  }

  SWIFT_IMPORT_UNSAFE
  SILType getBuiltinVectorElementType() const {
    auto vector = castTo<BuiltinVectorType>();
    return getPrimitiveObjectType(vector.getElementType());
  }

  /// Retrieve the NominalTypeDecl for a type that maps to a Swift
  /// nominal or bound generic nominal type.
  SWIFT_IMPORT_UNSAFE
  NominalTypeDecl *getNominalOrBoundGenericNominal() const {
    return getASTType().getNominalOrBoundGenericNominal();
  }

  /// If this type maps to a Swift class, check if that class is a foreign
  /// reference type.
  bool isForeignReferenceType() const {
    return getASTType().isForeignReferenceType();
  }

  /// True if the type is an address type.
  bool isAddress() const { return getCategory() == SILValueCategory::Address; }

  /// True if the type is an object type.
  bool isObject() const { return getCategory() == SILValueCategory::Object; }

  /// isAddressOnly - True if the type, or the referenced type of an address
  /// type, is address-only.  For example, it could be a resilient struct or
  /// something of unknown size.
  ///
  /// This is equivalent to, but possibly faster than, calling
  /// tc.getTypeLowering(type).isAddressOnly().
  static bool isAddressOnly(CanType type,
                            Lowering::TypeConverter &tc,
                            CanGenericSignature sig,
                            TypeExpansionContext expansion);

  /// Return true if this type must be returned indirectly.
  ///
  /// This is equivalent to, but possibly faster than, calling
  /// tc.getTypeLowering(type).isReturnedIndirectly().
  static bool isFormallyReturnedIndirectly(CanType type,
                                           Lowering::TypeConverter &tc,
                                           CanGenericSignature sig) {
    return isAddressOnly(type, tc, sig, TypeExpansionContext::minimal());
  }

  /// Return true if this type must be passed indirectly.
  ///
  /// This is equivalent to, but possibly faster than, calling
  /// tc.getTypeLowering(type).isPassedIndirectly().
  static bool isFormallyPassedIndirectly(CanType type,
                                         Lowering::TypeConverter &tc,
                                         CanGenericSignature sig) {
    return isAddressOnly(type, tc, sig, TypeExpansionContext::minimal());
  }

  /// True if the type, or the referenced type of an address type, is loadable.
  /// This is the opposite of isAddressOnly.
  bool isLoadable(const SILFunction &F) const {
    return !isAddressOnly(F);
  }

  bool isLoadable(const SILFunction *f) const { return isLoadable(*f); }

  /// True if either:
  /// 1) The type, or the referenced type of an address type, is loadable.
  /// 2) The SIL Module conventions uses lowered addresses
  bool isLoadableOrOpaque(const SILFunction &F) const;

  /// True if the type, or the referenced type of an address type, is
  /// address-only. This is the opposite of isLoadable.
  bool isAddressOnly(const SILFunction &F) const;

  /// True if the underlying AST type is trivial, meaning it is loadable and can
  /// be trivially copied, moved or destroyed. Returns false for address types
  /// even though they are technically trivial.
  bool isTrivial(const SILFunction &F) const;

  bool isTrivial(const SILFunction *f) const { return isTrivial(*f); }

  /// True if the type is the Builtin.RawPointer or a struct/tuple/enum which
  /// contains a Builtin.RawPointer.
  /// Returns false for types for which this property is not known, e.g. generic
  /// types.
  bool isOrContainsRawPointer(const SILFunction &F) const;

  /// An efficient implementation of `!isTrivial() && isOrContainsRawPointer()`.
  bool isNonTrivialOrContainsRawPointer(const SILFunction *f) const;

  /// Whether the type contains a generic parameter declared as a parameter
  /// pack.
  bool hasParameterPack() const { return getASTType()->hasParameterPack(); }

  /// Whether the type contains a concrete pack.
  bool hasConcretePack() const { return getASTType()->hasConcretePack(); }

  /// Whether the type contains some flavor of pack.
  bool hasPack() const { return getASTType()->hasPack(); }

  /// True if the type is an empty tuple or an empty struct or a tuple or
  /// struct containing only empty types.
  bool isEmpty(const SILFunction &F) const;

  /// True if the type, or the referenced type of an address type, is known to
  /// be a scalar reference-counted type such as a class, box, or thick function
  /// type. Returns false for non-trivial aggregates.
  bool isReferenceCounted(SILModule &M) const;

  bool isReferenceCounted(SILFunction *f) const;

  bool isUnownedStorageType() const { return is<UnownedStorageType>(); }

  /// Returns true if the referenced type is a function type that never
  /// returns.
  bool isNoReturnFunction(SILModule &M, TypeExpansionContext context) const;

  /// Returns true if the referenced AST type has reference semantics, even if
  /// the lowered SIL type is known to be trivial.
  bool hasReferenceSemantics() const {
    return getASTType().hasReferenceSemantics();
  }

  /// The lifetime of values of this type (which are not otherwise annotated).
  ///
  /// Trivial types are ::None.
  /// Non-trivial types are ::Lexical by default.
  /// Non-trivial types which are annotated @_eagerMove are ::EagerMove.
  /// Aggregates which consist entirely of ::EagerMove fields are ::EagerMove.
  /// All other types are ::Lexical.
  Lifetime getLifetime(const SILFunction &F) const;

  /// Returns true if the referenced type is any sort of class-reference type,
  /// meaning anything with reference semantics that is not a function type.
  bool isAnyClassReferenceType() const {
    return getASTType().isAnyClassReferenceType();
  }
  
  /// Returns true if the referenced type is guaranteed to have a
  /// single-retainable-pointer representation.
  bool hasRetainablePointerRepresentation() const {
    return getASTType()->hasRetainablePointerRepresentation();
  }
  /// Returns true if the referenced type is an existential type.
  bool isExistentialType() const {
    return getASTType().isExistentialType();
  }
  /// Returns true if the referenced type is any kind of existential type.
  bool isAnyExistentialType() const {
    return getASTType().isAnyExistentialType();
  }
  /// Returns true if the referenced type is a class existential type.
  bool isClassExistentialType() const {
    return getASTType()->isClassExistentialType();
  }

  /// Returns true if the referenced type is an opened existential type
  /// (which is actually a kind of archetype).
  bool isOpenedExistential() const {
    return getASTType()->isOpenedExistential();
  }

  /// Returns true if the referenced type is expressed in terms of one
  /// or more opened existential types.
  bool hasOpenedExistential() const {
    return getASTType()->hasOpenedExistential();
  }

  TypeTraitResult canBeClass() const {
    return getASTType()->canBeClass();
  }

  /// Returns true if the referenced type is expressed in terms of one
  /// or more local archetypes.
  bool hasLocalArchetype() const {
    return getASTType()->hasLocalArchetype();
  }

  /// Returns true if the referenced type is expressed in terms of one
  /// or more parameterized protocol types.
  bool hasParameterizedExistential() const {
    return getASTType()->hasParameterizedExistential();
  }
  
  /// Returns the representation used by an existential type. If the concrete
  /// type is provided, this may return a specialized representation kind that
  /// can be used for that type. Otherwise, returns the most general
  /// representation kind for the type. Returns None if the type is not an
  /// existential type.
  ExistentialRepresentation
  getPreferredExistentialRepresentation(Type containedType = Type()) const;
  
  /// Returns true if the existential type can use operations for the given
  /// existential representation when working with values of the given type,
  /// or when working with an unknown type if containedType is null.
  bool
  canUseExistentialRepresentation(ExistentialRepresentation repr,
                                  Type containedType = Type()) const;
  
  /// True if the type contains a type parameter.
  bool hasTypeParameter() const {
    return getASTType()->hasTypeParameter();
  }
  
  /// True if the type is bridgeable to an ObjC object pointer type.
  bool isBridgeableObjectType() const {
    return getASTType()->isBridgeableObjectType();
  }

  static bool isClassOrClassMetatype(Type t) {
    if (auto *meta = t->getAs<AnyMetatypeType>()) {
      return bool(meta->getInstanceType()->getClassOrBoundGenericClass());
    } else {
      return bool(t->getClassOrBoundGenericClass());
    }
  }

  /// True if the type is a class type or class metatype type.
  bool isClassOrClassMetatype() {
    return isObject() && isClassOrClassMetatype(getASTType());
  }

  bool isFunctionTypeWithContext() const {
    if (auto *fTy = getASTType()->getAs<SILFunctionType>()) {
      return fTy->getExtInfo().hasContext();
    }
    return false;
  }

  bool isNoEscapeFunction() const {
    if (auto *fTy = getASTType()->getAs<SILFunctionType>()) {
      return fTy->isNoEscape();
    }
    return false;
  }

  /// True if the type involves any archetypes.
  bool hasArchetype() const { return getASTType()->hasArchetype(); }

  /// True if the type involves any opaque archetypes.
  bool hasOpaqueArchetype() const {
    return getASTType()->hasOpaqueArchetype();
  }
  
  /// Returns the ASTContext for the referenced Swift type.
  ASTContext &getASTContext() const {
    return getASTType()->getASTContext();
  }

  /// True if the given type has at least the size and alignment of a native
  /// pointer.
  bool isPointerSizeAndAligned(SILModule &M,
                               ResilienceExpansion expansion) const;

  /// True if the layout of the given type consists of a single native Swift-
  /// refcounted object reference, possibly nullable.
  bool isSingleSwiftRefcounted(SILModule &M,
                               ResilienceExpansion expansion) const;

  /// True if `operTy` can be cast by single-reference value into `resultTy`.
  static bool canRefCast(SILType operTy, SILType resultTy, SILModule &M);

  /// True if the type is block-pointer-compatible, meaning it either is a block
  /// or is an Optional with a block payload.
  bool isBlockPointerCompatible() const {
    // Look through one level of optionality.
    SILType ty = *this;
    if (auto optPayload = ty.getOptionalObjectType()) {
      ty = optPayload;
    }

    auto fTy = ty.getAs<SILFunctionType>();
    if (!fTy)
      return false;
    return fTy->getRepresentation() == SILFunctionType::Representation::Block;
  }

  bool isTuple() const { return is<TupleType>(); }
  bool isFunction() const { return is<SILFunctionType>(); }
  bool isMetatype() const { return is<MetatypeType>(); }

  VarDecl *getFieldDecl(intptr_t fieldIndex) const;

  /// Given that this is a nominal type, return the lowered type of
  /// the given field.  Applies substitutions as necessary.  The
  /// result will be an address type if the base type is an address
  /// type or a class.
  SILType getFieldType(VarDecl *field, Lowering::TypeConverter &TC,
                       TypeExpansionContext context) const;

  SILType getFieldType(VarDecl *field, SILModule &M,
                       TypeExpansionContext context) const;

  SILType getFieldType(VarDecl *field, SILFunction *fn) const;

  SILType getFieldType(intptr_t fieldIndex, SILFunction *function) const;

  SWIFT_IMPORT_UNSAFE
  StringRef getFieldName(intptr_t fieldIndex) const;

  // Returns < 0 if the field was not found.
  intptr_t getFieldIdxOfNominalType(StringRef fieldName) const;

  // Returns < 0 if the field was not found.
  intptr_t getCaseIdxOfEnumType(StringRef caseName) const;

  /// Given that this is an enum type, return the lowered type of the
  /// data for the given element.  Applies substitutions as necessary.
  /// The result will have the same value category as the base type.
  SILType getEnumElementType(EnumElementDecl *elt, Lowering::TypeConverter &TC,
                             TypeExpansionContext context) const;

  SILType getEnumElementType(EnumElementDecl *elt, SILModule &M,
                             TypeExpansionContext context) const;

  /// Given that this is an enum type, return the lowered type of the
  /// data for the given element.  Applies substitutions as necessary.
  /// The result will have the same value category as the base type.
  ///
  /// NOTE: Takes the type expansion context from \p fn.
  SILType getEnumElementType(EnumElementDecl *elt, SILFunction *fn) const;

  EnumElementDecl *getEnumElement(int caseIndex) const;

  /// Given that this is an enum type, return true if this type is effectively
  /// exhausted.
  bool isEffectivelyExhaustiveEnumType(SILFunction *f);

  unsigned getNumTupleElements() const {
    TupleType *tupleTy = castTo<TupleType>();
    return tupleTy->getNumElements();
  }

  /// Given that this is a tuple type, return the lowered type of the
  /// given tuple element.  The result will have the same value
  /// category as the base type.
  SILType getTupleElementType(intptr_t index) const {
    return SILType(castTo<TupleType>().getElementType(index), getCategory());
  }

  /// Given that this is a pack type, return the lowered type of the
  /// given pack element.  The result will have the same value
  /// category as the base type.
  SILType getPackElementType(unsigned index) const {
    return SILType(castTo<SILPackType>()->getElementType(index), getCategory());
  }

  unsigned getNumNominalFields() const ;

  /// Given that this is a pack expansion type, return the lowered type
  /// of the pattern type.  The result will have the same value category
  /// as the base type.
  SILType getPackExpansionPatternType() const {
    return SILType(castTo<PackExpansionType>().getPatternType(),
                   getCategory());
  }

  /// Return the immediate superclass type of this type, or null if
  /// it's the most-derived type.
  SILType getSuperclass() const {
    auto superclass = getASTType()->getSuperclass();
    if (!superclass) return SILType();
    return SILType::getPrimitiveObjectType(superclass->getCanonicalType());
  }

  /// Return true if Ty is a subtype of this exact SILType, or false otherwise.
  bool isExactSuperclassOf(SILType Ty) const {
    return getASTType()->isExactSuperclassOf(Ty.getASTType());
  }

  /// Return true if Ty is a subtype of this SILType, or if this SILType
  /// contains archetypes that can be found to form a supertype of Ty, or false
  /// otherwise.
  bool isBindableToSuperclassOf(SILType Ty) const {
    return getASTType()->isBindableToSuperclassOf(Ty.getASTType());
  }

  /// Look through reference-storage types on this type.
  SILType getReferenceStorageReferentType() const {
    return SILType(getASTType().getReferenceStorageReferent(), getCategory());
  }

  /// Return the reference ownership of this type if it is a reference storage
  /// type. Otherwise, return None.
  llvm::Optional<ReferenceOwnership> getReferenceStorageOwnership() const {
    auto type = getASTType()->getAs<ReferenceStorageType>();
    if (!type)
      return llvm::None;
    return type->getOwnership();
  }

  /// Attempt to wrap the passed in type as a type with reference ownership \p
  /// ownership. For simplicity, we always return an address since reference
  /// storage types may not be loadable (e.x.: weak ownership).
  SILType getReferenceStorageType(const ASTContext &ctx,
                                  ReferenceOwnership ownership) const {
    auto *type = ReferenceStorageType::get(getASTType(), ownership, ctx);
    return SILType::getPrimitiveAddressType(type->getCanonicalType());
  }

  /// Transform the function type SILType by replacing all of its interface
  /// generic args with the appropriate item from the substitution.
  ///
  /// Only call this with function types!
  SILType substGenericArgs(Lowering::TypeConverter &TC, SubstitutionMap SubMap,
                           TypeExpansionContext context) const;

  SILType substGenericArgs(SILModule &M, SubstitutionMap SubMap,
                           TypeExpansionContext context) const;

  /// If the original type is generic, pass the signature as genericSig.
  ///
  /// If the replacement types are generic, you must push a generic context
  /// first.
  SILType subst(Lowering::TypeConverter &tc, TypeSubstitutionFn subs,
                LookupConformanceFn conformances,
                CanGenericSignature genericSig = CanGenericSignature(),
                SubstOptions options = llvm::None) const;

  SILType subst(SILModule &M, TypeSubstitutionFn subs,
                LookupConformanceFn conformances,
                CanGenericSignature genericSig = CanGenericSignature(),
                SubstOptions options = llvm::None) const;

  SILType subst(Lowering::TypeConverter &tc,
                InFlightSubstitution &IFS,
                CanGenericSignature genericSig) const;

  SILType subst(Lowering::TypeConverter &tc, SubstitutionMap subs) const;

  SILType subst(SILModule &M, SubstitutionMap subs) const;
  SILType subst(SILModule &M, SubstitutionMap subs,
                TypeExpansionContext context) const;

  /// Return true if this type references a "ref" type that has a single pointer
  /// representation. Class existentials do not always qualify.
  bool isHeapObjectReferenceType() const;

  /// Returns true if this SILType is an aggregate that contains \p Ty
  bool aggregateContainsRecord(SILType Ty, SILModule &SILMod,
                               TypeExpansionContext context) const;

  /// Returns true if this SILType is an aggregate with unreferenceable storage,
  /// meaning it cannot be fully destructured in SIL.
  bool aggregateHasUnreferenceableStorage() const;

  /// Returns the lowered type for T if this type is Optional<T>;
  /// otherwise, return the null type.
  SILType getOptionalObjectType() const;

  /// Unwraps one level of optional type.
  /// Returns the lowered T if the given type is Optional<T>.
  /// Otherwise directly returns the given type.
  SILType unwrapOptionalType() const;

  /// Returns true if this is the AnyObject SILType;
  bool isAnyObject() const { return getASTType()->isAnyObject(); }

  /// Returns true if this type is a first class move only type or a move only
  /// wrapped type.
  bool isMoveOnly() const;

  /// Is this a type that is a first class move only type. This returns false
  /// for a move only wrapped type.
  bool isPureMoveOnly() const;

  /// Return true if this is a value type (struct/enum) that requires
  /// deinitialization beyond destruction of its members.
  bool isValueTypeWithDeinit() const;

  /// Returns true if and only if this type is a first class move only
  /// type. NOTE: Returns false if the type is a move only wrapped type.
  bool isMoveOnlyNominalType() const;

  /// Returns true if this SILType is a move only wrapper type.
  ///
  /// Canonical way to check if a SILType is move only. Using is/getAs/castTo
  /// will look through moveonly-ness.
  bool isMoveOnlyWrapped() const {
    return getRawASTType()->is<SILMoveOnlyWrappedType>();
  }

  /// If this is already a moveonlywrapped type, return *this. Otherwise, wrap
  /// the copyable type in the moveonlywrapper.
  SILType addingMoveOnlyWrapper() const {
    if (isMoveOnlyWrapped())
      return *this;
    auto newType = SILMoveOnlyWrappedType::get(getRawASTType());
    return SILType::getPrimitiveType(newType, getCategory());
  }

  /// If this is already a copyable type, just return *this. Otherwise, if this
  /// is a move only wrapped copyable type, return the inner type.
  SILType removingMoveOnlyWrapper() const {
    if (!isMoveOnlyWrapped())
      return *this;
    auto moveOnly = getRawASTType()->castTo<SILMoveOnlyWrappedType>();
    return SILType::getPrimitiveType(moveOnly->getInnerType(), getCategory());
  }

  /// If \p otherType is move only wrapped, return this type that is move only
  /// as well. Otherwise, returns self. Useful for propagating "move only"-ness
  /// from a parent type to a subtype.
  SILType copyingMoveOnlyWrapper(SILType otherType) const {
    if (otherType.isMoveOnlyWrapped()) {
      return addingMoveOnlyWrapper();
    }
    return *this;
  }

  /// If this is a box type containing a moveonlywrapped type, return a new box
  /// with the moveonlywrapped type unwrapped.
  ///
  /// DISCUSSION: This is separate from addingMoveOnlyWrapper since this API
  /// requires a SILFunction * and is specialized.
  SILType addingMoveOnlyWrapperToBoxedType(const SILFunction *fn);

  /// If this is a box type containing a copyable type, return a new box type
  /// with the copyable type wrapped in a moveonly wrapped type.
  ///
  /// DISCUSSION: This is separate from removingMoveOnlyWrapper since this API
  /// requires a SILFunction * and is specialized.
  SILType removingMoveOnlyWrapperToBoxedType(const SILFunction *fn);

  /// Returns a SILType with any archetypes mapped out of context.
  SILType mapTypeOutOfContext() const;

  /// Given a lowered type (but without any particular value category),
  /// map it out of its current context.  Equivalent to
  /// SILType::getPrimitiveObjectType(type).mapTypeOutOfContext().getASTType().
  static CanType mapTypeOutOfContext(CanType type);

  /// Given two SIL types which are representations of the same type,
  /// check whether they have an abstraction difference.
  bool hasAbstractionDifference(SILFunctionTypeRepresentation rep,
                                SILType type2);

  /// Returns true if this SILType could be potentially a lowering of the given
  /// formal type. Meant for verification purposes/assertions.
  bool isLoweringOf(TypeExpansionContext context, SILModule &M,
                    CanType formalType);

  /// Returns true if this SILType is a differentiable type.
  bool isDifferentiable(SILModule &M) const;

  /// If this is a SILBoxType, return getSILBoxFieldType(). Otherwise, return
  /// SILType().
  ///
  /// \p field Return the type of the ith field of the box. Default set to 0
  /// since we only support one field today. This is just future proofing.
  SILType getSILBoxFieldType(const SILFunction *f, unsigned field = 0) const;

  /// Returns the hash code for the SILType.
  llvm::hash_code getHashCode() const {
    return llvm::hash_combine(*this);
  }
  
  /// If a type is visibly a singleton aggregate (a tuple with one element, a
  /// struct with one field, or an enum with a single payload case), return the
  /// type of its field, which it is guaranteed to have identical layout to.
  SILType getSingletonAggregateFieldType(SILModule &M,
                                         ResilienceExpansion expansion) const;

  /// \returns true if this is a SILBoxType containing a noncopyable type.
  bool isBoxedNonCopyableType(const SILFunction *fn) const {
    if (!this->is<SILBoxType>())
      return false;
    return getSILBoxFieldType(fn).isMoveOnly();
  }

  bool isBoxedNonCopyableType(const SILFunction &fn) const {
    return isBoxedNonCopyableType(&fn);
  }

  bool isBoxedMoveOnlyWrappedType(const SILFunction *fn) const {
    if (!this->is<SILBoxType>())
      return false;
    return getSILBoxFieldType(fn).isMoveOnlyWrapped();
  }

  SILType getInstanceTypeOfMetatype(SILFunction *function) const;

  bool isOrContainsObjectiveCClass() const;

  bool isCalleeConsumedFunction() const {
    auto funcTy = castTo<SILFunctionType>();
    return funcTy->isCalleeConsumed() && !funcTy->isNoEscape();
  }

  bool isMarkedAsImmortal() const;

  //
  // Accessors for types used in SIL instructions:
  //
  
  /// Get the NativeObject type as a SILType.
  static SILType getNativeObjectType(const ASTContext &C);
  /// Get the BridgeObject type as a SILType.
  static SILType getBridgeObjectType(const ASTContext &C);
  /// Get the RawPointer type as a SILType.
  static SILType getRawPointerType(const ASTContext &C);
  /// Get a builtin integer type as a SILType.
  static SILType getBuiltinIntegerType(unsigned bitWidth, const ASTContext &C);
  /// Get the IntegerLiteral type as a SILType.
  static SILType getBuiltinIntegerLiteralType(const ASTContext &C);
  /// Get a builtin floating-point type as a SILType.
  static SILType getBuiltinFloatType(BuiltinFloatType::FPKind Kind,
                                     const ASTContext &C);
  /// Get the builtin word type as a SILType;
  static SILType getBuiltinWordType(const ASTContext &C);

  /// Given a value type, return an optional type wrapping it.
  static SILType getOptionalType(SILType valueType);

  /// Get the standard exception type.
  static SILType getExceptionType(const ASTContext &C);

  /// Get the SIL token type.
  static SILType getSILTokenType(const ASTContext &C);

  /// Get the type for pack indexes.
  static SILType getPackIndexType(const ASTContext &C);

  /// Return '()'
  static SILType getEmptyTupleType(const ASTContext &C);

  //
  // Utilities for treating SILType as a pointer-like type.
  //
  static SILType getFromOpaqueValue(void *P) {
    return SILType(ValueType::getFromOpaqueValue(P));
  }
  void *getOpaqueValue() const {
    return value.getOpaqueValue();
  }
  
  bool operator==(SILType rhs) const {
    return value.getOpaqueValue() == rhs.value.getOpaqueValue();
  }
  bool operator!=(SILType rhs) const {
    return value.getOpaqueValue() != rhs.value.getOpaqueValue();
  }

  /// Return the mangled name of this type, ignoring its prefix. Meant for
  /// diagnostic purposes.
  std::string getMangledName() const;

  std::string getAsString() const;
  void dump() const;
  void print(raw_ostream &OS,
             const PrintOptions &PO = PrintOptions::printSIL()) const;

  std::string getDebugDescription() const;
};

// Statically prevent SILTypes from being directly cast to a type
// that's not legal as a SIL value.
#define NON_SIL_TYPE(ID)                                             \
template<> Can##ID##Type SILType::getAs<ID##Type>() const = delete;  \
template<> Can##ID##Type SILType::castTo<ID##Type>() const = delete; \
template<> bool SILType::is<ID##Type>() const = delete;
NON_SIL_TYPE(Function)
NON_SIL_TYPE(GenericFunction)
NON_SIL_TYPE(AnyFunction)
NON_SIL_TYPE(LValue)
NON_SIL_TYPE(InOut)
#undef NON_SIL_TYPE

#define TYPE(ID, PARENT)
#define UNCHECKED_TYPE(ID, PARENT)                                   \
template<> Can##ID##Type SILType::getAs<ID##Type>() const = delete;  \
template<> Can##ID##Type SILType::castTo<ID##Type>() const = delete; \
template<> bool SILType::is<ID##Type>() const = delete;
#include "swift/AST/TypeNodes.def"

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SILType T) {
  T.print(OS);
  return OS;
}

inline SILType SILBlockStorageType::getCaptureAddressType() const {
  return SILType::getPrimitiveAddressType(getCaptureType());
}

inline SILType SILPackType::getSILElementType(unsigned index) const {
  return SILType::getPrimitiveType(getElementType(index),
                                   isElementAddress()
                                     ? SILValueCategory::Address
                                     : SILValueCategory::Object);
}

/// The hash of a SILType is the hash of its opaque value.
static inline llvm::hash_code hash_value(SILType V) {
  return llvm::hash_value(V.getOpaqueValue());
}

inline SILType SILField::getAddressType() const {
  return SILType::getPrimitiveAddressType(getLoweredType());
}
inline SILType SILField::getObjectType() const {
  return SILType::getPrimitiveObjectType(getLoweredType());
}

CanType getSILBoxFieldLoweredType(TypeExpansionContext context,
                                  SILBoxType *type, Lowering::TypeConverter &TC,
                                  unsigned index);

inline SILType getSILBoxFieldType(TypeExpansionContext context,
                                  SILBoxType *type, Lowering::TypeConverter &TC,
                                  unsigned index) {
  return SILType::getPrimitiveAddressType(
      getSILBoxFieldLoweredType(context, type, TC, index));
}

} // end swift namespace

namespace llvm {

// Allow the low bit of SILType to be used for nefarious purposes, e.g. putting
// a SILType into a PointerUnion.
template<>
struct PointerLikeTypeTraits<swift::SILType> {
public:
  static inline void *getAsVoidPointer(swift::SILType T) {
    return T.getOpaqueValue();
  }
  static inline swift::SILType getFromVoidPointer(void *P) {
    return swift::SILType::getFromOpaqueValue(P);
  }
  // SILType is just a wrapper around its ValueType, so it has a bit available.
  enum { NumLowBitsAvailable =
    PointerLikeTypeTraits<swift::SILType::ValueType>::NumLowBitsAvailable };
};


// Allow SILType to be used as a DenseMap key.
template<>
struct DenseMapInfo<swift::SILType> {
  using SILType = swift::SILType;
  using PointerMapInfo = DenseMapInfo<void*>;
public:
  static SILType getEmptyKey() {
    return SILType::getFromOpaqueValue(PointerMapInfo::getEmptyKey());
  }
  static SILType getTombstoneKey() {
    return SILType::getFromOpaqueValue(PointerMapInfo::getTombstoneKey());
  }
  static unsigned getHashValue(SILType t) {
    return PointerMapInfo::getHashValue(t.getOpaqueValue());
  }
  static bool isEqual(SILType LHS, SILType RHS) {
    return LHS == RHS;
  }
};

} // end llvm namespace

#endif
